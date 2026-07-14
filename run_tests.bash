#!/usr/bin/env bash
#
# run_tests.bash — the full test procedure, in the order TESTING.md documents.
# Every layer must be green before the next one is worth running.
#
#   ./run_tests.bash          # everything except the leak checker
#   ./run_tests.bash --leaks  # also run the memory checks (slow)

set -euo pipefail
cd "$(dirname "$0")"

pass() { printf '\033[32m  ✓ %s\033[0m\n' "$1"; }
step() { printf '\033[1m==> %s\033[0m\n' "$1"; }

step "1. format and vet"
test -z "$(gofmt -l .)" || { gofmt -l .; echo "gofmt: files need formatting"; exit 1; }
go vet ./...
pass "gofmt + go vet clean"

step "2. unit tests (lexer, parser, dec, checker, IR)"
go test ./internal/...
pass "unit tests"

step "3. front-end goldens (IR listings, sample outputs, negative diagnostics)"
go test ./cmd/voila/
pass "goldens"

step "4. conformance — interpreted"
go test ./tests/
pass "conformance (interpreter)"

step "5. conformance — NATIVE, both engines compared"
VOILA_NATIVE=1 go test ./tests/
pass "conformance (native + equivalence)"

step "6. self-host differential (Voilà lexer vs Go lexer)"
go test ./cmd/voila/ -run TestSelfhostLexerDifferential
pass "differential"

if [[ "${1:-}" == "--leaks" ]]; then
  step "7. memory: refcount discipline under stress"
  go build -o /tmp/voila-test ./cmd/voila
  /tmp/voila-test build tests/stress/churn.voi -o /tmp/churn-test
  if command -v leaks >/dev/null 2>&1; then
    # `leaks` prints a harmless "not debuggable" note for ad-hoc-signed
    # binaries but still reports the counts, so read the count line.
    out=$(leaks --atExit -- /tmp/churn-test 2>&1 | grep -oE "[0-9]+ leaks for [0-9]+ total" | head -1)
    echo "  $out"
    [[ "$out" == "0 leaks for 0 total" ]] \
      && pass "leaks: 0 bytes" || { echo "LEAKS DETECTED"; exit 1; }
  elif command -v valgrind >/dev/null 2>&1; then
    valgrind --leak-check=full --error-exitcode=9 /tmp/churn-test >/dev/null
    pass "valgrind clean"
  else
    echo "  (no leak checker found; skipping)"
  fi
fi

printf '\n\033[1;32mALL GREEN\033[0m\n'
