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

# stdin_for <file.voi> — the fixture's .in file if it has one, /dev/null if
# not. EVERY compiled program below is run with this as its stdin and never
# with the terminal's: one that reads stdin would otherwise block, and the
# suite would hang waiting for a human who is not there. The path is absolute
# because two of the stages run from inside another directory.
stdin_for() {
    local i="$PWD/${1%.voi}.in"
    if [ -f "$i" ]; then printf '%s\n' "$i"; else printf '/dev/null\n'; fi
}

VOILA=./bin/voila
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

if [ ! -x "$VOILA" ]; then
    # No voila binary yet — bootstrap from the C seed (needs only cc). Once a
    # binary exists, ./build.sh (the self-hosted path) is what rebuilds it.
    bold "==> bootstrapping the toolchain from the C seed"
    ./bootstrap.bash >/dev/null || { bad "bootstrap.bash failed"; exit 1; }
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
    "$exe" < "$(stdin_for "$f")" > "$TMP/got.txt" 2> "$TMP/got.err"
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
        stdin="$(stdin_for "$f")"
        (cd samples && "$exe" < "$stdin" > "$TMP/got.txt" 2>"$TMP/got.err")
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

# ---------------------------------------------------------------- the guide
# Every program printed in GettingStartedWithVoila.md is compiled and run here.
# A tutorial whose programs do not work is worse than no tutorial: the reader
# cannot tell whether the language or their typing is at fault.
bold "==> the programs in GettingStartedWithVoila.md"
gpass=0
for f in samples/guide/g0*.voi; do
    n="$(basename "${f%.voi}")"
    want="samples/guide/golden/$n.out"
    [ -f "$want" ] || continue
    exe="$TMP/$n"
    if ! "$VOILA" build "$f" -o "$exe" >"$TMP/build.log" 2>&1; then
        bad "$n: build failed"
        head -3 "$TMP/build.log" | sed 's/^/      /'
        fail=$((fail + 1))
        continue
    fi
    # g08 reports elapsed times, which are not reproducible; the counts are.
    "$exe" < "$(stdin_for "$f")" 2>&1 | grep -v " ms$" > "$TMP/got.txt"
    if cmp -s "$TMP/got.txt" "$want"; then
        gpass=$((gpass + 1))
    else
        bad "$n: output differs from the golden"
        diff "$want" "$TMP/got.txt" | head -6 | sed 's/^/      /'
        fail=$((fail + 1))
    fi
done
# The guide's multi-package example.
if "$VOILA" build samples/guide/queens/main.voi -o "$TMP/gq" >/dev/null 2>&1 &&
   "$TMP/gq" < /dev/null > "$TMP/gq.out" 2>&1 &&
   cmp -s "$TMP/gq.out" samples/guide/golden/queens.out; then
    gpass=$((gpass + 1))
else
    bad "the guide's package example"
    fail=$((fail + 1))
fi
ok "guide: $gpass programs"

# ---------------------------------------------------------------- multi-package
# One stage per multi-package program that must BUILD and RUN: tests/multipkg
# is the broad one, tests/multipkg-shared proves two packages may declare the
# same variant and trait names (they could not before those namespaces were
# qualified).
bold "==> multi-package programs"
for d in tests/multipkg tests/multipkg-shared; do
    name="$(basename "$d")"
    if "$VOILA" build "$d/main.voi" -o "$TMP/mp" >/dev/null 2>&1; then
        "$TMP/mp" < /dev/null > "$TMP/got.txt" 2>&1
        if [ -f "$d/main.out" ] && cmp -s "$TMP/got.txt" "$d/main.out"; then
            ok "$name"
        elif [ ! -f "$d/main.out" ]; then
            ok "$name (built and ran; no golden to compare)"
        else
            bad "$name: output differs"
            diff "$d/main.out" "$TMP/got.txt" | head -6 | sed 's/^/      /'
            fail=$((fail + 1))
        fi
    else
        bad "$name: build failed"
        fail=$((fail + 1))
    fi
done

