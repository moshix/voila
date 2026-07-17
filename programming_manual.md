# Voilà Programming Manual

## Language Reference and Programming Guide

**Program Number 5799-VLA · Version 0, Release 4, Modification 0**

---

**Third Edition (July 2026)**

This edition applies to Version 0 Release 4 Modification 0 of the Voilà Language
Toolchain (self-hosted), and to all subsequent releases and modifications
until otherwise indicated in new editions. Make sure you are using the
correct edition for the level of the product.

This manual is written in the tradition of the language reference
manuals that accompanied IBM COBOL and PL/I on the mainframe: it is a
reference, not a tutorial. Statements are presented with syntax
diagrams, general rules, and usage notes. Read Chapter 1 first;
thereafter the manual is designed for random access.

**Note.** Where the behavior of the toolchain deliberately differs from
the *Voilà Language Specification, Version 0.4.0 (Draft)*, the difference
is recorded in Appendix D. Programs should be written to this manual.

---

## Contents

- Chapter 1. Introduction to the Voilà Language
- Chapter 2. Language Elements
- Chapter 3. Data Types and Declarations
- Chapter 4. Expressions and Operators
- Chapter 5. Statements
- Chapter 6. Procedures
- Chapter 7. Aggregates: Structures, Enumerations, Traits
- Chapter 8. Storage Management and Ownership
- Chapter 9. Condition Handling
- Chapter 10. Concurrent Processing
- Chapter 11. The Standard Library
- Chapter 12. Compiling and Running Programs
- Appendix A. Reserved Words
- Appendix B. Format Verb Summary
- Appendix C. Completion Codes
- Appendix D. Deviations from the Specification
- Appendix E. Glossary

---

## About the Syntax Diagrams

Syntax diagrams are read from left to right, top to bottom, following
the main path.

```
   >>── begins a statement.
   ──>< ends a statement.
   ──>  the syntax continues on the next line.
   >──  the syntax is continued from the previous line.
```

Keywords appear in CAPITALS on the diagram but are written in lowercase
in source programs; Voilà is case-sensitive and all keywords are
lowercase. Variables you supply appear in italics (rendered here in
lowercase between angle brackets). A vertical stack of items indicates a
choice; an arrow returning to the left above an item indicates that the
item may be repeated.

```
                 ┌─,──────────────┐
   >>──SAY───────▼──<expression>──┴──────────────────────────────────────><
```

---

# Chapter 1. Introduction to the Voilà Language

## 1.1 Nature of the Language

Voilà is a statically typed, memory-safe, concurrent programming
language. A source program is compiled: the `voila run` command
compiles it and executes it in one step, and the `voila build` command
leaves a self-contained executable behind. Both use the same compiler
and produce the same behavior — the *Equivalence Guarantee*, discussed
in Chapter 12.

The compiler is itself written in Voilà.

The language derives its design from four ancestors:

| Ancestor | Inheritance |
|---|---|
| Go | Syntax, packages, channels, structured tasks |
| Rust | Ownership, moves, deterministic destruction |
| REXX | SAY, PARSE, decimal arithmetic, approachability |
| C | The printf formatting conventions |

## 1.2 A Complete Program

```voila
// hello.voi
package main

func main() {
    say "Hello, world!"
}
```

The same program may be written in *script form*, in which the package
declaration and the main procedure are implicit:

```voila
#!/usr/bin/env voila
say "Hello, world!"
say "2 + 2 =", 2 + 2
```

## 1.3 Guarantees

Programs accepted by the Voilà translator enjoy the following
guarantees, none of which requires programmer discipline:

1. **No dangling references.** Borrows cannot escape the scope that
   created them (Chapter 8).
2. **No storage leaks.** The type graph is verified acyclic; reference
   counts therefore always reach zero (Section 8.4).
3. **No data races.** A value is either shared immutably or owned by
   exactly one task (Chapter 10).
4. **No orphaned tasks.** Every task belongs to a GROUP block, which
   does not complete until all of its tasks have completed
   (Section 10.1).
5. **Exact decimal arithmetic.** The `dec` type never introduces binary
   rounding error (Section 3.3).

---

# Chapter 2. Language Elements

## 2.1 Character Set

Source programs are written in UTF-8. Identifiers may contain any
Unicode letter, decimal digits, and the underscore, and must not begin
with a digit. The language is case-sensitive.

The identifier consisting of a single underscore (`_`) is the *blank
identifier*; a value assigned to it is discarded.

**Visibility rule.** An identifier whose first letter is uppercase is
exported from its package. A lowercase identifier is package-private.

## 2.2 Comments

```voila
// line comment (to end of line)
/* block comment, /* which nests, */ unlike C */
/// documentation comment (attaches to the following declaration)
```

**Note.** `//` is *only* a comment. Floor division is the distinct
operator `~/` (Section 4.4), so no spacing rule is needed to tell them
apart.

## 2.3 Statement Termination

A statement ends at the end of a line. A semicolon is inserted
automatically at a line end when the last token of the line is one that
can end an expression: an identifier, a literal, a closing bracket
(`)`, `]`, `}`), one of the keywords `return`, `break`, `continue`,
`true`, `false`, `nil`, `self`, `say`, or the discard period of a PARSE
template. A line ending in any other token — an operator, a comma, an
opening bracket, `=>`, and so on — continues onto the next line.

Explicit semicolons are permitted and allow several statements on one
line.

**Programming note.** Place `else`, `catch`, and `finally` on the same
line as the closing brace of the preceding block, in the manner of Go.
The translator also accepts them at the start of the following line.

## 2.4 Literals

