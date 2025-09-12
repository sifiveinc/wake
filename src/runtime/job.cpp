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

#include "job.h"

#include <assert.h>
#include <fcntl.h>
#include <limits.h>
#include <math.h>
#include <re2/re2.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <iostream>
#include <limits>
#include <list>
#include <map>
#include <sstream>
#include <thread>
#include <vector>

#include "compat/mtime.h"
#include "compat/physmem.h"
#include "compat/rusage.h"
#include "compat/sigwinch.h"
#include "compat/spawn.h"
#include "config.h"
#include "database.h"
#include "prim.h"
#include "status.h"
#include "types/data.h"
#include "types/type.h"
#include "util/execpath.h"
#include "util/location.h"
#include "util/poll.h"
#include "util/shell.h"
#include "util/term.h"
#include "value.h"
#include "wcl/defer.h"

static job_cache::Cache *internal_job_cache = nullptr;

void set_job_cache(job_cache::Cache *cache) {
  if (internal_job_cache) return;
  internal_job_cache = cache;
}

// How many times to SIGTERM a process before SIGKILL
#define TERM_ATTEMPTS 6
// How long between first and second SIGTERM attempt (exponentially increasing)
#define TERM_BASE_GAP_MS 100
// The most file descriptors used by wake for itself (database/stdio/etc)
#define MAX_SELF_FDS 24
// The default memory to provision for jobs (2MB)
#define DEFAULT_PHYS_USAGE (2 * 1024 * 1024)

// #define DEBUG_PROGRESS

#define ALMOST_ONE (1.0 - 2 * std::numeric_limits<double>::epsilon())

#define STATE_FORKED 1       // in database and running
#define STATE_STDOUT 2       // stdout fully in database
#define STATE_STDERR 4       // stderr fully in database
#define STATE_RUNNER_OUT 8   // runner_out has been closed and in database
#define STATE_RUNNER_ERR 16  // runner_err has been closed and in database
#define STATE_MERGED 32      // exit status in struct
#define STATE_FINISHED 64    // inputs+outputs+status+runtime in database

// Can be queried at multiple stages of the job's lifetime
struct Job final : public GCObject<Job, Value> {
  typedef GCObject<Job, Value> Parent;

  Database *db;
  HeapPointer<String> label, cmdline, stdin_file, dir;
  int state;
  Hash code;  // hash(dir, stdin, environ, cmdline)
  pid_t pid;
  long job;
  bool keep;
  std::string echo;
  std::string stream_out;
  std::string stream_err;
  std::string runner_out;  // Runner-specific output
  std::string runner_err;  // Runner-specific errors
  HeapPointer<Value> bad_launch;
  HeapPointer<Value> bad_finish;
  double pathtime;
  struct timespec start, stop;
  Usage record;       // retrieved from DB (user-facing usage)
  Usage predict;      // prediction of Runners given record (used by scheduler)
  Usage reality;      // actual measured local usage
  Usage report;       // usage to save into DB + report in Job API
  int runner_status;  // Status code from the runner (0 = success)

  // There are 4 distinct wait queues for jobs
  HeapPointer<Continuation> q_stdout;      // waken once stdout closed
  HeapPointer<Continuation> q_stderr;      // waken once stderr closed
  HeapPointer<Continuation> q_runner_out;  // waken once runner output available
  HeapPointer<Continuation> q_runner_err;  // waken once runner error available
  HeapPointer<Continuation> q_reality;     // waken once job merged (reality available)
  HeapPointer<Continuation> q_inputs;   // waken once job finished (inputs+outputs+report available)
  HeapPointer<Continuation> q_outputs;  // waken once job finished (inputs+outputs+report available)
  HeapPointer<Continuation> q_report;   // waken once job finished (inputs+outputs+report available)

  Job(Database *db_, String *label_, String *dir_, String *stdin_file_, String *environ,
      String *cmdline_, bool keep, const char *echo, const char *stream_out, const char *stream_err,
      const char *runner_out, const char *runner_err);

  template <typename T, T (HeapPointerBase::*memberfn)(T x)>
  T recurse(T arg);

  void format(std::ostream &os, FormatState &state) const override;
  Hash shallow_hash() const override;

  uint64_t memory() const;
  double threads() const;
};

template <typename T, T (HeapPointerBase::*memberfn)(T x)>
T Job::recurse(T arg) {
  arg = Parent::recurse<T, memberfn>(arg);
  arg = (label.*memberfn)(arg);
  arg = (cmdline.*memberfn)(arg);
  arg = (stdin_file.*memberfn)(arg);
  arg = (dir.*memberfn)(arg);
  arg = (bad_launch.*memberfn)(arg);
  arg = (bad_finish.*memberfn)(arg);
  arg = (q_stdout.*memberfn)(arg);
  arg = (q_stderr.*memberfn)(arg);
  arg = (q_runner_out.*memberfn)(arg);
  arg = (q_runner_err.*memberfn)(arg);
  arg = (q_reality.*memberfn)(arg);
  arg = (q_inputs.*memberfn)(arg);
  arg = (q_outputs.*memberfn)(arg);
  arg = (q_report.*memberfn)(arg);
  return arg;
}

template <>
HeapStep Job::recurse<HeapStep, &HeapPointerBase::explore>(HeapStep step) {
  // We don't want to explore the work-queues or bad_finish/launch children
  // Instead, we front-loaded the hash calculation
  return step;
}

// Check if Job can wake up any computation
struct WJob final : public GCObject<WJob, Work> {
  typedef GCObject<WJob, Work> Parent;
  HeapPointer<Job> job;

  WJob(Job *job_) : job(job_) {}

  template <typename T, T (HeapPointerBase::*memberfn)(T x)>
  T recurse(T arg);

  void execute(Runtime &runtime) override;
};

template <typename T, T (HeapPointerBase::*memberfn)(T x)>
T WJob::recurse(T arg) {
  arg = Parent::recurse<T, memberfn>(arg);
  arg = (job.*memberfn)(arg);
  return arg;
}

void Job::format(std::ostream &os, FormatState &state) const {
  if (APP_PRECEDENCE < state.p()) os << "(";
  os << "Job " << job;
  if (APP_PRECEDENCE < state.p()) os << ")";
}

uint64_t Job::memory() const {
  if (predict.membytes == 0) {
    return DEFAULT_PHYS_USAGE;
  } else {
    return predict.membytes;
  }
}

double Job::threads() const {
  double estimate;

  if (predict.runtime == 0) {
    // We have no prior execution statistics
    // However, a Runner can still predict thread usage in this case using cputime.
    estimate = predict.cputime;
  } else {
    // Estimate the threads required
    estimate = predict.cputime / predict.runtime;
  }

  if (estimate > 1.0) {
    // This is a multi-threaded Job. It needs more than one slot.
    // Multiply by 1.3 to prevent runaway effect; see below
    return estimate * 1.3;
  }

  // This is probably a single-threaded job.

  // If the job is bottlenecked by IO or something else, scheduling more jobs might help.
  // However, suppose we previously 2N 100% CPU jobs on N cores. These jobs would also
  // have an estimate of 0.5. If we made our decision based on that, we'd schedule 2N again.
  // Worse, if there is even any additional overhead (there probably is), we will get an
  // even lower estimate next time and schedule more and more and more jobs each run.

  // To combat this effect, we conservatively double the CPU utilization of jobs.
  // However, it's probably still single-threaded, so cap this pessimism there.
  estimate *= 2.0;
  if (estimate > 1.0) estimate = 1.0;

  // Finally, the cputime/realtime will be VERY low if the job was executed remotely.
  // However, even in that case we don't want to fork-bomb the local machine, so impose
  // an absolute lower limit of 0.01 (ie: 100*max_jobs).
  if (estimate < 0.01) estimate = 0.01;

  return estimate;
}

// A Task is a job that is not yet forked
struct Task {
  RootPointer<Job> job;
  std::string dir;
  std::string stdin_file;
  std::string environ;
  std::string cmdline;
  bool is_atty;
  Task(RootPointer<Job> &&job_, const std::string &dir_, const std::string &stdin_file_,
       const std::string &environ_, const std::string &cmdline_, bool is_atty_)
      : job(std::move(job_)),
        dir(dir_),
        stdin_file(stdin_file_),
        environ(environ_),
        cmdline(cmdline_),
        is_atty(is_atty_) {}
};

static bool operator<(const std::unique_ptr<Task> &x, const std::unique_ptr<Task> &y) {
  // anything with dependants on stderr/stdout is infinity (ie: run first)
  if (x->job->q_stdout || x->job->q_stderr) return false;
  if (y->job->q_stdout || y->job->q_stderr) return true;
  // 0 (unknown runtime) is infinity for this comparison (ie: run first)
  if (x->job->predict.runtime == 0 && y->job->predict.runtime != 0) return false;
  if (y->job->predict.runtime == 0 && x->job->predict.runtime != 0) return true;
  if (x->job->pathtime < y->job->pathtime) return true;
  if (x->job->pathtime > y->job->pathtime) return false;
  return x->job->job < y->job->job;
}

// A JobEntry is a forked job with pid|stdout|stderr incomplete
struct JobEntry {
  JobTable::detail *imp;
  RootPointer<Job> job;  // if unset, available for reuse
  pid_t pid;             //  0 if merged
  int pipe_stdout;       // -1 if closed
  int pipe_stderr;       // -1 if closed
  int pipe_runner_out;   // -1 if closed (fd 3)
  int pipe_runner_err;   // -1 if closed (fd 4)
  std::string echo_line;
  std::list<Status>::iterator status;
  std::unique_ptr<std::streambuf> stdout_linebuf;
  std::unique_ptr<std::streambuf> stderr_linebuf;
  std::unique_ptr<std::streambuf> runner_out_linebuf;
  std::unique_ptr<std::streambuf> runner_err_linebuf;

  JobEntry(JobTable::detail *imp_, RootPointer<Job> &&job_, std::unique_ptr<std::streambuf> stdout,
           std::unique_ptr<std::streambuf> stderr, std::unique_ptr<std::streambuf> runner_out,
           std::unique_ptr<std::streambuf> runner_err)
      : imp(imp_),
        job(std::move(job_)),
        pid(0),
        pipe_stdout(-1),
        pipe_stderr(-1),
        pipe_runner_out(-1),
        pipe_runner_err(-1),
        stdout_linebuf(std::move(stdout)),
        stderr_linebuf(std::move(stderr)),
        runner_out_linebuf(std::move(runner_out)),
        runner_err_linebuf(std::move(runner_err)) {}
  ~JobEntry();

  double runtime(struct timespec now);
};

double JobEntry::runtime(struct timespec now) {
  return now.tv_sec - job->start.tv_sec + (now.tv_nsec - job->start.tv_nsec) / 1000000000.0;
}

struct CriticalJob {
  double pathtime;
  double runtime;
};

// Implementation details for a JobTable
struct JobTable::detail {
  Poll poll;
  long num_running;
  std::map<pid_t, std::shared_ptr<JobEntry>> pidmap;
  std::map<int, std::shared_ptr<JobEntry>> pipes;
  std::vector<std::unique_ptr<Task>> pending;
  sigset_t block;  // signals that can race with poll.wait()
  Database *db;
  double active, limit;              // CPUs
  uint64_t phys_active, phys_limit;  // memory
  long max_children;                 // hard cap on jobs allowed
  bool debug;
  bool verbose;
  bool quiet;
  bool check;
  bool batch;
  struct timespec wall;
  RUsage childrenUsage;

