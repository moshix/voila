# Voilà Programming Manual

## Language Reference and Programming Guide

**Program Number 5799-VLA · Version 0, Release 4, Modification 2**

---

**Fourth Edition (July 2026)**

This edition applies to Version 0 Release 4 Modification 2 of the Voilà Language
Toolchain (self-hosted), and to all subsequent releases and modifications
until otherwise indicated in new editions. Make sure you are using the
correct edition for the level of the product.

This manual is written in the tradition of the language reference
manuals that accompanied IBM COBOL and PL/I on the mainframe: it is a
reference, not a tutorial. Statements are presented with syntax
diagrams, general rules, and usage notes. Read Chapter 1 first;
thereafter the manual is designed for random access.

**Note.** Where the behavior of the toolchain deliberately differs from
the *Voilà Language Specification, Version 0.5.0 (Draft)*, the difference
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

A trait method may supply a DEFAULT implementation, which an implementor
inherits by not writing the method. `= expr` is shorthand for the block
`{ return expr }`.

```voila
trait Titled {
    func title(self) str                    // abstract: every IMPL owes it
    func subtitle(self) str = "(none)"      // a default, short form
    func banner(self) str {                 // a default, block form
        return "[" || self.title() || " / " || self.subtitle() || "]"
    }
}

impl Titled for Page {
    func title(self) str { return self.name }   // subtitle, banner inherited
}
```

The default is compiled once and shared by every implementor; dispatch
is on the receiver's run-time type, so a default that calls
`self.title()` reaches the implementor's own `title`. Writing the method
overrides the default — including in an inherent `impl T { ... }` block,
which the flat method table cannot tell apart from a trait one. When two
traits implemented by one type default the same method name, the earlier
IMPL block wins.

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

A constraint is enforced wherever an ACTUAL type is written in place of
the parameter: a composite literal `Stack[Point]{}`, an annotation
`var s Stack[Point]`, and an explicit instantiation `max[Point](a, b)`.
The argument must provide the constraint's methods, and a method counts
wherever the program writes it — the flat method table does not tell an
inherent `impl T { ... }` apart from an `impl Trait for T`. The six
PREDECLARED constraints have no method set of their own, so nothing is
judged against one. A type argument may itself be an instantiation:
`Stack[Box[int]]`.

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

A `cell` protects one value it owns. When what needs protecting is not
inside a cell — an ordinary variable several tasks write, a pair of
values that must change together, a one-time initialisation — reach for
`std/sync` (Section 11.7): `Mutex`, `RWLock`, `Atomic`, `Once` and
`WaitGroup`.

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
| std/time | two halves. Stopwatch: `now since sleep after millis seconds`; `Second`, `Millisecond`, ... durations with integer arithmetic. Calendar: `DateTime`, `now_utc now_local date utc from_unix from_instant parse_rfc3339 is_leap_year days_in_month`. See 11.8. |
| std/json | `encode pretty decode decode[T]` — numbers with fractions decode as `dec` |
| std/csv | RFC 4180 — `decode(text) encode(recs)`, and a streaming `Reader`/`Writer` over any `io` source. A quoted field may contain commas, quotes and NEWLINES, so a record is not a line. See 11.10. |
| std/log | `info warn error debug` to the standard error stream |
| std/net | TCP, UDP and Unix-domain sockets — `listen connect listen_udp dial_udp listen_unix dial_unix resolve`; `Listener`, `Conn`, `Udp` methods. See 11.5. |
| std/path | lexical paths — `base dir split join ext stem with_ext clean is_abs abs rel has_prefix`, and the `SEP` constant. `join` cleans its result, so `join(dir(p), base(p))` is `clean(p)` for every p. Nothing here touches the disk except `abs`. |
| std/sync | `mutex() rwlock() atomic(n) once() wait_group()` — mutual exclusion, read/write locks, atomic integers, run-once and work counting. See 11.7. |
| std/regex | RE2 semantics (linear time): `compile matches find find_all replace split`; a compiled `Regex` carries the same methods plus `source`. No backreferences and no lookaround — neither is expressible in a finite automaton, and supporting them would forfeit the linear-time guarantee. Note `{n}` inside a Voilà string literal is interpolation: write `\{n\}`. |
| std/http | HTTP/1.1 client and server — `get(url) post(url, body) request(method, url, headers, body)` returning a `Response{status, reason, proto, headers, body}`; `listen(addr)` and `serve_one(handler)` for a server; `parse_url`, `Headers` (case-insensitive), chunked decoding. **No TLS**: an `https://` URL is refused. See 11.12. |
| std/conv | the conversion functions, package-qualified |
| std/sort | `sort sort_by reversed` |
| std/rand, std/uuid | `rand.int(n) rand.float rand.seed(n)` — a fast, SEEDABLE, reproducible sequence, predictable and **not for secrets**; `uuid.new()`, which draws from std/crypto instead |
| std/crypto | `sha256 sha512` and the legacy `sha1 md5`, each also as `_raw`; `hmac_sha256 hmac_sha512 hmac_sha1 hmac_md5` (+`_raw`); `random(n) random_int(n) equal(a, b)`. `crypto.random` is the only random source fit for a key or a token. See 11.11. |
| std/test | assertions and table tests — `suite(name)`, then `eq ne near ok not_ok raises raises_msg fail table run`, and `summary report done`. See 11.9. |

