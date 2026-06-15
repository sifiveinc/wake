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

#include <vector>

#include "ssa.h"

// LVL -- Loop-invariant Lifting (the optimization the PRIM_* flag docs in types/primfn.h
// name "LVL").  wake has no loops; the analogue is a lambda body, so this is more precisely
// lambda-invariant code motion, but we keep the codebase's established "LVL" name.
//
// A function body's terms are re-evaluated on every call (runtime.cpp:128 iterates and
// interprets every body term).  Any body term that is pure and independent of the function's
// argument(s) produces the same value on every call, so we hoist it into the enclosing scope
// where it is computed once instead of once per call.  CSE (which runs right after this pass)
// then dedups identical hoisted terms originating from different lambdas.
//
// SAFETY: wake evaluates eagerly, but a lambda's body only runs when the lambda is *called*,
// which may be zero times or conditional (e.g. an `if`/match arm, or `filter pred []`).
// Hoisting a term out of such a lambda makes it run when the *parent* runs regardless, so the
// term must be TOTAL -- it must not diverge or abort.  Purity is not enough: a pure prim like
// `vget` aborts out of range, and `RApp(mid, mid)` (the mutual-recursion fixpoint, tossa.cpp)
// is pure yet diverges if evaluated eagerly.  We therefore hoist only provably-total terms:
//   * RLit, RCon, RGet            -- a literal, a tuple allocation, a field projection
//   * RPrim that is pure, total (!PRIM_PARTIAL) and does not invoke a fn arg (!PRIM_FNARG)
// and never RApp/RDes (callee/handler totality is unknown; this also excludes recursive
// calls).  Hoisting a total term that the lambda might not have run only ever wastes one
// evaluation -- it can never change a result, diverge, or abort.
//
// Coordinate system: during the optimizer a body term's args are source-coordinate flat
// indices.  For an RFun at source position P, let S = P + 1 be the source index of its first
// body term and N = terms.size().  Because TargetScope::unwind reuses positions, a body
// term's arg x is always either:
//   x <  S - 1      a capture from an enclosing scope (already placed in the SourceMap)
//   x == S - 1      the RFun itself (a recursive self-reference, tossa.cpp:48)
//   S <= x < S + N  the body term at index x - S
//   x >= S + N      a later sibling (mutual recursion); desugared, so guarded defensively
// Only the first and third forms are safe inside a hoisted term: hoisting a term that
// references the function itself or a later sibling would create a forward reference.

struct PassLVL {
  TermStream stream;
  PassLVL(TargetScope &scope) : stream(scope) {}
};

// A term is safe to speculatively evaluate (total) iff it is a literal, a tuple
// allocation/projection, or a total pure primitive.  RApp/RDes/RArg/RFun are never total here.
static bool lvl_total(Term *t) {
  const std::type_info &id = t->id();
  if (id == typeid(RLit) || id == typeid(RCon) || id == typeid(RGet)) return true;
  if (id == typeid(RPrim)) {
    int pflags = static_cast<RPrim *>(t)->pflags;
    return (pflags & (PRIM_ORDERED | PRIM_EFFECT | PRIM_PARTIAL | PRIM_FNARG)) == 0;
  }
  return false;
}

// Every leaf/redux term is a passthrough: rewrite its args through the current map and
// re-emit it.  Only RFun (below) performs hoisting.

void RArg::pass_lvl(PassLVL &p, std::unique_ptr<Term> self) { p.stream.transfer(std::move(self)); }

void RLit::pass_lvl(PassLVL &p, std::unique_ptr<Term> self) { p.stream.transfer(std::move(self)); }

void RApp::pass_lvl(PassLVL &p, std::unique_ptr<Term> self) {
  update(p.stream.map());
  p.stream.transfer(std::move(self));
}

void RPrim::pass_lvl(PassLVL &p, std::unique_ptr<Term> self) {
  update(p.stream.map());
  p.stream.transfer(std::move(self));
}

void RGet::pass_lvl(PassLVL &p, std::unique_ptr<Term> self) {
  update(p.stream.map());
  p.stream.transfer(std::move(self));
}

void RDes::pass_lvl(PassLVL &p, std::unique_ptr<Term> self) {
  update(p.stream.map());
  p.stream.transfer(std::move(self));
}

void RCon::pass_lvl(PassLVL &p, std::unique_ptr<Term> self) {
  update(p.stream.map());
  p.stream.transfer(std::move(self));
}

void RFun::pass_lvl(PassLVL &p, std::unique_ptr<Term> self) {
  // Source index of the first body term (== CheckPoint(begin()).source after transfer).
  size_t S = p.stream.map().end() + 1;
  size_t N = terms.size();

  // ---- analysis: which body terms are liftable, in topological (forward) order ----
  std::vector<char> liftable(N, 0);
  for (size_t k = 0; k < N; ++k) {
    Term *t = terms[k].get();
    bool ok = lvl_total(t);
    if (ok) {
      // Only Redux subtypes carry args; RLit has none, so it is trivially liftable.
      Redux *r = dynamic_cast<Redux *>(t);
      if (r) {
        for (size_t x : r->args) {
          if (x >= S) {
            // body term (S <= x < S+N) or a later sibling (x >= S+N).  A later sibling, or a
            // body dep that is not itself liftable, makes t non-liftable.
            if (x >= S + N || !liftable[x - S]) {
              ok = false;
              break;
            }
          } else if (x == S - 1) {
            ok = false;  // the enclosing RFun itself (a recursive self-reference)
            break;
          }
          // else x < S-1: capture of an enclosing value -- valid at the hoist site.
        }
      }
    }
    liftable[k] = ok;
  }

  // ---- emit liftable terms into the PARENT scope, before transferring self ----
  std::vector<size_t> hoisted(N, Term::invalid);
  for (size_t k = 0; k < N; ++k) {
    if (!liftable[k]) continue;
    if (Redux *r = dynamic_cast<Redux *>(terms[k].get())) {
      for (size_t &x : r->args) x = (x >= S) ? hoisted[x - S] : p.stream.map()[x];
    }
    hoisted[k] = p.stream.include(std::move(terms[k]));
  }

  // ---- emit the RFun node, then its retained body (recursing into nested lambdas) ----
  p.stream.transfer(std::move(self));
  CheckPoint body = p.stream.begin();
  for (size_t k = 0; k < N; ++k) {
    if (hoisted[k] != Term::invalid) {
      p.stream.discard(hoisted[k]);  // map body slot S+k onto the parent slot
    } else {
      Term *t = terms[k].get();
      t->pass_lvl(p, std::move(terms[k]));
    }
  }
  update(p.stream.map());  // remap output now that the whole body is placed
  terms = p.stream.end(body);
}

std::unique_ptr<Term> Term::pass_lvl(std::unique_ptr<Term> term) {
  TargetScope scope;
  PassLVL pass(scope);
  // The top-level function stays at index 0 and runs exactly once, so we do not hoist out
  // of it; we only transfer it and process its body, recursing into nested lambdas.
  RFun *root = static_cast<RFun *>(term.get());
  pass.stream.transfer(std::move(term));
  CheckPoint body = pass.stream.begin();
  for (auto &x : root->terms) {
    Term *t = x.get();
    t->pass_lvl(pass, std::move(x));
  }
  root->update(pass.stream.map());
  root->terms = pass.stream.end(body);
  return scope.finish();
}
