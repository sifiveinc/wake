/*
 * Copyright 2026 SiFive, Inc.
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

#include <gmp.h>

#include <chrono>
#include <cstdlib>
#include <ctime>
#include <string>

#include "prim.h"
#include "status.h"
#include "types/data.h"
#include "value.h"

// Pre-process a format string to expand %f sub-second specifiers before passing to strftime.
// Supported: %f (9 digits), %.f (trimmed), %.Nf (dot + N digits), %Nf (N digits) where N is 1-9
static std::string expand_subsecond_formats(const char *fmt, int64_t nanoseconds) {
  int64_t sub_nanos = nanoseconds % 1000000000LL;
  if (sub_nanos < 0) sub_nanos += 1000000000LL;

  char nanos_str[10];
  snprintf(nanos_str, sizeof(nanos_str), "%09lld", (long long)sub_nanos);

  std::string result;
  for (const char *p = fmt; *p; ++p) {
    if (*p != '%') {
      result += *p;
      continue;
    }

    // Look ahead from '%'
    if (p[1] == '.' && p[2] >= '1' && p[2] <= '9' && p[3] == 'f') {
      // %.Nf — dot + fixed N digits (N = 1-9)
      int n = p[2] - '0';
      result += '.';
      result.append(nanos_str, n);
      p += 3;
    } else if (p[1] == '.' && p[2] == 'f') {
      // %.f — dot + trimmed trailing zeros
      std::string frac(nanos_str, 9);
      size_t last = frac.find_last_not_of('0');
      if (last != std::string::npos) {
        result += '.';
        result.append(frac, 0, last + 1);
      }
      p += 2;
    } else if (p[1] >= '1' && p[1] <= '9' && p[2] == 'f') {
      // %Nf — fixed N digits, no dot (N = 1-9)
      int n = p[1] - '0';
      result.append(nanos_str, n);
      p += 2;
    } else if (p[1] == 'f') {
      // %f — all 9 digits, no dot
      result.append(nanos_str, 9);
      p += 1;
    } else {
      // Not a sub-second specifier — pass through for strftime
      result += '%';
    }
  }
  return result;
}

// Format a Time's nanoseconds using strftime with sub-second expansion.
// If timezone is nullptr, uses UTC (gmtime_r). Otherwise sets TZ and uses localtime_r.
// Empty string timezone means system default (unset TZ).
static std::string format_time_str(const char *fmt, int64_t nanoseconds, const char *timezone) {
  std::chrono::nanoseconds nanos_duration(nanoseconds);
  std::chrono::system_clock::time_point tp(
      std::chrono::duration_cast<std::chrono::system_clock::duration>(nanos_duration));
  time_t seconds = std::chrono::system_clock::to_time_t(tp);

  struct tm tm_info;

  if (timezone == nullptr) {
    gmtime_r(&seconds, &tm_info);
  } else {
    char *old_tz = getenv("TZ");
    std::string saved_tz;
    bool had_tz = (old_tz != nullptr);
    if (had_tz) saved_tz = old_tz;

    if (timezone[0] == '\0') {
      unsetenv("TZ");
    } else {
      setenv("TZ", timezone, 1);
    }
    tzset();
    localtime_r(&seconds, &tm_info);

    if (had_tz) {
      setenv("TZ", saved_tz.c_str(), 1);
    } else {
      unsetenv("TZ");
    }
    tzset();
  }

  std::string expanded = expand_subsecond_formats(fmt, nanoseconds);
  char buffer[256];
  strftime(buffer, sizeof(buffer), expanded.c_str(), &tm_info);
  return std::string(buffer);
}

// Get current time in nanoseconds since Unix epoch using chrono
static PRIMFN(prim_get_time) {
  EXPECT(0);

  // Use chrono to get current time with nanosecond precision
  auto now = std::chrono::system_clock::now();
  auto duration = now.time_since_epoch();
  auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(duration);

  RETURN(Time::alloc(runtime.heap, nanos.count()));
}

static PRIMTYPE(type_get_time) { return args.size() == 0 && out->unify(Data::typeTime); }

// Format a Time value using strftime (UTC)
static PRIMFN(prim_format_time) {
  EXPECT(2);
  STRING(format, 0);

  HeapObject *time_obj = args[1];
  REQUIRE(typeid(*time_obj) == typeid(Time));
  Time *time = static_cast<Time *>(time_obj);

  std::string result = format_time_str(format->c_str(), time->nanoseconds, nullptr);
  RETURN(String::alloc(runtime.heap, result.c_str(), result.size()));
}

static PRIMTYPE(type_format_time) {
  return args.size() == 2 && args[0]->unify(Data::typeString) && args[1]->unify(Data::typeTime) &&
         out->unify(Data::typeString);
}

// Print a timestamped message: format Time with timezone, prepend to message, write to stream
// Args: format, timezone, time, stream, message
static PRIMFN(prim_time_print) {
  EXPECT(5);
  STRING(format, 0);
  STRING(timezone, 1);

  HeapObject *time_obj = args[2];
  REQUIRE(typeid(*time_obj) == typeid(Time));
  Time *time = static_cast<Time *>(time_obj);

  STRING(stream, 3);
  STRING(message, 4);

  std::string ts = format_time_str(format->c_str(), time->nanoseconds, timezone->c_str());

  runtime.heap.reserve(reserve_unit());
  status_get_generic_stream(stream->c_str()) << ts << message->as_str();
  RETURN(claim_unit(runtime.heap));
}

static PRIMTYPE(type_time_print) {
  return args.size() == 5 && args[0]->unify(Data::typeString) && args[1]->unify(Data::typeString) &&
         args[2]->unify(Data::typeTime) && args[3]->unify(Data::typeString) &&
         args[4]->unify(Data::typeString) && out->unify(Data::typeUnit);
}

// Extract nanoseconds from a Time value as an Integer
static PRIMFN(prim_time_to_nanos) {
  EXPECT(1);

  HeapObject *time_obj = args[0];
  REQUIRE(typeid(*time_obj) == typeid(Time));
  Time *time = static_cast<Time *>(time_obj);

  MPZ result;
  mpz_set_si(result.value, time->nanoseconds);

  RETURN(Integer::alloc(runtime.heap, result));
}

static PRIMTYPE(type_time_to_nanos) {
  return args.size() == 1 && args[0]->unify(Data::typeTime) && out->unify(Data::typeInteger);
}

void prim_register_time(PrimMap &pmap) {
  prim_register(pmap, "get_time", prim_get_time, type_get_time, PRIM_ORDERED);
  prim_register(pmap, "format_time", prim_format_time, type_format_time, PRIM_PURE);
  prim_register(pmap, "time_to_nanos", prim_time_to_nanos, type_time_to_nanos, PRIM_PURE);
  prim_register(pmap, "time_print", prim_time_print, type_time_print, PRIM_ORDERED);
}
