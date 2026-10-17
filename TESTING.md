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
- `sized_traps.voi` — the same for the SIZED integer types: at `-O3` the
  operands sit in typed registers and the sized forms are native
  (`vl_iadd_sz` and friends), and every message must still come from the
  one cold `vl_trap_sz*` function the boxed path uses;
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
`use` aliases, `std/test`, `std/csv`, `std/crypto`, `std/regex`, `std/http`,
and the expression syntax of `31_exprsyntax.voi`: `if`/`match` as values,
typed collection literals, open-ended slices, postfix `?`, tuples, parallel
assignment, or/range/struct/tuple patterns and `else { … }`; and the second
batch: per-enum variant namespaces (`34_variants`), the follow-ups to the
first batch — a parenthesised tuple in a subscript, structural tuple keys and
`?` in a function with no result (`35_followups`) — `Box[T]` and recursive
sum types (`36_box`, which also builds and frees a million-cell list),
newtypes and aliases (`37_newtype`), the sized-integer range traps
(`38_sized`), closures over loop variables (`39_loopclosure`), and the
run-time half of the native argument kinds (`40_argkinds`, whose `.err`
golden pins an abort's stack trace)); and the third batch: generic traits,
multiple and `where` bounds, `std/iter` and the `Ord` protocol
(`41_generictraits`), `Self` and static trait methods through a type
parameter's hidden descriptor (`42_selfstatic`), Box value semantics and
the cycle a `set` can no longer form (`43_boxvalue`, run it under `leaks`),
the sized-integer stores the second batch missed (`44_sizedstores`), and
natives reached as values and over sets (`45_nativesets`); and the fourth
batch: struct value semantics (`46_valuesemantics` — copies through every
write path, `mut self` and `mut` parameters, the Box-in-a-field cycle that
leaked; run it under `leaks`), the programs the static type check must
ACCEPT (`47_typecheck`), and the third batch's review follow-ups — a
forwarded type descriptor, `Wrap[int].make()` and impls on builtin types in
method-call form (`48_followups3`); and the first PACKAGE batch:
std/slices, std/maps, std/heap, std/chan and the in-place slice methods
`insert`/`remove` (`49_collections`), typed recursive `json.decode[T]` with
path-naming errors and its inverse `encode` (`50_jsontyped`), std/flag
(`51_flag`), std/ffi against the C library — prototypes, varargs, raw
memory and its checks (`52_ffi`), process handles, signals on a channel,
`os.walk`/`os.glob` and `path.matches` (`53_process`), and `trim_prefix`,
`cut`, regex named groups, URL decoding and `Server.serve`'s graceful stop
(`54_textnet`); and containers' value semantics with
the fourth batch's review follow-ups — a slice or map in a struct, in an element, in a
literal, through a `mut` parameter; a closure into a function newtype;
`nil` inferring an optional; a sized receiver's `Self`; a long append loop
that must copy nothing (`55_containers`, run it under `leaks`); and the
follow-ups to its review — flow-sensitive nil narrowing, newtype literals
as map keys and elements, a `weak` outliving its value, float map keys, a
closure an escaped closure reads, the package batch's write paths
(`56_followups4`, run it under `leaks`); and the
package batch's review follow-ups — JSON numbers out of range, map keys of
every type round-tripping, encode's depth limit, a NUL in a `*char`,
`chan.tee` drained sequentially, `os.signal_stop`, `os.walk_all`, the
refusal of `os.kill(0)` (`57_p1fixes`); an abort in an http handler
contained to its request and `serve_for`'s deadline (`58_contain`, whose
`.err` pins the abort reports); and the ffi registry after a long
alloc/free churn, a linear decode of 200,000 strings and the double-free
abort (`59_fficheck`, `.err` pins the abort); and round five's review
follow-ups — a self-referential closure freed with its frame, narrowing of
fields, elements and map entries, `x = x + 1` on a narrowed `x`, a literal
on the left of `in`, a binding's value dying at the end of its loop
iteration (`60_followups5`, run it under `leaks`); and round six's —
a closure reaching itself through a struct, map, slice, enum or Box its
frame alone holds, freed with the frame (`61_closurecycles`, run it under
`leaks`), narrowing at a variable index or key and the writes that keep
it (`62_narrowindex`), and `?`/`try` in a finally raising instead of
returning (`63_finallyq`); `sort.sort`, `sort.sort_by` and the prelude
`append` keep a second name's value, and a `mut` argument in a captured
variable is copied in once a closure captured the frame (`65_nativemut`);
round seven's — a captured variable's value
keeps copy-on-write with tasks: a snapshot taken by a task, before the
tasks or after the join is unchanged by their stores, and every scenario
prints the same with tasks and without (`64_tasksnapshot`); and the package
review's second round — a contained abort runs `finally` blocks (a lock taken around the
body is released for the next request), a defer or finally that aborts or
throws while one is unwinding is contained too, and `$abort` is never
printed (`66_faultunwind`, `.err`); a throw through xs.map, filter,
reduce, sort_by, a `show` reached by printing or join, or a bad printf
releases the natives' temporaries (`67_unwindnatives` — run it under
`leaks`); and chan.tee as one task, a fast and a slow consumer over
200,000 values, and 10,000 values read one output after the other or
alternately (`68_tee`); and the second package batch,
the packages over vendored C libraries (from 70): std/compress's codecs —
streams made by zlib itself decoded byte for byte, round trips at every
level, every corruption an error, the decompression-bomb bound, and the
`io` streams (`70_compress`) — its zip archives, one made by Python's
zipfile among them, with the CRC check and the zip-slip refusal
(`71_zip`), and its tar, including Python's pax and GNU long names
(`72_tar`); std/sqlite — every parameter and column kind, typed rows,
transactions rolled back on an error and on a throw, SQLite's own error
messages, and tasks on several connections and on one (`73_sqlite`); and
std/tls and HTTPS on loopback with the test-only certificates in
`tests/tls` — verification that succeeds, and that FAILS for a wrong host
name and for an untrusted authority (`74_tls`, `75_https`); the
verification policy std/tls adds over BearSSL — EKU, a required
subjectAltName, no SHA-1, the wildcard rule, IP-address SANs
(`76_tlspolicy`, with the extra leaves `tests/tls/gen.sh policy` makes);
and a transaction taking its shared connection from other tasks
(`77_sqlitetx`); and that closing a TLS connection whose peer has stopped
reading is quick, and does not hold up an unrelated close meanwhile
(`78_tlsclose`, bounds of a second against a 1.5 s send timeout). Run all nine under `leaks`. A stage of its own forges a
cache claim whose builder died — a runtime archive's and an extension's —
and checks that the next build heals it. None needs the network: every
server is the program's own, on 127.0.0.1, and `tests/tls/gen.sh` (which
the suite never runs) is how the certificates — valid for a century —
were made —
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

