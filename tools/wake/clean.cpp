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

#include <iostream>

#include "cas/content_hash.h"
#include "util/execpath.h"
#include "wcl/filepath.h"

int remove_paths(Database &db, CASContext &cas_ctx, const std::vector<std::string> &paths,
                 const std::string &wake_cwd) {
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
    std::string normalized;

    if (wcl::is_relative(path)) {
      // Interpret relative paths according to where wake was invoked (wake_cwd).
      // `wake_cwd` format is "" or "subdir/", so it doesn't need an extra '/'.
      std::string full_path = wake_cwd + path;
      normalized = wcl::make_canonical(full_path);
    } else {
      // Additionally relativize absolute paths to match their database representation.
      normalized = wcl::relative_to(workspace_root, path);
    }

    // Reject dangerous paths
    if (normalized.empty() || normalized == ".") {
      std::cerr << "wake --rm: cannot remove the workspace root: '" << path << "'" << std::endl;
      return EXIT_FAILURE;
    }
    if (normalized == "wake.db") {
      std::cerr << "wake --rm: cannot remove the Wake database: '" << path << "'" << std::endl;
      return EXIT_FAILURE;
    }
    if (normalized == ".build/cas" || normalized.find(".build/cas/") == 0) {
      std::cerr << "wake --rm: remove materialized paths, not the CAS blobs directly: '" << path
                << "'" << std::endl;
      return EXIT_FAILURE;
    }

    normalized_paths.push_back(std::move(normalized));
  }

  // Remove files from database and CAS within a single transaction.
  // `deleted_blobs` is currently unused, but is returned for future reporting (e.g. freed space).
  auto [paths_to_remove, _] = db.remove_blobs(cas, normalized_paths);

  // Delete the returned workspace files outside of that transaction.
  for (const auto &path : paths_to_remove) {
    if (unlink(path.c_str()) != 0 && errno != ENOENT) {
      std::cerr << "warning: failed to remove file '" << path << "': " << strerror(errno)
                << std::endl;
    }
  }

  return EXIT_SUCCESS;
}
