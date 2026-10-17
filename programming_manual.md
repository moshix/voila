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
the *Voilà Language Specification, Version 0.6.0 (Draft)*, the difference
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
Box[T]            owned, unique heap indirection (Section 8.4)
```

A collection value may be written as a *typed literal*, the type
followed by the members in braces:

```voila
[]int{1, 2, 3}                 a slice ([1, 2, 3] also works)
[]str{}                        an empty slice of a stated type
map[str]int{"ann": 31, "bob": 42}
set[int]{3, 1, 3}              duplicates collapse
```

Members are separated by commas or newlines. In a map literal a
repeated key keeps its last value. A map literal always names its
type; a bare brace opens a block.

The members of a tuple are selected by position: `t.0`, `t.1`, and
`t.0.1` inside a nested tuple. A position is plain decimal digits; a
member cannot be assigned; and a position past the end of a tuple whose
width is known is diagnosed at compile time.

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

   >>──┬─LET─┬──(──<name>──┬───────────┬──)──=──<expression>──────────────><
       └─VAR─┘             └─,─<name>──┘ (repeated)

   >>──CONST──<name>──=──<constant-expression>────────────────────────────><
```

A LET binding is immutable; a VAR binding may be reassigned. The
parenthesized form takes a tuple apart, one name per member, `_`
discarding one: `let (q, r) = divmod(17, 5)`, `var (lo, hi) = (0, 9)`.
A VAR
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

**Sized integers.** Every integer is held as an `int` at run time; the
range of a sized type (`i8` through `u64`) is enforced by the
translator wherever it knows the type statically:

- arithmetic (`+ - * ** ~/`, unary `-`) whose type is sized. An
  untyped literal takes the sized type beside it when it fits in it
  (`x + 1` with `x i8` is an `i8` sum; `x + 1000` is an `int` sum),
  and two sized operands widen to the narrowest type both fit in;
- a compound assignment to a sized variable, field or element —
  an element's field too (`xs[0].a += n`);
