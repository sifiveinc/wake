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

#ifndef DATABASE_H
#define DATABASE_H

#include <memory>
#include <string>
#include <vector>

#include "json/json5.h"

struct FileReflection {
  std::string path;
  std::string hash;
  FileReflection(std::string &&path_, std::string &&hash_)
      : path(std::move(path_)), hash(std::move(hash_)) {}
};

struct Usage {
  bool found;
  int status;  // -signal, +code
  double runtime;
  double cputime;
  uint64_t membytes;
  uint64_t ibytes;
  uint64_t obytes;

  Usage() : found(false) {}
};

struct JobTag {
  long job;
  std::string uri;
  std::string content;
  JobTag(JobTag &&o) = default;
  JobTag(long job_, std::string &&uri_, std::string &&content_)
      : job(job_), uri(std::move(uri_)), content(std::move(content_)) {}
};

struct Time {
  int64_t t;
  Time() : t(0) {}
  explicit Time(int64_t _t) : t(_t) {}
  int64_t as_int64() const { return t; }
  std::string as_string() const;
};

struct RunReflection {
  int id;
  Time time;
  std::string cmdline;
  RunReflection() = default;
  RunReflection(int id_, int64_t time_, std::string cmdline_)
      : id(id_), time(time_), cmdline(cmdline_) {}
};

struct JobReflection {
  long job;
  bool stale;
  std::string label;
  std::string directory;
  std::vector<std::string> commandline;
  std::vector<std::string> environment;
  std::string stack;
  std::string stdin_file;
  Time starttime;
  Time endtime;
  Time wake_start;
  std::string wake_cmdline;
  // List of interleaved writes to (1) stdout, (2) stderr, (3) runner output, and (4) runner errors
  std::vector<std::pair<std::string, int>> std_writes;
  Usage usage;
  std::vector<FileReflection> visible;
  std::vector<FileReflection> inputs;
  std::vector<FileReflection> outputs;
  std::vector<JobTag> tags;

  JAST to_json() const;
  JAST to_structured_json() const;
  JAST to_simple_json() const;
};

struct JobEdge {
  long user;
  long used;
  JobEdge(long user_, long used_) : user(user_), used(used_) {}
};

struct FileDependency {
  long writer;  // The job that writes a file
  long reader;  // The job that reads said file

  JAST to_json() const;
};

struct Database {
  struct detail;
  std::unique_ptr<detail> imp;

  Database(bool debugdb);
  ~Database();

  std::string open(bool wait, bool memory, bool tty);
  void close();

  void entropy(uint64_t *key, int words);

  void prepare(const std::string &cmdline);  // prepare for job execution
  void clean();                              // finished execution; sweep stale jobs

  void begin_txn() const;
  void end_txn() const;

  Usage reuse_job(const std::string &directory, const std::string &environment,
                  const std::string &commandline,
                  const std::string &stdin_file,  // "" -> /dev/null
                  uint64_t signature, bool is_atty, const std::string &visible, bool check,
                  long &job, std::vector<FileReflection> &out, double *pathtime);
  Usage predict_job(uint64_t hashcode, double *pathtime);
  void insert_job(  // also wipes out any old runs
      const std::string &directory, const std::string &environment, const std::string &commandline,
      const std::string &stdin_file,  // "" -> /dev/null
      // ^^^ only these matter to identify the job
      uint64_t signature,  // this must match to qualify for reuse
      const std::string &label, const std::string &stack, bool is_atty, const std::string &visible,
      long *job);  // key used for accesses below
  void finish_job(long job,
                  const std::string &inputs,       // null separated
                  const std::string &outputs,      // null separated
                  const std::string &all_outputs,  // null seperated
                  int64_t starttime, int64_t endtime, uint64_t hashcode, bool keep, Usage reality);
  std::vector<FileReflection> get_tree(int kind, long job);

  void tag_job(long job, const std::string &uri, const std::string &content);

  void save_output(  // call only if needs_build -> true
      long job, int descriptor, const char *buffer, int size, double runtime);
  std::string get_output(long job, int descriptor) const;
  void replay_output(long job, const char *stdout, const char *stderr, const char *runner_out,
                     const char *runner_err);

  // Returns all files created by wake jobs
  std::vector<std::string> get_outputs() const;

  // A single transaction that does the following
  // 1) finds all files created by wake jobs
  // 2) clears all jobs
  // 3) removes all of those files
  // 4) finishes the transaction and returns the paths
  //    of the removed files
  std::vector<std::string> clear_jobs();

  void add_hash(const std::string &file, const std::string &hash, long modified);

  std::string get_hash(const std::string &file, long modified);

  // In core_filters, the outer vec is a set of filters to be AND'd together, inner vec is a set of
  // queries to be OR'd together. This holds for input_file_filters and output_file_filters as well
  // but is less useful as its restricted to the column 'path' in the files table.
  std::vector<JobReflection> matching(const std::vector<std::vector<std::string>> &core_filters,
                                      std::vector<std::vector<std::string>> input_file_filters,
                                      std::vector<std::vector<std::string>> output_file_filters);

  std::vector<JobEdge> get_edges();
  std::vector<JobTag> get_tags();

  std::vector<RunReflection> get_runs() const;

  std::vector<FileDependency> get_file_dependencies() const;

  std::vector<std::pair<std::string, int>> get_interleaved_output(long job_id) const;

  void set_runner_status(long job_id, int status);

  int get_runner_status(long job_id);
};

#endif
