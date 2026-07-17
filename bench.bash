#!/usr/bin/env bash
#
# bench.bash — the Voilà performance benchmark.
#
# Two suites:
#   RUNTIME    — the bench/*.voi programs. EVERY timed run's output is compared
#                against bench/expected/ and its exit code checked, so a "fast
#                but wrong" (or fast-because-it-crashed) run can never post a
#                number.
#   COMPILE    — the compiler compiling itself (front end, C emission, full
#                build) plus a small-program build (isolates the fixed
#                per-build overhead).
#
# Usage:
#     ./bench.bash                 both suites, default optimization
#     ./bench.bash -O1|-O2|-O3     pass the flag to every build in both suites
#     ./bench.bash runtime|compile one suite only
#
# Methodology: best-of-N wall time (N=3, override N=…), measured with bash's
# EPOCHREALTIME — no subprocess enters the timed window. Binaries are built
# and run once before timing (warms the code signature and page cache).
# Numbers are comparable only within one machine. NOTE: the self-compile
# workload grows as the compiler grows; compile-suite deltas across
# milestones conflate compiler speed with compiler size.
set -uo pipefail
cd "$(dirname "$0")"

# EPOCHREALTIME needs bash >= 5; a silent fallback would reintroduce the
# ~20 ms python-startup bias inside the timing window.
[ -n "${EPOCHREALTIME:-}" ] || { echo "bench.bash requires bash >= 5 (EPOCHREALTIME)"; exit 2; }

VOILA=${VOILA:-./voila}
N=${N:-3}
[[ "$N" =~ ^[1-9][0-9]*$ ]] || { echo "bad N: $N"; exit 2; }
OPT=""
SUITE="all"
for a in "$@"; do
    case "$a" in
        -O1|-O2|-O3) OPT="$a" ;;
        runtime|compile) SUITE="$a" ;;
        *) echo "usage: ./bench.bash [-O1|-O2|-O3] [runtime|compile]"; exit 2 ;;
    esac
done

bold() { printf '\033[1m%s\033[0m\n' "$1"; }
fail() { printf '\033[1;31m  ✗ %s\033[0m\n' "$1"; exit 1; }

now_us() {                      # microseconds, no subprocess
    local s=${EPOCHREALTIME/./}
    echo "${s}"
}

# best_ms CMD... — run N times; every run must exit 0. Echo best wall ms.
best_ms() {
    local best=999999999
    for _ in $(seq 1 "$N"); do
        local t0 t1 ms
        t0=$(now_us)
        "$@" > /dev/null 2>&1 || fail "timed command exited $?: $*"
        t1=$(now_us)
        ms=$(( (t1 - t0) / 1000 ))
        [ "$ms" -lt "$best" ] && best=$ms
    done
    echo "$best"
}

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if [ "$SUITE" != "compile" ]; then
    bold "== RUNTIME SUITE ${OPT:+(${OPT})} — best of $N, ms =="
    printf '%-14s %10s   %s\n' "program" "ms" "verified output"
    for src in bench/b*_*.voi; do
        name="$(basename "${src%.voi}")"
        exe="$TMP/$name"
        # shellcheck disable=SC2086  # $OPT must word-split away when empty
        if ! $VOILA build $OPT "$src" -o "$exe" > "$TMP/$name.build.log" 2>&1; then
            cat "$TMP/$name.build.log" | head -5
            fail "$name: build failed${OPT:+ at $OPT}"
        fi
        best=999999999
        for _ in $(seq 1 "$N"); do
            t0=$(now_us)
            "$exe" > "$TMP/$name.out" 2>&1 || fail "$name: run exited $?"
            t1=$(now_us)
            # EVERY timed run is verified — a partial or wrong output is
            # rejected even if an earlier run of the same binary was correct.
            cmp -s "$TMP/$name.out" "bench/expected/$name.out" \
                || { diff "bench/expected/$name.out" "$TMP/$name.out" | head -3
                     fail "$name: WRONG OUTPUT${OPT:+ at $OPT} — timing rejected"; }
            ms=$(( (t1 - t0) / 1000 ))
            [ "$ms" -lt "$best" ] && best=$ms
        done
        printf '%-14s %10s   %s\n' "$name" "$best" "$(cat "$TMP/$name.out")"
    done
fi

if [ "$SUITE" != "runtime" ]; then
    bold "== COMPILE SUITE ${OPT:+(${OPT})} — best of $N, ms =="
    # The self-compile, split by stage boundary:
    #   check    = lex + parse + load + all static checks
    #   emit-c   = check + lower (+ optimizer when flagged) + C generation
    #   build    = emit-c + cc + link
    # shellcheck disable=SC2086
    printf '%-34s %10s\n' "workload" "ms"
    printf '%-34s %10s\n' "self: check (front end)" \
        "$(best_ms $VOILA check voilac/main.voi)"
    printf '%-34s %10s\n' "self: --emit=c (front+mid)" \
        "$(best_ms $VOILA build $OPT --emit=c voilac/main.voi)"
    printf '%-34s %10s\n' "self: full build (adds cc)" \
        "$(best_ms $VOILA build $OPT voilac/main.voi -o "$TMP/self")"
    printf '%-34s %10s\n' "small: full build (b1_queens)" \
        "$(best_ms $VOILA build $OPT bench/b1_queens.voi -o "$TMP/small")"
fi