`exhaustive_expr.voi` holds a `match` used as a VALUE to the same
exhaustiveness rule as the statement, nested patterns included (a refutable
payload such as `Some(0)` does not cover `Some`), and carries the nested
matches that are complete and must stay silent. `structpat.voi`,
`orbind.voi`, `ifnoelse.voi`, `armvalue.voi`, `elseblock.voi` and
`openrange.voi` pin the other refusals of the expression syntax, and
`closurematch`, `openincl`, `somevariant`, `optnil`, `tuplewidth`,
`tupleassign`, `tuplesep`, `dupbind` and `typeargcount` those a review
added (with `33_exprsyntax2.voi` as their legal counterpart).

The second batch adds `variantns.voi` (a bare variant name two enums of
one file declare, beside the qualified and unique spellings that must stay
legal), `recenum.voi` (a sum type that contains itself DIRECTLY, beside the
`Box`, `?Box` and `?T` spellings that must stay silent), `loopassign.voi` and
`loopassign2.voi` (a closure over a loop variable that is assigned after the
closure copied it — in the closure, and in the loop — refused at the closure
by name, beside the captures that stay legal) and `nativekinds.voi` (native
calls whose argument is statically of the wrong kind, with `!` lines for the
legal calls: a rune for a str-or-rune, a value of unknown type, a local that
shadows the native).

