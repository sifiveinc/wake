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

#include "cas/content_hash.h"

static std::optional<std::string> do_hash(const char* file) {
  struct stat st;
  if (lstat(file, &st) != 0) {
    std::cerr << "wake-hash: lstat(" << file << "): " << strerror(errno) << std::endl;
    return {};
  }

  if (S_ISLNK(st.st_mode)) {
    // For symlinks, hash the target string rather than following the link.
    std::string target(8192, '\0');
    ssize_t len = readlink(file, target.data(), target.size());
    if (len < 0) {
      std::cerr << "wake-hash: readlink(" << file << "): " << strerror(errno) << std::endl;
      return {};
    }
    target.resize(len);
    return cas::ContentHash::from_string(target).to_hex();
  }

  auto result = cas::ContentHash::from_file(file);
  if (!result) {
    std::cerr << "wake-hash: read(" << file << "): " << strerror(result.error()) << std::endl;
    return {};
  }
  return result->to_hex();
}

std::vector<std::optional<std::string>> hash_all_files(
    const std::vector<std::string>& files_to_hash) {
  std::atomic<size_t> counter{0};
  // We have to pre-alocate all the hashes so that we can overwrite them each
  // at anytime and maintain order
  std::vector<std::optional<std::string>> hashes(files_to_hash.size());
  // The cost of thread creation is fairly low with Linux on x86 so we allow opening up-to one
  // thread per-file.
  size_t num_threads = std::min(size_t(std::thread::hardware_concurrency()), files_to_hash.size());
  // We need to join all the threads at the end so we keep a to_join list
  std::vector<std::future<void>> to_join;

  // A common case is that we only hash one file so optimize for that case
  if (num_threads == 1) {
    hashes[0] = do_hash(files_to_hash[0].c_str());
    return hashes;
  }

  // Now kick off our threads
  for (size_t i = 0; i < num_threads; ++i) {
    // In each thread we work steal a thing to hash
    to_join.emplace_back(std::async([&counter, &hashes, &files_to_hash]() {
      while (true) {
        size_t idx = counter.fetch_add(1);
        // No more work to do so we exit
        if (idx >= files_to_hash.size()) {
          return;
        }
        // Output the result directly into the output location. This
        // lets us maintain the output order while not worrying about
        // the order in which things are added.
        hashes[idx] = do_hash(files_to_hash[idx].c_str());
      }
    }));
  }

  // Now join all of our threads
  for (auto& fut : to_join) {
    fut.wait();
  }

  return hashes;
}

int main(int argc, char** argv) {
  std::vector<std::string> files_to_hash;

  // Find all the files we want to hash. Sometimes there are too many
  // files to hash and we cannot accept them via the command line. In this
  // case we accept them via stdin
  if (argc == 2 && std::string(argv[1]) == "@") {
    std::string line;
    while (std::getline(std::cin, line)) {
      if (line == "\n") break;
      files_to_hash.push_back(line);
    }
  } else {
    for (int i = 1; i < argc; ++i) {
      files_to_hash.push_back(argv[i]);
    }
  }

  std::vector<std::optional<std::string>> hashes = hash_all_files(files_to_hash);

  // Now output them in the same order that we received them. If we could
  // not hash something, return "BadHash" in that case.
  for (auto& hash : hashes) {
    if (hash) {
      std::cout << *hash << std::endl;
    } else {
      std::cout << "BadHash" << std::endl;
    }
  }

  return 0;
}
