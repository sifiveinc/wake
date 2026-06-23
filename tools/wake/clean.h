/*
 * Copyright 2019 SiFive, Inc.
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

#ifndef RM_H
#define RM_H

#include <string>
#include <vector>

#include "runtime/cas_context.h"
#include "runtime/database.h"

// Remove specified paths from the workspace, and any corresponding, exclusively-owned CAS blobs.
// Returns 0 on success, EX_USAGE (64) if `paths` is empty, and 1 on other errors, for easy CLI use.
// wake_cwd is the path where wake was invoked, relative to the workspace root ("" or "subdir/").
// If recursive is true, directories will recursively include all their children.
int remove_paths(Database &db, CASContext &cas_ctx, const std::vector<std::string> &paths,
                 const std::string &wake_cwd, bool recursive);

#endif
