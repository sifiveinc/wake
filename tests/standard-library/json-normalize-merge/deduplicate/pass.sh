#! /bin/sh

set -e
WAKE="${1:+$1/wake}"

ln -f ../json-test.wake .
"${WAKE:-wake}" --quiet --stdout=warning,report 'jsonTest normalizeJSONCompat' \
    valid.json \
    valid-nesting.json \
    invalid-boolean.json \
    invalid-double.json \
    invalid-int.json \
    invalid-string.json \
    invalid-nesting.json
rm json-test.wake
rm -r build