Collections carry methods directly: slices have `append pop first last
map filter reduce sort sort_by sorted contains index_of reverse join
any all each`; maps have `keys values has get set delete`; sets have
`add remove has items`.

---

## 11.5 The std/net Package

`std/net` is Voilà's networking package: TCP and UDP over IPv4 and IPv6, and
Unix-domain sockets for local inter-process communication. It is the first
standard package **written in Voilà**: only the raw system calls are in the C
runtime, and the interface the program uses is Voilà source that `use
"std/net"` compiles into the program (11.6, and 12.8 for the pattern).

### 11.5.1 The Connection Model

A program opens a connection in one of two roles. A **server** creates a
`Listener` and repeatedly ACCEPTs connections; each accepted connection is a
`Conn`. A **client** CONNECTs and receives a `Conn` directly. A `Conn` is a
byte stream in both directions.

```
   >>──NET.LISTEN──(──"host:port"──)──────────────────────────────><
   >>──NET.CONNECT──(──"host:port"──)─────────────────────────────><
```

An address is `"host:port"`. The host may be a name (`"example.com"`), an
IPv4 literal (`"127.0.0.1"`), or a bracketed IPv6 literal (`"[::1]"`); an
empty host (`":8080"`) means every local interface. Binding **port 0** asks
the operating system to choose a free port, which the `Listener` then reports
through `addr()` — the idiom for a self-contained server.

Because a task is an operating-system thread (Chapter 10), the natural design
is **one task per connection**: ACCEPT in a loop and SPAWN a task to serve
each `Conn`.

```voila
let server = must net.listen(":8080")
group {
    while true {
        let conn = must server.accept()
        spawn handle(conn)
    }
}
```

### 11.5.2 Constructors

| Function | Result | Purpose |
|---|---|---|
| `net.listen(addr)` | `Listener` | a TCP listener |
| `net.connect(addr)` | `Conn` | a TCP client connection |
| `net.listen_udp(addr)` | `Udp` | a bound UDP socket |
| `net.dial_udp(addr)` | `Udp` | a UDP socket connected to one peer |
| `net.listen_unix(path)` | `Listener` | a Unix-domain stream listener |
| `net.dial_unix(path)` | `Conn` | a Unix-domain stream client |
| `net.resolve(host)` | `[]str` | the host's IP addresses (DNS) |

Each may fail, returning an `IOError` value; see 11.5.5.

### 11.5.3 Methods

**`Listener`** — `accept()` (blocks for the next connection), `addr()` (the
bound `"host:port"`), `set_timeout(ms)`, `close()`.

**`Conn`** — `read(max)` (up to `max` bytes; `""` at end of stream),
`read_line()` (through the next newline, or to end of stream), `read_all()`
(to end of stream), `write(data)` (returns bytes written), `write_all(data)`
(all of it, or an error), `local_addr()`, `remote_addr()`,
`set_timeout(ms)`, `close()`.

**`Udp`** — `send_to(data, addr)`, `recv_from(max)` (returns
`(data, sender)`), `send(data)` / `recv(max)` (for a `dial_udp` socket),
`addr()`, `set_timeout(ms)`, `close()`.

A socket's descriptor is closed automatically when the last reference to it
is dropped; `close()` is for closing it earlier, and is idempotent.

### 11.5.4 Blocking and Timeouts

Every read and ACCEPT **blocks** the calling task's thread until data
arrives or the connection ends. A blocked socket call is **not** interrupted
by a GROUP timeout (Chapter 10) — the group's cancellation is observed only
at the language's own blocking points, not inside a system call. Where a
socket must not block indefinitely, give it a deadline with
`set_timeout(ms)`; a read or ACCEPT that then exceeds the deadline fails with
an `IOError` whose message is `timed out`. `set_timeout(0)` clears the
deadline. This is recorded as deviation D.6.

### 11.5.5 Errors

Every fallible operation follows the standard two-mechanism model (Chapter
8): it returns the value on success, or an `IOError` value on failure.

```voila
match net.connect("127.0.0.1:80") {
    Ok(conn) => serve(conn)
    Err(e)   => say "cannot connect:", e.message()
}
```

`try` propagates such a failure to the caller; `must` turns it into a thrown
exception. A malformed address (`net.connect("nonsense")`) fails the same
way, as an `IOError` — it is never a program-ending abort.

---

## 11.6 Packages

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
use "std/str" as s        // a standard package takes an alias too
```

An alias **adds** a spelling to a standard package rather than replacing one:
a std qualifier is known program-wide, so `s.upper(x)` and `str.upper(x)` are
the same call and both compile. A user package has no such second spelling —
`g.Area(x)` is the only way to reach it once it is aliased.

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
5. Enumeration VARIANT names and TRAIT names are **qualified like every other
   declaration**: `geom.Circle`, `geom.Named`. Two packages may each declare an
   `Empty` or a `Named` without colliding. They are also the only names an
   importer may spell **without** a qualifier — an unqualified variant or trait
   resolves in the current package, then in exactly one import, and two
   candidates is an error naming both. The qualified form always works and is
   how the ambiguity is settled.

   ```voila
   match s {
       geom.Circle(r) => …      // always unambiguous
       Rect(w, h)     => …      // short form: only geom offers it
   }

   impl geom.Named for Box { … }
   func show[T: geom.Named](x T) str { return x.Name() }
   ```
6. A package file may not contain loose statements; only the entry file may
   (Section 1.2).
7. Selective (`use … { A, B }`) and wildcard (`use … *`) imports are not
   available for user packages.

**Depending on code you did not write.** `voila.mod` takes four directives and
nothing else — `module` (exactly one), `voila <version>` (the language version
the module needs; a version newer than the translator is refused), `require
<modpath> <version>`, and `replace <modpath> [<version>] => <localpath>` or
`=> <modpath> <version>`. `require` and `replace` also take a parenthesised
block. An unknown directive is an **error**, not something quietly skipped.

An import path is resolved in a fixed order: a Voilà-source standard package, a
C standard package, a `replace` target, the module cache, a directory under the
module root — and then a refusal that names every one of those places.

```
   voila get <modpath>@<version>    fetch into the module cache (shells out to git)
   voila mod init [modpath]         write a voila.mod
   voila mod verify                 re-hash the cached dependencies
