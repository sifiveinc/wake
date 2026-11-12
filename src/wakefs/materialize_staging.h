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

#include <cstdint>
#include <optional>
#include <regex>
#include <string>
#include <vector>

namespace wakefs {

enum class StagingEntryType { File, Symlink, Directory };

// Each FUSE execution owns one directory directly below the CAS staging root. Its regular output
// payloads are flat children of that directory, with the recovery manifest alongside them:
//   <staging>/job-<jobID>-execution-<UUID>/recovery.json
//   <staging>/job-<jobID>-execution-<UUID>/1-main.o
//   <staging>/wakebox-execution-<UUID>/...      (standalone Wakebox without WakeDB IDs)
// Legacy (version 1) manifests live in <staging>/recovery/ and refer to flat PID_counter sources.
constexpr const char* kRecoveryManifestName = "recovery.json";
constexpr const char* kLegacyRecoveryDirName = "recovery";

// Filter candidate directories by name; the manifest remains authoritative for identity.
inline bool is_staging_execution_directory(const std::string& name) {
  static const std::regex pattern(R"((job--?[0-9]+|wakebox)-execution-[0-9a-f]{32})");
  return std::regex_match(name, pattern);
}

// The directory leaf for one execution: job-<jobID>-execution-<UUID>, or
// wakebox-execution-<UUID> when there is no Wake job ID.
std::string staging_job_dir_name(std::optional<int64_t> wake_job_id,
                                 const std::string& execution_uuid);

// True for the 32 lowercase hexadecimal digits of a serialized execution UUID.
bool is_execution_uuid(const std::string& value);

// The readable backing filename for the number-th output of an execution: the decimal number,
// a hyphen, and the basename with unsafe bytes replaced by underscores. The readable part is
// truncated so that the whole name fits in 100 bytes, leaving room for CAS ingestion's suffix.
// The unique number prevents collisions even if sanitized or truncated basenames match.
std::string staging_payload_name(uint64_t number, const std::string& basename);

// One output projection described by a final staging manifest.
struct StagingEntry {
  std::string destination;  // Workspace-relative projection path.
  StagingEntryType type;
  std::string staging_path;
  std::string target;
  mode_t mode = 0;
  int64_t mtime_sec = 0;
  long mtime_nsec = 0;
};

// The persisted per-job mapping from staging sources to workspace projections.
struct StagingManifest {
  // 1: legacy flat sources under staging/recovery/. 2: job-local recovery.json whose sources are
  // direct regular children of job_staging_dir.
  int version = 1;
  // Version 2 only: the safe directory leaf below the staging root and its execution UUID.
  std::string job_staging_dir;
  std::string execution_uuid;
  std::string job_key;
  int64_t created_at_ns = 0;
  int64_t daemon_pid = 0;
  // Wake-launched manifests persist both IDs; standalone manifests omit both.
  std::optional<int64_t> wake_run_id;
  std::optional<int64_t> wake_job_id;
  // Set only after every destination has been placed. A retry of a completed
  // manifest consumes its named regular sources without touching destinations.
  bool materialization_complete = false;
  std::vector<StagingEntry> entries;
};

// Parse and validate the final-staging manifest schema. This validates
// lexical path safety; filesystem ownership and confinement are checked when
// materializing.
bool parse_staging_manifest(const std::string& text, StagingManifest* manifest, std::string* error);
bool read_staging_manifest(const std::string& path, StagingManifest* manifest, std::string* error);
bool write_staging_manifest_atomic(const std::string& path, const StagingManifest& manifest,
                                   std::string* error);

// The outcome for one manifest entry during a materialization operation.
struct StagingEntrySummary {
  std::string destination;
  bool materialized = false;
  bool consumed = false;
  std::string error;
};

// Aggregate outcome for one completed-workspace recovery.
struct StagingMaterializationSummary {
  std::vector<StagingEntrySummary> entries;
  size_t materialized = 0;
  size_t consumed = 0;
  size_t failed = 0;
  bool manifest_removed = false;
  // A version 2 job directory that was left empty after cleanup was removed.
  bool directory_removed = false;

  bool success() const { return failed == 0; }
};

// A parsed completed manifest. Wakebox processes recovered manifests in creation
// time and filename order.
struct CompletedStagingManifest {
  std::string path;
  StagingManifest manifest;
};

// Scan only direct regular-file children of recovery_dir, parse each final
// manifest, and return its path and parsed contents. Results are sorted oldest
// first by creation time, then pathname, so processing deterministically lets a
// newer manifest replace an older projection at the same destination.
bool discover_completed_staging_manifests(const std::string& recovery_dir,
                                          std::vector<CompletedStagingManifest>* manifests,
                                          std::string* error);

// Scan the CAS staging root for retained final manifests: each execution-named direct child
// directory's recovery.json, plus the legacy records directly under <staging_root>/recovery/.
// Private execution trees (such as job-N-execution-U/workspace/) are never scanned, and directories
// without a manifest and atomic-write temporaries are ignored. Results are ordered like
// discover_completed_staging_manifests.
bool discover_staging_manifests(const std::string& staging_root,
                                std::vector<CompletedStagingManifest>* manifests,
                                std::string* error);

// For version 2 manifests, check that manifest_path is
// <staging root>/<job_staging_dir>/recovery.json. Version 1 manifests carry no location
// constraint.
bool validate_manifest_location(const std::string& manifest_path, const StagingManifest& manifest,
                                std::string* error);

// Restore outputs to their recorded workspace, then remove their staging sources
// and completed manifest entries.
bool materialize_completed_workspace(const std::string& manifest_path,
                                     const StagingManifest& manifest,
                                     StagingMaterializationSummary* summary, std::string* error);
bool materialize_completed_workspace(const std::string& manifest_path,
                                     StagingMaterializationSummary* summary, std::string* error);

}  // namespace wakefs