  std::unordered_map<int, std::unique_ptr<std::streambuf>> fd_bufs;
  std::unordered_map<int, std::unique_ptr<TermInfoBuf>> term_bufs;

  detail() {}

  ~detail() {
    // We need a very specific release order on these so we do it manually
    for (auto &entry : pidmap) {
      entry.second.reset();
    }
    for (auto &entry : pipes) {
      entry.second.reset();
    }
    for (auto &entry : fd_bufs) {
      entry.second.release();
    }
    for (auto &entry : term_bufs) {
      entry.second.release();
    }
  }

  CriticalJob critJob(double nexttime) const;
};

CriticalJob JobTable::detail::critJob(double nexttime) const {
  CriticalJob out;
  out.pathtime = nexttime;
  out.runtime = 0;
  for (auto &pm : pidmap) {
    Job *job = pm.second->job.get();
    if (job->pathtime > out.pathtime) {
      out.pathtime = job->pathtime;
      out.runtime = job->record.runtime;
    }
  }
  for (auto &j : pending) {
    if (j->job->pathtime > out.pathtime) {
      out.pathtime = j->job->pathtime;
      out.runtime = j->job->record.runtime;
    }
  }
  return out;
}

static bool nice_end(const char *s) {
  if (s[0] == 0) return true;
  if (s[0] == 'B' && s[1] == 0) return true;
  if (s[0] == 'i' && s[1] == 'B' && s[2] == 0) return true;
  return false;
}

const char *ResourceBudget::parse(const char *str, ResourceBudget &output) {
  char *dtail;
  double percentage = strtod(str, &dtail);

  if (dtail[0] == '%' && dtail[1] == 0) {
    if (percentage < 1) {
      return "percentage must be >= 1%";
    } else {
      output.percentage = percentage / 100.0;
      output.fixed = 0;
      return nullptr;
    }
  }

  char *ltail;
  long long val = strtoll(str, &ltail, 0);
  bool overflow = val == LLONG_MAX && errno == ERANGE;
  uint64_t limit = UINT64_MAX / 1024;

  if (val <= 0) {
    return "value must be > 0";
  }

  output.percentage = 0;
  output.fixed = val;

  const char *toobig = "value exceeds 64-bits";
  if (nice_end(ltail)) return overflow ? toobig : nullptr;

  overflow |= output.fixed > limit;
  output.fixed *= 1024;
  if (ltail[0] == 'k' && nice_end(ltail + 1)) return overflow ? toobig : nullptr;

  overflow |= output.fixed > limit;
  output.fixed *= 1024;
  if (ltail[0] == 'M' && nice_end(ltail + 1)) return overflow ? toobig : nullptr;

  overflow |= output.fixed > limit;
  output.fixed *= 1024;
  if (ltail[0] == 'G' && nice_end(ltail + 1)) return overflow ? toobig : nullptr;

  overflow |= output.fixed > limit;
  output.fixed *= 1024;
  if (ltail[0] == 'T' && nice_end(ltail + 1)) return overflow ? toobig : nullptr;

  overflow |= output.fixed > limit;
  output.fixed *= 1024;
  if (ltail[0] == 'P' && nice_end(ltail + 1)) return overflow ? toobig : nullptr;

  overflow |= output.fixed > limit;
  output.fixed *= 1024;
  if (ltail[0] == 'E' && nice_end(ltail + 1)) return overflow ? toobig : nullptr;

  // Error reporting
  if (ltail == dtail) {
    return "integer value must be followed by nothing or one of [kMGTPE]";
  } else {
    return "percentage value must be followed by a '%'";
  }
}

std::string ResourceBudget::format(uint64_t x) {
  int suffix = 0;
  int up = 0;
  static const char *SI[] = {"B", "kiB", "MiB", "GiB", "TiB", "PiB", "EiB"};
  while (x >= static_cast<uint64_t>(10000 - up)) {
    ++suffix;
    up = (x % 1024) >= 512;
    x /= 1024;
  }
  x += up;
  return std::to_string(x) + SI[suffix];
}

static volatile bool child_ready = false;
static volatile bool exit_asap = false;

static void handle_SIGCHLD(int sig) {
  (void)sig;
  child_ready = true;
}

static void handle_exit(int sig) {
  (void)sig;
  exit_asap = true;
}

bool JobTable::exit_now() { return exit_asap; }

static int get_concurrency() {
#ifdef __linux
  int cpus = sysconf(_SC_NPROCESSORS_CONF);

  cpu_set_t *cpuset = CPU_ALLOC(cpus);
  size_t size = CPU_ALLOC_SIZE(cpus);
  int ret = sched_getaffinity(0, size, cpuset);
  int avail = CPU_COUNT_S(size, cpuset);
  CPU_FREE(cpuset);

  if (ret == 0 && avail > 0 && avail <= cpus) {
    return avail;
  } else {
    return cpus;
  }
#else
  return std::thread::hardware_concurrency();
#endif
}

JobTable::JobTable(Database *db, ResourceBudget memory, ResourceBudget cpu, bool debug,
                   bool verbose, bool quiet, bool check, bool batch)
    : imp(new JobTable::detail) {
  imp->num_running = 0;
  imp->debug = debug;
  imp->verbose = verbose;
  imp->quiet = quiet;
  imp->check = check;
  imp->batch = batch;
  imp->db = db;
  imp->active = 0;
  imp->limit = cpu.get(get_concurrency());
  imp->phys_active = 0;
  imp->phys_limit = memory.get(get_physical_memory());
  memset(&imp->childrenUsage, 0, sizeof(struct RUsage));

  // Double-check that ::parse() did not do something crazy.
  assert(imp->limit > 0);

  status_get_generic_stream("echo")
      << "wake: targeting utilization for " << imp->limit << " threads and "
      << ResourceBudget::format(imp->phys_limit) << " of memory." << std::endl;

  // Wake creates files + dirs with explicit permissions.
  // We do not want the umask to interfere.
  // However, we must be careful to restore this for children.
  umask(0);

  sigemptyset(&imp->block);

  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));

  // Ignore these signals
  sa.sa_handler = SIG_IGN;
  sa.sa_flags = SA_RESTART;
  sigaction(SIGPIPE, &sa, 0);
  sigaction(SIGUSR1, &sa, 0);
  sigaction(SIGUSR2, &sa, 0);

  // SIGCHLD interrupts poll.wait()
  sa.sa_handler = handle_SIGCHLD;
  sa.sa_flags = SA_NOCLDSTOP;
  // no SA_RESTART, because we need to interrupt poll.wait() portably
  // to protect other syscalls, we keep SIGCHLD blocked except in poll.wait()
  sigaddset(&imp->block, SIGCHLD);
  sigprocmask(SIG_BLOCK, &imp->block, 0);
  sigaction(SIGCHLD, &sa, 0);

  // These signals cause wake to exit cleanly
  sa.sa_handler = handle_exit;
  sa.sa_flags = 0;  // no SA_RESTART, because we want to terminate blocking calls
  sigaction(SIGHUP, &sa, 0);
  sigaction(SIGINT, &sa, 0);
  sigaction(SIGQUIT, &sa, 0);
  sigaction(SIGTERM, &sa, 0);
  sigaction(SIGXCPU, &sa, 0);
  sigaction(SIGXFSZ, &sa, 0);

  // Add to the set of signals we need to interrupt poll.wait()
  sigaddset(&imp->block, SIGHUP);
  sigaddset(&imp->block, SIGINT);
  sigaddset(&imp->block, SIGQUIT);
  sigaddset(&imp->block, SIGTERM);
  sigaddset(&imp->block, SIGXCPU);
  sigaddset(&imp->block, SIGXFSZ);

  // These are handled in status.cpp
  sigaddset(&imp->block, SIGALRM);
  sigaddset(&imp->block, wake_SIGWINCH);

  // Calculate the maximum number of children we will run

  // We need enough cores (Job::threads has minimum 1% usage)
  imp->max_children = imp->limit * 100;

  // We need enough process identifiers
  long sys_child_max = sysconf(_SC_CHILD_MAX);
  if (sys_child_max != -1) {
    if (imp->max_children > sys_child_max / 2) imp->max_children = sys_child_max / 2;
  } else {
#ifdef CHILD_MAX
    if (imp->max_children > CHILD_MAX / 2) imp->max_children = CHILD_MAX / 2;
#endif
  }

  // We need enough file descriptors for pipes
  int maxfd = imp->poll.max_fds();
  if (imp->max_children > (maxfd - MAX_SELF_FDS) / 2) {
    if (maxfd < 1024) {
      std::cerr << "wake wanted a limit of " << imp->max_children << " children, but only got "
                << (maxfd - MAX_SELF_FDS) / 2 << ", because only " << maxfd
                << " file descriptors are available." << std::endl;
    }
    imp->max_children = (maxfd - MAX_SELF_FDS) / 2;
  }

  // We need at least one child to make forward progress
  if (imp->max_children < 1) imp->max_children = 1;

  // std::cerr << "max children " << imp->max_children << "/" << sys_child_max << std::endl;
}

static struct timespec mytimersub(struct timespec a, struct timespec b) {
  struct timespec out;
  out.tv_sec = a.tv_sec - b.tv_sec;
  out.tv_nsec = a.tv_nsec - b.tv_nsec;
  if (out.tv_nsec < 0) {
    --out.tv_sec;
    out.tv_nsec += 1000000000;
  }
  return out;
}

static struct timespec mytimerdouble(struct timespec a) {
  a.tv_sec <<= 1;
  a.tv_nsec <<= 1;
  if (a.tv_nsec > 1000000000) {
    ++a.tv_sec;
    a.tv_nsec -= 1000000000;
  }
  return a;
}

JobTable::~JobTable() {
  // Disable the status refresh signal
  struct itimerval timer;
  memset(&timer, 0, sizeof(timer));
  setitimer(ITIMER_REAL, &timer, 0);

  // We don't care about file descriptors any more
  imp->poll.clear();

  // SIGTERM strategy is to double the gap between termination attempts every retry
  struct timespec limit;
  limit.tv_sec = TERM_BASE_GAP_MS / 1000;
  limit.tv_nsec = (TERM_BASE_GAP_MS % 1000) * 1000000;

  // Try to kill children gently first, once every second
  for (int retry = 0; !imp->pidmap.empty() && retry < TERM_ATTEMPTS;
       ++retry, limit = mytimerdouble(limit)) {
    // Send every child SIGTERM
    for (auto &i : imp->pidmap) kill(i.first, SIGTERM);

    // Reap children for one second; exit early if none remain
    struct timespec start, now, remain, timeout;
    clock_gettime(CLOCK_REALTIME, &start);
    for (now = start;
         !imp->pidmap.empty() && (remain = mytimersub(limit, mytimersub(now, start))).tv_sec >= 0;
         clock_gettime(CLOCK_REALTIME, &now)) {
      // Block racey signals between here and poll.wait()
      sigset_t saved;
      sigprocmask(SIG_BLOCK, &imp->block, &saved);
      sigdelset(&saved, SIGCHLD);

      // Continue waiting for the full second
      timeout.tv_sec = 0;
      timeout.tv_nsec = remain.tv_nsec;

      // Sleep until timeout or a signal arrives
      if (!child_ready) imp->poll.wait(&timeout, &saved);

      // Restore signals
      child_ready = false;
      sigaddset(&saved, SIGCHLD);
      sigprocmask(SIG_SETMASK, &saved, 0);

      pid_t pid;
      int status;
      while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (WIFSTOPPED(status)) continue;
        imp->pidmap.erase(pid);
      }
    }
  }

  // Force children to die
  for (auto &i : imp->pidmap) {
    std::stringstream s;
    s << "Force killing " << i.first << " after " << TERM_ATTEMPTS << " attempts with SIGTERM"
      << std::endl;
    // TODO: Note that this is from wake
    status_get_generic_stream(STREAM_ERROR) << s.str() << std::endl;
    kill(i.first, SIGKILL);
  }
}

