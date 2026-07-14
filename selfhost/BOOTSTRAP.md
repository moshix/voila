# The Voilà Bootstrap Plan

Voilà is implemented in two stages. **Stage 0** is the Go toolchain in this
repository: lexer, parser, checker, tree-walking interpreter, and the
`voila build` packager. **Stage 1** is Voilà written in Voilà, executed by
stage 0 until it can compile itself.

```
stage 0 (Go)          stage 1 (Voilà, runs on stage 0)     stage 2
────────────────      ────────────────────────────────     ─────────────────
voila run/build  ───▶ lexer.voi      ✅ done               voilac.voi builds
                      parser.voi     ⬜ next                voilac.voi; Go
                      checker.voi    ⬜                     toolchain retires
                      interp.voi     ⬜                     to bootstrap duty
```

## Status

| Component | File | State | Proof |
|---|---|---|---|
| Lexer | `lexer.voi` | **done** | `TestSelfhostLexerDifferential` diffs its token dump against the stage-0 lexer over shared fixtures — including `lexer.voi` lexing **itself**. The dumps are byte-identical. |
| Parser | `parser.voi` | not started | will dump a canonical AST S-expression, diffed the same way |
| Checker | `checker.voi` | not started | same diagnostics on the `testdata/negative` fixtures |
| Evaluator | `interp.voi` | not started | golden outputs of `samples/` must match stage 0 |

## Why the lexer first

The lexer is the canary for interpreter completeness: it leans on exactly
the features a young interpreter gets wrong — enums with payloads and
exhaustive `match`, `mut self` methods that really mutate through method
calls, byte-level string access and slicing, `T!` error values threaded with
`try`, string building in loops, and the language's two subtlest lexical
rules (semicolon insertion, and the `//` comment-vs-floor-division
disambiguation), reimplemented from the spec rather than ported.

Bugs it has already caught in stage 0: the multi-byte rune handling of
string indexing, the interpolation escape rules for `\{`, and the
missing `//` operator entry in a longest-match table.

## What stage 0 must grow before parser.voi

- ~~**Multi-file programs**~~ — **done.** `use "app/pkg"` resolves to a
  directory under the module root (nearest ancestor with `voila.mod`); one
  directory is one package; exported names are capitalised. The loader
  flattens everything into one AST with qualified names, so the checker,
  interpreter, IR and C backend need no notion of packages. The compiler can
  now be split across files.
- **Richer generics in practice** — the AST will want `[]Node` where Node is
  a 40-variant enum; the type-erased scheme works, but checker coverage of
  variant payload arity should tighten first.
- **Performance** — the tree-walker lexes `lexer.voi` in ~150 ms. Fine for
  the lexer; a parser bootstrapping itself will want the planned bytecode VM
  (the spec's §12 architecture) or at least environment-lookup caching.
  The VM's instruction set already exists: `voila build -S` lowers any
  program to the register IR (`internal/ir`) and prints it as an assembly
  listing — the VM milestone is "execute what -S emits".

## Ground rules for stage-1 code

1. Write to the **spec**, not to stage-0 quirks. Divergences must be listed
   in the programming manual's "Stage-0 deviations" appendix.
2. Every stage-1 component ships with a **differential test** against its
   stage-0 counterpart before it may grow features the stage-0 side lacks.
3. Keep components **deterministic**: no wall-clock, no randomness, ordered
   maps only — differential testing depends on it.
