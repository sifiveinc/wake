/* Wake FUSE driver to capture inputs/outputs
 *
 * Copyright 2019 SiFive, Inc.
 * Copyright 2001-2007  Miklos Szeredi <miklos@szeredi.hu>
 * Copyright 2011       Sebastian Pipping <sebastian@pipping.org>
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
#define FUSE_USE_VERSION 26

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <fuse.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include <map>
#include <set>
#include <sstream>
#include <string>

#include "compat/nofollow.h"
#include "compat/utimens.h"
#include "json/json5.h"
#include "util/execpath.h"
#include "util/unlink.h"

#define MAX_JSON (128 * 1024 * 1024)

// We ensure STDIN is /dev/null, so this is a safe sentinel value for open files
#define BAD_FD STDIN_FILENO

#ifndef __APPLE__
// Attempt to lazily unmount a stale mount point (fails silently if not mounted)
static void try_unmount_stale(const char *path, bool debug) {
  std::string cmd = "fusermount -uz '" + std::string(path) + "' 2>/dev/null";
  if (system(cmd.c_str()) == 0 && debug) {
    fprintf(stderr, "Unmounted stale mount at %s\n", path);
  }
}
#endif

// How long to wait for a new client to connect before the daemon exits
static int linger_timeout;
static std::set<std::string> hardlinks = {};

// How to retry umount while quitting
// (2^8-1)*100ms = 25.5s worst-case quit time
#define QUIT_RETRY_MS 100
#define QUIT_RETRY_ATTEMPTS 8

struct Job {
  std::set<std::string> files_visible;
  std::set<std::string> files_read;
  std::set<std::string> files_wrote;
  std::string json_in;
  std::string json_out;
  long ibytes, obytes;
  int json_in_uses;
  int json_out_uses;
  int uses;

  Job() : ibytes(0), obytes(0), json_in_uses(0), json_out_uses(0), uses(0) {}

  void parse();
  void dump();

  bool is_writeable(const std::string &path);
  bool is_readable(const std::string &path);
  bool is_visible(const std::string &path);
  bool should_erase() const;
};

void Job::parse() {
  JAST jast;
  std::stringstream s;
  if (!JAST::parse(json_in, s, jast)) {
    fprintf(stderr, "Parse error: %s\n", s.str().c_str());
    return;
  }

  // We only need to make the relative paths visible; absolute paths are already
  files_visible.clear();
  for (auto &x : jast.get("visible").children)
    if (!x.second.value.empty() && x.second.value[0] != '/')
      files_visible.insert(std::move(x.second.value));
}

void Job::dump() {
  if (!json_out.empty()) return;

  bool first;
  std::stringstream s;

  s << "{\"ibytes\":" << ibytes << ",\"obytes\":" << obytes << ",\"inputs\":[";

  for (auto &x : files_wrote) files_read.erase(x);

  first = true;
  for (auto &x : files_read) {
    s << (first ? "" : ",") << "\"" << json_escape(x) << "\"";
    first = false;
  }

  s << "],\"outputs\":[";

  first = true;
  const std::string prefix = ".fuse_hidden";
  for (auto &x : files_wrote) {
    // files prefixed with .fuse_hidden are implementation details of libfuse
    // and should not be treated as outputs.
    // see: https://github.com/libfuse/libfuse/blob/fuse-3.10.3/include/fuse.h#L161-L177
    size_t start = 0;
    size_t lastslash = x.rfind("/");
    if (lastslash != std::string::npos) start = lastslash + 1;
    if (x.compare(start, prefix.length(), prefix) == 0) continue;

    s << (first ? "" : ",") << "\"" << json_escape(x) << "\"";
    first = false;
  }

  s << "]}" << std::endl;

  json_out = s.str();
}

struct Context {
  std::map<std::string, Job> jobs;
  int rootfd, uses;
  Context() : jobs(), rootfd(-1), uses(0) {}
  bool should_exit() const;
};

bool Context::should_exit() const { return 0 == uses && jobs.empty(); }

static Context context;

bool Job::is_visible(const std::string &path) {
  if (files_visible.find(path) != files_visible.end()) return true;

  auto i = files_visible.lower_bound(path + "/");
  return i != files_visible.end() && i->size() > path.size() && (*i)[path.size()] == '/' &&
         0 == i->compare(0, path.size(), path);
}

bool Job::is_writeable(const std::string &path) {
  return files_wrote.find(path) != files_wrote.end();
}

bool Job::is_readable(const std::string &path) { return is_visible(path) || is_writeable(path); }

bool Job::should_erase() const { return 0 == uses && 0 == json_in_uses && 0 == json_out_uses; }

static std::pair<std::string, std::string> split_key(const char *path) {
  const char *end = strchr(path + 1, '/');
  if (end) {
    return std::make_pair(std::string(path + 1, end), std::string(end + 1));
  } else {
    return std::make_pair(std::string(path + 1), std::string("."));
  }
}

struct Special {
  std::map<std::string, Job>::iterator job;
  char kind;
  Special() : job(), kind(0) {}
  operator bool() const { return kind; }
};

static Special is_special(const char *path) {
  Special out;

  if (path[0] != '/' || path[1] != '.' || !path[2] || path[3] != '.' || !path[4]) return out;

  auto it = context.jobs.find(path + 4);
  switch (path[2]) {
    case 'f':
      out.kind = strcmp(path + 4, "fuse-waked") ? 0 : 'f';
      return out;
    case 'o':
      if (it != context.jobs.end() && !it->second.json_out.empty()) {
        out.kind = path[2];
        out.job = it;
      }
      return out;
    case 'i':
    case 'l':
      if (it != context.jobs.end()) {
        out.kind = path[2];
        out.job = it;
      }
      return out;
    default:
      return out;
  }
}

// If exit_attempts is > 0, we are in the impossible-to-stop process of exiting.
// On a clean shutdown, exit_attempts will only ever be increased if context.should_exit() is true.
static volatile int exit_attempts = 0;

// You must make context.should_exit() false BEFORE calling cancel_exit.
// Return of 'true' guarantees the process will not exit
static bool cancel_exit() {
  // It's too late to stop exiting if even one attempt has been made
  // The umount process is asynchronous and outside our ability to stop
  if (exit_attempts > 0) return false;

  struct itimerval retry;
  memset(&retry, 0, sizeof(retry));
  setitimer(ITIMER_REAL, &retry, 0);

  return true;
}

static void schedule_exit() {
  struct itimerval retry;
  memset(&retry, 0, sizeof(retry));
  if (exit_attempts == 0) {
    // Wait a while for new clients before the daemon exits.
    // In particular, wait longer than the client waits to reach us.
    retry.it_value.tv_sec = linger_timeout;
  } else {
    // When trying to quit, be aggressive to get out of the way.
    // A new daemon might need us gone so it can start.
    long retry_ms = QUIT_RETRY_MS << (exit_attempts - 1);
    retry.it_value.tv_sec = retry_ms / 1000;
    retry.it_value.tv_usec = (retry_ms % 1000) * 1000;
  }
  setitimer(ITIMER_REAL, &retry, 0);
}

static const char *trace_out(int code) {
  static char buf[20];
  if (code < 0) {
    return strerror(-code);
  } else {
    snprintf(&buf[0], sizeof(buf), "%d", code);
    return &buf[0];
  }
}

static int wakefuse_getattr(const char *path, struct stat *stbuf) {
  if (auto s = is_special(path)) {
    int res = fstat(context.rootfd, stbuf);
    if (res == -1) res = -errno;
    stbuf->st_nlink = 1;
    stbuf->st_ino = 0;
    switch (s.kind) {
      case 'i':
        stbuf->st_mode = S_IFREG | 0644;
        stbuf->st_size = s.job->second.json_in.size();
        return res;
      case 'o':
        stbuf->st_mode = S_IFREG | 0444;
        stbuf->st_size = s.job->second.json_out.size();
        return res;
      case 'l':
        stbuf->st_mode = S_IFREG | 0644;
        stbuf->st_size = 0;
        return res;
      case 'f':
        stbuf->st_mode = S_IFREG | 0444;
        stbuf->st_size = 0;
        return res;
      default:
        return -ENOENT;  // unreachable
    }
  }

  auto key = split_key(path);
  if (key.first.empty()) {
    int res = fstat(context.rootfd, stbuf);
    stbuf->st_nlink = 1;
    stbuf->st_ino = 0;
    if (res == -1) res = -errno;
    return res;
  }

  auto it = context.jobs.find(key.first);
  if (it == context.jobs.end()) return -ENOENT;

  if (key.second == ".") {
    int res = fstat(context.rootfd, stbuf);
    stbuf->st_nlink = 1;
    stbuf->st_ino = 0;
    if (res == -1) res = -errno;
    return res;
  }

  if (!it->second.is_readable(key.second)) return -ENOENT;

  int res = fstatat(context.rootfd, key.second.c_str(), stbuf, AT_SYMLINK_NOFOLLOW);
  if (res == -1) res = -errno;
  return res;
}

static int wakefuse_getattr_trace(const char *path, struct stat *stbuf) {
  int out = wakefuse_getattr(path, stbuf);
  fprintf(stderr, "getattr(%s) = %s\n", path, trace_out(out));
  return out;
}

static int wakefuse_access(const char *path, int mask) {
  if (auto s = is_special(path)) {
    switch (s.kind) {
      case 'o':
      case 'i':
        return (mask & X_OK) ? -EACCES : 0;
      default:
        return (mask & (X_OK | W_OK)) ? -EACCES : 0;
    }
  }

  auto key = split_key(path);
  if (key.first.empty()) return 0;

  auto it = context.jobs.find(key.first);
  if (it == context.jobs.end()) return -ENOENT;

  if (key.second == ".") return 0;

  if (!it->second.is_readable(key.second)) return -ENOENT;

  int res = faccessat(context.rootfd, key.second.c_str(), mask, 0);
  if (res == -1) return -errno;

  return 0;
}

static int wakefuse_access_trace(const char *path, int mask) {
  int out = wakefuse_access(path, mask);
  fprintf(stderr, "access(%s, %d) = %s\n", path, mask, trace_out(out));
  return out;
}

static int wakefuse_readlink(const char *path, char *buf, size_t size) {
  if (is_special(path)) return -EINVAL;

  auto key = split_key(path);
  if (key.first.empty()) return -EINVAL;

  auto it = context.jobs.find(key.first);
  if (it == context.jobs.end()) return -ENOENT;

  if (key.second == ".") return -EINVAL;

  if (!it->second.is_readable(key.second)) return -ENOENT;

  int res = readlinkat(context.rootfd, key.second.c_str(), buf, size - 1);
  if (res == -1) return -errno;

  buf[res] = '\0';
  it->second.files_read.insert(std::move(key.second));
  return 0;
}

static int wakefuse_readlink_trace(const char *path, char *buf, size_t size) {
  int out = wakefuse_readlink(path, buf, size);
  fprintf(stderr, "readlink(%s, %lu) = %s\n", path, (unsigned long)size, trace_out(out));
  return out;
}

static int wakefuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset,
                            struct fuse_file_info *fi) {
  (void)offset;
  (void)fi;

  if (is_special(path)) return -ENOTDIR;

  auto key = split_key(path);
  if (key.first.empty()) {
    filler(buf, ".f.fuse-waked", 0, 0);
    for (auto &job : context.jobs) {
      filler(buf, job.first.c_str(), 0, 0);
      filler(buf, (".l." + job.first).c_str(), 0, 0);
      filler(buf, (".i." + job.first).c_str(), 0, 0);
      if (!job.second.json_out.empty()) filler(buf, (".o." + job.first).c_str(), 0, 0);
    }
    return 0;
  }

  auto it = context.jobs.find(key.first);
  if (it == context.jobs.end()) return -ENOENT;

  int dfd;
  if (key.second == ".") {
    dfd = dup(context.rootfd);
  } else if (!it->second.is_readable(key.second)) {
    return -ENOENT;
  } else {
    dfd = openat(context.rootfd, key.second.c_str(), O_RDONLY | O_NOFOLLOW | O_DIRECTORY);
  }
  if (dfd == -1) return -errno;

  DIR *dp = fdopendir(dfd);
  if (dp == NULL) {
    int res = -errno;
    (void)close(dfd);
    return res;
  }

  rewinddir(dp);
  struct dirent *de;
  while ((de = readdir(dp)) != NULL) {
    struct stat st;
    memset(&st, 0, sizeof(st));
    st.st_ino = de->d_ino;
    st.st_mode = de->d_type << 12;

    std::string file;
    if (key.second != ".") {
      file += key.second;
      file += "/";
    }
    file += de->d_name;

    if (!it->second.is_readable(file)) {
      // Allow '.' and '..' links in this directory.
      // This directory was earlier checked as visible (for '.') and
      // the parent of a readable directory should also be visible (for '..').
      std::string name(de->d_name);
      if (!(name == "." || name == "..")) continue;
    }

    if (filler(buf, de->d_name, &st, 0)) break;
  }

  (void)closedir(dp);
  return 0;
}

static int wakefuse_readdir_trace(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset,
                                  struct fuse_file_info *fi) {
  int out = wakefuse_readdir(path, buf, filler, offset, fi);
  fprintf(stderr, "readdir(%s, %lld) = %s\n", path, (long long)offset, trace_out(out));
  return out;
}

static int wakefuse_mknod(const char *path, mode_t mode, dev_t rdev) {
  if (is_special(path)) return -EEXIST;

  auto key = split_key(path);
  if (key.first.empty()) return -EEXIST;

  auto it = context.jobs.find(key.first);
  if (it == context.jobs.end()) {
    if (key.second == ".")
      return -EACCES;
    else
      return -ENOENT;
  }

  if (key.second == ".") return -EEXIST;

  if (it->second.is_visible(key.second)) return -EEXIST;

  if (!it->second.is_writeable(key.second)) (void)deep_unlink(context.rootfd, key.second.c_str());

  int res;
  if (S_ISREG(mode)) {
    res = openat(context.rootfd, key.second.c_str(), O_CREAT | O_EXCL | O_WRONLY, mode);
    if (res >= 0) res = close(res);
  } else if (S_ISDIR(mode)) {
    res = mkdirat(context.rootfd, key.second.c_str(), mode);
  } else if (S_ISFIFO(mode)) {
#ifdef __APPLE__
    res = mkfifo(key.second.c_str(), mode);
#else
    res = mkfifoat(context.rootfd, key.second.c_str(), mode);
#endif
  } else {
#ifdef __APPLE__
    res = mknod(key.second.c_str(), mode, rdev);
#else
    res = mknodat(context.rootfd, key.second.c_str(), mode, rdev);
#endif
  }

  if (res == -1) return -errno;

  it->second.files_wrote.insert(std::move(key.second));
  return 0;
}

static int wakefuse_mknod_trace(const char *path, mode_t mode, dev_t rdev) {
  int out = wakefuse_mknod(path, mode, rdev);
  fprintf(stderr, "mknod(%s, 0%o, 0x%lx) = %s\n", path, mode, (unsigned long)rdev, trace_out(out));
  return out;
}

static int wakefuse_create(const char *path, mode_t mode, struct fuse_file_info *fi) {
  if (is_special(path)) return -EEXIST;

  auto key = split_key(path);
  if (key.first.empty()) return -EEXIST;

  if (key.second == "." && key.first.size() > 3 && key.first[0] == '.' && key.first[1] == 'l' &&
      key.first[2] == '.' && key.first[3] != '.') {
    std::string jobid = key.first.substr(3);
    Job &job = context.jobs[jobid];
    ++job.uses;
    if (!cancel_exit()) {
      --job.uses;
      if (job.should_erase()) context.jobs.erase(jobid);
      return -EPERM;
    }
    fi->fh = BAD_FD;
    return 0;
  }

  auto it = context.jobs.find(key.first);
  if (it == context.jobs.end()) {
    if (key.second == ".")
      return -EACCES;
    else
      return -ENOENT;
  }

  if (key.second == ".") return -EEXIST;

  if (it->second.is_visible(key.second)) return -EEXIST;

  if (!it->second.is_writeable(key.second)) (void)deep_unlink(context.rootfd, key.second.c_str());

  int fd = openat(context.rootfd, key.second.c_str(), fi->flags, mode);
  if (fd == -1) return -errno;

  fi->fh = fd;
  it->second.files_wrote.insert(std::move(key.second));
  return 0;
}

static int wakefuse_create_trace(const char *path, mode_t mode, struct fuse_file_info *fi) {
  int out = wakefuse_create(path, mode, fi);
  fprintf(stderr, "create(%s, 0%o) = %s\n", path, mode, trace_out(out));
  return out;
}

static int wakefuse_mkdir(const char *path, mode_t mode) {
  if (is_special(path)) return -EEXIST;

  auto key = split_key(path);
  if (key.first.empty()) return -EEXIST;

  auto it = context.jobs.find(key.first);
  if (it == context.jobs.end()) {
    if (key.second == ".")
      return -EACCES;
    else
      return -ENOENT;
  }

  if (key.second == ".") return -EEXIST;

  if (it->second.is_visible(key.second)) return -EEXIST;

  bool create_new = !it->second.is_writeable(key.second);
  if (create_new) {
    // Remove any file or link that might be in the way
    int res = unlinkat(context.rootfd, key.second.c_str(), 0);
    if (res == -1 && errno != EPERM && errno != ENOENT && errno != EISDIR) return -errno;
  }

  int res = mkdirat(context.rootfd, key.second.c_str(), mode);

  // If a directory already exists, change permissions and claim it
  if (create_new && res == -1 && (errno == EEXIST || errno == EISDIR))
    res = fchmodat(context.rootfd, key.second.c_str(), mode, 0);

  if (res == -1) return -errno;

  it->second.files_wrote.insert(std::move(key.second));
  return 0;
}

static int wakefuse_mkdir_trace(const char *path, mode_t mode) {
  int out = wakefuse_mkdir(path, mode);
  fprintf(stderr, "mkdir(%s, 0%o) = %s\n", path, mode, trace_out(out));
  return out;
}

static int wakefuse_unlink(const char *path) {
  if (is_special(path)) return -EACCES;

  auto key = split_key(path);
  if (key.first.empty()) return -EPERM;

  auto it = context.jobs.find(key.first);
  if (it == context.jobs.end()) return -ENOENT;

  if (key.second == ".") return -EPERM;

  if (!it->second.is_readable(key.second)) return -ENOENT;

  if (!it->second.is_writeable(key.second)) return -EACCES;

  int res = unlinkat(context.rootfd, key.second.c_str(), 0);
  if (res == -1) return -errno;

  it->second.files_wrote.erase(key.second);
  it->second.files_read.erase(key.second);
  return 0;
}

static int wakefuse_unlink_trace(const char *path) {
  int out = wakefuse_unlink(path);
  fprintf(stderr, "unlink(%s) = %s\n", path, trace_out(out));
  return out;
}

// Check if the directory has children that were written and not yet deleted.
// Note: we don't check the 'files_visible' list as when the directory itself or
// a file within that directory is visible then is_writable() would have already failed.
static bool has_written_children(const std::string &dir, Job &job) {
  auto i = job.files_wrote.lower_bound(dir + "/");
  return i != job.files_wrote.end() && i->size() > dir.size() &&
         0 == i->compare(0, dir.size(), dir);
}

static int wakefuse_rmdir(const char *path) {
  if (is_special(path)) return -ENOTDIR;

  auto key = split_key(path);
  if (key.first.empty()) return -EACCES;

  auto it = context.jobs.find(key.first);
  if (it == context.jobs.end()) return -ENOENT;

  if (key.second == ".") return -EACCES;

  if (!it->second.is_readable(key.second)) return -ENOENT;

  if (!it->second.is_writeable(key.second)) return -EACCES;

  int res = unlinkat(context.rootfd, key.second.c_str(), AT_REMOVEDIR);
  if (res == -1) {
    if ((errno == ENOTEMPTY) && !has_written_children(key.second, it->second)) {
      // Let the fuse client process believe that the directory was unlinked,
      // even though the underlying filesystem still has a populated directory.
    } else {
      return -errno;
    }
  }

  it->second.files_wrote.erase(key.second);
  it->second.files_read.erase(key.second);
  return 0;
}

static int wakefuse_rmdir_trace(const char *path) {
  int out = wakefuse_rmdir(path);
  fprintf(stderr, "rmdir(%s) = %s\n", path, trace_out(out));
  return out;
}

static int wakefuse_symlink(const char *from, const char *to) {
  if (is_special(to)) return -EEXIST;

  auto key = split_key(to);
  if (key.first.empty()) return -EEXIST;

  auto it = context.jobs.find(key.first);
  if (it == context.jobs.end()) {
    if (key.second == ".")
      return -EACCES;
    else
      return -ENOENT;
  }

  if (key.second == ".") return -EEXIST;

  if (it->second.is_visible(key.second)) return -EEXIST;

  if (!it->second.is_writeable(key.second)) (void)deep_unlink(context.rootfd, key.second.c_str());

  int res = symlinkat(from, context.rootfd, key.second.c_str());
  if (res == -1) return -errno;

  it->second.files_wrote.insert(std::move(key.second));
  return 0;
}

static int wakefuse_symlink_trace(const char *from, const char *to) {
  int out = wakefuse_symlink(from, to);
  fprintf(stderr, "symlink(%s, %s) = %s\n", from, to, trace_out(out));
  return out;
}

static void move_members(std::set<std::string> &from, std::set<std::string> &to,
                         const std::string &dir, const std::string &dest) {
  // Find half-open range [i, e) that includes all strings matching `{dir}/.*`
  auto i = from.upper_bound(dir + "/");
  auto e = from.lower_bound(dir + "0");  // '0' = '/' + 1

  if (i != e) {
    // If the range is non-empty, make it inclusive; [i, e]
    // This is necessary, because it would otherwise be possible
    // for the insert call to put something between 'i' and 'e'.
    // For example, if dir="foo" and from={"foo/aaa", "zoo"},
    // then i=>"foo/aaa" and e=>"zoo". Renaming "foo" to "bar"
    // would cause us to insert "bar/aaa", which is in [i, e).
    // By changing to an inclusive range, e=>"foo/aaa" also.

    --e;
    bool last;
    do {
      last = i == e;  // Record this now, because we erase i
      to.insert(dest + i->substr(dir.size()));
      from.erase(i++);  // increment i and then erase the old i
    } while (!last);
  }
}

static int wakefuse_rename(const char *from, const char *to) {
  if (is_special(to)) return -EACCES;

  if (is_special(from)) return -EACCES;

  auto keyt = split_key(to);
  if (keyt.first.empty()) return -ENOTEMPTY;

  auto keyf = split_key(from);
  if (keyf.first.empty()) return -EACCES;

  auto it = context.jobs.find(keyf.first);
  if (it == context.jobs.end()) return -ENOENT;

  if (keyf.second == ".") return -EACCES;

  if (keyt.second == ".") {
    if (context.jobs.find(keyt.first) == context.jobs.end())
      return -EACCES;
    else
      return -EEXIST;
  }

  if (keyt.first != keyf.first) return -EXDEV;

  if (!it->second.is_readable(keyf.second)) return -ENOENT;

  if (!it->second.is_writeable(keyf.second)) return -EACCES;

  if (it->second.is_visible(keyt.second)) return -EACCES;

  if (!it->second.is_writeable(keyt.second)) (void)deep_unlink(context.rootfd, keyt.second.c_str());

  int res = renameat(context.rootfd, keyf.second.c_str(), context.rootfd, keyt.second.c_str());
  if (res == -1) return -errno;

  it->second.files_wrote.erase(keyf.second);
  it->second.files_read.erase(keyf.second);
  it->second.files_wrote.insert(keyt.second);

  // Move any children as well
  move_members(it->second.files_wrote, it->second.files_wrote, keyf.second, keyt.second);
  move_members(it->second.files_read, it->second.files_wrote, keyf.second, keyt.second);

  return 0;
}

static int wakefuse_rename_trace(const char *from, const char *to) {
  int out = wakefuse_rename(from, to);
  fprintf(stderr, "rename(%s, %s) = %s\n", from, to, trace_out(out));
  return out;
}

static int wakefuse_link(const char *from, const char *to) {
  if (is_special(to)) return -EEXIST;

  if (is_special(from)) return -EACCES;

  auto keyt = split_key(to);
  if (keyt.first.empty()) return -EEXIST;

  auto keyf = split_key(from);
  if (keyf.first.empty()) return -EACCES;

  auto it = context.jobs.find(keyf.first);
  if (it == context.jobs.end()) return -ENOENT;

  if (keyf.second == ".") return -EACCES;

  if (keyt.second == ".") {
    if (context.jobs.find(keyt.first) == context.jobs.end())
      return -EACCES;
    else
      return -EEXIST;
  }

  if (keyt.first != keyf.first) return -EXDEV;

  if (!it->second.is_readable(keyf.second)) return -ENOENT;

  if (it->second.is_visible(keyt.second)) return -EEXIST;

  if (!it->second.is_writeable(keyt.second)) (void)deep_unlink(context.rootfd, keyt.second.c_str());

  int res = linkat(context.rootfd, keyf.second.c_str(), context.rootfd, keyt.second.c_str(), 0);
  if (res == -1) return -errno;

  hardlinks.insert(std::string(to));

  it->second.files_wrote.insert(std::move(keyt.second));
  return 0;
}

static int wakefuse_link_trace(const char *from, const char *to) {
  int out = wakefuse_link(from, to);
  fprintf(stderr, "link(%s, %s) = %s\n", from, to, trace_out(out));
  return out;
}

static int wakefuse_chmod(const char *path, mode_t mode) {
  if (is_special(path)) return -EACCES;

  auto key = split_key(path);
  if (key.first.empty()) return -EACCES;

  auto it = context.jobs.find(key.first);
  if (it == context.jobs.end()) return -ENOENT;

  if (key.second == ".") return -EACCES;

  if (!it->second.is_readable(key.second)) return -ENOENT;

  if (!it->second.is_writeable(key.second)) return -EACCES;

#ifdef __linux__
  // Linux is broken and violates POSIX by returning EOPNOTSUPP even for non-symlinks
  int res = fchmodat(context.rootfd, key.second.c_str(), mode, 0);
#else
  int res = fchmodat(context.rootfd, key.second.c_str(), mode, AT_SYMLINK_NOFOLLOW);
#endif
  if (res == -1) return -errno;

  return 0;
}

static int wakefuse_chmod_trace(const char *path, mode_t mode) {
  int out = wakefuse_chmod(path, mode);
  fprintf(stderr, "chmod(%s, 0%o) = %s\n", path, mode, trace_out(out));
  return out;
}

static int wakefuse_chown(const char *path, uid_t uid, gid_t gid) {
  if (is_special(path)) return -EACCES;

  auto key = split_key(path);
  if (key.first.empty()) return -EACCES;

  auto it = context.jobs.find(key.first);
  if (it == context.jobs.end()) return -ENOENT;

  if (key.second == ".") return -EACCES;

  if (!it->second.is_readable(key.second)) return -ENOENT;

  if (!it->second.is_writeable(key.second)) return -EACCES;

  int res = fchownat(context.rootfd, key.second.c_str(), uid, gid, AT_SYMLINK_NOFOLLOW);
  if (res == -1) return -errno;

  return 0;
}

static int wakefuse_chown_trace(const char *path, uid_t uid, gid_t gid) {
  int out = wakefuse_chown(path, uid, gid);
  fprintf(stderr, "chown(%s, %d, %d) = %s\n", path, uid, gid, trace_out(out));
  return out;
}

static int wakefuse_truncate(const char *path, off_t size) {
  if (auto s = is_special(path)) {
    switch (s.kind) {
      case 'i':
        if (size <= MAX_JSON) {
          s.job->second.json_in.resize(size);
          return 0;
        } else {
          return -ENOSPC;
        }
      default:
        return -EACCES;
    }
  }

  auto key = split_key(path);
  if (key.first.empty()) return -EISDIR;

  auto it = context.jobs.find(key.first);
  if (it == context.jobs.end()) return -ENOENT;

  if (key.second == ".") return -EISDIR;

  if (!it->second.is_readable(key.second)) return -ENOENT;

  if (!it->second.is_writeable(key.second)) return -EACCES;

  int fd = openat(context.rootfd, key.second.c_str(), O_WRONLY | O_NOFOLLOW);
  if (fd == -1) return -errno;

  int res = ftruncate(fd, size);
  if (res == -1) {
    res = -errno;
    (void)close(fd);
    return res;
  } else {
    it->second.files_wrote.insert(std::move(key.second));
    (void)close(fd);
    return 0;
  }
}

static int wakefuse_truncate_trace(const char *path, off_t size) {
  int out = wakefuse_truncate(path, size);
  fprintf(stderr, "truncate(%s, %lld) = %s\n", path, (long long)size, trace_out(out));
  return out;
}

static int wakefuse_utimens(const char *path, const struct timespec ts[2]) {
  if (is_special(path)) return -EACCES;

  auto key = split_key(path);
  if (key.first.empty()) return -EACCES;

  auto it = context.jobs.find(key.first);
  if (it == context.jobs.end()) return -ENOENT;

  if (key.second == ".") return -EACCES;

  if (!it->second.is_readable(key.second)) return -ENOENT;

  if (!it->second.is_writeable(key.second)) return -EACCES;

  int res = wake_utimensat(context.rootfd, key.second.c_str(), ts);
  if (res == -1) return -errno;

  it->second.files_wrote.insert(std::move(key.second));
  return 0;
}

static int wakefuse_utimens_trace(const char *path, const struct timespec ts[2]) {
  int out = wakefuse_utimens(path, ts);
  fprintf(stderr, "utimens(%s, %ld.%09ld, %ld.%09ld) = %s\n", path, (long)ts[0].tv_sec,
          (long)ts[0].tv_nsec, (long)ts[1].tv_sec, (long)ts[1].tv_nsec, trace_out(out));
  return out;
}

static int wakefuse_open(const char *path, struct fuse_file_info *fi) {
  if (auto s = is_special(path)) {
    switch (s.kind) {
      case 'i':
        ++s.job->second.json_in_uses;
        break;
      case 'o':
        ++s.job->second.json_out_uses;
        break;
      case 'l':
        ++s.job->second.uses;
        break;
      case 'f': {
        // This lowers context.should_exit().
        // Consequently, exit_attempts no longer transitions from 0 to non-zero for a clean exit.
        ++context.uses;
        if (!cancel_exit()) {
          // Could not abort exit; reject open attempt.
          // This will cause the fuse.cpp client to restart a fresh daemon.
          --context.uses;
          return -ENOENT;
        }
        break;
      }
      default:
        return -ENOENT;  // unreachable
    }
    fi->fh = BAD_FD;
    return 0;
  }

  auto key = split_key(path);
  if (key.first.empty()) return -EINVAL;  // open is for files only

  auto it = context.jobs.find(key.first);
  if (it == context.jobs.end()) return -ENOENT;

  if (key.second == ".") return -EINVAL;

  if (!it->second.is_readable(key.second)) return -ENOENT;

  if (hardlinks.count(std::string(path))) {
    fi->direct_io = true;
  }

  int fd = openat(context.rootfd, key.second.c_str(), fi->flags, 0);
  if (fd == -1) return -errno;

  fi->fh = fd;
  return 0;
}

static int wakefuse_open_trace(const char *path, struct fuse_file_info *fi) {
  int out = wakefuse_open(path, fi);
  fprintf(stderr, "open(%s) = %s, direct_io = %d\n", path, trace_out(out), fi->direct_io);
  return out;
}

static int read_str(const std::string &str, char *buf, size_t size, off_t offset) {
  if (offset >= (ssize_t)str.size()) {
    return 0;
  } else {
    size_t got = std::min(str.size() - (size_t)offset, size);
    memcpy(buf, str.data() + offset, got);
    return got;
  }
}

static int wakefuse_read(const char *path, char *buf, size_t size, off_t offset,
                         struct fuse_file_info *fi) {
  if (fi->fh != BAD_FD) {
    auto key = split_key(path);
    auto it = context.jobs.find(key.first);
    if (it == context.jobs.end()) return -ENOENT;

    int res = pread(fi->fh, buf, size, offset);
    if (res == -1) res = -errno;

    it->second.ibytes += res;
    it->second.files_read.insert(std::move(key.second));
    return res;
  }

  if (auto s = is_special(path)) {
    switch (s.kind) {
      case 'i':
        return read_str(s.job->second.json_in, buf, size, offset);
      case 'o':
        return read_str(s.job->second.json_out, buf, size, offset);
      default:
        return 0;
    }
  }

  return -EIO;
}

static int wakefuse_read_trace(const char *path, char *buf, size_t size, off_t offset,
                               struct fuse_file_info *fi) {
  int out = wakefuse_read(path, buf, size, offset, fi);
  fprintf(stderr, "read(%s, %lu, %lld) = %s\n", path, (unsigned long)size, (long long)offset,
          trace_out(out));
  return out;
}

static int write_str(std::string &str, const char *buf, size_t size, off_t offset) {
  if (offset >= MAX_JSON) {
    return 0;
  } else {
    size_t end = std::min((off_t)MAX_JSON, offset + (off_t)size);
    size_t got = end - offset;
    if (end > str.size()) str.resize(end);
    str.replace(offset, got, buf, got);
    return got;
  }
}

static int wakefuse_write(const char *path, const char *buf, size_t size, off_t offset,
                          struct fuse_file_info *fi) {
  if (fi->fh != BAD_FD) {
    auto key = split_key(path);
    auto it = context.jobs.find(key.first);
    if (it == context.jobs.end()) return -ENOENT;

    if (!it->second.is_writeable(key.second)) return -EACCES;

    int res = pwrite(fi->fh, buf, size, offset);
    if (res == -1) res = -errno;

    it->second.obytes += res;
    return res;
  }

  if (auto s = is_special(path)) {
    switch (s.kind) {
      case 'i':
        return write_str(s.job->second.json_in, buf, size, offset);
      case 'l':
        s.job->second.dump();
        return -ENOSPC;
      default:
        return -EACCES;
    }
  }

  return -EIO;
}

static int wakefuse_write_trace(const char *path, const char *buf, size_t size, off_t offset,
                                struct fuse_file_info *fi) {
  int out = wakefuse_write(path, buf, size, offset, fi);
  fprintf(stderr, "write(%s, %lu, %lld) = %s\n", path, (unsigned long)size, (long long)offset,
          trace_out(out));
  return out;
}

static int wakefuse_statfs(const char *path, struct statvfs *stbuf) {
  int fd;
  auto key = split_key(path);
  if (key.first.empty() || is_special(path)) {
    fd = dup(context.rootfd);
  } else {
    auto it = context.jobs.find(key.first);
    if (it == context.jobs.end()) {
      return -ENOENT;
    } else if (key.second == ".") {
      fd = dup(context.rootfd);
    } else if (!it->second.is_readable(key.second)) {
      return -ENOENT;
    } else {
      fd = openat(context.rootfd, key.second.c_str(), O_RDONLY | O_NOFOLLOW);
    }
  }
  if (fd == -1) return -errno;

  int res = fstatvfs(fd, stbuf);
  if (res == -1) {
    res = -errno;
    (void)close(fd);
    return res;
  } else {
    (void)close(fd);
    return 0;
  }
}

static int wakefuse_statfs_trace(const char *path, struct statvfs *stbuf) {
  int out = wakefuse_statfs(path, stbuf);
  fprintf(stderr, "statfs(%s) = %s\n", path, trace_out(out));
  return out;
}

static int wakefuse_release(const char *path, struct fuse_file_info *fi) {
  if (fi->fh != BAD_FD) {
    int res = close(fi->fh);
    if (res == -1) return -errno;
  }

  if (auto s = is_special(path)) {
    switch (s.kind) {
      case 'f':
        --context.uses;
        break;
      case 'i':
        if (--s.job->second.json_in_uses == 0) s.job->second.parse();
        break;
      case 'o':
        --s.job->second.json_out_uses;
        break;
      case 'l':
        --s.job->second.uses;
        break;
      default:
        return -EIO;
    }
    if ('f' != s.kind && s.job->second.should_erase()) context.jobs.erase(s.job);
    if (context.should_exit()) schedule_exit();
  }

  return 0;
}

static int wakefuse_release_trace(const char *path, struct fuse_file_info *fi) {
  int out = wakefuse_release(path, fi);
  fprintf(stderr, "release(%s) = %s\n", path, trace_out(out));
  return out;
}

static int wakefuse_fsync(const char *path, int isdatasync, struct fuse_file_info *fi) {
  int res;

  if (fi->fh == BAD_FD) return 0;

#ifdef HAVE_FDATASYNC
  if (isdatasync)
    res = fdatasync(fi->fh);
  else
#else
  (void)isdatasync;
#endif
    res = fsync(fi->fh);

  if (res == -1) return -errno;

  return 0;
}

static int wakefuse_fsync_trace(const char *path, int isdatasync, struct fuse_file_info *fi) {
  int out = wakefuse_fsync(path, isdatasync, fi);
  fprintf(stderr, "fsync(%s, %d) = %s\n", path, isdatasync, trace_out(out));
  return out;
}

#ifdef HAVE_FALLOCATE
static int wakefuse_fallocate(const char *path, int mode, off_t offset, off_t length,
                              struct fuse_file_info *fi) {
  (void)fi;

  if (mode) return -EOPNOTSUPP;

  if (is_special(path)) return -EACCES;

  auto key = split_key(path);
  if (key.first.empty()) return -EISDIR;

  auto it = context.jobs.find(key.first);
  if (it == context.jobs.end()) return -ENOENT;

  if (key.second == ".") return -EISDIR;

  if (!it->second.is_readable(key.second)) return -ENOENT;

  if (!it->second.is_writeable(key.second)) return -EACCES;

  int fd = openat(context.rootfd, key.second.c_str(), O_WRONLY | O_NOFOLLOW);
  if (fd == -1) return -errno;

  int res = posix_fallocate(fd, offset, length);
  if (res != 0) {
    (void)close(fd);
    return -res;
  } else {
    it->second.files_wrote.insert(std::move(key.second));
    (void)close(fd);
    return 0;
  }
}

static int wakefuse_fallocate_trace(const char *path, int mode, off_t offset, off_t length,
                                    struct fuse_file_info *fi) {
  int out = wakefuse_fallocate(path, mode, offset, length, fi);
  fprintf(stderr, "fallocate(%s, 0%o, %lld, %lld) = %s\n", path, mode, (long long)offset,
          (long long)length, trace_out(out));
  return out;
}
#endif

static std::string path;
static struct fuse *fh;
static struct fuse_chan *fc;
static sigset_t saved;

static struct fuse_operations wakefuse_ops;

static void *wakefuse_init(struct fuse_conn_info *conn) {
  // unblock signals
  sigprocmask(SIG_SETMASK, &saved, 0);

  return 0;
}

static void handle_exit(int sig) {
  // It is possible that SIGALRM still gets delivered after a successful call to cancel_exit
  // In that case, we need to uphold the promise of cancel_exit
  if (sig == SIGALRM && 0 == exit_attempts && !context.should_exit()) return;

  // We only start the exit sequence once for SIG{INT,QUIT,TERM}
  if (sig != SIGALRM && 0 != exit_attempts) return;

  static struct timeval start;
  static pid_t pid = -1;
  static bool linger = false;
  struct timeval now;

  // Unfortunately, fuse_unmount can fail if the filesystem is still in use.
  // Yes, this can even happen on linux with MNT_DETACH / lazy umount.
  // Worse, fuse_unmount closes descriptors and frees memory, so can only be called once.
  // Thus, calling fuse_exit here would terminate fuse_loop and then maybe fail to unmount.

  // Instead of terminating the loop directly via fuse_exit, try to unmount.
  // If this succeeds, fuse_loop will terminate anyway.
  // In case it fails, we setup an itimer to keep trying to unmount.

  if (exit_attempts == 0) {
    // Record when the exit sequence began
    gettimeofday(&start, nullptr);
  }

  // Reap prior attempts
  if (pid != -1) {
    int status = 0;
    do {
      int ret = waitpid(pid, &status, 0);
      if (ret == -1) {
        if (errno == EINTR) {
          continue;
        } else {
          fprintf(stderr, "waitpid(%d): %s\n", pid, strerror(errno));
          break;
        }
      }
    } while (WIFSTOPPED(status));
    pid = -1;

    if (WIFEXITED(status) && WEXITSTATUS(status) == 42) {
      linger = true;
    } else {
      // Attempts numbered counting from 1:
      gettimeofday(&now, nullptr);
      double waited = (now.tv_sec - start.tv_sec) + (now.tv_usec - start.tv_usec) / 1000000.0;
      fprintf(stderr, "Unable to umount on attempt %d, %.1fs after we started to shutdown\n",
              exit_attempts, waited);
    }
  }

  if (linger) {
    // The filesystem was successfully unmounted
    fprintf(stderr, "Successful file-system umount, with lingering child processes\n");
    // Release our lock so that a new daemon can start in our place
    struct flock fl;
    memset(&fl, 0, sizeof(fl));
    fl.l_type = F_UNLCK;
    fl.l_whence = SEEK_SET;
    fl.l_start = 0;
    fl.l_len = 0;  // 0=largest possible
    int log = STDOUT_FILENO;
    if (fcntl(log, F_SETLK, &fl) != 0) {
      fprintf(stderr, "fcntl(unlock): %s\n", strerror(errno));
    }
    // Return to fuse_loop and wait for the kernel to indicate we're finally detached.
  } else if (exit_attempts == QUIT_RETRY_ATTEMPTS) {
    fprintf(
        stderr,
        "Too many umount attempts; unable to exit cleanly. Leaving a broken mount point behind.\n");
    exit(1);
  } else if ((pid = fork()) == 0) {
    // We need to fork before fuse_unmount, in order to be able to try more than once.
#ifdef __APPLE__
    unmount(path.c_str(), MNT_FORCE);
#else
    fuse_unmount(path.c_str(), fc);
#endif
    std::string marker = path + "/.f.fuse-waked";
    if (access(marker.c_str(), F_OK) == 0) {
      // umount did not disconnect the mount
      exit(1);
    } else {
      // report that the mount WAS disconnected
      exit(42);
    }
  } else {
    // By incrementing exit_attempts, we ensure cancel_exit never stops the next scheduled attempt
    ++exit_attempts;
    schedule_exit();
  }
}

int main(int argc, char *argv[]) {
  bool debug = getenv("DEBUG_FUSE_WAKE");

  wakefuse_ops.init = wakefuse_init;
  wakefuse_ops.getattr = debug ? wakefuse_getattr_trace : wakefuse_getattr;
  wakefuse_ops.access = debug ? wakefuse_access_trace : wakefuse_access;
  wakefuse_ops.readlink = debug ? wakefuse_readlink_trace : wakefuse_readlink;
  wakefuse_ops.readdir = debug ? wakefuse_readdir_trace : wakefuse_readdir;
  wakefuse_ops.mknod = debug ? wakefuse_mknod_trace : wakefuse_mknod;
  wakefuse_ops.create = debug ? wakefuse_create_trace : wakefuse_create;
  wakefuse_ops.mkdir = debug ? wakefuse_mkdir_trace : wakefuse_mkdir;
  wakefuse_ops.symlink = debug ? wakefuse_symlink_trace : wakefuse_symlink;
  wakefuse_ops.unlink = debug ? wakefuse_unlink_trace : wakefuse_unlink;
  wakefuse_ops.rmdir = debug ? wakefuse_rmdir_trace : wakefuse_rmdir;
  wakefuse_ops.rename = debug ? wakefuse_rename_trace : wakefuse_rename;
  wakefuse_ops.link = debug ? wakefuse_link_trace : wakefuse_link;
  wakefuse_ops.chmod = debug ? wakefuse_chmod_trace : wakefuse_chmod;
  wakefuse_ops.chown = debug ? wakefuse_chown_trace : wakefuse_chown;
  wakefuse_ops.truncate = debug ? wakefuse_truncate_trace : wakefuse_truncate;
  wakefuse_ops.utimens = debug ? wakefuse_utimens_trace : wakefuse_utimens;
  wakefuse_ops.open = debug ? wakefuse_open_trace : wakefuse_open;
  wakefuse_ops.read = debug ? wakefuse_read_trace : wakefuse_read;
  wakefuse_ops.write = debug ? wakefuse_write_trace : wakefuse_write;
  wakefuse_ops.statfs = debug ? wakefuse_statfs_trace : wakefuse_statfs;
  wakefuse_ops.release = debug ? wakefuse_release_trace : wakefuse_release;
  wakefuse_ops.fsync = debug ? wakefuse_fsync_trace : wakefuse_fsync;

  // xattr were removed because they are not hashed!
#ifdef HAVE_FALLOCATE
  wakefuse_ops.fallocate = wakefuse_fallocate;
#endif

  int status = 1;
  sigset_t block;
  struct sigaction sa;
  struct fuse_args args;
  struct flock fl;
  pid_t pid;
  int log, null;
  bool madedir;
  struct rlimit rlim;

  if (argc != 3) {
    fprintf(stderr, "Syntax: fuse-waked <mount-point> <min-timeout-seconds>\n");
    goto term;
  }
  path = argv[1];

  linger_timeout = atol(argv[2]);
  if (linger_timeout < 1) linger_timeout = 1;
  if (linger_timeout > 240) linger_timeout = 240;

  null = open("/dev/null", O_RDONLY);
  if (null == -1) {
    perror("open /dev/null");
    goto term;
  }

  log = open((path + ".log").c_str(), O_CREAT | O_RDWR | O_APPEND,
             0644);  // S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
  if (log == -1) {
    fprintf(stderr, "open %s.log: %s\n", path.c_str(), strerror(errno));
    goto term;
  }

  if (log != STDOUT_FILENO) {
    dup2(log, STDOUT_FILENO);
    close(log);
    log = STDOUT_FILENO;
  }

  umask(0);

  context.rootfd = open(".", O_RDONLY);
  if (context.rootfd == -1) {
    perror("open .");
    goto term;
  }

  madedir = mkdir(path.c_str(), 0775) == 0;
  if (!madedir && errno != EEXIST) {
    fprintf(stderr, "mkdir %s: %s\n", path.c_str(), strerror(errno));
    goto rmroot;
  }

  if (getrlimit(RLIMIT_NOFILE, &rlim) != 0) {
    fprintf(stderr, "getrlimit(RLIMIT_NOFILE): %s\n", strerror(errno));
    goto rmroot;
  }

  rlim.rlim_cur = rlim.rlim_max;
#ifdef __APPLE__
  // Work around OS/X's misreporting of rlim_max ulimited
  if (rlim.rlim_cur > 20480) rlim.rlim_cur = 20480;
#endif

  if (setrlimit(RLIMIT_NOFILE, &rlim) != 0) {
    fprintf(stderr, "setrlimit(RLIMIT_NOFILE, cur=max): %s\n", strerror(errno));
    goto rmroot;
  }

  // Become a daemon
  pid = fork();
  if (pid == -1) {
    perror("fork");
    goto rmroot;
  } else if (pid != 0) {
    status = 0;
    goto term;
  }

  if (setsid() == -1) {
    perror("setsid");
    goto rmroot;
  }

  pid = fork();
  if (pid == -1) {
    perror("fork2");
    goto rmroot;
  } else if (pid != 0) {
    status = 0;
    goto term;
  }

  // Open the logfile and use as lock on it to ensure we retain ownership
  // This has to happen after fork (which would drop the lock)
  memset(&fl, 0, sizeof(fl));
  fl.l_type = F_WRLCK;
  fl.l_whence = SEEK_SET;
  fl.l_start = 0;
  fl.l_len = 0;  // 0=largest possible
  if (fcntl(log, F_SETLK, &fl) != 0) {
    if (errno == EAGAIN || errno == EACCES) {
      if (debug) {
        fprintf(stderr, "fcntl(%s.log): %s -- assuming another daemon exists\n", path.c_str(),
                strerror(errno));
      }
      status = 0;  // another daemon is already running
    } else {
      fprintf(stderr, "fcntl(%s.log): %s\n", path.c_str(), strerror(errno));
    }
    goto term;
  }

  // block those signals where we wish to terminate cleanly
  sigemptyset(&block);
  sigaddset(&block, SIGINT);
  sigaddset(&block, SIGQUIT);
  sigaddset(&block, SIGTERM);
  sigaddset(&block, SIGALRM);
  sigprocmask(SIG_BLOCK, &block, &saved);

  memset(&sa, 0, sizeof(sa));

  // ignore these signals
  sa.sa_handler = SIG_IGN;
  sa.sa_flags = SA_RESTART;
  sigaction(SIGPIPE, &sa, 0);
  sigaction(SIGUSR1, &sa, 0);
  sigaction(SIGUSR2, &sa, 0);
  sigaction(SIGHUP, &sa, 0);

  // hook these signals
  sa.sa_handler = handle_exit;
  sa.sa_flags = SA_RESTART;
  sigaction(SIGINT, &sa, 0);
  sigaction(SIGQUIT, &sa, 0);
  sigaction(SIGTERM, &sa, 0);
  sigaction(SIGALRM, &sa, 0);

  args = FUSE_ARGS_INIT(0, NULL);
#if FUSE_VERSION >= 24 && FUSE_VERSION < 30 && !defined(__APPLE__)
  /* Allow mounting on non-empty .fuse directories
   * This anti-feature was added in 2.4.0 and removed in 3.0.0.
   */
  if (fuse_opt_add_arg(&args, "wake") != 0 || fuse_opt_add_arg(&args, "-o") != 0 ||
      fuse_opt_add_arg(&args, "nonempty") != 0) {
#else
  if (fuse_opt_add_arg(&args, "wake") != 0) {
#endif
    fprintf(stderr, "fuse_opt_add_arg failed\n");
    goto rmroot;
  }

  if (debug && fuse_opt_add_arg(&args, "-odebug") != 0) {
    fprintf(stderr, "fuse_opt_add_arg debug failed\n");
    goto rmroot;
  }

#ifndef __APPLE__
  // Attempt to clean up any stale mount before trying to mount
  try_unmount_stale(path.c_str(), debug);
#endif

  fc = fuse_mount(path.c_str(), &args);
  if (!fc) {
    fprintf(stderr, "fuse_mount failed\n");
    goto freeargs;
  }

  fh = fuse_new(fc, &args, &wakefuse_ops, sizeof(wakefuse_ops), 0);
  if (!fh) {
    fprintf(stderr, "fuse_new failed\n");
    goto unmount;
  }

  fflush(stdout);
  fflush(stderr);

  dup2(log, STDERR_FILENO);

  if (null != STDIN_FILENO) {
    dup2(null, STDIN_FILENO);
    close(null);
  }

  if (fuse_loop(fh) != 0) {
    fprintf(stderr, "fuse_loop failed");
    goto unmount;
  }

  status = 0;

  // Block signals again
  sigprocmask(SIG_BLOCK, &block, 0);

unmount:
  // out-of-order completion: unmount THEN destroy
  fuse_unmount(path.c_str(), fc);
  if (fh) fuse_destroy(fh);
freeargs:
  fuse_opt_free_args(&args);
rmroot:
  if (madedir && rmdir(path.c_str()) != 0) {
    fprintf(stderr, "rmdir %s: %s\n", path.c_str(), strerror(errno));
  }
term:
  return status;
}
