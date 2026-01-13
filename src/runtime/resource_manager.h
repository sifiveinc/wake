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

#ifndef RESOURCE_MANAGER_H
#define RESOURCE_MANAGER_H

#include <map>
#include <string>
#include <vector>

// Represents a single resource requirement from a job
struct ResourceRequirement {
  std::string name;
  int64_t count;

  ResourceRequirement() : name(""), count(0) {}
  ResourceRequirement(const std::string& n, int64_t c) : name(n), count(c) {}
};

// Configuration for resource limits, parsed from .wakeroot
struct ResourceLimits {
  // Map from resource name to maximum available count
  std::map<std::string, int64_t> limits;

  // Default constructor
  ResourceLimits() = default;

  // Get the limit for a resource, returns -1 if unlimited (not configured)
  int64_t get_limit(const std::string& name) const {
    auto it = limits.find(name);
    if (it != limits.end()) {
      return it->second;
    }
    return -1;  // Unlimited
  }

  bool empty() const { return limits.empty(); }
};

// Manages resource allocation for jobs
// Uses counting semaphores to limit concurrent usage
class ResourceManager {
 public:
  // Initialize with resource limits from config
  explicit ResourceManager(const ResourceLimits& limits);

  // Default constructor for when no limits are configured
  ResourceManager() = default;

  // Check if resources can be acquired for a job (non-blocking)
  // Returns true if all required resources are available
  bool can_acquire(const std::vector<ResourceRequirement>& requirements) const;

  // Acquire resources for a job
  // Decrements available counts for each resource
  // Caller should check can_acquire first
  void acquire(const std::vector<ResourceRequirement>& requirements);

  // Release resources when a job completes
  // Increments available counts for each resource
  void release(const std::vector<ResourceRequirement>& requirements);

  // Get current available count for a resource
  int64_t available(const std::string& name) const;

  // Get the limit for a resource (-1 if unlimited)
  int64_t limit(const std::string& name) const;

  // Check if any resource limits are configured
  bool has_limits() const { return !limits_.empty(); }

  // Parse resource requirements from JSON string
  // Format: [{"name": "resource_1", "count": 1}, {"name": "resource_2", "count": 2}]
  static std::vector<ResourceRequirement> parse_resources_json(const std::string& json_str);

 private:
  // Configured limits for each resource type
  ResourceLimits limits_;

  // Current available count for each resource type
  // Only tracks resources that have limits configured
  std::map<std::string, int64_t> available_;
};

#endif  // RESOURCE_MANAGER_H

