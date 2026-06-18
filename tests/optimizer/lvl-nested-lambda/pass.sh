#! /bin/sh

# LVL through nested lambdas.  An invariant independent of both lambda arguments must be hoisted
# out past both lambdas; a term depending on the inner argument stays deep in the inner body.
#
#   hoistedMarker  -- independent of x and y -> lifted to the outermost scope
#   retainedMarker -- depends on inner arg y -> stays in the inner lambda body
#
# def labels survive into --stop-after-ssa as "(name)"; the line's leading-space count is its
# scope depth.  We assert hoistedMarker ends up at a strictly shallower indentation than
# retainedMarker.

set -e
WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

marker_indent() {
  line=$(grep -E "\($1\)" "$2" | head -1)
  if [ -z "$line" ]; then echo "FAIL: marker '$1' not found in SSA dump" >&2; exit 1; fi
  printf '%s\n' "$line" | sed -e 's/[^ ].*$//' | awk '{print length; exit}'
}

"${WAKE}" --stop-after-ssa -x 'nested Nil Nil' > ssa.txt 2>/dev/null

hoisted=$(marker_indent hoistedMarker ssa.txt)
retained=$(marker_indent retainedMarker ssa.txt)

if [ "$hoisted" -lt "$retained" ]; then
  echo "OK: invariant hoisted out of nested lambdas (indent $hoisted) above inner body (indent $retained)"
else
  echo "FAIL: hoistedMarker (indent $hoisted) was not lifted above the inner lambda body (indent $retained)"
  exit 1
fi

rm -f ssa.txt