- a store into a sized location of a value not known to fit: an
  initializer, an assignment, an argument (checked on entry to the
  procedure; not the elements of a variadic one), a returned value (a
  typed closure's included), a structure field, an indexed slice
  element, an appended element (`bs.append(v)` on a `[]u8`), a map
  value (`m[k] = v`) and a value sent on a sized channel;
- arithmetic on a tuple member of known type (`t.0`) and on the
  variable of an EACH over a sized slice;
- an integer literal stored into one, at translation time.

The condition is OverflowError, with a message naming the operation and
the type — `integer overflow: 127 + 1 is out of range for i8` —
identical under every `-O` level. A value whose type the translator
cannot see is computed as an `int` and checked when it is stored. `u64`
holds 0 to 2**63 - 1. Shifts and bitwise operators are not
range-checked.

## 3.6 Type Declarations

```
   >>──TYPE──<name>──┬──=──<type>─────────────────┬──────────────────────><
                     ├──<type>────────────────────┤
                     └──STRUCT──{──<fields>──}────┘
```

`type X = T` declares an **alias**: X is another name for T,
interchangeable with it, and `X(v)` is `T(v)`. `type X T` declares a
**newtype**: a distinct type with T's representation.

```voila
type UserID = int                 // alias
type Cel int                      // newtype
impl Cel { func to_fah(self) float { return float(int(self)) * 1.8 + 32.0 } }
let c = Cel(21)                   // explicit conversion in
say c, int(c) + 1, c.to_fah()     // 21 22 69.8 — and out
```

A newtype has methods and trait implementations of its own, which
dispatch on the newtype rather than on T. `X(v)` converts v to T first
(so `type Age u8` keeps the u8 range) and `T(x)` takes the value back
out; an untyped literal stored where the newtype is declared (`var c
Cel = 5`) converts implicitly. Between two values of the same newtype,
T's operators apply and an
arithmetic result is the newtype again; mixed with any other type, a
plain `int` included, the operation is refused — convert explicitly. A
newtype prints as its underlying value unless it implements Show, and
it is transparent to the builtins that read a value: a newtype over a
slice indexes, iterates and has `len` and the slice methods; one over a
procedure type can be called; one over a structure has its fields.

---

# Chapter 4. Expressions and Operators

## 4.1 Operator Precedence

From highest (bound first) to lowest:

| Level | Operators | Association |
|---|---|---|
| 11 | `x.f  x[i]  x(...)  x?  t.0` | left |
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

## 4.6 Conditional Expressions

IF and MATCH may be used where a value is expected:

```voila
let size = if n < 10 { "small" } else if n < 100 { "medium" } else { "large" }
let name = match shape { Circle(r) => "circle {r}"; Empty => "empty"; _ => "other" }
let v    = if let Some(x) = to_int(s) { x } else { 0 }
```

As a value, IF requires an ELSE. The value of a branch or arm is the
value of its last statement, which must be an expression; an IF with an
ELSE, a MATCH, or a block, yielding its value in turn; or a RETURN,
THROW, BREAK, or CONTINUE, in which case the branch *diverges* and
produces nothing. Statements may precede the last one. Any other ending
is diagnosed at compile time.

## 4.7 Parallel Assignment

```voila
a, b = b, a                    // exchange
xs[0], xs[1] = xs[1], xs[0]
q, r = divmod(17, 5)           // a tuple-valued right-hand side
```

All values on the right are evaluated before any target is assigned.

## 4.8 Static Type Checking

Every expression is typed, and every value is judged against the place it
is put — the translator refuses the program, with the file and line, rather
than letting it run into a wrong answer or an abort:

```voila
func f(a int) int { return a }
f("s")                      // argument 1 of `f` is `int`, but `str` is given
var n = 3
n = "three"                 // an unannotated var KEEPS its type
let xs = []int{"a"}         // an element of `[]int` must be `int`
say 1 == "x"                // cannot compare `int` with `str`
say "a" + 1                 // `+` does not concatenate — use `||`
if n { … }                  // a condition is a bool: no truthiness
"x".lstrip("-")             // `str` has no method `lstrip`
let k int = to_int(s)       // a `?int` is not an `int` until unwrapped
let c Cel = n               // a newtype takes only a literal implicitly
```

The rules are those of Section 3.5 and the specification's §5.5. A generic
procedure is instantiated at each call by matching its parameter types with
the arguments, so `mx(1, "x")` for `mx[T](a T, b T)` is refused at the second
argument, and a type argument found that way is held to the bounds as a
written one is. Inside a generic body a value of a type parameter answers only
the methods its bounds declare. An IF or MATCH used as a value has one type.

The check is **sound in one direction**: where a type is not written down
anywhere — an untyped JSON value, a value of a trait type, which dispatches
at run time, the result of a native whose type no table states — the value
is accepted wherever it is put, and a *name* holding an optional is taken to
have been narrowed before it is used (`if x == nil { return }`), since the
translator does not follow that flow. A missed error is possible; a refused
working program is the defect the check is built to avoid.

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
violation, permitting recovery. Either bound of a slice may be omitted:
`s[a..]` runs to `len(s)`, `s[..b]` starts at 0, and `s[..]` is all of
`s`. Indexing a map with an absent key yields `nil`.

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
arrow `=>`, and one statement or a block; a one-statement arm may end
in a comma. Patterns are literals, `_`, `nil`, binding identifiers
(lowercase), variant patterns (`Some(v)`, `Ok(v)`, `Err(e)`, and user
enumeration variants), and the compound forms:

```voila
0 | 1 | 2              or-pattern; every alternative binds the same names
3..10   'a'..='z'      range of literals (exclusive / inclusive); only
                       values of the bounds' own kind match
P{x: 0, y}             struct pattern; a bare field name binds it; `..` ends
(0, y)                 tuple pattern
```

Patterns nest, and exhaustiveness is judged on the nested parts too.
MATCH may also be used as a value (Section 4.6).

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

**Captured variables and tasks.** A captured variable is one variable,
shared with every task its procedure spawns: a task may store into a
slice element or a field of it (`spawn { out[j] = j * 10 }`), and the
stores of different tasks never undo each other. Its value keeps the
value semantics of Section 6.4 — `let row = out[0]` is a snapshot,
whatever the tasks did before. A spawned body may not assign the
variable itself, store into a map in it, grow, shrink or reorder a
collection in it, call a `mut self` method on it, or hand it to a `mut`
parameter: the translator rejects each, and points to a channel, a
CELL or a SHARED value instead. A closure that would break those rules
may not be run by a task either — captured by a spawned body, handed to
a parameter that its callee runs in a task, or sent over a channel.
Writes inside your own critical section (`mu.lock()` … `mu.unlock()`,
or a `with_write` closure) are left to you. `tot[0] += 1` from many
tasks loses no update. Whatever route a closure takes to a task, memory
stays safe: a read of a captured variable takes a snapshot of its value
(under its frame's lock once a closure over the frame may escape — one
that is stored, sent, spawned, returned or handed to anything but a
builtin collection method that runs it at once), a writer that finds the
value held copies it first, and a `mut` argument or `mut self` receiver in
such a variable works on a copy stored back after the call. A variable
never written after it is made is read without the lock. Reading a
captured variable in a tight loop costs a lock and a reference count per
read; take one snapshot (`let t = table`) and read that. A closure that
uses `self` in a `mut self` method may only be handed straight to a
builtin collection method that runs it at once, such as `xs.map`.

**Loop variables.** Each iteration of a loop is a fresh binding of the
variables declared in it — the loop variable and those of the body —
as in Go 1.22. A closure created in an iteration, whether called at
once, stored, deferred or spawned, keeps that iteration's values:

```voila
var fs = []
for i in 1..4 { fs.append(fn() int { return i }) }
each f in fs { say f() }               // 1, 2, 3
```

The closure receives its own copy of each such variable, made when the
closure is created; a variable declared before the loop is one binding
shared by all iterations and closures. A loop variable that is
assigned after a closure copied it — in the loop or in the closure —
would silently stop being shared, and is diagnosed at translation time,
by name: declare it before the loop to share it, or copy it into a LET
to capture its value.

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

A variant may always be qualified by its enumeration — `Shape.Empty`,
`Shape.Circle(2.0)`, `Shape.Circle` as a procedure value, and
`Shape.Circle(r)` in a pattern; from another package,
`geom.Shape.Circle`. Each enumeration is a namespace of its own, so two
enumerations may declare the same variant name:

```voila
enum Light { Red, Green }
enum Flag  { Red, White }
let a = Light.Red                 // qualified: always legal
let g = Green                     // bare: legal, Green is unique
let r = Red                       // ERROR: Light.Red or Flag.Red?
```

A variant name that one enumeration of its package declares may be
written bare; one that several declare must be qualified. A variant
name still may not coincide with another declaration of the package.

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
traits implemented by one type default the same method name and the type
writes it in neither, the program is refused: one default would silently
answer for both traits. Write the method for the type.

A method a program writes for a BUILTIN type — `impl Show for int`, or a
trait default inherited by `str` — answers in method-call form like any
other (`n.show()`), after the type's own builtin methods. Every integer is
an `int` at run time, so an impl for a sized integer type is reached by
a call the run time sees on an `int` only when `int` has the method too.

### 7.3.1 Generic Traits

A trait may take type parameters. An IMPL names the arguments — as many
as the trait declares — and a default method of a generic trait is
compiled once and inherited like any other:

```voila
trait Into[T] {
    func into(self) T
    func twice(self) (T, T) = (self.into(), self.into())
}

impl Into[str] for Cel { func into(self) str { return "{int(self)}C" } }

var xs []Into[str] = [Cel(1), Cel(2)]   // a trait at arguments is a type
```

Dispatch is by the receiver's run-time type and the method NAME, never
by trait argument. One type may therefore implement one generic trait
at several arguments only when those IMPLs write different methods; two
IMPLs that both write `into` are refused (the second could never be
reached). A marker trait, or one whose methods all have defaults, may be
implemented at any number of arguments.

There are no associated types. Where a trait would carry `type Item`,
make the trait generic over it: `trait Iterator[T] { func next(mut
self) ?T }`. A generic parameter says the same thing, and the bound
syntax already reads it (`[I: Iterator[int]]`).

### 7.3.2 Self and Static Methods

Inside a trait, `Self` is the implementing type. Inside an IMPL it is
an alias for the target, wherever a type or a type's name may appear:

```voila
trait Default {
    func default() Self                    // static: no self
    func reset(self) Self = Self.default() // Self, through the receiver
}

impl Default for Point {
    func default() Self { return Self{x: 0, y: 0} }
}

say Point.default()                        // on the type
func make[T: Default]() T { return T.default() }   // through a parameter
say make[Point]()
```

A method without `self` is STATIC. It is called on a type —
`Point.default()`, `Cel.default()` for a newtype, `int.default()` after
`impl Default for int`, `Wrap[int].make()` on an instantiated generic type
(whose arguments, generics being erased, say nothing the call needs) — or
through a type parameter, `T.default()`,
inside a generic procedure bounded by the trait (7.4). In a trait
default with a receiver, `Self.m()` calls `m` of the receiver's
run-time type; a default WITHOUT a receiver cannot write `Self.m()`,
having no way to know which type implements the trait.

### 7.3.3 Ordering and Iteration

The `Ord` constraint's method is `cmp(self, other Self) int`, answering
negative, zero or positive. A structure or enumeration that provides
one is ordered by it: the comparison operators, `min`, `max`, `sort`
and `sorted` consult it. Equality stays structural, and a newtype keeps
its underlying type's order.

```voila
impl Ord for Ver {
    func cmp(self, o Ver) int { return self.major - o.major }
}
sort.sort(versions)                       // by cmp
```

EACH iterates a user type by calling its `next(mut self) ?T` until it
answers nil. `std/iter` names that protocol as the trait
`iter.Iterator[T]`, with `iter.collect(it)` and `iter.count(it)`, so a
type can promise it and a generic procedure can require it. A trait of
a Voilà-source standard package is spelled with its qualifier:
`impl iter.Iterator[int] for Countdown`.

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

A parameter may carry several bounds joined by `+`, inline or in a
WHERE clause after the signature; the two spellings combine. An IMPL
block takes a WHERE clause before its brace. `where` is contextual: it
is a keyword only as `where NAME :` in that position.

```voila
func both[T: Show + Into[str]](x T) str { ... }
func same[T](x T) str where T: Show + Into[str] { ... }
impl[T] Shelf[T] where T: Into[str] { ... }
```

**Type descriptors.** Erasure leaves one compiled body per generic
procedure, so `T.default()` — a static method through a type parameter
— has no receiver to dispatch on. The translator therefore passes the
TYPE. Each type parameter of a top-level procedure whose bounds include
a trait that declares a static method receives a hidden, leading
argument: the type's name as a string, which is exactly the key the
run-time method table is searched by. `T.default()` becomes a lookup of
`default` under that name (`vl_callt`). The caller supplies the
descriptor

1. from an explicit type argument — `make[Point]()`; inside another
   generic procedure `make[T]()` forwards that procedure's own;
2. else from a value argument declared `T`, by its run-time type —
   `like(p)` for `func like[T: Default](x T) T`; a Box is looked
   through, and an argument of a known sized integer type passes that
   type, since every integer is an `int` at run time — while an argument
   declared as the ENCLOSING generic procedure's own parameter forwards
   that procedure's descriptor, so `like(x)` inside `wrap[T](x T)` keeps
   the `u8` that `wrap`'s caller passed;
3. else the call is refused at compile time: write `make[Type](...)`.

The descriptor occupies the callee's lowest registers, ahead of the
declared parameters, so defaults, named and variadic arguments are
unaffected. Two consequences follow, both diagnosed: a procedure that
takes descriptors cannot be used as a value (wrap the call in a
closure), and the type parameters of a method, an IMPL block or a trait
carry none, so `T.m()` is refused there (D.11). Bounds whose traits
have only methods WITH a receiver need no descriptor: the value carries
its type.

A constraint is enforced wherever an ACTUAL type is written in place of
the parameter: a composite literal `Stack[Point]{}`, an annotation
`var s Stack[Point]`, and an explicit instantiation `max[Point](a, b)`.
The argument must provide each bound's methods — and, where it
implements a generic trait explicitly, at the bound's arguments: a type
implementing only `Into[int]` does not satisfy `T: Into[str]` — and a
method counts wherever the program writes it — the flat method table does not tell an
inherent `impl T { ... }` apart from an `impl Trait for T`. The six
PREDECLARED constraints have no method set of their own, so nothing is
judged against one. A type argument that is INFERRED from a call's
arguments (Section 4.8) is held to the bounds in the same way —
`show(3.5)` is refused when `float` lacks a bound's method — and inside
the generic body a value of a bounded parameter answers only the methods
its bounds declare. A type argument may itself be an instantiation:
`Stack[Box[int]]`. A declaration with several parameters takes its
arguments in one bracket, in order: `mk[str, int](k, v)`,
`Lru[str, int]{cap: 8}`, `var c Lru[str, int]`. Any type form may be
an argument (`mk[[]int, ?str](...)`), and the count must match.

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

A move happens BY NAME, and only for a binding the procedure owns (a LET,
a VAR or an `own` parameter): `let b = a`, `b = a` and `ch <- a` move `a`
when its type owns storage — a slice, map, set, channel or procedure, or a
structure, tuple, enumeration or optional holding one. A Copy value is
copied, and a borrowed parameter bound to a second name is a second borrow.
The translator follows moves straight-line: within one block, into the
blocks nested in it, and up to a re-assignment, which revives the name.

A copied structure is copied LAZILY: the two names share it until one of
them writes it — a field or an element assigned through any path, a
`mut self` method called, a `mut` parameter given it — and the writer then
gets its own. Each level of a path is copied only when another holder still
shares it, so a structure only one name holds is always written in place.
Slices, maps and sets are values in the same way, wherever they are held —
a field, an element, a map value, a literal they were put into: a write
through one holder (an element stored, an append, a map store) is never
seen through another. A value read by an earlier expression and never read
again (`len(self.items)` in a loop condition) is dropped once it is dead,
so it is not counted as a holder, and a loop of appends through a field
copies nothing.

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
> through `weak[T]`, through `Box[T]` and `?Box[T]`, and through
> optional and collection edges, which are tree-shaped by construction.

**Box.** `Box[T]` is an owned, unique heap indirection, and the way a
recursive enumeration or structure is written:

```voila
enum Tree[T] { Leaf, Node(v T, l Box[Tree[T]], r Box[Tree[T]]) }

func insert(t Tree[int], x int) Tree[int] {
    return match t {
        Leaf => Node(x, Box(Leaf), Box(Leaf))
        Node(v, l, r) => if x < v { Node(v, Box(insert(l.get(), x)), r) }
                         else { Node(v, l, Box(insert(r.get(), x))) }
    }
}
```

`Box(v)` constructs one, `b.get()` reads it and `b.set(v)` replaces the
value. A Box has VALUE semantics: `var b = a` gives `b` its own Box,
and a `set` — or a field or element assigned — through `b` is not seen
through `a`. The copy is lazy: a Box is copied when it is written
through while another owner still holds it, and written in place when
it is not. A Box passed by name to a `mut` parameter is made unique
by the caller and written in place by the callee, so the caller sees
the write. Since the value written is evaluated first,
`b.set(LNode{next: b})` keeps the old Box inside the new one, and so
does `n.next = n`; through a `mut` parameter such a write goes to the
callee's own copy. A Box cannot be made to contain itself, which is
what lets the Acyclicity Rule exempt it. Otherwise a Box is transparent: fields and methods are
reached through it, comparison, arithmetic and SAY see the value, and
MATCH sees through it (`Node(v, Leaf, _)` matches a node whose left Box
holds `Leaf`; a binding pattern binds the Box itself). A **direct**
self-reference, `Node(v T, l Tree[T])`, is not boxed implicitly: the
specification admits self-reference only through `weak` or a Box, so
the translator reports the cycle and names both remedies. Destruction
of a deep structure is iterative and uses no stack in proportion to
its depth. A program that declares its own type named `Box` shadows
this one.

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
out of the enclosing procedure; the postfix form `e?` is identical. In a
procedure whose result is a plain `T`, or which declares no result at
all, and which declares no THROWS clause, `try`/`?` instead raises the
failure as an exception, as MUST does — a caller could not receive it
otherwise. The `main` procedure is the exception: a failure propagated
out of it ends the program with code 1 (Appendix C). The
`else` operator supplies a default; its right operand may instead be a
diverging statement (`return`, `throw`, `break`, `continue`), or a
block that runs only on failure:

```voila
let opts = parse(args) else {
    usage()
    os.exit(2)
}
```

The block's last statement is its value, or it diverges, under the
rules of Section 4.6 — except when the whole expression is a statement
whose value is unused (`save(d) else { say "not saved" }`); then the
block is an ordinary block.

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

**A FINALLY block runs to its end.** Control may not leave it: a RETURN
anywhere in it, and a BREAK or CONTINUE whose loop encloses it, is a
translation-time error (`control flow out of a finally block is not
allowed`), as in C#. Java's alternative — the finally's RETURN wins —
silently discards the exception in flight or the value already being
returned. A loop written inside the finally may be left as usual, and a
closure written there is a procedure of its own. For the same reason a
`?` or TRY inside a finally does not return the failure, even in a
procedure whose result is `T!` or `?T`: it RAISES it, exactly as MUST
does, the way a THROW written there would.

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

The trace is written to standard error after the message, one line per
active procedure, innermost first: its name as the program spells it
(a closure is shown as the procedure it is written in) and the file and
line of the statement that failed.

```
abort: index out of bounds: 5 (len 2)
  at pick (app.voi:3)
  at outer (app.voi:7)
  at main (app.voi:14)
```

An unhandled exception prints `unhandled exception: <message>` and the
trace recorded where it was first thrown — the same lines `e.trace()`
returns — followed by any `caused by:` chain. At most 64 frames are
listed; the rest are counted. Each procedure records its current line
with one store per source line that can fail, which is the whole run-
time cost of the facility.

**Recovery points.** One place in the library may CONTAIN an abort:
std/http's server calls each handler under a recovery point (11.15), so a
defect in one request costs that request a 500 — the abort's message goes
to standard error — and the server keeps serving, as Go's net/http
recovers a handler's panic. Under a recovery point the aborts a program's
own code provokes at the language level are contained: an index or a key
out of bounds, a field of nil or of the wrong kind, a call of something
that is not a function, a missing method, an operand of the wrong kind, a
failed `assert`, a native given an argument of the wrong kind. The abort
then unwinds like an exception — DEFERs and FINALLY blocks **do** run, so
a deferred unlock or a `try { … } finally { mu.unlock() }` still releases
its lock — but no `catch` clause sees it; only the recovery point does. A
defer or a finally that itself aborts or throws while the request is
being unwound makes that the request's failure, and it is contained the
same way. Everything else stays fatal even there: running out of memory,
a failure inside the runtime itself, a stack overflow, and an abort on a
task the recovery point did not start (recovery is per task). A program cannot open a recovery point of its
own; outside one, every abort ends the process as described above.

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
`enumerate`, `keys`, `values`, `shared`, `cell`, `weak`, `Box`, `set`, and the
common string helpers (`trim`, `pad`, `pad_left`, `center`, `upper`,
`lower`, `split`, `join`, ...).

**Argument kinds.** A library procedure implemented in the runtime
states the kind of each argument its body relies on — a string, an
integer, a slice, a map, a procedure. A call whose argument's type the
translator knows and which is of the wrong kind is diagnosed at
translation time:

```
app.voi:3:32: argument 2 of `os.run_capture` must be a slice, but `str` is given
```

Where the type is not known — the procedure called as a value, or an
argument of unknown type — the call is checked when it is made and a
wrong kind is an abort naming the procedure and the argument, never a
memory fault. A newtype or Box whose value is of the right kind is
accepted, by a direct call and through a value alike. The procedures
that walk a sequence — `join`, `sum`, `avg`, `zip`, `enumerate`,
`from_bytes`, `sort.reversed` — accept a set as well as a slice, in the
set's insertion order. The kinds cover std/os, std/io, std/str, std/fmt, std/sort
and every prelude procedure that reads a slice, a map or a channel.

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

Case/space: `upper lower title trim trim_left trim_right strip
trim_prefix trim_suffix pad pad_left center`. Search: `contains index
last_index starts_with ends_with count equal_fold`. Split: `substr left
right split split_n cut lines fields word words subword delword`. Build: `join replace
replace_n repeat reverse translate overlay insert delstr`. Convert:
`bytes from_bytes runes from_runes rune_len is_digit is_alpha is_space
is_upper valid_utf8 hex unhex b64 unb64`. Efficient building:
`str.Builder{}` with `.write(x)` and `.done()`.

`trim_prefix(s, p)` and `trim_suffix(s, p)` remove ONE leading or trailing
copy of `p` (where `strip` removes any run of a set of characters), and
`cut(s, sep)` splits around the first `sep`:

```voila
let (key, value, found) = "Content-Type: text/plain".cut(": ")
// ("Content-Type", "text/plain", true); not found → (s, "", false)
```

Case mapping and the `is_` tests are Unicode-aware: `upper("héllo")` is
`HÉLLO` and `is_alpha('é')` is true. `is_digit` alone stays ASCII `0-9`,
because it guards `int()`. A `str` is a byte string, so `from_bytes`
accepts any byte (binary data travels in a `str`); use `valid_utf8(s)` to
check whether a string is well-formed text.

## 11.4 Other Packages

| Package | Contents |
|---|---|
| std/os | `read write open create exists remove args env exit stderr stdout on_abort`; File: `lines() records(n) read_all() write() close()`. Processes and signals: `start kill getpid signals` and the `SIGINT`… constants (11.15); `walk glob` read directory trees. **`os.args()` does not include the program's name** — `args()[0]` is the first argument the user typed, unlike C's `argv` and Go's `os.Args` |
| std/math | `pi e sqrt sin cos tan ln log2 log10 exp pow hypot mod` |
| std/time | two halves. Stopwatch: `now since sleep after millis seconds`; `Second`, `Millisecond`, ... durations with integer arithmetic. Calendar: `DateTime`, `now_utc now_local date utc from_unix from_instant parse_rfc3339 is_leap_year days_in_month`. See 11.8. |
| std/json | `encode pretty decode decode[T]` — numbers with fractions decode as `dec`; `decode[T]` is typed and recursive, and reports a mismatch with its JSON path (11.16) |
| std/csv | RFC 4180 — `decode(text) encode(recs)`, and a streaming `Reader`/`Writer` over any `io` source. A quoted field may contain commas, quotes and NEWLINES, so a record is not a line. See 11.10. |
| std/log | `info warn error debug` to the standard error stream |
| std/net | TCP, UDP and Unix-domain sockets — `listen connect listen_udp dial_udp listen_unix dial_unix resolve`; `Listener`, `Conn`, `Udp` methods. See 11.5. |
| std/path | lexical paths — `base dir split join ext stem with_ext clean is_abs abs rel has_prefix matches`, and the `SEP` constant. `matches(pattern, name)` is a shell pattern — `*`, `?`, `[a-z]`, `[!x]`, `\c` — where `*` never crosses a `/`. `join` cleans its result, so `join(dir(p), base(p))` is `clean(p)` for every p. Nothing here touches the disk except `abs`. |
| std/sync | `mutex() rwlock() atomic(n) once() wait_group()` — mutual exclusion, read/write locks, atomic integers, run-once and work counting. See 11.7. |
| std/regex | RE2 semantics (linear time): `compile matches find find_all find_named names replace split`; a compiled `Regex` carries the same methods plus `source`. `find` returns `[]str` — the whole match, then each group by position — or nil; a NAMED group `(?P<year>\d+)` (or `(?<year>…)`) is also reachable by name: `find_named` returns a `map[str]str` of every named group plus `"0"` for the whole match (nil for no match), `names()` the group names by position, and `${year}` in a replacement inserts it. No backreferences and no lookaround — neither is expressible in a finite automaton, and supporting them would forfeit the linear-time guarantee. Note `{n}` inside a Voilà string literal is interpolation: write `\{n\}`. |
| std/http | HTTP/1.1 client and server — `get(url) post(url, body) request(method, url, headers, body)` returning a `Response{status, reason, proto, headers, body}`; `listen(addr)`, `serve_one(handler)` and `serve(handler, stop)` for a server; `parse_url`, `Headers` (case-insensitive), chunked decoding, `url_decode url_encode parse_query` and `Request.query_params()`. `https://` URLs are fetched over TLS with the certificate verified, and `listen_tls(addr, cert, key)` serves HTTPS. See 11.12. |
| std/tls | TLS 1.2 for `net.Conn`: `connect client server listen check_server version system_ca_file` and `Config`, verified against the system's CA bundle (11.18) |
| std/compress | `deflate inflate zlib unzlib gzip gunzip`, as streams `deflate_writer … gunzip_reader`, `crc32 adler32`; zip (`zip_open zip_create`) and tar (`tar_read tar_write`); every decoder bounded by `max` (11.19) |
| std/sqlite | an embedded SQL database: `open open_readonly`, `DB.exec run query query_one prepare transaction`, `Stmt`, `Row`, `Value`, `decode[T]` (11.20) |
| std/conv | the conversion functions, package-qualified |
| std/sort | `sort sort_by reversed` — `sort` and `sorted` order a user type by its `cmp` (7.3.3) |
| std/slices | generic, and none changes its argument: `contains index_of last_index_of index_where count reversed insert remove binary_search binary_search_by compact unique chunk flatten concat equal min max partition repeat` (11.14) |
| std/maps | `keys keys_sorted values items merge filter map_values invert from_items get_or equal` — insertion-ordered, so deterministic (11.14) |
| std/heap | the priority queue: `heap.new[T]()` (smallest first, by `<` or a type's `cmp`), `heap.new_by(less)`, `heap.from(xs)`; `push pop peek len is_empty items_sorted` (11.14) |
| std/chan | channel plumbing: `from collect map filter merge fan_out tee throttle limiter` (11.14) |
| std/flag | typed command-line flags: `str int bool float dur`, `parse usage about program`, and independent `set(prog, about)` Sets (11.17) |
| std/ffi | C interop and raw memory — OUTSIDE the safety guarantees: `ffi "C"` blocks, `alloc free cstr from_bytes null at sizeof`, `Ptr` (11.13) |
| std/iter | the `Iterator[T]` trait EACH drives; `collect(it) count(it)` (7.3.3) |
| std/rand, std/uuid | `rand.int(n) rand.float rand.seed(n)` — a fast, SEEDABLE, reproducible sequence, predictable and **not for secrets**; `uuid.new()`, which draws from std/crypto instead |
| std/crypto | `sha256 sha512` and the legacy `sha1 md5`, each also as `_raw`; `hmac_sha256 hmac_sha512 hmac_sha1 hmac_md5` (+`_raw`); `random(n) random_int(n) equal(a, b)`. `crypto.random` is the only random source fit for a key or a token. See 11.11. |
| std/test | assertions and table tests — `suite(name)`, then `eq ne near ok not_ok raises raises_msg fail table run`, and `summary report done`. See 11.9. |

Collections carry methods directly: slices have `append pop first last
map filter reduce sort sort_by sorted contains index_of reverse join
any all each insert remove` (`insert(i, v)` and `remove(i)` change the
slice in place and throw RangeError for an index out of range; `remove`
returns the element); maps have `keys values has get set delete`; sets have
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

### 11.12.1 HTTPS

An `https://` URL is fetched over TLS 1.2 (std/tls, 11.18), on port 443
unless the URL names another, and the server's certificate is **verified**
— against the system's CA bundle, for the URL's host — before a byte of the
request is written. A certificate that fails is an error; an `https` URL is
never fetched in the clear and never downgraded to `http://`. The client's
`tls` field is the std/tls `Config` it uses:

```voila
var c = http.client()
c.tls.ca_file = "corp-ca.pem"        // trust a private authority instead
let r = try c.get("https://intranet.example/status")
```

`c.tls.server_name` names the host to verify when the URL's is not the
certificate's (an IP address, say), and `c.tls.insecure = true` turns
verification off — for a test against a throwaway certificate, never for
anything real.

A server speaks HTTPS from `http.listen_tls(addr, cert_file, key_file)`:
the certificate chain and the key are PEM files, loaded (and checked) when
it starts, and every verb — `serve_one`, `accept`, `serve`, `serve_for` —
works on it unchanged. Under `serve` each connection's handshake runs on
that connection's own task, so a slow or hostile client delays nobody else.

### 11.12.2 Headers, URLs and chunked bodies

`Headers` is an ordered LIST of `Name: value` pairs, not a map: a header may
legally repeat (`Set-Cookie`), and the order is stable across runs. Lookup is
case-INSENSITIVE throughout — `get has all remove set` all match with
`str.equal_fold`, so `Content-Length` and `content-length` are one header.
`add` keeps what is there; `set` replaces it in place.

`http.parse_url` yields `URL{scheme, host, port, path, query}` with the port
filled in (80 when the URL omits it, 443 for https), the fragment dropped (it is never sent),
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

## 11.13 The std/ffi Package

`std/ffi` calls C and reaches raw C memory. **Everything that goes through
it is outside the safety guarantees**: a wrong prototype, a pointer used
after its block was freed, a C function that writes past a buffer — none is
caught. It is therefore marked: every `use "std/ffi"` draws the warning
`ffi`, which is off by default, printed by `-Wall`, and fatal under
`--strict` (12.6), so `voila check --strict` refuses a program that uses it.

```
   >>──FFI──"C"──{──┬───────────────────────────────────┬──}────────────><
                    ├──LINK──"library"──────────────────┤
                    └──FUNC──name──(──params──)──ctype──┘
```

```voila
use "std/ffi"

ffi "C" {
    link "z"                                   // the build adds -lz
    func strlen(s *char) usize
    func zlibVersion() *char
    func compress(dst *void, dstlen *void, src *char, n ulong) int
    func snprintf(buf *void, n usize, fmt *char, args ...) int
}

func main() {
    say strlen("hello"), zlibVersion()
    let buf = ffi.alloc(64)
    defer ffi.free(buf)
    snprintf(buf, 64, "%s=%lld", "answer", 42)
    say buf.read_cstr(0)                       // answer=42
}
```

Each `func` of a block is a C prototype and, from then on, an ordinary
procedure of the package: called, typed and checked like any other. Its C
types fix the Voilà types a caller passes:

| C type | Voilà type |
|---|---|
| `char short int long` and the `u`/`unsigned` forms, `i8`…`u64`, `size_t usize ssize_t intptr_t off_t` | an integer: a narrow parameter is range-checked on entry; every integer result is `int` (`long` is 64 bits) |
| `float double f32 f64` | `float` |
| `bool` | `bool` |
| `*char` | `str` as a parameter — its bytes are lent, READ-ONLY, for the call: C must neither keep the pointer nor write through it (a buffer C fills — `strcpy`, `read`, `getcwd` — is a `*void` given an `ffi.alloc` block); a str holding a NUL byte is refused with ValueError, since C would read only up to it; `?str` as a result, copied at once, `nil` for NULL |
| any other pointer — `*void *u8 **char` … | `ffi.Ptr`; `nil` or `ffi.null()` is NULL |
| `...` | C varargs, at most six, by each value's run-time kind: a float travels as a double, an int as a 64-bit integer (so `%lld`, not `%d`, for an int wider than 32 bits), a str or a `Ptr` as an address |

`link "name"` hands `-lname` to the C compiler; a program with no `link`
still depends on libc alone. The prototype is bound to the C symbol under a
name of the compiler's own, so it never clashes with a declaration the
system headers make, and a C function whose name is a Voilà keyword cannot
be declared. There are **no callbacks** from C into Voilà.

**Raw memory.** `ffi.alloc(n)` is `n` zeroed bytes, freed with
`ffi.free(p)`; `ffi.cstr(s)` and `ffi.from_bytes(b)` are allocated copies.
A `Ptr` is `{addr, size}`: the size is known for a block this package
allocated and `-1` for a pointer C returned. Its methods read and write at a
byte offset — `read_i8 read_u8 read_i16 read_u16 read_i32 read_u32
read_i64 read_u64 read_f32 read_f64 read_ptr read_cstr read_str(off, n)
read_bytes(off, n)` and the `write_` forms — and every one is checked
against the size when it is known (RangeError), and against NULL
(RuntimeError). `offset(n)` moves the pointer; `ffi.sizeof("int")` gives a
C type's size, which is how a struct's field offsets are written down —
there is no struct layout syntax. `ffi.free` accepts only a live block
`ffi.alloc` returned, and aborts on anything else (a double free, memory C
allocated, an offset into a block), because a bad free corrupts the
allocator for the whole process.

## 11.14 Collections: std/slices, std/maps, std/heap, std/chan

All four are Voilà source, generic in their element types, and none
changes its argument: a function that "edits" a slice returns a new one.
The in-place forms are slice methods (`xs.insert(i, v)`, `xs.remove(i)`,
`xs.reverse()`, `xs.sort()`), which a `var` may call.

```voila
let xs = [5, 1, 4, 1, 5, 9]
slices.binary_search(xs.sorted(), 4)    // (2, true): index, found
slices.compact([1, 1, 2, 1])            // [1, 2, 1] — runs collapsed
slices.unique(xs)                       // [5, 1, 4, 9] — first of each
slices.chunk(xs, 4)                     // [[5, 1, 4, 1], [5, 9]]
maps.keys_sorted(m)                     // keys in order, whatever built m
var q = heap.new[int]()                 // smallest first; new_by(less)
```

Ordering is the language's own (`<`, or a type's `cmp`, 7.3.3), so
`binary_search`, `min`, `max` and a default heap work on anything `sort`
can order; equality is `==`. A map iterates in insertion order, so every
`std/maps` result is deterministic.

`chan.tee(src)` copies every value to two outputs, each with a queue of its
own without bound, so the two may be drained at different speeds or one
after the other — at the cost of holding what the slower one has not read.

`std/chan` builds pipelines. Each function that returns a channel SPAWNS the
task that feeds it and returns at once; that task belongs to the caller's
group (10.1), so the group does not finish until it has — and it finishes
when its input is closed. The other side of that rule: a pipeline nobody
drains never finishes, and its group waits for it forever — `let c =
chan.from(xs); say <-c` hangs at the end of `main`. Consume what you start:

```voila
group {
    let sq = chan.map(chan.from([1, 2, 3]), fn(n int) int { return n * n })
    say chan.collect(sq)                          // [1, 4, 9]
    let all = chan.merge([a, b])                  // fan-in
    let workers = chan.fan_out(jobs, 4)           // fan-out
}
```

`chan` is a keyword, and the package's qualifier is spelled the same:
directly before a `.` the word is the qualifier (the channel type and its
constructor are always `chan[`). `chan.limiter(every, burst)` is a
token-bucket `Limiter` (`wait()`, `allow()`), and `chan.throttle(src,
every)` the same applied to a channel.

## 11.15 Processes and Signals

`os.start(prog, args)` runs a program WITHOUT waiting for it and returns a
process handle: `pid()`, `wait()` (the exit status, or 128 + the signal
that ended it — the convention of `os.run`; it may be called again),
`running()`, `kill(sig)` (SIGTERM by default; safe while another task is
in `wait()`), and `stdin()` and
`stdout()`, the child's standard input and output as a std/io writer and
reader. Its standard error is the parent's. `wait` closes the child's stdin
first, so a filter that reads to the end of its input finishes; `kill` may
be called while another task waits. From the first `os.start` on, SIGPIPE
is ignored by this process (unless the program installed its own
disposition), so writing to a child that has exited is an IOError rather
than the end of the program — the child itself gets the default back.

```voila
var p = must os.start("sort", [])
must p.stdin().write("pear\napple\n")
p.stdin().close()
each line in p.stdout() { say line }      // apple, pear
say must p.wait()                          // 0
```

`os.signals(os.SIGINT, os.SIGTERM)` returns a channel that receives each
listed signal's number as it arrives, which a task reads with `<-`, `each`
or `select`. The signal handler itself is async-signal-safe — it only
writes the number to a pipe, and a dispatcher thread delivers it — and a
signal that arrives while the channel's buffer is full is dropped (a burst
of SIGINTs is one request to stop). Once a signal is subscribed it no longer
terminates the process. `os.signal_stop(ch)` ends a subscription and closes
its channel; a signal nobody subscribes to any more goes back to its
default action. `os.kill(pid, sig)` sends a signal to one process and
refuses a pid of 0 or less — which kill(2) would read as a whole process
group, or every process the program may signal; `os.kill_group(pgid,
sig)` signals a group by name. `os.getpid()` is the process's id. The
`os.SIG…` constants are the platform's numbers.

