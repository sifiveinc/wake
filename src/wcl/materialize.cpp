/*
 * Copyright 2026 SiFive, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include "materialize.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>

#include "compat/nofollow.h"

namespace fs = std::filesystem;

namespace wcl {
namespace {

std::atomic<uint64_t> materialize_counter{0};

std::string temporary_name() {
  return ".wake-materialize-" + std::to_string(getpid()) + "." +
         std::to_string(materialize_counter.fetch_add(1));
}

result<bool, posix_error_t> replace_at(int parentfd, const std::string& temporary,
                                       const std::string& destination) {
  if (renameat(parentfd, temporary.c_str(), parentfd, destination.c_str()) == 0)
    return make_result<bool, posix_error_t>(true);
  return make_errno<bool>();
}

bool set_file_mtime(int fd, time_t seconds, long nanoseconds) {
  // (0, 0) means retain the mtime assigned by reflink or copy rather than
  // forcing the file's timestamp to the Unix epoch.
  if (seconds == 0 && nanoseconds == 0) return true;
  struct timespec times[2] = {{0, UTIME_OMIT}, {seconds, nanoseconds}};
  return futimens(fd, times) == 0;
}

// Opens the parent directory of destination and returns its file descriptor.
// Writes only destination's final path component to name; it does not open the
// file identified by name within destination.
result<int, posix_error_t> open_destination_parent(const std::string& destination,
                                                   std::string* name) {
  fs::path path(destination);
  *name = path.filename().string();
  fs::path parent = path.parent_path();
  int fd = open(parent.empty() ? "." : parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0) return make_errno<int>();
  return make_result<int, posix_error_t>(fd);
}

}  // namespace

result<CopyResult, posix_error_t> materialize_regular_file_at(int src_fd, int destination_parent_fd,
                                                              const std::string& destination_name,
                                                              mode_t mode, time_t mtime_sec,
                                                              long mtime_nsec,
                                                              bool attempt_reflink) {
  // Copy to a unique sibling first so rename publishes either the complete file
  // or the previous destination, never a partially materialized file.
  const std::string temporary = temporary_name();
  int destination = openat(destination_parent_fd, temporary.c_str(),
                           O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode & 07777);
  if (destination < 0) return make_errno<CopyResult>();
  auto copy = reflink_or_copy_fd(src_fd, destination, attempt_reflink);
  int failure = copy ? 0 : copy.error();
  if (failure == 0 && fchmod(destination, mode & 07777) != 0) failure = errno;
  // Apply mtime before replacement so observers never see a new file with stale metadata.
  if (failure == 0 && !set_file_mtime(destination, mtime_sec, mtime_nsec)) failure = errno;
  if (close(destination) != 0 && failure == 0) failure = errno;
  if (failure != 0) {
    unlinkat(destination_parent_fd, temporary.c_str(), 0);
    return make_error<CopyResult, posix_error_t>(failure);
  }
  auto placed = replace_at(destination_parent_fd, temporary, destination_name);
  if (!placed) {
    unlinkat(destination_parent_fd, temporary.c_str(), 0);
    return make_error<CopyResult, posix_error_t>(placed.error());
  }
  return copy;
}

result<CopyResult, posix_error_t> materialize_regular_file(const std::string& src,
                                                           const std::string& destination,
                                                           mode_t mode, time_t mtime_sec,
                                                           long mtime_nsec, bool attempt_reflink) {
  int source = open(src.c_str(), O_RDONLY | O_CLOEXEC);
  if (source < 0) return make_errno<CopyResult>();
  std::string name;
  auto parent = open_destination_parent(destination, &name);
  if (!parent) {
    const int saved = parent.error();
    close(source);
    return make_error<CopyResult, posix_error_t>(saved);
  }
  auto result = materialize_regular_file_at(source, *parent, name, mode, mtime_sec, mtime_nsec,
                                            attempt_reflink);
  close(*parent);
  close(source);
  return result;
}

result<bool, posix_error_t> materialize_symlink_at(int destination_parent_fd,
                                                   const std::string& destination_name,
                                                   const std::string& target, time_t mtime_sec,
                                                   long mtime_nsec) {
  const std::string temporary = temporary_name();
  if (symlinkat(target.c_str(), destination_parent_fd, temporary.c_str()) != 0)
    return make_errno<bool>();
  // (0, 0) means retain the mtime assigned when the symlink was created rather than
  // forcing the symlink's timestamp to the Unix epoch.
  if (mtime_sec != 0 || mtime_nsec != 0) {
    struct timespec times[2] = {{0, UTIME_OMIT}, {mtime_sec, mtime_nsec}};
    // Symlink timestamps are not supported consistently across filesystems.
    (void)utimensat(destination_parent_fd, temporary.c_str(), times, AT_SYMLINK_NOFOLLOW);
  }
  auto placed = replace_at(destination_parent_fd, temporary, destination_name);
  if (!placed) {
    unlinkat(destination_parent_fd, temporary.c_str(), 0);
    return placed;
  }
  return make_result<bool, posix_error_t>(true);
}

result<bool, posix_error_t> materialize_symlink(const std::string& destination,
                                                const std::string& target, time_t mtime_sec,
                                                long mtime_nsec) {
  std::string name;
  auto parent = open_destination_parent(destination, &name);
  if (!parent) return make_error<bool, posix_error_t>(parent.error());
  auto result = materialize_symlink_at(*parent, name, target, mtime_sec, mtime_nsec);
  close(*parent);
  return result;
}

result<DirectoryResult, posix_error_t> ensure_directory_at(int destination_parent_fd,
                                                           const std::string& destination_name,
                                                           mode_t initial_mode) {
  bool created = false;
  if (mkdirat(destination_parent_fd, destination_name.c_str(), initial_mode & 07777) == 0) {
    created = true;
  } else if (errno == EEXIST) {
    struct stat status;
    if (fstatat(destination_parent_fd, destination_name.c_str(), &status, AT_SYMLINK_NOFOLLOW) != 0)
      return make_errno<DirectoryResult>();
    if (!S_ISDIR(status.st_mode)) {
      if (unlinkat(destination_parent_fd, destination_name.c_str(), 0) != 0 ||
          mkdirat(destination_parent_fd, destination_name.c_str(), initial_mode & 07777) != 0)
        return make_errno<DirectoryResult>();
      created = true;
    }
  } else {
    return make_errno<DirectoryResult>();
  }
  int fd = openat(destination_parent_fd, destination_name.c_str(),
                  O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return make_errno<DirectoryResult>();
  return make_result<DirectoryResult, posix_error_t>(DirectoryResult{fd, created});
}

result<bool, posix_error_t> apply_directory_metadata(int directory_fd, mode_t mode,
                                                     time_t mtime_sec, long mtime_nsec) {
  if (fchmod(directory_fd, mode & 07777) != 0) return make_errno<bool>();
  // (0, 0) means retain the directory's existing mtime rather than forcing it
  // to the Unix epoch.
  if (mtime_sec == 0 && mtime_nsec == 0) return make_result<bool, posix_error_t>(true);
  struct timespec times[2] = {{0, UTIME_OMIT}, {mtime_sec, mtime_nsec}};
  if (futimens(directory_fd, times) != 0) return make_errno<bool>();
  return make_result<bool, posix_error_t>(true);
}

result<bool, posix_error_t> materialize_directory(const std::string& destination, mode_t mode,
                                                  time_t mtime_sec, long mtime_nsec) {
  std::string name;
  auto parent = open_destination_parent(destination, &name);
  if (!parent) return make_error<bool, posix_error_t>(parent.error());
  auto directory = ensure_directory_at(*parent, name, mode);
  close(*parent);
  if (!directory) return make_error<bool, posix_error_t>(directory.error());
  auto result = apply_directory_metadata(directory->fd, mode, mtime_sec, mtime_nsec);
  close(directory->fd);
  return result;
}

}  // namespace wcl
