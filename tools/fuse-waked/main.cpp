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

#include <assert.h>
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

#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <variant>

#include "cas/content_hash.h"
#include "compat/nofollow.h"
#include "compat/utimens.h"
#include "json/json5.h"
#include "util/execpath.h"
#include "util/mkdir_parents.h"
#include "util/unlink.h"
#include "wcl/file_ops.h"

#define MAX_JSON (128 * 1024 * 1024)

// We ensure STDIN is /dev/null, so this is a safe sentinel value for open files
#define BAD_FD STDIN_FILENO

// How long to wait for a new client to connect before the daemon exits
static int linger_timeout;
static std::set<std::string> hardlinks = {};

// Staging directory for CAS
static std::string g_staging_dir;

// Helper for std::visit with multiple lambdas
template <class... Ts>
struct overloaded : Ts... {
  using Ts::operator()...;
};
template <class... Ts>
overloaded(Ts...) -> overloaded<Ts...>;

// Staged item types - written to .cas/staging/ during job execution, hashed by wakebox after
struct StagedFileData {
  std::string staging_path;
  mode_t mode;
};

struct StagedSymlinkData {
  std::string target;
  struct timespec mtime;
};

struct StagedDirectoryData {
  mode_t mode;
  struct timespec mtime;
};

// Hardlinks are not true filesystem hardlinks. On link(), we reflink/copy the source's staging
// file so each entry owns an independent staging_path.
struct StagedHardlinkData {
  std::string staging_path;
  mode_t mode;
};

using StagedItemData =
    std::variant<StagedFileData, StagedSymlinkData, StagedDirectoryData, StagedHardlinkData>;

struct StagedItem {
  std::string dest_path;
  std::string job_id;
  StagedItemData data;

  // Type query helpers
  bool is_file() const { return std::holds_alternative<StagedFileData>(data); }
  bool is_symlink() const { return std::holds_alternative<StagedSymlinkData>(data); }
  bool is_directory() const { return std::holds_alternative<StagedDirectoryData>(data); }
  bool is_hardlink() const { return std::holds_alternative<StagedHardlinkData>(data); }

  // Type name for JSON output
  const char *type_name() const {
    return std::visit(
        overloaded{
            [](const StagedFileData &) { return "file"; },
            [](const StagedSymlinkData &) { return "symlink"; },
            [](const StagedDirectoryData &) { return "directory"; },
            // Serialize hardlinks as ordinary file entries. The shared staging_path still preserves
            // the deduplication behavior Wake cares about during hashing and CAS ingestion.
            [](const StagedHardlinkData &) { return "file"; },
        },
        data);
  }

  std::optional<std::string_view> staging_path() const {
    if (auto *f = std::get_if<StagedFileData>(&data)) return f->staging_path;
    if (auto *h = std::get_if<StagedHardlinkData>(&data)) return h->staging_path;
    return std::nullopt;
  }

  // Get mode (valid for file, directory, and hardlink)
  mode_t mode() const {
    return std::visit(overloaded{
                          [](const StagedFileData &f) { return f.mode; },
                          [](const StagedSymlinkData &) { return static_cast<mode_t>(0777); },
                          [](const StagedDirectoryData &d) { return d.mode; },
                          [](const StagedHardlinkData &h) { return h.mode; },
                      },
                      data);
  }

  // Set mode (for chmod support)
  void set_mode(mode_t m) {
    std::visit(overloaded{
                   [m](StagedFileData &f) { f.mode = m; },
                   [](StagedSymlinkData &) {},  // Symlinks don't have mode
                   [m](StagedDirectoryData &d) { d.mode = m; },
                   // Known limitation: hardlink mode is tracked per-entry rather than shared across
                   // all names pointing to the same staged file. A chmod on one hardlink path will
                   // not update sibling hardlink entries, diverging from POSIX inode semantics.
                   // TODO: Add robust hardlink support
                   [m](StagedHardlinkData &h) { h.mode = m; },
               },
               data);
  }

  // Get mtime (valid for symlink and directory; files/hardlinks use the backing staging file)
  struct timespec mtime() const {
    if (auto *l = std::get_if<StagedSymlinkData>(&data)) return l->mtime;
    if (auto *d = std::get_if<StagedDirectoryData>(&data)) return d->mtime;
    return {0, 0};
  }

  // Set timestamps (for utimens support; only symlinks/directories use in-memory times)
  void set_times(const struct timespec &at, const struct timespec &mt) {
    (void)at;
    if (auto *l = std::get_if<StagedSymlinkData>(&data)) {
      l->mtime = mt;
    } else if (auto *d = std::get_if<StagedDirectoryData>(&data)) {
      d->mtime = mt;
      // Known limitation: same as set_mode — timestamps are tracked per-entry, so an utimens on
      // one hardlink path does not update sibling hardlink entries.
      // TODO: Add robust hardlink support
    }
  }
};

// Encapsulates the nested map of staged files with helper methods
// Structure: job_id -> (path -> StagedItem)
// Also tracks fd -> StagedItem* for open file handles
class StagedFilesStore {
 public:
  using JobMap = std::map<std::string, StagedItem, std::less<>>;
  using StoreMap = std::map<std::string, JobMap, std::less<>>;

  // Find a staged item, returns nullptr if not found
  StagedItem *find(std::string_view job_id, std::string_view path) {
    auto job_it = files_.find(job_id);
    if (job_it == files_.end()) return nullptr;
    auto it = job_it->second.find(path);
    if (it == job_it->second.end()) return nullptr;
    return &it->second;
  }

  StagedItem *insert(const std::string &job_id, const std::string &path, StagedItem item) {
    return &(files_[job_id][path] = std::move(item));
  }

  // Erase a staged item, returns true if found and erased
  bool erase(const std::string &job_id, const std::string &path) {
    auto job_it = files_.find(job_id);
    if (job_it == files_.end()) return false;
    return job_it->second.erase(path) > 0;
  }

  // Erase all staged items for a job
  void erase_job(const std::string &job_id) { files_.erase(job_id); }

