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

#include "runtime.h"

#include <assert.h>
#include <signal.h>
#include <sys/time.h>

#include "job.h"
#include "optimizer/ssa.h"
#include "profile.h"
#include "status.h"
#include "tuple.h"
#include "value.h"

#define PROFILE_HZ 1000

static volatile bool trace_needed = false;
static void handle_SIGPROF(int sig) {
  (void)sig;
  trace_needed = true;
}

Closure::Closure(RFun *fun_, size_t applied_, Scope *scope_)
    : fun(fun_), applied(applied_), scope(scope_) {}

void Work::format(std::ostream &os, FormatState &state) const { os << "Work"; }

Category Work::category() const { return WORK; }

Runtime::Runtime(Profile *profile_, int profile_heap, double heap_factor)
    : abort(false),
      profile(profile_),
      heap(profile_heap, heap_factor),
      stack(heap.root<Work>(nullptr)),
      output(heap.root<HeapObject>(nullptr)),
      sources(heap.root<HeapObject>(nullptr)) {
  if (profile) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));

    // Setup a SIGPROF timer to trigger stack tracing
    struct itimerval timer;
    timer.it_value.tv_sec = 0;
    timer.it_value.tv_usec = 1000000 / PROFILE_HZ;
    timer.it_interval = timer.it_value;

    sa.sa_handler = handle_SIGPROF;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGPROF, &sa, 0);
    setitimer(ITIMER_PROF, &timer, 0);
  }
}

Runtime::~Runtime() {
  struct itimerval timer;
  memset(&timer, 0, sizeof(timer));
  setitimer(ITIMER_PROF, &timer, 0);
}

struct Interpret final : public GCObject<Interpret, Work> {
  RFun *fun;
  size_t index;
  HeapPointer<Scope> scope;
  HeapPointer<Continuation> cont;

  Interpret(RFun *fun_, Scope *scope_, Continuation *cont_)
      : fun(fun_), index(0), scope(scope_), cont(cont_) {}

  template <typename T, T (HeapPointerBase::*memberfn)(T x)>
  T recurse(T arg) {
    arg = Work::recurse<T, memberfn>(arg);
    arg = (scope.*memberfn)(arg);
    arg = (cont.*memberfn)(arg);
    return arg;
  }

  void execute(Runtime &runtime) override;
};

struct InterpretContext {
  Runtime &runtime;
  Interpret *interpret;
  Scope *scope;
  size_t output;       // index into scope
  Continuation *cont;  // set for tail calls

  InterpretContext(Runtime &runtime_) : runtime(runtime_) {}
  static Promise *arg(Scope *scope, size_t arg);

  Promise *arg(size_t arg_) { return arg(scope, arg_); }
  Continuation *defer();
  void finish(HeapObject *obj);
  void finish(Promise *p);
};

void Interpret::execute(Runtime &runtime) {
  InterpretContext context(runtime);
  context.interpret = this;
  context.scope = scope.get();
  context.cont = nullptr;

  size_t limit = fun->terms.size();
  bool tail = fun->output == make_arg(0, limit - 1) && fun->terms.back()->tailCallOk();
  if (tail) --limit;

  next = nullptr;  // potentially reschedule
  for (context.output = index; context.output < limit; context.output = index) {
    fun->terms[context.output]->interpret(context);
    index = context.output + 1;
    if (!context.interpret) return;
  }

  if (tail) {
    context.interpret = nullptr;
    context.cont = cont.get();
    fun->terms.back()->interpret(context);
  } else {
    context.arg(fun->output)->await(runtime, cont.get());
  }
}

Continuation *InterpretContext::defer() {
  if (cont) {
    return cont;
  } else {
    return scope->claim_fulfiller(runtime, output);
  }
}

void InterpretContext::finish(HeapObject *obj) {
  if (cont) {
    cont->resume(runtime, obj);
  } else {
    scope->at(output)->instant_fulfill(obj);
  }
}

void InterpretContext::finish(Promise *p) {
  if (*p) {
    finish(p->coerce<HeapObject>());
  } else {
    p->await(runtime, defer());
  }
}

Promise *InterpretContext::arg(Scope *it, size_t arg) {
  for (size_t depth = arg_depth(arg); depth > 0; --depth) it = it->next.get();
  return it->at(arg_offset(arg));
}

