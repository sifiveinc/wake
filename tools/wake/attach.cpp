/*
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

#include "attach.h"

#ifdef __linux__

#include <fcntl.h>
#include <sched.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

// Retry budget for waiting on the sandboxed payload to fork and unshare its
// namespaces (there's a short window right after wakebox forks it).
constexpr int kPayloadRetries = 40;
constexpr int kPayloadRetryDelayUs = 50000;  // 50ms * 40 = 2s
constexpr size_t kWarningContentWidth = 64;

std::string warning_line(std::string content) {
  if (content.size() > kWarningContentWidth) {
    content.resize(kWarningContentWidth - 3);
    content += "...";
  }
  return "| " + content + std::string(kWarningContentWidth - content.size(), ' ') + " |";
}

std::string read_comm(pid_t pid) {
  std::ifstream comm_file("/proc/" + std::to_string(pid) + "/comm");
  std::string comm;
  std::getline(comm_file, comm);
  return comm;
}

std::vector<pid_t> read_children(pid_t pid) {
  std::ifstream children("/proc/" + std::to_string(pid) + "/task/" + std::to_string(pid) +
                         "/children");
  std::vector<pid_t> result;
  pid_t child;
  while (children >> child) result.push_back(child);
  return result;
}

std::optional<std::string> read_ns_link(pid_t pid, const char *ns) {
  std::error_code error;
  std::filesystem::path link =
      std::filesystem::read_symlink("/proc/" + std::to_string(pid) + "/ns/" + ns, error);
  if (error) return std::nullopt;
  return link.string();
}

// unshare(CLONE_NEWUSER) leaves uid_map empty until wakebox writes the mapping.
// Entering earlier gives the attached shell overflow (nobody) credentials.
bool user_namespace_ready(pid_t pid) {
  std::ifstream uid_map("/proc/" + std::to_string(pid) + "/uid_map");
  return uid_map.peek() != std::ifstream::traits_type::eof();
}

// Find the direct child of `wakebox_main_pid` that has entered its own mount
// namespace -- that's the FUSE-sandboxed payload (wakebox-main itself stays in
// the host mount namespace). Excludes the wb-timer sibling (only present when
// a command timeout is configured), which never unshares and would otherwise
// be mistaken for the payload. Retries briefly to cover the race where the
// payload hasn't forked/unshared yet or has not finished initializing its user
// namespace.
std::optional<pid_t> resolve_payload_pid(pid_t wakebox_main_pid) {
  auto main_mnt_ns = read_ns_link(wakebox_main_pid, "mnt");
  if (!main_mnt_ns) return std::nullopt;

  for (int attempt = 0; attempt < kPayloadRetries; ++attempt) {
    for (pid_t candidate_pid : read_children(wakebox_main_pid)) {
      if (read_comm(candidate_pid) == "wb-timer") continue;

      auto mnt_ns = read_ns_link(candidate_pid, "mnt");
      if (mnt_ns && *mnt_ns != *main_mnt_ns && user_namespace_ready(candidate_pid))
        return candidate_pid;
    }
    usleep(kPayloadRetryDelayUs);
  }
  return std::nullopt;
}

bool setns_path(const std::string &path, int nstype) {
  int fd = open(path.c_str(), O_RDONLY);
  if (fd < 0) {
    std::cerr << "wake --attach: open(" << path << "): " << strerror(errno) << std::endl;
    return false;
  }
  bool ok = setns(fd, nstype) == 0;
  if (!ok) std::cerr << "wake --attach: setns(" << path << "): " << strerror(errno) << std::endl;
  close(fd);
  return ok;
}

}  // namespace

int attach_job(Database &db, long job_id) {
  auto info = db.get_live_job(job_id);
  if (!info) {
    std::cerr << "wake --attach: job " << job_id
              << " is not currently running (unknown job id, or already finished)" << std::endl;
    return EXIT_FAILURE;
  }

  if (!db.is_live_run(info->run_id)) {
    std::cerr << "wake --attach: job " << job_id
              << " is not currently running (its wake run is no longer live)" << std::endl;
    return EXIT_FAILURE;
  }

  // Signal 0 does not kill anything; it checks that the live job's process
  // still exists and is accessible. errno is meaningful only if it fails.
  if (kill(info->pid, 0) != 0) {
    if (errno == ESRCH) {
      std::cerr << "wake --attach: job " << job_id << "'s process is no longer running"
                << std::endl;
    } else if (errno == EPERM) {
      std::cerr << "wake --attach: job " << job_id
                << " belongs to another user; attach only supports jobs you launched" << std::endl;
    } else {
      std::cerr << "wake --attach: could not probe job " << job_id
                << "'s process: " << strerror(errno) << std::endl;
    }
    return EXIT_FAILURE;
  }

  // The recorded pid is wakebox-main, which stays in the host namespaces. The
  // FUSE-sandboxed view lives in its payload child (the one that unshared into
  // its own mount namespace); resolve that before entering anything.
  auto payload_pid = resolve_payload_pid(info->pid);
  if (!payload_pid) {
    std::cerr << "wake --attach: could not find a sandboxed process for job " << job_id
              << " -- it may not have been run under the FUSE sandbox runner, or the sandbox "
                 "hasn't finished initializing yet"
              << std::endl;
    return EXIT_FAILURE;
  }

  const char *shell = getenv("SHELL");
  if (!shell || !*shell) shell = "/bin/sh";

  // setns(CLONE_NEWUSER) requires a single-threaded process, so enter the
  // namespaces from a forked child. The attached view is read/write.
  pid_t child = fork();
  if (child < 0) {
    std::cerr << "wake --attach: fork: " << strerror(errno) << std::endl;
    return EXIT_FAILURE;
  }

  if (child == 0) {
    std::string ns_dir = "/proc/" + std::to_string(*payload_pid) + "/ns/";
    std::string cwd_link = "/proc/" + std::to_string(*payload_pid) + "/cwd";

    // Keep the job's cwd open before changing mount namespaces, then restore it
    // with fchdir() after entering the sandbox. Read its path for the notice.
    std::error_code error;
    std::filesystem::path cwd_path = std::filesystem::read_symlink(cwd_link, error);
    std::string cwd_display = error ? "the job's working directory" : cwd_path.string();
    int cwd_fd = open(cwd_link.c_str(), O_RDONLY | O_DIRECTORY);
    if (cwd_fd < 0) {
      std::cerr << "wake --attach: open(" << cwd_link << "): " << strerror(errno) << std::endl;
      _exit(EXIT_FAILURE);
    }

    // User namespace first (grants CAP_SYS_ADMIN within it, which the
    // subsequent mount-namespace setns requires), then the mount namespace.
    if (!setns_path(ns_dir + "user", CLONE_NEWUSER) || !setns_path(ns_dir + "mnt", CLONE_NEWNS))
      _exit(EXIT_FAILURE);

    // setns(CLONE_NEWNS) resets cwd to the new namespace's root; fchdir back to
    // the job's own working directory via the fd captured above.
    if (fchdir(cwd_fd) != 0) {
      std::cerr << "wake --attach: could not enter the job's working directory (" << cwd_display
                << "): " << strerror(errno) << std::endl;
      _exit(EXIT_FAILURE);
    }
    close(cwd_fd);

    std::cerr << "+------------------------------------------------------------------+\n"
              << warning_line("Attached to the live sandbox for Wake job " + std::to_string(job_id))
              << '\n'
              << warning_line("PID: " + std::to_string(*payload_pid) + "  CWD: " + cwd_display)
              << '\n'
              << warning_line("") << '\n'
              << warning_line("WARNING: This shell is read/write. Any changes you make affect")
              << '\n'
              << warning_line("the in-progress job and may change the build result.") << '\n'
              << "+------------------------------------------------------------------+"
              << std::endl;

    execl(shell, shell, nullptr);
    std::cerr << "wake --attach: exec(" << shell << "): " << strerror(errno) << std::endl;
    _exit(EXIT_FAILURE);
  }

  int status = 0;
  if (waitpid(child, &status, 0) != child) {
    std::cerr << "wake --attach: waitpid: " << strerror(errno) << std::endl;
    return EXIT_FAILURE;
  }
  return WIFEXITED(status) ? WEXITSTATUS(status) : EXIT_FAILURE;
}

#endif  // __linux__
