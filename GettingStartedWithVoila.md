<div align="center">

# Voilà

## Getting Started

### A Programming Guide for the New User

**Program Number 5799-VLA · Version 0.4.2**

*First Edition*

</div>

---

## About This Guide

This guide teaches the Voilà programming language to a programmer who has
never used it. It assumes you can read a program in some other language —
COBOL, PL/I, REXX, C, Java — and that you have access to a system with a C
compiler.

It is a *tutorial*, not a reference. It shows one way to do each thing,
and it shows it working. Where the language offers a second way, the guide
says so and moves on. When you want the whole story — every clause of every
statement, every rule of the type system — read the
**[Programming Manual](programming_manual.md)**, which is the reference
publication for this language.

Every program in this guide is a real program. They are held in the
`samples/guide/` directory of the distribution, they are compiled and run
whenever the toolchain is tested, and the outputs printed here were produced
by running them.

### How This Guide Is Organized

| Chapter | Subject |
|---|---|
| 1 | Installing the compiler, and your first program |
| 2 | Using the compiler: RUN, BUILD, CHECK |
| 3 | Data items, arithmetic, and the SAY statement |
| 4 | Loops, conditions, and a printed report |
| 5 | PARSE and exact decimal arithmetic |
| 6 | Structures, enumerations, MATCH, and errors |
| 7 | **The eight queens** — a backtracking search |
| 8 | Tasks, channels, and structured concurrency |
| 9 | **The eight queens, in parallel** |
| 10 | **A network service** — sockets and structured concurrency |
| 11 | Dividing a program into packages |
| 12 | What to read next |

### Notation

Program text and terminal output are shown in a monospace font. A line of
the form

```console
$ voila run hello.voi
```

is a command you type at your terminal; the `$` is the prompt and is not
typed. Text shown below such a command is what the system prints back.

**Programming Notes** call out a rule that surprises newcomers. There are
six of them in this guide, and each one is a mistake the author made while
writing it.

---

# Chapter 1. Your First Program

## 1.1 What Voilà Is

Voilà is a compiled, statically typed language. It is memory-safe without a
garbage collector, it has exact decimal arithmetic for money, and its
concurrency is *structured* — a task cannot outlive the block that started
it.

Its manners come from four ancestors:

| Ancestor | What Voilà took from it |
|---|---|
| **Rust** | ownership, moves, borrows — safety without a collector |
| **Go** | braces, packages, channels, `select` |
| **REXX** | `say`, `parse`, exact decimals, script form |
| **PL/I** | `numeric digits`, fixed-point discipline, the manuals |

The compiler is itself written in Voilà. This matters to you only in one
way: the language is large enough, and fast enough, to write a compiler in —
which is a stronger claim than any brochure could make.

## 1.2 Installing the Compiler

Because the compiler is written in Voilà, there are two ways to obtain a
working `voila`, and which one you use depends on whether you already have a
Voilà binary.

**If you already have a Voilà binary** (from a release, or a previous build)
of version **0.4.1 or newer**, that binary builds the compiler — Voilà builds
Voilà:

```console
$ ./build.sh
==> bootstrap: prebuilt binary  bin/voila  (0.4.1)
  ✓ fixpoint: C(voilac-1) == C(voilac-2)
  ✓ optimized fixpoint: C(-O3, gen1) == C(-O3, gen2)
  ✓ ./voila  ->  bin/voila-0.4.2
==> done — ./voila is ready
```

**On a fresh machine with only a C compiler** and no Voilà yet, there is a
checked-in copy of the compiler's own C output — the *seed*. `bootstrap.bash`
compiles it with `cc` into the first `voila` for your machine, then builds the
real compiler with it:

```console
$ ./bootstrap.bash
==> cc: bootstrap/voilac.c + runtime  →  the seed
  ✓ build/voila-seed
  ✓ fixpoint: C(voilac-1) == C(voilac-2)
==> done — ./voila is ready
```

You need `bootstrap.bash` only once per machine; afterwards `./build.sh` is the
faster, self-hosted path. (A C compiler is still required either way: Voilà's
backend emits C, and `voila build` calls `cc` to turn it into a native binary.)

What just happened, on both paths: a working compiler compiled the real
compiler from its Voilà source, and *that* compiler compiled itself once more.
The two generations emitted byte-identical C — the classical proof that a
self-hosted compiler is not quietly mistranslating itself. The binary is named
for its version — `bin/voila-0.4.2` — with `bin/voila` and `./voila` as
symlinks to it. Put `voila` on your PATH, or call it as `./voila`.

## 1.3 The First Program

Create a file named `hello.voi` containing exactly one line:

```voila
say "Hello, world!"
```

Run it:

```console
$ voila run hello.voi
Hello, world!
```

That is a complete program. There is no procedure division, no `main`, no
imports, no boilerplate. A file of loose statements is a **script-form**
program, and Voilà runs it from the top.

> ### Programming Note 1 — `say`, not `print`
>
> The output statement is `say`, from REXX. It writes its arguments
> separated by a blank and appends a newline. It is not a function, so it
> takes no parentheses:
>
> ```voila
> say "COUNT =", 12        // right
> say("COUNT =", 12)       // wrong — that is a call to a procedure named `say`
> ```

