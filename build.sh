#!/usr/bin/env bash
# build.sh — build the Voilà toolchain from an existing Voilà compiler.
#
# This is the SELF-HOSTED path: Voilà builds Voilà. It uses a prebuilt `voila`
# binary (>= MIN_BOOTSTRAP) to compile the compiler from voilac/*.voi, then
# compiles the compiler with the compiler and asserts the two emit identical C:
#
#     prebuilt voila (>= 0.4.1) ──▶ voilac-1 ──▶ voilac-2
#                                        │           │
#                                        └── C == C ─┘   the fixpoint
#
# A machine that has no Voilà binary yet (a fresh architecture) cannot run this
# path — there is nothing to build with. Run ./bootstrap.bash instead: it
# compiles the checked-in C seed with `cc` to produce the first binary, then
# hands back here. (cc is still used INSIDE every `voila build` to compile the
# emitted C — that never changes; this script is only about the FIRST compiler.)
#
#     ./build.sh              build, and verify the fixpoint
#     ./build.sh reseed       …and regenerate bootstrap/voilac.c (the fallback seed)
#     ./build.sh seed         stop after the first-generation compiler
set -euo pipefail
cd "$(dirname "$0")"

MODE="${1:-all}"
MIN_BOOTSTRAP="0.4.1"          # the oldest voila that can build voilac/ (BOOTSTRAP.md)

note() { printf '\033[1m==> %s\033[0m\n' "$1"; }
ok()   { printf '\033[32m  ✓ %s\033[0m\n' "$1"; }
die()  { printf '\033[1;31m  ✗ %s\033[0m\n' "$1" >&2; exit 1; }

# ver_ge A B : succeed if dotted version A >= B. Missing components count as 0;
# a non-numeric suffix (e.g. "-rc1") is trimmed. Pure shell — BSD/macOS `sort`
# has no -V.
ver_ge() {
    local A B x y i
    IFS=. read -ra A <<< "${1%%[!0-9.]*}"
    IFS=. read -ra B <<< "${2%%[!0-9.]*}"
    for i in 0 1 2; do
        x=$(( 10#${A[i]:-0} )); y=$(( 10#${B[i]:-0} ))
        (( x > y )) && return 0
        (( x < y )) && return 1
    done
    return 0
}

# probe_version BIN : echo "0.4.1" from `BIN version`, or fail (non-zero) if BIN
# is not a runnable voila of this architecture — not executable, wrong arch
# (exec-format error), crashes, or prints "unknown"/anything non-numeric.
probe_version() {
    local out ver
    out="$("$1" version 2>/dev/null)" || return 1
    ver="${out#voila }"
    case "$ver" in
        [0-9]*.[0-9]*) printf '%s\n' "$ver" ;;
        *) return 1 ;;
    esac
}

# choose_binary : set the global BOOT to the first usable voila (>=
# MIN_BOOTSTRAP), searching $VOILA, then bin/voila, then `voila` on PATH — or
# die() if none. An explicit $VOILA that is unusable is a hard error (explicit
# intent), not a silent skip. Runs in the MAIN shell (not a command
# substitution) so die() exits cleanly and a "skipping" note cannot pollute the
# result.
BOOT=""
choose_binary() {
    local c v
    if [ -n "${VOILA:-}" ]; then
        v="$(probe_version "$VOILA")" \
            || die "VOILA=$VOILA is not a runnable voila binary for this machine"
        ver_ge "$v" "$MIN_BOOTSTRAP" \
            || die "VOILA=$VOILA is $v, but $MIN_BOOTSTRAP is the minimum to build voila"
        BOOT="$VOILA"; return
    fi
    for c in bin/voila "$(command -v voila 2>/dev/null || true)"; do
        [ -n "$c" ] && [ -x "$c" ] || continue
        v="$(probe_version "$c")" || continue
        ver_ge "$v" "$MIN_BOOTSTRAP" || { note "$c is $v (< $MIN_BOOTSTRAP) — skipping"; continue; }
        BOOT="$c"; return
    done
    die "no Voilà >= $MIN_BOOTSTRAP found (looked at \$VOILA, bin/voila, and PATH).
build.sh is the self-hosted path: it needs an existing voila to build voila.
On a machine with no voila yet, run:  ./bootstrap.bash   (it needs only a C compiler)."
}

mkdir -p build bin
choose_binary   # sets $BOOT, or dies with a clear message

note "bootstrap: ${VOILA_KIND:-prebuilt binary}  $BOOT  ($(probe_version "$BOOT"))"

# Exec a COPY in build/, never the binary we may be about to reinstall: this
# guarantees tools/build.voi's install (and the final `rm -rf build`) can never
# disturb the process running this build. (See BOOTSTRAP.md / install() in
# tools/build.voi for why the running binary must stay untouched.)
if [ "$BOOT" != build/voila-boot ]; then
    cp "$BOOT" build/voila-boot
fi
build/voila-boot version >/dev/null 2>&1 || true   # warm the macOS code signature
ok "using build/voila-boot"

# From here on, Voilà builds Voilà.
export VOILA=build/voila-boot
exec build/voila-boot run tools/build.voi "$MODE"
