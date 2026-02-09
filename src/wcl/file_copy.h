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

#include <sys/types.h>

#include <string>

#include "result.h"

namespace wcl {

// Strategy used for file copying
enum class CopyStrategy {
  Reflink,  // Copy-on-write clone (fastest, shares blocks)
  Copy      // Full copy (slowest, but always works)
};

// Result of a copy operation
struct CopyResult {
  CopyStrategy strategy_used;
  size_t bytes_copied;  // 0 for reflink/hardlink, actual bytes for copy
};

// ============================================================================
// File copy operations with fallback chain:
//   reflink -> copy
// ============================================================================

// Copy a file with automatic strategy selection
// Tries reflink first, then falls back to full copy
// Returns the strategy that was used
result<CopyResult, posix_error_t> reflink_or_copy_file(const std::string& src, const std::string& dst,
                                                       mode_t mode);

// Try to reflink a file (copy-on-write clone)
// Returns success (true) if reflink worked, error if not supported
result<bool, posix_error_t> try_reflink(const std::string& src, const std::string& dst,
                                        mode_t mode);

// Full copy of a file using sendfile/copy_file_range
result<size_t, posix_error_t> copy_file_full(const std::string& src, const std::string& dst,
                                             mode_t mode);

// ============================================================================
// Filesystem capability detection
// ============================================================================

// Check if the filesystem supports reflinks
// (checks if src and dst are on same filesystem that supports FICLONE)
bool supports_reflink(const std::string& path);

}  // namespace wcl