static char **split_null(std::string &str) {
  int nulls = 0;
  for (char c : str) nulls += (c == 0);
  char **out = new char *[nulls + 1];

  nulls = 0;
  out[0] = const_cast<char *>(str.c_str());
  char *end = out[0] + str.size();
  for (char *scan = out[0]; scan != end; ++scan) {
    if (*scan == 0) {
      ++nulls;
      out[nulls] = scan + 1;
    }
  }
  out[nulls] = 0;

  return out;
}

static std::string pretty_cmd(const std::string &x) {
  std::stringstream out;

  size_t e;
  for (size_t s = 0; s != x.size(); s = e + 1) {
    e = x.find('\0', s);
    if (s) out << ' ';
    out << shell_escape(x.c_str() + s);
  }

  return out.str();
}

static void create_pipe(int io[2]) {
  if (pipe(io) == -1) {
    std::cerr << "failed to create pipe" << std::endl;
    exit(1);
  }
}

#ifdef __APPLE__

// MacOS doesn't have ptsname_r and thus needs to fall back to
// regular pipes
static void create_psuedoterminal(int io[2]) { create_pipe(io); }

#else

// Create a psuedoterminal by setting
// io[0] to the parent fd, and io[1] to
// the child fd. Prints an error and fails
// if an error occurs for some reason.
// Both the parent and the bidirectional but
// this function is intended to repalce calling
// `pipe` so you might instead only read from
// the parent and write to the child.
static void create_psuedoterminal(int io[2]) {
  // Create a psuedo-terminal
  int parent_term_fd = open("/dev/ptmx", O_RDWR);
  if (parent_term_fd == -1) {
    std::cerr << "open failed to open new psuedo-terminal" << std::endl;
    exit(1);
  }
  if (grantpt(parent_term_fd) != 0) {
    std::cerr << "grantpt failed" << std::endl;
    exit(1);
  }
  if (unlockpt(parent_term_fd)) {
    std::cerr << "unlockpt failed" << std::endl;
    exit(1);
  }
  char child_term_name[80];
  if (ptsname_r(parent_term_fd, child_term_name, sizeof(child_term_name)) != 0) {
    std::cerr << "ptsname_r failed" << std::endl;
    exit(1);
  }
  int child_term_fd = open(child_term_name, O_RDWR);
  if (child_term_fd == -1) {
    std::cerr << "open failed to open child end of psuedo-terminal" << std::endl;
    exit(1);
  }
  io[0] = parent_term_fd;
  io[1] = child_term_fd;
}

#endif

// Helper function to set up a file descriptor buffer
// Make the raw output streams and the TermInfoBufs
// that jobs will use. We make one TermInfoBuf per
// file descriptor that we're outputting to.
// TODO: We could add file tee-ing here as well
static void setup_fd_buf(JobTable *jobtable, int fd) {
  if (!jobtable->imp->fd_bufs.count(fd)) {
    std::unique_ptr<std::streambuf> fd_buf;
    if (fd != -1) {
      fd_buf = std::make_unique<FdBuf>(fd);
    } else {
      fd_buf = std::make_unique<NullBuf>();
    }
    jobtable->imp->term_bufs[fd] = std::make_unique<TermInfoBuf>(fd_buf.get(), !isatty(fd));
    jobtable->imp->fd_bufs[fd] = std::move(fd_buf);
  }
}

// Helper function to create a stream buffer
static std::unique_ptr<std::streambuf> create_stream_buf(
    JobTable *jobtable, int fd, const char *stream_name,
    const wcl::optional<std::string> &job_label_str, int color) {
  if (fd != -1 && RE2::FullMatch(*job_label_str, *WakeConfig::get()->label_filter)) {
    return std::make_unique<StatusBuf>(stream_name, job_label_str, color,
                                       *jobtable->imp->term_bufs[fd].get());
  } else {
    return std::make_unique<NullBuf>();
  }
}

// Helper function to set up pipe/terminal and configure file descriptors
static void setup_stream(int stream[2], bool is_atty, int &pipe_fd, JobTable *jobtable,
                         std::shared_ptr<JobEntry> entry) {
  if (is_atty) {
    create_psuedoterminal(stream);
  } else {
    create_pipe(stream);
  }

  int flags;
  if ((flags = fcntl(stream[0], F_GETFD, 0)) != -1) fcntl(stream[0], F_SETFD, flags | FD_CLOEXEC);

  jobtable->imp->poll.add(pipe_fd = stream[0]);
  jobtable->imp->pipes[stream[0]] = entry;
}

static void launch(JobTable *jobtable) {
  // Note: We schedule jobs whenever we are under CPU quota, without considering if the
  // new job will cause us to exceed the quota. This is necessary, for two reasons:
  //   1 - a job could require more compute than allowed; we require forward progress
  //   2 - if the next optimal job to schedule needs more compute than available
  //     a - it would waste idle compute if we don't schedule something
  //     b - it would hurt the build critical path if we schedule a sub-optimal job
  // => just oversubscribe compute and let the kernel sort it out
  // For memory, we follow a more conservative policy. We don't start a job that would
  // oversubscribe RAM, unless there are no other jobs running yet. The rational:
  //   - exceeding memory would slow down the build due to thrashing
  //   - RAM is never "wasted" (disk cache / etc), so just wait for the next critical job
  //   - even if a job uses more memory than the system has, eventually attempt it anyway (progress)
  auto &heap = jobtable->imp->pending;
  while (!heap.empty() && jobtable->imp->num_running < jobtable->imp->max_children &&
         jobtable->imp->active < jobtable->imp->limit &&
         (jobtable->imp->phys_active == 0 ||
          jobtable->imp->phys_active + heap.front()->job->memory() < jobtable->imp->phys_limit)) {
    Task &task = *heap.front();
    jobtable->imp->active += task.job->threads();
    jobtable->imp->phys_active += task.job->memory();

    // Get file descriptors and colors for all streams
    int fd_out = status_get_fd(task.job->stream_out.c_str());
    int color_out = status_get_colour(task.job->stream_out.c_str());
    int fd_err = status_get_fd(task.job->stream_err.c_str());
    int color_err = status_get_colour(task.job->stream_err.c_str());
    int fd_runner_out = status_get_fd(task.job->runner_out.c_str());
    int color_runner_out = status_get_colour(task.job->runner_out.c_str());
    int fd_runner_err = status_get_fd(task.job->runner_err.c_str());
    int color_runner_err = status_get_colour(task.job->runner_err.c_str());

    // Set up file descriptor buffers
    setup_fd_buf(jobtable, fd_out);
    setup_fd_buf(jobtable, fd_err);
    setup_fd_buf(jobtable, fd_runner_out);
    setup_fd_buf(jobtable, fd_runner_err);

    // Create stream buffers

    // Now we need to figure out if we're outputting to a null buffer to avoid rendering
    // or if we're going to output to a nicely rendered StatusBuf.
    auto job_label_str = wcl::some(task.job->label->as_str());
    std::unique_ptr<std::streambuf> out =
        create_stream_buf(jobtable, fd_out, task.job->stream_out.c_str(), job_label_str, color_out);
    std::unique_ptr<std::streambuf> err =
        create_stream_buf(jobtable, fd_err, task.job->stream_err.c_str(), job_label_str, color_err);
    std::unique_ptr<std::streambuf> runner_out = create_stream_buf(
        jobtable, fd_runner_out, task.job->runner_out.c_str(), job_label_str, color_runner_out);
    std::unique_ptr<std::streambuf> runner_err = create_stream_buf(
        jobtable, fd_runner_err, task.job->runner_err.c_str(), job_label_str, color_runner_err);

    // Create job entry
    std::shared_ptr<JobEntry> entry =
        std::make_shared<JobEntry>(jobtable->imp.get(), std::move(task.job), std::move(out),
                                   std::move(err), std::move(runner_out), std::move(runner_err));

    // Set up all streams
    int stdout_stream[2], stderr_stream[2], runner_out_stream[2], runner_err_stream[2];
    setup_stream(stdout_stream, task.is_atty, entry->pipe_stdout, jobtable, entry);
    setup_stream(stderr_stream, task.is_atty, entry->pipe_stderr, jobtable, entry);
    setup_stream(runner_out_stream, task.is_atty, entry->pipe_runner_out, jobtable, entry);
    setup_stream(runner_err_stream, task.is_atty, entry->pipe_runner_err, jobtable, entry);

    clock_gettime(CLOCK_REALTIME, &entry->job->start);
    std::stringstream prelude;
    prelude << find_execpath() << "/../lib/wake/shim-wake" << '\0'
            << (task.stdin_file.empty() ? "/dev/null" : task.stdin_file.c_str()) << '\0'
            << std::to_string(stdout_stream[1]) << '\0' << std::to_string(stderr_stream[1]) << '\0'
            << std::to_string(runner_out_stream[1]) << '\0' << std::to_string(runner_err_stream[1])
            << '\0' << task.dir << '\0';
    std::string shim = prelude.str() + task.cmdline;
    auto cmdline = split_null(shim);
    auto environ = split_null(task.environ);

    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGCHLD);
    sigprocmask(SIG_UNBLOCK, &set, 0);
    pid_t pid = wake_spawn(cmdline[0], cmdline, environ);
    sigprocmask(SIG_BLOCK, &set, 0);

    delete[] cmdline;
    delete[] environ;
    ++jobtable->imp->num_running;
    jobtable->imp->pidmap[pid] = entry;
    entry->job->pid = entry->pid = pid;
    entry->job->state |= STATE_FORKED;
    close(stdout_stream[1]);
    close(stderr_stream[1]);
    close(runner_out_stream[1]);
    close(runner_err_stream[1]);
    bool indirect = *entry->job->cmdline != task.cmdline;
    double predict = entry->job->predict.status == 0 ? entry->job->predict.runtime : 0;
    std::string pretty = pretty_cmd(entry->job->cmdline->as_str());
    std::string clone(entry->job->label->empty() ? pretty : entry->job->label->as_str());
    for (auto &c : clone)
      if (c == '\n') c = ' ';
    entry->status =
        status_state.jobs.emplace(status_state.jobs.end(), clone, predict, entry->job->start);
    std::stringstream s;
    if (*entry->job->dir != ".") s << "cd " << entry->job->dir->c_str() << "; ";
    s << pretty;
    if (!entry->job->stdin_file->empty())
      s << " < " << shell_escape(entry->job->stdin_file->c_str());
    if (indirect && jobtable->imp->debug) {
      s << " # launched by: ";
      if (task.dir != ".") s << "cd " << task.dir << "; ";
      s << pretty_cmd(task.cmdline);
      if (!task.stdin_file.empty()) s << " < " << shell_escape(task.stdin_file);
    }
    s << std::endl;
    std::string echo_line = s.str();
    if (jobtable->imp->batch) {
      entry->echo_line = std::move(echo_line);
    } else {
      status_get_generic_stream(entry->job->echo.c_str()) << echo_line.data() << std::endl;
    }

    std::pop_heap(heap.begin(), heap.end());
    heap.resize(heap.size() - 1);
  }
}

