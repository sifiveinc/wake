/*
 * Copyright 2023 SiFive, Inc.
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

#include <wcl/result.h>

#include <algorithm>
#include <memory>
#include <random>

#include "unit.h"

TEST(result_err_is_err) {
  wcl::result<int, int> err = wcl::result_error<int>(10);
  EXPECT_FALSE((bool)err);
  EXPECT_EQUAL(10, err.error());
}

TEST(result_value) {
  wcl::result<int, int> value = wcl::result_value<int>(10);
  ASSERT_TRUE((bool)value);
  EXPECT_EQUAL(10, *value);
}

TEST(result_inplace_value) {
  wcl::result<std::pair<int, int>, int> pair = wcl::make_result<std::pair<int, int>, int>(10, 10);
  ASSERT_TRUE((bool)pair);
  EXPECT_EQUAL(std::make_pair(10, 10), *pair);
}

TEST(result_inplace_error) {
  wcl::result<int, std::pair<int, int>> pair = wcl::make_error<int, std::pair<int, int>>(10, 10);
  ASSERT_FALSE((bool)pair);
  EXPECT_EQUAL(std::make_pair(10, 10), pair.error());
}

TEST(result_copy) {
  wcl::result<int, int> err1 = wcl::result_error<int>(10);
  wcl::result<int, int> err2(err1);
  EXPECT_FALSE((bool)err1);
  EXPECT_FALSE((bool)err2);
  EXPECT_EQUAL(10, err1.error());
  EXPECT_EQUAL(10, err2.error());

  wcl::result<int, int> value1 = wcl::result_value<int>(10);
  wcl::result<int, int> value2(value1);
  ASSERT_TRUE((bool)value1);
  EXPECT_EQUAL(10, *value1);
  ASSERT_TRUE((bool)value2);
  EXPECT_EQUAL(10, *value2);
}

TEST(result_move) {
  wcl::result<int, int> err1 = wcl::result_error<int>(10);
  wcl::result<int, int> err2(std::move(err1));
  EXPECT_FALSE((bool)err1);
  EXPECT_FALSE((bool)err2);
  EXPECT_EQUAL(10, err2.error());

  wcl::result<int, int> value1 = wcl::result_value<int>(10);
  wcl::result<int, int> value2(std::move(value1));
  ASSERT_TRUE((bool)value1);
  ASSERT_TRUE((bool)value2);
  EXPECT_EQUAL(10, *value2);

  // We want to make sure we can make result move only things.
  {
    wcl::result<std::unique_ptr<int>, int> move_only1(std::in_place, std::make_unique<int>(10));
    wcl::result<std::unique_ptr<int>, int> move_only2(std::move(move_only1));
    EXPECT_TRUE((bool)move_only1);
    ASSERT_TRUE((bool)move_only2);
    EXPECT_EQUAL(10, *move_only2->get());
    EXPECT_FALSE(move_only2->get() == nullptr);
    EXPECT_FALSE((*move_only2).get() == nullptr);
    EXPECT_EQUAL(nullptr, move_only1->get());
    EXPECT_EQUAL(nullptr, (*move_only1).get());
    const auto& ref = move_only2;
    EXPECT_FALSE(ref->get() == nullptr);
    EXPECT_FALSE((*ref).get() == nullptr);
    const auto& ref2 = move_only1;
    EXPECT_TRUE(ref2->get() == nullptr);
    EXPECT_TRUE((*ref2).get() == nullptr);
  }

  // We also want to know the same for errors
  {
    wcl::result<int, std::unique_ptr<int>> move_only1(wcl::in_place_error_t{},
                                                      std::make_unique<int>(10));
    wcl::result<int, std::unique_ptr<int>> move_only2(std::move(move_only1));
    EXPECT_FALSE((bool)move_only1);
    EXPECT_FALSE((bool)move_only2);
    EXPECT_EQUAL(10, *move_only2.error().get());
    EXPECT_FALSE(move_only2.error().get() == nullptr);
    EXPECT_TRUE(move_only1.error().get() == nullptr);
    const auto& ref = move_only2;
    EXPECT_FALSE(ref.error().get() == nullptr);
    EXPECT_FALSE(ref.error().get() == nullptr);
  }
}

TEST(result_forward_value) {
  wcl::result<std::pair<int, int>, int> opair(std::in_place, 10, 10);
  EXPECT_TRUE((bool)opair);
  EXPECT_EQUAL(std::make_pair(10, 10), *opair);
}

TEST(result_forward_error) {
  wcl::result<int, std::pair<int, int>> opair(wcl::in_place_error_t{}, 10, 10);
  EXPECT_FALSE((bool)opair);
  EXPECT_EQUAL(std::make_pair(10, 10), opair.error());
}

struct NoCopy {
  NoCopy() = delete;
  NoCopy(const NoCopy&) = delete;
  NoCopy(NoCopy&&) = default;
  ~NoCopy() {
    std::cerr << "You weren't ever supposed to be able to construct such a thing!!!" << std::endl;
    exit(1);
  }
};

TEST(result_no_construct) {
  wcl::result<NoCopy, int> error = wcl::result_error<NoCopy>(10);
  wcl::result<int, NoCopy> value = wcl::result_value<NoCopy>(10);
  EXPECT_TRUE((bool)value);
  EXPECT_FALSE((bool)error);
}

struct SetOnDestruct {
  const char*& msg;
  const char* on_destruct = nullptr;

  SetOnDestruct() = delete;
  SetOnDestruct(const char*& msg, const char* on_destruct) : msg(msg), on_destruct(on_destruct) {}

  ~SetOnDestruct() { msg = on_destruct; }
};

class ConstructDestructCount {
 private:
  int* count = nullptr;

 public:
  ConstructDestructCount() = default;
  ConstructDestructCount(int& count) : count(&count) { ++*this->count; }
  ConstructDestructCount(const ConstructDestructCount& other) : count(other.count) {
    ++*this->count;
  }
  ConstructDestructCount(ConstructDestructCount&& other) : count(other.count) {
    other.count = nullptr;
  }
  ConstructDestructCount& operator=(const ConstructDestructCount& other) {
    if (count) --*count;
    count = other.count;
    ++*count;
    return *this;
  }
  ConstructDestructCount& operator=(ConstructDestructCount&& other) {
    if (count) --*count;
    count = other.count;
    other.count = nullptr;
    return *this;
  }
  ~ConstructDestructCount() {
    if (count) --*count;
  }
  void swap(ConstructDestructCount& other) { std::swap(count, other.count); }
};

TEST(result_destructs) {
  const char* msg;
  const char* expected = "this is the expected string";
  int counter = 0;

  // First some really basic tests
  {
    wcl::result<SetOnDestruct, int> msg_setter(std::in_place, msg, expected);
    wcl::result<ConstructDestructCount, int> ocounter(std::in_place, counter);
  }
  EXPECT_EQUAL(msg, expected);
  ASSERT_EQUAL(0, counter);

  {
    wcl::result<int, SetOnDestruct> msg_setter(wcl::in_place_error_t{}, msg, expected);
    wcl::result<int, ConstructDestructCount> ocounter(wcl::in_place_error_t{}, counter);
  }
  EXPECT_EQUAL(msg, expected);
  ASSERT_EQUAL(0, counter);

  // Now some basic assignment tests.
  {
    wcl::result<ConstructDestructCount, int> counter1(std::in_place, counter);
    EXPECT_EQUAL(1, counter);
    wcl::result<ConstructDestructCount, int> counter2(wcl::in_place_error_t{}, 10);
    EXPECT_EQUAL(1, counter);
    counter2 = counter1;
    EXPECT_EQUAL(2, counter);
    EXPECT_TRUE((bool)counter2);
    EXPECT_TRUE((bool)counter1);
  }
  ASSERT_EQUAL(0, counter);
  {
    wcl::result<ConstructDestructCount, int> counter1(std::in_place, counter);
    EXPECT_EQUAL(1, counter);
    wcl::result<ConstructDestructCount, int> counter2(wcl::in_place_error_t{}, 10);
    EXPECT_EQUAL(1, counter);
    counter2 = std::move(counter1);
    EXPECT_EQUAL(1, counter);
    EXPECT_TRUE((bool)counter2);
    EXPECT_TRUE((bool)counter1);
  }
  ASSERT_EQUAL(0, counter);

  // Now do something complicated to really stress the counter.
  {
    std::vector<wcl::result<ConstructDestructCount, int>> counters;
    for (int i = 0; i < 1000; ++i) {
      counters.emplace_back(std::in_place, counter);
    }
    EXPECT_EQUAL(1000, counter);
    std::mt19937 gen;
    for (int i = 0; i < 10; ++i) std::shuffle(counters.begin(), counters.end(), gen);
    EXPECT_EQUAL(1000, counter);
  }
  ASSERT_EQUAL(0, counter);
}

TEST(result_assign1) {
  // Basic copy
  wcl::result<int, int> some1(std::in_place, 10);
  wcl::result<int, int> some2(wcl::in_place_error_t{}, 10);
  EXPECT_FALSE((bool)some2);
  some2 = some1;
  ASSERT_TRUE((bool)some1);
  ASSERT_TRUE((bool)some2);
  EXPECT_EQUAL(*some1, *some2);
}

TEST(result_assign2) {
  // Basic copy
  int counter = 0;
  {
    wcl::result<ConstructDestructCount, int> some1(std::in_place, counter);
    wcl::result<ConstructDestructCount, int> some2(wcl::in_place_error_t{}, 10);

    EXPECT_TRUE((bool)some1);
    EXPECT_FALSE((bool)some2);
    EXPECT_EQUAL(1, counter);

    some2 = some1;
    EXPECT_TRUE((bool)some1);
    EXPECT_TRUE((bool)some2);
    EXPECT_EQUAL(2, counter);

    some1 = some2;
    EXPECT_TRUE((bool)some1);
    EXPECT_TRUE((bool)some2);
    EXPECT_EQUAL(2, counter);

    some1 = wcl::result_error<ConstructDestructCount>(10);
    EXPECT_TRUE((bool)some2);
    EXPECT_FALSE((bool)some1);
    EXPECT_EQUAL(1, counter);
  }
  EXPECT_EQUAL(0, counter);
}
