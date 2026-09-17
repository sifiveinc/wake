# AGENTS.md

Onboarding for human and autonomous agents working in this repository. The full knowledge base lives at [`knowledge/INDEX.md`](knowledge/INDEX.md) — start there when you need depth. This file is the **fast on-ramp**.

## What this repo is

**Wake** — a build orchestration tool combined with a functional programming language. You write build rules in `.wake` files; the runtime executes them as parallelized jobs against a SQLite database (`wake.db`), with FUSE-based sandboxing and content-addressable caching. Self-hosted: a tiny C++ Makefile builds `bin/wake`, which then builds itself via `build.wake`.

For an extended overview see [`knowledge/00-overview.md`](knowledge/00-overview.md).

## Read first

| You're about to … | Open this |
|---|---|
| Get oriented quickly | [`knowledge/INDEX.md`](knowledge/INDEX.md) |
| Locate a subsystem | [`knowledge/01-repo-map.md`](knowledge/01-repo-map.md) |
| Write or read `.wake` source | [`knowledge/02-language.md`](knowledge/02-language.md) |
| Understand jobs, runners, caching | [`knowledge/04-build-model.md`](knowledge/04-build-model.md) |
| Touch the C++ compiler/runtime | [`knowledge/05-runtime-internals.md`](knowledge/05-runtime-internals.md) |
| Run or add tests | [`knowledge/07-testing.md`](knowledge/07-testing.md) |
| Drive `wake` from the CLI | [`knowledge/09-cli-and-config.md`](knowledge/09-cli-and-config.md) |
| Look up a recipe | [`knowledge/10-cookbook.md`](knowledge/10-cookbook.md) |

User-facing tutorials and references live in [`share/doc/wake/`](share/doc/wake/) (also reachable as `./docs/`).

## Repo geography (one-line version)

```
src/                C++ compiler + runtime (parser, types, optimizer, runtime, cas, wakefs)
share/wake/lib/     Wake standard library (core/, system/, gcc_wake/, rust_wake/)
share/doc/wake/     User-facing docs (tutorial, quickref, tour, how-to)
tools/              Installable binaries (wake, wake-format, lsp-wake, wakebox, fuse-waked, …)
tests/              Black-box regression tests (pass.sh / fail.sh + golden files)
rust/               Rust components (RSC remote cache server, log viewer)
extensions/vscode/  VSCode extension
build.wake          Self-hosted main build (defines variants)
Makefile            Bootstrap build (compiles bin/wake from C++)
```

Each major subdirectory has its own `AGENTS.md` with subsystem-specific guidance.

## Commands you'll reach for

```bash
make                        # Bootstrap + self-hosted build
make test                   # Integration tests (tests/)
make unittest               # C++ unit tests
make remoteCacheTests       # RSC end-to-end (needs PostgreSQL)
make format                 # Run clang-format + wake-format on changed files
make formatAll              # Same, full repo
make tarball                # Versioned source tarball

./bin/wake build default    # Equivalent to bare `make` after bootstrap
./bin/wake --last           # Recent jobs
./bin/wake --failed         # Failed jobs
./bin/wake -o path/file     # Which job produced this file?
./bin/wake -i path/file     # Which jobs read this file?
./bin/wake -x 'expr'        # Evaluate a Wake expression
./bin/wake -xv 'name'       # Same + show inferred type
```

CI runs `make tarball`, `make test`, `make unittest`, and `make remoteCacheTests` against PostgreSQL on Ubuntu 22.04 (see `.github/workflows/build.yaml`).

## Conventions

- **Format before commit.** CI rejects unformatted diffs. Run `make format`.
- **Add tests for behavior changes.** Most are golden-file shell tests under `tests/<category>/<name>/` with `.wakeroot` + `pass.sh` (or `fail.sh`) + optional `stdout`/`stderr`. See [`tests/AGENTS.md`](tests/AGENTS.md) and [`tests/README.md`](tests/README.md).
- **Don't `--no-verify` past hooks** unless explicitly asked. Hooks run `clang-format` and `wake-format`.
- **Don't manually amend published commits.** Always create a new commit.
- **Don't use `global def`** for new APIs in `.wake` code — use packages with `export`. `global` is the legacy escape hatch.
- **Don't reach for `unsafe_*` primitives** unless you're working on the runtime itself. They bypass purity guarantees and exist for stdlib authors.
- **Job outputs come from observation, not declaration.** Under `defaultRunner`, you list inputs (visible files); the sandbox records what the job actually wrote. Don't try to pre-declare outputs.
- **`print`/`println` is not persisted.** It writes to `logReport` and won't show up in `wake --last`. Use job stdout/stderr (or set the plan's `Stdout`/`Stderr` log levels) when you need persistence.

## What to avoid

- Don't introduce features beyond the task. Wake's stdlib is tightly curated; review existing functions in `share/wake/lib/core/` and `share/wake/lib/system/` before adding.
- Don't edit anything in `vendor/` unless updating a vendored dependency intentionally — it's third-party code.
- Don't remove tests from `tests/pending/` without understanding why they're parked.
- Don't change the `wake.db` schema without adding a migration in `tools/wake-migrate/`.
- Don't commit `wake.db`, `.build/`, `bin/`, or `lib/wake/` artifacts (already gitignored, but worth knowing).
- The user docs in `share/doc/wake/quickref.md` have drifted in places (e.g., `RunnerFilter`, plain `runJob`). When in doubt, the source-of-truth is `share/wake/lib/system/*.wake`. The knowledge base flags known drift.

