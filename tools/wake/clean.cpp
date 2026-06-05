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

int remove_paths(Database &db, CASContext &cas_ctx, const std::vector<std::string> &paths) {
  if (paths.empty()) {
    std::cerr << "error: no paths specified" << std::endl;
    return EX_USAGE;
  }

  auto *cas = cas_ctx.get_store();
  if (!cas) {
    std::cerr << "error: CAS store not initialized" << std::endl;
    return EXIT_FAILURE;
  }

  // Remove files from database and CAS within a single transaction.
  db.remove_files(cas, paths);

  // Delete workspace files outside of that transaction.
  for (const auto &path : paths) {
    if (unlink(path.c_str()) != 0 && errno != ENOENT) {
      std::cerr << "warning: failed to remove file '" << path << "': " << strerror(errno)
                << std::endl;
    }
  }

  return EXIT_SUCCESS;
}