| Form | Type | Example |
|---|---|---|
| Decimal integer | int | `42`, `1_000_000` |
| Radix integer | int | `0x1F`, `0o755`, `0b1010` |
| Floating point | float | `3.14`, `6.02e23` |
| Decimal (exact) | dec | `19.99d`, `3d` |
| Rune | rune | `'A'`, `'\n'`, `'\u{1F600}'` |
| String | str | `"text"` |
| Raw string | str | `` `no \escapes` `` |
| Boolean | bool | `true`, `false` |
| Nil | ?T | `nil` |

Underscores may appear between digits of any numeric literal and are
ignored.

**String escapes:** `\n \t \r \0 \\ \" \' \{ \}` and `\u{...}` with
hexadecimal digits.

**String interpolation.** Within a string literal, braces enclose an
expression whose value is converted to string form and substituted:

```voila
say "cost: {price * qty} euro"
```

A literal left brace in an interpolatable string must be written `\{`.
Raw strings (backquotes) are never interpolated and keep newlines,
which makes them the natural carrier for JSON and other brace-rich
text.

---

# Chapter 3. Data Types and Declarations

## 3.1 Elementary Types

| Type | Description |
|---|---|
| `int` | Signed 64-bit binary integer; the default integer type |
| `i8 i16 i32 i64` | Signed integers of stated width |
| `u8 u16 u32 u64` | Unsigned integers of stated width |
| `byte` | Synonym for `u8` |
| `float` | IEEE 754 binary64; the default floating type |
| `f32` | IEEE 754 binary32 |
| `dec` | Exact decimal of arbitrary precision (Section 3.3) |
| `bool` | `true` or `false`; there is no implicit truth value |
| `rune` | One Unicode code point |
| `str` | Immutable UTF-8 character string |
| `unit` | The empty type, written `()` |

## 3.2 Composite Types

```voila
[5]int            fixed-length array (a value)
[]int             slice: growable vector
map[str]int       hash table preserving insertion order
set[str]          hash set preserving insertion order
chan[Job]         communication channel (Chapter 10)
?int              optional: an int, or nil
int!              fallible: an int, or an error value (Chapter 9)
fn(int, int) int  procedure type
(int, str)        tuple
shared[T]         shared immutable ownership (Section 8.4)
cell[T]           shared mutable ownership under a lock (Section 10.4)
weak[T]           non-owning handle; upgrade() yields ?shared[T]
```

## 3.3 Decimal Arithmetic

The `dec` type performs exact decimal arithmetic in the manner of REXX.
Results are rounded to a *significant-digits context* established by the
NUMERIC DIGITS statement (Section 5.10); the initial context is 28
digits. Rounding is round-half-even.

```voila
numeric digits 31
let price = 19.99d
say price * 3d              // 59.97 — exactly. Never 59.96999999...
```

**General rule.** A `dec` value never passes through binary floating
point. The conversion `float` → `dec` is always explicit (Section 3.5).

## 3.4 Declarations

```
   >>──LET──<name>──┬─────────────┬──=──<expression>──────────────────────><
                    └──<type>─────┘

   >>──VAR──<name>──┬─────────────┬──┬──────────────────────┬─────────────><
                    └──<type>─────┘  └──=──<expression>─────┘

   >>──CONST──<name>──=──<constant-expression>────────────────────────────><
```

A LET binding is immutable; a VAR binding may be reassigned. A VAR
declared without an initializer receives the zero value of its type:
zero for numbers, the empty string, `false`, `nil` for optionals, the
empty collection, and a structure whose fields carry their declared
defaults or zero values.

There are no uninitialized variables and no null pointers. `nil`
inhabits only the optional types `?T`.

## 3.5 Conversion Rules

An implicit conversion is performed only when it cannot lose
information; the governing rule is:

> A value of type A converts implicitly to B if and only if every value
> of A is exactly representable in B.

Thus `u16` widens to `int`, any integer widens to `dec`, and `i8`
through `i32` widen to `float`; but `i64` → `float`, `float` → `int`,
and `float` → `dec` are compile-time errors, because each can lose
information.

Explicit conversions come in three families, so the treatment of
overflow is always stated at the point of conversion:

| Family | On loss | Example |
|---|---|---|
| `int(x)`, `u8(x)`, `dec(x)`… | signals ConvError | `int("42")` → 42 |
| `to_int(x)`, `to_u8(x)`… | yields `nil` | `to_u8(300)` → nil |
| `wrap_u8(x)` / `sat_u8(x)` | wraps / saturates | `wrap_u8(300)` → 44 |

`int(3.9)` truncates toward zero by definition (documented, not a
loss). `int(s, 16)` converts with the stated radix. `str(x)` renders
any value. Arithmetic overflow of the fixed-width integers signals
OverflowError; it never wraps silently (`wrap_add`, `wrap_mul` wrap on
request).

---

# Chapter 4. Expressions and Operators

## 4.1 Operator Precedence

From highest (bound first) to lowest:

| Level | Operators | Association |
|---|---|---|
| 11 | `x.f  x[i]  x(...)` | left |
| 10 | unary `-  not  ~  <-` | right |
| 9 | `**` (power) | right |
| 8 | `*  /  ~/  %` | left |
| 7 | `+  -` | left |
| 6 | `<<  >>  &  \|  ^` | left |
| 5.5 | `..  ..=` (range) | — |
| 5 | `\|\|` (concatenation) | left |
| 4 | `<  <=  >  >=  ==  !=  in` | none |
| 3 | `and` | left |
| 2 | `or` | left |
| 1.5 | `else` (default-on-failure, Section 9.2) | right |
| 1 | `=  +=  -=  *=  /=  %=  \|\|=` | statement |

## 4.2 Arithmetic Rules

1. `/` applied to two integers performs *exact division* and produces a
   `float`: `7 / 2` is `3.5`.
