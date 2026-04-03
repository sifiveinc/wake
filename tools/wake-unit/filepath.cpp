/*
 * Copyright 2022 SiFive, Inc.
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

#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wcl/filepath.h>
#include <wcl/xoshiro_256.h>

#include <fstream>
#include <map>
#include <random>
#include <set>
#include <string>
#include <vector>

#include "unit.h"

static std::vector<std::string> to_vec(std::string str) {
  std::vector<std::string> actual;

  for (std::string&& node : wcl::make_filepath_range(str)) {
    actual.emplace_back(std::move(node));
  }

  return actual;
}

TEST(filepath_range_basic) {
  std::vector<std::string> expected = {"this", "is", "a", "test"};

  {
    std::vector<std::string> actual = to_vec("this/is/a/test");
    EXPECT_EQUAL(expected, actual);
  }

  {
    std::vector<std::string> actual = to_vec("/this/is/a/test");
    EXPECT_EQUAL(expected, actual);
  }

  {
    std::vector<std::string> actual = to_vec("this/is/a/test/");
    EXPECT_EQUAL(expected, actual);
  }

  {
    std::vector<std::string> actual = to_vec("/this/is/a/test/");
    EXPECT_EQUAL(expected, actual);
  }
}

TEST(filepath_range_empty_node) {
  {
    std::vector<std::string> expected = {"this", "", "a", "test"};
    std::vector<std::string> actual = to_vec("/this//a/test");

    EXPECT_EQUAL(expected, actual);
  }

  {
    std::vector<std::string> expected = {"", "is", "a", "test"};
    std::vector<std::string> actual = to_vec("//is/a/test");

    EXPECT_EQUAL(expected, actual);
  }

  {
    std::vector<std::string> expected = {"this", "is", "a", ""};
    std::vector<std::string> actual = to_vec("/this/is/a//");

    EXPECT_EQUAL(expected, actual);
  }
}

TEST(filepath_range_no_node) {
  std::vector<std::string> expected = {};
  std::vector<std::string> actual = to_vec("");

  EXPECT_EQUAL(expected, actual);
}

TEST(filepath_range_only_slash) {
  std::vector<std::string> expected = {};
  std::vector<std::string> actual = to_vec("/");

  EXPECT_EQUAL(expected, actual);
}

TEST(filepath_range_two_slash) {
  std::vector<std::string> expected = {""};
  std::vector<std::string> actual = to_vec("//");

  EXPECT_EQUAL(expected, actual);
}

TEST(filepath_range_one_node) {
  std::vector<std::string> expected = {"test"};
  {
    std::vector<std::string> actual = to_vec("test");
    EXPECT_EQUAL(expected, actual);
  }

  {
    std::vector<std::string> actual = to_vec("/test");
    EXPECT_EQUAL(expected, actual);
  }

  {
    std::vector<std::string> actual = to_vec("test/");
    EXPECT_EQUAL(expected, actual);
  }

  {
    std::vector<std::string> actual = to_vec("/test/");
    EXPECT_EQUAL(expected, actual);
  }
}

template <class Gen>
static std::string posix_portable_name(int min_length, int max_length, Gen& gen) {
  static const std::string chars =
      ".-_abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
  std::uniform_int_distribution<int> length_dist(min_length, max_length);
  int size = length_dist(gen);

  std::uniform_int_distribution<int> node_length(0, chars.size() - 1);
  std::string out;
  for (int i = 0; i < size; ++i) {
    out += chars[node_length(gen)];
  }

  return out;
}

template <class Gen>
static std::vector<std::string> posix_portable_path(int min_length, int max_length, Gen& gen) {
  std::uniform_int_distribution<int> length_dist(min_length, max_length);
  int size = length_dist(gen);

  std::vector<std::string> out;
  for (int i = 0; i < size; ++i) {
    out.push_back(posix_portable_name(1, 5, gen));  // empty nodes not actully allowed
  }

  return out;
}

template <class Gen>
static std::string to_path(const std::vector<std::string>& vec, Gen& gen) {
  std::uniform_int_distribution<int> coin(0, 1);
  std::string out;

  // Do we want a leading slash?
  if (coin(gen)) {
    out += "/";
  }

  // Always trail and we'll remove it later randomly
  for (const auto& node : vec) {
    out += node;
    out += "/";
  }

  // Now randomly remove the trailing slash
  if (out.size() && coin(gen)) {
    out.pop_back();
  }

  return out;
}

TEST(filepath_range_fuzz_nice) {
  // seed the rng for fuzzing
  uint64_t seedV = 0xdeadbeefdeadbeef;
  std::tuple<uint64_t, uint64_t, uint64_t, uint64_t> seed = {seedV, seedV, seedV, seedV};
  wcl::xoshiro_256 rng(seed);

  for (int i = 0; i < 3; ++i) {
    auto expected = posix_portable_path(0, 5, rng);
    auto path = to_path(expected, rng);
    auto actual = to_vec(path);
    EXPECT_EQUAL(expected, actual) << "NOTE: path = " << path << "\n";
  }
}

static std::string gen_garbage(int min_length, int max_length, wcl::xoshiro_256& gen) {
  std::string out;

  std::uniform_int_distribution<int> length_dist(min_length, max_length);
  int size = length_dist(gen);

  for (int i = 0; i < size; i++) {
    out += static_cast<char>(gen());
  }

  return out;
}

TEST(filepath_range_fuzz_garbage) {
  // seed the rng for fuzzing
  uint64_t seedV = 0xdeadbeefdeadbeef;
  std::tuple<uint64_t, uint64_t, uint64_t, uint64_t> seed = {seedV, seedV, seedV, seedV};
  wcl::xoshiro_256 rng(seed);

  for (int i = 0; i < 10000; ++i) {
    auto path = gen_garbage(0, 128, rng);
    auto actual = to_vec(path);

    // We can't say much about the resulting path but there's a few santity checks we can
    // do. Mostly this test is just to make sure we don't segfault or throw an exception
    // somewhere.
    EXPECT_TRUE(actual.size() >= 0 && actual.size() <= 128)
        << "Actual size: " << actual.size() << ", path size = " << path.size();

    size_t total_size = 0;
    for (const auto& node : actual) {
      total_size += node.size();
    }

    EXPECT_TRUE(total_size <= path.size());
  }
}

TEST(filepath_make_canonical) {
  EXPECT_EQUAL(wcl::make_canonical("."), ".");
  EXPECT_EQUAL(wcl::make_canonical("hax/"), "hax");
  EXPECT_EQUAL(wcl::make_canonical("foo/.././bar.z"), "bar.z");
  EXPECT_EQUAL(wcl::make_canonical("foo/../../bar.z"), "../bar.z");
}

TEST(filepath_dir_range_empty) {
  ASSERT_TRUE(mkdir("test_dir", 0777) >= 0);
  auto dir_range = wcl::directory_range::open("test_dir");
  ASSERT_TRUE((bool)dir_range);
  for (auto entry : *dir_range) {
    if (entry && entry->name == ".") continue;
    if (entry && entry->name == "..") continue;
    // First assert that the entry has no error
    ASSERT_TRUE((bool)entry);
    // Now assert that we shouldn't have found this in the first place!
    EXPECT_TRUE(false) << " found entry '" << entry->name << "' but epected nothing";
  }
  ASSERT_TRUE(rmdir("test_dir") >= 0);
}

TEST(filepath_dir_range_basic) {
  // We need a clean dir for our tests
  ASSERT_TRUE(mkdir("test_dir", 0777) >= 0);

  std::map<std::string, wcl::file_type> expected_type;
  expected_type["."] = wcl::file_type::directory;
  expected_type[".."] = wcl::file_type::directory;

  auto touch = [&](std::string entry) {
    std::ofstream file("test_dir/" + entry);
    file << " ";
    file.close();
    expected_type[entry] = wcl::file_type::regular;
  };

  auto touch_dir = [&](std::string entry) {
    std::string dir = "test_dir/" + entry;
    ASSERT_TRUE(mkdir(dir.c_str(), 0777) >= 0);
    expected_type[entry] = wcl::file_type::directory;
  };

  auto touch_sym = [&](std::string entry) {
    std::string path = "test_dir/" + entry;
    ASSERT_TRUE(symlink("touch", path.c_str()) >= 0);
    expected_type[entry] = wcl::file_type::symlink;
  };

  touch("test1.txt");
  touch("test2.txt");
  touch("test3.txt");
  touch_dir("test1");
  touch_dir("test2");
  touch_dir("test3");
  touch_sym("sym1");
  touch_sym("sym2");

  auto dir_range = wcl::directory_range::open("test_dir");
  ASSERT_TRUE((bool)dir_range);
  size_t counter = 0;
  for (auto entry : *dir_range) {
    // First assert that the entry has no error
    ASSERT_TRUE((bool)entry) << "entry: " << entry->name;

    // Now check the type if we can
    EXPECT_TRUE((bool)expected_type.count(entry->name));
    if (expected_type.count(entry->name)) {
      EXPECT_EQUAL(expected_type[entry->name], entry->type) << " on entry '" << entry->name << "'";
    }

    // And record how many files we found to make sure we found
    // everything we expected
    counter++;
  }

  EXPECT_EQUAL(expected_type.size(), counter);

  // Now clean up
  for (auto entry : expected_type) {
    if (entry.first == ".") continue;
    if (entry.first == "..") continue;
    std::string path = "test_dir/" + entry.first;
    if (entry.second == wcl::file_type::directory) {
      ASSERT_TRUE(rmdir(path.c_str()) >= 0) << "on entry '" << entry.first << "'";
    } else {
      ASSERT_TRUE(unlink(path.c_str()) >= 0) << "on entry '" << entry.first << "'";
    }
  }
  rmdir("test_dir");
}

TEST(filepath_relative_to) {
  ASSERT_EQUAL("foo/bar", wcl::relative_to("/baz", "/baz/foo/bar"));
  ASSERT_EQUAL("foo/bar", wcl::relative_to("/baz", "foo/bar"));
  ASSERT_EQUAL("foo/bar", wcl::relative_to("/baz", "./foo/bar"));
  ASSERT_EQUAL("../foo/bar", wcl::relative_to("/baz", "../foo/bar"));
  ASSERT_EQUAL("foo/bar", wcl::relative_to("/x/y/z/w/e/f", "foo/bar"));
  ASSERT_EQUAL("../foo", wcl::relative_to("/bar", "/foo"));
  ASSERT_EQUAL("../bar", wcl::relative_to("/baz/foo", "/baz/bar"));
  ASSERT_EQUAL("../../bar/foo", wcl::relative_to("/baz/foo/bar", "/baz/bar/foo"));
  ASSERT_EQUAL("blurp", wcl::relative_to("/foo/bar/baz/blarg", "/foo/bar/baz/blarg/blurp"));
  ASSERT_EQUAL("../blurp", wcl::relative_to("/foo/bar/baz/blarg", "/foo/bar/baz/blurp"));
  ASSERT_EQUAL("../blurp/blarg",
               wcl::relative_to("/foo/bar/baz/blarg", "/foo/bar/baz/blurp/blarg"));
}
