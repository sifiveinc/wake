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

#pragma once

#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include <string>

#include "cas.h"
#include "wcl/result.h"

namespace cas {

// Error types for CAS operations
enum class CASError { NotFound, IOError, CorruptedData, AlreadyExists, InvalidHash };

// Convert CASError to string for logging
std::string cas_error_to_string(CASError error);

// CAS Store - manages content-addressable storage for blobs
// Directory structure:
//   {root}/
//     blobs/
//       {prefix}/
//         {suffix}          # Blob content
class CASStore {
 public:
  CASStore() = delete;
  CASStore(const CASStore&) = delete;
  CASStore(CASStore&&) = default;

  // Create a CAS store at the given root directory
  // Creates the directory structure if it doesn't exist
  static wcl::result<CASStore, CASError> open(const std::string& root);

  // Get the root directory of this store
  const std::string& root() const { return root_; }

  // Store a blob from a file, returns the content hash
  // Uses reflink if possible, otherwise copies the file
  wcl::result<ContentHash, CASError> store_blob_from_file(const std::string& path);

  // Store a blob from memory, returns the content hash
  wcl::result<ContentHash, CASError> store_blob(const std::string& data);

  // Check if a blob exists
  bool has_blob(const ContentHash& hash) const;

  // Get the path to a blob in the store (may not exist)
  std::string blob_path(const ContentHash& hash) const;

  // Read a blob's contents
  wcl::result<std::string, CASError> read_blob(const ContentHash& hash) const;

  // Materialize a blob to a file path (uses reflink if possible)
  wcl::result<bool, CASError> materialize_blob(const ContentHash& hash,
                                               const std::string& dest_path, mode_t mode) const;

 private:
  std::string root_;
  std::string blobs_dir_;

  explicit CASStore(std::string root);

  // Ensure the shard directory exists for a given hash
  // Returns true on success
  wcl::result<bool, CASError> ensure_shard_dir(const ContentHash& hash) const;
};

}  // namespace cas
