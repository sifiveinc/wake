#! /bin/sh
# Test: wake-migrate fails when database is held open by another process.
set -eu

MIGRATE="${1:+$1/wake-migrate}"
MIGRATE="${MIGRATE:-wake-migrate}"
cd "$(dirname "$0")"

HOLDER=""
FIFO_WRITER=""
cleanup() {
  for pid in $FIFO_WRITER $HOLDER; do
    if [ -n "$pid" ]; then
      kill "$pid" 2>/dev/null || true
      wait "$pid" 2>/dev/null || true
    fi
  done
  rm -f fifo ready wake.db wake.db-wal wake.db-shm wake.db.backup wake.db.migrated
}
trap cleanup EXIT
cleanup

# Create version 8 database (minimal schema; fails at checkpoint before migration runs)
sqlite3 wake.db "PRAGMA journal_mode=WAL; PRAGMA user_version=8; CREATE TABLE t(x);"

# Read transaction's SHARED lock prevents wake-migrate getting EXCLUSIVE.
# 'ready' fifo synchronizes: sqlite3 signals when it has the lock.
mkfifo fifo ready
printf 'BEGIN;\nSELECT * FROM t;\n.print READY\n.read fifo\n' | sqlite3 wake.db > ready &
HOLDER=$!
read _ < ready

# Migration should fail due to concurrent access
OUTPUT=$("$MIGRATE" wake.db 2>&1) || true

# Release holder (backgrounded in case sqlite3 already exited; cleanup kills if stuck)
echo "" > fifo &
FIFO_WRITER=$!

if echo "$OUTPUT" | grep -qE "(Checkpoint blocked|database is locked)"; then
  echo "PASS: wake-migrate correctly detected concurrent access"
  exit 0
fi

echo "FAIL: Expected concurrent access error, got: $OUTPUT" >&2
exit 1
