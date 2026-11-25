#! /bin/bash
#
# kb-bless.sh — Mark the KB and AGENTS.md files as up-to-date with HEAD.
#
# Records the current HEAD SHA in knowledge/.last-validated-sha. Run this
# AFTER you've reviewed kb-check.sh's delta report and updated whichever
# KB/AGENTS files needed updating.
#
# Refuses to run if the working tree has uncommitted changes to knowledge/
# or any AGENTS.md, since those changes should be committed first so the
# blessed SHA actually corresponds to a published state of the docs.
#
# Flags:
#   --force   bypass the dirty-tree check (use sparingly).

set -euo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" || {
    echo "kb-bless: not inside a git repository" >&2
    exit 2
}
cd "$REPO_ROOT"

FORCE=0
for arg in "$@"; do
    case "$arg" in
        --force) FORCE=1 ;;
        -h|--help)
            sed -n '2,/^$/p' "$0" | sed 's/^# \{0,1\}//'
            exit 0
            ;;
        *)
            echo "kb-bless: unknown flag: $arg" >&2
            exit 2
            ;;
    esac
done

SHA_FILE="knowledge/.last-validated-sha"
HEAD_SHA="$(git rev-parse HEAD)"
PREV_SHA=""
[ -f "$SHA_FILE" ] && PREV_SHA="$(tr -d '[:space:]' < "$SHA_FILE")"

if [ "$FORCE" -eq 0 ]; then
    DIRTY="$(git status --porcelain -- knowledge AGENTS.md '**/AGENTS.md' 2>/dev/null || true)"
    if [ -n "$DIRTY" ]; then
        echo "kb-bless: refusing to bless — KB/AGENTS files have uncommitted changes:" >&2
        printf '%s\n' "$DIRTY" >&2
        echo "Commit them first, then re-run. Or pass --force to override." >&2
        exit 1
    fi
fi

if [ "$PREV_SHA" = "$HEAD_SHA" ]; then
    echo "kb-bless: already at $HEAD_SHA — nothing to do."
    exit 0
fi

printf '%s\n' "$HEAD_SHA" > "$SHA_FILE"
echo "kb-bless: $SHA_FILE  $PREV_SHA → $HEAD_SHA"
echo "Stage and commit:"
echo "  git add $SHA_FILE && git commit -m 'kb: bless $HEAD_SHA'"