  // Check if a staged directory has any children
  bool has_children(const std::string &job_id, const std::string &dir) {
    auto job_it = files_.find(job_id);
    if (job_it == files_.end()) return false;
    std::string prefix = dir + "/";
    auto it = job_it->second.lower_bound(prefix);
    return it != job_it->second.end() && it->first.compare(0, prefix.size(), prefix) == 0;
  }

  // Get the inner map for a job (for iteration)
  JobMap *get_job(std::string_view job_id) {
    auto job_it = files_.find(job_id);
    if (job_it == files_.end()) return nullptr;
    return &job_it->second;
  }

  // Iterate over all jobs
  StoreMap::iterator begin() { return files_.begin(); }
  StoreMap::iterator end() { return files_.end(); }

  // FD tracking for open staged files
  void register_fd(int fd, StagedItem *item) {
    assert(item != nullptr);
    assert(fd_map_.find(fd) == fd_map_.end());
    fd_map_[fd] = item;
  }
  StagedItem *find_by_fd(int fd) {
    auto it = fd_map_.find(fd);
    return it != fd_map_.end() ? it->second : nullptr;
  }
  void unregister_fd(int fd) { fd_map_.erase(fd); }

 private:
  StoreMap files_;
  std::map<int, StagedItem *> fd_map_;  // fd -> StagedItem*
};

static StagedFilesStore g_staged_files;

// Counter for unique staging file names
static uint64_t g_staging_counter = 0;

// Path to CAS blobs directory for hash-based reads
static std::string g_cas_blobs_dir;

// Global flag to enable/disable CAS-first staging
// When false, files are written directly to workspace
// TODO: Remove the non-CAS workspace-write mode once WAKE_CAS is the default.
static bool g_use_cas = false;

// How to retry umount while quitting
// (2^8-1)*100ms = 25.5s worst-case quit time
#define QUIT_RETRY_MS 100
#define QUIT_RETRY_ATTEMPTS 8

struct Job {
  struct VisibleEntry {
    std::string type;
    std::optional<cas::ContentHash> content_hash;
    std::optional<mode_t> mode;
    long mtime;
  };

  std::set<std::string> files_visible;
  std::map<std::string, VisibleEntry> visible_entries;  // path -> type/hash/mode for CAS reads
  std::set<std::string> files_read;
  std::set<std::string> files_wrote;
  std::set<std::string> staged_paths;  // Paths staged for CAS (for is_readable)
  std::string json_in;
  std::string json_out;
  long ibytes, obytes;
  int json_in_uses;
  int json_out_uses;
  int uses;

  Job() : ibytes(0), obytes(0), json_in_uses(0), json_out_uses(0), uses(0) {}

  void parse();
  void dump(const std::string &job_id);

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

  std::string cas_dir = jast.get("cas_dir").value;
  if (!cas_dir.empty()) {
    g_cas_blobs_dir = cas_dir + "/blobs";
    g_staging_dir = cas_dir + "/staging";
  } else if (g_cas_blobs_dir.empty()) {
    g_cas_blobs_dir = ".build/cas/blobs";
    g_staging_dir = ".build/cas/staging";
  }
  if (g_use_cas) {
    int err = mkdir_with_parents(g_cas_blobs_dir, 0755);
    if (err != 0) {
      fprintf(stderr, "fuse-waked: failed to create CAS blobs directory '%s': %s\n",
              g_cas_blobs_dir.c_str(), strerror(err));
    }
    err = mkdir_with_parents(g_staging_dir, 0755);
    if (err != 0) {
      fprintf(stderr, "fuse-waked: failed to create CAS staging directory '%s': %s\n",
              g_staging_dir.c_str(), strerror(err));
    }
  }

  // We only need to make the relative paths visible; absolute paths are already
  files_visible.clear();
  visible_entries.clear();

  for (auto &x : jast.get("visible").children) {
    std::string path;
    std::string type;
    std::optional<cas::ContentHash> content_hash;
    std::optional<mode_t> mode;
    long mtime = 0;

    if (x.second.kind == JSON_OBJECT) {
      // New format: {"path": "...", "type": "...", "hash": "...", "mode": ..., "mtime": ...}
      path = x.second.get("path").value;
      type = x.second.get("type").value;
      const std::string &hash = x.second.get("hash").value;
      const std::string &mode_value = x.second.get("mode").value;
      const std::string &mtime_value = x.second.get("mtime").value;

      if (path.empty()) {
        fprintf(stderr, "Visible entry missing 'path'\n");
        continue;
      }
      if (type.empty()) {
        fprintf(stderr, "Visible entry '%s' missing 'type'\n", path.c_str());
        continue;
      }
      if (mode_value.empty()) {
        fprintf(stderr, "Visible entry '%s' missing 'mode'\n", path.c_str());
        continue;
      }
      if (mtime_value.empty()) {
        fprintf(stderr, "Visible entry '%s' missing 'mtime'\n", path.c_str());
        continue;
      }

      try {
        mode = static_cast<mode_t>(std::stoul(mode_value, nullptr, 10));
      } catch (const std::exception &e) {
        fprintf(stderr, "Visible entry '%s' has invalid 'mode' value '%s': %s\n", path.c_str(),
                mode_value.c_str(), e.what());
        continue;
      }

      try {
        mtime = std::stol(mtime_value, nullptr, 10);
      } catch (const std::exception &e) {
        fprintf(stderr, "Visible entry '%s' has invalid 'mtime' value '%s': %s\n", path.c_str(),
                mtime_value.c_str(), e.what());
        continue;
      }

      if (type != "directory") {
        if (hash.empty()) {
          fprintf(stderr, "Visible entry '%s' (type '%s') missing 'hash'\n", path.c_str(),
                  type.c_str());
          continue;
        }
        auto result = cas::ContentHash::from_hex(hash);
        if (!result) {
          fprintf(stderr, "Visible entry '%s' has invalid hash '%s'\n", path.c_str(), hash.c_str());
          continue;
        }
        content_hash = *result;
      }
    } else {
      // Legacy format: just a string path
      path = x.second.value;
    }

    if (!path.empty() && path[0] != '/') {
      files_visible.insert(path);
      visible_entries[path] = VisibleEntry{type, content_hash, mode, mtime};

      // Add implicit parent directories for this path.
      // Only needed in CAS mode where the filesystem is virtualized and parent
      // directories may not exist on disk.  In legacy mode the real filesystem
      // already contains these directories, and adding them to files_visible
      // causes wakefuse_mkdir to return EEXIST before the directory is created.
      if (g_use_cas) {
        for (size_t slash = path.find('/'); slash != std::string::npos;
             slash = path.find('/', slash + 1)) {
          std::string parent = path.substr(0, slash);
          if (visible_entries.find(parent) == visible_entries.end()) {
            files_visible.insert(parent);
            visible_entries[parent] = VisibleEntry{"directory", std::nullopt, std::nullopt, mtime};
          }
        }
      }
    }
  }
}

