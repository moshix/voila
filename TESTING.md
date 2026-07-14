# Testing Voilà

There is one command:

```sh
./selftest.bash
```

It builds the toolchain from the checked-in seed, verifies the self-hosting
fixpoint, and then holds the compiler to every golden output and every negative
fixture in the repository. Green means the language works.

```
./build.sh          bootstrap only: seed → voilac-1 → voilac-2, fixpoint asserted
./selftest.bash     the above, plus the whole corpus
./build_voila.bash  package for distribution (runs selftest first)
```

## What is actually being tested

### 1. The fixpoint

`./build.sh` compiles the compiler with the compiler, twice, and asserts that the
two generations emit **byte-identical C**. A compiler that mistranslates itself
almost never lands on a fixpoint, so this one cheap assertion covers an enormous
amount of ground — see [BOOTSTRAP.md](BOOTSTRAP.md) for the two real bugs it
caught that nothing else did.

### 2. The goldens

`tests/conformance/*.voi` — one file per language area (literals, arithmetic,
strings, collections, control flow, errors, ownership, concurrency, decimals,
parse/format, traits and generics, name binding) — each with a checked-in `.out`
(and `.err` where the program writes to stderr). `samples/*.voi` likewise, with
goldens under `samples/golden/`.

**These goldens are the semantics.** They were produced by the Go tree-walking
interpreter that bootstrapped this language, before it was deleted. That
interpreter was the oracle; these files are its verdicts, frozen. The compiler is
still held to them, so its behaviour cannot drift away from the semantics the
language was specified with — even though the oracle itself is gone.

### 3. The guide

Every program printed in [GettingStartedWithVoila.md](GettingStartedWithVoila.md)
lives in `samples/guide/`, is compiled and run by `selftest.bash`, and is
compared against a golden. A tutorial whose programs do not work is worse than
no tutorial — the reader cannot tell whether the language or their typing is at
fault — so the guide cannot drift from the compiler without the suite going red.

### 4. The negative fixtures

`tests/negative/*.voi` are programs the checker must **reject**: an ownership
cycle, a lossy widening, a non-exhaustive `match`, a use-after-move, a borrow
escaping into a struct field, a `printf` whose arguments do not match its format.
`tests/multipkg-bad/*/` are programs the loader must reject: an import cycle, a
private name, a duplicate package qualifier, a wildcard import of a user package,
a nonexistent import, a variant name claimed by two packages.

A compiler that accepts everything is not a compiler. `selftest.bash` fails if
any of these is accepted, and it compares the diagnostic text.

### 5. Memory

Voilà refcounts and has no garbage collector, so a leak is a defect in the
runtime or in the code the backend emits — not a matter of timing.

```sh
bin/voila build samples/10_life.voi -o /tmp/life
leaks --atExit -- /tmp/life          # 0 leaks for 0 total leaked bytes
```

Every sample and every conformance program is leak-clean, including the ones
that spawn tasks, throw across frames, and build a reference cycle between a
closure and the frame that owns it.

Undefined behaviour is checked with UBSan over the emitted C:

```sh
d=$(mktemp -d)
bin/voila build tests/conformance/08_tasks.voi -o "$d/x" --keep-c "$d"
cc -std=c11 -fsanitize=undefined -I"$d" -o "$d/ub" "$d/program.c" \
   runtime/src/*.c -lm -lpthread
"$d/ub"
```

(ASan aborts in its own initialiser in this environment, so `leaks` and UBSan
carry that load.)

## Adding a test

**A language feature** → a new `tests/conformance/NN_area.voi` and its golden:

```sh
bin/voila run tests/conformance/13_new.voi > tests/conformance/13_new.out
```

Read the golden before committing it. It is now the definition of correct.

**A bug** → the fixture goes wherever matches what *should* happen:
`tests/conformance/` if the program is valid and produced the wrong answer,
`tests/negative/` if it should have been rejected and was not. Both directories
are globbed; there is no list to edit.

**A lexer or parser edge case** → `tests/lexedge/`.

## The differential test (historical)

While the Go compiler still existed, `difftest.bash` diffed every stage of the
Voilà compiler against it — token streams, parse trees, flattened programs, IR
listings, emitted C — over all 79 `.voi` files in the repository, including the
compiler's own source. It passed at every stage, and that is what made deleting
the Go tree safe.

It cannot run any more, because there is nothing left to diff against. Its job
now belongs to the goldens and to the fixpoint.
