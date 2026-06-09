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

#include <cassert>

#include "optimizer/ssa.h"
#include "prim.h"
#include "tuple.h"
#include "types/data.h"
#include "types/datatype.h"
#include "types/type.h"
#include "value.h"

static const TypeVar arrayT("Array@builtin", 1);

static PRIMTYPE(type_vnew) {
  TypeVar vec;
  arrayT.clone(vec);
  return args.size() == 1 && args[0]->unify(Data::typeInteger) && out->unify(vec);
}

static PRIMFN(prim_vnew) {
  EXPECT(1);
  INTEGER_MPZ(arg0, 0);
  REQUIRE(mpz_cmp_si(arg0, 0) >= 0);
  RETURN(Record::alloc(runtime.heap, &Constructor::array, mpz_get_si(arg0)));
}

static PRIMTYPE(type_vget) {
  TypeVar vec;
  arrayT.clone(vec);
  return args.size() == 2 && args[0]->unify(vec) && args[1]->unify(Data::typeInteger) &&
         out->unify(vec[0]);
}

static PRIMFN(prim_vget) {
  EXPECT(2);
  RECORD(vec, 0);
  INTEGER_MPZ(arg1, 1);
  REQUIRE(mpz_cmp_si(arg1, 0) >= 0);
  REQUIRE(mpz_cmp_si(arg1, vec->size()) < 0);

  Promise *p = vec->at(mpz_get_si(arg1));
  if (*p) {
    scope->at(output)->fulfill(runtime, p->coerce<HeapObject>());
  } else {
    runtime.heap.reserve(Tuple::fulfiller_pads);
    p->await(runtime, scope->claim_fulfiller(runtime, output));
  }
}

static PRIMTYPE(type_vset) {
  TypeVar vec;
  arrayT.clone(vec);
  return args.size() == 3 && args[0]->unify(vec) && args[1]->unify(Data::typeInteger) &&
         args[2]->unify(vec[0]) && out->unify(Data::typeUnit);
}

static PRIMFN(prim_vset) {
  EXPECT(3);
  RECORD(vec, 0);
  INTEGER_MPZ(arg1, 1);

  // It's important to allocate before side-effects
  // Failed allocation causes the method to be re-entered
  runtime.heap.reserve(reserve_unit());

  // Getting this wrong means vector.wake is buggy and the heap will crash
  assert(mpz_cmp_si(arg1, 0) >= 0);
  assert(mpz_cmp_si(arg1, vec->size()) < 0);
  Promise *p = vec->at(mpz_get_si(arg1));
  assert(!*p);

  p->fulfill(runtime, args[2]);
  RETURN(claim_unit(runtime.heap));
}

// vfold: (Array a, start, end, fn: b -> a -> b, init: b) -> b
//
// Walks the Array slice [start, end) with a machine-int cursor, threading
// `init` through `fn(acc, elem)` for each element. Avoids the BigInt index
// arithmetic that wake-side `vfoldl` pays per iteration.
//
// Implemented as a self-rescheduling continuation (CVFold) because applying
// a wake closure to two arguments goes through the work-stealing scheduler
// (we cannot run a synchronous C++ for-loop over closure applications).
static const TypeVar fnT(FN, 2);

// Type:  Array a -> Integer -> Integer -> (b -> a -> b) -> b -> b
static PRIMTYPE(type_vfold) {
  TypeVar vec, outer, inner;
  arrayT.clone(vec);
  fnT.clone(outer);
  fnT.clone(inner);
  // outer : acc -> inner       (acc is outer[0])
  // inner : elem -> acc        (elem is inner[0], result is inner[1])
  outer[1].unify(inner);
  inner[0].unify(vec[0]);     // elem == vec's element type
  inner[1].unify(outer[0]);   // inner returns acc

  return args.size() == 5 && args[0]->unify(vec) && args[1]->unify(Data::typeInteger) &&
         args[2]->unify(Data::typeInteger) && args[3]->unify(outer) &&
         args[4]->unify(outer[0]) && out->unify(outer[0]);
}