2. `~/` is floor division and produces an integer: `17~/5` is `3`,
   `-17~/5` is `-4`.
3. `%` is the floor remainder, paired with `~/`: the identity
   `a == (a~/b)*b + a%b` holds for all signs.
4. `**` is exponentiation; `2 ** 10` is `1024`. A negative integer
   exponent of an integer base signals RangeError.
5. Mixed operands widen by the rule of Section 3.5: `int` with `dec`
   computes in `dec`; `int` with `float` computes in `float` provided
   the integer is exactly representable, and signals ConvError
   otherwise.

## 4.3 String Operators

`||` concatenates, converting both operands to string form. The `+`
operator **never** concatenates. `x in s` tests substring containment.

## 4.4 Floor Division

Floor division is the operator `~/`. It rounds the quotient toward
negative infinity and produces an integer:

```voila
17 ~/ 5        //  3
-17 ~/ 5       // -4
```

`~/` binds at the multiplicative level (Section 4.1) and is
left-associative. Its spelling is deliberately unrelated to the comment
marker `//`, which never divides, so spacing around `~/` is immaterial:
`a~/b`, `a ~/b`, and `a ~/ b` are identical. See Deviation D.1 for the
history of this choice.

## 4.5 Ranges

```voila
0..10          the integers 0 through 9
0..=10         the integers 0 through 10
0..10 by 2     0, 2, 4, 6, 8
```

A range is a first-class value: it may be iterated, tested with `in`,
and used to slice (Section 5.4).

---

# Chapter 5. Statements

## 5.1 SAY

```
                 ┌─,──────────────┐
   >>──SAY───────▼──<expression>──┴──────────────────────────────────────><
```

The SAY statement converts each expression to string form, joins the
results with single blanks, appends a newline, and transmits the line
to the standard output stream. SAY with no operands transmits a blank
line. If the type of an operand carries a user implementation of the
`Show` trait, that implementation supplies the string form.

## 5.2 PARSE

```
   >>──PARSE──┬─────────┬──┬──────┬──<source>──<template>────────────────><
              ├──UPPER──┤  └─VAR──┘
              └──LOWER──┘

   <template> is a sequence of:
       <name>       capture a field into a variable
       .            capture and discard
       "literal"    literal separator pattern
       <integer>    absolute column position (1-based)
```

PARSE performs template-directed decomposition of a string, in the
REXX tradition. UPPER (LOWER) folds the source to uppercase (lowercase)
before decomposition. Targets not previously declared are declared
implicitly as `var … str` in the current scope; parsing into a LET
binding is an error.

**General rules.**

1. A literal pattern divides the source at its next occurrence; the
   text before the occurrence forms the current field, and scanning
   resumes after the pattern.
2. A column pattern divides the source at the stated 1-based byte
   position; positions are clamped to the source length.
3. Within a field, word targets split on blanks; the final target
   receives the remainder unsplit. A final `.` therefore reduces the
   preceding target to a single word while absorbing the tail.
4. When a template *begins* with a literal pattern, the pattern is the
   terminator of the first field (specification usage):
   `parse line "," a "," b` assigns the first comma-field to `a`.

**Examples.**

```voila
parse line "," name "," age "," city
parse text word1 word2 rest .
parse record 1 recid 9 acct 21 amount .        // fixed-width records
parse upper cmd verb args .
```

## 5.3 IF

```
   >>──IF──<condition>──<block>──┬──────────────────────────────┬────────><
                                 └──ELSE──┬──<block>──────┬─────┘
                                          └──<if-statement>┘
```

The condition must be of type `bool`; there is no implicit truth value.
Braces are mandatory; parentheses around the condition are not written.

The form `if let <pattern> = <expression>` executes the block when the
pattern matches, with the pattern's bindings in scope (Section 7.2).

## 5.4 Loops; Indexing and Slicing

```voila
for i in 0..10        { ... }        // range iteration
each x in v           { ... }        // values of a collection
each i, x in v        { ... }        // index and value
each k, v in m        { ... }        // key and value of a map
while cond            { ... }
for                   { ... }        // indefinite; leave with break
```

Loops may be labeled; BREAK and CONTINUE accept an optional label.

Indexing `v[i]` checks bounds; a violation is an *abort* (Section 9.5).
Slicing `s[a..b]` verifies the range and *signals* RangeError on
violation, permitting recovery. Indexing a map with an absent key
yields `nil`.

## 5.5 SWITCH

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

There is no implicit fall-through; the statement `fallthrough` requests
it explicitly. For values of enumeration type, prefer MATCH.

## 5.6 MATCH

```voila
match s {
    Circle(r)  => return 3.14159 * r * r
    Rect(w, h) => return w * h
    Empty      => return 0.0
}
```

MATCH must be exhaustive; the translator names any missing cases. An
arm consists of a pattern, an optional guard (`if <condition>`), the
arrow `=>`, and one statement or a block. Patterns are literals, `_`,
`nil`, binding identifiers (lowercase), and variant patterns
(`Some(v)`, `Ok(v)`, `Err(e)`, and user enumeration variants).

## 5.7 DEFER and DROP

DEFER schedules a call or block to run when the enclosing procedure
returns, in last-in first-out order, on every exit path including
exception unwinding — but not on aborts (Section 9.5).

DROP destroys a binding before the end of its scope; later use of the
binding is a use-after-move error.

## 5.8 THROW and TRY

See Chapter 9.

## 5.9 GROUP, SPAWN, and SELECT

See Chapter 10.

## 5.10 NUMERIC DIGITS

```
   >>──NUMERIC──DIGITS──<integer>─────────────────────────────────────────><
```

