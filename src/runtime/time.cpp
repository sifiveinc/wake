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
#include <cstring>
#include <ctime>
#include <string>

#include "prim.h"
#include "types/data.h"
#include "types/internal.h"
#include "types/type.h"
#include "value.h"

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

// Format a Time value using chrono and strftime (UTC)
static PRIMFN(prim_format_time) {
  EXPECT(2);
  STRING(format, 0);

  HeapObject *time_obj = args[1];
  REQUIRE(typeid(*time_obj) == typeid(Time));
  Time *time = static_cast<Time *>(time_obj);

  // Convert nanoseconds to chrono time_point
  std::chrono::nanoseconds nanos_duration(time->nanoseconds);
  std::chrono::system_clock::time_point tp(
      std::chrono::duration_cast<std::chrono::system_clock::duration>(nanos_duration));

  // Convert to time_t for strftime
  time_t seconds = std::chrono::system_clock::to_time_t(tp);

  struct tm tm_info;
  gmtime_r(&seconds, &tm_info);

  char buffer[256];
  size_t len = strftime(buffer, sizeof(buffer), format->c_str(), &tm_info);

  RETURN(String::alloc(runtime.heap, buffer, len));
}

static PRIMTYPE(type_format_time) {
  return args.size() == 2 && args[0]->unify(Data::typeString) && args[1]->unify(Data::typeTime) &&
         out->unify(Data::typeString);
}

// Format a Time value using chrono and strftime with timezone support
static PRIMFN(prim_format_time_tz) {
  EXPECT(3);
  STRING(format, 0);
  STRING(timezone, 1);

  HeapObject *time_obj = args[2];
  REQUIRE(typeid(*time_obj) == typeid(Time));
  Time *time = static_cast<Time *>(time_obj);

  // Convert nanoseconds to chrono time_point
  std::chrono::nanoseconds nanos_duration(time->nanoseconds);
  std::chrono::system_clock::time_point tp(
      std::chrono::duration_cast<std::chrono::system_clock::duration>(nanos_duration));

  // Convert to time_t for strftime
  time_t seconds = std::chrono::system_clock::to_time_t(tp);

  struct tm tm_info;

  // Save current TZ environment variable
  char *old_tz = getenv("TZ");
  std::string saved_tz;
  bool had_tz = (old_tz != nullptr);
  if (had_tz) {
    saved_tz = old_tz;
  }

  // Set the requested timezone
  setenv("TZ", timezone->c_str(), 1);
  tzset();

  // Use localtime_r with the new timezone
  localtime_r(&seconds, &tm_info);

  char buffer[256];
  size_t len = strftime(buffer, sizeof(buffer), format->c_str(), &tm_info);

  // Restore original TZ environment variable
  if (had_tz) {
    setenv("TZ", saved_tz.c_str(), 1);
  } else {
    unsetenv("TZ");
  }
  tzset();

  RETURN(String::alloc(runtime.heap, buffer, len));
}

static PRIMTYPE(type_format_time_tz) {
  return args.size() == 3 && args[0]->unify(Data::typeString) && args[1]->unify(Data::typeString) &&
         args[2]->unify(Data::typeTime) && out->unify(Data::typeString);
}

// Parse a time string using strptime and convert to chrono time_point
static PRIMFN(prim_parse_time) {
  EXPECT(2);
  STRING(format, 0);
  STRING(timestr, 1);

  struct tm tm_info;
  memset(&tm_info, 0, sizeof(tm_info));

  char *result = strptime(timestr->c_str(), format->c_str(), &tm_info);

  // Check if parsing was successful
  REQUIRE(result != nullptr && *result == '\0');

  // Convert to time_t (assumes UTC)
  time_t seconds = timegm(&tm_info);
  REQUIRE(seconds != -1);

  // Convert to chrono time_point, then to nanoseconds
  auto tp = std::chrono::system_clock::from_time_t(seconds);
  auto duration = tp.time_since_epoch();
  auto nanos = std::chrono::duration_cast<std::chrono::nanoseconds>(duration);

  RETURN(Time::alloc(runtime.heap, nanos.count()));
}

