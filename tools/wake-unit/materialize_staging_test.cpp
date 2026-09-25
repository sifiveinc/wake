/*
 * Copyright 2026 SiFive, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 */

#include "wakefs/materialize_staging.h"

#include <sys/stat.h>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <fstream>

#include "unit.h"

namespace fs = std::filesystem;

namespace {

std::string test_root(const char* name) {
  return fs::absolute("materialize_staging_test_" + std::string(name) + "_" +
                      std::to_string(getpid()))
      .string();
}

std::string workspace(const std::string& root) { return root + "/workspace"; }

std::string staging(const std::string& root) { return workspace(root) + "/.build/cas/staging"; }

wakefs::StagingManifest basic_manifest(const std::string& root) {
  (void)root;
  wakefs::StagingManifest manifest;
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

bool materialize_from_workspace(const std::string& root, const std::string& manifest_path,
                                wakefs::StagingMaterializationSummary* summary,
                                std::string* error) {
  const fs::path previous = fs::current_path();
  fs::current_path(workspace(root));
  const bool result = wakefs::materialize_completed_workspace(manifest_path, summary, error);
  fs::current_path(previous);
  return result;
}

}  // namespace

TEST(staging_manifest_round_trip, "cas") {
  const std::string root = test_root("round_trip");
  fs::create_directories(staging(root));
  wakefs::StagingManifest manifest = basic_manifest(root);
  manifest.entries = {{"out/file", wakefs::StagingEntryType::File, "source", "", 0640, 123, 456},
                      {"out/link", wakefs::StagingEntryType::Symlink, "", "file", 0, 124, 457},
                      {"out", wakefs::StagingEntryType::Directory, "", "", 0750, 125, 458}};
  const std::string path = staging(root) + "/recovery.json";
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
      R"({"version":2,"job_key":"job","created_at_ns":1,"materialization_complete":false,"entries":[]})",
      &manifest, &error));
  EXPECT_FALSE(wakefs::parse_staging_manifest(
      R"({"version":1,"job_key":"job","created_at_ns":1,"materialization_complete":false,"entries":[{"destination":"../escape","type":"file","staging_path":"source","mode":420,"mtime_sec":0,"mtime_nsec":0}]})",
      &manifest, &error));
  EXPECT_FALSE(wakefs::parse_staging_manifest(
      R"({"version":1,"job_key":"job","created_at_ns":1,"materialization_complete":false,"entries":[{"destination":"out","type":"file","staging_path":"../source","mode":420,"mtime_sec":0,"mtime_nsec":0}]})",
      &manifest, &error));
  EXPECT_FALSE(wakefs::parse_staging_manifest(
      R"({"version":1,"job_key":"job","created_at_ns":1,"materialization_complete":false,"entries":[{"destination":"out","type":"directory","mode":493,"mtime_sec":0,"mtime_nsec":0},{"destination":"out","type":"directory","mode":493,"mtime_sec":0,"mtime_nsec":0}]})",
      &manifest, &error));
}

TEST(staging_manifest_workspace_materialization_retries, "cas") {
  const std::string root = test_root("retry");
  fs::create_directories(staging(root));
  write_file(staging(root) + "/one", "new one");
  write_file(workspace(root) + "/one", "old one");
  wakefs::StagingManifest manifest = basic_manifest(root);
  manifest.entries = {{"one", wakefs::StagingEntryType::File, "one", "", 0644, 10, 20},
                      {"two", wakefs::StagingEntryType::File, "two", "", 0600, 11, 21}};
  const std::string path = staging(root) + "/recovery.json";
  std::string error;
  ASSERT_TRUE(wakefs::write_staging_manifest_atomic(path, manifest, &error));
  wakefs::StagingMaterializationSummary first;
  EXPECT_FALSE(materialize_from_workspace(root, path, &first, &error));
  EXPECT_EQUAL(first.materialized, 1U);
  EXPECT_TRUE(fs::exists(workspace(root) + "/one"));
  EXPECT_EQUAL(read_file(workspace(root) + "/one"), std::string("new one"));
  EXPECT_TRUE(fs::exists(staging(root) + "/one"));
  EXPECT_TRUE(fs::exists(path));
  wakefs::StagingManifest pending;
  ASSERT_TRUE(wakefs::read_staging_manifest(path, &pending, &error));
  EXPECT_FALSE(pending.materialization_complete);
  EXPECT_EQUAL(pending.entries.size(), 2U);
  write_file(staging(root) + "/two", "new two");
  wakefs::StagingMaterializationSummary second;
  EXPECT_TRUE(materialize_from_workspace(root, path, &second, &error));
  EXPECT_EQUAL(read_file(workspace(root) + "/two"), std::string("new two"));
  EXPECT_FALSE(fs::exists(path));
  fs::remove_all(root);
}

