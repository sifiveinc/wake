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

#include <filesystem>
#include <fstream>
#include <optional>

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

// Fixed IDs keep tests predictable; two IDs let tests distinguish executions.
const char* kUuidA = "0123456789abcdef0123456789abcdef";
const char* kUuidB = "fedcba9876543210fedcba9876543210";

wakefs::StagingManifest v2_manifest(const std::string& uuid, std::optional<int64_t> run,
                                    std::optional<int64_t> job) {
  wakefs::StagingManifest manifest;
  manifest.version = 2;
  manifest.job_key = "key";
  manifest.created_at_ns = 1;
  manifest.daemon_pid = getpid();
  manifest.wake_run_id = run;
  manifest.wake_job_id = job;
  manifest.execution_uuid = uuid;
  manifest.job_staging_dir = wakefs::staging_job_dir_name(job, uuid);
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

TEST(staging_manifest_rejects_unsafe_input, "cas") {
  const std::string prefix =
      R"({"version":2,"job_key":"job","created_at_ns":1,"materialization_complete":false,"job_staging_dir":"wakebox-exec-0123456789abcdef0123456789abcdef","execution_uuid":"0123456789abcdef0123456789abcdef","entries":[)";
  const std::string file =
      R"({"destination":"out","type":"file","staging_path":"wakebox-exec-0123456789abcdef0123456789abcdef/1-source","mode":420,"mtime_sec":0,"mtime_nsec":0})";
  wakefs::StagingManifest manifest;
  std::string error;
  ASSERT_TRUE(wakefs::parse_staging_manifest(prefix + file + "]}", &manifest, &error));
  EXPECT_FALSE(wakefs::parse_staging_manifest(
      prefix +
          R"({"destination":"../escape","type":"file","staging_path":"wakebox-exec-0123456789abcdef0123456789abcdef/1-source","mode":420,"mtime_sec":0,"mtime_nsec":0}]})",
      &manifest, &error));
  EXPECT_EQUAL(error, std::string("entry destination must be a normalized relative path"));
  EXPECT_FALSE(wakefs::parse_staging_manifest(
      prefix +
          R"({"destination":"out","type":"file","staging_path":"../source","mode":420,"mtime_sec":0,"mtime_nsec":0}]})",
      &manifest, &error));
  EXPECT_EQUAL(error, std::string("entry staging_path is invalid"));
  EXPECT_FALSE(
      wakefs::parse_staging_manifest(prefix + file + "," + file + "]}", &manifest, &error));
  EXPECT_EQUAL(error, std::string("manifest contains duplicate destination: out"));
}

TEST(staging_manifest_wake_identity_is_paired, "cas") {
  wakefs::StagingManifest manifest;
  std::string error;
  EXPECT_FALSE(wakefs::parse_staging_manifest(
      R"({"version":2,"job_key":"job","created_at_ns":1,"materialization_complete":false,"job_staging_dir":"job-2-exec-0123456789abcdef0123456789abcdef","execution_uuid":"0123456789abcdef0123456789abcdef","wake_run_id":1,"entries":[]})",
      &manifest, &error));
  EXPECT_EQUAL(error, std::string("wake_run_id and wake_job_id must be provided together"));
  EXPECT_FALSE(wakefs::parse_staging_manifest(
      R"({"version":2,"job_key":"job","created_at_ns":1,"materialization_complete":false,"job_staging_dir":"job-2-exec-0123456789abcdef0123456789abcdef","execution_uuid":"0123456789abcdef0123456789abcdef","wake_run_id":"1","wake_job_id":2,"entries":[]})",
      &manifest, &error));
  EXPECT_EQUAL(error, std::string("wake_run_id and wake_job_id must be integers"));
}

TEST(staging_manifest_accepts_missing_source_with_existing_destination, "cas") {
  const std::string root = test_root("missing_source_destination");
  wakefs::StagingManifest manifest = v2_manifest(kUuidA, 4, 2008);
  const std::string job_dir = staging(root) + "/" + manifest.job_staging_dir;
  fs::create_directories(job_dir);
  write_file(workspace(root) + "/output", "already materialized");
  manifest.entries = {{"output", wakefs::StagingEntryType::File,
                       manifest.job_staging_dir + "/missing-source", "", 0644, 1, 2}};
  const std::string path = job_dir + "/recovery.json";
  std::string error;
  ASSERT_TRUE(wakefs::write_staging_manifest_atomic(path, manifest, &error));

  wakefs::StagingMaterializationSummary summary;
  ASSERT_TRUE(materialize_from_workspace(root, path, &summary, &error));
  EXPECT_EQUAL(summary.materialized, 1U);
  EXPECT_EQUAL(summary.consumed, 1U);
  EXPECT_EQUAL(read_file(workspace(root) + "/output"), std::string("already materialized"));
  EXPECT_TRUE(summary.directory_removed);
  EXPECT_FALSE(fs::exists(job_dir));
  fs::remove_all(root);
}

TEST(staging_manifest_rejects_missing_source_without_destination, "cas") {
  const std::string root = test_root("missing_source");
  wakefs::StagingManifest manifest = v2_manifest(kUuidA, 4, 2008);
  const std::string job_dir = staging(root) + "/" + manifest.job_staging_dir;
  fs::create_directories(job_dir);
  manifest.entries = {{"output", wakefs::StagingEntryType::File,
                       manifest.job_staging_dir + "/missing-source", "", 0644, 1, 2}};
  const std::string path = job_dir + "/recovery.json";
  std::string error;
  ASSERT_TRUE(wakefs::write_staging_manifest_atomic(path, manifest, &error));

  wakefs::StagingMaterializationSummary summary;
  EXPECT_FALSE(materialize_from_workspace(root, path, &summary, &error));
  ASSERT_EQUAL(summary.entries.size(), 1U);
  EXPECT_EQUAL(summary.failed, 1U);
  EXPECT_EQUAL(summary.entries[0].error,
               "staging source and materialized destination are both missing: " +
                   manifest.job_staging_dir + "/missing-source");
  EXPECT_TRUE(fs::exists(path));
  EXPECT_FALSE(fs::exists(workspace(root) + "/output"));
  fs::remove_all(root);
}

TEST(staging_manifest_blocks_destination_symlinks, "cas") {
  const std::string root = test_root("symlink");
  wakefs::StagingManifest manifest = v2_manifest(kUuidA, 4, 2008);
  const std::string job_dir = staging(root) + "/" + manifest.job_staging_dir;
  fs::create_directories(root + "/outside");
  write_file(job_dir + "/source", "safe");
  ASSERT_TRUE(symlink("../outside", (workspace(root) + "/link").c_str()) == 0);
  manifest.entries = {{"link/file", wakefs::StagingEntryType::File,
                       manifest.job_staging_dir + "/source", "", 0644, 1, 1}};
  const std::string path = job_dir + "/recovery.json";
  std::string error;
  ASSERT_TRUE(wakefs::write_staging_manifest_atomic(path, manifest, &error));
  wakefs::StagingMaterializationSummary summary;
  EXPECT_FALSE(materialize_from_workspace(root, path, &summary, &error));
  ASSERT_EQUAL(summary.entries.size(), 1U);
  EXPECT_EQUAL(summary.failed, 1U);
  EXPECT_TRUE(summary.entries[0].error.find("failed to open directory link:") == 0);
  EXPECT_EQUAL(read_file(job_dir + "/source"), std::string("safe"));
  EXPECT_TRUE(fs::exists(path));
  EXPECT_FALSE(fs::exists(root + "/outside/file"));
  fs::remove_all(root);
}

TEST(staging_manifest_removes_empty_manifest, "cas") {
  const std::string root = test_root("empty");
  wakefs::StagingManifest manifest = v2_manifest(kUuidA, 4, 2008);
  const std::string job_dir = staging(root) + "/" + manifest.job_staging_dir;
  fs::create_directories(job_dir);
  const std::string path = job_dir + "/recovery.json";
  std::string error;
  ASSERT_TRUE(wakefs::write_staging_manifest_atomic(path, manifest, &error));
  wakefs::StagingMaterializationSummary summary;
  ASSERT_TRUE(materialize_from_workspace(root, path, &summary, &error));
  EXPECT_TRUE(summary.manifest_removed);
  EXPECT_TRUE(summary.directory_removed);
  EXPECT_FALSE(fs::exists(job_dir));
  fs::remove_all(root);
}

TEST(staging_manifest_retries_completed_source_cleanup, "cas") {
  const std::string root = test_root("completed");
  wakefs::StagingManifest manifest = v2_manifest(kUuidA, 4, 2008);
  const std::string job_dir = staging(root) + "/" + manifest.job_staging_dir;
  write_file(job_dir + "/source", "do not place");
  write_file(workspace(root) + "/output", "already placed");
  manifest.materialization_complete = true;
  manifest.entries = {{"output", wakefs::StagingEntryType::File,
                       manifest.job_staging_dir + "/source", "", 0644, 1, 2},
                      {"other", wakefs::StagingEntryType::File,
                       manifest.job_staging_dir + "/missing-source", "", 0644, 1, 2}};
  const std::string path = job_dir + "/recovery.json";
  std::string error;
  ASSERT_TRUE(wakefs::write_staging_manifest_atomic(path, manifest, &error));
  wakefs::StagingMaterializationSummary summary;
  ASSERT_TRUE(materialize_from_workspace(root, path, &summary, &error));
  EXPECT_EQUAL(read_file(workspace(root) + "/output"), std::string("already placed"));
  EXPECT_FALSE(fs::exists(workspace(root) + "/other"));
  EXPECT_EQUAL(summary.consumed, 2U);
  EXPECT_TRUE(summary.directory_removed);
  EXPECT_FALSE(fs::exists(job_dir));
  fs::remove_all(root);
}

TEST(staging_manifest_completed_cleanup_rejects_symlink_source, "cas") {
  const std::string root = test_root("cleanup_symlink");
  wakefs::StagingManifest manifest = v2_manifest(kUuidA, 4, 2008);
  const std::string job_dir = staging(root) + "/" + manifest.job_staging_dir;
  fs::create_directories(job_dir);
  write_file(root + "/outside", "preserve");
  ASSERT_TRUE(symlink((root + "/outside").c_str(), (job_dir + "/source").c_str()) == 0);
  manifest.materialization_complete = true;
  manifest.entries = {{"output", wakefs::StagingEntryType::File,
                       manifest.job_staging_dir + "/source", "", 0644, 1, 2}};
  const std::string path = job_dir + "/recovery.json";
  std::string error;
  ASSERT_TRUE(wakefs::write_staging_manifest_atomic(path, manifest, &error));
  wakefs::StagingMaterializationSummary summary;
  EXPECT_FALSE(materialize_from_workspace(root, path, &summary, &error));
  ASSERT_EQUAL(summary.entries.size(), 1U);
  EXPECT_EQUAL(summary.entries[0].error,
               "staging source is not a regular file: " + manifest.job_staging_dir + "/source");
  EXPECT_TRUE(fs::is_symlink(job_dir + "/source"));
  EXPECT_TRUE(fs::exists(path));
  EXPECT_EQUAL(read_file(root + "/outside"), std::string("preserve"));
  fs::remove_all(root);
}

TEST(staging_manifest_requires_v2_fields, "cas") {
  wakefs::StagingManifest manifest;
  std::string error;
  EXPECT_FALSE(wakefs::parse_staging_manifest(
      R"({"version":2,"job_key":"job","created_at_ns":1,"job_staging_dir":"wakebox-exec-0123456789abcdef0123456789abcdef","execution_uuid":"0123456789abcdef0123456789abcdef","entries":[]})",
      &manifest, &error));
  EXPECT_EQUAL(error, std::string("manifest field 'materialization_complete' must be a boolean"));
  EXPECT_FALSE(wakefs::parse_staging_manifest(
      R"({"version":2,"job_key":"job","created_at_ns":1,"materialization_complete":1,"job_staging_dir":"wakebox-exec-0123456789abcdef0123456789abcdef","execution_uuid":"0123456789abcdef0123456789abcdef","entries":[]})",
      &manifest, &error));
  EXPECT_EQUAL(error, std::string("manifest field 'materialization_complete' must be a boolean"));
  EXPECT_FALSE(wakefs::parse_staging_manifest(
      R"({"version":2,"job_key":"job","created_at_ns":1,"materialization_complete":false,"entries":[]})",
      &manifest, &error));
  EXPECT_EQUAL(error, std::string("manifest field 'job_staging_dir' must be a nonempty string"));
  EXPECT_FALSE(wakefs::parse_staging_manifest(
      R"({"version":2,"job_key":"job","created_at_ns":1,"materialization_complete":false,"entries":[],"job_staging_dir":7})",
      &manifest, &error));
  EXPECT_EQUAL(error, std::string("manifest field 'job_staging_dir' must be a nonempty string"));
  EXPECT_FALSE(wakefs::parse_staging_manifest(
      R"({"version":2,"job_key":"job","created_at_ns":1,"materialization_complete":false,"entries":[],"job_staging_dir":"wakebox-exec-0123456789abcdef0123456789abcdef","execution_uuid":7})",
      &manifest, &error));
  EXPECT_EQUAL(error, std::string("manifest field 'execution_uuid' must be a nonempty string"));
  EXPECT_FALSE(wakefs::parse_staging_manifest(R"({"version":3})", &manifest, &error));
  EXPECT_EQUAL(error, std::string("unsupported staging manifest version"));
}

TEST(staging_manifest_materializes_symlink_and_directory, "cas") {
  const std::string root = test_root("symlink_mtime");
  wakefs::StagingManifest manifest = v2_manifest(kUuidA, 4, 2008);
  const std::string job_dir = staging(root) + "/" + manifest.job_staging_dir;
  fs::create_directories(job_dir);
  manifest.entries = {{"link", wakefs::StagingEntryType::Symlink, "", "target", 0, 7, 8},
                      {"dir", wakefs::StagingEntryType::Directory, "", "", 0750, 7, 8}};
  const std::string path = job_dir + "/recovery.json";
  std::string error;
  ASSERT_TRUE(wakefs::write_staging_manifest_atomic(path, manifest, &error));
  wakefs::StagingMaterializationSummary summary;
  ASSERT_TRUE(materialize_from_workspace(root, path, &summary, &error));
  EXPECT_TRUE(fs::is_symlink(workspace(root) + "/link"));
  EXPECT_EQUAL(fs::read_symlink(workspace(root) + "/link").string(), std::string("target"));
  struct stat st;
  ASSERT_TRUE(lstat((workspace(root) + "/link").c_str(), &st) == 0);
  EXPECT_EQUAL(st.st_mtim.tv_sec, 7L);
  EXPECT_EQUAL(st.st_mtim.tv_nsec, 8L);
  ASSERT_TRUE(stat((workspace(root) + "/dir").c_str(), &st) == 0);
  EXPECT_EQUAL(st.st_mode & 07777, static_cast<mode_t>(0750));
  EXPECT_EQUAL(st.st_mtim.tv_sec, 7L);
  EXPECT_EQUAL(st.st_mtim.tv_nsec, 8L);
  EXPECT_TRUE(summary.directory_removed);
  EXPECT_FALSE(fs::exists(job_dir));
  fs::remove_all(root);
}

TEST(staging_manifest_rejects_symlinked_staging_layout, "cas") {
  for (const char* component : {".build", "cas", "staging"}) {
    const std::string root = test_root(component);
    const std::string layout = workspace(root) + "/.build/cas/staging";
    fs::create_directories(workspace(root));
    if (std::string(component) == ".build") {
      fs::create_directories(root + "/outside/cas/staging");
      ASSERT_TRUE(symlink((root + "/outside").c_str(), (workspace(root) + "/.build").c_str()) == 0);
    } else if (std::string(component) == "cas") {
      fs::create_directories(workspace(root) + "/.build");
      fs::create_directories(root + "/outside/staging");
      ASSERT_TRUE(symlink((root + "/outside").c_str(), (workspace(root) + "/.build/cas").c_str()) ==
                  0);
    } else {
      fs::create_directories(workspace(root) + "/.build/cas");
      fs::create_directories(root + "/outside");
      ASSERT_TRUE(symlink((root + "/outside").c_str(), layout.c_str()) == 0);
    }
    wakefs::StagingManifest manifest = v2_manifest(kUuidA, 4, 2008);
    const std::string job_dir = layout + "/" + manifest.job_staging_dir;
    write_file(job_dir + "/source", "preserve");
    manifest.entries = {{"output", wakefs::StagingEntryType::File,
                         manifest.job_staging_dir + "/source", "", 0644, 1, 2}};
    const std::string path = job_dir + "/recovery.json";
    std::string error;
    ASSERT_TRUE(wakefs::write_staging_manifest_atomic(path, manifest, &error));
    wakefs::StagingMaterializationSummary summary;
    EXPECT_FALSE(materialize_from_workspace(root, path, &summary, &error));
    EXPECT_TRUE(error.find(std::string("failed to open directory ") + component + ":") == 0);
    EXPECT_TRUE(fs::exists(path));
    EXPECT_EQUAL(read_file(job_dir + "/source"), std::string("preserve"));
    EXPECT_FALSE(fs::exists(workspace(root) + "/output"));
    fs::remove_all(root);
  }
}

TEST(staging_job_dir_names, "cas") {
  EXPECT_EQUAL(wakefs::staging_job_dir_name(2008, kUuidA), std::string("job-2008-exec-") + kUuidA);
  EXPECT_EQUAL(wakefs::staging_job_dir_name(std::nullopt, kUuidA),
               std::string("wakebox-exec-") + kUuidA);
  EXPECT_TRUE(wakefs::is_staging_execution_directory("job-2008-exec-" + std::string(kUuidA)));
  EXPECT_TRUE(wakefs::is_staging_execution_directory("wakebox-exec-" + std::string(kUuidA)));
  EXPECT_FALSE(wakefs::is_staging_execution_directory("job-2008.1234.0"));
  EXPECT_FALSE(wakefs::is_staging_execution_directory("job-abc-exec-" + std::string(kUuidA)));
  EXPECT_FALSE(
      wakefs::is_staging_execution_directory("job-2008-exec-0123456789ABCDEF0123456789abcdef"));
  EXPECT_TRUE(wakefs::is_execution_uuid(kUuidA));
  EXPECT_FALSE(wakefs::is_execution_uuid("0123456789ABCDEF0123456789abcdef"));
  EXPECT_FALSE(wakefs::is_execution_uuid("0123456789abcdef0123456789abcde"));
  EXPECT_FALSE(wakefs::is_execution_uuid("01234567-89ab-cdef-0123-456789abcdef"));
}

TEST(staging_payload_names, "cas") {
  EXPECT_EQUAL(wakefs::staging_payload_name(1, "main.o"), std::string("1-main.o"));
  EXPECT_EQUAL(wakefs::staging_payload_name(2, "main.o"), std::string("2-main.o"));
  EXPECT_EQUAL(wakefs::staging_payload_name(1000, "a"), std::string("1000-a"));
  EXPECT_EQUAL(wakefs::staging_payload_name(4, "recovery.json"), std::string("4-recovery.json"));
  EXPECT_EQUAL(wakefs::staging_payload_name(6, "a b\n\xc3\xa9"), std::string("6-a_b___"));
  EXPECT_EQUAL(wakefs::staging_payload_name(7, ""), std::string("7-file"));
  EXPECT_EQUAL(wakefs::staging_payload_name(1, std::string(97, 'x') + "\xc3\xa9"),
               "1-" + std::string(97, 'x') + "_");
}

TEST(staging_manifest_v2_round_trip, "cas") {
  const std::string root = test_root("v2_round_trip");
  wakefs::StagingManifest manifest = v2_manifest(kUuidA, 4, 2008);
  const std::string dir = manifest.job_staging_dir;
  fs::create_directories(staging(root) + "/" + dir);
  manifest.entries = {
      {"build/main.o", wakefs::StagingEntryType::File, dir + "/1-main.o", "", 0644, 1, 2},
      {"src/main.o", wakefs::StagingEntryType::File, dir + "/2-main.o", "", 0644, 3, 4},
      {"link", wakefs::StagingEntryType::Symlink, "", "build/main.o", 0, 5, 6},
      {"build", wakefs::StagingEntryType::Directory, "", "", 0750, 7, 8}};
  const std::string path = staging(root) + "/" + dir + "/recovery.json";
  std::string error;
  const mode_t previous_umask = umask(0077);
  const bool written = wakefs::write_staging_manifest_atomic(path, manifest, &error);
  umask(previous_umask);
  ASSERT_TRUE(written);
  struct stat st;
  ASSERT_TRUE(stat(path.c_str(), &st) == 0);
  EXPECT_EQUAL(st.st_mode & 07777, static_cast<mode_t>(0644));
  const std::string text = read_file(path);
  EXPECT_TRUE(text.find("\"version\":2") != std::string::npos);
  EXPECT_TRUE(text.find("\"job_staging_dir\":\"" + dir + "\"") != std::string::npos);
  EXPECT_TRUE(text.find(std::string("\"execution_uuid\":\"") + kUuidA + "\"") != std::string::npos);

  wakefs::StagingManifest parsed;
  ASSERT_TRUE(wakefs::read_staging_manifest(path, &parsed, &error));
  EXPECT_EQUAL(parsed.version, 2);
  EXPECT_EQUAL(parsed.job_staging_dir, dir);
  EXPECT_EQUAL(parsed.execution_uuid, std::string(kUuidA));
  ASSERT_EQUAL(parsed.entries.size(), 4U);
  EXPECT_EQUAL(parsed.entries[0].staging_path, dir + "/1-main.o");
  EXPECT_EQUAL(parsed.entries[2].target, std::string("build/main.o"));
  EXPECT_EQUAL(parsed.entries[3].mode, static_cast<mode_t>(0750));
  fs::remove_all(root);
}

TEST(staging_manifest_v2_rejects_bad_layout, "cas") {
  const std::string root = test_root("v2_bad_layout");
  std::string error;
  wakefs::StagingManifest base = v2_manifest(kUuidA, 4, 2008);
  const std::string dir = base.job_staging_dir;
  const std::string job_dir = staging(root) + "/" + dir;
  fs::create_directories(job_dir);
  const std::string path = job_dir + "/recovery.json";
  base.entries = {{"out", wakefs::StagingEntryType::File, dir + "/1-x", "", 0644, 1, 2}};
  ASSERT_TRUE(wakefs::write_staging_manifest_atomic(path, base, &error));
  const std::string original = read_file(path);
  auto write_with = [&](wakefs::StagingManifest manifest) {
    return wakefs::write_staging_manifest_atomic(path, manifest, &error);
  };
  auto file_entry = [&](const std::string& source) {
    return wakefs::StagingEntry{"out", wakefs::StagingEntryType::File, source, "", 0644, 1, 2};
  };

  wakefs::StagingManifest manifest = base;
  manifest.entries = {file_entry("job-9-exec-" + std::string(kUuidB) + "/1-x")};
  EXPECT_FALSE(write_with(manifest));  // another job's directory
  EXPECT_EQUAL(error, std::string("entry staging_path is outside the job staging directory"));
  manifest.entries = {file_entry(dir + "/sub/1-x")};
  EXPECT_FALSE(write_with(manifest));  // not a direct child
  EXPECT_EQUAL(
      error, std::string("entry staging_path must be a direct payload child of its job directory"));
  manifest.entries = {file_entry(dir + "/recovery.json")};
  EXPECT_FALSE(write_with(manifest));  // the manifest itself
  EXPECT_EQUAL(
      error, std::string("entry staging_path must be a direct payload child of its job directory"));
  manifest.entries = {file_entry(dir + "/.hidden")};
  EXPECT_FALSE(write_with(manifest));  // hidden control file
  EXPECT_EQUAL(
      error, std::string("entry staging_path must be a direct payload child of its job directory"));
  manifest.entries = {file_entry("1-x")};
  EXPECT_FALSE(write_with(manifest));  // flat source
  EXPECT_EQUAL(error, std::string("entry staging_path is outside the job staging directory"));
  manifest = base;
  manifest.job_staging_dir = std::string("job-2008-exec-") + kUuidB;
  EXPECT_FALSE(write_with(manifest));  // directory and UUID disagree
  EXPECT_EQUAL(error, std::string("job_staging_dir does not match the execution identity"));
  manifest = base;
  manifest.execution_uuid = "NOT-A-UUID";
  EXPECT_FALSE(write_with(manifest));
  EXPECT_EQUAL(error, std::string("execution_uuid must be 32 lowercase hexadecimal digits"));
  manifest = base;
  manifest.job_staging_dir = "job-7-exec-" + std::string(kUuidA);
  EXPECT_FALSE(write_with(manifest));  // directory and job ID disagree
  EXPECT_EQUAL(error, std::string("job_staging_dir does not match the execution identity"));
  manifest = base;
  manifest.version = 3;
  EXPECT_FALSE(write_with(manifest));
  EXPECT_EQUAL(error, std::string("unsupported staging manifest version"));
  EXPECT_EQUAL(read_file(path), original);
  fs::remove_all(root);
}

TEST(staging_manifest_v2_parser_rejects_unsafe_sources, "cas") {
  const std::string prefix =
      R"({"version":2,"job_key":"job","created_at_ns":1,"materialization_complete":false,"job_staging_dir":"wakebox-exec-0123456789abcdef0123456789abcdef","execution_uuid":"0123456789abcdef0123456789abcdef","entries":[{"destination":"out","type":"file","staging_path":")";
  const std::string suffix = R"(","mode":420,"mtime_sec":0,"mtime_nsec":0}]})";
  wakefs::StagingManifest manifest;
  std::string error;
  ASSERT_TRUE(wakefs::parse_staging_manifest(
      prefix + "wakebox-exec-0123456789abcdef0123456789abcdef/1-x" + suffix, &manifest, &error));
  EXPECT_FALSE(wakefs::parse_staging_manifest(
      prefix + "wakebox-exec-fedcba9876543210fedcba9876543210/1-x" + suffix, &manifest, &error));
  EXPECT_EQUAL(error, std::string("entry staging_path is outside the job staging directory"));
  EXPECT_FALSE(wakefs::parse_staging_manifest(
      prefix + "wakebox-exec-0123456789abcdef0123456789abcdef/recovery.json" + suffix, &manifest,
      &error));
  EXPECT_EQUAL(
      error, std::string("entry staging_path must be a direct payload child of its job directory"));
}

