/*
 * Copyright 2019 SiFive, Inc.
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

#ifndef VALUE_H
#define VALUE_H

#include <gmp.h>
#include <re2/stringpiece.h>
#include <stdlib.h>

#include <limits>
#include <memory>
#include <string>
#include <vector>

#include "gc.h"

namespace re2 {
class RE2;
}  // namespace re2

#define APP_PRECEDENCE 14

// typeid().hash_code() changes between runs
#define TYPE_STRING 1
#define TYPE_INTEGER 2
#define TYPE_DOUBLE 3
#define TYPE_REGEXP 4
#define TYPE_JOB 5
#define TYPE_CLOSURE 6
#define TYPE_RECORD 7
#define TYPE_SCOPE 8
#define TYPE_TARGET 9

/* Values */

struct RFun;
struct Scope;
struct TypeVar;

struct FormatEntry {
  const HeapObject *value;
  int precedence;
  int state;
  FormatEntry(const HeapObject *value_ = nullptr, int precedence_ = 0, int state_ = 0)
      : value(value_), precedence(precedence_), state(state_) {}
};

struct FormatState {
  std::vector<FormatEntry> stack;
  FormatEntry current;
  bool detailed;
  int indent;  // -1 -> single-line
  void resume();
  void child(const HeapObject *value, int precedence);
  int get() const { return current.state; }
  int p() const { return current.precedence; }
};

struct String final : public GCObject<String, Value> {
  typedef GCObject<String, Value> Parent;

  size_t length;

  String(const char *str, size_t length);
  String(const String &s);

  const char *c_str() const { return static_cast<const char *>(data()); }
  char *c_str() { return static_cast<char *>(data()); }
  std::string as_str() const { return std::string(c_str(), length); }
  size_t size() { return length; }
  bool empty() { return length == 0; }

  int compare(const char *other) const;
  int compare(const char *other_data, size_t other_len) const;
  int compare(const String &other) const { return compare(other.c_str(), other.length); }
  int compare(const std::string &other) const { return compare(other.c_str(), other.size()); }

  template <typename T>
  bool operator<(T &&x) const {
    return compare(std::forward<T>(x)) < 0;
  }
  template <typename T>
  bool operator<=(T &&x) const {
    return compare(std::forward<T>(x)) <= 0;
  }
  template <typename T>
  bool operator==(T &&x) const {
    return compare(std::forward<T>(x)) == 0;
  }
  template <typename T>
  bool operator!=(T &&x) const {
    return compare(std::forward<T>(x)) != 0;
  }
  template <typename T>
  bool operator>=(T &&x) const {
    return compare(std::forward<T>(x)) >= 0;
  }
  template <typename T>
  bool operator>(T &&x) const {
    return compare(std::forward<T>(x)) > 0;
  }

  Hash shallow_hash() const override;
  void format(std::ostream &os, FormatState &state) const override;
  static void cstr_format(std::ostream &os, const char *s, size_t len);

  PadObject *objend() { return Parent::objend() + 1 + length / sizeof(PadObject); }
  static size_t reserve(size_t length) {
    return sizeof(String) / sizeof(PadObject) + 1 + length / sizeof(PadObject);
  }

  static String *claim(Heap &h, size_t length);
  static String *claim(Heap &h, const std::string &str);
  static String *claim(Heap &h, const char *str, size_t length);
  static String *alloc(Heap &h, size_t length);
  static String *alloc(Heap &h, const std::string &str);
  static String *alloc(Heap &h, const char *str);
  static String *alloc(Heap &h, const char *str, size_t length);

  // Never call this during runtime! It can invalidate the heap.
  static RootPointer<String> literal(Heap &h, const std::string &value);

 private:
  explicit String(size_t length_);
};

// An exception-safe wrapper for mpz_t
struct MPZ {
  mpz_t value;
  MPZ() { mpz_init(value); }
  MPZ(long v) { mpz_init_set_si(value, v); }
  MPZ(const std::string &v) { mpz_init_set_str(value, v.c_str(), 0); }
  ~MPZ() { mpz_clear(value); }
  MPZ(const MPZ &x) = delete;
  MPZ &operator=(const MPZ &x) = delete;
};

struct Integer final : public GCObject<Integer, Value> {
  typedef GCObject<Integer, Value> Parent;

  int length;  // abs(length) = number of mp_limb_t in object

  Integer(int length_);
  Integer(const Integer &i);

  std::string str(int base = 10) const;
  void format(std::ostream &os, FormatState &state) const override;
  Hash shallow_hash() const override;

  PadObject *objend() {
    return Parent::objend() +
           (abs(length) * sizeof(mp_limb_t) + sizeof(PadObject) - 1) / sizeof(PadObject);
  }
  static size_t reserve(const MPZ &mpz) {
    return sizeof(Integer) / sizeof(PadObject) +
           (abs(mpz.value[0]._mp_size) * sizeof(mp_limb_t) + sizeof(PadObject) - 1) /
               sizeof(PadObject);
  }

  static Integer *claim(Heap &h, const MPZ &mpz);
  static Integer *alloc(Heap &h, const MPZ &mpz);

  // create a fake mpz_t out of the heap object
  const __mpz_struct wrap() const {
    __mpz_struct out;
    out._mp_size = length;
    out._mp_alloc = abs(length);
    out._mp_d = static_cast<mp_limb_t *>(const_cast<void *>(data()));
    return out;
  }

  // Never call this during runtime! It can invalidate the heap.
  static RootPointer<Integer> literal(Heap &h, const std::string &str);
};

#define FIXED 0
#define SCIENTIFIC 1
#define HEXFLOAT 2
#define DEFAULTFLOAT 3
struct Double final : public GCObject<Double, Value> {
  typedef std::numeric_limits<double> limits;

  double value;

  Double(double value_ = 0) : value(value_) {}
  Double(const char *str) {
    char *end;
    value = strtod(str, &end);
  }

  std::string str(int format = DEFAULTFLOAT, int precision = limits::max_digits10) const;
  void format(std::ostream &os, FormatState &state) const override;
  Hash shallow_hash() const override;

  // Never call this during runtime! It can invalidate the heap.
  static RootPointer<Double> literal(Heap &h, const char *str);
};

struct RegExp final : public GCObject<RegExp, DestroyableObject> {
  typedef GCObject<RegExp, DestroyableObject> Parent;

  std::shared_ptr<re2::RE2> exp;

  RegExp(Heap &h, const re2::StringPiece &regexp);

  void format(std::ostream &os, FormatState &state) const override;
  Hash shallow_hash() const override;

  // Never call this during runtime! It can invalidate the heap.
  static RootPointer<RegExp> literal(Heap &h, const std::string &value);
};

struct Closure final : public GCObject<Closure, Value> {
  RFun *fun;
  size_t applied;
  HeapPointer<Scope> scope;

  Closure(RFun *fun_, size_t applied_, Scope *scope_);
  void format(std::ostream &os, FormatState &state) const override;
  Hash shallow_hash() const override;
  HeapStep explore_escape(HeapStep step);

  template <typename T, T (HeapPointerBase::*memberfn)(T x)>
  T recurse(T arg) {
    arg = HeapObject::recurse<T, memberfn>(arg);
    arg = (scope.*memberfn)(arg);
    return arg;
  }
};

template <>
inline HeapStep Closure::recurse<HeapStep, &HeapPointerBase::explore>(HeapStep step) {
  return explore_escape(step);
}

#endif