`std/http`'s `Server.serve(handler, stop)` answers each connection on its
own task until a value arrives on `stop`, then closes the listener and
waits for the requests in flight before it returns — so
`srv.serve(app, must os.signals(os.SIGINT, os.SIGTERM))` is a server that
shuts down gracefully on Ctrl-C. `serve_for(handler, stop, grace)` bounds
that wait: a connection still open `grace` after the stop is shut down
(`Conn.shutdown`). Each handler is called under a RECOVERY POINT (9.5): an
abort in it — an index out of bounds, a field of nil — answers that one
request 500 and is reported on standard error, and a throw answers 500
too; neither stops the server.

`os.walk(root)` lists every file and directory below `root`, depth first and
sorted, without following symbolic links, and fails on a directory it
cannot read; `os.walk_all(root)` carries on past one and returns `(entries,
skipped)`, a "path: reason" line per skipped directory (only an unreadable
root is an error). `os.glob(pattern)` is the paths a shell pattern matches,
sorted (no match is an empty list). Every file, socket and pipe the
runtime opens is close-on-exec, so none of them leaks into a child.

## 11.16 Typed JSON

`json.decode[T](s)` builds a `T` from the document, all the way down:
nested structures, slices and sets of them, maps (a non-`str` key is parsed
from the key text), `?T` (null, or a field that is absent), enumerations (a
unit variant is its name, `"Red"`; one with a payload is
`{"Circle": {"r": 1.5}}`), newtypes (their underlying value) and `Box[T]`.
A field the document lacks is an error unless its type is `?T` or it
declares a default; keys the structure does not declare are ignored. A
mismatch is an error VALUE whose message is the JSON path:

