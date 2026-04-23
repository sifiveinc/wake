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

#ifndef DATA_H
#define DATA_H

#include "type.h"

struct Data {
  // compiler builtins
  static TypeVar typeString;
  static TypeVar typeInteger;
  static TypeVar typeDouble;
  static TypeVar typeRegExp;
  static TypeVar typeJob;
  static TypeVar typeTarget;
  static TypeVar typeTime;
  // standard library supplied
  static TypeVar typeBoolean;
  static TypeVar typeOrder;
  static TypeVar typeUnit;
  static TypeVar typeJValue;
  static TypeVar typeError;
  // these are const to prevent unify() on them; use clone
  static const TypeVar typeList;
  static const TypeVar typePair;
  static const TypeVar typeResult;
};

#endif
