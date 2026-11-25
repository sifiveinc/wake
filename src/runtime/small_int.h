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

#ifndef SMALL_INT_H
#define SMALL_INT_H

#include <gmp.h>
#include <stdint.h>

#include <cstddef>

// Tagged-pointer encoding for "small Integer" values that fit in a single
// machine word. Bit 0 of any pointer to a real heap object is zero (heap
// objects are aligned to sizeof(PadObject) == 8); when bit 0 is set, the
// pointer is not a real pointer but a 63-bit signed integer payload.
struct HeapObject;

constexpr uintptr_t SMALL_INT_TAG = 1;

static_assert(sizeof(mp_limb_t) >= 8,
              "small_int.h assumes mp_limb_t is at least 64 bits; "
              "32-bit-limb GMP builds need the IntegerView two-limb path");

inline bool is_small_int(const HeapObject *p) {
  return (reinterpret_cast<uintptr_t>(p) & SMALL_INT_TAG) != 0;
}

inline int64_t small_int_value(const HeapObject *p) {
  return static_cast<int64_t>(reinterpret_cast<intptr_t>(p)) >> 1;
}

inline HeapObject *make_small_int(int64_t v) {
  return reinterpret_cast<HeapObject *>((static_cast<uintptr_t>(v) << 1) | SMALL_INT_TAG);
}

inline bool fits_small_int(int64_t v) {
  return v >= -(int64_t{1} << 62) && v < (int64_t{1} << 62);
}

// Returns true and writes the value to *out when the MPZ fits the small-int
// range; otherwise leaves *out untouched.
inline bool fits_small_mpz(const __mpz_struct &m, int64_t *out) {
  int s = m._mp_size;
  if (s == 0) {
    *out = 0;
    return true;
  }
  if (s == 1) {
    mp_limb_t l = m._mp_d[0];
    if (l < (uint64_t{1} << 62)) {
      *out = static_cast<int64_t>(l);
      return true;
    }
  } else if (s == -1) {
    mp_limb_t l = m._mp_d[0];
    // The most-negative representable value is -(2^62), corresponding to
    // limb == 2^62. Both INT64_MIN and overflow are excluded.
    if (l <= (uint64_t{1} << 62)) {
      *out = -static_cast<int64_t>(l);
      return true;
    }
  }
  return false;
}

#endif