Establishes the significant-digit context for `dec` arithmetic. The
context is restored when the enclosing block completes, including
completion by unwinding. Procedures called within the block compute
under the caller's context; a spawned task snapshots the context in
force at SPAWN. (This dynamic-activation reading follows the REXX
heritage; see Deviation D.4.)

---

# Chapter 6. Procedures

## 6.1 Procedure Declaration

```
   >>──FUNC──<name>──(──<parameters>──)──┬────────────┬──────────────────>
                                         └──<result>──┘
   >──┬──────────────────────────────┬──<block>───────────────────────────><
      ├──THROWS──<exception-list>────┤
      └──NOTHROW─────────────────────┘
```

```voila
func add(a int, b int) int  { return a + b }
func add(a, b int) int      { return a + b }         // shared type
func divmod(a, b int) (int, int) { return a~/b, a % b }
func sum(nums ...int) int   { ... }                  // variadic
func greet(name str, greeting str = "Hello") str     // default value
```

Arguments may be passed by position, by name (`greeting: "Ciao"`), or
spread from a slice (`sum(v...)`). Multiple results are returned as a
tuple and received by a destructuring LET.

## 6.2 Parameter Passing and Ownership

Ownership annotations appear on **declarations only**; call sites are
always plain `f(x)`.

| Declaration | Meaning |
|---|---|
| `func f(v []int)` | borrow: read-only access for the call's duration |
| `func f(v mut []int)` | mutable borrow: exclusive for the duration |
| `func f(own v []int)` | move: the callee takes ownership |

After an argument is passed to an `own` parameter, the caller's binding
is dead; further use is diagnosed (Section 8.2).

## 6.3 Closures

```voila
let double = fn(x int) int { return x * 2 }
nums.map(fn(x) { x * 2 })            // last expression is the value
let f = fn own [data] () { ... }     // capture list: move data in
```

A closure captures its free variables by reference. When the closure
must outlive the enclosing scope, the captured values are moved in
explicitly with a capture list.

## 6.4 Methods

```voila
impl Point {
    func dist(self, other Point) float { ... }   // borrows self
    func shift(mut self, dx float)     { ... }   // mutates self
    func into_pair(own self) (float, float)      // consumes self
    func origin() Point { return Point{0, 0} }   // associated function
}
```

---

# Chapter 7. Aggregates: Structures, Enumerations, Traits

## 7.1 Structures

```voila
struct Server {
    host   str
    port   int = 8080          // field default
    routes map[str]Handler
}

let s  = Server{host: "localhost"}      // port defaults to 8080
let p  = Point{x: 1.0, y: 2.0}          // by name
let p2 = Point{1.0, 2.0}                // by position
```

Field separators may be newlines, semicolons, or commas. Structures
whose fields all have Copy semantics are copied on assignment; all
other structures move (Section 8.2).

## 7.2 Enumerations

```voila
enum Shape {
    Circle(radius float)
    Rect(w float, h float)
    Empty
}
```

An enumeration value is constructed by calling the variant
(`Circle(2.0)`) or naming a payload-less variant (`Empty`). MATCH
destructures it (Section 5.6). The optional and fallible types are
enumerations in disguise: `?T` ≡ `Some(T) | nil`, `T!` ≡ `Ok(T) |
Err(error)`.

## 7.3 Traits

```voila
trait Show {
    func show(self) str
}

impl Show for Point {
    func show(self) str { return "({self.x}, {self.y})" }
}

func print_all(items []Show) {          // dynamic dispatch
    each it in items { say it.show() }
}
```

A trait is implemented explicitly with an IMPL block. Any type
implementing a trait may be passed where the trait is expected. SAY and
string interpolation consult a `Show` implementation when present.

## 7.4 Generic Declarations

```voila
func max[T: Ord](a T, b T) T { ... }

struct Stack[T] { items []T }

impl[T] Stack[T] {
    func push(mut self, own v T) { self.items.append(v) }
    func pop(mut self) ?T        { return self.items.pop() }
}
```

Constraints name traits. The translator erases type parameters and the
runtime checks representations; a later monomorphizing pass will change
the cost, not the observable behavior.

---

# Chapter 8. Storage Management and Ownership

## 8.1 The Model

Voilà has neither a garbage collector nor a `free` operation. Every
value has exactly one owner. When the owner's scope completes, the
value is destroyed, deterministically, in reverse declaration order.

## 8.2 Copy and Move

Types whose values occupy no owned storage — the elementary types, and
structures, tuples and enumerations composed only of such types — have
**Copy** semantics: assignment copies. Every other value **moves** on
assignment, on passing to an `own` parameter, and on sending into a
channel. A moved-from binding is dead:

```voila
var data = [1, 2, 3]
consume(data)                 // own parameter: data moves
say data                      // ERROR: use of moved value `data`
```

An explicit deep copy is always available as `x.clone()`.

## 8.3 Borrows Do Not Escape

A borrow (the default parameter mode) may not be stored in a structure
field, a channel, a global, or a closure that outlives it. The
translator rejects `struct Bad { p &Point }` outright. Because borrows
cannot escape, they cannot dangle, and no lifetime annotations are
needed anywhere in the language.

## 8.4 Shared Ownership and the Acyclicity Rule

```voila
shared[T]     reference-counted, deeply immutable
cell[T]       reference-counted, mutable under an internal lock
weak[T]       non-owning; upgrade() yields ?shared[T]
```

Reference counting leaks when cycles form. Voilà makes owning cycles
unrepresentable:

> **The Acyclicity Rule.** Following owning edges only — fields,
> `shared`, `cell`, and collection elements — the type graph must be a
> directed acyclic graph. A type that reaches itself through a scalar
> owning edge is rejected at translation time. Self-reference is legal
> through `weak[T]`, and through collection edges, which are tree-
> shaped by construction.

