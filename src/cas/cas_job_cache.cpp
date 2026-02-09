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

#include "cas_job_cache.h"

#include <sstream>

#include "util/mkdir_parents.h"
#include "wcl/file_copy.h"
#include "wcl/filepath.h"

namespace cas {

std::string get_cas_store_path(const std::string& cache_dir) {
  return wcl::join_paths(cache_dir, "cas");
}

wcl::result<ContentHash, CASJobCacheError> store_output_file(CASStore& store,
                                                             const std::string& source_path) {
  auto result = store.store_blob_from_file(source_path);
  if (!result) {
    return wcl::make_error<ContentHash, CASJobCacheError>(CASJobCacheError::BlobStoreFailed);
  }
  return wcl::make_result<ContentHash, CASJobCacheError>(*result);
}

wcl::result<CASJobOutputs, CASJobCacheError> store_output_files(
    CASStore& store, const std::vector<std::pair<std::string, std::string>>& files,
    const std::vector<std::pair<std::string, mode_t>>& /* modes */) {
  CASJobOutputs outputs;

  // Build a string from all file hashes to compute combined hash
  std::ostringstream combined;

  // Store each file
  for (const auto& file : files) {
    const std::string& source_path = file.first;
    const std::string& relative_path = file.second;

    // Store the file content
    auto hash_result = store.store_blob_from_file(source_path);
    if (!hash_result) {
      return wcl::make_error<CASJobOutputs, CASJobCacheError>(CASJobCacheError::BlobStoreFailed);
    }

    ContentHash hash = *hash_result;
    outputs.file_hashes.push_back({relative_path, hash});

    // Add to combined hash input
    combined << relative_path << ":" << hash.to_hex() << "\n";
  }

  // Compute combined hash from all file hashes
  outputs.tree_hash = ContentHash::from_string(combined.str());
  return wcl::make_result<CASJobOutputs, CASJobCacheError>(std::move(outputs));
}

wcl::result<bool, CASJobCacheError> materialize_file(CASStore& store, const ContentHash& hash,
                                                     const std::string& dest_path, mode_t mode) {
  // Get the blob path in CAS
  std::string blob_path = store.blob_path(hash);

  // Create parent directories
  auto parent_base = wcl::parent_and_base(dest_path);
  if ((bool)parent_base && !parent_base->first.empty()) {
    if (mkdir_with_parents(parent_base->first, 0755) != 0) {
      return wcl::make_error<bool, CASJobCacheError>(CASJobCacheError::IOError);
    }
  }

  // Copy using reflink if possible
  auto copy_result = wcl::reflink_or_copy_file(blob_path, dest_path, mode);
  if (!copy_result) {
    return wcl::make_error<bool, CASJobCacheError>(CASJobCacheError::MaterializeFailed);
  }

  return wcl::make_result<bool, CASJobCacheError>(true);
}

bool has_blob(CASStore& store, const ContentHash& hash) { return store.has_blob(hash); }

}  // namespace cas
