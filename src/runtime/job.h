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

#ifndef JOB_H
#define JOB_H

#include <cstdint>
#include <memory>
#include <string>

struct Database;
struct Runtime;

class ResourceBudget {
 public:
  ResourceBudget(double percentage_ = 0) : percentage(percentage_), fixed(0) {}

  double get(uint64_t max_available) const {
    if (fixed) {
      return fixed;
    } else {
      return max_available * percentage;
    }
  }

  // returns: nullptr on success; else a string describing the problem
  static const char *parse(const char *str, ResourceBudget &output);
  // Format an integer with 'kiB', 'MiB', 'GiB', etc as appropriate.
  // The values are rounded to nearest when reduced.
  // Guarantee is that the output string will have at most 4 digits.
  // If the number is >= 10, the output will have at least 2 digits.
  static std::string format(uint64_t x);

 private:
  // At least one must be 0 (= invalid)
  double percentage;
  uint64_t fixed;
};

struct JobTable {
  struct detail;
  std::unique_ptr<detail> imp;

  JobTable(Database *db, ResourceBudget memory, ResourceBudget cpu, bool debug, bool verbose,
           bool quiet, bool check, bool batch);
  ~JobTable();

  // Wait for a job to complete; false -> no more active jobs
  bool wait(Runtime &runtime);
  static bool exit_now();
};

#endif
