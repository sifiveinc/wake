/*
 * Copyright 2023 SiFive, Inc.
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

// Open Group Base Specifications Issue 7
#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <cstdarg>
#include <functional>
#include <memory>
#include <ostream>
#include <optional>
#include <string>
#include <unordered_map>

namespace wcl {
namespace log {

static constexpr const char* LOG_LEVEL = "level";
static constexpr const char* LOG_TIME = "time";
static constexpr const char* LOG_PID = "pid";
static constexpr const char* LOG_HOSTNAME = "hostname";
static constexpr const char* LOG_LEVEL_INFO = "info";
static constexpr const char* LOG_LEVEL_WARNING = "warning";
static constexpr const char* LOG_LEVEL_ERROR = "error";
static constexpr const char* LOG_MESSAGE = "message";
static constexpr const char* URGENT = "urgent";

struct Event {
  std::unordered_map<std::string, std::string> items;

  Event(std::initializer_list<std::pair<const std::string, std::string>> list) : items(list) {}

  const std::string* get(const std::string& key) const {
    auto it = items.find(key);
    if (it == items.end()) {
      return nullptr;
    }

    return &it->second;
  }

  Event message(const char* fmt, ...) && __attribute__((format(printf, 2, 3)));
  Event message(const char* fmt, va_list args) &&;

  Event urgent() && __attribute__((warn_unused_result));
  Event time() && __attribute__((warn_unused_result));
  Event pid() && __attribute__((warn_unused_result));
  Event hostname() && __attribute__((warn_unused_result));
  Event level(const char* level) && __attribute__((warn_unused_result));

  void operator()() &&;
  void operator()(std::initializer_list<std::pair<const std::string, std::string>> list) &&;
};

// Abstract
class Subscriber {
 public:
  virtual void receive(const Event& e) = 0;
  virtual ~Subscriber(){};
};

class FormatSubscriber : public Subscriber {
 private:
  std::ostream s;

 public:
  FormatSubscriber(std::streambuf* rdbuf) : s(rdbuf) {}
  void receive(const Event& e) override;
  ~FormatSubscriber() override{};
};

class SimpleFormatSubscriber : public Subscriber {
 private:
  std::ostream s;

 public:
  SimpleFormatSubscriber(std::streambuf* rdbuf) : s(rdbuf) {}
  void receive(const Event& e) override;
  ~SimpleFormatSubscriber() override{};
};

class FilterSubscriber : public Subscriber {
 private:
  std::unique_ptr<Subscriber> subscriber;
  std::function<bool(const Event&)> predicate;

 public:
  FilterSubscriber(std::unique_ptr<Subscriber> s, std::function<bool(const Event&)> p)
      : subscriber(std::move(s)), predicate(p) {}
  void receive(const Event& e) override;
  ~FilterSubscriber() override{};
};

void subscribe(std::unique_ptr<Subscriber>);
void clear_subscribers();

Event event() __attribute__((warn_unused_result));

Event info(const char* fmt, ...) __attribute__((format(printf, 1, 2)))
__attribute__((warn_unused_result));
Event warning(const char* fmt, ...) __attribute__((format(printf, 1, 2)))
__attribute__((warn_unused_result));
Event error(const char* fmt, ...) __attribute__((format(printf, 1, 2)))
__attribute__((warn_unused_result));

}  // namespace log
}  // namespace wcl
