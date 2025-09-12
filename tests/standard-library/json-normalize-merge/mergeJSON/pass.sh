#! /bin/sh

set -e
WAKE="${1:+$1/wake}"

ln -f ../json-test.wake .
"${WAKE:-wake}" --quiet --stdout=warning,report 'jsonArrayTest mergeJSON' \
    recurse.json \
    override.json
rm json-test.wake
rm -r build
