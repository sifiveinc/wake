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

#include "database.h"

#include <fcntl.h>
#include <sqlite3.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <iostream>
#include <set>
#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "status.h"
#include "wcl/iterator.h"

// Increment every time the database schema changes
#define SCHEMA_VERSION "7"

#define VISIBLE 0
#define INPUT 1
#define OUTPUT 2
#define INDEXES 3

struct Database::detail {
  bool debugdb;
  sqlite3 *db;
  sqlite3_stmt *get_entropy;
  sqlite3_stmt *set_entropy;
  sqlite3_stmt *add_run;
  sqlite3_stmt *begin_txn;
  sqlite3_stmt *commit_txn;
  sqlite3_stmt *predict_job;
  sqlite3_stmt *stats_job;
  sqlite3_stmt *insert_job;
  sqlite3_stmt *insert_tree;
  sqlite3_stmt *insert_log;
  sqlite3_stmt *wipe_file;
  sqlite3_stmt *insert_file;
  sqlite3_stmt *update_file;
  sqlite3_stmt *get_log;
  sqlite3_stmt *replay_log;
  sqlite3_stmt *get_tree;
  sqlite3_stmt *add_stats;
  sqlite3_stmt *link_stats;
  sqlite3_stmt *detect_overlap;
  sqlite3_stmt *delete_overlap;
  sqlite3_stmt *find_prior;
  sqlite3_stmt *update_prior;
  sqlite3_stmt *delete_prior;
  sqlite3_stmt *fetch_hash;
  sqlite3_stmt *delete_jobs;
  sqlite3_stmt *delete_dups;
  sqlite3_stmt *delete_stats;
  sqlite3_stmt *revtop_order;
  sqlite3_stmt *setcrit_path;
  sqlite3_stmt *tag_job;
  sqlite3_stmt *get_tags;
  sqlite3_stmt *get_all_tags;
  sqlite3_stmt *get_all_runs;
  sqlite3_stmt *get_edges;
  sqlite3_stmt *get_file_dependency;
  sqlite3_stmt *get_output_files;
  sqlite3_stmt *remove_output_files;
  sqlite3_stmt *remove_all_jobs;
  sqlite3_stmt *get_unhashed_file_paths;
  sqlite3_stmt *insert_unhashed_file;
  sqlite3_stmt *get_interleaved_output;
  sqlite3_stmt *set_runner_status;
  sqlite3_stmt *get_runner_status;

  long run_id;
  detail(bool debugdb_)
      : debugdb(debugdb_),
        db(0),
        get_entropy(0),
        set_entropy(0),
        add_run(0),
        begin_txn(0),
        commit_txn(0),
        predict_job(0),
        stats_job(0),
        insert_job(0),
        insert_tree(0),
        insert_log(0),
        wipe_file(0),
        insert_file(0),
        update_file(0),
        get_log(0),
        replay_log(0),
        get_tree(0),
        add_stats(0),
        link_stats(0),
        detect_overlap(0),
        delete_overlap(0),
        find_prior(0),
        update_prior(0),
        delete_prior(0),
        fetch_hash(0),
        delete_jobs(0),
        delete_dups(0),
        delete_stats(0),
        revtop_order(0),
        setcrit_path(0),
        tag_job(0),
        get_tags(0),
        get_all_tags(0),
        get_all_runs(0),
        get_edges(0),
        get_file_dependency(0),
        get_interleaved_output(0),
        set_runner_status(0),
        get_runner_status(0) {}
};

static void close_db(Database::detail *imp) {
  if (imp->db) {
    int ret = sqlite3_close(imp->db);
    if (ret != SQLITE_OK) {
      std::cerr << "Could not close wake.db: " << sqlite3_errmsg(imp->db) << std::endl;
      return;
    }
  }
  imp->db = 0;
}

Database::Database(bool debugdb) : imp(new detail(debugdb)) {}
Database::~Database() { close(); }

static int schema_cb(void *data, int columns, char **values, char **labels) {
  // values[0] = 0 if a fresh DB
  // values[1] = schema version

  // Returning non-zero causes SQLITE_ABORT
  if (columns != 2) return -1;

  // New DB? Ok to use it
  if (!strcmp(values[0], "0")) return 0;

  // No version set? This must be a pre-0.19 database.
  if (!values[1]) return -1;

  // Matching version? Ok to use it
  if (!strcmp(values[1], SCHEMA_VERSION)) return 0;

  // Versions do not match
  return -1;
}

