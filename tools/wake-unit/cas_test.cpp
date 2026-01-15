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

// Tests for the CAS (Content-Addressable Storage) module

#include "unit.h"

#include <cstdio>
#include <fstream>
#include <sys/stat.h>

#include "cas/cas.h"
#include "cas/cas_store.h"
#include "cas/file_ops.h"

using namespace cas;

// ============================================================================
// ContentHash tests
// ============================================================================

TEST(content_hash_from_string, "cas") {
  auto hash1 = ContentHash::from_string("hello world");
  auto hash2 = ContentHash::from_string("hello world");
  auto hash3 = ContentHash::from_string("hello world!");

  // Same content should produce same hash
  EXPECT_EQUAL(hash1.to_hex(), hash2.to_hex());

  // Different content should produce different hash
  EXPECT_FALSE(hash1.to_hex() == hash3.to_hex());
}

TEST(content_hash_hex_roundtrip, "cas") {
  auto original = ContentHash::from_string("test data");
  std::string hex = original.to_hex();
  auto restored = ContentHash::from_hex(hex);

  EXPECT_EQUAL(original.to_hex(), restored.to_hex());
}

TEST(content_hash_prefix_suffix, "cas") {
  auto hash = ContentHash::from_string("test");
  std::string hex = hash.to_hex();
  std::string prefix = hash.prefix();
  std::string suffix = hash.suffix();

  // Prefix should be first 2 chars
  EXPECT_EQUAL(prefix.length(), 2u);
  EXPECT_EQUAL(prefix, hex.substr(0, 2));

  // Suffix should be remaining chars
  EXPECT_EQUAL(suffix, hex.substr(2));
}

// ============================================================================
// File operations tests
// ============================================================================

TEST(mkdir_parents_basic, "cas") {
  std::string test_dir = "cas_test_dir/sub1/sub2";

  // Clean up first
  rmdir("cas_test_dir/sub1/sub2");
  rmdir("cas_test_dir/sub1");
  rmdir("cas_test_dir");

  auto result = mkdir_parents(test_dir);
  ASSERT_TRUE((bool)result);

  EXPECT_TRUE(path_exists(test_dir));
  EXPECT_TRUE(is_directory(test_dir));

  // Clean up
  rmdir("cas_test_dir/sub1/sub2");
  rmdir("cas_test_dir/sub1");
  rmdir("cas_test_dir");
}

