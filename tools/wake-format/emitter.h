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
#include <wcl/hash.h>

#include <cassert>
#include <iostream>
#include <optional>
#include <unordered_map>
#include <vector>

#include "formatter.h"
#include "parser/cst.h"

template <>
struct std::hash<std::pair<CSTElement, ctx_t>> {
  size_t operator()(std::pair<CSTElement, ctx_t> const& pair) const noexcept {
    return wcl::hash_combine(std::hash<CSTElement>{}(pair.first), std::hash<ctx_t>{}(pair.second));
  }
};

struct node_traits_t {
  bool format_off = false;
  bool top_level = false;

  void turn_format_off() { format_off = true; }
  void set_top_level() { top_level = true; }
};

class Emitter {
 public:
  // Walks the CST, formats it, and returns the representative doc
  wcl::doc layout(CST cst);

  ~Emitter();

 private:
  std::unordered_map<CSTElement, node_traits_t> node_traits = {};
  token_traits_map_t token_traits = {};

  // Do upkeep and dispatch to the provided function
  template <class Func>
  wcl::doc dispatch(ctx_t ctx, CSTElement node, Func func);

  // Top level tree walk. Dispatches out the calls for various nodes
  wcl::doc walk(ctx_t ctx, CSTElement node);

  wcl::doc walk_node(ctx_t ctx, CSTElement node);
  wcl::doc walk_token(ctx_t ctx, CSTElement node);

  // Walks a node and emits it without any formatting.
  wcl::doc walk_no_edit(ctx_t ctx, CSTElement node);
  wcl::doc walk_no_edit_acc(ctx_t ctx, CSTElement node);

  // Marks all node elements that have had their formatting disabled
  void mark_no_format_nodes(CSTElement node);

  // Marks top level nodes. Any node on 'top level' will have top level = true
  void mark_top_level_nodes(CSTElement node);

  // Binds all comments in the tree to their associated token as a human would consider it.
  //
  // A comment following a token without a newline is bound to that token
  // Ex:
  //   'def x = 5 # a number'
  //   will bind '# a number' to '5'
  //
  // A comment with a newline between it and the previous token gets bound to the first
  // non-whitespace, non-newline, non-comment following the it.
  // Ex:
  //   '''
  //   def x = 5
  //   # comment1
  //   # comment2
  //   def y = 4
  //   '''
  //   will bind '# comment1' and '# comment2' to the second 'def'
  void bind_comments(CSTElement node);
  void bind_top_level_comments(CSTElement node);
  void bind_nested_comments(CSTElement node);

  // Functions to combine binops using various 'full choice' options
  //
  // defs:
  //   combine flat: combine the LHS and RHS without NLs. Spaces may be added as appropiate per OP
  //   combine explode: combine the LHS and RHS with at least one NL
  //   with prefer_explode: ctx.prefer_explode = true
  //   with !prefer_explode: ctx.prefer_explode = false
  //
  // (1) combine_flat: format all elements with !prefer_explode, combine flat
  // (2) combine_explode_first: first element prefer_explode, rest !prefer_explode, combine explode
  // (3) combine_explode_last: last element prefer_explode, rest !prefer_explode, combine explode
  // (4) combine_explode_all: all elements prefer_explode, combine explode
  // (5) combine_explode_first_compress: first element prefer_explode, rest !prefer_explode, combine
  //     flat
  // (6) combine_explode_last_compress: last element prefer_explode, rest !prefer_explode, combine
  //     flat
  //
  // Making the 'full choice'
  //
  // - format all 6 options and apply the following rules
  //   - if 1, 5, or 6 have a NL, remove them from consideration
  //   - divide the remain options into two groups.
  //     - GT = options where max_width > MAX_COL_WIDTH
  //     - LTE = options where max_width <= MAX_COL_WIDTH
  //     if LTE has any elements choose the element with the smallest height
  //     otherwise choose the element with the smallest width from GT
  //
  std::optional<wcl::doc> combine_flat(ctx_t ctx, const std::vector<CSTElement>& parts);
  std::optional<wcl::doc> combine_explode_first(ctx_t ctx, const std::vector<CSTElement>& parts);
  std::optional<wcl::doc> combine_explode_last(ctx_t ctx, const std::vector<CSTElement>& parts);
  std::optional<wcl::doc> combine_explode_all(ctx_t ctx, const std::vector<CSTElement>& parts);
  std::optional<wcl::doc> combine_explode_first_compress(ctx_t ctx,
                                                         const std::vector<CSTElement>& parts);
  std::optional<wcl::doc> combine_explode_last_compress(ctx_t ctx,
                                                        const std::vector<CSTElement>& parts);