static std::string cas_blob_path(const cas::ContentHash &hash) {
  std::string hex = hash.to_hex();
  assert(hex.size() >= 2);
  return g_cas_blobs_dir + "/" + hex.substr(0, 2) + "/" + hex.substr(2);
}

// Extract file mode bits (07777) from a VisibleEntry, falling back to the given mode.
// Masks with 07777 to strip the file type bits from st_mode before use.
static mode_t visible_mode_or(const Job::VisibleEntry &entry, mode_t fallback) {
  return static_cast<mode_t>(entry.mode.value_or(fallback) & 07777);
}

// Read the entire contents of a CAS blob into a string.
// This is only used for reading symlink targets stored in CAS.
// Returns true on success, false if the blob cannot be read.
static bool read_cas_blob_bytes(const cas::ContentHash &hash, std::string *data) {
  std::ifstream ifs(cas_blob_path(hash), std::ios::binary);
  if (!ifs) return false;
  std::ostringstream oss;
  oss << ifs.rdbuf();
  if (!ifs && !ifs.eof()) return false;
  *data = oss.str();
  return true;
}

void Job::dump(const std::string &job_id) {
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

  // Output staging_files with metadata for wakebox to process (CAS mode only)
  // TODO: Remove the legacy non-CAS JSON shape once WAKE_CAS is the default.
  if (g_use_cas) {
    s << "],\"staging_files\":{";
    first = true;
    if (auto *job_staged = g_staged_files.get_job(job_id)) {
      for (auto &entry : *job_staged) {
        const StagedItem &sf = entry.second;

        size_t start = 0;
        size_t lastslash = sf.dest_path.rfind("/");
        if (lastslash != std::string::npos) start = lastslash + 1;
        if (sf.dest_path.compare(start, prefix.length(), prefix) == 0) continue;

        s << (first ? "" : ",") << "\"" << json_escape(sf.dest_path) << "\":{";
        s << "\"type\":\"" << sf.type_name() << "\"";

        // Emit type-specific fields using std::visit
        std::visit(overloaded{
                       [&s](const StagedFileData &f) {
                         struct stat st;
                         int ret = stat(f.staging_path.c_str(), &st);
                         assert(ret == 0 && "staging file must exist at dump time");
                         s << ",\"staging_path\":\"" << json_escape(f.staging_path) << "\"";
                         s << ",\"mode\":" << (f.mode & 07777);
                         s << ",\"mtime_sec\":" << st.st_mtim.tv_sec;
                         s << ",\"mtime_nsec\":" << st.st_mtim.tv_nsec;
                       },
                       [&s](const StagedSymlinkData &l) {
                         s << ",\"target\":\"" << json_escape(l.target) << "\"";
                         s << ",\"mtime_sec\":" << l.mtime.tv_sec;
                         s << ",\"mtime_nsec\":" << l.mtime.tv_nsec;
                       },
                       [&s](const StagedDirectoryData &d) {
                         s << ",\"mode\":" << (d.mode & 07777);
                         s << ",\"mtime_sec\":" << d.mtime.tv_sec;
                         s << ",\"mtime_nsec\":" << d.mtime.tv_nsec;
                       },
                       [&s](const StagedHardlinkData &h) {
                         struct stat st;
                         int ret = stat(h.staging_path.c_str(), &st);
                         assert(ret == 0 && "staging file must exist at dump time");
                         // Hardlink has same staging_path as source - client uses it as
                         // deduplication key
                         s << ",\"staging_path\":\"" << json_escape(h.staging_path) << "\"";
                         s << ",\"mode\":" << (h.mode & 07777);
                         s << ",\"mtime_sec\":" << st.st_mtim.tv_sec;
                         s << ",\"mtime_nsec\":" << st.st_mtim.tv_nsec;
                       },
                   },
                   sf.data);

        s << "}";
        first = false;
      }
    }
    s << "}}";
  } else {
    // Legacy mode: no staging_files, close outputs array and object
    s << "]}";
  }

  s << std::endl;

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

bool Job::is_readable(const std::string &path) {
  return is_visible(path) || is_writeable(path) || (staged_paths.find(path) != staged_paths.end());
}

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
// Returns file attributes. For staged items, stats the staging file or synthesizes
// attributes from stored metadata. Resolves hardlinks to their source.
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

  // Check if item is staged (file, symlink, directory, or hardlink)
  if (StagedItem *sf = g_staged_files.find(key.first, key.second)) {
    // Fill stat buffer based on staged item type
    return std::visit(overloaded{
                          [stbuf](const StagedFileData &f) {
                            // Stat the actual staging file
                            int res = stat(f.staging_path.c_str(), stbuf);
                            if (res == -1) return -errno;
                            // Combine file type from staging file with tracked permissions
                            stbuf->st_mode = (stbuf->st_mode & S_IFMT) | (f.mode & ~S_IFMT);
                            return 0;
                          },
                          [stbuf](const StagedSymlinkData &l) {
                            // Return synthetic symlink stat
                            memset(stbuf, 0, sizeof(*stbuf));
                            stbuf->st_mode = S_IFLNK | 0777;
                            stbuf->st_nlink = 1;
                            stbuf->st_size = l.target.size();
                            stbuf->st_uid = getuid();
                            stbuf->st_gid = getgid();
                            stbuf->st_mtim = l.mtime;
                            return 0;
                          },
                          [stbuf](const StagedDirectoryData &d) {
                            // Return synthetic directory stat
                            memset(stbuf, 0, sizeof(*stbuf));
                            stbuf->st_mode = S_IFDIR | (d.mode & 07777);
                            stbuf->st_nlink = 2;
                            stbuf->st_uid = getuid();
                            stbuf->st_gid = getgid();
                            stbuf->st_mtim = d.mtime;
                            return 0;
                          },
                          [stbuf](const StagedHardlinkData &h) {
                            int res = stat(h.staging_path.c_str(), stbuf);
                            if (res == -1) return -errno;
                            stbuf->st_mode = (stbuf->st_mode & S_IFMT) | (h.mode & ~S_IFMT);
                            return 0;
                          },
                      },
                      sf->data);
  }

  if (g_use_cas) {
    auto visible_it = it->second.visible_entries.find(key.second);
    if (visible_it != it->second.visible_entries.end()) {
      const std::string &type = visible_it->second.type;
      if (visible_it->second.content_hash) {
        const cas::ContentHash &hash = *visible_it->second.content_hash;
        if (type == "file") {
          int res = stat(cas_blob_path(hash).c_str(), stbuf);
          if (res == 0) {
            stbuf->st_mode = (stbuf->st_mode & S_IFMT) | visible_mode_or(visible_it->second, 0444);
            stbuf->st_mtim.tv_sec = visible_it->second.mtime / 1000000000L;
            stbuf->st_mtim.tv_nsec = visible_it->second.mtime % 1000000000L;
            return 0;
          }
        } else if (type == "symlink") {
          std::string target;
          if (read_cas_blob_bytes(hash, &target)) {
            memset(stbuf, 0, sizeof(*stbuf));
            stbuf->st_mode = S_IFLNK | 0777;
            stbuf->st_nlink = 1;
            stbuf->st_size = target.size();
            stbuf->st_uid = getuid();
            stbuf->st_gid = getgid();
            stbuf->st_mtim.tv_sec = visible_it->second.mtime / 1000000000L;
            stbuf->st_mtim.tv_nsec = visible_it->second.mtime % 1000000000L;
            return 0;
          }
        }
      }
      if (type == "directory") {
        memset(stbuf, 0, sizeof(*stbuf));
        stbuf->st_mode = S_IFDIR | visible_mode_or(visible_it->second, 0755);
        stbuf->st_nlink = 2;
        stbuf->st_uid = getuid();
        stbuf->st_gid = getgid();
        stbuf->st_mtim.tv_sec = visible_it->second.mtime / 1000000000L;
        stbuf->st_mtim.tv_nsec = visible_it->second.mtime % 1000000000L;
        return 0;
      }
    }
  }

  // TODO: Remove workspace fallback once CAS is on by default
  int res = fstatat(context.rootfd, key.second.c_str(), stbuf, AT_SYMLINK_NOFOLLOW);
  if (res == -1) res = -errno;
  return res;
}

