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

#include "sums.h"

#include <sstream>

#include "datatype.h"
#include "util/diagnostic.h"

std::shared_ptr<Sum> Boolean;
std::shared_ptr<Sum> Order;
std::shared_ptr<Sum> List;
std::shared_ptr<Sum> Unit;
std::shared_ptr<Sum> Pair;
std::shared_ptr<Sum> Result;
std::shared_ptr<Sum> JValue;
std::shared_ptr<Sum> Vector;

void check_special(const std::shared_ptr<Sum> &sump) {
  if (sump->name == "Boolean") Boolean = sump;
  if (sump->name == "Order") Order = sump;
  if (sump->name == "List") List = sump;
  if (sump->name == "Unit") Unit = sump;
  if (sump->name == "Pair") Pair = sump;
  if (sump->name == "Result") Result = sump;
  if (sump->name == "JValue") JValue = sump;
  if (sump->name == "Vector") Vector = sump;
}

void sums_ok() {
  if (Boolean) {
    if (Boolean->members.size() != 2 || Boolean->members[0].ast.args.size() != 0 ||
        Boolean->members[1].ast.args.size() != 0) {
      ERROR(Boolean->region.location(), "special constructor Boolean not defined correctly");
    }
  } else {
    ERROR(LOCATION, "required data type Boolean@wake not defined");
  }

  if (Order) {
    if (Order->members.size() != 3 || Order->members[0].ast.args.size() != 0 ||
        Order->members[1].ast.args.size() != 0 || Order->members[2].ast.args.size() != 0) {
      ERROR(Order->region.location(), "special constructor Order not defined correctly");
    }
  } else {
    ERROR(LOCATION, "required data type Order@wake not defined");
  }

  if (List) {
    if (List->members.size() != 2 || List->members[0].ast.args.size() != 0 ||
        List->members[1].ast.args.size() != 2) {
      std::ostringstream message;
      ERROR(List->region.location(), "special constructor List not defined correctly");
    }
  } else {
    ERROR(LOCATION, "required data type List@wake not defined");
  }

  if (Unit) {
    if (Unit->members.size() != 1 || Unit->members[0].ast.args.size() != 0) {
      ERROR(Unit->region.location(), "special constructor Unit not defined correctly");
    }
  } else {
    ERROR(LOCATION, "required data type Unit@wake not defined");
  }

  if (Pair) {
    if (Pair->members.size() != 1 || Pair->members[0].ast.args.size() != 2) {
      ERROR(Pair->region.location(), "special constructor Pair not defined correctly");
    }
  } else {
    ERROR(LOCATION, "required data type Pair@wake not defined");
  }

  if (Result) {
    if (Result->members.size() != 2 || Result->members[0].ast.args.size() != 1 ||
        Result->members[1].ast.args.size() != 1) {
      ERROR(Result->region.location(), "special constructor Result not defined correctly");
    }
  } else {
    ERROR(LOCATION, "required data type Result@wake not defined");
  }

  if (JValue) {
    if (JValue->members.size() != 7 || JValue->members[0].ast.args.size() != 1 ||
        JValue->members[1].ast.args.size() != 1 || JValue->members[2].ast.args.size() != 1 ||
        JValue->members[3].ast.args.size() != 1 || JValue->members[4].ast.args.size() != 0 ||
        JValue->members[5].ast.args.size() != 1 || JValue->members[6].ast.args.size() != 1) {
      ERROR(JValue->region.location(), "special constructor JValue not defined correctly");
    }
  } else {
    ERROR(LOCATION, "required data type JValue@wake not defined");
  }

  if (Vector) {
    if (Vector->members.size() != 1 || Vector->members[0].ast.args.size() != 3) {
      ERROR(Vector->region.location(), "special constructor Vector not defined correctly");
    }
  } else {
    ERROR(LOCATION, "required data type Vector@wake not defined");
  }
}
