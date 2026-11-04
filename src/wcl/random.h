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

#pragma once

#include <cstddef>
#include <string>

#include "result.h"

namespace wcl {

// Generate exactly digits lowercase hexadecimal characters using OS randomness. Zero returns an
// empty string on every target.
result<std::string, std::string> random_hex(size_t num_digits);

// Generate a UUIDv4 in canonical lowercase, hyphenated form.
result<std::string, std::string> uuidv4();

}  // namespace wcl
