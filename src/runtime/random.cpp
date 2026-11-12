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

#include "wcl/random.h"

#include "prim.h"
#include "types/data.h"
#include "value.h"

static PRIMTYPE(type_uuidv4) {
  TypeVar result;
  Data::typeResult.clone(result);
  result[0].unify(Data::typeString);
  result[1].unify(Data::typeString);
  return args.empty() && out->unify(result);
}

static PRIMFN(prim_uuidv4) {
  EXPECT(0);
  auto generated = wcl::uuidv4();
  const bool ok = static_cast<bool>(generated);
  const std::string& text = ok ? *generated : generated.error();
  runtime.heap.reserve(reserve_result() + String::reserve(text.size()));
  auto value = String::claim(runtime.heap, text);
  RETURN(claim_result(runtime.heap, ok, value));
}

static PRIMTYPE(type_random_hex) {
  TypeVar result;
  Data::typeResult.clone(result);
  result[0].unify(Data::typeString);
  result[1].unify(Data::typeString);
  return args.size() == 1 && args[0]->unify(Data::typeInteger) && out->unify(result);
}

static PRIMFN(prim_random_hex) {
  EXPECT(1);
  INTEGER_MPZ(length_mpz, 0);
  auto generated = !mpz_fits_ulong_p(length_mpz)
                       ? wcl::make_error<std::string, std::string>(
                             "randomHexString length must be nonnegative and fit the native "
                             "size type")
                       : wcl::random_hex(static_cast<size_t>(mpz_get_ui(length_mpz)));
  const bool ok = static_cast<bool>(generated);
  const std::string& text = ok ? *generated : generated.error();
  runtime.heap.reserve(reserve_result() + String::reserve(text.size()));
  auto value = String::claim(runtime.heap, text);
  RETURN(claim_result(runtime.heap, ok, value));
}

void prim_register_random(PrimMap& pmap) {
  prim_register(pmap, "unsafe_uuidv4String", prim_uuidv4, type_uuidv4, PRIM_ORDERED);
  prim_register(pmap, "unsafe_randomHexString", prim_random_hex, type_random_hex, PRIM_ORDERED);
}
