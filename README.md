# Vestra Transpiler

A compiler for Vestra, the systems language specified in
[`VESTRA_DRAFT.md`](VESTRA_DRAFT.md), that translates Vestra source to C++26.

Vestra is a Swift-flavoured systems language with no garbage collector:
ownership by parameter mode, capabilities as an effect row, typed errors,
structured concurrency, and compile-time metaprogramming. Its kernel-facing
surface (atomics, MMIO, raw memory, interrupts, boot code) is consolidated in
[`VESTRA_ANNEX_A.md`](VESTRA_ANNEX_A.md).

The repository holds two compilers:

- **The bootstrap compiler** (`vestra`), written in C++26. It is the complete,
  working implementation: lexer, parser, a full semantic checker, and a C++
  emitter. Every program in [`examples/`](examples/) transpiles, compiles and
  runs through it.
- **The self-hosted compiler**, the same compiler rewritten in Vestra under
  [`selfhost/`](selfhost/). It is built by the bootstrap compiler and proved
  phase by phase with differential tests: given the same input, it must
  produce byte-for-byte the same output as `vestra`. This is the active line
  of work.

## Status

*As of 2026-11-14. [`HANDOFF.md`](HANDOFF.md) carries the live numbers.*

| Self-hosted phase | Source | Proven against `vestra` by | State |
|---|---|---|---|
| Lexer | `selfhost/lexer.vst` | the `--dump-tokens` token stream | done |
| Parser | `selfhost/parser.vst` | the `--dump-ast` canonical AST print | done (142 of 143 examples) |
| Sema | `selfhost/sema.vst` | `vestra check` diagnostics | done, every diagnostic family |
| Codegen | `selfhost/codegen.vst` | the generated `.cpp` and `.hpp` | **127 of 150 examples identical** |

The codegen target is the whole `examples/` corpus byte-identical. Of the
remaining divergence, most sits in families deliberately left for last
(compile-time quote/splice, `select`, and comptime folding, which the
bootstrap evaluates away).

The test suite has **1564 tests**, all passing: unit tests for each layer,
155 end-to-end runs of the examples, and 274 self-hosting differentials.

## Quick start

```bash
cmake --preset debug              # configure into build/debug (Ninja)
cmake --build build/debug         # build the library, the CLI and the tests
ctest --test-dir build/debug -j8  # run the suite

# Transpile, compile and run a program
build/debug/vestra build examples/args_demo.vst -o /tmp/vestra-out
c++ -std=c++26 -I/tmp/vestra-out /tmp/vestra-out/args_demo.cpp -o /tmp/args_demo
/tmp/args_demo hello world        # prints each argument
```

A clean test run ends with `100% tests passed out of 1564`.

A module-less `func main()` becomes the C++ entry point, as in `args_demo`.
Most examples are modules with no entry point; their end-to-end tests link
them against a small C++ driver in `tests/e2e/main_<name>.cpp`.

## Requirements

- **A C++26 compiler.** Tested with Apple Clang 21 (Xcode 26); LLVM Clang 19+
  and GCC 15+ should work. The code leans on `std::format`, `std::print`,
  `std::expected`, `std::span` and designated initializers, so older
  toolchains will not build it.
- **CMake and Ninja.** The build declares CMake 3.25, but configuring
  `cxx_std_26` needs a CMake recent enough to know the C++26 feature; CMake 4
  is what is tested. (An IDE's bundled CMake can lag behind and fail at
  configure time; use the system one.)
- Optional: **clang-format**, **clang-tidy** and **ccache**, picked up
  automatically.

## Using the compiler

| Command | What it does |
|---|---|
| `vestra build FILE.vst -o DIR` | Check, then transpile to `DIR/FILE.hpp` and `.cpp`. Imports are followed and each module gets its own pair. Refuses to emit C++ if sema reports errors. |
| `vestra check FILE.vst` | Parse and run the semantic checks only; print any diagnostics. |
| `vestra fmt FILE.vst` | Pretty-print the source in canonical form. |
| `vestra expand FILE.vst` | Print the source after declaration-macro expansion. |
| `vestra audit FILE.vst --sysreg \| --no-libc \| --safety` | List every site that crosses a discipline boundary (system-register writes and their barriers, `extern` symbols under the freestanding profile, unsafe capability grants and their `// Safety:` comments). |

Useful `build` flags: `-I DIR` (import search root), `--target=aarch64|x86_64|riscv64`
with `--target-features=...`, `--no-libc` (freestanding), `--dump-tokens` and
`--dump-ast` (inspect the front end), and `--skip-check` (bypass sema, a debug
aid). `vestra help` lists them all.

## How the bootstrap compiler works

```
.vst source
  │  lex      hand-written lexer: every keyword and operator, numeric literals
  │           with separators, interpolated strings, newline-as-terminator rules
  ▼
  │  parse    recursive descent for declarations and statements, a Pratt
  │           parser for expressions, error recovery to the next declaration
  ▼
  │  sema     name resolution and bidirectional type checking, then ownership
  │           (move tracking), the Law of Exclusivity, capability checking,
  │           generics, and the comptime folder (incl. @when and reflection)
  ▼
  │  codegen  a C++26 header + source per module
  ▼
.hpp + .cpp
```