TEST(staging_manifest_v2_retries_partial_materialization, "cas") {
  const std::string root = test_root("v2_retry");
  wakefs::StagingManifest manifest = v2_manifest(kUuidA, 4, 2008);
  const std::string dir = manifest.job_staging_dir;
  const std::string job_dir = staging(root) + "/" + dir;
  write_file(job_dir + "/1-one", "first");
  write_file(workspace(root) + "/one", "old one");
  manifest.entries = {{"one", wakefs::StagingEntryType::File, dir + "/1-one", "", 0644, 1, 2},
                      {"two", wakefs::StagingEntryType::File, dir + "/2-two", "", 0644, 1, 2}};
  const std::string path = job_dir + "/recovery.json";
  std::string error;
  ASSERT_TRUE(wakefs::write_staging_manifest_atomic(path, manifest, &error));

  wakefs::StagingMaterializationSummary first;
  EXPECT_FALSE(materialize_from_workspace(root, path, &first, &error));
  EXPECT_EQUAL(first.materialized, 1U);
  EXPECT_EQUAL(first.failed, 1U);
  ASSERT_EQUAL(first.entries.size(), 2U);
  EXPECT_EQUAL(first.entries[1].error,
               "staging source and materialized destination are both missing: " + dir + "/2-two");
  EXPECT_EQUAL(read_file(workspace(root) + "/one"), std::string("first"));
  EXPECT_EQUAL(read_file(job_dir + "/1-one"), std::string("first"));
  wakefs::StagingManifest pending;
  ASSERT_TRUE(wakefs::read_staging_manifest(path, &pending, &error));
  EXPECT_FALSE(pending.materialization_complete);
  EXPECT_EQUAL(pending.entries.size(), 2U);

  write_file(job_dir + "/2-two", "second");
  wakefs::StagingMaterializationSummary second;
  ASSERT_TRUE(materialize_from_workspace(root, path, &second, &error));
  EXPECT_EQUAL(read_file(workspace(root) + "/one"), std::string("first"));
  EXPECT_EQUAL(read_file(workspace(root) + "/two"), std::string("second"));
  EXPECT_TRUE(second.manifest_removed);
  EXPECT_TRUE(second.directory_removed);
  EXPECT_FALSE(fs::exists(job_dir));
  fs::remove_all(root);
}

