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

#include "ssa.h"

#include "runtime/value.h"

const size_t Term::invalid;

Term::~Term() {}

void Redux::update(const SourceMap &map) {
  for (auto &x : args) x = map[x];
}

void Redux::format_args(std::ostream &os, TermFormat &format) const {
  bool first = true;
  for (auto x : args) {
    if (!first) os << " ";
    if (format.scoped) {
      os << arg_depth(x) << ":" << arg_offset(x);
    } else {
      os << x;
      if (x >= format.id) os << " !!!";
    }
    first = false;
  }
}

void RArg::format(std::ostream &os, TermFormat &format) const { os << "<arg>\n"; }

std::unique_ptr<Term> RArg::clone(TargetScope &scope, size_t id) const {
  return std::unique_ptr<Term>(new RArg(*this));
}

void RLit::format(std::ostream &os, TermFormat &format) const {
  HeapObject::format(os, value->get());
  os << "\n";
}

std::unique_ptr<Term> RLit::clone(TargetScope &scope, size_t id) const {
  return std::unique_ptr<Term>(new RLit(*this));
}

void RApp::format(std::ostream &os, TermFormat &format) const {
  os << "App(";
  format_args(os, format);
  os << ")\n";
}

static void clear_singleton(TargetScope &scope, size_t id, size_t x) {
  if (x < id) scope[x]->set(SSA_SINGLETON, false);
}
static void clear_singleton(TargetScope &scope, size_t id, const std::vector<size_t> &args) {
  for (auto x : args) clear_singleton(scope, id, x);
}

std::unique_ptr<Term> RApp::clone(TargetScope &scope, size_t id) const {
  std::unique_ptr<RApp> out(new RApp(*this));
  clear_singleton(scope, id, out->args);
  return static_unique_pointer_cast<Term>(std::move(out));
}

void RPrim::format(std::ostream &os, TermFormat &format) const {
  os << name << "(";
  format_args(os, format);
  os << ")\n";
}

std::unique_ptr<Term> RPrim::clone(TargetScope &scope, size_t id) const {
  std::unique_ptr<RPrim> out(new RPrim(*this));
  clear_singleton(scope, id, out->args);
  return static_unique_pointer_cast<Term>(std::move(out));
}

void RGet::format(std::ostream &os, TermFormat &format) const {
  os << "Get:" << index << "(";
  format_args(os, format);
  os << ")\n";
}

std::unique_ptr<Term> RGet::clone(TargetScope &scope, size_t id) const {
  std::unique_ptr<RGet> out(new RGet(*this));
  clear_singleton(scope, id, out->args);
  return static_unique_pointer_cast<Term>(std::move(out));
}

void RDes::format(std::ostream &os, TermFormat &format) const {
  os << "Des(";
  format_args(os, format);
  os << ")\n";
}

std::unique_ptr<Term> RDes::clone(TargetScope &scope, size_t id) const {
  std::unique_ptr<RDes> out(new RDes(*this));
  clear_singleton(scope, id, out->args);
  return static_unique_pointer_cast<Term>(std::move(out));
}

void RCon::format(std::ostream &os, TermFormat &format) const {
  os << "Con:" << kind->ast.name << "(";
  format_args(os, format);
  os << ")\n";
}

std::unique_ptr<Term> RCon::clone(TargetScope &scope, size_t id) const {
  std::unique_ptr<RCon> out(new RCon(*this));
  clear_singleton(scope, id, out->args);
  return static_unique_pointer_cast<Term>(std::move(out));
}

static std::string pad(int depth) { return std::string(depth, ' '); }

RFun::RFun(const RFun &o, TargetScope &scope, size_t id)
    : Term(o), fragment(o.fragment), output(o.output) {
  terms.reserve(o.terms.size());
  for (auto &x : o.terms) terms.emplace_back(x->clone(scope, id));
  clear_singleton(scope, id, output);
}

