# rust/ — Rust Components

Rust crates that ship alongside Wake. Built with Cargo (driven by `share/wake/lib/rust_wake/cargo.wake` rules in `build.wake`).

These are *separate* from the C++ Wake binary — the Rust components provide the **Remote Shared Cache (RSC)** server and the **log viewer**. Wake itself doesn't need Rust at runtime; teams that want distributed caching deploy these.

## Crates

| Crate | Binary / Library | Purpose |
|---|---|---|
| `rust/rsc/` | `rsc` (server), `rsc_tool` (CLI) | HTTP server for the remote cache. Talks PostgreSQL. Source: `rust/rsc/src/bin/rsc/`, `rust/rsc/src/bin/rsc_tool/`. |
| `rust/entity/` | library | SeaORM entity definitions for the cache DB (Job, Blob, Output, APIKey). |
| `rust/migration/` | library + CLI | SeaORM migrations. Apply with the standard SeaORM CLI pattern. |
| `rust/log_viewer/` | binary | Web UI for browsing build logs. |

## Build & test

```bash
# From the repo root, normal Wake-driven build covers Rust:
make                                    # bootstrap + self-host + Rust crates

# Or directly with Cargo (within rust/<crate>):
cargo build
cargo test
cargo run --bin rsc -- <args>

# RSC end-to-end tests (requires PostgreSQL service):
make remoteCacheTests
```

The RSC tests are exercised by `tests/remote-cache/` against a live PostgreSQL container — see `.github/workflows/build.yaml` for CI setup and `scripts/` for helpers.

## Conventions

- **Edition / toolchain**: match what's already declared in each crate's `Cargo.toml`.
- **`cargo fmt`** before committing. (CI's format check is currently primarily `clang-format` + `wake-format`; keeping Rust formatted is still expected.)
- **Don't introduce new top-level dependencies** without a clear justification. The dep graph here is intentionally small.
- **Database changes go through SeaORM migrations** in `rust/migration/`, never hand-rolled SQL.
- **Don't tightly couple to a specific PostgreSQL minor version**; the schema is portable across recent stable versions.

## When changing the cache schema

1. Add a new migration in `rust/migration/src/`.
2. Update entities in `rust/entity/` to match.
3. Update API handlers in `rust/rsc/`.
4. If client-side cache keys or formats change, update `share/wake/lib/system/remote_cache_api.wake` and `remote_cache_runner.wake` in lockstep.
5. Add coverage under `tests/remote-cache/`.

## Cross-references

- Wake-side client API: `share/wake/lib/system/remote_cache_api.wake`, `remote_cache_runner.wake`.
- Tests: `tests/remote-cache/`.
- Build glue: `share/wake/lib/rust_wake/cargo.wake`.
