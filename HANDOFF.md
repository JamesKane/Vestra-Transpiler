# Handoff

Orientation for picking up work on Vestra. What to do next, and in what
order, is in [`ROADMAP.md`](ROADMAP.md); where the compiler falls short of the
spec is in [`SPEC_GAPS.md`](SPEC_GAPS.md); the language is
[`VESTRA_DRAFT.md`](VESTRA_DRAFT.md) plus
[`VESTRA_ANNEX_A.md`](VESTRA_ANNEX_A.md). This file covers how the project is
put together, how a session works, and the traps to know about. It records no
history: `git log` does that.

> The per-slice engineering notes for the self-hosted compiler live in the
> auto-memory file `project_dogfood_compiler.md`, loaded each session.

## The two compilers

**The bootstrap compiler** (`vestra`, C++26, `src/` and `include/vestra/`) is
the stable substrate. Its pipeline is
`lex → parse → resolver → ownership → exclusivity → capability → comptime fold
→ C++ emitter`. Every phase reports on a shared `DiagnosticReporter`, and
`vestra build` refuses to emit C++ when sema reports an error (`--skip-check`
overrides). It builds hosted or freestanding (`--no-libc`, with
`--target=aarch64|x86_64|riscv64`); `vestra check` takes the same target
options.

**The self-hosted compiler** (`selfhost/`, about 25K lines of Vestra) is the
same compiler written in Vestra. Each phase is one `.vst` file building its own
binary, and each is proved by **differential testing** against the bootstrap:
same input, byte-identical output.

| Phase | Source | Binary | Differential (vs `vestra`) |
|---|---|---|---|
| Lexer   | `selfhost/lexer.vst`   | `vlex`   | `vestra build --dump-tokens` |
| Parser  | `selfhost/parser.vst`  | `vparse` | `vestra build --dump-ast` |
| Sema    | `selfhost/sema.vst`    | `vsema`  | `vestra check` diagnostics, grep-extracted by family |
| Codegen | `selfhost/codegen.vst` | `vgen`   | `vestra build` `.cpp`, and the `.hpp` after the prelude |

Each `.vst` is transpiled by the bootstrap, compiled with `c++ -std=c++26`,
and run over its fixtures. `parser.vst`, `sema.vst` and `codegen.vst` each
carry **their own copy of the lexer and parser** at the top, so a front-end
change goes into all three. `vsema` and `vgen` take `--no-libc`; `vgen` also
takes `--hpp`. Both load imported modules from the entry file's directory.

## Current state

- **1802 tests pass** (`ctest --test-dir build/debug`), 313 of them
  `e2e_selfhost` (4 lexer, 12 parser, 87 sema, 206 codegen, plus demos).
- **Lexer and parser:** byte-identical across the example corpus.
- **Sema:** resolution, capabilities, exclusivity, ownership with the full
  linear rules (branch-aware moves, path leaks, storage places), the
  divergence rules, `--no-libc` rejections, and a subset of type checking.
  Over `examples/` it still reports about 470 lines the bootstrap doesn't;
  closing those is the main open front-end item (ROADMAP, "Self-hosting").
- **Codegen:** 136 of 150 corpus files byte-identical, and every module of the
  Vestra-POC spike kernel without inline asm. The C++ output burndown stops
  here: the next phase lowers to LLVM IR instead (ROADMAP, "The next phase").
- **Inline asm** is bootstrap-only by decision.

## Verifying a change

Run all of these before each commit:

```
cmake --build build/debug -j
(cd build/debug && ctest -j8)
./scripts/sweep_codegen_corpus.sh --build --compare build/sweep/before.txt
```

The sweep re-generates the whole example corpus through `vgen` and flags any
example whose divergence grew or that stopped being identical. Take a fresh
baseline with `--report build/sweep/before.txt` before starting work. A
flagged example is either a real gap the fixtures missed or a measurement
problem, and both are worth stopping for.

When a change touches freestanding output or the kernel surface, also run the
spike in `../Vestra-POC` (not a git repo; back it up before editing it):

```
python3 tools/build.py     # build the kernel and user image
python3 tools/run.py       # boot on QEMU; needs preemptions > 0
python3 tools/test.py      # host IPC tests
python3 tools/flaws.py     # injected flaws, all must be caught
```

## How a slice proceeds

1. Pick the next item from ROADMAP.
2. **Probe the reference.** Run `vestra check` or `vestra build` on a tiny
   program and read the output byte-exactly (`cat -e`, `diff`). Read the
   bootstrap function that produces it: for self-hosting work `src/` is the
   spec, and each lowering or diagnostic has one owner.