TEST(staging_manifest_v2_keeps_directory_with_unknown_contents, "cas") {
  const std::string root = test_root("v2_unknown");
  wakefs::StagingManifest manifest = v2_manifest(kUuidA, std::nullopt, std::nullopt);
  const std::string dir = manifest.job_staging_dir;
  const std::string job_dir = staging(root) + "/" + dir;
  write_file(job_dir + "/1-out", "data");
  write_file(job_dir + "/unrelated", "not ours");
  manifest.entries = {{"out", wakefs::StagingEntryType::File, dir + "/1-out", "", 0644, 1, 2}};
  const std::string path = job_dir + "/recovery.json";
  std::string error;
  ASSERT_TRUE(wakefs::write_staging_manifest_atomic(path, manifest, &error));

  wakefs::StagingMaterializationSummary summary;
  ASSERT_TRUE(materialize_from_workspace(root, path, &summary, &error));
  EXPECT_TRUE(summary.manifest_removed);
  EXPECT_FALSE(summary.directory_removed);  // rmdir is never recursive
  EXPECT_TRUE(fs::exists(job_dir + "/unrelated"));
  fs::remove_all(root);
}

TEST(staging_manifest_v2_rejects_misplaced_manifest, "cas") {
  const std::string root = test_root("v2_misplaced");
  wakefs::StagingManifest manifest = v2_manifest(kUuidA, 4, 2008);
  const std::string dir = manifest.job_staging_dir;
  write_file(staging(root) + "/" + dir + "/1-out", "data");
  manifest.entries = {{"out", wakefs::StagingEntryType::File, dir + "/1-out", "", 0644, 1, 2}};
  std::string error;

  // Not named recovery.json.
  const std::string renamed = staging(root) + "/" + dir + "/other.json";
  ASSERT_TRUE(wakefs::write_staging_manifest_atomic(renamed, manifest, &error));
  wakefs::StagingMaterializationSummary summary;
  EXPECT_FALSE(materialize_from_workspace(root, renamed, &summary, &error));
  EXPECT_EQUAL(error, "manifest is not " + dir + "/recovery.json: " + renamed);
  EXPECT_TRUE(fs::exists(renamed));

  // In the wrong directory.
  const std::string wrong = staging(root) + "/job-9-exec-" + kUuidB + "/recovery.json";
  fs::create_directories(fs::path(wrong).parent_path());
  ASSERT_TRUE(wakefs::write_staging_manifest_atomic(wrong, manifest, &error));
  EXPECT_FALSE(materialize_from_workspace(root, wrong, &summary, &error));
  EXPECT_EQUAL(error, "manifest is not " + dir + "/recovery.json: " + wrong);
  EXPECT_TRUE(fs::exists(wrong));
  EXPECT_FALSE(fs::exists(workspace(root) + "/out"));
  EXPECT_TRUE(fs::exists(staging(root) + "/" + dir + "/1-out"));
  fs::remove_all(root);
}

