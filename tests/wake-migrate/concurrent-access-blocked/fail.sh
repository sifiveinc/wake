#! /bin/sh
# Test: wake-migrate fails when database is held open by another process.
set -eu

MIGRATE="${1:+$1/wake-migrate}"
MIGRATE="${MIGRATE:-wake-migrate}"
cd "$(dirname "$0")"

RM_ARTIFACTS="commands ready"
cleanup() {
  if [ -n "${S_PID:-}" ]; then
    kill "$S_PID" 2>/dev/null || true
    wait "$S_PID" 2>/dev/null || true
  fi
  rm -f $RM_ARTIFACTS
}
trap cleanup EXIT
rm -f wake.db*
cleanup

# Create dummy "version 8" database.
sqlite3 wake.db "PRAGMA journal_mode=WAL; PRAGMA user_version=8; CREATE TABLE t(x);"

# Read transaction's SHARED lock prevents wake-migrate getting EXCLUSIVE.
# 'ready' fifo is used to wait until sqlite opens the transaction.
# 'commands' is input, intentionally kept open.
mkfifo ready commands
exec 3<> commands
sqlite3 wake.db > ready < commands &
S_PID=$!

printf 'BEGIN;\nSELECT * FROM t;\n.print READY\n' >&3

read _ < ready

# Migration should fail due to concurrent access
"$MIGRATE" wake.db