  // Functions to combine apply using various 'full choice' options
  //
  std::optional<wcl::doc> combine_apply_flat(ctx_t ctx, const std::vector<CSTElement>& parts);
  std::optional<wcl::doc> combine_apply_constructor(ctx_t ctx,
                                                    const std::vector<CSTElement>& parts);
  std::optional<wcl::doc> combine_apply_explode_all(ctx_t ctx,
                                                    const std::vector<CSTElement>& parts);
  std::optional<wcl::doc> combine_apply_pattern(ctx_t ctx, const std::vector<CSTElement>& parts);

  // Returns a formatter that inserts the next node
  // on the current line if it fits, or on a new nested line
  auto rhs_fmt(bool always_newline = false);

  // Returns a formatter that consumes a pattern continuing until *stop_at* is seen. Pattern is
  // separated by spaces if possible, freshlines otherwise
  auto pattern_fmt(cst_id_t stop_at);

  // Returns a doc with a binop surrounded by the appropiate separation. This depends heavily on
  // the context and which operator is being used.
  //
  // Ex:
  // "flat comma": ', '
  // "flat or": ' | '
  // "flat dot": '.'
  // "explode or": '\n| '
  // "explode comma": ',\n'
  //
  // When an operator grows a NL, the rules are slightly different as the spacing is forced.
  //
  // Ex:
  //  '''x
  //     + # comment
  // <FR>5
  //  '''
  wcl::doc place_binop(CSTElement op, bool is_flat, ctx_t ctx);

  wcl::doc walk_apply(ctx_t ctx, CSTElement node);
  wcl::doc walk_arity(ctx_t ctx, CSTElement node);
  wcl::doc walk_ascribe(ctx_t ctx, CSTElement node);
  wcl::doc walk_binary(ctx_t ctx, CSTElement node);
  wcl::doc walk_block(ctx_t ctx, CSTElement node);
  wcl::doc walk_case(ctx_t ctx, CSTElement node);
  wcl::doc walk_data(ctx_t ctx, CSTElement node);
  wcl::doc walk_def(ctx_t ctx, CSTElement node);
  wcl::doc walk_export(ctx_t ctx, CSTElement node);
  wcl::doc walk_flag_export(ctx_t ctx, CSTElement node);
  wcl::doc walk_flag_global(ctx_t ctx, CSTElement node);
  wcl::doc walk_guard(ctx_t ctx, CSTElement node);
  wcl::doc walk_hole(ctx_t ctx, CSTElement node);
  wcl::doc walk_identifier(ctx_t ctx, CSTElement node);
  wcl::doc walk_ideq(ctx_t ctx, CSTElement node);
  wcl::doc walk_if(ctx_t ctx, CSTElement node);
  wcl::doc walk_import(ctx_t ctx, CSTElement node);
  wcl::doc walk_interpolate(ctx_t ctx, CSTElement node);
  wcl::doc walk_kind(ctx_t ctx, CSTElement node);
  wcl::doc walk_lambda(ctx_t ctx, CSTElement node);
  wcl::doc walk_literal(ctx_t ctx, CSTElement node);
  wcl::doc walk_literal(ctx_t ctx, CSTElement node, size_t whitespace_prefix);
  wcl::doc walk_match(ctx_t ctx, CSTElement node);
  wcl::doc walk_op(ctx_t ctx, CSTElement node);
  wcl::doc walk_package(ctx_t ctx, CSTElement node);
  wcl::doc walk_paren(ctx_t ctx, CSTElement node);
  wcl::doc walk_prim(ctx_t ctx, CSTElement node);
  wcl::doc walk_publish(ctx_t ctx, CSTElement node);
  wcl::doc walk_require(ctx_t ctx, CSTElement node);
  wcl::doc walk_req_else(ctx_t ctx, CSTElement node);
  wcl::doc walk_subscribe(ctx_t ctx, CSTElement node);
  wcl::doc walk_target(ctx_t ctx, CSTElement node);
  wcl::doc walk_target_args(ctx_t ctx, CSTElement node);
  wcl::doc walk_top(ctx_t ctx, CSTElement node);
  wcl::doc walk_topic(ctx_t ctx, CSTElement node);
  wcl::doc walk_tuple(ctx_t ctx, CSTElement node);
  wcl::doc walk_tuple_elt(ctx_t ctx, CSTElement node);
  wcl::doc walk_type(ctx_t ctx, CSTElement node);
  wcl::doc walk_unary(ctx_t ctx, CSTElement node);
  wcl::doc walk_error(ctx_t ctx, CSTElement node);

  wcl::doc walk_placeholder(ctx_t ctx, CSTElement node);
};
