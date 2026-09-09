#! /bin/bash
#
# kb-check.sh — Validate the knowledge base / AGENTS.md against the live tree.
#
# Three passes:
#   1. PATH    — every backtick-wrapped path in knowledge/ and AGENTS.md that
#                ends in a known source extension must resolve, either relative
#                to the citing file's directory or relative to the repo root.
#   2. SYMBOL  — a curated set of stdlib symbol citations is grepped for in the
#                live tree. Soft check: warn only.
#   3. DELTA   — diff HEAD against knowledge/.last-validated-sha; for each
#                changed file, print the KB topics most likely to need review.
#
# Exit codes:
#   0 — clean: zero hard errors AND no delta files mapped to KB topics.
#   1 — hard errors (missing paths) OR delta files mapping to KB topics.
#   2 — script-level failure (bad invocation, not a git repo, etc.).
#
# CI should treat exit 1 as a "review needed" gate. Run kb-bless.sh after
# updating the KB to clear the delta.
#
# Compatible with bash 3.2 (macOS ships 3.2 only).

set -uo pipefail

REPO_ROOT="$(git rev-parse --show-toplevel 2>/dev/null)" || {
    echo "kb-check: not inside a git repository" >&2
    exit 2
}
cd "$REPO_ROOT"

SHA_FILE="knowledge/.last-validated-sha"
if [ ! -f "$SHA_FILE" ]; then
    echo "kb-check: $SHA_FILE missing — run scripts/kb-bless.sh to seed it" >&2
    exit 2
fi
LAST_SHA="$(tr -d '[:space:]' < "$SHA_FILE")"
HEAD_SHA="$(git rev-parse HEAD)"

# Collect KB and AGENTS files.
KB_LIST="$(
    {
        find knowledge -maxdepth 2 -type f -name '*.md' 2>/dev/null
        find . -maxdepth 4 -name 'AGENTS.md' \
             -not -path './.git/*' \
             -not -path './node_modules/*' 2>/dev/null
    } | sort -u
)"

if [ -t 1 ]; then
    red()    { printf '\033[31m%s\033[0m' "$*"; }
    green()  { printf '\033[32m%s\033[0m' "$*"; }
    yellow() { printf '\033[33m%s\033[0m' "$*"; }
    bold()   { printf '\033[1m%s\033[0m'  "$*"; }
else
    red()    { printf '%s' "$*"; }
    green()  { printf '%s' "$*"; }
    yellow() { printf '%s' "$*"; }
    bold()   { printf '%s' "$*"; }
fi

bold "kb-check"; printf '  last-validated=%s  head=%s\n\n' "$LAST_SHA" "$HEAD_SHA"

PATH_LOG="$(mktemp -t kbcheck.XXXXXX)"
SYMBOL_LOG="$(mktemp -t kbcheck.XXXXXX)"
DELTA_LOG="$(mktemp -t kbcheck.XXXXXX)"
trap 'rm -f "$PATH_LOG" "$SYMBOL_LOG" "$DELTA_LOG"' EXIT

# Path-citation extensions we validate. Anything else (build outputs, globs,
# bare directories without an ext) is skipped — too noisy to verify reliably.
EXT_PATTERN='\.(wake|cpp|c|h|hpp|md|adoc|txt|json|json5|sh|y\.m4|re|in|toml|ts|yaml|yml|tmLanguage\.json)$'

