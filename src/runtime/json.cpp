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

// Open Group Base Specifications Issue 7
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include <assert.h>

#include <limits>
#include <sstream>

#include "json/json5.h"
#include "prim.h"
#include "types/data.h"
#include "types/datatype.h"
#include "types/sums.h"
#include "value.h"

typedef std::numeric_limits<double> dlimits;
static double nan() { return dlimits::quiet_NaN(); }
static double inf(char c) { return c == '+' ? dlimits::infinity() : -dlimits::infinity(); }

static size_t measure_jast(const JAST &jast) {
  switch (jast.kind) {
    case JSON_NULLVAL:
      return Record::reserve(0);
    case JSON_TRUE:
      return Record::reserve(1) + reserve_bool();
    case JSON_FALSE:
      return Record::reserve(1) + reserve_bool();
    case JSON_INTEGER:
      return Record::reserve(1) + Integer::reserve(MPZ(jast.value));
    case JSON_DOUBLE:
      return Record::reserve(1) + Double::reserve();
    case JSON_INFINITY:
      return Record::reserve(1) + Double::reserve();
    case JSON_NAN:
      return Record::reserve(1) + Double::reserve();
    case JSON_STR:
      return Record::reserve(1) + String::reserve(jast.value.size());
    case JSON_OBJECT: {
      // JObject wraps a Vector (Pair String JValue)
      // Vector is `Vector (Array a) Integer Integer`, so we need:
      //   Record(JObject) + Record(Vector) + 2*Integer + Record(Array) + each pair
      size_t n = jast.children.size();
      size_t out = Record::reserve(1) + Record::reserve(3) + 2 * Integer::reserve(MPZ((long)n)) +
                   Record::reserve(n);
      for (auto &c : jast.children)
        out += reserve_tuple2() + String::reserve(c.first.size()) + measure_jast(c.second);
      return out;
    }
    case JSON_ARRAY: {
      size_t n = jast.children.size();
      size_t out = Record::reserve(1) + Record::reserve(3) + 2 * Integer::reserve(MPZ((long)n)) +
                   Record::reserve(n);
      for (auto &c : jast.children) out += measure_jast(c.second);
      return out;
    }
    default: {
      assert(0);
      return 0;
    }
  }
}

static Value *getJValue(Heap &h, Value *value, int member) {
  Record *out = Record::claim(h, &JValue->members[member], 1);
  out->at(0)->instant_fulfill(value);
  return out;
}

static Value *convert_jast(Heap &h, const JAST &jast) {
  switch (jast.kind) {
    case JSON_NULLVAL:
      return Record::claim(h, &JValue->members[4], 0);
    case JSON_TRUE:
      return getJValue(h, claim_bool(h, true), 3);
    case JSON_FALSE:
      return getJValue(h, claim_bool(h, false), 3);
    case JSON_INTEGER:
      return getJValue(h, Integer::claim(h, MPZ(jast.value)), 1);
    case JSON_DOUBLE:
      return getJValue(h, Double::claim(h, jast.value.c_str()), 2);
    case JSON_INFINITY:
      return getJValue(h, Double::claim(h, inf(jast.value[0])), 2);
    case JSON_NAN:
      return getJValue(h, Double::claim(h, nan()), 2);
    case JSON_STR:
      return getJValue(h, String::claim(h, jast.value), 0);
    case JSON_OBJECT: {
      size_t n = jast.children.size();
      Record *arr = Record::claim(h, &Constructor::array, n);
      for (size_t i = 0; i < n; ++i) {
        auto &c = jast.children[i];
        arr->at(i)->instant_fulfill(
            claim_tuple2(h, String::claim(h, c.first), convert_jast(h, c.second)));
      }
      Record *vec = Record::claim(h, &Vector->members[0], 3);
      vec->at(0)->instant_fulfill(arr);
      vec->at(1)->instant_fulfill(Integer::claim(h, MPZ((long)0)));
      vec->at(2)->instant_fulfill(Integer::claim(h, MPZ((long)n)));
      return getJValue(h, vec, 5);
    }
    case JSON_ARRAY: {
      size_t n = jast.children.size();
      Record *arr = Record::claim(h, &Constructor::array, n);
      for (size_t i = 0; i < n; ++i) {
        arr->at(i)->instant_fulfill(convert_jast(h, jast.children[i].second));
      }
      Record *vec = Record::claim(h, &Vector->members[0], 3);
      vec->at(0)->instant_fulfill(arr);
      vec->at(1)->instant_fulfill(Integer::claim(h, MPZ((long)0)));
      vec->at(2)->instant_fulfill(Integer::claim(h, MPZ((long)n)));
      return getJValue(h, vec, 6);
    }
    default: {
      assert(0);
      return nullptr;
    }
  }
}

static PRIMTYPE(type_json) {
  TypeVar result;
  Data::typeResult.clone(result);
  result[0].unify(Data::typeJValue);
  result[1].unify(Data::typeString);
  return args.size() == 1 && args[0]->unify(Data::typeString) && out->unify(result);
}

static PRIMFN(prim_json_file) {
  EXPECT(1);
  STRING(file, 0);
  std::stringstream errs;
  JAST jast;
  if (JAST::parse(file->c_str(), errs, jast)) {
    size_t need = measure_jast(jast) + reserve_result();
    runtime.heap.reserve(need);
    RETURN(claim_result(runtime.heap, true, convert_jast(runtime.heap, jast)));
  } else {
    std::string s = errs.str();
    size_t need = String::reserve(s.size()) + reserve_result();
    runtime.heap.reserve(need);
    RETURN(claim_result(runtime.heap, false, String::claim(runtime.heap, s)));
  }
}

static PRIMFN(prim_json_body) {
  EXPECT(1);
  STRING(body, 0);
  std::stringstream errs;
  JAST jast;
  if (JAST::parse(body->c_str(), body->size(), errs, jast)) {
    size_t need = measure_jast(jast) + reserve_result();
    runtime.heap.reserve(need);
    RETURN(claim_result(runtime.heap, true, convert_jast(runtime.heap, jast)));
  } else {
    std::string s = errs.str();
    size_t need = String::reserve(s.size()) + reserve_result();
    runtime.heap.reserve(need);
    RETURN(claim_result(runtime.heap, false, String::claim(runtime.heap, s)));
  }
}

static PRIMTYPE(type_jstr) {
  return args.size() == 1 && args[0]->unify(Data::typeString) && out->unify(Data::typeString);
}

static PRIMFN(prim_json_str) {
  EXPECT(1);
  STRING(str, 0);
  RETURN(String::alloc(runtime.heap, json_escape(str->c_str(), str->size())));
}

void prim_register_json(PrimMap &pmap) {
  // Parsed tree as a persistent constant would be bad.
  prim_register(pmap, "json_file", prim_json_file, type_json, PRIM_ORDERED);
  prim_register(pmap, "json_body", prim_json_body, type_json, PRIM_PURE);
  prim_register(pmap, "json_str", prim_json_str, type_jstr, PRIM_PURE);
}
