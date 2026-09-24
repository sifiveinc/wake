#! /bin/sh

set -e
WAKE="${1:+$1/wake}"

RM_ARTIFACTS="output-minimal.json output-compact.json output-full.json"
rm -f wake.db wake.log $RM_ARTIFACTS

"${WAKE:-wake}" --stdout=warning,report testWriteMinimal
diff -u reference-minimal.json output-minimal.json

"${WAKE:-wake}" --stdout=warning,report testWriteCompact
diff -u reference-compact.json output-compact.json

"${WAKE:-wake}" --stdout=warning,report testWriteFull
diff -u reference-full.json output-full.json

rm -f $RM_ARTIFACTS
