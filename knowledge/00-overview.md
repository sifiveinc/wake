# Wake — Overview & Glossary

## What Wake is

Wake is **two things in one**: a **functional programming language** and a **build orchestration system**. You write build rules in the language; the runtime executes them as parallelized jobs against a persistent SQLite database, with optional FUSE-based sandboxing and content-addressable caching.

- Repo: `git@github.com:sifiveinc/wake.git`
- License: Apache-2.0 (with vendored components under various permissive licenses; see `LICENSE`).
- Self-hosted: a tiny C++ bootstrap (`Makefile`) builds `bin/wake`, then `wake` builds itself via `build.wake`.
- Binary surface: `wake`, `wake-format`, `wake-unit`, `wake-migrate`, `wakebox`, `fuse-waked`, `shim-wake`, `wake-hash`, `lsp-wake`, `bsp-wake` (see `tools/`).

## Why Wake exists (vs. Make / Bazel / Tup)

Classical build systems require all jobs to be declarable up-front. Wake is built around **dependent job execution**: the set of jobs to run can depend on outputs of previous jobs in the same build. Concrete consequences:

- **Outputs rarely need to be declared.** Wake observes which files a job actually wrote (via FUSE) and records them in the database.
- **Under-specifying inputs fails the build.** Reads outside the declared input set are detected and rejected.
- **Over-specifying inputs is auto-pruned.** Wake records only the inputs the job *actually* read.
- **Builds are introspectable.** Every job, its inputs, outputs, stdout/stderr, exit status, and timing are persisted to `wake.db` (SQLite). Query with `wake -o file`, `wake -i file`, `wake --last`, `wake --failed`, `wake --job <id>`.
- **Implicit parallelism.** The functional core has no shared mutable state; the runtime extracts parallelism from data dependencies automatically.
- **Shared caching.** Content-addressed local cache plus an optional Remote Shared Cache (RSC, in `rust/rsc/`, PostgreSQL-backed) lets teammates reuse each other's build artifacts.

## Glossary

These terms appear constantly in source and docs.

| Term | Meaning |
|------|---------|
| **Plan** | A description of a job to run (label, command, inputs, environment, resources). Built via `makePlan` and `editPlan*` editors in `share/wake/lib/system/plan.wake`. |
| **Job** | The result of executing a Plan via `runJobWith`. Carries stdout, stderr, exit status, observed inputs, observed outputs. See `share/wake/lib/system/job.wake`. |
| **Result** | `Pass a` or `Fail Error`. Wake's pervasive success/failure type. See `share/wake/lib/core/result.wake`. |
| **Option** | `Some a` or `None`. See `share/wake/lib/core/option.wake`. |
| **Path** | Opaque handle to a file (source or built). No public constructor — created via `source`, `sources`, or as a job output. |
| **Runner** | Function that executes a Plan. Built-ins: `localRunner` (no sandbox, in-workspace), `defaultRunner` (FUSE-sandboxed), `virtualRunner` / `workspaceRunner` (used by `sources` / virtual outputs). Invoked via `runJobWith runner plan`. Custom runners via `makeRunner`. See `share/wake/lib/system/runner.wake` and `share/wake/lib/system/job.wake`. |
| **target** | A keyword; turns a `def` into a memoized function (cache by argument). Critical for build correctness when the same intermediate is referenced from multiple sites. See `share/doc/wake/tour/targets.adoc`. |
| **publish / subscribe / topic** | Workspace-wide append-only channels. Files `publish` to a `topic`; any function can `subscribe` and read the accumulated list. Used to register runners, tests, environment packages. See `share/doc/wake/tutorial.md`. |
| **package** | Namespace for identifiers (one per file). Declared with `package name`; controlled with `from pkg import …` / `from pkg export …`. See `share/doc/wake/tour/packages.adoc`. |
| **tuple** | Named product type with auto-generated `getX*`, `setX*`, `editX*` accessors. See `share/doc/wake/tour/tuples.adoc`. |
| **data** | Sum / discriminated-union type. Constructors are pattern-matchable. |
| **require** | Early-return on `Result`/`Option`/data constructor: `require Pass x = result` (`else …` optional). Like F#'s `let!` or Rust's `?`. |
| **CAS** | Content-Addressable Store. BLAKE2-hashed artifacts. See `share/wake/lib/system/cas.wake` and `src/cas/`. |
| **WakeFS** | FUSE filesystem that observes a job's reads/writes for sandboxing. See `src/wakefs/` and the `fuse-waked` daemon in `tools/fuse-waked/`. |
| **wakebox** | Lightweight execution sandbox (chroot/namespaces) used by jobs. See `tools/wakebox/`. |
| **RSC** | Remote Shared Cache. HTTP service backed by PostgreSQL; lives in `rust/rsc/`. |
| **fnref** | Compiler-internal: a reference to a Wake function used in continuations. Appears in `src/runtime/`. |
| **DST** | Desugared Syntax Tree. Compiler IR after lowering CST. See `src/dst/`. |
| **CST** | Concrete Syntax Tree. Parser output. See `src/parser/`. |
| **SSA** | Static Single Assignment IR used by the optimizer. See `src/optimizer/ssa.cpp`. |
| **Variant** | A build flavor (`default`, `static`, `debug`, `wasm`). Defined in `build.wake`. |
| **wake.db** | SQLite database at the workspace root holding job history. |
| **`.wakeroot`** | Marker file for a Wake workspace; can carry version constraint and config pointer. |
| **`.wakemanifest`** | Auto-generated list of `.wake` source files (used in tarball release). |
| **`.wakeignore`** | Glob patterns to exclude `.wake` files from compilation. |

## Versioning

Latest line: v49.x. Master HEAD at the time this knowledge base was built: `66c3aa1f` ("Set release date."). See `debian/changelog.in` for full history.

## Where to read next

- For language syntax → `knowledge/02-language.md`
- For where files live → `knowledge/01-repo-map.md`
- For Plan/Job/Runner/cache mental model → `knowledge/04-build-model.md`
- For the C++ pipeline → `knowledge/05-runtime-internals.md`
