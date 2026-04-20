#!/bin/sh
# Test that file overwriting works correctly in CAS mode.
# When a file is overwritten, the old staging file should be deleted
# and a new one created.
# Verifies that:
# 1. File can be overwritten multiple times
# 2. Each read returns the current content
# 3. Final content is correct (last cat returns "version 3", not an earlier write)
set -eu

export PATH=/bin:/sbin:/usr/bin:/usr/sbin:/usr/local/bin
export WAKE_CAS=1

trap 'rm -f overwrite.txt' EXIT

OUTPUT=$(${1}/wakebox -p input.json 2>&1)

for v in "version 1" "version 2" "version 3"; do
    echo "$OUTPUT" | grep -q "$v" || { echo "FAIL: '$v' missing from output"; echo "$OUTPUT"; exit 1; }
done

LAST=$(echo "$OUTPUT" | tail -1)
[ "$LAST" = "version 3" ] || { echo "FAIL: final content is '$LAST', expected 'version 3'"; exit 1; }

echo "PASS"
