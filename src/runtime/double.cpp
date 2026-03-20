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

#include <cmath>
#include <cstdlib>

#include "prim.h"
#include "types/data.h"
#include "types/internal.h"
#include "types/type.h"
#include "value.h"

static PRIMTYPE(type_unop) {
  return args.size() == 1 && args[0]->unify(Data::typeDouble) && out->unify(Data::typeDouble);
}

#define UNOP(name, fn)                        \
  static PRIMFN(prim_##name) {                \
    EXPECT(1);                                \
    DOUBLE(arg0, 0);                          \
    double out = fn(arg0->value);             \
    RETURN(Double::alloc(runtime.heap, out)); \
  }

static double neg(double x) { return -x; }
UNOP(abs, std::abs)
UNOP(neg, neg)
UNOP(cos, std::cos)
UNOP(sin, std::sin)
UNOP(tan, std::tan)
UNOP(acos, std::acos)
UNOP(asin, std::asin)
UNOP(exp, std::exp)
UNOP(log, std::log)
UNOP(expm1, std::expm1)
UNOP(log1p, std::log1p)
UNOP(erf, std::erf)
UNOP(erfc, std::erfc)
UNOP(tgamma, std::tgamma)
UNOP(lgamma, std::lgamma)

static PRIMTYPE(type_binop) {
  return args.size() == 2 && args[0]->unify(Data::typeDouble) && args[1]->unify(Data::typeDouble) &&
         out->unify(Data::typeDouble);
}

#define BINOP(name, fn)                        \
  static PRIMFN(prim_##name) {                 \
    EXPECT(2);                                 \
    DOUBLE(arg0, 0);                           \
    DOUBLE(arg1, 1);                           \
    double out = fn(arg0->value, arg1->value); \
    RETURN(Double::alloc(runtime.heap, out));  \
  }

static double add(double x, double y) { return x + y; }
static double sub(double x, double y) { return x - y; }
static double mul(double x, double y) { return x * y; }
static double div(double x, double y) { return x / y; }

BINOP(add, add)
BINOP(sub, sub)
BINOP(mul, mul)
BINOP(div, div)
BINOP(pow, pow)
BINOP(atan, std::atan2)

static PRIMTYPE(type_fma) {
  return args.size() == 3 && args[0]->unify(Data::typeDouble) && args[1]->unify(Data::typeDouble) &&
         args[2]->unify(Data::typeDouble) && out->unify(Data::typeDouble);
}

static PRIMFN(prim_fma) {
  EXPECT(3);
  DOUBLE(arg0, 0);
  DOUBLE(arg1, 1);
  DOUBLE(arg2, 2);
  double out = std::fma(arg0->value, arg1->value, arg2->value);
  RETURN(Double::alloc(runtime.heap, out));
}

static PRIMTYPE(type_str) {
  return args.size() == 3 && args[0]->unify(Data::typeInteger) &&
         args[1]->unify(Data::typeInteger) && args[2]->unify(Data::typeDouble) &&
         out->unify(Data::typeString);
}

static PRIMFN(prim_str) {
  EXPECT(3);
  INTEGER_MPZ(arg0, 0);
  INTEGER_MPZ(arg1, 1);
  DOUBLE(arg2, 2);
  long format = 0, precision = 0;

  bool ok = mpz_fits_slong_p(arg0);
  if (ok) {
    format = mpz_get_si(arg0);
    ok &= format >= 0 && format <= 3;
  }

  ok &= mpz_fits_slong_p(arg1);
  if (ok) {
    precision = mpz_get_si(arg1);
    ok &= precision >= 1 && precision <= 40;
  }

  RETURN(String::alloc(runtime.heap, ok ? arg2->str(format, precision) : ""));
}

static PRIMTYPE(type_dbl) {
  TypeVar list;
  Data::typeList.clone(list);
  list[0].unify(Data::typeDouble);
  return args.size() == 1 && args[0]->unify(Data::typeString) && out->unify(list);
}

