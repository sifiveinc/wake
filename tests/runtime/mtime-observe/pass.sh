#!/bin/sh
# Check setting a file's mtime and then observing it works.

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

export WAKE_CAS=1

rm -rf .build .fuse wake.db* wake.log output.txt

"${WAKE}" -x "go Unit"
