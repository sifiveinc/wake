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

#include "status.h"

#include <assert.h>
#include <curses.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/time.h>
#include <term.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unordered_map>

#include "compat/sigwinch.h"
#include "job.h"
#include "runtime/config.h"
#include "util/term.h"
#include "wcl/defer.h"

// How often is the status updated (should be a multiple of 2 for budget=0)
#define REFRESH_HZ 6
// Processes which last less than this time do not get displayed
#define MIN_DRAW_TIME 0.2

#define ALMOST_ONE (1.0 - 2 * std::numeric_limits<double>::epsilon())

StatusState status_state;

static volatile bool refresh_needed = false;
static volatile bool spinner_update = false;
static volatile bool resize_detected = false;

static int used = 0;
static int ticks = 0;
static int rows = 0, cols = 0;

static void write_all(int fd, const char *data, size_t len) {
  ssize_t got;
  size_t done;
  for (done = 0; !JobTable::exit_now() && done < len; done += got) {
    got = write(fd, data + done, len - done);
    if (got < 0 && errno != EINTR) break;
  }
}

static void status_clear_state() {
  if (!term_tty()) {
    return;
  }
  std::string nrm = term_normal();
  write_all(1, nrm.c_str(), nrm.size());
}

static void status_clear() {
  if (term_tty() && used) {
    std::stringstream os;
    for (; used; --used) os << term_cuu1();
    os << term_cr();
    os << term_ed();
    std::string s = os.str();
    write_all(2, s.data(), s.size());
  }
}

// This function takes in a string with variables of the form
// $var in them and splits the string into the printed part,
// and the variables parts. The resulting vector alternates
// between strings and vars. It will always begin with a string
// and end with a string. Since '$' is treated specially, it can
// be escaped with a second '$' as in "That will be $$5.00" would
// render to "That will be $5.00".
static std::vector<std::string> split_by_var(std::string fmt) {
  std::vector<std::string> out;
  auto front = fmt.begin();
  std::string cur;

  // While looping we maintain the invariant that either
  // the `out` vector is empty, or the last item was a variable.
  while (front < fmt.end()) {
    // find the next possible split point.
    auto iter = std::find(front, fmt.end(), '$');

    // First we might be at the end, so go ahead and append that last
    // string.
    if (iter == fmt.end()) {
      // We just append to the current string and let
      // the loop invariant be maintained.
      cur.append(front, iter);
      break;
    }

    // Next we might want to escape a dollar
    if (iter + 1 < fmt.end() && *(iter + 1) == '$') {
      front += 2;
      cur += '$';
      continue;
    }

    // Lastly this might be a variable
    // By first pushing back a string and then the variable,
    // we ensure that the loop invariant is maintained.
    cur.append(front, iter);
    out.emplace_back(std::move(cur));

    // Move past the '$'
    ++iter;
    front = iter;

    // Now find the full variable name
    while (iter < fmt.end() && isalpha(*iter)) ++iter;

    // And lastly maintain the loop invariant by pushing back the variable
    out.emplace_back(front, iter);

    // move past the variable
    front = iter;
  }

  // This ensures that the last item is always a string. Note that
  // if the last thing was a variable, this might be the empty string
  // and that's ok.
  out.emplace_back(std::move(cur));
  return out;
}

