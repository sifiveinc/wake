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

#ifndef JSON5_H
#define JSON5_H

#include <memory>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

#include "util/location.h"
#include "wcl/tracing.h"
#include "wcl/unique_fd.h"

class JsonSubscriber : public wcl::log::Subscriber {
 public:
  using fd_t = wcl::precise_unique_fd<O_APPEND | O_CREAT | O_WRONLY, 0644>;

 private:
  fd_t to_append;

 public:
  explicit JsonSubscriber(fd_t f) : to_append(std::move(f)) {}
  JsonSubscriber(const JsonSubscriber &) = delete;
  JsonSubscriber(JsonSubscriber &&) = default;
  JsonSubscriber() = delete;

  void receive(const wcl::log::Event &e) override;
  ~JsonSubscriber() override{};
};

enum SymbolJSON {
  // appear in JAST and JSymbol
  JSON_NULLVAL,
  JSON_TRUE,
  JSON_FALSE,
  JSON_NAN,
  JSON_INTEGER,
  JSON_DOUBLE,
  JSON_INFINITY,
  JSON_STR,
  // appear only in JAST
  JSON_OBJECT,
  JSON_ARRAY,
  // appear only in JSymbol
  JSON_ERROR,
  JSON_END,
  JSON_SOPEN,
  JSON_SCLOSE,
  JSON_BOPEN,
  JSON_BCLOSE,
  JSON_COLON,
  JSON_ID,
  JSON_COMMA
};

extern const char *jsymbolTable[];

struct JAST;
typedef std::pair<std::string, JAST> JChild;
typedef std::vector<JChild> JChildren;

struct JAST {
  SymbolJSON kind;
  std::string value;
  JChildren children;

  JAST() : kind(JSON_ERROR) {}
  JAST(SymbolJSON kind_) : kind(kind_) {}
  JAST(SymbolJSON kind_, std::string &&value_) : kind(kind_), value(std::move(value_)) {}
  JAST(SymbolJSON kind_, JChildren &&children_) : kind(kind_), children(std::move(children_)) {}

  static bool parse(const char *file, std::ostream &errs, JAST &out);
  static bool parse(const std::string &body, std::ostream &errs, JAST &out);
  static bool parse(const char *body, size_t len, std::ostream &errs, JAST &out);

  template <class T>
  static JAST from_vec(const std::vector<T> &items) {
    JAST json(JSON_ARRAY);
    for (const T &i : items) {
      json.add("", i.to_json());
    }
    return json;
  }

  const JAST &get(const std::string &key) const;
  JAST &get(const std::string &key);
  std::optional<const JAST *> get_opt(const std::string &key) const;

  // Add a child to a JObject
  JAST &add(std::string key, SymbolJSON kind, std::string &&value);
  JAST &add_bool(std::string key, bool value) {
    if (value)
      return add(std::move(key), JSON_TRUE, "true");
    else
      return add(std::move(key), JSON_FALSE, "false");
  }
  JAST &add(std::string key, uint64_t value) {
    return add(std::move(key), JSON_INTEGER, std::to_string(value));
  }
  JAST &add(std::string key, int value) {
    return add(std::move(key), JSON_INTEGER, std::to_string(value));
  }
  JAST &add(std::string key, long value) {
    return add(std::move(key), JSON_INTEGER, std::to_string(value));
  }
  JAST &add(std::string key, long long value) {
    return add(std::move(key), JSON_INTEGER, std::to_string(value));
  }
  JAST &add(std::string key, double value) {
    return add(std::move(key), JSON_DOUBLE, std::to_string(value));
  }
  JAST &add(std::string key, std::string value) {
    return add(std::move(key), JSON_STR, std::move(value));
  }
  JAST &add(std::string key, SymbolJSON kind) { return add(std::move(key), kind, std::string()); }
  JAST &add(std::string key, JAST &&child) {
    children.emplace_back(std::move(key), std::move(child));
    return children.back().second;
  }
  // Add a child to a JArray
  JAST &add(SymbolJSON kind, std::string &&value) {
    return add(std::string(), kind, std::move(value));
  }
  JAST &add(SymbolJSON kind) { return add(std::string(), kind, std::string()); }
  JAST &add(std::string value) { return add(std::string(), JSON_STR, std::move(value)); }

  std::optional<std::string> expect_string() const {
    if (kind == JSON_STR) {
      return std::optional<std::string>{value};
    }
    return {};
  }

  std::optional<int64_t> expect_integer() const {
    if (kind == JSON_INTEGER) {
      return std::optional<int64_t>{std::stol(value)};
    }
    return {};
  }

  std::optional<bool> expect_boolean() const {
    if (kind == JSON_TRUE) {
      return std::make_optional(true);
    }
    if (kind == JSON_FALSE) {
      return std::make_optional(false);
    }
    return {};
  }
};

std::ostream &operator<<(std::ostream &os, const JAST &jast);

struct JSymbol {
  SymbolJSON type;
  Location location;
  std::string value;

  JSymbol(SymbolJSON type_, const Location &location_) : type(type_), location(location_) {}
  JSymbol(SymbolJSON type_, const Location &location_, std::string &&value_)
      : type(type_), location(location_), value(std::move(value_)) {}
};

struct jinput_t;

struct JLexer {
  std::unique_ptr<jinput_t> engine;
  JSymbol next;
  bool fail;

  JLexer(const char *file);
  JLexer(const std::string &body);
  JLexer(const char *body, size_t len);
  ~JLexer();

  void consume();
};

std::string json_escape(const char *str, size_t len);
inline std::string json_escape(const std::string &x) { return json_escape(x.c_str(), x.size()); }

#endif
