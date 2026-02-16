/*
 * Copyright 2022 SiFive, Inc.
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

#pragma once

#include <sys/stat.h>

#include <optional>
#include <string>
#include <vector>

#include <optional>

#include "gopt/gopt-arg.h"
#include "gopt/gopt.h"
#include "wcl/iterator.h"

struct CommandLineOptions {
  bool check;
  bool verbose;
  bool debug;
  bool quiet;
  bool wait;
  bool workspace;
  bool tty;
  bool fwarning;
  int profileh;
  bool last_use;
  bool last_exe;
  bool history;
  bool lsp;
  bool failed;
  bool script;
  bool metadata;
  bool simple_metadata;
  bool json;
  bool version;
  bool html;
  bool global;
  bool help;
  bool config;
  bool debugdb;
  bool parse;
  bool tcheck;
  bool dumpssa;
  bool optim;
  bool exports;
  bool timeline;
  bool simple_timeline;
  bool simple;
  bool canceled;
  bool clean;
  bool list_outputs;
  bool include_hidden;
  std::optional<bool> log_header_align;
  std::optional<bool> cache_miss_on_failure;

  const char *percent_str;
  const char *jobs_str;
  const char *memory_str;
  const char *heapf;
  const char *profile;
  const char *init;
  const char *chdir;
  const char *in;
  const char *exec;
  char *shebang;
  const char *tagdag;
  const char *taguri;
  const char *api;
  const char *fd1;
  const char *fd2;
  const char *fd3;
  const char *fd4;
  const char *fd5;
  const char *label_filter;  // TODO: Allow unions of multiple filters
  const char *log_header;
  const char *user_config;

  std::optional<int64_t> log_header_source_width;

  std::vector<std::vector<std::string>> job_ids = {};
  std::vector<std::vector<std::string>> input_files = {};
  std::vector<std::vector<std::string>> output_files = {};
  std::vector<std::vector<std::string>> labels = {};
  std::vector<std::vector<std::string>> tags = {};

  int argc;
  char **argv;

  CommandLineOptions(int argc_in, char **argv_in) {
    argv = argv_in;
    std::vector<char *> job_ids_buffer(argc_in, nullptr);
    std::vector<char *> input_files_buffer(argc_in, nullptr);
    std::vector<char *> output_files_buffer(argc_in, nullptr);
    std::vector<char *> labels_buffer(argc_in, nullptr);
    std::vector<char *> tags_buffer(argc_in, nullptr);

    // clang-format off
    struct option options[] {
      {'p', "percent", GOPT_ARGUMENT_REQUIRED | GOPT_ARGUMENT_NO_HYPHEN},
      {'j', "jobs", GOPT_ARGUMENT_REQUIRED | GOPT_ARGUMENT_NO_HYPHEN},
      {'m', "memory", GOPT_ARGUMENT_REQUIRED | GOPT_ARGUMENT_NO_HYPHEN},
      {'c', "check", GOPT_ARGUMENT_FORBIDDEN},
      {'v', "verbose", GOPT_ARGUMENT_FORBIDDEN | GOPT_REPEATABLE},
      {'d', "debug", GOPT_ARGUMENT_FORBIDDEN},
      {'q', "quiet", GOPT_ARGUMENT_FORBIDDEN},
      {0, "no-wait", GOPT_ARGUMENT_FORBIDDEN},
      {0, "no-workspace", GOPT_ARGUMENT_FORBIDDEN},
      {0, "no-tty", GOPT_ARGUMENT_FORBIDDEN},
      {0, "fatal-warnings", GOPT_ARGUMENT_FORBIDDEN},
      {0, "heap-factor", GOPT_ARGUMENT_REQUIRED | GOPT_ARGUMENT_NO_HYPHEN},
      {0, "profile-heap", GOPT_ARGUMENT_FORBIDDEN | GOPT_REPEATABLE},
      {0, "profile", GOPT_ARGUMENT_REQUIRED},
      {'C', "chdir", GOPT_ARGUMENT_REQUIRED},
      {0, "in", GOPT_ARGUMENT_REQUIRED},
      {'x', "exec", GOPT_ARGUMENT_REQUIRED},
      {0, "job", GOPT_ARGUMENT_REQUIRED | GOPT_REPEATABLE_VALUE, job_ids_buffer.data(), (unsigned int)argc_in},
      {'i', "input", GOPT_ARGUMENT_REQUIRED | GOPT_REPEATABLE_VALUE, input_files_buffer.data(), (unsigned int)argc_in},
      {'o', "output", GOPT_ARGUMENT_REQUIRED | GOPT_REPEATABLE_VALUE, output_files_buffer.data(), (unsigned int)argc_in},
      {0, "label", GOPT_ARGUMENT_REQUIRED | GOPT_REPEATABLE_VALUE, labels_buffer.data(), (unsigned int)argc_in},
      {0, "tag", GOPT_ARGUMENT_REQUIRED | GOPT_REPEATABLE_VALUE, tags_buffer.data(), (unsigned int)argc_in},
      {'l', "last", GOPT_ARGUMENT_FORBIDDEN},
      {0, "last-used", GOPT_ARGUMENT_FORBIDDEN},
      {0, "last-executed", GOPT_ARGUMENT_FORBIDDEN},
      {0, "history", GOPT_ARGUMENT_FORBIDDEN},
      {0, "lsp", GOPT_ARGUMENT_FORBIDDEN},
      {'f', "failed", GOPT_ARGUMENT_FORBIDDEN},
      {'s', "script", GOPT_ARGUMENT_FORBIDDEN},
      {0, "metadata", GOPT_ARGUMENT_FORBIDDEN},
      {0, "simple-metadata", GOPT_ARGUMENT_FORBIDDEN},
      {0, "json", GOPT_ARGUMENT_FORBIDDEN},
      {0, "init", GOPT_ARGUMENT_REQUIRED},
      {0, "version", GOPT_ARGUMENT_FORBIDDEN},
      {'g', "globals", GOPT_ARGUMENT_FORBIDDEN},
      {'e', "exports", GOPT_ARGUMENT_FORBIDDEN},
      {0, "html", GOPT_ARGUMENT_FORBIDDEN},
      {0, "timeline", GOPT_ARGUMENT_FORBIDDEN},
      {0, "simple-timeline", GOPT_ARGUMENT_FORBIDDEN},
      {0, "simple", GOPT_ARGUMENT_FORBIDDEN},
      {0, "canceled", GOPT_ARGUMENT_FORBIDDEN},
      {'h', "help", GOPT_ARGUMENT_FORBIDDEN},
      {0, "config", GOPT_ARGUMENT_FORBIDDEN},
      {0, "debug-db", GOPT_ARGUMENT_FORBIDDEN},
      {0, "stop-after-parse", GOPT_ARGUMENT_FORBIDDEN},
      {0, "stop-after-type-check", GOPT_ARGUMENT_FORBIDDEN},
      {0, "stop-after-ssa", GOPT_ARGUMENT_FORBIDDEN},
      {0, "no-optimize", GOPT_ARGUMENT_FORBIDDEN},
      {0, "tag-dag", GOPT_ARGUMENT_REQUIRED},
      {0, "tag-uri", GOPT_ARGUMENT_REQUIRED},
      {0, "export-api", GOPT_ARGUMENT_REQUIRED},
      {0, "stdout", GOPT_ARGUMENT_REQUIRED},
      {0, "stderr", GOPT_ARGUMENT_REQUIRED},
      {0, "clean", GOPT_ARGUMENT_FORBIDDEN },
      {0, "list-outputs", GOPT_ARGUMENT_FORBIDDEN },
      {0, "fd:3", GOPT_ARGUMENT_REQUIRED},
      {0, "fd:4", GOPT_ARGUMENT_REQUIRED},
      {0, "fd:5", GOPT_ARGUMENT_REQUIRED},
      {0, "label-filter", GOPT_ARGUMENT_REQUIRED},
      {0, "log-header", GOPT_ARGUMENT_REQUIRED},
      {0, "log-header-source-width", GOPT_ARGUMENT_REQUIRED},
      {0, "log-header-align", GOPT_ARGUMENT_FORBIDDEN},
      {0, "no-log-header-align", GOPT_ARGUMENT_FORBIDDEN},
      {0, "cache-miss-on-failure", GOPT_ARGUMENT_FORBIDDEN},
      {0, "no-cache-miss-on-failure", GOPT_ARGUMENT_FORBIDDEN},
      {0, "user-config", GOPT_ARGUMENT_REQUIRED},
      {':', "shebang", GOPT_ARGUMENT_REQUIRED},
      {0, "include-hidden", GOPT_ARGUMENT_FORBIDDEN},
      {0, 0, GOPT_LAST}
    };
    // clang-format on

    argc = gopt(argv, options);
    gopt_errors(argv[0], options);

    check = arg(options, "check")->count;
    verbose = arg(options, "verbose")->count;
    debug = arg(options, "debug")->count;
    quiet = arg(options, "quiet")->count;
    wait = !arg(options, "no-wait")->count;
    workspace = !arg(options, "no-workspace")->count;
    tty = !arg(options, "no-tty")->count;
    fwarning = arg(options, "fatal-warnings")->count;
    profileh = arg(options, "profile-heap")->count;
    last_use = arg(options, "last")->count || arg(options, "last-used")->count;
    last_exe = arg(options, "last-executed")->count;
    history = arg(options, "history")->count;
    lsp = arg(options, "lsp")->count;
    failed = arg(options, "failed")->count;
    script = arg(options, "script")->count;
    metadata = arg(options, "metadata")->count;
    simple_metadata = arg(options, "simple-metadata")->count;
    json = arg(options, "json")->count;
    version = arg(options, "version")->count;
    html = arg(options, "html")->count;
    global = arg(options, "globals")->count;
    help = arg(options, "help")->count;
    config = arg(options, "config")->count;
    debugdb = arg(options, "debug-db")->count;
    parse = arg(options, "stop-after-parse")->count;
    tcheck = arg(options, "stop-after-type-check")->count;
    dumpssa = arg(options, "stop-after-ssa")->count;
    optim = !arg(options, "no-optimize")->count;
    exports = arg(options, "exports")->count;
    timeline = arg(options, "timeline")->count;
    simple_timeline = arg(options, "simple-timeline")->count;
    simple = arg(options, "simple")->count;
    canceled = arg(options, "canceled")->count;
    clean = arg(options, "clean")->count;
    list_outputs = arg(options, "list-outputs")->count;
    include_hidden = arg(options, "include-hidden")->count;

    percent_str = arg(options, "percent")->argument;
    jobs_str = arg(options, "jobs")->argument;
    memory_str = arg(options, "memory")->argument;
    heapf = arg(options, "heap-factor")->argument;
    profile = arg(options, "profile")->argument;
    init = arg(options, "init")->argument;
    chdir = arg(options, "chdir")->argument;
    in = arg(options, "in")->argument;
    exec = arg(options, "exec")->argument;
    shebang = arg(options, "shebang")->argument;
    tagdag = arg(options, "tag-dag")->argument;
    taguri = arg(options, "tag-uri")->argument;
    api = arg(options, "export-api")->argument;
    fd1 = arg(options, "stdout")->argument;
    fd2 = arg(options, "stderr")->argument;
    fd3 = arg(options, "fd:3")->argument;
    fd4 = arg(options, "fd:4")->argument;
    fd5 = arg(options, "fd:5")->argument;
    label_filter = arg(options, "label-filter")->argument;
    log_header = arg(options, "log-header")->argument;
    user_config = arg(options, "user-config")->argument;

    if (arg(options, "log-header-align")->count) {
      log_header_align = std::make_optional(true);
    }

    if (arg(options, "no-log-header-align")->count) {
      log_header_align = std::make_optional(false);
    }

    if (arg(options, "cache-miss-on-failure")->count) {
      cache_miss_on_failure = std::make_optional(true);
    }

    if (arg(options, "no-cache-miss-on-failure")->count) {
      cache_miss_on_failure = std::make_optional(false);
    }

    auto lhsw_str = arg(options, "log-header-source-width")->argument;
    if (lhsw_str) log_header_source_width = std::optional<int64_t>{std::stol(lhsw_str)};

    for (unsigned int i = 0; i < arg(options, "job")->count; i++) {
      std::string line(job_ids_buffer[i]);
      std::vector<std::string> parts = wcl::split_by_fn(
          ',', line.begin(), line.end(), [](auto a, auto b) { return std::string(a, b); });
      job_ids.emplace_back(std::move(parts));
    }

    for (unsigned int i = 0; i < arg(options, "input")->count; i++) {
      std::string line(input_files_buffer[i]);
      std::vector<std::string> parts = wcl::split_by_fn(
          ',', line.begin(), line.end(), [](auto a, auto b) { return std::string(a, b); });
      input_files.emplace_back(std::move(parts));
    }

    for (unsigned int i = 0; i < arg(options, "output")->count; i++) {
      std::string line(output_files_buffer[i]);
      std::vector<std::string> parts = wcl::split_by_fn(
          ',', line.begin(), line.end(), [](auto a, auto b) { return std::string(a, b); });
      output_files.emplace_back(std::move(parts));
    }

    for (unsigned int i = 0; i < arg(options, "label")->count; i++) {
      std::string line(labels_buffer[i]);
      std::vector<std::string> parts = wcl::split_by_fn(
          ',', line.begin(), line.end(), [](auto a, auto b) { return std::string(a, b); });
      labels.emplace_back(std::move(parts));
    }

    for (unsigned int i = 0; i < arg(options, "tag")->count; i++) {
      std::string line(tags_buffer[i]);
      std::vector<std::string> parts = wcl::split_by_fn(
          ',', line.begin(), line.end(), [](auto a, auto b) { return std::string(a, b); });
      tags.emplace_back(std::move(parts));
    }

    if (!percent_str) {
      percent_str = getenv("WAKE_PERCENT");
    }

    if (!memory_str) {
      memory_str = getenv("WAKE_MEMORY");
    }

    if (!jobs_str) {
      jobs_str = getenv("WAKE_JOBS");
    }
  }

  std::optional<std::string> validate() {
    if (quiet && verbose) {
      return std::optional<std::string>{"Cannot specify both -v and -q!"};
    }

    if (profile && !debug) {
      return std::optional<std::string>{"Cannot profile without stack trace support (-d)!"};
    }

    if (shebang && chdir) {
      return std::optional<std::string>{"Cannot specify chdir and shebang simultaneously!"};
    }

    if (shebang && argc < 2) {
      return std::optional<std::string>{
          "Shebang invocation requires a script name as the first non-option argument"};
    }

    struct stat sbuf;

    if (fstat(1, &sbuf) != 0) {
      return std::optional<std::string>{
          "Wake must be run with an open standard output (file descriptor 1)"};
    }

    if (fstat(2, &sbuf) != 0) {
      return std::optional<std::string>{
          "Wake must be run with an open standard error (file descriptor 2)"};
    }

    if (fd3 && fstat(3, &sbuf) != 0) {
      return std::optional<std::string>{
          "Cannot specify --fd:3 unless file descriptor 3 is already open"};
    }

    if (fd4 && fstat(4, &sbuf) != 0) {
      return std::optional<std::string>{
          "Cannot specify --fd:4 unless file descriptor 4 is already open"};
    }

    if (fd5 && fstat(5, &sbuf) != 0) {
      return std::optional<std::string>{
          "Cannot specify --fd:5 unless file descriptor 5 is already open"};
    }

    return {};
  }
};
