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

#include <cassert>

#include "prim.h"
#include "types/data.h"
#include "types/datatype.h"
#include "types/type.h"
#include "value.h"

static const TypeVar arrayT("Array@builtin", 1);

static PRIMTYPE(type_vnew) {
  TypeVar vec;
  arrayT.clone(vec);
  return args.size() == 1 && args[0]->unify(Data::typeInteger) && out->unify(vec);
}

static PRIMFN(prim_vnew) {
  EXPECT(1);
  INTEGER_MPZ(arg0, 0);
  REQUIRE(mpz_cmp_si(arg0, 0) >= 0);
  RETURN(Record::alloc(runtime.heap, &Constructor::array, mpz_get_si(arg0)));
}

static PRIMTYPE(type_vget) {
  TypeVar vec;
  arrayT.clone(vec);
  return args.size() == 2 && args[0]->unify(vec) && args[1]->unify(Data::typeInteger) &&
         out->unify(vec[0]);
}

static PRIMFN(prim_vget) {
  EXPECT(2);
  RECORD(vec, 0);
  INTEGER_MPZ(arg1, 1);
  REQUIRE(mpz_cmp_si(arg1, 0) >= 0);
  REQUIRE(mpz_cmp_si(arg1, vec->size()) < 0);

  Promise *p = vec->at(mpz_get_si(arg1));
  if (*p) {
    scope->at(output)->fulfill(runtime, p->coerce<HeapObject>());
  } else {
    runtime.heap.reserve(Tuple::fulfiller_pads);
    p->await(runtime, scope->claim_fulfiller(runtime, output));
  }
}

static PRIMTYPE(type_vset) {
  TypeVar vec;
  arrayT.clone(vec);
  return args.size() == 3 && args[0]->unify(vec) && args[1]->unify(Data::typeInteger) &&
         args[2]->unify(vec[0]) && out->unify(Data::typeUnit);
}

static PRIMFN(prim_vset) {
  EXPECT(3);
  RECORD(vec, 0);
  INTEGER_MPZ(arg1, 1);

  // It's important to allocate before side-effects
  // Failed allocation causes the method to be re-entered
  runtime.heap.reserve(reserve_unit());

  // Getting this wrong means vector.wake is buggy and the heap will crash
  assert(mpz_cmp_si(arg1, 0) >= 0);
  assert(mpz_cmp_si(arg1, vec->size()) < 0);
  Promise *p = vec->at(mpz_get_si(arg1));
  assert(!*p);

  p->fulfill(runtime, args[2]);
  RETURN(claim_unit(runtime.heap));
}

void prim_register_vector(PrimMap &pmap) {
  prim_register(pmap, "vget", prim_vget, type_vget,
                PRIM_PURE | PRIM_PARTIAL);  // aborts out of range
  prim_register(pmap, "vnew", prim_vnew, type_vnew, PRIM_ORDERED);
  prim_register(pmap, "vset", prim_vset, type_vset, PRIM_IMPURE);
}
