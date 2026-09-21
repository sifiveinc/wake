/*
 * Copyright 2026 SiFive, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>

#include "wakefs/materialize_staging.h"
#include "unit.h"

namespace fs = std::filesystem;

namespace {

std::string test_root(const char* name) {
  return "materialize_staging_test_" + std::string(name) + "_" + std::to_string(getpid());
}

std::string absolute(const std::string& path) { return fs::absolute(path).string(); }

wakefs::StagingManifest basic_manifest(const std::string& root) {
  wakefs::StagingManifest manifest;
  manifest.workspace_root = absolute(root + "/workspace");
  manifest.cas_staging_root = absolute(root + "/staging");
  manifest.job_key = "job-1";
  manifest.created_at_ns = 1;
  manifest.daemon_pid = getpid();
  return manifest;
}

void write_file(const std::string& path, const std::string& text) {
  fs::create_directories(fs::path(path).parent_path());
  std::ofstream stream(path);
  stream << text;
}

std::string read_file(const std::string& path) {
  std::ifstream stream(path);
  return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

}  // namespace

TEST(staging_manifest_round_trip, "cas") {
  const std::string root = test_root("round_trip");
  fs::create_directories(root + "/workspace");
  fs::create_directories(root + "/staging");
  wakefs::StagingManifest manifest = basic_manifest(root);
  manifest.entries = {{"out/file", wakefs::StagingEntryType::File, "source", "", 0640, 123, 456},
                      {"out/link", wakefs::StagingEntryType::Symlink, "", "file", 0, 124, 457},
                      {"out", wakefs::StagingEntryType::Directory, "", "", 0750, 125, 458}};
  const std::string path = root + "/staging/recovery.json";
  std::string error;
  ASSERT_TRUE(wakefs::write_staging_manifest_atomic(path, manifest, &error));
  wakefs::StagingManifest parsed;
  ASSERT_TRUE(wakefs::read_staging_manifest(path, &parsed, &error));
  EXPECT_EQUAL(parsed.entries.size(), 3U);
  EXPECT_EQUAL(parsed.entries[0].staging_path, std::string("source"));
  EXPECT_EQUAL(parsed.entries[1].target, std::string("file"));
  EXPECT_EQUAL(parsed.entries[2].mode, static_cast<mode_t>(0750));
  fs::remove_all(root);
}

TEST(staging_manifest_rejects_unsafe_input, "cas") {
  wakefs::StagingManifest manifest;
  std::string error;
  EXPECT_FALSE(wakefs::parse_staging_manifest(
      R"({"version":2,"workspace_root":"/workspace","cas_staging_root":"/staging","job_key":"job","created_at_ns":1,"entries":[]})",
      &manifest, &error));
  EXPECT_FALSE(wakefs::parse_staging_manifest(
      R"({"version":1,"workspace_root":"/workspace","cas_staging_root":"/staging","job_key":"job","created_at_ns":1,"entries":[{"destination":"../escape","type":"file","staging_path":"source","mode":420,"mtime_sec":0,"mtime_nsec":0}]})",
      &manifest, &error));
  EXPECT_FALSE(wakefs::parse_staging_manifest(
      R"({"version":1,"workspace_root":"/workspace","cas_staging_root":"/staging","job_key":"job","created_at_ns":1,"entries":[{"destination":"out","type":"file","staging_path":"../source","mode":420,"mtime_sec":0,"mtime_nsec":0}]})",
      &manifest, &error));
  EXPECT_FALSE(wakefs::parse_staging_manifest(
      R"({"version":1,"workspace_root":"/workspace","cas_staging_root":"/staging","job_key":"job","created_at_ns":1,"entries":[{"destination":"out","type":"directory","mode":493,"mtime_sec":0,"mtime_nsec":0},{"destination":"out","type":"directory","mode":493,"mtime_sec":0,"mtime_nsec":0}]})",
      &manifest, &error));
}

TEST(staging_manifest_workspace_materialization_retries, "cas") {
  const std::string root = test_root("retry");
  fs::create_directories(root + "/workspace");
  fs::create_directories(root + "/staging");
  write_file(root + "/staging/one", "new one");
  write_file(root + "/workspace/one", "old one");
  wakefs::StagingManifest manifest = basic_manifest(root);
  manifest.entries = {{"one", wakefs::StagingEntryType::File, "one", "", 0644, 10, 20},
                      {"two", wakefs::StagingEntryType::File, "two", "", 0600, 11, 21}};
  const std::string path = root + "/staging/recovery.json";
  std::string error;
  ASSERT_TRUE(wakefs::write_staging_manifest_atomic(path, manifest, &error));
  wakefs::StagingMaterializationSummary first;
  EXPECT_FALSE(wakefs::materialize_completed_workspace(path, &first, &error));
  EXPECT_EQUAL(first.materialized, 1U);
  EXPECT_TRUE(fs::exists(root + "/workspace/one"));
  EXPECT_EQUAL(read_file(root + "/workspace/one"), std::string("new one"));
  EXPECT_TRUE(fs::exists(root + "/staging/one"));
  EXPECT_TRUE(fs::exists(path));
  wakefs::StagingManifest pending;
  ASSERT_TRUE(wakefs::read_staging_manifest(path, &pending, &error));
  EXPECT_FALSE(pending.materialization_complete);
  EXPECT_EQUAL(pending.entries.size(), 2U);
  write_file(root + "/staging/two", "new two");
  wakefs::StagingMaterializationSummary second;
  EXPECT_TRUE(wakefs::materialize_completed_workspace(path, &second, &error));
  EXPECT_EQUAL(read_file(root + "/workspace/two"), std::string("new two"));
  EXPECT_FALSE(fs::exists(path));
  fs::remove_all(root);
}

TEST(staging_manifest_blocks_destination_symlinks, "cas") {
  const std::string root = test_root("symlink");
  fs::create_directories(root + "/workspace");
  fs::create_directories(root + "/staging");
  fs::create_directories(root + "/outside");
  write_file(root + "/staging/source", "safe");
  symlink("../outside", (root + "/workspace/link").c_str());
  wakefs::StagingManifest manifest = basic_manifest(root);
  manifest.entries = {{"link/file", wakefs::StagingEntryType::File, "source", "", 0644, 1, 1}};
  const std::string path = root + "/staging/recovery.json";
  std::string error;
  ASSERT_TRUE(wakefs::write_staging_manifest_atomic(path, manifest, &error));
  wakefs::StagingMaterializationSummary summary;
  EXPECT_FALSE(wakefs::materialize_completed_workspace(path, &summary, &error));
  EXPECT_TRUE(fs::exists(root + "/staging/source"));
  EXPECT_FALSE(fs::exists(root + "/outside/file"));
  fs::remove_all(root);
}

TEST(staging_manifest_removes_empty_manifest, "cas") {
  const std::string root = test_root("empty");
  fs::create_directories(root + "/workspace");
  fs::create_directories(root + "/staging");
  wakefs::StagingManifest manifest = basic_manifest(root);
  const std::string path = root + "/staging/recovery.json";
  std::string error;
  ASSERT_TRUE(wakefs::write_staging_manifest_atomic(path, manifest, &error));
  wakefs::StagingMaterializationSummary summary;
  EXPECT_TRUE(wakefs::materialize_completed_workspace(path, &summary, &error));
  EXPECT_TRUE(summary.manifest_removed);
  EXPECT_FALSE(fs::exists(path));
  fs::remove_all(root);
}

TEST(staging_manifest_retries_completed_source_cleanup, "cas") {
  const std::string root = test_root("completed");
  fs::create_directories(root + "/workspace");
  fs::create_directories(root + "/staging");
  write_file(root + "/workspace/output", "already placed");
  wakefs::StagingManifest manifest = basic_manifest(root);
  manifest.materialization_complete = true;
  manifest.entries = {{"output", wakefs::StagingEntryType::File, "missing-source", "", 0644, 1, 2}};
  const std::string path = root + "/staging/recovery.json";
  std::string error;
  ASSERT_TRUE(wakefs::write_staging_manifest_atomic(path, manifest, &error));
  wakefs::StagingMaterializationSummary summary;
  ASSERT_TRUE(wakefs::materialize_completed_workspace(path, &summary, &error));
  EXPECT_EQUAL(read_file(root + "/workspace/output"), std::string("already placed"));
  EXPECT_EQUAL(summary.consumed, 1U);
  EXPECT_FALSE(fs::exists(path));
  fs::remove_all(root);
}

TEST(staging_manifest_completed_cleanup_rejects_symlink_source, "cas") {
  const std::string root = test_root("cleanup_symlink");
  fs::create_directories(root + "/workspace");
  fs::create_directories(root + "/staging");
  write_file(root + "/outside", "preserve");
  symlink("../outside", (root + "/staging/source").c_str());
  wakefs::StagingManifest manifest = basic_manifest(root);
  manifest.materialization_complete = true;
  manifest.entries = {{"output", wakefs::StagingEntryType::File, "source", "", 0644, 1, 2}};
  const std::string path = root + "/staging/recovery.json";
  std::string error;
  ASSERT_TRUE(wakefs::write_staging_manifest_atomic(path, manifest, &error));
  wakefs::StagingMaterializationSummary summary;
  EXPECT_FALSE(wakefs::materialize_completed_workspace(path, &summary, &error));
  EXPECT_TRUE(fs::is_symlink(root + "/staging/source"));
  EXPECT_TRUE(fs::exists(path));
  EXPECT_EQUAL(read_file(root + "/outside"), std::string("preserve"));
  fs::remove_all(root);
}

TEST(staging_manifest_requires_completion_state, "cas") {
  wakefs::StagingManifest manifest;
  std::string error;
  EXPECT_FALSE(wakefs::parse_staging_manifest(
      R"({"version":1,"workspace_root":"/workspace","cas_staging_root":"/staging","job_key":"job","created_at_ns":1,"entries":[]})",
      &manifest, &error));
  EXPECT_FALSE(wakefs::parse_staging_manifest(
      R"({"version":1,"workspace_root":"/workspace","cas_staging_root":"/staging","job_key":"job","created_at_ns":1,"materialization_complete":1,"entries":[]})",
      &manifest, &error));
  EXPECT_FALSE(wakefs::parse_staging_manifest(
      R"({"version":1,"workspace_root":"/workspace","cas_staging_root":"/staging","job_key":"job","created_at_ns":1,"materialization_complete":false,"entries":[{"destination":"out","type":"file","staging_path":"source","mode":420,"mtime_sec":0,"mtime_nsec":0,"placed":true}]})",
      &manifest, &error));
}


TEST(staging_manifest_materializes_symlink_with_mtime, "cas") {
  const std::string root = test_root("symlink_mtime");
  fs::create_directories(root + "/workspace");
  fs::create_directories(root + "/staging");
  wakefs::StagingManifest manifest = basic_manifest(root);
  manifest.entries = {{"link", wakefs::StagingEntryType::Symlink, "", "target", 0, 7, 8}};
  const std::string path = root + "/staging/recovery.json";
  std::string error;
  ASSERT_TRUE(wakefs::write_staging_manifest_atomic(path, manifest, &error));
  wakefs::StagingMaterializationSummary summary;
  ASSERT_TRUE(wakefs::materialize_completed_workspace(path, &summary, &error));
  EXPECT_TRUE(fs::is_symlink(root + "/workspace/link"));
  EXPECT_EQUAL(fs::read_symlink(root + "/workspace/link").string(), std::string("target"));
  EXPECT_FALSE(fs::exists(path));
  fs::remove_all(root);
}
