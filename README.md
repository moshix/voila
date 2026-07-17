<div align="center">

# ✨ Voilà

**A statically typed, memory-safe, concurrent language —<br>
that compiles itself.**

![language](https://img.shields.io/badge/language-Voil%C3%A0%200.3.2-blueviolet?style=for-the-badge)
![gc](https://img.shields.io/badge/GC-none-orange?style=for-the-badge)

`say "Hello, world!"` · files end in `.voi` · the toolchain is `voila`, and it is written in Voilà

</div>

---

## 🧬 What the language borrows — and from whom

| 🎁 From | Voilà takes |
|---|---|
| 🐹 **Go** | braces, packages, channels, `select`, familiar `func` |
| 🦀 **Rust** | ownership + moves + deterministic destruction — **without lifetime annotations** |
| 🖥️ **REXX** | `say`, `parse`, exact decimal arithmetic, learn-it-in-an-afternoon |
| ⚙️ **C** | the `printf` verbs everyone already knows |

## 👀 Thirty seconds of syntax

```go
package main

use "std/fmt"
use "std/os"

exception BadRecord { line int, why str }

struct Txn { acct int, holder str, amount dec }

func parse_txn(n int, line str) Txn {
    parse line "|" acct "|" holder "|" amount        // REXX-style templates
    if trim(acct) == "" { throw BadRecord{line: n, why: "empty account"} }
    return Txn{
        acct:   int(trim(acct)),                     // throws ConvError on junk
        holder: title(trim(holder)),
        amount: dec(trim(amount)),                   // exact decimal, always
    }
}

func main() {
    numeric digits 31                                // REXX heritage: exact money
    var total = 0d

    let f = must os.open("TXN.DAT")                  // error value → exception
    defer f.close()                                  // runs on every exit path

    each i, line in f.lines() {
        try {
            let t = parse_txn(i + 1, line)
            total += t.amount
            fmt.printf("%6d  %-24s %12.2f\n", t.acct, t.holder, t.amount)
        } catch e: BadRecord {
            fmt.eprintf("skipping line %d: %s\n", e.line, e.why)
        }
    }
    fmt.printf("%-24s %14.2f\n", "TOTAL", total)     // exact to 31 digits
}
```

## 🛡️ The guarantees (not conventions — **compile errors**)

- 🚫 **No leaks.** The type graph must be acyclic through owning edges;
  back-edges are `weak[T]`. Refcounts always reach zero:

  ```text
  error: type `Node` forms an ownership cycle through field `parent`
    --> tree.voi:4:5
     |
   4 |     parent shared[Node]
     |            ^^^^^^^^^^^^ owning edge back to `Node`
     = note: owning cycles can leak; Voilà forbids them
     = help: use `weak[Node]` for a back-edge
  ```

- 🚫 **No dangling.** Borrows can't be stored in structs, channels, or
  escaping closures — so they can't dangle, and nobody writes `'a`.
- 🚫 **No orphaned tasks.** `spawn` lives inside `group { }`; the group
  joins **all** of its tasks before it exits. A failing task cancels its
  siblings and the failure rethrows at the boundary.
- 🚫 **No silent precision loss.** `u16 → int` is free; `i64 → float`
  is a compile error until you write `float(x)`. Overflow **traps**.
- 🚫 **No 59.96999999999999.** `19.99d * 3d == 59.97d`. Exactly.

## ⚡ Concurrency that can't leak threads

```go
try group {
    spawn fetch("https://a.example")
    spawn fetch("https://b.example")     // throws? siblings get cancelled,
}                                        // everyone is joined, error surfaces here
```

Channels **move** their payloads (`ch <- v` kills `v` in the sender), so
data races are unrepresentable. A million tasks is a normal Tuesday.

## 🎭 Errors: values *and* exceptions — pick per call site

```go
let p = try parse_port(s)          // propagate the error value
let p = parse_port(s) else 8080    // or default it
let p = must parse_port(s)         // or promote it to an exception
let r = attempt risky()            // or demote an exception to a value
```

## 📦 What's in the box

| | |
|---|---|
| 🪞 `voilac/` | **the compiler, written in Voilà** — `lex` · `par` · `load` · `chk` · `low` · `cg` |
| 🔤 `voilac/lex` `par` | full grammar: interpolation, nested comments, semicolon insertion, the arena AST |
| 🔎 `voilac/chk` | acyclicity rule, widening lattice, exhaustive `match`, use-after-move, compile-time `printf` checking |
| 📦 `voilac/cg` | **C backend**: IR → C → `cc`. Real native binaries; zero leaks under `leaks` |
| ⚙️ `runtime/` | **libvoila**: the C runtime — refcounted values (no GC), exact decimals, exceptions, tasks/channels/select |
| 📚 stdlib | `fmt` `str` `os` `math` `time` `json` `log` `conv` `sort` `rand` `uuid` |
| 📜 `voila build -S` | register-IR **assembly listings** in HLASM style — location counter, constant pool, DSECTs, ≤79 columns |
| 🌱 `bootstrap/voilac.c` | the seed: the C the compiler emits **for itself**, so `cc` alone can rebuild everything |
| 🧪 tests | goldens, negative fixtures, and the **fixpoint**: the compiler compiled by itself emits identical C ([TESTING.md](TESTING.md)) |

## 🏃 Quick start


```console
$ ./build.sh                          # cc the seed, then Voilà builds Voilà
  ✓ fixpoint: C(voilac-1) == C(voilac-2)
  ✓ bin/voila

$ bin/voila run samples/04_calculator.voi
$ bin/voila check samples/07_orgchart.voi
$ bin/voila build samples/10_life.voi -o life && ./life
$ leaks --atExit -- ./life             # refcounting: 0 leaks, 0 bytes

$ ./selftest.bash                     # the toolchain reproduces every golden
```

See **[TESTING.md](TESTING.md)** for the full procedure and
**[BOOTSTRAP.md](BOOTSTRAP.md)** for how the compiler compiles itself.

## 📖 Ten sample programs (none of them trivial)

| Sample | Shows off |
|---|---|
| `01_worker_pool.voi` | 8-worker prime-counting pool: `group`/`spawn`/channels/join |
| `02_accounts.voi` | fixed-width mainframe records, column `parse`, exact `dec` totals |
| `03_txnreport.voi` | exceptions + error values cooperating; typed `catch`; `defer` |
| `04_calculator.voi` | a full recursive-descent evaluator in ~200 lines: enums, `match`, `T!` |
| `05_pipeline.voi` | channel pipeline stages, `select`, group `timeout`, cancellation |
| `06_wordfreq.voi` | text analytics: `translate`, `fields`, ordered maps, `sort_by` |
| `07_orgchart.voi` | `shared`/`weak` trees — the acyclicity rule in action |
| `08_shapes.voi` | traits + dynamic dispatch + generic `Stack[T]` |
| `09_jsonreport.voi` | typed & dynamic JSON, `dec`-safe money round-trips |
| `10_life.voi` | Conway's Life on a torus: 2-D slices, `str.Builder` frames |

## 🪞 It compiles itself
Voilà is completely self-hosted and it compiles itself from the very first version. 
In other words, the compiler is written in Voilà. To build it you need a compiler, so one
generation of its own C output is checked in as `bootstrap/voilac.c` — a
fossil, not a source. `./build.sh` compiles that with `cc`, uses the result to
compile `voilac/*.voi`, uses *that* to compile `voilac/*.voi` again, and asserts
the two emit **byte-identical C**. It bootstrapped from a Go compiler that no
longer exists; every stage of the Voilà one was diffed against it, byte for
byte, over every file in this repository — and then it was deleted.

## 🗺️ Roadmap

- ✅ **Self-hosted** — the compiler is written in Voilà; the fixpoint holds
- ✅ Native binaries, multi-file packages, the full checker, `run`/`build`/`check`
- ⬜ an unboxing pass (values are boxed; performance is not yet C-class)
- ⬜ green threads (tasks are OS threads today: right semantics, wrong cost model)
- ⬜ `std/regex`, `std/http` in C; `voila fmt/test/repl`; the flow-sensitive
  borrow checker

📕 **Docs:** start with **[Getting Started](GettingStartedWithVoila.md)** — a
tutorial from `hello.voi` to a parallel eight-queens solver, every program
tested on every build. Then the [Programming Manual](programming_manual.md),
the reference — written in the house style of the IBM mainframe language
references, syntax diagrams included — and the
[Language Specification](voila_specification.md).

## 📜 License

Voilà is free software under the **GNU General Public License, version 2** —
see [LICENSE](LICENSE).

---

<div align="center">

*Voilà: Rust's guarantees, Go's plumbing, REXX's manners.*

</div>
