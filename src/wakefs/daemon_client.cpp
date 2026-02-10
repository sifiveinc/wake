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

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include <fstream>
#include <iostream>
#include <sstream>

#include "fuse.h"
#include "json/json5.h"
#include "util/execpath.h"
#include "util/mkdir_parents.h"

// The user-id and group-id are used so that fuse daemons with different uid:gid pairs
// running within the same build can co-exist without trying to share. The kernel
// prevents cross-user sharing when the 'allow_other' fuse mount option is not used.
const std::string mount_path_id(const std::string &base_dir) {
  return base_dir + "/.fuse/" + std::to_string(getuid()) + "." + std::to_string(getgid());
}

daemon_client::daemon_client(const std::string &base_dir)
    : executable(find_execpath() + "/../lib/wake/fuse-waked"),
      mount_path(mount_path_id(base_dir)),
      mount_subdir(mount_path + "/" + std::to_string(getpid())),
      output_path(mount_path + "/.o." + std::to_string(getpid())),
      is_running_path(mount_path + "/.f.fuse-waked"),
      subdir_live_file(mount_path + "/.l." + std::to_string(getpid())),
      visibles_path(mount_path + "/.i." + std::to_string(getpid())) {}

// The arg 'visible' is destroyed/moved in the interest of performance with large visible lists.
bool daemon_client::connect(std::vector<std::string> &visible, bool close_live_file) {
  int err = mkdir_with_parents(mount_path, 0775);
  if (0 != err) {
    std::cerr << "mkdir_with_parents ('" << mount_path << "'):" << strerror(err) << std::endl;
    return false;
  }

  bool debug_fuse = getenv("DEBUG_FUSE_WAKE") != nullptr;

  int ffd = -1;
  int wait_ms = 10;
  for (int retry = 0; (ffd = open(is_running_path.c_str(), O_RDONLY)) == -1 && retry < 12;
       ++retry) {
    int open_errno = errno;
    if (debug_fuse) {
      std::cerr << "[fuse_client] connect retry=" << retry
                << " errno=" << open_errno << " (" << strerror(open_errno) << ")" << std::endl;
    }

    struct timespec delay;
    delay.tv_sec = wait_ms / 1000;
    delay.tv_nsec = (wait_ms % 1000) * INT64_C(1000000);

    pid_t pid = fork();
    if (pid == 0) {
      // The daemon should wait at least 4x as long to exit as we wait for it to start.
      int exit_delay = 4 * delay.tv_sec;
      if (exit_delay < 2) exit_delay = 2;
      std::string delayStr = std::to_string(exit_delay);
      const char *env[3] = {"PATH=/usr/bin:/bin:/usr/sbin:/sbin", 0, 0};
      if (debug_fuse) env[1] = "DEBUG_FUSE_WAKE=1";
      execle(executable.c_str(), "fuse-waked", mount_path.c_str(), delayStr.c_str(), nullptr, env);
      std::cerr << "execl " << executable << ": " << strerror(errno) << std::endl;
      exit(1);
    }

    // Sleep the full amount (even if signals like SIGWINCH arrive)
    int ok;
    do {
      ok = nanosleep(&delay, &delay);
    } while (ok == -1 && errno == EINTR);

    wait_ms <<= 1;

    int status;
    do waitpid(pid, &status, 0);
    while (WIFSTOPPED(status));
  }

  if (ffd == -1) {
    int final_errno = errno;
    std::cerr << "[fuse_client] Could not contact FUSE daemon" << std::endl;
    if (debug_fuse) {
      std::cerr << "[fuse_client] errno=" << final_errno
                << " (" << strerror(final_errno) << ")" << std::endl;
    }
    return false;
  }

  // subdir_live_file remains open until we terminate.
  // Without a PID namespace the live file will be leaked into every child process.
  int flags = O_CREAT | O_RDWR | O_EXCL;
  if (close_live_file) {
    // Only allow wakebox itself to hold the live file.
    // A PID namespace should be created to re-parent and reap all child processes.
    flags |= O_CLOEXEC;
  }
  live_fd = open(subdir_live_file.c_str(), flags, 0644);
  if (live_fd == -1) {
    std::cerr << "open " << subdir_live_file << ": " << strerror(errno) << std::endl;
    return false;
  }

  // We can safely release the global handle now that we hold a live_fd
  (void)close(ffd);

  // The fuse-waked process takes an input file containing visible files, json formatted.
  JAST for_daemon(JSON_OBJECT);
  auto &vis = for_daemon.add("visible", JSON_ARRAY);
  for (auto &s : visible) vis.add(std::move(s));

  std::ofstream ijson(visibles_path);
  ijson << for_daemon;
  if (ijson.fail()) {
    std::cerr << "write " << visibles_path << ": " << strerror(errno) << std::endl;
    return false;
  }
  ijson.close();
  return true;
}

bool daemon_client::disconnect(std::string &result) {
  // Cause the daemon_output_path to be generated (this write will fail)
  (void)!write(live_fd, "x", 1);  // the ! convinces older gcc that it's ok to ignore the write
  (void)fsync(live_fd);

  // Read the output file
  std::ifstream ifs(output_path);
  result.assign((std::istreambuf_iterator<char>(ifs)), (std::istreambuf_iterator<char>()));
  if (ifs.fail()) {
    std::cerr << "read " << output_path << ": " << strerror(errno) << std::endl;
    return false;
  }
  ifs.close();
  return true;
}
