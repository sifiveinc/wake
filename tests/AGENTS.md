# tests/ — Black-box Regression Tests

See also: [`README.md`](README.md) (canonical conventions) and [`../knowledge/07-testing.md`](../knowledge/07-testing.md) (deeper guide).

## What's here

```
tests/
├── README.md           ← read this first if you've never written a wake test
├── tests.wake          test harness; publishes `runTests` / `runUnitTests`
├── command-line/       CLI flag behavior
├── config/             .wakeroot / wake.json parsing
├── dst/                desugaring & binding
├── inspection/         `wake --last`, `--failed`, `-o`, `-i`, etc.
├── parser/             syntax & lexer corner cases
├── pending/            stashed tests (do NOT run; do NOT casually move tests here)
├── remote-cache/       RSC client/server (needs PostgreSQL)
├── runtime/            job execution, caching, mtime, sandbox
├── standard-library/   stdlib functions
├── type-system/        inference, errors, ADTs
├── wake-format/        formatter golden output
├── wake-migrate/       wake.db schema migrations
├── wake-unit/          C++ unit tests (compiled into wake-unit binary)
└── wakebox/            sandbox tool
```

## Test convention (per directory)

Each test is a directory containing some subset of:

| File | Required? | Purpose |
|---|---|---|
| `.wakeroot` | yes | Empty file marking this directory as a Wake workspace |
| `pass.sh` **or** `fail.sh` | exactly one | Executable; receives wake binary path as `$1` (default `wake`); test passes when the script's exit semantics match its name |
| `stdout`, `stderr` | optional | Golden files (byte-exact match against actual output) |
| `test.wake`, fixture files | optional | Whatever the test needs |

Minimal `pass.sh`:
```sh
#!/bin/sh
"${1:-wake}" -x '1 + 1'
```

Don't depend on system-installed `wake`; always use `${1:-wake}`. Don't require network (the deliberate exception is `remote-cache/`, gated to `make remoteCacheTests`).

## Running

```bash
make test                  # all integration tests except remote-cache
make unittest              # C++ unit tests
make remoteCacheTests      # RSC end-to-end (PostgreSQL service required)

# single test by hand
cd tests/<category>/<name>
./pass.sh ../../../bin/wake
```

CI (`.github/workflows/build.yaml`) runs all three.

## Adding a regression test

```bash
mkdir -p tests/<category>/<name>
cd tests/<category>/<name>
touch .wakeroot
# write test.wake, pass.sh (or fail.sh), optional stdout/stderr
chmod +x pass.sh
cd ../../..
make test
```

## Adding a C++ unit test

Land it under `tests/wake-unit/`. The runner binary is built from `../tools/wake-unit/`. Run via `make unittest`.

## House style

- Keep `pass.sh` POSIX-shell-portable (`#!/bin/sh`); only use bash when the test specifically exercises bash-isms.
- Goldens are exact bytes — eyeball any diff before re-recording.
- Don't depend on absolute paths or environment variables; let the harness control them.
- Don't park real failures in `pending/` to bypass CI. That directory exists for tests intentionally held out (known issues, future features).
- A test that produces FUSE traffic will fail in CI sandboxes without kernel support — keep those scoped to `runtime/` or `wakebox/` and ensure the harness handles the skip.
