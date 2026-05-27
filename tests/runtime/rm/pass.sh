#!/bin/sh
# Tests for --rm: basic smoke test that the flag is hooked up

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

rm -rf wake.db* wake.log .wake output.txt

# Test single path
"${WAKE}" --rm output.txt

# Test multiple paths (subcommand style)
"${WAKE}" --rm foo.txt bar.txt baz.txt

# Test glob expansion (create some files first for the glob to expand into)
touch glob1.txt glob2.txt
"${WAKE}" --rm glob*.txt

# Clean up
rm -f glob*.txt
