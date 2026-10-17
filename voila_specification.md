# Voilà — Language Specification

**Version 0.6.0 (Draft)** — 0.5.0 grows the Voilà-authored standard library (`std/csv`, `std/http`, `std/path`, `std/sync`, `std/test`, `std/time`) on new runtime support for I/O, regular expressions, hashing and synchronization. 0.4.2 made a prebuilt Voilà binary (>= 0.4.1) the primary way to build the compiler (BOOTSTRAP.md); the C seed is a portability fallback. 0.4.1 added `std/net` (TCP/UDP/Unix sockets; manual §11.5), the first Voilà-authored std package. 0.4 added the optimizing backend (`voila build -O1|-O2|-O3`; manual §12.5). Floor division is spelled `~/`; `//` is a comment only (§5.1). Version 0.2 added exceptions (§8), implicit numeric conversions (§3.2), and the core library (§13).
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

Reserved for future use: `async`, `await`, `macro`, `unsafe`. `where` opens a bound clause (§3.7) and is **contextual**: it is read as a keyword only as `where NAME :` after a declaration's signature, and is an ordinary identifier everywhere else.

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

That holds for the **sized** types as well as for `int`. **Integer narrowing is the one lossy conversion that is implicit**: a value of any integer type may be stored where another integer type is declared (`let b u8 = n` with `n int`, an argument, a field, a returned value), and the compiler accepts it — the GUARANTEE is the run-time range check, which traps with `OverflowError` when the value does not fit, and never wraps. (Every other lossy conversion — float → int, dec → float, float → dec, i64 → float — is a compile-time error.) The range of `i8`…`u64` is enforced on:

- arithmetic (`+ - * ** ~/`, unary `-`) whose static type is sized — an untyped literal takes the sized type beside it when it fits (`x + 1` with `x i8` is an `i8` sum, `x + 1000` is not), and two sized operands widen to the narrowest type both fit in (`u8 + i8` is `i16`);
- a compound assignment to a sized location, `x += n`, whatever `n` is;
- a store into a sized location of a value not known to fit — an initialiser, an assignment, an argument (checked on entry to the callee; not the elements of a variadic one), a returned value (a typed closure's too), a struct field (an element's field included, `xs[0].a += n`), an indexed slice element (`bs[i] = v`), an appended element (`bs.append(v)`), a map value (`m[k] = v`) and a value sent on a sized channel (`ch <- v`);
- arithmetic on a value read from a sized location the compiler can see — a tuple member (`t.0` of a `(u8, int)`), the variable of an `each` over a sized slice;
- an integer literal stored into one, at compile time (`let b u8 = 300` is an error).

Each is an `OverflowError` whose message names the operation and the type — `integer overflow: 127 + 1 is out of range for i8` — and is the same at every optimisation level. Where the compiler cannot see that a value is sized (a value of unknown type), the arithmetic is `int` arithmetic and traps at the `int` bounds; the narrower check moves to the store. `u64` holds 0 … 2⁶³−1: every integer is an `int` at run time. Shifts and bitwise operators are not range-checked.

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
Box[Node]           // owned, unique heap indirection (§6.3)
```

Collections have typed literals; the type is written once, in front of the braces:

```voila
let xs   = []int{1, 2, 3}             // a slice (a bare [1, 2, 3] infers its type)
let none = []str{}                    // an empty slice of a stated type
let ages = map[str]int{"ann": 31, "bob": 42}
let seen = set[int]{3, 1, 3}          // duplicates collapse: 3 and 1
```

Separators are commas, and a newline is as good as one. In a statement header (`if`, `for`, …) a `set[T]{…}` literal must be parenthesised, as a struct literal must. In a map literal a repeated key keeps its last value. There is no untyped `{k: v}` form: a brace with nothing in front of it opens a block, so a map literal always names its type.

A tuple's members are selected by position, `t.0`, `t.1` (`t.0.1` selects inside a nested tuple), and a tuple is taken apart by a destructuring binding (§4). A position is plain decimal digits. A member cannot be assigned — a tuple is rebuilt, not updated — and where the tuple's width is known (a literal, an annotation, a multi-result call) a position past its end is a compile-time error, as is `.N` on a value that is certainly not a tuple.

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

Structs are value types (copied on assignment) **if all fields are `Copy`** (primitives, and structs of primitives). Otherwise they are **moved** (§6). Either way no struct is ever seen through two names: after `var b = a`, `b.x = 2` leaves `a.x` as it was — and so does a write through a field or an element (`b.inner.x = 2`, `xs[0].x = 2`), a `mut self` method called on `b`, and a `mut` parameter handed `b`. (The implementation shares the struct and copies it on the first write, §6.4.)

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

Patterns nest, and besides literals, `_`, `nil`, bindings and variants there are four compound forms:

```voila
match n {
    0 | 1 | 2      => say "tiny"          // or-pattern: any alternative
    3..10          => say "one digit"     // range: 3 through 9
    10..=99        => say "two digits"    // inclusive range
    _              => say "more"
}
match c { 'a'..='z' | 'A'..='Z' => say "letter"; _ => say "other" }
match p {
    P{x: 0, y: 0}  => say "origin"        // struct pattern
    P{x: 0, y}     => say "on the y axis at", y   // `y` alone binds field y
    P{x: 1, ..}    => say "x is 1"        // `..`: the other fields are not examined
    _              => say "elsewhere"
}
match t { (0, y) => say y; (x, _) => say x }      // tuple pattern
```

A range pattern's bounds are two literals of one ordered kind (ints, floats, decimals, runes or strings); a value of another kind does not match it — `98` is not in `'a'..='z'`, and `0.5` is not in `0..1`. A variant may also be written qualified by its enum, `Shape.Circle(r)` — in a pattern, and equally as a value (`Shape.Empty`), a constructor (`Shape.Circle(1.0)`) and a first-class function (`xs.map(Shape.Circle)`).

**Each enum is a namespace of its own.** Two enums may declare the same variant name — `enum Light { Red, Green }` beside `enum Flag { Red, White }` — and the qualified spelling (`Light.Red`, from another package `geom.Light.Red`) always says which is meant. A variant name declared by exactly one enum of its package may also be written bare, as before; one declared by several may not, and the bare spelling is an error that lists the qualified ones. Variant names still may not collide with the package's other declarations (a `func Red` beside `Light.Red`). Every alternative of an or-pattern must bind the same names. A struct pattern names a declared struct type (not an exception type) and only fields it has. Exhaustiveness is judged on the whole pattern, nested parts included: `Some(0)` does not cover `Some`, and `(true, _) | (false, _)` covers a `(bool, T)`. Ranges and literals other than `true`/`false` never complete a match on their own — a `_` (or a binding) is needed after them.

Arms are separated by newlines or `;`, and a one-statement arm may also end in a comma: `match x { A => 1, B => 2 }`.

`match` is also an **expression** (§5.3): `let s = match x { A => "a"; B(n) => "b{n}" }`.

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

A trait method may carry a **default implementation**, which an implementor inherits by not writing the method. Two spellings; `= expr` is shorthand for `{ return expr }`:

```voila
trait Titled {
    func title(self) str                    // abstract: every impl owes it
    func subtitle(self) str = "(none)"      // a default, short form
    func banner(self) str {                 // a default, block form
        return "[" || self.title() || " / " || self.subtitle() || "]"
    }
}

