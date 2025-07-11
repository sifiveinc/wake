/* Linux namespace and mount ops for wakebox
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

#ifdef __linux__

#include "namespace.h"

#include <errno.h>
#include <fcntl.h>
#include <sched.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <fstream>
#include <iostream>

#include "fuse.h"
#include "json/json5.h"
#include "util/mkdir_parents.h"

// Location in the parent namespace to base the new root on.
static const std::string root_mount_prefix = "/tmp/.wakebox-mount";

// Path to place a squashfs mount before it's moved to the real mountpoint.
// While this location will be mounted-over, it will be uncovered when we do the move.
// Must not hide 'root_mount_prefix'.
static const std::string squashfs_staging_location = "/tmp/.wakebox-mount-squashfs";

// Path within a squashfs mount containing where its temporary mount should be moved to.
static const std::string mount_location_data = ".wakebox/mountpoint";

// Path within a squashfs mount containing environment modification data.
static const std::string mounted_environment_location = ".wakebox/environment";

// Path within a squashfs mount containing json description of further required mounts.
static const std::string helper_mounts_location = ".wakebox/mounts";

static void write_file(const char *file, const char *content, int len) {
  int fd;

  fd = open(file, O_WRONLY);
  if (fd < 0) {
    std::cerr << "open " << file << ": " << strerror(errno) << std::endl;
    exit(1);
  }

  if (write(fd, content, len) != len) {
    std::cerr << "write " << file << ": " << strerror(errno) << std::endl;
    exit(1);
  }

  close(fd);
}

static void map_id(const char *file, uint32_t from, uint32_t to) {
  char buf[80];
  snprintf(buf, sizeof(buf), "%u %u 1", from, to);
  write_file(file, buf, strlen(buf));
}

// Inside a user namespace, you are not allowed to separate mounts that you inherit from another
// mount namespace from their child mounts.
// Therefore, when a mount has subdirectories containing other mounts we must be recursive when we
// bind mount.
static bool bind_mount(const std::string &source, const std::string &destination, bool readonly) {
  const char *src = source.c_str();
  const char *dest = destination.c_str();

  if (0 != mount(src, dest, NULL, MS_BIND | MS_REC, NULL)) {
    std::cerr << "bind mount (" << source << " -> " << destination << "): " << strerror(errno)
              << std::endl;
    return false;
  }
  // Re-mount to set destination as read-only.
  // Source filesystem must not have 'MS_NODEV' (a.k.a. 'nodev') set.
  if (readonly) {
    if (0 != mount(src, dest, NULL, MS_BIND | MS_REC | MS_RDONLY | MS_REMOUNT, NULL)) {
      std::cerr << "bind mount (" << source << " -> " << destination << "): " << strerror(errno)
                << std::endl;
      return false;
    }
  }
  return true;
}

static bool validate_mount(const std::string &op, const std::string &source) {
  static const std::vector<std::string> mount_ops{
      // must be sorted
      "bind", "create-dir", "create-file", "squashfs", "tmpfs", "workspace"};

  if (!std::binary_search(mount_ops.begin(), mount_ops.end(), op)) {
    std::cerr << "unknown mount type: '" << op << "'" << std::endl;
    return false;
  }

  if ((op != "bind" && op != "squashfs") && !source.empty()) {
    std::cerr << "mount: " << op << " can not have 'source' option" << std::endl;
    return false;
  }

  return true;
}

// The pivot_root syscall has no glibc wrapper.
static int pivot_root(const char *new_root, const char *put_old) {
#ifndef __NR_pivot_root
#error 'pivot_root' syscall number missing from <sys/syscall.h>
#endif
  return syscall(__NR_pivot_root, new_root, put_old);
}

/* Many systems have an ancient manpage entry for pivot_root,
 * See 2019-era docs at: https://lwn.net/Articles/800381/
 *
 * new_root and put_old may be the same directory.
 * In particular, the following sequence allows a pivot-root operation without needing
 * to create and remove a temporary directory:
 *
 *   chdir(new_root);
 *   pivot_root(".", ".");
 *   umount2(".", MNT_DETACH);
 *
 * This sequence succeeds because the pivot_root() call stacks the old root mount point
 * on top of the new root mount point at /. At that point, the calling process's root
 * directory and current working directory refer to the new root mount point (new_root).
 * During the subsequent umount() call, resolution of "." starts with new_root and then
 * moves up the list of mounts stacked at /, with the result that old root mount point
 * is unmounted.
 */
