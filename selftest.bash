#!/usr/bin/env bash
# selftest.bash — end-to-end validation of the SELF-HOSTED toolchain.
#
# difftest.bash proves the Voilà compiler agrees with the Go one stage by stage
# (tokens, AST, flattened tree, IR listing, generated C). This script proves the
# thing that actually matters: a program compiled by bin/voila produces exactly
# the output the golden files say it should.
#
#     ./selftest.bash          build everything and run the whole corpus
#     ./selftest.bash quick    skip the samples (conformance only)
set -uo pipefail
cd "$(dirname "$0")"

bold() { printf '\033[1m%s\033[0m\n' "$1"; }
ok()   { printf '\033[32m  ✓ %s\033[0m\n' "$1"; }
bad()  { printf '\033[1;31m  ✗ %s\033[0m\n' "$1"; }

VOILA=./bin/voila
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if [ ! -x "$VOILA" ]; then
    bold "==> building the self-hosted toolchain"
    ./build.sh >/dev/null || { bad "build.sh failed"; exit 1; }
fi

fail=0
pass=0

# ---------------------------------------------------------------- conformance
bold "==> conformance: compiled by voilac, output must match the golden"
for f in tests/conformance/*.voi; do
    want="${f%.voi}.out"
    [ -f "$want" ] || continue
    exe="$TMP/$(basename "${f%.voi}")"
    if ! "$VOILA" build "$f" -o "$exe" >"$TMP/build.log" 2>&1; then
        bad "$(basename "$f"): build failed"
        head -3 "$TMP/build.log" | sed 's/^/      /'
        fail=$((fail + 1))
        continue
    fi
    "$exe" > "$TMP/got.txt" 2> "$TMP/got.err"
    if ! cmp -s "$TMP/got.txt" "$want"; then
        bad "$(basename "$f"): stdout differs from the golden"
        diff "$want" "$TMP/got.txt" | head -6 | sed 's/^/      /'
        fail=$((fail + 1))
        continue
    fi
    werr="${f%.voi}.err"
    if [ -f "$werr" ] && ! cmp -s "$TMP/got.err" "$werr"; then
        bad "$(basename "$f"): stderr differs from the golden"
        diff "$werr" "$TMP/got.err" | head -6 | sed 's/^/      /'
        fail=$((fail + 1))
        continue
    fi
    pass=$((pass + 1))
done
ok "conformance: $pass matched"

# ---------------------------------------------------------------- samples
if [ "${1:-}" != quick ]; then
    bold "==> samples: compiled by voilac, output must match the golden"
    spass=0
    for f in samples/*.voi; do
        want="samples/golden/$(basename "${f%.voi}").stdout"
        [ -f "$want" ] || continue
        exe="$TMP/$(basename "${f%.voi}")"
        if ! "$VOILA" build "$f" -o "$exe" >"$TMP/build.log" 2>&1; then
            bad "$(basename "$f"): build failed"
            head -3 "$TMP/build.log" | sed 's/^/      /'
            fail=$((fail + 1))
            continue
        fi
        (cd samples && "$exe" > "$TMP/got.txt" 2>"$TMP/got.err")
        if cmp -s "$TMP/got.txt" "$want"; then
            spass=$((spass + 1))
        else
            bad "$(basename "$f"): output differs from the golden"
            diff "$want" "$TMP/got.txt" | head -6 | sed 's/^/      /'
            fail=$((fail + 1))
        fi
    done
    ok "samples: $spass matched"
fi

# ---------------------------------------------------------------- multi-package
bold "==> multi-package program"
if "$VOILA" build tests/multipkg/main.voi -o "$TMP/mp" >/dev/null 2>&1; then
    "$TMP/mp" > "$TMP/got.txt" 2>&1
    if [ -f tests/multipkg/main.out ] && cmp -s "$TMP/got.txt" tests/multipkg/main.out; then
        ok "multipkg"
    elif [ ! -f tests/multipkg/main.out ]; then
        ok "multipkg (built and ran; no golden to compare)"
    else
        bad "multipkg: output differs"
        diff tests/multipkg/main.out "$TMP/got.txt" | head -6 | sed 's/^/      /'
        fail=$((fail + 1))
    fi
else
    bad "multipkg: build failed"
    fail=$((fail + 1))
fi

# ---------------------------------------------------------------- negative
# A compiler that accepts everything is not a compiler. Every fixture here is a
# program the checker must REJECT, with the diagnostic recorded beside it.
bold "==> negative: invalid programs must be rejected"
npass=0
for f in tests/negative/*.voi; do
    want="${f%.voi}.err"
    got="$("$VOILA" check "$f" 2>&1)"
    if [ -z "$got" ] || [ "$got" = ok ]; then
        bad "$(basename "$f"): ACCEPTED — the checker must reject it"
        fail=$((fail + 1))
        continue
    fi
    if [ -f "$want" ]; then
        # The golden holds the stage-0 diagnostic; compare the message text,
        # which is what a user reads (the rendering differs: stage 0 drew a
        # caret diagram).
        wmsg="$(grep -o 'error: .*' "$want" | sed 's/^error: //' | head -1)"
        gmsg="$(echo "$got" | head -1 | sed 's/^[^ ]*: //')"
        if [ -n "$wmsg" ] && [ "$wmsg" != "$gmsg" ]; then
            bad "$(basename "$f"): wrong diagnostic"
            echo "      want: $wmsg" ; echo "      got:  $gmsg"
            fail=$((fail + 1))
            continue
        fi
    fi
    npass=$((npass + 1))
done
ok "negative: $npass rejected"

# -------------------------------------------------------------- bad packages
bold "==> multi-package programs that must not load"
mpass=0
for d in tests/multipkg-bad/*/; do
    [ -f "$d/main.voi" ] || continue
    if "$VOILA" check "$d/main.voi" >/dev/null 2>&1; then
        bad "$(basename "$d"): ACCEPTED — the loader must reject it"
        fail=$((fail + 1))
    else
        mpass=$((mpass + 1))
    fi
done
ok "multipkg-bad: $mpass rejected"

# ---------------------------------------------------------------- run + cache
bold "==> the run verb (compile, cache, execute)"
if "$VOILA" run tests/conformance/01_literals.voi > "$TMP/r1.txt" 2>&1 &&
   cmp -s "$TMP/r1.txt" tests/conformance/01_literals.out; then
    ok "run reproduces the golden"
else
    bad "run does not reproduce the golden"
    fail=$((fail + 1))
fi

echo
if [ "$fail" -eq 0 ]; then
    printf '\033[1;32mSELF-HOSTED TOOLCHAIN: ALL GREEN\033[0m\n'
else
    printf '\033[1;31mSELF-HOSTED TOOLCHAIN: %d FAILURE(S)\033[0m\n' "$fail"
fi
exit "$fail"
