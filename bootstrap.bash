#!/usr/bin/env bash
# bootstrap.bash — bring Voilà up on a machine that has NO Voilà binary yet.
#
# This is the portability path, and the ONLY one that needs no prior Voilà. It
# compiles the checked-in C seed (bootstrap/voilac.c + runtime/src/*.c) with cc
# into the first native `voila` for this architecture, then hands off to
# build.sh, which builds voilac/ with it and asserts the self-hosting fixpoint.
#
#     bootstrap/voilac.c + runtime ──cc──▶ voila-seed ──▶ (build.sh) ──▶ voilac-1 ──▶ voilac-2
#
# The seed is architecture-independent C: it compiles anywhere a C11 compiler
# exists. Once this has run, you have a `voila` binary and can use ./build.sh
# (the fast, self-hosted path) for every subsequent rebuild.
#
#     ./bootstrap.bash            seed → build, verify the fixpoint, install
#     ./bootstrap.bash reseed     …and regenerate bootstrap/voilac.c
set -euo pipefail
cd "$(dirname "$0")"

CC="${CC:-cc}"
CFLAGS="${CFLAGS:--std=c11 -O2}"
MODE="${1:-all}"

printf '\033[1m==> cc: bootstrap/voilac.c + runtime  →  the seed\033[0m\n'
mkdir -p build bin
$CC $CFLAGS -Iruntime/src -o build/voila-seed \
    bootstrap/voilac.c runtime/src/*.c -lm -lpthread
printf '\033[32m  ✓ build/voila-seed\033[0m\n'

# Hand the seed to build.sh as the bootstrap binary. The seed reports the
# current version (>= the minimum), so build.sh's version gate accepts it,
# copies it to build/voila-boot, and drives tools/build.voi — so the
# fixpoint / reseed / install logic lives in exactly ONE place.
export VOILA=build/voila-seed
export VOILA_KIND="C seed"
exec ./build.sh "$MODE"
