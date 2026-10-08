#!/usr/bin/env bash
# Copyright (c) 2026 The Bitcoin Tolerant developers
# Distributed under the MIT software license, see the accompanying
# file COPYING or http://www.opensource.org/licenses/mit-license.php.
#
# Review a Bitcoin Core release before rebasing Bitcoin Tolerant onto it.
#
#   contrib/tolerant/upstream-review.sh <old-core-tag> <new-core-tag> [tolerant-ref]
#   contrib/tolerant/upstream-review.sh v30.3 v32.0rc3
#
# Prints a Markdown report, ready to paste into doc/tolerant-upstream.md:
#   1. Changed defaults in block-space policy headers   -> may need a decision
#   2. Block-space policy PRs (the watchlist)           -> triage tier 3
#   3. Consensus PRs                                    -> tier 1: always accepted
#   4. Rebase risk: Core files Tolerant wires into that upstream also changed
# See doc/tolerant-upstream.md for the principles and the triage these feed.

set -euo pipefail

if [ $# -lt 2 ]; then
    echo "usage: $0 <old-core-tag> <new-core-tag> [tolerant-ref]" >&2
    exit 64
fi
OLD=$1
NEW=$2
TOLERANT=${3:-HEAD}
UPSTREAM_REMOTE=${UPSTREAM_REMOTE:-upstream-core}

cd "$(git rev-parse --show-toplevel)"

ensure_ref() {
    if ! git rev-parse --verify --quiet "$1^{commit}" >/dev/null; then
        echo "fetching $1 from $UPSTREAM_REMOTE..." >&2
        git fetch --quiet "$UPSTREAM_REMOTE" "refs/tags/$1:refs/tags/$1"
    fi
}
ensure_ref "$OLD"
ensure_ref "$NEW"

# Tier 3: where Bitcoin Tolerant's identity lives (block-space and data policy).
WATCHLIST="src/policy src/validation.cpp src/node/miner.cpp src/node/miner.h
src/node/mempool_args.cpp src/kernel/mempool_options.h src/kernel/mempool_entry.h
src/txmempool.cpp src/txmempool.h src/script/script.cpp src/script/script.h"
# Headers whose DEFAULT_/MAX_/MIN_ constants are policy defaults.
DEFAULT_HEADERS="src/policy/policy.h src/policy/feerate.h src/policy/packages.h src/policy/truc_policy.h
src/policy/ephemeral_policy.h src/kernel/mempool_options.h src/node/miner.h src/node/types.h src/validation.h"
# Tier 1: consensus. Always accepted unchanged; listed only for awareness.
CONSENSUS="src/consensus src/script/interpreter.cpp src/script/interpreter.h src/pow.cpp src/pow.h"
# Flagging, matched as whole words in PR titles:
#  STRONG_RE: near-certain block-space/data signals; searched across the whole
#             release, since such changes can live outside the watched paths
#             (e.g. option help text in init.cpp, or docs).
#  BROAD_RE:  common words; only counted for PRs that touch the watched paths.
STRONG_RE='datacarrier|datacarriersize|op_return|dust|standardness|non-?standard|isstandard|minrelay|minrelaytxfee|incrementalrelayfee|blockmintxfee|inscriptions?|ordinals?|block ?space'
BROAD_RE='packages?|truc|ephemeral|feerates?|fee rates?|templates?|createnewblock|addpackagetxs|rbf|replacement|cluster'
# Consensus-file PRs that are tooling or docs, not behavior.
TOOLING_RE='^#[0-9]+ (ci|iwyu|doc|docs|scripted-diff: \\[doc\\])[:( ]'
# Files Bitcoin Tolerant owns (never conflict with upstream).
OWNED_RE='tolerant|^README\.md$'

# Map each non-merge commit touching the given paths to the Core PR merge that
# introduced it, and print unique "#NNNN title" lines.
prs_touching() {
    # shellcheck disable=SC2086
    git rev-list --no-merges "$OLD..$NEW" -- $1 | while read -r c; do
        git log --merges --ancestry-path --format='%s' "$c..$NEW" | tail -1
    done | to_pr_lines
}

to_pr_lines() {
    sed -n 's/^Merge bitcoin\/bitcoin#\([0-9]*\): \(.*\)$/#\1 \2/p' | sort -t'#' -k2 -n -u
}

# NAME VALUE pairs for every DEFAULT_/MAX_/MIN_ constant defined in the policy
# headers at a ref, normalized so that refactors (static -> inline, digit
# separators, spacing) don't read as value changes.
policy_constants() {
    local ref=$1 f
    for f in $DEFAULT_HEADERS; do
        git show "$ref:$f" 2>/dev/null || true
    done | grep -oE '\b(DEFAULT|MAX|MIN)_[A-Z0-9_]+[[:space:]]*(\{[^};]*\}|=[^;]*;)' \
         | sed -E "s/[[:space:]]+//g; s/'//g; s/^([A-Z0-9_]+)[{=]/\1 /; s/[};]+$//" \
         | sort -u
}

echo "## Bitcoin Core $OLD → $NEW"
echo
echo "_Generated $(date -u +%Y-%m-%d) by contrib/tolerant/upstream-review.sh._"
echo "$(git rev-list --count "$OLD..$NEW") upstream commits in total."
echo

echo "### 1. Changed policy defaults"
echo
old_consts=$(policy_constants "$OLD")
new_consts=$(policy_constants "$NEW")
changed=$(
    join -a1 -a2 -e '(none)' -o '0,1.2,2.2' \
        <(echo "$old_consts" | sort -k1,1) <(echo "$new_consts" | sort -k1,1) \
    | awk '$2 != $3 { printf "| `%s` | `%s` | `%s` |\n", $1, $2, $3 }'
)
if [ -z "$changed" ]; then
    echo "None. No DEFAULT_/MAX_/MIN_ constant in the policy headers changed value."
else
    echo "| Constant | $OLD | $NEW |"
    echo "|---|---|---|"
    echo "$changed"
    echo
    echo "Each row needs a decision: accept, or override it in"
    echo "src/node/tolerant_defaults.cpp with the principle that justifies it."
fi
echo

echo "### 2. Block-space policy PRs (needs triage)"
echo
policy_prs=$(prs_touching "$WATCHLIST")
# Keyword sweep over every PR in the range: some policy changes live outside
# the watched paths (e.g. option help text in init.cpp, or docs).
all_prs=$(git log --merges --format='%s' "$OLD..$NEW" | to_pr_lines)
if [ -z "$policy_prs$all_prs" ]; then
    echo "None."
else
    flagged=$({ echo "$all_prs" | grep -wiE "$STRONG_RE"; echo "$policy_prs" | grep -wiE "$STRONG_RE|$BROAD_RE"; } \
        | sort -t'#' -k2 -n -u || true)
    other=$(echo "$policy_prs" | grep -vwiE "$STRONG_RE|$BROAD_RE" || true)
    n_flag=$(printf '%s' "$flagged" | grep -c . || true)
    n_other=$(printf '%s' "$other" | grep -c . || true)
    echo "**Flagged ($n_flag):** read these. Data, standardness and relay-fee PRs from anywhere in the release; package, RBF, cluster and block-template PRs that touch watched files."
    echo
    [ -n "$flagged" ] && echo "$flagged" | sed 's/^/- [ ] /'
    echo
    echo "<details><summary>Other PRs touching watched files ($n_other) — usually refactors</summary>"
    echo
    [ -n "$other" ] && echo "$other" | sed 's/^/- /'
    echo
    echo "</details>"
fi
echo

echo "### 3. Consensus PRs (tier 1: always accepted)"
echo
consensus_prs=$(prs_touching "$CONSENSUS")
behavior=$(echo "$consensus_prs" | grep -vE "$TOOLING_RE" || true)
n_tooling=$(echo "$consensus_prs" | grep -cE "$TOOLING_RE" || true)
if [ -z "$behavior" ]; then
    echo "None."
else
    echo "$behavior" | sed 's/^/- /'
fi
[ "$n_tooling" -gt 0 ] && echo && echo "Plus $n_tooling CI, include-what-you-use or docs PRs touching consensus files."
echo

echo "### 4. Rebase risk"
echo
if ! git merge-base --is-ancestor "$OLD" "$TOLERANT" 2>/dev/null; then
    echo "> Note: $TOLERANT is not built on $OLD; the list below may be incomplete."
    echo
fi
wiring=$(git diff --name-only "$OLD" "$TOLERANT" | grep -vE "$OWNED_RE" || true)
upstream_changed=$(git diff --name-only "$OLD" "$NEW")
risk=$(comm -12 <(echo "$wiring" | sort) <(echo "$upstream_changed" | sort) || true)
if [ -z "$risk" ]; then
    echo "None of the Core files Tolerant wires into changed upstream."
else
    echo "Core files Tolerant modifies that also changed upstream (expect conflicts here):"
    echo
    echo "$risk" | sed 's/^/- `/; s/$/`/'
fi
