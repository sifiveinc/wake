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

#include "describe.h"

#include <assert.h>
#include <re2/re2.h>

#include <deque>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "runtime/database.h"
#include "util/execpath.h"
#include "util/shell.h"
#include "util/term.h"

#define SHORT_HASH 8

static void indent(std::ostream &s, const std::string &tab, const std::string &body) {
  size_t i, j;
  for (i = 0; (j = body.find('\n', i)) != std::string::npos; i = j + 1) {
    s << "\n" << tab;
    s.write(body.data() + i, j - i);
  }
  s.write(body.data() + i, body.size() - i);
  s << std::endl;
}

static std::string describe_hash(const std::string &hash, bool verbose, bool stale) {
  if (stale) return "<out-of-date>";
  if (verbose) return hash;
  return hash.substr(0, SHORT_HASH);
}

static void describe_json(const std::vector<JobReflection> &jobs) {
  TermInfoBuf tbuf(std::cout.rdbuf());
  std::ostream out(&tbuf);

  JAST json(JSON_OBJECT);
  JAST &job_array = json.add("jobs", JSON_ARRAY);

  for (auto &job : jobs) {
    job_array.add("", job.to_structured_json());
  }

  out << json;
}

static void describe_metadata(const std::vector<JobReflection> &jobs, bool debug, bool verbose,
                              bool files) {
  TermInfoBuf tbuf(std::cout.rdbuf());
  std::ostream out(&tbuf);

  for (auto &job : jobs) {
    out << "Job " << job.job;
    if (!job.label.empty()) out << " (" << job.label << ")";
    out << ":" << std::endl << "  Command-line:";
    for (auto &arg : job.commandline) out << " " << shell_escape(arg);
    out << std::endl << "  Environment:" << std::endl;
    for (auto &env : job.environment) out << "    " << shell_escape(env) << std::endl;
    out << "  Directory: " << job.directory << std::endl
        << "  Built:     " << job.endtime.as_string() << std::endl
        << "  Runtime:   " << job.usage.runtime << std::endl
        << "  CPUtime:   " << job.usage.cputime << std::endl
        << "  Mem bytes: " << job.usage.membytes << std::endl
        << "  In  bytes: " << job.usage.ibytes << std::endl
        << "  Out bytes: " << job.usage.obytes << std::endl
        << "  Status:    " << job.usage.status << std::endl
        << "  Stdin:     " << job.stdin_file << std::endl;
    if (verbose) {
      out << "  Wake run:  " << job.wake_start.as_string() << " (" << job.wake_cmdline << ")"
          << std::endl;
      out << "Visible:" << std::endl;
      for (auto &in : job.visible)
        out << "  " << describe_hash(in.hash, verbose, job.stale) << " " << in.path << std::endl;
    }
    if (files) {
      out << "Inputs:" << std::endl;
      for (auto &in : job.inputs) {
        out << "  " << describe_hash(in.hash, verbose, job.stale) << " " << in.path << std::endl;
      }
      out << "Outputs:" << std::endl;
      for (auto &output : job.outputs) {
        out << "  " << describe_hash(output.hash, verbose, false) << " " << output.path
            << std::endl;
      }
    }
    if (debug) {
      out << "Stack:";
      indent(out, "  ", job.stack);
    }

    std::stringstream stdout_writes;
    std::stringstream stderr_writes;
    std::stringstream runner_out_writes;
    std::stringstream runner_err_writes;

    for (auto &write : job.std_writes) {
      if (write.second == 1) {
        stdout_writes << write.first;
      }
      if (write.second == 2) {
        stderr_writes << write.first;
      }
      if (write.second == 3) {
        runner_out_writes << write.first;
      }
      if (write.second == 4) {
        runner_err_writes << write.first;
      }
    }

    if (verbose) {
      std::string stdout_str = stdout_writes.str();
      if (!stdout_str.empty()) {
        out << "Stdout:";
        indent(out, "  ", stdout_str);
      }

      std::string stderr_str = stderr_writes.str();
      if (!stderr_str.empty()) {
        out << "Stderr:";
        indent(out, "  ", stderr_str);
      }

      std::string runner_out_str = runner_out_writes.str();
      if (!runner_out_str.empty()) {
        out << "Runner Output:";
        indent(out, "  ", runner_out_str);
      }

      std::string runner_err_str = runner_err_writes.str();
      if (!runner_err_str.empty()) {
        out << "Runner Error:";
        indent(out, "  ", runner_err_str);
      }
    }

    if (!job.tags.empty()) {
      out << "Tags:" << std::endl;
      for (auto &x : job.tags) {
        out << "  " << x.uri << ": ";
        indent(out, "    ", x.content);
      }
    }
  }
}