## 1.4 Procedure Form

A program of any size is written in **procedure form**, with a `main`:

```voila
func main() {
    say "Hello, world!"
}
```

The two forms are equivalent for a program this small. Use script form for a
one-page utility; use procedure form for everything else. From Chapter 6 on,
this guide uses procedure form exclusively.

---

# Chapter 2. Using the Compiler

## 2.1 The Commands

```
  voila run    <file.voi> [args…]   compile (cached) and run
  voila build  <file.voi> [-o exe]  compile to a binary
  voila build -O2 <file.voi>        the same, optimized (see below)
  voila build -S       <file.voi>   emit the assembly listing
  voila build --emit=c <file.voi>   emit the C
  voila check  <file.voi>           verify only
```

### RUN

```console
$ voila run samples/guide/g01_hello.voi
Hello, world!
```

`run` **compiles** the program and executes it. It does not interpret. The
first run of a program pays for a compilation — roughly a second — and the
binary is then cached under `~/.cache/voila`, keyed by a hash of every source
file that took part. The second run starts in about thirty milliseconds.

Editing any file of any package the program imports invalidates the cache
entry, as does rebuilding the compiler itself. You never have to think about
it.

### BUILD

```console
$ voila build samples/guide/g01_hello.voi -o hello
voila: built hello
$ ./hello
Hello, world!
$ otool -L hello                 # or: ldd hello
	/usr/lib/libSystem.B.dylib
```

The binary contains no compiler, no interpreter and no virtual machine. It
links the Voilà runtime and the C library, and nothing else. You can copy it
to a machine that has never heard of Voilà.

### CHECK

```console
$ voila check samples/guide/g05_types.voi
ok
```

`check` runs the whole front end — lexer, parser, package loader, and every
static check — and stops. It is what you bind to a key in your editor.

### The Optimizer

`voila build -O1`, `-O2`, or `-O3` (and `voila run -O2 file.voi`) ask for
an optimized binary; without the option you get the plain translation,
byte for byte, every time. The levels are cumulative — `-O2` elides frame
allocations, `-O3` turns provably-typed registers (int, float, and bool) into machine arithmetic —
and the guarantee is the one that matters: **the program's outputs, its
error signals, and even the text of its error messages are identical at
every level.** The compiler's own test procedure compiles its entire
conformance suite at `-O1`, `-O2`, and `-O3` and compares against the same
goldens. Arithmetic-heavy programs gain up to about 40 percent at `-O3`;
a program that mostly shuffles strings gains little. When the machine
that builds is the machine that runs, `--native` tunes for its processor.

## 2.2 What CHECK Rejects

The compiler refuses a program it cannot vouch for. It does not warn; it
refuses. These are the principal families of diagnostic:

| The compiler says | Because |
|---|---|
| `use of moved value `x`` | you used a value after giving it away (Chapter 6) |
| `type `Node` forms an ownership cycle …` | your data would leak; use `weak[T]` |
| `match on `Shape` is not exhaustive; missing cases: Rect` | you forgot a variant |
| `i64 → float may lose information` | the conversion is lossy; say so explicitly |
| `fmt.printf format needs 2 argument(s), call passes 1` | your format string lies |
| `borrow stored in struct field` | a borrow would outlive what it borrowed |
| `` `add` takes 2 arguments, but 1 is given`` | the call and the declaration disagree |
| `` `Point` has no field `z` `` | the composite names a field that does not exist |
| `unknown identifier `total`` | the name is declared nowhere in scope |

Each is a defect that would otherwise have reached production. `check`
runs the same front end as `build` — a program that checks clean builds,
and a program `build` would refuse, `check` refuses too.

## 2.3 The Assembly Listing

`voila build -S` prints the program's intermediate form as an assembler
listing, in the manner of a mainframe SYSPRINT: a heading block, the
constant pool, the structures as DSECT commentary, and one CSECT per
procedure with a location counter and the source line echoed at the right.

```console
$ voila build -S hello.voi
*------------------------------------------------------------------------------
*        VOILA VM ASSEMBLY LISTING
*        SOURCE: hello.voi
*
*        REGISTER-BASED LINEAR IR (SPEC #12). THIS IS THE FORM
*        THE C BACKEND CONSUMES; IT IS NOT EXECUTED DIRECTLY.
*        ENTRY ORDER: $INIT, $SCRIPT, MAIN. ^NAME DENOTES AN
*        UPVALUE OF THE ENCLOSING FRAME: A CLOSURE CAPTURES THAT
*        FRAME BY REFERENCE, NOT ITS VALUES BY COPY.
*------------------------------------------------------------------------------
*
*        CONSTANT POOL
K0      DC       STR    'Hello, world!'
*
$SCRIPT CSECT                                   * script-form top-level statem…
000000           LOADK    r0,K0                 * 1| say "Hello, world!"
000004           TOSTR    r1,r0
000008           SAY      (r1)
00000C           RET
*
        END
```

