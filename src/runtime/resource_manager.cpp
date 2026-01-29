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

#include "resource_manager.h"

#include <iostream>
#include <sstream>

#include "json/json5.h"

ResourceManager::ResourceManager(const ResourceLimits& limits) : limits_(limits) {
  // Initialize available counts to the configured limits
  for (const auto& kv : limits_.limits) {
    available_[kv.first] = kv.second;
  }
}

bool ResourceManager::can_acquire(const std::vector<ResourceRequirement>& requirements) const {
  if (requirements.empty()) {
    return true;
  }

  for (const auto& req : requirements) {
    if (req.count <= 0) {
      continue;  // Skip invalid requirements
    }

    // Check if this resource has a configured limit
    int64_t lim = limits_.get_limit(req.name);
    if (lim < 0) {
      // No limit configured, always available
      continue;
    }

    // Check if enough resources are available
    auto it = available_.find(req.name);
    if (it == available_.end()) {
      // Should not happen if limits_ and available_ are in sync
      return false;
    }

    if (it->second < req.count) {
      // Not enough resources available
      return false;
    }
  }

  return true;
}

void ResourceManager::acquire(const std::vector<ResourceRequirement>& requirements) {
  for (const auto& req : requirements) {
    if (req.count <= 0) {
      continue;
    }

    // Only decrement if this resource has a configured limit
    int64_t lim = limits_.get_limit(req.name);
    if (lim < 0) {
      continue;  // No limit, nothing to track
    }

    auto it = available_.find(req.name);
    if (it != available_.end()) {
      it->second -= req.count;
    }
  }
}

void ResourceManager::release(const std::vector<ResourceRequirement>& requirements) {
  for (const auto& req : requirements) {
    if (req.count <= 0) {
      continue;
    }

    // Only increment if this resource has a configured limit
    int64_t lim = limits_.get_limit(req.name);
    if (lim < 0) {
      continue;  // No limit, nothing to track
    }

    auto it = available_.find(req.name);
    if (it != available_.end()) {
      it->second += req.count;
      // Cap at the configured limit
      if (it->second > lim) {
        it->second = lim;
      }
    }
  }
}

int64_t ResourceManager::available(const std::string& name) const {
  auto it = available_.find(name);
  if (it != available_.end()) {
    return it->second;
  }
  return -1;  // Not tracked (unlimited)
}

int64_t ResourceManager::limit(const std::string& name) const { return limits_.get_limit(name); }

std::vector<ResourceRequirement> ResourceManager::parse_resources_json(const std::string& json_str) {
  std::vector<ResourceRequirement> result;

  if (json_str.empty()) {
    return result;
  }

  // Parse the JSON string
  // Format: [{"name": "resource_1", "count": 1}, {"name": "resource_2", "count": 2}]
  JAST jast;
  std::stringstream errors;
  if (!JAST::parse(json_str, errors, jast)) {
    std::cerr << "Failed to parse resources JSON: " << errors.str() << std::endl;
    return result;
  }

  if (jast.kind != JSON_ARRAY) {
    return result;
  }

  for (const auto& child : jast.children) {
    if (child.second.kind != JSON_OBJECT) {
      continue;
    }

    std::string name;
    int64_t count = 0;

    for (const auto& field : child.second.children) {
      if (field.first == "name" && field.second.kind == JSON_STR) {
        name = field.second.value;
      } else if (field.first == "count" && field.second.kind == JSON_INTEGER) {
        auto val = field.second.expect_integer();
        if (val) {
          count = *val;
        }
      }
    }

    if (!name.empty() && count > 0) {
      result.emplace_back(name, count);
    }
  }

  return result;
}

