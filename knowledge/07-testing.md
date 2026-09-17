# Testing

Most testing is **black-box, golden-file** integration testing. C++ unit testing is a small slice run separately.

Authoritative reference: `tests/README.md`.

## Layout

```
tests/
├── README.md
├── tests.wake              # publishes runTests / runUnitTests entry points
├── command-line/           # CLI flag behavior
├── config/                 # .wakeroot / wake.json parsing
├── dst/                    # desugaring & binding
├── inspection/             # `wake --last`, `--failed`, `-o`, `-i`, etc.
├── parser/                 # syntax & lexer corner cases
├── pending/                # stashed/skipped tests (do not run)
├── remote-cache/           # RSC client/server interaction
├── runtime/                # job execution, caching, mtime, sandbox
├── standard-library/       # stdlib function behavior
├── type-system/            # inference, errors, ADTs
├── wake-format/            # formatter golden output
├── wake-migrate/           # DB schema migrations
├── wake-unit/              # C++ unit tests
└── wakebox/                # sandbox tool
```

## Test layout (per test)

Each test is a directory containing some subset of:

| File | Required? | Purpose |
|---|---|---|
| `.wakeroot` | yes | Empty file marking this directory as a Wake workspace. |
| `pass.sh` **or** `fail.sh` | exactly one | Executable. Receives the `wake` binary path as `$1` (default: `wake`). Test passes if `pass.sh` exits 0, or if `fail.sh` exits non-zero. |
| `stdout` | optional | Expected stdout (golden compared after script exits). |
| `stderr` | optional | Expected stderr. |
| `test.wake` | optional | Wake source the script invokes. |
| `wake.json`, custom files | optional | Whatever the test needs. |

Minimal `pass.sh`:
```bash
#!/bin/sh
"${1:-wake}" -x '1 + 1'
```

The harness diffs actual vs. golden stdout/stderr and reports.

## Running tests

```bash
make test               # all integration tests under tests/ (excluding remote-cache)
make unittest           # C++ unit tests via tools/wake-unit
make remoteCacheTests   # RSC end-to-end (requires PostgreSQL — see scripts/)
```

CI runs all three (see `.github/workflows/build.yaml`). `make test` is what you should run before opening a PR.

To run a single test by hand:
```bash
cd tests/<category>/<name>/
./pass.sh ../../../bin/wake     # or wherever your wake binary lives
```

## Adding a regression test

1. Pick a category (or create a new directory under `tests/` if no fit exists).
2. `mkdir tests/<category>/<name>` and `cd` in.
3. `touch .wakeroot`.
4. Write `pass.sh` (or `fail.sh`); make it executable (`chmod +x`).
5. Optionally write `stdout`/`stderr` golden files for output assertions.
6. Run it locally; iterate until green; commit.

Conventions:
- Keep `pass.sh` POSIX-shell-portable (`#!/bin/sh`, no bashisms unless a specific test needs them — there are some bash tests).
- Don't depend on absolute paths or system-installed wake — use `${1:-wake}`.
- Don't require network access (RSC tests are the deliberate exception, gated to `make remoteCacheTests`).
- Goldens use the exact bytes wake emits; if you regenerate, eyeball the diff before committing.

## Adding a unit test

Unit tests live under `tests/wake-unit/`. They are C++ compiled into the `wake-unit` binary (in `tools/wake-unit/`) and run by `make unittest`. Smaller and faster than integration tests; reach for them when testing a piece of C++ runtime in isolation.

## Pending tests

`tests/pending/` is the parking lot for tests that are intentionally not run (perhaps for known bugs or future features). Don't move tests there casually.
