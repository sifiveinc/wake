#! /bin/sh

# LVL hoists a pure, total, argument-independent invariant out of a lambda body.
#
# test.wake names two invariants with `def`s so we can find them in the SSA dump by label:
#   hoistedMarker  -- total + argument-independent -> lifted to an outer (shallower) scope
#   retainedMarker -- depends on the argument       -> stays in the per-call lambda body
#
# def labels survive into --stop-after-ssa as "(name)"; the line's leading-space count is its
# scope depth.  A hoisted invariant ends up at a strictly shallower indentation than a term that
# must stay in the lambda body.

set -e
WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

# Indentation (scope depth) of the SSA line carrying the given def label.
marker_indent() {
  line=$(grep -E "\($1\)" "$2" | head -1)
  if [ -z "$line" ]; then echo "FAIL: marker '$1' not found in SSA dump" >&2; exit 1; fi
  printf '%s\n' "$line" | sed -e 's/[^ ].*$//' | awk '{print length; exit}'
}

"${WAKE}" --stop-after-ssa -x 'hoistMe Nil' > ssa.txt 2>/dev/null

hoisted=$(marker_indent hoistedMarker ssa.txt)
retained=$(marker_indent retainedMarker ssa.txt)

if [ "$hoisted" -lt "$retained" ]; then
  echo "OK: hoistedMarker lifted to outer scope (indent $hoisted) above lambda body (indent $retained)"
else
  echo "FAIL: hoistedMarker (indent $hoisted) was not lifted above the lambda body (indent $retained)"
  exit 1
fi

rm -f ssa.txt
