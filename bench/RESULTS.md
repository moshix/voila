
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