static bool do_pivot(const std::string &newroot) {
  // The pivot_root syscall requires that the new root location is a mountpoint.
  // Bind mount the new root onto itself to ensure this.
  if (!bind_mount(newroot, newroot, false)) {
    return false;
  }
  if (0 != chdir(newroot.c_str())) {
    std::cerr << "chdir: " << strerror(errno) << std::endl;
    return false;
  }
  if (0 != pivot_root(".", ".")) {
    std::cerr << "pivot_root(\".\", \".\"): " << strerror(errno) << std::endl;
    return false;
  }
  if (0 != umount2(".", MNT_DETACH)) {
    std::cerr << "umount2: " << strerror(errno) << std::endl;
    return false;
  }
  return true;
}

static bool mount_tmpfs(const std::string &destination) {
  if (0 != mount("tmpfs", destination.c_str(), "tmpfs", 0UL, NULL)) {
    std::cerr << "tmpfs mount (" << destination << "): " << strerror(errno) << std::endl;
    return false;
  }
  return true;
}

static bool equal_dev_ids(dev_t a, dev_t b) { return major(a) == major(b) && minor(a) == minor(b); }

static bool do_squashfuse_mount(const std::string &source, const std::string &mountpoint) {
  // The squashfuse executable doesn't give a clear error message when the file is missing.
  if (access(source.c_str(), R_OK | F_OK) != 0) {
    std::cerr << "squashfs mount ('" << source << "'): " << strerror(errno) << std::endl;
    return false;
  }

  int err = mkdir_with_parents(mountpoint, 0555);
  if (0 != err) {
    std::cerr << "mkdir_with_parents ('" << mountpoint << "'):" << strerror(err) << std::endl;
    return false;
  }

  pid_t pid = fork();
  if (pid == 0) {
    // kernel to send SIGKILL to squashfuse when wakebox terminates
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) == -1) {
      std::cerr << "squashfuse prctl: " << strerror(errno) << std::endl;
      exit(1);
    }
    execlp("squashfuse", "squashfuse", "-f", source.c_str(), mountpoint.c_str(), NULL);
    std::cerr << "execlp squashfuse: " << strerror(errno) << std::endl;
    exit(1);
  }

  // Wait for the mount to exist before we continue by checking if the
  // stat() device id or the inode changes.
  struct stat before;
  if (0 != stat(mountpoint.c_str(), &before)) {
    std::cerr << "stat (" << mountpoint << "): " << strerror(errno) << std::endl;
    return false;
  }

  for (int i = 0; i < 10; i++) {
    struct stat after;
    if (0 != stat(mountpoint.c_str(), &after)) {
      std::cerr << "stat (" << mountpoint << "): " << strerror(errno) << std::endl;
      return false;
    }

    if (!equal_dev_ids(before.st_dev, after.st_dev) || (before.st_ino != after.st_ino)) {
      return true;
    } else {
      int ms = 10 << i;  // 10ms * 2^i
      struct timespec delay;
      delay.tv_sec = ms / 1000;
      delay.tv_nsec = (ms % 1000) * INT64_C(1000000);
      nanosleep(&delay, nullptr);
    }
  }

  std::cerr << "squashfs mount failed: " << source << std::endl;
  return false;
}

