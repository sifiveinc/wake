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

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

namespace {

// Retry budget for waiting on the sandboxed payload to fork and unshare its
// namespaces (there's a short window right after wakebox forks it).
constexpr int kPayloadRetries = 40;
constexpr int kPayloadRetryDelayUs = 50000;  // 50ms * 40 = 2s

std::string read_comm(pid_t pid) {
  FILE *f = fopen(("/proc/" + std::to_string(pid) + "/comm").c_str(), "r");
  if (!f) return "";
  char buf[64] = {};
  if (!fgets(buf, sizeof(buf), f)) buf[0] = 0;
  fclose(f);
  std::string comm(buf);
  while (!comm.empty() && comm.back() == '\n') comm.pop_back();
  return comm;
}

// Parses ppid (field 4) out of /proc/<pid>/stat, skipping past the "(comm)"
// field by scanning to the last ')' -- comm can itself contain spaces/parens.
std::optional<pid_t> read_ppid(pid_t pid) {
  FILE *f = fopen(("/proc/" + std::to_string(pid) + "/stat").c_str(), "r");
  if (!f) return std::nullopt;
  char buf[4096];
  size_t n = fread(buf, 1, sizeof(buf) - 1, f);
  fclose(f);
  buf[n] = 0;

  char *rparen = strrchr(buf, ')');
  int ppid = 0;
  if (!rparen || sscanf(rparen + 1, " %*c %d", &ppid) != 1) return std::nullopt;
  return ppid;
}

std::optional<std::string> read_ns_link(pid_t pid, const char *ns) {
  std::string path = "/proc/" + std::to_string(pid) + "/ns/" + ns;
  char buf[256];
  ssize_t n = readlink(path.c_str(), buf, sizeof(buf) - 1);
  if (n < 0) return std::nullopt;
  return std::string(buf, n);
}

// Find the direct child of `wakebox_main_pid` that has entered its own mount
// namespace -- that's the FUSE-sandboxed payload (wakebox-main itself stays in
// the host mount namespace). Excludes the wb-timer sibling (only present when
// a command timeout is configured), which never unshares and would otherwise
// be mistaken for the payload. Retries briefly to cover the race where the
// payload hasn't forked/unshared yet.
//
// This is the caller's own job, so /proc is fully visible; no privilege or
// CAP_SYS_PTRACE is needed to read /proc/<pid>/ns/*.
std::optional<pid_t> resolve_payload_pid(pid_t wakebox_main_pid) {
  auto main_mnt_ns = read_ns_link(wakebox_main_pid, "mnt");
  if (!main_mnt_ns) return std::nullopt;

  for (int attempt = 0; attempt < kPayloadRetries; ++attempt) {
    DIR *proc = opendir("/proc");
    if (!proc) return std::nullopt;

    std::optional<pid_t> found;
    struct dirent *entry;
    while ((entry = readdir(proc)) != nullptr) {
      char *end = nullptr;
      long candidate = strtol(entry->d_name, &end, 10);
      if (end == entry->d_name || *end != '\0' || candidate <= 0) continue;  // not a pid dir
      pid_t candidate_pid = static_cast<pid_t>(candidate);

      auto ppid = read_ppid(candidate_pid);
      if (!ppid || *ppid != wakebox_main_pid) continue;
      if (read_comm(candidate_pid) == "wb-timer") continue;

      auto mnt_ns = read_ns_link(candidate_pid, "mnt");
      if (mnt_ns && *mnt_ns != *main_mnt_ns) {
        found = candidate_pid;
        break;
      }
    }
    closedir(proc);
    if (found) return found;
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

  // kill(pid, 0) with signal 0 only probes for existence/permission, it
  // sends nothing. ESRCH means the pid is really gone (stale record from a
  // crashed run). EPERM means it exists but belongs to another uid -- attach
  // only supports the owning user (only the owner's euid satisfies the
  // kernel's setns ownership check without privilege), so that's an error too.
  if (kill(info->pid, 0) != 0) {
    if (errno == ESRCH) {
      std::cerr << "wake --attach: job " << job_id
                << "'s process is no longer running (stale record from a crashed run -- run "
                   "'wake' or 'wake --clean' to reap it)"
                << std::endl;
    } else {
      std::cerr << "wake --attach: job " << job_id
                << " belongs to another user; attach only supports jobs you launched" << std::endl;
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
                 "hasn't started yet"
              << std::endl;
    return EXIT_FAILURE;
  }

  const char *shell = getenv("SHELL");
  if (!shell || !*shell) shell = "/bin/sh";

  // fork(): the child is single-threaded regardless of how many threads wake
  // has, which is exactly what setns(CLONE_NEWUSER) requires. No privileged
  // helper is needed -- the caller owns the job, so its euid matches the
  // sandbox's user-namespace owner and the kernel permits the setns with no
  // capabilities at all. The view is read/write (writes land in the same FUSE
  // staging the job itself writes to); guaranteeing read-only would require
  // CAP_SYS_ADMIN the owner doesn't have here (see the design doc's revision).
  pid_t child = fork();
  if (child < 0) {
    std::cerr << "wake --attach: fork: " << strerror(errno) << std::endl;
    return EXIT_FAILURE;
  }

  if (child == 0) {
    std::string ns_dir = "/proc/" + std::to_string(*payload_pid) + "/ns/";
    std::string cwd_link = "/proc/" + std::to_string(*payload_pid) + "/cwd";

    // Grab the job's working directory as an fd *before* entering the sandbox.
    // The workspace can be mounted at an arbitrary path inside the sandbox
    // (e.g. /workspace under a container rootfs, not the host workspace path),
    // so a guessed absolute path is wrong in general. /proc/<payload>/cwd is a
    // magic symlink the kernel resolves straight to the job's cwd dentry --
    // openable here even though that path isn't reachable from our mount ns --
    // and fchdir()ing it after setns lands us exactly where the job is. Also
    // read its path string (as seen inside the sandbox) purely for the notice.
    char cwd_path[PATH_MAX];
    ssize_t n = readlink(cwd_link.c_str(), cwd_path, sizeof(cwd_path) - 1);
    std::string cwd_display = n > 0 ? std::string(cwd_path, n) : "the job's working directory";
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

    std::cerr << "wake --attach: read/write shell in the live sandbox of job " << job_id
              << " (pid " << *payload_pid << ", cwd " << cwd_display
              << "); writes go to the job's in-progress outputs" << std::endl;

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