void RFun::update(const SourceMap &map) { output = map[output]; }

size_t RFun::args() const {
  for (size_t out = 0; out < terms.size(); ++out)
    if (terms[out]->id() != typeid(RArg)) return out;
  return terms.size();
}

void RFun::format(std::ostream &os, TermFormat &format) const {
  format.depth += 2;
  size_t index = 0;
  os << "Fun(" << fragment.location() << "):" << std::endl;
  if (!escapes.empty()) {
    os << pad(format.depth) << "escapes:";
    for (auto x : escapes) os << " " << arg_depth(x) << ":" << arg_offset(x);
    os << "\n";
  }
  if (format.scoped) {
    os << pad(format.depth) << "hash: " << hash.data[0] << "\n";
    os << pad(format.depth) << "returns: " << arg_depth(output) << ":" << arg_offset(output);
  } else {
    os << pad(format.depth) << "returns: " << output;
    if (output > format.id + terms.size()) os << " !!!";
  }
  os << "\n";
  for (auto &x : terms) {
    os << pad(format.depth);
    if (format.scoped) {
      os << index++;
    } else {
      os << ++format.id;
    }
    if (!x->label.empty()) os << " (" << x->label << ")";
    os << " [";
    if ((x->flags & SSA_RECURSIVE) != 0) {
      os << "R";  // recursive
      // only functions can be recursive => not E or O
    } else if ((x->flags & SSA_EFFECT) != 0) {
      os << "E";  // effects
      // if effects are produced, must also be ordered
    } else if ((x->flags & SSA_ORDERED) != 0) {
      os << "O";  // ordered
    } else if ((x->flags & SSA_USED) == 0) {
      os << "U";  // unused, and not an effect!
    } else {
      // nothing interesting
      os << "-";
    }
    os << "," << x->meta << "] = ";
    x->format(os, format);
  }
  format.id -= terms.size();
  format.depth -= 2;
}

std::unique_ptr<Term> RFun::clone(TargetScope &scope, size_t id) const {
  return std::unique_ptr<Term>(new RFun(*this, scope, id));
}

std::vector<std::unique_ptr<Term> > TargetScope::unwind(size_t newend) {
  std::vector<std::unique_ptr<Term> > out;
  out.reserve(terms.size() - newend);
  for (size_t i = newend; i < terms.size(); ++i) out.emplace_back(std::move(terms[i]));
  terms.resize(newend);
  return out;
}

std::unique_ptr<Term> Term::optimize(std::unique_ptr<Term> term, Runtime &runtime) {
  term = Term::pass_purity(std::move(term), PRIM_EFFECT, SSA_EFFECT);
  term = Term::pass_purity(std::move(term), PRIM_ORDERED, SSA_ORDERED);
  term = Term::pass_usage(std::move(term));
  term = Term::pass_sweep(std::move(term));
  term = Term::pass_inline(std::move(term), 20, runtime);
  term = Term::pass_purity(std::move(term), PRIM_EFFECT, SSA_EFFECT);
  term = Term::pass_purity(std::move(term), PRIM_ORDERED, SSA_ORDERED);
  term = Term::pass_usage(std::move(term));
  term = Term::pass_sweep(std::move(term));
  // Hoist lambda-invariant pure terms out of their functions so CSE (next) can dedup
  // identical invariants from different lambdas, and they run once instead of per-call.
  term = Term::pass_lvl(std::move(term));
  term = Term::pass_cse(std::move(term), runtime);
  term = Term::pass_usage(std::move(term));
  term = Term::pass_inline(std::move(term), 50, runtime);
  term = Term::pass_purity(std::move(term), PRIM_EFFECT, SSA_EFFECT);
  term = Term::pass_purity(std::move(term), PRIM_ORDERED, SSA_ORDERED);
  term = Term::pass_usage(std::move(term));
  term = Term::pass_sweep(std::move(term));
  term = Term::pass_cse(std::move(term), runtime);
  return term;
}
