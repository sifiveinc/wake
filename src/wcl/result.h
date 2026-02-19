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

#pragma once

#include <errno.h>

#include <cstdint>
#include <optional>
#include <utility>

namespace wcl {

// This is a helpful rename of `int`
// to indicate that the error type is
// a posix error value as returned by
// errno from some internal function.
using posix_error_t = int;

// this helps us tell apart the error and non-error case.
class in_place_error_t {};

static constexpr uint64_t max(uint64_t a, uint64_t b) { return (a < b) ? b : a; }

template <class T, class E, bool copy>
class result_base {};

// wcl::result demands that the error and value be movable
// 1) a non-movable value is essentially never useful because
//    it cannot be returned except by out parameter which is
//    the whole pattern we're trying to avoid.
// 2) there is no "default" error or value so default constructing
//    a `result` doesn't make much sense and thus we need a valid
//    semantics for moving a `result` for both underlying values.

// furthermore, a `result` is only copyable if both the error and
// value type are copyable because otherwise we would be missing
// a valid copy constructor for one of them!

// This is the copyable case which would be used for things like
// std::string, int, std::vector, etc... this would not be used for
// std::unique_ptr however.
// NOTE: On both of these we had to set the alignment to the max alignment
//       of the two values.
template <class T, class E>
class alignas(max(alignof(T), alignof(E))) result_base<T, E, true> {
 protected:
  union {
    T value;
    E error_;
  };
  bool is_error = true;

 public:
  ~result_base() {
    if (is_error) {
      error_.~E();
    } else {
      value.~T();
    }
  }
  result_base() = delete;
  result_base(const result_base& other) {
    is_error = other.is_error;
    if (is_error) {
      new (&error_) E(other.error_);
    } else {
      new (&value) T(other.value);
    }
  }

  result_base(result_base&& other) {
    is_error = other.is_error;
    // NOTE: On move we just use the move constructors
    // of each value and we don't call their destructors yet.
    // So if you move a result, it will keep its errorness
    // but the underlying value may be changed. This matches
    // the behavior of std::optional.
    if (is_error) {
      new (&error_) E(std::move(other.error_));
    } else {
      new (&value) T(std::move(other.value));
    }
  }

  result_base& operator=(const result_base& other) {
    if (is_error) {
      error_.~E();
    } else {
      value.~T();
    }

    is_error = other.is_error;
    if (other.is_error) {
      new (&error_) E(other.error_);
    } else {
      new (&value) T(other.value);
    }

    return *this;
  }

  result_base& operator=(result_base&& other) {
    if (is_error) {
      error_.~E();
    } else {
      value.~T();
    }

    is_error = other.is_error;
    if (other.is_error) {
      new (&error_) E(std::move(other.error_));
    } else {
      new (&value) T(std::move(other.value));
    }

    return *this;
  }

  template <class... Args>
  result_base(std::in_place_t, Args&&... args)
      : value(std::forward<Args>(args)...), is_error(false) {}

  template <class... Args>
  result_base(in_place_error_t, Args&&... args)
      : error_(std::forward<Args>(args)...), is_error(true) {}
};

// Same reasons as above but here we don't require copyable
// errors or values. This is very useful for things like
// std::unique_ptr
template <class T, class E>
class alignas(max(alignof(T), alignof(E))) result_base<T, E, false> {
 protected:
  union {
    T value;
    E error_;
  };
  bool is_error = true;

 public:
  ~result_base() {
    if (is_error) {
      error_.~E();
    } else {
      value.~T();
    }
  }
  result_base() = delete;
  result_base(const result_base& other) = delete;
  result_base(result_base&& other) {
    is_error = other.is_error;
    // on move we just use the move constructors
    // of each value and we don't call their destructors yet.
    // so if you move a result, it will keeps its errorness
    // but the underlying value may be changed.
    if (is_error) {
      new (&error_) E(std::move(other.error_));
    } else {
      new (&value) T(std::move(other.value));
    }
  }

  result_base& operator=(const result_base& other) = delete;

  result_base& operator=(result_base&& other) {
    if (is_error) {
      error_.~E();
    } else {
      value.~T();
    }

    is_error = other.is_error;
    if (other.is_error) {
      new (&error_) E(std::move(other.error_));
    } else {
      new (&value) T(std::move(other.value));
    }
  }

  template <class... Args>
  result_base(std::in_place_t, Args&&... args)
      : value(std::forward<Args>(args)...), is_error(false) {}

  template <class... Args>
  result_base(in_place_error_t, Args&&... args)
      : error_(std::forward<Args>(args)...), is_error(true) {}
};

template <class T, class E>
using result_base_t =
    result_base<T, E, std::is_copy_constructible<T>::value && std::is_copy_constructible<E>::value>;

// Now we make the actual wrapper class that gives us our final public interface
template <class T, class E>
class result : public result_base_t<T, E> {
 public:
  template <class... Args>
  result(std::in_place_t x, Args&&... args) : result_base_t<T, E>(x, std::forward<Args>(args)...) {}
  template <class... Args>
  result(in_place_error_t x, Args&&... args)
      : result_base_t<T, E>(x, std::forward<Args>(args)...) {}

  result() = default;
  result(const result&) = default;
  result(result&&) = default;
  ~result() = default;

  result& operator=(const result&) = default;
  result& operator=(result&&) = default;

  explicit operator bool() const { return !this->is_error; }

  T& operator*() { return this->value; }

  const T& operator*() const { return this->value; }

  T* operator->() { return &this->value; }

  const T* operator->() const { return &this->value; }

  E& error() { return this->error_; }

  const E& error() const { return this->error_; }
};

// Creates a result value from an existing object. Does not
// require the value type which can be infered, but the error
// type must be specified. result_value<posix_error_t>("foo")
// would have type wcl::result<const char *, posix_error_t>
// for instance.
template <class E, class T>
result<T, E> result_value(T&& x) {
  return result<T, E>{std::in_place, std::forward<T>(x)};
}

// `make_result` is the more general sibling of `result_value`
// that allows you to construct the value in place using any
// constructor. Both types must be specified when using this.
template <class T, class E, class... Args>
result<T, E> make_result(Args&&... args) {
  return result<T, E>{std::in_place, std::forward<Args>(args)...};
}

// Creates a result error from an existing object. Only requires the
// value type and not the error type which can be infered.
template <class T, class E>
result<T, E> result_error(E&& err) {
  return result<T, E>{in_place_error_t{}, std::forward<E>(err)};
}

// `make_error` is the more general sibling of `result_error`.
template <class T, class E, class... Args>
result<T, E> make_error(Args&&... args) {
  return result<T, E>{in_place_error_t{}, std::forward<Args>(args)...};
}

// It's very common that you just want to wrap errno in a result when
// dealing with posix functions. This just does that for you in a
// single function. You must specify the value type however as that
// cannot be infered.
template <class T>
result<T, posix_error_t> make_errno() {
  return result<T, posix_error_t>{in_place_error_t{}, errno};
}

}  // namespace wcl
