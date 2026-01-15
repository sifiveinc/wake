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

#include <sys/types.h>

#include <string>

#include "wcl/result.h"

namespace cas {

// Strategy used for file copying
enum class CopyStrategy {
  Reflink,   // Copy-on-write clone (fastest, shares blocks)
  Hardlink,  // Hard link (fast, shares inode)
  Copy       // Full copy (slowest, but always works)
};

// Result of a copy operation
struct CopyResult {
  CopyStrategy strategy_used;
  size_t bytes_copied;  // 0 for reflink/hardlink, actual bytes for copy
};

// ============================================================================
// File copy operations with fallback chain:
//   reflink -> hardlink -> copy
// ============================================================================

// Copy a file with automatic strategy selection
// Tries reflink first, then hardlink (if allow_hardlink), then full copy
// Returns the strategy that was used
wcl::result<CopyResult, wcl::posix_error_t> copy_file(
    const std::string& src,
    const std::string& dst,
    mode_t mode,
    bool allow_hardlink = true);

// Try to reflink a file (copy-on-write clone)
// Returns success (true) if reflink worked, error if not supported
wcl::result<bool, wcl::posix_error_t> try_reflink(
    const std::string& src,
    const std::string& dst,
    mode_t mode);

// Try to hardlink a file
// Returns success (true) if hardlink worked, error otherwise
wcl::result<bool, wcl::posix_error_t> try_hardlink(
    const std::string& src,
    const std::string& dst);

// Full copy of a file using sendfile/copy_file_range
wcl::result<size_t, wcl::posix_error_t> copy_file_full(
    const std::string& src,
    const std::string& dst,
    mode_t mode);

// ============================================================================
// Directory operations
// ============================================================================

// Create a directory and all parent directories
// Returns true on success
wcl::result<bool, wcl::posix_error_t> mkdir_parents(const std::string& path);

// Check if a path exists
bool path_exists(const std::string& path);

// Check if a path is a directory
bool is_directory(const std::string& path);

// Check if a path is a regular file
bool is_regular_file(const std::string& path);

// Check if a path is a symlink
bool is_symlink(const std::string& path);

// Read a symlink target
wcl::result<std::string, wcl::posix_error_t> read_symlink(const std::string& path);

// Create a symlink
// Returns true on success
wcl::result<bool, wcl::posix_error_t> create_symlink(
    const std::string& target,
    const std::string& link_path);

// Get file mode
wcl::result<mode_t, wcl::posix_error_t> get_file_mode(const std::string& path);

// ============================================================================
// Filesystem capability detection
// ============================================================================

// Check if the filesystem supports reflinks
// (checks if src and dst are on same filesystem that supports FICLONE)
bool supports_reflink(const std::string& path);

// Check if two paths are on the same filesystem
bool same_filesystem(const std::string& path1, const std::string& path2);

}  // namespace cas