std::string Database::open(bool wait, bool memory, bool tty) {
  if (imp->db) return "";
  // Increment the SCHEMA_VERSION every time the below string changes.
  const char *schema_sql =
      "pragma auto_vacuum=incremental;"
      "pragma journal_mode=wal;"
      "pragma synchronous=0;"
      "pragma locking_mode=exclusive;"
      "pragma foreign_keys=on;"
      "create table if not exists entropy("
      "  row_id integer primary key autoincrement,"
      "  seed   integer not null);"
      "update entropy set seed=0 where 0;"  // "write" to acquire exclusive lock
      "create table if not exists schema("
      "  version integer primary key);"
      "create table if not exists runs("
      "  run_id  integer primary key autoincrement,"
      "  time    integer not null,"
      "  cmdline text    not null);"
      "create table if not exists files("
      "  file_id  integer primary key,"
      "  path     text    not null,"
      "  hash     text    not null,"
      "  modified integer not null);"
      "create unique index if not exists filenames on files(path);"
      "create table if not exists stats("
      "  stat_id    integer primary key autoincrement,"
      "  hashcode   integer not null,"  // on collision, prefer largest stat_id (ie: newest)
      "  status     integer not null,"
      "  runtime    real    not null,"
      "  cputime    real    not null,"
      "  membytes   integer not null,"
      "  ibytes     integer not null,"
      "  obytes     integer not null,"
      "  pathtime   real);"
      "create index if not exists stathash on stats(hashcode);"
      "create table if not exists jobs("
      "  job_id      integer primary key autoincrement,"
      "  run_id      integer not null references runs(run_id),"
      "  use_id      integer not null references runs(run_id),"
      "  label       text    not null,"
      "  directory   text    not null,"
      "  commandline blob    not null,"
      "  environment blob    not null,"
      "  stdin       text    not null,"  // might point outside the workspace
      "  signature   integer not null,"  // hash(FnInputs, FnOutputs, Resources, Keep)
      "  stack       blob    not null,"
      "  stat_id     integer references stats(stat_id),"  // null if unmerged
      "  starttime   integer not null default 0,"
      "  endtime     integer not null default 0,"
      "  keep        integer not null default 0,"
      "  stale       integer not null default 0,"     // 0=false, 1=true
      "  is_atty     integer not null default 0,"     // 0=false, 1=true
      "  runner_status integer not null default 0);"  // 0=success, non-zero=failure
      "create index if not exists job on jobs(directory, commandline, environment, stdin, "
      "signature, keep, job_id, stat_id);"
      "create index if not exists jobstats on jobs(stat_id);"
      "create table if not exists filetree("
      "  tree_id  integer primary key autoincrement,"
      "  access   integer not null,"  // 0=visible, 1=input, 2=output
      "  job_id   integer not null references jobs(job_id) on delete cascade,"
      "  file_id  integer not null references files(file_id),"
      "  unique(job_id, access, file_id) on conflict ignore);"
      "create index if not exists filesearch on filetree(file_id, access, job_id);"
      "create table if not exists log("
      "  log_id     integer primary key autoincrement,"
      "  job_id     integer not null references jobs(job_id) on delete cascade,"
      "  descriptor integer not null,"  // 1=stdout, 2=stderr, 3=runner_out, 4=runner_err
      "  seconds    real    not null,"  // seconds after job start
      "  output     text    not null);"
      "create index if not exists logorder on log(job_id, descriptor, log_id);"
      "create table if not exists tags("
      "  job_id  integer not null references jobs(job_id) on delete cascade,"
      "  uri     text,"
      "  content text,"
      "  unique(job_id, uri) on conflict replace);"
      "create table if not exists unhashed_files("
      "  unhashed_file_id integer primary key autoincrement,"
      "  job_id integer not null references jobs(job_id) on delete cascade,"
      "  path             text not null);"
      "create index if not exists unhashed_outputs on unhashed_files(job_id);";

  bool waiting = false;
  int ret;

  while (true) {
    ret = sqlite3_open_v2(memory ? ":memory:" : "wake.db", &imp->db, SQLITE_OPEN_READWRITE, 0);
    if (ret != SQLITE_OK) {
      if (!imp->db) return "sqlite3_open: out of memory";
      std::string out = sqlite3_errmsg(imp->db);
      close_db(imp.get());
      return out;
    }

#if SQLITE_VERSION_NUMBER >= 3007011
    if (sqlite3_db_readonly(imp->db, 0)) {
      return "read-only";
    }
#endif

    char *fail;
    ret = sqlite3_exec(imp->db, schema_sql, 0, 0, &fail);
    if (ret == SQLITE_OK) {
      if (waiting) {
        std::cerr << std::endl;
      }
      // Use an empty entropy table as a proxy for a new database (it gets filled automatically)
      const char *get_version =
          "select (select count(row_id) from entropy), (select max(version) from schema);";
      const char *set_version = "insert or ignore into schema(version) values(" SCHEMA_VERSION ");";
      ret = sqlite3_exec(imp->db, get_version, &schema_cb, 0, 0);
      if (ret == SQLITE_OK) {
        sqlite3_exec(imp->db, set_version, 0, 0, 0);
        break;
      } else {
        close_db(imp.get());
        return "produced by an incompatible version of wake; remove it.";
      }
    }

    // We must close the DB so that we don't hold it shared, preventing an eventual exclusive
    // winner.
    close_db(imp.get());

    std::string out = fail;
    sqlite3_free(fail);

    if (!wait || ret != SQLITE_BUSY) {
      if (waiting) {
        std::cerr << std::endl;
      }
      return out;
    } else {
      if (tty) {
        if (waiting) {
          std::cerr << ".";
        } else {
          waiting = true;
          std::cerr << "Database wake.db is busy; waiting .";
        }
      }
      sleep(1);
    }
  }

  // prepare statements
  const char *sql_get_entropy = "select seed from entropy order by row_id";
  const char *sql_set_entropy = "insert into entropy(seed) values(?)";
  const char *sql_add_run = "insert into runs(time, cmdline) values(?, ?)";
  const char *sql_begin_txn = "begin transaction";
  const char *sql_commit_txn = "commit transaction";
  const char *sql_predict_job =
      "select status, runtime, cputime, membytes, ibytes, obytes, pathtime"
      " from stats where hashcode=? order by stat_id desc limit 1";
  const char *sql_stats_job =
      "select status, runtime, cputime, membytes, ibytes, obytes, pathtime"
      " from stats where stat_id=?";
  const char *sql_insert_job =
      "insert into jobs(run_id, use_id, label, directory, commandline, environment, stdin, "
      "signature, stack, is_atty)"
      " values(?, ?1, ?, ?, ?, ?, ?, ?, ?, ?)";
  const char *sql_insert_tree =
      "insert into filetree(access, job_id, file_id)"
      " values(?, ?, (select file_id from files where path=?))";
  const char *sql_insert_log =
      "insert into log(job_id, descriptor, seconds, output)"
      " values(?, ?, ?, ?)";
  const char *sql_wipe_file =
      "update jobs set stale=1 where job_id in"
      " (select t.job_id from files f, filetree t"
      "  where f.path=? and f.hash<>? and t.file_id=f.file_id and t.access=1)";
  const char *sql_insert_file =
      "insert or ignore into files(hash, modified, path) values (?, ?, ?)";
  const char *sql_update_file = "update files set hash=?, modified=? where path=?";
  const char *sql_get_log =
      "select output from log where job_id=? and descriptor=? order by log_id";
  const char *sql_replay_log = "select descriptor, output from log where job_id=? order by log_id";
  const char *sql_get_tree =
      "select f.path, f.hash from filetree t, files f"
      " where t.job_id=? and t.access=? and f.file_id=t.file_id order by t.tree_id";
  const char *sql_add_stats =
      "insert into stats(hashcode, status, runtime, cputime, membytes, ibytes, obytes)"
      " values(?, ?, ?, ?, ?, ?, ?)";
  const char *sql_link_stats =
      "update jobs set stat_id=?, starttime=?, endtime=?, keep=? where job_id=?";
  const char *sql_detect_overlap =
      "select f.path from filetree t1, filetree t2, files f"
      " where t1.job_id=?1 and t1.access=2 and t2.file_id=t1.file_id and t2.access=2 and "
      "t2.job_id<>?1 and f.file_id=t1.file_id";
  const char *sql_delete_overlap =
      "delete from jobs where use_id<>? and job_id in "
      "(select t2.job_id from filetree t1, filetree t2"
      "  where t1.job_id=?2 and t1.access=2 and t2.file_id=t1.file_id and t2.access=2 and "
      "t2.job_id<>?2)";
  const char *sql_find_prior =
      "select job_id, stat_id from jobs where "
      "directory=? and commandline=? and environment=? and stdin=? and signature=? and is_atty=? "
      "and keep=1 and "
      "stale=0";
  const char *sql_update_prior = "update jobs set use_id=? where job_id=?";
  const char *sql_delete_prior =
      "delete from jobs where use_id<>?1 and job_id in"
      " (select j2.job_id from jobs j1, jobs j2"
      "  where j1.job_id=?2 and j1.directory=j2.directory and j1.commandline=j2.commandline"
      "  and j1.environment=j2.environment and j1.stdin=j2.stdin and j1.is_atty=j2.is_atty and "
      "j2.job_id<>?2)";
  const char *sql_fetch_hash = "select hash from files where path=? and modified=?";
  const char *sql_delete_jobs =
      "delete from jobs where job_id in"
      " (select job_id from jobs where keep=0 and use_id<>? except select job_id from filetree "
      "where access=2)";
  const char *sql_delete_dups =
      "delete from stats where stat_id in"
      " (select stat_id from (select hashcode, count(*) as num, max(stat_id) as keep from stats "
      "group by hashcode) d, stats s"
      "  where d.num>1 and s.hashcode=d.hashcode and s.stat_id<>d.keep except select stat_id from "
      "jobs)";
  const char *sql_delete_stats =
      "delete from stats where stat_id in"
      " (select stat_id from stats"
      "  where stat_id not in (select stat_id from jobs)"
      "  order by stat_id desc limit 9999999 offset 4*(select count(*) from jobs))";
  const char *sql_revtop_order =
      "select job_id from jobs where use_id=(select max(run_id) from runs) order by job_id desc";
  const char *sql_setcrit_path =
      "update stats set pathtime=runtime+("
      "  select coalesce(max(s.pathtime),0) from filetree f1, filetree f2, jobs j, stats s"
      "  where f1.job_id=?1 and f1.access=2 and f1.file_id=f2.file_id and f2.access=1 and "
      "f2.job_id=j.job_id and j.stat_id=s.stat_id"
      ") where stat_id=(select stat_id from jobs where job_id=?1)";
  const char *sql_tag_job = "insert into tags(job_id, uri, content) values(?, ?, ?)";
  const char *sql_get_tags = "select job_id, uri, content from tags where job_id=?";
  const char *sql_get_all_tags = "select job_id, uri, content from tags";
  const char *sql_get_all_runs = "select run_id, time, cmdline from runs order by time ASC";
  const char *sql_get_edges =
      "select distinct user.job_id as user, used.job_id as used"
      "  from filetree user, filetree used"
      "   where user.access=1 and user.file_id=used.file_id and used.access=2";
  const char *sql_get_file_dependency =
      "SELECT l.job_id, r.job_id"
      " FROM filetree l"
      " INNER JOIN filetree r"
      " ON l.file_id = r.file_id"
      " WHERE l.access = 2 AND r.access = 0";
  const char *sql_get_output_files =
      "select f.path"
      " from filetree ft join files f on f.file_id=ft.file_id join jobs j on ft.job_id=j.job_id"
      " where ft.access = 2"
      " and substr(cast(j.commandline as varchar), 1, 8) != '<source>'"
      " and substr(cast(j.commandline as varchar), 1, 7) != '<claim>'";
  const char *sql_remove_output_files =
      "delete from files"
      " where file_id in ("
      "   select f.file_id"
      "   from filetree ft join files f on f.file_id=ft.file_id join jobs j on ft.job_id=j.job_id"
      "   where ft.access = 2"
      "   and substr(cast(j.commandline as varchar), 1, 8) != '<source>'"
      "   and substr(cast(j.commandline as varchar), 1, 7) != '<claim>'"
      " )";
  const char *sql_remove_all_jobs = "delete from jobs";
  const char *sql_get_unhashed_file_paths = "select path from unhashed_files";
  const char *sql_insert_unhashed_file = "insert into unhashed_files(job_id, path) values(?, ?)";
  const char *sql_get_interleaved_output =
      "select l.output, l.descriptor"
      " from log l"
      " where l.job_id = ?"
      " order by l.seconds";
  const char *sql_set_runner_status = "update jobs set runner_status=? where job_id=?";
  const char *sql_get_runner_status = "select runner_status from jobs where job_id=?";

#define PREPARE(sql, member)                                                                     \
  ret = sqlite3_prepare_v2(imp->db, sql, -1, &imp->member, 0);                                   \
  if (ret != SQLITE_OK) {                                                                        \
    std::string out = std::string("sqlite3_prepare_v2 " #member ": ") + sqlite3_errmsg(imp->db); \
    close();                                                                                     \
    return out;                                                                                  \
  }

  PREPARE(sql_get_entropy, get_entropy);
  PREPARE(sql_set_entropy, set_entropy);
  PREPARE(sql_add_run, add_run);
  PREPARE(sql_begin_txn, begin_txn);
  PREPARE(sql_commit_txn, commit_txn);
  PREPARE(sql_predict_job, predict_job);
  PREPARE(sql_stats_job, stats_job);
  PREPARE(sql_insert_job, insert_job);
  PREPARE(sql_insert_tree, insert_tree);
  PREPARE(sql_insert_log, insert_log);
  PREPARE(sql_wipe_file, wipe_file);
  PREPARE(sql_insert_file, insert_file);
  PREPARE(sql_update_file, update_file);
  PREPARE(sql_get_log, get_log);
  PREPARE(sql_replay_log, replay_log);
  PREPARE(sql_get_tree, get_tree);
  PREPARE(sql_add_stats, add_stats);
  PREPARE(sql_link_stats, link_stats);
  PREPARE(sql_detect_overlap, detect_overlap);
  PREPARE(sql_delete_overlap, delete_overlap);
  PREPARE(sql_find_prior, find_prior);
  PREPARE(sql_update_prior, update_prior);
  PREPARE(sql_delete_prior, delete_prior);
  PREPARE(sql_fetch_hash, fetch_hash);
  PREPARE(sql_delete_jobs, delete_jobs);
  PREPARE(sql_delete_dups, delete_dups);
  PREPARE(sql_delete_stats, delete_stats);
  PREPARE(sql_revtop_order, revtop_order);
  PREPARE(sql_setcrit_path, setcrit_path);
  PREPARE(sql_tag_job, tag_job);
  PREPARE(sql_get_tags, get_tags);
  PREPARE(sql_get_all_tags, get_all_tags);
  PREPARE(sql_get_all_runs, get_all_runs);
  PREPARE(sql_get_edges, get_edges);
  PREPARE(sql_get_file_dependency, get_file_dependency);
  PREPARE(sql_get_output_files, get_output_files);
  PREPARE(sql_remove_output_files, remove_output_files);
  PREPARE(sql_remove_all_jobs, remove_all_jobs);
  PREPARE(sql_get_unhashed_file_paths, get_unhashed_file_paths);
  PREPARE(sql_insert_unhashed_file, insert_unhashed_file);
  PREPARE(sql_get_interleaved_output, get_interleaved_output);
  PREPARE(sql_set_runner_status, set_runner_status);
  PREPARE(sql_get_runner_status, get_runner_status);

  return "";
}

void Database::close() {
  int ret;

#define FINALIZE(member)                                                                       \
  if (imp->member) {                                                                           \
    ret = sqlite3_finalize(imp->member);                                                       \
    if (ret != SQLITE_OK) {                                                                    \
      std::cerr << "Could not sqlite3_finalize " << #member << ": " << sqlite3_errmsg(imp->db) \
                << std::endl;                                                                  \
      return;                                                                                  \
    }                                                                                          \
  }                                                                                            \
  imp->member = 0;

  FINALIZE(get_entropy);
  FINALIZE(set_entropy);
  FINALIZE(add_run);
  FINALIZE(begin_txn);
  FINALIZE(commit_txn);
  FINALIZE(predict_job);
  FINALIZE(stats_job);
  FINALIZE(insert_job);
  FINALIZE(insert_tree);
  FINALIZE(insert_log);
  FINALIZE(wipe_file);
  FINALIZE(insert_file);
  FINALIZE(update_file);
  FINALIZE(get_log);
  FINALIZE(replay_log);
  FINALIZE(get_tree);
  FINALIZE(add_stats);
  FINALIZE(link_stats);
  FINALIZE(detect_overlap);
  FINALIZE(delete_overlap);
  FINALIZE(find_prior);
  FINALIZE(update_prior);
  FINALIZE(delete_prior);
  FINALIZE(fetch_hash);
  FINALIZE(delete_jobs);
  FINALIZE(delete_dups);
  FINALIZE(delete_stats);
  FINALIZE(revtop_order);
  FINALIZE(setcrit_path);
  FINALIZE(tag_job);
  FINALIZE(get_tags);
  FINALIZE(get_all_tags);
  FINALIZE(get_all_runs);
  FINALIZE(get_edges);
  FINALIZE(get_file_dependency);
  FINALIZE(get_output_files);
  FINALIZE(remove_output_files);
  FINALIZE(remove_all_jobs);
  FINALIZE(get_unhashed_file_paths);
  FINALIZE(insert_unhashed_file);
  FINALIZE(get_interleaved_output);
  FINALIZE(set_runner_status);
  FINALIZE(get_runner_status);

  close_db(imp.get());
}

static void finish_stmt(const char *why, sqlite3_stmt *stmt, bool debug) {
  int ret;

  if (debug) {
    std::stringstream s;
    s << "DB:: ";
#if SQLITE_VERSION_NUMBER >= 3014000
    char *tmp = sqlite3_expanded_sql(stmt);
    s << tmp;
    sqlite3_free(tmp);
#else
    s << sqlite3_sql(stmt);
#endif
    s << std::endl;
    status_get_generic_stream(STREAM_LOG) << s.str() << std::endl;
  }

  ret = sqlite3_reset(stmt);
  if (ret != SQLITE_OK) {
    std::cerr << why << "; sqlite3_reset: " << sqlite3_errmsg(sqlite3_db_handle(stmt)) << std::endl;
    exit(1);
  }

  ret = sqlite3_clear_bindings(stmt);
  if (ret != SQLITE_OK) {
    std::cerr << why << "; sqlite3_clear_bindings: " << sqlite3_errmsg(sqlite3_db_handle(stmt))
              << std::endl;
    exit(1);
  }
}

static void single_step(const char *why, sqlite3_stmt *stmt, bool debug) {
  int ret;

  ret = sqlite3_step(stmt);
  if (ret != SQLITE_DONE) {
    std::cerr << why << "; sqlite3_step: " << sqlite3_errmsg(sqlite3_db_handle(stmt)) << std::endl;
    std::cerr << "The failing statement was: ";
#if SQLITE_VERSION_NUMBER >= 3014000
    char *tmp = sqlite3_expanded_sql(stmt);
    std::cerr << tmp;
    sqlite3_free(tmp);
#else
    std::cerr << sqlite3_sql(stmt);
#endif
    std::cerr << std::endl;
    exit(1);
  }

  finish_stmt(why, stmt, debug);
}

static void bind_blob(const char *why, sqlite3_stmt *stmt, int index, const char *str, size_t len) {
  int ret;
  ret = sqlite3_bind_blob(stmt, index, str, len, SQLITE_STATIC);
  if (ret != SQLITE_OK) {
    std::cerr << why << "; sqlite3_bind_blob(" << index
              << "): " << sqlite3_errmsg(sqlite3_db_handle(stmt)) << std::endl;
    exit(1);
  }
}

static void bind_blob(const char *why, sqlite3_stmt *stmt, int index, const std::string &x) {
  bind_blob(why, stmt, index, x.data(), x.size());
}

static void bind_string(const char *why, sqlite3_stmt *stmt, int index, const char *str,
                        size_t len) {
  int ret;
  ret = sqlite3_bind_text(stmt, index, str, len, SQLITE_STATIC);
  if (ret != SQLITE_OK) {
    std::cerr << why << "; sqlite3_bind_text(" << index
              << "): " << sqlite3_errmsg(sqlite3_db_handle(stmt)) << std::endl;
    exit(1);
  }
}

static void bind_string(const char *why, sqlite3_stmt *stmt, int index, const std::string &x) {
  bind_string(why, stmt, index, x.data(), x.size());
}

static void bind_integer(const char *why, sqlite3_stmt *stmt, int index, long x) {
  int ret;
  ret = sqlite3_bind_int64(stmt, index, x);
  if (ret != SQLITE_OK) {
    std::cerr << why << "; sqlite3_bind_int64(" << index
              << "): " << sqlite3_errmsg(sqlite3_db_handle(stmt)) << std::endl;
    exit(1);
  }
}

static void bind_double(const char *why, sqlite3_stmt *stmt, int index, double x) {
  int ret;
  ret = sqlite3_bind_double(stmt, index, x);
  if (ret != SQLITE_OK) {
    std::cerr << why << "; sqlite3_bind_double(" << index
              << "): " << sqlite3_errmsg(sqlite3_db_handle(stmt)) << std::endl;
    exit(1);
  }
}

static std::string rip_column(sqlite3_stmt *stmt, int col) {
  return std::string(static_cast<const char *>(sqlite3_column_blob(stmt, col)),
                     sqlite3_column_bytes(stmt, col));
}

void Database::entropy(uint64_t *key, int words) {
  const char *why = "Could not restore entropy";
  int word;

  begin_txn();

  // Use entropy from DB
  for (word = 0; word < words; ++word) {
    if (sqlite3_step(imp->get_entropy) != SQLITE_ROW) break;
    key[word] = sqlite3_column_int64(imp->get_entropy, 0);
  }
  finish_stmt(why, imp->get_entropy, imp->debugdb);

  // Save any additional entropy needed
  for (; word < words; ++word) {
    bind_integer(why, imp->set_entropy, 1, key[word]);
    single_step(why, imp->set_entropy, imp->debugdb);
  }

  end_txn();
}

void Database::prepare(const std::string &cmdline) {
  struct timespec now;
  clock_gettime(CLOCK_REALTIME, &now);
  int64_t ts = static_cast<int64_t>(now.tv_sec) * 1000000000 + now.tv_nsec;

  const char *why = "Could not insert run";
  bind_integer(why, imp->add_run, 1, ts);
  bind_string(why, imp->add_run, 2, cmdline);
  single_step(why, imp->add_run, imp->debugdb);
  imp->run_id = sqlite3_last_insert_rowid(imp->db);
}

void Database::clean() {
  const char *why = "Could not compute critical path";
  begin_txn();
  while (sqlite3_step(imp->revtop_order) == SQLITE_ROW) {
    bind_integer(why, imp->setcrit_path, 1, sqlite3_column_int64(imp->revtop_order, 0));
    single_step(why, imp->setcrit_path, imp->debugdb);
  }
  finish_stmt(why, imp->revtop_order, imp->debugdb);
  end_txn();

  bind_integer(why, imp->delete_jobs, 1, imp->run_id);
  single_step("Could not clean database jobs", imp->delete_jobs, imp->debugdb);
  single_step("Could not clean database dups", imp->delete_dups, imp->debugdb);
  single_step("Could not clean database stats", imp->delete_stats, imp->debugdb);

  // This cannot be a prepared statement, because pragmas may run on prepare
  char *fail;
  int ret = sqlite3_exec(imp->db, "pragma incremental_vacuum;", 0, 0, &fail);
  if (ret != SQLITE_OK) std::cerr << "Could not recover space: " << fail << std::endl;
}

void Database::begin_txn() const {
  single_step("Could not begin a transaction", imp->begin_txn, imp->debugdb);
}

void Database::end_txn() const {
  single_step("Could not commit a transaction", imp->commit_txn, imp->debugdb);
}

// This function needs to be able to run twice in succession and return the same results
// ... because heap allocations are created to hold the file list output by this function.
// Fortunately, updating use_id is the only side-effect and it does not affect reuse_job.
Usage Database::reuse_job(const std::string &directory, const std::string &environment,
                          const std::string &commandline, const std::string &stdin_file,
                          uint64_t signature, bool is_atty, const std::string &visible, bool check,
                          long &job, std::vector<FileReflection> &files, double *pathtime) {
  Usage out;
  long stat_id;

  const char *why = "Could not check for a cached job";
  begin_txn();
  bind_string(why, imp->find_prior, 1, directory);
  bind_blob(why, imp->find_prior, 2, commandline);
  bind_blob(why, imp->find_prior, 3, environment);
  bind_string(why, imp->find_prior, 4, stdin_file);
  bind_integer(why, imp->find_prior, 5, signature);
  bind_integer(why, imp->find_prior, 6, is_atty);
  out.found = sqlite3_step(imp->find_prior) == SQLITE_ROW;
  if (out.found) {
    job = sqlite3_column_int64(imp->find_prior, 0);
    stat_id = sqlite3_column_int64(imp->find_prior, 1);
  }
  finish_stmt(why, imp->find_prior, imp->debugdb);

  if (!out.found) {
    end_txn();
    return out;
  }

  bind_integer(why, imp->stats_job, 1, stat_id);
  if (sqlite3_step(imp->stats_job) == SQLITE_ROW) {
    out.status = sqlite3_column_int64(imp->stats_job, 0);
    out.runtime = sqlite3_column_double(imp->stats_job, 1);
    out.cputime = sqlite3_column_double(imp->stats_job, 2);
    out.membytes = sqlite3_column_int64(imp->stats_job, 3);
    out.ibytes = sqlite3_column_int64(imp->stats_job, 4);
    out.obytes = sqlite3_column_int64(imp->stats_job, 5);
    *pathtime = sqlite3_column_double(imp->stats_job, 6);
  } else {
    out.found = false;
  }
  finish_stmt(why, imp->stats_job, imp->debugdb);

  // Create a hash table of visible files
  std::unordered_set<std::string> vis;
  const char *tok = visible.c_str();
  const char *end = tok + visible.size();
  for (const char *scan = tok; scan != end; ++scan) {
    if (*scan == 0 && scan != tok) {
      vis.emplace(tok, scan - tok);
      tok = scan + 1;
    }
  }

  // Confirm all inputs are still visible
  bind_integer(why, imp->get_tree, 1, job);
  bind_integer(why, imp->get_tree, 2, INPUT);
  while (sqlite3_step(imp->get_tree) == SQLITE_ROW) {
    if (vis.find(rip_column(imp->get_tree, 0)) == vis.end()) out.found = false;
  }
  finish_stmt(why, imp->get_tree, imp->debugdb);

  // Confirm all outputs still exist, and report their old hashes
  bind_integer(why, imp->get_tree, 1, job);
  bind_integer(why, imp->get_tree, 2, OUTPUT);
  while (sqlite3_step(imp->get_tree) == SQLITE_ROW) {
    std::string path = rip_column(imp->get_tree, 0);
    if (faccessat(AT_FDCWD, path.c_str(), R_OK, AT_SYMLINK_NOFOLLOW) != 0) out.found = false;
    files.emplace_back(std::move(path), rip_column(imp->get_tree, 1));
  }
  finish_stmt(why, imp->get_tree, imp->debugdb);

  // If we need to rerun the job (outputs don't exist), wipe the files-to-check list
  if (!out.found) {
    files.clear();
  }

  if (out.found && !check) {
    bind_integer(why, imp->update_prior, 1, imp->run_id);
    bind_integer(why, imp->update_prior, 2, job);
    single_step(why, imp->update_prior, imp->debugdb);
  }

  end_txn();

  return out;
}

Usage Database::predict_job(uint64_t hashcode, double *pathtime) {
  Usage out;
  const char *why = "Could not predict a job";
  bind_integer(why, imp->predict_job, 1, hashcode);
  if (sqlite3_step(imp->predict_job) == SQLITE_ROW) {
    out.found = true;
    out.status = sqlite3_column_int(imp->predict_job, 0);
    out.runtime = sqlite3_column_double(imp->predict_job, 1);
    out.cputime = sqlite3_column_double(imp->predict_job, 2);
    out.membytes = sqlite3_column_int64(imp->predict_job, 3);
    out.ibytes = sqlite3_column_int64(imp->predict_job, 4);
    out.obytes = sqlite3_column_int64(imp->predict_job, 5);
    *pathtime = sqlite3_column_double(imp->predict_job, 6);
  } else {
    out.found = false;
    out.status = 0;
    out.runtime = 0;
    out.cputime = 0;
    out.membytes = 0;
    out.ibytes = 0;
    out.obytes = 0;
    *pathtime = 0;
  }
  finish_stmt(why, imp->predict_job, imp->debugdb);
  return out;
}

void Database::insert_job(const std::string &directory, const std::string &commandline,
                          const std::string &environment, const std::string &stdin_file,
                          uint64_t signature, const std::string &label, const std::string &stack,
                          bool is_atty, const std::string &visible, long *job) {
  const char *why = "Could not insert a job";
  begin_txn();
  bind_integer(why, imp->insert_job, 1, imp->run_id);
  bind_string(why, imp->insert_job, 2, label);
  bind_string(why, imp->insert_job, 3, directory);
  bind_blob(why, imp->insert_job, 4, commandline);
  bind_blob(why, imp->insert_job, 5, environment);
  bind_string(why, imp->insert_job, 6, stdin_file);
  bind_integer(why, imp->insert_job, 7, signature);
  bind_blob(why, imp->insert_job, 8, stack);
  bind_integer(why, imp->insert_job, 9, is_atty);
  single_step(why, imp->insert_job, imp->debugdb);
  *job = sqlite3_last_insert_rowid(imp->db);
  const char *tok = visible.c_str();
  const char *end = tok + visible.size();
  for (const char *scan = tok; scan != end; ++scan) {
    if (*scan == 0 && scan != tok) {
      bind_integer(why, imp->insert_tree, 1, VISIBLE);
      bind_integer(why, imp->insert_tree, 2, *job);
      bind_string(why, imp->insert_tree, 3, tok, scan - tok);
      single_step(why, imp->insert_tree, imp->debugdb);
      tok = scan + 1;
    }
  }
  end_txn();
}

template <class F>
static void scan_until_sep(char sep, const std::string &to_scan, F f) {
  auto begin = to_scan.begin();
  auto cur_start = begin;
  auto end = to_scan.end();
  while (begin < end) {
    if (*begin == sep) {
      // TODO: This would be better as a string_view. Since the allocation
      //       would not be forced. To compremise we can use an r-value string
      //       so that the user doesn't have to pay an *additional* allocation
      //       if they don't want to. This means its identical for user's that
      //       want to allocate this string anyway.
      f(std::string(cur_start, begin));
      ++begin;
      cur_start = begin;
    } else {
      ++begin;
    }
  }
}

void Database::finish_job(long job, const std::string &inputs, const std::string &outputs,
                          const std::string &all_outputs, int64_t starttime, int64_t endtime,
                          uint64_t hashcode, bool keep, Usage reality) {
  // Compute the unhashed_outputs
  std::set<std::string> output_set;
  std::vector<std::string> unhashed_outputs;
  scan_until_sep('\0', outputs, [&](std::string &&path) { output_set.emplace(std::move(path)); });
  scan_until_sep('\0', all_outputs, [&](std::string &&path) {
    if (!output_set.count(path)) unhashed_outputs.emplace_back(std::move(path));
  });

  const char *why = "Could not save job inputs and outputs";
  begin_txn();

  bind_integer(why, imp->add_stats, 1, hashcode);
  bind_integer(why, imp->add_stats, 2, reality.status);
  bind_double(why, imp->add_stats, 3, reality.runtime);
  bind_double(why, imp->add_stats, 4, reality.cputime);
  bind_integer(why, imp->add_stats, 5, reality.membytes);
  bind_integer(why, imp->add_stats, 6, reality.ibytes);
  bind_integer(why, imp->add_stats, 7, reality.obytes);
  single_step(why, imp->add_stats, imp->debugdb);
  bind_integer(why, imp->link_stats, 1, sqlite3_last_insert_rowid(imp->db));
  bind_integer(why, imp->link_stats, 2, starttime);
  bind_integer(why, imp->link_stats, 3, endtime);
  bind_integer(why, imp->link_stats, 4, keep ? 1 : 0);
  bind_integer(why, imp->link_stats, 5, job);
  single_step(why, imp->link_stats, imp->debugdb);

  // Grab the visible set
  std::set<std::string> visible;
  bind_integer(why, imp->get_tree, 1, job);
  bind_integer(why, imp->get_tree, 2, VISIBLE);
  while (sqlite3_step(imp->get_tree) == SQLITE_ROW) visible.insert(rip_column(imp->get_tree, 0));
  finish_stmt(why, imp->get_tree, imp->debugdb);

  // Insert inputs, confirming they are visible
  scan_until_sep('\0', inputs, [&, this](const std::string &input) {
    if (visible.find(input) == visible.end()) {
      std::stringstream s;
      s << "Job " << job << " erroneously added input '" << input
        << "' which was not a visible file." << std::endl;
      status_get_generic_stream(STREAM_ERROR) << s.str() << std::endl;
    } else {
      bind_integer(why, imp->insert_tree, 1, INPUT);
      bind_integer(why, imp->insert_tree, 2, job);
      bind_string(why, imp->insert_tree, 3, input);
      single_step(why, imp->insert_tree, imp->debugdb);
    }
  });

  // Insert outputs
  for (const auto &output : output_set) {
    bind_integer(why, imp->insert_tree, 1, OUTPUT);
    bind_integer(why, imp->insert_tree, 2, job);
    bind_string(why, imp->insert_tree, 3, output);
    single_step(why, imp->insert_tree, imp->debugdb);
  }

  // Insert unhashed outputs
  for (const auto &unhashed_output : unhashed_outputs) {
    bind_integer(why, imp->insert_unhashed_file, 1, job);
    bind_string(why, imp->insert_unhashed_file, 2, unhashed_output);
    single_step(why, imp->insert_unhashed_file, imp->debugdb);
  }

  bind_integer(why, imp->delete_prior, 1, imp->run_id);
  bind_integer(why, imp->delete_prior, 2, job);
  single_step(why, imp->delete_prior, imp->debugdb);

  bind_integer(why, imp->delete_overlap, 1, imp->run_id);
  bind_integer(why, imp->delete_overlap, 2, job);
  single_step(why, imp->delete_overlap, imp->debugdb);

  bool fail = false;
  bind_integer(why, imp->detect_overlap, 1, job);
  while (sqlite3_step(imp->detect_overlap) == SQLITE_ROW) {
    std::stringstream s;
    s << "File output by multiple Jobs: " << rip_column(imp->detect_overlap, 0) << std::endl;
    status_get_generic_stream(STREAM_ERROR) << s.str() << std::endl;
    fail = true;
  }
  finish_stmt(why, imp->detect_overlap, imp->debugdb);

  end_txn();

  if (fail) exit(1);
}

std::vector<std::string> Database::clear_jobs() {
  const char *why = "Could not clear jobs";
  std::vector<std::string> out;

  begin_txn();

  while (sqlite3_step(imp->get_output_files) == SQLITE_ROW) {
    out.emplace_back(rip_column(imp->get_output_files, 0));
  }
  finish_stmt(why, imp->get_output_files, imp->debugdb);

  while (sqlite3_step(imp->get_unhashed_file_paths) == SQLITE_ROW) {
    out.emplace_back(rip_column(imp->get_unhashed_file_paths, 0));
  }
  finish_stmt(why, imp->get_unhashed_file_paths, imp->debugdb);

  // Now clear everything.
  single_step(why, imp->remove_all_jobs, imp->debugdb);
  single_step(why, imp->remove_output_files, imp->debugdb);

  end_txn();

  return out;
}

void Database::tag_job(long job, const std::string &uri, const std::string &content) {
  const char *why = "Could not tag a job";
  bind_integer(why, imp->tag_job, 1, job);
  bind_string(why, imp->tag_job, 2, uri);
  bind_string(why, imp->tag_job, 3, content);
  single_step(why, imp->tag_job, imp->debugdb);
}

std::vector<FileReflection> Database::get_tree(int kind, long job) {
  std::vector<FileReflection> out;
  const char *why = "Could not read job tree";
  bind_integer(why, imp->get_tree, 1, job);
  bind_integer(why, imp->get_tree, 2, kind);
  while (sqlite3_step(imp->get_tree) == SQLITE_ROW)
    out.emplace_back(rip_column(imp->get_tree, 0), rip_column(imp->get_tree, 1));
  finish_stmt(why, imp->get_tree, imp->debugdb);
  return out;
}

void Database::save_output(long job, int descriptor, const char *buffer, int size, double runtime) {
  const char *why = "Could not save job output";
  bind_integer(why, imp->insert_log, 1, job);
  bind_integer(why, imp->insert_log, 2, descriptor);
  bind_double(why, imp->insert_log, 3, runtime);
  bind_string(why, imp->insert_log, 4, buffer, size);
  single_step(why, imp->insert_log, imp->debugdb);
}

std::string Database::get_output(long job, int descriptor) const {
  std::stringstream out;
  const char *why = "Could not read job output";
  bind_integer(why, imp->get_log, 1, job);
  bind_integer(why, imp->get_log, 2, descriptor);
  while (sqlite3_step(imp->get_log) == SQLITE_ROW) {
    out.write(static_cast<const char *>(sqlite3_column_blob(imp->get_log, 0)),
              sqlite3_column_bytes(imp->get_log, 0));
  }
  finish_stmt(why, imp->get_log, imp->debugdb);
  return out.str();
}

void Database::replay_output(long job, const char *stdout, const char *stderr,
                             const char *runner_out, const char *runner_err) {
  const char *why = "Could not replay job output";
  bind_integer(why, imp->replay_log, 1, job);
  while (sqlite3_step(imp->replay_log) == SQLITE_ROW) {
    int fd = sqlite3_column_int64(imp->replay_log, 0);
    const char *str = static_cast<const char *>(sqlite3_column_blob(imp->replay_log, 1));
    int len = sqlite3_column_bytes(imp->replay_log, 1);
    if (len) {
      if (fd == 1) {
        status_get_generic_stream(stdout) << std::string(str, len);
      } else if (fd == 2) {
        status_get_generic_stream(stderr) << std::string(str, len);
      } else if (fd == 3) {
        status_get_generic_stream(runner_out) << std::string(str, len);
      } else if (fd == 4) {
        status_get_generic_stream(runner_err) << std::string(str, len);
      }
    }
  }
  finish_stmt(why, imp->replay_log, imp->debugdb);
}

void Database::add_hash(const std::string &file, const std::string &hash, long modified) {
  const char *why = "Could not insert a hash";
  begin_txn();
  bind_string(why, imp->wipe_file, 1, file);
  bind_string(why, imp->wipe_file, 2, hash);
  single_step(why, imp->wipe_file, imp->debugdb);
  bind_string(why, imp->update_file, 1, hash);
  bind_integer(why, imp->update_file, 2, modified);
  bind_string(why, imp->update_file, 3, file);
  single_step(why, imp->update_file, imp->debugdb);
  bind_string(why, imp->insert_file, 1, hash);
  bind_integer(why, imp->insert_file, 2, modified);
  bind_string(why, imp->insert_file, 3, file);
  single_step(why, imp->insert_file, imp->debugdb);
  end_txn();
}

std::string Database::get_hash(const std::string &file, long modified) {
  std::string out;
  const char *why = "Could not fetch a hash";
  bind_string(why, imp->fetch_hash, 1, file);
  bind_integer(why, imp->fetch_hash, 2, modified);
  if (sqlite3_step(imp->fetch_hash) == SQLITE_ROW) out = rip_column(imp->fetch_hash, 0);
  finish_stmt(why, imp->fetch_hash, imp->debugdb);
  return out;
}

static std::vector<std::string> chop_null(const std::string &str) {
  std::vector<std::string> out;
  const char *tok = str.c_str();
  const char *end = tok + str.size();
  for (const char *scan = tok; scan != end; ++scan) {
    if (*scan == 0 && scan != tok) {
      out.emplace_back(tok, scan - tok);
      tok = scan + 1;
    }
  }
  return out;
}

std::string Time::as_string() const {
  char buf[100];
  struct tm tm;
  time_t time = t / 1000000000;
  localtime_r(&time, &tm);
  strftime(buf, sizeof(buf) - 10, "%Y-%m-%d %H:%M:%S", &tm);
  return buf;
}

JAST JobReflection::to_simple_json() const {
  JAST json(JSON_OBJECT);
  json.add("job", job);
  json.add("label", label.c_str());

  std::stringstream commandline_stream;
  for (const std::string &line : commandline) {
    commandline_stream << line << " ";
  }
  json.add("commandline", commandline_stream.str());

  json.add("starttime", starttime.as_int64());
  json.add("endtime", endtime.as_int64());
  json.add("wake_start", wake_start.as_int64());

  std::stringstream tags_stream;
  for (const auto &tag : tags) {
    tags_stream << "{<br>"
                << "  job: " << tag.job << ",<br>"
                << "  uri: " << tag.uri << ",<br>"
                << "  content: " << tag.content << "<br>},<br>";
  }
  json.add("tags", tags_stream.str());

  return json;
}

JAST JobReflection::to_structured_json() const {
  JAST json(JSON_OBJECT);
  json.add("job", job);
  json.add("label", label);
  json.add("stale", stale);
  json.add("directory", directory);

  JAST &commandline_json = json.add("commandline", JSON_ARRAY);
  for (const std::string &line : commandline) {
    commandline_json.add("", line);
  }

  JAST &environment_json = json.add("environment", JSON_ARRAY);
  for (const std::string &line : environment) {
    environment_json.add("", line);
  }

  json.add("stack", stack);
  json.add("stdin_file", stdin_file);
  json.add("starttime", starttime.as_int64());
  json.add("endtime", endtime.as_int64());
  json.add("wake_start", wake_start.as_int64());
  json.add("wake_cmdline", wake_cmdline);

  std::string out_stream;
  std::string err_stream;
  std::string runner_out_stream;
  std::string runner_err_stream;

  for (auto &write : std_writes) {
    if (write.second == 1) {
      out_stream += write.first;
    } else if (write.second == 2) {
      err_stream += write.first;
    } else if (write.second == 3) {
      runner_out_stream += write.first;
    } else if (write.second == 4) {
      runner_err_stream += write.first;
    }
  }

  json.add("stdout", out_stream);
  json.add("stderr", err_stream);
  json.add("runner_output", runner_out_stream);
  json.add("runner_error", runner_err_stream);

  JAST &usage_json = json.add("usage", JSON_OBJECT);
  usage_json.add("status", usage.status);
  usage_json.add("runtime", usage.runtime);
  usage_json.add("cputime", usage.cputime);
  usage_json.add("membytes", usage.membytes);
  usage_json.add("ibytes", usage.ibytes);
  usage_json.add("obytes", usage.obytes);

  JAST &visible_json = json.add("visible_files", JSON_ARRAY);
  for (const auto &visible_file : visible) {
    visible_json.add("", visible_file.path);
  }

  JAST &input_json = json.add("input_files", JSON_ARRAY);
  for (const auto &input : inputs) {
    input_json.add("", input.path);
  }

  JAST &output_json = json.add("output_files", JSON_ARRAY);
  for (const auto &output : outputs) {
    output_json.add("", output.path);
  }

  JAST &tags_json = json.add("tags", JSON_ARRAY);
  for (const auto &tag : tags) {
    JAST &tag_json = tags_json.add("", JSON_OBJECT);
    tag_json.add("uri", tag.uri);
    tag_json.add("content", tag.content);
  }

  return json;
}

// TODO: Delete this and update --timeline to use to_structured_json()
JAST JobReflection::to_json() const {
  JAST json(JSON_OBJECT);
  json.add("job", job);
  json.add("label", label.c_str());
  json.add("stale", stale);
  json.add("directory", directory.c_str());

  std::stringstream commandline_stream;
  for (const std::string &line : commandline) {
    commandline_stream << line << " ";
  }
  json.add("commandline", commandline_stream.str());

  std::stringstream environment_stream;
  for (const std::string &line : environment) {
    environment_stream << line << " ";
  }
  json.add("environment", environment_stream.str());

  json.add("stack", stack.c_str());

  json.add("stdin_file", stdin_file.c_str());

  json.add("starttime", starttime.as_int64());
  json.add("endtime", endtime.as_int64());
  json.add("wake_start", wake_start.as_int64());

  json.add("wake_cmdline", wake_cmdline.c_str());

  std::string out_stream;
  std::string err_stream;
  for (auto &write : std_writes) {
    if (write.second == 1) {
      out_stream += write.first;
    }
    if (write.second == 2) {
      err_stream += write.first;
    }
  }

  json.add("stdout_payload", out_stream.c_str());
  json.add("stderr_payload", err_stream.c_str());

  std::stringstream usage_stream;
  usage_stream << "status: " << usage.status << "<br>"
               << "runtime: " << usage.runtime << "<br>"
               << "cputime: " << usage.cputime << "<br>"
               << "membytes: " << std::to_string(usage.membytes) << "<br>"
               << "ibytes: " << std::to_string(usage.ibytes) << "<br>"
               << "obytes: " << std::to_string(usage.obytes);
  json.add("usage", usage_stream.str());

  std::stringstream visible_stream;
  for (const auto &visible_file : visible) {
    visible_stream << visible_file.path << "<br>";
  }
  json.add("visible", visible_stream.str());

  std::stringstream inputs_stream;
  for (const auto &input : inputs) {
    inputs_stream << input.path << "<br>";
  }
  json.add("inputs", inputs_stream.str());

  std::stringstream outputs_stream;
  for (const auto &output : outputs) {
    outputs_stream << output.path << "<br>";
  }
  json.add("outputs", outputs_stream.str());

  std::stringstream tags_stream;
  for (const auto &tag : tags) {
    tags_stream << "{<br>"
                << "  job: " << tag.job << ",<br>"
                << "  uri: " << tag.uri << ",<br>"
                << "  content: " << tag.content << "<br>},<br>";
  }
  json.add("tags", tags_stream.str());

  return json;
}

JAST FileDependency::to_json() const {
  JAST json(JSON_OBJECT);
  json.add("writer", writer);
  json.add("reader", reader);
  return json;
}

static JobReflection find_one(const Database *db, sqlite3_stmt *query) {
  const char *why = "Could not describe job";
  JobReflection desc;
  // grab flat values
  desc.job = sqlite3_column_int64(query, 0);
  desc.label = rip_column(query, 1);
  desc.directory = rip_column(query, 2);
  desc.commandline = chop_null(rip_column(query, 3));
  desc.environment = chop_null(rip_column(query, 4));
  desc.stack = rip_column(query, 5);
  desc.stdin_file = rip_column(query, 6);
  desc.starttime = Time(sqlite3_column_int64(query, 7));
  desc.endtime = Time(sqlite3_column_int64(query, 8));
  desc.stale = sqlite3_column_int64(query, 9) != 0;
  desc.wake_start = Time(sqlite3_column_int64(query, 10));
  desc.wake_cmdline = rip_column(query, 11);
  desc.usage.status = sqlite3_column_int64(query, 12);
  desc.usage.runtime = sqlite3_column_double(query, 13);
  desc.usage.cputime = sqlite3_column_double(query, 14);
  desc.usage.membytes = sqlite3_column_int64(query, 15);
  desc.usage.ibytes = sqlite3_column_int64(query, 16);
  desc.usage.obytes = sqlite3_column_int64(query, 17);
  if (desc.stdin_file.empty()) desc.stdin_file = "/dev/null";

  desc.std_writes = db->get_interleaved_output(desc.job);

  // visible
  bind_integer(why, db->imp->get_tree, 1, desc.job);
  bind_integer(why, db->imp->get_tree, 2, VISIBLE);
  while (sqlite3_step(db->imp->get_tree) == SQLITE_ROW)
    desc.visible.emplace_back(rip_column(db->imp->get_tree, 0), rip_column(db->imp->get_tree, 1));
  finish_stmt(why, db->imp->get_tree, db->imp->debugdb);
  // tags
  bind_integer(why, db->imp->get_tags, 1, desc.job);
  while (sqlite3_step(db->imp->get_tags) == SQLITE_ROW)
    desc.tags.emplace_back(sqlite3_column_int64(db->imp->get_tags, 0),
                           rip_column(db->imp->get_tags, 1), rip_column(db->imp->get_tags, 2));
  finish_stmt(why, db->imp->get_tags, db->imp->debugdb);

  // inputs
  bind_integer(why, db->imp->get_tree, 1, desc.job);
  bind_integer(why, db->imp->get_tree, 2, INPUT);
  while (sqlite3_step(db->imp->get_tree) == SQLITE_ROW)
    desc.inputs.emplace_back(rip_column(db->imp->get_tree, 0), rip_column(db->imp->get_tree, 1));
  finish_stmt(why, db->imp->get_tree, db->imp->debugdb);

  // outputs
  bind_integer(why, db->imp->get_tree, 1, desc.job);
  bind_integer(why, db->imp->get_tree, 2, OUTPUT);
  while (sqlite3_step(db->imp->get_tree) == SQLITE_ROW)
    desc.outputs.emplace_back(rip_column(db->imp->get_tree, 0), rip_column(db->imp->get_tree, 1));
  finish_stmt(why, db->imp->get_tree, db->imp->debugdb);

  return desc;
}

static std::vector<JobReflection> find_all(const Database *db, sqlite3_stmt *query) {
  const char *why = "Could not explain file";
  std::vector<JobReflection> out;

  db->begin_txn();
  while (sqlite3_step(query) == SQLITE_ROW) out.emplace_back(find_one(db, query));
  finish_stmt(why, query, db->imp->debugdb);
  db->end_txn();

  return out;
}

std::vector<std::string> Database::get_outputs() const {
  const char *why = "Could not get outputs";
  std::vector<std::string> out;

  begin_txn();
  while (sqlite3_step(imp->get_output_files) == SQLITE_ROW) {
    out.emplace_back(rip_column(imp->get_output_files, 0));
  }
  finish_stmt(why, imp->get_output_files, imp->debugdb);
  while (sqlite3_step(imp->get_unhashed_file_paths) == SQLITE_ROW) {
    out.emplace_back(rip_column(imp->get_unhashed_file_paths, 0));
  }
  finish_stmt(why, imp->get_unhashed_file_paths, imp->debugdb);
  end_txn();

  return out;
}

static std::vector<FileDependency> get_all_file_dependencies(const Database *db,
                                                             sqlite3_stmt *query) {
  const char *why = "Could not get file dependencies";
  std::vector<FileDependency> out;

  db->begin_txn();
  while (sqlite3_step(query) == SQLITE_ROW) {
    FileDependency dep;
    dep.writer = sqlite3_column_int64(query, 0);
    dep.reader = sqlite3_column_int64(query, 1);
    out.emplace_back(dep);
  }
  finish_stmt(why, query, db->imp->debugdb);
  db->end_txn();

  return out;
}

std::string collapse_or(const std::vector<std::string> &ors) {
  if (ors.empty()) {
    return "";
  }

  if (ors.size() == 1) {
    return ors.front();
  }

  std::string out = "(";

  for (std::vector<std::string>::size_type i = 0; i < ors.size() - 1; i++) {
    out += ors[i] + " OR ";
  }

  out += ors.back() + ")";

  return out;
}

std::string collapse_and(const std::vector<std::vector<std::string>> &ands, int nest) {
  if (ands.empty()) {
    return "";
  }

  if (ands.size() == 1) {
    return collapse_or(ands.front());
  }

  std::string out = "";

  std::string indent = "";
  for (int i = 0; i < nest; i++) {
    indent += "    ";
  }
  std::string query_indent = indent + "    ";

  for (std::vector<std::vector<std::string>>::size_type i = 0; i < ands.size() - 1; i++) {
    out += collapse_or(ands[i]) + "\n" + indent + "AND\n" + query_indent;
  }

  out += collapse_or(ands.back());

  return out;
}

std::vector<JobReflection> Database::matching(
    const std::vector<std::vector<std::string>> &core_filters,
    std::vector<std::vector<std::string>> input_file_filters,
    std::vector<std::vector<std::string>> output_file_filters) {
  std::string input_file_join = "";
  if (!input_file_filters.empty()) {
    input_file_filters.push_back({"access = 1"});
    std::string conds = collapse_and(input_file_filters, 3);
    input_file_join =
        "        INNER JOIN (\n"
        "            SELECT filetree.job_id FROM filetree\n"
        "            INNER JOIN files\n"
        "            ON filetree.file_id=files.file_id\n"
        "            WHERE\n"
        "                " +
        conds + "\n" + "        ) ft_input ON core.job_id = ft_input.job_id\n";
  }

  std::string output_file_join = "";
  if (!output_file_filters.empty()) {
    output_file_filters.push_back({"access = 2"});
    std::string conds = collapse_and(output_file_filters, 3);
    output_file_join =
        "        INNER JOIN (\n"
        "            SELECT filetree.job_id FROM filetree\n"
        "            INNER JOIN files\n"
        "            ON filetree.file_id=files.file_id\n"
        "            WHERE\n"
        "                " +
        conds + "\n" + "        ) ft_output ON core.job_id = ft_output.job_id\n";
  }

  // This query creates a subtable of the following shape:
  //
  // clang-format off
  // | job_id | label | run_id | use_id | endtime | commandline | status | runtime |       tags       |
  // --------------------------------------------------------------------------------------------------
  // |    1   |  foo  |   1    |    1   |  1234   | ls lah .    |   0    |   2.8   | <d>a=b<d>c=d<d>  |
  // |    2   |  bar  |   1    |    1   |  0000   | cat f.txt   |   0    |   0.0   |      null        |
  // clang-format on
  //
  // The subtable is constructed by joining the jobs table with the minimal set of other dependent
  // tables with the following extra processing excluding input_files and output_files which are
  // too expensive to include.
  // 1. tags are flattened from two columns (uri, content) to one column (tags) with a = separator
  // 2. tags are group_concat'd into a single row per job. <d> is
  //    used as a deliminator between each value. The deliminator is also placed at the beginning
  //    and end of each row so that queries don't need to special case the first/last entry.
  //
  // Any inspection flag/user code may add any WHERE expression conditions to the main query using
  // the columns of the subtable for fine grain filters.
  //
  // For example, the query below will return all jobs that exited with status code 0 and where
  // tagged with key = foo, value = var
  //   SELECT job_id FROM **SUBTABLE**
  //   WHERE status = 0 AND tags like '%<d>foo=bar<d>%'
  std::string core_table = R"delim(        (
            SELECT
                j.job_id,
                j.label,
                j.run_id,
                j.use_id,
                j.endtime,
                j.commandline,
                s.status,
                s.runtime,
                '<d>' || group_concat(t.tag, '<d>') || '<d>' tags
            FROM jobs j
            LEFT JOIN (
                SELECT stat_id, status, runtime FROM stats
            ) s
            ON j.stat_id=s.stat_id
            LEFT JOIN (
                SELECT job_id, uri || '=' || content tag FROM tags
            ) t
            ON j.job_id = t.job_id
            GROUP BY
                j.job_id
        ) core
)delim";

  std::string subtable = core_table + input_file_join + output_file_join;

  // This query wraps the subtable, applies the requested filters, and returns the matching jobs
  std::string id_query =
      "    SELECT core.job_id\n"
      "    FROM\n"
      "    (\n" +
      subtable + "    )";

  if (!core_filters.empty()) {
    id_query +=
        "\n"
        "    WHERE\n"
        "        " +
        collapse_and(core_filters, 1);
  }

  // Adapts the id_query to match the columns needed to create a JobReflection
  std::string query =
      "SELECT j.job_id, j.label, j.directory, j.commandline, j.environment, j.stack, j.stdin, "
      "j.starttime, j.endtime, j.stale, r.time, r.cmdline, s.status, s.runtime, s.cputime, "
      "s.membytes, s.ibytes, s.obytes\n"
      "FROM jobs j\n"
      "LEFT JOIN stats s\n"
      "ON j.stat_id=s.stat_id\n"
      "LEFT JOIN runs r\n"
      "ON j.run_id=r.run_id\n"
      "WHERE j.job_id IN (\n" +
      id_query +
      "\n)\n"
      "ORDER BY j.job_id";

  sqlite3_stmt *stmt;
  if (sqlite3_prepare_v2(imp->db, query.c_str(), -1, &stmt, 0) != SQLITE_OK) {
    std::string out = std::string("sqlite3_prepare_v2 (dyn matching): ") + sqlite3_errmsg(imp->db);
    std::cerr << out << std::endl;
    sqlite3_finalize(stmt);
    return {};
  }

  auto out = find_all(this, stmt);

  sqlite3_finalize(stmt);
  return out;
}