JobEntry::~JobEntry() {
  status_state.jobs.erase(status);
  --imp->num_running;
  imp->active -= job->threads();
  imp->phys_active -= job->memory();
  if (imp->batch) {
    if (!echo_line.empty())
      status_get_generic_stream(job->echo.c_str()) << echo_line.c_str() << std::endl;
    imp->db->replay_output(job->job, job->stream_out.c_str(), job->stream_err.c_str(),
                           job->runner_out.c_str(), job->runner_err.c_str());
  }
}

// Helper function to handle pipe data
static bool handle_pipe_data(JobTable::detail &imp, int fd, std::shared_ptr<JobEntry> entry,
                             int &pipe_fd, bool &wait_flag, int state_flag, int stream_id,
                             char *buffer, size_t buffer_size, const struct timespec &now,
                             Runtime &runtime) {
  int got = read(fd, buffer, buffer_size);
  if (got == 0 || (got < 0 && errno != EINTR)) {
    imp.pipes.erase(imp.pipes.find(fd));
    imp.poll.remove(fd);
    close(fd);
    pipe_fd = -1;
    wait_flag = false;
    entry->job->state |= state_flag;
    runtime.heap.guarantee(WJob::reserve());
    runtime.schedule(WJob::claim(runtime.heap, entry->job.get()));
    return true;
  } else {
    entry->job->db->save_output(entry->job->job, stream_id, buffer, got, entry->runtime(now));
    if (!imp.batch) {
      switch (stream_id) {
        case 1:  // stdout
          entry->stdout_linebuf->sputn(buffer, got);
          break;
        case 2:  // stderr
          entry->stderr_linebuf->sputn(buffer, got);
          break;
        case 3:  // runner_out
          entry->runner_out_linebuf->sputn(buffer, got);
          break;
        case 4:  // runner_err
          entry->runner_err_linebuf->sputn(buffer, got);
          break;
      }
    }
    return false;
  }
}

bool JobTable::wait(Runtime &runtime) {
  char buffer[4096];
  struct timespec nowait;
  memset(&nowait, 0, sizeof(nowait));

  launch(this);

  bool compute = false;
  while (!exit_now() && imp->num_running) {
    // Block all signals we expect to interrupt pselect
    sigset_t saved;
    sigprocmask(SIG_BLOCK, &imp->block, &saved);
    sigdelset(&saved, SIGCHLD);

    // Check for all signals that are now blocked
    struct timespec *timeout = 0;
    if (child_ready) timeout = &nowait;
    if (exit_now()) timeout = &nowait;

#if !defined(__linux__)
    struct timespec alarm;
    // In case SIGALRM with SA_RESTART doesn't stop pselect
    if (!timeout) {
      struct itimerval timer;
      getitimer(ITIMER_REAL, &timer);
      if (timer.it_value.tv_sec || timer.it_value.tv_usec) {
        alarm.tv_sec = timer.it_value.tv_sec;
        alarm.tv_nsec = (10000 + timer.it_value.tv_usec) * 1000;
        timeout = &alarm;
      }
    }
#endif
    status_refresh(true);

    // Wait for a status change, with signals atomically unblocked in pselect
    std::vector<int> ready_fds = imp->poll.wait(timeout, &saved);

    // Restore signal mask
    sigaddset(&saved, SIGCHLD);
    sigprocmask(SIG_SETMASK, &saved, 0);

    struct timespec now;
    clock_gettime(CLOCK_REALTIME, &now);

    int done = 0;

    for (auto fd : ready_fds) {
      auto it = imp->pipes.find(fd);
      assert(it != imp->pipes.end());  // ready_fds <= poll_fds == pipes.keys()
      std::shared_ptr<JobEntry> entry = it->second;
      assert(entry);

      if (entry->pipe_stdout == fd) {
        if (handle_pipe_data(*imp, fd, entry, entry->pipe_stdout, entry->status->wait_stdout,
                             STATE_STDOUT, 1, buffer, sizeof(buffer), now, runtime)) {
          ++done;
        }
      }
      if (entry->pipe_stderr == fd) {
        if (handle_pipe_data(*imp, fd, entry, entry->pipe_stderr, entry->status->wait_stderr,
                             STATE_STDERR, 2, buffer, sizeof(buffer), now, runtime)) {
          ++done;
        }
      }
      if (entry->pipe_runner_out == fd) {
        if (handle_pipe_data(*imp, fd, entry, entry->pipe_runner_out,
                             entry->status->wait_runner_out, STATE_RUNNER_OUT, 3, buffer,
                             sizeof(buffer), now, runtime)) {
          ++done;
        }
      }
      if (entry->pipe_runner_err == fd) {
        if (handle_pipe_data(*imp, fd, entry, entry->pipe_runner_err,
                             entry->status->wait_runner_err, STATE_RUNNER_ERR, 4, buffer,
                             sizeof(buffer), now, runtime)) {
          ++done;
        }
      }
    }

    int status;
    pid_t pid;
    child_ready = false;
    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
      if (WIFSTOPPED(status)) continue;

      ++done;
      int code = 0;
      if (WIFEXITED(status)) {
        code = WEXITSTATUS(status);
      } else if (WIFSIGNALED(status)) {
        code = -WTERMSIG(status);
      }

      RUsage totalUsage = getRUsageChildren();
      RUsage childUsage = rusage_sub(totalUsage, imp->childrenUsage);
      imp->childrenUsage = totalUsage;

      // It is possible that this is not our child
      auto it = imp->pidmap.find(pid);
      if (it == imp->pidmap.end()) continue;

      std::shared_ptr<JobEntry> entry = it->second;
      imp->pidmap.erase(it);
      assert(entry);

      entry->pid = 0;
      entry->status->merged = true;
      entry->job->state |= STATE_MERGED;
      entry->job->stop = now;
      entry->job->reality.found = true;
      entry->job->reality.status = code;
      entry->job->reality.runtime = entry->runtime(now);
      entry->job->reality.cputime = childUsage.utime + childUsage.stime;
      entry->job->reality.membytes = childUsage.membytes;
      entry->job->reality.ibytes = childUsage.ibytes;
      entry->job->reality.obytes = childUsage.obytes;
      runtime.heap.guarantee(WJob::reserve());
      runtime.schedule(WJob::claim(runtime.heap, entry->job.get()));

      // Force close any remaining runner pipes when process exits.
      // There are situations where the polling mechanism doesn't detect the EOF condition
      // on unused pipes (child processes that inherit the fd but never write to it).
      // This ensures that we always clean up after the child process.
      if (entry->pipe_runner_out != -1) {
        imp->pipes.erase(entry->pipe_runner_out);
        imp->poll.remove(entry->pipe_runner_out);
        close(entry->pipe_runner_out);
        entry->pipe_runner_out = -1;
        entry->job->state |= STATE_RUNNER_OUT;
        runtime.heap.guarantee(WJob::reserve());
        runtime.schedule(WJob::claim(runtime.heap, entry->job.get()));
      }
      if (entry->pipe_runner_err != -1) {
        imp->pipes.erase(entry->pipe_runner_err);
        imp->poll.remove(entry->pipe_runner_err);
        close(entry->pipe_runner_err);
        entry->pipe_runner_err = -1;
        entry->job->state |= STATE_RUNNER_ERR;
        runtime.heap.guarantee(WJob::reserve());
        runtime.schedule(WJob::claim(runtime.heap, entry->job.get()));
      }

      // If this was the job on the critical path, adjust remain
      if (entry->job->pathtime == status_state.remain) {
        auto crit = imp->critJob(ALMOST_ONE * (entry->job->pathtime - entry->job->record.runtime));
#ifdef DEBUG_PROGRESS
        std::cerr << "RUN DONE CRIT: " << status_state.remain << " => " << crit.pathtime << "  /  "
                  << status_state.total << std::endl;
#endif
        status_state.remain = crit.pathtime;
        status_state.current = crit.runtime;
        if (crit.runtime == 0) imp->wall = now;
      }
    }

    // In case the expected next critical job is never scheduled, fall back to the next
    double dwall =
        (now.tv_sec - imp->wall.tv_sec) + (now.tv_nsec - imp->wall.tv_nsec) / 1000000000.0;
    if (status_state.current == 0 && dwall * 5 > status_state.remain) {
      auto crit = imp->critJob(0);
      if (crit.runtime != 0) {
        status_state.total = crit.pathtime + (status_state.total - status_state.remain);
        status_state.remain = crit.pathtime;
        status_state.current = crit.runtime;
      }
    }

    if (done > 0) {
      compute = true;
      break;
    }
  }

  return compute;
}

Job::Job(Database *db_, String *label_, String *dir_, String *stdin_file_, String *environ,
         String *cmdline_, bool keep_, const char *echo_, const char *stream_out_,
         const char *stream_err_, const char *runner_out_, const char *runner_err_)
    : db(db_),
      label(label_),
      cmdline(cmdline_),
      stdin_file(stdin_file_),
      dir(dir_),
      state(0),
      code(),
      pid(0),
      job(-1),
      keep(keep_),
      echo(echo_),
      stream_out(stream_out_),
      stream_err(stream_err_),
      runner_out(runner_out_),
      runner_err(runner_err_),
      runner_status(0) {
  start.tv_sec = stop.tv_sec = 0;
  start.tv_nsec = stop.tv_nsec = 0;

  std::vector<uint64_t> codes;
  Hash(dir->c_str(), dir->size()).push(codes);
  Hash(stdin_file->c_str(), stdin_file->size()).push(codes);
  Hash(environ->c_str(), environ->size()).push(codes);
  Hash(cmdline->c_str(), cmdline->size()).push(codes);
  code = Hash(codes);
}

Hash Job::shallow_hash() const { return Hash(job) ^ TYPE_JOB; }

#define JOB(arg, i)                       \
  do {                                    \
    HeapObject *arg = args[i];            \
    REQUIRE(typeid(*arg) == typeid(Job)); \
  } while (0);                            \
  Job *arg = static_cast<Job *>(args[i]);

static void parse_usage(Usage *usage, Value **args, Runtime &runtime, Scope *scope) {
  INTEGER_MPZ(status, 0);
  DOUBLE(rtime, 1);
  DOUBLE(ctime, 2);
  INTEGER_MPZ(membytes, 3);
  INTEGER_MPZ(ibytes, 4);
  INTEGER_MPZ(obytes, 5);

  usage->status = mpz_get_si(status);
  usage->runtime = rtime->value;
  usage->cputime = ctime->value;
  usage->membytes = mpz_get_si(membytes);
  usage->ibytes = mpz_get_si(ibytes);
  usage->obytes = mpz_get_si(obytes);
}