# Try to resolve a token as a path. Returns 0 if found, 1 otherwise.
# Resolution order:
#   1. relative to the citing file's directory
#   2. relative to repo root
resolve_path() {
    local kb="$1"
    local tok="$2"
    local kb_dir
    kb_dir="$(dirname -- "$kb")"

    # Strip trailing punctuation that often sneaks in from prose.
    tok="${tok%.}"
    tok="${tok%,}"

    # Skip globs.
    case "$tok" in
        *'*'*|*'?'*|*'['*) return 2 ;;
    esac

    # Allowlist build artifacts that don't exist until `make`.
    case "$tok" in
        .build/|.build/*|bin/|bin/*|lib/wake/|lib/wake/*|wake.db) return 0 ;;
    esac

    # Resolve {h,cpp}-style brace expansions: success requires *all* expansions.
    if [[ "$tok" == *'{'*'}'* ]]; then
        local base="${tok%%\{*}"
        local rest="${tok#*\{}"
        local exts="${rest%%\}*}"
        local tail="${rest#*\}}"
        local missed=0
        local IFS=','
        local exts_arr=()
        for e in $exts; do exts_arr+=("$e"); done
        unset IFS
        for e in "${exts_arr[@]}"; do
            local cand="${base}${e}${tail}"
            if [ -e "$kb_dir/$cand" ] || [ -e "$cand" ]; then
                :
            else
                missed=1
                break
            fi
        done
        return "$missed"
    fi

    if [ -e "$kb_dir/$tok" ] || [ -e "$tok" ]; then
        return 0
    fi
    return 1
}

# --------------------------------------------------------------------------
# Pass 1: PATH existence
# --------------------------------------------------------------------------
bold "[1/3] path citations"; echo

: > "$PATH_LOG"

printf '%s\n' "$KB_LIST" | while IFS= read -r kb; do
    [ -n "$kb" ] || continue
    grep -oE '`[A-Za-z0-9_./{}*?-]+`' "$kb" 2>/dev/null \
        | sed 's/^`//; s/`$//' \
        | while IFS= read -r tok; do
            case "$tok" in
                http*|/*|~/*) continue ;;
            esac
            # Must look like a path.
            case "$tok" in
                */*) ;;
                *)   continue ;;
            esac
            # Must end in a known extension OR look like a directory we care
            # about (ends in /). Bare paths without ext we skip.
            if ! [[ "$tok" =~ $EXT_PATTERN ]] && [[ "$tok" != */ ]]; then
                continue
            fi
            printf '%s\t%s\n' "$kb" "$tok"
        done
done | while IFS=$'\t' read -r kb tok; do
    if resolve_path "$kb" "$tok"; then
        printf 'OK\t%s\t%s\n' "$kb" "$tok" >> "$PATH_LOG"
    else
        rc=$?
        if [ "$rc" -eq 2 ]; then
            : # glob, skipped
        else
            printf 'MISS\t%s\t%s\n' "$kb" "$tok" >> "$PATH_LOG"
        fi
    fi
done

PATH_OK=$(grep -c '^OK	' "$PATH_LOG" 2>/dev/null)
PATH_ERRORS=$(grep -c '^MISS	' "$PATH_LOG" 2>/dev/null)
PATH_OK="${PATH_OK:-0}"
PATH_ERRORS="${PATH_ERRORS:-0}"

if [ "$PATH_ERRORS" -gt 0 ]; then
    printf '  '; red "✗"; printf ' %s broken citation(s):\n' "$PATH_ERRORS"
    grep '^MISS	' "$PATH_LOG" | sort -u | while IFS=$'\t' read -r _ kb tok; do
        printf '    %s  ←  %s\n' "$tok" "$kb"
    done
else
    printf '  '; green "✓"; printf ' %s path citation(s) all resolve\n' "$PATH_OK"
fi
echo

# --------------------------------------------------------------------------
# Pass 2: SYMBOL existence (soft warning)
# --------------------------------------------------------------------------
bold "[2/3] symbol citations (best-effort)"; echo

SYMBOLS="
runJobWith makePlan makeExecPlan makeShellPlan makeRunner makeJSONRunner
localRunner defaultRunner virtualRunner workspaceRunner
compileC linkO subscribe publish
getJobStdout getJobStderr getJobStatus getJobInputs getJobOutputs
getJobReport getJobId getJobDescription
setPlanShare setPlanKeep setPlanOnce setPlanFilterOutputs setPlanEnvVar
findFail rmap omap
"

: > "$SYMBOL_LOG"
for sym in $SYMBOLS; do
    if grep -rq --include='*.wake' --include='*.cpp' --include='*.h' \
        -E "(^|[^A-Za-z0-9_])${sym}([^A-Za-z0-9_]|$)" \
        share/wake/lib src tools 2>/dev/null; then
        echo "OK $sym" >> "$SYMBOL_LOG"
    else
        echo "MISS $sym" >> "$SYMBOL_LOG"
    fi
done

SYMBOL_WARN=$(grep -c '^MISS ' "$SYMBOL_LOG" 2>/dev/null)
SYMBOL_OK=$(grep -c '^OK ' "$SYMBOL_LOG" 2>/dev/null)
SYMBOL_WARN="${SYMBOL_WARN:-0}"
SYMBOL_OK="${SYMBOL_OK:-0}"

if [ "$SYMBOL_WARN" -gt 0 ]; then
    printf '  '; yellow "⚠"; printf ' %s curated symbol(s) not found in source:\n' "$SYMBOL_WARN"
    grep '^MISS ' "$SYMBOL_LOG" | awk '{print "    " $2}'