## Asking the database

Most "why does this file exist?" / "what changed?" questions are answerable without re-running anything:

```bash
wake --last                 # most recent jobs
wake --failed               # failures only
wake --job 1234 -v -d       # full record + stack trace
wake -o build/main.o        # provenance for an output
wake -i src/main.cpp        # consumers of an input
wake --metadata --last      # metadata only (skip stdout/stderr)
wake -g | grep linkO        # locate a stdlib symbol
```

Schema: `src/runtime/database.{h,cpp}`. Migrations: `tools/wake-migrate/`.

## Keeping the KB in sync (`make kb-check` / `make kb-bless`)

The knowledge base under `knowledge/` and the per-directory `AGENTS.md` files are *load-bearing for agents*. To keep them honest as the code changes there's a tiny upkeep flow:

```bash
make kb-check       # validate citations + show what's drifted since the last bless
make kb-bless       # mark current HEAD as up-to-date (after you've reviewed and updated)
```

What `kb-check` does (script: [`scripts/kb-check.sh`](scripts/kb-check.sh)):

1. **Path citations** — every backtick-wrapped path in `knowledge/*.md` and `**/AGENTS.md` that ends in a known source extension must resolve in the live tree. Broken citations are a **hard error** (exit 1).
2. **Symbol citations** — a curated list of stdlib/runtime identifiers we name explicitly is grepped for. Missing symbols are a **warning**.
3. **Delta since last bless** — diffs HEAD against the SHA in [`knowledge/.last-validated-sha`](knowledge/.last-validated-sha) and maps each changed file to the KB topic(s) most likely to need review (e.g., `share/wake/lib/system/job.wake` → `knowledge/03-stdlib.md`, `knowledge/04-build-model.md`). Any delta hits in tracked areas exit 1 with a review checklist.

The mapping table lives inside the script (a `case` statement). When you add a new tracked subsystem (e.g., a new directory under `tools/`), append a clause so future changes there fan out to the right topic file(s).

### Workflow when you change tracked code

1. Make your code changes as usual.
2. Run `make kb-check`. If it lists topics needing review:
   - Open each topic file under `knowledge/` (or the relevant `*/AGENTS.md`).
   - Update what's now wrong or out-of-date. Add coverage for new APIs.
3. Re-run `make kb-check`. Once it's clean (`exit 0`), run `make kb-bless`.
4. `make kb-bless` rewrites `knowledge/.last-validated-sha` to current HEAD. Stage and commit it alongside your KB updates: `git add knowledge/ **/AGENTS.md && git commit`.
5. CI (`.github/workflows/kb-check.yml`) re-runs the same check. Path-citation breakage is gated; delta findings are surfaced as warnings.

`kb-bless` refuses to run if `knowledge/` or any `AGENTS.md` has uncommitted changes — the blessed SHA must correspond to a published state of the docs. Pass `--force` to override (sparingly).

### When the SHA is unreachable

If you rebase or rewrite history past the recorded SHA, `kb-check` will refuse with `recorded SHA … is unreachable`. Fix by either fetching the missing commits or, if the lost SHA is gone for good, manually edit `knowledge/.last-validated-sha` to a known-reachable ancestor and re-bless from there.

### When this feels like overhead

It shouldn't. Steady-state, `make kb-check` runs in ~2 seconds and the only time you touch a KB file is when you've actually changed something the KB describes. If a file you changed *isn't* mapped to any topic, the script says so and exits clean — no work for you.

## Sub-level AGENTS.md files

Each major subdirectory carries its own `AGENTS.md` for context-specific workflow:

- [`src/AGENTS.md`](src/AGENTS.md) — C++ compiler/runtime pipeline
- [`share/wake/lib/AGENTS.md`](share/wake/lib/AGENTS.md) — standard library conventions
- [`share/doc/wake/AGENTS.md`](share/doc/wake/AGENTS.md) — user-facing docs index
- [`tools/AGENTS.md`](tools/AGENTS.md) — installable binaries
- [`tests/AGENTS.md`](tests/AGENTS.md) — test layout & how to add one
- [`rust/AGENTS.md`](rust/AGENTS.md) — Rust components (RSC, log viewer)
- [`extensions/vscode/AGENTS.md`](extensions/vscode/AGENTS.md) — VSCode extension

When you make a change that affects subsystem workflow, update the relevant sub-level `AGENTS.md`.

## Notes for autonomous agents

- The build is self-hosted: a meaningful change to C++ may need `make clean && make` to flush stale objects.
- FUSE tests fail without kernel support; don't expect them to run in arbitrary CI sandboxes.
- `make test` runs many subprocesses; expect minutes, not seconds. Plan timeouts accordingly.
- When you need to verify a Wake-side change, prefer running an existing relevant test under `tests/` over hand-crafting one — it'll be faster and you'll learn the expected idioms.
- The knowledge base under `knowledge/` was authored against `master` at `66c3aa1f` (v49.x line). If you change architecture, schemas, CLI flags, or stdlib APIs, update the relevant topic file there.

## Quick license note

Apache-2.0 (see `LICENSE` and `LICENSE.Apache2`). Vendored components in `vendor/` carry their own permissive licenses — don't strip headers when refactoring.
