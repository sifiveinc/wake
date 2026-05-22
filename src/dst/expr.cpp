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

#include "expr.h"

#include <cassert>
#include <sstream>

#include "util/diagnostic.h"

CPPFile expr_h("src/dst/expr.h");
static CPPFile cppFile(__FILE__);

Expr::~Expr() {}
const TypeDescriptor Prim::type("Prim");
const TypeDescriptor App::type("App");
const TypeDescriptor Lambda::type("Lambda");
const TypeDescriptor VarRef::type("VarRef");
const TypeDescriptor Literal::type("Literal");
const TypeDescriptor DefBinding::type("DefBinding");
const TypeDescriptor Get::type("Get");
const TypeDescriptor Construct::type("Construct");
const TypeDescriptor Destruct::type("Destruct");
// these are removed by bind
const TypeDescriptor Subscribe::type("Subscribe");
const TypeDescriptor Ascribe::type("Ascribe");
const TypeDescriptor Match::type("Match");
const TypeDescriptor DefMap::type("DefMap");
// these are just useful for dumping json ast
const TypeDescriptor VarDef::type("VarDef");
const TypeDescriptor VarArg::type("VarArg");

Top::Top() : packages(), globals(), def_package(nullptr) {
  Package *builtin = new Package();
  packages.insert(std::make_pair("builtin", std::unique_ptr<Package>(builtin)));

  // These types can be constructed by literals, so must always be in scope!
  builtin->package.types.insert(
      std::make_pair("String", SymbolSource(FRAGMENT_CPP_LINE, "String@builtin", SYM_LEAF)));
  builtin->package.types.insert(
      std::make_pair("Integer", SymbolSource(FRAGMENT_CPP_LINE, "Integer@builtin", SYM_LEAF)));
  builtin->package.types.insert(
      std::make_pair("Double", SymbolSource(FRAGMENT_CPP_LINE, "Double@builtin", SYM_LEAF)));
  builtin->package.types.insert(
      std::make_pair("RegExp", SymbolSource(FRAGMENT_CPP_LINE, "RegExp@builtin", SYM_LEAF)));
  builtin->package.types.insert(
      std::make_pair("binary =>", SymbolSource(FRAGMENT_CPP_LINE, "binary =>@builtin", SYM_LEAF)));
  globals = builtin->package;

  // These types come from the runtime.
  builtin->package.types.insert(
      std::make_pair("Array", SymbolSource(FRAGMENT_CPP_LINE, "Array@builtin", SYM_LEAF)));
  builtin->package.types.insert(
      std::make_pair("Job", SymbolSource(FRAGMENT_CPP_LINE, "Job@builtin", SYM_LEAF)));
  builtin->package.types.insert(
      std::make_pair("Time", SymbolSource(FRAGMENT_CPP_LINE, "Time@builtin", SYM_LEAF)));
  builtin->exports = builtin->package;
}

Literal::Literal(const FileFragment &fragment_, std::string &&value_, TypeVar *litType_)
    : Expr(&type, fragment_), value(std::move(value_)), litType(litType_) {}

static std::string pad(int depth) { return std::string(depth, ' '); }

std::string Expr::to_str() const {
  std::stringstream str;
  str << this;
  return str.str();
}

void VarRef::format(std::ostream &os, int depth) const {
  os << pad(depth) << "VarRef(";
  os << meta << ", ";
  if (index != -1) os << name << "," << index;
  os << "): " << typeVar << " @ " << fragment.location() << std::endl;
}

void Subscribe::format(std::ostream &os, int depth) const {
  os << pad(depth) << "Subscribe(" << name << ") @ " << fragment.location() << std::endl;
}

void Ascribe::format(std::ostream &os, int depth) const {
  os << pad(depth) << "Ascribe @ " << fragment.location() << std::endl;
  os << pad(depth + 2) << "signature = " << signature << std::endl;
  body->format(os, depth + 2);
}

void Match::format(std::ostream &os, int depth) const {
  os << pad(depth) << "Match: " << typeVar << " @ " << fragment.location() << std::endl;
  for (auto &a : args) a->format(os, depth + 2);
  for (auto &p : patterns) {
    os << pad(depth + 2) << p.pattern << " = " << std::endl;
    p.expr->format(os, depth + 4);
    if (p.guard) {
      os << pad(depth + 2) << "if" << std::endl;
      p.guard->format(os, depth + 4);
    }
  }
}

void App::format(std::ostream &os, int depth) const {
  os << pad(depth) << "App: ";
  os << meta << " ";
  os << typeVar << " @ " << fragment.location() << std::endl;
  fn->format(os, depth + 2);
  val->format(os, depth + 2);
}

void Lambda::format(std::ostream &os, int depth) const {
  os << pad(depth) << "Lambda(";
  os << meta << " " << name;
  if (!fnname.empty()) os << ", " << fnname;
  os << " @ " << token.location() << "): " << typeVar << " @ " << fragment.location() << std::endl;
  body->format(os, depth + 2);
}