static PRIMTYPE(type_parse_time) {
  return args.size() == 2 && args[0]->unify(Data::typeString) && args[1]->unify(Data::typeString) &&
         out->unify(Data::typeTime);
}

// Subtract two Time values using chrono, return Integer nanoseconds
static PRIMFN(prim_time_sub) {
  EXPECT(2);

  HeapObject *time1_obj = args[0];
  HeapObject *time2_obj = args[1];
  REQUIRE(typeid(*time1_obj) == typeid(Time));
  REQUIRE(typeid(*time2_obj) == typeid(Time));

  Time *time1 = static_cast<Time *>(time1_obj);
  Time *time2 = static_cast<Time *>(time2_obj);

  // Use chrono for type-safe arithmetic
  std::chrono::nanoseconds nanos1(time1->nanoseconds);
  std::chrono::nanoseconds nanos2(time2->nanoseconds);
  auto diff = nanos1 - nanos2;

  MPZ result;
  mpz_set_si(result.value, diff.count());

  RETURN(Integer::alloc(runtime.heap, result));
}

static PRIMTYPE(type_time_sub) {
  return args.size() == 2 && args[0]->unify(Data::typeTime) && args[1]->unify(Data::typeTime) &&
         out->unify(Data::typeInteger);
}

// Add Integer nanoseconds to a Time value using chrono, return new Time
static PRIMFN(prim_time_add) {
  EXPECT(2);

  HeapObject *time_obj = args[0];
  REQUIRE(typeid(*time_obj) == typeid(Time));
  Time *time = static_cast<Time *>(time_obj);

  INTEGER_MPZ(nanos_to_add, 1);

  // Convert to int64_t
  int64_t add_value = 0;
  if (mpz_fits_slong_p(nanos_to_add)) {
    add_value = mpz_get_si(nanos_to_add);
  }

  // Use chrono for type-safe arithmetic
  std::chrono::nanoseconds time_nanos(time->nanoseconds);
  std::chrono::nanoseconds add_nanos(add_value);
  auto result = time_nanos + add_nanos;

  RETURN(Time::alloc(runtime.heap, result.count()));
}

static PRIMTYPE(type_time_add) {
  return args.size() == 2 && args[0]->unify(Data::typeTime) && args[1]->unify(Data::typeInteger) &&
         out->unify(Data::typeTime);
}

// Compare two Time values, return Order (LT/EQ/GT)
static PRIMFN(prim_time_cmp) {
  EXPECT(2);

  HeapObject *time1_obj = args[0];
  HeapObject *time2_obj = args[1];
  REQUIRE(typeid(*time1_obj) == typeid(Time));
  REQUIRE(typeid(*time2_obj) == typeid(Time));

  Time *time1 = static_cast<Time *>(time1_obj);
  Time *time2 = static_cast<Time *>(time2_obj);

  int cmp = 0;
  if (time1->nanoseconds < time2->nanoseconds)
    cmp = -1;
  else if (time1->nanoseconds > time2->nanoseconds)
    cmp = 1;

  RETURN(alloc_order(runtime.heap, cmp));
}

static PRIMTYPE(type_time_cmp) {
  return args.size() == 2 && args[0]->unify(Data::typeTime) && args[1]->unify(Data::typeTime) &&
         out->unify(Data::typeOrder);
}

void prim_register_time(PrimMap &pmap) {
  prim_register(pmap, "get_time", prim_get_time, type_get_time, PRIM_ORDERED);
  prim_register(pmap, "format_time", prim_format_time, type_format_time, PRIM_PURE);
  prim_register(pmap, "format_time_tz", prim_format_time_tz, type_format_time_tz, PRIM_PURE);
  prim_register(pmap, "parse_time", prim_parse_time, type_parse_time, PRIM_PURE);
  prim_register(pmap, "time_sub", prim_time_sub, type_time_sub, PRIM_PURE);
  prim_register(pmap, "time_add", prim_time_add, type_time_add, PRIM_PURE);
  prim_register(pmap, "time_cmp", prim_time_cmp, type_time_cmp, PRIM_PURE);
}
