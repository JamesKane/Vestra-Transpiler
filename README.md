# Vestra Transpiler

A Vestra-to-C++23 transpiler, written in Modern C++23.

Vestra is the systems language specified in [`VESTRA_DRAFT.md`](VESTRA_DRAFT.md). The
spec is a draft (3.2) describing a Swift-ish language with no GC, ownership-by-mode,
algebraic-effect-style capabilities, typed errors, structured concurrency, and
compile-time metaprogramming. This repo is the *implementation*, intended to grow
toward end-to-end coverage of that spec.

This is **v0**: a working front end plus a partial codegen. The end-to-end pipeline
(transpile → host-compile → run) works on a meaningful subset; the deeper semantics
of the language (ownership, capability resolution, exclusivity, generics
monomorphization) are not yet implemented and are tracked under
[Roadmap](#roadmap).

## Quick start

```bash
# Configure with the default Debug preset
cmake --preset debug

# Build everything (library + CLI + tests)
cmake --build build/debug

# Run all 17 tests, including the end-to-end pipeline
ctest --test-dir build/debug --output-on-failure

# Transpile the example
build/debug/vestra build examples/hello.vst -o /tmp/vestra-out
cat /tmp/vestra-out/hello.hpp
```

A successful test run ends with:

```
100% tests passed, 0 tests failed out of 17
```

## Requirements

- **CMake ≥ 3.25** and **Ninja**
- A **C++23** compiler — tested with Apple Clang 17 / LLVM Clang 17+ / GCC 13+.
  We use `std::format`, `std::span`, designated initializers, and `[[nodiscard]]`
  heavily; older toolchains will not build the project.
- (Optional) **clang-format**, **clang-tidy**, **ccache** — auto-detected by CMake.

## Project layout

```
.
├── CMakeLists.txt                 — top-level build (C++23, warnings, sanitizers)
├── CMakePresets.json              — debug / release / asan / tsan / tidy presets
├── .clang-format / .clang-tidy    — style and lint configuration
├── cmake/
│   ├── CompilerWarnings.cmake     — -Wall -Wextra ... -Werror per target
│   ├── Sanitizers.cmake           — VESTRA_SANITIZE=address+undefined etc.
│   ├── StaticAnalyzers.cmake      — optional clang-format / clang-tidy targets
│   └── Dependencies.cmake         — FetchContent for doctest
├── include/vestra/                — public headers, one subdir per layer
│   ├── diag/                        source manager + diagnostics
│   ├── lex/                         tokens + lexer
│   ├── ast/                         AST nodes + pretty-printer
│   ├── parse/                       recursive-descent + Pratt parser
│   ├── codegen/                     C++ emitter
│   └── driver/                      CLI driver
├── src/                           — implementations mirroring include/
├── tests/
│   ├── lex/                         lexer tests
│   ├── parse/                       parser + printer tests
│   ├── codegen/                     emitter tests
│   ├── e2e/                         end-to-end harness (transpile + compile + run)
│   └── CMakeLists.txt
└── examples/
    └── hello.vst                  — the integration test input
```

## Architecture

The transpiler is a classic layered front end with a single codegen pass:

```
.vst source
    │
    ▼
┌──────────────────┐  SourceManager owns the buffer; SourceLoc/SourceRange
│  diag            │  point into it; DiagnosticReporter renders to stderr
└──────────────────┘
    │
    ▼
┌──────────────────┐  Hand-rolled lexer covering all §17.1 keywords/operators,
│  lex             │  with §3 newline-as-terminator rules (suppress in brackets,
│  → Token stream  │  suppress at binary-operator boundaries).
└──────────────────┘
    │
    ▼
┌──────────────────┐  Recursive descent for declarations/statements, Pratt
│  parse           │  for expressions (precedence table follows §17.7).
│  → AST           │  Each error is a Diagnostic + resync to the next decl.
└──────────────────┘
    │
    ▼
┌──────────────────┐  Two-pass resolver: collect top-level decls → walk
│  sema            │  bodies, resolving identifiers and computing types.
│  → Resolution    │  Side tables map every Expr* to its TypePtr and bound
│                  │  Symbol; bidirectional checking lets literals adapt.
│                  │  Then ownership/move tracking flags use-after-move
│                  │  on sink calls and `return` (phase 1 of §5/§19.2),
│                  │  and the Law of Exclusivity (§5.4 / §19.3) checks
│                  │  every call: overlapping &-borrows of a single
│                  │  place are rejected when any one is `inout`.
└──────────────────┘
    │
    ▼
┌──────────────────┐  Walks the AST and emits C++23 — primitives map to
│  codegen         │  <cstdint>, modules to nested namespaces, structs to
│  → .hpp + .cpp   │  structs, enums to enum class or std::variant.
└──────────────────┘
    │
    ▼
.hpp + .cpp under the chosen output directory
```

The AST (`include/vestra/ast/nodes.hpp`) uses a `Decl` / `Stmt` / `Expr` / `Type` /
`Pattern` base hierarchy with a `NodeKind` discriminator so every visitor is an
exhaustive switch — no virtual dispatch beyond the destructor.

## Quality gates

All of these run by default in `debug`:

- `-Werror` with a strict warning set (`cmake/CompilerWarnings.cmake`) — including
  `-Wconversion`, `-Wsign-conversion`, `-Wshadow`, `-Wold-style-cast`,
  `-Wnull-dereference`, `-Wimplicit-fallthrough`.
- `[[nodiscard]]` on every observer + return value where loss-of-result would be
  a bug; verified by clang-tidy's `modernize-use-nodiscard`.

Available on demand:

- **Sanitizers**: `cmake --preset asan` (ASan + UBSan) or `--preset tsan`. The
  underlying knob is `-DVESTRA_SANITIZE=address+undefined` and is composable.
- **clang-tidy**: `cmake --preset tidy` enables build-time linting; or
  `cmake --build build/debug --target tidy` to lint the whole tree against
  `compile_commands.json`.
- **clang-format**: `cmake --build build/debug --target format` (rewrites) or
  `target format-check` (CI-friendly dry run).
- **ccache**: auto-detected; speeds repeated builds.

## What works today (v0)

- **Lexer** — all §17.1 keywords and operators; numeric literals with underscores,
  hex/oct/binary/exponent; strings with escapes; byte strings; char literals;
  nested block comments; newline-as-terminator with continuation rules.
- **Parser** — modules, imports, funcs (with generics, params, effects clause,
  result), structs, enums, protocols, extensions, opaque types, const/static,
  derive; statements (let/var/return/break/continue/while/for/assignment),
  Pratt-parsed expressions honouring §17.7, types (named with generics, optional,
  vector, tuple, function, some/dyn).
- **AST pretty-printer** — emits canonical Vestra source; doubles as `vestra fmt`.
- **Sema (name resolution + type checking)** — two-pass resolver builds a
  scope chain over the global namespace + function/block scopes; reports
  undefined names, duplicate definitions, type mismatches, wrong arg counts,
  bad operator operands. Bidirectional checking lets integer/float literals
  adopt annotated types (`let x: Int32 = 42` works). Member access types
  struct fields (with `embed` flattening per §6); struct construction
  (`Point(x: 1, y: 2)`) checks labels + missing fields; enum case
  construction (`Color.red` and leading-dot `.red` against expected type);
  match arm typing with exhaustiveness checking for enum scrutinees;
  visibility plumbing in place (private/internal/package/public on every
  Symbol). `vestra check` runs sema alone; `vestra build` refuses to emit
  C++ if sema reports errors (override with `--skip-check`).
- **C++ emitter** — funcs (parameter modes → const&/&/&&), primitives →
  `<cstdint>` aliases, modules → nested namespaces, structs → structs +
  designated-initializer construction, bare enums → `enum class`, payloaded
  enums → `std::variant` wrappers, let/var, while, for, binary/unary ops,
  calls, member access, `Enum::case` lowering for both `Color.red` and
  leading-dot `.red`, and `match` over bare enums as a `switch` inside an
  IIFE. The emitter consumes the resolver's side table when available, so
  context-sensitive lowering (struct vs function call, enum case spelling,
  match scrutinee type) is correct.
- **Ownership / move tracking (phase 1)** — every non-trivial binding
  (Local / Parameter) is flow-tracked through its function. Passing a
  bare identifier to a `sink` parameter or `return`ing it consumes the
  binding. Subsequent uses are rejected with a diagnostic that names
  both the use and the original move site. `copy x` reads x without
  consuming it (the copy becomes the consumed value). Reassigning a
  consumed `var` revives it. Trivial primitives (numerics, Bool, Char,
  Unit) are exempt and freely reusable. Phase 1 is intentionally linear
  — it does not yet merge state across `if` / `match` branches.
- **Law of Exclusivity (phase 1)** — at every call, the live borrows
  of each storage location must be either all `read` or exactly one
  `inout` (§5.4, §19.3). A *Place* is a root binding plus a path of
  field selectors; two places overlap iff they share a root and one
  path is a prefix of the other. So `swap(&p.x, &p.y)` is fine
  (clause 3 — disjoint fields), but `swap(&p, &p.x)` is a parent/child
  overlap and gets rejected with the original borrow site as a note.
  Sink arguments are consumes (handled by ownership), not borrows.
  Phase 2 needs cross-statement liveness, partition primitives (clause
  4), and index-based subviews.
- **End-to-end** — `vestra build` parses, sema-checks (including
  ownership), and produces `.hpp/.cpp` that compiles and runs for
  `examples/hello.vst`, `examples/shapes.vst`, and
  `examples/ownership.vst` (the `e2e_*_run` CTest cases verify each).
  Sink-mode parameters lower to C++ `T&&` with `std::move(...)` at the
  call site, and `let` bindings emit as `auto` (not `const auto`) so
  ownership transfers are actually expressible.

## Roadmap

What's **deliberately stubbed** today, in roughly the order I'd tackle them:

1. ~~**Name resolution + scope tracking**~~ — **done** in v0.5
   (`include/vestra/sema/`).
2. ~~**Type checking**~~ — **mostly done** as of this iteration: arg/return/
   operator typing with bidirectional inference, member access (including
   `embed`), struct construction, enum case construction, match
   exhaustiveness. Still missing: generic instantiation, full visibility
   enforcement (the hook is in place; needs richer "owning scope" tracking),
   protocol conformance verification.
3. ~~**Ownership / move tracking**~~ — **phase 1 done**: linear flow
   analysis catches use-after-move on sink/return, `copy` salvages a
   binding, var reassignment revives one. Phase 2 (branch-aware flow
   merging, linear types, full Trivial detection for user structs)
   remains.
4. ~~**Law of Exclusivity** checker~~ — **phase 1 done**: per-call
   overlap analysis catches conflicting borrows; disjoint fields are
   independent. Phase 2 needs cross-statement borrow liveness,
   recognition of `chunks(n)` / `split(at:)` partitions as disjoint,
   and index-based subview disambiguation.
5. **Capability resolution** (§8, §19.7) — `using` rows resolved to enclosing
   `with` bindings; unsafe capability discharge auditing (`vestra audit`).
6. **Generics monomorphization** (§7) — currently generic decls parse but don't
   specialize; `Matrix[R, C, T]` and friends can't be emitted yet.
7. **`comptime` interpreter** (§12) — the engine of reflection, `derive`, and
   declaration macros. Likely the largest single piece of work.
8. **String interpolation lowering** (§4) — produce `Display::display(into:)`
   calls into a `String` sink; the lexer already has the splitting hooks.
9. **`async` / `spawn` / `select` / `parallel` lowering** (§11) — currently
   parsed as expressions but emitted as `unsupported` comments.
10. **SIMD `[N]T` lowering** (§13) — map to `std::experimental::simd` or
    target intrinsics where available; clean scalar fallback elsewhere.
11. **Conditional compilation `cfg` / `@when`** (§12.6) — needs the
    `comptime` interpreter so the predicate is evaluable at compile time.

Each of these adds one §-block of the spec at a time. The acceptance test for any
of them is: "the relevant `examples/*.vst` file transpiles to C++ that compiles
and produces the right answer."

## Common commands

| Command                                              | What it does                       |
|------------------------------------------------------|------------------------------------|
| `cmake --preset debug`                               | Configure a Debug build            |
| `cmake --build build/debug`                          | Build everything                   |
| `ctest --test-dir build/debug --output-on-failure`   | Run all tests                      |
| `cmake --preset asan && cmake --build build/asan`    | Build with ASan + UBSan            |
| `cmake --build build/debug --target format`          | Rewrite all sources via clang-format |
| `cmake --build build/debug --target tidy`            | Lint the whole tree                |
| `build/debug/vestra build foo.vst -o out/`           | Transpile a Vestra source file     |
| `build/debug/vestra check foo.vst`                   | Run sema only; report any errors   |
| `build/debug/vestra fmt foo.vst`                     | Pretty-print to stdout             |
| `build/debug/vestra build foo.vst --dump-tokens`     | Inspect the token stream           |
| `build/debug/vestra build foo.vst --dump-ast`        | Inspect the parsed AST             |
| `build/debug/vestra build foo.vst --skip-check`      | Bypass sema (debug aid only)       |