static bool squashfs_helper_mounts(const std::string &squashfs_base_path,
                                   const std::string &mount_prefix) {
  // Check if there is a helper mounts file to parse.
  std::string path = squashfs_base_path + "/" + helper_mounts_location;
  std::ifstream helper_mounts_stream(path);
  if (!helper_mounts_stream)
    // Lack of helper mounts is not an error.
    return true;

  const std::string json((std::istreambuf_iterator<char>(helper_mounts_stream)),
                         (std::istreambuf_iterator<char>()));
  if (helper_mounts_stream.fail()) {
    std::cerr << "read " << path << ": " << strerror(errno) << std::endl;
    return false;
  }

  JAST jast;
  if (!jast.parse(json, std::cerr, jast)) return false;

  // While this json format looks similar to 'struct json_args' it is parsed separately
  // so that they may evolve independently as these helper mounts will be embedded in
  // the squashfs itself.
  for (auto &x : jast.get("mount-ops").children) {
    const std::string &type = x.second.get("type").value;
    const std::string &source = x.second.get("source").value;
    const std::string &destination = x.second.get("destination").value;

    if (type != "bind" && type != "tmpfs" && type != "create-dir") {
      std::cerr << "Unexpected mount type '" << type << "' in " << path << std::endl;
      return false;
    }

    const std::string target = mount_prefix + destination;
    if (type == "bind" && !bind_mount(source, target, false)) return false;
    if (type == "tmpfs" && !mount_tmpfs(target)) return false;
  }
  return true;
}

static bool move_squashfs_mount(const std::string &mount_prefix, const std::string &source,
                                std::string &mounted_at) {
  // Read the file that specifies the correct mountpoint.
  std::string contents;
  std::ifstream mount_info(squashfs_staging_location + "/" + mount_location_data);
  if (mount_info.is_open()) std::getline(mount_info, contents);
  mount_info.close();
  if (contents.empty()) {
    std::cerr << "squashfs (" << source
              << "): no destination provided and "
                 " '.wakebox/mountpoint' did not contain a mountpoint on first line."
              << std::endl;
    return false;
  }

  // Make the new mountpoint and any parent directories, it's ok if it already exists.
  std::string new_target = mount_prefix + contents;
  int err = mkdir_with_parents(new_target, 0777);
  if (0 != err) {
    std::cerr << "mkdir_with_parents ('" << new_target << "'):" << strerror(err) << std::endl;
    return errno;
  }

  // Move the staging mount to the correct mountpoint.
  if (0 != mount(squashfs_staging_location.c_str(), new_target.c_str(), "", MS_MOVE, 0UL)) {
    std::cerr << "move mount (" << squashfs_staging_location << ", " << new_target
              << "): " << strerror(errno) << std::endl;
    return false;
  }
  mounted_at = new_target;
  return true;
}

// Collect any squashfs-provided environment modifications.
// They should be sh-compatible files that can be sourced.
static void add_squashfs_environment(const std::string &mount_prefix,
                                     const std::string &squashfs_mountpoint,
                                     std::vector<std::string> &environments) {
  auto s = squashfs_mountpoint + "/" + mounted_environment_location;
  std::ifstream env_file(s);
  if (env_file) {
    std::string without_prefix = squashfs_mountpoint.substr(mount_prefix.length());
    environments.push_back(without_prefix + "/" + mounted_environment_location);
  }
}

static bool squashfs_mount(const std::string &source, const std::string &mount_prefix,
                           const std::string &dest_from_json, const std::string &dest_with_prefix,
                           std::vector<std::string> &environments) {
  std::string mounted_at = dest_with_prefix;

  // If we have a destination use it directly. otherwise use a staging mount and move it.
  if (!dest_from_json.empty()) {
    if (!do_squashfuse_mount(source, dest_with_prefix)) {
      return false;
    }
  } else {
    if (!do_squashfuse_mount(source, squashfs_staging_location)) {
      return false;
    }

    // mutates 'mounted_at'
    if (!move_squashfs_mount(mount_prefix, squashfs_staging_location, mounted_at)) {
      return false;
    }
  }

  // The squashfs can specify additional mounts, for example: /proc -> /proc
  if (!squashfs_helper_mounts(mounted_at, mount_prefix)) return false;

  // The squashfs can provide environment modifications
  add_squashfs_environment(mount_prefix, mounted_at, environments);

  return true;
}

