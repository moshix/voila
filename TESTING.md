# Testing Voilà

There is one command:

```sh
./selftest.bash
```

It builds the toolchain (from an existing Voilà binary, or — on a machine with
none — from the checked-in C seed via `bootstrap.bash`), verifies the
self-hosting fixpoint, and then holds the compiler to every golden output and
every negative fixture in the repository. Green means the language works.

```
./build.sh          self-hosted build: a voila >= 0.4.1 → voilac-1 → voilac-2, BOTH fixpoints
./bootstrap.bash    new machine, no voila yet: cc the C seed first, then build.sh
./selftest.bash     build (bootstrap.bash if there is no binary yet), plus the whole corpus
./build_voila.bash  package for distribution (runs selftest first)
```

## What is actually being tested

### 1. The fixpoint

`./build.sh` compiles the compiler with the compiler, twice, and asserts that the
two generations emit **byte-identical C**. A compiler that mistranslates itself
almost never lands on a fixpoint, so this one cheap assertion covers an enormous
amount of ground — see [BOOTSTRAP.md](BOOTSTRAP.md) for the two real bugs it
caught that nothing else did.

Since 0.4 the shipped compiler is built at `-O3` and there are TWO fixpoints:
the classic one on default emission, and an **optimized fixpoint** — the
`-O3`-built generation-1 and generation-2 compilers must emit byte-identical
`-O3` C for the compiler itself. A smuggled nondeterminism (map iteration,
say) in any optimizer pass breaks it immediately.

### 2. The optimizer gates

The optimizer is opt-in, and its contract is byte-equivalence: same outputs,
same trap messages, at every level. `selftest.bash` enforces this by
compiling the ENTIRE conformance suite at `-O1`, `-O2`, and `-O3` and
comparing against the same goldens as the default build, plus an emission
determinism check (`--emit=c -O1` twice, byte-compared). The dedicated
probes live in `tests/opt/`:

- `unwind.voi` — throws through `-O2`-elided frames holding boxed locals
  (run under `leaks`: the unwinder must release them);
