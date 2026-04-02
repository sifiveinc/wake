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

#ifndef CAS_CONTEXT_H
#define CAS_CONTEXT_H

#include <memory>
#include <string>
#include <utility>

#include "cas/cas.h"

// Runtime-owned CAS store handle for a single workspace.
class CASContext {
 public:
  explicit CASContext(std::string workspace) : workspace_(std::move(workspace)) {
    std::string cas_root = workspace_ + "/.cas";
    auto store_result = cas::Cas::open(cas_root);
    if (store_result) {
      store_ = std::make_unique<cas::Cas>(std::move(*store_result));
    }
  }
  ~CASContext() = default;

  cas::Cas* get_store() const { return store_.get(); }

  bool has_store() const { return store_ != nullptr; }

 private:
  std::unique_ptr<cas::Cas> store_;
  std::string workspace_;
};

#endif