static PRIMTYPE(type_job_fail) {
  return args.size() == 2 && args[0]->unify(Data::typeJob) && args[1]->unify(Data::typeError) &&
         out->unify(Data::typeUnit);
}

static PRIMFN(prim_job_fail_launch) {
  EXPECT(2);
  JOB(job, 0);

  REQUIRE(job->state == 0);

  size_t need = reserve_unit() + WJob::reserve();
  runtime.heap.reserve(need);

  job->bad_launch = args[1];
  job->reality.found = true;
  job->reality.status = 128;
  job->reality.runtime = 0;
  job->reality.cputime = 0;
  job->reality.membytes = 0;
  job->reality.ibytes = 0;
  job->reality.obytes = 0;
  job->state = STATE_FORKED | STATE_STDOUT | STATE_STDERR | STATE_MERGED;

  runtime.schedule(WJob::claim(runtime.heap, job));
  RETURN(claim_unit(runtime.heap));
}

static PRIMTYPE(type_job_launch) {
  return args.size() == 12 && args[0]->unify(Data::typeJob) && args[1]->unify(Data::typeString) &&
         args[2]->unify(Data::typeString) && args[3]->unify(Data::typeString) &&
         args[4]->unify(Data::typeString) && args[5]->unify(Data::typeInteger) &&
         args[6]->unify(Data::typeDouble) && args[7]->unify(Data::typeDouble) &&
         args[8]->unify(Data::typeInteger) && args[9]->unify(Data::typeInteger) &&
         args[10]->unify(Data::typeInteger) && args[11]->unify(Data::typeInteger) &&
         out->unify(Data::typeUnit);
}

static PRIMFN(prim_job_launch) {
  JobTable *jobtable = static_cast<JobTable *>(data);
  EXPECT(12);
  JOB(job, 0);
  STRING(dir, 1);
  STRING(stdin_file, 2);
  STRING(env, 3);
  STRING(cmd, 4);
  INTEGER_MPZ(is_atty, 11)

  runtime.heap.reserve(reserve_unit());
  parse_usage(&job->predict, args + 5, runtime, scope);
  job->predict.found = true;

  REQUIRE(job->state == 0);

  auto &heap = jobtable->imp->pending;
  heap.emplace_back(new Task(runtime.heap.root(job), dir->as_str(), stdin_file->as_str(),
                             env->as_str(), cmd->as_str(), mpz_cmp_si(is_atty, 0) != 0));
  std::push_heap(heap.begin(), heap.end());

  // If a scheduled job claims a longer critical path, we need to adjust the total path time
  if (job->pathtime >= status_state.remain) {
#ifdef DEBUG_PROGRESS
    std::cerr << "RUN RAISE CRIT: " << status_state.remain << " => " << job->pathtime << "  /  "
              << status_state.total << " => "
              << (job->pathtime + status_state.total - status_state.remain) << std::endl;
#endif
    status_state.total = job->pathtime + (status_state.total - status_state.remain);
    status_state.remain = job->pathtime;
    status_state.current = job->record.runtime;
  }

  RETURN(claim_unit(runtime.heap));
}

static PRIMTYPE(type_job_virtual) {
  return args.size() == 11 && args[0]->unify(Data::typeJob) && args[1]->unify(Data::typeString) &&
         args[2]->unify(Data::typeString) && args[3]->unify(Data::typeString) &&
         args[4]->unify(Data::typeString) && args[5]->unify(Data::typeInteger) &&
         args[6]->unify(Data::typeDouble) && args[7]->unify(Data::typeDouble) &&
         args[8]->unify(Data::typeInteger) && args[9]->unify(Data::typeInteger) &&
         args[10]->unify(Data::typeInteger) && out->unify(Data::typeUnit);
}

static PRIMFN(prim_job_virtual) {
  EXPECT(11);
  JOB(job, 0);
  STRING(stdout_payload, 1);
  STRING(stderr_payload, 2);
  STRING(runner_out_payload, 3);
  STRING(runner_err_payload, 4);

  size_t need = reserve_unit() + WJob::reserve();
  runtime.heap.reserve(need);

  parse_usage(&job->predict, args + 5, runtime, scope);
  job->predict.found = true;
  job->reality = job->predict;

  clock_gettime(CLOCK_REALTIME, &job->start);
  job->stop = job->start;

  if (!stdout_payload->empty())
    job->db->save_output(job->job, 1, stdout_payload->c_str(), stdout_payload->size(), 0);
  if (!stderr_payload->empty())
    job->db->save_output(job->job, 2, stderr_payload->c_str(), stderr_payload->size(), 0);
  if (!runner_out_payload->empty())
    job->db->save_output(job->job, 3, runner_out_payload->c_str(), runner_out_payload->size(), 0);
  if (!runner_err_payload->empty())
    job->db->save_output(job->job, 4, runner_err_payload->c_str(), runner_err_payload->size(), 0);

  REQUIRE(job->state == 0);

  std::stringstream s;
  s << pretty_cmd(job->cmdline->as_str());
  if (!job->stdin_file->empty()) s << " < " << shell_escape(job->stdin_file->c_str());
  s << std::endl;
  std::string out = s.str();
  // TODO: Clean up echo stuff
  status_get_generic_stream(job->echo.c_str()) << out << std::endl;

  if (!stdout_payload->empty()) {
    // TODO: Add a job_id
    status_get_generic_stream(job->stream_out.c_str()) << stdout_payload->c_str() << std::endl;
  }
  if (!stderr_payload->empty()) {
    // TODO: Add a job_id
    status_get_generic_stream(job->stream_err.c_str()) << stdout_payload->c_str() << std::endl;
  }
  if (!runner_out_payload->empty()) {
    status_get_generic_stream(job->runner_out.c_str()) << runner_out_payload->c_str() << std::endl;
  }
  if (!runner_err_payload->empty()) {
    status_get_generic_stream(job->runner_err.c_str()) << runner_err_payload->c_str() << std::endl;
  }

  job->state = STATE_FORKED | STATE_STDOUT | STATE_STDERR | STATE_RUNNER_OUT | STATE_RUNNER_ERR |
               STATE_MERGED;

  runtime.schedule(WJob::claim(runtime.heap, job));
  RETURN(claim_unit(runtime.heap));
}

static PRIMTYPE(type_job_create) {
  return args.size() == 14 && args[0]->unify(Data::typeString) &&
         args[1]->unify(Data::typeString) && args[2]->unify(Data::typeString) &&
         args[3]->unify(Data::typeString) && args[4]->unify(Data::typeString) &&
         args[5]->unify(Data::typeInteger) && args[6]->unify(Data::typeString) &&
         args[7]->unify(Data::typeInteger) && args[8]->unify(Data::typeString) &&
         args[9]->unify(Data::typeString) && args[10]->unify(Data::typeString) &&
         args[11]->unify(Data::typeString) && args[12]->unify(Data::typeString) &&
         args[13]->unify(Data::typeInteger) && out->unify(Data::typeJob);
}

static PRIMFN(prim_job_create) {
  JobTable *jobtable = static_cast<JobTable *>(data);
  EXPECT(14);
  STRING(label, 0);
  STRING(dir, 1);
  STRING(stdin_file, 2);
  STRING(env, 3);
  STRING(cmd, 4);
  INTEGER_MPZ(signature, 5);
  STRING(visible, 6);
  INTEGER_MPZ(keep, 7);
  STRING(echo, 8);
  STRING(stream_out, 9);
  STRING(stream_err, 10);
  STRING(runner_out, 11);
  STRING(runner_err, 12);
  INTEGER_MPZ(is_atty, 13);

  Hash hash;
  REQUIRE(mpz_sizeinbase(signature, 2) <= 8 * sizeof(hash.data));
  mpz_export(&hash.data[0], 0, 1, sizeof(hash.data[0]), 0, 0, signature);

  Job *out = Job::alloc(runtime.heap, jobtable->imp->db, label, dir, stdin_file, env, cmd,
                        mpz_cmp_si(keep, 0), echo->c_str(), stream_out->c_str(),
                        stream_err->c_str(), runner_out->c_str(), runner_err->c_str());

  out->record = jobtable->imp->db->predict_job(out->code.data[0], &out->pathtime);

  std::stringstream stack;
  for (auto &x : scope->stack_trace()) stack << x << std::endl;

  out->db->insert_job(dir->as_str(), cmd->as_str(), env->as_str(), stdin_file->as_str(),
                      hash.data[0], label->as_str(), stack.str(), mpz_cmp_si(is_atty, 0) != 0,
                      visible->as_str(), &out->job);

  RETURN(out);
}

static size_t reserve_tree(const std::vector<FileReflection> &files) {
  size_t need = reserve_list(files.size());
  for (auto &i : files)
    need += reserve_tuple2() + String::reserve(i.path.size()) + String::reserve(i.hash.size());
  return need;
}

static Value *claim_tree(Heap &h, const std::vector<FileReflection> &files) {
  std::vector<Value *> vals;
  vals.reserve(files.size());
  for (auto &i : files)
    vals.emplace_back(claim_tuple2(h, String::claim(h, i.path), String::claim(h, i.hash)));
  return claim_list(h, vals.size(), vals.data());
}

static PRIMTYPE(type_job_cache) {
  TypeVar spair;
  TypeVar plist;
  TypeVar jlist;
  TypeVar pair;
  Data::typePair.clone(spair);
  Data::typeList.clone(plist);
  Data::typeList.clone(jlist);
  Data::typePair.clone(pair);
  spair[0].unify(Data::typeString);
  spair[1].unify(Data::typeString);
  plist[0].unify(spair);
  jlist[0].unify(Data::typeJob);
  pair[0].unify(jlist);
  pair[1].unify(plist);
  return args.size() == 7 && args[0]->unify(Data::typeString) && args[1]->unify(Data::typeString) &&
         args[2]->unify(Data::typeString) && args[3]->unify(Data::typeString) &&
         args[4]->unify(Data::typeInteger) && args[5]->unify(Data::typeString) &&
         args[6]->unify(Data::typeInteger) && out->unify(pair);
}

