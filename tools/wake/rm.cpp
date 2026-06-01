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

#include "rm.h"

#include <errno.h>
#include <string.h>
#include <unistd.h>

#include <iostream>

int remove_paths(Database &db, const std::vector<std::string> &paths) {
  if (paths.empty()) {
    std::cerr << "error: no paths specified" << std::endl;
    return 1;
  }

  // For each path, find all jobs that output it and their CAS blob hashes
  for (const auto &path : paths) {
    auto results = db.get_output_hashes(path);

    if (results.empty()) {
      std::cerr << "warning: no jobs found with output '" << path << "'" << std::endl;
      continue;
    }

    // Display what we found (to stderr for debugging)
    for (const auto &result : results) {
      long job_id = std::get<0>(result);
      const std::string &label = std::get<1>(result);
      const std::string &hash = std::get<2>(result);

      std::cerr << "Job " << job_id << " (" << label << ") outputs '"
                << path << "' with hash '" << hash << "'" << std::endl;
    }

    // Delete the workspace file
    if (unlink(path.c_str()) == 0) {
      std::cerr << "Removed file: " << path << std::endl;
    } else if (errno == ENOENT) {
      // File doesn't exist - that's fine, we wanted to remove it anyway
      std::cerr << "File already removed: " << path << std::endl;
    } else {
      std::cerr << "warning: failed to remove file '" << path << "': "
                << strerror(errno) << std::endl;
    }

    // TODO: Implement the following steps:
    // 3. Check if each blob is referenced by other jobs
    // 4. Remove the database record
    // 6. Garbage collect unreferenced CAS blobs
  }

  return 0;
}
