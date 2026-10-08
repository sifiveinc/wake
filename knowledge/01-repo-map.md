# Repo Map

A tour of the top-level layout. Most files an agent touches live under `src/`, `share/wake/lib/`, `tools/`, `tests/`, or `build.wake`.

```
wake/
├── README.md              Project pitch, install, configuration overview.
├── LICENSE  LICENSE.Apache2  License headers.
├── Makefile               Bootstrap: builds bin/wake from C++ before any Wake code runs.
├── build.wake             Self-hosted main build. Defines variants and all artifacts.
├── wake.spec.in           RPM packaging template.
├── .wakeroot              Workspace marker (empty here; can pin version / config path).
│
├── src/                   C++ compiler + runtime. Self-contained; no Wake here.
│   ├── parser/            Lexer (re2c) + Lemon grammar → CST.
│   ├── dst/               Desugar/bind: CST → DST (the IR the typechecker sees).
│   ├── types/             Hindley–Milner inference, ADT/sum/tuple support, DSU unification.
│   ├── optimizer/         SSA, inlining, CSE, dead-code, purity, scope analysis.
│   ├── runtime/           Heap/GC, value rep, job scheduler, prims, SQLite database.
│   ├── cas/               Content-addressed store (BLAKE2 / SipHash).
│   ├── wakefs/            FUSE-backed observation layer (sandboxed input/output tracking).
│   ├── json/              JSON5 parser used for config/IPC.
│   ├── wcl/               Path/file/document utilities (used widely incl. LSP, formatter).
│   ├── util/              shell/term/tracing/diagnostic helpers.
│   ├── compat/            Cross-platform shims (spawn, mtime, rusage, sigwinch, physmem).
│   └── src.wake           Build rules that wire all of the above into binaries.
│
├── share/                 Files installed alongside the binary.
│   ├── wake/lib/          The Wake standard library.
│   │   ├── core/          Pure-functional core: list/map/tree/vector, string, regex, json,
│   │   │                  option, result, tuple, integer, double, boolean, order, syntax,
│   │   │                  print, types, unsafe.
│   │   ├── system/        Build-system surface: plan, job, runner, sources, path, io, http,
│   │   │                  time, cas, environment, staging_outputs, remote_cache_*.
│   │   ├── gcc_wake/      C/C++ rules: gcc.wake, pkgconfig.wake.
│   │   ├── rust_wake/     Cargo/Rust rules: cargo.wake.
│   │   └── nothing/       Stub package (no-op runner / placeholder for testing).
│   └── doc/wake/          User-facing documentation (also reachable as ./docs symlink).
│       ├── README.md (none) — see tutorial.md, basic_wake_tutorial.md, quickref.md, logging.md.
│       ├── datastructures.txt   Big-O cheat sheet for stdlib collections.
│       ├── tour/         packages.adoc, targets.adoc, tuples.adoc.
│       ├── how-to/       create-a-release.adoc, test-the-vscode-extension.adoc.
│       ├── syntax/       editor syntax files (emacs, joe, vim).
│       ├── wake-for-fsharp-developers.md
│       └── wake-for-scala-developers.adoc
│
├── docs/                  → symlink to share/doc/wake/.
│
├── tools/                 Wake-side build rules + sources for installable binaries.
│   ├── wake/              Main CLI: main.cpp, cli_options.h, describe.{h,cpp}, markup.{h,cpp}, wake.wake.
│   ├── wake-format/       AST-aware formatter for .wake files.
│   ├── wake-unit/         C++ unit-test runner.
│   ├── wake-migrate/      wake.db schema migration tool.
│   ├── wakebox/           Job execution sandbox (namespaces / chroot-ish).
│   ├── fuse-waked/        FUSE daemon backing WakeFS for sandboxed jobs.
│   ├── shim-wake/         Job exec wrapper (env setup, resource limits).
│   ├── wake-hash/         Content-hash CLI (CAS utility).
│   ├── lsp-wake/          Language Server Protocol backend.
│   ├── bsp-wake/          Build Server Protocol backend.
│   └── tools.wake         Build rules tying all the above together.
│
├── tests/                 Black-box regression tests. See knowledge/07-testing.md.
│   ├── README.md          Test conventions and how to run.
│   ├── tests.wake         Test harness: publishes runTests / runUnitTests entry points.
│   ├── command-line/  config/  dst/  inspection/  parser/  pending/
│   ├── runtime/  standard-library/  type-system/  remote-cache/
│   └── wake-format/  wake-migrate/  wake-unit/  wakebox/
│
├── rust/                  Rust components (built with cargo).
│   ├── rsc/               Remote Shared Cache HTTP server + rsc_tool CLI.
│   ├── entity/            SeaORM entities for the cache DB.
│   ├── migration/         SeaORM migrations.
│   └── log_viewer/        Web UI for build logs.
│
├── extensions/vscode/     VSCode extension: TextMate grammar, language config, LSP client.
├── debian/                Debian packaging (changelog.in, control, rules).
├── scripts/               CI helpers, format checkers.
├── vendor/                Vendored deps (BLAKE2, SipHash, gopt, lemon, whereami, utf8proc).
│                          External system deps (sqlite3, gmp, re2, libfuse, ncurses) are *not*
│                          vendored — the system installs provide them.
├── lib/wake/              Install staging area (populated by build, not source-controlled state).
└── .github/workflows/     CI: build.yaml runs format checks, tests, unit tests, RSC tests.
```

## Quick "where do I look" map

| If you want to … | Open … |
|---|---|
| Add a stdlib function | `share/wake/lib/core/<file>.wake` or `share/wake/lib/system/<file>.wake` |
| Change the language grammar | `src/parser/parser.y.m4` (regenerate via `make`), then types in `src/types/` |
| Change the type checker | `src/types/type.{h,cpp}`, `src/types/datatype.{h,cpp}` |
| Change job scheduling / parallelism | `src/runtime/job.{h,cpp}`, `src/runtime/runtime.{h,cpp}` |
| Change FUSE sandbox behavior | `src/wakefs/`, `tools/fuse-waked/` |
| Add a CLI flag | `tools/wake/cli_options.h`, `tools/wake/main.cpp` |
| Add a regression test | `tests/<category>/<name>/` with `pass.sh` or `fail.sh` |
| Add a build variant | `build.wake` |
| Touch the formatter | `tools/wake-format/` |
| Touch IDE integration | `tools/lsp-wake/`, `extensions/vscode/` |
| Touch shared remote cache | `rust/rsc/`, `rust/entity/`, `rust/migration/` |