No line exceeds 79 columns. You will not often need this, but when you want
to know what the machine is really doing, it is there.

---

# Chapter 3. Data Items and Arithmetic

## 3.1 Declaring Data

```voila
let name    = "OPERATOR"        // cannot be changed
var count   = 0                 // can be changed
let balance = 1234.56d          // exact decimal — note the `d`
```

`let` declares a constant; `var` declares a variable. Prefer `let`. The type
is inferred from the value, but you may state it, and you must state it when
there is no initial value:

```voila
let year int = 2026
var total dec
```

## 3.2 The Elementary Types

| Type | Holds | Literal |
|---|---|---|
| `int` | a 64-bit signed integer | `42`, `0xff`, `0o755`, `0b1011`, `1_000_000` |
| `float` | binary floating point | `3.14159`, `6.02e23` |
| `dec` | **exact** decimal | `19.99d` |
| `str` | text (UTF-8) | `"Armonk"` |
| `rune` | one character | `'Q'` |
| `bool` | truth | `true`, `false` |

> ### Programming Note 2 — money is `dec`, never `float`
>
> `0.1 + 0.2` in binary floating point is not `0.3`. It is
> `0.30000000000000004`, and no amount of rounding at the edges will save a
> ledger built on it. Voilà gives you `dec`: base-ten, arbitrary precision,
> round-half-even, and the arithmetic a COBOL programmer expects.
>
> Write the `d`. `19.99` is a `float`; `19.99d` is a `dec`. The compiler
> will not silently mix them: `binary float → dec is lossy` is a diagnostic,
> not a warning.

## 3.3 Interpolation

Any expression inside braces in a string is evaluated and inserted:

```voila
say "The year is {year}; pi is about {pi}."
```

This is the program `samples/guide/g02_greet.voi`:

```voila
use "std/str"

let name    = "OPERATOR"
let planet  = "Earth"
let year    = 2026
let pi      = 3.14159
let balance = 1234.56d          // exact decimal, not binary floating point

say "Greetings, {str.title(str.lower(name))} of {planet}."
say "The year is {year}; pi is about {pi}."
say "Your balance is {balance} — and it is EXACT."

var count = 0
count += 1
count += 1
say "COUNT =", count
```

```console
$ voila run samples/guide/g02_greet.voi
Greetings, Operator of Earth.
The year is 2026; pi is about 3.14159.
Your balance is 1234.56 — and it is EXACT.
COUNT = 2
```

`use "std/str"` makes the standard string library available. The libraries
are `fmt`, `str`, `os`, `math`, `time`, `json`, `log`, `conv`, `sort`,
`rand` and `uuid`.

---

# Chapter 4. Loops, Conditions, and a Report

## 4.1 The Loop Statements

```voila
for i in 0..10 { … }             // 0,1,…,9 — the end is EXCLUSIVE
for i in 1..=10 { … }            // 1,2,…,10 — inclusive
for i in 0..100 by 5 { … }       // stepped
each item in list { … }          // over a collection
each i, item in list { … }       // …with the index
while cond { … }                 // while
for { … }                        // forever (leave with `break`)
```

## 4.2 A Printed Report

`samples/guide/g03_report.voi`:

```voila
use "std/fmt"

let cities = ["ARMONK", "POUGHKEEPSIE", "ENDICOTT", "BOEBLINGEN"]
let temps  = [18, 22, 15, 24]

say "STATION          TEMP   CLASS"
say "---------------- ----   ---------"

each i, city in cities {
    let t = temps[i]
    var class = "MILD"
    if t >= 22 {
        class = "WARM"
    } else if t < 16 {
        class = "COOL"
    }
    fmt.printf("%-16s %4d   %s\n", city, t, class)
}

say ""
say "STATIONS REPORTED:", len(cities)
say "MEAN TEMPERATURE :", sum(temps) ~/ len(temps)   // floor division
```

```console
$ voila run samples/guide/g03_report.voi
STATION          TEMP   CLASS
---------------- ----   ---------
ARMONK             18   MILD
POUGHKEEPSIE       22   WARM
ENDICOTT           15   COOL
BOEBLINGEN         24   WARM

STATIONS REPORTED: 4
MEAN TEMPERATURE : 19
```

`fmt.printf` takes the verbs you already know: `%s` `%d` `%f` `%x`, with
widths (`%-16s` left-justifies in sixteen columns, `%4d` right-justifies in
four). The compiler counts your arguments at compile time and refuses a
format string that lies about them.

> ### Programming Note 3 — floor division is `~/`, and `//` is only a comment
>
> Floor division — divide and round down to an integer — is spelled `~/`:
>
> ```voila
> let mean = total ~/ count    // floor division; 17 ~/ 5 is 3
> ```
>
> In Voilà `//` is **always** a comment, never division. This is a trap for
> anyone arriving from Python, where `//` divides:
>
> ```voila
> let mean = total // count    // a COMMENT — `mean` is assigned `total`!
> ```
>
> The author of this guide wrote exactly that by accident while drafting
> Chapter 4 and reported a mean temperature of 79 degrees. The lesson is
> not "mind the spaces" — it is "use `~/` to divide." Spacing around `~/`
> never matters.