void StatusBuf::emit_header() {
  auto fmt_vec = split_by_var(log_header);

  // Push the current state so that we can overwrite
  // but restore it later
  buf.push_state();

  // Now start writing things out
  std::ostream out(&buf);

  // Set color
  out << term_normal();
  int colour = color % 8;
  int intensity = color / 16;
  if (colour != TERM_DEFAULT) {
    out << term_colour(colour);
  }
  if (intensity != TERM_DEFAULT) {
    out << term_intensity(intensity);
  }

  // Now output the format the user requested.
  out << fmt_vec[0];
  const size_t extra_width = log_header_source_width;
  const bool should_align = log_header_align;
  for (size_t i = 1; i < fmt_vec.size(); i += 2) {
    const std::string &var = fmt_vec[i];

    // A var is always followed by a string, so defer that
    // until the end, that way we can break after each var
    // check
    auto write_string = wcl::make_defer([i, &out, &fmt_vec]() {
      out << std::setw(0);
      out << fmt_vec[i + 1];
    });

    if (var == "time") {
      // Get the current time
      time_t t = time(NULL);
      struct tm tm = *localtime(&t);
      out << std::setw(0);
      out << std::put_time(&tm, "%FT%T%z");
      continue;
    }

    if (var == "stream") {
      // TODO: Make this configurable
      constexpr size_t stream_width = 7;
      if (should_align) {
        out << std::setw(stream_width);
      }
      out << name;
      continue;
    }

    if (var == "source") {
      std::string tmp_extra;

      // We still cap the maximum length even if we don't align things
      if (extra) {
        if (extra->size() <= extra_width) {
          tmp_extra = *extra;
        } else if (extra_width >= 3) {
          tmp_extra.append(extra->begin(), extra->begin() + (extra_width - 3));
          tmp_extra += "...";
        } else {
          tmp_extra.append(extra->begin(), extra->begin() + extra_width);
        }
      }
      if (should_align) {
        out << std::setw(extra_width);
      }
      out << tmp_extra;
      continue;
    }
  }

  buf.pop_state();
}

int StatusBuf::overflow(int c) {
  line_buf += c;
  if (c == '\n') {
    status_clear();
    emit_header();
    buf.sputn(line_buf.data(), line_buf.size());
    status_clear_state();
    line_buf = "";
  }

  return 0;
}

StatusBuf::StatusBuf(std::string name, std::optional<std::string> extra, int color,
                     TermInfoBuf &buf)
    : name(name), extra(extra), color(color), buf(buf) {
  log_header = WakeConfig::get()->log_header;
  log_header_source_width = WakeConfig::get()->log_header_source_width;
  log_header_align = WakeConfig::get()->log_header_align;
}

StatusBuf::~StatusBuf() {
  if (line_buf.size()) {
    status_clear();
    line_buf += '\n';
    emit_header();
    buf.sputn(line_buf.data(), line_buf.size());
    status_clear_state();
  }
  sync();
}

static int ilog10(int x) {
  int out = 1;
  for (; x >= 10; x /= 10) ++out;
  return out;
}

