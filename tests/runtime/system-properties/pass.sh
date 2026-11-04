#!/bin/sh

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

"${WAKE}" --property test.value=command-line systemProperty test.value
"${WAKE}" systemProperty missing.value
"${WAKE}" --property test.value= systemProperty test.value
# This is testing that environment variables of same name don't affect system properties
# The behavior of WAKE_PROPERTIES is tested in config/nominal
env 'test.value=environment' "${WAKE}" systemProperty test.value