# ---------------------------------------------------------------- lexer edges
# tests/lexedge/ pins the lexer's corners: an empty `\u{}` (which is U+0000, a
# NUL inside the golden), a literal run between two interpolations, a radix
# that admits only its own digits (`0b12` is `0b1` followed by `2`), and `func`
# in type position. The assertion is the TOKEN STREAM rather than the program's
# output, because that is the stage these files are about — and because
# radix.voi deliberately does not parse, so it has no output to compare.
bold "==> lexer edge cases: the token stream must match the golden"
# A globbed stage whose glob matches nothing passes vacuously forever, so the
# fixtures' existence is itself asserted — as it is for the native tables.
if ! ls tests/lexedge/*.tokens >/dev/null 2>&1; then
    bad "lexedge: no .tokens goldens; the fixtures have gone missing"
    fail=$((fail + 1))
fi
lpass=0
for f in tests/lexedge/*.voi; do
    want="${f%.voi}.tokens"
    [ -f "$want" ] || continue
    # The two streams are kept apart: stdout is block-buffered into a file
    # while stderr is not, so folding them together would make the golden
    # depend on interleaving the moment `tokens` has anything to complain
    # about. The golden is the token stream; stderr must be empty.
    if ! "$VOILA" tokens "$f" > "$TMP/tok.txt" 2> "$TMP/tok.err"; then
        bad "$(basename "$f"): tokens failed"
        head -3 "$TMP/tok.err" | sed 's/^/      /'
        fail=$((fail + 1))
        continue
    fi
    if [ -s "$TMP/tok.err" ]; then
        bad "$(basename "$f"): tokens wrote to stderr"
        head -3 "$TMP/tok.err" | sed 's/^/      /'
        fail=$((fail + 1))
        continue
    fi
    if cmp -s "$TMP/tok.txt" "$want"; then
        lpass=$((lpass + 1))
    else
        bad "$(basename "$f"): the token stream differs from the golden"
        diff "$want" "$TMP/tok.txt" | head -6 | sed 's/^/      /'
        fail=$((fail + 1))
    fi
done
ok "lexedge: $lpass token streams"

# ---------------------------------------------------------------- negative
# A compiler that accepts everything is not a compiler. Every fixture here is a
# program the checker must REJECT, with the diagnostic recorded beside it.
bold "==> negative: invalid programs must be rejected"
npass=0
for f in tests/negative/*.voi; do
    want="${f%.voi}.expect"
    got="$("$VOILA" check "$f" 2>&1)"
    if [ -z "$got" ] || [ "$got" = ok ]; then
        bad "$(basename "$f"): ACCEPTED — the checker must reject it"
        fail=$((fail + 1))
        continue
    fi
    # Each line of the .expect file is a phrase the diagnostics MUST contain.
    # (This used to look for a .err file, which never existed, so the golden was
    # never compared and a checker emitting nonsense would still have passed.)
    #
    # A line starting with `!` is a phrase that must be ABSENT — the same
    # convention the warn stage uses. Without it a fixture could only assert
    # what a check DOES say, and the lines a fixture includes to prove a check
    # stays QUIET (a shadowed package name, a private native) were comments
    # rather than tests: an extra diagnostic never failed anything.
    missing=0
    while IFS= read -r line; do
        [ -z "$line" ] && continue
        case "$line" in
            "!"*)
                phrase="${line#!}"
                case "$got" in
                    *"$phrase"*)
                        bad "$(basename "$f"): must NOT mention: $phrase"
                        missing=1 ;;
                esac ;;
            *)
                case "$got" in
                    *"$line"*) ;;
                    *)  bad "$(basename "$f"): the diagnostic does not mention: $line"
                        missing=1 ;;
                esac ;;
        esac
    done < "$want"
    if [ "$missing" -eq 1 ]; then
        echo "      got: $got" | head -2 | sed 's/^/      /'
        fail=$((fail + 1))
        continue
    fi
    npass=$((npass + 1))
done
ok "negative: $npass rejected"

# ------------------------------------------------------- backend refusals
# The backend has a small set of documented refusals — constructs the checker
# lets through because they are well-formed, and the C backend declines to
# translate rather than answer differently from the specification. They are
# therefore rejected by `build`, not by `check`, which is what separates this
# stage from the one above.
bold "==> programs the native backend must refuse to compile"
# One fixture today, so an empty glob is a real hazard here.
if ! ls tests/negative-native/*.expect >/dev/null 2>&1; then
    bad "negative-native: no fixtures; the .expect files have gone missing"
    fail=$((fail + 1))
fi
bpass=0
for f in tests/negative-native/*.voi; do
    want="${f%.voi}.expect"
    [ -f "$want" ] || continue
    if got="$("$VOILA" build "$f" -o "$TMP/nn" 2>&1)"; then
        bad "$(basename "$f"): COMPILED — the backend must refuse it"
        fail=$((fail + 1))
        continue
    fi
    # Phrase-matched, exactly as the negative stage above.
    missing=0
    while IFS= read -r line; do
        [ -z "$line" ] && continue
        case "$got" in
            *"$line"*) ;;
            *)  bad "$(basename "$f"): the refusal does not mention: $line"
                missing=1 ;;
        esac
    done < "$want"
    if [ "$missing" -eq 1 ]; then
        echo "      got: $got" | head -2 | sed 's/^/      /'
        fail=$((fail + 1))
        continue
    fi
    bpass=$((bpass + 1))
done
ok "negative-native: $bpass refused"

# ------------------------------------------------------------------ warnings
# Warnings are the OTHER severity, and they fail in two directions. One that
# never fires is invisible; one that fires on working code gets the whole
# warning switched off by the first project it annoys — so every fixture here
# records BOTH what its diagnostics must say and what they must not.
#
#   <f>.expect   one phrase per line, substring-matched as everywhere else;
#                a line starting with `!` must be ABSENT from the output.
#   <f>.flags    extra options for `voila check` (default: none).
#   <f>.exit     the expected exit status (default: 0 — a warning is advice,
#                not a refusal, until -Werror says otherwise).
bold "==> warnings: severity, -Wno-, -Werror, --strict"
if ! ls tests/warn/*.expect >/dev/null 2>&1; then
    bad "warn: no fixtures; the .expect files have gone missing"
    fail=$((fail + 1))
fi
wpass=0
for f in tests/warn/*.voi; do
    want="${f%.voi}.expect"
    # No skipping here (unlike the stages above): a fixture whose .expect is
    # missing would pass vacuously, which is how a warning stops being tested
    # without anyone noticing.
    if [ ! -f "$want" ]; then
        bad "$(basename "$f"): no .expect beside it"
        fail=$((fail + 1))
        continue
    fi
    wflags=""
    [ -f "${f%.voi}.flags" ] && wflags="$(cat "${f%.voi}.flags")"
    wantrc=0
    [ -f "${f%.voi}.exit" ] && wantrc="$(cat "${f%.voi}.exit")"
    # Unquoted on purpose: .flags holds a whole option list.
    got="$("$VOILA" check $wflags "$f" 2>&1)"
    rc=$?
    if [ "$rc" != "$wantrc" ]; then
        bad "$(basename "$f"): exit $rc, expected $wantrc"
        echo "$got" | head -3 | sed 's/^/      /'
        fail=$((fail + 1))
        continue
    fi
    missing=0
    while IFS= read -r line; do
        [ -z "$line" ] && continue
        case "$line" in
            "!"*)
                phrase="${line#!}"
                case "$got" in
                    *"$phrase"*)
                        bad "$(basename "$f"): must NOT mention: $phrase"
                        missing=1 ;;
                esac ;;
            *)
                case "$got" in
                    *"$line"*) ;;
                    *)  bad "$(basename "$f"): does not mention: $line"
                        missing=1 ;;
                esac ;;
        esac
    done < "$want"
    if [ "$missing" -eq 1 ]; then
        echo "$got" | head -3 | sed 's/^/      /'
        fail=$((fail + 1))
        continue
    fi
    wpass=$((wpass + 1))
done
ok "warn: $wpass fixtures"

# `voila run` caches the compiled binary under ~/.cache/voila. Two properties
# have to hold, and they are held by two different mechanisms, so they get two
# checks. Both stages install cache entries; the snapshot taken here is what
# lets the cleanup at the end remove exactly the ones they made, rather than
# leaving a fresh pair behind on every suite run.
cache="${HOME:-}/.cache/voila"
ckhave=0
if [ -n "${HOME:-}" ] && [ -d "$cache" ]; then
    ckhave=1
    ls "$cache" | sort > "$TMP/ck_before.txt"
fi

# 1. END TO END: a lenient run must not license a strict one. Today the
#    refusal comes from main.voi reporting BEFORE the cache is consulted, not
#    from the key — but this is the property a user actually depends on, and
#    it must survive whichever mechanism provides it.
bold "==> voila run: --strict is never served a cached lenient binary"
"$VOILA" run tests/warn/strict.voi >/dev/null 2>&1
lenient=$?
"$VOILA" run --strict tests/warn/strict.voi >/dev/null 2>&1
strict=$?
if [ "$lenient" -eq 0 ] && [ "$strict" -ne 0 ]; then
    ok "run --strict refuses after a cached lenient run"
else
    bad "run cache: lenient exit $lenient, --strict exit $strict"
    fail=$((fail + 1))
fi

# 2. THE KEY ITSELF (diag.Policy.Key). Two -W states must land on two cache
#    entries, and one -W state must land on the same entry twice. Check 1
#    above would pass even if the key ignored -W entirely, so without this
#    the key would be untested weight — and Key() walking a MAP instead of
#    diag.Codes() would be invisible until it produced a different key on
#    each run and quietly stopped caching.
#
#    The probe source is generated fresh so the cache is guaranteed cold. What
#    is compared is the DELTA against a snapshot — `comm -13`, not a count of
#    the whole directory — so a `voila run` in another terminal cannot flap
#    this stage.
bold "==> voila run: the -W state is part of the cache key"
if [ "$ckhave" -eq 1 ]; then
    ck="$TMP/cachekey.voi"
    # The leading comment carries a unique tag, so the probe's CONTENT — which
    # is what source_key hashes — differs from every previous suite run and
    # the cache is guaranteed cold.
    {
        printf '// cache-key probe %s\n' "$(date +%s)-$$"
        cat <<'PROBE'
use "std/fmt"

func main() {
    fmt.printf("cache key probe\n")
}
PROBE
    } > "$ck"
    ls "$cache" | sort > "$TMP/ck0.txt"
    "$VOILA" run "$ck" >/dev/null 2>&1
    ls "$cache" | sort > "$TMP/ck1.txt"
    "$VOILA" run -Wno-unreachable "$ck" >/dev/null 2>&1
    "$VOILA" run -Wno-unreachable "$ck" >/dev/null 2>&1
    ls "$cache" | sort > "$TMP/ck2.txt"
    # One entry for the default policy; a SECOND for -Wno-unreachable, and the
    # repeat of it must add none — so the delta is exactly 1 then exactly 2.
    n1=$(comm -13 "$TMP/ck0.txt" "$TMP/ck1.txt" | wc -l | tr -d ' ')
    n2=$(comm -13 "$TMP/ck0.txt" "$TMP/ck2.txt" | wc -l | tr -d ' ')
    if [ "$n1" -eq 1 ] && [ "$n2" -eq 2 ]; then
        ok "a different -W state is a different cache entry, and a stable one"
    else
        bad "run cache key: new entries were $n1 then $n2, wanted 1 then 2"
        fail=$((fail + 1))
    fi
else
    ok "run cache key: skipped (no HOME, so no per-user cache to inspect)"
fi

# Both stages above are done with the cache; take back exactly what they added.
if [ "$ckhave" -eq 1 ]; then
    ls "$cache" | sort > "$TMP/ck_after.txt"
    comm -13 "$TMP/ck_before.txt" "$TMP/ck_after.txt" \
        | while IFS= read -r e; do [ -n "$e" ] && rm -f "$cache/$e"; done
fi

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

# --------------------------------------------------------------- module system
# voila.mod, the module cache, version selection and voila.sum. The cache is a
# FIXTURE directory rather than the user's real one: these tests must never
# reach the network, and must never depend on what happens to be cached on the
# machine running them.
bold "==> the module system: require, replace, the cache and voila.sum"
export VOILA_MODCACHE="$PWD/tests/mod/cache"
modpass=0
for d in tests/mod/*/; do
    name="$(basename "$d")"
    [ "$name" = cache ] && continue
    [ "$name" = bad ] && continue
    [ -f "$d/main.out" ] || continue
    if ! "$VOILA" build "$d/main.voi" -o "$TMP/md" >"$TMP/build.log" 2>&1; then
        bad "mod/$name: build failed"
        head -3 "$TMP/build.log" | sed 's/^/      /'
        fail=$((fail + 1))
        continue
    fi
    "$TMP/md" < /dev/null > "$TMP/got.txt" 2>&1
    if cmp -s "$TMP/got.txt" "$d/main.out"; then
        modpass=$((modpass + 1))
    else
        bad "mod/$name: output differs"
        diff "$d/main.out" "$TMP/got.txt" | head -6 | sed 's/^/      /'
        fail=$((fail + 1))
    fi