```voila
struct Limits { max_mb int }
struct Config { name str, limits Limits, tags []str, nick ?str }
match json.decode[Config](`{"name": "svc", "limits": {"max_mb": "big"}}`) {
    Ok(c)  => say c.limits.max_mb
    Err(e) => say e.message()     // limits.max_mb: expected int, got string
}
```

The parser is strict (RFC 8259): trailing text, a trailing comma, a bare
control character in a string, a leading zero, or an exponent beyond
±999999999 is `bad JSON at line L, column C: …`, and a number too large
for a `float` field is an error rather than an infinity. A map's keys may
be of any type `decode` can read — they are written as their text (`80`,
`true`, `Red`) and read back from it. `json.encode` is the exact inverse of `decode[T]`, so a value
survives the round trip (a float that is NaN or infinite, which JSON cannot
spell, encodes as `null`); a value nested deeper than 512 levels is a
ValueError rather than a stack overflow. There is no field-renaming tag: the specification
has no annotation syntax, so a JSON key is the field's name.

## 11.17 The std/flag Package

```voila
use "std/flag"
use "std/time"

func main() {
    flag.str("name|n", "world", "who to greet")
    flag.int("count", 1, "how many times")
    flag.bool("loud|l", false, "shout it")
    flag.dur("pause", time.Second * 0, "wait between greetings")
    let opts = flag.parse()
    each i in 0..opts.int("count") { say "hello,", opts.str("name") }
    say opts.args()                  // the positional arguments
}
```

