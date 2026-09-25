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

#include "materialize_staging.h"

#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <climits>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <set>
#include <sstream>

#include "compat/nofollow.h"
#include "json/json5.h"
#include "wcl/file_ops.h"
#include "wcl/materialize.h"

namespace fs = std::filesystem;

namespace wakefs {

namespace fs = std::filesystem;

bool write_staging_manifest_atomic(const std::string& path, const StagingManifest& manifest,
                                   std::string* error);

namespace {

std::atomic<uint64_t> manifest_counter{0};

bool fail(std::string* error, const std::string& message) {
  if (error) *error = message;
  return false;
}

std::string errno_message(const std::string& action) { return action + ": " + strerror(errno); }

bool is_safe_relative_path(const std::string& path) {
  if (path.empty() || path.find('\0') != std::string::npos) return false;

  const fs::path value(path);
  if (!value.is_relative() || path.back() == '/' || value != value.lexically_normal()) return false;

  for (const fs::path& component : value) {
    if (component == fs::path(".") || component == fs::path("..")) return false;
  }
  return true;
}

bool required_string(const JAST& object, const char* key, std::string* value, std::string* error) {
  auto child = object.get_opt(key);
  if (!child || (*child)->kind != JSON_STR || (*child)->value.empty()) {
    return fail(error, std::string("manifest field '") + key + "' must be a nonempty string");
  }
  *value = (*child)->value;
  return true;
}

bool required_integer(const JAST& object, const char* key, int64_t* value, std::string* error) {
  auto child = object.get_opt(key);
  auto parsed = child ? (*child)->expect_integer() : std::nullopt;
  if (!parsed) {
    return fail(error, std::string("manifest field '") + key + "' must be an integer");
  }
  *value = *parsed;
  return true;
}

bool required_boolean(const JAST& object, const char* key, bool* value, std::string* error) {
  auto child = object.get_opt(key);
  if (!child) return fail(error, std::string("manifest field '") + key + "' must be a boolean");
  auto parsed = (*child)->expect_boolean();
  if (!parsed) return fail(error, std::string("manifest field '") + key + "' must be a boolean");
  *value = *parsed;
  return true;
}

// Validate the serialized manifest contract before trusting any paths or metadata.
bool validate_metadata(const StagingManifest& manifest, std::string* error) {
  if (manifest.job_key.empty() || manifest.job_key.find('/') != std::string::npos ||
      manifest.job_key.find('\0') != std::string::npos)
    return fail(error, "job_key must be a nonempty path component");
  if (manifest.wake_run_id.has_value() != manifest.wake_job_id.has_value())
    return fail(error, "wake_run_id and wake_job_id must be provided together");
  std::set<std::string> destinations;
  for (const StagingEntry& entry : manifest.entries) {
    if (!is_safe_relative_path(entry.destination))
      return fail(error, "entry destination must be a normalized relative path");
    if (!destinations.insert(entry.destination).second)
      return fail(error, "manifest contains duplicate destination: " + entry.destination);
    if (entry.mtime_nsec < 0 || entry.mtime_nsec >= 1000000000L)
      return fail(error, "entry mtime_nsec is outside its valid range");
    if ((entry.type == StagingEntryType::File && !is_safe_relative_path(entry.staging_path)) ||
        (entry.type != StagingEntryType::File && !entry.staging_path.empty()))
      return fail(error, "entry staging_path is invalid");
    if ((entry.type == StagingEntryType::Symlink && entry.target.find('\0') != std::string::npos) ||
        (entry.type != StagingEntryType::Symlink && !entry.target.empty()))
      return fail(error, "entry target is invalid");
    if ((entry.type == StagingEntryType::File || entry.type == StagingEntryType::Directory) &&
        (entry.mode & ~07777) != 0)
      return fail(error, "entry mode has unsupported bits");
  }
  return true;
}

bool canonical_existing_directory(const std::string& path, std::string* canonical,
                                  std::string* error) {
  char resolved[PATH_MAX];
  if (!realpath(path.c_str(), resolved))
    return fail(error, errno_message("failed to canonicalize " + path));
  struct stat st;
  if (stat(resolved, &st) != 0 || !S_ISDIR(st.st_mode))
    return fail(error, "path is not a directory: " + path);
  *canonical = resolved;
  return true;
}

bool open_directory_at(int parent, const std::string& name, wcl::unique_fd* fd,
                       std::string* error) {
  int opened = openat(parent, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (opened < 0) return fail(error, errno_message("failed to open directory " + name));
  *fd = wcl::unique_fd(opened);
  return true;
}

// Walk a validated relative path below rootfd without following directory symlinks.
// For example, relative "a/b" returns a descriptor for rootfd/a/b.
bool open_relative_directory(int rootfd, const std::string& relative, bool create,
                             wcl::unique_fd* fd, std::string* error) {
  // Work on a duplicate so walking the path never closes the caller's root FD.
  wcl::unique_fd current(dup(rootfd));
  if (!current.valid()) return fail(error, errno_message("failed to duplicate root directory"));

  // Walk from the root one validated path component at a time.
  size_t begin = 0;
  while (begin < relative.size()) {
    size_t end = relative.find('/', begin);
    if (end == std::string::npos) end = relative.size();
    const std::string directory_name = relative.substr(begin, end - begin);

    // Create missing parent directories when requested.
    if (create && mkdirat(current.get(), directory_name.c_str(), 0755) != 0 && errno != EEXIST) {
      const int saved = errno;
      errno = saved;
      return fail(error, errno_message("failed to create directory " + directory_name));
    }

    // Open without following symlinks, keeping traversal inside the directory tree.
    wcl::unique_fd next;
    if (!open_directory_at(current.get(), directory_name, &next, error)) return false;

    // The new descriptor becomes the base for the next component.
    current = std::move(next);
    begin = end + 1;
  }

  // Return ownership of the final directory descriptor to the caller.
  *fd = std::move(current);
  return true;
}

// Splits a validated path into its parent directory and file or directory name.
void split_parent(const std::string& path, std::string* parent, std::string* leaf) {
  const fs::path filesystem_path(path);
  *parent = filesystem_path.parent_path().string();
  *leaf = filesystem_path.filename().string();
}

// Safely open a regular staging source and atomically place it below target_rootfd.
bool materialize_file(int source_rootfd, int target_rootfd, const StagingEntry& entry,
                      std::string* error) {
  std::string source_parent_path, source_leaf, target_parent_path, target_leaf;
  split_parent(entry.staging_path, &source_parent_path, &source_leaf);
  split_parent(entry.destination, &target_parent_path, &target_leaf);
  wcl::unique_fd source_parent;
  if (!open_relative_directory(source_rootfd, source_parent_path, false, &source_parent, error))
    return false;

  wcl::unique_fd target_parent;
  if (!open_relative_directory(target_rootfd, target_parent_path, true, &target_parent, error))
    return false;

  int source_fd =
      openat(source_parent.get(), source_leaf.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
  if (source_fd < 0)
    return fail(error, errno_message("failed to open staging source " + entry.staging_path));
  wcl::unique_fd source(source_fd);

  struct stat source_stat;
  if (fstat(source.get(), &source_stat) != 0 || !S_ISREG(source_stat.st_mode))
    return fail(error, "staging source is not a regular file: " + entry.staging_path);

  auto copy =
      wcl::materialize_regular_file_at(source.get(), target_parent.get(), target_leaf, entry.mode,
                                       static_cast<time_t>(entry.mtime_sec), entry.mtime_nsec);
  if (!copy) {
    errno = copy.error();
    return fail(error, errno_message("failed to copy staging source " + entry.staging_path));
  }
  return true;
}

// After an interrupted placement, a missing source is safe to accept only when
// its exact destination is already a regular file below non-symlink parents.
bool regular_destination_exists(int target_rootfd, const StagingEntry& entry, std::string* error) {
  std::string parent_path, leaf;
  split_parent(entry.destination, &parent_path, &leaf);
  wcl::unique_fd parent;
  if (!open_relative_directory(target_rootfd, parent_path, false, &parent, error)) return false;
  struct stat destination_stat;
  const bool present =
      fstatat(parent.get(), leaf.c_str(), &destination_stat, AT_SYMLINK_NOFOLLOW) == 0;
  if (!present) {
    const int saved = errno;
    if (saved == ENOENT) return false;
    errno = saved;
    return fail(error,
                errno_message("failed to inspect materialized destination " + entry.destination));
  }
  return S_ISREG(destination_stat.st_mode);
}

// Atomically replace a target-root-relative leaf with the manifest's symlink target.
bool materialize_symlink(int target_rootfd, const StagingEntry& entry, std::string* error) {
  std::string parent_path, leaf;
  split_parent(entry.destination, &parent_path, &leaf);
  wcl::unique_fd parent;
  if (!open_relative_directory(target_rootfd, parent_path, true, &parent, error)) return false;
  auto result = wcl::materialize_symlink_at(parent.get(), leaf, entry.target,
                                            static_cast<time_t>(entry.mtime_sec), entry.mtime_nsec);
  if (!result) {
    errno = result.error();
    return fail(error, errno_message("failed to create symlink " + entry.destination));
  }
  return true;
}

// Ensure a target-root-relative directory exists, optionally applying final metadata.
bool materialize_directory(int target_rootfd, const StagingEntry& entry, bool apply_metadata,
                           std::string* error) {
  std::string parent_path, leaf;
  split_parent(entry.destination, &parent_path, &leaf);
  wcl::unique_fd parent;
  if (!open_relative_directory(target_rootfd, parent_path, true, &parent, error)) return false;
  auto directory = wcl::ensure_directory_at(parent.get(), leaf, 0700);
  if (!directory) {
    errno = directory.error();
    return fail(error, errno_message("failed to create directory " + entry.destination));
  }
  if (apply_metadata) {
    auto metadata = wcl::apply_directory_metadata(
        directory->fd.get(), entry.mode, static_cast<time_t>(entry.mtime_sec), entry.mtime_nsec);
    if (!metadata) {
      const int saved = metadata.error();
      errno = saved;
      return fail(error, errno_message("failed to apply directory metadata " + entry.destination));
    }
  }
  return true;
}

// Consume one exact named regular source without following a final symlink.
// The manifest-level completion checkpoint makes ENOENT safe on retry.
bool consume_regular_source(int source_rootfd, const StagingEntry& entry, std::string* error) {
  std::string parent_path, leaf;
  split_parent(entry.staging_path, &parent_path, &leaf);
  wcl::unique_fd parent;
  if (!open_relative_directory(source_rootfd, parent_path, false, &parent, error)) {
    if (errno == ENOENT) return true;
    return false;
  }
  struct stat source_stat;
  const bool valid = fstatat(parent.get(), leaf.c_str(), &source_stat, AT_SYMLINK_NOFOLLOW) == 0;
  if (!valid && errno == ENOENT) {
    return true;
  }
  if (!valid) {
    const int saved = errno;
    errno = saved;
    return fail(error, errno_message("failed to inspect staging source " + entry.staging_path));
  }
  if (!S_ISREG(source_stat.st_mode)) {
    return fail(error, "staging source is not a regular file: " + entry.staging_path);
  }
  if (unlinkat(parent.get(), leaf.c_str(), 0) != 0 && errno != ENOENT) {
    const int saved = errno;
    errno = saved;
    return fail(error, errno_message("failed to consume staging source " + entry.staging_path));
  }
  return true;
}

// Orders paths by length, then lexicographically. This puts parent paths before
// their children; unrelated paths have no meaningful traversal order.
bool parent_path_before(const StagingEntry& left, const StagingEntry& right) {
  return left.destination.size() < right.destination.size() ||
         (left.destination.size() == right.destination.size() &&
          left.destination < right.destination);
}

bool entry_is_directory(const StagingEntry& entry) {
  return entry.type == StagingEntryType::Directory;
}

// Resolve the fixed staging layout below the current workspace. Each component
// is opened without following symlinks before it is used as a descriptor root.
struct MaterializationRoots {
  wcl::unique_fd source;
  wcl::unique_fd workspace;
};

std::optional<MaterializationRoots> validate_roots(std::string* error) {
  char current[PATH_MAX];
  if (!getcwd(current, sizeof(current))) {
    fail(error, errno_message("failed to get current workspace"));
    return std::nullopt;
  }
  std::string workspace;
  if (!canonical_existing_directory(current, &workspace, error)) return std::nullopt;

  int workspace_fd = open(workspace.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (workspace_fd < 0) {
    fail(error, errno_message("failed to open current workspace"));
    return std::nullopt;
  }
  wcl::unique_fd workspace_root(workspace_fd);
  wcl::unique_fd build;
  if (!open_directory_at(workspace_root.get(), ".build", &build, error)) return std::nullopt;
  wcl::unique_fd cas;
  if (!open_directory_at(build.get(), "cas", &cas, error)) return std::nullopt;
  wcl::unique_fd staging;
  if (!open_directory_at(cas.get(), "staging", &staging, error)) return std::nullopt;
  return MaterializationRoots{std::move(staging), std::move(workspace_root)};
}

StagingEntrySummary* find_summary(StagingMaterializationSummary* summary,
                                  const std::string& destination) {
  for (StagingEntrySummary& entry : summary->entries) {
    if (entry.destination == destination) return &entry;
  }
  return nullptr;
}

}  // namespace

// Parse a versioned manifest and reject schema or lexical-safety violations.
bool parse_staging_manifest(const std::string& text, StagingManifest* manifest,
                            std::string* error) {
  std::stringstream parse_errors;
  JAST root;
  if (!JAST::parse(text, parse_errors, root) || root.kind != JSON_OBJECT)
    return fail(error, "invalid staging manifest: " + parse_errors.str());
  int64_t version;
  if (!required_integer(root, "version", &version, error)) return false;
  if (version != 1) return fail(error, "unsupported staging manifest version");
  StagingManifest parsed;
  if (!required_string(root, "job_key", &parsed.job_key, error) ||
      !required_integer(root, "created_at_ns", &parsed.created_at_ns, error) ||
      !required_boolean(root, "materialization_complete", &parsed.materialization_complete, error))
    return false;
  auto daemon_pid = root.get_opt("daemon_pid");
  if (daemon_pid) {
    auto value = (*daemon_pid)->expect_integer();
    if (!value) return fail(error, "manifest field 'daemon_pid' must be an integer");
    parsed.daemon_pid = *value;
  }
  auto wake_run_id = root.get_opt("wake_run_id");
  auto wake_job_id = root.get_opt("wake_job_id");
  if (wake_run_id.has_value() != wake_job_id.has_value())
    return fail(error, "wake_run_id and wake_job_id must be provided together");
  if (wake_run_id) {
    auto run_id = (*wake_run_id)->expect_integer();
    auto job_id = (*wake_job_id)->expect_integer();
    if (!run_id || !job_id) return fail(error, "wake_run_id and wake_job_id must be integers");
    parsed.wake_run_id = *run_id;
    parsed.wake_job_id = *job_id;
  }
  auto entries = root.get_opt("entries");
  if (!entries || (*entries)->kind != JSON_ARRAY)
    return fail(error, "manifest field 'entries' must be an array");
  for (const JChild& child : (*entries)->children) {
    const JAST& json = child.second;
    if (json.kind != JSON_OBJECT) return fail(error, "manifest entry must be an object");
    StagingEntry entry;
    std::string type;
    if (!required_string(json, "destination", &entry.destination, error) ||
        !required_string(json, "type", &type, error) ||
        !required_integer(json, "mtime_sec", &entry.mtime_sec, error))
      return false;
    int64_t nsec;
    if (!required_integer(json, "mtime_nsec", &nsec, error) || nsec < 0 || nsec >= 1000000000L)
      return fail(error, "entry mtime_nsec is outside its valid range");
    entry.mtime_nsec = static_cast<long>(nsec);
    if (type == "file") {
      entry.type = StagingEntryType::File;
      int64_t mode;
      if (!required_string(json, "staging_path", &entry.staging_path, error) ||
          !required_integer(json, "mode", &mode, error) || mode < 0 || mode > 07777)
        return fail(error, "file entry has invalid staging_path or mode");
      entry.mode = static_cast<mode_t>(mode);
    } else if (type == "symlink") {
      entry.type = StagingEntryType::Symlink;
      if (!required_string(json, "target", &entry.target, error)) return false;
    } else if (type == "directory") {
      entry.type = StagingEntryType::Directory;
      int64_t mode;
      if (!required_integer(json, "mode", &mode, error) || mode < 0 || mode > 07777)
        return fail(error, "directory entry has invalid mode");
      entry.mode = static_cast<mode_t>(mode);
    } else {
      return fail(error, "entry has unknown type");
    }
    parsed.entries.emplace_back(std::move(entry));
  }
  if (!validate_metadata(parsed, error)) return false;
  *manifest = std::move(parsed);
  return true;
}

bool read_staging_manifest(const std::string& path, StagingManifest* manifest, std::string* error) {
  std::ifstream stream(path, std::ios::binary);
  if (!stream) return fail(error, "open manifest " + path + ": " + strerror(errno));
  std::stringstream text;
  text << stream.rdbuf();
  if (!stream.good() && !stream.eof()) return fail(error, "read manifest " + path);
  return parse_staging_manifest(text.str(), manifest, error);
}

bool discover_completed_staging_manifests(const std::string& recovery_dir,
                                          std::vector<CompletedStagingManifest>* manifests,
                                          std::string* error) {
  if (!manifests) return fail(error, "manifest list is required");
  manifests->clear();
  std::error_code ec;
  if (!fs::exists(recovery_dir, ec)) {
    if (ec) return fail(error, "inspect recovery directory: " + ec.message());
    return true;
  }
  if (!fs::is_directory(recovery_dir, ec)) {
    if (ec) return fail(error, "inspect recovery directory: " + ec.message());
    return fail(error, "recovery path is not a directory: " + recovery_dir);
  }
  for (fs::directory_iterator it(recovery_dir, fs::directory_options::skip_permission_denied, ec),
       end;
       it != end; it.increment(ec)) {
    if (ec) return fail(error, "scan recovery directory: " + ec.message());
    std::error_code status_error;
    const fs::file_status status = it->symlink_status(status_error);
    if (status_error || !fs::is_regular_file(status)) continue;
    StagingManifest manifest;
    std::string manifest_error;
    if (!read_staging_manifest(it->path().string(), &manifest, &manifest_error))
      return fail(error,
                  "invalid recovery manifest " + it->path().string() + ": " + manifest_error);
    manifests->push_back({it->path().string(), std::move(manifest)});
  }
  std::sort(manifests->begin(), manifests->end(),
            [](const CompletedStagingManifest& left, const CompletedStagingManifest& right) {
              if (left.manifest.created_at_ns != right.manifest.created_at_ns)
                return left.manifest.created_at_ns < right.manifest.created_at_ns;
              return left.path < right.path;
            });
  return true;
}

// Write a complete temporary manifest, then rename it into place so an interrupted
// recovery retains a parseable record of either the old or new remaining work.
bool write_staging_manifest_atomic(const std::string& path, const StagingManifest& manifest,
                                   std::string* error) {
  if (!validate_metadata(manifest, error)) return false;
  JAST root(JSON_OBJECT);
  root.add("version", 1);
  root.add("job_key", manifest.job_key);
  root.add("daemon_pid", static_cast<long long>(manifest.daemon_pid));
  root.add("created_at_ns", static_cast<long long>(manifest.created_at_ns));
  root.add_bool("materialization_complete", manifest.materialization_complete);
  if (manifest.wake_run_id) {
    root.add("wake_run_id", static_cast<long long>(*manifest.wake_run_id));
    root.add("wake_job_id", static_cast<long long>(*manifest.wake_job_id));
  }
  JAST& entries = root.add("entries", JSON_ARRAY);
  for (const StagingEntry& entry : manifest.entries) {
    JAST& json = entries.add("", JSON_OBJECT);
    json.add("destination", entry.destination);
    if (entry.type == StagingEntryType::File) {
      json.add("type", "file");
      json.add("staging_path", entry.staging_path);
      json.add("mode", static_cast<long>(entry.mode & 07777));
    } else if (entry.type == StagingEntryType::Symlink) {
      json.add("type", "symlink");
      json.add("target", entry.target);
    } else {
      json.add("type", "directory");
      json.add("mode", static_cast<long>(entry.mode & 07777));
    }
    json.add("mtime_sec", static_cast<long long>(entry.mtime_sec));
    json.add("mtime_nsec", static_cast<long>(entry.mtime_nsec));
  }
  std::stringstream serialized;
  serialized << root << "\n";
  const size_t slash = path.rfind('/');
  const std::string parent = slash == std::string::npos ? "" : path.substr(0, slash + 1);
  const std::string filename = slash == std::string::npos ? path : path.substr(slash + 1);
  const std::string temporary = parent + "." + filename + ".tmp." + std::to_string(getpid()) + "." +
                                std::to_string(manifest_counter.fetch_add(1));
  int fd = open(temporary.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  if (fd < 0) return fail(error, errno_message("failed to create manifest temporary"));
  int saved = 0;
  const std::string data = serialized.str();
  if (!wcl::write_all(fd, data.data(), data.size())) saved = errno;
  if (saved == 0 && fchmod(fd, 0644) != 0) saved = errno;
  if (close(fd) != 0 && saved == 0) saved = errno;
  if (saved == 0 && rename(temporary.c_str(), path.c_str()) != 0) saved = errno;
  if (saved != 0) {
    unlink(temporary.c_str());
    errno = saved;
    return fail(error, errno_message("failed to publish manifest " + path));
  }
  return true;
}

// Recover one manifest into the current workspace and consume its staging sources.
bool materialize_completed_workspace(const std::string& manifest_path,
                                     StagingMaterializationSummary* summary, std::string* error) {
  StagingManifest manifest;
  if (!read_staging_manifest(manifest_path, &manifest, error)) return false;
  return materialize_completed_workspace(manifest_path, manifest, summary, error);
}

bool materialize_completed_workspace(const std::string& manifest_path,
                                     const StagingManifest& parsed_manifest,
                                     StagingMaterializationSummary* summary, std::string* error) {
  if (!summary) return fail(error, "materialization summary is required");
  *summary = {};
  StagingManifest manifest = parsed_manifest;
  for (const StagingEntry& entry : manifest.entries)
    summary->entries.push_back({entry.destination, false, false, ""});

  // Resolve manifest-relative paths from the current workspace.
  auto roots = validate_roots(error);
  if (!roots) return false;
  const int source_root = roots->source.get();
  const int workspace_root = roots->workspace.get();

  // Keep processing independent entries after a failure, while preserving the
  // first error that explains why each destination could not be recovered.
  auto record_failure = [&](const std::string& destination, const std::string& message) {
    StagingEntrySummary* entry = find_summary(summary, destination);
    if (entry && entry->error.empty()) entry->error = message;
    ++summary->failed;
  };
  if (!manifest.materialization_complete) {
    std::vector<StagingEntry> pending = manifest.entries;
    // Create parent directories before their children; final directory metadata
    // waits until every child has been placed.
    std::stable_sort(pending.begin(), pending.end(),
                     [](const StagingEntry& left, const StagingEntry& right) {
                       const bool left_directory = entry_is_directory(left);
                       const bool right_directory = entry_is_directory(right);
                       if (left_directory != right_directory) return left_directory;
                       return parent_path_before(left, right);
                     });
    for (const StagingEntry& entry : pending) {
      std::string placement_error;
      bool placed = false;
      if (entry.type == StagingEntryType::Directory) {
        placed = materialize_directory(workspace_root, entry, false, &placement_error);
      } else if (entry.type == StagingEntryType::Symlink) {
        placed = materialize_symlink(workspace_root, entry, &placement_error);
      } else {
        placed = materialize_file(source_root, workspace_root, entry, &placement_error);
        if (!placed && errno == ENOENT) {
          placement_error.clear();
          placed = regular_destination_exists(workspace_root, entry, &placement_error);
          if (!placed && placement_error.empty())
            placement_error = "staging source and materialized destination are both missing: " +
                              entry.staging_path;
        }
      }
      if (!placed) {
        record_failure(entry.destination, placement_error);
        continue;
      }
      StagingEntrySummary* result = find_summary(summary, entry.destination);
      if (result) result->materialized = true;
      ++summary->materialized;
    }
    // Do not apply restrictive final directory metadata when another entry
    // failed: the uncompleted manifest must remain retryable.
    if (summary->failed == 0) {
      std::stable_sort(pending.begin(), pending.end(),
                       [](const StagingEntry& left, const StagingEntry& right) {
                         return parent_path_before(right, left);
                       });
      for (const StagingEntry& entry : pending) {
        if (entry.type != StagingEntryType::Directory) continue;
        std::string metadata_error;
        if (!materialize_directory(workspace_root, entry, true, &metadata_error))
          record_failure(entry.destination, metadata_error);
      }
    }
    if (summary->failed == 0) {
      manifest.materialization_complete = true;
      std::string checkpoint_error;
      if (!write_staging_manifest_atomic(manifest_path, manifest, &checkpoint_error))
        record_failure("", checkpoint_error);
    }
  }

  if (summary->failed == 0 && manifest.materialization_complete) {
    for (const StagingEntry& entry : manifest.entries) {
      if (entry.type != StagingEntryType::File) continue;
      std::string cleanup_error;
      if (!consume_regular_source(source_root, entry, &cleanup_error)) {
        record_failure(entry.destination, cleanup_error);
        continue;
      }
      StagingEntrySummary* result = find_summary(summary, entry.destination);
      if (result) result->consumed = true;
      ++summary->consumed;
    }
  }
  if (summary->failed == 0 && manifest.materialization_complete) {
    if (unlink(manifest_path.c_str()) != 0 && errno != ENOENT) {
      record_failure("", errno_message("failed to remove completed manifest"));
    } else {
      summary->manifest_removed = true;
    }
  }
  return summary->success();
}

}  // namespace wakefs