bool RArg::tailCallOk() const {
  // We don't invoke return, so cannot be tail called
  return false;
}

void RArg::interpret(InterpretContext &context) {
  // No-Op; filled in by App during Scope construction
}

bool RLit::tailCallOk() const { return true; }

void RLit::interpret(InterpretContext &context) { context.finish(value->get()); }

bool RFun::tailCallOk() const { return !get(SSA_RECURSIVE); }

void RFun::interpret(InterpretContext &context) {
  context.finish(Closure::alloc(context.runtime.heap, this, 0, context.scope));
}

bool RCon::tailCallOk() const { return true; }

void RCon::interpret(InterpretContext &context) {
  size_t size = args.size();
  context.runtime.heap.reserve(Record::reserve(size) + size * Tuple::fulfiller_pads);
  Record *bind = Record::claim(context.runtime.heap, kind.get(), size);
  for (size_t i = 0; i < size; ++i)
    bind->claim_instant_fulfiller(context.runtime, i, context.arg(args[i]));
  context.finish(bind);
}

struct CGet final : public GCObject<CGet, Continuation> {
  HeapPointer<Continuation> cont;
  size_t index;

  CGet(Continuation *cont_, size_t index_) : cont(cont_), index(index_) {}

  template <typename T, T (HeapPointerBase::*memberfn)(T x)>
  T recurse(T arg) {
    arg = Continuation::recurse<T, memberfn>(arg);
    arg = (cont.*memberfn)(arg);
    return arg;
  }

  void execute(Runtime &runtime) override {
    Record *record = static_cast<Record *>(value.get());
    record->at(index)->await(runtime, cont.get());
  }
};

bool RGet::tailCallOk() const { return true; }

void RGet::interpret(InterpretContext &context) {
  Promise *arg = context.arg(args[0]);
  if (*arg) {
    context.runtime.heap.reserve(Tuple::fulfiller_pads);
    context.finish(arg->coerce<Record>()->at(index));
  } else {
    context.runtime.heap.reserve(Tuple::fulfiller_pads + CGet::reserve());
    arg->await(context.runtime, CGet::claim(context.runtime.heap, context.defer(), index));
  }
}

struct CDes final : public GCObject<CDes, Continuation> {
  HeapPointer<Scope> scope;
  HeapPointer<Continuation> cont;
  RDes *des;

  CDes(Scope *scope_, Continuation *cont_, RDes *des_) : scope(scope_), cont(cont_), des(des_) {}

  template <typename T, T (HeapPointerBase::*memberfn)(T x)>
  T recurse(T arg) {
    arg = Continuation::recurse<T, memberfn>(arg);
    arg = (scope.*memberfn)(arg);
    arg = (cont.*memberfn)(arg);
    return arg;
  }

  void execute(Runtime &runtime) override {
    Record *record = static_cast<Record *>(value.get());
    Closure *handler =
        InterpretContext::arg(scope.get(), des->args[record->cons->index])->coerce<Closure>();
    runtime.heap.reserve(runtime.reserve_apply(handler->fun));
    runtime.claim_apply(handler, record, cont.get(), scope.get());
  }
};

bool RDes::tailCallOk() const { return true; }

void RDes::interpret(InterpretContext &context) {
  Promise *arg = context.arg(args.back());
  if (*arg) {
    Record *record = arg->coerce<Record>();
    Closure *handler =
        InterpretContext::arg(context.scope, args[record->cons->index])->coerce<Closure>();
    context.runtime.heap.reserve(context.runtime.reserve_apply(handler->fun));
    if (context.interpret) {
      context.runtime.schedule(context.interpret);
      context.interpret = nullptr;
    }
    context.runtime.claim_apply(handler, record, context.defer(), context.scope);
  } else {
    context.runtime.heap.reserve(Tuple::fulfiller_pads + CDes::reserve());
    arg->await(context.runtime,
               CDes::claim(context.runtime.heap, context.scope, context.defer(), this));
  }
}

struct CPrim final : public GCObject<CPrim, Continuation> {
  HeapPointer<Scope> scope;
  size_t output;
  RPrim *prim;

  CPrim(Scope *scope_, size_t output_, RPrim *prim_)
      : scope(scope_), output(output_), prim(prim_) {}

