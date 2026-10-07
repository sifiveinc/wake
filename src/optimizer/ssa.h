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

#ifndef SSA_H
#define SSA_H

#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "runtime/gc.h"
#include "types/datatype.h"
#include "types/primfn.h"
#include "util/hash.h"
#include "util/location.h"

struct Value;
struct Expr;
struct SourceMap;
struct PassPurity;
struct PassUsage;
struct PassSweep;
struct PassWiden;
struct PassInline;
struct PassCSE;
struct PassLVL;
struct PassScope;
struct TargetScope;
struct InterpretContext;

struct TermFormat {
  int depth;
  size_t id;
  bool scoped;
  TermFormat(bool scoped_ = false) : depth(0), id(0), scoped(scoped_) {}
};

#define SSA_RECURSIVE 0x01
#define SSA_ORDERED 0x02
#define SSA_EFFECT 0x04
#define SSA_USED 0x08
#define SSA_SINGLETON 0x10
#define SSA_FRCON 0x20
#define SSA_MOVED 0x40

struct Term {
  static const size_t invalid = ~static_cast<size_t>(0);

  std::string label;  // not unique
  size_t flags;       // SSA flags, accumulated over many passes
  uintptr_t meta;     // temporary scratch space for a pass

  Term(const char *label_, size_t flags_ = 0, uintptr_t meta_ = 0)
      : label(label_), flags(flags_), meta(meta_) {}
  const std::type_info &id() { return typeid(*this); }
  void set(size_t flag, bool value) {
    flags = (flags & ~flag) | (-static_cast<size_t>(value) & flag);
  }
  bool get(size_t flag) const { return flags & flag; }

  virtual ~Term();
  virtual std::unique_ptr<Term> clone(TargetScope &scope, size_t id) const = 0;
  virtual void format(std::ostream &os, TermFormat &format) const = 0;
  virtual void interpret(InterpretContext &context) = 0;
  virtual bool tailCallOk() const = 0;

  // All terms must implement their pass behaviour
  virtual void pass_purity(PassPurity &p) = 0;
  virtual void pass_usage(PassUsage &p) = 0;
  virtual void pass_sweep(PassSweep &p) = 0;
  virtual void pass_inline(PassInline &p, std::unique_ptr<Term> self) = 0;
  virtual void pass_cse(PassCSE &p, std::unique_ptr<Term> self) = 0;
  virtual void pass_lvl(PassLVL &p, std::unique_ptr<Term> self) = 0;
  virtual void pass_scope(PassScope &p) = 0;

  // The top-level pass invocations
  static std::unique_ptr<Term> pass_purity(std::unique_ptr<Term> term, int pflag, size_t sflag);
  static std::unique_ptr<Term> pass_usage(std::unique_ptr<Term> term);
  static std::unique_ptr<Term> pass_sweep(std::unique_ptr<Term> term);
  static std::unique_ptr<Term> pass_inline(std::unique_ptr<Term> term, size_t threshold,
                                           Runtime &runtime);
  static std::unique_ptr<Term> pass_cse(std::unique_ptr<Term> term, Runtime &runtime);
  static std::unique_ptr<Term> pass_lvl(std::unique_ptr<Term> term);

  // Create SSA from AST
  static std::unique_ptr<Term> fromExpr(std::unique_ptr<Expr> expr, Runtime &runtime);
  // The overall optimization strategy
  static std::unique_ptr<Term> optimize(std::unique_ptr<Term> term, Runtime &runtime);
  // Convert Redux argument references to Scope indexes
  static std::unique_ptr<Term> scope(std::unique_ptr<Term> term, Runtime &runtime);
};

template <typename T, typename F>
std::unique_ptr<T> static_unique_pointer_cast(std::unique_ptr<F> &&x) {
  return std::unique_ptr<T>(static_cast<T *>(x.release()));
}

struct Leaf : public Term {
  Leaf(const char *label_) : Term(label_) {}
};

// After scope pass, Redux.args is suitable for interpretation
inline size_t arg_depth(size_t arg) { return arg & 65535; }
inline size_t arg_offset(size_t arg) { return arg >> 16; }
inline size_t make_arg(size_t depth, size_t offset) { return (offset << 16) | depth; }

