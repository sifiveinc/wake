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

#ifndef PRIMFN_H
#define PRIMFN_H

#include <map>
#include <memory>
#include <vector>

struct Scope;
struct Value;
struct Runtime;
struct TypeVar;

typedef bool (*PrimType)(const std::vector<TypeVar *> &args, TypeVar *out);
typedef void (*PrimFn)(void *data, Runtime &runtime, Scope *scope, size_t output, size_t nargs,
                       Value **args);
#define PRIMTYPE(name) bool name(const std::vector<TypeVar *> &args, TypeVar *out)
#define PRIMFN(name) \
  void name(void *data, Runtime &runtime, Scope *scope, size_t output, size_t nargs, Value **args)

/* The evaluation order of wake makes two guarantees:
 *   [1] Exactly the effects of straight-line execution are produced.
 *   [2] If value A is needed to evaluate B, A happens before B.
 *
 * This means that the order of effects is only defined if one depends
 * on the value produced by the other. However, there is some subtlety;
 * some effects depend on effects by virtue of being invoked or not.
 *
 * def a = <some-effect>
 * def b = a + 1
 * def c = <some-effect-producing-function-whose-effect-dependings-its-1st-argument> b
 * Clearly, 'c' depends on 'a' and will run after it.
 *
 * def a = <some-effect>
 * def c = if a then <some-effect> else Nil
 * In this case, while the second effect does not directly depend on 'a',
 * it's invocation depends on 'a'; therefore, it will run after it.
 *
 * def a = <some-file-producing-effect>
 * def c = if a then <enumerate-files> else Nil
 * In this case, while 'c' has no effects, the enumeration step must be
 * evaluated after 'a'; therefore, the new files will be detected.
 */

/* Function only depends on it's arguments and has no effects.
 * Allow: all optimizations
 */
#define PRIM_PURE 0

/* Observes location in the happens-before stream (beyond it's arguments).
 * May not be moved earlier in the dependency tree (ie: up the AST).
 * Enumerating files or the stack are examples in this category.
 * Allow:   deadcode elimination (DE), lowering to uses (LTU), Inlining
 * Forbid:  loop invariant lifting (LVL), common sub-expression elimination (CSE)
 * Unclear: duplicating
 */
#define PRIM_ORDERED 1

/* Produces something visible outside wake.
 * Number of invocations must remain unchanged.
 * Implies PRIM_ORDERED (use PRIM_IMPURE when setting)
 * Allow:  Inlining
 * Forbid: LVL, CSE, DE, LTU
 */
#define PRIM_EFFECT 2

#define PRIM_IMPURE (PRIM_EFFECT | PRIM_ORDERED)

/* This primitive has a function argument which it will invoke.
 * The status of the primitive depends on that argument.
 */
#define PRIM_FNARG 4

/* This primitive is partial: it may abort the runtime (via require_fail) on some well-typed
 * inputs -- e.g. vget out of bounds, integer division by zero.  Such a primitive is NOT safe
 * to speculatively execute, so loop-invariant code motion (LVL, see src/optimizer/lvl.cpp)
 * must not hoist it out of a lambda that might be invoked zero times or only conditionally:
 * doing so could turn a never-taken abort into an actual abort.  Purity (PRIM_PURE) is thus
 * necessary but NOT sufficient for hoisting -- a hoistable pure prim must also be total.
 * Set this on any PURE primitive whose body can REQUIRE()-fail for a non-type/non-arity
 * reason.  (Effectful/ordered prims are already excluded from hoisting by their own flags.)
 */
#define PRIM_PARTIAL 8

/* Register primitive functions */
struct PrimDesc {
  PrimFn fn;
  PrimType type;
  int flags;
  void *data;

  PrimDesc(PrimFn fn_, PrimType type_, int flags_, void *data_ = 0)
      : fn(fn_), type(type_), flags(flags_), data(data_) {}
};

typedef std::map<std::string, PrimDesc> PrimMap;
void prim_register(PrimMap &pmap, const char *key, PrimFn fn, PrimType type, int flags,
                   void *data = 0);

#endif
