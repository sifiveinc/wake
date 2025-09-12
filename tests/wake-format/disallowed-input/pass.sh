#! /bin/sh

set -e

tmp=$(mktemp ./basic.wake.XXXXXX)
out=$(mktemp ./basic.wake.out.XXXXXX)

# Running wake-format on itself shouldn't create any new changes
"${1}/wake-format" disallowed-input.wake > "$tmp"
"${1}/wake-format" "$tmp" > "$out"

# nothing output if they are the same
diff "$out" "$tmp"

# re-emit to compare against stdout
cat "$out"

rm "$out"
rm "$tmp"