- `traps.voi` — every arithmetic trap message, byte-compared across levels
  (the messages live once, in the runtime's cold `vl_trap_*` functions);
- `typing_adv.voi` — adversarial cases for the `-O3` inference: match-bind
  counters, parse targets, closure-captured counters, setjmp interplay,
  int/float joins, loop-carried temporaries, hot-loop overflow, `dec`;
- `shared_frames.voi` — 8 tasks hammering one closure-captured frame's
  refcount (the biased-RC scheme); ThreadSanitizer-clean on Linux.

### 3. The goldens

`tests/conformance/*.voi` — one file per language area (literals, arithmetic,
strings, collections, control flow, errors, ownership, concurrency, decimals,
parse/format, traits and generics, name binding, networking, standard input and
`std/io`, generic bounds, `std/sync`, `std/path`, `std/time`'s calendar,
`use` aliases, `std/test`, `std/csv`, `std/crypto`, `std/regex`, `std/http`) —
each with a checked-in
`.out` (and `.err` where the program writes to stderr). `samples/*.voi` likewise, with
goldens under `samples/golden/`.

**These goldens are the semantics.** They were produced by the Go tree-walking
interpreter that bootstrapped this language, before it was deleted. That
interpreter was the oracle; these files are its verdicts, frozen. The compiler is
still held to them, so its behaviour cannot drift away from the semantics the
language was specified with — even though the oracle itself is gone.

**Standard input** is `<test>.in` when the fixture provides that file, and
`/dev/null` otherwise. This holds for *every* stage that runs a compiled
program — conformance, samples, the guide, multipkg, both optimizer stages and
the `run` verb — not only the one that owns the golden: the optimizer stage
re-runs the whole conformance corpus at `-O1`/`-O2`/`-O3`, so a fixture whose
stdin were wired up in only one place would pass once and then fail three
times. It is never the terminal, either: a program that reads stdin would
block, and the suite would hang waiting for a human who is not there.

### 4. The guide

Every program printed in [GettingStartedWithVoila.md](GettingStartedWithVoila.md)
lives in `samples/guide/`, is compiled and run by `selftest.bash`, and is
compared against a golden. A tutorial whose programs do not work is worse than
no tutorial — the reader cannot tell whether the language or their typing is at
fault — so the guide cannot drift from the compiler without the suite going red.

### 5. The negative fixtures

`tests/negative/*.voi` are programs the checker must **reject**: an ownership
cycle, a lossy widening, a non-exhaustive `match`, a use-after-move, a borrow
escaping into a struct field, a `printf` whose arguments do not match its
format, a `use` of a std package the specification documents but this release
does not implement, and a `use` of a std package that is simply misspelled.

The three `std*member`-shaped fixtures cover the other half of §13.4, a
`pkg.member` that names nothing: `stdmember.voi` misspells a member of a
grafted Voilà-source package, `stdnouse.voi` names a real member of one that
was never imported, and `stdnative.voi` misspells a member of a C-native
package, where the natives table and the package constants are the whole
declared surface. All three carry the union that must stay silent beside the
misses — a native, a constant that is deliberately absent from that table, both
halves of `std/time`, a type in type position, and a binding that has taken the
qualifier back.

The type-checker fixtures are the same idea one layer up: an `impl` that
promises a trait and provides nothing, a method whose shape does not match the
trait's, an `impl` for a trait that was never declared, a generic bound naming
a trait that does not exist, a generic bound left UNSATISFIED at each of the
three places a type argument is written — a composite literal, an annotation
and an explicit `f[T](x)` — a reassigned `let` and a reassigned `const`, a
write through a borrowed parameter and through a non-`mut` `self`, a type name
that resolves to nothing, a function whose control can reach the end of a body
that owes a result, a `return` of the wrong number of values, and an enum
variant built and matched with the wrong payload count.

Several of them also carry the cases that must stay **legal**, side by side
with the ones that must not: `letassign.voi` mutates a field and an index of a
`let` binding and shadows one with a `var`; `borrowparam.voi` rebinds the bare
name of a borrowed parameter, which the caller cannot observe; `retarity.voi`
forwards a whole tuple from one expression; and `noreturn.voi` ends functions
with a `fallthrough` switch, a `while true`, a `group` block and an
`os.exit`. A fixture that only recorded the rejections would let the checks be
tightened until they rejected working programs — three of those five legal
cases are there because a review found the check doing exactly that. The bound
fixtures follow the same discipline from the other side: `bound_inst.voi`
carries a type that satisfies its bound through an INHERENT impl block, and
`tests/conformance/17_bounds.voi` is nothing but the cases that must stay
legal — the union rule, a nested type argument, a generic parameter at the
definition site, a trait as the argument, and a builtin bound, which has no
method set for anything to be judged against.

`tests/multipkg-bad/*/` are programs the loader must reject: an import cycle, a
private name, a duplicate package qualifier, a wildcard import of a user package,
a nonexistent import, a bare variant name two imports both offer, and a
package-private variant reached by its BARE name — short-name resolution has to
be a convenience on top of package privacy, never a way around it.

`tests/multipkg-shared/` is the positive counterpart, and it used to be a
refusal: two packages each declare `Empty`, `Node` and a trait `Named`. Enum
variant and trait names are package-qualified now (§11.2.1), so that program is
legal — and the fixture pins every spelling that has to keep them apart:

- a variant of the WRONG package's enum, which must fail to match at run time
  rather than silently succeed;
- a qualified trait in an `impl` header, a generic bound and an annotation;
- the entry file's OWN `enum Local { Empty }` beating two imports that both
  offer a bare `Empty` — the entry file has no package to be qualified by, so
  step 1 of the short-name rule has to be given to it explicitly;
- a package declaring `Some` and `Show`, which must NOT reach an importer.
  Those are the language's names: `vl_match` tests `Some`/`Ok`/`Err` by BARE
  name before it looks at any enum, so a hijacked `Some(x)` pattern would
  compile and then never fire. The nine builtin trait names are duplicated
  between `load/rewrite.voi` and `typ.BuiltinTraits()` and held equal by a
  cross-check stage, the same way the native tables are.

`tests/mod/` is the module system. `tests/mod/cache/` is a FIXTURE module
cache, pointed at by `$VOILA_MODCACHE` for the duration of the stage: these
tests must never reach the network, and must never depend on what happens to be
cached on the machine running them. The directories with a `main.out` must
build and run — a cache hit, a `replace` to a local path, transitive selection,
and the maximum-version rule — and those with an `expect` must be refused: one
directory per `voila.mod` rule under `tests/mod/bad/`, plus an unfetched
dependency and `voila.sum` drift. `tests/mod/sumok/voila.sum` pins a real hash,
so editing anything under `tests/mod/cache/example.com/lib@v1.0.0/` turns that
fixture red on purpose.

`tests/negative-native/*.voi` are the backend's **documented refusals** —
programs the checker lets through because they are well-formed, and the C
backend declines to translate rather than answer differently from the
specification (today: a closure capturing a loop variable, whose fix is
per-iteration frames). They are rejected by `build`, not by `check`, which is
what puts them in their own stage.

Each `.expect` file is matched **line by line as a substring** of the
diagnostics, not byte-compared. Adding a diagnostic therefore cannot break an
existing fixture — which is what lets a new *warning* appear on stderr beside
an existing refusal without going red; rewording one only breaks it if the
recorded phrase disappears.

A compiler that accepts everything is not a compiler. `selftest.bash` fails if
any of these is accepted, and it compares the diagnostic text.

### 6. Memory

Voilà refcounts and has no garbage collector, so a leak is a defect in the
runtime or in the code the backend emits — not a matter of timing.

```sh
bin/voila build samples/10_life.voi -o /tmp/life
leaks --atExit -- /tmp/life          # 0 leaks for 0 total leaked bytes
```

Every sample and every conformance program is leak-clean, including the ones
that spawn tasks, throw across frames, and build a reference cycle between a
closure and the frame that owns it.

`tests/stress/churn.voi` walks every path where refcounting is easy to get
wrong at once — closures capturing frames, exceptions unwinding through
defers, values crossing channels, shared/weak graphs, deep recursion, string
building, decimals. Under `leaks` it is the gate for this section; in
`selftest.bash` it is the cheap version of the same test, which is that it
builds, runs, and exits 0.

```sh
bin/voila build tests/stress/churn.voi -o /tmp/churn
leaks --atExit -- /tmp/churn         # 0 leaks for 0 total leaked bytes
```

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

### 7. The lexer's edges

`tests/lexedge/*.voi` pin the corners of the lexer: an empty `\u{}` (which is
U+0000, so the golden contains a real NUL byte), a literal run between two
interpolations and one interpolation nested inside another, a radix that admits
only its own digits (`0b12` is `0b1` followed by `2`), and `func` in type
position.

What is compared is the **token stream** — `bin/voila tokens <file>` against a
checked-in `.tokens` golden — not the program's output. That is the stage these
files are about, and `radix.voi` deliberately does not parse, so it has no
output to compare.

### 8. The native tables

Adding a C native means editing two hand-maintained lists — `table[]` and
`str_table[]` in `runtime/src/std.c`, and the compiler-side list that matches
it — and **both** failure modes are silent. Only `prelude_natives()` is still
written out in `voilac/cg/instr.voi`; the other four live in
`voilac/low/names.voi`, because a second stage needs each of them on its own
and `chk` must not import `cg`:

- `StrMethodNames()` — std/str, reachable bare, as `str.NAME` and as a method.
  Read again by `typ.PrimitiveMethod`, which judges a generic bound against it.
- `StdCNatives()` — the packages that are only C (fmt, os, io, math, …).
- `StdSrcNatives()` — the natives of the packages that also have Voilà source
  grafted alongside (net, sync, time).
- `PkgConstantNames()` — `time.Second`, `math.pi`, `os.stdout`, …

All four are read again by `chk/stdpkg.voi`, which judges `pkg.member` against
their union so that a misspelling is reported at the reference instead of
refused by the backend. `str_natives()`, `pkg_natives()` and
`pkg_constant_names()` in `instr.voi` are splices of those four, not copies, so
**this stage reads each list where it lives** — extracting through a wrapper
would find nothing and pass vacuously — and it asserts that the three wrappers
hold **no name of their own**, because one written back into `pkg_natives()`
would then be read from neither side.

The failure modes:

- in `std.c` but not in the compiler → the function is invisible to every program;
- in the compiler but not `std.c` → the program compiles and links, then prints
  `voila: missing runtime function` at startup.

`selftest.bash` extracts both sets and diffs them, printing any name present on
only one side. Package *constants* (`time.Second`, `math.pi`, `os.stdout`, …)
are subtracted first: the runtime resolves those in `vl_lookup_value`, not as
function pointers, so they belong in `PkgConstantNames()` and must **not** be
in `table[]`. `chk/stdpkg.voi` adds them back, because a constant is a member a
program may legitimately name and no natives table will ever hold it.

### 8b. The native arities

A native carries **no static signature**: the code generator knows only a bare
name, so nothing in the compiler could count a call's arguments and nothing in
the runtime knew how many to expect. `time.millis()` reached a body that reads
`argv[0]` the caller never provided and died of SIGSEGV — after `check` had
said `ok`. `NativeArities()` (`voilac/low/names.voi`) gives every native a
`name|min|max` (`-1` = no maximum) and `chk/native.voi` enforces it at compile
time, which is where the check/build agreement main.voi guards requires it.

Two stages guard the table, because both of its failure modes are silent:

- **coverage** — `NativeArities()` and the four name lists above must hold the
  same 233 names, minus the constants. A native added without an arity is
  simply not judged, which is the same silent gap stage 8 closes for the names.
- **agreement with the runtime** — `str_arity[]` in `runtime/src/str.c` bounds
  the std/str half a second time, at run time, because that is what still
  catches the METHOD spelling (`s.substr()`, `s.trim_left(".")`) that the
  checker cannot judge: the receiver is an expression, and its type is what
  would decide. std/str is the only package with that second spelling, which
  is why it is the only one bounded twice. Two tables that disagreed would
  refuse the same call with two different bounds, so both columns of all 52
  entries are compared.

`min` and `max` answer different questions and are read from different
evidence — see the comment on `NativeArities()`. The one rule that matters
when adding an entry: **an optional argument is real**, and rejecting a working
call (`str.substr(s, 2)`, `os.input()`, `io.buffered(r)`) is a worse bug than
the crash the table exists to prevent.

Two fixtures pin the runtime half, one per direction, because an abort ends the
program and a file can only prove one: `19_strarity.voi` (surplus) and
`24_strmin.voi` (missing). `tests/negative/nativearity.voi` pins the
compile-time half, in both directions and with `!` lines for the calls that
must stay accepted.

One hole is known and is not closable here: a SPREAD call (`f(xs...)`) has no
static count, so neither table can judge it. Every other spelling is covered.

The same trap exists one level up, on **package** names, and gets its own
stage: `std_c_packages()` (`voilac/load/load.voi`) plus the generated
`std.Packages()` decide which `use` the loader admits, while `PackageNames()`
(`voilac/low/names.voi`) decides which dotted name may carry a package
qualifier. The two must be equal. A package in the first only grafts and then
reports its own functions as unknown identifiers; in the second only, the `use`
is refused while bare calls still compile. Neither shows up in a build.

### 9. The warnings

A warning is the other severity, and it fails in two directions. One that never
fires is invisible; one that fires on working code gets the whole warning
switched off by the first project it annoys. `tests/warn/` records both halves.

```
<f>.voi      the program
<f>.expect   one phrase per line, substring-matched; a line starting with `!`
             must be ABSENT from the output
<f>.flags    extra options for `voila check` (default: none)
<f>.exit     the expected exit status (default: 0 — a warning is advice, not a
             refusal, until -Werror says otherwise)
```

`typemismatch.voi` fires every diagnostic `chk/expr.voi` can produce, once
each: an initialiser, an assignment, an argument (including one whose declared
type is a generic parameter that the receiver's own type argument resolves,
which is the only caller of `typ.Subst`), a returned value, an undeclared
field, an unknown method, a method arity, and an unknown method on a
trait-typed receiver. `typesilent.voi` beside it is the more important half
and its `.expect` is nothing but absence lines: generics at their definition
site, a method inherited from a trait default, trait-typed values and
receivers, the `str` primitive/package duality in all three spellings, `?T`
and `T!` through `must`/`else`/`nil`, the numeric lattice, a variadic
parameter, a struct field holding a callable, `unit`, a box, and the shapes
`each` really has. The same program runs as
`tests/conformance/30_exprtype.voi`, because the reason each shape is accepted
is a fact about what the runtime does and not a convention. Three of its cases
are there because a checker written from intuition gets them wrong and a
review caught it on a program that runs: `each v in m` yields the map's
VALUES and not its keys; a TRAIT-typed receiver dispatches on the value's
runtime type, so a method the trait never declared is reachable through it;
and a module-level name may be re-declared, with a function written between
the two bindings seeing the later one.

`unreachable.voi` fires the warning once for each of the four keywords that
flatly leave a block; `reachable.voi` beside it holds the constructs that must
stay **silent** — a `for` with no iterator that breaks out, a `fallthrough`
case, a `match` all of whose arms return. `unusedimport.voi` fires on an unused
std package and an unused user package, and stays silent on an import used only
through its **alias** and on `std/str`, whose functions are reachable bare
(`s.upper()` *is* `str.upper(s)`) and which is therefore exempt.

Four more fixtures exist because a package is **not reached only through its
qualifier**, and every one of them is a working program the warning would
have broken:

- `pkgscope.voi` — a `use` written in one file of a package and mentioned in
  another is *used*. The loader resolves qualifiers against the package's
  merged import map, so reporting it would advise a removal that stops the
  sibling compiling. One that no file of the package mentions *is* reported,
  and the message names the package it searched.
- `globalname.voi` — trait names and enum variants are global across a
  program (patterns and trait bounds have no qualified form), so `use
  "shapes"` can be load-bearing while `shapes` appears nowhere.
- `traitimpl.voi` — a package that adds methods to somebody else's type is
  reached by *dispatch*; `p.wide()` names nothing of it. Both spellings are
  covered, because they fail differently: `deco` writes `impl Wide for
  geom.Point`, and `deco2` writes a bare `impl geom.Point` with no trait at
  all. Deleting either `use` **compiles** and then aborts at run time.
- `initeffect.voi` — a package imported for the effect of a package-level
  initializer. Deleting that `use` says nothing at all; the program just
  quietly does less.

Channels 3 and 4 leave **no node in the importing file**, so they cannot be
answered by scanning it — they are answered on the exporting side, by asking
the imported package what it declares.

Each of the four carries a genuinely unused import as its control, so none can
pass by the warning having been switched off — and `initeffect.voi`'s control
is the load-bearing one: `plain` builds its globals with `make` and `set` and
has an inherent impl on its *own* type, so it must still be reported. A rule
that counted every call, or every impl, would have swallowed the warning
whole. `silenced.voi` turns both
codes off with `-Wno-`, `werror.voi` makes one fatal with `-Werror=<code>`,
and `strict.voi` runs under `--strict`.

The severities are resolved in `voilac/diag`, whose two boundary rules are the
familiar ones: the `enabled` map and the `werrorCodes` set are **lookup-only**
(map iteration order is the fixpoint hazard of §1), and ordering is by position
and nothing else, with a stable insertion sort.

`unreachable` and `unused-import` default **on**, and that was measured
rather than assumed. Swept across every entry point in `voilac/`, `tools/`,
`std/`, `samples/`, `samples/guide/`, `tests/` and `bench/` — 88 programs —
`unreachable` fired **0** times and `unused-import` fired **6**, every one of
them a genuinely unused `use` (now removed, along with three more found by
checking package files one at a time). A code that had produced false
positives would default off and live behind `-Wall`.

`type-mismatch` defaults **off**, and that is a rollout decision rather than a
measurement: the same sweep, run over **156** programs (`voilac/main.voi` and
`tools/*.voi`, which between them `use` every `voilac` package, plus every
file under `std/`, `samples/`, `samples/guide/`, `tests/` and `bench/`), left
exactly **one** report — `tests/negative/trait_missing.voi`, where `Canvas`
genuinely has no `describe` and the trait check already refuses the program.
It is off all the same because it is the youngest and largest check in the
compiler, it reasons about a partial *inference* rather than about a
declaration, and the failure mode of getting it wrong is refusing a working
program. Ask for it with `-Wtype-mismatch`, `-Wall` or `--strict`.

Sweeping a single file of a MULTI-FILE package is not a valid measurement and
must not be counted: the loader loads that file alone, so the sibling `impl
Checker { … }` blocks are missing and every `self.method()` call in
`voilac/chk/chk.voi` is reported as a missing method. Each `std/` package is
one file and is therefore complete on its own; the `voilac/` packages are
covered whole through `voilac/main.voi` and `tools/*.voi`.

This is worth stating plainly because `type-mismatch` is the **first** check
that turns a clean single-file `check` into a wall of diagnostics.
`voila check voilac/par/par.voi` says `ok` today, and with `-Wall` it reports
nine missing `Parser` methods that live in `par/expr.voi` and `par/stmt.voi`.
The other multi-file packages already error out on the same input
(`unknown type \`Edge\``), so nobody had to notice before. There is no way for
`chk` to tell an entry point from a fragment — it is handed one flattened
arena either way — so the answer is the sweep methodology, not a rule in the
checker.

Two more stages stand beside the fixtures, because `voila run` caches its
binaries under `~/.cache/voila` and two different things have to hold:

- **End to end** — a lenient run must not license a strict one. The stage runs
  the fixture lenient first (which compiles and installs the binary) and then
  demands that `--strict` still refuses. Today the refusal comes from the
  driver reporting *before* the cache is consulted, not from the key; this is
  the property a user depends on, whichever mechanism provides it.
- **The key itself** — `diag.Policy.Key` puts the `-W` state into the cache
  key, and the check above would pass even if it did not. So a freshly
  generated probe is run three times: two `-W` states must produce two cache
  entries, and one `-W` state must produce the same entry twice. That second
  half is the determinism gate — a `Key()` that walked a map instead of
  `diag.Codes()` would quietly stop caching, and nothing else would notice.
  The probe removes the entries it created.

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

**A lexer or parser edge case** → `tests/lexedge/`, with its token golden:

```sh
bin/voila tokens tests/lexedge/new.voi > tests/lexedge/new.tokens
```

**A construct the backend must refuse** (as opposed to one the checker must
reject) → `tests/negative-native/`, with a `.expect` holding one phrase per
line. Globbed too; still no list to edit.

**A warning** → `tests/warn/`, with the four files above — and a second fixture
holding the code that must *not* trigger it. Half a warning test is the half
that lets the check be tightened until it accuses working programs.

## The differential test (historical)

While the Go compiler still existed, `difftest.bash` diffed every stage of the
Voilà compiler against it — token streams, parse trees, flattened programs, IR
listings, emitted C — over all 79 `.voi` files in the repository, including the
compiler's own source. It passed at every stage, and that is what made deleting
the Go tree safe.

It cannot run any more, because there is nothing left to diff against. Its job
now belongs to the goldens and to the fixpoint.
