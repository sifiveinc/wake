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

#ifndef RUN_LOCK_H
#define RUN_LOCK_H

#include <string>

#include "wcl/result.h"
#include "wcl/unique_fd.h"

// RAII wrapper for run lock files.
// Runs keep write lock on these while live.
class RunLock {
 public:
  using LockAcquireRetTy = wcl::result<RunLock, std::string>;

  // Create and acquire a lock for the given run_id.
  static LockAcquireRetTy create_and_acquire(long run_id, bool wait);

  // Move-only
  RunLock(RunLock&& other) noexcept = default;
  RunLock& operator=(RunLock&& other) noexcept = default;
  RunLock(const RunLock&) = delete;
  RunLock& operator=(const RunLock&) = delete;

  // Close the file descriptor for this lock.
  // Use unlink_file() to remove it on graceful exit path.
  ~RunLock() = default;

  // Explicitly unlink the lock file (call on successful run completion)
  void unlink_file();

 private:
  RunLock(wcl::unique_fd&& fd, std::string path);

  wcl::unique_fd fd;
  std::string path;
};

// Utility functions for probing other processes' run locks
namespace RunLockProbe {
// Check if a run's lock is held by a live process.
// Returns true if dead, false if alive.
// On error, return string message.  Consider alive.
// If dead and the lock file exists, it will be cleaned up.
wcl::result<bool, std::string> probe_and_cleanup_if_dead(long run_id, int64_t start_time,
                                                         int64_t current_time);
}  // namespace RunLockProbe

#endif