```

The module cache is `$VOILA_MODCACHE`, defaulting to
`~/.cache/voila/mod/<modpath>@<version>/`; a cached dependency is loaded and
flattened from source exactly like a local one. Where two requirements name one
module path, the **maximum** of the required versions is selected — for a graph
of exact requirements that is minimum version selection, and it needs no
backtracking. A `replace` applies **only from the main module**, never
transitively; that is the one rule that stays predictable as a graph deepens.

`voila.sum` records one `<modpath> <version> <hash>` line per dependency. It is
a **drift detector, not a security boundary**: the hash is FNV-1a-64, which is
not cryptographic, and it is written `f1a64:…` so a future real digest can be
told apart. It catches a cached tree edited or truncated underfoot; it does not
resist a deliberate collision. A module with no `voila.sum` is not checked; once
the file exists a missing entry is as much a refusal as a wrong one. A `replace`
to a local directory is never hashed.

---

## 11.7 The std/sync Package

Tasks are real operating-system threads (Chapter 10), so two of them running
at once are genuinely running at once. `cell` (10.4) protects one value it
owns; `std/sync` protects everything else.

```voila
use "std/sync"

let mu = sync.mutex()
var total = 0

let bump = fn() {
    for i in 0..500 {
        mu.with(fn() { total = total + 1 })
    }
}
group { for t in 0..8 { spawn bump() } }
say total                      // 4000, exactly, every time
```

### 11.7.1 The Five Types

| Type | Made by | Methods |
|---|---|---|
| `Mutex` | `sync.mutex()` | `lock unlock try_lock with(f)` |
| `RWLock` | `sync.rwlock()` | `read_lock read_unlock write_lock write_unlock try_read_lock try_write_lock with_read(f) with_write(f)` |
| `Atomic` | `sync.atomic(n)` | `load store add sub inc dec swap cas` |
| `Once` | `sync.once()` | `do(f) has_run` |
| `WaitGroup` | `sync.wait_group()` | `add done wait count` (single-use) |

A `Mutex` is **not recursive**: a task that already holds it and asks again
gets a `RuntimeError` rather than a deadlock, and so does a task that unlocks
one it does not hold. An `RWLock` admits any number of readers *or* one
writer; POSIX makes no such promise about re-entering one, so do not.

`Atomic` holds one integer. Every operation is sequentially consistent, and
`add` returns the NEW value — which is what makes "was I the one who brought
it to zero" answerable without a lock.

### 11.7.2 Locks and DEFER

`defer mu.unlock()` on the line after `mu.lock()` is a requirement, not a
habit. A critical section can be left by `return`, by `break`, and by a throw —
and the throw need not be one you wrote. Cooperative cancellation (Chapter 10)
raises `Cancelled` at every loop backedge, at every channel operation, and
inside `time.sleep`, so a group timeout or a sibling task's failure can unwind
a task out of the middle of a critical section. Without `defer` the lock is
then held by a task that no longer exists and every other task waits on it
forever: a hang, not a diagnostic. The runtime runs a frame's defers on the
**unwind** path as well as the return path, which is what makes the one line
enough.

**`defer` is function-scoped, not block-scoped.** Inside a loop body it does
not unlock at the end of the iteration; it queues an unlock for when the
*function* returns, so the next turn round the loop asks for a lock this task
already holds — which a Voilà `Mutex` reports as a `RuntimeError` instead of
deadlocking. Use `with` for a per-iteration critical section.

`mu.with(f)` is the same thing said once instead of twice, and returns whatever
`f` returns:

```voila
let n = mu.with(fn() { return queue.len() })
```

Going the other way: a task **already blocked** in `lock()` is inside a system
wait that cooperative cancellation cannot reach, so a group timeout does not
interrupt it — the same rule `net.accept` follows (11.5.4). `WaitGroup.wait` is
the deliberate exception: it waits on a channel, and cancellation does reach
those.

Four calls run a closure with a lock held — `Mutex.with`, `RWLock.with_read`,
`RWLock.with_write` and `Once.do`. Inside one of them:

- a **throw** is safe; the lock is released on the way out;
- **re-entering the same Mutex** is a `RuntimeError`, not a hang;
- **blocking** (a channel receive, an `await`, a socket read) is permitted but
  holds the lock for as long as it takes — keep critical sections short;
- **unlocking the lock you are running under** is an error, because the
  `defer` that follows would then unlock a lock this task no longer holds.

### 11.7.3 Once and WaitGroup

`once.do(f)` runs `f` the first time and never again, and does not return until
`f` has finished — so a second task waits for the first one's work rather than
racing past it. If `f` throws, the exception propagates and the `Once` stays
**unrun**, so a later `do` tries again: initialisation that failed did not
happen.

`WaitGroup` counts unfinished work down to zero. `add` before spawning, `done`
when a worker finishes, `wait` to block until the count reaches zero — and
`wait` observes a group's cancellation and its timeout, because it is built on
a channel and not on a raw condition variable.

A `WaitGroup` is **single-use**, and the rule that follows from it is: count
*all* the work with one `add` before any of it can finish. One `add` per item,
interleaved with the work, lets the count touch zero while items remain, and a
`WaitGroup` whose count has reached zero is used up — the later `add` is
refused with a `RuntimeError` rather than quietly not being waited for. Make a
new one for each round.

```voila
let wg = sync.wait_group()
wg.add(len(jobs))
group {
    each j in jobs { spawn handle(j, wg) }   // each `handle` ends with wg.done()
    spawn { wg.wait(); say "all done" }
}
```

More `done()` than `add()` drives the count below zero, which is a bug and is
reported as a `RuntimeError`.

---

## 11.8 The std/time Package

`std/time` has two halves, and they meet only at `instant`.

The **stopwatch** half measures elapsed time. `time.now()` returns an
`instant` (nanoseconds since the Unix epoch, read from the real-time clock),
`time.since(i)` returns a `dur`, and `time.sleep` and `time.after` wait. The
six duration constants — `Nanosecond Microsecond Millisecond Second Minute
Hour` — multiply by integers, so `40 * time.Millisecond` is a duration and
`2 * time.Second` is the timeout in `group timeout 2 * time.Second`.

The **calendar** half answers what day it is.

```voila
use "std/time"

