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

// LVL -- Loop-invariant Lifting.
//
// WHAT: hoist a lambda body's pure, argument-independent terms into the enclosing scope, so
//   they run once instead of once-per-call (runtime.cpp re-interprets every body term on every
//   call).  CSE, the next pass, then dedups identical hoisted terms from different lambdas.
//   (wake has no loops; a lambda body is the analogue, but we keep the established "LVL" name.)
//
// WHY "TOTAL", not just "pure": a lambda body runs only when the lambda is called, which may be
//   zero times or conditional (an `if`/match arm, `filter pred []`).  Hoisting makes a term run
//   whenever the PARENT runs, so a non-total term could turn a never-taken failure into a real
//   one.  Purity is necessary but not sufficient -- see lvl_total() for exactly what we hoist.

struct PassLVL {
  TermStream stream;
  PassLVL(TargetScope &scope) : stream(scope) {}
};

// A term is TOTAL (safe to speculatively evaluate) iff it is a literal, a tuple
// allocation/projection, or a pure+total primitive.  RApp/RDes can call code of unknown
// totality (and RApp covers recursive calls, which diverge if eagerly evaluated), and
// RArg/RFun are never hoistable, so all of those return false.
static bool lvl_total(Term *t) {
  const std::type_info &id = t->id();
  if (id == typeid(RLit) || id == typeid(RCon) || id == typeid(RGet)) return true;
  if (id == typeid(RPrim)) {
    int pflags = static_cast<RPrim *>(t)->pflags;
    return (pflags & (PRIM_ORDERED | PRIM_EFFECT | PRIM_PARTIAL | PRIM_FNARG)) == 0;
  }
  return false;
}

// Coordinate system for the analysis/emit below.  During the optimizer a body term's args are
// source-coordinate flat indices.  For an RFun whose first body term has source index S and
// whose body has N terms, every arg x of a body term falls into one of four ranges:
//
//   arg x range        meaning                            liftable?
//   x <  S - 1         capture from an enclosing scope    yes (still in scope at the parent)
//   x == S - 1         the RFun itself (recursive self)   no  (would forward-reference itself)
//   S <= x < S + N     another body term (index x - S)    only if that term is liftable
//   x >= S + N         a later sibling (mutual recursion) no  (desugared; guarded defensively)
enum class ArgKind { Capture, SelfRef, BodyTerm, LaterSibling };

static ArgKind classify(size_t x, size_t S, size_t N) {
  if (x >= S + N) return ArgKind::LaterSibling;
  if (x >= S) return ArgKind::BodyTerm;
  if (x == S - 1) return ArgKind::SelfRef;
  return ArgKind::Capture;
}

// Phase 1 -- analysis.  Fill liftable[k] for each body term, in topological (forward) order so
// that a term's body dependencies have already been decided when we reach it.
static std::vector<char> markLiftable(const std::vector<std::unique_ptr<Term>> &terms, size_t S) {
  size_t N = terms.size();
  std::vector<char> liftable(N, 0);
  for (size_t k = 0; k < N; ++k) {
    Term *t = terms[k].get();
    bool ok = lvl_total(t);
    // Only Redux subtypes carry args; RLit has none and is trivially liftable if total.
    if (ok) {
      if (Redux *r = dynamic_cast<Redux *>(t)) {
        for (size_t x : r->args) {
          ArgKind kind = classify(x, S, N);
          // A capture is always fine; a body-term dep is fine only if it too is liftable; a
          // self-reference or later sibling can never be hoisted (forward reference).
          if (kind == ArgKind::BodyTerm) {
            if (!liftable[x - S]) ok = false;
          } else if (kind == ArgKind::SelfRef || kind == ArgKind::LaterSibling) {
            ok = false;
          }
          if (!ok) break;
        }
      }
    }
    liftable[k] = ok;
  }
  return liftable;
}

// Phase 2 -- emit liftable terms into the PARENT scope (before transferring self).  Returns
// hoisted[k] = the parent slot a hoisted term now lives at, or Term::invalid if not hoisted.
static std::vector<size_t> hoistLiftable(std::vector<std::unique_ptr<Term>> &terms,
                                         const std::vector<char> &liftable, size_t S, PassLVL &p) {
  size_t N = terms.size();
  std::vector<size_t> hoisted(N, Term::invalid);
  for (size_t k = 0; k < N; ++k) {
    if (!liftable[k]) continue;
    if (Redux *r = dynamic_cast<Redux *>(terms[k].get())) {
      // Remap each arg to its new parent slot: a body-term dep was itself hoisted; everything
      // else (a capture) is remapped through the current parent map.
      for (size_t &x : r->args) x = (x >= S) ? hoisted[x - S] : p.stream.map()[x];
    }
    hoisted[k] = p.stream.include(std::move(terms[k]));
  }
  return hoisted;
}

// Every leaf/redux term below is an identical passthrough: rewrite its args through the current
// map and re-emit it.  The per-type duplication is required only because the visitor pattern
// dispatches one override per Term subtype.  Only RFun does real work.

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

  // 1. Decide which body terms are liftable, then 2. move them into the parent scope.
  std::vector<char> liftable = markLiftable(terms, S);
  std::vector<size_t> hoisted = hoistLiftable(terms, liftable, S, p);

  // 3. Emit the RFun node, then its retained body (recursing into nested lambdas).  A hoisted
  //    term's body slot is mapped onto its new parent slot; everything else is re-emitted.
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
