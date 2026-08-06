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

#include "actions.h"
#include "catters.h"
#include "parser/cst.h"
#include "parser/syntax.h"
#include "predicates.h"
#include "types.h"

template <class Catter>
struct ConcatFormatter {
  Catter catter;

  ConcatFormatter(Catter c) : catter(c) {}

  ConcatFormatter<SeqCatter<Catter, SpaceCatter>> space(uint8_t count = 1) {
    return {{catter, {count}}};
  }

  ConcatFormatter<SeqCatter<Catter, NewlineCatter>> newline() { return {{catter, {}}}; }

  ConcatFormatter<SeqCatter<Catter, FreshlineCatter>> freshline() { return {{catter, {}}}; }

  ConcatFormatter<SeqCatter<Catter, BreaklineCatter>> breakline() { return {{catter, {}}}; }

  ConcatFormatter<SeqCatter<Catter, LiteralCatter>> lit(wcl::doc lit) { return {{catter, {lit}}}; }

  template <class CTR>
  ConcatFormatter<SeqCatter<Catter, NestCatter<CTR>>> nest(CTR ctr) {
    return {{catter, {ctr}}};
  }

  template <class CTR>
  ConcatFormatter<SeqCatter<Catter, PreferExplodeCatter<CTR>>> prefer_explode(CTR ctr) {
    return {{catter, {ctr}}};
  }

  template <class CTR>
  ConcatFormatter<SeqCatter<Catter, PreventExplodeCatter<CTR>>> prevent_explode(CTR ctr) {
    return {{catter, {ctr}}};
  }

  template <class CTR>
  ConcatFormatter<SeqCatter<Catter, AllowExplodeCatter<CTR>>> allow_explode(CTR ctr) {
    return {{catter, {ctr}}};
  }

  template <class CTR>
  ConcatFormatter<SeqCatter<Catter, JoinCatter<CTR>>> join(CTR ctr) {
    return {{catter, {ctr}}};
  }

  template <class FMT>
  ConcatFormatter<SeqCatter<Catter, FormatCatter<FMT>>> fmt(CSTElement node,
                                                            const token_traits_map_t& traits,
                                                            FMT formatter) {
    return {{catter, {formatter, node, traits}}};
  }

  wcl::doc concat(ctx_t ctx) {
    wcl::doc_builder builder;
    catter.cat(builder, ctx);
    return std::move(builder).build();
  }
};

inline ConcatFormatter<EpsilonCatter> cat() { return {{}}; }

template <class Action>
struct Formatter {
  Action action;

  Formatter(Action a) : action(a) {}

  Formatter<SeqAction<Action, ConsumeWhitespaceAction>> consume_wsnlc() { return {{action, {}}}; }

  Formatter<SeqAction<Action, WhitespaceTokenAction>> ws() { return {{action, {}}}; }

  Formatter<SeqAction<Action, SpaceAction>> space(uint8_t count = 1) { return {{action, {count}}}; }

  // Unconditionally inserts a \n
  Formatter<SeqAction<Action, NewlineAction>> newline() { return {{action, {}}}; }

  // Moves to the next "best" position for a "fresh line"
  //
  // A freshline is defined as a line with exactly nest level * space per nest whitespaces
  // The minimal amount of emission to complete this is done.
  //
  // Ex: (assume nest level = 1, space = 4)
  //
  // line = ""  -> emits "    " so line = "    "
  // line = "    text" -> emits "\n    " so line = "    "
  // line = "  " -> emits "  " so line = "    "
  Formatter<SeqAction<Action, FreshlineAction>> freshline() { return {{action, {}}}; }