let d = must time.utc(2026, 8, 25, 13, 4, 5)
say d                                   // 2026-08-25T13:04:05Z
say d.year, d.month, d.day              // 2026 8 25
say d.weekday_name(), d.yearday()       // Tuesday 237
say d.to_offset(19800)                  // 2026-08-25T18:34:05+05:30
say time.now_utc().date_string()        // today, in UTC
```

### 11.8.1 DateTime

`DateTime` is a plain struct: `year month day hour minute second nanosecond
offset`, all `int`. `offset` is **seconds east of UTC**, so 0 is UTC and 19800
is +05:30. Two DateTimes with different offsets can name the same moment;
`same_moment`, `before` and `after` compare moments, while `==` compares
fields and so also asks whether the two were read at the same offset.

| Built by | |
|---|---|
| `time.now_utc()` `time.now_local()` | the current moment |
| `time.date(y, mo, d, h, mi, s, ns, offset)` | every part, checked |
| `time.utc(y, mo, d, h, mi, s)` | `date` with no fraction, in UTC |
| `time.from_unix(secs)` `time.from_unix_at(secs, offset)` | a Unix timestamp |
| `time.from_unix_nanos(ns)` `time.from_unix_nanos_at(ns, offset)` | the same, finer |
| `time.from_unix_seconds_at(secs, ns, offset)` | a second and its fraction, kept apart |
| `time.from_instant(i)` `time.from_instant_at(i, offset)` `time.local(i)` | an `instant` |
| `time.parse_rfc3339(s)` | RFC 3339 text |

| Method | |
|---|---|
| `rfc3339() date_string() time_string()` | formatting |
| `unix() unix_millis() unix_nanos() to_instant()` | back to a timestamp |
| `to_utc() to_offset(secs) to_local()` | the same moment, read elsewhere |
| `add(d)` | move by a `dur` |
| `before(o) after(o) same_moment(o)` | ordering |
| `weekday() weekday_name() month_name() yearday()` | calendar names and numbers |

`weekday()` is 0 for Sunday through 6 for Saturday. The names are English on
every machine and in every locale, deliberately: nothing in this package calls
`strftime`, because `strftime` is locale-dependent and Voilà programs are
expected to print the same bytes everywhere.

Package functions `time.is_leap_year(y)` and `time.days_in_month(y, m)` answer
the two calendar questions that need no DateTime.

### 11.8.2 Time Zones: UTC and Fixed Offsets Only

There is **no tzdata**. Named zones do not exist in this package, and a
`DateTime` carries an offset rather than a zone — so it cannot tell you what
its own offset will become after the next daylight-saving change. Exactly
three things are available:

1. **UTC** — offset 0.
2. **A fixed offset** you supply, in seconds east of Greenwich.
3. **The process's own offset**, as the C library reports it *for a given
   instant* (`localtime_r`). `time.now_local()`, `time.local(i)`,
   `time.local_offset()` and `time.offset_at(i)` are the four ways to ask,
   and because the instant is passed through, a summer timestamp reads in
   summer time even when it is January now.

If you need "what is the time in Europe/Berlin", this package is the wrong
tool, and saying so is better than half-answering it.

### 11.8.3 RFC 3339, Exactly

`rfc3339()` writes `2026-08-25T13:04:05Z`, or `…+05:30` for a non-zero
offset. The fraction is omitted when the nanosecond is zero and otherwise
printed to the shortest of 3, 6 or 9 digits that loses nothing — a
deterministic rule, which matters when the output is compared byte for byte.

`time.parse_rfc3339` accepts `T`, RFC 3339 §5.6's lower-case `t`, and the
NOTE's space; `Z`, `z`, or `±HH:MM`; and a fraction of any length, truncated
to nanoseconds. The offset is **not** optional — a timestamp without one does
not say what moment it means, and guessing UTC is how timestamps drift by
hours. `-00:00`, to which RFC 3339 §4.3 gives the distinct meaning "offset
unknown", is read as plain UTC and writes back as `Z`; a `DateTime` has
nowhere to record "unknown". A leap second (`:60`) is refused: there is no
leap-second table here and folding it into the next minute would move the
timestamp.

Three ranges apply, and they are far apart. The calendar itself is seconds
arithmetic and good to year ±292 billion. Nanoseconds are `int64`, so
`unix_nanos()`, `to_instant()` and `add()` cover
1677-09-21T00:12:43.145224192Z to 2262-04-11T23:47:16.854775807Z and overflow
(which aborts) outside it. RFC 3339 has four year digits and no room for
offset seconds, so `rfc3339()` is a legal RFC 3339 timestamp for years
0..9999, and an offset that is not a whole number of minutes renders to its
whole-minute part — including an offset of −59..−1 seconds, which renders
`-00:00` and reads back as UTC.

---

## 11.9 The std/test Package

`std/test` is assertions with a tally. It sits between `assert(cond, msg)`,
which aborts the process on the first disagreement, and comparing a whole
program's output against a golden file: it runs every check, prints the ones
that failed with what was expected and what arrived, and reports the count.

```voila
use "std/test"
use "std/str"

