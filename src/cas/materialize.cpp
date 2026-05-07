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

#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include "materialize.h"

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <optional>
#include <string>

#include "content_hash.h"
#include "util/mkdir_parents.h"

// Global counter for unique temp file names during materialization
// Used to avoid collisions when multiple processes materialize files concurrently
static std::atomic<uint64_t> g_materialize_counter{0};

namespace {

static std::string make_temp_path(const std::string& dest) {
  return dest + "." + std::to_string(getpid()) + "." +
         std::to_string(g_materialize_counter.fetch_add(1));
}

static std::optional<std::string> atomic_replace(const std::string& temp, const std::string& dest,
                                                 const std::string& label) {
  std::error_code ec;
  std::filesystem::rename(temp, dest, ec);
  if (!ec) return std::nullopt;
  std::filesystem::remove(temp, ec);
  return "Failed to place " + label + " " + dest + ": " + ec.message();
}

static bool apply_mtime(const std::string& path, time_t sec, long nsec, int flags) {
  if (sec == 0 && nsec == 0) return true;
  struct timespec times[2];
  times[0].tv_sec = 0;
  times[0].tv_nsec = UTIME_OMIT;
  times[1].tv_sec = sec;
  times[1].tv_nsec = nsec;
  return utimensat(AT_FDCWD, path.c_str(), times, flags) == 0;
}

static std::optional<std::string> make_dir_or_chmod(const std::string& path, mode_t mode) {
  if (mkdir(path.c_str(), mode) != 0) {
    if (errno == EEXIST) {
      chmod(path.c_str(), mode);
    } else {
      return "Failed to create directory " + path + ": " + strerror(errno);
    }
  }
  return std::nullopt;
}

static std::optional<std::string> ensure_parent_dirs(const std::string& dest) {
  std::filesystem::path parent = std::filesystem::path(dest).parent_path();
  if (!parent.empty() && mkdir_with_parents(parent.string(), 0755) != 0)
    return "Failed to create parent directories for " + dest;
  return std::nullopt;
}

}  // namespace

namespace cas {

std::optional<std::string> materialize_item(Cas& store, const std::string& dest_path,
                                            const std::string& type,
                                            const std::string& hash_or_target, mode_t mode,
                                            time_t mtime_sec, long mtime_nsec) {
  if (auto msg = ensure_parent_dirs(dest_path)) return msg;

  if (type == "file") {
    auto hash_result = ContentHash::from_hex(hash_or_target);
    if (!hash_result) return "Invalid CAS hash: " + hash_or_target;

    auto mat = store.materialize_blob(*hash_result, dest_path, mode, mtime_sec, mtime_nsec);
    if (!mat) return "Failed to materialize blob " + hash_or_target + " to " + dest_path;

  } else if (type == "symlink") {
    auto hash_result = ContentHash::from_hex(hash_or_target);
    if (!hash_result) return "Invalid CAS hash: " + hash_or_target;

    auto target = store.read_blob(*hash_result);
    if (!target) return "Failed to materialize symlink " + hash_or_target + " to " + dest_path;

    std::string temp_path = make_temp_path(dest_path);
    if (symlink(target->c_str(), temp_path.c_str()) != 0)
      return "Failed to create symlink " + dest_path + ": " + strerror(errno);

    (void)apply_mtime(temp_path, mtime_sec, mtime_nsec, AT_SYMLINK_NOFOLLOW);

    if (auto msg = atomic_replace(temp_path, dest_path, "symlink")) return msg;

  } else if (type == "directory") {
    struct stat st;
    if (stat(dest_path.c_str(), &st) == 0) {
      if (S_ISDIR(st.st_mode)) {
        chmod(dest_path.c_str(), mode);
      } else {
        (void)unlink(dest_path.c_str());
        if (auto msg = make_dir_or_chmod(dest_path, mode)) return msg;
      }
    } else {
      if (auto msg = make_dir_or_chmod(dest_path, mode)) return msg;
    }

    if (!apply_mtime(dest_path, mtime_sec, mtime_nsec, 0))
      return "Failed to update timestamps for directory " + dest_path + ": " + strerror(errno);

  } else {
    return "Unknown item type: " + type;
  }

  return std::nullopt;
}

}  // namespace cas