  // Ensures a "break" between the previous emitted content and the emit pointer.
  // The previous content does not require a newline as it is auto detected.
  // Each call to breakline will increase the break size by one
  //
  // A break is defined as two lines with a \n between them (Ex: line\n\nline\n)
  // The minimal amount of emission to complete this is done.
  //
  // Ex:
  // line = "text"  -> emits "\n\n"
  // line = "text\n"  -> emits "\n"
  // line = "text\n\n" -> emits "\n"
  Formatter<SeqAction<Action, BreaklineAction>> breakline() { return {{action, {}}}; }

  Formatter<SeqAction<Action, LiteralAction>> lit(wcl::doc lit) { return {{action, {lit}}}; }

  Formatter<SeqAction<Action, TokenReplaceAction>> token(cst_id_t id) { return {{action, {id}}}; }

  Formatter<SeqAction<Action, TokenReplaceAction>> token(cst_id_t id, const char* str) {
    return {{action, {id, str}}};
  }

  template <class FMT>
  Formatter<SeqAction<Action, NestAction<FMT>>> nest(FMT formatter) {
    return {{action, {formatter}}};
  }

  template <class FMT>
  Formatter<SeqAction<Action, PreferExplodeAction<FMT>>> prefer_explode(FMT formatter) {
    return {{action, {formatter}}};
  }

  template <class FMT>
  Formatter<SeqAction<Action, PreventExplodeAction<FMT>>> prevent_explode(FMT formatter) {
    return {{action, {formatter}}};
  }

  template <class FMT>
  Formatter<SeqAction<Action, AllowExplodeAction<FMT>>> allow_explode(FMT formatter) {
    return {{action, {formatter}}};
  }

  Formatter<SeqAction<Action, NextAction>> next() { return {{action, {}}}; }

  template <class FMT>
  Formatter<SeqAction<Action, IfElseAction<FmtPredicate<std::initializer_list<cst_id_t>>, FMT,
                                           Formatter<EpsilonAction>>>>
  fmt_if(std::initializer_list<cst_id_t> ids, FMT formatter) {
    return fmt_if_else(ids, formatter, Formatter<EpsilonAction>({}));
  }

  template <class FMT, class Predicate>
  Formatter<SeqAction<Action, IfElseAction<FmtPredicate<Predicate>, FMT, Formatter<EpsilonAction>>>>
  fmt_if(Predicate predicate, FMT formatter) {
    return fmt_if_else(predicate, formatter, Formatter<EpsilonAction>({}));
  }

  template <class IFMT, class EFMT>
  Formatter<
      SeqAction<Action, IfElseAction<FmtPredicate<std::initializer_list<cst_id_t>>, IFMT, EFMT>>>
  fmt_if_else(std::initializer_list<cst_id_t> ids, IFMT if_formatter, EFMT else_formatter) {
    return {{action, {ids, if_formatter, else_formatter}}};
  }

  template <class IFMT, class EFMT, class Predicate>
  Formatter<SeqAction<Action, IfElseAction<FmtPredicate<Predicate>, IFMT, EFMT>>> fmt_if_else(
      Predicate predicate, IFMT if_formatter, EFMT else_formatter) {
    return {{action, {predicate, if_formatter, else_formatter}}};
  }

  template <class IFMT, class EFMT, class Predicate>
  Formatter<
      SeqAction<Action, IfElseAction<FmtPredicate<TryPredicate<Predicate, IFMT>>, IFMT, EFMT>>>
  fmt_try_else(Predicate predicate, IFMT try_formatter, EFMT else_formatter) {
    return fmt_if_else(TryPredicate<Predicate, IFMT>(predicate, try_formatter), try_formatter,
                       else_formatter);
  }

  template <class IFMT, class EFMT>
  Formatter<SeqAction<Action,
                      IfElseAction<FmtPredicate<TryPredicate<DocFitsFirstPred, IFMT>>, IFMT, EFMT>>>
  fmt_if_fits_first(IFMT fits_formatter, EFMT else_formatter) {
    return fmt_try_else(DocFitsFirstPred(), fits_formatter, else_formatter);
  }

