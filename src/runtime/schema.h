#ifndef WAKE_SCHEMA_H
#define WAKE_SCHEMA_H

#define SCHEMA_VERSION "10"

// Increment the SCHEMA_VERSION every time the below string changes.
// Also add migrations to the wake-migration tool if needed.
// Version 10: Changed hash algorithm from BLAKE2b to BLAKE3.
inline const char* getWakeSchemaSQL() {
  return "pragma auto_vacuum=incremental;"
         "pragma journal_mode=wal;"
         "pragma synchronous=0;"
         "pragma locking_mode=normal;"
         "pragma busy_timeout=30000;"
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
         "  stale       integer not null default 0,"  // 0=false, 1=true
         "  is_atty     integer not null default 0,"  // 0=false, 1=true
         "  runner_status text);"  // NULL=success, non-null string=failure message
         "create index if not exists job on jobs(directory, commandline, environment, stdin, "
         "signature, keep, job_id, stat_id);"
         "create index if not exists runner_status_idx on jobs(runner_status) WHERE runner_status "
         "IS NOT NULL;"
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
}

#define WAKE_SCHEMA_SQL getWakeSchemaSQL()

#endif
