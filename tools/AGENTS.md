# tools/ — Installable Binaries

Each subdirectory builds a standalone binary that ships with Wake. C++ sources, plus per-tool `*.wake` build rules. Common utilities are pulled from `../src/`.

Deeper catalog: [`../knowledge/06-tooling.md`](../knowledge/06-tooling.md).

## Subdirectories

| Path | Binary | Role |
|---|---|---|
| `wake/` | `wake` | Main CLI. Entry: `main.cpp`. Options: `cli_options.h`. DB introspection: `describe.{h,cpp}`. Output styling: `markup.{h,cpp}`. |
| `wake-format/` | `wake-format` | AST-aware formatter for `.wake` files. Run via `make format`. |
| `wake-unit/` | `wake-unit` | C++ unit-test runner. Tests live under `../tests/wake-unit/`. Run via `make unittest`. |
| `wake-migrate/` | `wake-migrate` | `wake.db` schema migration tool. Tested under `../tests/wake-migrate/`. |
| `wakebox/` | `wakebox` | Standalone job sandbox (namespaces / chroot-style). Tested under `../tests/wakebox/`. |
| `fuse-waked/` | `fuse-waked` | Long-lived FUSE daemon backing WakeFS. Pairs with `../src/wakefs/`. |
| `shim-wake/` | `shim-wake` | Thin wrapper to set up env / resource limits before exec. |
| `wake-hash/` | `wake-hash` | CLI to compute BLAKE2 hashes the way Wake does (CAS utility). |
| `lsp-wake/` | `lsp-wake` | Language Server Protocol backend (consumed by `extensions/vscode/`). |
| `bsp-wake/` | `bsp-wake` | Build Server Protocol backend. |

`tools/tools.wake` ties all of the above into the self-hosted build (`build.wake`).

## Build

Bootstrapping (`make` at repo root) compiles every `.cpp` under `tools/` along with `src/`. Self-hosted (`./bin/wake build default`) re-builds via the rules in `tools/tools.wake`.

If you add a new tool:
1. Create `tools/<name>/` with sources + a `<name>.wake` build rule.
2. Wire it into `tools/tools.wake`.
3. If it's a regular release artifact, also add it to `Makefile`'s install target.
4. Add tests under `../tests/<area>/`.

## Conventions

- **Don't reach into `wake-internal` headers from a tool unless necessary.** Tools should depend on what `src/` exposes through public headers.
- **Tools share utilities via `src/wcl/`, `src/util/`, `src/compat/`.** Don't duplicate path or shell helpers; reuse those.
- **Format with `make format`.** Both `clang-format` (C++) and `wake-format` (.wake) are enforced by CI.
- **CLI options live in option tables**, not in ad-hoc parsing. See `tools/wake/cli_options.h` for the pattern.
- **The LSP and formatter share document machinery from `src/wcl/doc.h` / `doc_state.h`.** Reuse those.

## Editor-side tooling

`extensions/vscode/` is the VSCode client wrapping `lsp-wake`. See [`../extensions/vscode/AGENTS.md`](../extensions/vscode/AGENTS.md).

Smaller editor support (syntax highlighting only) lives in [`../share/doc/wake/syntax/`](../share/doc/wake/syntax/) — emacs, joe, vim.
