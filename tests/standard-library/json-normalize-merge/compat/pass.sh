#! /bin/sh

set -e
WAKE="${1:+$1/wake}"

ln -f ../json-test.wake .
"${WAKE:-wake}" --quiet --stdout=warning,report 'jsonTest normalizeJSONCompat' \
    infinity.json \
    nan.json \
    unicode.json \
    unicode-merge.json
rm json-test.wake
rm -r build
