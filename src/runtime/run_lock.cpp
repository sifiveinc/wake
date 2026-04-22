/*
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

#include "run_lock.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <sstream>

#include "util/mkdir_parents.h"

// Generate lock file path for a given run_id.
static std::string run_lock_path(long run_id) {
  return ".wake/locks/run_" + std::to_string(run_id) + ".lock";
}

// Returns true if lock acquired, false otherwise (sets errno).
static bool acquire_lock(int fd, bool wait) {
  struct flock fl;
  memset(&fl, 0, sizeof(fl));
  fl.l_type = F_WRLCK;
  fl.l_whence = SEEK_SET;
  fl.l_start = 0;
  fl.l_len = 0;  // 0 = entire file
  return fcntl(fd, wait ? F_SETLKW : F_SETLK, &fl) == 0;
}

// Release fcntl lock on file descriptor
static void release_lock(int fd) {
  struct flock fl;
  memset(&fl, 0, sizeof(fl));
  fl.l_type = F_UNLCK;
  fl.l_whence = SEEK_SET;
  fl.l_start = 0;
  fl.l_len = 0;
  fcntl(fd, F_SETLK, &fl);
}

// Write current PID to file descriptor
static bool write_pid_fd(int fd) {
  static_assert(sizeof(pid_t) <= sizeof(long), "pid_t must fit in long");
  char buf[32];
  int len = snprintf(buf, sizeof(buf), "%ld\n", (long)getpid());
  if (len <= 0 || len >= (int)sizeof(buf)) {
    return false;
  }
  for (int off = 0; off < len;) {
    auto n = write(fd, buf + off, len - off);
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    off += n;
  }
  return true;
}

template <typename R = RunLock, typename... Args>
static auto make_error(Args&&... args) {
  std::stringstream ss;
  (ss << ... << std::forward<Args>(args));

  return wcl::make_error<R, std::string>(ss.str());
}

template <typename R = RunLock, typename... Args>
static auto make_errno(Args&... args) {
  return make_error<R>(std::forward<Args>(args)..., ": ", strerror(errno));
}

// RunLock implementation

RunLock::RunLock(wcl::unique_fd&& fd, std::string path)
    : fd(std::move(fd)), path(std::move(path)) {}

RunLock::LockAcquireRetTy RunLock::create_and_acquire(long run_id, bool wait) {
  // Ensure lock directory exists
  mkdir_with_parents(".wake/locks", 0755);

  std::string lock_path = run_lock_path(run_id);
  auto fd = wcl::unique_fd::open(lock_path.c_str(), O_CREAT | O_CLOEXEC | O_RDWR, 0644);
  if (!fd) {
    errno = fd.error();
    return make_errno("failed to create run lock '", lock_path, "'");
  }

  if (!write_pid_fd(fd->get())) {
    auto err = make_errno("failed to write pid to run lock '", lock_path, "'");
    fd->close();
    return err;
  }

  if (!acquire_lock(fd->get(), wait)) {
    int saved_errno = errno;
    fd->close();
    unlink(lock_path.c_str());
    errno = saved_errno;
    return make_errno("failed to acquire own run lock '", lock_path, "'");
  }

  return wcl::result_value<std::string>(RunLock{std::move(*fd), std::move(lock_path)});
}

void RunLock::unlink_file() { ::unlink(path.c_str()); }

// RunLockProbe implementation

namespace RunLockProbe {

wcl::result<bool, std::string> probe_and_cleanup_if_dead(long run_id, int64_t start_time,
                                                         int64_t current_time) {
  std::string lock_path = run_lock_path(run_id);
  int fd = ::open(lock_path.c_str(), O_RDWR | O_CLOEXEC);

  if (fd < 0) {
    if (errno == ENOENT) {
      // No lock file! Conservatively consider dead only if started > 24h ago.
      constexpr int64_t dead_threshold_ns = 24LL * 60 * 60 * 1000000000LL;
      return wcl::result_value<std::string>(current_time - start_time > dead_threshold_ns);
    }
    return make_errno<bool>("failed to open run lock ", run_id);
  }

  // Try to acquire the lock non-blocking
  if (acquire_lock(fd, false)) {
    // Lock acquired - process is dead
    unlink(lock_path.c_str());  // remove while locked
    release_lock(fd);
    ::close(fd);
    return wcl::result_value<std::string>(true);
  }
  if (errno != EAGAIN && errno != EACCES) {
    ::close(fd);
    return make_errno<bool>("failed to probe run lock ", run_id);
  }

  ::close(fd);
  return wcl::result_value<std::string>(false);
}

}  // namespace RunLockProbe