impl Titled for Page {
    func title(self) str { return self.name }   // subtitle, banner inherited
}
```

A default is compiled ONCE and shared by every implementor — dispatch is by the receiver's run-time type, so a default calling `self.title()` reaches the implementor's own `title`. An implementor that writes the method overrides the default; a method written in an inherent `impl T { … }` block counts as written. Where two traits implemented by one type both default a method of one name and the type writes it in neither, the program is refused: the method table holds one entry per (type, method), so one default would silently answer for both traits. Write the method for the type itself.

**Generic traits.** A trait may take type parameters, which are in scope in every method of it; an `impl` names the arguments, as many as the trait declares:

```voila
trait Into[T] {
    func into(self) T
    func twice(self) (T, T) = (self.into(), self.into())   // a default
}

impl Into[str] for Cel {
    func into(self) str { return "{int(self)}C" }
}

func render[T: Into[str]](x T) str { return "[" || x.into() || "]" }
```

A trait at arguments is a type like a plain trait: `var xs []Into[str]` holds any implementor. Methods dispatch by the receiver's run-time type and the method's NAME, not by trait argument, so one type may implement one generic trait at several arguments only when those impls write different methods — two impls that both write `into` are refused, since the second could never be reached. (A marker trait with no methods, or one whose methods all have defaults, may be implemented at any number of arguments.) There are no associated types: where another language would write `type Item` in a trait, Voilà makes the trait generic — `trait Iterator[T] { func next(mut self) ?T }`.

**`Self`.** Inside a trait, `Self` is the implementing type — `func default() Self`, `func same(self, o Self) bool`. Inside an impl block it is an alias for the target, in every position a type or a type's name may be written: `Self{x: 0}`, `Self.default()`, a `Self{…}` pattern. In a trait DEFAULT method with a receiver, `Self.m()` calls `m` of the type the call was dispatched as — the receiver's run-time type, or, for a receiver of a sized integer type the compiler can see, that type (`b.reset()` with `b u8` reaches `u8`'s `default`, though every integer is an `int` at run time).

**Static methods.** A trait method without `self` is static. It is called on a type — `Point.default()`, `Cel.default()`, `int.default()` — or through a type parameter bounded by the trait, `T.default()`, inside a generic function (§3.7).

**Ordering.** The `Ord` constraint's method is `cmp(self, other Self) int`, negative, zero or positive. A struct or enum that provides one is ordered by it: `<`, `<=`, `>`, `>=`, `min`, `max`, `sort` and `sorted` consult it, so a type that implements `Ord` sorts the way it says. (A newtype keeps its underlying type's order, §3.8; equality stays structural.)

**Iteration.** `each` iterates a user type by calling its `next(mut self) ?T` until it answers nil. `std/iter` names that protocol — `trait Iterator[T] { func next(mut self) ?T }`, with `iter.collect` and `iter.count` — so a type can promise it (`impl iter.Iterator[int] for Countdown`) and a generic function can require it (`[I: iter.Iterator[int]]`). A type with a `next` and no `impl` iterates all the same.

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

A generic with several parameters takes its type arguments in one bracket, in declaration order, wherever a type argument may be written: `mk[str, int](k, v)`, `Lru[str, int]{cap: 8}`, `var c Lru[str, int]`. Any type form may be an argument — `mk[[]int, ?str](…)`, `Wrap[map[str]int]{…}` — and the number of arguments must match the declaration's parameters.

Constraints are traits: `Ord`, `Eq`, `Hash`, `Show`, `Copy`, `Send`, or any user trait. Generics are monomorphised in compiled mode and type-erased in the interpreter — identical observable behaviour.

A parameter may carry several bounds, joined by `+`, and the bounds may be written inline or in a `where` clause after the signature — the two spellings mean the same and combine: `func f[T: Show + Into[str]](x T)` is `func f[T](x T) str where T: Show + Into[str]`. An impl block takes a `where` clause before its brace, `impl[T] Shelf[T] where T: Into[str] { … }`. A bound naming a generic trait gives its arguments (`T: Into[str]`), as many as the trait declares.

**Static methods through a parameter.** `T.default()` calls a static method of whatever type `T` is. Where generics are erased — as they are in this release, one compiled body serving every instantiation (manual 7.4) — such a function receives the type at run time: every type parameter of a top-level function whose bounds include a trait that declares a static method takes a hidden, leading **type-descriptor** argument — the type's name, the key the method table is searched by. The caller supplies it from an explicit type argument (`make[Point]()`; `make[T]()` inside another generic function forwards that function's own descriptor), or else reads it off a value argument declared `T` (`like(p)` for `func like[T: Default](x T) T`) — its run-time type, looking through a Box, except that an argument of a known sized integer type passes that type (`u8`), since every integer is an `int` at run time, and an argument declared as one of the ENCLOSING generic function's own parameters forwards that function's descriptor (`like(x)` inside `wrap[T: Default](x T)` reaches what `wrap`'s caller said `T` was); a call that offers neither is a compile-time error asking for `make[Type](…)`. Because the descriptor is part of the call, such a function cannot be used as a value, and a method's, an impl block's or a trait's own type parameters carry none: `T.m()` there is refused. A call through a parameter takes positional arguments only.

A constraint is **checked wherever a real type is written in place of the parameter**: in a composite literal (`Stack[Point]{}`), in an annotation (`var s Stack[Point]`), and in an explicit instantiation (`max[Point](a, b)`). The argument must provide each bound's methods — and where it implements a generic trait explicitly, at the bound's arguments: a type that implements only `Into[int]` does not satisfy `T: Into[str]` — and a method counts wherever it is written — dispatch is by name and does not distinguish an inherent `impl T { … }` from an `impl Trait for T`, so both satisfy. The six predeclared constraints above have no method set of their own and nothing is judged against one. A type argument that is not written but INFERRED — from the arguments of a call, by unification (§5.5) — is checked against the bounds exactly as a written one is: `show(3.5)` for `func show[T: Sh](x T)` is an error when `float` does not implement `Sh`. Inside a generic body a value of a bounded parameter answers only the methods its bounds declare (and `str`, `clone`): `x.nope()` on an `x T` where no bound of `T` declares `nope` is an error, since an instantiating type need not have one. (A parameter with no bound, or with a bound naming nothing the program declares, is not judged: its calls dispatch at run time.)

A type argument may itself be an instantiation — `Stack[Box[int]]`, in an annotation and in a composite literal alike.

### 3.8 Type declarations

```voila
type UserID = int              // ALIAS: another name for int
type Celsius struct { deg float }   // named struct
type Cel int                   // NEWTYPE: a distinct type over int
type Handler fn(Request) Response   // a newtype over a function type
```

The two forms without `struct` differ by the `=`:

- `type X = T` is an **alias**. `X` and `T` are the same type: interchangeable everywhere, with `T`'s methods and none of its own, and `X(v)` is `T(v)`.
- `type X T` is a **newtype**: a distinct named type with `T`'s representation. It takes methods (`impl Cel { … }`) and trait implementations of its own, which dispatch on `Cel` and not on `int`. A value is made and taken apart by explicit conversion — `Cel(5)` converts to `T` first, as `int(5.7)` would, so `type Age u8` keeps `u8`'s range, and `int(c)` yields the underlying value. An untyped LITERAL stored where a newtype is declared converts implicitly, as an untyped constant does in Go — wherever the type check expects the newtype: `var c Cel = 5`, an assignment, a struct field, a returned literal, an argument (a generic one at the newtype too), a map key (`m[2]` on a `map[Idx]str`, and `2 in m`), an element of a collection literal (`let xs []Idx = [5]`) — and so does a CLOSURE literal where a newtype over a function type is declared (`Route{h: fn(p str) str { … }}` for `type Handler fn(str) str`); any other value needs the conversion. A newtype keeps its underlying type's operators **between two values of the same newtype** (`c1 + c2`, `c1 < c2`; an arithmetic result is the newtype again); mixing it with another type, a plain `int` included, is refused — convert explicitly. It prints as its underlying value unless it implements `Show`, and it is otherwise transparent to the builtins that read a value: a newtype over a slice can be indexed, iterated, measured with `len` and handed to a native, one over a function type can be called, and one over a struct has the struct's fields.

---

## 4. Declarations and Bindings

```voila
let x = 10             // immutable binding, type inferred (int)
let y int = 10         // explicit type
var count = 0          // mutable binding
count = count + 1
var buf []byte         // zero value: empty slice
const MAX = 1024       // compile-time constant, untyped until used
let (q, r) = divmod(17, 5)   // destructure a tuple; also `let q, r = …`
var (lo, hi) = (0, 9)        // …into mutable names
```

A destructuring binding takes one name per tuple member; `_` discards one.

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
- Postfix `x?` propagates a failure: it is exactly `try x` (§8.1), written after the operand so a chain reads left to right: `half(half(n)?)?`. It is unrelated to the PREFIX `?T` of an optional type, which only appears where a type is written.
- Postfix `x!` has no expression meaning; `T!` is a type form only.
- `t.0`, `t.1` select a tuple's members by position (§3.3).

### 5.2 Ranges

```voila
0..10       // exclusive, 0 through 9
0..=10      // inclusive
0..10 by 2  // stepped
```

As a slice index either bound may be left out: `s[2..]` runs to the end, `s[..3]` starts at 0, and `s[..]` is the whole slice or string. A missing upper bound is `len(s)`, evaluated after `s`. Elsewhere a range needs both bounds.

### 5.3 `if` and `match` as expressions

```voila
let size = if n < 10 { "small" } else if n < 100 { "medium" } else { "large" }
let name = match shape {
    Circle(r)  => "circle {r}"
    Rect(w, h) => "rect {w}x{h}"
    Empty      => "empty"
}
let v = if let Some(x) = to_int(s) { x } else { 0 }
```

In value position, `if` needs an `else`, and the value of each branch (or arm) is the value of its **last statement**, which must be one of:

- an expression;
- an `if` … `else`, a `match`, or a `{ … }` block, whose own branches yield the value in the same way;
- `return`, `throw`, `break` or `continue` — the branch **diverges** and produces no value, so the others decide it.

Statements may come before the last one. A branch that ends any other way (empty, in a `let`, in a loop, in `say`) is a compile-time error: it neither produces a value nor leaves. A call that never comes back, such as `os.exit(2)`, is an expression and is accepted as a final statement.

A `match` expression is exhaustive by the same rules as the statement.

This is a rule about VALUE position. A statement-form `if` or `match` written last in a closure body is still a statement, not the closure's trailing value (§10); write `return if … else …` there.

### 5.4 Parallel assignment

```voila
a, b = b, a                 // swap
a, b, c = b, c, a           // rotate
xs[0], xs[1] = xs[1], xs[0]
q, r = divmod(17, 5)        // any tuple-valued right-hand side
```

Every right-hand value is evaluated before any target is written, so the targets may appear among the values. The targets are then stored left to right, each one exactly as a single assignment would store it — so in `i, xs[i] = 1, 9` the index is read after `i` became 1. Each target is a name, field or index that a single assignment could take; `_` discards. Only plain `=` takes several targets, and when the right-hand side is written as a list its length must equal the number of targets (likewise the names of a destructuring binding and a literal tuple).

### 5.5 Static typing

Every expression has a type, worked out locally: a `let` or `var` without an annotation takes its initialiser's type, and an unannotated `var` **keeps** it — `var n = 3; n = "three"` is an error. A call is judged against the signature it calls; a generic one is instantiated by unifying its parameter types with the arguments, left to right, so `mx(1, "x")` for `func mx[T](a T, b T) T` is an error at the second argument, while `mx(1, 2.5)` is a `float` call (an untyped literal's binding gives way to a wider number). A returned value is judged against the declared result.

A value must fit the hole it is put in — an initialiser, an assignment, an argument (a variant's payload, a struct field in a composite literal), a returned value, an element of a typed collection literal or one appended or added, a value sent on a typed channel. It fits when the types are the same, along §3.2's implicit conversions (an integer stored into any integer type is range-checked at run time), when an untyped literal fits (§3.8), when the hole is `?T` and the value is a `T` or `nil`, and when the hole is a trait the value's type implements. `nil` fits only an optional; a `?T` coming straight from a call is not a `T` until it is unwrapped (`must`, `else`, `if let`, `match`). An `if` or `match` used as a value has one type: each branch is judged against the type the context expects, and with nothing expected the branches must agree.

Operators take the kinds they are defined on: `+ - * / ~/ % **` numbers (a newtype only with the same newtype), `and`/`or`/`not` and every condition a `bool` — there is no truthiness — and `==`/`<` two values of one kind (a rune compares with the one-character string it denotes, and numbers with numbers). `+` never concatenates. A method must exist on a receiver whose method set is visible — a struct, an enum, a newtype, a primitive, a builtin collection — and a read-only location — a borrowed parameter, or `self` in a method without `mut self` — can neither receive a `mut self` call nor be handed to a `mut` parameter (§6.1).

A NAME of an optional type — or a LOCATION under a name: a field (`s.v`), an element at a literal index or at an index that is itself such a location (`xs[0].v`, `xs[i]`), a map entry at such a key (`m["a"]`, `m[p.k]`) — is a `T` only where it is NARROWED, and narrowing follows the flow of the function: a binding or an assignment of a certainly non-nil value narrows the name, a condition narrows it in the branch it guards (`x != nil` in its own branch, `x == nil` in the other, through `and`, `or` and `not`), and at a join it stays narrowed only if every branch that falls through leaves it narrowed — a branch that returns, throws, breaks or continues does not count, so `if x == nil { return }` narrows `x` below it. A `while` loop with no `break` leaves its condition false behind it (`while x == nil { x = … }` narrows `x`), a `match` arm after an unguarded `nil` arm sees its NAME subject non-nil, `assert(c)` narrows what `c` proves, and `os.exit` leaves. An assignment of a value that may be nil undoes it, a loop forgets it for every name the loop assigns, a closure, a spawned block and a deferred one keep only the narrowing of `let` names (which cannot change), a name a closure assigns is never narrowed, and neither is a module-level `var` (any function may reassign it). A location's narrowing is also undone by an assignment that may reach it — to it, to a location above it, or through an index that may be equal (`xs[j] = …` reaches `xs[i]` and `xs[0]`; `xs[1] = …` does not reach `xs[0]`, nor `s.w = …` `s.v`) — by any write to a name its index reads (`i += 1` for `xs[i]`), and by any write under its root that is not an assignment: a method that writes its receiver, an argument a call may write, a collection method that writes. An operator's result is never nil, so `x = x + 1` keeps a narrowed `x` narrowed. A location whose index is no literal and no location (`xs[i + 1].v`, `f().v`) is never narrowed: a `?T` read from it is used as a `?T`. A `?T` name used as a `T` anywhere else is an error, like a `?T` straight from a call. A map is indexed by its key type, a slice or a string by an integer position; a function value is called with as many arguments as its type says; `len` and `each` take a string or a collection; a primitive has no fields; a destructuring takes one name per member of a tuple of known width. A std native's result has the type the runtime states (`os.run_capture` is `(int, str, str)!`). A generic parameter that an argument `nil` binds is an optional: `pick(nil, 3)` is a `?int` call.

Where the information is not there the check is silent rather than guessing: a value whose type nothing writes down (an untyped JSON value, a native's result), a trait-typed value (its run-time type decides, §3.6), a value of a type parameter inside its generic body (beyond its methods, §3.7), an exception, a std package's opaque type, a `shared`/`cell`/`weak` handle — and the narrowing of a FIELD or an element (`if p.age != nil { p.age + 1 }` is accepted unjudged).

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

**`Box[T]`** is an owned, unique heap indirection — the spelling of a recursive data type:

```voila
enum Tree[T] { Leaf, Node(v T, l Box[Tree[T]], r Box[Tree[T]]) }
struct LNode { v int, next ?Box[LNode] }

