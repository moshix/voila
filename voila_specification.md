# Voilà — Language Specification

**Version 0.3.2 (Draft)** — floor division is spelled `~/`; `//` is a comment only (§5.1). Version 0.2 added exceptions (§8), implicit numeric conversions (§3.2), and the core library (§13).
File extension: `.voi` · Module file: `voila.mod` · Toolchain: `voila`

---

## 0. Design Goals

Voilà (written `voila` in ASCII contexts) is a statically typed, memory-safe, concurrent language that is **both interpreted and compiled from the same source, with identical semantics**.

| Goal | Borrowed from | How Voilà does it |
|---|---|---|
| Trivial concurrency | Go | Lightweight tasks, channels, `select`, but **structured** — no orphaned tasks |
| Familiar syntax | C / Go | Braces, `func`, types after names, no semicolons needed |
| No leaks, no GC pauses | Rust | Ownership + moves + deterministic destruction, **but no lifetime annotations** |
| Learnable in an afternoon | REXX | `say`, `parse`, decimal arithmetic, everything printable, REPL, shebang scripts |
| Painless packages | Go | `use "host/path/pkg"`, `voila.mod`, `voila get`, minimum version selection |

**The core bet:** Rust's safety comes mostly from *one owner + no cycles*, not from lifetime syntax. Voilà keeps the guarantee and throws away the notation by (a) putting ownership annotations on **declarations, never call sites**, and (b) making the type system reject reference cycles outright.

---

## 1. Hello, World

```voila
// hello.voi
package main

func main() {
    say "Hello, world!"
}
```

```
$ voila run hello.voi          # interpreted, ~5 ms startup
Hello, world!

$ voila build hello.voi -o hello   # AOT native, static binary
$ ./hello
Hello, world!
```

Script form (REXX-style, no ceremony — an implicit `package main` and `func main`):

```voila
#!/usr/bin/env voila
say "Hello, world!"
say "2 + 2 =", 2 + 2
```

The three-liner and the packaged form compile to the same thing. `say` is a builtin: it stringifies every argument, joins with a space, appends a newline.

---

## 2. Lexical Structure

### 2.1 Source encoding
UTF-8. Identifiers may contain any Unicode letter. Source is case-sensitive.

### 2.2 Comments
```voila
// line comment
/* block comment, /* nests */ unlike C */
/// doc comment (attaches to next declaration, consumed by `voila doc`)
```

### 2.3 Identifiers
```
ident = letter { letter | digit | "_" } ;
```
`_` alone is the **blank identifier** (discards a value).

Visibility follows Go: an identifier **exported** from a package starts with an uppercase letter. Lowercase = package-private.

### 2.4 Statement termination
Newlines end statements. A semicolon is *inserted* at end of line unless the line ends with an operator, comma, `(`, `[`, `{`, or a keyword expecting continuation. Explicit `;` is legal and allows multiple statements per line.

### 2.5 Keywords

```
and      as       attempt  break    case     catch    chan     const
continue defer    drop     each     else     enum     exception false
finally  for      func     group    if       impl     import   in
let      match    must     mut      nil      not      nothrow  numeric
or       own      package  parse    return   say      select   self
shared   spawn    struct   switch   throw    throws   trait    true
try      type     use      var      weak     while    yield
```

Reserved for future use: `async`, `await`, `macro`, `unsafe`, `where`.

### 2.6 Literals

```voila
42            // int
0x1F  0o755  0b1010
1_000_000     // underscores allowed anywhere between digits
3.14          // float (f64)
6.02e23
19.99d        // dec — arbitrary-precision decimal (REXX-style)
'A'           // rune (u32 codepoint)
"text"        // str, UTF-8, immutable, \n \t \\ \" \u{1F600} escapes
`raw
 string`      // raw str, no escapes, newlines preserved
"cost: {x}"   // interpolation: any expression inside { }
true false nil
```

---

## 3. Types

### 3.1 Primitive

| Type | Meaning |
|---|---|
| `int` | signed 64-bit (the default integer) |
| `i8 i16 i32 i64` / `u8 u16 u32 u64` | sized integers |
| `byte` | alias for `u8` |
| `float` | IEEE-754 binary64 (default float) |
| `f32` | binary32 |
| `dec` | **arbitrary-precision decimal**, exact, governed by `numeric digits` |
| `bool` | `true` / `false` — no implicit truthiness |
| `rune` | Unicode codepoint |
| `str` | immutable UTF-8 string |
| `unit` | the empty type, written `()`; return type of a function with no result |

**REXX homage — decimal arithmetic:**

```voila
numeric digits 34          // file- or block-scoped pragma; default 28
let price = 19.99d
let qty   = 3d
say price * qty            // 59.97   exactly. Never 59.96999999999999
```

### 3.2 Numeric Conversions

Voilà converts **implicitly when the conversion cannot lose information**, and demands an explicit call when it can. The rule is one sentence:

> A value of type `A` converts implicitly to `B` if and only if every value of `A` is exactly representable in `B`.

**The widening lattice** (implicit, free, `→` is transitive):

```
i8 → i16 → i32 → i64 (int) ─┐
u8 → u16 → u32 → u64 ───────┼→ dec
i8..i32, u8..u32 ───────────┴→ float → dec?  (no: float → dec is EXPLICIT, see below)
f32 → float
u8 → i16,  u16 → i32,  u32 → i64      (unsigned into a wider signed type)
i8..i64, u8..u64 → dec                (integers are exact decimals)
i8..i32, u8..u32 → float              (fits in 53-bit mantissa; i64 → float does NOT)
f32 → float
```

So this all just works:

```voila
var total int   = 0
let count u16   = 300
total += count              // u16 → int, implicit

func scale(f float) float
scale(3)                    // untyped literal → float
scale(count)                // u16 → float, implicit

let price dec = 19.99d
let qty   int = 3
say price * qty             // int → dec, implicit. Result: 59.97d exactly
```

And these do not, because they can lose information:

```voila
let big i64 = 9_007_199_254_740_993
let f float = big           // ERROR: i64 → float may lose precision. Use float(big).
let n int   = 3.7           // ERROR: float → int truncates. Use int(3.7) or round(3.7).
let d dec   = 0.1           // ERROR: binary float → dec is lossy. Use dec("0.1") or 0.1d.
let b u8    = 300           // ERROR: literal 300 does not fit in u8.
```

