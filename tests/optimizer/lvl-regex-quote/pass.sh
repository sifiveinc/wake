#! /bin/sh

# A regex/quote memory-blowup scenario.
#
#   quoteMarker -- `quote` builds a RegExp from a constant string; pure + TOTAL -> MUST be
#                  hoisted, so the pattern is compiled once and shared.
#   matchMarker -- `matches` uses the PRIM_PARTIAL `match` prim (can abort on huge inputs);
#                  even with constant arguments it MUST stay inside the lambda body.
#
# def labels survive into --stop-after-ssa as "(name)"; the line's leading-space count is its
# scope depth.  We assert quoteMarker ends up at a strictly shallower indentation (an outer
# scope) than matchMarker, which must remain in the per-call lambda body.

set -e
WAKE="${1:+$1/wake}"
WAKE="${WAKE:-wake}"

marker_indent() {
  line=$(grep -E "\($1\)" "$2" | head -1)
  if [ -z "$line" ]; then echo "FAIL: marker '$1' not found in SSA dump" >&2; exit 1; fi
  printf '%s\n' "$line" | sed -e 's/[^ ].*$//' | awk '{print length; exit}'
}

"${WAKE}" --stop-after-ssa -x 'regexCase Nil' > ssa.txt 2>/dev/null

quote=$(marker_indent quoteMarker ssa.txt)
match=$(marker_indent matchMarker ssa.txt)

if [ "$quote" -lt "$match" ]; then
  echo "OK: quote regex hoisted/shared (indent $quote); partial match kept in lambda (indent $match)"
else
  echo "FAIL: quoteMarker (indent $quote) was not hoisted above the partial match (indent $match)"
  exit 1
fi

rm -f ssa.txt