let t = Node(5, Box(Leaf), Box(Node(7, Box(Leaf), Box(Leaf))))
match t { Node(v, Leaf, Node(w, _, _)) => say v + w; _ => say "other" }
```

`Box(v)` makes one; `b.get()` reads the value and `b.set(v)` replaces it. A Box is otherwise **transparent**: field access and method calls reach the value inside, `==`, ordering, arithmetic and printing see it, and a pattern matches through it (a binding pattern binds the Box itself).

A Box has **value semantics**: it is a unique owner, so `var b = a` gives `b` a Box of its own, and `b.set(2)` — or a field or element assigned through `b` — is not seen through `a`. The copy is made lazily, when a Box that something else still holds is written through (copy-on-write); a Box only one name holds is written in place. A `mut` parameter is the exception it should be: a Box passed to one — by name, or as a field or an element (`bump(arr[0])`, `bump(m["k"])`) — is first made unique where it lives, and the callee then writes that location's Box in place, not one another name still holds. Because the value written — the argument of `b.set(…)`, the right-hand side of `b.next = …` — is complete before the write, `b.set(LNode{v: 2, next: b})` stores the node in a NEW Box and the node holds the OLD one — and `n.next = n` likewise, as does `n.b = Box(n)` for a struct `n` with a `?Box` field (the struct has value semantics, §3.4, so the Box holds the struct as it was); a write through a `mut` parameter whose value holds the parameter's Box goes to the callee's own copy, the caller's Box keeping its old contents. A Box can never be made to contain itself, so the recursion a Box carries is a tree, and both `Box[T]` and `?Box[T]` are excluded from the Acyclicity Rule's owning edges. A program may declare its own type named `Box`, which then shadows this one.

A **direct** self-reference — `Node(v T, l Tree[T])` — is not boxed for the program: the rule above admits self-reference only through `weak[T]` or a Box, so the indirection is written where it happens, and the diagnostic for the cycle names both spellings. Destruction of a deep structure is iterative; a list a million cells long is freed without growing the stack.

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
  A value moves when it is handed to an `own` parameter, dropped, or — for a type that owns storage (a slice, map or set, a channel, a function, or a struct, tuple, enum or `?T` holding one) — bound to a second name, assigned, or sent on a channel **by name**. A `Box` has value semantics of its own (§6.3) and moves with its contents; the shared handles `shared`, `cell` and `weak` exist to be held twice and do not move. Only a binding the function owns moves this way — a `let`/`var` or an `own` parameter; a borrowed parameter bound to a second name is a second borrow. The check is straight-line: a move inside a branch or a loop body is tracked to the end of that body, a move before one is seen by every use inside it, and a re-assignment revives the name.
- A Copy struct is **copied**, and the copy is made lazily: the two names share the value until one of them writes it — a field or an element assigned through any path, a `mut self` method, a `mut` parameter — and the writer then gets its own (each level of the path is copied only if another holder still shares it). A program cannot tell a lazy copy from an eager one, except by its speed.
- The same holds for every value with storage of its own — a struct, a slice, a map, a set, a Box — wherever it is held: a field, an element, a map value, a second name, a struct or tuple or collection literal it was put into. A write through one holder (an element stored, `append`, a map store, a `mut self` method, a `mut` parameter) is never seen through another: `let alias = arr[0]; arr[0].tags[0] = 9` leaves `alias.tags` alone, and so does `let h = Holder{v: xs}; xs[0] = 9` for `h.v`. Where a move by name applies (above), it is a compile-time error to use the moved name; everywhere else the value is copied on its first write.
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
Braces are mandatory; parentheses around the condition are not allowed. An `if` with an `else` may also be used as a value (§5.3).

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

`try f()` is exactly `match f() { Ok(v) => v, Err(e) => return Err(e) }`. The postfix spelling `f()?` means the same thing and is often easier to read in a chain: `let n = half(half(x)?)?`.

Propagation returns the failure from the enclosing function, which is meaningful when that function declares `T!` or `?T` or declares `throws` (and in a closure with no written result, or a `spawn`/`defer` body), and in `main`, where a returned failure is the documented "error value escaping the main procedure" (exit status 1). In a function that declares a plain result `T` — or no result at all, whose caller would discard a failure — and no `throws` clause, `try` and `?` instead **raise** the failure as an exception, exactly as `must` does (§8.5), rather than hand the caller an error disguised as a `T` or drop it on the floor. That is what makes `load` in §8.2 throw when `os.read` fails.

The alternative of `else` may also be a **block**, which runs only on failure:

```voila
let opts = parse(args) else {
    usage()
    os.exit(2)
}
let n = to_int(s) else { say "not a number:", s; 0 }
```

The block follows the rule of §5.3: its last statement is its value (here `0`), or it diverges (`return`, `throw`, `break`, `continue`, or a call such as `os.exit` that never returns). A block that does neither is a compile-time error — unless the whole `f() else { … }` is a statement of its own, whose value nobody uses: `save(doc) else { say "not saved" }` simply runs the block on failure.

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

**Rethrow** with a bare `throw` inside a `catch`. **Chain** with `throw ParseError{...} from e` — `e` becomes `cause()`, and `e.trace()` retains the original stack: it is recorded when the exception is FIRST thrown, innermost frame first, one `function (file:line)` string per frame.

A `finally` block always runs to its end: a `return` in it, or a `break`/`continue` whose loop encloses it, is a compile-time error — control can leave a finally only by falling off its end or by a throw. Inside a finally, `?` and `try` do not propagate a failure as the function's result (that would be a hidden `return`): they raise it as an exception, as `must` does (§8.1), in every function.

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

The report goes to standard error, the message first and then one line per active frame, innermost first, naming the function as the program spells it (a closure as the function it is written in) and the line of the statement that failed:

```
abort: index out of bounds: 5 (len 2)
  at pick (app.voi:3)
  at outer (app.voi:7)
  at main (app.voi:14)
