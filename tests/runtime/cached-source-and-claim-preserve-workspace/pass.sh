#!/bin/sh
# Checks that `source` and `claim` cache hits preserve existing files, including their inode, mode,
# and edited contents when the original mtime is retained.

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

rm -rf wake.db* wake.log .wake .build src.txt claimed.txt ref-src.txt ref-claimed.txt

fail() {
    echo "FAIL: $1" >&2
    exit 1
}

echo original-src > src.txt
echo original-claimed > claimed.txt
# Give claimed.txt a distinct, arbitrary-but-fixed mtime unrelated to `touch -r`'s "now" default,
# so we can restore it exactly after mutating content.
touch -d '2020-01-01 00:00:00 UTC' src.txt
touch -d '2020-06-01 00:00:00 UTC' claimed.txt
cp -p src.txt ref-src.txt
cp -p claimed.txt ref-claimed.txt

echo "=== Run 1: record source and claim ==="
"${WAKE}" -q --no-tty -x 'useSource Unit'
"${WAKE}" -q --no-tty -x 'useClaim Unit'

src_inode_before=$(ls -i src.txt | awk '{print $1}')
claim_inode_before=$(ls -i claimed.txt | awk '{print $1}')
chmod 0755 src.txt claimed.txt

echo "=== Run 2: matching cache hits leave source and claim inodes and modes unchanged ==="
"${WAKE}" -q --no-tty -x 'useSource Unit'
"${WAKE}" -q --no-tty -x 'useClaim Unit'

test "$src_inode_before" = "$(ls -i src.txt | awk '{print $1}')" ||
    fail "matching source cache hit changed its inode"
test "$claim_inode_before" = "$(ls -i claimed.txt | awk '{print $1}')" ||
    fail "matching claim cache hit changed its inode"
test "$(stat -c %a src.txt)" = 755 ||
    fail "matching source cache hit reset its mode"
test "$(stat -c %a claimed.txt)" = 755 ||
    fail "matching claim cache hit reset its mode"

# Mutate both files, but restore their exact original mtimes (nanosecond-preserving `cp -p`
# references) so their Keep virtual-job keys still select the historical cache records.
echo replacement-src > src.txt
echo replacement-claimed > claimed.txt
touch -r ref-src.txt src.txt
touch -r ref-claimed.txt claimed.txt

echo "=== Run 3: same mtime, different bytes -- cache hits must not rewrite either file ==="
"${WAKE}" -q --no-tty -x 'useSource Unit'
"${WAKE}" -q --no-tty -x 'useClaim Unit'

test "$(cat src.txt)" = "replacement-src" || fail "source bytes were rehydrated from stale cache"
test "$(cat claimed.txt)" = "replacement-claimed" || fail "claim bytes were rehydrated from stale cache"

echo "PASS: source and claim cache hits preserved workspace files" >&2

rm -rf wake.db* wake.log .wake .build src.txt claimed.txt ref-src.txt ref-claimed.txt