static PRIMFN(prim_dbl) {
  EXPECT(1);
  STRING(arg0, 0);
  char *end;
  double val = strtod(arg0->c_str(), &end);
  if (*end) {
    RETURN(alloc_nil(runtime.heap));
  } else {
    size_t need = Double::reserve() + reserve_list(1);
    runtime.heap.reserve(need);
    Value *out = Double::claim(runtime.heap, val);
    RETURN(claim_list(runtime.heap, 1, &out));
  }
}

static PRIMTYPE(type_cmp) {
  TypeVar list;
  Data::typeList.clone(list);
  list[0].unify(Data::typeOrder);
  return args.size() == 2 && args[0]->unify(Data::typeDouble) && args[1]->unify(Data::typeDouble) &&
         out->unify(list);
}

static PRIMFN(prim_cmp) {
  EXPECT(2);
  DOUBLE(arg0, 0);
  DOUBLE(arg1, 1);
  if (std::isnan(arg0->value) || std::isnan(arg1->value)) {
    RETURN(alloc_nil(runtime.heap));
  } else {
    size_t need = reserve_order() + reserve_list(1);
    runtime.heap.reserve(need);
    int x = (arg0->value > arg1->value) - (arg0->value < arg1->value);
    Value *out = alloc_order(runtime.heap, x);
    RETURN(claim_list(runtime.heap, 1, &out));
  }
}

static PRIMFN(prim_cmp_nan_lt) {
  EXPECT(2);
  DOUBLE(arg0, 0);
  DOUBLE(arg1, 1);
  int x;
  bool n0 = std::isnan(arg0->value);
  bool n1 = std::isnan(arg1->value);
  if (n0) {
    if (n1) {
      x = 0;
    } else {
      x = -1;  // nan < x
    }
  } else {
    if (n1) {
      x = 1;  // x > nan
    } else {
      x = (arg0->value > arg1->value) - (arg0->value < arg1->value);
    }
  }
  RETURN(alloc_order(runtime.heap, x));
}

static PRIMTYPE(type_class) {
  return args.size() == 1 && args[0]->unify(Data::typeDouble) && out->unify(Data::typeInteger);
}

static PRIMFN(prim_class) {
  EXPECT(1);
  DOUBLE(arg0, 0);
  int code;
  switch (std::fpclassify(arg0->value)) {
    case FP_INFINITE:
      code = 1;
      break;
    case FP_NAN:
      code = 2;
      break;
    case FP_ZERO:
      code = 3;
      break;
    case FP_SUBNORMAL:
      code = 4;
      break;
    default:
      code = 5;
      break;
  }
  RETURN(Integer::alloc(runtime.heap, code));
}

static PRIMTYPE(type_frexp) {
  TypeVar pair;
  Data::typePair.clone(pair);
  pair[0].unify(Data::typeDouble);
  pair[1].unify(Data::typeInteger);
  return args.size() == 1 && args[0]->unify(Data::typeDouble) && out->unify(pair);
}

static PRIMFN(prim_frexp) {
  EXPECT(1);
  DOUBLE(arg0, 0);

  int exp;
  double frac = std::frexp(arg0->value, &exp);
  MPZ val(exp);

  size_t need = reserve_tuple2() + Double::reserve() + Integer::reserve(val);
  runtime.heap.reserve(need);

  auto out = claim_tuple2(runtime.heap, Double::claim(runtime.heap, frac),
                          Integer::claim(runtime.heap, val));
  RETURN(out);
}

static PRIMTYPE(type_ldexp) {
  return args.size() == 2 && args[0]->unify(Data::typeDouble) &&
         args[1]->unify(Data::typeInteger) && out->unify(Data::typeDouble);
}

static PRIMFN(prim_ldexp) {
  EXPECT(2);
  DOUBLE(arg0, 0);
  INTEGER_MPZ(arg1, 1);

  if (mpz_cmp_si(arg1, -10000) < 0) {
    RETURN(Double::alloc(runtime.heap, 0.0));
  } else if (mpz_cmp_si(arg1, 10000) > 0) {
    RETURN(Double::alloc(runtime.heap, arg0->value / 0.0));
  } else {
    auto res = std::ldexp(arg0->value, mpz_get_si(arg1));
    RETURN(Double::alloc(runtime.heap, res));
  }
}