done
ok "mod: $modpass programs resolved and ran"

# The refusals: a malformed voila.mod (one directory per rule), an unfetched
# dependency, and voila.sum drift. Each `expect` line is a PHRASE the
# diagnostics must contain, the same convention tests/negative uses.
modbad=0
for d in tests/mod/bad/*/ tests/mod/sumdrift tests/mod/missing; do
    name="mod/$(basename "$d")"
    [ -f "$d/expect" ] || continue
    got="$("$VOILA" check "$d/main.voi" 2>&1)"
    if "$VOILA" check "$d/main.voi" >/dev/null 2>&1; then
        bad "$name: ACCEPTED — it must be rejected"
        fail=$((fail + 1))
        continue
    fi
    missing=0
    while IFS= read -r line; do
        [ -z "$line" ] && continue
        case "$got" in
            *"$line"*) ;;
            *) bad "$name: diagnostics do not contain: $line"
               echo "      got: $got"
               missing=1 ;;
        esac
    done < "$d/expect"
    if [ "$missing" = 0 ]; then
        modbad=$((modbad + 1))
    else
        fail=$((fail + 1))
    fi
done
ok "mod: $modbad refusals"

# `voila mod init` and `voila mod verify`, driven from a scratch module.
modinit="$TMP/modinit"
mkdir -p "$modinit"
if (cd "$modinit" && "$OLDPWD/$VOILA" mod init example.com/scratch >/dev/null 2>&1) &&
   grep -q '^module example.com/scratch$' "$modinit/voila.mod"; then
    ok "voila mod init"
else
    bad "voila mod init"
    fail=$((fail + 1))
fi
if (cd tests/mod/sumok && "$OLDPWD/$VOILA" mod verify 2>&1 | grep -q 'example.com/lib v1.0.0  ok'); then
    ok "voila mod verify"
else
    bad "voila mod verify"
    fail=$((fail + 1))
fi
if (cd tests/mod/sumdrift && "$OLDPWD/$VOILA" mod verify >/dev/null 2>&1); then
    bad "voila mod verify: accepted a drifted voila.sum"
    fail=$((fail + 1))
else
    ok "voila mod verify rejects a drifted voila.sum"
fi
unset VOILA_MODCACHE

# ------------------------------------------------------- cross-file diagnostics
# A diagnostic raised inside an imported package must name THAT file and THAT
# line. The loader flattens every package into one arena, so this is easy to get
# wrong — and it was: every diagnostic used to be attributed to the entry file,
# at a line that need not even exist there.
bold "==> a diagnostic in an imported package names the right file"
got="$("$VOILA" check tests/multipkg-diag/main.voi 2>&1)"
want="$(cat tests/multipkg-diag/expect)"
case "$got" in
    *"$want"*) ok "cross-file attribution" ;;
    *)  bad "cross-file attribution"
        echo "      want to contain: $want"
        echo "      got:             $got"
        fail=$((fail + 1)) ;;
esac

# ------------------------------------------------- builtin trait cross-check
# The loader must know which trait names the LANGUAGE predeclares, so that an
# imported package declaring `trait Show` cannot silently claim every bare
# `impl Show for T` in every file that imports it. `load` cannot import `typ`
# (it is the earliest stage and typ is a far heavier dependency than one list
# is worth), so the list is duplicated — and the duplicate is held equal here,
# the same way the native tables are.
bold "==> the loader and typ agree on the builtin trait names"
lt="$(sed -n '/^func builtin_trait_names/,/^}/p' voilac/load/rewrite.voi |
      grep -o '"[A-Za-z]*"' | tr -d '"' | sort)"
tt="$(sed -n '/^func BuiltinTraits/,/^}/p' voilac/typ/typ.voi |
      grep -o '"[A-Za-z]*"' | tr -d '"' | sort)"
if [ -z "$lt" ] || [ -z "$tt" ]; then
    bad "builtin traits: one of the two lists could not be read"
    fail=$((fail + 1))
elif [ "$lt" = "$tt" ]; then
    ok "builtin traits: $(printf '%s\n' "$lt" | wc -l | tr -d ' ') names agree"
else
    bad "builtin traits: the two lists differ"
    diff <(printf '%s\n' "$tt") <(printf '%s\n' "$lt") | head -6 |
        sed 's/^/      /'
    fail=$((fail + 1))
fi

# ------------------------------------------------------------- native tables
# Adding a C native means editing two hand-maintained lists, and BOTH failure
# modes are silent: a name in runtime/src/std.c but not in voilac/cg/instr.voi
# is invisible to every program, and a name in instr.voi but not in std.c
# compiles and links and then dies at startup with "missing runtime function".
# This stage turns that trap into a red test.
bold "==> the C runtime and the compiler agree on the native names"
# Every sort and comm below is LC_ALL=C: these names carry `.` and `_`, and a
# UTF-8 locale's collation ignores punctuation while comm compares bytes — the
# two would disagree about what "sorted" means and invent differences that are
# not there. The setting is per-command so no later stage inherits it.
# The C side: the string keys of table[] plus the E(name) entries of
# str_table[] (the `#define E(n)` line is a macro, not an entry).
sed -n '/^static const Entry table\[\] = {/,/^};/p' runtime/src/std.c \
    | grep -o '{"[^"]*"' | tr -d '{"' > "$TMP/nat_c.txt"
sed -n '/^static const Entry str_table\[\] = {/,/^};/p' runtime/src/std.c \
    | grep -v '^#' | grep -o 'E([a-z0-9_]*)' | sed 's/E(\(.*\))/\1/' \
    >> "$TMP/nat_c.txt"
# The compiler side: one list per function. Most of the surface now lives in
# voilac/low/names.voi rather than cg/instr.voi, because chk/stdpkg.voi judges
# `pkg.member` against the very same lists and chk must not import cg — so
# cg/instr.voi's pkg_natives and pkg_constant_names are splices, not copies.
# Each list is read WHERE IT LIVES; going through the wrapper would extract
# nothing.
# voi_list <file> <func> — the quoted strings in a `func NAME() []str` body.
# The range ends at the first line that is `}` in column 1, so it wants the
# MULTI-LINE form; point it at a one-line `func f() []str { return g() }` and it
# runs on into the next function. Every list it is used on below is multi-line,
# and the one-line splices go through voi_body, which brace-counts.
voi_list() {
    sed -n "/^func $2() \[\]str {/,/^}/p" "$1" | grep -o '"[^"]*"' | tr -d '"'
}
nat_voi() { voi_list voilac/cg/instr.voi "$1"; }
nat_low() { voi_list voilac/low/names.voi "$1"; }
nat_voi prelude_natives  > "$TMP/nat_v.txt"
# str's method-call duality set: cg/instr.voi's str_natives returns it and
# typ.PrimitiveMethod judges bounds against it.
nat_low StrMethodNames >> "$TMP/nat_v.txt"
# Package CONSTANTS are deliberately absent from table[]: LOADFN of one yields
# a value, and the runtime resolves it in vl_lookup_value, not as a function
# pointer. So they are subtracted before the comparison.
nat_low PkgConstantNames | LC_ALL=C sort -u > "$TMP/nat_k.txt"
# The package natives come in two lists — StdCNatives for the packages that
# are only C, StdSrcNatives for the ones with Voilà source grafted alongside.
# They go through the SAME constant subtraction: std/time's six duration
# constants are among them, as are math.pi and the three os handles.
{ nat_low StdCNatives
  nat_low StdSrcNatives
} | LC_ALL=C sort -u \
    | LC_ALL=C comm -23 - "$TMP/nat_k.txt" >> "$TMP/nat_v.txt"
# …and the three functions in cg/instr.voi that splice those lists together
# must hold no name of their own, or this stage would stop seeing it: a name
# written back into `pkg_natives` and NOT into std.c would once have shown up
# as "in instr.voi but not std.c", and now would be read from neither side.
# chk cannot import cg at all, so a name kept there would also be invisible to
# chk/stdpkg.voi. voi_body brace-counts, so it reads a one-line `func f() []str
# { return g() }` as exactly that line and stops.
voi_body() {
    awk -v fn="$2" '
        !inb && $0 ~ "^func " fn "\\(" { inb = 1 }
        inb {
            print
            depth += gsub(/\{/, "{") - gsub(/\}/, "}")
            if (depth <= 0) exit
        }
    ' "$1"
}
for w in str_natives pkg_natives pkg_constant_names; do
    body="$(voi_body voilac/cg/instr.voi "$w")"
    if [ -z "$body" ]; then
        # Renaming a splice would make the assertion below match nothing and
        # pass forever, while names written into the renamed function stayed
        # invisible to both sides of the diff. So its EXISTENCE is asserted
        # first, exactly as the fixture globs above assert theirs.
        bad "native tables: cg/instr.voi has no \`$w()\` — it must have been renamed"
        fail=$((fail + 1))
        continue
    fi
    stray="$(printf '%s\n' "$body" | grep -o '"[^"]*"' | tr -d '"')"
    if [ -n "$stray" ]; then
        bad "cg/instr.voi's \`$w()\` must hold no names of its own —"
        bad "move these to voilac/low/names.voi, where chk can read them:"
        echo "$stray" | sed 's/^/        /'
        fail=$((fail + 1))
    fi
done
LC_ALL=C sort -u "$TMP/nat_c.txt" -o "$TMP/nat_c.txt"
LC_ALL=C sort -u "$TMP/nat_v.txt" -o "$TMP/nat_v.txt"
only_c="$(LC_ALL=C comm -23 "$TMP/nat_c.txt" "$TMP/nat_v.txt")"
only_v="$(LC_ALL=C comm -13 "$TMP/nat_c.txt" "$TMP/nat_v.txt")"
if [ ! -s "$TMP/nat_c.txt" ] || [ ! -s "$TMP/nat_v.txt" ]; then
    # An empty side means the tables were renamed and the extraction above no
    # longer finds them — which would make this stage pass vacuously forever.
    bad "native tables: extracted nothing; the table names must have changed"
    fail=$((fail + 1))
elif [ -n "$only_c" ] || [ -n "$only_v" ]; then
    if [ -n "$only_c" ]; then
        bad "in runtime/src/std.c but not in cg/instr.voi (unreachable):"
        echo "$only_c" | sed 's/^/        /'
    fi
    if [ -n "$only_v" ]; then
        bad "in cg/instr.voi but not in runtime/src/std.c (link-time death):"
        echo "$only_v" | sed 's/^/        /'
    fi
    fail=$((fail + 1))
else
    ok "native tables: $(wc -l < "$TMP/nat_c.txt" | tr -d ' ') names agree"
fi

# ------------------------------------------------------- native arities
# A native carries no static signature, so low.NativeArities is the only place
# that says how many arguments one takes and chk/native.voi is the only reader.
# A native added without an arity is INVISIBLE to that check — the exact silent
# gap the stage above exists to close for the names themselves — so the two
# name sets are held equal here.
bold "==> every native has an argument count"
nat_low NativeArities | cut -d'|' -f1 | LC_ALL=C sort -u > "$TMP/nat_a.txt"
only_a="$(LC_ALL=C comm -23 "$TMP/nat_a.txt" "$TMP/nat_v.txt")"
only_n="$(LC_ALL=C comm -13 "$TMP/nat_a.txt" "$TMP/nat_v.txt")"
if [ ! -s "$TMP/nat_a.txt" ]; then
    bad "native arities: extracted nothing; NativeArities must have been renamed"
    fail=$((fail + 1))
elif [ -n "$only_a" ] || [ -n "$only_n" ]; then
    if [ -n "$only_a" ]; then
        bad "in NativeArities but is no native:"
        echo "$only_a" | sed 's/^/        /'
    fi
    if [ -n "$only_n" ]; then
        bad "a native with no arity — chk/native.voi cannot judge these calls:"
        echo "$only_n" | sed 's/^/        /'
    fi
    fail=$((fail + 1))
else
    ok "native arities: $(wc -l < "$TMP/nat_a.txt" | tr -d ' ') natives bounded"
fi
# The std/str half is bounded TWICE — at compile time from NativeArities and at
# run time by str_arity[] in runtime/src/str.c. The runtime copy is not
# redundant: it is what still catches the METHOD spelling `s.substr()`, whose
# callee is an expression rather than a name, so the checker cannot judge it
# and `"hello".substr()` was a live SIGSEGV. Two tables that disagreed would
# refuse the same call with two different bounds, so BOTH columns are compared.
sed -n '/^static const StrArity str_arity\[\] = {/,/^};/p' runtime/src/str.c \
    | grep -o '{"[a-z0-9_]*", *[0-9]*, *[0-9]*}' \
    | sed 's/{"\([a-z0-9_]*\)", *\([0-9]*\), *\([0-9]*\)}/\1 \2 \3/' \
    | LC_ALL=C sort > "$TMP/str_c_ar.txt"
nat_low StrMethodNames | LC_ALL=C sort -u > "$TMP/str_names.txt"
nat_low NativeArities | grep -v '\.' | awk -F'|' '{print $1, $2, $3}' \
    | awk 'NR==FNR{n[$1]=1;next} n[$1]{print $1, $2, $3}' "$TMP/str_names.txt" - \
    | LC_ALL=C sort > "$TMP/str_v_ar.txt"
if [ ! -s "$TMP/str_c_ar.txt" ] || [ ! -s "$TMP/str_v_ar.txt" ]; then
    bad "str arities: extracted nothing; str_arity[] must have been renamed"
    fail=$((fail + 1))
elif ! diff -q "$TMP/str_c_ar.txt" "$TMP/str_v_ar.txt" >/dev/null; then
    bad "runtime/src/str.c's str_arity[] and low.NativeArities disagree:"
    diff "$TMP/str_c_ar.txt" "$TMP/str_v_ar.txt" | head -8 | sed 's/^/        /'
    fail=$((fail + 1))
else
    ok "str arities: $(wc -l < "$TMP/str_c_ar.txt" | tr -d ' ') bounds agree with the runtime"
fi

# The same trap one level up, on PACKAGE names. The loader decides which `use`
# is admitted (std_c_packages plus the generated std.Packages); the lowerer
# decides which dotted name may carry a package qualifier (PackageNames). A
# package in the first and not the second grafts and then reports its own
# functions as unknown identifiers; in the second and not the first, the `use`
# is refused while bare calls still compile. Neither shows up in a build.
bold "==> the loader and the lowerer agree on the std package names"
{ voi_list voilac/load/load.voi std_c_packages
  voi_list voilac/std/blob.voi Packages
} | LC_ALL=C sort -u > "$TMP/pkg_load.txt"
voi_list voilac/low/names.voi PackageNames \
    | LC_ALL=C sort -u > "$TMP/pkg_low.txt"
only_load="$(LC_ALL=C comm -23 "$TMP/pkg_load.txt" "$TMP/pkg_low.txt")"
only_low="$(LC_ALL=C comm -13 "$TMP/pkg_load.txt" "$TMP/pkg_low.txt")"
if [ ! -s "$TMP/pkg_load.txt" ] || [ ! -s "$TMP/pkg_low.txt" ]; then
    bad "std packages: extracted nothing; a list must have been renamed"
    fail=$((fail + 1))
elif [ -n "$only_load" ] || [ -n "$only_low" ]; then
    if [ -n "$only_load" ]; then
        bad "the loader admits, but low/names.voi cannot qualify:"
        echo "$only_load" | sed 's/^/        /'
    fi
    if [ -n "$only_low" ]; then
        bad "low/names.voi qualifies, but the loader refuses the \`use\`:"
        echo "$only_low" | sed 's/^/        /'
    fi
    fail=$((fail + 1))
else
    ok "std packages: $(wc -l < "$TMP/pkg_load.txt" | tr -d ' ') names agree"
fi

# ---------------------------------------------------------------- optimizer
# Every conformance golden must hold at every implemented -O level, and the
# optimizer must be deterministic (the fixpoint depends on it). Levels are
# added here as they land: -O1 (M3), -O2 (M4), -O3 (M5).
OPT_LEVELS="-O1 -O2 -O3"
for LV in $OPT_LEVELS; do
    bold "==> optimizer: goldens at $LV"
    opass=0
    for f in tests/conformance/*.voi; do
        n="$(basename "${f%.voi}")"
        want="tests/conformance/$n.out"
        [ -f "$want" ] || continue
        exe="$TMP/opt_$n"
        if ! "$VOILA" build $LV "$f" -o "$exe" >"$TMP/opt.log" 2>&1; then
            bad "$n: build failed at $LV"
            head -3 "$TMP/opt.log" | sed 's/^/      /'
            fail=$((fail + 1))
            continue
        fi
        "$exe" < "$(stdin_for "$f")" > "$TMP/opt.out" 2> "$TMP/opt.err"
        if ! cmp -s "$TMP/opt.out" "$want"; then
            bad "$n: stdout differs at $LV"
            diff "$want" "$TMP/opt.out" | head -4 | sed 's/^/      /'
            fail=$((fail + 1))
            continue
        fi
        werr="tests/conformance/$n.err"
        if [ -f "$werr" ] && ! cmp -s "$TMP/opt.err" "$werr"; then
            bad "$n: stderr differs at $LV"
            fail=$((fail + 1))
            continue
        fi
        opass=$((opass + 1))
    done
    ok "optimizer $LV: $opass goldens hold"
done

# The tests/opt probes carry their own goldens and must hold byte-for-byte at
# EVERY level — that identity (traps, typed unboxing, frame elision, and the
# thread-safe frame refcounts) is the whole point of these fixtures. Gating
# them here keeps a golden from silently drifting (a message-rendering change
# once slipped past because these were run by hand, not by the suite).
if ls tests/opt/*.voi >/dev/null 2>&1; then
    bold "==> optimizer: tests/opt probes at -O0/-O1/-O2/-O3"
    ppass=0
    for f in tests/opt/*.voi; do
        n="$(basename "${f%.voi}")"
        want="tests/opt/$n.out"
        [ -f "$want" ] || continue
        for LV in "" "-O1" "-O2" "-O3"; do
            exe="$TMP/optp_$n"
            if ! "$VOILA" build $LV "$f" -o "$exe" >"$TMP/optp.log" 2>&1; then
                bad "$n: build failed at ${LV:--O0}"
                head -3 "$TMP/optp.log" | sed 's/^/      /'
                fail=$((fail + 1)); continue
            fi
            si="$(stdin_for "$f")"
            if ! "$exe" < "$si" 2>/dev/null | cmp -s - "$want"; then
                bad "$n: output differs from golden at ${LV:--O0}"
                fail=$((fail + 1)); continue
            fi
        done
        ppass=$((ppass + 1))
    done
    ok "optimizer probes: $ppass held at every level"
fi

bold "==> optimizer: determinism (--emit=c -O1 twice, byte-identical)"
"$VOILA" build -O1 --emit=c tests/conformance/05_control.voi > "$TMP/det1.c" 2>&1
"$VOILA" build -O1 --emit=c tests/conformance/05_control.voi > "$TMP/det2.c" 2>&1
if cmp -s "$TMP/det1.c" "$TMP/det2.c"; then
    ok "deterministic"
else
    bad "optimizer output is nondeterministic"
    fail=$((fail + 1))
fi

# ---------------------------------------------------------------- run + cache
bold "==> the run verb (compile, cache, execute)"
if "$VOILA" run tests/conformance/01_literals.voi < /dev/null \
       > "$TMP/r1.txt" 2>&1 &&
   cmp -s "$TMP/r1.txt" tests/conformance/01_literals.out; then
    ok "run reproduces the golden"
else
    bad "run does not reproduce the golden"
    fail=$((fail + 1))
fi

# ---------------------------------------------------------------- stress
# tests/stress/churn.voi walks every path where reference counting is easy to
# get wrong at once — closures capturing frames, exceptions unwinding through
# defers, values crossing channels, shared/weak graphs, deep recursion, string
# building, decimals. Under `leaks` it is the memory gate (TESTING.md §6);
# here it is the cheap one: it must build, run, and exit 0.
bold "==> refcount stress (tests/stress/churn.voi)"
churn=tests/stress/churn.voi
if ! "$VOILA" build "$churn" -o "$TMP/churn" >"$TMP/churn.log" 2>&1; then
    bad "churn: build failed"
    head -3 "$TMP/churn.log" | sed 's/^/      /'
    fail=$((fail + 1))
elif "$TMP/churn" < /dev/null > "$TMP/churn.out" 2>&1; then
    ok "churn ran clean"
else
    bad "churn: exited non-zero"
    tail -3 "$TMP/churn.out" | sed 's/^/      /'
    fail=$((fail + 1))
fi

echo
if [ "$fail" -eq 0 ]; then
    printf '\033[1;32mSELF-HOSTED TOOLCHAIN: ALL GREEN\033[0m\n'
else
    printf '\033[1;31mSELF-HOSTED TOOLCHAIN: %d FAILURE(S)\033[0m\n' "$fail"
fi
exit "$fail"
