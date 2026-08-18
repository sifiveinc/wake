#! /bin/sh

# LVL safety: a PURE but PARTIAL primitive (integer division, which aborts on a zero divisor)
# must NOT be hoisted out of a lambda body, because the lambda may run zero times.  A total `+`
# over different literals is the control: it MUST still hoist, so a regression in either
# direction is caught.
#
# def labels survive into --stop-after-ssa as "(name)"; the line's leading-space count is its
# scope depth.  We assert the total hoistedMarker ends up at a strictly shallower indentation
# (an outer scope) than the partialMarker, which must remain inside the lambda body.

set -e
WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

marker_indent() {
  line=$(grep -E "\($1\)" "$2" | head -1)
  if [ -z "$line" ]; then echo "FAIL: marker '$1' not found in SSA dump" >&2; exit 1; fi
  printf '%s\n' "$line" | sed -e 's/[^ ].*$//' | awk '{print length; exit}'
}

"${WAKE}" --stop-after-ssa -x 'dontHoist Nil' > ssa.txt 2>/dev/null

partial=$(marker_indent partialMarker ssa.txt)
hoisted=$(marker_indent hoistedMarker ssa.txt)

if [ "$hoisted" -lt "$partial" ]; then
  echo "OK: partialMarker stayed in lambda body (indent $partial); total control hoisted (indent $hoisted)"
else
  echo "FAIL: partialMarker (indent $partial) was hoisted out of the lambda (control indent $hoisted)"
  exit 1
fi

rm -f ssa.txt
