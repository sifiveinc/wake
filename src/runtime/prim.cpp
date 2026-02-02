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

#include "prim.h"

#include <bitset>
#include <cassert>
#include <cstdlib>
#include <iosfwd>
#include <sstream>
#include <unordered_map>

#include "cas_prim.h"
#include "optimizer/ssa.h"
#include "status.h"
#include "tuple.h"
#include "types/datatype.h"
#include "types/sums.h"
#include "util/location.h"
#include "value.h"

void require_fail(const char *message, unsigned size, Runtime &runtime, const Scope *scope) {
  std::stringstream ss;
  ss.write(message, size - 1);
  for (auto &x : scope->stack_trace()) ss << "  from " << x << std::endl;
  status_get_generic_stream(STREAM_ERROR) << ss.str() << std::endl;
  runtime.abort = true;
}

Value *alloc_order(Heap &h, int x) {
  int m;
  if (x < 0)
    m = 0;
  else if (x > 0)
    m = 2;
  else
    m = 1;
  return Record::alloc(h, &Order->members[m], 0);
}

Value *alloc_nil(Heap &h) { return Record::alloc(h, &List->members[0], 0); }

Value *claim_unit(Heap &h) { return Record::claim(h, &Unit->members[0], 0); }

Value *claim_bool(Heap &h, bool x) { return Record::claim(h, &Boolean->members[x ? 0 : 1], 0); }

Value *claim_tuple2(Heap &h, Value *first, Value *second) {
  Record *out = Record::claim(h, &Pair->members[0], 2);
  out->at(0)->instant_fulfill(first);
  out->at(1)->instant_fulfill(second);
  return out;
}

Value *claim_result(Heap &h, bool ok, Value *value) {
  Record *out = Record::claim(h, &Result->members[ok ? 0 : 1], 1);
  out->at(0)->instant_fulfill(value);
  return out;
}

Value *claim_list(Heap &h, size_t elements, Value **values) {
  Record *out = Record::claim(h, &List->members[0], 0);
  while (elements) {
    --elements;
    Record *next = Record::claim(h, &List->members[1], 2);
    next->at(0)->instant_fulfill(values[elements]);
    next->at(1)->instant_fulfill(out);
    out = next;
  }
  return out;
}

struct HeapHash {
  Hash code;
  Promise *broken;
};

HeapStep Closure::explore_escape(HeapStep step) {
  Scope *it = scope.get();
  for (size_t i = 0, size; i < applied; i += size) {
    size = it->size();
    for (size_t j = size; j > 0; --j)
      step = it->at(j - 1)->recurse<HeapStep, &HeapPointerBase::explore>(step);
    it = it->next.get();
  }
  for (auto x : fun->escapes) {
    Scope *s = it;
    for (size_t depth = 0; depth < arg_depth(x); ++depth) s = s->next.get();
    step = s->at(arg_offset(x))->recurse<HeapStep, &HeapPointerBase::explore>(step);
  }
  return step;
}

static HeapHash deep_hash_imp(Heap &heap, HeapObject *obj) {
  std::unordered_map<uintptr_t, size_t> explored;
  void *vscratch = heap.scratch(heap.used());
  HeapObject **scratch = static_cast<HeapObject **>(vscratch);

  HeapStep step;
  scratch[0] = obj;
  step.found = &scratch[1];
  step.broken = nullptr;

  Hash code;
  for (HeapObject **done = scratch; done != step.found; ++done) {
    HeapObject *head = *done;
    assert(head->category() == VALUE);
    Value *value = static_cast<Value *>(head);

    // Assign objects virtual addreses based on visitation order
    uintptr_t key = reinterpret_cast<uintptr_t>(static_cast<void *>(value));
    auto out = explored.insert(std::make_pair(key, done - scratch));

    // Include hash of child's virtual address
    code = code + out.first->second;
    // Only visit object once
    if (!out.second) continue;

    // Hash this object and enqueue its children for hashing
    step = value->explore(step);
    code = code + value->shallow_hash();
  }

  HeapHash out;
  out.code = code;
  out.broken = step.broken;
  return out;
}

Hash Value::deep_hash(Heap &heap) {
  HeapHash x = deep_hash_imp(heap, this);
  assert(!x.broken);
  return x.code;
}

struct CHash final : public GCObject<CHash, Continuation> {
  HeapPointer<HeapObject> obj;
  HeapPointer<Continuation> cont;

  CHash(HeapObject *obj_, Continuation *cont_) : obj(obj_), cont(cont_) {}

  template <typename T, T (HeapPointerBase::*memberfn)(T x)>
  T recurse(T arg) {
    arg = Continuation::recurse<T, memberfn>(arg);
    arg = (obj.*memberfn)(arg);
    arg = (cont.*memberfn)(arg);
    return arg;
  }

  void execute(Runtime &runtime) override;
};

void CHash::execute(Runtime &runtime) {
  MPZ out("0xffffFFFFffffFFFFffffFFFFffffFFFF");  // 128 bit
  runtime.heap.reserve(Integer::reserve(out));

  auto hash = deep_hash_imp(runtime.heap, obj.get());
  if (hash.broken) {
    next = nullptr;  // reschedule
    hash.broken->await(runtime, this);
  } else {
    Hash &h = hash.code;
    mpz_import(out.value, sizeof(h.data) / sizeof(h.data[0]), 1, sizeof(h.data[0]), 0, 0,
               &h.data[0]);
    cont->resume(runtime, Integer::claim(runtime.heap, out));
  }
}

size_t reserve_hash() { return CHash::reserve(); }

Work *claim_hash(Heap &h, Value *value, Continuation *continuation) {
  return CHash::claim(h, value, continuation);
}

PrimMap prim_register_all(StringInfo *info, JobTable *jobtable, CASContext *cas_ctx) {
  PrimMap pmap;
  prim_register_string(pmap, info);
  prim_register_vector(pmap);
  prim_register_integer(pmap);
  prim_register_double(pmap);
  prim_register_exception(pmap);
  prim_register_regexp(pmap);
  prim_register_target(pmap);
  prim_register_json(pmap);
  prim_register_job(jobtable, pmap);
  prim_register_sources(pmap);
  if (cas_ctx) {
    prim_register_cas(cas_ctx, pmap);
  }
  return pmap;
}