A flag has a NAME — with an optional one-letter alias after a bar,
`"name|n"` — a DEFAULT, whose type is the flag's (`str int bool float
dur`), and a line of HELP. Accepted: `--name=value`, `--name value`, `-n
value`, `-n=value`; a bool alone is true, and `--name=false` or
`--no-name` sets it false; `--` ends the flags; flags and positional
arguments may be interleaved. A duration is written as it prints: `90s`,
`1m30s`, `250ms`. `flag.parse()` parses `os.args()`: `--help` and `-h`
print the usage (generated from the declarations) and exit 0, and an
undeclared flag, a missing value or a value of the wrong type prints the
error and the usage to standard error and exits 2. `flag.set(prog,
about)` is an independent Set with the same methods whose `parse(args)`
returns an error VALUE instead — for tests and subcommands.

**`os.args()` does not include the program's name.** Its first element is
the first argument after it, which is unlike C's `argv[0]` and Go's
`os.Args[0]`; so the whole of `os.args()` is what is parsed. The usage line
names the executable's base name (`flag.program(name)` sets it — under
`voila run` the binary is the run cache's).

## 11.18 The std/tls Package

`std/tls` puts TLS 1.2 under a `std/net` connection. It is built on
BearSSL, vendored in the toolchain (12.2), so it needs no OpenSSL and the
program still links libc alone.

```voila
use "std/tls"

