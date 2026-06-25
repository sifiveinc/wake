/*
 * Copyright 2019 SiFive, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You should have received a copy of LICENSE.Apache2 along with
 * this software. If not, you may obtain a copy of LICENSE.Apache2 along with
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

#include "clean.h"

#include <errno.h>
#include <string.h>
#include <sysexits.h>
#include <unistd.h>

#include <filesystem>
#include <iostream>

#include "cas/content_hash.h"
#include "util/execpath.h"

int remove_paths(Database &db, CASContext &cas_ctx, const std::vector<std::string> &paths,
                 const std::string &wake_cwd, bool recursive) {
  if (paths.empty()) {
    std::cerr << "error: no paths specified" << std::endl;
    return EX_USAGE;
  }

  auto *cas = cas_ctx.get_store();
  if (!cas) {
    std::cerr << "error: CAS store not initialized" << std::endl;
    return EXIT_FAILURE;
  }
  // Normalize all paths to be relative to the workspace root, allowing this to be called from
  // directories other than the root.
  std::string workspace_root = get_cwd();
  std::vector<std::string> normalized_paths;
  normalized_paths.reserve(paths.size());

  for (const auto &path : paths) {
    std::filesystem::path workspace_path(path);

    if (workspace_path.is_relative()) {
      // Interpret relative paths according to where wake was invoked (wake_cwd).
      // `wake_cwd` format is "" or "subdir/", but coercion means the result will never be absolute.
      workspace_path = wake_cwd / workspace_path;
      workspace_path = workspace_path.lexically_normal();
    } else {
      // Additionally relativize absolute paths to match their database representation.
      workspace_path = std::filesystem::relative(workspace_path, workspace_root);
    }

    // Reject dangerous paths
    if (workspace_path == ".") {
      std::cerr << "wake --rm: cannot remove the workspace root: '" << path << "'" << std::endl;
      return EXIT_FAILURE;
    }
    if (workspace_path == "wake.db") {
      std::cerr << "wake --rm: cannot remove the Wake database: '" << path << "'" << std::endl;
      return EXIT_FAILURE;
    }
    auto relative_to_cas = workspace_path.lexically_relative(".build/cas");
    if (relative_to_cas.empty() || relative_to_cas.string().find("..") != 0) {
      std::cerr << "wake --rm: remove materialized paths, not the CAS blobs directly: '" << path
                << "'" << std::endl;
      return EXIT_FAILURE;
    }

    normalized_paths.push_back(workspace_path.string());
  }

  // Remove files from database and CAS within a single transaction.
  auto result = db.remove_blobs(cas, normalized_paths, recursive);

  // Delete the returned workspace files outside of that transaction.
  for (const auto &path : result.files) {
    if (unlink(path.c_str()) != 0 && errno != ENOENT) {
      std::cerr << "warning: failed to remove file '" << path << "': " << strerror(errno)
                << std::endl;
    }
  }

  // Remove directories in reverse order (deepest first) to ensure children are removed first.
  for (auto it = result.directories.rbegin(); it != result.directories.rend(); ++it) {
    if (rmdir(it->c_str()) != 0 && errno != ENOENT) {
      std::cerr << "warning: failed to remove directory '" << *it << "': " << strerror(errno)
                << std::endl;
    }
  }

  return EXIT_SUCCESS;
}
