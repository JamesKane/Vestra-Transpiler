#!/usr/bin/env bash
#
# Corpus sweep for the self-hosted codegen phase (the burndown metric).
#
# For every examples/*.vst, runs BOTH codegen differentials against the
# bootstrap compiler and reports whether the file is byte-identical:
#
#   vgen FILE          vs  the generated FILE.cpp
#   vgen --hpp FILE    vs  the FILE.hpp per-program tail (everything after
#                          the fixed runtime prelude)
#
# Two numbers matter. The IDENTICAL-EXAMPLE COUNT is the arc's headline, but
# it is coarse mid-arc: a file only flips once its LAST divergent family
# closes, so a slice can remove thousands of divergent lines without moving
# it. TOTAL DIVERGENT LINES is the fine-grained signal — use --compare
# against a pre-slice report to see it, and to catch per-example regressions
# that an unchanged headline would hide.
#
# Usage:
#   scripts/sweep_codegen_corpus.sh [--build] [--report FILE] [--compare FILE]
#                                   [--work DIR] [--vgen PATH] [--vestra PATH]
#
#   --build          (re)build vgen from selfhost/codegen.vst first
#   --report FILE    per-example report (default: <work>/report.txt)
#   --compare FILE   compare totals against an earlier report and list
#                    any example whose divergence GREW
#   --work DIR       scratch dir for reference builds (default: build/sweep)
#   --vgen PATH      vgen binary (default: <work>/vgen)
#   --vestra PATH    bootstrap compiler (default: build/debug/vestra)
#
# Report lines are stable and greppable, so reports compare across runs:
#   OK   <stem>
#   DIFF <stem> cpp=<n> hpp=<n>
#   SKIP <stem> <reason>
#
# Line counts come from MINIMAL diffs (-d). Do not compare a report against
# one produced without it — plain diff's edit script is not canonical, and
# the counts will differ by a few lines for the same content.
#
set -uo pipefail

root=$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)
cd "$root"

work="build/sweep"
vestra="build/debug/vestra"
vgen=""
report=""
compare=""
do_build=0

while [[ $# -gt 0 ]]; do
    case "$1" in
        --build)   do_build=1; shift ;;
        --report)  report="$2"; shift 2 ;;
        --compare) compare="$2"; shift 2 ;;
        --work)    work="$2"; shift 2 ;;
        --vgen)    vgen="$2"; shift 2 ;;
        --vestra)  vestra="$2"; shift 2 ;;
        -h|--help) sed -n '2,40p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) echo "unknown option: $1" >&2; exit 2 ;;
    esac
done

: "${vgen:=$work/vgen}"
: "${report:=$work/report.txt}"

if [[ ! -x "$vestra" ]]; then
    echo "no bootstrap compiler at $vestra (build it, or pass --vestra)" >&2
    exit 1
fi

mkdir -p "$work"

if [[ $do_build -eq 1 ]]; then
    echo "building vgen from selfhost/codegen.vst ..."
    "$vestra" build selfhost/codegen.vst -o "$work" >/dev/null || exit 1
    # Warnings are expected (the emitted code drops some [[nodiscard]] results);
    # only a hard failure should stop the sweep.
    "${CXX:-c++}" -std=c++26 -O0 -I"$work" "$work/codegen.cpp" -o "$work/vgen" 2>/dev/null || {
        echo "vgen failed to compile" >&2; exit 1; }
    vgen="$work/vgen"
fi

if [[ ! -x "$vgen" ]]; then
    echo "no vgen binary at $vgen (pass --build, or --vgen PATH)" >&2
    exit 1
fi

: > "$report"
ok=0; bad=0; skipped=0; lines=0

for f in examples/*.vst; do
    stem=$(basename "$f" .vst)
    d="$work/ref/$stem"
    rm -rf "$d"; mkdir -p "$d"

    # The reference. A file that sema rejects, or that emits no .cpp, is not a
    # valid differential target — record it rather than counting it as a pass.
    if ! "$vestra" build "$f" -o "$d" >/dev/null 2>&1; then
        echo "SKIP $stem bootstrap-build-failed" >> "$report"
        skipped=$((skipped + 1)); continue
    fi
    if [[ ! -f "$d/$stem.cpp" || ! -f "$d/$stem.hpp" ]]; then
        echo "SKIP $stem no-output" >> "$report"
        skipped=$((skipped + 1)); continue
    fi

    awk 'f{print} index($0,"#endif  // VESTRA_RUNTIME_PRELUDE"){f=1}' \
        "$d/$stem.hpp" > "$d/tail.ref"

    "$vgen" "$f"       > "$d/gen.cpp"  2>"$d/gen.cpp.err"
    "$vgen" --hpp "$f" > "$d/tail.gen" 2>"$d/tail.gen.err"

    # -d (minimal) matters for the LINE COUNT, not for OK/DIFF. Plain diff
    # picks a fast, non-minimal edit script whose size depends on how the
    # surrounding text happens to align, so fixing one hunk can INFLATE the
    # reported count elsewhere and fake a regression. -d agrees with a true
    # LCS (difflib, autojunk off) on this corpus.
    dc=$(diff -d "$d/$stem.cpp" "$d/gen.cpp"  | grep -c '^[<>]')
    dh=$(diff -d "$d/tail.ref"  "$d/tail.gen" | grep -c '^[<>]')

    if [[ $dc -eq 0 && $dh -eq 0 ]]; then
        echo "OK $stem" >> "$report"
        ok=$((ok + 1))
    else
        echo "DIFF $stem cpp=$dc hpp=$dh" >> "$report"
        bad=$((bad + 1)); lines=$((lines + dc + dh))
    fi
done

summary="=== identical: $ok  divergent: $bad  skipped: $skipped  divergent-lines: $lines"
echo "$summary" >> "$report"
echo "$summary"
echo "report: $report"

# Per-example totals from a report, as "<stem> <total-diff-lines>".
totals() {
    grep '^DIFF' "$1" | sed 's/^DIFF //; s/cpp=//; s/hpp=//' \
        | awk '{print $1, $2 + $3}' | sort
}

if [[ -n "$compare" ]]; then
    if [[ ! -f "$compare" ]]; then
        echo "no report to compare at $compare" >&2
        exit 1
    fi
    before=$(grep -c '^OK' "$compare")
    before_lines=$(totals "$compare" | awk '{n += $2} END {print n + 0}')
    echo
    echo "vs $compare"
    echo "  identical:       $before -> $ok"
    echo "  divergent-lines: $before_lines -> $lines"
    echo "  regressions (divergence grew, or a passing example broke):"
    # An example that was OK and is now DIFF has no "before" row to join, so
    # check both directions.
    join <(totals "$compare") <(totals "$report") \
        | awk '$3 > $2 {print "    WORSE " $1 ": " $2 " -> " $3}'
    comm -12 <(grep '^OK' "$compare" | awk '{print $2}' | sort) \
             <(grep '^DIFF' "$report" | awk '{print $2}' | sort) \
        | sed 's/^/    BROKE /'
fi