func main() {
    var conn = must tls.connect("example.com:443", tls.Config{})
    must conn.write_all("HEAD / HTTP/1.1\r\nHost: example.com\r\n" ||
                        "Connection: close\r\n\r\n")
    say must conn.read_line()
    conn.close()
}
```

A TLS connection **is** a `net.Conn`. Once the handshake is done the session
sits beneath the socket, and `read`, `read_line`, `write_all`, `io.reader`,
`set_timeout` and `close` all work through it — which is why std/http
speaks HTTPS without a line of TLS of its own. `tls.client(conn, cfg)`
upgrades a connection that is already open; `tls.version(conn)` says
`"TLS1.2"`, or `""` for a plain one.

`tls.Config` has six fields, all defaulted, so `tls.Config{}` is a
verifying client:

| Field | |
|---|---|
| `server_name` | the name to verify and send as SNI (default: the host of the address) |
| `ca_file` | a PEM bundle to trust INSTEAD of the system's |
| `insecure` | skip verification — tests only |
| `cert_file`, `key_file` | a server's PEM certificate chain (leaf first) and private key |
| `timeout_ms` | bounds the handshake and, after it, each read and write (30000) |

### 11.18.1 Verification

A client checks three things: the server's chain leads to a trusted
authority, every certificate is within its dates, and the leaf names the
host. The authorities are the platform's bundle — `$SSL_CERT_FILE` when set,
else the first of `/etc/ssl/cert.pem` (macOS, the BSDs, Alpine),
`/etc/ssl/certs/ca-certificates.crt` (Debian, Ubuntu),
`/etc/pki/tls/certs/ca-bundle.crt` (Fedora, RHEL) and the other usual
places; `tls.system_ca_file()` says which — or `ca_file`. Each failure says
which check failed:

```
tls: certificate is not trusted (unknown certificate authority)
tls: certificate does not match host name "example.org"
tls: certificate has expired or is not yet valid
```

On top of BearSSL's checks of the chain, std/tls applies the rules the
rest of the Web PKI expects, as Go's crypto/x509 does: the certificate must
carry a subjectAltName (a Common Name alone is refused); a name is matched
against its DNS entries, where a wildcard is only a whole left-most label
with at least two labels after it (`*.example.com` matches `a.example.com`,
never `a.b.example.com`, and `*.com` matches nothing); a host that is an IP
address is matched against its IP-address entries (and is not sent as
SNI); an extendedKeyUsage, when present, must allow serverAuth; and no
certificate the server sends may be signed with SHA-1. Neither side
renegotiates. A server name is checked before it is used at all: a NUL,
a `*`, an empty label, a non-ASCII byte (use the IDNA `xn--` form) or any
character no host name has is refused. `insecure: true` turns all of it off; the connection is then
encrypted but not authenticated, which is what TLS exists to prevent — use
it against a throwaway test certificate and nowhere else.

### 11.18.2 Servers

```voila
var lis = must tls.listen("127.0.0.1:8443",
                          tls.Config{cert_file: "srv.pem", key_file: "srv.key"})
var conn = must lis.accept()          // TCP accept, then the handshake
```

The key is a PEM RSA or EC private key — PKCS#1, SEC 1 or PKCS#8, not
encrypted. `listen` (and `tls.check_server(cfg)`) load both files at once, so
a bad one is reported at start-up rather than by the first client; they are
cached, and read again when either changes, so a renewed certificate is
picked up without a restart. `tls.server(conn, cfg)` handshakes a connection
accepted some other way — do it on the connection's own task, as std/http's
`serve` does, so one slow client cannot stall the accept loop.

### 11.18.3 Limits

A peer that ends the connection WITHOUT TLS's own close (close_notify) is
an error, not an end of stream — anyone on the path can cut a TCP
connection, and a body read to its end would otherwise be silently
truncated. std/tls's own `close` always sends close_notify.

TLS 1.2 only: BearSSL 0.6 has no TLS 1.3, and servers still offer 1.2. No
client certificates, no session resumption, no revocation checking. A
session is a single state machine, so tasks sharing one connection take
turns — a read in progress makes another task's write wait. HTTP/1.1, which
alternates, is unaffected; a protocol that reads and writes at once on two
tasks wants two connections.

## 11.19 The std/compress Package

`std/compress` is DEFLATE in its three framings, zip and tar. The codecs are
miniz, vendored (12.2); tar is plain Voilà. Data is a `str`, which is a
byte string — `bytes` and `from_bytes` convert.

```voila
use "std/compress"

func main() {
    let text = str.repeat("all work and no play\n", 1000)
    let packed = must compress.gzip(text)
    say len(text), "->", len(packed)
    say must compress.gunzip(packed) == text
}
```

| Encode | Decode | Framing |
|---|---|---|
| `deflate(data, level)` | `inflate(data, max)` | raw DEFLATE (RFC 1951) — what zip uses inside |
| `zlib(data, level)` | `unzlib(data, max)` | zlib (RFC 1950): header and Adler-32 |
| `gzip(data, level)` | `gunzip(data, max)` | gzip (RFC 1952): header, CRC-32 and length |

`level` is 0 (store) to 9 (smallest), default 6. A decoder is **strict**:
the input must be exactly one stream — for gzip, one or more members, as
`gunzip` accepts — with nothing after it, and every checksum must hold.
Truncated, corrupt or trailing data is an `IOError` naming the problem; no
input makes a decoder crash. gzip output has no file name and a zero
timestamp, so equal input always gives equal output. `crc32(data, crc)` and
`adler32(data, adler)` are the checksums, continuable a piece at a time.

### 11.19.1 Decompression bombs

A few kilobytes can inflate to gigabytes. Every decoder therefore takes
`max`, the most bytes it will produce — `compress.DEFAULT_MAX` (256 MiB)
unless given, `0` for no limit — and stops with an error the moment its
output would pass it:

```voila
match compress.gunzip(upload, 1048576) {
    Ok(data) => process(data)
    Err(e)   => say e.message()   // compress: gzip output exceeds the limit of 1048576 bytes
}
```

### 11.19.2 Streams

Each framing is also a stream: an `io.Writer` that compresses what it is
given into another writer, and an `io.Reader` that decodes what it reads
from another reader — so `read_line`, `read_all`, `each` and `io.copy`
work on them.

```voila
var w = must compress.gzip_writer(must io.writer(must os.create("log.gz")))
each line in lines { must w.write_all(line || "\n") }
must w.close()                     // the final block and the trailer

