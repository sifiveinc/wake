# The Wake Language

Wake is a strict, statically typed, ML-family functional language with Hindley–Milner type inference. Indentation is **not** significant; layout is governed by `=` and the `match` / `require` constructs.

Authoritative references:
- `share/doc/wake/quickref.md` — cheat sheet
- `share/doc/wake/tutorial.md` — language tour
- `share/doc/wake/basic_wake_tutorial.md` — beginner intro
- `share/doc/wake/tour/packages.adoc` — packages, exports, imports
- `share/doc/wake/tour/targets.adoc` — memoization
- `share/doc/wake/tour/tuples.adoc` — tuples & accessors
- `share/doc/wake/wake-for-fsharp-developers.md`, `share/doc/wake/wake-for-scala-developers.adoc` — comparison guides

## Definitions

```wake
def add x y = x + y         # one expression per def; recursion allowed by default
global def square x = x * x # legacy; prefer `export` + packages
export def double x = x + x # makes the name visible to other packages
```

`def` allows nested `def`s as inner bindings:

```wake
def example x =
    def helper y = y + 1
    helper (helper x)
```

Type annotations are optional; when given, syntax is `name: Type` for parameters and `: Type =` for the result:

```wake
export def decrement (i: Integer): Integer = i - 1
```

## Lambdas

```wake
\x x + 1           # backslash-lambda
\x \y x + y        # multi-arg via curry
(_ + 1)            # underscore-hole shorthand (one slot per `_`)
```

## Application, pipe, dot

- Application is juxtaposition: `f x y` (left-associative; `f x y` = `(f x) y`).
- Pipe: `x | f | g` ≡ `g (f x)`. Pipes read left-to-right.
- Dot: `x.f` ≡ `f x`. Often used for tuple-accessor calls and method-style chains.

## Pattern matching: `match`, `require`, `if`

```wake
def describe x = match x
    Some 0   = "zero"
    Some n   = "n is {str n}"
    None     = "absent"

def first x = if x < 0 then 0 else x

def safeDiv a b =
    require Some r = if b == 0 then None else Some (a / b) else 0
    r
```

`require` desugars to `match`; the `else` branch is the failure value. If omitted, the failure type must propagate (e.g., a `Result Fail` cascading up).

## Built-in types

- Primitives: `Integer` (arbitrary precision via GMP), `Double` (IEEE-754), `String` (Unicode), `Boolean` (`True`/`False`).
- Collections: `List a` (cons-based; constructed `1, 2, 3, Nil`), `Pair a b`, `Tree a`, `Vector a`, `Map k v`.
- Standard ADTs: `Option a` (`Some a` | `None`), `Result a b` (`Pass a` | `Fail b`), `Error` (cause + stack).
- Build types: `Path`, `Plan`, `Job`, `Runner`, `RunnerInput`, `RunnerOutput`, `SysLib`.

### String literals
```wake
"plain {expr} interpolation"     # Unicode, escapes
'raw bytes'                       # raw, no escapes
"%
multi-line with custom delimiter
%"
```

### Regex literals — backticks
```wake
def isHex s = matches `^[0-9a-fA-F]+$` s
```

## User-defined types

### `data` — sum types

```wake
export data Shape =
    Circle (radius: Double)
    Square (side: Double)

def area s = match s
    Circle r = 3.14159 *. r *. r
    Square s = s *. s
```

Constructors are functions; arity matches declared fields.

### `tuple` — product types with auto-accessors

```wake
export tuple Cat =
    export Name: String
    export Age:  Integer
```

Each field generates three globals: `getCatName`, `setCatName`, `editCatName`.

```wake
def kitten = Cat "Whiskers" 1
def older  = editCatAge (_ + 1) kitten
def name   = kitten.getCatName
```

Generic tuples take lowercase parameters: `tuple Pair a b = export First: a / export Second: b`.

To preserve binary/source compatibility when adding fields later, hide the constructor and expose a factory:
```wake
tuple Cat =
    Name: String
    Age:  Integer
export def makeCat name age = Cat name age
```

## `target` — memoization

```wake
target fib n = if n < 2 then 1 else fib (n - 1) + fib (n - 2)
```

`target` caches the result keyed by the argument tuple. Use it whenever:
- the function is expensive,
- the function returns `Path` (so the same intermediate is shared across consumers),
- you'd otherwise re-execute equivalent jobs.

A "target subkey mismatch" error means the same key was used twice with conflicting hidden state — usually a sign that the function depends on something outside its declared parameters. See `share/doc/wake/tour/targets.adoc`.

## Operators

- Integer: `+ - * / % ^ << >>`
- Double: `+. -. *. /. ^.` (note the trailing dot)
- Comparison: `< > <= >= == !=` (and Unicode equivalents)
- Boolean: `&` (and), `|` (or; **also** the pipe operator — context disambiguates), `!`
- Strings: `++` concat
- Lists: `,` (cons), `++` (concat)

There is no overloading; many polymorphic-feeling operators are actually defined per-type.

## Packages, imports, exports

One package per file, declared at top:
```wake
package my_module
```
If omitted, the file lives in the unnamed package.

Imports:
```wake
from wake import _                       # bring in everything wake exports
from wake import def map filter          # selective, by category
from gcc_wake import compileC linkO      # by name
from upstream import alias=original      # rename
```

Re-exports (chain a public surface):
```wake
from upstream export def reexportedFn
from upstream export type ReexportedT
```

Identifier resolution order: definition-local → file-local → same-package → globals (legacy).

## Publish / subscribe

Workspace-wide append-only channels:
```wake
topic myChannel: String

publish myChannel = "hello", Nil
publish myChannel = "world", Nil

def listAll = subscribe myChannel  # ("hello", "world", Nil) (ordering is workspace-defined)
```

Used by the build system to register runners (`publish runner = …`), tests, environment packages, and resource providers. See uses in `share/wake/lib/system/runner.wake`.

## `.wakeignore`

Glob file that excludes `.wake` files from compilation. Lives next to a `.wakeroot`.

```
# share/doc/wake/tutorial.md describes the syntax
build/**            # ignore everything below build
**/legacy.wake      # ignore legacy.wake at any depth
?-temp.wake         # single-char wildcard
```

## Common idioms

### Result chaining via `require`
```wake
def buildOne file =
    require Pass src    = source file
    require Pass object = compileC variant flags Nil src
    Pass object
```

### Mapping with failure aggregation
```wake
require Pass srcs = sources @here `.*\.cpp`
map compile srcs
| findFail        # List (Result a Error) -> Result (List a) Error
```

### Anonymous holes
```wake
list | filter (_ >= 0) | map (_ * 2)
```

### Forcing strict order (sequencing side-effecting jobs)
Pure code is parallelized automatically. To force ordering, thread the `Job` (or its outputs) through a dependent expression — Wake honors data dependencies.

## Things to avoid

- **Don't `global def` new APIs**: it's the legacy escape hatch. Use packages with `export`.
- **Don't reach into `unsafe`** (`share/wake/lib/core/unsafe.wake`) unless you have a concrete reason; it bypasses purity guarantees.
- **Don't rely on `print`/`println` for build outputs**: their text goes to `logReport` and is **not** stored in `wake.db`, so it won't appear in `--last` queries. Use job stdout/stderr (or `setPlanStdout`/`setPlanStderr`) when you need persistence.