TEST(staging_manifest_v2_relocates_with_identical_bytes, "cas") {
  const std::string source = test_root("v2_reloc_source");
  const std::string destination = test_root("v2_reloc_destination");
  wakefs::StagingManifest manifest = v2_manifest(kUuidA, 4, 2008);
  const std::string dir = manifest.job_staging_dir;
  write_file(staging(source) + "/" + dir + "/1-out", "relocated");
  manifest.entries = {{"out", wakefs::StagingEntryType::File, dir + "/1-out", "", 0644, 1, 2}};
  std::string error;
  const std::string source_manifest = staging(source) + "/" + dir + "/recovery.json";
  ASSERT_TRUE(wakefs::write_staging_manifest_atomic(source_manifest, manifest, &error));

  fs::create_directories(staging(destination) + "/" + dir);
  fs::copy_file(source_manifest, staging(destination) + "/" + dir + "/recovery.json");
  fs::copy_file(staging(source) + "/" + dir + "/1-out",
                staging(destination) + "/" + dir + "/1-out");
  EXPECT_EQUAL(read_file(staging(destination) + "/" + dir + "/recovery.json"),
               read_file(source_manifest));

  wakefs::StagingMaterializationSummary summary;
  ASSERT_TRUE(materialize_from_workspace(
      destination, staging(destination) + "/" + dir + "/recovery.json", &summary, &error));
  EXPECT_EQUAL(read_file(workspace(destination) + "/out"), std::string("relocated"));
  EXPECT_FALSE(fs::exists(staging(destination) + "/" + dir));
  fs::remove_all(source);
  fs::remove_all(destination);
}