static int wakefuse_getattr_trace(const char *path, struct stat *stbuf) {
  int out = wakefuse_getattr(path, stbuf);
  fprintf(stderr, "getattr(%s) = %s\n", path, trace_out(out));
  return out;
}

// Checks file accessibility. Staged items are always accessible to their owning job.
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

  // Check if file is staged
  if (StagedItem *sf_ptr = g_staged_files.find(key.first, key.second)) {
    const StagedItem &sf = *sf_ptr;

    // Staged directories are purely virtual - they don't have a staging_path on disk.
    // They are always readable and writable since they were created by this job.
    if (sf.is_directory()) {
      // Check execute permission for directories (needed to traverse)
      if (mask & X_OK) {
        if (!(sf.mode() & (S_IXUSR | S_IXGRP | S_IXOTH))) {
          return -EACCES;
        }
      }
      // R_OK and W_OK are always granted for staged directories
      return 0;
    }
    // Check execute permission using tracked metadata (may differ from staging file on disk)
    if (mask & X_OK) {
      if (!(sf.mode() & (S_IXUSR | S_IXGRP | S_IXOTH))) {
        return -EACCES;
      }
    }
    // Check read permission on actual staging file (files and hardlinks have staging_path)
    if (mask & R_OK) {
      if (auto spath = sf.staging_path()) {
        int res = access(spath->data(), R_OK);
        if (res == -1) return -errno;
      }
    }
    return 0;
  }

  if (g_use_cas) {
    auto visible_it = it->second.visible_entries.find(key.second);
    if (visible_it != it->second.visible_entries.end()) {
      const std::string &type = visible_it->second.type;
      if (type == "directory") {
        if (mask & W_OK) return -EACCES;
        return 0;
      }
      if (type == "symlink") {
        return 0;
      }
      if (visible_it->second.content_hash) {
        std::string blob_path = cas_blob_path(*visible_it->second.content_hash);
        if (access(blob_path.c_str(), F_OK) == -1) return -errno;

        mode_t mode = visible_mode_or(visible_it->second, 0444);
        if ((mask & W_OK) != 0) return -EACCES;
        if ((mask & X_OK) != 0 && !(mode & (S_IXUSR | S_IXGRP | S_IXOTH))) return -EACCES;
        if ((mask & R_OK) != 0 && access(blob_path.c_str(), R_OK) == -1) return -errno;
        return 0;
      }
    }
  }

  // TODO: Remove workspace fallback once CAS is on by default
  int res = faccessat(context.rootfd, key.second.c_str(), mask, 0);
  if (res == -1) return -errno;

  return 0;
}

static int wakefuse_access_trace(const char *path, int mask) {
  int out = wakefuse_access(path, mask);
  fprintf(stderr, "access(%s, %d) = %s\n", path, mask, trace_out(out));
  return out;
}