void Database::set_runner_status(long job_id, int status) {
  const char *why = "Could not set runner status";
  bind_integer(why, imp->set_runner_status, 1, status);
  bind_integer(why, imp->set_runner_status, 2, job_id);
  single_step(why, imp->set_runner_status, imp->debugdb);
}

int Database::get_runner_status(long job_id) {
  int status = 0;
  const char *why = "Could not get runner status";
  bind_integer(why, imp->get_runner_status, 1, job_id);
  if (sqlite3_step(imp->get_runner_status) == SQLITE_ROW) {
    status = sqlite3_column_int(imp->get_runner_status, 0);
  }
  finish_stmt(why, imp->get_runner_status, imp->debugdb);
  return status;
}

std::vector<JobEdge> Database::get_edges() {
  std::vector<JobEdge> out;
  while (sqlite3_step(imp->get_edges) == SQLITE_ROW) {
    out.emplace_back(sqlite3_column_int64(imp->get_edges, 0),
                     sqlite3_column_int64(imp->get_edges, 1));
  }
  finish_stmt("Could not retrieve edges", imp->get_edges, imp->debugdb);
  return out;
}

std::vector<JobTag> Database::get_tags() {
  std::vector<JobTag> out;
  while (sqlite3_step(imp->get_all_tags) == SQLITE_ROW) {
    out.emplace_back(sqlite3_column_int64(imp->get_all_tags, 0), rip_column(imp->get_all_tags, 1),
                     rip_column(imp->get_all_tags, 2));
  }
  finish_stmt("Could not retrieve tags", imp->get_all_tags, imp->debugdb);
  return out;
}

