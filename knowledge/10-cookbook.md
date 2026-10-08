# Cookbook

Concrete recipes. Each is small enough to copy, adapt, and run.

## Add a function to the standard library

1. Pick the right file in `share/wake/lib/core/<area>.wake` or `share/wake/lib/system/<area>.wake`. (List ops → `share/wake/lib/core/list.wake`, etc.)
2. Add the function with `export def`:
   ```wake
   export def headOption list = match list
       Nil = None
       x, _ = Some x
   ```
3. If it should be available unprefixed, the consuming file imports `from wake import …`.
4. Add at least one regression test under `tests/standard-library/<name>/` with `.wakeroot` + `pass.sh`.
5. `make test` (and re-run `make` so the build picks up the new file).

## Run a one-off expression

```bash
wake --init .                      # if no .wakeroot
wake -x '1, 2, 3, Nil | map (_ * 10)'
```

For a typed sanity check: `wake -xv 'someFn'` prints the inferred type.

## Add a regression test

```bash
mkdir -p tests/runtime/my-new-test
cd tests/runtime/my-new-test
touch .wakeroot
cat > test.wake <<'WAKE'
package my_test
from wake import _

export def main _ = println "hello"
WAKE
cat > pass.sh <<'SH'
#!/bin/sh
"${1:-wake}" main
SH
chmod +x pass.sh
echo 'hello' > stdout
cd ../../..
make test
```

For an expected-failure test, name it `fail.sh` and exit non-zero.

## Query the database

```bash
wake --last                # last few jobs
wake --failed              # only failures
wake -o build/main.o       # what produced this file?
wake -i src/main.cpp       # which jobs read this header?
wake --job 42 -v           # full record for job 42, with stdout/stderr
wake -d --job 42           # add stack trace
```

## Define a custom runner

The current API (`share/wake/lib/system/runner.wake:145`) is:

```wake
export def makeRunner (name: String) (run: Job => RunnerInput => RunnerOutput): Runner
```

A runner is just a name plus a `run` function. The recommended pattern is to study `localRunner` (a minimal reference implementation in the same file) and adapt rather than wrap. For wrapping a JSON-driven external runner, use `makeJSONRunner` / `makeJSONRunnerPlan` (in `share/wake/lib/system/runner.wake`).

Skeleton:

```wake
package my_runners
from wake import _

export def myRunner: Runner =
    def run (job: Job) (input: RunnerInput): RunnerOutput =
        # call primJobLaunch / primJobFinish / etc. — see localRunner for a template
        ...
    makeRunner "my-runner" run
```

To run a plan with it explicitly: `plan | runJobWith myRunner`. Wake does not have an automatic "best runner" selector at the `runJobWith` level; the calling code chooses. (Some environment packages publish runner pools and dispatch via their own logic — search the codebase for `publish runner` for examples.)

## Debug a failing job

```bash
wake --failed                          # find the job id
wake --job <id> -v -d                  # stdout, stderr, runner channels, stack trace
wake -o <output-the-job-was-supposed-to-make>   # confirm provenance
```

If a job is failing with sandbox errors:
- Check inputs declared in `makePlan` cover everything the command actually touches.
- Run with `localRunner` temporarily to bypass sandbox and see the raw failure (`runJobWith localRunner plan`).
- Inspect runner channels: `unsafe_getJobRunnerOutput`, `unsafe_getJobRunnerError` (they capture what the runner reported, separate from the command's own stdout/stderr).

## Add a build variant in `build.wake`

`build.wake` already defines variants like `default`, `static`, `debug`, `wasm`. To add one:

1. Open `build.wake` and find the variant definition (search for the existing variant strings).
2. Add a new variant string (e.g., `"native-cpp17-asan"`) and the toolchain rules — typically a flags tuple consumed by `compileC` / `linkO`.
3. Wire it into the top-level dispatch (the function that maps `wake build <variant>` to artifacts).
4. Test with `./bin/wake build <new-variant>`.

## Format before commit

```bash
make format          # only changed files
make formatAll       # entire repo (slower)
```

CI rejects unformatted commits. The format checker invokes `clang-format` for C++ and `wake-format` for `.wake`.

## Pattern: collect outputs of N parallel jobs

```wake
require Pass srcs = sources @here `.*\.cpp`
def compile = compileC variant ("-I.", Nil) Nil
require Pass objss = map compile srcs | findFail
linkO variant Nil (flatten objss) "myapp" Nil
```

`map compile srcs` produces `List (Result (List Path) Error)`; `findFail` flips it to `Result (List (List Path)) Error`. Use `flatten` once you've unwrapped.

## Pattern: memoize an expensive function

Change `def` to `target`:

```wake
target compileOnce variant flags src = compileC variant flags Nil src
```

Now repeated calls with the same `(variant, flags, src)` reuse the same `Job` (and its outputs).

## Pattern: read a config file at build time

```wake
require Pass configFile = source "config.json"
require Pass raw        = read configFile
require Pass json       = parseJSON raw
# now traverse `json` (a JValue) to extract fields
```

## Resetting state

- Wipe local cache & history: `rm -rf wake.db .build` then `wake --init .`.
- Force re-run a specific output: `rm <output>; wake build …` (Wake will detect the missing file and re-run the producing job).
- Rebuild from scratch including bootstrap: `make clean && make`.
