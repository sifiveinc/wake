/*
 * Copyright 2026 SiFive, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#pragma once

#include <sys/types.h>

#include <string>

#include "file_ops.h"

namespace wcl {

struct DirectoryResult {
  int fd;
  bool created;
};

// Place an already-open regular source as a temporary sibling before atomically
// replacing it in an already-open destination parent directory.
result<CopyResult, posix_error_t> materialize_regular_file_at(int src_fd, int destination_parent_fd,
                                                              const std::string& destination_name,
                                                              mode_t mode, time_t mtime_sec,
                                                              long mtime_nsec,
                                                              bool attempt_reflink = true);

// Path wrapper for trusted callers whose parent directories already exist.
result<CopyResult, posix_error_t> materialize_regular_file(const std::string& src,
                                                           const std::string& destination,
                                                           mode_t mode, time_t mtime_sec,
                                                           long mtime_nsec,
                                                           bool attempt_reflink = true);

result<bool, posix_error_t> materialize_symlink_at(int destination_parent_fd,
                                                   const std::string& destination_name,
                                                   const std::string& target, time_t mtime_sec,
                                                   long mtime_nsec);
result<bool, posix_error_t> materialize_symlink(const std::string& destination,
                                                const std::string& target, time_t mtime_sec,
                                                long mtime_nsec);

// Ensure `destination_name` is a real directory beneath `destination_parent_fd`.
// Reuse an existing directory; replace any non-directory entry without following
// symlinks; never remove an existing directory or its contents. The returned
// directory FD is owned by the caller.
result<DirectoryResult, posix_error_t> ensure_directory_at(int destination_parent_fd,
                                                           const std::string& destination_name,
                                                           mode_t initial_mode);

// Apply final directory metadata through an already-open directory descriptor.
result<bool, posix_error_t> apply_directory_metadata(int directory_fd, mode_t mode,
                                                     time_t mtime_sec, long mtime_nsec);

// Path-oriented wrapper for trusted callers whose parent directories already exist.
result<bool, posix_error_t> materialize_directory(const std::string& destination, mode_t mode,
                                                  time_t mtime_sec, long mtime_nsec);

}  // namespace wcl