static bool create_dir(const std::string &dest) {
  int ret = mkdir_with_parents(dest, 0777);
  if (0 != ret) {
    std::cerr << "mkdir_with_parents (" << dest << "): " << strerror(ret) << std::endl;
    return false;
  }
  return true;
}

static bool create_file(const std::string &dest) {
  int fd = creat(dest.c_str(), 0777);
  if (fd < 0) {
    std::cerr << "creat (" << dest << "): " << strerror(errno) << std::endl;
    return false;
  }
  if (0 != close(fd)) {
    std::cerr << "close (" << dest << "): " << strerror(errno) << std::endl;
    return false;
  }
  return true;
}

// Do the mounts specified in the parsed input json.
// The input/caller responsibility to ensure that the mountpoint exists,
// that the platform supports the mount type/options, and to correctly order
// the layered mounts.
bool do_mounts(const std::vector<mount_op> &mount_ops, const std::string &fuse_mount_path,
               std::vector<std::string> &environments) {
  std::string mount_prefix;
  for (auto &x : mount_ops) {
    if (x.destination == "/") {
      // All mount ops from here onward will have a prefixed destination.
      // The prefix will be pivoted to after the final mount op.
      mount_prefix = root_mount_prefix;
      // Re-use if it already exists.
      if (0 != mkdir(mount_prefix.c_str(), 0555) && errno != EEXIST) {
        std::cerr << "mkdir (" << mount_prefix << "): " << strerror(errno) << std::endl;
        return false;
      }
    }
    const std::string target = mount_prefix + x.destination;

    if (!validate_mount(x.type, x.source)) return false;

    if (x.type == "bind" && !bind_mount(x.source, target, x.read_only)) return false;

    if (x.type == "workspace" && !bind_mount(fuse_mount_path, target, false)) return false;

    if (x.type == "tmpfs" && !mount_tmpfs(target)) return false;

    if (x.type == "create-dir" && !create_dir(target)) return false;

    if (x.type == "create-file" && !create_file(target)) return false;

    if (x.type == "squashfs" &&
        !squashfs_mount(x.source, mount_prefix, x.destination, target, environments))
      return false;
  }

  if (!mount_prefix.empty() && !do_pivot(mount_prefix)) return false;

  return true;
}

bool get_workspace_dir(const std::vector<mount_op> &mount_ops,
                       const std::string &host_workspace_dir, std::string &out) {
  for (auto &x : mount_ops) {
    if (x.type == "workspace") {
      out = x.destination;
      // convert a workspace relative path into absolute path
      if (out.at(0) != '/') out = host_workspace_dir + "/" + out;
      return true;
    }
  }
  return false;
}

bool setup_user_namespaces(int id_user, int id_group, bool isolate_network,
                           const std::string &hostname, const std::string &domainname) {
  // If we have permissions in the current mount namespace, create a fresh /proc mount.
  // This can avoid issues where the mount propagation setting for /proc in the
  // current namespace is set to MS_UNBINDABLE.
  int err = mount("proc", "/proc", "proc", 0, nullptr);
  if (err == -1 && errno != EPERM && errno != EBUSY) {
    std::cerr << "pre namespace setup: mount('proc', '/proc', 'proc', 0, nullptr): "
              << strerror(errno) << std::endl;
    exit(1);
  }

  uid_t real_euid = geteuid();
  gid_t real_egid = getegid();

  uid_t euid = id_user;
  gid_t egid = id_group;

  int flags = CLONE_NEWNS | CLONE_NEWUSER;

  if (!hostname.empty() || !domainname.empty()) flags |= CLONE_NEWUTS;

  if (isolate_network) flags |= CLONE_NEWNET;

  // Enter a new mount namespace we can control
  if (0 != unshare(flags)) {
    std::cerr << "unshare: " << strerror(errno) << std::endl;
    return false;
  }

  if (!hostname.empty() && sethostname(hostname.c_str(), hostname.length()) != 0)
    std::cerr << "sethostname(" << hostname << "): " << strerror(errno) << std::endl;

  if (!domainname.empty() && setdomainname(domainname.c_str(), domainname.length()) != 0)
    std::cerr << "setdomainname(" << domainname << "): " << strerror(errno) << std::endl;

  // Map our UID to either our original UID or root
  write_file("/proc/self/setgroups", "deny", 4);
  map_id("/proc/self/uid_map", euid, real_euid);
  map_id("/proc/self/gid_map", egid, real_egid);

  // Raise the loopback device.
  if (isolate_network) {
    // This uses 'ip' from the host system, not from a modified mount table.
    // We should be able to assume that 'lo' is the loopback device, as there
    // has been no oportunity to modify it since when the net namespace was created.
    if (0 != system("ip link set lo up")) {
      std::cerr << "Failed to raise loopback network interface" << std::endl;
      return false;
    }
  }

  return true;
}