func main() {
    var t = test.suite("str")
    t.eq("upper", str.upper("ab"), "AB")
    t.ne("case matters", str.upper("ab"), "ab")
    t.near("thirds", 1.0 / 3.0, 0.3333, 0.0001)
    t.ok("not empty", len("ab") > 0)
    t.done()                    // summary, then exit 1 if anything failed
}
```

A failure prints one line and carries on:

```
FAIL str/upper: expected "AB", got "Ab"
str: 3 passed, 1 failed
```

Every value in that line goes through `test.repr`, which renders a value the
way it would appear inside a collection — so a `str` comes out quoted, and
`expected "ab", got "ab "` names the trailing space that `expected ab, got ab`
would hide.

### 11.9.1 The assertions

| Call | Passes when |
|---|---|
| `t.eq(what, got, want)` | `got == want` — DEEP for lists and structs, so a whole record compares in one call |
| `t.ne(what, got, want)` | `got != want` |
| `t.near(what, got, want, tol)` | `abs(got - want) <= tol`; the assertion for `float` and `dec`, where `eq` is a trap. All three must be the same numeric type. |
| `t.ok(what, cond)` / `t.not_ok(what, cond)` | the condition is true / false |
| `t.raises(what, f)` | calling `f` THROWS; returns the message so you can go on asserting on it |
| `t.raises_msg(what, want, f)` | `f` throws and its message contains `want` |
| `t.fail(what, why)` | never — it records a failure outright |

Each returns a `bool`, so a caller may branch on the verdict. `raises` catches
exceptions only: a function returning an error value (`T!`) has not thrown, and
is asserted with `eq` on the `Err` arm or by wrapping the call in `must`.

### 11.9.2 Table tests

`test.row` builds a row whose two values are already computed; `test.Case`
carries an input for `run` to apply a function to.

```voila
t.table("base", [test.row("file", path.base("a/b.c"), "b.c"),
                 test.row("root", path.base("/"),     "")])

t.run("upper", [test.Case{name: "ascii", input: "ab", want: "AB"},
                test.Case{name: "empty", input: "",   want: ""}],
      fn(s) { return str.upper(s) })
```

Both run every row — stopping at the first failure would hide the other
nineteen — and both name the row in the failure, not just the table.

### 11.9.3 Running a suite

There is no `voila test` verb. A suite is an ordinary program with a `main`,
built and run like any other, and the exit status is what a shell-driven
harness reads:

- `t.report()` prints the summary and returns whether everything passed. It
  does not exit.
- `t.done()` is `report` plus `os.exit(1)` when anything failed. It is the
  last line of a suite's `main` and the only thing in the package that ends
  the process.
- `t.merge(other)` folds a second suite's tally and failures in; the failure
  lines already carry the other suite's name, so they stay attributable.
- `t.failures()` is every failure line, for a caller that wants to print them
  its own way. `t.verbose = true` prints the passes too.

---

## 11.10 The std/csv Package

`std/csv` reads and writes RFC 4180. The rule that earns it a package rather
than a `str.split` is that a quoted field may contain commas, quotes AND
NEWLINES — so **a record is not the same thing as a line**, and a parser built
on `each line in reader` is wrong on real data.

```voila
use "std/csv"

