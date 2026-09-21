#!/bin/sh

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

test "$("${WAKE}" --property test.value=command-line -x 'systemProperty Unit')" = '"command-line"'
test "$("${WAKE}" --property test.value=command-line -x 'missingSystemProperty Unit')" = '"absent"'
test "$("${WAKE}" --property test.value=command-line -x 'environmentProperty Unit')" = '"absent"'