---

# Chapter 5. PARSE and Exact Decimals

## 5.1 Reading Fixed-Width Records

Mainframe data arrives in columns. Voilà's `parse` — inherited directly from
REXX — takes a subject and a *template*, and fills the named variables. No
index arithmetic, no `substr` calls, no off-by-one.

```voila
parse rec item 6 desc 22 qty 29 price
```

Read that as: take `rec`; put everything up to column 6 in `item`; up to
column 22 in `desc`; up to column 29 in `qty`; and the rest in `price`. The
numbers are 1-based column positions, as REXX and COBOL count them.

A template may also split on literals — `parse line "," a "," b` — or on
blanks. See §5.2 of the Programming Manual for the whole grammar.

## 5.2 The Program

`samples/guide/g04_parse.voi`:

```voila
use "std/fmt"
use "std/str"

// A fixed-width dataset, as a mainframe would hold it.
//        1         2         3         4
// 234567890123456789012345678901234567890
let records = [
    "0001 CHAIR           000012 0049.95",
    "0002 DESK            000003 0299.00",
    "0003 LAMP            000027 0019.50",
]

numeric digits 15

var grand = 0d
say "ITEM DESCRIPTION           QTY      UNIT        VALUE"
say "---- ----------------- ------ --------- ------------"

each rec in records {
    parse rec item 6 desc 22 qty 29 price
    let q = int(str.trim(qty))
    let p = to_dec(str.trim(price))
    let value = dec(q) * p
    grand += value
    fmt.printf("%-4s %-17s %6d %9s %12s\n",
               str.trim(item), str.trim(desc), q,
               p.format(2), value.format(2))
}

say "---- ----------------- ------ --------- ------------"
fmt.printf("%-39s %12s\n", "GRAND TOTAL", grand.format(2))
```

```console
$ voila run samples/guide/g04_parse.voi
ITEM DESCRIPTION           QTY      UNIT        VALUE
---- ----------------- ------ --------- ------------
0001 CHAIR                 12     49.95       599.40
0002 DESK                   3    299.00       897.00
0003 LAMP                  27     19.50       526.50
---- ----------------- ------ --------- ------------
GRAND TOTAL                                  2022.90
```

`numeric digits 15` sets the working precision, exactly as in REXX and PL/I.
`x.format(2)` renders a `dec` with two places after the point — the trailing
zeros a money column requires. `dec(q)` converts an integer to a decimal;
the conversion is explicit because the compiler will not mix the numeric
kinds behind your back.

---

# Chapter 6. Structures, Enumerations, and Errors

## 6.1 Structures

```voila
struct Account {
    id      str
    holder  str
    balance dec = 0d        // a field may have a default
}

var acct = Account{id: "0001", holder: "M. CARDINAL"}
```

## 6.2 Enumerations and MATCH

An enumeration is a value that is *one of* several shapes, and each shape
may carry data:

```voila
enum Txn {
    Deposit(amount dec)
    Withdraw(amount dec)
    Close
}
```

You take it apart with `match`, which the compiler checks for
**exhaustiveness**: if you add a fourth kind of transaction and forget to
handle it, every `match` in the program that is now incomplete becomes a
compile error naming the case you missed. This is the single most valuable
thing in the type system.

```voila
match t {
    Deposit(a)  => self.balance += a
    Withdraw(a) => self.balance -= a
    Close       => self.balance = 0d
}
```

## 6.3 The Two Kinds of Failure

Voilà distinguishes them, and so should you.

**An expected failure** — bad input, a missing file, a rejected deposit — is
a *value*. A procedure that can fail this way returns `T!`, and the caller
cannot ignore it:

```voila
func apply(mut self, t Txn) dec! { … return err("deposit must be positive") }
```

| Form | Means |
|---|---|
| `try f()` | on failure, return the error to *my* caller |
| `try f() else 0d` | on failure, use this value instead |
| `must f()` | on failure, abort — I have proved this cannot fail |
| `match f() { Ok(v) => … Err(e) => … }` | handle both arms here |

**An unexpected failure** — a broken invariant, a defect — is an
*exception*. It unwinds. It does not appear in the signature of every
procedure between the throw and the catch:

```voila
exception Overdrawn { id str, want dec, have dec }

throw Overdrawn{id: self.id, want: a, have: self.balance}
…
try { … } catch e: Overdrawn { say "overdrawn by", e.want - e.have }
```

The rule of thumb: **if the caller can reasonably be expected to handle it,
return it. If handling it would be absurd, throw it.**

## 6.4 The Program

`samples/guide/g05_types.voi` puts all of this together — the full text is in
the distribution. Its output:

```console
$ voila run samples/guide/g05_types.voi
after deposit : 100.00
rejected      : deposit must be positive
overdrawn     : wanted 500.00, had 100.00
after withdraw: 60.00
```

