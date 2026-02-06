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

#ifndef FUSE_H
#define FUSE_H

#include <optional>
#include <string>
#include <vector>

#include "namespace.h"
#include "wcl/optional.h"

struct daemon_client {
  // Path to the fuse-waked daemon executable.
  const std::string executable;
  // Location that the fuse filesystem is mounted.
  const std::string mount_path;
  // Subdir in the fuse filesystem mount that will be used by this wakebox's job.
  const std::string mount_subdir;
  // Path that the fuse daemon will write result metadata to.
  const std::string output_path;
  // File that exists when the daemon is running/active.
  const std::string is_running_path;
  // File held open by each child of wakebox. When all children close it,
  // the daemon releases the resources for that job.
  const std::string subdir_live_file;
  // JSON input file to the fuse daemon, listing which files should be visible.
  const std::string visibles_path;

  daemon_client(const std::string &base_dir);

  bool connect(std::vector<std::string> &visible, bool close_live_file);
  bool disconnect(std::string &result);

 protected:
  // file descriptor for opened 'subdir_live_file'
  int live_fd;
};

struct json_args {
  std::vector<std::string> command;
  std::optional<int> command_timeout;  // timeout in seconds.
  std::vector<std::string> environment;
  std::vector<std::string> visible;
  std::string directory;
  std::string stdin_file;

  std::string hostname;
  std::string domainname;
  bool isolate_network;
  bool isolate_pids;

  int userid;
  int groupid;

  std::vector<mount_op> mount_ops;

  json_args() : isolate_network(false), isolate_pids(false), userid(0), groupid(0) {}
};

struct fuse_args : public json_args {
  const std::string working_dir;    // the original directory that this process was invoked from
  std::string command_running_dir;  // current working dir of the command when it executes
  bool use_stdin_file;
  daemon_client daemon;

  fuse_args(const std::string &cwd, bool use_stdin_file)
      : working_dir(cwd), use_stdin_file(use_stdin_file), daemon(cwd) {}
};

bool json_as_struct(const std::string &json, json_args &result);

bool run_in_fuse(fuse_args &args, int &retcode, std::string &result_json);

int execve_wrapper(const std::vector<std::string> &command,
                   const std::vector<std::string> &environment);

#endif
