/*
 * Copyright 2024 SiFive, Inc.
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

#ifndef CAS_PRIM_H
#define CAS_PRIM_H

#include <memory>
#include <string>

#include "cas/cas_store.h"
#include "types/primfn.h"

namespace cas {
class CASStore;
}

// Context for CAS operations, manages the CASStore lifecycle
class CASContext {
 public:
  CASContext() = default;
  ~CASContext() = default;

  // Get or create the CAS store for a workspace
  // Returns nullptr if CAS initialization fails
  cas::CASStore* get_store(const std::string& workspace);

  // Check if CAS is available
  bool has_store() const { return store_ != nullptr; }

 private:
  std::unique_ptr<cas::CASStore> store_;
  std::string workspace_;
};

// Register CAS primitives with the Wake runtime
void prim_register_cas(CASContext* ctx, PrimMap& pmap);

#endif

