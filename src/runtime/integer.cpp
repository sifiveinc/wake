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

#include <gmp.h>

#include "prim.h"
#include "types/data.h"
#include "types/internal.h"
#include "types/type.h"
#include "value.h"

#define UNOP(name, fn)                         \
  static PRIMFN(prim_##name) {                 \
    EXPECT(1);                                 \
    INTEGER_MPZ(arg0, 0);                      \
    MPZ out;                                   \
    fn(out.value, arg0);                       \
    RETURN(Integer::alloc(runtime.heap, out)); \
  }

UNOP(com, mpz_com)
UNOP(abs, mpz_abs)
UNOP(neg, mpz_neg)

#define BINOP(name, fn)                        \
  static PRIMFN(prim_##name) {                 \
    EXPECT(2);                                 \
    INTEGER_MPZ(arg0, 0);                      \
    INTEGER_MPZ(arg1, 1);                      \
    MPZ out;                                   \
    fn(out.value, arg0, arg1);                 \
    RETURN(Integer::alloc(runtime.heap, out)); \
  }

BINOP(add, mpz_add)
BINOP(sub, mpz_sub)
BINOP(mul, mpz_mul)
BINOP(xor, mpz_xor)
BINOP(and, mpz_and)
BINOP(or, mpz_ior)
BINOP(gcd, mpz_gcd)
BINOP(lcm, mpz_lcm)

#define BINOP_ZERO(name, fn)                          \
  static PRIMFN(prim_##name) {                        \
    EXPECT(2);                                        \
    INTEGER_MPZ(arg0, 0);                             \
    INTEGER_MPZ(arg1, 1);                             \
    bool division_by_zero = mpz_cmp_si(arg1, 0) == 0; \
    REQUIRE(!division_by_zero);                       \
    MPZ out;                                          \
    fn(out.value, arg0, arg1);                        \
    RETURN(Integer::alloc(runtime.heap, out));        \
  }

BINOP_ZERO(div, mpz_tdiv_q)
BINOP_ZERO(mod, mpz_tdiv_r)

#define BINOP_SI2(name, fn1, fn2)              \
  static PRIMFN(prim_##name) {                 \
    EXPECT(2);                                 \
    INTEGER_MPZ(arg0, 0);                      \
    INTEGER_MPZ(arg1, 1);                      \
    MPZ out;                                   \
    if (mpz_sgn(arg1) >= 0) {                  \
      fn1(out.value, arg0, mpz_get_si(arg1));  \
    } else {                                   \
      fn2(out.value, arg0, -mpz_get_si(arg1)); \
    }                                          \
    RETURN(Integer::alloc(runtime.heap, out)); \
  }

BINOP_SI2(shl, mpz_mul_2exp, mpz_tdiv_q_2exp)
BINOP_SI2(shr, mpz_tdiv_q_2exp, mpz_mul_2exp)

#define BINOP_SI0(name, fn)                    \
  static PRIMFN(prim_##name) {                 \
    EXPECT(2);                                 \
    INTEGER_MPZ(arg0, 0);                      \
    INTEGER_MPZ(arg1, 1);                      \
    MPZ out;                                   \
    if (mpz_sgn(arg1) >= 0) {                  \
      fn(out.value, arg0, mpz_get_si(arg1));   \
    }                                          \
    RETURN(Integer::alloc(runtime.heap, out)); \
  }

BINOP_SI0(exp, mpz_pow_ui)
BINOP_SI0(root, mpz_root)

static PRIMTYPE(type_powm) {
  return args.size() == 3 && args[0]->unify(Data::typeInteger) &&
         args[1]->unify(Data::typeInteger) && args[2]->unify(Data::typeInteger) &&
         out->unify(Data::typeInteger);
}

static PRIMFN(prim_powm) {
  EXPECT(3);
  INTEGER_MPZ(arg0, 0);
  INTEGER_MPZ(arg1, 1);
  INTEGER_MPZ(arg2, 2);
  bool division_by_zero = mpz_cmp_si(arg2, 0) == 0;
  REQUIRE(!division_by_zero);
  MPZ out;
  mpz_powm(out.value, arg0, arg1, arg2);
  RETURN(Integer::alloc(runtime.heap, out));
}

static PRIMTYPE(type_str) {
  return args.size() == 2 && args[0]->unify(Data::typeInteger) &&
         args[1]->unify(Data::typeInteger) && out->unify(Data::typeString);
}

