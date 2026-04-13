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

#include "data.h"

TypeVar Data::typeString("String@builtin", 0);
TypeVar Data::typeInteger("Integer@builtin", 0);
TypeVar Data::typeDouble("Double@builtin", 0);
TypeVar Data::typeRegExp("RegExp@builtin", 0);
TypeVar Data::typeJob("Job@builtin", 0);
TypeVar Data::typeTarget("Target@builtin", 0);
TypeVar Data::typeTime("Time@builtin", 0);

TypeVar Data::typeBoolean("Boolean@wake", 0);
TypeVar Data::typeOrder("Order@wake", 0);
TypeVar Data::typeUnit("Unit@wake", 0);
TypeVar Data::typeJValue("JValue@wake", 0);
TypeVar Data::typeError("Error@wake", 0);

const TypeVar Data::typeList("List@wake", 1);
const TypeVar Data::typePair("Pair@wake", 2);
const TypeVar Data::typeResult("Result@wake", 2);
