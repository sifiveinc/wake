#!/bin/sh
# Regression test for read-only inspection requiring SQLite's WAL shared-memory file.
#
# This was deliberately a fail.sh while the bug was present: --last was expected to
# exit non-zero with the sqlite3_prepare_v2 get_entropy read-only diagnostic. Once
# the bug is fixed, the test is now pass.sh and the same setup requires --last
# to succeed.
#
# The expected diagnostic is kept in stderr as a golden file. When the fix lands,
# the test is pass.sh and stderr is empty to require a clean inspection.

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

cleanup() {
  # Restore both permissions even when a command or assertion fails. SQLite needs
  # the directory writable while the fixture is being created and cleaned up.
  if [ -e wake.db ]; then
    chmod 0644 wake.db 2>/dev/null || true
  fi
  chmod 0755 . 2>/dev/null || true
  rm -rf .wake wake.db* wake.log
}
trap cleanup EXIT
rm -rf .wake wake.db* wake.log
cleanup

# A real job, rather than --clean/--list-outputs/--prune/--rm, fully populates
# entropy through Wake's normal write path and gives --last something to inspect.
"${WAKE}" -x 'makePlan "true" Nil "true" | runJobWith stagingDirRunner'

# Confirm the genuine database has WAL mode, the current schema, and both entropy
# rows before making the workspace read-only. This uses Python only for inspection.
python3 - <<'PY'
import sqlite3
con = sqlite3.connect("wake.db")
assert con.execute("pragma journal_mode").fetchone()[0] == "wal"
assert con.execute("pragma user_version").fetchone()[0] == 17
assert con.execute("select count(*) from entropy").fetchone()[0] == 2
con.close()
PY

# Remove sidecars so read-only SQLite must create wake.db-shm, then block both the
# database and its containing directory.
rm -f wake.db-wal wake.db-shm
chmod 0444 wake.db
chmod 0555 .

# The invocation must succeed (i.e. it uses the "immutable" fallback mode to read).
"${WAKE}" --last
