#!/bin/sh
# Test --checkout-to: materializes job outputs into a destination directory.

set -eu

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

cleanup() {
  rm -rf wake.db* wake.log .wake .build out out2 nonempty notadir hello.txt
}
trap cleanup EXIT
cleanup

"${WAKE}" -x 'build Unit' >/dev/null

# 1. Destination doesn't exist -- created and populated.
echo "== fresh =="
"${WAKE}" --checkout-to out
(cd out && find . -mindepth 1 | sort)
printf 'content: %s\n' "$(cat out/hello.txt)"

# 2. Empty pre-existing dir -- also accepted.
echo "== empty existing =="
mkdir out2
"${WAKE}" --checkout-to out2
(cd out2 && find . -mindepth 1 | sort)

# 3. Non-empty pre-existing dir -- refused, contents untouched.
echo "== non-empty existing =="
mkdir nonempty
touch nonempty/preexisting
"${WAKE}" --checkout-to nonempty || echo "exit=$?"
(cd nonempty && find . -mindepth 1 | sort)

# 4. Existing path that isn't a directory -- refused.
echo "== not-a-directory =="
touch notadir
"${WAKE}" --checkout-to notadir || echo "exit=$?"