struct Redux : public Term {
  std::vector<size_t> args;

  Redux(const char *label_, std::vector<size_t> &&args_) : Term(label_), args(std::move(args_)) {}
  void update(const SourceMap &map);
  void format_args(std::ostream &os, TermFormat &format) const;
};

struct RArg final : public Leaf {
  RArg(const char *label_ = "") : Leaf(label_) {}

  std::unique_ptr<Term> clone(TargetScope &scope, size_t id) const override;
  void format(std::ostream &os, TermFormat &format) const override;
  void interpret(InterpretContext &context) override;
  bool tailCallOk() const override;

  void pass_purity(PassPurity &p) override;
  void pass_usage(PassUsage &p) override;
  void pass_sweep(PassSweep &p) override;
  void pass_inline(PassInline &p, std::unique_ptr<Term> self) override;
  void pass_cse(PassCSE &p, std::unique_ptr<Term> self) override;
  void pass_lvl(PassLVL &p, std::unique_ptr<Term> self) override;
  void pass_scope(PassScope &p) override;
};

struct RLit final : public Leaf {
  std::shared_ptr<RootPointer<Value> > value;

  RLit(std::shared_ptr<RootPointer<Value> > value_, const char *label_ = "")
      : Leaf(label_), value(std::move(value_)) {}

  std::unique_ptr<Term> clone(TargetScope &scope, size_t id) const override;
  void format(std::ostream &os, TermFormat &format) const override;
  void interpret(InterpretContext &context) override;
  bool tailCallOk() const override;

  void pass_purity(PassPurity &p) override;
  void pass_usage(PassUsage &p) override;
  void pass_sweep(PassSweep &p) override;
  void pass_inline(PassInline &p, std::unique_ptr<Term> self) override;
  void pass_cse(PassCSE &p, std::unique_ptr<Term> self) override;
  void pass_lvl(PassLVL &p, std::unique_ptr<Term> self) override;
  void pass_scope(PassScope &p) override;
};

struct RApp final : public Redux {
  // arg0 = fn, arg1+ = arguments
  RApp(size_t fn, size_t arg, const char *label_ = "") : Redux(label_, {fn, arg}) {}

  std::unique_ptr<Term> clone(TargetScope &scope, size_t id) const override;
  void format(std::ostream &os, TermFormat &format) const override;
  void interpret(InterpretContext &context) override;
  bool tailCallOk() const override;

  void pass_purity(PassPurity &p) override;
  void pass_usage(PassUsage &p) override;
  void pass_sweep(PassSweep &p) override;
  void pass_inline(PassInline &p, std::unique_ptr<Term> self) override;
  void pass_cse(PassCSE &p, std::unique_ptr<Term> self) override;
  void pass_lvl(PassLVL &p, std::unique_ptr<Term> self) override;
  void pass_scope(PassScope &p) override;
};

struct RPrim final : public Redux {
  std::string name;
  PrimFn fn;
  void *data;
  int pflags;

  RPrim(const char *name_, PrimFn fn_, void *data_, int pflags_, std::vector<size_t> &&args_,
        const char *label_ = "")
      : Redux(label_, std::move(args_)), name(name_), fn(fn_), data(data_), pflags(pflags_) {}

  std::unique_ptr<Term> clone(TargetScope &scope, size_t id) const override;
  void format(std::ostream &os, TermFormat &format) const override;
  void interpret(InterpretContext &context) override;
  bool tailCallOk() const override;

  void pass_purity(PassPurity &p) override;
  void pass_usage(PassUsage &p) override;
  void pass_sweep(PassSweep &p) override;
  void pass_inline(PassInline &p, std::unique_ptr<Term> self) override;
  void pass_cse(PassCSE &p, std::unique_ptr<Term> self) override;
  void pass_lvl(PassLVL &p, std::unique_ptr<Term> self) override;
  void pass_scope(PassScope &p) override;
};

struct RGet final : public Redux {
  // arg0 = object
  size_t index;

  RGet(size_t index_, size_t obj, const char *label_ = "") : Redux(label_, {obj}), index(index_) {}

  std::unique_ptr<Term> clone(TargetScope &scope, size_t id) const override;
  void format(std::ostream &os, TermFormat &format) const override;
  void interpret(InterpretContext &context) override;
  bool tailCallOk() const override;

