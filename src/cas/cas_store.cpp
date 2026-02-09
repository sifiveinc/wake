/*
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

#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include "cas_store.h"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <fstream>
#include <sstream>

#include "util/mkdir_parents.h"
#include "wcl/file_copy.h"
#include "wcl/filepath.h"

namespace cas {

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

// ============================================================================
// CASStore construction
// ============================================================================

CASStore::CASStore(std::string root)
    : root_(std::move(root)), blobs_dir_(wcl::join_paths(root_, "blobs")) {}

wcl::result<CASStore, CASError> CASStore::open(const std::string& root) {
  CASStore store(root);

  // Create directory structure
  if (mkdir_with_parents(store.blobs_dir_, 0755) != 0) {
    return wcl::make_error<CASStore, CASError>(CASError::IOError);
  }

  return wcl::make_result<CASStore, CASError>(std::move(store));
}

// ============================================================================
// Helper methods
// ============================================================================

wcl::result<bool, CASError> CASStore::ensure_shard_dir(const ContentHash& hash) const {
  std::string shard_dir = wcl::join_paths(blobs_dir_, hash.prefix());
  if (mkdir_with_parents(shard_dir, 0755) != 0) {
    return wcl::make_error<bool, CASError>(CASError::IOError);
  }
  return wcl::make_result<bool, CASError>(true);
}

std::string CASStore::blob_path(const ContentHash& hash) const {
  return wcl::join_paths(blobs_dir_, hash.prefix(), hash.suffix());
}

// ============================================================================
// Blob operations
// ============================================================================

bool CASStore::has_blob(const ContentHash& hash) const { return wcl::path_exists(blob_path(hash)); }

wcl::result<ContentHash, CASError> CASStore::store_blob_from_file(const std::string& path) {
  // First, compute the hash
  auto hash_result = ContentHash::from_file(path);
  if (!hash_result) {
    return wcl::make_error<ContentHash, CASError>(CASError::IOError);
  }

  ContentHash hash = *hash_result;

  // Check if blob already exists
  std::string dest = blob_path(hash);
  if (wcl::path_exists(dest)) {
    return wcl::make_result<ContentHash, CASError>(hash);
  }

  // Ensure shard directory exists
  auto shard_result = ensure_shard_dir(hash);
  if (!shard_result) {
    return wcl::make_error<ContentHash, CASError>(shard_result.error());
  }

  // Get source file mode
  auto mode_result = wcl::get_file_mode(path);
  if (!mode_result) {
    return wcl::make_error<ContentHash, CASError>(CASError::IOError);
  }

  // Copy file to store (using reflink if possible)
  auto copy_result = wcl::reflink_or_copy_file(path, dest, *mode_result);
  if (!copy_result) {
    return wcl::make_error<ContentHash, CASError>(CASError::IOError);
  }

  return wcl::make_result<ContentHash, CASError>(hash);
}

wcl::result<ContentHash, CASError> CASStore::store_blob(const std::string& data) {
  ContentHash hash = ContentHash::from_string(data);

  // Check if blob already exists
  std::string dest = blob_path(hash);
  if (wcl::path_exists(dest)) {
    return wcl::make_result<ContentHash, CASError>(hash);
  }

  // Ensure shard directory exists
  auto shard_result = ensure_shard_dir(hash);
  if (!shard_result) {
    return wcl::make_error<ContentHash, CASError>(shard_result.error());
  }

  // Write data to file
  std::string temp_dest = dest + ".tmp";
  {
    std::ofstream ofs(temp_dest, std::ios::binary);
    if (!ofs) {
      return wcl::make_error<ContentHash, CASError>(CASError::IOError);
    }
    ofs.write(data.data(), data.size());
    if (!ofs) {
      unlink(temp_dest.c_str());
      return wcl::make_error<ContentHash, CASError>(CASError::IOError);
    }
  }

  // Atomic rename
  if (rename(temp_dest.c_str(), dest.c_str()) < 0) {
    unlink(temp_dest.c_str());
    return wcl::make_error<ContentHash, CASError>(CASError::IOError);
  }

  return wcl::make_result<ContentHash, CASError>(hash);
}

wcl::result<std::string, CASError> CASStore::read_blob(const ContentHash& hash) const {
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

wcl::result<bool, CASError> CASStore::materialize_blob(const ContentHash& hash,
                                                       const std::string& dest_path,
                                                       mode_t mode) const {
  std::string src_path = blob_path(hash);

  if (!wcl::path_exists(src_path)) {
    return wcl::make_error<bool, CASError>(CASError::NotFound);
  }

  // Create parent directories if needed
  auto parent = wcl::parent_and_base(dest_path);
  if ((bool)parent && !parent->first.empty()) {
    if (mkdir_with_parents(parent->first, 0755) != 0) {
      return wcl::make_error<bool, CASError>(CASError::IOError);
    }
  }

  // Remove existing file if present (copy_file uses O_EXCL)
  (void)unlink(dest_path.c_str());

  // Use reflink/copy to materialize
  auto copy_result = wcl::reflink_or_copy_file(src_path, dest_path, mode);
  if (!copy_result) {
    return wcl::make_error<bool, CASError>(CASError::IOError);
  }

  return wcl::make_result<bool, CASError>(true);
}

}  // namespace cas
