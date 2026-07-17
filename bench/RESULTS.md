
## After M2 (front-end: Graft-in-place, find_any/skip_any lexer, slim tokens, first-byte ops) — 2026-07-17

== COMPILE SUITE  — best of 3, ms ==
workload                                   ms
self: check (front end)                  1022
self: --emit=c (front+mid)               1427
self: full build (adds cc)               5046
small: full build (b1_queens)             127

## After M4 (frame elision -O2; biased frame refcounts) — 2026-07-17

Runtime changes in this milestone affect ALL levels: frame refcounts are now
thread-safe via a `shared` flag (set when a closure captures the frame) —
unshared frames keep plain non-atomic counts, so the default path measures
within noise of pre-M4 (interleaved A/B: 4156 vs 4142 ms on b1). A first,
fully-atomic version cost ~5% on every call-heavy program and was replaced.

== RUNTIME SUITE — interleaved best-of-5 where it mattered, ms ==
program           -O0     -O2   delta
b1_queens        4143    3979   -4.0%   (interleaved; suite runs are noisier)
b2_life          1181    1049  -11.2%
b3_wordfreq       416     402   -3.4%
b4_ledger         987     972   -1.5%   (dec never elides its boxes — expected)
b5_churn          810     771   -4.8%
b6_partasks       851     771   -9.4%
b7_strings        523     520   -0.6%

-O2 frame elision removes the calloc/free per call but NOT the boxed
arithmetic dispatch, which dominates b1/b4/b7 — the unboxing milestone (M5)
targets that. New probes: tests/opt/unwind.voi (throw through an elided
frame holding boxed locals; 0 leaks), tests/opt/traps.voi (trap messages
byte-identical at every level), tests/opt/shared_frames.voi (8 tasks × 20k
cross-thread closure calls × 25 runs per level, deterministic, 0 leaks).
TSan is broken on this macOS toolchain (a trivial pthread hello segfaults);
a Linux TSan pass over shared_frames is queued for M6's per-arch gate.

## After M5 (typed unboxing -O3; cc -O3 for flagged program TUs) — 2026-07-17

== RUNTIME SUITE — suite best-of-3 (interleaved A/B in parens), ms ==
program           -O0     -O3   delta
b1_queens        4153    3589   -14%  (interleaved: 3982 vs 3784, -5%)
b2_life          1181     710   -40%  (interleaved: 1116 vs 708, -37%)
b3_wordfreq       416     382    -8%
b4_ledger         987     971    -2%  (dec never unboxes — by design)
b5_churn          810     742    -8%  (interleaved: 849 vs 748, -12%)
b6_partasks       851     716   -16%
b7_strings        523     496    -5%

The unboxing pass types whole registers (flow-insensitive join over all
defs): loop counters, LCG arithmetic, accumulator temps go native — hence
b2_life's -37%. The known structural limit: PARAMETERS stay boxed (the
calling convention passes Values), so recursion-through-params workloads
(b1_queens' solver) gain far less. Candidates for a later milestone:
call-site specialization, and rewriting `for i in range` iterators into
native counters (today ITNEXT forces its loop variable to BOX).

New: tests/opt/typing_adv.voi (match-bind counters, parse targets, UPSET
closures, setjmp-clobber probe, int/float join demotion, loop-carried
temps, hot-loop overflow, dec at -O3) — byte-identical at all four levels,
0 leaks. Cross-build identity: the compiler built at -O3 emits byte-equal
C (default AND -O3) for conformance, bench, samples, and itself. selftest
now runs goldens at -O1, -O2 and -O3.

## After M6 (architecture layer) — 2026-07-17

-O3 raises the program TU to `cc -O3` (landed with M5); new opt-in
`voila build --native` tunes for the build machine (probes the toolchain
for -march=native vs -mcpu=native; never default; runtime archive stays
generic/portable).

Per-arch verification:
- macOS/arm64 (Apple clang): full selftest + all opt probes green
  (throughout M4/M5).
- Linux/amd64 (Ubuntu 24.04, gcc 13.3, ovh2): bootstrap fixpoint, full
  selftest ALL GREEN, opt probes (traps/unwind/typing_adv) byte-identical,
  --native builds and runs. ThreadSanitizer over tests/opt/shared_frames
  at -O0/-O2/-O3: CLEAN — the biased frame-refcount scheme shows no data
  race on real hardware (macOS TSan is broken; this was the queued gate).
- Linux bench spot (interleaved best-of-3):
  b1_queens -O0 6242ms → -O3 5367ms (-14%); b2_life 1856ms → 1059ms (-43%).
- Windows/mingw, Linux/arm64: tarball cross-builds unchanged; not
  re-verified this milestone (no toolchain at hand — "as available" per
  plan).
