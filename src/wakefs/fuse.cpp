/* Wake FUSE launcher to capture inputs/outputs
 *
 * Copyright 2019 SiFive, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You should have received a copy of LICENSE.Apache2 along with
 * this software. If not, you may obtain a copy at
 *
 *    https://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// Open Group Base Specifications Issue 7
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include "fuse.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fstream>
#include <iostream>
#include <map>
#include <sstream>

#ifdef __linux__
#include <sys/prctl.h>
#endif

#include "compat/rusage.h"
#include "json/json5.h"
#include "namespace.h"
#include "util/execpath.h"
#include "util/shell.h"

#ifndef HOST_NAME_MAX
#define HOST_NAME_MAX 255
#endif

namespace {

volatile sig_atomic_t cancellation_signal = 0;
volatile sig_atomic_t cancellation_repeated = 0;

extern "C" void record_cancellation_signal(int signal) {
  if (cancellation_signal == 0) {
    cancellation_signal = signal;
  } else {
    cancellation_repeated = 1;
  }
}

class CancellationSignalHandlers {
 public:
  CancellationSignalHandlers() {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = record_cancellation_signal;
    sigemptyset(&action.sa_mask);
    sigaddset(&action.sa_mask, SIGINT);
    sigaddset(&action.sa_mask, SIGTERM);
    action.sa_flags = 0;
    interrupt_installed_ = sigaction(SIGINT, &action, &interrupt_) == 0;
    installed_ = interrupt_installed_ && sigaction(SIGTERM, &action, &terminate_) == 0;
    if (!installed_ && interrupt_installed_) sigaction(SIGINT, &interrupt_, nullptr);
  }

  ~CancellationSignalHandlers() {
    if (!installed_) return;
    sigaction(SIGINT, &interrupt_, nullptr);
    sigaction(SIGTERM, &terminate_, nullptr);
  }

  bool installed() const { return installed_; }
  bool requested() const { return cancellation_signal != 0; }
  bool repeated() const { return cancellation_repeated != 0; }
  int signal() const { return cancellation_signal; }

 private:
  struct sigaction interrupt_ = {};
  struct sigaction terminate_ = {};
  bool interrupt_installed_ = false;
  bool installed_ = false;
};

bool cancellation_deadline_passed(const struct timespec &deadline) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return now.tv_sec > deadline.tv_sec ||
         (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec);
}

bool process_group_exists(pid_t leader) { return kill(-leader, 0) == 0 || errno == EPERM; }

}  // namespace

bool json_as_struct(const std::string &json, json_args &result) {
  JAST jast;
  if (!JAST::parse(json, std::cerr, jast)) return false;

  for (auto &x : jast.get("command").children) result.command.push_back(x.second.value);

  for (auto &x : jast.get("environment").children) result.environment.push_back(x.second.value);

  for (auto &x : jast.get("visible").children) {
    visible_file vf;
    if (x.second.kind == JSON_OBJECT) {
      // New format: {"path": "...", "type": "...", "hash": "...", "mode": ..., "mtime": ...}
      vf.path = x.second.get("path").value;
      vf.type = x.second.get("type").value;
      vf.hash = x.second.get("hash").value;
      const std::string &mode_value = x.second.get("mode").value;
      const std::string &mtime_value = x.second.get("mtime").value;

      if (vf.path.empty()) {
        std::cerr << "Visible entry missing 'path'\n";
        return false;
      }
      if (vf.type.empty()) {
        std::cerr << "Visible entry '" << vf.path << "' missing 'type'\n";
        return false;
      }
      if (mode_value.empty()) {
        std::cerr << "Visible entry '" << vf.path << "' missing 'mode'\n";
        return false;
      }
      if (mtime_value.empty()) {
        std::cerr << "Visible entry '" << vf.path << "' missing 'mtime'\n";
        return false;
      }

      try {
        vf.mode = std::stoi(mode_value);
      } catch (const std::exception &e) {
        std::cerr << "Visible entry '" << vf.path << "' has invalid 'mode' value '" << mode_value
                  << "': " << e.what() << "\n";
        return false;
      }

      try {
        vf.mtime = std::stol(mtime_value);
      } catch (const std::exception &e) {
        std::cerr << "Visible entry '" << vf.path << "' has invalid 'mtime' value '" << mtime_value
                  << "': " << e.what() << "\n";
        return false;
      }

      if (vf.type != "directory" && vf.hash.empty()) {
        std::cerr << "Visible entry '" << vf.path << "' (type '" << vf.type
                  << "') missing 'hash'\n";
        return false;
      }
    } else {
      // Legacy format: just a string path (no CAS lookup possible)
      vf.path = x.second.value;
      vf.type = "";
      vf.hash = "";  // Empty hash means read from workspace
      vf.mode.reset();
      vf.mtime = 0;
    }
    result.visible.push_back(vf);
  }

  // Parse CAS root directory (layout: {cas_dir}/blobs, {cas_dir}/staging)
  result.cas_dir = jast.get("cas-dir").value;
  if (result.cas_dir.empty()) {
    result.cas_dir = ".build/cas";
  }
  if (result.cas_dir != ".build/cas") {
    std::cerr << "cas-dir must be exactly .build/cas" << std::endl;
    return false;
  }

  JAST timeout_entry = jast.get("command-timeout");
  if (timeout_entry.kind == JSON_INTEGER) {
    int timeout = std::stoi(timeout_entry.value);
    if (timeout <= 0) {
      std::cerr << "timeout must be be an integer value greater than 0" << std::endl;
      return false;
    }

    result.command_timeout = std::optional<int>{timeout};
  } else if (timeout_entry.kind != JSON_NULLVAL) {
    std::cerr << "timeout must be be an integer value greater than 0" << std::endl;
    return false;
  }

  result.directory = jast.get("directory").value;
  result.stdin_file = jast.get("stdin").value;

  JAST wake_run_id = jast.get("wake_run_id");
  JAST wake_job_id = jast.get("wake_job_id");
  if ((wake_run_id.kind == JSON_NULLVAL) != (wake_job_id.kind == JSON_NULLVAL)) {
    std::cerr << "wake_run_id and wake_job_id must be provided together" << std::endl;
    return false;
  }
  if (wake_run_id.kind != JSON_NULLVAL) {
    if (wake_run_id.kind != JSON_INTEGER) {
      std::cerr << "wake_run_id must be an integer value" << std::endl;
      return false;
    }
    if (wake_job_id.kind != JSON_INTEGER) {
      std::cerr << "wake_job_id must be an integer value" << std::endl;
      return false;
    }
    try {
      result.wake_run_id = std::stol(wake_run_id.value);
      result.wake_job_id = std::stol(wake_job_id.value);
    } catch (const std::exception &e) {
      std::cerr << "wake_run_id and wake_job_id must be integer values: " << e.what() << std::endl;
      return false;
    }
  }

  result.isolate_network = jast.get("isolate-network").kind == JSON_TRUE;
  result.isolate_pids = jast.get("isolate-pids").kind == JSON_TRUE;

  result.hostname = jast.get("hostname").value;
  result.domainname = jast.get("domainname").value;

  std::string userid = jast.get("user-id").value;
  result.userid = !userid.empty() ? std::stoi(userid) : geteuid();

  std::string groupid = jast.get("group-id").value;
  result.groupid = !groupid.empty() ? std::stoi(groupid) : getegid();

  for (auto &x : jast.get("mount-ops").children) {
    result.mount_ops.push_back({x.second.get("type").value, x.second.get("source").value,
                                x.second.get("destination").value,
                                x.second.get("read_only").kind == JSON_TRUE});
  }
  return true;
}

int execve_wrapper(const std::vector<std::string> &command,
                   const std::vector<std::string> &environment) {
  std::vector<const char *> cmd_args;
  for (auto &s : command) cmd_args.push_back(s.c_str());
  cmd_args.push_back(0);

  std::vector<const char *> env;
  for (auto &e : environment) env.push_back(e.c_str());
  env.push_back(0);

  execve(command[0].c_str(), const_cast<char *const *>(cmd_args.data()),
         const_cast<char *const *>(env.data()));
  return errno;
}

static bool collect_result_metadata(const std::string daemon_output, const struct timeval &start,
                                    const struct timeval &stop, const pid_t pid, const int status,
                                    const RUsage &rusage, bool timed_out, int canceled_signal,
                                    std::string &result_json) {
  JAST from_daemon;
  std::stringstream ss;
  if (!JAST::parse(daemon_output, ss, from_daemon)) {
    // stderr is closed, so report the error on the only output we have
    result_json = ss.str();
    return false;
  }

  JAST result_jast(JSON_OBJECT);
  auto &usage = result_jast.add("usage", JSON_OBJECT);
  usage.add("status", status);
  usage.add("membytes", static_cast<long long>(rusage.membytes));
  usage.add("inbytes", std::stoll(from_daemon.get("ibytes").value));
  usage.add("outbytes", std::stoll(from_daemon.get("obytes").value));
  usage.add("runtime", stop.tv_sec - start.tv_sec + (stop.tv_usec - start.tv_usec) / 1000000.0);
  usage.add("cputime", rusage.utime + rusage.stime);

  result_jast.add("inputs", JSON_ARRAY).children = std::move(from_daemon.get("inputs").children);
  result_jast.add("outputs", JSON_ARRAY).children = std::move(from_daemon.get("outputs").children);
  result_jast.add_bool("timed-out", timed_out);
  if (canceled_signal != 0) result_jast.add("canceled-signal", canceled_signal);

  auto staging_files_opt = from_daemon.get_opt("staging_files");
  if (staging_files_opt && (*staging_files_opt)->kind == JSON_OBJECT) {
    result_jast.add("staging_files", JSON_OBJECT).children =
        std::move((*staging_files_opt)->children);
  }
  auto recovery_manifest_opt = from_daemon.get_opt("recovery_manifest");
  if (recovery_manifest_opt && (*recovery_manifest_opt)->kind == JSON_STR) {
    result_jast.add("recovery_manifest", (*recovery_manifest_opt)->value);
  }

  char hostname[HOST_NAME_MAX + 1];
  if (0 == gethostname(hostname, sizeof(hostname))) result_jast.add("run-host", hostname);

  std::stringstream result_ss;
  result_ss << result_jast;
  result_json = result_ss.str();

  return !result_ss.fail();
}

bool run_in_fuse(fuse_args &args, int &status, std::string &result_json, FuseRunOutcome &outcome) {
  if (0 != chdir(args.working_dir.c_str())) {
    std::cerr << "chdir " << args.working_dir << ": " << strerror(errno) << std::endl;
    return false;
  }

  if (!args.daemon.connect(args.visible, args.cas_dir, args.isolate_pids, args.wake_run_id,
                           args.wake_job_id))
    return false;
  cancellation_signal = 0;
  cancellation_repeated = 0;
  CancellationSignalHandlers signal_handlers;
  if (!signal_handlers.installed()) {
    std::cerr << "wakebox: install cancellation signal handler: " << strerror(errno) << std::endl;
    return false;
  }

  struct timeval start;
  gettimeofday(&start, 0);

  pid_t payload_pid = fork();
  if (payload_pid == 0) {
    struct sigaction default_action;
    memset(&default_action, 0, sizeof(default_action));
    default_action.sa_handler = SIG_DFL;
    sigemptyset(&default_action.sa_mask);
    sigaction(SIGINT, &default_action, nullptr);
    sigaction(SIGTERM, &default_action, nullptr);
    // Isolate the payload and its descendants so wakebox can signal them as a group.
    (void)setpgid(0, 0);
    std::vector<std::string> command = args.command;
    std::vector<std::string> envs_from_mounts;
#ifdef __linux__
    // This process should terminate if the parent process exits.
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) == -1) {
      std::cerr << "run_in_fuse prctl: " << strerror(errno) << std::endl;
      exit(1);
    }

    if (!setup_user_namespaces(args.userid, args.groupid, args.isolate_network, args.hostname,
                               args.domainname))
      exit(1);

    if (!do_mounts(args.mount_ops, args.daemon.mount_subdir, envs_from_mounts)) exit(1);

    prctl(PR_SET_NAME, "wb-mount-ns", 0, 0, 0);
#endif

    if (chdir(args.command_running_dir.c_str()) != 0) {
      std::cerr << "chdir " << args.command_running_dir << ": " << strerror(errno) << std::endl;
      exit(1);
    }

    if (envs_from_mounts.empty()) {
      // Search the PATH for the executable location.
      command[0] = find_in_path(command[0], find_path(args.environment));
    } else {
      // 'source' the environments provided by any mounts before running command.
      // The shell will search the PATH for the executable location.
      command = {"/bin/sh", "-c"};
      std::stringstream cmd_ss;
      for (auto &e : envs_from_mounts) cmd_ss << ". " << shell_escape(e) << " && ";

      cmd_ss << "exec";
      for (auto &s : args.command) cmd_ss << " " << shell_escape(s);
      command.push_back(cmd_ss.str());
    }

    if (args.use_stdin_file) {
      std::string stdin_file = args.stdin_file;
      if (stdin_file.empty()) stdin_file = "/dev/null";

      int fd = open(stdin_file.c_str(), O_RDONLY);
      if (fd == -1) {
        std::cerr << "open " << stdin_file << ":" << strerror(errno) << std::endl;
        exit(1);
      }
      if (fd != STDIN_FILENO) {
        dup2(fd, STDIN_FILENO);
        close(fd);
      }
    }

#ifdef __linux__
    if (args.isolate_pids) {
      pidns_args nsargs = {command, args.environment};
      exec_in_pidns(nsargs);
    } else {
#endif
      int err = execve_wrapper(command, args.environment);
      std::cerr << "execve " << command[0] << ": " << strerror(err) << std::endl;
#ifdef __linux__
    }
#endif
    exit(1);
  }
  if (payload_pid < 0) {
    std::cerr << "wakebox: fork payload: " << strerror(errno) << std::endl;
    return false;
  }
  // Either side may run first; repeat the setup here to close that fork race.
  (void)setpgid(payload_pid, payload_pid);

  // Don't hold IO open while waiting
  (void)close(STDIN_FILENO);
  (void)close(STDOUT_FILENO);
  (void)close(STDERR_FILENO);

  pid_t timeout_pid = -1;

  // Launch timer process
  if (args.command_timeout) {
    timeout_pid = fork();
    if (timeout_pid < 0) {
      std::cerr << "wakebox: failed to fork timeout process" << std::endl;
      exit(1);
    }

    if (timeout_pid == 0) {
      prctl(PR_SET_NAME, "wb-timer", 0, 0, 0);
      sleep(*args.command_timeout);
      exit(124);
    }
  }

  constexpr time_t cancellation_grace_seconds = 5;
  bool payload_reaped = false;
  bool cancellation_started = false;
  bool payload_killed = false;
  int payload_wait_status = 0;
  struct timespec cancellation_deadline = {};
  outcome = FuseRunOutcome::Completed;
  pid_t wait_pid;
  while (!payload_reaped) {
    // Poll after cancellation so we can enforce the grace deadline.
    wait_pid = waitpid(-1, &status, cancellation_started ? WNOHANG : 0);
    if (wait_pid == -1 && errno == EINTR) {
      // The handler recorded cancellation; handle it below in normal control flow.
    } else if (wait_pid == -1 && errno == ECHILD) {
      // Nothing remains to reap; use the fallback status below.
      break;
    } else if (wait_pid == -1) {
      return false;
    } else if (wait_pid == 0) {
      // The payload is still shutting down after cancellation.
      struct timespec pause = {0, 100000000};
      nanosleep(&pause, nullptr);
    } else if (wait_pid == timeout_pid && WIFEXITED(status) && !signal_handlers.requested()) {
      // Preserve the existing timeout behavior unless an external signal won the race.
      kill(payload_pid, SIGKILL);

      struct timeval stop;
      gettimeofday(&stop, 0);
      std::string output;
      args.daemon.disconnect(output);
      RUsage usage = {};
      status = 124;
      outcome = FuseRunOutcome::TimedOut;
      return collect_result_metadata(output, start, stop, payload_pid, 124, usage, true, 0,
                                     result_json);
    } else if (wait_pid == payload_pid && !WIFSTOPPED(status)) {
      // The direct payload exited; descendants are handled below if cancellation began.
      payload_wait_status = status;
      payload_reaped = true;
      if (args.command_timeout) {
        kill(timeout_pid, SIGKILL);
      }
    }

    if (!signal_handlers.requested()) continue;
    if (!cancellation_started) {
      // First external signal: stop the timer and give the payload group time to exit.
      cancellation_started = true;
      clock_gettime(CLOCK_MONOTONIC, &cancellation_deadline);
      cancellation_deadline.tv_sec += cancellation_grace_seconds;
      if (timeout_pid != -1) (void)kill(timeout_pid, SIGKILL);
      (void)kill(-payload_pid, SIGTERM);
    }
    if (!payload_killed &&
        (signal_handlers.repeated() || cancellation_deadline_passed(cancellation_deadline))) {
      // A repeated signal or expired grace period requires immediate termination.
      (void)kill(-payload_pid, SIGKILL);
      payload_killed = true;
    }
  }

  if (WIFEXITED(payload_wait_status)) {
    status = WEXITSTATUS(payload_wait_status);
  } else {
    status = WIFSIGNALED(payload_wait_status) ? -WTERMSIG(payload_wait_status) : -SIGTERM;
  }
  if (cancellation_started && status == 0) status = -signal_handlers.signal();

  // The process group can outlive its direct child. Keep the daemon live until
  // every writer has exited so its final manifest cannot freeze mid-write.
  if (cancellation_started && !args.isolate_pids) {
    while (process_group_exists(payload_pid)) {
      if (!payload_killed &&
          (signal_handlers.repeated() || cancellation_deadline_passed(cancellation_deadline))) {
        // The payload exited, but a descendant still needs escalation.
        (void)kill(-payload_pid, SIGKILL);
        payload_killed = true;
      }
      struct timespec pause = {0, 100000000};
      nanosleep(&pause, nullptr);
    }
  }

  // RUsage is calculated for all child processes that have 1) terminated and 2) been wait()ed on.
  // Though we may fork two processes, we only ever wait on the payload process after termination
  // (assuming it doesn't timeout) so the RUsage will only include the payload process useage.
  RUsage usage = getRUsageChildren();

  struct timeval stop;
  gettimeofday(&stop, 0);

  std::string output;
  args.daemon.disconnect(output);

  if (cancellation_started) outcome = FuseRunOutcome::Canceled;
  return collect_result_metadata(output, start, stop, payload_pid, status, usage, false,
                                 outcome == FuseRunOutcome::Canceled ? signal_handlers.signal() : 0,
                                 result_json);
}