var r = must compress.gunzip_reader(must io.reader(must os.open("log.gz")))
each line in r { say line }
```

A writer **must be closed**: `close` writes the final block and, for gzip,
the trailer, and a file without them is truncated. Closing it does not
close the writer underneath. `flush()` makes everything written so far
decodable by the far end, at a few bytes' cost. The readers take `max`.

### 11.19.3 zip

```voila
let z = must compress.zip_open(must os.read("site.zip"))
each e in z.entries { say e.name, e.size, e.is_dir }
let page = must z.read("index.html")
say must z.extract_to("out")      // files written

var w = must compress.zip_create()
must w.add("hello.txt", "hi\n")
must w.add_dir("docs")
must os.write("new.zip", must w.finish())
```

Archives are read from, and built in, memory. A `ZipEntry` has `name size
compressed is_dir crc32 mtime mode is_link`. Reading verifies each member's CRC-32; an
encrypted member, or one compressed other than by store or deflate, is
refused by name. A member's DECLARED size is checked against `max` before
anything is inflated, and extraction never writes past it. `extract_to`
checks the WHOLE archive before writing anything: a name that would land
outside the directory — an absolute path, a `..` component, a backslash,
the "zip slip" — a symbolic link or a device, a name that is both a file and
another's directory, declared sizes totalling more than `max`, and then
every member's data and CRC, inflated to nowhere — a second pass inflates
each member straight into its file, so memory stays at one buffer. File
and directory clashes are found case-insensitively ("D" and "d/x.txt"
are one path on macOS), and so is anything already in the destination
that is in the way (a directory where a file goes). Files are written
under temporary names and renamed into place only when all of them are
written. So a hostile or broken ARCHIVE writes nothing; the filesystem
failing part-way (a full disk) can still leave the directories made so
far, and a symbolic link already inside the directory is followed — extract
into one of your own. A zip member whose recorded Unix mode has no file
type (as Python's zipfile writes) is a regular file.
`compress.safe_join(dir, name)` is the same name check for a program that
extracts by hand, and `compress.extract(members, dir)` the same writer. `add(name, data, level, mtime)`: `mtime` is Unix seconds,
0 for now (zip stores local time, to two seconds).

### 11.19.4 tar

```voila
let tarball = must compress.tar_write([
    compress.TarEntry{name: "notes", kind: "dir", mode: 493},
    compress.TarEntry{name: "notes/a.txt", data: "hello\n"},
])
must os.write("notes.tar.gz", must compress.gzip(tarball))
each e in must compress.tar_read(must compress.gunzip(must os.read("notes.tar.gz"))) {
    say e.kind, e.name, len(e.data)
}
```

A `TarEntry` is `name data kind mode mtime link`, where `kind` is `"file"`,
`"dir"`, `"symlink"`, `"link"` (hard) or, read from an archive, `"other"`.
Writing is POSIX ustar: a name of up to 100 bytes, or up to 255 split at a
`/`; a longer one is refused. Reading also understands GNU's long-name
records and POSIX pax headers, so archives from GNU tar and bsdtar read
correctly. Every header's checksum is verified, every size checked against
what is there, and the total bounded by `max`.
`compress.tar_extract(bytes, dir, max)` writes an archive out with every
protection `extract_to` has: links, devices and unsafe names are refused,
and nothing is written unless the whole archive passes. Permission bits
are not applied.

## 11.20 The std/sqlite Package

`std/sqlite` is SQLite, vendored (12.2): an SQL database in a file, or in
memory, with nothing to install and nothing beyond libc to link.

```voila
use "std/sqlite"

struct User {
    id   int
    name str
    age  int = 0
}

