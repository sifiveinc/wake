/*
 * Copyright 2023 SiFive, Inc.
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

/* Wake vfork exec shim */
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/time.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <future>
#include <iostream>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "cas/resolve_path.h"

static bool use_cas_hashes() { return getenv("WAKE_CAS") != nullptr; }

static std::optional<cas::ResolvedPath> do_resolve(const char* file) {
  auto policy =
      use_cas_hashes() ? cas::SpecialFilePolicy::Reject : cas::SpecialFilePolicy::LegacyExoticHash;

  auto result = cas::resolve_path(file, policy);
  if (!result) {
    std::cerr << "wake-hash: " << result.error() << std::endl;
    return {};
  }
  return *result;
}

std::vector<std::optional<cas::ResolvedPath>> resolve_all_files(
    const std::vector<std::string>& files_to_hash) {
  std::atomic<size_t> counter{0};
  // We have to pre-alocate all the results so that we can overwrite them each
  // at anytime and maintain order
  std::vector<std::optional<cas::ResolvedPath>> identities(files_to_hash.size());
  // The cost of thread creation is fairly low with Linux on x86 so we allow opening up-to one
  // thread per-file.
  size_t num_threads = std::min(size_t(std::thread::hardware_concurrency()), files_to_hash.size());
  // We need to join all the threads at the end so we keep a to_join list
  std::vector<std::future<void>> to_join;

  // A common case is that we only hash one file so optimize for that case
  if (num_threads == 1) {
    identities[0] = do_resolve(files_to_hash[0].c_str());
    return identities;
  }

  // Now kick off our threads
  for (size_t i = 0; i < num_threads; ++i) {
    // In each thread we work steal a thing to hash
    to_join.emplace_back(std::async([&counter, &identities, &files_to_hash]() {
      while (true) {
        size_t idx = counter.fetch_add(1);
        // No more work to do so we exit
        if (idx >= files_to_hash.size()) {
          return;
        }
        // Output the result directly into the output location. This
        // lets us maintain the output order while not worrying about
        // the order in which things are added.
        identities[idx] = do_resolve(files_to_hash[idx].c_str());
      }
    }));
  }

  // Now join all of our threads
  for (auto& fut : to_join) {
    fut.wait();
  }

  return identities;
}

int main(int argc, char** argv) {
  std::vector<std::string> files_to_hash;
  bool emit_type = false;
  int arg_start = 1;

  if (argc > 1 && std::string(argv[1]) == "--typed") {
    emit_type = true;
    arg_start = 2;
  }

  // Find all the files we want to hash. Sometimes there are too many
  // files to hash and we cannot accept them via the command line. In this
  // case we accept them via stdin
  if (argc == arg_start + 1 && std::string(argv[arg_start]) == "@") {
    std::string line;
    while (std::getline(std::cin, line)) {
      if (line == "\n") break;
      files_to_hash.push_back(line);
    }
  } else {
    for (int i = arg_start; i < argc; ++i) {
      files_to_hash.push_back(argv[i]);
    }
  }

  std::vector<std::optional<cas::ResolvedPath>> identities = resolve_all_files(files_to_hash);

  // Now output them in the same order that we received them. If we could
  // not hash something, return "BadHash" in that case.
  for (auto& identity : identities) {
    if (identity) {
      if (emit_type) {
        std::cout << identity->type << ":" << identity->hash << ":"
                  << static_cast<unsigned>(identity->mode & 07777) << std::endl;
      } else {
        std::cout << identity->hash << std::endl;
      }
    } else {
      std::cout << "BadHash" << std::endl;
    }
  }

  return 0;
}