static void describe_shell(const std::vector<JobReflection> &jobs, bool debug, bool verbose) {
  TermInfoBuf tbuf(std::cout.rdbuf(), true);
  std::ostream out(&tbuf);

  out << "#! /bin/sh -ex" << std::endl;

  for (auto &job : jobs) {
    out << std::endl << "# Wake job " << job.job;
    if (!job.label.empty()) out << " (" << job.label << ")";
    out << ":" << std::endl;
    out << "cd " << shell_escape(get_cwd()) << std::endl;
    if (job.directory != ".") {
      out << "cd " << shell_escape(job.directory) << std::endl;
    }
    out << "env -i \\" << std::endl;
    for (auto &env : job.environment) {
      out << "\t" << shell_escape(env) << " \\" << std::endl;
    }
    for (auto &arg : job.commandline) {
      out << shell_escape(arg) << " \\" << std::endl << '\t';
    }
    out << "< " << shell_escape(job.stdin_file) << std::endl << std::endl;
    out << "# When wake ran this command:" << std::endl
        << "#   Built:     " << job.endtime.as_string() << std::endl
        << "#   Runtime:   " << job.usage.runtime << std::endl
        << "#   CPUtime:   " << job.usage.cputime << std::endl
        << "#   Mem bytes: " << job.usage.membytes << std::endl
        << "#   In  bytes: " << job.usage.ibytes << std::endl
        << "#   Out bytes: " << job.usage.obytes << std::endl
        << "#   Status:    " << job.usage.status << std::endl;
    if (verbose) {
      out << "#  Wake run:  " << job.wake_start.as_string() << " (" << job.wake_cmdline << ")"
          << std::endl;
      out << "# Visible:" << std::endl;
      for (auto &in : job.visible)
        out << "#  " << describe_hash(in.hash, verbose, job.stale) << " " << in.path << std::endl;
    }
    out << "# Inputs:" << std::endl;
    for (auto &in : job.inputs)
      out << "#  " << describe_hash(in.hash, verbose, job.stale) << " " << in.path << std::endl;
    out << "# Outputs:" << std::endl;
    for (auto &output : job.outputs)
      out << "#  " << describe_hash(output.hash, verbose, false) << " " << output.path << std::endl;
    if (debug) {
      out << "# Stack:";
      indent(out, "#   ", job.stack);
    }

    std::stringstream stdout_writes;
    std::stringstream stderr_writes;
    std::stringstream runner_out_writes;
    std::stringstream runner_err_writes;

    for (auto &write : job.std_writes) {
      if (write.second == 1) {
        stdout_writes << write.first;
      }
      if (write.second == 2) {
        stderr_writes << write.first;
      }
      if (write.second == 3) {
        runner_out_writes << write.first;
      }
      if (write.second == 4) {
        runner_err_writes << write.first;
      }
    }

    std::string stdout_str = stdout_writes.str();
    if (!stdout_str.empty()) {
      out << "# Stdout:";
      indent(out, "#   ", stdout_str);
    }

    std::string stderr_str = stderr_writes.str();
    if (!stderr_str.empty()) {
      out << "# Stderr:";
      indent(out, "#   ", stderr_str);
    }

    std::string runner_out_str = runner_out_writes.str();
    if (!runner_out_str.empty()) {
      out << "# Runner Output:";
      indent(out, "#   ", runner_out_str);
    }

    std::string runner_err_str = runner_err_writes.str();
    if (!runner_err_str.empty()) {
      out << "# Runner Error:";
      indent(out, "#   ", runner_err_str);
    }

    if (!job.tags.empty()) {
      out << "# Tags:" << std::endl;
      for (auto &x : job.tags) {
        out << "#   " << x.uri << ": ";
        indent(out, "#     ", x.content);
      }
    }
  }
}

