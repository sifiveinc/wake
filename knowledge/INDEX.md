# Wake Knowledge Base — Index

Curated knowledge for coding agents and autonomous agents working in the Wake repository. Each file is self-contained: load just the topic you need. File-path citations point back into the live repo so you can drill down to source.

This base is **summarized and indexed**, not exhaustive. The authoritative material remains in `share/doc/wake/**` and the source files. When in doubt, this base tells you *where* to look.

| File | Read this when you need to … |
|---|---|
| [00-overview.md](00-overview.md) | Understand what Wake *is*, why it exists, the glossary of terms (Plan, Job, Runner, target, publish/subscribe, CAS, FUSE, RSC, …). |
| [01-repo-map.md](01-repo-map.md) | Locate a subsystem. Annotated tour of every top-level directory and a "where do I look" lookup table. |
| [02-language.md](02-language.md) | Write or read Wake source. Syntax, types, pattern matching, packages, `target`, publish/subscribe, idioms, gotchas. |
| [03-stdlib.md](03-stdlib.md) | Find the right stdlib function. The `share/wake/lib/` subpackages (`core`, `system`, `gcc_wake`, `rust_wake`) cataloged file-by-file with key functions called out. |
| [04-build-model.md](04-build-model.md) | Reason about a build. End-to-end Source → Plan → Job → Cache, runners, sandboxing, determinism rules. |
| [05-runtime-internals.md](05-runtime-internals.md) | Touch the C++. Pipeline (parser → DST → types → optimizer → runtime), per-subsystem file map, where to start common changes. |
| [06-tooling.md](06-tooling.md) | Use or modify a binary. `wake`, `wake-format`, `wake-unit`, `wake-migrate`, `wakebox`, `fuse-waked`, `shim-wake`, `wake-hash`, `lsp-wake`, `bsp-wake`, RSC. |
| [07-testing.md](07-testing.md) | Run or add tests. Layout, `pass.sh`/`fail.sh`/`stdout` golden-file convention, how to add a regression or unit test. |
| [08-build-and-bootstrap.md](08-build-and-bootstrap.md) | Build the project. Bootstrap Makefile → self-hosted `build.wake` → variants, install, tarball, format, CI. |
| [09-cli-and-config.md](09-cli-and-config.md) | Drive `wake` from the command line. Flags, DB queries, log levels and routing, `.wakeroot` / `wake.json` / `.wakemanifest` / `.wakeignore`. |
| [10-cookbook.md](10-cookbook.md) | Get a concrete recipe. Add stdlib fn, write a test, query the DB, define a runner, debug a job, add a variant, format, common patterns. |

## Suggested reading order for a new agent

1. **00-overview** — orientation.
2. **01-repo-map** — geography.
3. **02-language** *if* you'll touch `.wake`.
4. **04-build-model** — the central mental model; almost every task touches it.
5. Skim **03-stdlib** for what's available, **09-cli-and-config** for daily operation.
6. Reach for **05/06/07/08/10** as the task demands.

## Pointers to authoritative docs

This base summarizes; for primary sources see:
- `README.md` — pitch + install
- `share/doc/wake/quickref.md` (also reachable as `docs/quickref.md`) — language cheat sheet
- `share/doc/wake/tutorial.md` — language tour with build examples
- `share/doc/wake/basic_wake_tutorial.md` — beginner-level intro
- `share/doc/wake/logging.md` — log routing & loggers
- `share/doc/wake/datastructures.txt` — stdlib data-structure complexities
- `share/doc/wake/tour/{packages,targets,tuples}.adoc` — feature tours
- `share/doc/wake/wake-for-fsharp-developers.md`, `share/doc/wake/wake-for-scala-developers.adoc` — comparison guides
- `share/doc/wake/how-to/{create-a-release,test-the-vscode-extension}.adoc` — operational how-tos
- `tests/README.md` — test conventions

## Notes on freshness

- KB authored against `master` at `66c3aa1f` ("Set release date.") — Wake v49.x line.
- The `docs/` directory at the repo root is a symlink to `share/doc/wake/`; this base cites the canonical `share/doc/wake/...` paths.
- "inherited-porcupine" appears in the plan filename but is not part of this repo; the user confirmed it is out of scope.
- If you change architecture, schemas, CLI flags, or stdlib APIs, update the relevant topic file here. The base is small enough that drift is easy to fix.
