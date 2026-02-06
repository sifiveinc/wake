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
#include <errno.h>
#include <fcntl.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <future>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

#include "blake2/blake2.h"
#include "compat/nofollow.h"
#include "wcl/optional.h"
#include "wcl/unique_fd.h"

#include <optional>
#include "wcl/xoshiro_256.h"

// Can increase to 64 if needed
#define HASH_BYTES 32

static inline uint8_t hex_to_nibble(char hex) {
  if (hex >= '0' && hex <= '9') return hex - '0';
  if (hex >= 'a' && hex <= 'f') return hex - 'a' + 10;
  if (hex >= 'A' && hex <= 'F') return hex - 'A' + 10;
  // This case is invalid but I suspect it will blow
  // up the quickest if violated.
  return 0xFF;
}

template <size_t size>
static inline void get_hex_data(const std::string& s, uint8_t (*data)[size]) {
  uint8_t* start = *data;
  const uint8_t* end = start + size;
  for (size_t i = 0; start < end && i < s.size(); i += 2) {
    // Bytes are in little endian but nibbles are in big endian...
    // I could put the entire number in big endian but that's extremely
    // frustrating to work with...but also if the nibbles aren't in big
    // endian it gets really confusing to look at certain things when you
    // use the pure-little endian to_hex. So to make to_hex and get_hex_data
    // match and to make to_hex results easy to read, we do this.
    start[0] = (hex_to_nibble(s[i]) << 4) & 0xF0;
    if (i + 1 < s.size()) start[0] |= hex_to_nibble(s[i + 1]) & 0xF;
    ++start;
  }
}

struct Hash256 {
  uint64_t data[4] = {0};

  Hash256() {
    data[0] = 0;
    data[1] = 0;
    data[2] = 0;
    data[3] = 0;
  }
  Hash256(const Hash256& other) {
    data[0] = other.data[0];
    data[1] = other.data[1];
    data[2] = other.data[2];
    data[3] = other.data[3];
  }

  static Hash256 from_hex(const std::string& hash) {
    assert(hash.size() == 64);
    Hash256 out;
    get_hex_data(hash, reinterpret_cast<uint8_t(*)[32]>(&out.data));
    return out;
  }

  static Hash256 from_hash(uint8_t (*data)[32]) {
    Hash256 out;
    uint64_t(&data64)[4] = *reinterpret_cast<uint64_t(*)[4]>(data);
    out.data[0] = data64[0];
    out.data[1] = data64[1];
    out.data[2] = data64[2];
    out.data[3] = data64[3];
    return out;
  }

  std::string to_hex() const { return wcl::to_hex(&data); }

  bool operator==(Hash256 other) {
    return data[0] == other.data[0] && data[1] == other.data[1] && data[2] == other.data[2] &&
           data[3] == other.data[3];
  }

  bool operator!=(Hash256 other) { return !(*this == other); }
};

// If a file handle is not a symlink, directory, or regular file
// then we consider it "exotic". This includes block devices,
// character devices, FIFOs, and sockets.
static std::optional<Hash256> hash_exotic() {
  Hash256 out;
  out.data[0] = 1;
  return wcl::make_some<Hash256>(out);
}

static std::optional<Hash256> hash_dir() { return wcl::some(Hash256()); }

static std::optional<Hash256> hash_link(const char* link) {
  blake2b_state S;
  uint8_t hash[HASH_BYTES];
  std::vector<char> buffer(8192, 0);

  while (true) {
    int bytes_read = readlink(link, buffer.data(), buffer.size());
    if (bytes_read < 0) {
      std::cerr << "wake-hash: readlink(" << link << "): " << strerror(errno) << std::endl;
      return {};
    }
    if (static_cast<size_t>(bytes_read) != buffer.size()) {
      buffer.resize(bytes_read);
      break;
    }
    buffer.resize(2 * buffer.size(), 0);
  }

  blake2b_init(&S, sizeof(hash));
  blake2b_update(&S, reinterpret_cast<uint8_t*>(buffer.data()), buffer.size());
  blake2b_final(&S, &hash[0], sizeof(hash));

  return wcl::some(Hash256::from_hash(&hash));
}

static std::optional<Hash256> hash_file(const char* file, int fd) {
  blake2b_state S;
  uint8_t hash[HASH_BYTES], buffer[8192];
  ssize_t got;

  blake2b_init(&S, sizeof(hash));
  while ((got = read(fd, &buffer[0], sizeof(buffer))) > 0) blake2b_update(&S, &buffer[0], got);
  blake2b_final(&S, &hash[0], sizeof(hash));

  if (got < 0) {
    std::cerr << "wake-hash read(" << file << "): " << strerror(errno) << std::endl;
    return {};
  }

  return wcl::some(Hash256::from_hash(&hash));
}

static std::optional<Hash256> do_hash(const char* file) {
  struct stat stat;
  auto fd = wcl::unique_fd::open(file, O_RDONLY | O_NOFOLLOW);

  if (!fd) {
    if (fd.error() == EISDIR) return hash_dir();
    if (fd.error() == ELOOP || errno == EMLINK) return hash_link(file);
    if (fd.error() == ENXIO) return hash_exotic();
    std::cerr << "wake-hash open(" << file << "): " << strerror(errno);
    return {};
  }

  if (fstat(fd->get(), &stat) != 0) {
    if (errno == EISDIR) return hash_dir();
    std::cerr << "wake-hash fstat(" << file << "): " << strerror(errno);
    return {};
  }

  if (S_ISDIR(stat.st_mode)) return hash_dir();
  if (S_ISLNK(stat.st_mode)) return hash_link(file);
  if (S_ISREG(stat.st_mode)) return hash_file(file, fd->get());

  return hash_exotic();
}

std::vector<std::optional<Hash256>> hash_all_files(const std::vector<std::string>& files_to_hash) {
  std::atomic<size_t> counter{0};
  // We have to pre-alocate all the hashes so that we can overwrite them each
  // at anytime and maintain order
  std::vector<std::optional<Hash256>> hashes(files_to_hash.size());
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

  std::vector<std::optional<Hash256>> hashes = hash_all_files(files_to_hash);

  // Now output them in the same order that we received them. If we could
  // not hash something, return "BadHash" in that case.
  for (auto& hash : hashes) {
    if (hash) {
      std::cout << hash->to_hex() << std::endl;
    } else {
      std::cout << "BadHash" << std::endl;
    }
  }

  return 0;
}
