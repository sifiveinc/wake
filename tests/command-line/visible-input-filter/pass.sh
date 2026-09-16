#!/bin/sh

set -e
WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

"${WAKE}" test
"${WAKE}" --input visible.txt --label '*' --simple-metadata \
    | grep -v 'Built\|Runtime\|CPUtime\|Mem bytes'
"${WAKE}" --input visible.txt --label '*' --verbose \
    | grep -v 'Built\|Runtime\|CPUtime\|Mem bytes'