**Explicit conversion — three flavours**, so you always say what you want to happen on overflow:

| Form | On overflow / loss | Example |
|---|---|---|
| `int(x)` `u8(x)` `float(x)` `dec(x)` | **throws** `ConvError` | `int(3.9) == 3` (truncates toward zero, documented, not a loss) |
| `to_int(x)` `to_u8(x)` … | returns `?T` (`nil` on failure) | `to_u8(300) == nil` |
| `wrap_u8(x)` `sat_u8(x)` | wraps / saturates silently | `wrap_u8(300) == 44`, `sat_u8(300) == 255` |

**Strings are numbers when you ask them to be** (the REXX comfort, without REXX's weak typing):

```voila
int("42")        // 42        — throws ConvError on "banana"
to_int("banana") // nil
float("3.14")    // 3.14
dec("19.99")     // exact
int("ff", 16)    // 255       — radix
str(42)          // "42"      — every type has str(); `say` calls it for you
```

**Overflow in arithmetic** traps (throws `OverflowError`) in debug builds and in the interpreter; release builds trap too by default and can opt into wrapping per-expression with `wrap_add(a, b)`, `wrap_mul(a, b)`. Silent wraparound is never the default.

Rounding helpers: `round(x)`, `floor(x)`, `ceil(x)`, `trunc(x)`, `round_to(x, places)`.

### 3.3 Composite

```voila
[5]int              // array, fixed length, value type
[]int               // slice: view or owned growable vector
map[str]int         // hash map
set[str]            // hash set
chan[Job]           // channel
?int                // optional: an int or nil
int!                // fallible: an int or an error   (see §8)
fn(int, int) int    // function type
(int, str)          // tuple
```

### 3.4 Structs

```voila
struct Point {
    x float
    y float
}

struct Server {
    host   str
    port   int = 8080          // field default
    routes map[str]Handler
}

let p = Point{x: 1.0, y: 2.0}
let q = Point{1.0, 2.0}        // positional also allowed
let s = Server{host: "localhost"}   // port defaults to 8080
```

Structs are value types (copied on assignment) **if all fields are `Copy`** (primitives, and structs of primitives). Otherwise they are **moved** (§6).

### 3.5 Enums (sum types) and `match`

```voila
enum Shape {
    Circle(radius float)
    Rect(w float, h float)
    Empty
}

func area(s Shape) float {
    match s {
        Circle(r)  => return 3.14159 * r * r
        Rect(w, h) => return w * h
        Empty      => return 0.0
    }
}
```

`match` must be **exhaustive**; the compiler enumerates missing cases. `_` is the catch-all. Guards are allowed:

```voila
match n {
    x if x < 0  => say "negative"
    0           => say "zero"
    _           => say "positive"
}
```

`?T` and `T!` are ordinary enums in disguise:
`?T` = `enum { Some(T), nil }`, `T!` = `enum { Ok(T), Err(error) }`.

### 3.6 Traits (interfaces)

Traits are Go-flavoured interfaces with Rust-flavoured explicit implementation.

```voila
trait Show {
    func show(self) str
}

trait Reader {
    func read(mut self, buf mut []byte) int!
}

impl Show for Point {
    func show(self) str {
        return "({self.x}, {self.y})"
    }
}

func print_all(items []Show) {      // dynamic dispatch
    each it in items { say it.show() }
}
```

Any type implementing `Show` may be passed where `Show` is expected. `say` uses `Show` if present, else the derived structural form.

### 3.7 Generics

```voila
func max[T: Ord](a T, b T) T {
    if a > b { return a }
    return b
}

struct Stack[T] {
    items []T
}

impl[T] Stack[T] {
    func push(mut self, own v T) { self.items.append(v) }
    func pop(mut self) ?T        { return self.items.pop() }
}
```

Constraints are traits: `Ord`, `Eq`, `Hash`, `Show`, `Copy`, `Send`, or any user trait. Generics are monomorphised in compiled mode and type-erased in the interpreter — identical observable behaviour.

### 3.8 Type declarations

```voila
type UserID = int              // alias
type Celsius struct { deg float }   // named struct
type Handler fn(Request) Response
```

---

## 4. Declarations and Bindings

```voila
let x = 10             // immutable binding, type inferred (int)
let y int = 10         // explicit type
var count = 0          // mutable binding
count = count + 1
var buf []byte         // zero value: empty slice
const MAX = 1024       // compile-time constant, untyped until used
```

`let` bindings cannot be reassigned. Mutability is **not** transitive through channels or `shared` — see §6.

Zero values: `int` → 0, `float` → 0.0, `bool` → false, `str` → `""`, `?T` → `nil`, slice/map/set → empty, struct → all fields zero (or their declared defaults).

There are **no uninitialised variables** and **no null pointers**. `nil` inhabits only `?T`.

---

## 5. Expressions and Operators

### 5.1 Precedence (highest to lowest)

| Lvl | Operators | Assoc |
|---|---|---|
| 11 | `x.f` `x[i]` `x(...)` `x!` `x?` | left |
| 10 | unary `-` `not` `~` | right |
| 9 | `**` (power) | right |
| 8 | `*` `/` `~/` `%` | left |
| 7 | `+` `-` | left |
| 6 | `<<` `>>` `&` `\|` `^` | left |
| 5 | `\|\|` (string concat) | left |
| 4 | `<` `<=` `>` `>=` `==` `!=` `in` | none |
| 3 | `and` | left |
| 2 | `or` | left |
| 1 | `=` `+=` `-=` `*=` `/=` `%=` `\|\|=` | none (statements) |

Notes:
- `/` on integers is **exact division producing `float`**; `~/` is floor division producing `int`. (Removes the classic C/Go trap.) `//` is *only* a comment; floor division has its own operator `~/` so the two never collide.
- `||` concatenates strings (REXX). `+` never concatenates.
- `and` / `or` short-circuit and require `bool`. `&&`/`!` are *not* in the language — spelled `and`/`not` for readability.
- `x in coll` works on slices, maps (keys), sets, strings (substring), and ranges.
- `**` is power; `2 ** 10 == 1024`.

### 5.2 Ranges

```voila
0..10       // exclusive, 0 through 9
0..=10      // inclusive
0..10 by 2  // stepped
```

---

## 6. Memory Model — safety without lifetimes

Voilà has **no garbage collector** and **no `free`**. Every value has exactly one owner; when the owner's scope ends, the value is destroyed, deterministically, in reverse declaration order.

### 6.1 The three parameter modes

Ownership annotations appear **only on declarations**. Call sites are always plain `f(x)` — you never write `&`, `&mut`, `.clone()`, or `*`.

```voila
func peek(v []int) int          // BORROW (default): read-only, cannot outlive the call
func fill(v mut []int)          // MUTABLE BORROW: exclusive, cannot outlive the call
func consume(own v []int)       // MOVE: callee takes ownership; caller's binding is dead
```

```voila
var data = [1, 2, 3]
say peek(data)      // borrow — data still usable
fill(data)          // mutable borrow — data still usable
consume(data)       // move
say peek(data)      // COMPILE ERROR: `data` was moved on line 4
```

The compiler enforces the aliasing rule that makes this sound:

> **At any point, a value has either any number of read borrows, or exactly one `mut` borrow — never both.**

This is Rust's rule. What Voilà removes is the *syntax tax*: lifetimes are always inferred, and the only borrow-returning signature permitted is the elidable one (a returned borrow derives from the single borrowed parameter, or from `self`). Anything more entangled must use `own` or `shared` — which in practice is what beginners write anyway.

### 6.2 Escape analysis

A borrow may never be stored in a struct field, a channel, a global, or a closure that outlives the borrow. This is checked statically and is the entire reason no lifetime notation is needed: **borrows cannot escape, so they cannot dangle.**

```voila
struct Bad { p &Point }    // COMPILE ERROR: borrow in struct field
struct Ok  { p Point }     // owns it
struct Ok2 { p shared[Point] }   // shares it
```

### 6.3 Shared ownership — and why it cannot leak

```voila
shared[T]    // atomically reference-counted, DEEPLY IMMUTABLE
cell[T]      // shared + mutable, protected by an internal lock
weak[T]      // non-owning handle to a shared/cell; upgrade() -> ?shared[T]
```

Reference counting normally leaks on cycles. Voilà makes cycles **unrepresentable**:

> **The Acyclicity Rule.** The type graph, following only owning edges (fields, `shared`, `cell`, collection elements), must be a DAG. A type that transitively reaches itself through an owning edge is rejected at compile time. Self-reference is legal only through `weak[T]` or `?Box[T]` in a tree-shaped position (verified acyclic by construction, e.g. linked lists, trees).

Corollaries:
- `shared[T]` values are immutable, so a cycle cannot be *created* after construction either — you cannot mutate a node to point back at an ancestor.
- `cell[T]` is mutable, so it is subject to the Acyclicity Rule at the type level: a `cell[Node]` where `Node` contains `cell[Node]` is a compile error. Write `weak[Node]` for the back-edge.
- Therefore: **refcounts always reach zero. Voilà programs cannot leak memory.** No `Rc<RefCell<>>` cycle bugs, no leak detectors, no `Box::leak`.

```voila
struct Node {
    value  int
    kids   []shared[Node]
    parent weak[Node]        // back-edge must be weak; `shared` here = compile error
}
```

### 6.4 Copy, move, destroy

- Types implementing `Copy` (all primitives, arrays/structs of `Copy`) are copied on assignment.
- Everything else is **moved**. Using a moved-from binding is a compile error with the move site in the message.
- Want a real copy? `x.clone()` — explicit, from the `Clone` trait.
- `drop x` destroys `x` early. `defer` runs a statement at scope exit (LIFO), like Go:

```voila
func read_config(path str) str! {
    let f = try os.open(path)
    defer f.close()
    return try f.read_all()
}
```

### 6.5 No `unsafe` in the core
Raw memory lives behind the `std/ffi` boundary (§12). The safe subset — which is the whole language — has no way to produce a dangling reference, a data race, a double free, or a leak.

---

## 7. Statements

### 7.1 Conditionals
```voila
if x > 10 {
    say "big"
} else if x > 5 {
    say "medium"
} else {
    say "small"
}

if let Some(v) = maybe {    // optional binding
    say v
}
```
Braces are mandatory; parentheses around the condition are not allowed.

### 7.2 Loops

```voila
for i in 0..10        { say i }           // range
for i in 0..len(v)    { }                 // classic index loop
each x in v           { say x }           // iterate values
each i, x in v        { say i, x }        // index + value
each k, v in m        { say k, "=", v }   // map
each line in file.lines() { say line }    // any Iter
while x > 0           { x -= 1 }
for                   { ... break }       // infinite

for i in 0..10 {
    if i == 3 { continue }
    if i == 7 { break }
}

outer: for i in 0..3 {          // labels
    for j in 0..3 {
        if j == 2 { continue outer }
    }
}
```

### 7.3 `switch`

```voila
switch day {
case "sat", "sun":
    say "weekend"
case "mon":
    say "ugh"
default:
    say "weekday"
}
```
No fallthrough by default (use `fallthrough` explicitly). For sum types prefer `match`.

### 7.4 `parse` — the REXX inheritance

Template-directed destructuring of strings. This is Voilà's most distinctive statement and it makes text munging a one-liner.

```voila
let line = "moshix,55,Milan"
parse line "," name "," age "," city
// name = "moshix"   age = "55"   city = "Milan"

parse csv_row "," id "," rest .          // `.` discards the tail
parse text word1 word2 rest .            // whitespace-delimited words
parse record 1 recid 9 acct 21 amount .  // COLUMN positions (fixed-width records!)
parse upper cmd verb args .              // case-fold before parsing
parse var s . . third .                  // skip fields with `.`
```

Targets are declared implicitly as `str` unless already bound. Numeric coercion is explicit: `int(age)`. Fixed-column parsing makes flat-file and mainframe-record processing native:

```voila
each rec in file.records(80) {
    parse rec 1 code 3 name 33 balance 45 .
    if code == "AC" { say name, dec(balance) }
}
```

### 7.5 `say`
```voila
say "x is", x, "and y is", y      // space-joined, newline appended
say                               // blank line
```

---

## 8. Errors and Exceptions

Voilà offers **both** mechanisms, because they answer different questions, and it makes the boundary between them explicit rather than leaving it to taste.

| | Use for | Cost |
|---|---|---|
| **Error values** — `T!` | Expected, local, frequent failures the caller will obviously handle: a missing key, a bad port number, EOF | Zero. It is a return value. |
| **Exceptions** — `throw` / `catch` | Exceptional, non-local failures that most frames in between have nothing useful to say about: out of disk, connection reset mid-transaction, a bug in a library ten frames down | Zero on the happy path; unwinding is slow, and it should be |

Rule of thumb: **if the immediate caller can plausibly recover, return `T!`. If recovery belongs many frames up, `throw`.** The two convert into each other in one word, so this is never a decision you are locked into.

### 8.1 Error values

```voila
trait Error {
    func message(self) str
    func cause(self) ?Error = nil       // default impl: no cause
}

func parse_port(s str) int! {           // `int!` = int-or-error
    let n = try to_int(s) else return err("not a number: {s}")
    if n < 1 or n > 65535 {
        return err("port out of range: {n}")
    }
    return n
}
```

Three ways to consume a `T!`:

```voila
let p = try parse_port(s)                 // propagate upward (caller must return `!`)
let p = parse_port(s) else 8080           // supply a default
match parse_port(s) {                     // handle explicitly
    Ok(p)  => say "port", p
    Err(e) => say "bad:", e.message()
}
```

`try f()` is exactly `match f() { Ok(v) => v, Err(e) => return Err(e) }`.

### 8.2 Exceptions

An exception is a value of any type implementing `Error`. Declare one with `exception`, which is sugar for a struct plus the `Error` impl:

```voila
exception NotFound  { path str }         // message() derives to "NotFound{path: ...}"
exception Timeout   { after Duration }
exception ConvError { from str, to str, value str }

exception ParseError {
    line int
    col  int
    func message(self) str {             // ...or write your own
        return "parse error at {self.line}:{self.col}"
    }
}
```

Throw and catch:

```voila
func load(path str) Config {
    if not os.exists(path) {
        throw NotFound{path: path}
    }
    return decode(try os.read(path))     // `try` here is the ERROR-VALUE try
}

func main() {
    try {
        let cfg = load("/etc/app.conf")
        run(cfg)
    } catch e: NotFound {                // typed catch, matched in order
        say "no config at", e.path, "— using defaults"
        run(default_config())
    } catch e: Timeout, NetError {       // several types in one clause
        say "network trouble:", e.message()
    } catch e {                          // catch-all: e is `Error`
        say "fatal:", e.message()
        each frame in e.trace() { say "   ", frame }
        os.exit(1)
    } finally {
        log.flush()                      // always runs: normal exit, throw, or return
    }
}
```

**Rethrow** with a bare `throw` inside a `catch`. **Chain** with `throw ParseError{...} from e` — `e` becomes `cause()`, and `e.trace()` retains the original stack.

Note the deliberate overload of `try`: `try expr` propagates an *error value*; `try { ... } catch` handles *exceptions*. The parser distinguishes them by the following token (`{` or not). They read the same way in English and that is the point.

### 8.3 Unwinding is safe and leak-free

This is the part that matters, and it falls straight out of §6. When an exception unwinds a frame:

1. Every `defer` in the frame runs, LIFO.
2. Every owned value in the frame is destroyed, in reverse declaration order.
3. Refcounts on `shared`/`cell` handles are decremented.

Because ownership is a DAG (§6.3) and destruction is deterministic, **an exception cannot leak memory, leave a file handle open, or leave a lock held.** This is the classic C++ exception-safety problem, and it is solved not by discipline but by the ownership model. Locks are RAII; a `throw` inside a `cell.update` closure releases the lock and propagates.

### 8.4 Which functions can throw?

Exceptions are **unchecked by default** — no `throws` clause needed, no propagation boilerplate. This is the REXX-ease concession, and it is where Voilà consciously trades a little rigour for a lot of approachability.

Two opt-ins for when you want the rigour back:

```voila
func f() int throws NotFound, Timeout    // CHECKED: may throw only these; the
                                         // compiler verifies every callee
func g() int nothrow                     // VERIFIED not to throw at all
```

`nothrow` is not decoration — the compiler proves it, and three places **require** it:
- destructors (`impl Drop`),
- `cell.update` closures (a throw under a lock is fine, but a throw that *escapes* an update would break the invariant the lock protects),
- `ffi` callbacks (unwinding across a C frame is undefined).

`voila check --strict` additionally reports every unchecked `throw` site, for teams that want the Java-style contract without the language forcing it on everyone.

### 8.5 Bridging the two mechanisms

```voila
let n = must parse_port(s)      // T! → exception: throws the Err as an exception
let r = attempt load(path)      // exception → T!: returns Config!, catching any throw
```

So a library that returns `T!` can be used in exception style, and a library that throws can be used in value style, in one word, at the call site, by whoever is calling. Neither library author gets to impose their taste on you.

### 8.6 Aborts — the third category

Some conditions are neither errors nor exceptions and are **not catchable**: `assert` failure, array index out of bounds, an unhandled exception reaching the top of a task, or a `throw` escaping a `nothrow` function. These abort the process with a full stack trace. They indicate a bug in the program, and the correct response to a bug is to stop, not to limp on.

`os.on_abort(fn)` may register a last-gasp handler (flush logs, notify a supervisor) — it runs, then the process still dies.

### 8.7 Exceptions and concurrency

An exception escaping a `spawn`ed task does **not** abort the process. It:

1. cancels the task's sibling group members (via `ctx`),
2. is rethrown at the enclosing `group` boundary, in the parent task,
3. carries both stacks — the spawn site and the throw site.

```voila
try {
    group {
        spawn fetch("https://a.example")
        spawn fetch("https://b.example")   // throws Timeout
    }                                      // <-- rethrows Timeout here, after
} catch e: Timeout {                       //     cancelling and joining the sibling
    say "one of the fetches timed out; all tasks are already reaped"
}
```

If several siblings throw, the first is delivered and the rest are attached as `e.suppressed()`.

---

---

## 9. Concurrency

### 9.1 Tasks are structured

Go's `go` statement can leak goroutines. Voilà's `spawn` cannot: every task belongs to a `group`, and a `group` block does not exit until all of its tasks have finished.

```voila
func main() {
    group {
        spawn worker(1)
        spawn worker(2)
        spawn worker(3)
    }                      // <-- blocks here until all three return
    say "all workers done"
}
```

If any task returns an error, the group cancels its siblings (cooperatively, via their `ctx`) and the `group` block yields that error:

```voila
func fetch_all(urls []str) []str! {
    var results = make([]str, len(urls))
    try group {
        each i, u in urls {
            spawn {
                results[i] = try http.get(u)   // disjoint indices — no race
            }
        }
    }
    return results
}
```

Tasks are green threads, multiplexed over an M:N scheduler with work stealing. They cost ~4 KB. Launching a million is fine.

To get a value back from one task: `spawn` yields a `Task[T]`; `.await()` joins it.

```voila
group {
    let a = spawn compute_a()
    let b = spawn compute_b()
    say a.await() + b.await()
}
```

### 9.2 Channels

```voila
let ch = chan[int](0)       // unbuffered (synchronous rendezvous)
let ch = chan[Job](100)     // buffered

ch <- job                   // send  (blocks if full)
let job = <-ch              // receive (blocks if empty)
let job, ok = <-ch          // ok == false if closed and drained
close(ch)
each job in ch { ... }       // ranges until closed
```

**Sending moves.** `ch <- v` transfers ownership of `v` into the channel; `v` is dead in the sender. This is what makes channels data-race-free with zero copying — the Go "don't share memory, communicate" slogan is enforced by the compiler rather than by convention. Only `Send` types may cross a channel (`shared[T]` yes, borrows never).

### 9.3 `select`

```voila
select {
case job := <-jobs:
    process(job)
case results <- value:
    say "sent"
case <-timer.after(5 * time.Second):
    say "timeout"
default:
    say "nothing ready"      // makes the select non-blocking
}
```

### 9.4 Shared mutable state

```voila
let counter = cell[int](0)
group {
    for i in 0..1000 {
        spawn { counter.update(fn(n) { return n + 1 }) }
    }
}
say counter.get()      // 1000, guaranteed
```

`cell` is a lock-protected box; the closure runs under the lock and cannot itself block on a channel (checked: closures passed to `update` must be `Nonblocking`, so lock-order deadlocks of the classic kind are ruled out). Lower-level `sync.Mutex`, `sync.RWLock`, `sync.WaitGroup`, and atomics exist in `std/sync` for when you need them.

**Data-race freedom is a static guarantee**, inherited directly from §6: two tasks cannot hold a `mut` borrow of the same value, because `mut` borrows cannot escape into a task in the first place.

### 9.5 Cancellation

Every task receives an implicit `ctx`. `ctx.done()` is a channel; `ctx.cancelled()` is a bool. `group.cancel()` cancels all children. Timeouts: `group timeout 5*time.Second { ... }`.

---

## 10. Functions and Closures

```voila
func add(a int, b int) int { return a + b }
func add(a, b int) int     { return a + b }      // shared type
func divmod(a, b int) (int, int) { return a // b, a % b }
let q, r = divmod(17, 5)

func sum(nums ...int) int {          // variadic
    var t = 0
    each n in nums { t += n }
    return t
}
sum(1, 2, 3)
sum(v...)                            // spread a slice

func greet(name str, greeting str = "Hello") str {   // default args
    return "{greeting}, {name}!"
}
greet("moshix")
greet("moshix", greeting: "Ciao")    // named args

let double = fn(x int) int { return x * 2 }        // closure
let double = fn(x) { return x * 2 }                // inferred in expression position
nums.map(fn(x) { x * 2 })                          // last expression is the value
```

Closures capture by borrow if they do not outlive the borrow, otherwise ownership must be moved in explicitly: `fn own [data] () { ... }`. The compiler tells you which one you need.

Methods:
```voila
impl Point {
    func dist(self, other Point) float { ... }   // borrows self
    func shift(mut self, dx float)     { ... }   // mutates self
    func into_pair(own self) (float, float) { ... }  // consumes self
    func origin() Point { return Point{0, 0} }   // no self = static/associated
}
```

---

## 11. Packages and Modules

Go's model, essentially unchanged, because it works.

### 11.1 `voila.mod`
```
module github.com/moshix/voila-bbs
voila 0.1

require (
    github.com/moshix/go3270  v1.4.2
    codenotary.com/immudb-voi v0.9.0
    std                        0.1
)
```
`voila.sum` pins content hashes. Resolution is **minimum version selection** — reproducible, no lockfile drift, no solver surprises.

### 11.2 `use`
```voila
use "std/io"
use "std/http"
use "github.com/moshix/go3270" as t3270      // alias
use "std/str" { upper, words }               // selective import
use "std/math" *                             // wildcard (discouraged; allowed in scripts)
```
Import path == repository path == directory. One directory = one package. Unused imports are a **warning**, not an error (a deliberate softening of Go, for REPL/script comfort).

### 11.3 Commands
```
voila run main.voi        # interpret (bytecode VM)
voila run .               # interpret the current module
voila build -o app .      # AOT native, static by default
voila build --target=linux/arm64 .
voila test ./...          # tests are funcs named Test* in *_test.voi
voila bench ./...
voila fmt ./...           # canonical formatter, no options (gofmt doctrine)
voila get github.com/x/y@v1.2.0
voila doc ./pkg
voila repl                # interactive, with full type checking
voila check .             # type + borrow + acyclicity check, no codegen
```

---

## 12. Interpreted ⇄ Compiled

Both modes share the **same front end**: lexer → parser → type checker → borrow checker → acyclicity checker → IR.

- `voila run` lowers IR to a register bytecode and executes it on the VM (with a tracing JIT above a hotness threshold). Startup ≈ 5 ms.
- `voila build` lowers the same IR through an SSA optimiser to native code (LLVM or a native backend), statically linked, no runtime dependency beyond the scheduler and allocator.

**The Equivalence Guarantee:** any program accepted by one mode is accepted by the other and produces identical observable behaviour, including destruction order and error values. There is *no* dynamic-only escape hatch — no `interpret` instruction that could compile-fail. Runtime code evaluation is available explicitly and in both modes:

```voila
use "std/vm"
let f = try vm.compile("fn(x int) int { return x * x }")
say f(7)      // 49 — works in the compiled binary too; the VM is embeddable
```

FFI:
```voila
use "std/ffi"

ffi "C" {
    func printf(fmt *char, args ...) int
}
```
Everything inside an `ffi` block is outside the safety guarantees and is the one place where the compiler cannot promise you anything. It must be marked, and `voila check --strict` rejects it.

---

## 13. The Core Library

Everything below is in the prelude — importable, but the most common names (`say`, `printf`, `len`, `str`, `int`, `err`, `assert`) are visible without any `use` at all.

### 13.1 `std/fmt` — formatted output

C's `printf` conventions, kept because two generations of programmers already know them, plus a `%v` escape hatch so you never have to remember which one an `int` wants.

```voila
use "std/fmt"

fmt.printf("%-12s %6.2f %s\n", name, balance, currency)
fmt.printf("account %d: %s\n", acct, holder)

let line = fmt.sprintf("%08.3f", x)        // to a string
fmt.fprintf(os.stderr, "warning: %s\n", msg)
fmt.eprintf("warning: %s\n", msg)          // shorthand for the above
```

**Verbs**

| Verb | Applies to | Example |
|---|---|---|
| `%d` | any integer (incl. `dec` with no fraction) | `%5d` → `   42` |
| `%i` | alias for `%d` | |
| `%u` | unsigned | |
| `%f` | `float`, `f32`, `dec` | `%.2f` → `19.99` |
| `%e` `%g` | scientific / shortest | `%e` → `6.02e+23` |
| `%s` | `str`, or any type implementing `Show` | |
| `%q` | quoted, escaped string | `%q` → `"he said \"hi\""` |
| `%x` `%X` | hex (ints, `[]byte`, `str`) | `%02x` → `0f` |
| `%o` `%b` | octal, binary | |
| `%c` | `rune` | |
| `%t` | `bool` | |
| `%v` | **anything** — the type's natural form | `%v` on a struct → `Point{x: 1, y: 2}` |
| `%+v` | anything, with field names and nesting | |
| `%%` | a literal `%` | |

**Flags:** `%[flags][width][.precision]verb`, where flags are `-` (left-align), `+` (always sign), `0` (zero-pad), `' '` (space for sign), `#` (alternate form: `0x`, `0b`). Width and precision may be `*`, taking the value from the argument list.

**Format strings are checked at compile time.** A literal format string is verified against the argument types; `%d` with a `str` argument is a compile error, not a runtime surprise. A computed format string falls back to a runtime `FormatError` exception.

`dec` formats exactly — `%.2f` on `19.995d` rounds half-even to `20.00`, deterministically, on every platform.

Also in `std/fmt`: `fmt.print(...)` (no newline), `fmt.println(...)` (`say` is the alias), `fmt.pad(s, n)`, `fmt.pad_left(s, n)`, `fmt.center(s, n)`, `fmt.comma(n)` → `1,234,567`, `fmt.bytes(n)` → `1.4 MiB`, `fmt.table(rows)` → aligned columns.

### 13.2 `std/str` — string manipulation

Strings are immutable UTF-8. Every function below is Unicode-correct and returns a **new** string; indices are byte offsets unless the name says `rune`. Where REXX has an idiom worth keeping, Voilà keeps it.

Methods are callable both ways — `str.upper(s)` and `s.upper()` are the same call.

**Case and whitespace**
| | |
|---|---|
| `upper(s)` / `lower(s)` | case fold |
| `title(s)` | Title Case Each Word |
| `trim(s)` / `trim_left(s)` / `trim_right(s)` | strip whitespace |
| `strip(s, cutset)` | strip any of the given chars |
| `pad(s, n)` / `pad_left(s, n)` / `center(s, n)` | fixed-width fields (REXX `LEFT`/`RIGHT`/`CENTER`) |

**Search and test**
| | |
|---|---|
| `contains(s, sub)` | (also `sub in s`) |
| `index(s, sub)` / `last_index(s, sub)` | byte offset, or `-1` |
| `starts_with(s, p)` / `ends_with(s, p)` | |
| `count(s, sub)` | non-overlapping occurrences |
| `equal_fold(a, b)` | case-insensitive equality |

**Slice and split**
| | |
|---|---|
| `substr(s, start, len)` | REXX-style, forgiving: clamps rather than throwing |
| `s[a..b]` | slice by byte range, throws on out-of-range or non-boundary |
| `left(s, n)` / `right(s, n)` | first / last n chars |
| `split(s, sep)` → `[]str` | |
| `split_n(s, sep, n)` | at most n pieces |
| `lines(s)` → `[]str` | |
| `fields(s)` → `[]str` | split on runs of whitespace |
| `word(s, n)` / `words(s)` | REXX: the nth blank-delimited word / the count |
| `delword(s, n, len)` / `subword(s, n, len)` | REXX word surgery |

**Build and transform**
| | |
|---|---|
| `join(parts, sep)` | |
| `replace(s, old, new)` / `replace_n(s, old, new, n)` | |
| `repeat(s, n)` | (REXX `COPIES`) |
| `reverse(s)` | rune-aware |
| `translate(s, from, to)` | REXX `TRANSLATE` character mapping |
| `overlay(s, new, at)` | REXX `OVERLAY` — splice in place |
| `insert(s, new, at)` / `delstr(s, at, len)` | |

**Encoding and conversion**
| | |
|---|---|
| `bytes(s)` → `[]byte` / `from_bytes(b)` → `str!` | |
| `runes(s)` → `[]rune` / `from_runes(r)` | |
| `len(s)` | **bytes** |
| `rune_len(s)` | codepoints |
| `is_digit(s)` / `is_alpha(s)` / `is_space(s)` / `is_upper(s)` | REXX `DATATYPE` family |
| `hex(b)` / `unhex(s)` / `b64(b)` / `unb64(s)` | |

**Building strings efficiently** — `str` is immutable, so concatenation in a loop is `O(n²)`. Use a builder:

```voila
var sb = str.Builder{}
each w in words { sb.write(w); sb.write(" ") }
let out = sb.done()          // consumes the builder; zero copies
```

The compiler warns on `s ||= x` inside a loop and points you here.

### 13.3 The thirty you will actually use

Ranked by how often they appear in real programs, prelude-visible unless a package is shown:

| # | Function | Does |
|---|---|---|
| 1 | `say(...)` | print, space-joined, newline |
| 2 | `fmt.printf(fmt, ...)` | formatted print |
| 3 | `fmt.sprintf(fmt, ...)` | format to string |
| 4 | `len(x)` | length of str (bytes), slice, map, set, chan |
| 5 | `str(x)` | anything → string |
| 6 | `int(x)` / `float(x)` / `dec(x)` | conversion, throws on loss (§3.2) |
| 7 | `to_int(x)` | safe conversion → `?int` |
| 8 | `str.split(s, sep)` | string → `[]str` |
| 9 | `str.join(parts, sep)` | `[]str` → string |
| 10 | `str.trim(s)` | strip whitespace |
| 11 | `str.upper(s)` / `str.lower(s)` | case fold |
| 12 | `str.replace(s, old, new)` | substitute |
| 13 | `str.contains(s, sub)` | substring test |
| 14 | `append(v, x...)` | grow a slice |
| 15 | `make([]T, n)` / `make(map[K]V)` | allocate with capacity |
| 16 | `v.map(f)` / `v.filter(f)` / `v.reduce(f, init)` | the usual three |
| 17 | `v.sort()` / `v.sort_by(f)` | in-place, stable |
| 18 | `v.contains(x)` / `v.index_of(x)` | linear search |
| 19 | `min(a, b)` / `max(a, b)` / `abs(x)` | |
| 20 | `sum(v)` / `avg(v)` | over any numeric slice |
| 21 | `round(x)` / `floor(x)` / `ceil(x)` / `round_to(x, n)` | |
| 22 | `os.read(path)` → `str!` / `os.write(path, s)` | whole-file I/O |
| 23 | `os.open(path)` → `File!` | streaming; `.lines()`, `.records(n)`, `.close()` |
| 24 | `os.args()` / `os.env(k)` / `os.exit(n)` | |
| 25 | `json.encode(x)` / `json.decode[T](s)` | |
| 26 | `http.get(url)` / `http.post(url, body)` | |
| 27 | `time.now()` / `time.since(t)` / `time.sleep(d)` | |
| 28 | `err(msg)` / `errf(fmt, ...)` | make an error value |
| 29 | `assert(cond, msg)` | abort if false (§8.6) |
| 30 | `log.info(...)` / `log.error(...)` | structured logging to stderr |

Bonus, because they earn their place: `range(a, b)`, `zip(a, b)`, `enumerate(v)`, `keys(m)`, `values(m)`, `rand.int(n)`, `uuid.new()`.

### 13.4 Full package list

| Package | Contents |
|---|---|
| `std/fmt` | `printf` family, `Show`, padding, tables (§13.1) |
| `std/str` | string manipulation (§13.2) |
| `std/io` | readers, writers, buffering, pipes |
| `std/os` | files, processes, env, args, signals |
| `std/math` | numerics, `std/math/dec` for decimal contexts |
| `std/conv` | the conversion functions of §3.2 |
| `std/time` | instants, durations, timers, tickers |
| `std/sync` | `Mutex`, `RWLock`, `WaitGroup`, atomics, `Once` |
| `std/chan` | fan-in, fan-out, pipelines, rate limiters |
| `std/sort` `std/slices` `std/maps` | collection algorithms |
| `std/regex` | RE2 — linear time, no catastrophic backtracking |
| `std/json` `std/csv` `std/xml` `std/yaml` | encodings |
| `std/http` `std/net` `std/crypto` | networking, TLS, hashes, signatures |
| `std/log` | structured logging |
| `std/test` | assertions, table tests, fuzzing, benchmarks |
| `std/vm` | embed the interpreter: `eval`, `compile` |
| `std/ffi` | C interop |

---

## 14. Worked Examples

### 14.1 Concurrent worker pool
```voila
package main

use "std/http"
use "std/time"

struct Result { url str; status int; ms int }

func fetch(url str) Result! {
    let t0 = time.now()
    let resp = try http.get(url)
    return Result{url: url, status: resp.status, ms: time.since(t0).millis()}
}

func main() {
    let urls = ["https://moshix.tech", "https://codenotary.com", "https://example.com"]
    let jobs    = chan[str](len(urls))
    let results = chan[Result](len(urls))

    try group {
        for i in 0..8 {                     // 8 workers
            spawn {
                each u in jobs {
                    results <- try fetch(u)
                }
            }
        }
        spawn {
            each u in urls { jobs <- u }
            close(jobs)
        }
        spawn {
            for i in 0..len(urls) {
                let r = <-results
                say "{r.status}  {r.ms}ms  {r.url}"
            }
        }
    }
    say "done"          // no goroutine can still be running here. Ever.
}
```

### 14.2 Fixed-width record processing (REXX heritage)
```voila
use "std/os"

func main() {
    let f = try os.open("ACCOUNTS.DAT")
    defer f.close()

    numeric digits 31
    var total = 0d

    each rec in f.records(80) {
        parse rec 1 code 3 acct 15 name 45 amount 60 .
        if code != "AC" { continue }
        let bal = dec(trim(amount))
        total += bal
        say pad(trim(name), 30), pad_left(bal.str(), 14)
    }
    say "TOTAL:", total          // exact to 31 significant digits
}
```

### 14.3 A tree, with safe back-edges
```voila
struct Node {
    name   str
    kids   []shared[Node]
    parent weak[Node]
}

func depth(n shared[Node]) int {
    match n.parent.upgrade() {
        Some(p) => return 1 + depth(p)
        nil     => return 0
    }
}
```
Replace `weak[Node]` with `shared[Node]` and the compiler says:

```
error: type `Node` forms an ownership cycle through field `parent`
  --> tree.voi:4:5
   |
 4 |     parent shared[Node]
   |            ^^^^^^^^^^^^ owning edge back to `Node`
   = note: owning cycles can leak; Voilà forbids them
   = help: use `weak[Node]` for a back-edge
```

### 14.4 Exceptions, conversions and `printf` together

```voila
package main

use "std/fmt"
use "std/str"
use "std/os"

exception BadRecord { line int, why str }

struct Txn { acct int, holder str, amount dec }

func parse_txn(line_no int, line str) Txn {
    parse line "|" acct "|" holder "|" amount
    if str.trim(acct) == "" {
        throw BadRecord{line: line_no, why: "empty account number"}
    }
    return Txn{
        acct:   int(str.trim(acct)),                 // throws ConvError on garbage
        holder: str.title(str.trim(holder)),
        amount: dec(str.trim(amount)),
    }
}

func main() {
    numeric digits 31
    var total = 0d
    var bad   = 0

    let f = must os.open("TXN.DAT")      // `must`: turn the T! into an exception
    defer f.close()                      // runs even if we unwind out of main

    each i, line in f.lines() {
        try {
            let t = parse_txn(i + 1, line)
            total += t.amount
            fmt.printf("%6d  %-24s %12.2f\n", t.acct, t.holder, t.amount)
        } catch e: BadRecord {
            fmt.eprintf("skipping line %d: %s\n", e.line, e.why)
            bad += 1
        } catch e: ConvError {
            fmt.eprintf("skipping line %d: cannot read %q as %s\n", i+1, e.value, e.to)
            bad += 1
        }
    }

    fmt.printf("\n%-24s %12.2f\n", "TOTAL", total)
    fmt.printf("%-24s %12d\n", "REJECTED", bad)
}
```

Note what is happening: the loop body throws freely, the `catch` clauses sit exactly where recovery is possible (skip one record, keep going), `f.close()` is guaranteed by `defer` no matter which path runs, and `total` is exact decimal because it never touches a binary float.

---

## 15. Grammar (EBNF, abridged)

```ebnf
program     = package_decl { use_decl } { top_decl } ;
package_decl= "package" ident ;
use_decl    = "use" str [ "as" ident | "{" ident_list "}" | "*" ] ;

top_decl    = func_decl | struct_decl | enum_decl | trait_decl | impl_decl
            | exception_decl | type_decl | const_decl | var_decl ;
exception_decl = "exception" ident "{" { field | func_decl } "}" ;

func_decl   = [ doc ] "func" ident [ generics ] "(" [ params ] ")" [ result ]
              [ "throws" type_list | "nothrow" ] block ;
params      = param { "," param } ;
param       = [ "own" | "mut" ] ident_list type [ "=" expr ] | "..." ;
result      = type | "(" type { "," type } ")" ;
generics    = "[" ident [ ":" trait_bound ] { "," ident [ ":" trait_bound ] } "]" ;

struct_decl = "struct" ident [ generics ] "{" { field } "}" ;
field       = ident type [ "=" expr ] ;
enum_decl   = "enum" ident [ generics ] "{" { variant } "}" ;
variant     = ident [ "(" params ")" ] ;
trait_decl  = "trait" ident "{" { func_sig } "}" ;
impl_decl   = "impl" [ generics ] [ ident "for" ] type "{" { func_decl } "}" ;

type        = ident | "?" type | type "!" | "[" [ expr ] "]" type
            | "map" "[" type "]" type | "set" "[" type "]"
            | "chan" "[" type "]" | "shared" "[" type "]"
            | "cell" "[" type "]" | "weak" "[" type "]"
            | "fn" "(" [ type_list ] ")" [ type ]
            | ident "[" type_list "]" ;

block       = "{" { stmt } "}" ;
stmt        = let_stmt | var_stmt | assign | if_stmt | for_stmt | each_stmt
            | while_stmt | match_stmt | switch_stmt | select_stmt
            | group_stmt | spawn_stmt | parse_stmt | say_stmt
            | defer_stmt | drop_stmt | return_stmt | break_stmt
            | continue_stmt | numeric_stmt | throw_stmt | trycatch_stmt
            | expr_stmt | block ;

throw_stmt  = "throw" [ expr [ "from" expr ] ] ;   (* bare throw = rethrow *)
trycatch_stmt = "try" block { catch_clause } [ "finally" block ] ;
catch_clause  = "catch" [ ident [ ":" type { "," type } ] ] block ;

let_stmt    = "let" pattern [ type ] "=" expr ;
var_stmt    = "var" ident [ type ] [ "=" expr ] ;
if_stmt     = "if" ( expr | "let" pattern "=" expr ) block [ "else" ( if_stmt | block ) ] ;
for_stmt    = [ label ":" ] "for" [ ident "in" expr ] block ;
each_stmt   = [ label ":" ] "each" ident [ "," ident ] "in" expr block ;
while_stmt  = [ label ":" ] "while" expr block ;
match_stmt  = "match" expr "{" { pattern [ "if" expr ] "=>" ( expr | block ) } "}" ;
select_stmt = "select" "{" { "case" comm_clause ":" { stmt } } [ "default" ":" { stmt } ] "}" ;
group_stmt  = [ "try" ] "group" [ "timeout" expr ] block ;
spawn_stmt  = "spawn" ( call | block ) ;
parse_stmt  = "parse" [ "upper" | "lower" ] [ "var" ] expr { parse_term } ;
parse_term  = ident | "." | str | int ;      (* var | discard | literal sep | column *)
say_stmt    = "say" [ expr { "," expr } ] ;
defer_stmt  = "defer" ( call | block ) ;
drop_stmt   = "drop" ident ;
numeric_stmt= "numeric" "digits" int ;
return_stmt = "return" [ expr ] ;

expr        = binary_expr ;
unary       = [ "-" | "not" | "~" | "<-" ] primary ;
primary     = literal | ident | call | index | field | "(" expr ")"
            | composite_lit | closure
            | "try" expr | "must" expr | "attempt" expr | expr "else" expr ;
```

---

## 16. Cheat Sheet

```voila
say "hi"                        // print
let x = 1                       // immutable
var y = 2                       // mutable
func f(a int) int!              // fallible function (error VALUE)
try f(1)                        // propagate error
f(1) else 0                     // default on error
throw NotFound{path: p}         // exception
try { ... } catch e: NotFound { ... } finally { ... }
must f(1)                       // error value -> exception
attempt g()                     // exception -> error value
int(x)  float(x)  dec("19.99")  // checked conversion (throws on loss)
to_int(x)                       // safe conversion -> ?int
fmt.printf("%-10s %6.2f\n", n, v)   // formatted output
func g(v []int)                 // borrow
func g(v mut []int)             // mutable borrow
func g(own v []int)             // take ownership
shared[T] cell[T] weak[T]       // shared ownership; weak breaks cycles
group { spawn a(); spawn b() }  // structured concurrency; joins on exit
ch <- v ;  v = <-ch             // channel send / receive (moves)
match e { A(x) => ..., _ => ... }   // exhaustive
parse s "," a "," b             // REXX-style destructuring
numeric digits 34               // exact decimal arithmetic
defer f.close()                 // deterministic cleanup
use "github.com/user/pkg"       // Go-style imports
voila run f.voi | voila build   // same source, same semantics
```

---

## 17. What Voilà deliberately does *not* have

- Garbage collector · manual `free` · lifetime annotations · null pointers
- Inheritance · operator overloading · silent integer wraparound · lossy implicit conversion
- Checked exceptions *by default* (opt in with `throws`) · catchable aborts
- Macros (v0.2) · a build-configuration language · more than one way to format code

---

*Voilà 0.3.2 draft specification. Designed for review — the acyclicity rule (§6.3), the structured-concurrency guarantee (§9.1), and the two-mechanism error story (§8) are the three claims most worth attacking first.*