static PRIMFN(prim_job_cache) {
  JobTable *jobtable = static_cast<JobTable *>(data);
  EXPECT(7);
  STRING(dir, 0);
  STRING(stdin_file, 1);
  STRING(env, 2);
  STRING(cmd, 3);
  INTEGER_MPZ(signature, 4);
  STRING(visible, 5);
  INTEGER_MPZ(is_atty, 6);

  Hash hash;
  REQUIRE(mpz_sizeinbase(signature, 2) <= 8 * sizeof(hash.data));
  mpz_export(&hash.data[0], 0, 1, sizeof(hash.data[0]), 0, 0, signature);

  // This function can be rerun; it's side effect has no impact on re-execution of reuse_job.
  long job;
  double pathtime;
  std::vector<FileReflection> files;
  Usage reuse = jobtable->imp->db->reuse_job(
      dir->as_str(), env->as_str(), cmd->as_str(), stdin_file->as_str(), hash.data[0],
      mpz_cmp_si(is_atty, 0) != 0, visible->as_str(), jobtable->imp->check, job, files, &pathtime);

  size_t need = reserve_tuple2() + reserve_tree(files) + reserve_list(1) + Job::reserve();
  runtime.heap.reserve(need);

  Value *joblist;
  if (reuse.found && !jobtable->imp->check) {
    Job *jobp = Job::claim(runtime.heap, jobtable->imp->db, dir, dir, stdin_file, env, cmd, true,
                           STREAM_ECHO, STREAM_INFO, STREAM_WARNING, STREAM_INFO, STREAM_ERROR);
    jobp->state = STATE_FORKED | STATE_STDOUT | STATE_STDERR | STATE_RUNNER_OUT | STATE_RUNNER_ERR |
                  STATE_MERGED | STATE_FINISHED;
    jobp->job = job;
    jobp->record = reuse;
    // predict + reality unusued since Job not run
    jobp->report = reuse;
    jobp->reality = reuse;
    jobp->pathtime = pathtime;

    Value *obj = jobp;
    joblist = claim_list(runtime.heap, 1, &obj);

    // Even though this job is not run, it might have been the 'next' job of something that DID run
    if (pathtime >= status_state.remain &&
        pathtime * ALMOST_ONE * ALMOST_ONE <= status_state.remain) {
      auto crit = jobtable->imp->critJob(ALMOST_ONE * (pathtime - reuse.runtime));
#ifdef DEBUG_PROGRESS
      std::cerr << "DECREASE CRIT: " << status_state.remain << " => " << crit.pathtime << "  /  "
                << status_state.total << " => "
                << (status_state.total - status_state.remain - crit.pathtime) << std::endl;
#endif
      status_state.total = crit.pathtime + (status_state.total - status_state.remain);
      status_state.remain = crit.pathtime;
      status_state.current = crit.runtime;
      if (crit.runtime == 0) clock_gettime(CLOCK_REALTIME, &jobtable->imp->wall);
    }
  } else {
    joblist = claim_list(runtime.heap, 0, nullptr);
  }

  RETURN(claim_tuple2(runtime.heap, joblist, claim_tree(runtime.heap, files)));
}

static size_t reserve_usage(const Usage &usage) {
  MPZ s(usage.status);
  MPZ m(usage.membytes);
  MPZ i(usage.ibytes);
  MPZ o(usage.obytes);
  return Integer::reserve(s) + Double::reserve() + Double::reserve() + Integer::reserve(m) +
         Integer::reserve(i) + Integer::reserve(o) + reserve_tuple2() * 5;
}

static Value *claim_usage(Heap &h, const Usage &usage) {
  MPZ s(usage.status);
  MPZ m(usage.membytes);
  MPZ i(usage.ibytes);
  MPZ o(usage.obytes);
  return claim_tuple2(
      h, claim_tuple2(h, Integer::claim(h, s), Double::claim(h, usage.runtime)),
      claim_tuple2(h, claim_tuple2(h, Double::claim(h, usage.cputime), Integer::claim(h, m)),
                   claim_tuple2(h, Integer::claim(h, i), Integer::claim(h, o))));
}

static PRIMTYPE(type_job_output) {
  TypeVar result;
  Data::typeResult.clone(result);
  result[0].unify(Data::typeString);
  result[1].unify(Data::typeError);
  return args.size() == 2 && args[0]->unify(Data::typeJob) && args[1]->unify(Data::typeInteger) &&
         out->unify(result);
}

static PRIMFN(prim_job_output) {
  EXPECT(2);
  JOB(arg0, 0);
  INTEGER_MPZ(arg1, 1);

  runtime.heap.reserve(Tuple::fulfiller_pads + WJob::reserve());
  Continuation *continuation = scope->claim_fulfiller(runtime, output);

  if (mpz_cmp_si(arg1, 1) == 0) {
    runtime.schedule(WJob::claim(runtime.heap, arg0));
    continuation->next = arg0->q_stdout;
    arg0->q_stdout = continuation;
  } else if (mpz_cmp_si(arg1, 2) == 0) {
    runtime.schedule(WJob::claim(runtime.heap, arg0));
    continuation->next = arg0->q_stderr;
    arg0->q_stderr = continuation;
  } else {
    bool stdin_or_stderr = false;
    REQUIRE(stdin_or_stderr);
  }
}

static PRIMFN(prim_job_runner_output) {
  EXPECT(2);
  JOB(arg0, 0);
  INTEGER_MPZ(arg1, 1);

  runtime.heap.reserve(Tuple::fulfiller_pads + WJob::reserve());
  Continuation *continuation = scope->claim_fulfiller(runtime, output);

  if (mpz_cmp_si(arg1, 1) == 0) {
    runtime.schedule(WJob::claim(runtime.heap, arg0));
    continuation->next = arg0->q_runner_out;
    arg0->q_runner_out = continuation;
  } else if (mpz_cmp_si(arg1, 2) == 0) {
    runtime.schedule(WJob::claim(runtime.heap, arg0));
    continuation->next = arg0->q_runner_err;
    arg0->q_runner_err = continuation;
  } else {
    bool valid_stream = false;
    REQUIRE(valid_stream);
  }
}

static PRIMTYPE(type_job_report_runner_error) {
  return args.size() == 2 && args[0]->unify(Data::typeJob) && args[1]->unify(Data::typeString) &&
         out->unify(Data::typeJob);
}

static PRIMFN(prim_job_report_runner_error) {
  EXPECT(2);
  JOB(job, 0);
  STRING(error_message, 1);

  job->db->save_output(job->job, 4, error_message->c_str(), error_message->size(), 0);
  job->state |= STATE_RUNNER_ERR;

  runtime.heap.reserve(WJob::reserve());
  runtime.schedule(WJob::claim(runtime.heap, job));

  RETURN(args[0]);
}

static PRIMTYPE(type_job_report_runner_output) {
  return args.size() == 2 && args[0]->unify(Data::typeJob) && args[1]->unify(Data::typeString) &&
         out->unify(Data::typeJob);
}

static PRIMFN(prim_job_report_runner_output) {
  EXPECT(2);
  JOB(job, 0);
  STRING(output_message, 1);

  job->db->save_output(job->job, 3, output_message->c_str(), output_message->size(), 0);
  job->state |= STATE_RUNNER_OUT;

  runtime.heap.reserve(WJob::reserve());
  runtime.schedule(WJob::claim(runtime.heap, job));

  RETURN(args[0]);
}

static PRIMTYPE(type_job_tree) {
  TypeVar list;
  TypeVar pair;
  Data::typeList.clone(list);
  Data::typePair.clone(pair);
  list[0].unify(pair);
  pair[0].unify(Data::typeString);
  pair[1].unify(Data::typeString);
  TypeVar result;
  Data::typeResult.clone(result);
  result[0].unify(list);
  result[1].unify(Data::typeError);
  return args.size() == 2 && args[0]->unify(Data::typeJob) && args[1]->unify(Data::typeInteger) &&
         out->unify(result);
}

static PRIMFN(prim_job_tree) {
  EXPECT(2);
  JOB(arg0, 0);
  INTEGER_MPZ(arg1, 1);

  runtime.heap.reserve(Tuple::fulfiller_pads + WJob::reserve());
  Continuation *continuation = scope->claim_fulfiller(runtime, output);

  if (mpz_cmp_si(arg1, 1) == 0) {
    runtime.schedule(WJob::claim(runtime.heap, arg0));
    continuation->next = std::move(arg0->q_inputs);
    arg0->q_inputs = std::move(continuation);
  } else if (mpz_cmp_si(arg1, 2) == 0) {
    runtime.schedule(WJob::claim(runtime.heap, arg0));
    continuation->next = std::move(arg0->q_outputs);
    arg0->q_outputs = std::move(continuation);
  } else {
    bool stdin_or_stderr = false;
    REQUIRE(stdin_or_stderr);
  }
}

static PRIMTYPE(type_job_id) {
  return args.size() == 1 && args[0]->unify(Data::typeJob) && out->unify(Data::typeInteger);
}

static PRIMFN(prim_job_id) {
  EXPECT(1);
  JOB(arg0, 0);
  MPZ out(arg0->job);
  RETURN(Integer::alloc(runtime.heap, out));
}

static PRIMTYPE(type_job_desc) {
  return args.size() == 1 && args[0]->unify(Data::typeJob) && out->unify(Data::typeString);
}

static PRIMFN(prim_job_desc) {
  EXPECT(1);
  JOB(arg0, 0);
  std::string desc =
      arg0->label->empty() ? pretty_cmd(arg0->cmdline->as_str()) : arg0->label->as_str();
  RETURN(String::alloc(runtime.heap, desc));
}

static PRIMTYPE(type_job_finish) {
  return args.size() == 10 && args[0]->unify(Data::typeJob) && args[1]->unify(Data::typeString) &&
         args[2]->unify(Data::typeString) && args[3]->unify(Data::typeString) &&
         args[4]->unify(Data::typeInteger) && args[5]->unify(Data::typeDouble) &&
         args[6]->unify(Data::typeDouble) && args[7]->unify(Data::typeInteger) &&
         args[8]->unify(Data::typeInteger) && args[9]->unify(Data::typeInteger) &&
         out->unify(Data::typeUnit);
}

static int64_t int64_ns(struct timespec tv) {
  return static_cast<int64_t>(tv.tv_sec) * 1000000000 + tv.tv_nsec;
}

static PRIMFN(prim_job_finish) {
  EXPECT(10);
  JOB(job, 0);
  STRING(inputs, 1);
  STRING(outputs, 2);
  STRING(all_outputs, 3);

  REQUIRE(job->state & STATE_MERGED);
  REQUIRE(!(job->state & STATE_FINISHED));

  size_t need = WJob::reserve() + reserve_unit();
  runtime.heap.reserve(need);

  parse_usage(&job->report, args + 4, runtime, scope);
  job->report.found = true;

  bool keep = !job->bad_launch && !job->bad_finish && job->keep && job->report.status == 0;
  job->db->finish_job(job->job, inputs->as_str(), outputs->as_str(), all_outputs->as_str(),
                      int64_ns(job->start), int64_ns(job->stop), job->code.data[0], keep,
                      job->report);

  // Runner status is left untouched to avoid overwriting any errors set by `prim_job_set_runner_status`
  // if that was called first, with the safe fallback of a NULL database value on runner success.

  job->state |= STATE_FINISHED;

  runtime.schedule(WJob::claim(runtime.heap, job));
  RETURN(claim_unit(runtime.heap));
}

static PRIMTYPE(type_job_tag) {
  return args.size() == 3 && args[0]->unify(Data::typeJob) && args[1]->unify(Data::typeString) &&
         args[2]->unify(Data::typeString) && out->unify(Data::typeUnit);
}

static PRIMFN(prim_job_tag) {
  EXPECT(3);
  JOB(job, 0);
  STRING(uri, 1);
  STRING(content, 2);

  runtime.heap.reserve(reserve_unit());
  job->db->tag_job(job->job, uri->as_str(), content->as_str());
  RETURN(claim_unit(runtime.heap));
}

static PRIMTYPE(type_add_hash) {
  return args.size() == 2 && args[0]->unify(Data::typeString) && args[1]->unify(Data::typeString) &&
         out->unify(Data::typeString);
}