void describe_simple(const std::vector<JobReflection> &jobs) {
  TermInfoBuf tbuf(std::cout.rdbuf());
  std::ostream out(&tbuf);
  for (size_t i = 0; i < jobs.size(); i++) {
    const auto &job = jobs[i];
    out << term_colour(TERM_GREEN) << "# " << job.label << " (" << job.job << ")";

    if (!job.tags.empty()) {
      out << " [";
      for (auto &tag : job.tags) {
        out << tag.uri << "=" << tag.content << ",";
      }
      out << "]";
    }

    out << "\n";
    out << term_normal() << "$ " << term_colour(TERM_CYAN);
    for (size_t i = 0; i < job.commandline.size(); i++) {
      const auto &cmd_part = job.commandline[i];
      out << cmd_part;

      if (i != job.commandline.size() - 1) {
        out << " ";
      }
    }

    out << "\n" << term_normal();

    if (i + 1 < jobs.size()) {
      out << "\n";
    }
  }
}

void describe_human(const std::vector<JobReflection> &jobs) {
  TermInfoBuf tbuf(std::cout.rdbuf());
  std::ostream out(&tbuf);
  for (size_t i = 0; i < jobs.size(); i++) {
    const auto &job = jobs[i];
    out << term_colour(TERM_GREEN) << "# " << job.label << " (" << job.job << ")";

    if (!job.tags.empty()) {
      out << " [";
      for (auto &tag : job.tags) {
        out << tag.uri << "=" << tag.content << ",";
      }
      out << "]";
    }

    out << "\n";
    out << term_normal() << "$ " << term_colour(TERM_CYAN);
    for (size_t i = 0; i < job.commandline.size(); i++) {
      const auto &cmd_part = job.commandline[i];
      out << cmd_part;

      if (i != job.commandline.size() - 1) {
        out << " ";
      }
    }

    out << "\n" << term_normal();

    // We have to use our special stream for the output of the program
    for (auto &log_line : job.std_writes) {
      out << log_line.first;
    }

    if (i + 1 < jobs.size()) {
      out << "\n";
    }
  }
}

void describe_timeline(const std::vector<JobReflection> &jobs,
                       const std::vector<FileDependency> &dependencies) {
  TermInfoBuf tbuf(std::cout.rdbuf(), true);
  std::ostream out(&tbuf);

  std::ifstream html_template(find_execpath() + "/../share/wake/html/timeline_template.html");
  std::ifstream arrow_library(find_execpath() + "/../share/wake/html/timeline_arrow_lib.js");
  std::ifstream main(find_execpath() + "/../share/wake/html/timeline_main.js");

  out << html_template.rdbuf();

  out << R"(<script type="application/json" id="jobReflections">)" << std::endl;
  out << JAST::from_vec(jobs);
  out << "</script>" << std::endl;

  out << R"(<script type="application/json" id="fileDependencies">)" << std::endl;
  out << JAST::from_vec(dependencies);
  out << "</script>" << std::endl;

  out << R"(<script type="text/javascript">)" << std::endl;
  out << arrow_library.rdbuf();
  out << "</script>" << std::endl;

  out << R"(<script type="module">)" << std::endl;
  out << main.rdbuf();
  out << "</script>\n"
         "</body>\n"
         "</html>\n";
}

void describe_simple_timeline(const std::vector<JobReflection> &jobs,
                              const std::vector<FileDependency> &dependencies) {
  TermInfoBuf tbuf(std::cout.rdbuf(), true);
  std::ostream out(&tbuf);

  std::ifstream html_template(find_execpath() + "/../share/wake/html/timeline_template.html");
  std::ifstream arrow_library(find_execpath() + "/../share/wake/html/timeline_arrow_lib.js");
  std::ifstream main(find_execpath() + "/../share/wake/html/timeline_main.js");

  out << html_template.rdbuf();

  out << R"(<script type="application/json" id="jobReflections">)" << std::endl;
  {
    JAST json(JSON_ARRAY);
    for (const JobReflection &j : jobs) {
      json.add("", j.to_simple_json());
    }
    out << json;
  }
  out << "</script>" << std::endl;

  out << R"(<script type="application/json" id="fileDependencies">)" << std::endl;
  out << JAST::from_vec(dependencies);
  out << "</script>" << std::endl;

  out << R"(<script type="text/javascript">)" << std::endl;
  out << arrow_library.rdbuf();
  out << "</script>" << std::endl;

  out << R"(<script type="module">)" << std::endl;
  out << main.rdbuf();
  out << "</script>\n"
         "</body>\n"
         "</html>\n";
}