TEST(staging_manifest_accepts_missing_source_with_existing_destination, "cas") {
  const std::string root = test_root("missing_source_destination");
  fs::create_directories(staging(root));
  write_file(workspace(root) + "/output", "already materialized");
  wakefs::StagingManifest manifest = basic_manifest(root);
  manifest.entries = {{"output", wakefs::StagingEntryType::File, "missing-source", "", 0644, 1, 2}};
  const std::string path = staging(root) + "/recovery.json";
  std::string error;
  ASSERT_TRUE(wakefs::write_staging_manifest_atomic(path, manifest, &error));

  wakefs::StagingMaterializationSummary summary;
  ASSERT_TRUE(materialize_from_workspace(root, path, &summary, &error));
  EXPECT_EQUAL(summary.materialized, 1U);
  EXPECT_EQUAL(summary.consumed, 1U);
  EXPECT_EQUAL(read_file(workspace(root) + "/output"), std::string("already materialized"));
  EXPECT_FALSE(fs::exists(path));
  fs::remove_all(root);
}

TEST(staging_manifest_rejects_missing_source_without_destination, "cas") {
  const std::string root = test_root("missing_source");
  fs::create_directories(staging(root));
  wakefs::StagingManifest manifest = basic_manifest(root);
  manifest.entries = {{"output", wakefs::StagingEntryType::File, "missing-source", "", 0644, 1, 2}};
  const std::string path = staging(root) + "/recovery.json";
  std::string error;
  ASSERT_TRUE(wakefs::write_staging_manifest_atomic(path, manifest, &error));

  wakefs::StagingMaterializationSummary summary;
  EXPECT_FALSE(materialize_from_workspace(root, path, &summary, &error));
  EXPECT_TRUE(fs::exists(path));
  fs::remove_all(root);
}

TEST(staging_manifest_blocks_destination_symlinks, "cas") {
  const std::string root = test_root("symlink");
  fs::create_directories(staging(root));
  fs::create_directories(root + "/outside");
  write_file(staging(root) + "/source", "safe");
  symlink("../outside", (workspace(root) + "/link").c_str());
  wakefs::StagingManifest manifest = basic_manifest(root);
  manifest.entries = {{"link/file", wakefs::StagingEntryType::File, "source", "", 0644, 1, 1}};
  const std::string path = staging(root) + "/recovery.json";
  std::string error;
  ASSERT_TRUE(wakefs::write_staging_manifest_atomic(path, manifest, &error));
  wakefs::StagingMaterializationSummary summary;
  EXPECT_FALSE(materialize_from_workspace(root, path, &summary, &error));
  EXPECT_TRUE(fs::exists(staging(root) + "/source"));
  EXPECT_FALSE(fs::exists(root + "/outside/file"));
  fs::remove_all(root);
}

TEST(staging_manifest_removes_empty_manifest, "cas") {
  const std::string root = test_root("empty");
  fs::create_directories(staging(root));
  wakefs::StagingManifest manifest = basic_manifest(root);
  const std::string path = staging(root) + "/recovery.json";
  std::string error;
  ASSERT_TRUE(wakefs::write_staging_manifest_atomic(path, manifest, &error));
  wakefs::StagingMaterializationSummary summary;
  EXPECT_TRUE(materialize_from_workspace(root, path, &summary, &error));
  EXPECT_TRUE(summary.manifest_removed);
  EXPECT_FALSE(fs::exists(path));
  fs::remove_all(root);
}

TEST(staging_manifest_retries_completed_source_cleanup, "cas") {
  const std::string root = test_root("completed");
  fs::create_directories(staging(root));
  write_file(workspace(root) + "/output", "already placed");
  wakefs::StagingManifest manifest = basic_manifest(root);
  manifest.materialization_complete = true;
  manifest.entries = {{"output", wakefs::StagingEntryType::File, "missing-source", "", 0644, 1, 2}};
  const std::string path = staging(root) + "/recovery.json";
  std::string error;
  ASSERT_TRUE(wakefs::write_staging_manifest_atomic(path, manifest, &error));
  wakefs::StagingMaterializationSummary summary;
  ASSERT_TRUE(materialize_from_workspace(root, path, &summary, &error));
  EXPECT_EQUAL(read_file(workspace(root) + "/output"), std::string("already placed"));
  EXPECT_EQUAL(summary.consumed, 1U);
  EXPECT_FALSE(fs::exists(path));
  fs::remove_all(root);
}