// Reads symlink target. For staged symlinks, returns the stored target string.
static int wakefuse_readlink(const char *path, char *buf, size_t size) {
  if (is_special(path)) return -EINVAL;
  if (!size) return -EINVAL;

  auto key = split_key(path);
  if (key.first.empty()) return -EINVAL;

  auto it = context.jobs.find(key.first);
  if (it == context.jobs.end()) return -ENOENT;

  if (key.second == ".") return -EINVAL;

  if (!it->second.is_readable(key.second)) return -ENOENT;

  // Check if symlink is staged
  if (StagedItem *sf = g_staged_files.find(key.first, key.second)) {
    if (auto *l = std::get_if<StagedSymlinkData>(&sf->data)) {
      size_t len = std::min(l->target.size(), size - 1);
      memcpy(buf, l->target.c_str(), len);
      buf[len] = '\0';
      it->second.files_read.insert(std::move(key.second));
      return 0;
    }
  }

  if (g_use_cas) {
    auto visible_it = it->second.visible_entries.find(key.second);
    if (visible_it != it->second.visible_entries.end()) {
      const std::string &type = visible_it->second.type;
      if (visible_it->second.content_hash) {
        if (type != "symlink") return -EINVAL;

        std::string target;
        if (read_cas_blob_bytes(*visible_it->second.content_hash, &target)) {
          size_t len = std::min(target.size(), size - 1);
          memcpy(buf, target.c_str(), len);
          buf[len] = '\0';
          it->second.files_read.insert(std::move(key.second));
          return 0;
        }
      }
    }
  }

  // TODO: Remove workspace fallback once CAS is on by default
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

// Lists directory contents. Merges staged items with workspace contents,
// including virtual parent directories for nested staged paths.
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

  if (key.second != "." && !it->second.is_readable(key.second)) {
    return -ENOENT;
  }

  std::string dir_prefix = (key.second == ".") ? "" : (key.second + "/");
  std::set<std::string> already_listed;

  // TODO: Remove workspace fallback once CAS is on by default
  // Try to read from the real filesystem directory
  int dfd;
  if (key.second == ".") {
    dfd = dup(context.rootfd);
  } else {
    dfd = openat(context.rootfd, key.second.c_str(), O_RDONLY | O_NOFOLLOW | O_DIRECTORY);
  }

  if (dfd != -1) {
    DIR *dp = fdopendir(dfd);
    if (dp != NULL) {
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

        already_listed.insert(de->d_name);
        if (filler(buf, de->d_name, &st, 0)) break;
      }
      (void)closedir(dp);
    } else {
      (void)close(dfd);
    }
  }

  // For virtual directories (created by mkdir or CAS-only visible dirs), add . and ..
  if (dfd == -1 &&
      (it->second.is_writeable(key.second) || (g_use_cas && it->second.is_visible(key.second)))) {
    filler(buf, ".", 0, 0);
    filler(buf, "..", 0, 0);
    already_listed.insert(".");
    already_listed.insert("..");
  }

  // Helper to add first path component to directory listing
  auto add_child_entry = [&](const std::string &path) {
    // Case 1: Listing root directory (dir_prefix is empty)
    // For path "a/b/c", extract "a" as the child entry
    if (dir_prefix.empty()) {
      size_t slash = path.find('/');
      std::string name = (slash == std::string::npos) ? path : path.substr(0, slash);
      if (!name.empty() && already_listed.insert(name).second) {
        filler(buf, name.c_str(), 0, 0);
      }
      // Case 2: Listing subdirectory (dir_prefix is e.g. "a/b/")
      // Check if path starts with dir_prefix, then extract next component
      // For dir_prefix="a/b/" and path="a/b/c/d", extract "c"
    } else if (path.size() > dir_prefix.size() &&
               path.compare(0, dir_prefix.size(), dir_prefix) == 0) {
      std::string rest = path.substr(dir_prefix.size());
      size_t slash = rest.find('/');
      std::string name = (slash == std::string::npos) ? rest : rest.substr(0, slash);
      if (!name.empty() && already_listed.insert(name).second) {
        filler(buf, name.c_str(), 0, 0);
      }
    }
  };

  // Add staged files and virtual subdirectories that are children of this directory
  if (auto *job_staged = g_staged_files.get_job(key.first)) {
    for (auto &entry : *job_staged) {
      add_child_entry(entry.second.dest_path);
    }
  }

  // Add virtual subdirectories from files_wrote
  for (auto &path : it->second.files_wrote) {
    add_child_entry(path);
  }

  // Add visible entries (CAS-tracked files, symlinks, and directories)
  if (g_use_cas) {
    for (auto &ve : it->second.visible_entries) {
      add_child_entry(ve.first);
    }
  }

  return 0;
}

static int wakefuse_readdir_trace(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset,
                                  struct fuse_file_info *fi) {
  int out = wakefuse_readdir(path, buf, filler, offset, fi);
  fprintf(stderr, "readdir(%s, %lld) = %s\n", path, (long long)offset, trace_out(out));
  return out;
}

// Creates special files. Only used for job registration; regular mknod not supported.
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

  // TODO: Remove workspace writes once backwards compatibility is no longer needed
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

// Creates a file in the staging directory, not the workspace.
// The file will be ingested into CAS when the job completes.
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
      if (job.should_erase()) {
        g_staged_files.erase_job(jobid);
        context.jobs.erase(jobid);
      }
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

  if (g_use_cas) {
    // CAS mode: write to staging directory (wakebox will hash and store in CAS)
    // Check if this path was already staged by this job - if so, delete the old staging file
    if (StagedItem *existing = g_staged_files.find(key.first, key.second)) {
      if (auto existing_staging_path = existing->staging_path()) {
        unlink(existing_staging_path->data());
        g_staged_files.erase(key.first, key.second);
      }
    }

    // Include PID to avoid collisions between concurrent wake processes
    std::string staging_path =
        g_staging_dir + "/" + std::to_string(getpid()) + "_" + std::to_string(++g_staging_counter);
    mode_t perm_bits = mode & 07777;
    int fd = open(staging_path.c_str(), O_CREAT | O_RDWR | O_TRUNC, perm_bits);
    if (fd == -1) return -errno;

    StagedItem staged;
    staged.dest_path = key.second;
    staged.job_id = key.first;
    staged.data = StagedFileData{staging_path, mode};
    StagedItem *inserted = g_staged_files.insert(key.first, key.second, std::move(staged));

    g_staged_files.register_fd(fd, inserted);
    fi->fh = fd;
    it->second.staged_paths.insert(key.second);
  } else {
    // TODO: Remove the direct-to-workspace create path once WAKE_CAS is the default.
    // Legacy mode: create directly in workspace
    if (!it->second.is_writeable(key.second)) (void)deep_unlink(context.rootfd, key.second.c_str());
    int fd = openat(context.rootfd, key.second.c_str(), O_CREAT | O_RDWR | O_TRUNC, mode);
    if (fd == -1) return -errno;
    fi->fh = fd;
  }

  it->second.files_wrote.insert(key.second);
  return 0;
}