  void pass_purity(PassPurity &p) override;
  void pass_usage(PassUsage &p) override;
  void pass_sweep(PassSweep &p) override;
  void pass_inline(PassInline &p, std::unique_ptr<Term> self) override;
  void pass_cse(PassCSE &p, std::unique_ptr<Term> self) override;
  void pass_lvl(PassLVL &p, std::unique_ptr<Term> self) override;
  void pass_scope(PassScope &p) override;
};

struct RDes final : public Redux {
  // args = handlers for cases, then obj
  RDes(std::vector<size_t> &&args_, const char *label_ = "") : Redux(label_, std::move(args_)) {}

  std::unique_ptr<Term> clone(TargetScope &scope, size_t id) const override;
  void format(std::ostream &os, TermFormat &format) const override;
  void interpret(InterpretContext &context) override;
  bool tailCallOk() const override;

  void pass_purity(PassPurity &p) override;
  void pass_usage(PassUsage &p) override;
  void pass_sweep(PassSweep &p) override;
  void pass_inline(PassInline &p, std::unique_ptr<Term> self) override;
  void pass_cse(PassCSE &p, std::unique_ptr<Term> self) override;
  void pass_lvl(PassLVL &p, std::unique_ptr<Term> self) override;
  void pass_scope(PassScope &p) override;
};

struct RCon final : public Redux {
  // arg0+ = tuple elements
  std::shared_ptr<Constructor> kind;

  RCon(std::shared_ptr<Constructor> kind_, std::vector<size_t> &&args_, const char *label_ = "")
      : Redux(label_, std::move(args_)), kind(std::move(kind_)) {}

  void format(std::ostream &os, TermFormat &format) const override;
  std::unique_ptr<Term> clone(TargetScope &scope, size_t id) const override;
  void interpret(InterpretContext &context) override;
  bool tailCallOk() const override;

  void pass_purity(PassPurity &p) override;
  void pass_usage(PassUsage &p) override;
  void pass_sweep(PassSweep &p) override;
  void pass_inline(PassInline &p, std::unique_ptr<Term> self) override;
  void pass_cse(PassCSE &p, std::unique_ptr<Term> self) override;
  void pass_lvl(PassLVL &p, std::unique_ptr<Term> self) override;
  void pass_scope(PassScope &p) override;
};

struct RFun final : public Term {
  FileFragment fragment;
  Hash hash;      // unique function identifier
  size_t output;  // output can refer to a non-member Term
  std::vector<std::unique_ptr<Term> > terms;
  std::vector<size_t> escapes;

  RFun(const RFun &o, TargetScope &scope, size_t id);
  RFun(const FileFragment &fragment_, const char *label_, size_t flags_,
       size_t output_ = Term::invalid)
      : Term(label_, flags_), fragment(fragment_), output(output_) {}

  void update(const SourceMap &map);
  size_t args() const;

  std::unique_ptr<Term> clone(TargetScope &scope, size_t id) const override;
  void format(std::ostream &os, TermFormat &format) const override;
  void interpret(InterpretContext &context) override;
  bool tailCallOk() const override;

  void pass_purity(PassPurity &p) override;
  void pass_usage(PassUsage &p) override;
  void pass_sweep(PassSweep &p) override;
  void pass_inline(PassInline &p, std::unique_ptr<Term> self) override;
  void pass_cse(PassCSE &p, std::unique_ptr<Term> self) override;
  void pass_lvl(PassLVL &p, std::unique_ptr<Term> self) override;
  void pass_scope(PassScope &p) override;
};

struct TargetScope {
  // Finish the rewrite and claim term 0
  std::unique_ptr<Term> finish();

  Term *operator[](size_t index);
  size_t append(std::unique_ptr<Term> term);
  size_t append(Term *term) { return append(std::unique_ptr<Term>(term)); }

  size_t end() const;
  std::vector<std::unique_ptr<Term> > unwind(size_t newend);

 private:
  std::vector<std::unique_ptr<Term> > terms;  // new AST
};

inline std::unique_ptr<Term> TargetScope::finish() {
  std::unique_ptr<Term> out = std::move(terms[0]);
  terms.clear();
  return out;
}

