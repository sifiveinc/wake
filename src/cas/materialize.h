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

#include <sys/types.h>

#include <optional>
#include <string>

#include "cas.h"

namespace cas {

// Materialize a single item from CAS (or create symlink/directory) at dest_path.
// Creates parent directories as needed.
// - type="file":      hash_or_target = hex hash of file blob; uses mode and mtime
// - type="symlink":   hash_or_target = hex hash of symlink target string blob in CAS; uses mtime
// - type="directory": hash_or_target = "" (unused); uses mode and mtime
// Returns nullopt on success, error message on failure.
std::optional<std::string> materialize_item(Cas& store, const std::string& dest_path,
                                            const std::string& type,
                                            const std::string& hash_or_target, mode_t mode,
                                            time_t mtime_sec, long mtime_nsec);

}  // namespace cas