TEST(staging_manifest_discovers_job_local_records, "cas") {
  const std::string root = test_root("discovery_layouts");
  std::string error;

  // Current layout: two executions of the same Wake job, and a standalone Wakebox execution.
  wakefs::StagingManifest first = v2_manifest(kUuidA, 4, 2008);
  first.created_at_ns = 20;
  wakefs::StagingManifest second = v2_manifest(kUuidB, 4, 2008);
  second.created_at_ns = 30;
  wakefs::StagingManifest standalone = v2_manifest(kUuidA, std::nullopt, std::nullopt);
  standalone.created_at_ns = 40;
  for (const wakefs::StagingManifest* manifest : {&first, &second, &standalone}) {
    fs::create_directories(staging(root) + "/" + manifest->job_staging_dir);
    ASSERT_TRUE(wakefs::write_staging_manifest_atomic(
        staging(root) + "/" + manifest->job_staging_dir + "/recovery.json", *manifest, &error));
  }

  // Ignore CAS scratch, incomplete atomic writes, orphans, and private workspace contents.
  write_file(staging(root) + "/" + first.job_staging_dir + "/.recovery.json.tmp.1.0", "{ partial");
  write_file(staging(root) + "/scratch-file.123.0", "scratch");
  write_file(staging(root) + "/job-5-exec-" + kUuidA + "/1-orphan", "orphan");
  const std::string private_manifest =
      staging(root) + "/job-6-exec-" + kUuidA + "/workspace/recovery.json";
  fs::create_directories(fs::path(private_manifest).parent_path());
  ASSERT_TRUE(wakefs::write_staging_manifest_atomic(private_manifest, first, &error));

  std::vector<wakefs::CompletedStagingManifest> manifests;
  ASSERT_TRUE(wakefs::discover_staging_manifests(staging(root), &manifests, &error));
  ASSERT_EQUAL(manifests.size(), 3U);
  EXPECT_EQUAL(manifests[0].manifest.execution_uuid, std::string(kUuidA));
  ASSERT_TRUE(manifests[0].manifest.wake_run_id.has_value());
  ASSERT_TRUE(manifests[0].manifest.wake_job_id.has_value());
  EXPECT_EQUAL(*manifests[0].manifest.wake_run_id, 4);
  EXPECT_EQUAL(*manifests[0].manifest.wake_job_id, 2008);
  EXPECT_EQUAL(manifests[1].manifest.execution_uuid, std::string(kUuidB));
  EXPECT_EQUAL(manifests[2].manifest.job_staging_dir, std::string("wakebox-exec-") + kUuidA);
  EXPECT_TRUE(fs::exists(private_manifest));

  // Missing staging root is not an error.
  ASSERT_TRUE(wakefs::discover_staging_manifests(root + "/absent", &manifests, &error));
  EXPECT_TRUE(manifests.empty());
  fs::remove_all(root);
}
