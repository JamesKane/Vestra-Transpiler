# Handoff

Session-continuity notes for the Vestra-to-C++26 transpiler. The
spec lives in [`VESTRA_DRAFT.md`](VESTRA_DRAFT.md); the implementation
strategy and high-level "what works" are in [`README.md`](README.md);
the forward-looking phase menu is in [`ROADMAP.md`](ROADMAP.md). This
file is the *back-looking* counterpart — what shipped, how the work
proceeds, and the rough edges to watch out for.

## Current state

- **339 / 339 tests pass** (`ctest --test-dir build/debug`), Debug
  build with ASan + UBSan via the `asan` preset, format-check clean.
- Tip commit: `c5861ce sema+codegen: §6 tuple literals + destructuring patterns`.
- Toolchain in use: Apple Clang 21 (Xcode 26) at `/usr/bin/c++`,
  CMake 4, Ninja 1.13. clang-format from Homebrew at
  `/opt/homebrew/bin/clang-format`. clang-tidy is optional.
- The pipeline is `lex → parse → resolver → ownership → exclusivity →
  capability → comptime fold → C++ emitter`. Every phase produces
  diagnostics on a shared `DiagnosticReporter`; `vestra build` refuses
  to emit C++ when sema reports an error (override with `--skip-check`).

## How the work has been proceeding

The user drives a tight loop:

1. They type **"let's start the next phase"**.
2. I respond with a single `AskUserQuestion` containing **four**
   well-scoped options (each one paragraph). The options are picked
   from the menu in [`ROADMAP.md`](ROADMAP.md) plus whatever fresh
   candidates the most recent phase opened up.
3. The user picks one.
4. I plan with `TaskCreate`, then implement the phase end-to-end:
   AST → parser → sema → codegen → unit tests → e2e example +
   driver + CMakeLists test entry. Each phase finishes with a
   format-pass, all tests green, and a single commit using the
   convention below.

A few standing constraints, **honored on every commit**:

- **Do not touch** `WHY_VESTRA*.md` or `_posts/`. The user authors
  those independently. Every commit uses **explicit `git add` paths**
  (never `git add -A` / `git add .`) so those files never get swept
  in.
- **No `--no-verify`**, **no force-push to main**.
- Commit message convention: subject like `sema+codegen: §N feature
  (one-liner)`, a paragraphs-of-rationale body, trailer:
  `Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>`.

## Phase log (newest first)

Every phase below ends with all tests green and format-check clean.
The commit hash is the canonical reference; the summary is the
"what + why" for skimming.