inline Term *TargetScope::operator[](size_t index) { return terms[index].get(); }

inline size_t TargetScope::append(std::unique_ptr<Term> term) {
  size_t out = terms.size();
  terms.emplace_back(std::move(term));
  return out;
}

inline size_t TargetScope::end() const { return terms.size(); }

struct SourceMap {
  size_t operator[](size_t index) const;

  // These methods should only be used indirectly via a TermStream:
  SourceMap(size_t start_ = 0);  // The mapping is initialized with [0,start) -> [0,start)

  void place(size_t at);  // in the TargetScope, this Term's index is 'at'

  size_t end() const;
  void unwind(size_t newend);

 private:
  size_t start;
  std::vector<size_t> map;  // from old AST to new AST
};

inline size_t SourceMap::operator[](size_t index) const {
  if (index < start) return index;
  return map[index - start];
}

inline SourceMap::SourceMap(size_t start_) : start(start_) {}

inline void SourceMap::place(size_t at) { map.emplace_back(at); }

inline size_t SourceMap::end() const { return start + map.size(); }

inline void SourceMap::unwind(size_t newend) { map.resize(newend - start); }

struct CheckPoint {
  size_t target;
  size_t source;
  CheckPoint(size_t target_, size_t source_) : target(target_), source(source_) {}
};

struct TermStream {
  // The stream is initialized with the mapping [0,start) -> [0,start)
  TermStream(TargetScope &scope_, size_t start_ = 0);

  // Retrieve the mapping for rewriting Terms
  const SourceMap &map() const;
  SourceMap &map();
  TargetScope &scope();

  // This new TargetScope Term corresponds to a Term from the SourceMap
  size_t transfer(std::unique_ptr<Term> term);
  size_t transfer(Term *term) { return transfer(std::unique_ptr<Term>(term)); }

  // This new TargetScope Term was created from nothing
  size_t include(std::unique_ptr<Term> term);
  size_t include(Term *term) { return include(std::unique_ptr<Term>(term)); }

  // Skip a Term from SourceMap (no Term in TargetScope corresponds)
  void discard(size_t at, bool singleton = false);
  void discard();

  // Retrieve a Term from the TargetScope
  Term *operator[](size_t index);

  // Save the state of the rewriter to mark a function start
  CheckPoint begin() const;
  // Restore the state of the rewriter, popping the function body
  std::vector<std::unique_ptr<Term> > end(CheckPoint cp);

 private:
  TargetScope &tscope;
  SourceMap smap;
};

inline TermStream::TermStream(TargetScope &scope_, size_t start_) : tscope(scope_), smap(start_) {}

inline const SourceMap &TermStream::map() const { return smap; }

inline SourceMap &TermStream::map() { return smap; }

inline TargetScope &TermStream::scope() { return tscope; }

inline size_t TermStream::transfer(std::unique_ptr<Term> term) {
  size_t out = tscope.append(std::move(term));
  smap.place(out);
  return out;
}

inline size_t TermStream::include(std::unique_ptr<Term> term) {
  return tscope.append(std::move(term));
}

inline void TermStream::discard(size_t at, bool singleton) {
  if (!singleton) tscope[at]->set(SSA_SINGLETON, false);
  smap.place(at);
}

inline void TermStream::discard() { smap.place(Term::invalid); }

inline Term *TermStream::operator[](size_t index) { return tscope[index]; }

inline CheckPoint TermStream::begin() const { return CheckPoint(tscope.end(), smap.end()); }

inline std::vector<std::unique_ptr<Term> > TermStream::end(CheckPoint cp) {
  smap.unwind(cp.source);
  return tscope.unwind(cp.target);
}

struct ScopeAnalysis {
  size_t last();
  Term *operator[](size_t i);
  void push(Term *term);
  void pop(size_t n = 1);

 private:
  std::vector<Term *> scope;
};

inline size_t ScopeAnalysis::last() { return scope.size() - 1; }

inline Term *ScopeAnalysis::operator[](size_t i) { return scope[i]; }

inline void ScopeAnalysis::push(Term *term) { scope.emplace_back(term); }

inline void ScopeAnalysis::pop(size_t n) { scope.resize(scope.size() - n); }

#endif