func main() {
    let db = must sqlite.open(":memory:")
    defer db.close()
    must db.exec("CREATE TABLE users (id INTEGER PRIMARY KEY, name TEXT, age INTEGER)")
    must db.run("INSERT INTO users (name, age) VALUES (?, ?)", "ada", 36)
    must db.run("INSERT INTO users (name, age) VALUES (?, ?)", "bob", nil)
    each row in must db.query("SELECT id, name, age FROM users ORDER BY id") {
        say row.int("id"), row.str("name"), row.is_null("age")
        let u = must sqlite.decode[User](row)
    }
}
```

| | |
|---|---|
| `open(path)`, `open_readonly(path)` | a `DB`; `":memory:"` is private and in memory |
| `db.exec(sql)` | a statement, or a whole script of them, with no parameters and no rows |
| `db.run(sql, args...)` | ONE statement; returns the rows it changed |
| `db.query(sql, args...)`, `db.query_one(...)` | its rows, as `[]Row`, or the first as `?Row` |
| `db.prepare(sql)` | a `Stmt`: `bind next run query reset close`, and `cols` |
| `db.transaction(body)` | `body` in BEGIN … COMMIT, rolled back on an error or a throw |
| `begin commit rollback in_transaction` | the same by hand |
| `last_insert_id changes set_busy_timeout close` | |

### 11.20.1 Parameters and columns

Parameters are `?`, `?NNN` or `:name`, bound by position. A value is an
`int`, `float`, `bool` (1 or 0), `str` (TEXT), `[]byte` (BLOB), `dec` (its
exact TEXT — a REAL would round it) or `nil` (NULL): anything that
implements `sqlite.Arg`. Their number must match the statement's; a missing
or an extra one is an error before the statement runs.

A column is a `sqlite.Value`, SQLite's five storage classes:

```voila
match row.get("age") {
    sqlite.Value.Null    => say "unknown"
    sqlite.Value.Int(n)  => say n
    _                    => say "something else"
}
```

`sqlite.decode` takes one `Row`; given anything else — a `[]Row` from a
query — it returns an error rather than aborting.

`row.int(col)`, `float`, `str` and `bytes` read a column the way SQLite
converts one — NULL is the zero value. `row.at(i)`, `row.map()`,
`row.index(col)` and `row.json()` are there too; an unknown column name
throws `KeyError`. `sqlite.decode[T](row)` builds a `T` from a row, column
names as field names, by `json.decode[T]`'s rules (11.16): a field with a
default may be missing, an unknown column or a value of the wrong kind is
an error that names the field.

### 11.20.2 Errors and transactions

Every failure — a syntax error, a violated constraint, a locked database —
is an `IOError` value whose message is SQLite's own, prefixed `sqlite:`
(`sqlite: NOT NULL constraint failed: users.name`). `prepare` (and so `run`
and `query`) accepts exactly ONE statement; `exec` is the verb for a
script.

`db.transaction(fn() unit! { … })` commits when the function returns `Ok`
and rolls back when it returns an error — which `transaction` returns — or
throws, when the exception is rethrown after the rollback. `begin` is
`BEGIN IMMEDIATE`: it takes the write lock at once, so a transaction that
will write fails, or waits, at its start rather than halfway through.

### 11.20.3 Tasks

Tasks are threads (10.1), and SQLite is built SERIALIZED: every call on a
connection holds that connection's lock, so a `DB` and its statements may
be used from any task without corrupting anything.

A transaction TAKES its connection. SQLite has one transaction per
connection, so from `begin` (or the start of `transaction`) until the
commit or the rollback, every other task's call on that connection waits —
it would otherwise run inside the transaction, and be rolled back with it.
The waiting is real: a task holding a transaction must not wait for a task
that is waiting for the connection. Transactions do not nest. `transaction`
rolls back on every way out — an error, a throw, or a fault a recovery
point contains (an http handler's) — and a task that ENDS holding a
transaction it began by hand has it rolled back, and the connection given
back, as its thread exits. `in_transaction()` never waits: it says whether
THIS task holds the connection's transaction. The gate isolates calls,
not iterations: a `Stmt` stepped with `next()` across another task's
transaction on the same connection can see that transaction's uncommitted
writes — use a separate connection for isolation. A transaction is one
task's: a task it spawns cannot use the connection until the transaction
ends, so one spawned in a `group` the transaction waits for fails at once
with a `deadlock` error instead of hanging — pass the work back, or give
the other task its own connection. Two tasks stepping one
`Stmt` still interleave its rows. For concurrent writers the pattern is a
connection per task: `sqlite.open` the same
file in each — connections wait up to five seconds (`set_busy_timeout`) for
each other's locks, and SQLite's file locking keeps them consistent. For a
file shared by many writers, `PRAGMA journal_mode = WAL` lets readers
proceed while one writes.

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

The three packages built on vendored C libraries — std/tls (BearSSL),
std/compress (miniz) and std/sqlite (SQLite) — are compiled into a program
only when it uses them. The first such build compiles the library, which
takes a moment (SQLite, the largest, about six seconds), and caches it in
`~/.cache/voila` beside the runtime; every later build links the cached copy.
None of them needs anything but libc, so `otool -L` still shows libc alone.

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
| `ffi` | A `use "std/ffi"`: what goes through it is outside the safety guarantees (11.13). Off by DEFAULT, so a plain `voila check` of a program that calls C says `ok` and nothing else; `-Wall` prints it and `--strict` makes it fatal, which is how `voila check --strict` refuses a program that calls C or touches raw memory. |
| `type-mismatch` | Retired. It named the first, partial version of the expression type check, which reported warnings; that check is now the static type check of Section 4.8 and its findings are **errors**. The code is still accepted on the command line, so that a build script naming it keeps working, and nothing is reported under it. |

`unused-import` and `unreachable` are on by default. Warnings never change
the code that is emitted — but
the option state *is* part of the `voila run` cache key, so a `--strict` run is
never answered with the binary a lenient one left behind.

## 12.7 Completion Codes

See Appendix C.

## 12.8 Authoring a Standard Package in Voilà

`std/net` (11.5) is the first standard package written in Voilà rather than
in C; `std/sync` (11.7), `std/path`, the calendar half of `std/time` (11.8),
`std/test` (11.9), `std/csv` (11.10) and `std/http` (11.12) are the next six,
and `std/iter`, `std/slices`, `std/maps`, `std/heap`, `std/chan`, `std/flag`
and `std/ffi` (11.13–11.17) followed. A package that a program reaches with `use "std/X"` may
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

**A package over a vendored C library** — std/tls, std/compress and
std/sqlite (11.18–11.20) — differs in steps 2, 3 and 7. Its natives must not
be in libvoila, or every program would link the library; so the shim is a
runtime EXTENSION, `runtime/src/ext/X.c`, beside the library's source in
`runtime/src/vendor/`, and its natives sit in that file's own table,
`const VlExtEntry vl_ext_X[]`, not in std.c's. `low.VendorOf` maps the
package's natives to the extension; when a program calls one, the generated
C carries a `/* voila-vendor: X */` line and registers the table in `main`,
and the driver compiles the extension into `~/.cache/voila/ext-X-<key>/` on
first use and links it before libvoila. The extension list is `EXTENSIONS`
in `tools/build.voi`, which regenerates `voilac/rt/vendor.voi` (the sources
the compiler carries) as it does the runtime's blob. An extension hands a
resource to Voilà as a `VlHandle` (runtime/src/voila_int.h), whose ops a
reader, a writer or a socket filter can drive.

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

Reserved for future use: `async await macro unsafe`.
Contextual (not reserved): `fallthrough by upper lower digits timeout
fn cell where`. (`where` opens a bound clause, 7.4, only as `where NAME
:` after a declaration's signature.)

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
reachable, and answers nil once it has been freed — a handle shares a small
control block with its target, never the target itself, so it outlives the
value safely. Destruction timing for shared values held only by a
temporary is not guaranteed, and that is the only way a program could
observe the difference.

**D.6 — What the compiler refuses.** One semantic is not expressible
in the generated C, and the compiler **refuses** the program rather than
produce a binary that would behave differently from the specification:
the implicit `ctx` of a task.

(Two more items used to stand in this list. A closure that captures a
variable declared inside a loop now receives a per-iteration copy of
the loop variables it names (Section 6.3), which is exactly a fresh
binding per iteration; only a loop variable that is reassigned after
the copy is refused, and that is diagnosed by `voila check` with its
position and name. `parse` into a variable captured from an enclosing
procedure is translated through a scratch register and written back.)

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
provide — `std/vm`, `std/xml`, `std/yaml` — are refused at the `use`, not
silently mistranslated, so a program that names one fails to compile rather
than at run time. (`std/regex`, `std/http`, `std/chan`, `std/slices`,
`std/maps` and `std/ffi` were in this list until they shipped; see Sections
11.12 to 11.14.)

Values are boxed in the default translation. Under `-O3` (Section 12.5)
the toolchain converts provably-typed INT, FLOAT, and BOOL registers to
machine arithmetic and elides frame allocation; measured gains reach
40 percent on arithmetic-bound programs. Procedure parameters and
values that cross procedure boundaries remain boxed — C-class arithmetic
throughout is future work, not a present property. The static type check
(Section 4.8) does not feed the `-O3` inference: a type it infers is a
fact about the program text, but its UNKNOWN is not a proof, and a
parameter arrives boxed by the calling convention whatever its declared
type, so the register-level inference keeps its own, flow-insensitive,
proof.

**D.7 — Reference cycles through closures.** The Acyclicity Rule (Section
8.4) governs the type graph and makes owning cycles among *values*
impossible. A **closure** that captures the procedure frame in which it
is itself stored is outside that rule; the runtime breaks it when the
procedure completes: every register of a captured frame that no closure
reads through an upvalue is dropped then — a closure held directly, or
inside a structure or a collection (`return H{less: fn(a, b) …}`). A
closure that another, escaped closure still reads (`let h = fn…; return
fn() { h() }`) is kept, and the cycle it forms with the frame is broken
when the last outside holder lets the frame go — or, for a closure that
refers to itself (`var fib = …; fib = fn(n) { … fib(n - 1) … }; return
fib`), when the last holder of the closure lets it go. The closure may
be held by the register directly or inside a structure, enum, tuple,
`Box`, slice or map that nothing else holds (`var h = H{…}; h.f = fn(n)
{ … h.f(n - 1) … }`, `let b = Box2{f: fn…}; return fn() { b.f() }`); the
runtime looks at most 256 values deep into such containers. Two cases
are not detected and retain storage until the process ends: a closure
buried deeper than that, and a container that refers to itself through
its own closure and is *returned* (`var h = H{…}; h.f = fn…h.f…; return
h`) — the copy the caller holds shares the container, and its release
gives the runtime no frame to examine.

**D.8 — Identifiers outside ASCII.** The specification says an
identifier begins with a letter. The lexer accepts any byte above 127 in
an identifier rather than consulting the Unicode letter tables, so a
non-letter such as `€` is admitted where the specification would reject
it. Identifiers in the standard library and in the compiler are ASCII.

**D.9 — Not yet implemented.** `voila mod tidy`, `voila fmt/test/doc/bench/repl`,
`std/vm`, `std/xml/yaml`, signatures in `std/crypto`, checked `throws`
verification, and the full flow-sensitive borrow checker (a straight-line
use-after-move analysis is provided). Cross-compilation is by C: `voila build
--emit=c` produces a translation unit that any C compiler for any target can
build (see `build_voila.bash`). The unimplemented constructs parse correctly
and are diagnosed where semantics are missing.

**D.10 — Copy semantics of structures.** Resolved. Structures,
slices, maps, sets and Boxes have value semantics (Section 8.2) wherever
they are held, copied lazily on the first write while another holder still
shares the value. A register the program will not read again is dropped
as soon as it is dead, and a block's bindings at the block's end, so that
neither counts as a holder; a binding whose value may hold a `shared`
handle is kept to the end of its scope, as §6.3 requires of a weak
reference's target.

**D.11 — Static methods through a type parameter.** `T.m()` for a static
trait method is available in a top-level generic procedure only (7.4):
a method's, an IMPL block's and a trait's own type parameters carry no
type descriptor, because a method is reached by dynamic dispatch and its
caller cannot know which body, and so which descriptors, it will reach.
For the same reason a procedure that takes descriptors cannot be used as
a value. A descriptor read from a value argument is the value's RUN-TIME
type, looking through a Box: `like(Box(p))` reaches `P`'s method, not
`Box`'s. (An argument whose static type is a sized integer passes that
type instead.) A static call through a parameter takes positional
arguments only; a named one is refused. In a trait default, `Self` is the
type the call was dispatched for — for a receiver whose static type is a
sized integer, that type — but inside a generic body a value of a type
parameter is dispatched by its run-time type, so a `u8` there reaches
`int`'s method.

**D.12 — The foreign function interface.** The specification shows an
`ffi` block and no more. This release adds `link "name"` inside the block
for the libraries a program links, maps C types to Voilà types as in 11.13,
passes at most six variadic arguments, and has no callbacks from C and no
struct layout syntax (a struct is memory at offsets). The prototype is
trusted: a wrong one is undefined behaviour, which is what the `ffi` mark
says.

**D.13 — How a decimal prints.** A `dec` keeps the scale it was written
or computed with (`1.50d * 2` is held as 300 × 10⁻², that is `3.00`), but
`say`, `str` and `%v` print it NORMALIZED, without trailing fractional
zeros: `1.00d` prints as `1`, `1.50d` as `1.5`. REXX would print `1.00`. The frozen goldens of this
release depend on the normalized form, so it stays; use `x.format(2)` or
`%.2f` (both exact, half-even) for a fixed number of places.

**D.14 — Aborts under a recovery point.** §8.6 makes every abort final.
This release contains the language-level ones inside std/http's handler
calls (9.5) — a server's one defective request should not end every
other request — and nowhere else; a program cannot open a recovery point
of its own.

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
