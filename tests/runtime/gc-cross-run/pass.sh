#! /bin/sh
# GC should delete old job when new job outputs same file (different runs)

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

rm -f wake.db* wake.log shared.txt

# Run 1: job1 creates shared.txt
"${WAKE}" -q -x 'job1 Unit'

# Run 2: job2 creates shared.txt, should GC job1
"${WAKE}" -q -x 'job2 Unit'

# Only job2 should remain (job1 was GC'd)
output=$("${WAKE}" --output '*shared.txt' --simple 2>&1)

if echo "$output" | grep -q 'job1'; then
  echo "FAIL: job1 should have been GC'd"
  exit 1
fi

if ! echo "$output" | grep -q 'job2'; then
  echo "FAIL: job2 should produce shared.txt"
  exit 1
fi

# Verify shared.txt has job2's content
content=$(cat shared.txt)
if [ "$content" != "job2" ]; then
  echo "FAIL: Expected 'job2', got '$content'"
  exit 1
fi

rm -f wake.db* wake.log shared.txt