else
    printf '  '; green "✓"; printf ' %s curated symbol(s) all present\n' "$SYMBOL_OK"
fi
echo

# --------------------------------------------------------------------------
# Pass 3: DELTA — what changed since last_validated_sha?
# --------------------------------------------------------------------------
bold "[3/3] delta since last bless"; echo

DELTA_HITS=0

if [ "$LAST_SHA" = "$HEAD_SHA" ]; then
    printf '  '; green "✓"; printf ' HEAD == last-validated; nothing to review.\n'
else
    if ! git cat-file -e "$LAST_SHA" 2>/dev/null; then
        printf '  '; red "✗"; printf ' recorded SHA %s is unreachable from this clone.\n' "$LAST_SHA" >&2
        printf '    (fetch the missing history, or bless again from a known-good commit)\n' >&2
        exit 2
    fi

    CHANGED="$(git diff --name-only "$LAST_SHA"..HEAD)"

    if [ -z "$CHANGED" ]; then
        echo "  (no file changes between $LAST_SHA and HEAD — perhaps just history rewrites)"
    else
        # Map a changed path → KB topic file(s) via prefix matching, most-specific
        # first. Implemented as a case statement (bash 3.2 has no associative arrays).
        topics_for_prefix() {
            case "$1" in
                build.wake|Makefile)              echo "knowledge/08-build-and-bootstrap.md" ;;
                .github/workflows/*)              echo "knowledge/08-build-and-bootstrap.md" ;;
                debian/changelog.in)              echo "knowledge/08-build-and-bootstrap.md" ;;

                src/runtime/database*)            echo "knowledge/04-build-model.md knowledge/09-cli-and-config.md knowledge/05-runtime-internals.md" ;;
                src/parser/*)                     echo "knowledge/05-runtime-internals.md knowledge/02-language.md" ;;
                src/dst/*)                        echo "knowledge/05-runtime-internals.md" ;;
                src/types/*)                      echo "knowledge/05-runtime-internals.md knowledge/02-language.md" ;;
                src/optimizer/*)                  echo "knowledge/05-runtime-internals.md" ;;
                src/runtime/*)                    echo "knowledge/05-runtime-internals.md knowledge/04-build-model.md" ;;
                src/cas/*)                        echo "knowledge/04-build-model.md knowledge/05-runtime-internals.md" ;;
                src/wakefs/*)                     echo "knowledge/04-build-model.md knowledge/05-runtime-internals.md" ;;
                src/json/*|src/util/*|src/wcl/*|src/compat/*)
                                                  echo "knowledge/05-runtime-internals.md" ;;
                src/*)                            echo "src/AGENTS.md knowledge/05-runtime-internals.md" ;;

                share/wake/lib/system/job.wake)        echo "knowledge/03-stdlib.md knowledge/04-build-model.md" ;;
                share/wake/lib/system/plan.wake)       echo "knowledge/03-stdlib.md knowledge/04-build-model.md" ;;
                share/wake/lib/system/runner.wake)     echo "knowledge/03-stdlib.md knowledge/04-build-model.md knowledge/10-cookbook.md" ;;
                share/wake/lib/system/sources.wake)    echo "knowledge/03-stdlib.md knowledge/04-build-model.md" ;;
                share/wake/lib/system/cas.wake)        echo "knowledge/04-build-model.md" ;;
                share/wake/lib/system/remote_cache*)   echo "knowledge/04-build-model.md knowledge/06-tooling.md" ;;
                share/wake/lib/system/*)               echo "knowledge/03-stdlib.md knowledge/04-build-model.md" ;;
                share/wake/lib/core/*)                 echo "knowledge/03-stdlib.md knowledge/02-language.md" ;;
                share/wake/lib/gcc_wake/*)             echo "knowledge/03-stdlib.md" ;;
                share/wake/lib/rust_wake/*)            echo "knowledge/03-stdlib.md" ;;
                share/wake/lib/*)                      echo "knowledge/03-stdlib.md share/wake/lib/AGENTS.md" ;;

                share/doc/wake/quickref.md)            echo "knowledge/02-language.md knowledge/09-cli-and-config.md share/doc/wake/AGENTS.md" ;;
                share/doc/wake/logging.md)             echo "knowledge/09-cli-and-config.md" ;;
                share/doc/wake/datastructures.txt)     echo "knowledge/03-stdlib.md" ;;
                share/doc/wake/tour/*)                 echo "knowledge/02-language.md" ;;
                share/doc/wake/how-to/*)               echo "knowledge/08-build-and-bootstrap.md" ;;
                share/doc/wake/*)                      echo "knowledge/02-language.md share/doc/wake/AGENTS.md" ;;

                tools/wake/cli_options.h)              echo "knowledge/09-cli-and-config.md" ;;
                tools/wake/main.cpp)                   echo "knowledge/09-cli-and-config.md knowledge/06-tooling.md" ;;
                tools/wake/*)                          echo "knowledge/06-tooling.md knowledge/09-cli-and-config.md" ;;
                tools/wake-format/*)                   echo "knowledge/06-tooling.md" ;;
                tools/wake-unit/*)                     echo "knowledge/06-tooling.md knowledge/07-testing.md" ;;
                tools/wake-migrate/*)                  echo "knowledge/06-tooling.md knowledge/04-build-model.md" ;;
                tools/wakebox/*)                       echo "knowledge/06-tooling.md knowledge/04-build-model.md" ;;
                tools/fuse-waked/*)                    echo "knowledge/06-tooling.md knowledge/04-build-model.md" ;;
                tools/lsp-wake/*)                      echo "knowledge/06-tooling.md extensions/vscode/AGENTS.md" ;;
                tools/bsp-wake/*)                      echo "knowledge/06-tooling.md" ;;
                tools/*)                               echo "knowledge/06-tooling.md tools/AGENTS.md" ;;

                tests/README.md)                       echo "knowledge/07-testing.md tests/AGENTS.md" ;;
                tests/*)                               echo "knowledge/07-testing.md tests/AGENTS.md" ;;

                rust/*)                                echo "knowledge/06-tooling.md rust/AGENTS.md" ;;
                extensions/vscode/*)                   echo "extensions/vscode/AGENTS.md knowledge/06-tooling.md" ;;

                *) echo "" ;;
            esac
        }

        : > "$DELTA_LOG"
        printf '%s\n' "$CHANGED" | while IFS= read -r f; do
            [ -n "$f" ] || continue
            topics="$(topics_for_prefix "$f")"
            if [ -n "$topics" ]; then
                for t in $topics; do
                    printf '%s\t%s\n' "$t" "$f"
                done
            fi
        done >> "$DELTA_LOG"

        DELTA_HITS=$(wc -l < "$DELTA_LOG" | tr -d '[:space:]')
        TOPIC_COUNT=$(awk -F'\t' '{print $1}' "$DELTA_LOG" | sort -u | sed '/^$/d' | wc -l | tr -d '[:space:]')
        TOTAL_CHANGED=$(printf '%s\n' "$CHANGED" | wc -l | tr -d '[:space:]')

        if [ "$DELTA_HITS" -eq 0 ]; then
            printf '  '; green "✓"; printf ' %s file(s) changed, none in KB-tracked areas.\n' "$TOTAL_CHANGED"
        else
            printf '  '; yellow "⚠"; printf ' changed files map to %s KB topic(s):\n' "$TOPIC_COUNT"
            for topic in $(awk -F'\t' '{print $1}' "$DELTA_LOG" | sort -u); do
                printf '    '; bold "$topic"; echo
                awk -F'\t' -v t="$topic" '$1==t {print "      • " $2}' "$DELTA_LOG" | sort -u
            done
            echo
            echo "  After reviewing & updating, run: scripts/kb-bless.sh"
        fi
    fi
fi
echo

# --------------------------------------------------------------------------
# Summary
# --------------------------------------------------------------------------
bold "summary"; echo
if [ "$PATH_ERRORS" -gt 0 ]; then
    printf '  paths:   '; red "$PATH_ERRORS broken"; echo
else
    printf '  paths:   '; green "OK ($PATH_OK)"; echo
fi
if [ "$SYMBOL_WARN" -gt 0 ]; then
    printf '  symbols: '; yellow "$SYMBOL_WARN to review"; echo
else
    printf '  symbols: '; green "OK ($SYMBOL_OK)"; echo
fi
if [ "$DELTA_HITS" -gt 0 ]; then
    printf '  delta:   '; yellow "$DELTA_HITS file→topic mapping(s)"; echo
else
    printf '  delta:   '; green "OK"; echo
fi

if [ "$PATH_ERRORS" -gt 0 ] || [ "$DELTA_HITS" -gt 0 ]; then
    exit 1
fi
exit 0