static int wakefuse_create_trace(const char *path, mode_t mode, struct fuse_file_info *fi) {
  int out = wakefuse_create(path, mode, fi);
  fprintf(stderr, "create(%s, 0%o) = %s\n", path, mode, trace_out(out));
  return out;
}

// Records a virtual directory. No directory is created on disk until CAS ingestion.
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

  // Already created by this job
  if (it->second.is_writeable(key.second)) return -EEXIST;

  if (g_use_cas) {
    // CAS mode: track as virtual directory, will be created during post-processing
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    StagedItem staged;
    staged.dest_path = key.second;
    staged.job_id = key.first;
    staged.data = StagedDirectoryData{mode, now};
    g_staged_files.insert(key.first, key.second, std::move(staged));
    it->second.staged_paths.insert(key.second);
  } else {
    // TODO: Remove the direct-to-workspace mkdir path once WAKE_CAS is the default.
    // Legacy mode: create directory in workspace
    int res = unlinkat(context.rootfd, key.second.c_str(), 0);
    if (res == -1 && errno != EPERM && errno != ENOENT && errno != EISDIR) return -errno;

    res = mkdirat(context.rootfd, key.second.c_str(), mode);

    // If a directory already exists, change permissions and claim it
    if (res == -1 && (errno == EEXIST || errno == EISDIR))
      res = fchmodat(context.rootfd, key.second.c_str(), mode, 0);

    if (res == -1) return -errno;
  }

  it->second.files_wrote.insert(key.second);
  return 0;
}

static int wakefuse_mkdir_trace(const char *path, mode_t mode) {
  int out = wakefuse_mkdir(path, mode);
  fprintf(stderr, "mkdir(%s, 0%o) = %s\n", path, mode, trace_out(out));
  return out;
}

