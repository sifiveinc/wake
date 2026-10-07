#!/bin/sh

set -e
WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

"${WAKE}" test
"${WAKE}" --input visible.txt --label '*' --simple-metadata \
    | grep -v 'Built\|Runtime\|CPUtime\|Mem bytes' \
    | sed 's/^Job [0-9]\+/Job X/' # Job numbering isn't consistent between different CI containers.
"${WAKE}" --input visible.txt --label '*' --verbose \
    | grep -v 'Built\|Runtime\|CPUtime\|Mem bytes\|Wake run' \
    | sed 's/^Job [0-9]\+/Job X/'
