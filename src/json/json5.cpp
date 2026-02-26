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

#include "json5.h"

#include <memory>
#include <optional>
#include <sstream>

#include "wcl/tracing.h"

const char *jsymbolTable[] = {
    // appear in JAST and JSymbol
    "NULLVAL", "TRUE", "FALSE", "NAN", "INTEGER", "DOUBLE", "INFINITY", "STR",
    // appear only in JAST
    "OBJECT", "ARRAY",
    // appear only in JSymbol
    "ERROR", "END", "SOPEN", "SCLOSE", "BOPEN", "BCLOSE", "COLON", "ID", "COMMA"};

static JAST null(JSON_NULLVAL);

const JAST &JAST::get(const std::string &key) const {
  if (kind == JSON_OBJECT)
    for (auto &x : children)
      if (x.first == key) return x.second;
  return null;
}

JAST &JAST::get(const std::string &key) {
  if (kind == JSON_OBJECT)
    for (auto &x : children)
      if (x.first == key) return x.second;
  return null;
}

std::optional<const JAST *> JAST::get_opt(const std::string &key) const {
  const JAST &item = get(key);
  if (&item == &null) {
    return {};
  }
  return std::make_optional(&item);
}

JAST &JAST::add(std::string key, SymbolJSON kind, std::string &&value) {
  children.emplace_back(std::move(key), JAST(kind, std::move(value)));
  return children.back().second;
}

static char hex(unsigned char x) {
  if (x < 10) return '0' + x;
  return 'a' + x - 10;
}

std::string json_escape(const char *str, size_t len) {
  std::string out;
  char escape[] = "\\u0000";
  const char *end = str + len;
  for (const char *i = str; i != end; ++i) {
    char z = *i;
    unsigned char c = z;
    if (z == '"')
      out.append("\\\"");
    else if (z == '\\')
      out.append("\\\\");
    else if (c >= 0x20) {
      out.push_back(c);
    } else if (z == '\b') {
      out.append("\\b");
    } else if (z == '\f') {
      out.append("\\f");
    } else if (z == '\n') {
      out.append("\\n");
    } else if (z == '\r') {
      out.append("\\r");
    } else if (z == '\t') {
      out.append("\\t");
    } else {
      escape[4] = hex(c >> 4);
      escape[5] = hex(c & 0xf);
      out.append(escape);
    }
  }
  return out;
}

static std::ostream &formatObject(std::ostream &os, const JAST &jast) {
  os << "{";
  for (size_t i = 0; i < jast.children.size(); ++i) {
    if (i != 0) os << ',';
    const JChild &child = jast.children[i];
    os << '"' << json_escape(child.first) << "\":" << child.second;
  }
  return os << "}";
}

static std::ostream &formatArray(std::ostream &os, const JAST &jast) {
  os << "[";
  for (size_t i = 0; i < jast.children.size(); ++i) {
    if (i != 0) os << ',';
    os << jast.children[i].second;
  }
  return os << "]";
}

std::ostream &operator<<(std::ostream &os, const JAST &jast) {
  switch (jast.kind) {
    case JSON_NULLVAL:
      return os << "null";
    case JSON_TRUE:
      return os << "true";
    case JSON_FALSE:
      return os << "false";
    case JSON_NAN:
      return os << "NaN";
    case JSON_INTEGER:
      return os << jast.value;
    case JSON_DOUBLE:
      return os << jast.value;
    case JSON_INFINITY:
      return os << jast.value << "Infinity";
    case JSON_STR:
      return os << '"' << json_escape(jast.value) << '"';
    case JSON_OBJECT:
      return formatObject(os, jast);
    case JSON_ARRAY:
      return formatArray(os, jast);
    default:
      return os << "corrupt";
  }
}

void JsonSubscriber::receive(const wcl::log::Event &e) {
  std::string warning_msg = "{\"message\": \"warning: The next line may be corrupted\"}\n";
  JAST out(JSON_OBJECT);
  for (const auto &item : e.items) {
    out.add(item.first, item.second);
  }

  std::stringstream ss;
  ss << out << std::endl;
  std::string line = ss.str();

  if (line.size() > 4095) {
    (void)write(to_append.get(), warning_msg.data(), warning_msg.size());
  }
  (void)write(to_append.get(), line.data(), line.size());
}
