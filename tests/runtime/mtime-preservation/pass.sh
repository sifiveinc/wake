#!/bin/sh
# Two runs write same data to same file, with downstream jobs reading it.
# They do this with different modification times.
# Check each run gets the expected data, and check reuse works as well.

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

export WAKE_CAS=1

rm -rf .build .fuse wake.db* wake.log output.txt result-a.txt result-b.txt

echo "Fresh runs:"

"${WAKE}" -q --no-tty -x "consumerA Unit"
"${WAKE}" -q --no-tty -x "consumerB Unit"

tail result-a.txt result-b.txt

echo
echo "Reuse:"

"${WAKE}" -q --no-tty -x "consumerA Unit"
"${WAKE}" -q --no-tty -x "consumerB Unit"

# (output.txt can have either value)
tail result-a.txt result-b.txt