```voila
struct Node {
    value  int
    kids   []shared[Node]     // tree-shaped: permitted
    parent weak[Node]         // back-edge: must be weak
}
```

Writing `parent shared[Node]` produces:

```
error: type `Node` forms an ownership cycle through field `parent`
  --> tree.voi:4:5
   |
 4 |     parent shared[Node]
   |            ^^^^^^^^^^^^ owning edge back to `Node`
   = note: owning cycles can leak; Voilà forbids them
   = help: use `weak[Node]` for a back-edge
```

**Consequence.** Reference counts always reach zero; a Voilà program
cannot leak storage, whatever exceptions or early returns occur.

---

# Chapter 9. Condition Handling

Voilà provides two condition mechanisms with an explicit boundary
between them, plus a third category that is not a condition at all.

| Mechanism | Use when | Cost |
|---|---|---|
| Error values, `T!` | the immediate caller can plausibly recover | none: a return value |
| Exceptions | recovery belongs several frames up | none on the normal path |
| Aborts | the program is wrong | the process ends |

## 9.1 Error Values

A procedure whose result type is `T!` returns either a T or an error
value made by `err(message)` / `errf(format, ...)`.

```voila
func parse_port(s str) int! {
    let n = try to_int(s) else return err("not a number: {s}")
    if n < 1 or n > 65535 { return err("port out of range: {n}") }
    return n
}
```

## 9.2 Consuming a Fallible Result

```voila
let p = try parse_port(s)            // propagate to the caller
let p = parse_port(s) else 8080      // supply a default
match parse_port(s) {                // handle both arms
    Ok(p)  => say "port", p
    Err(e) => say "bad:", e.message()
}
```

`try e` propagates failure (an error value, or `nil` from an optional)
out of the enclosing procedure. The `else` operator supplies a default;
its right operand may instead be a diverging statement (`return`,
`throw`, `break`, `continue`).

## 9.3 Exceptions

An EXCEPTION declaration introduces an exception type: a structure plus
a derived `message()`. A user `message()` may be supplied in the body.

```voila
exception NotFound  { path str }
exception ParseError {
    line int
    col  int
    func message(self) str { return "parse error at {self.line}:{self.col}" }
}
```

```
   >>──THROW──<expression>──┬────────────────────────┬───────────────────><
                            └──FROM──<expression>────┘
```

THROW with no operand, within a CATCH block, rethrows the caught
exception. THROW ... FROM e records e as the `cause()`.

```voila
try {
    run(load("/etc/app.conf"))
} catch e: NotFound {                 // typed clause, tested in order
    run(defaults())
} catch e: Timeout, NetError {        // several types, one clause
    say "network trouble:", e.message()
} catch e {                           // catch-all
    say "fatal:", e.message()
    each frame in e.trace() { say "   ", frame }
} finally {
    log.flush()                       // every path, including unwinding
}
```

**Unwinding is safe.** As an exception unwinds a frame, that frame's
DEFERs run in LIFO order and its owned values are destroyed. An
exception can therefore neither leak storage nor leave a file open.

## 9.4 Bridging the Mechanisms

```voila
let n = must parse_port(s)      // T! → exception: throws the Err
let r = attempt load(path)      // exception → T!: catches any throw
```

Either style of library is usable in either style of caller, in one
word, at the call site.

Exceptions are unchecked by default. A procedure may opt into rigor
with `throws E1, E2` (the translator verifies every callee) or with
`nothrow` (the translator proves no exception escapes; required for
destructors, `cell.update` closures, and foreign callbacks).

## 9.5 Aborts

An assertion failure, an index out of bounds, an exception escaping a
`nothrow` procedure, or an unhandled exception reaching the top of the
program is an **abort**: the process prints a stack trace and ends. An
abort is not catchable, and DEFERs do not run: the correct response to
a wrong program is to stop, not to limp on. `os.on_abort(f)` may
register one last-gasp handler (flush logs, notify a supervisor).

---

# Chapter 10. Concurrent Processing

## 10.1 GROUP and SPAWN

```
   >>──┬───────┬──GROUP──┬───────────────────────────┬──<block>──────────><
       └──TRY──┘         └──TIMEOUT──<duration>──────┘
```

SPAWN starts a task — a green thread costing a few kilobytes — inside
the innermost GROUP. **The GROUP block does not complete until every
task spawned within it has completed.** No task outlives its group;
there is no orphaned-goroutine failure mode.

```voila
group {
    spawn worker(1)
    spawn worker(2)
}                       // joins here
say "all workers done"  // provably nothing still running
```

SPAWN yields a `Task[T]`; `.await()` joins one task and yields its
result.

## 10.2 Failure Semantics

When a task fails — by exception, or by an error value propagating out
of it — the group cancels its remaining tasks cooperatively, joins them
all, and re-raises the first failure at the group boundary; later
failures are attached as `e.suppressed()`. `try group { ... }` converts
the delivered failure into an error value. A GROUP TIMEOUT that expires
delivers `Timeout`.

Cancellation is delivered through each task's context: blocking channel
operations and `time.sleep` observe it and raise `Cancelled` inside the
task, so a group can always reap a blocked task.

## 10.3 Channels and SELECT

```voila
let ch = chan[int](0)          // unbuffered: rendezvous
let ch = chan[Job](100)        // buffered
ch <- job                      // send — MOVES the value
let job = <-ch                 // receive
let job, ok = <-ch             // ok is false when closed and drained
close(ch)
each job in ch { ... }         // iterate until closed
```

Sending moves ownership into the channel: the sender's binding is dead
afterward. Communication is therefore race-free without copying.

```voila
select {
case job := <-jobs:      process(job)
case results <- value:   say "sent"
case <-time.after(5 * time.Second): say "timeout"
default:                 say "nothing ready"
}
```