struct CVFold;

// Helper continuation: awaits an Array slot, then re-enters the parent CVFold
// with the resolved element. Needed because Promise::await overwrites the
// continuation's `value` field, which CVFold uses to hold the accumulator.
struct CVAwait final : public GCObject<CVAwait, Continuation> {
  HeapPointer<CVFold> fold;

  CVAwait(CVFold *fold_) : fold(fold_) {}

  template <typename T, T (HeapPointerBase::*memberfn)(T x)>
  T recurse(T arg) {
    arg = Continuation::recurse<T, memberfn>(arg);
    arg = (fold.*memberfn)(arg);
    return arg;
  }

  void execute(Runtime &runtime) override;
};

// Helper for the curried case (fn shaped as 1-arg-returning-1-arg). After fn
// has been applied to acc, the result lands here as a partial Closure. We
// then apply that Closure to elem with CVFold as the next continuation, so
// the final acc lands back in CVFold's `value`.
struct CVStep final : public GCObject<CVStep, Continuation> {
  HeapPointer<HeapObject> elem;
  HeapPointer<CVFold> fold;

  CVStep(HeapObject *elem_, CVFold *fold_) : elem(elem_), fold(fold_) {}

  template <typename T, T (HeapPointerBase::*memberfn)(T x)>
  T recurse(T arg) {
    arg = Continuation::recurse<T, memberfn>(arg);
    arg = (elem.*memberfn)(arg);
    arg = (fold.*memberfn)(arg);
    return arg;
  }

  void execute(Runtime &runtime) override;
};

// CVFold drives the fold loop. Each time it executes, it either:
//   - delivers the final accumulator (when i == end), or
//   - reads arr[i] and schedules an Interpret of fn(acc, elem) with itself
//     as the result continuation. The new accumulator lands in `value`.
//
// Continuation::value holds the current accumulator across iterations.
// `pending_elem` holds an element retrieved by CVAwait while we were waiting
// for an unfulfilled Array slot.
struct CVFold final : public GCObject<CVFold, Continuation> {
  HeapPointer<Record> arr;          // the Array
  HeapPointer<Closure> fn;          // 2-arg combining function (b -> a -> b)
  HeapPointer<Continuation> done;   // delivers the final accumulator
  HeapPointer<HeapObject> pending_elem;  // set by CVAwait when arr[i] required awaiting
  size_t i;                         // current cursor
  size_t end;                       // exclusive end

  CVFold(Record *arr_, Closure *fn_, Continuation *done_, size_t i_, size_t end_)
      : arr(arr_), fn(fn_), done(done_), pending_elem(nullptr), i(i_), end(end_) {}

  template <typename T, T (HeapPointerBase::*memberfn)(T x)>
  T recurse(T arg) {
    arg = Continuation::recurse<T, memberfn>(arg);
    arg = (arr.*memberfn)(arg);
    arg = (fn.*memberfn)(arg);
    arg = (done.*memberfn)(arg);
    arg = (pending_elem.*memberfn)(arg);
    return arg;
  }