void describe(const std::vector<JobReflection> &jobs, DescribePolicy policy, const Database &db) {
  switch (policy.type) {
    case DescribePolicy::SCRIPT: {
      describe_shell(jobs, true, true);
      break;
    }
    case DescribePolicy::HUMAN: {
      describe_human(jobs);
      break;
    }
    case DescribePolicy::METADATA: {
      describe_metadata(jobs, false, false, true);
      break;
    }
    case DescribePolicy::SIMPLE_METADATA: {
      describe_metadata(jobs, false, false, false);
      break;
    }
    case DescribePolicy::JSON: {
      describe_json(jobs);
      break;
    }
    case DescribePolicy::DEBUG: {
      describe_metadata(jobs, true, true, true);
      break;
    }
    case DescribePolicy::VERBOSE: {
      describe_metadata(jobs, false, true, true);
      break;
    }
    case DescribePolicy::TAG_URI: {
      for (auto &job : jobs)
        for (auto &tag : job.tags)
          if (tag.uri == policy.tag) std::cout << tag.content << std::endl;
      break;
    }
    case DescribePolicy::SIMPLE_TIMELINE: {
      std::unordered_set<long> job_ids;
      for (const JobReflection &job : jobs) {
        job_ids.insert(job.job);
      }

      std::vector<FileDependency> all_deps = db.get_file_dependencies();
      std::vector<FileDependency> filtered_deps;

      while (!all_deps.empty()) {
        FileDependency last = std::move(all_deps.back());
        all_deps.pop_back();
        if (job_ids.count(last.reader) && job_ids.count(last.writer)) {
          filtered_deps.emplace_back(std::move(last));
        }
      }

      describe_simple_timeline(jobs, filtered_deps);
      break;
    }
    case DescribePolicy::TIMELINE: {
      std::unordered_set<long> job_ids;
      for (const JobReflection &job : jobs) {
        job_ids.insert(job.job);
      }

      std::vector<FileDependency> all_deps = db.get_file_dependencies();
      std::vector<FileDependency> filtered_deps;

      while (!all_deps.empty()) {
        FileDependency last = std::move(all_deps.back());
        all_deps.pop_back();
        if (job_ids.count(last.reader) && job_ids.count(last.writer)) {
          filtered_deps.emplace_back(std::move(last));
        }
      }

      describe_timeline(jobs, filtered_deps);
      break;
    }
    case DescribePolicy::SIMPLE: {
      describe_simple(jobs);
      break;
    }
  }
}

class BitVector {
 public:
  BitVector() : imp() {}
  BitVector(BitVector &&x) : imp(std::move(x.imp)) {}

  bool get(size_t i) const;
  void toggle(size_t i);
  long max() const;

  // Bulk operators
  BitVector &operator|=(const BitVector &o);
  void clear(const BitVector &o);

 private:
  mutable std::vector<uint64_t> imp;
};

bool BitVector::get(size_t i) const {
  size_t j = i / 64, k = i % 64;
  if (j >= imp.size()) return false;
  return (imp[j] >> k) & 1;
}

void BitVector::toggle(size_t i) {
  size_t j = i / 64, k = i % 64;
  if (j >= imp.size()) imp.resize(j + 1, 0);
  imp[j] ^= static_cast<uint64_t>(1) << k;
}

long BitVector::max() const {
  while (!imp.empty()) {
    uint64_t x = imp.back();
    if (x != 0) {
      // Find the highest set bit
      int best = 0;
      for (int i = 0; i < 64; ++i)
        if (((x >> i) & 1)) best = i;
      return ((imp.size() - 1) * 64) + best;
    } else {
      imp.pop_back();
    }
  }
  return -1;
}

BitVector &BitVector::operator|=(const BitVector &o) {
  size_t both = std::min(imp.size(), o.imp.size());
  for (size_t i = 0; i < both; ++i) imp[i] |= o.imp[i];
  if (imp.size() < o.imp.size()) imp.insert(imp.end(), o.imp.begin() + both, o.imp.end());
  return *this;
}

void BitVector::clear(const BitVector &o) {
  size_t both = std::min(imp.size(), o.imp.size());
  for (size_t i = 0; i < both; ++i) imp[i] &= ~o.imp[i];
}