## 10.4 Shared Mutable State

```voila
let counter = cell[int](0)
group {
    for i in 0..1000 {
        spawn { counter.update(fn(n) { return n + 1 }) }
    }
}
say counter.get()              // 1000, guaranteed
```

The closure passed to `update` runs under the cell's lock and must not
allow an exception to escape (the nothrow requirement of Section 9.4);
a violation is an abort.

---

# Chapter 11. The Standard Library

## 11.1 The Prelude

Visible without any USE declaration: `say`, `printf`, `sprintf`, `len`,
`str`, the conversion families of Section 3.5, `append`, `make`,
`close`, `min`, `max`, `abs`, `sum`, `avg`, `round`, `floor`, `ceil`,
`trunc`, `round_to`, `err`, `errf`, `assert`, `range`, `zip`,
`enumerate`, `keys`, `values`, `shared`, `cell`, `weak`, `set`, and the
common string helpers (`trim`, `pad`, `pad_left`, `center`, `upper`,
`lower`, `split`, `join`, ...).

## 11.2 std/fmt

`fmt.printf` follows the C conventions; the verbs are summarized in
Appendix B. A literal format string is verified at translation time; a
computed one is verified at run time (FormatError). `%f` applied to a
`dec` rounds half-even exactly: `%.2f` of `19.995d` is `20.00` on every
platform. Also provided: `sprintf`, `eprintf`, `fprintf(file, ...)`,
`print`, `println`, `pad`, `pad_left`, `center`, `comma` (1,234,567),
`bytes` (1.4 MiB), `table`.

## 11.3 std/str

Every function is Unicode-correct and returns a new string. Indices are
0-based byte offsets (see Deviation D.2). Functions are callable both
ways: `str.upper(s)` and `s.upper()` are the same call.

Case/space: `upper lower title trim trim_left trim_right strip pad
pad_left center`. Search: `contains index last_index starts_with
ends_with count equal_fold`. Split: `substr left right split split_n
lines fields word words subword delword`. Build: `join replace
replace_n repeat reverse translate overlay insert delstr`. Convert:
`bytes from_bytes runes from_runes rune_len is_digit is_alpha is_space
is_upper hex unhex b64 unb64`. Efficient building: `str.Builder{}` with
`.write(x)` and `.done()`.

## 11.4 Other Packages

| Package | Contents |
|---|---|
| std/os | `read write open create exists remove args env exit stderr stdout on_abort`; File: `lines() records(n) read_all() write() close()` |
| std/math | `pi e sqrt sin cos tan ln log2 log10 exp pow hypot mod` |
| std/time | `now since sleep after millis seconds`; `Second`, `Millisecond`, ... durations with integer arithmetic |
| std/json | `encode pretty decode decode[T]` — numbers with fractions decode as `dec` |
| std/log | `info warn error debug` to the standard error stream |
| std/regex | RE2 semantics (linear time): `compile matches find find_all replace`; compiled `Regex` methods incl. `groups` |
| std/http | `get post` returning `Response{status, body, headers}` |
| std/conv | the conversion functions, package-qualified |
| std/sort | `sort sort_by reversed` |
| std/rand, std/uuid | `rand.int(n) rand.float rand.seed(n)`; `uuid.new()` |

Collections carry methods directly: slices have `append pop first last
map filter reduce sort sort_by sorted contains index_of reverse join
any all each`; maps have `keys values has get set delete`; sets have
`add remove has items`.

---

## 11.5 Packages

A program may be divided into packages. **One directory is one package.**

```
   >>──USE──<"import-path">──┬──────────────────┬───────────────────────><
                             └──AS──<qualifier>─┘
```

The *module root* is the nearest ancestor directory containing a `voila.mod`
file; failing that, it is the directory of the program's entry file. An import
path that resolves to a directory under the module root names a **user
package**; the paths beginning `std/` (and the bare names `fmt`, `str`, `os`,
…) name the **standard packages** of Chapter 11.

```voila
// voila.mod:  module app
use "app/geom"            // the directory <root>/geom
use "app/geom" as g       // ...under another qualifier
```

**General rules.**

1. Every `.voi` file in the directory belongs to the package; they see one
   another's declarations without qualification.
2. A declaration is **exported** when its name begins with a capital letter,
   and only exported names may be reached from another package
   (Section 2.1). A package-private name reached from outside is an error.
3. Members are reached through the package qualifier: `geom.Area(x)`,
   `geom.Box{…}`, `catch e: geom.BadShape`.
4. **Import cycles are rejected.** So are two packages with the same name, an
   import path that resolves to nothing, and an import path that escapes the
   module root.
5. Enumeration VARIANT names and TRAIT names are **global to the program**,
   because a pattern and a trait bound have no qualified form. Two packages
   may therefore not both declare a variant `Empty` or a trait `Show`; the
   translator says so.
6. A package file may not contain loose statements; only the entry file may
   (Section 1.2).
7. Selective (`use … { A, B }`) and wildcard (`use … *`) imports are not
   available for user packages.

---

# Chapter 12. Compiling and Running Programs

## 12.1 Commands

```
voila run   <file.voi> [args...]    compile (cached) and execute
voila run -O2 <file.voi> [args...]  the same, optimized (12.5)
voila build <file.voi> -o <name>    produce a native executable
voila build -O1|-O2|-O3 <file.voi>  optimize the executable (12.5)
voila build --native <file.voi>     additionally tune for THIS machine
voila build -S <file.voi>           produce an assembly listing (12.4)
voila build --emit=c <file.voi>     produce the C translation unit
voila check <file.voi>              translate and verify only
voila version                       print the toolchain version
```

## 12.2 How a Program Is Compiled

