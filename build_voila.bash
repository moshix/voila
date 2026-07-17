#!/usr/bin/env bash
#
# build_voila.bash — package the Voilà toolchain for other machines.
#
# Cross-compiling a Go binary meant `GOOS=linux go build`. The toolchain is no
# longer a Go binary: it is C. So "cross-compiling" now means one of two things,
# and this script does both.
#
#   1. SOURCE DISTRIBUTION (always produced, works on every platform)
#      dist/voila-src.tar.gz holds the generated C of the compiler plus
#      libvoila's source and a two-line build script. Anybody with a C compiler
#      unpacks it and runs `./build.sh`. No Voilà, no Go, no make.
#
#   2. NATIVE BINARIES for whichever targets a cross-compiler is installed for.
#      A cross-cc is named by convention (x86_64-linux-gnu-gcc,
#      x86_64-w64-mingw32-gcc, …); each one found produces a binary in dist/.
#      Missing toolchains are skipped, loudly, not silently.
#
# Usage:
#     ./build_voila.bash                 # test, then package
#     SKIP_TESTS=1 ./build_voila.bash    # package only
set -euo pipefail
cd "$(dirname "$0")"

VERSION="0.3.2"
DIST="dist"

bold() { printf '\033[1m%s\033[0m\n' "$1"; }
ok()   { printf '\033[32m  ✓ %s\033[0m\n' "$1"; }
skip() { printf '\033[33m  – %s\033[0m\n' "$1"; }

# The toolchain must be able to build itself before it is shipped anywhere.
bold "==> bootstrap"
./build.sh >/dev/null
ok "bin/voila (fixpoint verified)"

if [[ "${SKIP_TESTS:-}" != "1" ]]; then
    bold "==> tests"
    ./selftest.bash >/dev/null || { echo "selftest failed"; exit 1; }
    ok "self-hosted toolchain reproduces every golden"
fi

rm -rf "$DIST"
mkdir -p "$DIST"

# ---------------------------------------------------------------- 1. source
bold "==> source distribution"
STAGE="$DIST/voila-$VERSION"
mkdir -p "$STAGE/runtime"
cp bootstrap/voilac.c "$STAGE/voilac.c"
cp runtime/src/* "$STAGE/runtime/"
cat > "$STAGE/build.sh" <<'EOF'
#!/usr/bin/env sh
# Build the Voilà toolchain. The only requirement is a C compiler.
set -e
CC="${CC:-cc}"
$CC -std=c11 -O2 -Iruntime -o voila voilac.c runtime/*.c -lm -lpthread
echo "built ./voila — try: ./voila run hello.voi"
EOF
chmod +x "$STAGE/build.sh"
cat > "$STAGE/hello.voi" <<'EOF'
say "Voilà."
EOF
cat > "$STAGE/README" <<EOF
Voilà $VERSION — source distribution.

    ./build.sh          builds ./voila with your C compiler
    ./voila run x.voi   compile and execute
    ./voila build x.voi compile to a native binary

voilac.c is the compiler, written in Voilà and emitted as C by itself.
runtime/ is libvoila. Nothing else is needed: no Go, no Voilà, no make.
EOF
tar -C "$DIST" -czf "$DIST/voila-src.tar.gz" "voila-$VERSION"
rm -rf "$STAGE"
ok "$DIST/voila-src.tar.gz"

# ---------------------------------------------------------------- 2. natives
bold "==> native binaries (for the cross-compilers installed here)"

build_for() {   # build_for <name> <cc> [extra cflags…]
    local name="$1" cc="$2"
    shift 2
    if ! command -v "$cc" >/dev/null 2>&1; then
        skip "$name (no $cc)"
        return
    fi
    "$cc" -std=c11 -O2 -Iruntime/src -o "$DIST/voila-$name" \
          bootstrap/voilac.c runtime/src/*.c "$@" -lm -lpthread 2>/dev/null || {
        skip "$name ($cc failed — see the note on pthreads below)"
        return
    }
    ok "$DIST/voila-$name"
}

HOST="$(uname -s)-$(uname -m)"
cp bin/voila "$DIST/voila-$HOST"
ok "$DIST/voila-$HOST (this machine)"

build_for linux-amd64   x86_64-linux-gnu-gcc
build_for linux-arm64   aarch64-linux-gnu-gcc
build_for windows-amd64 x86_64-w64-mingw32-gcc

cat <<'NOTE'

Notes
  · libvoila uses POSIX threads. On Windows it needs a pthreads-providing
    toolchain (MinGW-w64 ships winpthreads); MSVC is not supported.
  · The source tarball is the portable path: it builds anywhere a C11
    compiler exists, and it is what a package maintainer should consume.
NOTE
bold "done — see $DIST/"
