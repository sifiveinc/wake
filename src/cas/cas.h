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

#include "blake2/blake2.h"
#include "wcl/result.h"

namespace cas {

// Forward declarations
class CASStore;

// 256-bit content hash using BLAKE2b
struct ContentHash {
  uint64_t data[4] = {0};

  ContentHash() = default;
  ContentHash(const ContentHash&) = default;
  ContentHash& operator=(const ContentHash&) = default;

  // Create hash from raw bytes
  static ContentHash from_bytes(const uint8_t* bytes, size_t len);

  // Create hash from a file's contents
  static wcl::result<ContentHash, wcl::posix_error_t> from_file(const std::string& path);

  // Create hash from string data
  static ContentHash from_string(const std::string& data);

  // Create hash from hex string (64 characters)
  static ContentHash from_hex(const std::string& hex);

  // Convert to hex string
  std::string to_hex() const;

  // Get the first two hex characters (for directory sharding)
  std::string prefix() const;

  // Get the remaining hex characters (for filename)
  std::string suffix() const;

  bool operator==(const ContentHash& other) const;
  bool operator!=(const ContentHash& other) const;
  bool operator<(const ContentHash& other) const;

  // Check if hash is zero/empty
  bool is_empty() const;
};

}  // namespace cas
