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

#include "cas.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <sstream>

#include "wcl/file_ops.h"

namespace fs = std::filesystem;

namespace cas {

// Helper functions for hash-based directory sharding
static std::string hash_prefix(const ContentHash& hash) {
  std::string hex = hash.to_hex();
  return hex.substr(0, 2);
}

static std::string hash_suffix(const ContentHash& hash) {
  std::string hex = hash.to_hex();
  return hex.substr(2);
}

std::string cas_error_to_string(CASError error) {
  switch (error) {
    case CASError::NotFound:
      return "Not found";
    case CASError::IOError:
      return "I/O error";
    case CASError::CorruptedData:
      return "Corrupted data";
    case CASError::AlreadyExists:
      return "Already exists";
    case CASError::InvalidHash:
      return "Invalid hash";
    default:
      return "Unknown error";
  }
}

Cas::Cas(const std::string& root, const std::string& blobs_dir, const std::string& staging_dir)
    : root_(root), blobs_dir_(blobs_dir), staging_dir_(staging_dir) {}

wcl::result<Cas, CASError> Cas::open(const std::string& root, const std::string& blobs_subdir,
                                     const std::string& staging_subdir) {
  std::string blobs_dir = (fs::path(root) / blobs_subdir).string();
  std::string staging_dir = (fs::path(root) / staging_subdir).string();
  Cas store(root, blobs_dir, staging_dir);

  // Create directory structure
  std::error_code ec;
  fs::create_directories(store.blobs_dir_, ec);
  if (ec) {
    return wcl::make_error<Cas, CASError>(CASError::IOError);
  }
  fs::create_directories(store.staging_dir_, ec);
  if (ec) {
    return wcl::make_error<Cas, CASError>(CASError::IOError);
  }

  return wcl::make_result<Cas, CASError>(std::move(store));
}

wcl::result<bool, CASError> Cas::ensure_shard_dir(const ContentHash& hash) const {
  std::error_code ec;
  fs::create_directories(fs::path(blobs_dir_) / hash_prefix(hash), ec);
  if (ec) {
    return wcl::make_error<bool, CASError>(CASError::IOError);
  }
  return wcl::make_result<bool, CASError>(true);
}

std::string Cas::blob_path(const ContentHash& hash) const {
  return (fs::path(blobs_dir_) / hash_prefix(hash) / hash_suffix(hash)).string();
}

bool Cas::has_blob(const ContentHash& hash) const { return fs::exists(blob_path(hash)); }

wcl::result<ContentHash, CASError> Cas::store_blob_from_file(const std::string& path) {
  std::error_code ec;

  // Get source file mode
  auto perms = fs::status(path, ec).permissions();
  if (ec) {
    return wcl::make_error<ContentHash, CASError>(CASError::IOError);
  }

  // Extract permission bits from fs::perms enum
  mode_t mode = static_cast<mode_t>(perms) & 07777;

  // Copy to staging area first.
  std::string temp = (fs::path(staging_dir_) /
                      (fs::path(path).filename().string() + "." + std::to_string(getpid())))
                         .string();
  auto copy_result = wcl::reflink_or_copy_file(path, temp, mode);
  if (!copy_result) {
    fs::remove(temp, ec);
    return wcl::make_error<ContentHash, CASError>(CASError::IOError);
  }

  // Hash what we actually stored
  auto hash_result = ContentHash::from_file(temp);
  if (!hash_result) {
    fs::remove(temp, ec);
    return wcl::make_error<ContentHash, CASError>(CASError::IOError);
  }
  ContentHash hash = *hash_result;

  // Check if blob already exists
  std::string dest = blob_path(hash);
  if (fs::exists(dest)) {
    fs::remove(temp, ec);
    return wcl::make_result<ContentHash, CASError>(hash);
  }

  // Ensure shard directory exists
  auto shard_result = ensure_shard_dir(hash);
  if (!shard_result) {
    fs::remove(temp, ec);
    return wcl::make_error<ContentHash, CASError>(shard_result.error());
  }

  // Atomic rename into place
  fs::rename(temp, dest, ec);
  if (ec) {
    fs::remove(temp, ec);
    return wcl::make_error<ContentHash, CASError>(CASError::IOError);
  }

  return wcl::make_result<ContentHash, CASError>(hash);
}

wcl::result<ContentHash, CASError> Cas::store_blob(const std::string& data) {
  ContentHash hash = ContentHash::from_string(data);

  // Check if blob already exists
  std::string dest = blob_path(hash);
  if (fs::exists(dest)) {
    return wcl::make_result<ContentHash, CASError>(hash);
  }

  // Ensure shard directory exists
  auto shard_result = ensure_shard_dir(hash);
  if (!shard_result) {
    return wcl::make_error<ContentHash, CASError>(shard_result.error());
  }

  // Write data to file
  std::string temp =
      (fs::path(staging_dir_) / (hash.to_hex() + "." + std::to_string(getpid()))).string();
  std::error_code ec;
  {
    std::ofstream ofs(temp, std::ios::binary);
    if (!ofs) {
      return wcl::make_error<ContentHash, CASError>(CASError::IOError);
    }
    ofs.write(data.data(), data.size());
    if (!ofs) {
      fs::remove(temp, ec);  // cleanup on failure (ignore error)
      return wcl::make_error<ContentHash, CASError>(CASError::IOError);
    }
  }

  // Atomically insert into the CAS via rename
  fs::rename(temp, dest, ec);
  if (ec) {
    fs::remove(temp, ec);  // cleanup on failure (ignore error)
    return wcl::make_error<ContentHash, CASError>(CASError::IOError);
  }

  return wcl::make_result<ContentHash, CASError>(hash);
}

wcl::result<std::string, CASError> Cas::read_blob(const ContentHash& hash) const {
  std::string path = blob_path(hash);

  std::ifstream ifs(path, std::ios::binary);
  if (!ifs) {
    return wcl::make_error<std::string, CASError>(CASError::NotFound);
  }

  std::ostringstream oss;
  oss << ifs.rdbuf();
  if (!ifs && !ifs.eof()) {
    return wcl::make_error<std::string, CASError>(CASError::IOError);
  }

  return wcl::make_result<std::string, CASError>(oss.str());
}

wcl::result<bool, CASError> Cas::materialize_blob(const ContentHash& hash,
                                                  const std::string& dest_path, mode_t mode) const {
  std::string src_path = blob_path(hash);

  if (!fs::exists(src_path)) {
    return wcl::make_error<bool, CASError>(CASError::NotFound);
  }

  // Create parent directories if needed
  fs::path dest_fs_path(dest_path);
  std::error_code ec;
  if (dest_fs_path.has_parent_path()) {
    fs::create_directories(dest_fs_path.parent_path(), ec);
    if (ec) {
      return wcl::make_error<bool, CASError>(CASError::IOError);
    }
  }

  // Copy to temp file first, then atomically rename to destination.
  std::string temp_path = dest_path + "." + std::to_string(getpid());
  auto copy_result = wcl::reflink_or_copy_file(src_path, temp_path, mode);
  if (!copy_result) {
    fs::remove(temp_path, ec);
    return wcl::make_error<bool, CASError>(CASError::IOError);
  }

  // Atomically rename over destination - last one wins
  fs::rename(temp_path, dest_path, ec);
  if (ec) {
    fs::remove(temp_path, ec);
    return wcl::make_error<bool, CASError>(CASError::IOError);
  }

  return wcl::make_result<bool, CASError>(true);
}

}  // namespace cas
