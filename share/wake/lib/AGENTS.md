# share/wake/lib/ — The Wake Standard Library

The standard library that ships with `wake`. Pure Wake code (`.wake` files); no C++ here except the primitives it binds to (which live in `src/runtime/prim.cpp`).

Deeper catalog: [`../../../knowledge/03-stdlib.md`](../../../knowledge/03-stdlib.md).

## Layout

```
core/        Pure, no side effects.
  list.wake     Lists & higher-order ops (map, filter, foldl/r, …)
  string.wake   String ops (cat, replace, tokenize, …)
  regexp.wake   Compiled regex
  integer.wake  Integer math
  double.wake   Double math
  boolean.wake  Booleans
  option.wake   Option a (Some / None) + omap, oflatMap, getOrElse
  result.wake   Result a b (Pass / Fail) + rmap, rflatMap, findFail
  tuple.wake    Pair, helpers
  tree.wake     Ordered tree (O(log n) insert/contains/delete)
  vector.wake   Array-like
  map.wake      Key-value map (tree-backed)
  json.wake     JValue ADT, parseJSON, formatJSON
  order.wake    Order ADT (LT/EQ/GT)
  print.wake    print, println, LogLevel definitions
  syntax.wake   Pretty printing
  types.wake    Type-meta utilities
  unsafe.wake   Escape hatches (unsafe_*) — for runtime authors only

system/      Build & runtime surface.
  path.wake             Path operations
  sources.wake          source / sources / @here
  plan.wake             Plan tuple + makePlan / editPlan* / setPlan*
  job.wake              runJobWith, getJob* accessors, primJob* primitives
  runner.wake           localRunner, defaultRunner, virtualRunner, workspaceRunner,
                        makeRunner, makeJSONRunner
  cas.wake              Content-addressable store API
  environment.wake      Build-env helpers, tool location
  staging_outputs.wake  Staging area for declared outputs
  io.wake               File read/write (read, write)
  http.wake             HTTP client primitives
  time.wake             epoch, formatTime, time arithmetic
  remote_cache_api.wake          RSC client API
  remote_cache_runner.wake       RSC-aware runner
  remote_cache_api_test.wake     RSC client API tests

gcc_wake/    C/C++ rules: gcc.wake (compileC, linkO), pkgconfig.wake
rust_wake/   Cargo rules: cargo.wake
nothing/     Stub package (testing / no-op runner)
```

## Conventions

- **Use `export`, not `global`.** `global def` is legacy. New APIs live in named packages with explicit `export def`.
- **Keep `core/` pure.** Anything that does I/O, spawns jobs, or touches the workspace belongs in `system/` or further down.
- **Tuple accessors are auto-generated.** A `tuple Foo = export Bar: T` gives you `getFooBar`, `setFooBar`, `editFooBar` for free. Don't hand-roll them.
- **Document with `#` line comments above the def.** The LSP and formatters surface these.
- **`unsafe_*` is reserved.** Functions in `core/unsafe.wake` and the `unsafe_*` family in `system/job.wake` bypass purity. Don't expose them through wrappers.
- **Run `make format`.** `wake-format` enforces house style on `.wake` files.

## Adding a new function

1. Pick the right file (don't create new ones unless the topic is genuinely new).
2. `export def name args = …` with a brief `#` doc comment above.
3. Add a regression test under `tests/standard-library/<name>/` (see [`../../../tests/AGENTS.md`](../../../tests/AGENTS.md)).
4. `make format && make test`.

## Adding a new primitive

If the function needs runtime support (calls into C++):

1. Implement in `src/runtime/prim.cpp` and register in the prim table.
2. Bind in the appropriate `.wake` file with `export def primFoo … = prim "foo"`.
3. Wrap with a clean Wake-side API (don't expose `prim*` directly outside the stdlib).
4. Tests under both `tests/wake-unit/` (for the C++) and `tests/standard-library/` (for the Wake-side).

## Cross-cutting

- The Plan tuple is defined in `system/plan.wake` (~line 33). Auto-generated accessors cover every field. Manual helpers (`setPlanShare`, `setPlanKeep`, `setPlanOnce`, `setPlanFilterOutputs`, `setPlanEnvVar`, `prependPlanPath`) layer on top.
- Runners are `Runner name run` where `run: Job => RunnerInput => RunnerOutput`. `localRunner` in `system/runner.wake` is the canonical reference implementation.
- The standard-library docs in `share/doc/wake/` summarize but sometimes drift from the live API (notably `quickref.md`'s `runJob` / `RunnerFilter`). The source here is authoritative.
