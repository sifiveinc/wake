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

#ifndef RUNTIME_H
#define RUNTIME_H

#include "gc.h"

struct RFun;
struct Closure;
struct Runtime;
struct Continuation;
struct Record;
struct Scope;

struct Work : public HeapObject {
  HeapPointer<Work> next;

  virtual void execute(Runtime &runtime) = 0;
  void format(std::ostream &os, FormatState &state) const override;
  Category category() const override;

  template <typename T, T (HeapPointerBase::*memberfn)(T x)>
  T recurse(T arg) {
    arg = HeapObject::recurse<T, memberfn>(arg);
    arg = (next.*memberfn)(arg);
    return arg;
  }
};

struct Profile;
struct Runtime {
  bool abort;
  Profile *profile;
  Heap heap;
  RootPointer<Work> stack;
  RootPointer<HeapObject> output;
  RootPointer<Record> sources;  // Vector String

  Runtime(Profile *profile_, int profile_heap, double heap_factor, double heap_pivot_mb);
  ~Runtime();
  void run();

  void schedule(Work *work) {
#ifdef DEBUG_GC
    assert(!work->next);
#endif
    work->next = stack;
    stack = work;
  }

  void init(RFun *root);

  // Caller must guarantee clo->applied==0 and clo->fun.args()==1
  static size_t reserve_apply(RFun *fun);
  void claim_apply(Closure *clo, HeapObject *value, Continuation *cont, Scope *caller);
};

struct Continuation : public Work {
  HeapPointer<HeapObject> value;

  void resume(Runtime &runtime, HeapObject *obj) {
    value = obj;
    runtime.schedule(this);
  }

  template <typename T, T (HeapPointerBase::*memberfn)(T x)>
  T recurse(T arg) {
    arg = Work::recurse<T, memberfn>(arg);
    arg = (value.*memberfn)(arg);
    return arg;
  }
};

#endif
