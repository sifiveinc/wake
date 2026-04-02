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

// Open Group Base Specifications Issue 7
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include "content_hash.h"

#include <fcntl.h>
#include <unistd.h>

#include "wcl/unique_fd.h"

namespace cas {

// Helper to convert nibble to hex char
static char nibble_to_hex(uint8_t nibble) { return nibble < 10 ? '0' + nibble : 'a' + nibble - 10; }

// Helper to convert hex char to nibble
static uint8_t hex_to_nibble(char hex) {
  if (hex >= '0' && hex <= '9') return hex - '0';
  if (hex >= 'a' && hex <= 'f') return hex - 'a' + 10;
  if (hex >= 'A' && hex <= 'F') return hex - 'A' + 10;
  return 0xFF;
}

ContentHash ContentHash::from_bytes(const uint8_t* bytes, size_t len) {
  ContentHash hash;
  blake2b_state state;
  blake2b_init(&state, sizeof(hash.data));
  blake2b_update(&state, bytes, len);
  blake2b_final(&state, reinterpret_cast<uint8_t*>(hash.data), sizeof(hash.data));
  return hash;
}

ContentHash ContentHash::from_string(const std::string& data) {
  return from_bytes(reinterpret_cast<const uint8_t*>(data.data()), data.size());
}

wcl::result<ContentHash, wcl::posix_error_t> ContentHash::from_file(const std::string& path) {
  auto fd = wcl::unique_fd::open(path.c_str(), O_RDONLY);
  if (!fd) {
    return wcl::make_error<ContentHash, wcl::posix_error_t>(fd.error());
  }

  ContentHash hash;
  blake2b_state state;
  blake2b_init(&state, sizeof(hash.data));

  static thread_local uint8_t buffer[8192];
  ssize_t bytes_read;
  while ((bytes_read = read(fd->get(), buffer, sizeof(buffer))) > 0) {
    blake2b_update(&state, buffer, bytes_read);
  }

  if (bytes_read < 0) {
    return wcl::make_errno<ContentHash>();
  }

  blake2b_final(&state, reinterpret_cast<uint8_t*>(hash.data), sizeof(hash.data));
  return wcl::make_result<ContentHash, wcl::posix_error_t>(hash);
}

wcl::result<ContentHash, ContentHashError> ContentHash::from_hex(const std::string& hex) {
  if (hex.size() != 64) {
    return wcl::make_error<ContentHash, ContentHashError>(ContentHashError::InvalidHexLength);
  }

  ContentHash hash;
  uint8_t* bytes = reinterpret_cast<uint8_t*>(hash.data);
  for (size_t i = 0; i < 32; ++i) {
    uint8_t high = hex_to_nibble(hex[i * 2]);
    uint8_t low = hex_to_nibble(hex[i * 2 + 1]);
    if (high == 0xFF || low == 0xFF) {
      return wcl::make_error<ContentHash, ContentHashError>(ContentHashError::InvalidHexChar);
    }
    bytes[i] = (high << 4) | low;
  }
  return wcl::make_result<ContentHash, ContentHashError>(hash);
}

std::string ContentHash::to_hex() const {
  std::string result;
  result.reserve(64);
  const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data);
  for (size_t i = 0; i < 32; ++i) {
    // Extract high nibble (upper 4 bits) and convert to hex
    result += nibble_to_hex((bytes[i] >> 4) & 0x0F);
    // Extract low nibble (lower 4 bits) and convert to hex
    result += nibble_to_hex(bytes[i] & 0x0F);
  }
  return result;
}

bool ContentHash::operator==(const ContentHash& other) const {
  return data[0] == other.data[0] && data[1] == other.data[1] && data[2] == other.data[2] &&
         data[3] == other.data[3];
}

bool ContentHash::operator!=(const ContentHash& other) const { return !(*this == other); }

bool ContentHash::operator<(const ContentHash& other) const {
  for (int i = 0; i < 4; ++i) {
    if (data[i] < other.data[i]) return true;
    if (data[i] > other.data[i]) return false;
  }
  return false;
}

}  // namespace cas
