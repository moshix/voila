#!/usr/bin/env bash
# difftest.bash — the self-hosting differential test.
#
# Every component of the Voilà compiler (voilac/) is validated against its Go
# counterpart (internal/) over every .voi source in the tree: the two must agree
# token for token and node for node. A single differing line fails the run.
#
#     ./difftest.bash            every stage
#     ./difftest.bash tokens     the lexer only
#     ./difftest.bash ast        the parser only
#     ./difftest.bash load       the package loader only (the FLATTENED tree)
set -uo pipefail
cd "$(dirname "$0")"

WHAT="${1:-all}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "building the Go reference tools…"
go build -o "$TMP/voila" ./cmd/voila || exit 1
go build -o "$TMP/godump" ./cmd/godump || exit 1

# Every Voilà source in the repository is a test case: the samples, the
# conformance suite, the multi-package fixtures and the compiler's own sources.
mapfile -t SOURCES < <(find samples tests voilac selfhost -name '*.voi' 2>/dev/null | sort)

fail=0
# Outputs go through FILES, never command substitution: a source may contain a
# NUL (tests/lexedge/escapes.voi does), and $(...) silently drops it.
run_one() {           # run_one <verb> <file>
    local verb="$1" file="$2"
    if [ "$verb" = tokens ]; then
        "$TMP/godump" "$file" >"$TMP/g.out" 2>/dev/null
    else
        "$TMP/godump" "$verb" "$file" >"$TMP/g.out" 2>/dev/null
    fi
    "$TMP/voila" run voilac/main.voi "$verb" "$file" >"$TMP/v.out" 2>/dev/null
    if cmp -s "$TMP/g.out" "$TMP/v.out"; then
        return 0
    fi
    echo "  DIFFER ($verb): $file"
    diff "$TMP/g.out" "$TMP/v.out" | head -20 | sed 's/^/      /'
    return 1
}

for verb in tokens ast load; do
    if [ "$WHAT" != all ] && [ "$WHAT" != "$verb" ]; then continue; fi
    same=0 diffs=0
    echo
    echo "=== $verb: Go front end vs. voilac ==="
    for f in "${SOURCES[@]}"; do
        if run_one "$verb" "$f"; then
            same=$((same + 1))
        else
            diffs=$((diffs + 1))
            fail=1
        fi
    done
    echo "identical: $same   differing: $diffs"
done

echo
if [ "$fail" -eq 0 ]; then
    echo "DIFFERENTIAL TEST PASSED"
else
    echo "DIFFERENTIAL TEST FAILED"
fi
exit "$fail"