  template <typename T, T (HeapPointerBase::*memberfn)(T x)>
  T recurse(T arg) {
    arg = Continuation::recurse<T, memberfn>(arg);
    arg = (scope.*memberfn)(arg);
    return arg;
  }

  static Promise *doit(Runtime &runtime, Scope *scope, size_t output, RPrim *prim);
  void execute(Runtime &runtime) override {
    if (Promise *p = doit(runtime, scope.get(), output, prim)) {
      next = nullptr;  // reschedule
      p->await(runtime, this);
    }
  }
};

Promise *CPrim::doit(Runtime &runtime, Scope *scope, size_t output, RPrim *prim) {
  Value *pargs[prim->args.size()];
  Promise *p = nullptr;
  size_t i;
  for (i = 0; i < prim->args.size(); ++i) {
    p = InterpretContext::arg(scope, prim->args[i]);
    if (!*p) break;
    pargs[i] = p->coerce<Value>();
  }
  if (i == prim->args.size()) {
    prim->fn(prim->data, runtime, scope, output, prim->args.size(), pargs);
    return nullptr;
  } else {
    return p;
  }
}

bool RPrim::tailCallOk() const { return false; }

void RPrim::interpret(InterpretContext &context) {
  context.runtime.heap.reserve(Tuple::fulfiller_pads + CPrim::reserve());
  if (Promise *p = CPrim::doit(context.runtime, context.scope, context.output, this)) {
    CPrim *prim = CPrim::claim(context.runtime.heap, context.scope, context.output, this);
    p->await(context.runtime, prim);
  }
}

struct CApp final : public GCObject<CApp, Continuation> {
  HeapPointer<Continuation> cont;
  HeapPointer<Scope> caller;
  size_t output;
  RApp *app;

  CApp(Continuation *cont_, Scope *caller_, size_t output_, RApp *app_)
      : cont(cont_), caller(caller_), output(output_), app(app_) {}

  template <typename T, T (HeapPointerBase::*memberfn)(T x)>
  T recurse(T arg) {
    arg = Continuation::recurse<T, memberfn>(arg);
    arg = (cont.*memberfn)(arg);
    arg = (caller.*memberfn)(arg);
    return arg;
  }

  static void doit(Runtime &runtime, Closure *closure, Continuation *cont, Scope *caller,
                   size_t output, RApp *app, Interpret *&resume);
  void execute(Runtime &runtime) override {
    Interpret *null = nullptr;
    doit(runtime, static_cast<Closure *>(value.get()), cont.get(), caller.get(), output, app, null);
  }
};

void CApp::doit(Runtime &runtime, Closure *closure, Continuation *cont, Scope *caller,
                size_t output, RApp *app, Interpret *&resume) {
  RFun *fun = closure->fun;
  size_t applied = closure->applied;
  size_t nargs = app->args.size() - 1;
  size_t fargs = fun->args();
  size_t terms = fun->terms.size();
  Scope *callee = closure->scope.get();

  if (applied + nargs == fargs) {
    runtime.heap.reserve(Scope::reserve(terms) + (1 + fargs) * Tuple::fulfiller_pads +
                         Interpret::reserve());
    // Skip over partially applied arguments
    Scope *it = callee;
    for (size_t pop = applied; pop; it = it->next.get()) pop -= it->size();
    // Fully applied function; allocate "stack" frame
    Scope *bind = Scope::claim(runtime.heap, terms, it, caller, fun);
    // Fill in App() args
    for (size_t i = 0; i < nargs; ++i)
      bind->claim_instant_fulfiller(runtime, applied + i,
                                    InterpretContext::arg(caller, app->args[i + 1]));
    // Forward the partially applied arguments
    it = callee;
    size_t pop = applied;
    while (pop) {
      size_t size = it->size();
      pop -= size;
      for (size_t i = 0; i < size; ++i) bind->claim_instant_fulfiller(runtime, pop + i, it->at(i));
      it = it->next.get();
    }
    // Schedule an Interpreter
    Interpret *interpret = Interpret::claim(runtime.heap, fun, bind,
                                            cont ? cont : caller->claim_fulfiller(runtime, output));
    if (resume) {
      runtime.schedule(resume);
      resume = nullptr;
    }
    runtime.schedule(interpret);
  } else {
    runtime.heap.reserve(Scope::reserve(nargs) + nargs * Tuple::fulfiller_pads +
                         Closure::reserve());
    Scope *bind = Scope::claim(runtime.heap, nargs, callee, caller, fun);
    for (size_t i = 0; i < nargs; ++i)
      bind->claim_instant_fulfiller(runtime, i, InterpretContext::arg(caller, app->args[i + 1]));
    Closure *closure = Closure::claim(runtime.heap, fun, applied + nargs, bind);
    if (cont) {
      cont->resume(runtime, closure);
    } else {
      caller->at(output)->fulfill(runtime, closure);
    }
  }
}

