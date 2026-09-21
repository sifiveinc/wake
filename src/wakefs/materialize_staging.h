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
#include <string>
#include <vector>

namespace wakefs {

enum class StagingEntryType { File, Symlink, Directory };

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
  std::string workspace_root;
  std::string cas_staging_root;
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

// Parse and validate the version-1 final-staging manifest schema. This validates
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

  bool success() const { return failed == 0; }
};

// Restore every output to its recorded workspace, persist a manifest-level
// completion checkpoint, then consume only its named regular staging sources.
bool materialize_completed_workspace(const std::string& manifest_path,
                                     StagingMaterializationSummary* summary, std::string* error);

}  // namespace wakefs
