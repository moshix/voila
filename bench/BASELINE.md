# Benchmark baseline — the optimizer's reference point

Machine: darwin/arm64 (Apple clang). Method: best-of-3 wall time via bash
`EPOCHREALTIME` (no subprocess inside the timing window), binaries pre-run
once (code-signature + page-cache warm), idle machine, every timed run's
output verified against `bench/expected/`. Produced by `./bench.bash`.
Numbers are comparable only within one machine.

State at freeze: **voila 0.3.2 + M1** (the runtime-archive cache) — this is
the baseline the optimizer milestones (M3–M5) are measured against.

## Runtime suite (default build — no `-O`)

| program | ms | what it exercises |
|---|---:|---|
| b1_queens | 3973 | integer recursion + compares — the unboxing target |
| b2_life | 1129 | 2-D slice indexing — bounds checks, `vl_index` retains |
| b3_wordfreq | 404 | map insert/update (timed region is map-dominated; the LCG runs before it) |
| b4_ledger | 968 | `dec` under `numeric digits` — must NOT regress (dec is never unboxed) |
| b5_churn | 807 | struct/slice allocation churn — frame elision target (~70 % in the `mid()` call loop) |
| b6_partasks | 668 | tasks/channels/group — correctness under optimization; higher variance than the serial programs |
| b7_strings | 519 | Builder, split, fixed-window substring — boxed-string path |

## Compile suite

| workload | ms | note |
|---|---:|---|
| self-compile: `check` (front end) | 2109 | M2's target (load = two full AST walks) |
| self-compile: `--emit=c` (front + mid) | 2534 | lower+cgen ≈ 0.28 s of this |
| self-compile: full `build` (adds cc) | 5909 | cc on the 41 kLoC program.c ≈ 3.3 s |
| small program: full `build` (b1_queens) | 89 | **was 867 before M1** — the runtime-recompile tax is gone |

Caveat for cross-milestone reading: the self-compile workload grows as the
compiler grows (the optimizer adds ~1–2 kLoC of source); compile-suite
deltas conflate compiler speed with compiler size. The `check`/`--emit=c`
rows are the cleanest speed signals.

Historical pre-M1 reference (0.3.2 exactly): small build 867 ms; full
self-build 6497 ms; the difference is the fixed ~0.9 s runtime recompile
that M1's `libvoila.a` cache removed.

Each later milestone appends its table to `bench/RESULTS.md`; this file
stays frozen.