  void execute(Runtime &runtime) override {
    HeapObject *acc = value.get();

    if (i == end) {
      done->resume(runtime, acc);
      return;
    }

    HeapObject *elem;
    if (pending_elem.get()) {
      elem = pending_elem.get();
      pending_elem = nullptr;
    } else {
      Promise *slot = arr->at(i);
      if (!*slot) {
        // Element not yet ready: register a CVAwait that will stash the
        // element into `pending_elem` and reschedule us. Note that we do NOT
        // advance `i` here — when we resume we'll re-read arr[i] and find
        // it in pending_elem.
        runtime.heap.reserve(CVAwait::reserve());
        CVAwait *waiter = CVAwait::claim(runtime.heap, this);
        next = nullptr;  // we are about to be re-scheduled (via the waiter)
        slot->await(runtime, waiter);
        return;
      }
      elem = slot->coerce<HeapObject>();
    }

    // Apply fn(acc, elem). prim_vfold validates that need == 1 || need == 2.
    Closure *clo = fn.get();
    size_t need = clo->fun->args() - clo->applied;
    assert(need == 1 || need == 2);

    // Reserve heap *before* advancing the cursor: heap.reserve can throw
    // GCNeededException, in which case execute() is retried with our state
    // unchanged. If we incremented `i` first we'd skip an element on retry.
    if (need == 2) {
      runtime.heap.reserve(Runtime::reserve_apply2(clo->fun));
    } else {
      runtime.heap.reserve(Runtime::reserve_apply1(clo->fun) + CVStep::reserve());
    }

    ++i;
    next = nullptr;  // we are about to be re-scheduled

    if (need == 2) {
      // Single 2-arg RFun: bind both args, run Interpret once.
      runtime.claim_apply2(clo, acc, elem, this, /*caller=*/nullptr);
    } else {
      // Curried: apply acc to fn -> get partial closure, then CVStep applies
      // that to elem with `this` as the next continuation.
      CVStep *step = CVStep::claim(runtime.heap, elem, this);
      runtime.claim_apply1(clo, acc, step, /*caller=*/nullptr);
    }
  }
};

void CVAwait::execute(Runtime &runtime) {
  CVFold *f = fold.get();
  f->pending_elem = value.get();
  runtime.schedule(f);
}

void CVStep::execute(Runtime &runtime) {
  // value is the partial closure produced by applying fn to acc.
  Closure *partial = static_cast<Closure *>(value.get());
  // The partial must take exactly one more arg to produce the final acc;
  // the type system guarantees this for `b -> a -> b`.
  size_t need = partial->fun->args() - partial->applied;
  (void)need;
  assert(need == 1);

  runtime.heap.reserve(Runtime::reserve_apply1(partial->fun));
  runtime.claim_apply1(partial, elem.get(), fold.get(), /*caller=*/nullptr);
}

static PRIMFN(prim_vfold) {
  EXPECT(5);
  RECORD(arr, 0);
  INTEGER_MPZ(arg1, 1);
  INTEGER_MPZ(arg2, 2);
  CLOSURE(fn, 3);
  HeapObject *init = args[4];

  REQUIRE(mpz_cmp_si(arg1, 0) >= 0);
  REQUIRE(mpz_cmp_si(arg2, arr->size()) <= 0);
  REQUIRE(mpz_cmp(arg1, arg2) <= 0);
  // We only support fully-applicable 2-arg closures (the common shape produced
  // by wake for `acc -> elem -> acc`). Other shapes would need extra apply steps.
  // Accept any closure shape that completes after exactly 2 more arguments:
  //   applied + 2 == fargs : single 2-arg RFun (the common case for `_ + _`)
  //   applied + 1 == fargs : applying once yields another 1-arg-needing closure
  //                          which we then apply to the second arg
  size_t need = fn->fun->args() - fn->applied;
  REQUIRE(need == 1 || need == 2);
  size_t s = mpz_get_si(arg1);
  size_t e = mpz_get_si(arg2);

  runtime.heap.reserve(Tuple::fulfiller_pads + CVFold::reserve());
  Continuation *done = scope->claim_fulfiller(runtime, output);
  CVFold *fold = CVFold::claim(runtime.heap, arr, fn, done, s, e);
  fold->value = init;
  runtime.schedule(fold);
}

void prim_register_vector(PrimMap &pmap) {
  prim_register(pmap, "vget", prim_vget, type_vget, PRIM_PURE);
  prim_register(pmap, "vnew", prim_vnew, type_vnew, PRIM_ORDERED);
  prim_register(pmap, "vset", prim_vset, type_vset, PRIM_IMPURE);
  prim_register(pmap, "vfold", prim_vfold, type_vfold, PRIM_FNARG);
}