3. Implement it. For a front-end rule, implement it in the bootstrap and in
   `selfhost/sema.vst` (or `codegen.vst`) together, and hold them to
   identical output.
4. **Diff the self-hosted binary directly** while iterating:
   ```
   (cd build/debug && ctest -R "e2e_selfhost_codegen_(compile|transpile)")
   diff <(build/debug/_e2e/vgen FILE.vst) <(build/debug/vestra build FILE.vst -o OUT && cat OUT/FILE.cpp)
   ```
   (`vsema` and `vparse` build the same way, under `_e2e/`.)
5. Add a curated fixture under `tests/e2e/sema/` or `tests/e2e/codegen/` and
   wire its stem into the `foreach(prog ...)` list in `tests/CMakeLists.txt`.
   Codegen fixtures go in **both** the `.cpp` and `.hpp` lists. A new sema
   message family is added to `sema_grep`. Keep test outputs on per-test
   paths: shared paths race under `ctest -j8`.
6. Compare whole families, not just fixtures. Over the corpus, the spike, or
   line-deletion mutants of a real file, the two compilers should report the
   same diagnostics. It catches what a fixture misses.
7. One commit per slice, then delete the item from ROADMAP (and SPEC_GAPS if
   it is listed there) in the same commit. Update the memory file.

## Standing constraints

- **Build via system cmake** at `build/debug`. CLion's `cmake-build-debug`
  fails on `cxx_std_26`.
- **Stage explicit paths** (`src include tests selfhost examples`, or named
  files). The working tree carries untracked spec drafts the user writes
  independently, and they must never be swept into a commit.
- **No `--no-verify`, no force-push.** Work commits directly to `main`.
- Commit messages: a subject naming the area (`sema:`, `codegen:`,
  `selfhost sema:`, ...), a rationale body, and a `Co-Authored-By:` trailer
  naming the active Claude model. Use `git commit -F <file>` or a quoted
  heredoc so backticks survive.

## Traps when writing Vestra-in-Vestra

- **Types must be declared before use** in a `.vst` file; functions may be in
  any order.
- **A match arm body is one expression.** No `return`, assignment or
  statement list in an arm (except a `{ ... }` statement arm in a statement
  `match`); extract a helper, or use `return match x { ... }`.
- **All arms share one type**: a `String` arm can't sit beside a `StrConst`
  literal. `own("")` is an empty owned `String`, `own(x)` copies a `Str`.
- **There is no `?:`.** Use `if`.
- **`?? ""` on a `Vec[String]` element doesn't compile** in the generated
  C++; use `?? own("")`.
- **Exclusivity:** never pass `env.<field>` in the same call that takes `env`
  as `inout`. Bind it to a local first.
- **`byteAt` returns `UInt8`.** Wrap it in `Int(...)` before mixing it with
  `Int` arithmetic.
- **Prefix keywords keep a trailing space** in `prefixE` nodes: match
  `"try "`, `"copy "`, `"await "`.
- **Reserved names:** `Expr`, `Decl`, `Type` and `Unit` as type names; `sink`,
  `inout`, `linear` and `nonescaping` as identifiers; `pi` and `e` as
  function names (built-in constants).
- **Several helpers already exist** under the names you'd reach for
  (`dotsToSlashes`, `tyArrayElem`, `intToStr`, `containsSub`). Grep before
  adding one, or the build fails on a duplicate definition.
- `intToStr` prints non-negative values only.
- A fixture's reference output must compile too: a `Box`-owning value passed
  as a `read` parameter can't be moved, so such fixtures take `sink`.
- The C++ AST printer dumps unhandled constructs as `/*?expr*/` and similar.
  The self-hosted parser still captures the full node for later phases without
  adding a print case, so the parser differential stays identical.

## Traps in the measurements

- **Count divergent lines, not just identical files.** A file flips only when
  its last divergent family closes; lines are the fine-grained signal.
- **Line counts come from minimal diffs** (`diff -d`, as the sweep does).
  Plain `diff` can inflate a count elsewhere when one hunk is repaired.
- **Check which side is the reference** before characterising a diff, and
  read the generated output directly rather than the `<` / `>` markers.
- **Output must not depend on hashing.** Iterating an `unordered_map` into
  generated text makes the bootstrap's output depend on the C++ library;
  emit in declaration order.
