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

#include <algorithm>
#include <unordered_map>

#include "runtime/runtime.h"
#include "runtime/value.h"
#include "ssa.h"

namespace std {
template <>
struct hash<Hash> {
  size_t operator()(Hash x) const { return x.data[0]; }
};
}  // namespace std

struct PassCSE {
  TermStream stream;
  std::vector<Hash> *undo;
  std::vector<size_t> starts;
  std::unordered_map<Hash, size_t> table;
  Runtime &runtime;

  PassCSE(TargetScope &scope, std::vector<Hash> *undo_, Runtime &runtime_)
      : stream(scope), undo(undo_), runtime(runtime_) {}
};

static Hash hash_arg(PassCSE &p, size_t input) {
  // upper_bound = first index > input; -- for last index <= input
  auto it = --std::upper_bound(p.starts.begin(), p.starts.end(), input);
  size_t depth = it - p.starts.begin();
  size_t offset = input - *it;
  return Hash(depth, offset);
}

static Hash hash_redux(PassCSE &p, Redux *redux, size_t type) {
  std::vector<uint64_t> codes;
  size_t num = redux->args.size();
  codes.reserve(num * 2 + 2);
  codes.push_back(type);
  codes.push_back(num);
  for (auto x : redux->args) hash_arg(p, x).push(codes);
  return Hash(codes);
}

static Hash hash_redux(PassCSE &p, Redux *redux, size_t type, Hash hash) {
  std::vector<uint64_t> codes;
  size_t num = redux->args.size();
  codes.reserve(num * 2 + 4);
  codes.push_back(type);
  codes.push_back(num);
  hash.push(codes);
  for (auto x : redux->args) hash_arg(p, x).push(codes);
  return Hash(codes);
}

static void cse_reduce(PassCSE &p, Hash hash, std::unique_ptr<Term> self) {
  auto ins = p.table.insert(std::make_pair(hash, p.stream.scope().end()));
  if (ins.second || self->get(SSA_ORDERED)) {
    p.undo->push_back(hash);
    p.stream.transfer(std::move(self));
  } else {
    size_t prior = ins.first->second;
    p.stream.discard(prior);
  }
}

void RArg::pass_cse(PassCSE &p, std::unique_ptr<Term> self) { p.stream.transfer(std::move(self)); }

void RLit::pass_cse(PassCSE &p, std::unique_ptr<Term> self) {
  HeapObject *obj = value->get();
  // Tagged small-Integer pointers carry no object to dispatch on; tag both
  // representations as Integer so the CSE bucket is consistent.
  std::size_t type_code =
      is_small_int(obj) ? typeid(Integer).hash_code() : typeid(*obj).hash_code();
  Hash h = Hash(typeid(RLit).hash_code(), type_code) + (*value)->deep_hash(p.runtime.heap);
  cse_reduce(p, h, std::move(self));
}

void RApp::pass_cse(PassCSE &p, std::unique_ptr<Term> self) {
  update(p.stream.map());
  Hash h = hash_redux(p, this, typeid(RApp).hash_code());
  cse_reduce(p, h, std::move(self));
}

void RPrim::pass_cse(PassCSE &p, std::unique_ptr<Term> self) {
  update(p.stream.map());
  Hash g = Hash(name);
  Hash h = hash_redux(p, this, typeid(RPrim).hash_code(), g);
  cse_reduce(p, h, std::move(self));
}

void RGet::pass_cse(PassCSE &p, std::unique_ptr<Term> self) {
  update(p.stream.map());
  Hash g = Hash(index);
  Hash h = hash_redux(p, this, typeid(RGet).hash_code(), g);
  cse_reduce(p, h, std::move(self));
}

void RDes::pass_cse(PassCSE &p, std::unique_ptr<Term> self) {
  update(p.stream.map());
  Hash h = hash_redux(p, this, typeid(RDes).hash_code());
  cse_reduce(p, h, std::move(self));
}

void RCon::pass_cse(PassCSE &p, std::unique_ptr<Term> self) {
  update(p.stream.map());
  Hash g = Hash(kind->ast.name);
  Hash h = hash_redux(p, this, typeid(RCon).hash_code(), g);
  cse_reduce(p, h, std::move(self));
}

void RFun::pass_cse(PassCSE &p, std::unique_ptr<Term> self) {
  CheckPoint fun = p.stream.begin();
  // We include our definition as something we own so that
  // a recursive function does not depend on it's parent offset
  p.starts.push_back(fun.target);
  p.stream.transfer(std::move(self));
  CheckPoint body = p.stream.begin();

  size_t args = 0;
  std::vector<Hash> undo;
  std::vector<Hash> *save = p.undo;
  p.undo = &undo;

  for (auto &x : terms) {
    if (x->id() == typeid(RArg)) ++args;
    x->pass_cse(p, std::move(x));
  }
  update(p.stream.map());
  terms = p.stream.end(body);

  std::vector<uint64_t> codes;
  codes.reserve(undo.size() * 2 + 6);
  codes.push_back(typeid(RFun).hash_code());
  codes.push_back(p.starts.size());  // See comment [*]
  codes.push_back(args);
  codes.push_back(terms.size());
  hash_arg(p, output).push(codes);
  for (auto x : undo) {
    x.push(codes);
    p.table.erase(x);
  }

  // [*] CSE cannot merge functions of different depths,
  //     because we hash only depth:offset references.
  //     Thus, fn1's 1:0 might be his argument, whereas
  //     fn2's 1:0 might be his parent's argument.
  //     Fortunately, lifting equalizes identical fn levels.

  p.undo = save;
  auto me = p.stream.end(fun);
  p.starts.pop_back();
  cse_reduce(p, Hash(codes), std::move(me[0]));
}

std::unique_ptr<Term> Term::pass_cse(std::unique_ptr<Term> term, Runtime &runtime) {
  TargetScope scope;
  std::vector<Hash> undo;
  PassCSE pass(scope, &undo, runtime);
  term->pass_cse(pass, std::move(term));
  return scope.finish();
}
