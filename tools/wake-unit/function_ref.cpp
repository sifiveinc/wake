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

#include <wcl/function_ref.h>

#include <optional>
#include <string>

#include "unit.h"

// Compile-time checks.
static_assert(std::is_trivially_copyable<wcl::function_ref<void()>>::value,
              "function_ref should be trivially copyable");
static_assert(std::is_trivially_copyable<wcl::function_ref<int(int, double)>>::value,
              "function_ref should be trivially copyable");
static_assert(sizeof(wcl::function_ref<void()>) == 2 * sizeof(void*),
              "function_ref should be exactly 2 pointers");
static_assert(sizeof(wcl::function_ref<int(int, double, std::string)>) == 2 * sizeof(void*),
              "function_ref should be exactly 2 pointers");

// Test helper functions
static int add_one(int x) { return x + 1; }
static int multiply_by_two(int x) { return x * 2; }
static std::optional<std::string> return_nullopt(const std::string&) { return std::nullopt; }
static std::optional<std::string> return_error(const std::string& s) { return "error: " + s; }

// Test basic construction and invocation
TEST(function_ref_lambda_capture) {
  int counter = 0;
  auto lambda = [&counter]() { return ++counter; };
  wcl::function_ref<int()> ref = lambda;

  EXPECT_EQUAL(1, ref());
  EXPECT_EQUAL(2, ref());
  EXPECT_EQUAL(2, counter);
}

TEST(function_ref_const_lambda) {
  const auto lambda = [](int x) { return x * 3; };
  wcl::function_ref<int(int)> ref = lambda;

  EXPECT_EQUAL(15, ref(5));
}

TEST(function_ref_function_pointer) {
  wcl::function_ref<int(int)> ref = add_one;

  EXPECT_EQUAL(43, ref(42));
  EXPECT_EQUAL(101, ref(100));
}

TEST(function_ref_function_reference) {
  // Functions decay to pointers
  wcl::function_ref<int(int)> ref = multiply_by_two;

  EXPECT_EQUAL(10, ref(5));
}

TEST(function_ref_copy_constructor) {
  auto lambda = [](int x) { return x + 10; };
  wcl::function_ref<int(int)> ref1 = lambda;
  wcl::function_ref<int(int)> ref2 = ref1;

  EXPECT_EQUAL(15, ref1(5));
  EXPECT_EQUAL(15, ref2(5));
}

TEST(function_ref_copy_assignment) {
  auto lambda1 = [](int x) { return x + 1; };
  auto lambda2 = [](int x) { return x + 2; };

  wcl::function_ref<int(int)> ref1 = lambda1;
  wcl::function_ref<int(int)> ref2 = lambda2;

  EXPECT_EQUAL(11, ref1(10));
  EXPECT_EQUAL(12, ref2(10));

  ref2 = ref1;  // Copy assign

  EXPECT_EQUAL(11, ref2(10));  // Now points to lambda1
}

TEST(function_ref_return_conversion) {
  // int return type converts to long
  auto lambda = [](int x) { return x + 1; };
  wcl::function_ref<long(int)> ref = lambda;

  long result = ref(41);
  EXPECT_EQUAL(42L, result);
}

TEST(function_ref_optional_return) {
  wcl::function_ref<std::optional<std::string>(const std::string&)> ref1 = return_nullopt;
  wcl::function_ref<std::optional<std::string>(const std::string&)> ref2 = return_error;

  auto result1 = ref1("test");
  EXPECT_FALSE(result1.has_value());

  auto result2 = ref2("test");
  ASSERT_TRUE(result2.has_value());
  EXPECT_EQUAL("error: test", *result2);
}

TEST(function_ref_functor) {
  struct Adder {
    int value;
    int operator()(int x) const { return x + value; }
  };

  Adder add5{5};
  wcl::function_ref<int(int)> ref = add5;

  EXPECT_EQUAL(15, ref(10));
}

TEST(function_ref_mutable_lambda) {
  auto lambda = [x = 0]() mutable { return ++x; };
  wcl::function_ref<int()> ref = lambda;

  EXPECT_EQUAL(1, ref());
  EXPECT_EQUAL(2, ref());
  EXPECT_EQUAL(3, ref());
}

TEST(function_ref_stateless_lambda) {
  auto lambda = [](int x) { return x * x; };
  wcl::function_ref<int(int)> ref = lambda;

  EXPECT_EQUAL(16, ref(4));
  EXPECT_EQUAL(25, ref(5));
}

// Test multiple parameters
TEST(function_ref_multiple_params) {
  auto lambda = [](int a, int b, int c) { return a + b + c; };
  wcl::function_ref<int(int, int, int)> ref = lambda;

  EXPECT_EQUAL(6, ref(1, 2, 3));
  EXPECT_EQUAL(15, ref(4, 5, 6));
}

// Test void return type
TEST(function_ref_void_return) {
  int counter = 0;
  auto lambda = [&counter](int x) { counter += x; };
  wcl::function_ref<void(int)> ref = lambda;

  ref(5);
  EXPECT_EQUAL(5, counter);
  ref(10);
  EXPECT_EQUAL(15, counter);
}
