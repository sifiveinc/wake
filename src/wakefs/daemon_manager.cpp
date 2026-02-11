/* Wake FUSE daemon lifecycle manager
 *
 * Copyright 2026 SiFive, Inc.
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

#include "daemon_manager.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <iostream>

#include "util/execpath.h"
#include "util/mkdir_parents.h"

// Helper to construct the mount path with uid.gid suffix
// This matches the logic in daemon_client.cpp
static std::string make_mount_path(const std::string &base_dir) {
  return base_dir + "/.fuse/" + std::to_string(getuid()) + "." + std::to_string(getgid());
}

FuseDaemonManager::FuseDaemonManager(const std::string &workspace_dir)
    : workspace_dir_(workspace_dir),
      mount_path_(make_mount_path(workspace_dir)),
      executable_(find_execpath() + "/../lib/wake/fuse-waked"),
      is_running_path_(mount_path_ + "/.f.fuse-waked"),
      global_fd_(-1) {}

FuseDaemonManager::~FuseDaemonManager() {
  // Release our hold on the daemon
  // The daemon will exit after its linger timeout if no other clients remain
  if (global_fd_ >= 0) {
    close(global_fd_);
    global_fd_ = -1;
  }
}

bool FuseDaemonManager::is_daemon_alive() const {
  // The marker file /.f.fuse-waked only exists when daemon is mounted and running
  int fd = open(is_running_path_.c_str(), O_RDONLY);
  if (fd >= 0) {
    close(fd);
    return true;
  }
  return false;
}

bool FuseDaemonManager::ensure_daemon_running() {
  // Create mount directory structure if needed
  int err = mkdir_with_parents(mount_path_, 0775);
  if (0 != err) {
    std::cerr << "fuse-daemon: mkdir_with_parents('" << mount_path_ << "'): " << strerror(err)
              << std::endl;
    return false;
  }

  // Try to open the marker file - if successful, daemon is already running
  global_fd_ = open(is_running_path_.c_str(), O_RDONLY);
  if (global_fd_ >= 0) {
    // Daemon is already running, we now hold a reference to keep it alive
    return true;
  }

  // Daemon not running - need to start it
  // Use exponential backoff similar to daemon_client.cpp
  int wait_ms = 10;
  const int max_retries = 12;

  for (int retry = 0; retry < max_retries; ++retry) {
    struct timespec delay;
    delay.tv_sec = wait_ms / 1000;
    delay.tv_nsec = (wait_ms % 1000) * INT64_C(1000000);

    pid_t pid = fork();
    if (pid == 0) {
      // Child process - exec the daemon
      // Use a longer linger timeout since wake main process holds it for build duration
      // The daemon will wait this long after all clients disconnect before exiting
      int exit_delay = 60;  // 60 seconds default linger
      std::string delay_str = std::to_string(exit_delay);

      // Minimal environment for the daemon
      const char *env[3] = {"PATH=/usr/bin:/bin:/usr/sbin:/sbin", nullptr, nullptr};
      if (getenv("DEBUG_FUSE_WAKE")) {
        env[1] = "DEBUG_FUSE_WAKE=1";
      }

      execle(executable_.c_str(), "fuse-waked", mount_path_.c_str(), delay_str.c_str(), nullptr,
             env);

      // If we get here, exec failed
      std::cerr << "fuse-daemon: execle(" << executable_ << "): " << strerror(errno) << std::endl;
      _exit(1);
    }

    if (pid == -1) {
      std::cerr << "fuse-daemon: fork failed: " << strerror(errno) << std::endl;
      return false;
    }

    // Wait for the forked process to complete
    // The daemon double-forks internally to become a true daemon
    int status;
    do {
      waitpid(pid, &status, 0);
    } while (WIFSTOPPED(status));

    // Sleep to give daemon time to initialize and mount
    int ok;
    do {
      ok = nanosleep(&delay, &delay);
    } while (ok == -1 && errno == EINTR);

    // Try to connect to the daemon
    global_fd_ = open(is_running_path_.c_str(), O_RDONLY);
    if (global_fd_ >= 0) {
      // Successfully connected to daemon
      return true;
    }

    // Exponential backoff for next retry
    wait_ms <<= 1;
  }

  std::cerr << "fuse-daemon: Could not start or contact FUSE daemon after " << max_retries
            << " attempts" << std::endl;
  return false;
}