static PRIMFN(prim_add_hash) {
  JobTable *jobtable = static_cast<JobTable *>(data);
  EXPECT(2);
  STRING(file, 0);
  STRING(hash, 1);
  jobtable->imp->db->add_hash(file->as_str(), hash->as_str(), getmtime_ns(file->c_str()));
  RETURN(args[0]);
}

static PRIMTYPE(type_get_hash) {
  return args.size() == 1 && args[0]->unify(Data::typeString) && out->unify(Data::typeString);
}

static PRIMFN(prim_get_hash) {
  JobTable *jobtable = static_cast<JobTable *>(data);
  EXPECT(1);
  STRING(file, 0);
  std::string hash = jobtable->imp->db->get_hash(file->as_str(), getmtime_ns(file->c_str()));
  RETURN(String::alloc(runtime.heap, hash));
}

static PRIMTYPE(type_get_modtime) {
  return args.size() == 1 && args[0]->unify(Data::typeString) && out->unify(Data::typeInteger);
}

static PRIMFN(prim_get_modtime) {
  EXPECT(1);
  STRING(file, 0);
  MPZ out(getmtime_ns(file->c_str()));
  RETURN(Integer::alloc(runtime.heap, out));
}

static PRIMTYPE(type_search_path) {
  return args.size() == 2 && args[0]->unify(Data::typeString) && args[1]->unify(Data::typeString) &&
         out->unify(Data::typeString);
}

static PRIMFN(prim_search_path) {
  EXPECT(2);
  STRING(path, 0);
  STRING(exec, 1);

  auto out = find_in_path(exec->as_str(), path->as_str());
  RETURN(String::alloc(runtime.heap, out));
}

static void usage_type(TypeVar &pair) {
  TypeVar pair0;
  TypeVar pair1;
  TypeVar pair10;
  TypeVar pair11;
  Data::typePair.clone(pair);
  Data::typePair.clone(pair0);
  Data::typePair.clone(pair1);
  Data::typePair.clone(pair10);
  Data::typePair.clone(pair11);
  pair[0].unify(pair0);
  pair[1].unify(pair1);
  pair0[0].unify(Data::typeInteger);
  pair0[1].unify(Data::typeDouble);
  pair1[0].unify(pair10);
  pair10[0].unify(Data::typeDouble);
  pair10[1].unify(Data::typeInteger);
  pair1[1].unify(pair11);
  pair11[0].unify(Data::typeInteger);
  pair11[1].unify(Data::typeInteger);
}

static PRIMTYPE(type_job_reality) {
  TypeVar pair;
  usage_type(pair);
  TypeVar result;
  Data::typeResult.clone(result);
  result[0].unify(pair);
  result[1].unify(Data::typeError);
  return args.size() == 1 && args[0]->unify(Data::typeJob) && out->unify(result);
}

static PRIMFN(prim_job_reality) {
  EXPECT(1);
  JOB(job, 0);

  runtime.heap.reserve(Tuple::fulfiller_pads + WJob::reserve());
  Continuation *continuation = scope->claim_fulfiller(runtime, output);

  runtime.schedule(WJob::claim(runtime.heap, job));
  continuation->next = job->q_reality;
  job->q_reality = continuation;
}

static PRIMTYPE(type_job_report) {
  TypeVar pair;
  usage_type(pair);
  TypeVar result;
  Data::typeResult.clone(result);
  result[0].unify(pair);
  result[1].unify(Data::typeError);
  return args.size() == 1 && args[0]->unify(Data::typeJob) && out->unify(result);
}

static PRIMFN(prim_job_report) {
  EXPECT(1);
  JOB(job, 0);

  runtime.heap.reserve(Tuple::fulfiller_pads + WJob::reserve());
  Continuation *continuation = scope->claim_fulfiller(runtime, output);

  runtime.schedule(WJob::claim(runtime.heap, job));
  continuation->next = job->q_report;
  job->q_report = continuation;
}

static PRIMTYPE(type_job_record) {
  TypeVar list;
  TypeVar pair;
  Data::typeList.clone(list);
  usage_type(pair);
  list[0].unify(pair);
  return args.size() == 1 && args[0]->unify(Data::typeJob) && out->unify(list);
}

static PRIMFN(prim_job_record) {
  EXPECT(1);
  JOB(job, 0);

  size_t need = reserve_usage(job->record) + reserve_list(1);
  runtime.heap.reserve(need);

  if (job->record.found) {
    Value *obj = claim_usage(runtime.heap, job->record);
    RETURN(claim_list(runtime.heap, 1, &obj));
  } else {
    RETURN(claim_list(runtime.heap, 0, nullptr));
  }
}

static PRIMTYPE(type_job_set_runner_status) {
  return args.size() == 2 && args[0]->unify(Data::typeJob) && args[1]->unify(Data::typeString) &&
         out->unify(Data::typeUnit);
}

static PRIMFN(prim_job_set_runner_status) {
  EXPECT(2);
  JOB(job, 0);
  STRING(error_message, 1);

  job->db->set_runner_status(job->job, error_message->as_str());

  runtime.heap.reserve(reserve_unit());
  RETURN(claim_unit(runtime.heap));
}

static PRIMTYPE(type_job_clear_runner_status) {
  return args.size() == 1 && args[0]->unify(Data::typeJob) && out->unify(Data::typeUnit);
}

static PRIMFN(prim_job_clear_runner_status) {
  EXPECT(1);
  JOB(job, 0);

  job->db->set_runner_status(job->job);

  runtime.heap.reserve(reserve_unit());
  RETURN(claim_unit(runtime.heap));
}

static PRIMTYPE(type_job_runner_status) {
  TypeVar inner_result;
  Data::typeResult.clone(inner_result);
  inner_result[0].unify(Data::typeUnit);
  inner_result[1].unify(Data::typeString);

  TypeVar outer_result;
  Data::typeResult.clone(outer_result);
  outer_result[0].unify(inner_result);
  outer_result[1].unify(Data::typeError);

  return args.size() == 1 && args[0]->unify(Data::typeJob) && out->unify(outer_result);
}

static PRIMFN(prim_job_runner_status) {
  EXPECT(1);
  JOB(job, 0);

  std::pair<bool, std::string> status_result = job->db->get_runner_status(job->job);
  bool has_error = status_result.first;
  std::string error_message = status_result.second;

  // Create the inner `Result Unit String` (isomorphic to `Option String`)
  HeapObject *inner_result;
  if (has_error) {
    // Failure case: runner error message present (including empty string) -> Fail String
    runtime.heap.reserve(reserve_result() + String::reserve(error_message.size()));
    inner_result = claim_result(runtime.heap, false, String::claim(runtime.heap, error_message));
  } else {
    // Success case: no runner error (NULL in database) -> Pass Unit
    runtime.heap.reserve(reserve_result() + reserve_unit());
    inner_result = claim_result(runtime.heap, true, claim_unit(runtime.heap));
  }

  // Wrap in outer Result for lookup-prim success -> Pass (Result Unit String)
  runtime.heap.reserve(reserve_result());
  RETURN(claim_result(runtime.heap, true, static_cast<Value*>(inner_result)));
}

static PRIMTYPE(type_access) {
  return args.size() == 2 && args[0]->unify(Data::typeString) &&
         args[1]->unify(Data::typeInteger) && out->unify(Data::typeBoolean);
}

static PRIMFN(prim_access) {
  EXPECT(2);
  STRING(file, 0);
  INTEGER_MPZ(kind, 1);

  runtime.heap.reserve(reserve_bool());
  int mode = R_OK;
  if (mpz_cmp_si(kind, 1) == 0) mode = W_OK;
  if (mpz_cmp_si(kind, 2) == 0) mode = X_OK;
  RETURN(claim_bool(runtime.heap, access(file->c_str(), mode) == 0));
}

static PRIMTYPE(type_job_cache_read) {
  TypeVar result;
  Data::typeResult.clone(result);
  result[0].unify(Data::typeString);
  result[1].unify(Data::typeString);
  return args.size() == 1 && args[0]->unify(Data::typeString) && out->unify(result);
}

static PRIMFN(prim_job_cache_read) {
  wcl::log::info("prim_job_cache_read enter")();
  auto defer = wcl::make_defer([]() { wcl::log::info("prim_job_cache_read exit")(); });
  EXPECT(1);
  STRING(request_str, 0);

  // First, the user may have not turned on the job cache
  if (!internal_job_cache) {
    std::string s =
        "A job cache has not been specified. Please use WAKE_LOCAL_JOB_CACHE=<path> to turn on job "
        "caching";
    size_t need = String::reserve(s.size()) + reserve_result();
    runtime.heap.reserve(need);
    RETURN(claim_result(runtime.heap, false, String::claim(runtime.heap, s)));
  }

  // Now since we receive the request in the form of a json (because we need
  // rather complex information) we need to parse the json (because currently
  // we have pre-made macro to take a JValue directly)
  std::stringstream errs;
  JAST jast;
  if (!JAST::parse(request_str->c_str(), request_str->size(), errs, jast)) {
    std::string s = errs.str();
    size_t need = String::reserve(s.size()) + reserve_result();
    runtime.heap.reserve(need);
    RETURN(claim_result(runtime.heap, false, String::claim(runtime.heap, s)));
  }

  // Now we actully perform the request
  // TODO: It's probably not great that wake hard-fails if this json isn't
  // valid. I should fix that.
  job_cache::FindJobRequest request(jast);
  job_cache::FindJobResponse response = internal_job_cache->read(request);

  // Because I'm very lazy however we also return a string and not a JValue.
  // This is because the `measure_jast` function is defined in json.cpp and I
  // don't want to lift it or duplicate it right now.
  // TODO: lift the measure_jast function
  std::stringstream result_json_stream;
  result_json_stream << response.to_json();
  std::string result_json_str = result_json_stream.str();
  size_t need = String::reserve(result_json_str.size()) + reserve_result();
  runtime.heap.reserve(need);

  RETURN(claim_result(runtime.heap, true, String::claim(runtime.heap, result_json_str)));
}

static PRIMTYPE(type_job_cache_add) {
  TypeVar result;
  Data::typeResult.clone(result);
  result[0].unify(Data::typeString);
  result[1].unify(Data::typeString);
  return args.size() == 1 && args[0]->unify(Data::typeString) && out->unify(result);
}

static PRIMFN(prim_job_cache_add) {
  wcl::log::info("prim_job_cache_add enter")();
  auto defer = wcl::make_defer([]() { wcl::log::info("prim_job_cache_add exit")(); });
  EXPECT(1);
  STRING(request_str, 0);

  // First, the user may have not turned on the job cache
  if (!internal_job_cache) {
    std::string s =
        "A job cache has not been specified. Please use WAKE_LOCAL_JOB_CACHE=<path> to turn on job "
        "caching";
    size_t need = String::reserve(s.size()) + reserve_result();
    runtime.heap.reserve(need);
    RETURN(claim_result(runtime.heap, false, String::claim(runtime.heap, s)));
  }

  // Now since we receive the request in the form of a json (because we need
  // rather complex information) we need to parse the json (because currently
  // we have pre-made macro to take a JValue directly)
  std::stringstream errs;
  JAST jast;
  if (!JAST::parse(request_str->c_str(), request_str->size(), errs, jast)) {
    std::string s = errs.str();
    size_t need = String::reserve(s.size()) + reserve_result();
    runtime.heap.reserve(need);
    RETURN(claim_result(runtime.heap, false, String::claim(runtime.heap, s)));
  }

  // TODO: This just fails if an issue occurs. Would be nice to fail
  //       with a bit more information. Right now we just use a simple
  //       string.
  job_cache::AddJobRequest request = job_cache::AddJobRequest::from_implicit(jast);
  internal_job_cache->add(request);
  std::string result_json_str = "successfully added job";
  size_t need = String::reserve(result_json_str.size()) + reserve_result();
  runtime.heap.reserve(need);

  wcl::log::info("Cache::add successfully called")();
  RETURN(claim_result(runtime.heap, true, String::claim(runtime.heap, result_json_str)));
}

