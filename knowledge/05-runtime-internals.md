# Runtime & Compiler Internals (C++)

The compiler and runtime live entirely in `src/`. The CLI entry point is in `tools/wake/`. Everything ships as one binary, `bin/wake`.

## Compilation pipeline

```
.wake source
    │
    ▼  src/parser/  (re2c lexer + lemon grammar)
CST  ─ Concrete Syntax Tree
    │
    ▼  src/dst/  (todst, bind)
DST  ─ Desugared / name-bound AST
    │
    ▼  src/types/  (HM inference, ADT/sum/tuple)
Typed DST
    │
    ▼  src/optimizer/  (SSA, inline, CSE, sweep, scope, purity, usage)
Optimized IR
    │
    ▼  src/runtime/  (eval loop, GC, scheduler, prims)
Job execution + database writes
```

## Subsystems

### `src/parser/`
- `lexer.re` — re2c lexer specification (the generated lexer is committed as `lexer.cpp.gz` — `make` decompresses).
- `parser.y.m4` — Lemon grammar, m4-templated.
- `cst.{h,cpp}` — node types for the concrete syntax tree.
- `syntax.{h,cpp}` — error recovery / reporting.
- `wakefiles.{h,cpp}` — discovers `.wake` files (honoring `.wakeignore`) and feeds them through the lexer.

### `src/dst/`
- `todst.{h,cpp}` — lowers CST to DST (sugar removal: `require`, `match`, pattern guards).
- `bind.{h,cpp}` — name resolution, scope, closure capture.
- `expr.{h,cpp}` — DST expression node types.
- `dst.wake` — Wake-side build glue.

### `src/types/`
- `type.{h,cpp}` — type representation; unification.
- `datatype.{h,cpp}` — user-defined `data` (sum) types.
- `data.{h,cpp}` — record/tuple representation.
- `sums.{h,cpp}` — sum-type machinery.
- `internal.{h,cpp}` — built-in type bindings (`Integer`, `String`, `List`, etc.).
- `dsu.h` — disjoint-set-union for unification (Tarjan-style).
- `types.wake` — Wake-side type definitions exposed to user code.

### `src/optimizer/`
- `ssa.{h,cpp}` — convert IR to Static Single Assignment.
- `inline.cpp` — inlining.
- `cse.cpp` — common subexpression elimination.
- `scope.cpp` — lifetime / scope analysis.
- `purity.cpp` — classify functions pure / impure (drives parallelism opportunities).
- `tossa.cpp` — IR-to-SSA conversion driver.
- `sweep.cpp` — dead-code elimination.
- `usage.cpp` — usage-count tracking.
- `optimizer.wake` — Wake-side build rule.

### `src/runtime/`
The hot path. Where execution actually happens.
- `runtime.{h,cpp}` — main eval loop, continuation passing, GC integration.
- `value.{h,cpp}` — runtime value rep (tagged unions for primitives, closures, data, paths, jobs).
- `gc.{h,cpp}` — garbage collector.
- `job.{h,cpp}` — `Job` data structures, scheduling, dependency tracking.
- `prim.{h,cpp}` — primitive implementations called from Wake (everything from `+` to job-launch primitives like `primJobLaunch`).
- `database.{h,cpp}` — SQLite integration; opens `wake.db`, persists jobs, queries them.
- `profile.{h,cpp}` — runtime profiling counters.
- `runtime.wake` — Wake-side build glue and exposed runtime types.

### `src/cas/`
Content-Addressable Store. BLAKE2 / SipHash hashing, blob lookup. Backs both local cache content and the RSC client.

### `src/wakefs/`
- `fuse.{h,cpp}` — FUSE callback implementations.
- `namespace.{h,cpp}` — view layering (workspace + read-only deps + writable scratch).
- `daemon_client.cpp` — IPC to `fuse-waked`.

### `src/json/`
JSON5 parser used for `.wakeroot`, `wake.json`, IPC, log structure.
- `jlexer.re` — re2c lexer.
- `json5.{h,cpp}` — parser and tree.
- `jparser.cpp` — glue.

### `src/wcl/` — Wake C-runtime utilities
- `filepath.{h,cpp}` — path normalization, joining.
- `file_ops.{h,cpp}` — file I/O helpers.
- `doc.{h,cpp}` / `doc_state.h` — text-document machinery (used by LSP and formatter).

### `src/util/`
- `shell.{h,cpp}` — shell-quoting / process control.
- `term.{h,cpp}` — terminal capabilities (colors, width, progress bar).
- `tracing.{h,cpp}` — internal trace logging.

### `src/compat/`
Platform shims:
- `spawn.{h,c}` — `posix_spawn` portability.
- `mtime.{h,c}` — mtime quirks across filesystems.
- `rusage.{h,c}` — resource accounting.
- `sigwinch.{h,c}` — terminal resize.
- `physmem.{h,c}` — RAM detection.

## CLI entry point

`tools/wake/main.cpp` parses CLI options (declared in `tools/wake/cli_options.h`) and dispatches:

- `wake <fn> [args]` — run `fn args` (where `fn: List String -> a`).
- `wake -x 'expr'` — evaluate an expression.
- `wake --init` — initialize a workspace (create empty `.wakeroot` and `wake.db`).
- `wake --last`, `--failed`, `-o`, `-i`, `--job` — query the database (handled by `tools/wake/describe.{h,cpp}`).
- `wake -g` — list all globals.

`markup.{h,cpp}` handles styled terminal output (color, bold, dim).

## How the binary is assembled

`make` (top-level Makefile):
1. Compiles every `.cpp` under `src/` and `tools/` to objects.
2. Links a single `bin/wake` against vendored deps (`vendor/`) plus system libs (sqlite3, gmp, re2, libfuse, ncurses, dash).
3. Compiles helpers (`fuse-waked`, `wakebox`, `shim-wake`, `wake-hash`).

Once `bin/wake` exists, `bin/wake build default` (driven by `build.wake`) re-builds everything via Wake itself, with full caching and dependency tracking.

## Where to start when changing internals

- **Grammar change**: `src/parser/parser.y.m4`, regenerate by running `make`. Pair with an AST node in `src/parser/cst.h` and a desugaring in `src/dst/todst.cpp`.
- **New built-in type**: `src/types/internal.{h,cpp}`, plus a primitive in `src/runtime/prim.cpp`, plus a Wake-side binding in `share/wake/lib/core/types.wake`.
- **New primitive function**: register in `src/runtime/prim.cpp`; expose to user code in `share/wake/lib/core/<area>.wake`.
- **Job scheduling tweak**: `src/runtime/job.{h,cpp}` and `src/runtime/runtime.cpp`.
- **DB schema**: `src/runtime/database.{h,cpp}` plus a migration via `tools/wake-migrate/`.
- **Optimizer pass**: add a `.cpp` under `src/optimizer/`, register it in the IR pipeline.
