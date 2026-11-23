#!/bin/sh
# Regression test for entropy() writing from a read-only inspection connection.
#
# This is deliberately a fail.sh while the bug is present: --last is expected to
# exit non-zero with the begin-immediate-transaction error. Once the bug is
# fixed, convert this file to pass.sh and invert the final status assertion so
# that the same setup requires --last to succeed.
#
# The expected diagnostic is kept in stderr as a golden file. When the fix lands,
# convert this to pass.sh and empty stderr to require a clean inspection.

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

cleanup() {
  # Restore permissions even when a command or assertion fails. The directory
  # remains writable intentionally: SQLite must be able to create WAL sidecars.
  if [ -e wake.db ]; then
    chmod 0644 wake.db 2>/dev/null || true
  fi
  rm -f wake.db-wal wake.db-shm wake.log
}
trap cleanup EXIT
rm -rf wake.db* wake.log .wake
cleanup

# This must be the first wake invocation: it creates the real schema with an
# empty entropy table, without any hand-built database state.
"${WAKE}" --clean

# Make only the database file read-only; do not chmod the containing directory.
chmod 0444 wake.db

# The empty database makes --last return status 1 for the normal no-jobs result.
# The harness compares stderr with the golden file, so only that expected status
# is accepted here; the read-only entropy write failure has a different diagnostic.
set +e
"${WAKE}" --last
[ $? -eq 1 ]
