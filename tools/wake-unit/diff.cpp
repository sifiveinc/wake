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

#include <wcl/diff.h>
#include <wcl/xoshiro_256.h>

#include <string>
#include <vector>

#include "unit.h"

// This test function verifies that a given diff is maps origin to target
template <class T>
TEST_FUNC(void, verify_diff, std::vector<wcl::diff_t<T>> diff, std::vector<T> origin,
          std::vector<T> target) {
  auto iter = origin.begin();
  std::vector<T> out = {};

  EXPECT_TRUE(diff.size() <= origin.size() + target.size());

  // Now we go through the diff, advancing the origin iterator
  // as we go
  for (const auto& line : diff) {
    switch (line.type) {
      case wcl::diff_type_t::Add:
        // If we encounter an add we add it but there's nothing
        // to check the origin against. Additionally we don't advance
        // the iterator.
        out.push_back(line.value);
        break;
      case wcl::diff_type_t::Sub:
        // If we encounter the sub we advance the iterator without adding
        // anything to `out` but we also make sure that we're removing what
        // we think we are.
        EXPECT_EQUAL(*iter, line.value)
            << "line " << (iter - origin.begin())
            << " in origin did not match subtraction diff line " << (&line - &diff.front()) << "'";
        iter++;
        break;
      case wcl::diff_type_t::Keep:
        // If we're keeping we want to make sure we're keeping what we think
        // we are but otherwise we can just push AND advance the iterator.
        EXPECT_EQUAL(*iter, line.value)
            << "line " << (iter - origin.begin()) << " in origin did not match keep diff line "
            << (&line - &diff.front()) << "'";
        out.push_back(line.value);
        iter++;
        break;
      default:
        // This should hopefully be unreachable.
        ASSERT_TRUE(false);
    }
  }

  // Now we've constructed what *should* be a copy of `target` but we want to verify
  // that it in fact *is* exactly target.
  ASSERT_EQUAL(out.size(), target.size());
  auto target_iter = target.begin();
  for (const auto& out_line : out) {
    EXPECT_EQUAL(*target_iter++, out_line);
  }
}

TEST(diff_empty) {
  std::vector<int> a = {};
  std::vector<int> b = {};
  auto diff = wcl::diff<int>(a.begin(), a.end(), b.begin(), b.end());
  TEST_FUNC_CALL(verify_diff, diff, a, b);
  EXPECT_EQUAL(size_t(0), diff.size());
}

TEST(diff_unit_same) {
  std::vector<int> a = {1};
  std::vector<int> b = {1};
  auto diff = wcl::diff<int>(a.begin(), a.end(), b.begin(), b.end());
  TEST_FUNC_CALL(verify_diff, diff, a, b);
  EXPECT_EQUAL(size_t(1), diff.size());
}

TEST(diff_unit_diff) {
  std::vector<int> a = {1};
  std::vector<int> b = {2};
  auto diff = wcl::diff<int>(a.begin(), a.end(), b.begin(), b.end());
  TEST_FUNC_CALL(verify_diff, diff, a, b);
  EXPECT_EQUAL(size_t(2), diff.size());
}

TEST(diff_id) {
  std::vector<int> a = {1, 2, 3};
  std::vector<int> b = {1, 2, 3};
  auto diff = wcl::diff<int>(a.begin(), a.end(), b.begin(), b.end());
  TEST_FUNC_CALL(verify_diff, diff, a, b);
  ASSERT_EQUAL(size_t(3), diff.size());
  EXPECT_EQUAL(wcl::diff_type_t::Keep, diff[0].type);
  EXPECT_EQUAL(1, diff[0].value);
  EXPECT_EQUAL(wcl::diff_type_t::Keep, diff[1].type);
  EXPECT_EQUAL(2, diff[1].value);
  EXPECT_EQUAL(wcl::diff_type_t::Keep, diff[2].type);
  EXPECT_EQUAL(3, diff[2].value);
}

TEST(diff_add) {
  std::vector<int> a = {1, 3};
  std::vector<int> b = {1, 2, 3};
  auto diff = wcl::diff<int>(a.begin(), a.end(), b.begin(), b.end());
  TEST_FUNC_CALL(verify_diff, diff, a, b);
  EXPECT_EQUAL(wcl::diff_type_t::Keep, diff[0].type);
  EXPECT_EQUAL(1, diff[0].value);
  EXPECT_EQUAL(wcl::diff_type_t::Add, diff[1].type);
  EXPECT_EQUAL(2, diff[1].value);
  EXPECT_EQUAL(wcl::diff_type_t::Keep, diff[2].type);
  EXPECT_EQUAL(3, diff[2].value);
}

TEST(diff_sub) {
  std::vector<int> a = {1, 2, 3};
  std::vector<int> b = {1, 3};
  auto diff = wcl::diff<int>(a.begin(), a.end(), b.begin(), b.end());
  TEST_FUNC_CALL(verify_diff, diff, a, b);
  EXPECT_EQUAL(diff[0].type, wcl::diff_type_t::Keep);
  EXPECT_EQUAL(diff[0].value, 1);
  EXPECT_EQUAL(diff[1].type, wcl::diff_type_t::Sub);
  EXPECT_EQUAL(diff[1].value, 2);
  EXPECT_EQUAL(diff[2].type, wcl::diff_type_t::Keep);
  EXPECT_EQUAL(diff[2].value, 3);
}

TEST(diff_permute) {
  // There are multiple valid answers here so we rely on `verify_diff`
  // This is just a small test to ensure that everything is working togethor
  // before we move on.
  std::vector<int> a = {1, 2, 3, 4, 5};
  std::vector<int> b = {1, 3, 4, 2, 5};
  auto diff = wcl::diff<int>(a.begin(), a.end(), b.begin(), b.end());
  TEST_FUNC_CALL(verify_diff, diff, a, b);
}