void Symbols::format(const char *kind, std::ostream &os, int depth) const {
  for (auto &i : defs) {
    os << pad(depth) << kind << " " << i.first << " = " << i.second.qualified << std::endl;
  }
}

static bool smap_join(Symbols::SymbolMap &dest, const Symbols::SymbolMap &src, const char *scope,
                      const char *kind) {
  bool ok = true;
  for (auto &sym : src) {
    auto it = dest.insert(sym);
    if (!it.second) {
      ok = false;
      if (scope) {
        ERROR(sym.second.fragment.location(), scope << " " << kind << " '" << sym.first
                                                    << "' also defined at "
                                                    << it.first->second.fragment.location());
        ERROR(it.first->second.fragment.location(), scope << " " << kind << " '" << sym.first
                                                          << "' also defined at "
                                                          << sym.second.fragment.location());
      }
    }
  }
  return ok;
}

bool Symbols::join(const Symbols &symbols, const char *scope) {
  bool ok = true;
  if (!smap_join(defs, symbols.defs, scope, "definition")) ok = false;
  if (!smap_join(types, symbols.types, scope, "type")) ok = false;
  if (!smap_join(topics, symbols.topics, scope, "topic")) ok = false;
  return ok;
}

static void smap_setpkg(Symbols::SymbolMap &dest, const std::string &pkgname) {
  for (auto &sym : dest) {
    if (sym.second.qualified.empty()) {
      sym.second.qualified = sym.first + "@" + pkgname;
    }
  }
}

void Symbols::setpkg(const std::string &pkgname) {
  smap_setpkg(defs, pkgname);
  smap_setpkg(types, pkgname);
  smap_setpkg(topics, pkgname);
}

void DefMap::format(std::ostream &os, int depth) const {
  os << pad(depth) << "DefMap @ " << fragment.location() << std::endl;
  for (auto &i : defs) {
    os << pad(depth + 2) << i.first << " =" << std::endl;
    i.second.body->format(os, depth + 4);
  }
  imports.format("import", os, depth + 2);
  if (body) body->format(os, depth + 2);
}

void Package::format(std::ostream &os, int depth) const {
  os << pad(depth) << "Package " << name << std::endl;
  exports.format("export", os, depth + 2);
  for (auto &i : files) {
    i.content->format(os, depth + 2);
    for (auto &j : i.pubs) {
      os << pad(depth + 4) << "publish " << j.first << " = " << std::endl;
      j.second.body->format(os, depth + 6);
    }
  }
}

void Literal::format(std::ostream &os, int depth) const {
  os << pad(depth) << "Literal: " << typeVar << " @ " << fragment.location() << " = " << value
     << std::endl;
}

void Prim::format(std::ostream &os, int depth) const {
  os << pad(depth) << "Prim(" << args << "," << name << "): " << typeVar << " @ "
     << fragment.location() << std::endl;
}

void Top::format(std::ostream &os, int depth) const {
  os << pad(depth) << "Top" << std::endl;
  globals.format("global", os, depth + 2);
  for (auto &i : packages) i.second->format(os, depth + 2);
  body->format(os, depth + 2);
}

void DefBinding::format(std::ostream &os, int depth) const {
  os << pad(depth) << "DefBinding: ";
  os << meta << " " << typeVar << " @ " << fragment.location() << std::endl;

  // invert name=>index map
  std::vector<const char *> names(order.size());
  for (auto &i : order) names[i.second.index] = i.first.c_str();

  for (int i = 0; i < (int)val.size(); ++i) {
    os << pad(depth + 2) << "val " << names[i] << " = " << std::endl;
    val[i]->format(os, depth + 4);
  }
  for (int i = 0; i < (int)fun.size(); ++i) {
    os << pad(depth + 2) << "fun " << names[i + val.size()] << " (" << scc[i]
       << ") = " << std::endl;
    fun[i]->format(os, depth + 4);
  }
  body->format(os, depth + 2);
}

void Get::format(std::ostream &os, int depth) const {
  os << pad(depth) << "Get(" << cons->ast.name << ", " << index << "): " << typeVar << " @ "
     << fragment.location() << std::endl;
}

void Construct::format(std::ostream &os, int depth) const {
  os << pad(depth) << "Construct(" << cons->ast.name << "): " << typeVar << " @ "
     << fragment.location() << std::endl;
}

void Destruct::format(std::ostream &os, int depth) const {
  os << pad(depth) << "Destruct(" << sum->name << "): " << typeVar << " @ " << fragment.location()
     << std::endl;
  for (auto &lam : cases) lam->format(os, depth + 2);
  arg->format(os, depth + 2);
}

std::ostream &operator<<(std::ostream &os, const Expr *expr) {
  expr->format(os, 0);
  return os;
}

void VarDef::format(std::ostream &os, int depth) const {
  os << pad(depth) << "VarDef @ " << fragment.location() << std::endl;
}

void VarArg::format(std::ostream &os, int depth) const {
  os << pad(depth) << "VarArg @ " << fragment.location() << std::endl;
}
