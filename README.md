<div align="center">

# ✨ Voilà

**A small, fast, safe language that compiles itself.**

![language](https://img.shields.io/badge/language-Voil%C3%A0%200.5.0-blueviolet?style=for-the-badge)
![gc](https://img.shields.io/badge/GC-none-orange?style=for-the-badge)

</div>

---

## What is it?

Voilà is a compiled programming language. You write a `.voi` file, run
`voila build`, and get a normal native executable that depends on nothing but
the C library.

Three things make it unusual:

- **It frees memory for you, without a garbage collector.** Nothing pauses
  your program to go looking for garbage. Memory is released the moment the
  last thing using it goes away.
- **Money arithmetic is exact.** `19.99 * 3` is `59.97`, not
  `59.96999999999999`.
- **It is written in itself.** The Voilà compiler is a Voilà program. It
  compiles its own source code.

## Where the ideas come from

| From | What Voilà borrows |
|---|---|
| **Go** | curly braces, packages, and its approach to doing many things at once |
| **Rust** | memory safety without a garbage collector — but without Rust's difficult lifetime annotations |
| **REXX** | `say` for printing, `parse` for pulling apart text, exact decimal maths, and the goal of being learnable in an afternoon |
| **C** | the `printf` formatting codes you already know |

## Thirty seconds of syntax

This reads a file of pipe-separated records, adds up the amounts, and skips
bad lines instead of crashing:

```go
package main

use "std/fmt"
use "std/os"

exception BadRecord { line int, why str }

struct Txn { acct int, holder str, amount dec }

func parse_txn(n int, line str) Txn {
    parse line "|" acct "|" holder "|" amount        // pull the fields apart
    if trim(acct) == "" { throw BadRecord{line: n, why: "empty account"} }
    return Txn{
        acct:   int(trim(acct)),                     // fails loudly on junk
        holder: title(trim(holder)),
        amount: dec(trim(amount)),                   // exact decimal, always
    }
}

func main() {
    numeric digits 31                                // 31 digits of precision
    var total = 0d

    let f = must os.open("TXN.DAT")
    defer f.close()                                  // always runs, even on error

    each i, line in f.lines() {
        try {
            let t = parse_txn(i + 1, line)
            total += t.amount
            fmt.printf("%6d  %-24s %12.2f\n", t.acct, t.holder, t.amount)
        } catch e: BadRecord {
            fmt.eprintf("skipping line %d: %s\n", e.line, e.why)
        }
    }
    fmt.printf("%-24s %14.2f\n", "TOTAL", total)
}
```

## What the language promises

**Your program will not leak memory.**

Voilà frees a value as soon as nothing refers to it any more. That works
perfectly until data points back at itself — a tree where each child also
points at its parent, for example. Then the parent is keeping the child
alive, the child is keeping the parent alive, and neither is ever freed.

So Voilà refuses to compile that shape, and tells you the fix:

```text
error: type `Node` forms an ownership cycle through field `parent`
  --> tree.voi:4:5
   |
 4 |     parent shared[Node]
   |            ^^^^^^^^^^^^ this points back at `Node`
   = note: two values that keep each other alive are never freed
   = help: use `weak[Node]` for a link back to a parent
```

A `weak` link lets you point at something without keeping it alive — exactly
what a "back to my parent" link should do.

**You will not get a dangling pointer.** When you lend a value to a function,
Voilà stops that borrowed reference from being squirrelled away somewhere
that outlives it. You never write lifetime annotations to get this.

**No task is left running behind your back.** Background work lives inside a
`group`, and the group waits for everything it started before it finishes. If
one task fails, its siblings are cancelled and the error surfaces where you
started them.

**Numbers do not lose precision quietly.** Converting a small integer type to
a bigger one is free. Anything that could lose information — a large integer
to a floating-point number, say — is a compile error until you ask for it
explicitly. Arithmetic that overflows stops the program rather than silently
wrapping around.

## Doing several things at once

```go
try group {
    spawn fetch("https://a.example")
    spawn fetch("https://b.example")     // if one fails, the other is cancelled,
}                                        // both are waited for, and the error
                                         // is reported right here
```

When you send a value down a channel, it *moves* — the sender no longer has
it. That means two tasks can never be looking at the same value at the same
time, so a whole category of concurrency bug simply cannot be written.

## Errors: pick the style at each call

```go
let p = try parse_port(s)          // pass the failure up to my caller
let p = parse_port(s) else 8080    // or just use a default
let p = must parse_port(s)         // or treat failure as fatal
let r = attempt risky()            // or turn a crash into a value I can inspect
```

## What is in the box

| | |
|---|---|
| `voilac/` | **the compiler, written in Voilà** |
| `runtime/` | the small C library every compiled program links against |
| `std/` | the standard library: `fmt` `str` `os` `io` `net` `http` `math` `time` `json` `csv` `regex` `crypto` `log` `conv` `sort` `path` `sync` `test` `rand` `uuid` |
| `bootstrap/` | one generation of the compiler's own C output, so a machine with only a C compiler can get started |
| `tests/` | the test suite (see [TESTING.md](TESTING.md)) |

## Quick start

If you already have a Voilà binary, `./build.sh` uses it to build the
compiler — Voilà compiling Voilà:

```console
$ ./build.sh
==> bootstrap: prebuilt binary  bin/voila  (0.4.1)
  ✓ fixpoint: C(voilac-1) == C(voilac-2)
  ✓ optimized fixpoint: C(-O3, gen1) == C(-O3, gen2)
  ✓ bin/voila-0.5.0
```

On a machine with no Voilà at all, `./bootstrap.bash` compiles the checked-in
C file into a first working compiler, then uses that:

```console
$ ./bootstrap.bash
  ✓ build/voila-seed
  ✓ bin/voila-0.5.0
```

Then:

```console
$ bin/voila run samples/04_calculator.voi
$ bin/voila check samples/07_orgchart.voi
$ bin/voila build samples/10_life.voi -o life && ./life
$ ./selftest.bash                     # run everything
```

**About that "fixpoint" line.** The build compiles the compiler with the
compiler — twice — and checks that both rounds produce byte-for-byte
identical output. If the compiler had a bug that mistranslated its own source,
the second round would differ. It is one cheap check that covers an enormous
amount of ground, and it has caught real bugs nothing else did.

## Fifteen sample programs

| Sample | Shows off |
|---|---|
| `01_worker_pool.voi` | eight workers counting primes in parallel |
| `02_accounts.voi` | fixed-width records and exact money totals |
| `03_txnreport.voi` | the two error styles working together |
| `04_calculator.voi` | a complete expression evaluator in ~200 lines |
| `05_pipeline.voi` | stages connected by channels, with a timeout |
| `06_wordfreq.voi` | counting words in text |
| `07_orgchart.voi` | a tree with parent links — the `weak` rule in action |
| `08_shapes.voi` | traits, and a generic `Stack[T]` |
| `09_jsonreport.voi` | JSON in and out, without losing decimal precision |
| `10_life.voi` | Conway's Life on a wrapping grid |
| `11_echo.voi` | a TCP echo server |
| `12_udp.voi` | UDP messaging |
| `13_http.voi` | an HTTP client and server with `std/http` |
| `14_unix.voi` | local inter-process communication |
| `15_calc.voi` | the calculator again, reading from standard input |

## Performance

The optimizer is opt-in. Without `-O`, output is unchanged from 0.3. With it
(measured on an Apple silicon Mac; Linux gains are similar or better):

| benchmark | what it stresses | default | `-O3` | change |
|---|---|---|---|---|
| `b2_life.voi` | integer arithmetic in loops | 1181 ms | 710 ms | **−40 %** |
| `b6_partasks.voi` | tasks and channels | 851 ms | 716 ms | −16 % |
| `b1_queens.voi` | recursion | 4153 ms | 3589 ms | −14 % |
| `b5_churn.voi` | allocating lots of structs | 810 ms | 742 ms | −8 % |
| `b4_ledger.voi` | exact decimal money | 987 ms | 971 ms | −2 % (by design) |

Full tables in `bench/BASELINE.md` and `bench/RESULTS.md`.

## What works today

- Native executables, multi-file packages, `run` / `build` / `check`
- An optimizing backend (`-O1` to `-O3`) that never changes what your program
  does, only how fast it does it
- Reading standard input, buffered file and network I/O (`std/io`)
- Real concurrency primitives — mutexes, read/write locks, atomics, `Once`
  and wait groups (`std/sync`) — and lexical path handling (`std/path`)
- Calendar dates and RFC 3339 (`std/time`), in UTC or a fixed offset
- CSV that streams and gets quoting right (`std/csv`), and assertions with
  table tests for testing your own code (`std/test`)
- An HTTP/1.1 client and server (`std/http`) — `get`, `post`, `request`, a
  handler-based server, URL parsing and chunked bodies, written in Voilà over
  `std/net`. Plaintext only: there is no TLS, and an `https://` URL is refused
  rather than fetched in the clear
- Checks that catch mistakes before you run: unimplemented trait methods,
  generic constraints, assigning to a `let`, unknown type names, missing
  `return`s, wrong numbers of arguments
- Warnings you can tune or promote to errors (`-Wall`, `-Werror`, `--strict`)

## What is still missing

- **Type checking is partial.** Declared types are verified in many places,
  but not everywhere — the compiler does not yet check every expression.
- **No package manager.** You can only use code inside your own project;
  there is no way to depend on someone else's library yet.
- **Some standard library gaps**: no named time zones (`std/time` knows UTC,
  a fixed offset, and the machine's own offset — there is no tzdata).
  `std/crypto` has hashes, HMAC and a cryptographically secure random source,
  but no signatures — and there is **no TLS at all**, so `std/http` speaks
  plaintext HTTP and refuses an `https://` URL rather than fetching it in the
  clear. No XML or YAML.
- **No editor support, formatter, debugger, or `voila test` runner.**
  `std/test` gives you assertions and a pass/fail tally; a suite is an
  ordinary program you run yourself.
- **Background tasks are real operating-system threads.** They behave
  correctly, but a million of them is expensive.

## Documentation

Start with **[Getting Started](GettingStartedWithVoila.md)**, a tutorial that
runs from `hello.voi` to a network service. Then the
[Programming Manual](programming_manual.md) for the details, and the
[Language Specification](voila_specification.md) for the fine print.

## License

Voilà is free software under the **GNU General Public License, version 2** —
see [LICENSE](LICENSE).

---

<div align="center">

*Safety without a garbage collector. Exact arithmetic. Compiles itself.*

</div>