void prim_register_job(JobTable *jobtable, PrimMap &pmap) {
  /*****************************************************************************************
   * These require a Job argument so won't get const-prop evaluated (they don't return)    *
   *****************************************************************************************/

  // Get's the stdout/stderr of a job
  prim_register(pmap, "job_output", prim_job_output, type_job_output, PRIM_PURE);

  // Get's the runner_out/runner_err of a job
  prim_register(pmap, "job_runner_output", prim_job_runner_output, type_job_output, PRIM_PURE);

  // Reports a runner error for a job
  prim_register(pmap, "job_report_runner_error", prim_job_report_runner_error,
                type_job_report_runner_error, PRIM_IMPURE);

  // Reports runner output for a job
  prim_register(pmap, "job_report_runner_output", prim_job_report_runner_output,
                type_job_report_runner_output, PRIM_IMPURE);

  // Sets the runner status for a job
  prim_register(pmap, "job_set_runner_status", prim_job_set_runner_status,
                type_job_set_runner_status, PRIM_IMPURE);

  // Clears the runner status for a job (sets to NULL)
  prim_register(pmap, "job_clear_runner_status", prim_job_clear_runner_status,
                type_job_clear_runner_status, PRIM_IMPURE);

  // Gets the runner status for a job
  prim_register(pmap, "job_runner_status", prim_job_runner_status, type_job_runner_status,
                PRIM_PURE);

  // Get's the set of file paths of a job: 0=visible, 1=input, 2=output
  prim_register(pmap, "job_tree", prim_job_tree, type_job_tree, PRIM_PURE);

  // The id of the job
  prim_register(pmap, "job_id", prim_job_id, type_job_id, PRIM_PURE);

  // The description of the job
  prim_register(pmap, "job_desc", prim_job_desc, type_job_desc, PRIM_PURE);

  // The usage of a job as observed by getrusage() as a result of job_launch.
  // Alternativelly if job_virtual is used instead this is what's reported to
  // job_virtual.
  prim_register(pmap, "job_reality", prim_job_reality, type_job_reality, PRIM_PURE);

  // The usage reported to job_finish. This is useful because a remote machine or a job
  // that uses caching might appear from observation (e.g. job_reality) to consume far
  // fewer resources than what we actully care about.
  prim_register(pmap, "job_report", prim_job_report, type_job_report, PRIM_PURE);

  // Previous usage (returns Option Usage if no prior use exists) if previouslly in the database
  prim_register(pmap, "job_record", prim_job_record, type_job_record, PRIM_PURE);

  /*****************************************************************************************
   * These should not be eliminated (they have effects)                                    *
   *****************************************************************************************/

  // Checks if a job is cached already or not. If it is cached, it returns a job that you
  // can already call all the queries on and they'll return immeditly.
  prim_register(pmap, "job_cache", prim_job_cache, type_job_cache, PRIM_IMPURE, jobtable);

  // Creates a job object
  prim_register(pmap, "job_create", prim_job_create, type_job_create, PRIM_IMPURE, jobtable);

  // Launches a job, note that this can have any specified command/env etc.. seperate from
  // what was passed to job_create. This actully causes a child process to kick off.
  prim_register(pmap, "job_launch", prim_job_launch, type_job_launch, PRIM_IMPURE, jobtable);

  // This is like job_launch but instead you supply what job_launch would return to "complete"
  // the created job.
  prim_register(pmap, "job_virtual", prim_job_virtual, type_job_virtual, PRIM_IMPURE, jobtable);

  // This is where you "finish" a job by explaining what its inputs, outputs, usage etc...
  // are. This call unblocks things like `job_output` for instance.
  prim_register(pmap, "job_finish", prim_job_finish, type_job_finish, PRIM_IMPURE);

  // Job's have a secret key-value store on them that maps strings to strings. This lets
  // you annotate jobs with some extra info which can be helpful for sort of structure
  // logging like practices.
  prim_register(pmap, "job_tag", prim_job_tag, type_job_tag, PRIM_IMPURE);

  // Explain to the wake runtime that the job has failed to launch. This can happen if
  // a pre-step of a runner fails in someway for instance.
  prim_register(pmap, "job_fail_launch", prim_job_fail_launch, type_job_fail, PRIM_IMPURE);

  // Specifies the hash of a given file. In practice wake kicks off a job against `shim-wake`
  // to do the hashing.
  prim_register(pmap, "add_hash", prim_add_hash, type_add_hash, PRIM_IMPURE, jobtable);

  // Adds a job to the job cache
  prim_register(pmap, "job_cache_add", prim_job_cache_add, type_job_cache_add, PRIM_IMPURE);

  // Adds a job to the job cache
  prim_register(pmap, "job_cache_read", prim_job_cache_read, type_job_cache_read, PRIM_IMPURE);

  /*****************************************************************************************
   * Dead-code elimination ok, but not CSE/const-prop ok (must be ordered wrt. filesystem) *
   *****************************************************************************************/

  // Get's the hash of a file if it was previouslly hashed in the database by a cached
  // job this session. Returns the empty string otherwise.
  prim_register(pmap, "get_hash", prim_get_hash, type_get_hash, PRIM_ORDERED, jobtable);

  // Get's the modtime of a file, super simple
  prim_register(pmap, "get_modtime", prim_get_modtime, type_get_modtime, PRIM_ORDERED);

  // Given a $PATH variable like path input, searches the filesystem for a matching
  // executable.
  prim_register(pmap, "search_path", prim_search_path, type_search_path, PRIM_ORDERED);

  // Returns true if a file has a given permission. For instance if you ask this function
  // if a file can be executed it will return true/false if wake can execute that file.
  prim_register(pmap, "access", prim_access, type_access, PRIM_ORDERED);
}

static void wake(Runtime &runtime, HeapPointer<Continuation> &q, HeapObject *value) {
  Continuation *c = q.get();
  while (c->next) {
    c->value = value;
    c = static_cast<Continuation *>(c->next.get());
  }
  c->value = value;
  c->next = runtime.stack;
  runtime.stack = q;
  q.reset();
}

void WJob::execute(Runtime &runtime) {
  // This function is a bit sneaky.
  // We don't reserve memory for all potential wake-up events up-front.
  // Instead, we allocate as we go, even causing side effects.
  // Each side-effect is guarded by an 'if' so it only happens the first time.

  if ((job->state & STATE_STDOUT) && job->q_stdout) {
    HeapObject *what;
    if (job->bad_launch) {
      runtime.heap.reserve(reserve_result());
      what = claim_result(runtime.heap, false, job->bad_launch.get());
    } else {
      std::string out(job->db->get_output(job->job, 1));
      runtime.heap.reserve(reserve_result() + String::reserve(out.size()));
      what = claim_result(runtime.heap, true, String::claim(runtime.heap, out));
    }
    wake(runtime, job->q_stdout, what);
  }

  if ((job->state & STATE_STDERR) && job->q_stderr) {
    HeapObject *what;
    if (job->bad_launch) {
      runtime.heap.reserve(reserve_result());
      what = claim_result(runtime.heap, false, job->bad_launch.get());
    } else {
      std::string out(job->db->get_output(job->job, 2));
      runtime.heap.reserve(reserve_result() + String::reserve(out.size()));
      what = claim_result(runtime.heap, true, String::claim(runtime.heap, out));
    }
    wake(runtime, job->q_stderr, what);
  }

  // Handle runner output if available
  if ((job->state & STATE_RUNNER_OUT) && job->q_runner_out) {
    HeapObject *what;
    if (job->bad_launch) {
      runtime.heap.reserve(reserve_result());
      what = claim_result(runtime.heap, false, job->bad_launch.get());
    } else {
      std::string out(job->db->get_output(job->job, 3));
      runtime.heap.reserve(reserve_result() + String::reserve(out.size()));
      what = claim_result(runtime.heap, true, String::claim(runtime.heap, out));
    }
    wake(runtime, job->q_runner_out, what);
  }

  // Handle runner errors if available
  if ((job->state & STATE_RUNNER_ERR) && job->q_runner_err) {
    HeapObject *what;
    if (job->bad_launch) {
      runtime.heap.reserve(reserve_result());
      what = claim_result(runtime.heap, false, job->bad_launch.get());
    } else {
      std::string out(job->db->get_output(job->job, 4));
      runtime.heap.reserve(reserve_result() + String::reserve(out.size()));
      what = claim_result(runtime.heap, true, String::claim(runtime.heap, out));
    }
    wake(runtime, job->q_runner_err, what);
  }

  if ((job->state & STATE_MERGED) && job->q_reality) {
    HeapObject *what;
    if (job->bad_launch) {
      runtime.heap.reserve(reserve_result());
      what = claim_result(runtime.heap, false, job->bad_launch.get());
    } else {
      runtime.heap.reserve(reserve_result() + reserve_usage(job->reality));
      what = claim_result(runtime.heap, true, claim_usage(runtime.heap, job->reality));
    }
    wake(runtime, job->q_reality, what);
  }

  if ((job->state & STATE_FINISHED) && job->q_inputs) {
    HeapObject *what;
    if (job->bad_finish) {
      runtime.heap.reserve(reserve_result());
      what = claim_result(runtime.heap, false, job->bad_finish.get());
    } else {
      auto files = job->db->get_tree(1, job->job);
      runtime.heap.reserve(reserve_result() + reserve_tree(files));
      what = claim_result(runtime.heap, true, claim_tree(runtime.heap, files));
    }
    wake(runtime, job->q_inputs, what);
  }

  if ((job->state & STATE_FINISHED) && job->q_outputs) {
    HeapObject *what;
    if (job->bad_finish) {
      runtime.heap.reserve(reserve_result());
      what = claim_result(runtime.heap, false, job->bad_finish.get());
    } else {
      auto files = job->db->get_tree(2, job->job);
      runtime.heap.reserve(reserve_result() + reserve_tree(files));
      what = claim_result(runtime.heap, true, claim_tree(runtime.heap, files));
    }
    wake(runtime, job->q_outputs, what);
  }

  if ((job->state & STATE_FINISHED) && job->q_report) {
    HeapObject *what;
    if (job->bad_finish) {
      runtime.heap.reserve(reserve_result());
      what = claim_result(runtime.heap, false, job->bad_finish.get());
    } else {
      runtime.heap.reserve(reserve_result() + reserve_usage(job->report));
      what = claim_result(runtime.heap, true, claim_usage(runtime.heap, job->report));
    }
    wake(runtime, job->q_report, what);
  }
}