TEST(copy_file_basic, "cas") {
  std::string src = "cas_test_src.txt";
  std::string dst = "cas_test_dst.txt";

  // Create source file
  {
    std::ofstream ofs(src);
    ofs << "test content for copy";
  }

  auto result = copy_file(src, dst, 0644, true);
  ASSERT_TRUE((bool)result);

  // Verify destination exists
  EXPECT_TRUE(path_exists(dst));

  // Verify content
  {
    std::ifstream ifs(dst);
    std::string content((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());
    EXPECT_EQUAL(content, std::string("test content for copy"));
  }

  // Clean up
  unlink(src.c_str());
  unlink(dst.c_str());
}

// ============================================================================
// CASStore tests
// ============================================================================

TEST(cas_store_open, "cas") {
  std::string store_path = "cas_test_store";

  // Clean up first
  system("rm -rf cas_test_store");

  auto store_result = CASStore::open(store_path);
  ASSERT_TRUE((bool)store_result);

  // Check directories were created
  EXPECT_TRUE(path_exists(store_path));
  EXPECT_TRUE(is_directory(store_path));

  // Clean up
  system("rm -rf cas_test_store");
}

TEST(cas_store_blob_roundtrip, "cas") {
  std::string store_path = "cas_test_store2";
  system("rm -rf cas_test_store2");

  auto store_result = CASStore::open(store_path);
  ASSERT_TRUE((bool)store_result);
  auto& store = *store_result;

  // Store a blob
  std::string content = "This is test blob content";
  auto hash_result = store.store_blob(content);
  ASSERT_TRUE((bool)hash_result);
  auto hash = *hash_result;

  // Verify it exists
  EXPECT_TRUE(store.has_blob(hash));

  // Read it back
  auto read_result = store.read_blob(hash);
  ASSERT_TRUE((bool)read_result);
  EXPECT_EQUAL(*read_result, content);

  // Clean up
  system("rm -rf cas_test_store2");
}

// ============================================================================
// CAS Job Cache Integration tests
// ============================================================================

#include "cas/cas_job_cache.h"

TEST(cas_job_cache_store_file, "cas") {
  std::string store_path = "cas_test_job_cache1";
  std::string test_file = "cas_test_job_cache_file.txt";

  // Clean up from previous runs
  system("rm -rf cas_test_job_cache1 cas_test_job_cache_file.txt");

  // Create a test file
  {
    std::ofstream ofs(test_file);
    ofs << "Test content for job cache";
  }

  // Create store
  auto store_result = CASStore::open(store_path);
  ASSERT_TRUE((bool)store_result);
  auto& store = *store_result;

  // Store the file
  auto hash_result = store_output_file(store, test_file);
  ASSERT_TRUE((bool)hash_result);
  auto hash = *hash_result;

  // Verify the blob exists
  EXPECT_TRUE(has_blob(store, hash));

  // Clean up
  system("rm -rf cas_test_job_cache1 cas_test_job_cache_file.txt");
}

TEST(cas_job_cache_store_multiple_files, "cas") {
  std::string store_path = "cas_test_job_cache2";
  std::string src_dir = "cas_test_job_cache_src";

  // Clean up from previous runs
  system("rm -rf cas_test_job_cache2 cas_test_job_cache_src");

  // Create source directory with files
  mkdir(src_dir.c_str(), 0755);
  {
    std::ofstream ofs(src_dir + "/output1.txt");
    ofs << "Output file 1";
  }
  {
    std::ofstream ofs(src_dir + "/output2.txt");
    ofs << "Output file 2";
  }

  // Create store
  auto store_result = CASStore::open(store_path);
  ASSERT_TRUE((bool)store_result);
  auto& store = *store_result;

  // Store multiple files
  std::vector<std::pair<std::string, std::string>> files = {
    {src_dir + "/output1.txt", "output1.txt"},
    {src_dir + "/output2.txt", "output2.txt"}
  };
  std::vector<std::pair<std::string, mode_t>> modes = {
    {"output1.txt", 0644},
    {"output2.txt", 0644}
  };

  auto result = store_output_files(store, files, modes);
  ASSERT_TRUE((bool)result);
  auto outputs = *result;

  // Verify combined hash is set (not empty/zero)
  EXPECT_FALSE(outputs.tree_hash.is_empty());

  // Verify individual file hashes
  EXPECT_EQUAL(outputs.file_hashes.size(), (size_t)2);

  // Clean up
  system("rm -rf cas_test_job_cache2 cas_test_job_cache_src");
}

TEST(cas_job_cache_materialize, "cas") {
  std::string store_path = "cas_test_job_cache3";
  std::string src_file = "cas_test_job_cache_src_file.txt";
  std::string dst_file = "cas_test_job_cache_dst_file.txt";

  // Clean up from previous runs
  system("rm -rf cas_test_job_cache3 cas_test_job_cache_src_file.txt cas_test_job_cache_dst_file.txt");

  // Create a test file
  {
    std::ofstream ofs(src_file);
    ofs << "Content to materialize";
  }

  // Create store and store the file
  auto store_result = CASStore::open(store_path);
  ASSERT_TRUE((bool)store_result);
  auto& store = *store_result;

  auto hash_result = store_output_file(store, src_file);
  ASSERT_TRUE((bool)hash_result);
  auto hash = *hash_result;

  // Materialize to a new location
  auto mat_result = materialize_file(store, hash, dst_file, 0644);
  ASSERT_TRUE((bool)mat_result);

  // Verify the file was created
  EXPECT_TRUE(path_exists(dst_file));

  // Verify content
  {
    std::ifstream ifs(dst_file);
    std::string content((std::istreambuf_iterator<char>(ifs)),
                         std::istreambuf_iterator<char>());
    EXPECT_EQUAL(content, std::string("Content to materialize"));
  }

  // Clean up
  system("rm -rf cas_test_job_cache3 cas_test_job_cache_src_file.txt cas_test_job_cache_dst_file.txt");
}

