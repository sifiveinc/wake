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

#ifndef STATUS_H
#define STATUS_H

#include <sys/time.h>
#include <util/term.h>

#include <iostream>
#include <optional>
#include <list>
#include <string>

struct Status {
  std::string cmdline;
  double budget;
  bool merged, wait_stdout, wait_stderr, wait_runner_out, wait_runner_err;
  struct timespec launch;
  Status(const std::string &cmdline_, double budget_, const struct timespec &launch_)
      : cmdline(cmdline_),
        budget(budget_),
        merged(false),
        wait_stdout(true),
        wait_stderr(true),
        wait_runner_out(true),
        wait_runner_err(true),
        launch(launch_) {}
};

struct StatusState {
  std::list<Status> jobs;
  // critical path stats:
  double remain;
  double total;
  double current;

  StatusState() : jobs(), remain(0), total(0), current(0) {}
};

class StatusBuf : public std::streambuf {
 private:
  std::string name;
  std::optional<std::string> extra;
  int color;
  TermInfoBuf &buf;
  std::string line_buf;
  std::string log_header;
  int32_t log_header_source_width;
  bool log_header_align;

  StatusBuf() = delete;
  StatusBuf(const StatusBuf &) = delete;
  StatusBuf(StatusBuf &&) = delete;

  void emit_header();

 public:
  explicit StatusBuf(std::string name, std::optional<std::string> extra, int color,
                     TermInfoBuf &buf);

  virtual ~StatusBuf() override;

  virtual int sync() override { return buf.pubsync(); }
  virtual int overflow(int c) override;
};

extern StatusState status_state;

#define STREAM_LOG "debug"
#define STREAM_BSP "bsp"
#define STREAM_INFO "info"
#define STREAM_INTERACTIVE "interactive"
#define STREAM_REPORT "report"
#define STREAM_ECHO "echo"
#define STREAM_WARNING "warning"
#define STREAM_ERROR "error"
#define STREAM_NULL "null"

void status_init();
void status_refresh(bool idle);
void status_finish();

void status_set_stream(const char *name, int fd);
void status_set_bulk_fd(int fd, const char *streams);

std::ostream &status_get_generic_stream(const char *name);
int status_get_colour(const char *name);
int status_get_fd(const char *name);

#endif