| Commit | Phase |
|---|---|
| (this branch) | **§A3 raw-mint primitives — Ptr[T] / MutPtr[T] + Span.raw / MutSpan.raw** — new TypeKind::Ptr / MutPtr lower to `const T* / T*`; four builtin mint shapes (Ptr.unchecked / MutPtr.unchecked / Span.raw / MutSpan.raw) all gated on the RawMemory capability. T for the .unchecked forms is inferred from the call-site expected type; T for .raw comes from the pointer argument. Lowers to `reinterpret_cast<T*>(addr)` and `std::span<T>(ptr, static_cast<size_t>(count))`. The non-escape rule on derived views isn't yet enforced (waits on §10's escape-rule landing); pointers themselves are Trivial and escapable per the spec. Unlocks §A5 cache management, §A6 MMIO, §A8 vector tables. |
| `dd7b8a0` | **§A5 first slice: sync intrinsics (fences + pipeline hints)** — nine free-function builtins (compilerFence, memoryBarrier, syncBarrier, instructionBarrier, waitForInterrupt, waitForEvent, signalEvent, relax, nop) and two builtin enums (BarrierScope, BarrierKind). Codegen runtime preamble adds matching `__vstr::*` shims that lower to `std::atomic_thread_fence` / `std::atomic_signal_fence` for the fences, with arch-gated inline asm for relax (yield on aarch64, pause on x86) and ISB on aarch64 for instructionBarrier. Cache management (§14.10.3), I-cache (§14.10.4), and TLB (§14.10.5) wait on §A3 raw-mint primitives + a Sysreg infrastructure piece. The `Asm` capability gate isn't enforced in v0.5 — kernel-targeting users get the same surface as hosted-targeting users. |
| `fa5b8d0` | **§A4 final slice: `compareExchangeWeak` + admitted-loop shape rule** — synthesizes `compareExchangeWeak` on Atomic[T] (same shape as the strong form) but only admits it inside the §14.9.3 retry-loop shapes: `while !X.succeeded { … }` or `while true { … }`. Free-standing weak CAS is rejected with a diagnostic suggesting `compareExchange`. The resolver tracks `weak_cas_loop_depth_` (incremented when entering an admitted WhileStmt, decremented on exit); the call-site check consults it. Codegen lowers via the same IIFE template, dispatching on member name to `compare_exchange_strong` / `compare_exchange_weak`. The `@retry_loop` function-level escape and the `@allow_weak_cas` call-site escape wait on follow-up phases. |
| `c11afd9` | **§A4 follow-up: bitwise atomics + strong compare-exchange** — fetchAnd / fetchOr / fetchXor join the synthesized method set (renamed to `fetch_and` / `fetch_or` / `fetch_xor` at the C++ layer alongside the existing fetch_add / fetch_sub). New TypeKind::CasResult exposes `succeeded: Bool` and `actual: T` via lookup_field. `compareExchange(expected, desired, success, failure)` lowers to an IIFE that mutates a local `__vstr_e`, calls `compare_exchange_strong`, and bundles the ok-flag + observed value into a `__vstr::CASResult<T>` template instance. Weak CAS + @retry_loop wait on the next slice. |
| `21a9a78` | **§A4 first slice: `Atomic[T]` memory model** — new TypeKind::Atomic + a builtin `Ordering` enum (relaxed / acquire / release / acqRel / seqCst). `Atomic[T]` (T must be primitive in v0.5) lowers to `std::atomic<T>`; Ordering lowers to `std::memory_order` and its cases emit as `std::memory_order_*` literals via a LeadingDotExpr special-case. Synthesized methods: load / store / exchange / fetchAdd / fetchSub (the latter two rename to snake_case at the C++ layer). Static-init takes the inner T's literal (`static c: Atomic[UInt32] = 0`) via a hint-peel in IntLit + an assignable rule. Compare-exchange, weak CAS + @retry_loop, wide atomics (UInt128/Int128), the audit surface, and the per-op ordering validation rules wait on follow-up phases. |
| `99bbfef` | **§A2 follow-up: Field.offset / Field.size / Field.alignment** — the reflection Field struct grew three Int members; the comptime folder populates each entry of `T.fields[i]` with the per-field offset (C-style padding) plus its own size + alignment. Sema eager-folds any member access on a Field value so `T.fields[i].offset` lands in C++ source as a literal. Str / StrConst gained primitive layout (16-byte std::string_view, 8-byte align). |
| `8545c7c` | **§A2 `@inline` directive + compile-time layout reflection** — `@inline(.always | .never | .hint)` lowers to `[[gnu::always_inline]] inline` / `[[gnu::noinline]]` / plain `inline`, in the right C++ order (gnu-attrs → [[nodiscard]] → inline → return type). `StructName.size` and `StructName.alignment` type as Int and fold at sema time via a C-style padding walk (`align_up` each field offset to its align, then round the running total to the struct's max-field align). Today the fold supports primitives, vectors of primitives, and structs of those; anything else (Optional / Box / nominal enum) diagnoses cleanly so the user knows the fold hit an unsizeable type. |
| `0379cf8` | **§A1 link attributes — `@noinit` / `@section` / `@symbol` / `@weak` / `@alias` / `@visibility`** — StaticDecl gets an `attributes` vector + `noinit` flag; the parser skips the `= value` when `@noinit` is present. Sema validates argument shapes (string literal for section/symbol/alias, none for weak/noinit, leading-dot case for visibility). Codegen emits `[[gnu::section(...)]]` / `[[gnu::weak]]` / `[[gnu::alias(...)]]` / `[[gnu::visibility(...)]]` prefixes on both static and func, plus an `asm("name")` trailer for `@symbol` on the function declaration (asm-label on a *definition* is rejected by C++, so the source-side body emits without it). Adds NodeKind::Static to the emit_decl switch. |
| `894e5f4` | **§16 closures in codegen + §9 map / filter combinators** — ClosureExpr `{ p1, p2, … => body }` lowers to a C++ `[&]` lambda with sema-typed params; the expected function type flows from the call site so closure literals get param types from context. New TypeKind::MapIter / FilterIter; `map(xs, f)` and `filter(xs, p)` recognized as free-function builtins alongside zip / take. Runtime preamble adds `__vstr::Map<A, F>` / `__vstr::Filter<A, P>` templates with CTAD deduction guides. .mapError now also accepts closure literals (it was already function-shaped, but the closure now has a path to type itself). |
| `099870d` | **§10 form-aware diagnostic for try-prefix over Optional** — `try? optExpr`, `try optExpr`, `try! optExpr` now each get a tailored suggestion (drop the prefix / use `?.` / use postfix `!`) instead of the generic "must produce a Result<T, E>" message. The old message still fires for non-Result, non-Optional operands. |
| `12adf1f` | **§9 iterator combinators (`zip` / `take`)** — free-function builtins recognized by sema; new `ZipIter` / `TakeIter` type kinds with synthesized `next() -> Element?` for the existing iterator-protocol for-loop. Runtime preamble adds `__vstr::Zip<A, B>` / `__vstr::Take<A>` templates with CTAD deduction guides. The iterator-protocol for-loop now lowers TuplePat as a C++ structured binding over `*__vstr_o`. |
| `2a7abea` | **§9 do/catch where-guard fall-through propagation** — sema requires an enclosing `throws(E)` matching the caught error; codegen extends the IfExpr/MatchExpr cond-hoist machinery to DoCatchExpr-with-guard, replacing the v0.5 panic-on-fall-through with `return std::unexpected{e}` from the lifted lambda. |
| `c5861ce` | **§6 tuple literals + destructuring patterns** — TupleLitExpr (`(e1, e2, …)`) and TuplePat (`let (a, b) = expr`); TupleType lowers to `std::tuple<…>`; patterns lower to C++17 structured bindings. ForStmt supports tuple patterns too. |
| `9536216` | **§10 Box[T] heap pointer + Alloc capability** — `Box[T]` → `std::unique_ptr<T>`; `Box.new(v)` → `std::make_unique<T>(v)`; `box.value` → `*box`. Capability checker rejects `Box.new` without `using Alloc` in scope. |
| `20f26e3` | **§4 layout attributes** — `@repr(packed)`, `@repr(align(N))` / `@align(N)`, `@bits(N)` on struct fields. Codegen emits `__attribute__((packed))`, `alignas(N)`, and C++ bit-fields. |
| `d7e7589` | **§3 opaque type — explicit construction + .value extraction** — `Q(t)` → `static_cast<Q>(t)`; `q.value` → `static_cast<UnderlyingT>(q)`. Static_assert in e2e pins the distinct-type and not-convertible properties. |
| `b32dbe4` | **§9 bare `catch NAME` with E inference** — drops the `(NAME: E)` annotation requirement. New per-function-scoped `do_catch_infer_stack_` captures the body's try-call error type; mismatched tries reported with the "use annotated form" hint. |
| `9f617e1` | **§4 Display protocol + interpolation conformance** — `is_display_conformant` check on every splice; `derive(Display)` shares the `derive(Debug)` std::formatter emission (single spec when both are present). |
| `d46fd4e` | **§5 `for x in xs` over Range and Iterator** — Range form lowers to counted `for`; iterator form lowers to `while-true / next() / break`. Drive-by: struct methods now emit inline (were silently dropped). |
| `c7dcf11` | **§12.3 derive(Clone) — explicit copies** — surfaces `.clone() -> T` via the resolver's `decl_derives` helper; emits `[[nodiscard]] T clone() const { return *this; }`. Bugfix: tightened CallExpr struct-construction codegen to only fire when the callee names a Struct (was returning `T{}` for any call returning T). |
| `6a089ca` | **derive(Hash) covers payloaded enums (§12.3)** — std::visit + constexpr-if chain, alt-index seed so different cases with equal bytes don't collide. |
| `22a3905` | **§9 do / catch inline error handling** — `do { body } catch (NAME: E) { handler }` lowers to nested IIFEs over std::expected. New `emit_sema_type` helper renders resolver-canonical types for the inner lambda's return slot. Hand-walks the do-body's BlockExpr trailing for try-hoisting. |
| `1d088aa` | **§9 optional chaining (`a?.b?.c`)** — MemberExpr.is_optional_chain flag; lowers to `.transform(…)` or `.and_then(…)` based on whether the field is already Optional (flattens). |
| `413d5c0` | **Small simplification pass on cpp_emitter.cpp** — namespace-path lambda dedup (5 copies → 1), `std::any_of` for has_type_generics, dedup'd TryExpr fallback. |
| `e3ad4ec` | **Hoist mid-expression `try` to a stmt-position let-binding** — fixes the v0.5 limitation noted in 9c1888d: every `try EXPR` is pre-emitted as the canonical 3-line escape (`auto __vstr_tN_r = …; if (!…) return std::unexpected{…}; auto __vstr_tN = *__vstr_tN_r;`) before the statement; `emit_expr(TryExpr)` substitutes the hoisted name. Walk refuses to descend into conditional contexts. |
| `9c1888d` | **§9 Result / throws / try / throw → std::expected** — TypeKind::Result; throws_stack_; per-form lowering (Propagating / Optional / Forced); emit_stmt_expr helper for ThrowExpr / IfExpr branches; `[[maybe_unused]]` on the catch binding. |
| `3af5632` | **§9 Optional end-to-end (nil / ?? / if let / !)** — KwNil; NilLit; UnaryOp::Unwrap (postfix `!`); IfExpr.let_name/let_init; implicit `T → T?` wrap; int-literal Optional-peel for type-hint propagation. |
| `61a0fc5` | **codegen: derive(Debug) emits a std::formatter spec (§12.3)** — three shapes (struct / bare enum / sum-type enum via std::visit + constexpr-if). |
| `848cb40` | **codegen: derive(Hash) emits std::hash specialization (§12.3)** — struct path only; boost-style hash_combine. Post-namespace walk because `std::hash<…>` must live in ::std. |
| `b0c113b` | **codegen: derive(Eq) emits a defaulted operator== (§12.3 phase 1)** — derives_by_target_ index built at the top of emit(); injected into struct/enum/case_t. Sum-type enum's wrapper gets it too. |
| `5c17c97` | **sema: reflection adds Field.type / Type.name (§12.2 phase 3)** — synthetic Type + Field StructDecls; positional ComptimeValue::elements vector mirrors them. |
| `52cf2e7` | **codegen: match-expression lowering + sum-type enum construction** — match over bare enum → switch IIFE; match over payloaded enum → std::visit + constexpr-if chain with `auto&&` payload bindings; `Shape.circle(radius: 1.0)` → `Shape{Shape::circle_t{1.0}}`. |
| `e62d3fd` | **sema: reflection promotes T.fields to [N]Field (§12.2 phase 2)** — Field carries `.name: Str`. |
| `92ac7ec` | **sema + codegen: reflection over struct field names (§12.2 phase 1)** — `T.fields` returns `[N]Str` at fold time. |
| `8e8402e` | **lex + sema + codegen: string interpolation lowering (§4 phase 1)** — Begin/Part/End token split with splice paren depth on a stack; lowers to `std::format(…)`. |
| `666c37e` | **sema + codegen: @embed compile-time file embedding (§12.1 phase 7)** — EmbedExpr; EmbedReader passed through the driver; types as `[N]UInt8`. |
| `9bf2c25` | **sema + codegen: type-as-callable conversions (§17.x / §12.1 phase 6)** — `Float64(i)` / `Int32(x)` → `static_cast<…>(arg)`. |
| `98501be` | **sema: comptime math stdlib (§12.1 phase 5)** — `sin`/`cos`/`tan`/`sqrt`, `tau`/`pi`/`e` dispatched to host `<cmath>` at fold time. |
| `59f0cc0` | **sema + codegen: comptime vectors as values (§12.1 phase 4)** — fold-time `Vector<T>` values, brace-init lowering. |
| `b9583c8` | **sema + codegen: comptime locals + loops (§12.1 phase 3)** — locals, `for`/`while` inside `comptime { ... }`, MaxLoopIterations cap. |
| `7059c9e` | **sema + codegen: conditional compilation (§12.6 phase 1)** — `@when(predicate)` decl gating; `cfg.{os,arch,endian,pointerBits,profile}`. |
| `6f44b3a` | **license: BSD-2-Clause LICENSE + SPDX headers on every source file**. |
| `db2c991` | **sema + codegen: comptime function calls (§12.1 phase 2)** — calls to `comptime func` declarations, 64-deep recursion cap. |
| `e3a5273` | **build: bump to C++26 + small surface refactors**. |
| `6230280` | **sema + codegen: comptime folding (§12.1 phase 1)** — tree-walking evaluator over the pure subset (literals, refs to earlier folded consts, arithmetic + logic, comparisons, `if`). |
| `7e93b0e` | **sema + codegen: generics monomorphization (phase 1)** — opaque GenericParam types; unification with `expected` propagation; emit as C++ templates. |
| `79d9a1b` | **sema: capability resolution (phase 1)** — `using` rows propagated and verified; `with` blocks satisfy them; async / await / spawn require Async. |
| `e77f2e0` | **sema: Law of Exclusivity (phase 1)** — per-call overlap analysis catches conflicting borrows; disjoint fields independent. |
| `b0727b9` | **sema: ownership / move tracking (phase 1) + codegen lowering** — linear flow analysis; sink/return consumes; `copy x` salvages; var reassignment revives. |
| `3c91c68` | **simplify: drop dead AST nodes, tokens, helpers, and SymbolKinds**. |
| `16a02e8` | **sema: member access, enum cases, match exhaustiveness, struct ctors**. |
| `6764277` | **Initial commit: Vestra-to-C++23 transpiler**. |

## V0.5 limitations to know about

These are real holes that don't break the build but constrain what the
user can write. Each one is in scope for a follow-on phase — see the
relevant entry in [`ROADMAP.md`](ROADMAP.md).

- **Mid-expression `try` inside an if/match arm** falls back to
  `.value()` (panic on error). The hoist pass (`e3ad4ec`) refuses to
  descend into conditional contexts so an arm's try isn't pulled out
  to the parent scope; per-arm rewriting via `emit_stmt_expr` covers
  it only when the try is the arm's whole body. A general fix needs
  per-branch hoist contexts.
- **Nested tuple destructuring** (`let ((a, b), c) = …`) type-checks
  in sema (TuplePat recurses through `bind_tuple_pattern`) but codegen
  only binds the *outer* element. C++ structured bindings are
  single-level — fixing this needs a sibling-statement hoist
  (`auto __t0 = …; auto [a, b] = __t0;`).
- **Bare-enum `derive(Clone)`** is sema-rejected: C++ `enum class` has
  no method slot for `.clone()`. The user gets the implicit copy on
  the value directly; only struct + sum-type enum get the synthetic.
- **derive(Display) and derive(Debug) produce identical output** in
  v0.5; both emit the same `std::formatter` spec. The spec's
  customizable `display(into: Sink)` body is a follow-on.
- **`do { body } catch NAME { handler }` with a body that has no
  try** is rejected ("requires at least one `try`"). The annotated
  form `catch (NAME: E)` is the workaround.
- **Struct method emission is minimal** (added in `d46fd4e` so the
  iterator e2e would work): inline-in-header only, no
  const-qualification (so an `inout` method works), no generics, no
  out-of-line bodies. Parameter modes map to `const &` / `&` / `&&`.
- **Optional chaining flattening** uses a StructDecl-field-AST scan
  rather than the sema type — works for direct struct member access,
  not for method-returning-Optional. Most chains are field chains so
  the practical loss is small.
- **Ownership / exclusivity / capability are all "phase 1"** as
  described in README. Branch-aware flow merging, partition
  primitives, row polymorphism, capability narrowing all deferred.
- **Tuple patterns at sites other than `let` / `var` / `for`** —
  e.g. function-parameter destructuring, match-arm payload — aren't
  wired through yet.
- **`@repr(C)` and unknown @repr modes** are accepted silently. The
  spec also lists `@repr(wire)` for cross-machine byte order; that
  one wires when the manifest piece lands.
- **Sema rule for sink-arg test functions** — many sema tests use
  names like `read`, which collides with the `read` parameter-mode
  keyword. Convention: use `deref`, `take`, `consume`, etc. instead.

## How to continue

The proven shape of the next session: the user opens with **"let's
start the next phase"** (verbatim). The immediate next move is to
spawn an `AskUserQuestion` with **four** options drawn from
[`ROADMAP.md`](ROADMAP.md). Pick options that haven't been offered in
the most recent two rounds — variety keeps the menu fresh. Don't
preemptively start any phase; the user always picks.

For each phase, the implementation order has been:

1. `TaskCreate` a small handful of sub-tasks (AST, parser, sema,
   codegen, tests, commit).
2. AST node first (so the parser has somewhere to put what it parses).
3. Parser next (smallest possible diff in `parse_*`).
4. Sema after that (type-checking + symbol surfacing).
5. Codegen last (now that sema can supply type info).
6. Unit tests in `tests/sema/` + `tests/codegen/` covering at least
   the happy path + one diagnostic.
7. E2E example: `examples/<phase>.vst` + `tests/e2e/main_<phase>.cpp`
   + a CMakeLists entry that drives transpile → host-compile → run.
8. `cmake --build build/debug --target format` then `format-check`
   then a full `ctest`. **All green** before the commit.
9. Single commit with the standard message shape.

## File map (where to look)

```
src/
├── lex/        — tokeniser. Adding a keyword → token.hpp + token.cpp.
├── parse/      — recursive-descent / Pratt. parse_*. parse_pattern.
├── ast/        — AST node defs, printer (≈ `vestra fmt`).
├── sema/
│   ├── types.{hpp,cpp}         — TypeArena, TypeKind, make_optional/box/…
│   ├── resolver.{hpp,cpp}      — name resolution + type checking
│   ├── ownership.cpp           — phase 1 move tracking
│   ├── exclusivity.cpp         — phase 1 borrow conflict checker
│   ├── capability.cpp          — phase 1 `using` row enforcement
│   ├── comptime.cpp            — folding interpreter
│   ├── builtins.cpp            — comptime stdlib (sin/cos/sqrt/tau/…)
│   └── scope.{hpp,cpp}         — ScopeStack
├── codegen/    — cpp_emitter.{hpp,cpp}. emit_decl / emit_stmt /
│                  emit_expr / emit_type / emit_sema_type /
│                  emit_stmt_expr / collect_try_hoists.
├── diag/       — DiagnosticReporter, SourceManager
└── driver/     — CLI (`vestra build` / `check` / `fmt`)

include/vestra/   — public headers mirroring src/

tests/
├── lex/       — lexer_test.cpp
├── parse/     — parser_test.cpp, printer_test.cpp
├── sema/      — resolver_test.cpp, ownership_test.cpp,
│                  exclusivity_test.cpp, capability_test.cpp,
│                  generics_test.cpp, comptime_test.cpp
├── codegen/   — cpp_emitter_test.cpp (SemaEmitFixture for sema-backed cases)
├── e2e/       — main_*.cpp drivers, one per example
└── CMakeLists.txt — `e2e_<phase>_{transpile,compile,run}` per example

examples/      — one .vst per phase, plus older shapes/hello/etc.
```

## Useful tactical knowledge

- **`read` is a reserved keyword** (parameter mode). Never name a test
  function `read` — use `deref` or `consume`.
- **`embed` is reserved too** (used in module paths and for `@embed`).
  Module names like `examples.embed` will not parse — use
  `examples.embeds` or `examples.opaque` → `examples.newtype`.
- **`opaque` is also reserved** in module paths.
- **`?` is lexed alone** (not as part of `?.`). The parser does a
  `peek(1) == Dot` lookahead to recognize optional chaining.
- **`??` lexes as a single `QuestionQuestion` token**, distinct from
  `? ?`.
- **`??` inside `std::format` strings** is rejected by Clang as a
  trigraph — write the spec text as `"'?\\?'"` in diagnostics.
- **The build is `-Werror`** with `-Wall -Wextra -Wshadow
  -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wunused
  -Woverloaded-virtual -Wconversion -Wsign-conversion
  -Wnull-dereference -Wdouble-promotion -Wformat=2
  -Wimplicit-fallthrough -Wmissing-declarations`. Most flake comes
  from `-Wswitch` when you add an enum value — touching `UnaryOp`,
  `BinaryOp`, `TypeKind`, or `NodeKind` means walking every `switch`.
- **The codegen emits inline structs via `emit_struct`**; methods on a
  struct go *inside* the struct body, not as out-of-line defs.
- **`SemaEmitFixture` in `cpp_emitter_test.cpp`** holds the
  SourceManager + DiagnosticReporter + TypeArena + parsed unit +
  Resolver alongside the EmittedUnit. Use it whenever the test
  needs the resolver's type / symbol side table (which is most
  things that aren't a primitive-only check).
- **Inline LSP diagnostics from the harness are noise** for this
  project (the LSP doesn't see the project's compile commands). Real
  errors come from `cmake --build build/debug`.

## Standing dev-loop commands

```bash
# Configure (once per worktree)
cmake --preset debug

# Build + run all tests + format gate
cmake --build build/debug
( cd build/debug && ctest --output-on-failure )
cmake --build build/debug --target format-check

# Format-pass before commit
cmake --build build/debug --target format

# Transpile + inspect a single .vst
build/debug/vestra build examples/<phase>.vst -o /tmp/vst-out
cat /tmp/vst-out/<phase>.hpp /tmp/vst-out/<phase>.cpp
```