// Removes a file. For staged files, deletes the staging file and removes tracking.
static int wakefuse_unlink(const char *path) {
  if (is_special(path)) return -EACCES;

  auto key = split_key(path);
  if (key.first.empty()) return -EPERM;

  auto it = context.jobs.find(key.first);
  if (it == context.jobs.end()) return -ENOENT;

  if (key.second == ".") return -EPERM;

  if (!it->second.is_readable(key.second)) return -ENOENT;

  if (!it->second.is_writeable(key.second)) return -EACCES;

  // Handle staged file removal
  if (StagedItem *sf = g_staged_files.find(key.first, key.second)) {
    // Only unlink staging file if it exists (files and hardlinks have staging_path)
    if (auto spath = sf->staging_path()) {
      unlink(spath->data());
    }
    g_staged_files.erase(key.first, key.second);
    it->second.staged_paths.erase(key.second);
    it->second.files_wrote.erase(key.second);
    it->second.files_read.erase(key.second);
    return 0;
  }

  // TODO: Remove workspace writes once backwards compatibility is no longer needed
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

// Removes a directory. For staged directories, fails if it has staged children.
static int wakefuse_rmdir(const char *path) {
  if (is_special(path)) return -ENOTDIR;

  auto key = split_key(path);
  if (key.first.empty()) return -EACCES;

  auto it = context.jobs.find(key.first);
  if (it == context.jobs.end()) return -ENOENT;

  if (key.second == ".") return -EACCES;

  if (!it->second.is_readable(key.second)) return -ENOENT;

  if (!it->second.is_writeable(key.second)) return -EACCES;

  // Handle staged directory removal
  if (StagedItem *sf = g_staged_files.find(key.first, key.second)) {
    if (sf->is_directory()) {
      // Check if directory has any staged children
      if (g_staged_files.has_children(key.first, key.second)) {
        return -ENOTEMPTY;
      }
      g_staged_files.erase(key.first, key.second);
      it->second.staged_paths.erase(key.second);
      it->second.files_wrote.erase(key.second);
      it->second.files_read.erase(key.second);
      return 0;
    }
  }

  // TODO: Remove workspace writes once backwards compatibility is no longer needed
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

// Records a virtual symlink. No symlink is created on disk until CAS ingestion.
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

  // Already created by this job
  if (it->second.is_writeable(key.second)) return -EEXIST;

  if (g_use_cas) {
    // CAS mode: track as virtual symlink, will be created during post-processing
    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);
    StagedItem staged;
    staged.dest_path = key.second;
    staged.job_id = key.first;
    staged.data = StagedSymlinkData{from, now};
    g_staged_files.insert(key.first, key.second, std::move(staged));
    it->second.staged_paths.insert(key.second);
  } else {
    // TODO: Remove the direct-to-workspace symlink path once WAKE_CAS is the default.
    // Legacy mode: create symlink in workspace
    if (!it->second.is_writeable(key.second)) (void)deep_unlink(context.rootfd, key.second.c_str());
    int res = symlinkat(from, context.rootfd, key.second.c_str());
    if (res == -1) return -errno;
  }

  it->second.files_wrote.insert(key.second);
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

// Move staged file entries that are children of dir to new paths under dest
// This is needed when a staged directory is renamed - all its children need new dest_paths
static void move_staged_children(const std::string &job_id, const std::string &dir,
                                 const std::string &dest) {
  auto *job_files = g_staged_files.get_job(job_id);
  if (!job_files) return;

  // Collect entries to move (can't modify map while iterating)
  std::vector<std::pair<std::string, StagedItem>> to_add;
  std::vector<std::string> to_remove;

  // Find all staged entries that are children of dir
  std::string prefix = dir + "/";
  for (auto it = job_files->lower_bound(prefix); it != job_files->end(); ++it) {
    // Stop when we've passed the prefix
    if (it->first.compare(0, prefix.size(), prefix) != 0) break;

    const std::string &old_path = it->first;
    std::string new_path = dest + old_path.substr(dir.size());
    StagedItem sf = it->second;
    sf.dest_path = new_path;
    to_add.push_back(std::make_pair(new_path, sf));
    to_remove.push_back(old_path);
  }

  // Remove old entries
  for (const auto &path : to_remove) {
    job_files->erase(path);
  }

  // Add new entries
  for (const auto &entry : to_add) {
    (*job_files)[entry.first] = entry.second;
  }
}

// Renames a file or directory. For staged items, updates paths in tracking.
// For staged directories, recursively updates all children's paths.
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

  // Handle staged file/directory rename
  if (StagedItem *from_sf = g_staged_files.find(keyf.first, keyf.second)) {
    StagedItem sf = *from_sf;
    sf.dest_path = keyt.second;
    g_staged_files.erase(keyf.first, keyf.second);
    // Check if destination already has a staged file - if so, delete its staging file
    if (StagedItem *existing_to = g_staged_files.find(keyt.first, keyt.second)) {
      if (auto existing_staging_path = existing_to->staging_path()) {
        unlink(existing_staging_path->data());
      }
    }
    g_staged_files.insert(keyt.first, keyt.second, sf);

    // Update the renamed item itself
    it->second.staged_paths.erase(keyf.second);
    it->second.staged_paths.insert(keyt.second);
    it->second.files_wrote.erase(keyf.second);
    it->second.files_read.erase(keyf.second);
    it->second.files_wrote.insert(keyt.second);

    // If this is a directory, also move all children in g_staged_files, staged_paths,
    // files_wrote, and files_read. This ensures that files inside a renamed directory
    // are still accessible under the new path.
    if (sf.is_directory()) {
      move_staged_children(keyf.first, keyf.second, keyt.second);
      move_members(it->second.staged_paths, it->second.staged_paths, keyf.second, keyt.second);
      move_members(it->second.files_wrote, it->second.files_wrote, keyf.second, keyt.second);
      move_members(it->second.files_read, it->second.files_read, keyf.second, keyt.second);
    }

    return 0;
  }

  // TODO: Remove workspace writes once backwards compatibility is no longer needed
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

// Creates a hardlink. In CAS mode, the new entry gets an independent copy of the
// source staging file (via reflink/copy), so each path owns its own staging file.
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

  // Handle link from staged file (CAS mode only)
  if (g_use_cas) {
    if (StagedItem *src_ptr = g_staged_files.find(keyf.first, keyf.second)) {
      const StagedItem &src = *src_ptr;
      // Hardlinks to directories are forbidden in POSIX
      if (src.is_directory()) return -EPERM;

      // Resolve source staging_path and metadata (works for both files and chained hardlinks)
      auto src_staging_path = src.staging_path();
      if (!src_staging_path) return -EPERM;  // symlinks have no staging_path

      mode_t src_mode = src.mode();

      // Create an independent copy of the staging file so each output owns its own
      // staging path. This avoids races where one consumer (CAS ingestion or workspace
      // materialization) deletes the shared file before another can read it.
      // Uses reflink (copy-on-write) when the filesystem supports it.
      std::string new_staging_path = g_staging_dir + "/" + std::to_string(getpid()) + "_" +
                                     std::to_string(++g_staging_counter);
      auto copy_result = wcl::reflink_or_copy_file(std::string(*src_staging_path), new_staging_path,
                                                   src_mode & 07777);
      if (!copy_result) return -copy_result.error();

      StagedItem sf;
      sf.dest_path = keyt.second;
      sf.job_id = keyf.first;
      sf.data = StagedHardlinkData{new_staging_path, src_mode};
      g_staged_files.insert(keyt.first, keyt.second, std::move(sf));

      it->second.staged_paths.insert(keyt.second);
      it->second.files_wrote.insert(keyt.second);
      // Both hardlink paths need direct_io to prevent kernel caching issues
      hardlinks.insert(std::string(from));
      hardlinks.insert(std::string(to));
      return 0;
    }
  }

  // TODO: Remove the workspace hardlink fallback once WAKE_CAS is the default.
  // Legacy mode (or non-staged source in CAS mode): create hardlink in workspace
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

// Changes file permissions. For staged items, updates stored mode for CAS ingestion.
static int wakefuse_chmod(const char *path, mode_t mode) {
  if (is_special(path)) return -EACCES;

  auto key = split_key(path);
  if (key.first.empty()) return -EACCES;

  auto it = context.jobs.find(key.first);
  if (it == context.jobs.end()) return -ENOENT;

  if (key.second == ".") return -EACCES;

  if (!it->second.is_readable(key.second)) return -ENOENT;

  if (!it->second.is_writeable(key.second)) return -EACCES;

  // Update mode in staged file if present
  if (StagedItem *sf = g_staged_files.find(key.first, key.second)) {
    sf->set_mode(mode);
    return 0;
  }
  assert(!g_use_cas && "chmod writing to workspace file in virtualization mode");

  // TODO: Remove workspace writes once backwards compatibility is no longer needed
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

// Changes ownership. No-op for staged items (ownership not tracked).
static int wakefuse_chown(const char *path, uid_t uid, gid_t gid) {
  if (is_special(path)) return -EACCES;

  auto key = split_key(path);
  if (key.first.empty()) return -EACCES;

  auto it = context.jobs.find(key.first);
  if (it == context.jobs.end()) return -ENOENT;

  if (key.second == ".") return -EACCES;

  if (!it->second.is_readable(key.second)) return -ENOENT;

  if (!it->second.is_writeable(key.second)) return -EACCES;

  // For staged files/directories, chown is a no-op (we don't track uid/gid)
  // but we should succeed rather than fail. See TODO in StagedFile struct.
  if (g_staged_files.find(key.first, key.second)) {
    return 0;
  }

  // TODO: Remove workspace writes once backwards compatibility is no longer needed
  int res = fchownat(context.rootfd, key.second.c_str(), uid, gid, AT_SYMLINK_NOFOLLOW);
  if (res == -1) return -errno;

  return 0;
}

static int wakefuse_chown_trace(const char *path, uid_t uid, gid_t gid) {
  int out = wakefuse_chown(path, uid, gid);
  fprintf(stderr, "chown(%s, %d, %d) = %s\n", path, uid, gid, trace_out(out));
  return out;
}

// Truncates a file. For staged files, truncates the staging file on disk.
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

  // Check if file is staged - if so, truncate the staging file
  if (StagedItem *sf = g_staged_files.find(key.first, key.second)) {
    // Truncate is not valid for directories or symlinks
    if (sf->is_directory()) return -EISDIR;
    if (sf->is_symlink()) return -EINVAL;

    auto spath = sf->staging_path();
    if (!spath) return -EINVAL;

    int fd = open(spath->data(), O_WRONLY);
    if (fd == -1) return -errno;

    int res = ftruncate(fd, size);
    if (res == -1) {
      res = -errno;
      (void)close(fd);
      return res;
    }
    (void)close(fd);
    return 0;
  }

  // TODO: Remove workspace writes once backwards compatibility is no longer needed
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

// Updates timestamps. For staged items, stores times for CAS ingestion.
static int wakefuse_utimens(const char *path, const struct timespec ts[2]) {
  if (is_special(path)) return -EACCES;

  auto key = split_key(path);
  if (key.first.empty()) return -EACCES;

  auto it = context.jobs.find(key.first);
  if (it == context.jobs.end()) return -ENOENT;

  if (key.second == ".") return -EACCES;

  if (!it->second.is_readable(key.second)) return -ENOENT;

  if (!it->second.is_writeable(key.second)) return -EACCES;

  if (StagedItem *sf = g_staged_files.find(key.first, key.second)) {
    if (auto spath = sf->staging_path()) {
      // File/hardlink: apply to backing (staing) file directly
      int res = utimensat(AT_FDCWD, spath->data(), ts, 0);
      if (res == -1) return -errno;
    } else {
      // Symlink/directory: no backing file, track in metadata
      sf->set_times(ts[0], ts[1]);
    }
    return 0;
  }
  assert(!g_use_cas && "utimens writing to workspace file in virtualization mode");

  // TODO: Remove workspace writes once backwards compatibility is no longer needed
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

// Opens a file. For staged files, opens the staging file on disk.
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
    // Hardlinked staged files have independent staging copies; writes would not
    // propagate to other links as a real hardlink would. Reject to avoid silent
    // divergence between hardlinked outputs.
    if ((fi->flags & O_ACCMODE) != O_RDONLY) return -EACCES;
    fi->direct_io = true;
  }

  // Check if file is staged (written by this job)
  if (StagedItem *sf = g_staged_files.find(key.first, key.second)) {
    // open() is not valid for directories
    if (sf->is_directory()) return -EISDIR;

    auto spath = sf->staging_path();
    if (!spath) return -EINVAL;

    int fd = open(spath->data(), fi->flags, 0);
    if (fd == -1) return -errno;
    g_staged_files.register_fd(fd, sf);
    fi->fh = fd;
    return 0;
  }

  // Check if this is a visible file with a known hash -> read from CAS (CAS mode only)
  if (g_use_cas) {
    auto visible_it = it->second.visible_entries.find(key.second);
    if (visible_it != it->second.visible_entries.end() && visible_it->second.content_hash) {
      const std::string &type = visible_it->second.type;
      if (type != "symlink" && type != "directory") {
        std::string blob_path = cas_blob_path(*visible_it->second.content_hash);
        int fd = open(blob_path.c_str(), O_RDONLY);
        if (fd != -1) {
          fi->fh = fd;
          return 0;
        }
        // Fall through to workspace if CAS blob not found
      }
    }
  }

  // Fallback: read from workspace
  // TODO: Remove workspace fallback once Source adds files to CAS directly
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

// Reads from an open file. May read from staging file if opened via staged path.
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

// Writes to an open file. May write to staging file if opened via staged path.
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
        s.job->second.dump(s.job->first);
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

// Returns filesystem statistics from the underlying workspace.
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

// Closes an open file. For staged files, syncs metadata from the staging file.
static int wakefuse_release(const char *path, struct fuse_file_info *fi) {
  if (fi->fh != BAD_FD) {
    // Check if this is a staged file
    if (g_staged_files.find_by_fd(fi->fh)) {
      int res = close(fi->fh);
      g_staged_files.unregister_fd(fi->fh);
      if (res == -1) return -errno;
      return 0;
    }

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
    if ('f' != s.kind && s.job->second.should_erase()) {
      g_staged_files.erase_job(s.job->first);
      context.jobs.erase(s.job);
    }
    if (context.should_exit()) schedule_exit();
  }

  return 0;
}

static int wakefuse_release_trace(const char *path, struct fuse_file_info *fi) {
  int out = wakefuse_release(path, fi);
  fprintf(stderr, "release(%s) = %s\n", path, trace_out(out));
  return out;
}

// Syncs file to disk. Operates on staging file if applicable.
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
// Preallocates space. For staged files, operates on the staging file.
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

  // Check if file is staged
  if (StagedItem *sf = g_staged_files.find(key.first, key.second)) {
    // fallocate is not valid for directories or symlinks
    if (sf->is_directory()) return -EISDIR;
    if (sf->is_symlink()) return -EINVAL;

    auto spath = sf->staging_path();
    if (!spath) return -EINVAL;

    int fd = open(spath->data(), O_WRONLY);
    if (fd == -1) return -errno;

    int res = posix_fallocate(fd, offset, length);
    if (res != 0) {
      (void)close(fd);
      return -res;
    }
    (void)close(fd);
    return 0;
  }

  // TODO: Remove workspace writes once backwards compatibility is no longer needed
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

  if (argc < 3 || argc > 4) {
    fprintf(stderr, "Syntax: fuse-waked <mount-point> <min-timeout-seconds> [--use-cas]\n");
    goto term;
  }
  path = argv[1];

  // Check for --use-cas flag
  if (argc == 4 && strcmp(argv[3], "--use-cas") == 0) {
    g_use_cas = true;
  }

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