static void status_redraw(bool idle) {
  std::stringstream os;
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);

  refresh_needed = false;
  if (resize_detected) {
    resize_detected = false;
    struct winsize size;
    if (ioctl(2, TIOCGWINSZ, &size) == 0) {
      rows = size.ws_row;
      cols = size.ws_col;
    }
  }

  int total = status_state.jobs.size();
  int rows3 = rows / 3;
  int overall = status_state.remain > 0 ? 1 : 0;
  if (term_tty() && rows3 >= 2 + overall && cols > 16)
    for (auto &x : status_state.jobs) {
      // Silence wake-internal messages like '<hash>'
      std::string msg_prefix = "'<";
      if (x.cmdline.compare(0, msg_prefix.size(), msg_prefix) == 0) continue;

      double runtime =
          (now.tv_sec - x.launch.tv_sec) + (now.tv_nsec - x.launch.tv_nsec) / 1000000000.0;

      if (x.budget < MIN_DRAW_TIME && runtime < MIN_DRAW_TIME) {
        --total;
        continue;
      }

      int rest = cols - 10;
      std::string cut;
      if ((int)x.cmdline.size() < rest) {
        cut = x.cmdline;
      } else {
        cut = x.cmdline.substr(0, (rest - 5) / 2) + " ... " +
              x.cmdline.substr(x.cmdline.size() - (rest - 4) / 2);
      }

      char progress[] = "[      ] ";
      if (x.merged) {
        if (!x.wait_stdout) {
          strcpy(progress, "[stdout] ");
        } else if (!x.wait_stderr) {
          strcpy(progress, "[stderr] ");
        } else {
          strcpy(progress, "[merged] ");
        }
      } else if (x.budget == 0) {
        long offset = lround(floor(fmod(2 * runtime, 6.0)));
        progress[offset + 1] = '#';
      } else if (runtime < x.budget) {
        for (long offset = lround(floor(7 * runtime / x.budget)); offset; --offset)
          progress[offset] = '#';
      } else {
        long over = lround(100.0 * runtime / x.budget);
        if (over > 99999) over = 99999;
        int len = ilog10(over);
        int wide = 5;
        snprintf(progress, sizeof(progress), "[%*d%%%*s] ", (wide + len) / 2, (int)over,
                 (wide - len + 1) / 2, "");
      }

      os << progress << cut << std::endl;
      ++used;
      if (used != total && used == rows3 - 1 - overall) {  // use at most 1/3 of the space
        os << "... +" << (total - used) << " more" << std::endl;
        ++used;
        break;
      }
    }

  if (term_tty() && rows3 > 0 && cols > 6 && status_state.remain > 0) {
    std::stringstream eta;
    long seconds = lround(status_state.remain);
    if (seconds > 3600) {
      eta << (seconds / 3600);
      eta << ":" << std::setfill('0') << std::setw(2);
    }
    eta << ((seconds % 3600) / 60);
    eta << ":" << std::setfill('0') << std::setw(2);
    eta << (seconds % 60);
    auto etas = eta.str();
    long width = etas.size();

    assert(status_state.total >= status_state.remain);
    assert(status_state.current >= 0);

    double progress = status_state.total - status_state.remain;
    long hashes = lround(floor((cols - 4) * progress * ALMOST_ONE / status_state.total));
    long current = lround(floor((cols - 4) * (progress + status_state.current) * ALMOST_ONE /
                                status_state.total)) -
                   hashes;
    long spaces = cols - 5 - hashes - current;
    assert(spaces >= 0);

    os << "[";
    if (spaces >= width + 3) {
      for (; hashes; --hashes) os << "#";
      for (; current; --current) os << ".";
      spaces -= width + 2;
      for (; spaces; --spaces) os << " ";
      os << etas << "  ";
    } else if (current >= width + 4) {
      current -= width + 3;
      for (; hashes; --hashes) os << "#";
      for (; current; --current) os << ".";
      os << " " << etas << " .";
      for (; spaces; --spaces) os << " ";
    } else if (hashes >= width + 4) {
      hashes -= width + 3;
      os << "# " << etas << " ";
      for (; hashes; --hashes) os << "#";
      for (; current; --current) os << ".";
      for (; spaces; --spaces) os << " ";
    } else {
      for (; hashes; --hashes) os << "#";
      for (; current; --current) os << ".";
      for (; spaces; --spaces) os << " ";
    }
    os << "]";
    if (idle) {
      os << " ." << std::endl;
    } else {
      os << " "
         << "/-\\|"[ticks] << std::endl;
      ticks = (ticks + spinner_update) & 3;
    }
    ++used;
  } else if (term_tty() && !idle) {
    os << std::string(std::max(cols - 2, 0), ' ') << "/-\\|"[ticks] << std::endl;
    ticks = (ticks + spinner_update) & 3;
    ++used;
  }
  spinner_update = false;

  std::string s = os.str();
  write_all(2, s.data(), s.size());
}

static void handle_SIGALRM(int sig) {
  (void)sig;
  refresh_needed = true;
  spinner_update = true;
}

static void handle_SIGWINCH(int sig) {
  (void)sig;
  refresh_needed = true;
  resize_detected = true;
}