static PRIMFN(prim_str) {
  EXPECT(2);
  INTEGER_MPZ(arg0, 0);
  INTEGER_MPZ(arg1, 1);
  long base = 0;
  bool ok = mpz_fits_slong_p(arg0);
  if (ok) {
    base = mpz_get_si(arg0);
    ok &= base <= 62 && base >= -36 && base != 0 && base != 1 && base != -1;
  }
  if (ok) {
    char buffer[mpz_sizeinbase(arg1, base) + 2];
    mpz_get_str(buffer, base, arg1);
    RETURN(String::alloc(runtime.heap, buffer));
  } else {
    RETURN(String::alloc(runtime.heap, ""));
  }
}

static PRIMTYPE(type_int) {
  TypeVar list;
  Data::typeList.clone(list);
  list[0].unify(Data::typeInteger);
  return args.size() == 2 && args[0]->unify(Data::typeInteger) &&
         args[1]->unify(Data::typeString) && out->unify(list);
}

static PRIMFN(prim_int) {
  EXPECT(2);
  INTEGER_MPZ(arg0, 0);
  STRING(arg1, 1);
  long base = 0;
  bool ok = mpz_fits_slong_p(arg0);
  if (ok) {
    base = mpz_get_si(arg0);
    ok &= base <= 62 && base >= 0 && base != 1;
  }
  MPZ val;
  if (ok && !mpz_set_str(val.value, arg1->c_str(), base)) {
    size_t need = Integer::reserve(val) + reserve_list(1);
    runtime.heap.reserve(need);
    Value *x = Integer::claim(runtime.heap, val);
    RETURN(claim_list(runtime.heap, 1, &x));
  } else {
    RETURN(alloc_nil(runtime.heap));
  }
}

// popcount, scan0, scan1 ?

static PRIMTYPE(type_unop) {
  return args.size() == 1 && args[0]->unify(Data::typeInteger) && out->unify(Data::typeInteger);
}

static PRIMTYPE(type_binop) {
  return args.size() == 2 && args[0]->unify(Data::typeInteger) &&
         args[1]->unify(Data::typeInteger) && out->unify(Data::typeInteger);
}

static PRIMFN(prim_icmp) {
  EXPECT(2);
  INTEGER_MPZ(arg0, 0);
  INTEGER_MPZ(arg1, 1);
  RETURN(alloc_order(runtime.heap, mpz_cmp(arg0, arg1)));
}

void prim_register_integer(PrimMap &pmap) {
  prim_register(pmap, "com", prim_com, type_unop, PRIM_PURE);
  prim_register(pmap, "abs", prim_abs, type_unop, PRIM_PURE);
  prim_register(pmap, "neg", prim_neg, type_unop, PRIM_PURE);
  prim_register(pmap, "add", prim_add, type_binop, PRIM_PURE);
  prim_register(pmap, "sub", prim_sub, type_binop, PRIM_PURE);
  prim_register(pmap, "mul", prim_mul, type_binop, PRIM_PURE);
  prim_register(pmap, "div", prim_div, type_binop, PRIM_PURE | PRIM_PARTIAL);  // aborts on /0
  prim_register(pmap, "mod", prim_mod, type_binop, PRIM_PURE | PRIM_PARTIAL);  // aborts on %0
  prim_register(pmap, "xor", prim_xor, type_binop, PRIM_PURE);
  prim_register(pmap, "and", prim_and, type_binop, PRIM_PURE);
  prim_register(pmap, "or", prim_or, type_binop, PRIM_PURE);
  prim_register(pmap, "gcd", prim_gcd, type_binop, PRIM_PURE);
  prim_register(pmap, "lcm", prim_lcm, type_binop, PRIM_PURE);
  prim_register(pmap, "shl", prim_shl, type_binop, PRIM_PURE);
  prim_register(pmap, "shr", prim_shr, type_binop, PRIM_PURE);
  prim_register(pmap, "exp", prim_exp, type_binop, PRIM_PURE);
  prim_register(pmap, "root", prim_root, type_binop, PRIM_PURE);
  prim_register(pmap, "powm", prim_powm, type_powm, PRIM_PURE | PRIM_PARTIAL);  // aborts on mod 0
  prim_register(pmap, "str", prim_str, type_str, PRIM_PURE);
  prim_register(pmap, "int", prim_int, type_int, PRIM_PURE);
  prim_register(pmap, "icmp", prim_icmp, type_icmp, PRIM_PURE);
}