  template <class IFMT, class EFMT>
  Formatter<
      SeqAction<Action, IfElseAction<FmtPredicate<TryPredicate<DocFitsAllPred, IFMT>>, IFMT, EFMT>>>
  fmt_if_fits_all(IFMT fits_formatter, EFMT else_formatter) {
    return fmt_try_else(DocFitsAllPred(), fits_formatter, else_formatter);
  }

  template <class IFMT, class EFMT>
  Formatter<SeqAction<
      Action, IfElseAction<FmtPredicate<TryPredicate<DocFitsWeaklyFlatPred, IFMT>>, IFMT, EFMT>>>
  fmt_if_fits_weakly_flat(IFMT fits_formatter, EFMT else_formatter) {
    return fmt_try_else(DocFitsWeaklyFlatPred(), fits_formatter, else_formatter);
  }

  template <class FMT>
  Formatter<SeqAction<Action, WhileAction<FmtPredicate<std::initializer_list<cst_id_t>>, FMT>>>
  fmt_while(std::initializer_list<cst_id_t> ids, FMT formatter) {
    return {{action, {ids, formatter}}};
  }

  template <class FMT, class Predicate>
  Formatter<SeqAction<Action, WhileAction<FmtPredicate<Predicate>, FMT>>> fmt_while(
      Predicate predicate, FMT formatter) {
    return {{action, {predicate, formatter}}};
  }

  template <class Case>
  Formatter<SeqAction<Action, MatchAction<Case>>> match(MatchAction<Case> match_action) {
    return {{action, match_action}};
  }

  template <class Walker>
  Formatter<SeqAction<Action, WalkPredicateAction<FmtPredicate<ConstPredicate>, Walker>>> walk(
      Walker texas_ranger) {
    return walk(ConstPredicate(true), texas_ranger);
  }

  template <class Walker>
  Formatter<
      SeqAction<Action, WalkPredicateAction<FmtPredicate<std::initializer_list<cst_id_t>>, Walker>>>
  walk(std::initializer_list<cst_id_t> ids, Walker texas_ranger) {
    return {{action, {ids, texas_ranger}}};
  }

  template <class Predicate, class Walker>
  Formatter<SeqAction<Action, WalkPredicateAction<FmtPredicate<Predicate>, Walker>>> walk(
      Predicate predicate, Walker texas_ranger) {
    return {{action, {predicate, texas_ranger}}};
  }

  template <class FMT>
  Formatter<SeqAction<Action, WalkAllAction<FMT>>> walk_all(FMT formatter) {
    return {{action, {formatter}}};
  }

  template <class FMT>
  Formatter<SeqAction<Action, JoinAction<FMT>>> join(FMT formatter) {
    return {{action, {formatter}}};
  }

  template <class F>
  Formatter<SeqAction<Action, EscapeAction<F>>> escape(F f) {
    return {{action, {f}}};
  }

  template <class F, class FMT>
  Formatter<SeqAction<Action, ChangeContextAction<F, FMT>>> ctx(F f, FMT formatter) {
    return {{action, {f, formatter}}};
  }

  wcl::doc format(ctx_t ctx, CSTElement node, const token_traits_map_t& traits) {
    wcl::doc_builder builder;
    action.run(builder, ctx, node, traits);

    FMT_ASSERT(node.empty(), node, "Not empty <" + std::string(symbolName(node.id())) + ">");

    return std::move(builder).build();
  }

  wcl::doc compose(ctx_t ctx, CSTElement& node, const token_traits_map_t& traits) {
    wcl::doc_builder builder;
    action.run(builder, ctx, node, traits);
    return std::move(builder).build();
  }
};

inline Formatter<EpsilonAction> fmt() { return {{}}; }

template <class Predicate, class FMT>
inline MatchAction<PredicateCase<FmtPredicate<Predicate>, FMT>> pred(Predicate predicate,
                                                                     FMT formatter) {
  return {{predicate, formatter}};
}