> ### Programming Note 4 — ownership, and the value you gave away
>
> Voilà has no garbage collector, and it does not need one, because every
> value has exactly one owner. Assigning a non-trivial value *moves* it:
>
> ```voila
> let a = [1, 2, 3]
> let b = a          // the slice MOVES to b
> say a              // error: use of moved value `a`
> ```
>
> This is caught at compile time. If you want both to see it, pass a
> **borrow** (`func f(v []int)` — read-only, for the duration of the call),
> or copy it explicitly. If you want shared ownership, say `shared[T]`. If
> you want a back-edge in a tree, say `weak[T]` — and if you forget, the
> compiler tells you that your type *forms an ownership cycle* and would
> leak.

---

# Chapter 7. The Eight Queens

## 7.1 The Problem

Place N queens on an N×N chessboard so that no two attack each other. A
queen attacks along her row, her column, and both diagonals.

## 7.2 The Method

The classical method is **backtracking**, and it rests on one observation:
*exactly one queen stands in each column.* So the search is over columns.

```
   Column 0        Column 1        Column 2      …
   try row 0  ──▶  try row 0  ✗
                   try row 1  ✗
                   try row 2  ──▶  try row 0  ✗
                                   try row 1  ✗
                                     …            ← dead end: BACK UP
                   try row 3  ──▶   …
   try row 1  ──▶   …
```

Place a queen in column 0. Descend to column 1 and try every row; for each
row that is *safe* against the queens already placed, descend again. When a
column has no safe row, the branch is dead: return, **take the last queen
back**, and try the next row in the column above. When you have placed a
queen in every column, you have a solution.

