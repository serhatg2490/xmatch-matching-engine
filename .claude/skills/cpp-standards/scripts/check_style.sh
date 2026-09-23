#!/usr/bin/env bash
# xmatch style check. Rule numbers match cpp-standards/SKILL.md;
# "bp-N" references match best-practices/references/practices.md.
#
# Usage:
#   check_style.sh              # C/C++ files changed according to git
#   check_style.sh <file>...    # the given files
# Exit: 1 if there are violations, 0 otherwise.

set -uo pipefail

MAX_COLS=110
repo_root=$(git rev-parse --show-toplevel 2>/dev/null) || repo_root=$PWD
cd "$repo_root" || exit 1

if [ $# -gt 0 ]; then
    files=("$@")
else
    mapfile -t files < <(
        { git diff --name-only --diff-filter=ACM HEAD
          git ls-files --others --exclude-standard; } 2>/dev/null \
        | grep -E '\.(cpp|hpp|c|h)$' | sort -u
    )
fi

violations=0
report() { echo "$1:$2: [$3] $4"; violations=$((violations + 1)); }

# Drops lines that consist only of a comment ("// ...", "* ...", "/* ...").
# Trailing comments are not dropped — deliberately: the code part is still scanned.
drop_comments() { grep -vE '^[0-9]+:[[:space:]]*(//|\*|/\*)'; }

for f in "${files[@]:-}"; do
    [ -f "$f" ] || continue
    case "$f" in *.cpp|*.hpp|*.c|*.h) ;; *) continue ;; esac
    # Make the path repo-relative: the src/* and include/* patterns below
    # depend on it, while the hook passes an absolute path.
    f=$(realpath --relative-to="$repo_root" "$f" 2>/dev/null || echo "$f")

    # Rule 1 — no tabs
    while IFS=: read -r n _; do
        [ -n "$n" ] && report "$f" "$n" "rule 1" "tab character; use 4 spaces"
    done < <(grep -nP '\t' "$f" 2>/dev/null)

    # Rule 2 — line length (tests/ exempt)
    case "$f" in tests/*) ;; *)
        while read -r n len; do
            report "$f" "$n" "rule 2" "line is $len columns, limit $MAX_COLS"
        done < <(awk -v m="$MAX_COLS" 'length>m{print NR, length}' "$f")
    ;; esac

    # Rule 3 — header guard
    case "$f" in *.hpp|*.h)
        grep -q '^#pragma once' "$f" || report "$f" 1 "rule 3" "missing #pragma once"
        n=$(grep -nE '^#ifndef +[A-Z_]+_H' "$f" | head -1 | cut -d: -f1)
        [ -n "$n" ] && report "$f" "$n" "rule 3" "include guard; use #pragma once"
    ;; esac

    # Rule 5 — namespace / extern "C" closing comment
    for pair in 'namespace:^namespace .*\{:^\} // namespace' 'extern "C":^extern "C" \{:^\} // extern "C"'; do
        what=${pair%%:*}; rest=${pair#*:}; open=${rest%%:*}; close=${rest#*:}
        o=$(grep -cE "$open" "$f"); c=$(grep -cE "$close" "$f")
        [ "$o" -ne "$c" ] && report "$f" 1 "rule 5" \
            "$what: $o opening / $c closing comments — add '} // $what ...'"
    done

    # Rule 7 — C-style cast
    while IFS=: read -r n _; do
        [ -n "$n" ] && report "$f" "$n" "rule 7" "C-style cast; use static_cast"
    done < <(grep -nP '\(\s*(int|unsigned|char|long|short|float|double)\s*\)\s*[A-Za-z_(]' "$f" | drop_comments)

    # Rule 9 — compiler extension
    while IFS=: read -r n _; do
        [ -n "$n" ] && report "$f" "$n" "rule 9" "__builtin_*; use the <bit> equivalent"
    done < <(grep -n '__builtin_' "$f" | drop_comments)

    # --- the mechanically measurable part of the best-practices items ---
    case "$f" in src/*|include/*)

        # bp-7 — allocation / syscall / logging on the hot path
        while IFS=: read -r n _; do
            [ -n "$n" ] && report "$f" "$n" "bp-7" "I/O or allocation call on the hot path"
        done < <(grep -nE '\b(printf|malloc|free|std::cout|std::cerr)\b' "$f" | drop_comments)

        # bp-14 — no float in price math (load-factor calculation exempt)
        case "$f" in *flat_hash_map.hpp) ;; *)
            while IFS=: read -r n _; do
                [ -n "$n" ] && report "$f" "$n" "bp-14" "float/double; use Price integer arithmetic"
            done < <(grep -nE '\b(float|double)\b' "$f" | drop_comments)
        ;; esac

        # bp-16 — catch (...) only at the ABI boundary
        case "$f" in src/engine.cpp|src/engine_api.cpp) ;; *)
            while IFS=: read -r n _; do
                [ -n "$n" ] && report "$f" "$n" "bp-16" "catch (...) belongs only at the API boundary"
            done < <(grep -n 'catch (\.\.\.)' "$f" | drop_comments)
        ;; esac

    ;; esac
done

if [ "$violations" -gt 0 ]; then
    echo "--- $violations violation(s). Rules: .claude/skills/cpp-standards/SKILL.md" >&2
    exit 1
fi
exit 0