The C++ is roughly 31,000 lines across `src/` and `include/vestra/`, about
12,500 of them in sema and 8,200 in codegen. Every phase reports through a
shared diagnostic reporter, and the AST uses a single `NodeKind` discriminator
so each visitor is an exhaustive switch.

## What the language covers today

The bootstrap compiler implements a large subset of the spec end to end.
Each feature has an example under `examples/` and an end-to-end test that
compiles and runs it.

- **Core:** functions, structs with methods, enums (plain and payload-carrying),
  modules and multi-file imports, `let` / `var`, loops including `for` over
  ranges, containers and user iterators, tuples and destructuring, string
  interpolation, opaque newtypes.
- **Ownership and safety:** `read` / `inout` / `sink` parameter modes with
  move tracking, use-after-move diagnostics, the Law of Exclusivity, and
  capability rows (`using Alloc`, `with X { ... }`) checked at every call.
- **Types:** `T?` with `??`, `if let`, `!` and optional chaining; `Box[T]`;
  `Vec`, `HashMap`, `Span` and `MutSpan`; generic functions, structs and
  enums with protocol bounds, `where` clauses and const generics.
- **Errors:** `throws(E)` as `std::expected`, the `try` family, `throw`,
  `do` / `catch` with typed and guarded handlers, `.mapError`.
- **Pattern matching:** exhaustive `match` with payload bindings, guards,
  or-patterns, tuple and value patterns, in any expression position.
- **Concurrency:** `async` / `await`, `spawn`, channels, `select`, `parallel`.
- **Metaprogramming:** the comptime folder, `@when` conditional compilation,
  reflection over fields, `@embed` (with an optional content-hashed manifest),
  quote / splice and declaration macros, and `derive` for Eq, Hash, Clone,
  Default, Debug and Display.
- **Systems (Annex A):** atomics and memory orderings, `PerCpu` and `Padded`
  storage, raw pointers and spans, MMIO views, typed system registers with
  automatic barriers, interrupt handlers and vector tables, boot and naked
  functions, link attributes (`@section`, `@symbol`, `@weak`, ...), struct
  layout (`@repr(packed)`, `@align`, `@bits`, unions), sync / cache / TLB
  intrinsics, a custom panic handler, SIMD, `Soa[T]`, and a freestanding
  (no-libc) profile.

## Self-hosting

Each self-hosted phase is a single `.vst` file. The bootstrap compiler
transpiles it to C++, the host compiler builds it into its own binary (`vlex`,
`vparse`, `vsema`, `vgen`), and that binary is run against the bootstrap on the
same inputs. Later phases carry a copy of the earlier front end, so
`codegen.vst` (about 11,400 lines) contains its own lexer, parser and the
ported type checker.

The codegen corpus is measured with:

```bash
./scripts/sweep_codegen_corpus.sh --build --report build/sweep/before.txt
# ... make a change ...
./scripts/sweep_codegen_corpus.sh --build --compare build/sweep/before.txt
```

`--compare` reports the identical-example count, the total divergent lines
(counted from minimal diffs), and any example that got worse.
[`HANDOFF.md`](HANDOFF.md) has the workflow, the ranked list of what is left,
and the known gaps.

## Project layout

```
.
├── src/, include/vestra/   the bootstrap compiler: diag, lex, parse, ast,
│                           sema, codegen, driver
├── selfhost/               the self-hosted compiler (lexer, parser, sema, codegen)
├── examples/               150 Vestra programs, the end-to-end and codegen corpus
├── tests/
│   ├── lex/ parse/ sema/ codegen/   unit tests (doctest)
│   └── e2e/                drivers for running examples, plus the self-hosting
│                           fixtures (e2e/codegen, e2e/sema)
├── scripts/                the codegen corpus sweep
├── cmake/                  warnings, sanitizers, static analysis, dependencies
├── VESTRA_DRAFT.md         the language specification
├── VESTRA_ANNEX_A.md       the kernel-completeness annex
├── ROADMAP.md              candidate next phases for the bootstrap compiler
├── HANDOFF.md              where the active work stands and how it proceeds
└── WHY_VESTRA_VS_*.md      comparisons with Rust, C++26 and Embedded Swift
```

## Quality gates

On by default in the `debug` preset:

- `-Werror` over a strict warning set (`cmake/CompilerWarnings.cmake`),
  including `-Wconversion`, `-Wsign-conversion`, `-Wshadow`,
  `-Wold-style-cast` and `-Wimplicit-fallthrough`.
- `[[nodiscard]]` wherever dropping a result would be a bug.

On demand:

| Command | What it does |
|---|---|
| `cmake --preset asan && cmake --build build/asan` | Build with AddressSanitizer + UBSan (`--preset tsan` for ThreadSanitizer) |
| `cmake --build build/debug --target format` | Rewrite sources with clang-format (`format-check` for a dry run) |
| `cmake --build build/debug --target tidy` | Lint the tree with clang-tidy (the target exists when clang-tidy is installed; `--preset tidy` lints during the build) |

## License

BSD 2-Clause; see [LICENSE](LICENSE). Each source file carries a matching
`SPDX-License-Identifier: BSD-2-Clause` header.
