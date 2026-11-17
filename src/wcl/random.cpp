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

#include "random.h"

#if defined(__linux__) || defined(__APPLE__)
#include <uuid/uuid.h>

#include <cstdint>
#include <exception>
#include <random>
#include <utility>
#endif

namespace wcl {

result<std::string, std::string> random_hex(size_t num_digits) {
  if (num_digits == 0) return make_result<std::string, std::string>("");

#if defined(__linux__) || defined(__APPLE__)
  try {
    std::random_device random("/dev/urandom");
    std::uniform_int_distribution<std::uint32_t> draw;
    constexpr char hex[] = "0123456789abcdef";
    std::string text(num_digits, '\0');
    for (size_t i = 0; i < num_digits;) {
      const std::uint32_t value = draw(random);
      // Extract up to eight hex digits from the 32-bit value, four bits at a time.
      for (int shift = 28; shift >= 0 && i < num_digits; shift -= 4, ++i) {
        text[i] = hex[(value >> shift) & 0x0f];
      }
    }
    return make_result<std::string, std::string>(std::move(text));
  } catch (const std::exception& error) {
    return make_error<std::string, std::string>(error.what());
  }
#else
  return make_error<std::string, std::string>(
      "Random hex generation is only supported on native Linux and macOS");
#endif
}

result<std::string, std::string> uuidv4() {
#if defined(__linux__) || defined(__APPLE__)
  uuid_t id;
  uuid_generate_random(id);
  char text[37];
  uuid_unparse_lower(id, text);
  return make_result<std::string, std::string>(text);
#else
  return make_error<std::string, std::string>(
      "UUIDv4 generation is only supported on native Linux and macOS");
#endif
}

}  // namespace wcl
