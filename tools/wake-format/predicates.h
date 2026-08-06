/*
 * Copyright 2022 SiFive, Inc.
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

#pragma once

#include <wcl/doc.h>

#include <bitset>

#include "parser/cst.h"
#include "types.h"

// TryPredicate Predicates
class DocFitsFirstPred {
 public:
  bool operator()(const wcl::doc_builder& builder, ctx_t ctx, wcl::doc doc) {
    return ctx.sub(builder)->last_width() + doc->first_width() <= MAX_COLUMN_WIDTH;
  }
};

class DocFitsAllPred {
 public:
  bool operator()(const wcl::doc_builder& builder, ctx_t ctx, wcl::doc doc) {
    return ctx.sub(builder)->last_width() + doc->first_width() <= MAX_COLUMN_WIDTH &&
           !doc->has_newline();
  }
};

// Accepts a doc that fits on the current line and puts no *content* on a following line.
//
// A single trailing newline is allowed because it carries no content. That is how a trailing
// comment is emitted, so 'def x = y # comment' stays on one line while 'def x = f\na\nb' does not.
// This is the "weakly flat" notion of is_weakly_flat(), measured from the doc alone.
class DocFitsWeaklyFlatPred {
 public:
  bool operator()(const wcl::doc_builder& builder, ctx_t ctx, wcl::doc doc) {
    if (ctx.sub(builder)->last_width() + doc->first_width() > MAX_COLUMN_WIDTH) {
      return false;
    }
    return !doc->has_newline() || (doc->newline_count() == 1 && doc->last_width() == 0);
  }
};

// fmt_if() style predicates
class IsWSNLCPredicate {
 public:
  bool operator()(wcl::doc_builder& builder, ctx_t ctx, CSTElement& node,
                  const token_traits_map_t& traits) const {
    return operator()(node);
  }

  bool operator()(const CSTElement& node) const {
    return node.id() == TOKEN_WS || node.id() == TOKEN_NL || node.id() == TOKEN_COMMENT;
  }
};

class IsNodeEmptyPredicate {
 public:
  bool operator()(wcl::doc_builder& builder, ctx_t ctx, CSTElement& node,
                  const token_traits_map_t& traits) const {
    return operator()(node);
  }

  bool operator()(const CSTElement& node) const { return node.empty(); }
};

class ConstPredicate {
 private:
  bool result;

 public:
  ConstPredicate(bool result) : result(result) {}
  bool operator()(wcl::doc_builder& builder, ctx_t ctx, CSTElement& node,
                  const token_traits_map_t& traits) {
    return result;
  }
};

template <class Predicate, class FMT>
class TryPredicate {
 private:
  Predicate predicate;
  FMT formatter;

 public:
  TryPredicate(Predicate predicate, FMT formatter) : predicate(predicate), formatter(formatter) {}

  bool operator()(wcl::doc_builder& builder, ctx_t ctx, CSTElement& node,
                  const token_traits_map_t& traits) {
    CSTElement copy = node;
    wcl::doc doc = formatter.compose(ctx.sub(builder), copy, traits);

    const wcl::doc_builder& safe_builder = builder;
    return predicate(safe_builder, ctx, std::move(doc));
  }
};

template <class A, class B>
struct DepDeclType {
  using type = A;
};

template <class B>
struct DepDeclType<ctx_t, B> {
  using type = B;
};

template <class Predicate>
struct FmtPredicate {
  Predicate predicate;
  FmtPredicate(Predicate predicate) : predicate(predicate) {}

  template <
      class CTX,
      std::enable_if_t<
          std::is_same<bool, decltype(std::declval<typename DepDeclType<CTX, Predicate>::type>()(
                                 cst_id_t()))>::value,
          bool> = true>
  bool operator()(wcl::doc_builder& builder, CTX ctx, CSTElement& node,
                  const token_traits_map_t& traits) {
    return predicate(node.id());
  }

  template <
      class CTX,
      std::enable_if_t<
          std::is_same<bool,
                       decltype(std::declval<typename DepDeclType<CTX, Predicate>::type>()(
                           std::declval<wcl::doc_builder&>(), ctx_t(), std::declval<CSTElement&>(),
                           std::declval<const token_traits_map_t&>()))>::value,
          bool> = true>
  bool operator()(wcl::doc_builder& builder, CTX ctx, CSTElement& node,
                  const token_traits_map_t& traits) {
    return predicate(builder, ctx, node, traits);
  }
};

template <>
struct FmtPredicate<cst_id_t> {
  cst_id_t id;
  FmtPredicate(cst_id_t id) : id(id) {}
  bool operator()(wcl::doc_builder& builder, ctx_t ctx, CSTElement& node,
                  const token_traits_map_t& traits) {
    return node.id() == id;
  }
};

template <>
struct FmtPredicate<int> {
  int id;
  FmtPredicate(int id) : id(id) {}
  bool operator()(wcl::doc_builder& builder, ctx_t ctx, CSTElement& node,
                  const token_traits_map_t& traits) {
    return node.id() == id;
  }
};

// if sizeof(cst_id_t) is increased then
// std::bitset<256> set from FmtPredicate needs to be increased
static_assert(sizeof(cst_id_t) == 1, "bitset size must match type size");

template <class T>
struct FmtPredicate<std::initializer_list<T>> {
  std::bitset<256> set;
  FmtPredicate(std::initializer_list<T> ids) {
    for (T id : ids) {
      set[id] = true;
    }
  }

  bool operator()(wcl::doc_builder& builder, ctx_t ctx, CSTElement& node,
                  const token_traits_map_t& traits) {
    return set[node.id()];
  }
};
