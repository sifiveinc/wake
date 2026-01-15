/*
 * Copyright 2024 SiFive, Inc.
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

#include <cstring>
#include <vector>

#include "wcl/filepath.h"
#include "wcl/unique_fd.h"

// Linux-specific includes for reflink and efficient copy
#ifdef __linux__
#include <sys/ioctl.h>
#include <sys/sendfile.h>
#ifdef FICLONE
#define HAS_FICLONE 1
#endif
#if __GLIBC__ >= 2 && __GLIBC_MINOR__ >= 27
#include <linux/fs.h>
#define HAS_COPY_FILE_RANGE 1
#endif
#endif

namespace cas {

// ============================================================================
// Reflink implementation
// ============================================================================

wcl::result<bool, wcl::posix_error_t> try_reflink(
    const std::string& src,
    const std::string& dst,
    mode_t mode) {
#ifdef HAS_FICLONE
  auto src_fd = wcl::unique_fd::open(src.c_str(), O_RDONLY);
  if (!src_fd) {
    return wcl::make_errno<bool>();
  }

  auto dst_fd = wcl::unique_fd::open(dst.c_str(), O_WRONLY | O_CREAT | O_EXCL, mode);
  if (!dst_fd) {
    return wcl::make_errno<bool>();
  }

  if (ioctl(dst_fd->get(), FICLONE, src_fd->get()) < 0) {
    // Clean up the created file on failure
    unlink(dst.c_str());
    return wcl::make_errno<bool>();
  }

  return wcl::make_result<bool, wcl::posix_error_t>(true);
#else
  (void)src;
  (void)dst;
  (void)mode;
  errno = EOPNOTSUPP;
  return wcl::make_errno<bool>();
#endif
}

// ============================================================================
// Hardlink implementation
// ============================================================================

wcl::result<bool, wcl::posix_error_t> try_hardlink(
    const std::string& src,
    const std::string& dst) {
  if (link(src.c_str(), dst.c_str()) < 0) {
    return wcl::make_errno<bool>();
  }
  return wcl::make_result<bool, wcl::posix_error_t>(true);
}

// ============================================================================
// Full copy implementation
// ============================================================================

wcl::result<size_t, wcl::posix_error_t> copy_file_full(
    const std::string& src,
    const std::string& dst,
    mode_t mode) {
  auto src_fd = wcl::unique_fd::open(src.c_str(), O_RDONLY);
  if (!src_fd) {
    return wcl::make_errno<size_t>();
  }

  auto dst_fd = wcl::unique_fd::open(dst.c_str(), O_WRONLY | O_CREAT | O_EXCL, mode);
  if (!dst_fd) {
    return wcl::make_errno<size_t>();
  }

  struct stat st;
  if (fstat(src_fd->get(), &st) < 0) {
    unlink(dst.c_str());
    return wcl::make_errno<size_t>();
  }

  size_t total_copied = 0;
  size_t remaining = st.st_size;

#ifdef HAS_COPY_FILE_RANGE
  // Use copy_file_range for efficient in-kernel copy
  while (remaining > 0) {
    ssize_t copied = copy_file_range(src_fd->get(), nullptr, dst_fd->get(), nullptr, remaining, 0);
    if (copied < 0) {
      if (errno == EXDEV || errno == EINVAL || errno == EOPNOTSUPP) {
        // Fall back to sendfile or read/write
        break;
      }
      unlink(dst.c_str());
      return wcl::make_errno<size_t>();
    }
    total_copied += copied;
    remaining -= copied;
  }
  if (remaining == 0) {
    return wcl::make_result<size_t, wcl::posix_error_t>(total_copied);
  }
  // Reset for fallback
  lseek(src_fd->get(), total_copied, SEEK_SET);
  lseek(dst_fd->get(), total_copied, SEEK_SET);
#endif

  // Fallback: use sendfile or read/write
#ifdef __linux__
  off_t offset = total_copied;
  while (remaining > 0) {
    ssize_t copied = sendfile(dst_fd->get(), src_fd->get(), &offset, remaining);
    if (copied < 0) {
      unlink(dst.c_str());
      return wcl::make_errno<size_t>();
    }
    total_copied += copied;
    remaining -= copied;
  }
#else
  // Generic fallback: read/write loop
  static thread_local char buffer[65536];
  while (remaining > 0) {
    ssize_t to_read = remaining > sizeof(buffer) ? sizeof(buffer) : remaining;
    ssize_t bytes_read = read(src_fd->get(), buffer, to_read);
    if (bytes_read < 0) {
      unlink(dst.c_str());
      return wcl::make_errno<size_t>();
    }
    if (bytes_read == 0) break;  // EOF

    ssize_t bytes_written = 0;
    while (bytes_written < bytes_read) {
      ssize_t written = write(dst_fd->get(), buffer + bytes_written, bytes_read - bytes_written);
      if (written < 0) {
        unlink(dst.c_str());
        return wcl::make_errno<size_t>();
      }
      bytes_written += written;
    }
    total_copied += bytes_read;
    remaining -= bytes_read;
  }
#endif

  return wcl::make_result<size_t, wcl::posix_error_t>(total_copied);
}

// ============================================================================
// Combined copy with fallback
// ============================================================================

wcl::result<CopyResult, wcl::posix_error_t> copy_file(
    const std::string& src,
    const std::string& dst,
    mode_t mode,
    bool /* allow_hardlink - unused, hardlinks disabled for materialization */) {

  // Try reflink first
  auto reflink_result = try_reflink(src, dst, mode);
  if (reflink_result) {
    return wcl::make_result<CopyResult, wcl::posix_error_t>(
        CopyResult{CopyStrategy::Reflink, 0});
  }

  // Only fall back if reflink is not supported
  int reflink_err = reflink_result.error();
  if (reflink_err != EOPNOTSUPP && reflink_err != EINVAL && reflink_err != EXDEV) {
    return wcl::make_error<CopyResult, wcl::posix_error_t>(reflink_err);
  }

  // Hardlinks are intentionally not used for materialization because:
  // 1. They share the same inode, so mode changes affect both CAS blob and destination
  // 2. The mode of the CAS blob may differ from the desired destination mode
  //    (e.g., chmod is called after the file is stored in CAS)

  // Fall back to full copy
  auto copy_result = copy_file_full(src, dst, mode);
  if (!copy_result) {
    return wcl::make_error<CopyResult, wcl::posix_error_t>(copy_result.error());
  }

  return wcl::make_result<CopyResult, wcl::posix_error_t>(
      CopyResult{CopyStrategy::Copy, *copy_result});
}

