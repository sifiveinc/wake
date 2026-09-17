# The Build Model: Sources → Plan → Job → Cache

This is the mental model every Wake build rule operates within.

## 1. Sources

Files that exist before any job runs. They are tracked (typically via the workspace's git index) and exposed to Wake code via:

```wake
require Pass file  = source "path/to/file.txt"      # one file
require Pass files = sources @here `.*\.cpp`        # regex, recursive from dir
```

`source` and `sources` (in `share/wake/lib/system/sources.wake`) return `Result Path Error` and `Result (List Path) Error`. Generated/built files are **never** returned — `Path`s exist only via these helpers or as observed job outputs.

`@here` is a compiler intrinsic: the absolute directory of the current `.wake` file.

## 2. Plan

A `Plan` describes a job to be run. Build one with `makePlan`, then customize via `editPlan*` helpers (in `share/wake/lib/system/plan.wake`):

```wake
require Pass src = source "main.cpp"

def plan =
    makePlan "compile main"                                     # label
        (src, Nil)                                              # Visible
        "g++ -O2 -c {src.getPathName} -o main.o"                # shell script
    | editPlanEnvironment (\_ ("PATH=/usr/bin", "LANG=C", Nil))
    | editPlanResources   (\_ ("python/3.7", Nil))
    | setPlanStdout       logInfo
    | setPlanStderr       logWarning
```

`makePlan` takes a String (run via `dash -c`). For an argv-style invocation use `makeExecPlan (cmd: List String) visible` instead.

Plan is a tuple (`share/wake/lib/system/plan.wake`); each field has auto-generated `get` / `set` / `edit` accessors. Notable fields:

| Field | Accessor examples | Purpose |
|---|---|---|
| `Label` | `getPlanLabel`, `editPlanLabel` | Human-readable identifier in logs / DB. |
| `Command` | `getPlanCommand`, `editPlanCommand` | Argv list (first element is the executable). |
| `Visible` | `getPlanVisible`, `editPlanVisible` | Files the job *may* read. Reads outside this set fail under the sandbox. |
| `Environment` | `editPlanEnvironment`, `setPlanEnvVar` | Env vars the job sees. |
| `Directory` | `editPlanDirectory` | Working directory. |
| `Resources` | `editPlanResources` | Tags consumed by runner scoring. |
| `Stdout` / `Stderr` | `setPlanStdout`, `setPlanStderr` | Which `LogLevel` job output is routed to. |
| `Echo` | `setPlanEcho` | Logger to which the command line itself prints. |
| `Persistence` | `editPlanPersistence`, `setPlanShare`, `setPlanKeep`, `setPlanOnce` | `ReRun` / `Once` / `Keep` / `Share` — controls dedup and remote-cache eligibility. |
| `FnInputs` / `FnOutputs` | `editPlanFnInputs`, `editPlanFnOutputs`, `setPlanFilterOutputs` | Hooks to refine the runner-reported input/output lists. Advanced. |
| `IsAtty` | `setPlanIsAtty` | Run inside a pseudo-tty. |

## 3. Runner

A `Runner` (in `share/wake/lib/system/runner.wake`, applied via `runJobWith` in `share/wake/lib/system/job.wake`) executes a Plan. Built-ins:

- **`defaultRunner`** — runs inside the WakeFS FUSE sandbox. Automatically detects which files were read (refining `inputs`) and which were written (populating `outputs`). Most user-facing rules use this — see `share/wake/lib/gcc_wake/gcc.wake` for examples.
- **`localRunner`** — runs the command directly in the workspace, no FUSE, no automatic input/output detection. Faster, but you take responsibility for declaring inputs/outputs.
- **`virtualRunner` / `workspaceRunner`** — internal-ish runners used by `share/wake/lib/system/sources.wake` to register source files and synthesize "virtual" jobs that publish content without executing a command.

Runners are invoked explicitly: `plan | runJobWith defaultRunner`. There is no implicit "pick a runner" function — the caller decides. Older docs (notably `share/doc/wake/quickref.md`) describe a `score`/`RunnerFilter`/published-runner selection algorithm; in current `share/wake/lib/system/runner.wake` that machinery has been simplified — environment packages and dispatchers can still consult `subscribe runner` and pick from the list, but it's not built into `runJobWith`.

Custom runner skeleton (current API, `share/wake/lib/system/runner.wake:145`):

```wake
export def myRunner: Runner =
    def run (job: Job) (input: RunnerInput): RunnerOutput =
        # see localRunner (same file) as a reference implementation
        ...
    makeRunner "my-runner" run
```

For wrapping a JSON-described external runner (the common case for environment packages), use `makeJSONRunner` / `makeJSONRunnerPlan` instead of writing `run` by hand.

## 4. Job

```wake
def job = plan | runJobWith defaultRunner
# or with a custom runner: runJobWith myRunner plan
```

The `Job` value carries everything observed from the execution. Key accessors (in `share/wake/lib/system/job.wake`):

| Accessor | Meaning |
|---|---|
| `getJobStdout job` | `Result String Error` — captured stdout |
| `getJobStderr job` | `Result String Error` — captured stderr |
| `getJobStatus job` | `Status` — `Exited n`, `Signaled n`, or `Aborted err` |
| `getJobOutputs job` | `Result (List Path) Error` — files the job wrote |
| `getJobInputs job` | `Result (List Path) Error` — files the job actually read |
| `getJobReport job` | `Result Usage Error` — resource accounting (cputime, mem, exit code, etc.) |
| `getJobId job` | `Integer` — DB id; pair with `wake --job <id>` |
| `getJobDescription job` | `String` — the plan label |
| `unsafe_getJobRunnerOutput job` | runner-specific stream from fd 3 |
| `unsafe_getJobRunnerError job` | runner-specific stream from fd 4 |

## 5. Caching layers

**Local cache (in-workspace)**:
- Every job execution is recorded in `wake.db` (SQLite, at workspace root).
- Re-running with identical inputs returns the cached job (no re-execution).
- Persistent across `wake` invocations.

**Content-addressed store (CAS)**:
- File contents are stored by BLAKE2 hash (`share/wake/lib/system/cas.wake`, `src/cas/`).
- Identical content from different builds is deduplicated.

**Remote Shared Cache (RSC)**:
- Optional. HTTP server in `rust/rsc/`, PostgreSQL backend.
- Use the `remote_cache_runner` (`share/wake/lib/system/remote_cache_runner.wake`) to consult it before executing.
- Workflow: pre-job → look up cache key → if miss, execute → push result.
- Configured via env vars / runner filter.

**Cache size limits** (config keys in `.wakeroot` or `~/.config/wake.json`):
- `max_cache_size` — collection threshold (default 25 GB).
- `low_cache_size` — collection target (default 15 GB).
- `cache_miss_on_failure` — soft-fail rather than abort on cache server errors.

## 6. Determinism rules

- **Under-spec fails**: a job that reads a file outside its declared `inputs` (under the sandbox runner) is killed.
- **Over-spec is auto-pruned**: declared inputs the job didn't actually read are dropped from the database record. So you can list a generous superset (e.g., a directory of headers) and Wake records only what mattered.
- **Outputs are observed**: with `defaultRunner`, you don't list outputs — Wake notices what the job wrote.
- **No hidden time/PID/randomness leakage**: jobs run with normalized environment (sandbox runner), stable working directory, etc. If you need nondeterministic input, declare it as an input source so its hash is part of the cache key.

## 7. Database queries

`wake.db` is queryable directly via the CLI. See `knowledge/09-cli-and-config.md` for the full list. Most useful:

```bash
wake --last           # recent jobs
wake --failed         # failed jobs only
wake -o path/to/file  # which job produced this file?
wake -i path/to/file  # which jobs read this file?
wake --job 1234       # full record for job id 1234
wake -v --last        # include captured stdout/stderr
wake --metadata --last    # metadata only (skip outputs)
```

The schema lives in `src/runtime/database.{h,cpp}`. `wake-migrate` (in `tools/wake-migrate/`) handles schema upgrades.

## 8. Sandboxing

The default execution sandbox is implemented by:
- `src/wakefs/` — the FUSE layer that observes reads/writes.
- `tools/fuse-waked/` — long-lived daemon that drives WakeFS.
- `tools/wakebox/` — a separate, lighter sandbox tool (namespaces / chroot-style isolation) usable from the command line and by jobs.

A typical sandboxed execution: `wake` → spawns `shim-wake` → invokes the command inside a WakeFS-backed mount → all FS calls flow through `fuse-waked` for tracking → on exit, observed I/O is rolled into the database record.