func main() {
    var rd = csv.reader(must os.open("prices.csv"))
    each rec in rd {
        say rec[0], rec[1]
    }
    rd.close()
}
```

`csv.reader` takes anything `io.reader` takes — a file, a socket, a `net.Conn`,
`os.stdin`, another reader — and holds only the record it is building, so a
four-gigabyte file costs the memory of its widest record. `each rec in rd`
works because `Reader.next(mut self) ?[]str` joins the iteration protocol
(11.4, `std/io`); `rd.next()` may equally be called by hand, and `rd.all()`
reads the rest into memory at once.

Writing is the mirror:

```voila
var w = csv.writer(must os.create("out.csv"))
w.write(["id", "note"])
w.write_all([["1", "a,b"], ["2", "line\nbreak"]])
w.close()                               // flushes first
```

For a document already in memory there is `csv.decode(text) [][]str` and
`csv.encode(recs) str` — the names `json.encode`/`json.decode` already use,
and the only ones available since `parse` is a reserved word (Appendix A).

`sep` is a public field on both `Reader` and `Writer`, so TSV is `rd.sep = '\t'`
after construction; `w.crlf = true` writes the `\r\n` the RFC specifies, which
the reader accepts either way.

### 11.10.1 What it accepts

Errors are **thrown**, not returned as values — the iteration protocol reads
`next`'s result as a record or nil, and an error value is neither. `attempt`
converts a throw back to a value in one word (9.5), and `try { … } catch e`
handles a whole loop at once. `FormatError` is thrown for malformed input;
`std/io` errors propagate for a failed read or write.

- **Line endings**: `\n` and `\r\n` both end a record, and the `\r` of a `\r\n`
  is not part of the field before it. A LONE `\r` is DATA.
- **Blank lines** carry no record and are skipped. A line holding `""` is not
  blank: it is one empty quoted field.
- **A quoted field is preserved byte for byte**, `\r\n` included.
- **Ragged records** are returned as they come; nothing enforces a field count.
- **Leniency**: text after a closing quote is appended (`"ab"c` reads as `abc`),
  and a bare quote inside an unquoted field is an ordinary character. The only
  input refused is a quoted field with no closing quote before end of input.

On the way out, a field is quoted when it contains the delimiter, a quote, a CR
or an LF — and also when it begins or ends with a space or a tab, which RFC 4180
does not require but enough readers mishandle to be worth saying explicitly.

## 11.11 The std/crypto Package

Message digests, HMAC, and the entropy source — written from FIPS 180-4,
RFC 1321 and RFC 2104 in the runtime's own C, with no OpenSSL and no other
third-party dependency.

```voila
use "std/crypto"

crypto.sha256("abc")                // "ba7816bf…f20015ad", lowercase hex
crypto.sha256(bytes("abc"))         // the same digest from a []byte
crypto.sha256_raw("abc").b64()      // the same digest, base64
crypto.hmac_sha256(key, message)    // RFC 2104
crypto.random(32)                   // 32 bytes fit to be a key
crypto.random(32).hex()             // …as a 64-character token
```

| | |
|---|---|
| `sha256(m)` `sha512(m)` | lowercase hex digest |
| `sha1(m)` `md5(m)` | **legacy** — see below |
| `sha256_raw(m)` and the other three | the same digest as raw bytes |
| `hmac_sha256(k, m)` `hmac_sha512(k, m)` | RFC 2104, lowercase hex |
| `hmac_sha1(k, m)` `hmac_md5(k, m)` | the legacy hashes, keyed |
| `hmac_sha256_raw(k, m)` and the other three | the same MAC as raw bytes |
| `random(n)` | `n` cryptographically secure random bytes |
| `random_int(n)` | a uniform integer in `[0, n)`, with no modulo bias |
| `equal(a, b)` | equality in time independent of *where* they differ |

**Two spellings in, two forms out.** A message or a key may be a `str` — which
in Voilà is a byte string, which is why `os.read` and `net.recv` return binary
in one — or a `[]byte`; the two give the same digest, so it never matters how
the caller happened to be holding the bytes. Output is lowercase hex from the
plain names and raw digest bytes from the `_raw` names, over which the encoders
that already exist are the whole encoding layer: `d.hex()`, `d.b64()`,
`bytes(d)`.

**SHA-1 and MD5 are broken for security.** They are here for checksums and for
interoperating with formats that already chose them. A practical collision
costs seconds for MD5 and has existed for SHA-1 since 2017, so neither may
carry a signature, a certificate, or a content-addressed identity. HMAC repairs
a great deal — there is no practical forgery against HMAC-MD5 or HMAC-SHA-1 —
but a new protocol still wants `hmac_sha256`. Nothing in the package picks one
of them for you; choosing a legacy hash is always something the program said
out loud.

**`crypto.random` is the only random source in the language fit for a key, a
token, a nonce, a password-reset link or a session id.** It draws from
`getrandom(2)` on Linux, `getentropy(2)` on macOS and the BSDs, and
`/dev/urandom` where neither is reachable, and it aborts rather than falling
back to a weak source: a CSPRNG that silently degrades keeps minting keys
that look fine and never says so. That is the *only* fatal case — a negative
count or a non-positive bound is a catchable `RangeError`, so
`crypto.random_int(len(xs))` over an empty collection is an error a program
can handle rather than a process kill.

`std/rand` (11.4) is the other generator and is a different tool: fast,
seedable, and *reproducible*, which is what makes a test with random input
worth writing. Its output is predictable from a few hundred samples, so it may
choose a card or a jitter interval and never a secret. `rand.seed(n)` still
deals the same sequence every time; what it no longer does is start from the
clock when nobody seeds it, which used to mean two processes launched in the
same second dealt identical hands. It is the one place that will fall back to
the clock, and only where no entropy source exists at all: every program runs
that seeding, so it must not be able to stop one from starting. `uuid.new()`
draws from `crypto.random` proper, so two UUIDs cannot collide that way
either.

**Compare a MAC with `crypto.equal`, not `==`.** `==` stops at the first byte
that differs, and that timing is enough to recover a MAC one byte at a time
from any service that will verify a guess.

---

## 11.12 The std/http Package

`std/http` speaks HTTP/1.1 over `std/net`, in both directions, and is written
in Voilà with no C of its own. The client is three functions:

```voila
use "std/http"