```

An exception that reaches the top of the program is reported the same way, from the trace it carries (`unhandled exception: …` followed by its `at` lines, then any `caused by:` chain). Only the innermost 64 frames are listed; the rest are counted.

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

**Data-race freedom is a static guarantee**, inherited directly from §6: two tasks cannot hold a `mut` borrow of the same value, because `mut` borrows cannot escape into a task in the first place. A spawned task that would write a variable it shares with its group in a way another task's write could race is refused at compile time (§10); the element and field stores that remain are made one at a time.

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

A captured variable is one VARIABLE, shared by the function and every closure that captures it — the tasks it spawns included. Its VALUE keeps the value semantics of §6.4 everywhere: `let row = out[0]` takes a snapshot whatever ran before, and a program without tasks behaves exactly as one whose tasks have joined. Inside a `spawn` body (its block, or the call `spawn f(x)` makes, and any closure written in it) a task may store into a slice element or a field of a captured variable — `for j in 0..4 { spawn { out[j] = j * 10 } }` fills `out`, and the stores of different tasks never undo each other — but nothing that could race another task's write is accepted: assigning the variable itself (`x = …`, `x += 1`, a lost update), storing into a map in it (a store may add a key), calling a method that grows, shrinks or reorders a collection in it (`append`, `insert`, `remove`, `pop`, `sort`, `sort_by`, `reverse`, `add`, `delete`, `set`) or any `mut self` method on it, and handing it to a `mut` parameter are compile-time errors. Results that grow are sent over a channel, or kept in a `cell` or a `shared` value (§9.4). The same rules follow a closure written outside the `spawn`: one that would break them is task-unsafe, and a spawned body may not run one it captures, nor may one be handed to a parameter its callee runs in a task, or sent over a channel. Writes inside the user's own critical section — between `mu.lock()` and `mu.unlock()`, or in a closure passed to `with`, `with_write`, `update` or `do` — are the user's synchronisation and are not refused. A compound store into an element or field (`tot[0] += 1`), and a store whose value reads the same variable and calls nothing (`s.v[1] = s.v[1] + 1`), is made in one step with respect to the other tasks. Where a closure still reaches a task unseen — inside a struct field, a slice, a map, a global, or returned from a function — the run time keeps memory safe. Every read of a captured variable takes its own reference to the value, a snapshot; once a closure over the frame MAY ESCAPE its task — any closure the translator cannot prove is only called in place, or handed straight to a builtin collection method that runs it at once (`xs.map(fn …)`), or bound by a `let` used only so — it takes that reference under the frame's lock, the lock every write through a captured variable holds. The frame is marked as such when that closure is made, on its own task, before the closure can be anywhere else; a binding never written after it is made (no assignment, compound assignment, `mut` argument, `mut self` call, element or field store through its name, or PARSE) is read without the lock, since no write can replace its value. A writer that finds the value held copies it before writing (§6.4), so a reader's snapshot is never reallocated, released or written under it, and the reader indexes, measures, iterates or hands it to a function with no lock held. A `mut` argument or a `mut self` receiver in a captured variable — Voilà code, which runs with no lock held, since it could wait on another task — works, once a closure over the frame may escape, on a private copy taken under the lock and stored back under it when the call returns: a closure that reads the variable during the call sees the value from before it, and concurrent calls may lose updates, never memory. The natives that write an argument in place — `sort.sort`, `sort.sort_by`, the prelude `append` — take it as a `mut` argument, so a second name keeps its value; a sort calls its comparator on a private array and stores the finished order in one step. An index that calls a function is computed before a write takes the lock, and in `xs[len(xs) - 1]` the index reads the same snapshot as the subscript. `self` of a `mut self` method is the caller's value, written in place: a spawned body may not use it, and a closure that uses it may only be passed straight to a builtin collection method that runs it at once (`xs.map`, `xs.each` — judged by the receiver's type: `chan.map` runs its function in a task). Module-level `var`s are not covered by these rules: tasks share them only through `cell`, `shared` or a channel.

**A loop variable is a fresh binding on every iteration**, as in Go 1.22: a closure created in an iteration — called at once, stored, deferred or spawned — keeps that iteration's value.

```voila
var fs = []
for i in 1..4 { fs.append(fn() int { return i }) }
each f in fs { say f() }               // 1, 2, 3
for t in ts { say len(xs.filter(fn(x) { x > t })) }
```

The closure receives its own copy of each loop variable it names, taken when the closure is created. A variable declared before the loop is one binding shared by every iteration and every closure. A loop variable that is ASSIGNED after a closure has copied it — in the loop body or inside the closure — would silently stop being shared, so that one case is a compile-time error naming the variable: declare it before the loop to share it, or copy it into a `let` to capture its value.

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
voila 0.6.0

require (
    github.com/moshix/go3270  v1.4.2
    codenotary.com/immudb-voi v0.9.0
)

replace codenotary.com/immudb-voi => ../immudb-voi
```
Directives, and nothing else:

| Directive | Form | Rule |
|---|---|---|
| `module` | `module <modpath>` | **exactly one** per file |
| `voila` | `voila <version>` | the language version the module needs; a version **newer** than the compiler is refused, and the error names both |
| `require` | `require <modpath> <version>`, or a parenthesised block of the same | a duplicate `require` for one path is an error unless the versions agree |
| `replace` | `replace <modpath> [<version>] => <localpath>` or `=> <modpath> <version>`, single or block | applies **only from the main module**, never transitively |

An **unknown directive is an error**, not something quietly skipped.

**Resolution order** for an import path, in full: a Voilà-source std package, a C std package, a `replace` target, the module cache, a directory under the module root — and then a refusal that names every one of those places it looked.

**Version selection** is the maximum of the required versions per module path. For a graph of exact requirements that *is* minimum version selection, and it needs no backtracking.

The **module cache** is `$VOILA_MODCACHE`, defaulting to `~/.cache/voila/mod/<modpath>@<version>/`. A cached dependency is loaded and flattened from source exactly like any other package, so nothing downstream of the loader knows where it came from.

`voila.sum` holds one `<modpath> <version> <hash>` line per dependency. **It is a drift detector, not a security boundary**: the hash is FNV-1a-64, which is not cryptographic. It catches a cached tree edited or truncated underfoot; it does not resist a deliberate collision. (`std/crypto` has a real SHA-256, and this deliberately does not use it yet: the compiler must stay buildable by a bootstrap compiler whose runtime has no `crypto.*` natives at all. The `f1a64:` prefix on every hash exists so the swap can be told apart when it happens.) A module with no `voila.sum` is not checked; once the file exists it is authoritative, and a missing entry is as much a refusal as a wrong one. A `replace` to a local directory is never hashed — it is source you are editing.

`voila get <modpath>@<version>` **shells out to `git clone --depth 1 --branch <version>`**. There is no network code in the compiler and there will not be: git owns the transport, the credentials and the trust. The compiler checks that the clone declares the module it is being filed as, hashes it, records or verifies the hash in `voila.sum`, and moves it into the cache under a claim-then-move rename.

### 11.2 `use`
```voila
use "std/io"
use "std/http"
use "github.com/moshix/go3270" as t3270      // alias
use "std/str" { upper, words }               // selective import
use "std/math" *                             // wildcard (discouraged; allowed in scripts)
```
Import path == repository path == directory. One directory = one package. Unused imports are a **warning**, not an error (a deliberate softening of Go, for REPL/script comfort).

### 11.2.1 Name qualification

**Every** exported declaration of a package is qualified by it — functions, types, exceptions, *enum variants* and *trait names* alike. `Circle` declared in `geom` is `geom.Circle`; `Named` declared in `geom` is `geom.Named`. Two independent packages may therefore each declare an `Empty` or a `Named` without colliding, which is what makes an ecosystem possible at all.

Variants and traits are also the only names an importer may spell **without** a qualifier, because patterns, `impl` headers and generic bounds read better bare. An unqualified name resolves in this order:

1. the current package;
2. exactly **one** import.

Two imports offering the same bare name is an error naming both candidates — never a silent pick. The qualified spelling always works and is how the ambiguity is resolved:

```voila
match s {
    geom.Circle(r) => …          // always unambiguous
    Rect(w, h)     => …          // short form: geom is the only import with it
}

impl geom.Named for Box { … }    // a trait may be qualified too
func show[T: geom.Named](x T) str { return x.Name() }
```

### 11.3 Commands
```
voila run main.voi        # interpret (bytecode VM)
voila run .               # interpret the current module
voila build -o app .      # AOT native, static by default
voila build --target=linux/arm64 .
voila test ./...          # tests are funcs named Test* in *_test.voi
voila bench ./...
voila fmt ./...           # canonical formatter, no options (gofmt doctrine)
voila get github.com/x/y@v1.2.0   # fetch into the module cache (via git)
voila mod init [modpath]  # write a voila.mod
voila mod verify          # re-hash the cached dependencies against voila.sum
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
    link "z"                                   // the build adds -lz
    func printf(fmt *char, args ...) int
    func compress(dst *u8, dstlen *u64, src *char, n u64) int
}
```
Everything inside an `ffi` block is outside the safety guarantees and is the one place where the compiler cannot promise you anything. It must be marked, and `voila check --strict` rejects it.