Voilà is a compiled language. The toolchain is a compiler — written in
Voilà — and a runtime library written in C:

```
   source ──lex──▶ tokens ──parse──▶ tree ──load──▶ flattened program
                                                          │
                                                        check
                                                          │
                                                        lower
                                                          ▼
                                              register IR (§12.4)
                                                          │
                                                     emit C
                                                          ▼
                                                  the system cc  ──▶  executable
```

The produced executable links libvoila and the C library. It contains
no compiler, no interpreter and no virtual machine; `otool -L` or `ldd`
shows nothing but libc.

## 12.3 The Equivalence Guarantee

A program behaves identically however it is run. The guarantee is kept
in the simplest way available: **there is only one engine.** `voila run`
compiles the program and executes it — it does not interpret. The
compiled binary is cached under `~/.cache/voila`, keyed by a hash of
every source file that took part, so the first run of a script pays for
a compilation and every later run starts instantly. Editing any file of
any package the program imports invalidates the entry.

An earlier toolchain shipped a tree-walking interpreter beside the
compiler, and the two had to be held in agreement by testing. The
outputs that interpreter produced are preserved as the golden files in
`tests/conformance/` and `samples/golden/`, and the compiler is still
held to them on every build. The oracle is gone; its verdicts are not.

## 12.4 Assembly Listings (the -S Option)

```
   >>──VOILA BUILD──-S──<file.voi>──┬────────────────────┬────────────────><
                                    └──-o──<listing.s>───┘
```

The -S option translates and verifies the program, lowers it to the
Voilà VM's register-based linear intermediate representation (the
"register bytecode" of specification §12), and writes an assembly
listing instead of an executable. The listing is written to the
standard output stream; redirect it to keep it
(`voila build -S prog.voi > prog.lst`).

The listing is presented in the style of a mainframe assembler
SYSPRINT: a heading block, the constant pool (`DC` entries, deduplicated
and insertion-ordered), the program's structures and enumerations as
DSECT commentary, and one CSECT per procedure with a location counter,
the operation and operand fields, and the source statement echoed as a
right-hand comment whenever the source line advances. No line exceeds
79 columns; over-long operand fields continue onto the next line, the
continued line ending in `+`.

**General rules.**

1. Registers `r0, r1, ...` are virtual and allocated monotonically;
   parameters occupy the lowest registers (`self` is `r0` in methods).
   `^name` denotes an upvalue: closures capture the enclosing frame by
   reference.
2. Execution order is documented in the heading: `$INIT` (global
   initializers, in declaration order), `$SCRIPT` (script-form
   statements), then `main`. Closure, defer and spawn bodies appear as
   synthetic CSECTs named `outer$fnN`.
3. Control flow is fully lowered: `and`/`or` become short-circuit
   branches, loops become labels and jumps with cancellation polls
   (CKCANC), MATCH becomes PMATCH tests ending in ABORT, and TRY
   becomes EHPUSH/EHPOP with the FINALLY block duplicated on every exit
   edge, each copy commented with its edge.
4. The listing is not executed directly: it is the intermediate form
   the C backend consumes on the way to a native binary. Because `voila
   run` and `voila build` share this one path, the Equivalence Guarantee
   (12.2) is unaffected.

## 12.5 The Optimizer (-O1, -O2, -O3)

Optimization is a request, never a default: a build without an `-O`
option produces exactly the translation the toolchain has always
produced, byte for byte. Each level contains the ones below it.

```
-O1   Dead pure definitions are removed, unreferenced labels pruned,
      frames shrunk to the registers actually used, and retains of
      immediate constants elided. Arithmetic is NEVER removed: an
      addition can signal OVERFLOWERROR, and a signal is observable
      behavior (12.3).

-O2   Frame elision. A procedure that creates no closure and defers
      nothing keeps its registers in the machine stack frame rather
      than allocated storage. The exception mechanism is unaffected:
      an elided frame is registered for unwinding exactly as an
      allocated one. NOTE: deep recursion reaches the machine stack
      limit sooner under -O2 (the registers now occupy stack); the
      limit expresses itself as an abnormal termination, not a
      diagnostic.

-O3   Typed registers. The translator proves, per register, that only
      INT, FLOAT, or BOOL values can ever reach it, and translates
      such registers to machine-typed storage with direct arithmetic.
      The proof is conservative: procedure parameters, registers
      bound by MATCH or PARSE, registers captured by closures, and
      any procedure containing TRY are left in the general form.
      DECIMAL arithmetic (Sections 3.3, 5.10) is never converted. The C
      translation unit is additionally compiled at the C compiler's
      highest standard level.
```

**The Equivalence Guarantee extends to every level** (12.3): outputs,
signals, and signal messages are byte-identical at -O0 through -O3.
Every trap message exists once, in the runtime, and is shared by the
optimized and unoptimized forms; the toolchain's own test procedure
compiles its whole conformance suite at every level and compares.

`--native` additionally tunes the program for the processor of the
machine performing the build. The resulting executable may not run on
older processors of the same family; it is never the default.

## 12.6 Completion Codes

See Appendix C.

---

# Appendix A. Reserved Words

```
and      as       attempt  break    case     catch    chan     const
continue defer    drop     each     else     enum     exception false
finally  for      func     group    if       impl     import   in
let      match    must     mut      nil      not      nothrow  numeric
or       own      package  parse    return   say      select   self
shared   spawn    struct   switch   throw    throws   trait    true
try      type     use      var      weak     while    yield
```

Reserved for future use: `async await macro unsafe where`.
Contextual (not reserved): `fallthrough by upper lower digits timeout
fn cell`.

# Appendix B. Format Verb Summary

