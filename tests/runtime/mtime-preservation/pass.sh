#!/bin/sh
# Two runs write same data to same file, with downstream jobs reading it.
# They do this with different modification times.
# Check each run gets the expected data, and check reuse works as well.

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

RM_ARTIFACTS="output.txt result-a.txt result-b.txt"
rm -rf .build .fuse wake.db* wake.log $RM_ARTIFACTS

echo "Fresh concurrent (if supported) runs:"

"${WAKE}" -q --no-tty -x "consumerA Unit" &
"${WAKE}" -q --no-tty -x "consumerB Unit" &

wait

# (output.txt can have either value)
tail result-a.txt result-b.txt

echo
echo "Reuse:"

"${WAKE}" -q --no-tty -x "consumerA Unit" &
"${WAKE}" -q --no-tty -x "consumerB Unit" &

wait

# (output.txt can have either value)
tail result-a.txt result-b.txt

# Clean up
rm -f $RM_ARTIFACTS