struct GraphNode {
  size_t usedUp, usesUp;
  std::vector<long> usedBy;
  std::vector<long> uses;
  BitVector closure;
  GraphNode() : usedUp(0), usesUp(0) {}
};

std::ostream &operator<<(std::ostream &os, const GraphNode &node) {
  os << "  uses";
  for (auto x : node.uses) os << " " << x;
  os << std::endl;
  os << "  usedBy";
  for (auto x : node.usedBy) os << " " << x;
  os << std::endl;
  os << "  closure ";
  for (long i = 0; i <= node.closure.max(); ++i) os << (node.closure.get(i) ? "X" : " ");
  return os << std::endl;
}

void output_tagdag(Database &db, const std::string &tagExpr) {
  RE2 exp(tagExpr);

  // Pick only those tags that match the RegExp
  std::unordered_multimap<long, JobTag> relevant;
  for (auto &tag : db.get_tags())
    if (RE2::FullMatch(tag.uri, exp)) relevant.emplace(tag.job, std::move(tag));

  // Create a bidirectional view of the graph
  std::unordered_map<long, GraphNode> graph;
  auto edges = db.get_edges();
  for (auto &x : edges) {
    graph[x.user].uses.push_back(x.used);
    graph[x.used].usedBy.push_back(x.user);
  }

  // Working queue for Job ids
  std::deque<long> queue;
  // Compressed map for tags
  std::vector<JobTag> uris;

  // Explore from all nodes which use nothing (ie: build leafs)
  for (auto &n : graph)
    if (n.second.uses.empty()) queue.push_back(n.first);

  // As we explore, accumulate the transitive closure of relevant nodes via BitVector
  while (!queue.empty()) {
    long job = queue.front();
    queue.pop_front();
    GraphNode &me = graph[job];

    // Compute the closure over anything relevant we use
    for (auto usesJob : me.uses) me.closure |= graph[usesJob].closure;

    // If we are relevant, add us to the bitvector, and top-sort the relevant jobs
    auto rel = relevant.equal_range(job);
    if (rel.first != rel.second) {
      me.closure.toggle(uris.size());
      for (; rel.first != rel.second; ++rel.first) uris.emplace_back(std::move(rel.first->second));
    }

    // Enqueue anything for which we are the last dependent
    for (auto userJob : me.usedBy) {
      GraphNode &user = graph[userJob];
      assert(user.usesUp < user.uses.size());
      if (++user.usesUp == user.uses.size()) queue.push_back(userJob);
    }
  }

  // Explore from nodes used by nothing (ie: build targets)
  for (auto &n : graph)
    if (n.second.usedBy.empty()) queue.push_back(n.first);

  // As we explore, emit those nodes which are relevant to JSON
  JAST out(JSON_ARRAY);
  while (!queue.empty()) {
    long job = queue.front();
    queue.pop_front();
    GraphNode &me = graph[job];

    // Enqueue anything for which we are the last user
    for (auto usesJob : me.uses) {
      GraphNode &uses = graph[usesJob];
      assert(uses.usedUp < uses.usedBy.size());
      if (++uses.usedUp == uses.usedBy.size()) queue.push_back(usesJob);
    }

    // If we are a relevant node, compute the closure
    if (relevant.find(job) == relevant.end()) continue;

    // Get our own name
    long max = me.closure.max();
    assert(max != -1 && me.closure.get(max));
    me.closure.toggle(max);

    JAST &entry = out.add(JSON_OBJECT);
    entry.add("job", JSON_INTEGER, std::to_string(uris[max].job));

    JAST &tags = entry.add("tags", JSON_OBJECT);
    for (size_t i = max; i < uris.size() && uris[i].job == job; ++i)
      tags.add(std::move(uris[i].uri), std::move(uris[i].content));

    JAST &deps = entry.add("deps", JSON_ARRAY);
    while ((max = me.closure.max()) != -1) {
      long depJob = uris[max].job;
      GraphNode &dep = graph[depJob];
      // Add this dependency
      deps.add(JSON_INTEGER, std::to_string(depJob));
      // Elminate transitively reachable children
      assert(dep.closure.get(max));
      me.closure.clear(dep.closure);
    }
  }

  std::cout << out << std::endl;
}