The third batch adds `generic_impl.voi` (a generic trait implemented or
bounded at the wrong number of arguments, two impls of one generic trait
that both write a method, and a bound at arguments the type does not
implement the trait at — beside a marker trait at two arguments and a
`where` clause that must stay silent), `where_param.voi`, `assoc_type.voi`,
and one fixture per refusal of a static method through a type parameter:
`static_nobound` (no bound declares it), `static_method` (a method's type
parameter carries no descriptor), `static_selfdefault` (`Self.m()` in a
default without `self`), `static_value` (a function taking descriptors used
as a value) and `static_infer` (nothing at the call says what `T` is). All
five are the lowerer's refusals, and the lowerer stops at the first, so each
has a file of its own.

The fourth batch is the static type check (chk/expr.voi, chk/infer.voi),
and its fixtures carry the legal neighbours of every refusal as `!` lines:
`types_holes` (an argument, a returned value — one of several too —, an
initialiser, an assignment, a struct field, the elements, keys and values of
typed collection literals, a value sent on a channel, a lossy numeric
store), `types_ops` (cross-kind comparisons, `+` on a string, arithmetic and
logic on the wrong kinds, truthiness), `types_newtype`, `types_optional`
(`nil` and a `?T` straight from a call where a `T` is wanted — a NAME
holding one stays silent), `types_branches` (`if`/`match` values against
the expected type and against each other), `types_generic` (unification at
the call, a bound judged at an INFERRED type argument, a method no bound
declares), `types_userbox`, `types_methods` (unknown methods on every
receiver whose method set is visible, and a `mut self` method through a
borrowed parameter), `types_readonly` (an in-place write through `self`
in a method without `mut self`, a borrowed parameter handed to a `mut` one,
and a `mut self` method on a tuple member), `types_var` (an unannotated
`var` keeps its type),
`narrow_paths` (a narrowed field, element or map entry written under its
root — assigned, reset by a `mut self` method or a `mut` argument, the root
reassigned, a map entry removed, a closure writing through the root),
`spawn_self` (a spawned body using `self` of a `mut self` method, and a
closure over it spawned, sent over a channel or stored in a struct; one
passed straight to `map`, and a read-only `self`, are allowed),
`spawn_closures` (a task-unsafe closure captured by a spawned body,
handed to a parameter run in a task — directly or relayed — or sent over
a channel; a closure that only stores elements, or writes under a lock, is
allowed), `spawn_races` (a spawned task assigning a variable it shares with its
group, storing into a map in it, growing, shrinking or reordering a
collection in it, calling a `mut self` method on it, handing it to a `mut`
parameter — and the element and field stores it may make), `type_value`
(a type's name where a value is wanted: `[int]`, `xs.map(str)`, a struct
or enum name),
`narrow_index` (a location at a variable index written through an index
that may be equal, its index variable assigned, bumped, passed `mut`,
changed in a loop or by a closure or shadowed; an index that is no
location),
`narrow` (an optional used where the flow has not proved it non-nil — an
`if` that neither leaves nor assigns, a narrowing undone, a loop, a
closure), `types_cheap` (an optional never narrowed in arithmetic, a map key, a
function value's arity, `len`/`each` of a number, a primitive's field, a
destructuring width, a non-integer index, a std native's stated result),
`moved_value` (a value that owns storage moves on `let`, assignment and
send — beside Copy structs, revived names, a borrowed parameter bound
twice, and a loop variable or pattern bind that reuses a moved name), `default_twice` (two traits defaulting one method), and
`typemismatch`, which was the warning fixture of the partial check.

The first package batch adds `ffi_nouse` (an `ffi` block with no `use
"std/ffi"`), `ffi_ctype` (the C side of a block: an unknown C type, `void`
as a parameter, a `link` that is not a library name, an ABI other than
"C" — beside the C spellings that must stay legal), `ffi_types` (a call to
a block's function judged by the Voilà types its prototype implies) and
`slice_methods` (`insert` and `remove` are known slice methods, and a
misspelling of them is still refused), `fieldcall` (a struct field
called like a method is refused unless it holds a function), and
`finally_return`, `finally_break` and `finally_continue` (control may not
leave a finally block — lowering one used to recurse until the compiler
crashed — beside the loop and the closure inside a finally that stay
legal). `tests/warn/ffistrict`, `ffiwall`
and `ffidefault` pin the `ffi` mark: fatal under `--strict`, printed under
`-Wall`, silent by default — and an ffi block counts as a use of std/ffi,
so none of them says `unused-import`.

`tests/ffi/*.voi` call real C libraries — the system's libz and
libsqlite3 — and each names its library in a `.lib` file. Their stage asks
`cc` whether that library LINKS on this machine and skips the fixture,
saying so, where it does not (macOS ships both; a Linux machine may lack
the development packages), so the suite stays green there. With `otool`
(macOS) or `ldd` (Linux) it also checks that such a binary links the library, and that
a program with no ffi block still links nothing but libc. The same stage
holds the promise the vendored libraries make: a program that uses
std/sqlite, std/compress and std/tls links nothing but libc either (the
libraries are compiled into it), and — with `nm` — a program that uses none
of them carries none of their code.

The second package batch adds `vendor_native_arity` (a vendored package's
native is bounded like any other, before its library is ever compiled),
`vendor_member` (a misspelt member of std/sqlite or std/tls, with the near
miss) and `vendor_nouse` (std/sqlite without its `use`).

`tests/multipkg-bad/*/` are programs the loader must reject: an import cycle, a
private name, a duplicate package qualifier, a wildcard import of a user package,
a nonexistent import, a bare variant name two imports both offer, a
package-private variant reached by its BARE name — short-name resolution has to
be a convenience on top of package privacy, never a way around it — and
(`variantns`) a variant name that two enums of ONE imported package declare,
written bare and as `pkg.Name`, where only `pkg.Enum.Name` says which.

`tests/multipkg-enums/` is that refusal's positive counterpart: two enums of
one package share a variant name, the importer writes `shapes.Shape.Unit`,
`shapes.Size.Big(7)` and the patterns of both, the package itself writes
`Shape.Unit`, and the entry file's own enums share a name too.

`tests/multipkg-traits/` carries the third batch across a package
boundary: a package's generic trait implemented by the importer under its
bare name, a trait with a static method implemented under its qualified one,
and the package's generic function that calls the static method through its
type parameter — so the importer's call supplies the hidden type descriptor,
for the package's type and for its own.

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
specification (today: the implicit `ctx` of a task, `ctx.voi`). They are
rejected by `build`, not by `check`, which is what puts them in their own
stage. The fixture this stage was written for — a closure capturing a loop
variable — is a legal program since the per-iteration environments, and it
now runs as part of `tests/conformance/39_loopclosure.voi`.

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

`selftest.bash`'s "native writers" stage greps runtime/src for natives that
hand an argument to a collection writer (`vl_slice_append(argv[k]`,
`vl_sort_slice(argv[k]` and the like) and wants exactly `sort.sort`,
`sort.sort_by` and the prelude `append` — the natives the lowerer treats as
`mut` arguments (native_mut_positions). A native that writes a collection's
storage directly through a cast of argv[k] is not seen; review such code by
hand.

`tests/stress/sharedwrites.voi` (golden `.out`) has tasks store into
different elements and nested fields of one captured variable, take
snapshots of it, and grow captured collections through closures that
reach the tasks inside a struct, a slice, a map, a global and a return
value, while other tasks index, measure, iterate and call natives (with
callbacks) on the same collection and the owner appends inside the group
body; and `mut` arguments and `mut self` receivers in captured variables
written from tasks while others read them. A write lost to a copy-on-write,
or a read torn by one, shows only now and then, so `selftest.bash` builds
it and `39_loopclosure` at -O0 through -O3 and runs each binary 64 times,
16 at once; every run must match the golden.

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
  grafted alongside (net, sync, time) — among them the natives of the runtime
  EXTENSIONS (tls, compress, sqlite), whose C side is not in `std.c` but in
  `runtime/src/ext/<package>.c`, each in its own `vl_ext_<package>[]` table.
  Those tables join the C side of the diff, and each must exist and hold
  only its own package's natives: the package is how cg decides which
  extension to link (`low.VendorOf`).
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

### 8a. The native argument kinds

`NativeKinds()` (`voilac/low/names.voi`) gives the natives whose C bodies
assume an argument's kind a `name|kinds` row — one letter per argument (`s`
str, `r` str or rune, `i` int, `n` number, `l` slice, `q` slice or set,
`m` map or set, `c` channel, `f` function, `b` bool, `a` anything), the last repeated. It is
read three times: `chk/expr.voi` refuses a call whose argument is statically
of the wrong kind, `cg` emits a `vl_argk` check before every direct call to
such a native, and `runtime/src/std.c`'s `kinds_table[]` — the same rows
again — checks a native called as a VALUE, which the generated code cannot
see. A stage compares the two copies row for row.

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
  same 277 names, minus the constants. A native added without an arity is
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

The type check these two fixtures were written for reports ERRORS since
the fourth batch (`tests/negative/types_*.voi`, and `typemismatch.voi`, which
moved there from this directory); the `type-mismatch` code is kept as a
valid spelling and carries nothing. `typesilent.voi` is still here, and its
`.expect` is still nothing but absence lines — it now proves that none of
those shapes is refused: generics at their definition
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

The partial check that `type-mismatch` named defaulted **off** — the
youngest and largest check in the compiler, refusing a working program if
wrong. The fourth batch replaced it with the static type check, whose
findings are ERRORS, and the promotion was measured the same way: swept
over every entry point (`voilac/main.voi`, `tools/`, `std/`, `samples/`,
`samples/guide/`, `tests/conformance`, `tests/opt`, `tests/stress`, `bench/`
and the multi-package fixtures). Every finding was a genuine violation of
the specification, fixed in the program rather than silenced in the
checker: two uses of a moved slice and map in the compiler itself
(`chk/traits.voi`, `opt/typing.voi`); in `std/http`, a `mut self` method
called through a borrowed parameter (the `Stream` readers) and a stream
advanced from a method without `mut self` (`Session.read_request`); and in
three network samples `conn.read_line()` on a borrowed `net.Conn`, the last
of which the guide prints. Two of its first rules were wrong and the
sweep caught them before anything else did: a rune compares equal to the
one-character string it denotes (`11_regressions` pins it), and an integer
stored into a sized type is range-checked at RUN time (§3.2), not refused.

Sweeping a single file of a MULTI-FILE package is not a valid measurement and
must not be counted: the loader loads that file alone, so the sibling `impl
Checker { … }` blocks are missing and every `self.method()` call in
`voilac/chk/chk.voi` is reported as a missing method. Each `std/` package is
one file and is therefore complete on its own; the `voilac/` packages are
covered whole through `voilac/main.voi` and `tools/*.voi`.

This is worth stating plainly because the type check turns a single-file
`check` of a package fragment into a wall of errors: `voila check
voilac/par/par.voi` reports `Parser` methods missing that live in
`par/expr.voi` and `par/stmt.voi`. A fragment is not a program, and there is
no way for `chk` to tell an entry point from one — it is handed one flattened
arena either way — so the answer is to check the entry point, not a rule in
the checker.

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
  The probe runs with a PRIVATE `HOME` under the suite's temporary directory,
  warmed first: `~/.cache/voila` is shared with every other `voila` the
  machine runs, and a build in another terminal adding entries between the
  snapshots used to flake this check.

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
