/*
 * Copyright 2026 SiFive, Inc.
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

#include "cas/cas.h"

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include "cas/content_hash.h"
#include "unit.h"
#include "wcl/file_ops.h"

namespace fs = std::filesystem;
using namespace cas;

// ============================================================================
// ContentHash tests
// ============================================================================

TEST(content_hash_from_string_same_content, "cas") {
  auto hash1 = ContentHash::from_string("hello world");
  auto hash2 = ContentHash::from_string("hello world");

  EXPECT_EQUAL(hash1, hash2);
  EXPECT_EQUAL(hash1.to_hex(), hash2.to_hex());
}

TEST(content_hash_from_string_different_content, "cas") {
  auto hash1 = ContentHash::from_string("hello world");
  auto hash2 = ContentHash::from_string("hello world!");

  EXPECT_FALSE(hash1.to_hex() == hash2.to_hex());
}

TEST(content_hash_from_string_empty, "cas") {
  auto hash = ContentHash::from_string("");

  // Empty string should still produce a valid hash
  EXPECT_EQUAL(hash.to_hex().length(), 64u);
}

TEST(content_hash_hex_roundtrip, "cas") {
  auto original = ContentHash::from_string("test data");
  std::string hex = original.to_hex();
  auto restored = ContentHash::from_hex(hex);
  ASSERT_TRUE((bool)restored);

  EXPECT_EQUAL(original.to_hex(), restored->to_hex());
}

TEST(content_hash_to_hex_length, "cas") {
  auto hash = ContentHash::from_string("test");
  std::string hex = hash.to_hex();

  // BLAKE2b-256 produces 64 hex characters
  EXPECT_EQUAL(hex.length(), 64u);
}

TEST(content_hash_to_hex_valid_chars, "cas") {
  auto hash = ContentHash::from_string("test");
  std::string hex = hash.to_hex();

  for (char c : hex) {
    bool valid = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
    EXPECT_TRUE(valid);
  }
}

TEST(content_hash_from_file, "cas") {
  std::string test_file = "cas_test_hash_file.txt";
  std::string content = "file content for hashing";

  // Create test file
  {
    std::ofstream ofs(test_file);
    ofs << content;
  }

  auto file_hash = ContentHash::from_file(test_file);
  ASSERT_TRUE((bool)file_hash);

  auto string_hash = ContentHash::from_string(content);
  EXPECT_EQUAL(file_hash->to_hex(), string_hash.to_hex());

  fs::remove(test_file);
}

TEST(content_hash_from_file_not_found, "cas") {
  auto result = ContentHash::from_file("nonexistent_file_12345.txt");
  EXPECT_FALSE((bool)result);
}

TEST(content_hash_equality, "cas") {
  auto hash1 = ContentHash::from_string("test");
  auto hash2 = ContentHash::from_string("test");
  auto hash3 = ContentHash::from_string("different");

  EXPECT_TRUE(hash1 == hash2);
  EXPECT_FALSE(hash1 != hash2);
  EXPECT_FALSE(hash1 == hash3);
  EXPECT_TRUE(hash1 != hash3);
}

// ============================================================================
// wcl::reflink_or_copy_file tests
// ============================================================================

TEST(reflink_or_copy_file_basic, "cas") {
  std::string src = "cas_test_src.txt";
  std::string dst = "cas_test_dst.txt";

  // Create source file
  {
    std::ofstream ofs(src);
    ofs << "test content for copy";
  }

  auto result = wcl::reflink_or_copy_file(src, dst, 0644);
  ASSERT_TRUE((bool)result);

  // Verify destination exists and has correct content
  EXPECT_TRUE(fs::exists(dst));
  {
    std::ifstream ifs(dst);
    std::string content((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    EXPECT_EQUAL(content, std::string("test content for copy"));
  }

  fs::remove(src);
  fs::remove(dst);
}

TEST(reflink_or_copy_file_src_not_found, "cas") {
  auto result = wcl::reflink_or_copy_file("nonexistent_src.txt", "dst.txt", 0644);
  EXPECT_FALSE((bool)result);
}

// ============================================================================
// Cas tests
// ============================================================================

TEST(cas_store_open, "cas") {
  std::string store_path = "cas_test_store";
  fs::remove_all(store_path);

  auto store_result = Cas::open(store_path);
  ASSERT_TRUE((bool)store_result);

  EXPECT_TRUE(fs::exists(store_path));
  EXPECT_TRUE(fs::is_directory(store_path));

  fs::remove_all(store_path);
}

TEST(cas_store_blob_roundtrip, "cas") {
  std::string store_path = "cas_test_store2";
  fs::remove_all(store_path);

  auto store_result = Cas::open(store_path);
  ASSERT_TRUE((bool)store_result);
  auto& store = *store_result;

  std::string content = "This is test blob content";
  auto hash_result = store.store_blob(content);
  ASSERT_TRUE((bool)hash_result);
  auto hash = *hash_result;

  EXPECT_TRUE(store.has_blob(hash));

  auto read_result = store.read_blob(hash);
  ASSERT_TRUE((bool)read_result);
  EXPECT_EQUAL(*read_result, content);

  fs::remove_all(store_path);
}

TEST(cas_store_blob_from_file, "cas") {
  std::string store_path = "cas_test_store3";
  std::string test_file = "cas_test_input.txt";
  fs::remove_all(store_path);

  // Create test file
  std::string content = "File content to store in CAS";
  {
    std::ofstream ofs(test_file);
    ofs << content;
  }

  auto store_result = Cas::open(store_path);
  ASSERT_TRUE((bool)store_result);
  auto& store = *store_result;

  auto hash_result = store.store_blob_from_file(test_file);
  ASSERT_TRUE((bool)hash_result);
  auto hash = *hash_result;

  EXPECT_TRUE(store.has_blob(hash));

  auto read_result = store.read_blob(hash);
  ASSERT_TRUE((bool)read_result);
  EXPECT_EQUAL(*read_result, content);

  fs::remove(test_file);
  fs::remove_all(store_path);
}

TEST(cas_store_has_blob_not_found, "cas") {
  std::string store_path = "cas_test_store4";
  fs::remove_all(store_path);

  auto store_result = Cas::open(store_path);
  ASSERT_TRUE((bool)store_result);
  auto& store = *store_result;

  auto hash = ContentHash::from_string("nonexistent content");
  EXPECT_FALSE(store.has_blob(hash));

  fs::remove_all(store_path);
}

TEST(cas_store_materialize_blob, "cas") {
  std::string store_path = "cas_test_store5";
  std::string output_file = "cas_test_output.txt";
  fs::remove_all(store_path);
  fs::remove(output_file);

  auto store_result = Cas::open(store_path);
  ASSERT_TRUE((bool)store_result);
  auto& store = *store_result;

  std::string content = "Content to materialize";
  auto hash_result = store.store_blob(content);
  ASSERT_TRUE((bool)hash_result);
  auto hash = *hash_result;

  auto materialize_result = store.materialize_blob(hash, output_file, 0644);
  ASSERT_TRUE((bool)materialize_result);

  EXPECT_TRUE(fs::exists(output_file));
  {
    std::ifstream ifs(output_file);
    std::string read_content((std::istreambuf_iterator<char>(ifs)),
                             std::istreambuf_iterator<char>());
    EXPECT_EQUAL(read_content, content);
  }

  fs::remove(output_file);
  fs::remove_all(store_path);
}

TEST(cas_store_deduplication, "cas") {
  std::string store_path = "cas_test_store6";
  fs::remove_all(store_path);

  auto store_result = Cas::open(store_path);
  ASSERT_TRUE((bool)store_result);
  auto& store = *store_result;

  std::string content = "Duplicate content";

  auto hash1_result = store.store_blob(content);
  ASSERT_TRUE((bool)hash1_result);

  auto hash2_result = store.store_blob(content);
  ASSERT_TRUE((bool)hash2_result);

  // Same content should produce same hash
  EXPECT_EQUAL(hash1_result->to_hex(), hash2_result->to_hex());

  fs::remove_all(store_path);
}
