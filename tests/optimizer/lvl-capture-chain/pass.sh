#! /bin/sh

# LVL hoists an invariant that depends on a CAPTURED value (still in scope at the hoist site),
# but not one that depends on the lambda's own argument.
#
#   captureMarker  -- depends on captured `c` -> lifted to an outer (shallower) scope
#   retainedMarker -- depends on argument `x` -> stays in the per-call lambda body
#
# def labels survive into --stop-after-ssa as "(name)"; the line's leading-space count is its
# scope depth.  We assert captureMarker ends up at a strictly shallower indentation than
# retainedMarker.

set -e
WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

marker_indent() {
  line=$(grep -E "\($1\)" "$2" | head -1)
  if [ -z "$line" ]; then echo "FAIL: marker '$1' not found in SSA dump" >&2; exit 1; fi
  printf '%s\n' "$line" | sed -e 's/[^ ].*$//' | awk '{print length; exit}'
}

"${WAKE}" --stop-after-ssa -x 'captureChain 5 Nil' > ssa.txt 2>/dev/null

capture=$(marker_indent captureMarker ssa.txt)
retained=$(marker_indent retainedMarker ssa.txt)

if [ "$capture" -lt "$retained" ]; then
  echo "OK: capture-dependent invariant hoisted (indent $capture) above lambda body (indent $retained)"
else
  echo "FAIL: captureMarker (indent $capture) was not hoisted above the lambda body (indent $retained)"
  exit 1
fi

rm -f ssa.txt
