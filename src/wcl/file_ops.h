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
enum class CopyStrategy { Reflink, Copy };

// Result of a copy operation
struct CopyResult {
  CopyStrategy strategy_used;
  size_t bytes_copied;  // 0 for reflink, actual bytes for copy
};

// Tries reflink first, then falls back to std::filesystem::copy_file
// Returns the strategy that was used
result<CopyResult, posix_error_t> reflink_or_copy_file(const std::string& src,
                                                       const std::string& dst, mode_t mode);

// Try to reflink a file (copy-on-write clone)
result<bool, posix_error_t> try_reflink(const std::string& src, const std::string& dst,
                                        mode_t mode);

}  // namespace wcl