static PRIMTYPE(type_modf) {
  TypeVar pair;
  Data::typePair.clone(pair);
  pair[0].unify(Data::typeInteger);
  pair[1].unify(Data::typeDouble);
  return args.size() == 1 && args[0]->unify(Data::typeDouble) && out->unify(pair);
}

static PRIMFN(prim_modf) {
  EXPECT(1);
  DOUBLE(arg0, 0);

  double intpart;
  double frac = std::modf(arg0->value, &intpart);
  MPZ i;
  mpz_set_d(i.value, arg0->value);

  size_t need = reserve_tuple2() + Integer::reserve(i) + Double::reserve();
  runtime.heap.reserve(need);

  auto out = claim_tuple2(runtime.heap, Integer::claim(runtime.heap, i),
                          Double::claim(runtime.heap, frac));
  RETURN(out);
}

void prim_register_double(PrimMap &pmap) {
  // basic functions
  prim_register(pmap, "dabs", prim_abs, type_unop, PRIM_PURE);
  prim_register(pmap, "dneg", prim_neg, type_unop, PRIM_PURE);
  prim_register(pmap, "dadd", prim_add, type_binop, PRIM_PURE);
  prim_register(pmap, "dsub", prim_sub, type_binop, PRIM_PURE);
  prim_register(pmap, "dmul", prim_mul, type_binop, PRIM_PURE);
  prim_register(pmap, "ddiv", prim_div, type_binop, PRIM_PURE);
  prim_register(pmap, "dpow", prim_pow, type_binop, PRIM_PURE);
  prim_register(pmap, "dfma", prim_fma, type_fma, PRIM_PURE);
  prim_register(pmap, "dcmp", prim_cmp, type_cmp, PRIM_PURE);
  prim_register(pmap, "dstr", prim_str, type_str, PRIM_PURE);
  prim_register(pmap, "ddbl", prim_dbl, type_dbl, PRIM_PURE);

  prim_register(pmap, "dcmp_nan_lt", prim_cmp_nan_lt, type_cmp_nan_lt, PRIM_PURE);

  // integer/double interop
  prim_register(pmap, "dclass", prim_class, type_class, PRIM_PURE);
  prim_register(pmap, "dfrexp", prim_frexp, type_frexp, PRIM_PURE);
  prim_register(pmap, "dldexp", prim_ldexp, type_ldexp, PRIM_PURE);
  prim_register(pmap, "dmodf", prim_modf, type_modf, PRIM_PURE);

  // handy numeric functions
  prim_register(pmap, "dcos", prim_cos, type_unop, PRIM_PURE);
  prim_register(pmap, "dsin", prim_sin, type_unop, PRIM_PURE);
  prim_register(pmap, "dtan", prim_tan, type_unop, PRIM_PURE);
  prim_register(pmap, "dacos", prim_acos, type_unop, PRIM_PURE);
  prim_register(pmap, "dasin", prim_asin, type_unop, PRIM_PURE);
  prim_register(pmap, "dexp", prim_exp, type_unop, PRIM_PURE);
  prim_register(pmap, "dlog", prim_log, type_unop, PRIM_PURE);
  prim_register(pmap, "dexpm1", prim_expm1, type_unop, PRIM_PURE);
  prim_register(pmap, "dlog1p", prim_log1p, type_unop, PRIM_PURE);
  prim_register(pmap, "derf", prim_erf, type_unop, PRIM_PURE);
  prim_register(pmap, "derfc", prim_erfc, type_unop, PRIM_PURE);
  prim_register(pmap, "dtgamma", prim_tgamma, type_unop, PRIM_PURE);
  prim_register(pmap, "dlgamma", prim_lgamma, type_unop, PRIM_PURE);
  prim_register(pmap, "datan", prim_atan, type_binop, PRIM_PURE);
}