func main() {
    match http.get("http://example.com/index.html") {
        Err(e) => say "failed:", e.message()
        Ok(r)  => {
            say r.status, r.reason, r.header("content-type")
            say r.body
        }
    }
}
```

`http.post(url, body)` sends one, and `http.request(method, url, headers, body)`
is the general form. Each opens a connection, sends one request, reads one
response and closes it: there is no keep-alive, no connection pool, and **no
automatic redirect following** — a 301 comes back as a 301, with its `Location`
intact, because a library that silently re-issues your request somewhere else
can silently re-issue it somewhere you did not authorise. `http.client()`
returns a `Client` whose `timeout_ms` and `max_body` are ordinary fields.

The server is a listener and a handler:

```voila
func hello(req http.Request) http.Response {
    if req.path == "/hello" { return http.text(200, "hi " || req.query || "\n") }
    return http.not_found()
}

func main() {
    var srv = must http.listen("127.0.0.1:8080")
    while true {
        match srv.serve_one(hello) {
            Ok(u)  => u
            Err(e) => say "http:", e.message()
        }
    }
}
```

`serve_one` accepts ONE connection, reads ONE request, calls the handler and
writes what it returns — the loop, and any `spawn` inside it, is the program's,
because std/net's model is thread-per-connection (11.5) and how many
connections to take is not a library's decision. `srv.accept()` hands back a
`Session` for a caller who wants `read_request()` and `respond()` by hand.
`Content-Length` is written from the body actually sent, so a handler cannot
declare one length and send another.

### 11.12.1 There is no TLS

This release has no TLS stack, so `https://` is **refused by name** at
`parse_url` — it is not fetched over port 443 in the clear and it is not
downgraded to `http://`. Either would hand a program's credentials to anyone
on the path while its source said `https`. Terminate TLS outside the program
(a local proxy, a sidecar) and point this at the plaintext side.

### 11.12.2 Headers, URLs and chunked bodies

`Headers` is an ordered LIST of `Name: value` pairs, not a map: a header may
legally repeat (`Set-Cookie`), and the order is stable across runs. Lookup is
case-INSENSITIVE throughout — `get has all remove set` all match with
`str.equal_fold`, so `Content-Length` and `content-length` are one header.
`add` keeps what is there; `set` replaces it in place.

`http.parse_url` yields `URL{scheme, host, port, path, query}` with the port
filled in (80 when the URL omits it), the fragment dropped (it is never sent),
and `userinfo` refused rather than silently discarded.

`Transfer-Encoding: chunked` is decoded on read, on both sides — a real server
sends it whenever it does not know the length in advance. Chunk extensions are
parsed and ignored, trailers are read and discarded, and every chunk size is
checked against the REMAINING budget so the total cannot exceed `max_body`.
Writing is always `Content-Length`.

### 11.12.3 What it refuses

A message this parser cannot vouch for is an error VALUE, never a guess: no
status line; a header with no colon, an empty name, or whitespace before the
colon; obsolete line folding; a `Content-Length` that is not decimal or that
exceeds `max_body`; two `Content-Length` headers that disagree; a
`Content-Length` alongside `Transfer-Encoding: chunked`; a body shorter than
declared; a `Transfer-Encoding` it cannot decode. The last four are how
request smuggling is built.

None of them can hang or exhaust memory. The length is parsed digit by digit
against the limit and refused **before** anything is allocated, so
`Content-Length: 99999999999999` costs a diagnostic and not ninety terabytes;
a header line is assembled by the package itself and refused at 65536 bytes,
so a peer that never sends a newline cannot grow the process; and every socket
carries a timeout, so a peer that says nothing at all is an error and not a
wedged task.

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

Every command that compiles a whole program — `run`, `build`, `check`, `-S`,
`--emit=c` — also accepts the warning options of 12.6.

## 12.2 How a Program Is Compiled

Voilà is a compiled language. The toolchain is a compiler — written in
Voilà — and a runtime library written in C. It is itself built by a prior
Voilà compiler (`./build.sh`, the self-hosted path, needs a Voilà ≥ 0.4.1),
or, on a machine with no Voilà binary, from a checked-in C seed (`./bootstrap.bash`);
either way the system `cc` produces the final binaries (BOOTSTRAP.md):

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

## 12.6 Warnings

A **warning** is advice: the program is translated, the executable is produced,
and the exit status is unchanged. Every warning is written to **standard
error** — never to standard output, which belongs to `-S` and `--emit=c` — and
carries a stable code in brackets:

```
prog.voi:4:1: warning[unused-import]: unused import `std/time`: nothing in this file mentions `time`
```

That code is what the options address it by:

```
-W<code>            turn the warning on
-Wno-<code>         turn it off
-Wall               turn every warning on
-Werror             every warning that is on becomes an error
-Werror=<code>      only this one becomes an error (and is turned on)
--strict            -Wall -Werror
```

A misspelt code is refused rather than ignored; `voila` with no arguments
lists the codes it knows. The codes today are:

| Code | What it says |
|---|---|
| `unused-import` | A `use` that nothing reaches. The scan is deliberately shy, because deleting an import that *is* reached breaks the program: it spans the whole package rather than the one file (a qualifier resolves against the package's merged imports); it counts a package's trait names and enum variants, which an importer may spell bare; it never reports a package that adds methods to somebody else's type, nor one whose package-level initializer runs its own code, since neither leaves anything in the importing file to find; and `std/str` is exempt, its functions being reachable bare (`s.upper()` *is* `str.upper(s)`). |
| `unreachable` | A statement written after a `return`, `throw`, `break` or `continue` in the same block. |
| `type-mismatch` | A value that does not fit the hole it is put in: an initialiser or an assignment against its annotation, an argument against the parameter, a returned value against the declared result, a field a struct does not declare, and the existence and arity of a method on a nominal receiver — which the runtime otherwise resolves by string and answers with an abort. A trait-typed hole takes any implementor and a trait-typed receiver answers the trait's own methods, so `[]Describe` is judged too. It is deliberately partial: it says nothing where it cannot be sure, and generics, `?T`/`T!` payloads, enum payloads, collection literals and most operators are among the things it declines to guess at. |

`unused-import` and `unreachable` are on by default. `type-mismatch` is **off**
by default — ask for it with `-Wtype-mismatch`, `-Wall` or `--strict`. It is
the youngest and largest check in the compiler and it reasons about a partial
inference rather than about a declaration; it fired on nothing in the whole
repository, but the cost of a mistake in it is a refused build, so it opts in. Warnings never change the code that is emitted — but
the option state *is* part of the `voila run` cache key, so a `--strict` run is
never answered with the binary a lenient one left behind.

## 12.7 Completion Codes

See Appendix C.

## 12.8 Authoring a Standard Package in Voilà

`std/net` (11.5) is the first standard package written in Voilà rather than
in C; `std/sync` (11.7), `std/path`, the calendar half of `std/time` (11.8),
`std/test` (11.9), `std/csv` (11.10) and `std/http` (11.12) are the next six. A package that a program reaches with `use "std/X"` may
now be Voilà source carried inside the compiler and compiled into the program,
exactly as the C runtime is (12.2). Only the operations that cross into the
operating system — the true system calls, the machine-level primitives
`std/sync` is built on, and `localtime_r` — remain a thin C shim. `std/path`,
`std/test`, `std/csv` and `std/http` have no shim at all: they are Voilà and
nothing else, built over what the language and the other packages already
provide — `std/http` over `std/net` and `std/io`, with no C of its own.

`std/time` is the first HYBRID: its stopwatch half is C natives with no Voilà
body at all (`time.now`, `time.sleep`, the duration constants), while its
calendar half is Voilà. That works because a qualified name resolves against
the grafted declarations first and falls through to the natives — the same
path that makes the public C name `net.resolve` reachable from a package
whose other functions are Voilà.

The parts of such a package, and where each lives:

```
   1.  std/X/*.voi          the package source, `package X`
   2.  a C shim (optional)  one runtime function per system call, in
                            runtime/src, returning a value or an IOError
   3.  runtime/src/std.c    each shim function entered in the native table
   4.  voilac/low/names.voi  each native name added to StdSrcNatives(), which
                            cg/instr.voi's pkg_natives() splices in and
                            chk/stdpkg.voi reads to tell a native from a typo
   5.  voilac/low/names.voi  X added to the package-name table
   6.  voilac/load/load.voi  X registered as a Voilà-source std package — and
                            REMOVED from std_c_packages() if it was there
   7.  voilac/std/blob.voi  REGENERATED — std/X embedded in the compiler
```

Step 4 is why a native of a Voilà-source package is listed apart from every
other native: once the package is grafted, `X.thing` must resolve to a
declaration OR to one of these names, and anything else is a misspelling the
checker reports at the reference rather than a name the backend refuses.

Step 7 is not edited by hand: `voila run tools/build.voi stdblob` regenerates
it from `std/`, and a full build regenerates it as its first act. After a
change to the shim or the package, rebuild and reseed as for any compiler
change — `./build.sh` then `./build.sh reseed` (or `./bootstrap.bash` on a
machine without a Voilà binary).

A Voilà-source package may export a `const` as well as functions —
`path.SEP` is one — because the grafted declaration is an ordinary program
global that the qualified name resolves to before any native is considered.

The division of labour is the point. The package's public functions are
Voilà, so a call to one of them compiles to a direct call into grafted Voilà
code; only the shim names resolve to C. A program therefore pays for the
system calls in C and for nothing else — the logic, the types, the error
handling, and the address parsing are all Voilà the reader can follow.

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
| 2 | translation failed: syntax or static-check diagnostics were issued — including a warning that `-Werror` promoted (12.6); also command usage errors |
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
cost model does not. A million tasks is not yet fine. A corollary reaches
`std/net` (11.5.4): a task blocked in a socket `read` or `accept` is
inside a system call, where the language's cooperative cancellation cannot
reach it, so a GROUP timeout does not interrupt it. A socket that must not
block indefinitely is given a deadline with `set_timeout`, which the
operating system enforces.

The packages named in the specification's §13.4 that this release does not
provide — `std/chan`, `std/slices`, `std/maps`, `std/vm`, `std/ffi`,
`std/xml`, `std/yaml` — are refused at the `use`, not silently mistranslated,
so a program that names one fails to compile rather than at run time.
(`std/regex` and `std/http` were in this list until they shipped; see
Sections 11.12 and 13.)

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

**D.9 — Not yet implemented.** `voila mod tidy`, `voila fmt/test/doc/bench/repl`,
`std/vm`, `std/ffi`, `std/xml/yaml`, signatures in `std/crypto`, checked `throws`
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
