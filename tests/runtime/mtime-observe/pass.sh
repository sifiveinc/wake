#!/bin/sh
# Check setting a file's mtime and then observing it works.

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

RM_ARTIFACTS="output.txt"
rm -rf .build .fuse wake.db* wake.log $RM_ARTIFACTS

"${WAKE}" -x "go Unit"

# Clean up
rm -f $RM_ARTIFACTS
