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

#ifndef RECORD_HELPERS_H
#define RECORD_HELPERS_H

#include <string>
#include <vector>

#include "types/sums.h"
#include "value.h"

// ---------------------------------------------------------------------------
// rh — Record field helpers with Promise-retry semantics.
//
// Each function reads one field from a Wake Record, returning nullptr on
// success (with the output written) or the first unfulfilled Promise so the
// caller can suspend and retry when that Promise is fulfilled.
//
// Typical usage inside a collect_* function:
//
//   Promise *broken;
//   #define TRY(expr) do { broken = (expr); if (broken) return broken; } while(0)
//   TRY(rh::string(rec, 0, data.label));
//   TRY(rh::integer(rec, 1, data.count));
//   #undef TRY
// ---------------------------------------------------------------------------

namespace rh {

inline Promise *string(Record *rec, int idx, std::string &out) {
  Promise *p = rec->at(idx);
  if (!*p) return p;
  out = p->coerce<String>()->as_str();
  return nullptr;
}

inline Promise *boolean(Record *rec, int idx, bool &out) {
  Promise *p = rec->at(idx);
  if (!*p) return p;
  // Boolean: True = members[0], False = members[1]
  out = (p->coerce<Record>()->cons == &Boolean->members[0]);
  return nullptr;
}

inline Promise *integer(Record *rec, int idx, long &out) {
  Promise *p = rec->at(idx);
  if (!*p) return p;
  mpz_t v = {p->coerce<Integer>()->wrap()};
  out = mpz_get_si(v);
  return nullptr;
}

// Option Integer: sets has_val=true and out if Some, has_val=false if None.
inline Promise *opt_integer(Record *rec, int idx, bool &has_val, long &out) {
  Promise *p = rec->at(idx);
  if (!*p) return p;
  Record *opt = p->coerce<Record>();
  // Option: Some a = index 0 (one inner field), None = index 1
  if (opt->cons->index == 0) {
    Promise *inner = opt->at(0);
    if (!*inner) return inner;
    mpz_t v = {inner->coerce<Integer>()->wrap()};
    out = mpz_get_si(v);
    has_val = true;
  } else {
    has_val = false;
  }
  return nullptr;
}

inline Promise *string_list(Record *rec, int idx, std::vector<std::string> &out) {
  Promise *p = rec->at(idx);
  if (!*p) return p;
  Record *list = p->coerce<Record>();
  while (list->cons == &List->members[1]) {  // Cons
    Promise *head = list->at(0);
    if (!*head) return head;
    out.push_back(head->coerce<String>()->as_str());
    Promise *tail = list->at(1);
    if (!*tail) return tail;
    list = tail->coerce<Record>();
  }
  return nullptr;
}

}  // namespace rh

#endif
