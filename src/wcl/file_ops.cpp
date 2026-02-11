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

namespace fs = std::filesystem;

namespace wcl {

result<bool, posix_error_t> try_reflink(const std::string& src, const std::string& dst,
                                        mode_t mode) {
#ifdef HAS_FICLONE
  auto src_fd = unique_fd::open(src.c_str(), O_RDONLY);
  if (!src_fd) {
    return make_errno<bool>();
  }

  auto dst_fd = unique_fd::open(dst.c_str(), O_WRONLY | O_CREAT | O_EXCL, mode);
  if (!dst_fd) {
    return make_errno<bool>();
  }

  if (ioctl(dst_fd->get(), FICLONE, src_fd->get()) < 0) {
    // Clean up the created file on failure
    unlink(dst.c_str());
    return make_errno<bool>();
  }

  return make_result<bool, posix_error_t>(true);
#else
  (void)src;
  (void)dst;
  (void)mode;
  errno = EOPNOTSUPP;
  return make_errno<bool>();
#endif
}

result<CopyResult, posix_error_t> reflink_or_copy_file(const std::string& src,
                                                       const std::string& dst, mode_t mode) {
  // Try reflink first
  auto reflink_result = try_reflink(src, dst, mode);
  if (reflink_result) {
    return make_result<CopyResult, posix_error_t>(CopyResult{CopyStrategy::Reflink, 0});
  }

  // Only fall back if reflink is not supported
  int reflink_err = reflink_result.error();
  if (reflink_err != EOPNOTSUPP && reflink_err != EINVAL && reflink_err != EXDEV) {
    return make_error<CopyResult, posix_error_t>(reflink_err);
  }

  // Fall back to std::filesystem::copy_file
  std::error_code ec;
  fs::copy_file(src, dst, ec);
  if (ec) {
    return make_error<CopyResult, posix_error_t>(ec.value());
  }

  // Set the file permissions (copy_file preserves source permissions, but we want explicit mode)
  fs::permissions(dst, static_cast<fs::perms>(mode), ec);
  if (ec) {
    fs::remove(dst);
    return make_error<CopyResult, posix_error_t>(ec.value());
  }

  // Get file size for the result
  auto file_size = fs::file_size(dst, ec);
  size_t bytes_copied = ec ? 0 : static_cast<size_t>(file_size);

  return make_result<CopyResult, posix_error_t>(CopyResult{CopyStrategy::Copy, bytes_copied});
}

}  // namespace wcl