void status_init() {
  if (term_tty()) {
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));

    static char lines_lit[] = "lines";
    static char cols_lit[] = "cols";
    rows = tigetnum(lines_lit);
    cols = tigetnum(cols_lit);

    // watch for resize events
    sa.sa_handler = handle_SIGWINCH;
    sa.sa_flags = SA_RESTART;  // we don't interrupt the main loop for this
    sigaction(wake_SIGWINCH, &sa, 0);
    handle_SIGWINCH(wake_SIGWINCH);

    // Setup a SIGALRM timer to trigger status redraw
    struct itimerval timer;
    timer.it_value.tv_sec = 0;
    timer.it_value.tv_usec = 1000000 / REFRESH_HZ;
    timer.it_interval = timer.it_value;

    sa.sa_handler = handle_SIGALRM;
    sa.sa_flags = SA_RESTART;  // we will truncate the pselect timeout to compensate
    sigaction(SIGALRM, &sa, 0);
    setitimer(ITIMER_REAL, &timer, 0);
  }
}

struct StreamSettings {
  int fd;
  bool istty;
  int colour;
  std::unique_ptr<std::streambuf> fd_buf;
  std::unique_ptr<TermInfoBuf> term_buf;
  std::unique_ptr<StatusBuf> log_buf;
  std::unique_ptr<std::ostream> generic_stream;
  StreamSettings() : fd(-1), istty(false), colour(TERM_DEFAULT) {}
};

static std::unordered_map<std::string, StreamSettings> settings;

int status_get_colour(const char *name) {
  if (settings.count(name)) return settings[name].colour;
  return 0;
}

int status_get_fd(const char *name) {
  if (settings.count(name)) return settings[name].fd;
  return -1;
}

static int stream_color(std::string name) {
  if (name == STREAM_ERROR) {
    return 1;
  }
  if (name == STREAM_WARNING) {
    return 3;
  }
  if (name == STREAM_ECHO) {
    return 0;
  }
  if (name == STREAM_INFO) {
    return 16;
  }
  if (name == STREAM_LOG) {
    return 4;
  }
  if (name == STREAM_BSP) {
    return 2;
  }
  if (name == STREAM_NULL) {
    return 0;
  }
  if (name == STREAM_INTERACTIVE) {
    return 6;
  }
  if (name == STREAM_REPORT) {
    return 5;
  }

  return 0;
}

std::ostream &status_get_generic_stream(const char *name) {
  if (settings.count(name)) {
    return *settings[name].generic_stream;
  }
  auto &stream = settings[name];
  stream.fd = -1;
  stream.istty = false;
  stream.colour = stream_color(name);
  stream.fd_buf = std::make_unique<NullBuf>();
  stream.generic_stream = std::make_unique<std::ostream>(stream.fd_buf.get());

  return *stream.generic_stream;
}

void status_set_fd(const char *name, int fd) {
  if (settings.count(name)) {
    return;
  }
  auto &stream = settings[name];
  stream.fd = fd;
  stream.istty = (fd == STDOUT_FILENO || fd == STDERR_FILENO) && isatty(fd) == 1;
  stream.colour = stream_color(name);
  stream.fd_buf = std::make_unique<FdBuf>(fd);
  stream.term_buf = std::make_unique<TermInfoBuf>(stream.fd_buf.get());
  stream.log_buf = std::make_unique<StatusBuf>(name, std::optional<std::string>{}, stream.colour,
                                               *stream.term_buf.get());
  stream.generic_stream = std::make_unique<std::ostream>(stream.log_buf.get());
}

void status_set_bulk_fd(int fd, const char *streams) {
  if (!streams) return;
  const char *start = streams;
  while (*start) {
    const char *end = start;
    while (*end && *end != ',') {
      ++end;
    }
    status_set_fd(std::string(start, end - start).c_str(), fd);
    start = *end ? (end + 1) : end;
  }
}

void status_refresh(bool idle) {
  if (refresh_needed) {
    status_clear();
    status_redraw(idle);
  }
}

void status_finish() {
  status_clear();
  if (term_tty()) {
    status_clear_state();
    struct itimerval timer;
    memset(&timer, 0, sizeof(timer));
    setitimer(ITIMER_REAL, &timer, 0);
  }
}
