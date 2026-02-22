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

// Open Group Base Specifications Issue 7
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "result.h"

namespace wcl {

class unique_fd {
 private:
  int fd = -1;

 public:
  unique_fd() = default;
  unique_fd(const unique_fd&) = delete;
  explicit unique_fd(int fd) : fd(fd) {}

  unique_fd& operator=(unique_fd&& f) {
    fd = f.fd;
    f.fd = -1;
    return *this;
  }
  unique_fd(unique_fd&& f) : fd(f.fd) { f.fd = -1; }

  ~unique_fd() {
    // We can't actully handle the error here because
    // destructors and constructors assume exceptions
    // will be used :(
    close();
  }

  void close() {
    if (fd > 0) {
      ::close(fd);
      fd = -1;
    }
  }

  bool valid() const { return fd > 0; }

  int get() const {
    assert(valid());
    return fd;
  }

  static result<unique_fd, posix_error_t> open(const char* str, int flags) {
    int fd = ::open(str, flags);
    if (fd == -1) {
      return make_errno<unique_fd>();
    }
    return make_result<unique_fd, posix_error_t>(unique_fd(fd));
  }

  // Helper that only returns successful file opens and exits
  // otherwise.
  static result<unique_fd, posix_error_t> open(const char* str, int flags, mode_t mode) {
    int fd = ::open(str, flags, mode);
    if (fd == -1) {
      return make_errno<unique_fd>();
    }
    return make_result<unique_fd, posix_error_t>(unique_fd(fd));
  }
};

template <int flags, mode_t mode>
class precise_unique_fd {
 private:
  unique_fd fd;

  precise_unique_fd(unique_fd&& fd) : fd(std::move(fd)) {}

 public:
  precise_unique_fd(precise_unique_fd&&) = default;
  precise_unique_fd(const precise_unique_fd&) = delete;
  precise_unique_fd() = delete;

  static result<precise_unique_fd, posix_error_t> open(const char* str) {
    auto fd = unique_fd::open(str, flags, mode);
    if (!fd) {
      return make_error<precise_unique_fd, posix_error_t>(fd.error());
    }
    return make_result<precise_unique_fd, posix_error_t>(precise_unique_fd(std::move(*fd)));
  }

  int get() const { return fd.get(); }

  bool valid() const { return fd.valid(); }
};

}  // namespace wcl
