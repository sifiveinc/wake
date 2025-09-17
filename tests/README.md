In order to create a new test, you need three things:
  1. a directory wake/tests/category/testname
  2. an empty `.wakeroot` file in that directory
  3. an executable `pass.sh` or `fail.sh` in that directory

All of these files must be `git add`-ed.

Your shell script will be provided with the path containing the wake executable
to test as its only option. It is helpful to default to just 'wake' for that
option so that the test can be run individually from the command-line.
This can be achieved by setting up the wake path to include the parameter:
```bash
WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"
```
...then invoke wake with `"$WAKE"`.

To pass:
  - `pass.sh` scripts are expected to exit with 0
  - `fail.sh` scripts are expected to NOT exit with 0
  - if a file `stdout` exists; the shell script must produce this standard output
  - if a file `stderr` exists; the shell script must produce this standard error

Generally, tests also include a `test.wake` which the shell script runs.
