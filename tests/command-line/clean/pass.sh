#! /bin/sh

set -e
WAKE="${1:+$1/wake}"

"${WAKE:-wake}" -q --init .
"${WAKE:-wake}" -q -x 'write "bug" ""'
"${WAKE:-wake}" --clean