std::vector<RunReflection> Database::get_runs() const {
  std::vector<RunReflection> out;
  begin_txn();
  while (sqlite3_step(imp->get_all_runs) == SQLITE_ROW) {
    RunReflection run;
    run.id = sqlite3_column_int(imp->get_all_runs, 0);
    run.time = Time(sqlite3_column_int64(imp->get_all_runs, 1));
    run.cmdline = rip_column(imp->get_all_runs, 2);
    out.emplace_back(run);
  }
  finish_stmt("Could not retrieve runs", imp->get_all_runs, imp->debugdb);
  end_txn();
  return out;
}

std::vector<std::pair<std::string, int>> Database::get_interleaved_output(long job_id) const {
  std::vector<std::pair<std::string, int>> out;

  const char *why = "Could not bind args";
  bind_integer(why, imp->get_interleaved_output, 1, job_id);
  while (sqlite3_step(imp->get_interleaved_output) == SQLITE_ROW) {
    out.emplace_back(rip_column(imp->get_interleaved_output, 0),
                     sqlite3_column_int(imp->get_interleaved_output, 1));
  }
  finish_stmt(why, imp->get_interleaved_output, imp->debugdb);

  return out;
}

std::vector<FileDependency> Database::get_file_dependencies() const {
  return get_all_file_dependencies(this, imp->get_file_dependency);
}
