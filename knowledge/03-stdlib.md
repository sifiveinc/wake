# The Wake Standard Library

Lives in `share/wake/lib/`. Four packages: `core`, `system`, `gcc_wake`, `rust_wake`, plus a stub `nothing`.

## `share/wake/lib/core/` — pure, no side effects

| File | Purpose | Highlights |
|---|---|---|
| `list.wake` | List operations | `map`, `mapFlat`, `filter`, `foldl`/`foldr`, `scanl`/`scanr`, `head`, `tail`, `take`, `drop`, `prepend`, `append`, `reverse`, `flatten`, `zip`, `unzip`, `seq`, `find`, `exists`, `forall`, `splitAt`, `splitBy`, `takeUntil`, `dropUntil`, `distinctBy`, `sortBy`, `cmp`, `++` |
| `string.wake` | Strings | `strlen`, `explode`, `cat`, `replace`, `tokenize`, `stringToRegExp`, `integerToUnicode`, `unicodeToInteger`, `++` |
| `regexp.wake` | Regex | Compiled-regex type, `matches`, `extract`, `replaceAll` |
| `integer.wake` | Integer math | `+ - * / % ^`, parsing, formatting |
| `double.wake` | Double math | `+. -. *. /. ^.`, parsing, formatting, NaN/inf |
| `boolean.wake` | Booleans | `&`, `|`, `!`, `xor` |
| `option.wake` | `Option a` | `Some`/`None`, `omap`, `oflatMap`, `getOrElse`, `isSome`, `isNone` |
| `result.wake` | `Result a b` | `Pass`/`Fail`, `rmap`, `rflatMap`, `findFail`, `failWithError`, `makeError` |
| `tuple.wake` | `Pair`, helpers | `Pair`, `→`, `getPairFirst`/`getPairSecond` |
| `tree.wake` | Ordered tree | `tinsert`, `tdelete`, `tcontains`, `tunion`, `tintersect`, `tsubtract`, `tmin`, `tmax` (O(log n)) |
| `vector.wake` | Array-like | random access in O(1), update in O(log n) typical |
| `map.wake` | Key-value map | Backed by tree; `mlookup`, `minsert`, `mdelete` |
| `json.wake` | JSON5 values | `JValue` ADT, `parseJSON`, `formatJSON`, traversal helpers |
| `order.wake` | Ordering | `Order` ADT (`LT`/`EQ`/`GT`), comparator helpers |
| `print.wake` | Output | `print`, `println`, `printLevel`, `format`, `LogLevel` definitions |
| `syntax.wake` | Pretty print / format | Pretty-printing helpers |
| `types.wake` | Type-meta utilities | rarely needed in user code |
| `unsafe.wake` | Escape hatches | `unsafe_*` family — reserved for runtime authors |

Big-O notes for the data structures live at `share/doc/wake/datastructures.txt`.

## `share/wake/lib/system/` — the build/runtime surface

| File | Role |
|---|---|
| `path.wake` | `Path` operations: `getPathName`, `getPathHash`, `getPathParent` |
| `sources.wake` | Discovering source files: `source "file"`, `sources dir regex`, `@here` macro |
| `plan.wake` | Build a `Plan` (a tuple — every field has auto-generated `getPlan*` / `setPlan*` / `editPlan*` accessors): `makePlan label visible command`, `makeExecPlan`, `makeShellPlan`, plus the manual helpers `setPlanFilterOutputs`, `setPlanEnvVar`, `setPlanShare` / `setPlanKeep` / `setPlanOnce`. |
| `job.wake` | Run plans: `runJobWith runner plan` (canonical entry point; pair with `localRunner` / `defaultRunner` / `virtualRunner` / `workspaceRunner` / a custom runner). Accessors: `getJobStdout`, `getJobStderr`, `getJobReport`, `getJobInputs`, `getJobOutputs`, plus `unsafe_getJobRunnerOutput` / `unsafe_getJobRunnerError`. |
| `runner.wake` | Define runners: `localRunner`, `defaultRunner`, `makeRunner`, `Runner`, `RunnerInput`, `RunnerOutput`, scoring & filtering |
| `cas.wake` | CAS operations: hashing, content addressing, retrieval |
| `environment.wake` | Read environment, build env strings, locate tools |
| `staging_outputs.wake` | Staging area helpers for declared outputs |
| `io.wake` | File read/write primitives (`read`, `write`) |
| `http.wake` | HTTP client primitives (used by the remote cache, networked tests) |
| `time.wake` | `epoch`, `formatTime`, time arithmetic |
| `remote_cache_api.wake` | Client API for the Remote Shared Cache (RSC) |
| `remote_cache_runner.wake` | Runner that consults RSC before/after job execution |
| `remote_cache_api_test.wake` | API-level tests for the RSC client |

### The Plan → Job lifecycle (one diagram, in code)

```wake
require Pass src = source "main.cpp"

def plan =
    makePlan "compile main"
        (src, Nil)                                  # visible inputs
        "g++ -c {src.getPathName} -o main.o"        # shell script string
    | editPlanEnvironment (\_ ("PATH=/usr/bin", Nil))

def job = plan | runJobWith defaultRunner

require Pass out = getJobOutputs job
# `out` is a List Path. Wake observed which files the job actually wrote.
```

For deeper detail on this flow see `knowledge/04-build-model.md`.

## `share/wake/lib/gcc_wake/` — C/C++ rules

| File | Role |
|---|---|
| `gcc.wake` | Toolchain abstractions and rules: `compileC variant flags headers src`, `linkO variant flags objs name extras`, plus C/C++ helpers and `SysLib` glue |
| `pkgconfig.wake` | Wrap `pkg-config` for include/lib flags |

The `variant` argument selects compilers and flags (e.g., `"native-cpp17-release"`, `"native-cpp17-debug"`, `"wasm-cpp17-release"`).

Typical use:
```wake
from wake import _
from gcc_wake import compileC linkO

def variant = "native-cpp17-release"

export def buildAll _ =
    require Pass srcs = sources @here `.*\.cpp`
    def compile = compileC variant ("-I.", Nil) Nil
    def objects = map compile srcs | findFail
    require Pass objs = objects
    linkO variant Nil (flatten objs) "myapp" Nil
```

## `share/wake/lib/rust_wake/` — Cargo / Rust rules

`cargo.wake` exposes Cargo invocations that integrate with Wake's job tracking. Used by the Wake repo itself to build the Rust components in `rust/`.

## `share/wake/lib/nothing/` — placeholder

`share/wake/lib/nothing/nothing.wake` is a stub package useful for testing the package system and as a no-op import target.

## Big-O cheat sheet (extracted)

From `share/doc/wake/datastructures.txt`:
- List: `head`/`tail` O(1), `append` O(n), `++` O(n), `reverse` O(n), `sortBy` O(n log n).
- Tree: `tinsert`/`tdelete`/`tcontains` O(log n); `tunion`/`tintersect` O(n+m).
- Vector: random access O(1); persistent updates O(log n).
- Map: O(log n) for `mlookup`/`minsert`.

When you need a fast set, prefer `Tree` over `List` with `distinctBy`.