TEST(staging_manifest_completed_cleanup_rejects_symlink_source, "cas") {
  const std::string root = test_root("cleanup_symlink");
  fs::create_directories(staging(root));
  write_file(root + "/outside", "preserve");
  symlink("../../../../outside", (staging(root) + "/source").c_str());
  wakefs::StagingManifest manifest = basic_manifest(root);
  manifest.materialization_complete = true;
  manifest.entries = {{"output", wakefs::StagingEntryType::File, "source", "", 0644, 1, 2}};
  const std::string path = staging(root) + "/recovery.json";
  std::string error;
  ASSERT_TRUE(wakefs::write_staging_manifest_atomic(path, manifest, &error));
  wakefs::StagingMaterializationSummary summary;
  EXPECT_FALSE(materialize_from_workspace(root, path, &summary, &error));
  EXPECT_TRUE(fs::is_symlink(staging(root) + "/source"));
  EXPECT_TRUE(fs::exists(path));
  EXPECT_EQUAL(read_file(root + "/outside"), std::string("preserve"));
  fs::remove_all(root);
}

TEST(staging_manifest_requires_completion_state, "cas") {
  wakefs::StagingManifest manifest;
  std::string error;
  EXPECT_FALSE(wakefs::parse_staging_manifest(
      R"({"version":1,"job_key":"job","created_at_ns":1,"entries":[]})", &manifest, &error));
  EXPECT_FALSE(wakefs::parse_staging_manifest(
      R"({"version":1,"job_key":"job","created_at_ns":1,"materialization_complete":1,"entries":[]})",
      &manifest, &error));
}

TEST(staging_manifest_materializes_symlink_with_mtime, "cas") {
  const std::string root = test_root("symlink_mtime");
  fs::create_directories(staging(root));
  wakefs::StagingManifest manifest = basic_manifest(root);
  manifest.entries = {{"link", wakefs::StagingEntryType::Symlink, "", "target", 0, 7, 8}};
  const std::string path = staging(root) + "/recovery.json";
  std::string error;
  ASSERT_TRUE(wakefs::write_staging_manifest_atomic(path, manifest, &error));
  wakefs::StagingMaterializationSummary summary;
  ASSERT_TRUE(materialize_from_workspace(root, path, &summary, &error));
  EXPECT_TRUE(fs::is_symlink(workspace(root) + "/link"));
  EXPECT_EQUAL(fs::read_symlink(workspace(root) + "/link").string(), std::string("target"));
  EXPECT_FALSE(fs::exists(path));
  fs::remove_all(root);
}

TEST(staging_manifest_rejects_symlinked_staging_layout, "cas") {
  for (const char* component : {".build", "cas", "staging"}) {
    const std::string root = test_root(component);
    const std::string layout = workspace(root) + "/.build/cas/staging";
    fs::create_directories(workspace(root));
    if (std::string(component) == ".build") {
      fs::create_directories(root + "/outside/cas/staging");
      symlink("../outside", (workspace(root) + "/.build").c_str());
    } else if (std::string(component) == "cas") {
      fs::create_directories(workspace(root) + "/.build");
      fs::create_directories(root + "/outside/staging");
      symlink("../../outside", (workspace(root) + "/.build/cas").c_str());
    } else {
      fs::create_directories(workspace(root) + "/.build/cas");
      fs::create_directories(root + "/outside");
      symlink("../../../../outside", layout.c_str());
    }
    wakefs::StagingManifest manifest = basic_manifest(root);
    const std::string path = root + "/manifest.json";
    std::string error;
    ASSERT_TRUE(wakefs::write_staging_manifest_atomic(path, manifest, &error));
    wakefs::StagingMaterializationSummary summary;
    EXPECT_FALSE(materialize_from_workspace(root, path, &summary, &error));
    EXPECT_TRUE(fs::exists(path));
    fs::remove_all(root);
  }
}

TEST(staging_manifest_materializes_relocated_record, "cas") {
  const std::string source = test_root("relocation_source");
  const std::string destination = test_root("relocation_destination");
  fs::create_directories(staging(source));
  write_file(staging(source) + "/source", "relocated output");
  wakefs::StagingManifest manifest = basic_manifest(source);
  manifest.entries = {{"output", wakefs::StagingEntryType::File, "source", "", 0644, 1, 2}};
  const std::string source_manifest = staging(source) + "/recovery.json";
  std::string error;
  ASSERT_TRUE(wakefs::write_staging_manifest_atomic(source_manifest, manifest, &error));
  const std::string original_manifest = read_file(source_manifest);

  fs::create_directories(staging(destination));
  fs::copy_file(source_manifest, staging(destination) + "/recovery.json");
  fs::copy_file(staging(source) + "/source", staging(destination) + "/source");
  const std::string copied_manifest = staging(destination) + "/recovery.json";
  EXPECT_EQUAL(read_file(copied_manifest), original_manifest);

  wakefs::StagingMaterializationSummary summary;
  ASSERT_TRUE(materialize_from_workspace(destination, copied_manifest, &summary, &error));
  EXPECT_EQUAL(read_file(workspace(destination) + "/output"), std::string("relocated output"));
  EXPECT_FALSE(fs::exists(staging(destination) + "/source"));
  EXPECT_FALSE(fs::exists(copied_manifest));
  fs::remove_all(source);
  fs::remove_all(destination);
}