An `ffi "C"` block is a top-level declaration of a file that has `use "std/ffi"`. Its members are `link "name"` — a library the program links, as cc's `-lname` — and C prototypes, `func name(param ctype, …) ctype`, each of which is then an ordinary function of the package: called, typed and checked like any other. The C types and the Voilà values they take:

| C type | Voilà | |
|---|---|---|
| `char` `short` `int` `long` and the `u`/`unsigned` forms, `i8`…`u64`, `size_t` `usize` `ssize_t` `intptr_t` … | an integer | a narrow parameter is range-checked on entry (§3.2); every integer result is an `int` (`long` is 64 bits: LP64) |
| `float` `double` (`f32` `f64`) | `float` | |
| `bool` | `bool` | |
| `*char` | `str` in, `?str` out | the str's bytes are lent, read-only, for the call — a buffer C writes is a `*void` given `ffi.alloc` memory; a result is copied, and NULL is `nil` |
| any other pointer — `*void`, `*u8`, `**char` … | `ffi.Ptr` | `nil` and `ffi.null()` are NULL |
| no result | `unit` | |
| `...` | anything | C varargs, at most six, passed by run-time kind under the default promotions (a float as a double, anything else as a 64-bit integer or an address) |

There are no callbacks into Voilà and no struct layout syntax: a C struct is memory, reached with `std/ffi`'s raw memory — `ffi.alloc(n)` and `ffi.free(p)`, a `Ptr` with `read_u8`…`read_f64`, `read_ptr`, `read_cstr`, `read_str`, `read_bytes` and the matching writes at byte offsets, checked against the block's size when it is known, and `ffi.sizeof(ctype)` for the offsets. The mark is the warning `ffi` (off by default, on under `-Wall`, fatal under `--strict`) at every `use "std/ffi"`.

---

## 13. The Core Library

Everything below is in the prelude — importable, but the most common names (`say`, `printf`, `len`, `str`, `int`, `err`, `assert`) are visible without any `use` at all.

**Argument kinds.** A library function implemented natively states what each argument must be — a `str`, an `int`, a slice, a map, a function — wherever its body depends on it: `os.run(prog, args)` takes a `str` and a slice of them, `str.join(parts, sep)` a slice — or a set, in its insertion order — first (so do `sum`, `avg`, `zip`, `enumerate`, `str.from_bytes` and `sort.reversed`). A call whose argument's static type is known and is of the wrong kind is a compile-time error (`os.run_capture("echo", "hello")`); where the type is not known (the function called as a value, an argument of unknown type), the call is checked when it is made and a wrong kind aborts with the function's name and the argument's position (§8.6). A newtype or a Box whose value is of the right kind is accepted for it, by a direct call and through a value alike (`let f = str.upper; f(Name("bob"))`).

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
| `upper(s)` / `lower(s)` | Unicode simple (1:1) case mapping, as Go's; `ß` and other characters with no single-character counterpart are left as they are. `title` uses titlecase for each word's first character (`ǆ` → `ǅ`) |
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
| `trim_prefix(s, p)` / `trim_suffix(s, p)` | `s` without one leading / trailing `p`, or `s` unchanged |
| `cut(s, sep)` → `(str, str, bool)` | the text before and after the FIRST `sep`, and whether it was found; `(s, "", false)` when it was not |
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
| `bytes(s)` → `[]byte` / `from_bytes(b)` → `str!` | a `str` is a byte string, so any bytes are kept; an element outside `0..255` is an error |
| `valid_utf8(s)` → `bool` | whether `s` is well-formed UTF-8 (text, not binary) |
| `runes(s)` → `[]rune` / `from_runes(r)` | |
| `len(s)` | **bytes** |
| `rune_len(s)` | codepoints |
| `is_digit(s)` / `is_alpha(s)` / `is_space(s)` / `is_upper(s)` | REXX `DATATYPE` family; true when every rune qualifies. `is_alpha` is any Unicode letter, `is_space` Unicode White_Space, `is_upper` "no lowercase or titlecase character, and at least one uppercase one" (as Python's `str.isupper`); `is_digit` is ASCII `0-9` only, matching what `int()` parses |
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
| 24 | `os.args()` / `os.env(k)` / `os.exit(n)` | `os.args()` is the arguments AFTER the program's name — unlike C's `argv` and Go's `os.Args`, element 0 is the first argument |
| 25 | `json.encode(x)` / `json.decode[T](s)` | `decode[T]` builds a `T` recursively — nested structs, slices, maps, sets, `?T` (null or missing), enums, newtypes, `Box` — and a mismatch is an error value naming the JSON path: `limits.max_mb: expected int, got string` |
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
| `std/os` | files, processes, env, args, signals: `os.start` (a process handle: `pid`, `wait`, `kill`, `stdin`/`stdout` pipes), `os.signals(sig…)` (a channel of signal numbers), `os.kill` (a pid > 0), `os.kill_group`, `os.signal_stop`, `os.getpid`, `os.walk`, `os.walk_all`, `os.glob`. Every descriptor the runtime opens is close-on-exec. `os.args()` does not include the program's name |
| `std/math` | numerics, `std/math/dec` for decimal contexts |
| `std/conv` | the conversion functions of §3.2 |
| `std/time` | instants, durations, timers, tickers; calendar dates and RFC 3339 (UTC and fixed offsets — no tzdata) |
| `std/path` | lexical file-system paths, and `path.matches(pattern, name)`, the shell-pattern half of `os.glob` |
| `std/sync` | `Mutex`, `RWLock`, `WaitGroup`, atomics, `Once` |
| `std/chan` | fan-in, fan-out, pipelines, rate limiters: `from`, `collect`, `map`, `filter`, `merge`, `fan_out`, `tee`, `throttle`, `limiter`. The qualifier is spelled like the keyword: `chan.merge(…)` |
| `std/sort` `std/slices` `std/maps` | collection algorithms. `slices`: `contains`, `index_of`, `binary_search`, `compact`, `unique`, `chunk`, `reversed`, `insert`, `remove`, `min`, `max`, … (a slice also has the in-place methods `insert(i, v)` and `remove(i)`); `maps`: `keys`, `keys_sorted`, `values`, `items`, `merge`, `filter`, `map_values`, `invert`, … |
| `std/heap` | a binary min-heap, the priority queue: `heap.new[T]()` by `<`, `heap.new_by(less)` |
| `std/flag` | typed command-line flags: `flag.str/int/bool/float/dur(name, default, help)`, aliases (`"name\|n"`), positional arguments, generated `--help` |
| `std/iter` | the `Iterator[T]` trait `each` drives, `collect`, `count` (§3.6) |
| `std/regex` | RE2 — linear time, no catastrophic backtracking. Named groups `(?P<name>…)`: `find_named` returns a `map[str]str` of each named group (and `"0"`, the whole match), `${name}` in a replacement |
| `std/json` `std/csv` `std/xml` `std/yaml` | encodings |
| `std/http` `std/net` | networking: TCP, UDP and Unix sockets; an HTTP/1.1 client and server, with percent-decoding (`url_decode`, `url_encode`, `parse_query`, `Request.query_params`) and `Server.serve(handler, stop)` / `serve_for(handler, stop, grace)`, which stop gracefully; an abort in a handler is contained to its request (a 500), the one exception to §8.6. `https://` URLs and `http.listen_tls` speak TLS through `std/tls`, verifying the server's certificate (§13.6) |
| `std/tls` | TLS 1.2 over a `net.Conn`, verified against the system's CA bundle or a given one, with SNI; a server from a PEM certificate and key (§13.6) |
| `std/compress` | DEFLATE, zlib and gzip, one-shot and as `io` streams, with a bound on every decoder's output; zip and tar archives (§13.7) |
| `std/sqlite` | an embedded SQL database: prepared statements, typed rows, transactions (§13.8) |
| `std/crypto` | SHA-256/512, SHA-1 and MD5 (legacy), HMAC, and a cryptographically secure random source (§13.5); signatures are not yet implemented |
| `std/log` | structured logging |
| `std/test` | assertions, table tests, fuzzing, benchmarks |
| `std/vm` | embed the interpreter: `eval`, `compile` |
| `std/ffi` | C interop (§12): `ffi "C"` blocks, and raw memory — `alloc`, `free`, `Ptr`, `cstr`, `sizeof` |

### 13.5 `std/crypto` — digests, HMAC, and randomness

Written from FIPS 180-4, RFC 1321 and RFC 2104 in the runtime's own C. There
is no OpenSSL and no other third-party dependency.

| | |
|---|---|
| `crypto.sha256(m)` `crypto.sha512(m)` | lowercase hex digest |
| `crypto.sha1(m)` `crypto.md5(m)` | **legacy** — see below |
| `crypto.<alg>_raw(m)` | the same digest as raw bytes |
| `crypto.hmac_sha256(k, m)` `crypto.hmac_sha512(k, m)` | RFC 2104, hex |
| `crypto.hmac_sha1(k, m)` `crypto.hmac_md5(k, m)` | legacy hashes, keyed |
| `crypto.hmac_<alg>_raw(k, m)` | the same MAC as raw bytes |
| `crypto.random(n)` | `n` cryptographically secure random bytes |
| `crypto.random_int(n)` | a uniform integer in `[0, n)`, no modulo bias |
| `crypto.equal(a, b)` | equality in time independent of where they differ |

A message and a key may each be spelled as a `str` (which is a byte string —
`os.read` and `net.recv` already carry binary in one) or as a `[]byte`; the
two produce the same digest. Digests come back as lowercase hex, or as raw
bytes from the `_raw` names, over which `str.hex`, `str.b64` and `bytes` are
the whole encoding layer:

```voila
crypto.sha256("abc")                     // "ba7816bf…f20015ad"
crypto.sha256_raw("abc").b64()           // "ungWv48Bz+pBQUDeXa4iI7ADYaOWF3qctBD/YfIAFa0="
bytes(crypto.sha256_raw("abc"))          // []byte, 32 long
crypto.random(32).hex()                  // a 256-bit token
```

**SHA-1 and MD5 are broken for security** and are provided for checksums and
for interoperating with formats that already chose them. Practical collisions
exist for both, so neither may carry a signature, a certificate, or a
content-addressed identity. HMAC over either has no practical forgery, but a
new protocol wants `crypto.hmac_sha256`.

**`crypto.random` is the only random source fit for a key, a token, a nonce
or a session id.** It draws from `getrandom(2)` on Linux, `getentropy(2)` on
macOS and the BSDs, and `/dev/urandom` where neither is reachable, and it
aborts rather than falling back to a weak source. A bad *count* is an
ordinary catchable `RangeError`, as everywhere else in the language; only a
missing entropy source is fatal. `std/rand` is a different thing: a fast,
seedable, *reproducible* sequence for simulations and tests, whose output is
predictable from a few hundred samples. `uuid.new()` draws from
`crypto.random`; `std/rand`'s default seed does too, falling back to the
clock only on a machine with no entropy source at all — a program that never
draws a random number must still be able to start.

Compare a MAC with `crypto.equal`, not `==`. `==` stops at the first byte
that differs, and that timing is enough to recover a MAC one byte at a time
from a service that will verify a guess.

**Vendored libraries.** `std/tls`, `std/compress` and `std/sqlite` are built
on C libraries carried in the toolchain's source (`runtime/src/vendor`:
BearSSL, miniz and the SQLite amalgamation), not taken from the system. Each
is compiled into a program only when the program uses its package — the
first such build compiles the library and caches it beside the runtime's
archive, every later one reuses it — and none of them needs anything beyond
libc, so the executable still links libc alone. A program that uses none of
them is built exactly as if they did not exist.