// This fuzzer randomly drops things
// from small sequences. This ensures a
// good mix of subtractions, additions, and keeps.
// The length is kind of a bell curve however since
// both a length of 0 and the full length are exponetially
// unlikely. So we sweep the full length over a range to
// ensure good coverage of many situations.
TEST(diff_fuzz1) {
  // seed the rng for fuzzing
  uint64_t seedV = 0xdeadbeefdeadbeef;
  std::tuple<uint64_t, uint64_t, uint64_t, uint64_t> seed = {seedV, seedV, seedV, seedV};
  wcl::xoshiro_256 rng(seed);
  std::uniform_int_distribution<int> coin(0, 1);

  // So for each length between 1 and 10 inclusive, we do 100
  // diff tests.
  for (size_t seq_length = 1; seq_length <= 10; ++seq_length) {
    for (int i = 0; i < 100; ++i) {
      std::vector<int> a;
      std::vector<int> b;
      for (size_t j = 0; j < seq_length; ++j) {
        if (coin(rng)) a.push_back(j);
        if (coin(rng)) b.push_back(j);
      }

      auto diff = wcl::diff<int>(a.begin(), a.end(), b.begin(), b.end());
      TEST_FUNC_CALL(verify_diff, diff, a, b);
    }
  }
}

// In this test we use uniform random sampling
// of sequences of small integers. This is a much
// higher entropy style of fuzzing that is less
// likely in each case to hit any given edge case
// but it has some probability of hitting every case.
TEST(diff_fuzz2) {
  // seed the rng for fuzzing
  uint64_t seedV = 0xdeadbeefdeadbeef;
  std::tuple<uint64_t, uint64_t, uint64_t, uint64_t> seed = {seedV, seedV, seedV, seedV};
  wcl::xoshiro_256 rng(seed);
  std::uniform_int_distribution<int> length(2, 10);
  std::uniform_int_distribution<int> values(0, 10);

  for (int i = 0; i < 10000; ++i) {
    std::vector<int> a;
    std::vector<int> b;
    int alen = length(rng);
    int blen = length(rng);
    for (int j = 0; j < alen; ++j) a.push_back(values(rng));
    for (int j = 0; j < blen; ++j) b.push_back(values(rng));

    auto diff = wcl::diff<int>(a.begin(), a.end(), b.begin(), b.end());
    TEST_FUNC_CALL(verify_diff, diff, a, b);
  }
}

// In regular fuzz2 we test a lots of small cases.
// In this case we want to test a small number of
// large cases. This gives us an idea of efficency but
// also increases the probability of finding a snag.
// This also doubles as a santity check to ensure that
// we aren't doing too bad on performance.
TEST(diff_fuzz2_large, "large") {
  // seed the rng for fuzzing
  uint64_t seedV = 0xdeadbeefdeadbeef;
  std::tuple<uint64_t, uint64_t, uint64_t, uint64_t> seed = {seedV, seedV, seedV, seedV};
  wcl::xoshiro_256 rng(seed);
  std::uniform_int_distribution<int> length(300, 1500);
  std::uniform_int_distribution<int> values(0, 20);

  for (int i = 0; i < 10; ++i) {
    std::vector<int> a;
    std::vector<int> b;
    int alen = length(rng);
    int blen = length(rng);
    for (int j = 0; j < alen; ++j) a.push_back(values(rng));
    for (int j = 0; j < blen; ++j) b.push_back(values(rng));

    auto diff = wcl::diff<int>(a.begin(), a.end(), b.begin(), b.end());
    TEST_FUNC_CALL(verify_diff, diff, a, b);
  }
}

// In fuzz1 we did not test for duplicates, in fuzz2 the odds of
// the two sequences looking related at all is very low. In order to
// get a good distribution of "related" sequences we generate a random
// input and then perform a random number of mutations to it.
TEST(diff_fuzz3) {
  // seed the rng for fuzzing
  uint64_t seedV = 0xdeadbeefdeadbeef;
  std::tuple<uint64_t, uint64_t, uint64_t, uint64_t> seed = {seedV, seedV, seedV, seedV};
  wcl::xoshiro_256 rng(seed);
  std::uniform_int_distribution<int> length(2, 10);
  std::uniform_int_distribution<int> num_mutations(1, 5);
  std::uniform_int_distribution<int> mutation_select(0, 3);
  std::uniform_int_distribution<int> values(-10, 10);

  for (int i = 0; i < 10000; ++i) {
    std::vector<int> a;
    std::vector<int> b;

    // Generate a
    int alen = length(rng);
    for (int j = 0; j < alen; ++j) a.push_back(values(rng));

    // Generate b as a mutation of a
    b = a;
    int muts = num_mutations(rng);
    for (int j = 0; j < muts; ++j) {
      // some mutations are not valid so we just skip them
      std::uniform_int_distribution<int> indexes(0, std::max(int(b.size()) - 1, 0));
      int mutation = mutation_select(rng);
      if (mutation == 0 && b.size()) {
        b[indexes(rng)] += values(rng);
      }
      if (mutation == 1 && b.size()) {
        std::swap(b[indexes(rng)], b[indexes(rng)]);
      }
      if (mutation == 2) {
        b.insert(b.begin() + indexes(rng), values(rng));
      }
      if (mutation == 3 && b.size()) {
        b.erase(b.begin() + indexes(rng));
      }
    }

    auto diff = wcl::diff<int>(a.begin(), a.end(), b.begin(), b.end());
    TEST_FUNC_CALL(verify_diff, diff, a, b);
  }
}