Two queens at `(r1,c1)` and `(r2,c2)` attack each other along a diagonal
exactly when `r1 - c1 == r2 - c2` (the `\` diagonal) or
`r1 + c1 == r2 + c2` (the `/` diagonal). That is the whole geometry.

## 7.3 The Program

The board is one slice of integers: `board[c] = r` means *the queen of
column c stands on row r*. `board[c] = -1` means the column is empty.

```voila
use "std/fmt"

struct Search {
    n       int
    board   []int    // board[c] = the row of the queen in column c
    count   int      // solutions found
    nodes   int      // placements tried — the cost of the search
    first   []int    // the first solution, kept for printing
}

impl Search {
    // safe reports whether a queen at (row, col) is attacked by any queen
    // already placed in columns 0 .. col-1.
    func safe(self, row int, col int) bool {
        var c = 0
        while c < col {
            let r = self.board[c]
            if r == row { return false }                  // same row
            if r - c == row - col { return false }        // same \ diagonal
            if r + c == row + col { return false }        // same / diagonal
            c += 1
        }
        return true
    }

    // place fills columns col .. n-1. It returns nothing: the results are
    // accumulated on self.
    func place(mut self, col int) {
        if col == self.n {
            self.count += 1
            if len(self.first) == 0 {
                each r in self.board { self.first.append(r) }
            }
            return
        }
        for row in 0..self.n {
            self.nodes += 1
            if self.safe(row, col) {
                self.board[col] = row
                self.place(col + 1)      // descend
                self.board[col] = 0 - 1  // take it back
            }
        }
    }
}

// solve counts every solution for a board of size n.
func solve(n int) Search {
    var s = Search{n: n, board: make([]int, n), count: 0, nodes: 0, first: []}
    for c in 0..n {
        s.board[c] = 0 - 1
    }
    s.place(0)
    return s
}

// draw renders one solution as a chess board.
func draw(sol []int, n int) {
    for r in 0..n {
        var line = ""
        for c in 0..n {
            if sol[c] == r {
                line = line || " Q"
            } else {
                line = line || " ."
            }
        }
        say line
    }
}

func main() {
    say "N   SOLUTIONS      NODES"
    say "-   ---------  ---------"
    for n in 1..11 {
        let s = solve(n)
        fmt.printf("%d   %9d  %9d\n", n, s.count, s.nodes)
    }

    say ""
    say "THE FIRST SOLUTION FOR N = 8"
    say ""
    let eight = solve(8)
    draw(eight.first, 8)
}
```

## 7.4 The Output

```console
$ voila run samples/guide/g06_queens.voi
N   SOLUTIONS      NODES
-   ---------  ---------
1           1          1
2           0          6
3           0         18
4           2         60
5          10        220
6           4        894
7          40       3584
8          92      15720
9         352      72378
10         724     348150

THE FIRST SOLUTION FOR N = 8

 Q . . . . . . .
 . . . . . . Q .
 . . . . Q . . .
 . . . . . . . Q
 . Q . . . . . .
 . . . Q . . . .
 . . . . . Q . .
 . . Q . . . . .
```

Ninety-two solutions for the eight-queens problem, which is the number
Gauss's correspondents established in 1850. Two and three queens have no
solution at all, which the search discovers by exhausting every branch.

## 7.5 Points of Style

**`|| ` is string concatenation**, not logical *or*. Logical or is the word
`or`. This is REXX's spelling, and it is why `+` is never overloaded for
text.

**`0 - 1`, not `-1`.** The board is initialised to `-1` to mean *empty*.
Written as `self.board[col] = 0 - 1`. A negative literal is fine in most
positions; the spelled-out form is used here because it reads unambiguously
next to the subtraction on the line above it.

**`self` is explicit.** A method takes `self` (read-only), `mut self`
(may modify), or `own self` (consumes the receiver). `safe` only reads, so
it takes `self`; `place` modifies the board and the counters, so it takes
`mut self`. The compiler enforces this: a method declared `self` cannot
assign to a field.

---

# Chapter 8. Tasks and Channels

## 8.1 Structured Concurrency

A **task** is an independent line of execution. In Voilà you cannot start
one loose:

```voila
group {
    spawn worker(1, jobs, results)
    spawn worker(2, jobs, results)
    …
}   // ← this brace does not close until BOTH workers have ended
```

`spawn` is legal only inside a `group`, and the group joins every task it
started before control passes its closing brace. There is no way to leak a
task, no way to forget to join one, and no way for a task to outlive the
data it was given. If one task fails, its siblings are cancelled and the
failure is re-raised at the group.

This is what *structured* concurrency means, and it is the same discipline
that makes `if` blocks safer than `goto`.

## 8.2 Channels

A **channel** is a typed queue that connects tasks.

```voila
let jobs = chan[int](16)     // buffered, capacity 16
jobs <- 7                    // send
let j = <-jobs               // receive
close(jobs)                  // no more will be sent
each j in jobs { … }         // drains until closed
```

`select` waits on several channels and takes whichever is ready first:

```voila
select {
    case let m = <-fast: say "FIRST TO ARRIVE:", m
    case let m = <-slow: say "FIRST TO ARRIVE:", m
}
```

## 8.3 A Worker Pool

`samples/guide/g07_tasks.voi`:

```voila
use "std/fmt"
use "std/time"

func worker(id int, jobs chan[int], results chan[int]) {
    each j in jobs {
        time.sleep(10 * time.Millisecond)
        results <- j * j
    }
}

func main() {
    let jobs    = chan[int](16)
    let results = chan[int](16)

    group {
        // Three workers draw from one queue.
        for w in 1..4 {
            spawn worker(w, jobs, results)
        }

        for j in 1..9 {
            jobs <- j
        }
        close(jobs)

        var total = 0
        for i in 0..8 {
            total += <-results
        }
        say "SUM OF SQUARES 1..8 =", total
    }
    say "ALL TASKS JOINED."
    …
}
```

```console
$ voila run samples/guide/g07_tasks.voi
SUM OF SQUARES 1..8 = 204
ALL TASKS JOINED.
FIRST TO ARRIVE: FAST
```

Three workers, eight jobs, one queue. Nobody assigns work to anybody: each
worker takes the next job when it is free, which is the whole point of a
pool.

> ### Programming Note 5 — a task must not capture the loop variable
>
> This is the mistake everyone makes, in every language that has closures:
>
> ```voila
> for row in 0..n {
>     spawn { search(row) }           // ← REFUSED by the compiler
> }
> ```
>
> Each iteration of the loop is a fresh binding of `row`, but a task that
> *captures* it captures the enclosing frame — and by the time the task
> runs, the loop may have moved on. Languages that permit this ship with a
> famous footgun.
>
> Voilà **refuses to compile it**, and names the variable:
>
> ```console
> $ voila build parqueens.voi
> main$fn1 captures `row`, which is declared inside a loop.
>   Each iteration of the loop is a SEPARATE binding of `row`, but a
>   closure captures the enclosing frame, and a frame is per procedure —
>   so every closure would end up sharing the LAST value. Rather than
>   answer differently from the specification, the compiler refuses.
>
>   Write a helper that takes the value as a parameter, so the closure
>   owns its own copy:
>
>       for row in 0..n { spawn branch(n, row, out) }
> ```
>
> The remedy is the one the compiler names — make the value a *parameter*,
> so the task owns its own copy:
>
> ```voila
> for row in 0..n {
>     spawn branch(n, row, out)       // ← row is passed, not captured
> }
> ```
>
> That is exactly what the next chapter does.

---

# Chapter 9. The Eight Queens, in Parallel

## 9.1 Where the Work Divides

The search tree of Chapter 7 splits cleanly at the root. The queen of column
0 stands on exactly one of N rows, and the N sub-searches below those N
choices are **wholly independent**: they share no state, they cannot
interfere, and their counts simply add up.

So there is nothing to lock, nothing to co-ordinate, and no reason to be
clever. Give each root row to a task; give each task a channel to post its
count on; let the group join them.

```
          ┌─ task 0 : column 0's queen on row 0 ─┐
          ├─ task 1 : column 0's queen on row 1 ─┤
  group ──┼─ task 2 : …                          ┼──▶ chan[int] ──▶ Σ
          ├─ …                                   ┤
          └─ task N-1 : …                        ┘
```

## 9.2 The Program

`samples/guide/g08_parqueens.voi` — the `Search` structure and its two
methods are exactly those of Chapter 7. Only the driver is new:

```voila
// branch counts every solution whose first queen stands on `row`. It is a
// plain function: a task's body must not capture the loop variable that
// produced it (Programming Note 5), so the row is a PARAMETER, and the
// task owns everything it touches.
func branch(n int, row int, out chan[int]) {
    var s = Search{n: n, board: make([]int, n), count: 0}
    for c in 0..n {
        s.board[c] = 0 - 1
    }
    s.board[0] = row
    s.place(1)              // column 0 is already filled
    out <- s.count
}

func parallel(n int) int {
    let out = chan[int](n)
    group {
        for row in 0..n {
            spawn branch(n, row, out)
        }
        // The group joins every task at the closing brace, so the counts
        // are all posted by the time the loop below has drained them.
        var total = 0
        for i in 0..n {
            total += <-out
        }
        return total
    }
    return 0
}

func main() {
    let n = 11

    // Each elapsed time must be taken WHERE it is true, not at the end.
    let t0 = time.now()
    let a = serial(n)
    let ser = time.since(t0)

    let t1 = time.now()
    let b = parallel(n)
    let par = time.since(t1)

    fmt.printf("N = %d\n\n", n)
    fmt.printf("%-10s %8s %10s\n", "SEARCH", "COUNT", "ELAPSED")
    fmt.printf("%-10s %8d %8d ms\n", "SERIAL", a, ser.millis())
    fmt.printf("%-10s %8d %8d ms\n", "PARALLEL", b, par.millis())

    if a != b {
        say "*** THE TWO SEARCHES DISAGREE — THAT IS A DEFECT ***"
    } else {
        say ""
        say "BOTH SEARCHES AGREE:", a, "SOLUTIONS."
    }
}
```

## 9.3 The Output

```console
$ voila run samples/guide/g08_parqueens.voi
N = 11

SEARCH        COUNT    ELAPSED
SERIAL         2680      721 ms
PARALLEL       2680      161 ms

BOTH SEARCHES AGREE: 2680 SOLUTIONS.
```

Four and a half times faster, on a machine with more than four cores, and
the two searches agree to the last solution — which is the only acceptance
test a parallel program ever really has.

Note what is *absent* from the parallel version: no mutex, no lock, no
atomic counter, no join list, no thread handles, no shutdown protocol. The
work was independent, so the program says so, and the group does the rest.

## 9.4 If the Work Is Not Independent

When tasks must share a total, do not reach for a lock. Use a `cell`:

```voila
let total = cell[int](0)
group {
    for row in 0..n {
        spawn add_into(n, row, total)
    }
}
say "TOTAL =", total.get()
```

`cell[T]` is a value that several tasks may update, one at a time; its
`update` method runs your closure under the cell's own lock. It is the only
sanctioned way to share mutable state between tasks, and it exists so that
you never write a data race by hand.

---

# Chapter 10. A Network Service

Chapters 8 and 9 taught tasks and channels. A network server is the program
those chapters were preparing you for: it does nothing but wait for other
programs to connect, and serve each one while the others wait their turn.
Voilà's `std/net` package gives you the sockets; structured concurrency gives
you a clean way to serve many clients at once.

## 10.1 The Shape of a Server

A server **listens** on an address, then **accepts** connections one at a
time. Each accepted connection is a `Conn` — a two-way stream of bytes you
`read` from and `write` to. A client does the mirror image: it **connects**
to the address and gets a `Conn` directly.

```voila
use "std/net"

let server = must net.listen(":8080")     // every interface, port 8080
group {
    while true {
        let conn = must server.accept()    // waits for the next client
        spawn serve(conn)                  // serve it in its own task
    }
}
```

That is the whole pattern: accept in a loop, and `spawn` a task per
connection so a slow client never blocks the others. Because the tasks live
in a `group`, none of them can outlive the server.

An address is `"host:port"`. Use `":8080"` to accept on every interface,
`"127.0.0.1:8080"` for loopback only, or **port 0** to let the operating
system pick a free port — which you then read back with `server.addr()`.
That last trick is what makes the program below self-contained: it runs its
own server *and* its own client, in one process, and needs no fixed port.

## 10.2 A Complete Example

The program in `samples/guide/g09_net.voi` is a "shout" service. The server
reads lines and writes each one back in capitals; the client sends three
words and prints the replies. Both run as tasks in one group, talking over
the loopback interface.

The server hands its real address to the client through a channel, then
accepts one connection and serves it:

```voila
func server(addr chan[str], done chan[str]) {
    match net.listen("127.0.0.1:0") {
        Err(e) => { addr <- "" ; done <- ("listen: " || e.message()) }
        Ok(lis) => {
            addr <- lis.addr()             // tell the client where to connect
            match lis.accept() {
                Err(e) => done <- ("accept: " || e.message())
                Ok(conn) => {
                    shout(conn)
                    conn.close()
                    lis.close()
                    done <- "server: done"
                }
            }
        }
    }
}
```

`shout` reads a line at a time until the client hangs up — `read_line`
returns an empty string at end of stream — and writes back the upper-cased
line:

```voila
func shout(conn net.Conn) {
    while true {
        match conn.read_line() {
            Err(e)   => return
            Ok(line) => {
                if len(line) == 0 { return }        // the client closed
                match conn.write_all(str.upper(line)) {
                    Ok(u)  => u
                    Err(e) => return
                }
            }
        }
    }
}
```

The client connects to the address it was given, then sends three words and
prints each reply:

```voila
func client(addr chan[str]) {
    let where = <-addr
    if where == "" { return }
    match net.connect(where) {
        Err(e) => say "connect:", e.message()
        Ok(conn) => {
            for word in ["hello\n", "voila\n", "networks\n"] {
                must conn.write_all(word)
                match conn.read_line() {
                    Err(e)  => say "read:", e.message()
                    Ok(got) => say str.trim(got)
                }
            }
            conn.close()
        }
    }
}
```

`main` starts the two tasks in one group and waits for the server to report
that it is finished:

```voila
func main() {
    let addr = chan[str](1)
    let done = chan[str](1)
    group {
        spawn server(addr, done)
        spawn client(addr)
    }
    say <-done
}
```

```console
$ voila run samples/guide/g09_net.voi
HELLO
VOILA
NETWORKS
server: done
```

> ### Programming Note 6
>
> Every read blocks the task until data arrives — and a *blocking* read is
> not interrupted by a `group` timeout, because the task is asleep inside
> the operating system, not at one of Voilà's own waiting points. If a
> connection must not hang forever, give it a deadline with
> `conn.set_timeout(ms)`; a read that then runs out of time fails with an
> `IOError` you can catch. This is the one place the structured-concurrency
> guarantees of Chapter 8 need a little help from you.

## 10.3 Beyond TCP

`std/net` also speaks **UDP**, for connectionless datagrams
(`net.listen_udp`, `send_to`, `recv_from`), and **Unix-domain sockets**, for
fast local communication addressed by a filesystem path rather than a port
(`net.listen_unix`, `net.dial_unix`). The samples `12_udp.voi`,
`13_http.voi` (a tiny web server), and `14_unix.voi` show each in turn.
`std/net` is itself written in Voilà — only the raw system calls are C — so
it doubles as a worked example of extending the standard library.

---

# Chapter 11. Packages

## 11.1 One Directory, One Package

A program larger than a page belongs in packages. **A directory is a
package.** A file names the package it belongs to on its first line, and a
declaration whose name begins with a capital letter is **exported** from it.

```
queens/
├── voila.mod           module queens
├── main.voi            the entry point
└── board/
    └── board.voi       package board
```

`voila.mod` names the module:

```
module queens
```

`board/board.voi` — note `package board`, the exported `Search`, `New` and
`Solve`, and the private `board` field and `place`/`safe` methods:

```voila
package board

struct Search {
    N     int
    Count int
    board []int      // lower case: private to this package
}

func New(n int) Search { … }

impl Search {
    func Solve(mut self) int { … }
    func safe(self, row int, col int) bool { … }   // private
    func place(mut self, col int) { … }            // private
}
```

`main.voi` imports it by path and refers to it by its last element:

```voila
use "queens/board"
use "std/fmt"

func main() {
    for n in 4..9 {
        var s = board.New(n)
        fmt.printf("N = %2d   SOLUTIONS = %4d\n", n, s.Solve())
    }
}
```

```console
$ voila run samples/guide/queens/main.voi
N =  4   SOLUTIONS =    2
N =  5   SOLUTIONS =   10
N =  6   SOLUTIONS =    4
N =  7   SOLUTIONS =   40
N =  8   SOLUTIONS =   92
```

## 11.2 The Rules

- A package may not import itself, directly or through a chain. The compiler
  reports the cycle and names the path.
- Two packages may not have the same last name, even in different
  directories: `a/util` and `b/util` would both be `util.` and one would
  silently win.
- `use "pkg" *` and `use "pkg" { name }` are *not* available for your own
  packages. Write `board.New`, so a reader can always see where a name comes
  from.
- Enumeration **variant** names and **trait** names are global across the
  program, because a pattern (`Deposit(a)`) and a trait bound have no
  qualified form. Two packages may not both define a variant called `Empty`.

---

# Chapter 12. What to Read Next

You now know enough to write real programs. Three suggestions for what to do
with the rest of the afternoon:

**Read the fourteen sample programs** in `samples/`. They are not toys: a worker
pool that counts primes, a fixed-width ledger with exact decimal totals, a
recursive-descent expression evaluator, a channel pipeline with timeouts and
cancellation, an org chart that demonstrates the ownership cycle rule, a
trait-based shapes program with a generic `Stack[T]`, JSON round-trips that
keep money exact, and Conway's Life on a torus.

**Read the [Programming Manual](programming_manual.md).** It is the
reference: every statement, every rule, with syntax diagrams. Appendix D
lists the places where the implementation deliberately departs from the
specification, and why.

**Read the compiler.** It is some 11,800 lines of Voilà in `voilac/`, and it is
the largest Voilà program in existence: a lexer, a recursive-descent parser
building a flat node arena, a package loader, a checker, an IR lowerer and a
C back end. If you want to see what idiomatic Voilà looks like at scale,
that is where it lives. `BOOTSTRAP.md` explains how it compiles itself, and
which two real bugs that discipline caught.

---

<div align="center">

*Voilà — Rust's guarantees, Go's plumbing, REXX's manners.*

</div>
