#!/bin/sh
# Tests for --prune: drops DB entries for jobs with missing or changed outputs

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

cleanup() {
  rm -rf wake.db* wake.log .wake output.txt
}
trap cleanup EXIT
cleanup

# --- Test 1: deleted output is pruned ---
"${WAKE}" -q --no-tty -x 'makeOutput Unit'
rm -f output.txt
if "${WAKE}" --prune 2>&1 | grep -q "Pruned 1"; then
  echo "PASS: deleted output pruned"
else
  echo "FAIL: expected 1 job pruned after deleting output"
  exit 1
fi

# --- Test 2: present, unmodified output is kept ---
"${WAKE}" -q --no-tty -x 'makeOutput Unit'
if "${WAKE}" --prune 2>&1 | grep -q "Pruned 0"; then
  echo "PASS: present output kept"
else
  echo "FAIL: expected 0 jobs pruned when output exists and is unmodified"
  exit 1
fi

# --- Test 3: output with changed content (mtime/hash differ) is kept ---
# prune only checks existence+type, not content; wake handles staleness on next build
echo changed > output.txt
if "${WAKE}" --prune 2>&1 | grep -q "Pruned 0"; then
  echo "PASS: content-changed output not pruned"
else
  echo "FAIL: expected 0 jobs pruned when output still exists as correct type"
  exit 1
fi

# --- Test 4: file replaced by symlink (type mismatch) is pruned ---
"${WAKE}" -q --no-tty -x 'makeOutput Unit'
rm -f output.txt
ln -sf /dev/null output.txt
if "${WAKE}" --prune 2>&1 | grep -q "Pruned 1"; then
  echo "PASS: type-mismatch output pruned"
else
  echo "FAIL: expected 1 job pruned after file-to-symlink replacement"
  exit 1
fi