### 13.6 `std/tls` — TLS for `std/net` connections

```voila
var conn = try tls.connect("example.com:443", tls.Config{})
try conn.write_all("GET / HTTP/1.1\r\nHost: example.com\r\n\r\n")
```

| | |
|---|---|
| `tls.connect(addr, cfg)` | dial TCP and handshake as a client → `net.Conn` |
| `tls.client(conn, cfg)` / `tls.server(conn, cfg)` | handshake over a connection already open |
| `tls.listen(addr, cfg)` | a `Listener` whose `accept()` handshakes each connection |
| `tls.check_server(cfg)` | load a server's certificate and key now, reporting what is wrong |
| `tls.version(conn)` | `"TLS1.2"`, or `""` for a plain connection |
| `tls.system_ca_file()` | the CA bundle a client trusts by default, or `""` |

`Config` has `server_name`, `ca_file`, `insecure`, `cert_file`, `key_file`
and `timeout_ms` (30000; it bounds the handshake), every one defaulted. A
TLS connection **is** a `net.Conn`: once the handshake is done the session
sits beneath the socket, and every method — reads, writes, `io.reader`,
timeouts, `close` — works through it. A client verifies the chain against
trust anchors (the platform's bundle — `$SSL_CERT_FILE`, `/etc/ssl/cert.pem`,
`/etc/ssl/certs/ca-certificates.crt`, `/etc/pki/tls/certs/ca-bundle.crt` and
the other usual places — or `ca_file`), the validity dates, and that the
certificate names the host (`server_name`, else the address's host, which is
also sent as SNI). A subjectAltName is required (no Common Name fallback); a wildcard is only a
whole left-most label with two labels after it; an IP-address host is
matched against IP-address SANs; an extendedKeyUsage must allow
serverAuth; no certificate the peer sends may be SHA-1-signed;
renegotiation is refused; a server name with a NUL, a `*`, an empty
label or a non-host character is refused before it is used. Each failure is an `IOError` that says which:
`certificate is not trusted (unknown certificate authority)`, `certificate
does not match host name "…"`, `certificate has expired or is not yet
valid`. `insecure` skips verification and is for tests only. A server's
certificate file is a PEM chain, leaf first; its key a PEM RSA or EC key
(PKCS#1, SEC 1 or PKCS#8, unencrypted), cached and re-read when either file
changes. A connection that ends without close_notify is an error, never
a silent end of stream. TLS 1.2 only; no client certificates, session resumption or
revocation checking. A session is one state machine, so tasks sharing a
connection take turns (a read in progress delays a write).

### 13.7 `std/compress` — DEFLATE, zlib, gzip, zip, tar

| | |
|---|---|
| `deflate(data, level)` / `inflate(data, max)` | raw DEFLATE (RFC 1951) |
| `zlib(data, level)` / `unzlib(data, max)` | zlib (RFC 1950) |
| `gzip(data, level)` / `gunzip(data, max)` | gzip (RFC 1952); every member of a multi-member input |
| `deflate_writer(w, level)` … `gunzip_reader(r, max)` | the same as an `io.Writer` / `io.Reader` |
| `crc32(data, crc)` / `adler32(data, adler)` | the running checksums |
| `zip_open(bytes)` → `ZipReader` | `entries`, `names()`, `read(name, max)`, `extract_to(dir, max)` |
| `zip_create()` → `ZipWriter` | `add(name, data, level, mtime)`, `add_dir(name)`, `finish()` → bytes |
| `tar_read(bytes, max)` / `tar_write(entries)` | ustar, reading GNU long names and pax headers too |

Data is a `str`, a byte string. `level` is 0–9 (default 6). Decoding is
strict — exactly one stream, nothing after it, every checksum verified — and
any corruption is an `IOError`, never a crash. **Every decoder is bounded**:
`max` (default `DEFAULT_MAX`, 256 MiB; `0` for no limit) is the most it will
produce, and it stops with an error the moment output would pass it, so a
decompression bomb costs an error rather than the machine's memory. A zip
entry's declared size is checked against `max` before anything is
inflated. `extract_to` and `tar_extract(bytes, dir, max)` check the whole
archive — names that would escape the directory, links, devices, sizes,
and (zip) every CRC — before writing anything; `safe_join(dir, name)` is
the name check alone. A compressing writer
must be closed — `close` writes the final block and the trailer — and
closing it leaves the writer beneath open. gzip output carries no name and
a zero timestamp, so equal input gives equal output.

### 13.8 `std/sqlite` — an embedded SQL database

```voila
let db = try sqlite.open("app.db")                  // or ":memory:"
try db.exec("CREATE TABLE t (id INTEGER PRIMARY KEY, name TEXT)")
try db.run("INSERT INTO t (name) VALUES (?)", "ada")
each row in try db.query("SELECT id, name FROM t WHERE id > ?", 0) {
    say row.int("id"), row.str("name")
}
let u = try sqlite.decode[User](row)                // a row into a struct
```

| | |
|---|---|
| `open(path)` / `open_readonly(path)` | a `DB` (`":memory:"` is private and in memory) |
| `db.exec(sql)` | one statement or a script, no parameters, no rows |
| `db.run(sql, args...)` → `int` | one statement; the rows it changed |
| `db.query(sql, args...)` → `[]Row` / `db.query_one(…)` → `?Row` | rows |
| `db.prepare(sql)` → `Stmt` | `bind`, `next`, `run`, `query`, `reset`, `close` |
| `db.transaction(fn() unit!)` | commit on `Ok`; roll back on an error or a throw |
| `db.begin()` `commit()` `rollback()` `in_transaction()` | by hand |
| `db.last_insert_id()` `changes()` `set_busy_timeout(ms)` `close()` | |
| `sqlite.decode[T](row)` | a row as a `T`, by `json.decode[T]`'s rules |

A parameter is anything implementing `sqlite.Arg` — `int`, `float`, `bool`,
`str` (TEXT), `[]byte` (BLOB), `dec` (its exact TEXT) — or `nil` (NULL), and
their count must match the statement's. A column is a `sqlite.Value` — `Null`,
`Int`, `Float`, `Text`, `Blob` — and `Row` also reads one as SQLite converts
it (`row.int(col)`, `float`, `str`, `bytes`; NULL is the zero value); an
unknown column throws `KeyError`. `prepare` accepts exactly one statement.
Every failure is an `IOError` carrying SQLite's own message. The library is
built serialized: a connection and its statements may be used from any task
without corruption, but one connection is one conversation — interleaved
steps are the program's to avoid. A transaction takes its connection:
while one is open, every other task's call on that connection waits for it
to end; it is rolled back on every way out of `transaction` (faults
included), and when a task that holds one ends. A transaction is one
task's: a task waiting on the connection while its owner waits for it
fails with a `deadlock` error. A connection per task is the pattern for concurrent writers, and
connections wait up to five seconds for each other's locks.

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
    // https:// is fetched over TLS, the certificate verified (§13.6).
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
              [ "throws" type_list | "nothrow" ] [ where ] block ;
params      = param { "," param } ;
param       = [ "own" | "mut" ] ident_list type [ "=" expr ] | "..." ;
result      = type | "(" type { "," type } ")" ;
generics    = "[" ident [ ":" bounds ] { "," ident [ ":" bounds ] } "]" ;
bounds      = trait_ref { "+" trait_ref } ;
trait_ref   = ident [ "." ident ] [ "[" type_list "]" ] ;
where       = "where" ident ":" bounds { "," ident ":" bounds } ;   (* contextual *)

struct_decl = "struct" ident [ generics ] "{" { field } "}" ;
field       = ident type [ "=" expr ] ;
enum_decl   = "enum" ident [ generics ] "{" { variant } "}" ;
variant     = ident [ "(" params ")" ] ;
trait_decl  = "trait" ident [ generics ] [ where ] "{" { func_sig } "}" ;
impl_decl   = "impl" [ generics ] [ trait_ref "for" ] type [ where ]
              "{" { func_decl } "}" ;

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

let_stmt    = ( "let" | "const" ) ( ident_list [ type ] | "(" ident_list ")" ) "=" expr ;
var_stmt    = "var" ( ident [ type ] [ "=" expr ] | "(" ident_list ")" "=" expr ) ;
assign      = expr { "," expr } assign_op expr { "," expr } ;
if_stmt     = "if" ( expr | "let" pattern "=" expr ) block [ "else" ( if_stmt | block ) ] ;
for_stmt    = [ label ":" ] "for" [ ident "in" expr ] block ;
each_stmt   = [ label ":" ] "each" ident [ "," ident ] "in" expr block ;
while_stmt  = [ label ":" ] "while" expr block ;
match_stmt  = "match" expr "{" { pattern [ "if" expr ] "=>" ( stmt | block ) [ "," ] } "}" ;
pattern     = alt_pat { "|" alt_pat } ;
alt_pat     = "_" | "nil" | ident | literal [ ( ".." | "..=" ) literal ]
            | ident [ "." ident ] "(" [ pattern { "," pattern } ] ")"
            | ident [ "." ident ] "{" [ field_pat { "," field_pat } ] [ ".." ] "}"
            | "(" pattern { "," pattern } [ "," ] ")" ;
field_pat   = ident [ ":" pattern ] ;
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
            | composite_lit | collection_lit | closure | if_expr | match_stmt
            | "try" expr | "must" expr | "attempt" expr | expr "?"
            | expr "else" ( expr | block | return_stmt | throw_stmt | "break" | "continue" ) ;
index       = expr "[" ( expr | [ expr ] ( ".." | "..=" ) [ expr ] | type_list ) "]"
            | expr "." int ;                 (* t.0: a tuple member *)
collection_lit = ( "[" "]" type | "set" "[" type "]" ) "{" [ expr { "," expr } ] "}"
            | "map" "[" type "]" type "{" [ expr ":" expr { "," expr ":" expr } ] "}" ;
if_expr     = "if" ( expr | "let" pattern "=" expr ) block "else" ( if_expr | block ) ;
```

---

## 16. Cheat Sheet

```voila
say "hi"                        // print
let x = 1                       // immutable
var y = 2                       // mutable
func f(a int) int!              // fallible function (error VALUE)
try f(1)                        // propagate error
f(1)?                           // the same, postfix
f(1) else 0                     // default on error
f(1) else { log(); 0 }          // …or a block that yields or diverges
let s = if c { "y" } else { "n" }   // if and match are expressions
let (a, b) = pair();  a, b = b, a   // tuples: destructure, swap; t.0
map[str]int{"a": 1}  set[int]{1}  []int{}   // typed literals
s[2..]  s[..3]                  // open-ended slices
match n { 0 | 1 => …; 2..=9 => …; _ => … }  // or / range patterns
match p { P{x: 0, y} => …; (a, _) => … }    // struct / tuple patterns
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

*Voilà 0.6.0 draft specification. Designed for review — the acyclicity rule (§6.3), the structured-concurrency guarantee (§9.1), and the two-mechanism error story (§8) are the three claims most worth attacking first.*
