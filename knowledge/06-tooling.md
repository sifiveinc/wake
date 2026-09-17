# Tooling

Sources for installable binaries live under `tools/`. Each subdirectory is its own little C++ project (with shared utilities pulled from `src/`). Everything is wired into `tools/tools.wake` and ultimately `build.wake`.

## `tools/wake/` — the main binary

The CLI users invoke as `wake`. Parses options, opens `wake.db`, drives the runtime.

- Entry: `main.cpp`.
- Options table: `cli_options.h`.
- Database introspection (`--last`, `--failed`, `-o`, `-i`, `--job`): `describe.{h,cpp}`.
- Styled output: `markup.{h,cpp}`.
- Build glue: `wake.wake`.

## `tools/wake-format/` — code formatter

AST-aware reformatter. Run via `make format` (changed files) or `make formatAll`. The format checker in CI uses this.

## `tools/wake-unit/` — C++ unit-test runner

Runs unit tests over the C++ runtime. Invoked by `make unittest`. Tests live under `tests/wake-unit/`.

## `tools/wake-migrate/` — DB schema migration

When the on-disk SQLite schema changes, `wake-migrate` upgrades old `wake.db` files. Tested under `tests/wake-migrate/`.

## `tools/wakebox/` — execution sandbox

A standalone sandbox utility (chroot/namespace-style). Used by the default runner to isolate jobs, but also runnable directly. Exposes a config file describing input/output mounts, env vars, command. Tested under `tests/wakebox/`.

Common knobs (see `tests/wakebox/` for examples): rootfs spec, ro/rw mount lists, env passthrough, working dir, command.

## `tools/fuse-waked/` — FUSE daemon

Long-lived process backing WakeFS. Each job's filesystem operations route through this daemon, which records actual reads/writes. Pairs with `src/wakefs/`.

## `tools/shim-wake/` — job exec wrapper

Thin wrapper that sets up env, resource limits, and execs the job command. Used by runners to standardize execution.

## `tools/wake-hash/` — content-hash CLI

Compute BLAKE2 hashes the way Wake does. Useful for offline verification of CAS state.

## `tools/lsp-wake/` — Language Server Protocol

Backend for editor integrations. Provides hover, go-to-definition, completions, diagnostics for `.wake`. Consumed by `extensions/vscode/` (and any other LSP-aware editor — Emacs / Neovim / etc.).

## `tools/bsp-wake/` — Build Server Protocol

Backend exposing Wake builds to build-aware IDEs.

## Rust components (`rust/`)

Built with Cargo (driven by `share/wake/lib/rust_wake/cargo.wake` rules in `build.wake`).

| Crate | What it does |
|---|---|
| `rust/rsc/` | Remote Shared Cache HTTP server (binary `rsc`) plus `rsc_tool` CLI. Talks PostgreSQL; serves cache lookups and pushes. Tested via `tests/remote-cache/` and `make remoteCacheTests`. |
| `rust/entity/` | SeaORM entity definitions for the cache DB (Job, Blob, Output, APIKey). |
| `rust/migration/` | SeaORM migrations for cache schema. |
| `rust/log_viewer/` | Web UI for browsing build logs. |

## Editor integration (`extensions/vscode/`)

- `extensions/vscode/syntaxes/wake.tmLanguage.json` — syntax highlighting grammar.
- `extensions/vscode/language-configuration.json` — comments, brackets, indent rules.
- `extensions/vscode/src/extension.ts` — VSCode client; spawns `lsp-wake` and routes LSP traffic.
- Build/package via `make vscode`.

Other editor support (smaller, file-only) sits under `share/doc/wake/syntax/` for **emacs**, **joe**, and **vim**.

## When an agent reaches for which tool

| Need | Tool |
|---|---|
| Run a build | `wake <target>` |
| Inspect history | `wake --last` / `wake --failed` / `wake -o file` |
| Reformat .wake | `make format` (or call `wake-format` directly) |
| Run C++ unit tests | `make unittest` |
| Add tests | edit under `tests/<category>/<name>/` |
| Migrate an old wake.db | `wake-migrate <path>` |
| Hash a file like Wake | `wake-hash <path>` |
| Drive an isolated job manually | `wakebox <config>` |
| Provide IDE features | `lsp-wake` (already wired by VSCode extension) |
| Stand up a remote cache | binaries in `rust/rsc/`, schemas in `rust/migration/` |
