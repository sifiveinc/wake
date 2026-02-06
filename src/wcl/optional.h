/*
 * Copyright 2022 SiFive, Inc.
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

#include <optional>
#include <type_traits>
#include <utility>

namespace wcl {

// `some` is used when you want to wrap a known value in an optional
// and don't want to mess with template arguments.
// Example: some(10) would be of type std::optional<int>
template <class T>
inline std::optional<std::decay_t<T>> some(T&& x) {
  return std::make_optional(std::forward<T>(x));
}

// `make_some` is the more general sibling of `some` which
// allows you to construct the value in place using any constructor.
// The type must be specified when using this.
template <class T, class... Args>
inline std::optional<T> make_some(Args&&... args) {
  return std::optional<T>{std::in_place, std::forward<Args>(args)...};
}

}  // namespace wcl