bool RApp::tailCallOk() const { return true; }

void RApp::interpret(InterpretContext &context) {
  Promise *fn = context.arg(args[0]);
  if (*fn) {
    CApp::doit(context.runtime, fn->coerce<Closure>(), context.cont, context.scope, context.output,
               this, context.interpret);
  } else {
    fn->await(context.runtime,
              CApp::alloc(context.runtime.heap, context.cont, context.scope, context.output, this));
  }
}

size_t Runtime::reserve_apply(RFun *fun) {
  return Scope::reserve(fun->terms.size()) + Tuple::fulfiller_pads + Interpret::reserve();
}

void Runtime::claim_apply(Closure *closure, HeapObject *value, Continuation *cont, Scope *caller) {
  RFun *fun = closure->fun;
  Scope *bind = Scope::claim(heap, fun->terms.size(), closure->scope.get(), caller, fun);
  bind->at(0)->instant_fulfill(value);
  schedule(Interpret::claim(heap, fun, bind, cont));
}

void Runtime::run() {
  int count = 0;
  bool lprofile = profile;
  trace_needed = false;  // don't count time spent waiting for Jobs

  // Track time spent for a run, and warn if it exceeds a threshold
  uint64_t work_items = 0;
  struct timespec run_start;
  clock_gettime(CLOCK_REALTIME, &run_start);
  double next_warning_time = 300.0;  // First warning at 5 minutes (300 seconds)

  while (stack && !abort) {
    if (++count >= 10000) {
      if (JobTable::exit_now()) break;
      status_refresh(false);

      work_items += count;
      count = 0;

      // Check elapsed time for warning
      struct timespec now;
      clock_gettime(CLOCK_REALTIME, &now);
      double elapsed = (now.tv_sec - run_start.tv_sec) +
                       (now.tv_nsec - run_start.tv_nsec) / 1e9;

      if (elapsed >= next_warning_time) {
        std::cerr << "[INTERPRETER WARNING] " << work_items
                  << " work items processed, " << elapsed << "s elapsed"
                  << std::endl;

        // Stack trace if debug enabled
        if (Scope::debug) {
          Work *w = stack.get();
          if (Interpret *i = dynamic_cast<Interpret *>(w)) {
            auto trace = i->scope->stack_trace(false);
            if (!trace.empty()) {
              std::cerr << "  Current execution context:" << std::endl;
              for (const auto &frame : trace) {
                std::cerr << "    " << frame << std::endl;
              }
            }
          }
        } else {
          std::cerr << "  (Run with --debug for stack trace)" << std::endl;
        }

        next_warning_time *= 2;  // Double for next warning
      }
    }

    Work *w = stack.get();
    stack = w->next;
    try {
      w->execute(*this);
      if (lprofile && trace_needed) {
        if (Interpret *i = dynamic_cast<Interpret *>(w)) {
          auto stack = i->scope->stack_trace(false);
          Profile *node = profile;
          for (auto it = stack.rbegin(); it != stack.rend(); ++it) node = &node->children[*it];
          ++node->count;
          trace_needed = false;
        }
      }
    } catch (GCNeededException gc) {
      // retry work after memory is available
      w->next = stack;
      stack = w;
      heap.GC(gc.needed);
      trace_needed = false;  // don't count time spent running GC
    }
  }
}

struct CInit final : public GCObject<CInit, Continuation> {
  void execute(Runtime &runtime) override { runtime.output = value; }
};

void Runtime::init(RFun *root) {
  heap.guarantee(CInit::reserve() + Closure::reserve() + reserve_apply(root));
  CInit *done = CInit::claim(heap);
  Closure *clo = Closure::claim(heap, root, 0, nullptr);
  claim_apply(clo, clo, done, nullptr);
}