[[noreturn]] int pidns_init(void *arg) {
  // We should be in a new PID namespace now, label the process in 'ps'.
  prctl(PR_SET_NAME, "wb-pid-ns", 0, 0, 0);
  // This process should terminate if the parent process exits.
  if (prctl(PR_SET_PDEATHSIG, SIGKILL) == -1) {
    std::cerr << "pidns_init prctl: " << strerror(errno) << std::endl;
    exit(1);
  }

  // A fresh mount of procfs over the previous namespace's /proc.
  // which we can only do as we're already in a mount namespace.
  // Note that this is not a bind mount of an existing /proc.
  // The new procfs will have the correct data for the new PID namespace, rather
  // than leaking process ids from the exterior namespace.
  int err = mount("proc", "/proc", "proc", 0, nullptr);
  if (err == -1) {
    std::cerr << "new pid namespace: mount('proc', '/proc', 'proc', 0, nullptr): "
              << strerror(errno) << std::endl;
    exit(1);
  }

  pid_t pid = fork();
  if (pid == 0) {
    // Execute the user-specified command.
    const auto *args = reinterpret_cast<const pidns_args *>(arg);
    err = execve_wrapper(args->command, args->environment);
    std::cerr << "execve " << args->command[0] << ": " << strerror(err) << std::endl;
    exit(1);
  }

  int user_cmd_status;
  waitpid(0, &user_cmd_status, 0);

  // Wait for child processes, we are init(1) in this PID namespace.
  int child_status;
  while ((pid = waitpid(-1, &child_status, 0)) != 0) {
    if (pid == -1) {
      if (errno == ECHILD) {
        break;  // all children have terminated
      }
      std::cerr << "waitpid: " << strerror(errno) << std::endl;
      exit(1);
    }
  }

  if (WIFEXITED(user_cmd_status)) {
    exit(WEXITSTATUS(user_cmd_status));
  }
  exit(-WTERMSIG(user_cmd_status));
}

[[noreturn]] void exec_in_pidns(pidns_args &nsargs) {
  alignas(16) uint8_t stack_buf[4096];
  void *child_stack = reinterpret_cast<void *>(stack_buf + sizeof(child_stack));

  // Create a new thread in a PID namespace, calling pidns_init.
  pid_t child_pid = clone(pidns_init, child_stack, CLONE_NEWPID | SIGCHLD, &nsargs);
  if (child_pid == -1) {
    std::cerr << "clone " << nsargs.command[0] << ": " << strerror(errno) << std::endl;
    exit(1);
  }

  // While we wait, the subdir_live_file for the fuse daemon will be held open.
  int status = 1;
  if (waitpid(child_pid, &status, 0) == -1) {
    std::cerr << "waitpid (clone) " << nsargs.command[0] << std::endl;
    exit(1);
  }

  if (WIFEXITED(status)) {
    exit(WEXITSTATUS(status));
  } else {
    exit(-WTERMSIG(status));
  }
}
#endif
