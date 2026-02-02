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

#ifndef PRIM_H
#define PRIM_H

#include <map>
#include <string>
#include <vector>

#include "tuple.h"
#include "types/primfn.h"

struct String;
struct Integer;
struct Double;
struct RegExp;
struct Scope;
struct Record;
struct Expr;

/* Macros for handling inputs from wake */
#define RETURN(val)                           \
  do {                                        \
    scope->at(output)->fulfill(runtime, val); \
    return;                                   \
  } while (0)

void require_fail(const char *message, unsigned size, Runtime &runtime, const Scope *scope);

#define STR(x) #x
#define STR2(x) STR(x)
#define REQUIRE(b)                                                                                 \
  do {                                                                                             \
    if (!(b)) {                                                                                    \
      const char message[] = "Requirement " STR(b) " failed at " __FILE__ ":" STR2(__LINE__) "\n"; \
      require_fail(message, sizeof(message), runtime, scope);                                      \
      return;                                                                                      \
    }                                                                                              \
  } while (0)

#define EXPECT(num)        \
  do {                     \
    (void)data;            \
    REQUIRE(nargs == num); \
  } while (0)

#define STRING(arg, i)                       \
  do {                                       \
    HeapObject *arg = args[i];               \
    REQUIRE(typeid(*arg) == typeid(String)); \
  } while (0);                               \
  String *arg = static_cast<String *>(args[i]);
#define INTEGER(arg, i)                       \
  do {                                        \
    HeapObject *arg = args[i];                \
    REQUIRE(typeid(*arg) == typeid(Integer)); \
  } while (0);                                \
  Integer *arg = static_cast<Integer *>(args[i]);
#define DOUBLE(arg, i)                       \
  do {                                       \
    HeapObject *arg = args[i];               \
    REQUIRE(typeid(*arg) == typeid(Double)); \
  } while (0);                               \
  Double *arg = static_cast<Double *>(args[i]);
#define REGEXP(arg, i)                       \
  do {                                       \
    HeapObject *arg = args[i];               \
    REQUIRE(typeid(*arg) == typeid(RegExp)); \
  } while (0);                               \
  RegExp *arg = static_cast<RegExp *>(args[i]);
#define CLOSURE(arg, i)                       \
  do {                                        \
    HeapObject *arg = args[i];                \
    REQUIRE(typeid(*arg) == typeid(Closure)); \
  } while (0);                                \
  Closure *arg = static_cast<Closure *>(args[i]);
#define RECORD(arg, i) Record *arg = static_cast<Record *>(args[i]);

#define INTEGER_MPZ(arg, i)                   \
  do {                                        \
    HeapObject *arg = args[i];                \
    REQUIRE(typeid(*arg) == typeid(Integer)); \
  } while (0);                                \
  mpz_t arg = {static_cast<Integer *>(args[i])->wrap()};

/* Useful expressions for primitives */
Value *alloc_order(Heap &h, int x);
Value *alloc_nil(Heap &h);
inline size_t reserve_unit() { return Record::reserve(0); }
inline size_t reserve_bool() { return Record::reserve(0); }
inline size_t reserve_order() { return Record::reserve(0); }
inline size_t reserve_tuple2() { return Record::reserve(2); }
inline size_t reserve_result() { return Record::reserve(1); }
inline size_t reserve_list(size_t elements) {
  return Record::reserve(2) * elements + Record::reserve(0);
}
Value *claim_unit(Heap &h);
Value *claim_bool(Heap &h, bool x);
Value *claim_tuple2(Heap &h, Value *first, Value *second);
Value *claim_result(Heap &h, bool ok, Value *value);
Value *claim_list(Heap &h, size_t elements, Value **values);

size_t reserve_hash();
Work *claim_hash(Heap &h, Value *value, Continuation *continuation);

void dont_report_future_targets();

struct JobTable;
class CASContext;

struct StringInfo {
  bool verbose;
  bool debug;
  bool quiet;
  std::string version;
  std::string wake_cwd;
  char **cmdline;
  StringInfo(bool v, bool d, bool q, const std::string &version_, const std::string &wake_cwd_,
             char **cmdline_)
      : verbose(v), debug(d), quiet(q), version(version_), wake_cwd(wake_cwd_), cmdline(cmdline_) {}
};

void prim_register_string(PrimMap &pmap, StringInfo *info);
void prim_register_vector(PrimMap &pmap);
void prim_register_integer(PrimMap &pmap);
void prim_register_double(PrimMap &pmap);
void prim_register_exception(PrimMap &pmap);
void prim_register_regexp(PrimMap &pmap);
void prim_register_target(PrimMap &pmap);
void prim_register_json(PrimMap &pmap);
void prim_register_job(JobTable *jobtable, PrimMap &pmap);
void prim_register_sources(PrimMap &pmap);

PrimMap prim_register_all(StringInfo *info, JobTable *jobtable, CASContext *cas_ctx);

#endif
