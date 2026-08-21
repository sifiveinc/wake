# src/ — C++ Compiler & Runtime

The entire C/C++ side of Wake. The CLI binary `bin/wake` is built from these sources plus `tools/wake/`. No `.wake` code lives here (other than `src.wake`, the build manifest).

Deeper map: [`../knowledge/05-runtime-internals.md`](../knowledge/05-runtime-internals.md).

## Pipeline

```
.wake source
  → src/parser/      (re2c lexer, lemon grammar) → CST
  → src/dst/         (desugar, bind names)        → DST
  → src/types/       (Hindley–Milner inference)   → typed DST
  → src/optimizer/   (SSA, inline, CSE, sweep)    → optimized IR
  → src/runtime/     (eval loop, GC, scheduler, prims, SQLite DB) → execution
```

Cross-cutting subsystems:
- `src/cas/` — content-addressable store (BLAKE2/SipHash)
- `src/wakefs/` — FUSE sandbox layer; pairs with `tools/fuse-waked/`
- `src/json/` — JSON5 parser used for config and IPC
- `src/wcl/` — path/file/document utilities (also used by LSP and formatter)
- `src/util/` — shell/term/tracing helpers
- `src/compat/` — platform shims (spawn, mtime, rusage, sigwinch, physmem)

## When you change …

| Change | Touch | Then |
|---|---|---|
| Grammar | `src/parser/parser.y.m4`, add CST node in `src/parser/cst.h`, desugar in `src/dst/todst.cpp` | `make` regenerates the lemon parser |
| New built-in type | `src/types/internal.{h,cpp}`, `src/runtime/prim.cpp`, `share/wake/lib/core/types.wake` | add unit tests under `tests/wake-unit/` and a regression test under `tests/type-system/` |
| New primitive function | register in `src/runtime/prim.cpp`, expose in the right `share/wake/lib/<area>.wake` | regression test under `tests/runtime/` or `tests/standard-library/` |
| Job scheduler | `src/runtime/job.{h,cpp}`, `src/runtime/runtime.cpp` | regression test under `tests/runtime/` |
| FUSE sandbox | `src/wakefs/`, possibly `tools/fuse-waked/` | mind macOS/Linux divergence |
| DB schema | `src/runtime/database.{h,cpp}` | add a migration in `tools/wake-migrate/`; bump schema version |
| Optimizer pass | new `.cpp` under `src/optimizer/`, register in IR pipeline | add coverage under `tests/runtime/` (behavior should be unchanged) |

## Build

The bootstrap (top-level `Makefile`) compiles every `.cpp` under `src/` and `tools/`, links against vendored deps in `vendor/` plus system libs (sqlite3, gmp, re2, libfuse, ncurses), and produces `bin/wake` and helper binaries under `lib/wake/`.

After bootstrap, `./bin/wake build default` re-builds everything via Wake itself, with caching. `src/src.wake` defines the Wake-side rules.

If you've made a deep parser/lexer change and rebuilds aren't picking it up: `make clean && make`.

## Conventions

- C++17, no exceptions thrown across runtime boundaries (we crash on unrecoverable conditions).
- `clang-format` enforced — `make format` before committing.
- Header guards use `#ifndef`/`#define`/`#endif` matching the file path.
- Don't add new dependencies. The set of vendored libs and system requirements has been stable; raise it before introducing one.
- Public C++ APIs between subsystems live in headers; internal helpers stay `.cpp`-local.

## Vendored deps

`vendor/` carries BLAKE2, SipHash, gopt, lemon, whereami, utf8proc. System-installed deps (sqlite3, gmp, re2, libfuse, ncurses, dash) are *not* vendored — the build uses pkg-config or system paths. See top-level `README.md` for version floors.
