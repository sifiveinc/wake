/* Wake FUSE daemon lifecycle manager
 *
 * Copyright 2024 SiFive, Inc.
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

#ifndef DAEMON_MANAGER_H
#define DAEMON_MANAGER_H

#include <string>

// Manages the lifecycle of the fuse-waked daemon from the main wake process.
// This ensures a daemon is running for the duration of the wake build and
// handles the case where another daemon may already be running.
class FuseDaemonManager {
 public:
  // Construct a manager for the given workspace directory.
  // The daemon will be mounted at {workspace}/.fuse/{uid}.{gid}/
  explicit FuseDaemonManager(const std::string &workspace_dir);

  // Destructor releases our hold on the daemon, allowing it to exit
  // after its linger timeout if no other clients are connected.
  ~FuseDaemonManager();

  // Non-copyable
  FuseDaemonManager(const FuseDaemonManager &) = delete;
  FuseDaemonManager &operator=(const FuseDaemonManager &) = delete;

  // Start the daemon if not already running, or connect to existing daemon.
  // Returns true on success, false on failure.
  // On success, the daemon will remain alive at least until this object
  // is destroyed.
  bool ensure_daemon_running();

  // Check if a daemon is currently running and accessible.
  bool is_daemon_alive() const;

  // Get the mount path where the fuse filesystem is mounted.
  const std::string &get_mount_path() const { return mount_path_; }

 private:
  std::string workspace_dir_;
  std::string mount_path_;
  std::string executable_;
  std::string is_running_path_;
  int global_fd_;  // File descriptor that keeps daemon alive while open
};

#endif  // DAEMON_MANAGER_H
