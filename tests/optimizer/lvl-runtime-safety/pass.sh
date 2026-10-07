#! /bin/sh

# Runtime safety of LVL.  A partial op (`x / n`) captured inside a lambda must stay inside it,
# so that for an empty list (lambda never invoked) the division never runs.
#
#   scaleAll 0 Nil       -> lambda never runs -> must SUCCEED (no division-by-zero)
#   scaleAll 0 (1, Nil)  -> lambda runs once  -> division-by-zero -> must FAIL
#
# The second case is a negative control: it proves the division really is partial, so the
# first case is a meaningful test rather than a no-op.

WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

# Case 1: uncalled lambda must not abort.
if ! "${WAKE}" -x 'scaleAll 0 Nil' >out1.txt 2>&1; then
  echo "FAIL: 'scaleAll 0 Nil' aborted -- partial division was wrongly hoisted/evaluated"
  cat out1.txt
  exit 1
fi
echo "OK: uncalled lambda with partial division did not abort"

# Case 2: called lambda must abort on division by zero (sanity control).
if "${WAKE}" -x 'scaleAll 0 (1, Nil)' >out2.txt 2>&1; then
  echo "FAIL: 'scaleAll 0 (1, Nil)' unexpectedly succeeded -- division is not actually partial"
  cat out2.txt
  exit 1
fi
echo "OK: invoked lambda aborts on division by zero (control)"

rm -f out1.txt out2.txt