// ============================================================================
// Directory operations
// ============================================================================

wcl::result<bool, wcl::posix_error_t> mkdir_parents(const std::string& path) {
  std::string current;
  if (!path.empty() && path[0] == '/') {
    current = "/";
  }

  for (const auto& component : wcl::make_filepath_range_ref(path)) {
    if (component.empty()) continue;
    if (!current.empty() && current.back() != '/') {
      current += '/';
    }
    current += component;

    if (mkdir(current.c_str(), 0755) < 0 && errno != EEXIST) {
      return wcl::make_errno<bool>();
    }
  }

  return wcl::make_result<bool, wcl::posix_error_t>(true);
}

bool path_exists(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0;
}

bool is_directory(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISDIR(st.st_mode);
}

bool is_regular_file(const std::string& path) {
  struct stat st;
  return stat(path.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

bool is_symlink(const std::string& path) {
  struct stat st;
  return lstat(path.c_str(), &st) == 0 && S_ISLNK(st.st_mode);
}

wcl::result<std::string, wcl::posix_error_t> read_symlink(const std::string& path) {
  char buf[PATH_MAX];
  ssize_t len = readlink(path.c_str(), buf, sizeof(buf) - 1);
  if (len < 0) {
    return wcl::make_errno<std::string>();
  }
  buf[len] = '\0';
  return wcl::make_result<std::string, wcl::posix_error_t>(std::string(buf));
}

wcl::result<bool, wcl::posix_error_t> create_symlink(
    const std::string& target,
    const std::string& link_path) {
  if (symlink(target.c_str(), link_path.c_str()) < 0) {
    return wcl::make_errno<bool>();
  }
  return wcl::make_result<bool, wcl::posix_error_t>(true);
}

wcl::result<mode_t, wcl::posix_error_t> get_file_mode(const std::string& path) {
  struct stat st;
  if (stat(path.c_str(), &st) < 0) {
    return wcl::make_errno<mode_t>();
  }
  return wcl::make_result<mode_t, wcl::posix_error_t>(st.st_mode);
}

// ============================================================================
// Filesystem capability detection
// ============================================================================

bool same_filesystem(const std::string& path1, const std::string& path2) {
  struct stat st1, st2;
  if (stat(path1.c_str(), &st1) < 0 || stat(path2.c_str(), &st2) < 0) {
    return false;
  }
  return st1.st_dev == st2.st_dev;
}

bool supports_reflink(const std::string& path) {
#ifdef HAS_FICLONE
  // We can't easily detect reflink support without trying it
  // This is a heuristic based on common COW filesystems
  struct stat st;
  if (stat(path.c_str(), &st) < 0) {
    return false;
  }
  // On Linux, we could check /proc/mounts for btrfs, xfs, etc.
  // For now, just return true and let try_reflink fail gracefully
  return true;
#else
  (void)path;
  return false;
#endif
}

}  // namespace cas

