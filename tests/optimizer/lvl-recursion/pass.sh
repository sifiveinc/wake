#! /bin/sh

# LVL must not hoist recursive calls (RApp is never total).  Were it to do so, these recursions
# would run unconditionally -- diverging or, for the list build, exhausting memory.  We bound
# each run with a timeout so a regression manifests as a failure rather than a hang.

set -e
WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

run() {
  # Prefer a hard timeout if available so a hoisting regression fails fast instead of hanging.
  if command -v timeout >/dev/null 2>&1; then
    timeout 60 "$@"
  else
    "$@"
  fi
}

# Plain numeric recursion must terminate and return 0.
out=$(run "${WAKE}" -x 'countdown 200000' 2>&1)
if [ "$out" != "0" ]; then
  echo "FAIL: countdown did not terminate cleanly with 0 (got: $out)"
  exit 1
fi
echo "OK: numeric recursion terminates (recursive call not hoisted)"

# List-building recursion must terminate; check it produced the right number of elements.
len=$(run "${WAKE}" -x 'len (buildList 50000)' 2>&1)
if [ "$len" != "50000" ]; then
  echo "FAIL: buildList did not terminate with expected length 50000 (got: $len)"
  exit 1
fi
echo "OK: allocation-shaped recursion terminates without memory blowup"
