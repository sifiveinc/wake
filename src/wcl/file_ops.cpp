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

#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include "file_ops.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <filesystem>

#include "unique_fd.h"

// Linux-specific includes for reflink
#ifdef __linux__
#include <sys/ioctl.h>
#ifndef FICLONE
#include <linux/fs.h>
#endif
#ifdef FICLONE
#define HAS_FICLONE 1
#endif
#endif

namespace wcl {

result<CopyResult, posix_error_t> reflink_or_copy_fd(int src_fd, int dst_fd, bool attempt_reflink) {
#ifdef HAS_FICLONE
  if (attempt_reflink && ioctl(dst_fd, FICLONE, src_fd) == 0)
    return make_result<CopyResult, posix_error_t>(CopyResult{CopyStrategy::Reflink, 0});
  if (attempt_reflink && errno != EOPNOTSUPP && errno != ENOTTY && errno != EINVAL &&
      errno != EXDEV)
    return make_errno<CopyResult>();
#else
  (void)attempt_reflink;
#endif
  char buffer[64 * 1024];
  size_t copied = 0;
  for (;;) {
    ssize_t read_bytes = read(src_fd, buffer, sizeof(buffer));
    if (read_bytes == 0) break;
    if (read_bytes < 0) {
      if (errno == EINTR) continue;
      return make_errno<CopyResult>();
    }
    ssize_t offset = 0;
    while (offset < read_bytes) {
      ssize_t written = write(dst_fd, buffer + offset, static_cast<size_t>(read_bytes - offset));
      if (written < 0) {
        if (errno == EINTR) continue;
        return make_errno<CopyResult>();
      }
      offset += written;
    }
    copied += static_cast<size_t>(read_bytes);
  }
  return make_result<CopyResult, posix_error_t>(CopyResult{CopyStrategy::Copy, copied});
}

result<bool, posix_error_t> try_reflink(const std::string& src, const std::string& dst,
                                        mode_t mode) {
#ifdef HAS_FICLONE
  auto src_fd = unique_fd::open(src.c_str(), O_RDONLY);
  if (!src_fd) return make_error<bool, posix_error_t>(src_fd.error());
  auto dst_fd = unique_fd::open(dst.c_str(), O_WRONLY | O_CREAT | O_EXCL, mode);
  if (!dst_fd) return make_error<bool, posix_error_t>(dst_fd.error());
  if (ioctl(dst_fd->get(), FICLONE, src_fd->get()) < 0) {
    const int saved = errno;
    dst_fd->close();
    std::error_code ignored;
    std::filesystem::remove(dst, ignored);
    return make_error<bool, posix_error_t>(saved);
  }
  return make_result<bool, posix_error_t>(true);
#else
  (void)src;
  (void)dst;
  (void)mode;
  return make_error<bool, posix_error_t>(EOPNOTSUPP);
#endif
}

result<CopyResult, posix_error_t> reflink_or_copy_file(const std::string& src,
                                                       const std::string& dst, mode_t mode,
                                                       bool attempt_reflink) {
  if (attempt_reflink) {
    auto reflink = try_reflink(src, dst, mode);
    if (reflink)
      return make_result<CopyResult, posix_error_t>(CopyResult{CopyStrategy::Reflink, 0});
    const int error = reflink.error();
    if (error != EOPNOTSUPP && error != ENOTTY && error != EINVAL && error != EXDEV)
      return make_error<CopyResult, posix_error_t>(error);
  }
  std::error_code error;
  std::filesystem::copy_file(src, dst, error);
  if (error) return make_error<CopyResult, posix_error_t>(error.value());
  std::filesystem::permissions(dst, static_cast<std::filesystem::perms>(mode), error);
  if (error) {
    const int saved = error.value();
    std::filesystem::remove(dst, error);
    return make_error<CopyResult, posix_error_t>(saved);
  }
  const auto size = std::filesystem::file_size(dst, error);
  if (error) {
    const int saved = error.value();
    std::filesystem::remove(dst, error);
    return make_error<CopyResult, posix_error_t>(saved);
  }
  return make_result<CopyResult, posix_error_t>(
      CopyResult{CopyStrategy::Copy, static_cast<size_t>(size)});
}

}  // namespace wcl
