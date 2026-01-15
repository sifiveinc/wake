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

// CAS-integrated job cache utilities
// These functions provide CAS-based storage for job cache outputs,
// enabling content deduplication and efficient materialization via reflinks.

#pragma once

#include <string>
#include <vector>

#include "cas.h"
#include "cas_store.h"
#include "wcl/result.h"

namespace cas {

// Error types for CAS job cache operations
enum class CASJobCacheError {
  StoreOpenFailed,
  BlobStoreFailed,
  BlobReadFailed,
  MaterializeFailed,
  FileNotFound,
  IOError
};

// Result of storing a job's outputs in CAS
struct CASJobOutputs {
  // Combined hash of all output files (computed from individual hashes)
  ContentHash tree_hash;

  // Individual file hashes for compatibility with existing job cache
  std::vector<std::pair<std::string, ContentHash>> file_hashes;
};

// Store a single file in CAS and return its content hash
// This is useful for storing individual output files
wcl::result<ContentHash, CASJobCacheError> store_output_file(CASStore& store,
                                                             const std::string& source_path);

// Store multiple output files in CAS
// Returns a combined hash representing all the files
wcl::result<CASJobOutputs, CASJobCacheError> store_output_files(
    CASStore& store,
    const std::vector<std::pair<std::string, std::string>>& files,  // (source_path, relative_path)
    const std::vector<std::pair<std::string, mode_t>>& modes);      // (relative_path, mode)

// Materialize a file from CAS to a destination path
// Uses reflinks when possible for efficiency
wcl::result<bool, CASJobCacheError> materialize_file(CASStore& store, const ContentHash& hash,
                                                     const std::string& dest_path, mode_t mode);

// Check if a blob exists in CAS (useful for cache hit detection)
bool has_blob(CASStore& store, const ContentHash& hash);

// Get the CAS store path for a given cache directory
std::string get_cas_store_path(const std::string& cache_dir);

}  // namespace cas