| Verb | Operand | Result |
|---|---|---|
| `%d` `%i` `%u` | integer, integral dec | decimal digits |
| `%f` | float, int, dec | fixed point; on dec: exact, half-even |
| `%e` `%g` | numeric | scientific / shortest |
| `%s` | any | string form (Show honored) |
| `%q` | any | quoted, escaped |
| `%x` `%X` | int, str, []byte | hexadecimal |
| `%o` `%b` | integer | octal, binary |
| `%c` | rune, int | the character |
| `%t` | bool | `true` / `false` |
| `%v` | any | natural form |
| `%%` | — | a literal % |

Flags `-` `+` `0` (blank) `#`; width and precision, either literal or
`*` (consumed from the argument list): `%[flags][width][.prec]verb`.

# Appendix C. Completion Codes

| Code | Meaning |
|---|---|
| 0 | normal completion |
| 1 | unhandled exception (with trace), abort, or an error value escaping the main procedure |
| 2 | translation failed: syntax or static-check diagnostics were issued; also command usage errors |
| n | the operand of `os.exit(n)` |

# Appendix D. Deviations from the Specification

**D.1 — Floor division.** The Version 0.1/0.2 specification spelled floor
division `//`, which also introduces a comment (§2.2); the two readings
are irreconcilable for input such as `return a // b`. As of Version 0.3
the implementation spells floor division `~/` (Section 4.4) and reserves
`//` for comments exclusively. The earlier heuristic — that `//` divided
only when it touched an operand — is withdrawn, and with it the class of
bug where a stray blank silently turned `a // b` into a comment.

**D.2 — String indices.** The specification's std/str tables inherit
REXX's 1-based positions for `substr` and `overlay` in spirit; the
implementation uses 0-based byte offsets uniformly, matching the
language's indexing. `word(s, n)` and PARSE column patterns remain
1-based, as REXX heritage.

**D.3 — Shared immutability.** The specification makes `shared[T]`
deeply immutable at creation. The implementation permits field
assignment through a shared handle during construction-time wiring; the
deep freeze is not yet performed. The Acyclicity Rule is enforced
statically in full.

**D.4 — NUMERIC DIGITS scope.** The specification says "file- or
block-scoped"; the implementation uses dynamic activation scope (a
callee inherits the caller's context), following REXX. Section 5.10
states the precise rule.

**D.5 — Weak handles.** `weak.upgrade()` succeeds while the target is
reachable. Destruction timing for shared values is not guaranteed, and
that is the only way a program could observe the difference.

**D.6 — What the compiler refuses.** Three semantics are not expressible
in the generated C, and the compiler **refuses** the program rather than
produce a binary that would behave differently from the specification:

1. a closure that captures a variable **declared inside a loop** (each
   iteration is a separate binding; a C frame is per procedure). Write a
   helper that takes the value as a parameter;
2. `parse` into a variable captured from an enclosing procedure;
3. the implicit `ctx` of a task.

Tasks are **operating-system threads**, not green threads: the semantics
of Chapter 10 hold exactly — a group joins every task, a failure cancels
the siblings, `select` and cancellation behave as specified — but the
cost model does not. A million tasks is not yet fine.

`std/regex` and `std/http` have no C implementation and are therefore
unavailable; a program that calls them is refused, not silently
mistranslated.

Values are boxed in the default translation. Under `-O3` (Section 12.5)
the toolchain converts provably-typed INT, FLOAT, and BOOL registers to
machine arithmetic and elides frame allocation; measured gains reach
40 percent on arithmetic-bound programs. Procedure parameters and
values that cross procedure boundaries remain boxed — C-class arithmetic
throughout is future work, not a present property.

**D.7 — Reference cycles through closures.** The Acyclicity Rule (Section
8.4) governs the type graph and makes owning cycles among *values*
impossible. A **closure** that captures the procedure frame in which it
is itself stored is outside that rule; the runtime detects and breaks
exactly this cycle when the procedure completes. A cycle formed
indirectly (a closure stored in a collection that the captured frame
owns) is not detected and will retain storage until the process ends.

**D.8 — Identifiers outside ASCII.** The specification says an
identifier begins with a letter. The lexer accepts any byte above 127 in
an identifier rather than consulting the Unicode letter tables, so a
non-letter such as `€` is admitted where the specification would reject
it. Identifiers in the standard library and in the compiler are ASCII.

**D.9 — Not yet implemented.** `voila get`, `voila fmt/test/doc/bench/repl`,
`std/vm`, `std/ffi`, `std/csv/xml/yaml`, `std/net/crypto`, checked `throws`
verification, and the full flow-sensitive borrow checker (a straight-line
use-after-move analysis is provided). Cross-compilation is by C: `voila build
--emit=c` produces a translation unit that any C compiler for any target can
build (see `build_voila.bash`). The unimplemented constructs parse correctly
and are diagnosed where semantics are missing.

# Appendix E. Glossary

**abort.** An uncatchable termination caused by a program defect.

**borrow.** Temporary, scope-bounded access to a value without transfer
of ownership.

**context (numeric).** The significant-digit setting governing dec
arithmetic.

**equivalence guarantee.** The property that a program behaves
identically however it is run. It is kept by construction: `voila run`
compiles and executes, so there is only one engine.

**error value.** A first-class value of type `T!` representing expected
failure; consumed by `try`, `else`, or MATCH.

**exception.** A condition raised by THROW and disposed of by the
nearest matching CATCH clause, unwinding safely.

**group.** The structured-concurrency block that owns tasks and joins
them before completing.

**move.** Transfer of ownership that leaves the source binding dead.

**task.** A lightweight thread of execution belonging to a group.

**variant.** One alternative of an enumeration, optionally carrying a
payload.

---

*End of manual. Report defects the way you would report any Voilà
defect: with the smallest program that reproduces them.*
