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

#include "resolve_path.h"

#include "compat/nofollow.h"

#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include <optional>
#include <sstream>
#include <vector>

#include "wcl/unique_fd.h"

namespace cas {

namespace {

constexpr const char* kDirectoryHash =
    "0000000000000000000000000000000000000000000000000000000000000000";
constexpr mode_t kSymlinkMode = 0777;

enum class PathKind { Directory, Symlink, RegularFile, Exotic };

std::string legacy_exotic_hash() {
  ContentHash out;
  out.data[0] = 1;
  return out.to_hex();
}

std::string syscall_error(const char* op, const std::string& path, int err) {
  std::stringstream str;
  str << op << "(" << path << "): " << strerror(err);
  return str.str();
}

wcl::result<PathKind, std::string> classify_path(const std::string& path) {
  auto fd = wcl::unique_fd::open(path.c_str(), O_RDONLY | O_NOFOLLOW);
  if (!fd) {
    if (fd.error() == EISDIR) return wcl::make_result<PathKind, std::string>(PathKind::Directory);
    if (fd.error() == ELOOP || fd.error() == EMLINK)
      return wcl::make_result<PathKind, std::string>(PathKind::Symlink);
    if (fd.error() == ENXIO) return wcl::make_result<PathKind, std::string>(PathKind::Exotic);
    return wcl::make_error<PathKind, std::string>(syscall_error("open", path, fd.error()));
  }

  struct stat statbuf;
  if (fstat(fd->get(), &statbuf) != 0) {
    if (errno == EISDIR) return wcl::make_result<PathKind, std::string>(PathKind::Directory);
    return wcl::make_error<PathKind, std::string>(syscall_error("fstat", path, errno));
  }

  if (S_ISDIR(statbuf.st_mode)) return wcl::make_result<PathKind, std::string>(PathKind::Directory);
  if (S_ISLNK(statbuf.st_mode)) return wcl::make_result<PathKind, std::string>(PathKind::Symlink);
  if (S_ISREG(statbuf.st_mode))
    return wcl::make_result<PathKind, std::string>(PathKind::RegularFile);
  return wcl::make_result<PathKind, std::string>(PathKind::Exotic);
}

wcl::result<std::string, std::string> read_symlink_target(const std::string& path) {
  std::vector<char> buffer(8192, 0);

  while (true) {
    ssize_t bytes_read = readlink(path.c_str(), buffer.data(), buffer.size());
    if (bytes_read < 0) {
      std::stringstream str;
      str << "readlink(" << path << "): " << strerror(errno);
      return wcl::make_error<std::string, std::string>(str.str());
    }
    if (static_cast<size_t>(bytes_read) != buffer.size()) {
      return wcl::make_result<std::string, std::string>(std::string(buffer.data(), bytes_read));
    }
    buffer.resize(2 * buffer.size(), 0);
  }
}

}  // namespace

wcl::result<ResolvedPath, std::string> resolve_path(const std::string& path,
                                                    SpecialFilePolicy special_policy) {
  auto kind_result = classify_path(path);
  if (!kind_result) return wcl::make_error<ResolvedPath, std::string>(kind_result.error());
  PathKind kind = *kind_result;

  struct stat statbuf;
  if (lstat(path.c_str(), &statbuf) != 0) {
    return wcl::make_error<ResolvedPath, std::string>(syscall_error("lstat", path, errno));
  }

  if (kind == PathKind::Directory) {
    return wcl::make_result<ResolvedPath, std::string>(
        ResolvedPath{"directory", kDirectoryHash, static_cast<mode_t>(statbuf.st_mode & 07777)});
  }

  if (kind == PathKind::Symlink) {
    auto target_result = read_symlink_target(path);
    if (!target_result) {
      return wcl::make_error<ResolvedPath, std::string>(target_result.error());
    }

    std::string hash = ContentHash::from_string(*target_result).to_hex();
    return wcl::make_result<ResolvedPath, std::string>(ResolvedPath{"symlink", hash, kSymlinkMode});
  }

  if (kind == PathKind::RegularFile) {
    auto hash_result = ContentHash::from_file(path);
    if (!hash_result) {
      std::stringstream str;
      str << "read(" << path << "): " << strerror(hash_result.error());
      return wcl::make_error<ResolvedPath, std::string>(str.str());
    }
    std::string hash = hash_result->to_hex();
    return wcl::make_result<ResolvedPath, std::string>(
        ResolvedPath{"file", hash, static_cast<mode_t>(statbuf.st_mode & 07777)});
  }

  if (special_policy == SpecialFilePolicy::LegacyExoticHash) {
    return wcl::make_result<ResolvedPath, std::string>(
        ResolvedPath{"file", legacy_exotic_hash(), static_cast<mode_t>(statbuf.st_mode & 07777)});
  }

  std::stringstream str;
  str << "unsupported file type for " << path;
  return wcl::make_error<ResolvedPath, std::string>(str.str());
}

}  // namespace cas
