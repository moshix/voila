#!/usr/bin/env bash
# build.sh — build the Voilà toolchain. The only requirement is a C compiler.
#
# The bootstrap has one irreducible non-Voilà step: something must turn the
# checked-in C seed into a binary, because until that binary exists there is
# nothing to run a Voilà program with. That step is the single `cc` command
# below.
#
# EVERYTHING ELSE IS WRITTEN IN VOILÀ — tools/build.voi — and the seed binary
# runs it. It compiles the compiler, compiles the compiler with the compiler,
# and asserts that the two emit identical C:
#
#     bootstrap/voilac.c ──cc──▶ voila-seed ──▶ voilac-1 ──▶ voilac-2
#                                                    │           │
#                                                    └── C == C ─┘  the fixpoint
#
#     ./build.sh              build, and verify the fixpoint
#     ./build.sh reseed       …and regenerate bootstrap/voilac.c
#     ./build.sh seed         stop after the seed
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

# From here on, Voilà builds Voilà.
export VOILA=build/voila-seed
exec build/voila-seed run tools/build.voi "$MODE"
