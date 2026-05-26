# Roadmap

Forward-looking menu of candidate phases for the Vestra-to-C++26
transpiler. The *back-looking* counterpart — what's shipped, with
commit references and known v0.5 limitations — is in
[`HANDOFF.md`](HANDOFF.md). The spec is
[`VESTRA_DRAFT.md`](VESTRA_DRAFT.md), with the kernel-completeness
annex consolidated in [`VESTRA_ANNEX_A.md`](VESTRA_ANNEX_A.md).

The list is **priority-ordered** — number 1 is the next phase to ship
when nothing else is forcing the choice. The user picks from a 4-item
menu each round; the AskUserQuestion draws four entries from this
list, biased toward the top.

Each candidate is sized for a single end-to-end session: AST → parser
→ sema → codegen → unit tests → e2e. Items flagged **(multi-session)**
are bigger swings.

---

## Next up — language polish (priority 1–5)

### 1. `panic` / `abort` / `unreachable` lowering

§10 says panic is terminal — no unwinding, no `deinit`. Lower
`panic(msg)`, `abort()`, `unreachable()` to `std::abort()` /
`__builtin_unreachable()` / a typed `Never`-returning shape. Today
force-unwrap (`!`) and `try!` rely on `std::bad_optional_access` /
`std::bad_expected_access` exceptions — unifying on a real `panic`
primitive moves the language toward §10's spec and opens the door to
the annex's `@panic_handler` later.

### 2. `Span[T]` — non-owning view (§10)

The borrowed, non-escapable view type — lowers to `std::span<T>`.
Lets a function take `Span[T]` over a `Box[T]` / fixed vector
without copying or transferring ownership. **First non-escapable
type in the system** — opens the door to the §5 escape rules and
unblocks the annex's `MutSpan.raw(at:count:)` (§10.5). New TypeKind,
sema rule, codegen lowering, e2e example.

### 3. `Optional` in a Display splice

Small clean win. `is_display_conformant` rejects Optional today
because `std::format("{}", std::optional<T>)` isn't standard. The fix
is a preamble shim that delegates to `T`'s formatter on `.some` and
prints `nil` on `.none`. Tiny surface area; makes `"\(maybeName)"`
interpolation "just work" for the common case.

### 4. Pattern matching enhancements

Three missing match-arm shapes plus the natural follow-on:
range patterns (`case 0..10:`), or-patterns (`case .red | .green:`),
literal patterns (`case 1:` `case "hello":`), and match over a tuple
scrutinee (`match tup { case (1, _): … }`). The bare-tuple form is
already enabled by the §6 tuple-pattern work; the match codegen needs
to recognize it. All four pieces are in `parse_pattern` +
`check_pattern` + `emit_match` — each is small individually.

### 5. Newtype opaque type follow-ons (§3)

Two ergonomic completions on top of the §3 opaque type:
(a) a `derive(Eq, Hash, Debug, Clone, Display) for Q` story for
opaque newtypes — the codegen path is mostly there for the underlying
`T`, needs a `static_cast` through `Q` to reuse it; (b) the inverse
conversion `T(q)` so the user has both directions written the same
way (`Q(t)` already works as `static_cast<Q>(t)`).

---

## Bounded follow-ons (priority 6–10)

### 6. `.mapError(_ f)` on Result

§9 spelling for explicit error-type widening between throws
functions: `try someCall().mapError(toE)`. Sema recognizes `.mapError`
as a built-in Result method (resolves the closure's input/output
types against the Result's `E` and a new `E'`); codegen lowers via
std::expected's `transform_error`. Completes the §9 multi-typed error
story.

### 7. `with X = expr { ... }` block satisfying a capability

The capability checker already recognizes `with` as a satisfier of a
`using C` row, but there's no sema/codegen for opening a `with` block
whose value is bound for the body's duration. Add parse + sema for
`with NAME = expr { ... }`, scope the binding inside the block, codegen
as a sub-scope `{ auto NAME = expr; ... }`. Unblocks the
`Mmio.narrowed(to:)` pattern and tightens the capability story.

### 8. `do { ... } catch e where guard { ... }`

§9 hints at guarded catch arms (the `case … where guard` syntax
already exists). Mirror the syntax + sema in `do/catch` so the user
can dispatch on the error value without writing an inner match.

### 9. Iterator combinators (`zip` / `map` / `filter` / `take`)

Library, not language — but needs `Iterator` and `for` to work,
which both do. Each combinator is a struct with a `next() -> Element?`
method that wraps an inner iterator. Pairs well with the just-shipped
tuple-pattern work: `zip(a, b)` yields tuples and a
`for (x, y) in zip(xs, ys) { ... }` exercises the recent feature set.
First one establishes the template; the rest follow.

### 10. `try?` over a non-Result Optional

Currently `try?` requires Result. Sometimes the user wants
`try? optional_func()` to short-circuit `nil` from a non-throwing
optional-returning call. Either a clean error or a sema-rewrite into
the matching Optional form.

---

## Bigger swings (priority 11+)

### 11. Generics phase 2 (multi-session)

`b9583c8`'s phase-1 covers function generics. Missing pieces:
const generics (`[const N: Int]`); user-defined generic structs /
enums (`struct Box[T] { ... }`); where-clause refinement
(`func f[T](x: T) where T: Eq -> Bool`); protocol-bound enforcement
at the call site. Unblocks `Span[T]` and `PerCpu[T]` as ordinary
library types rather than compiler-known shapes. Probably 2–3
sessions to land cleanly.

### 12. `async` / `spawn` / `select` / `parallel` (§11) (multi-session)

Today parsed but emitted as `unsupported` comments. The C++26 target
has `std::execution` and senders/receivers; mapping async/await to
those is the structurally clean path. `spawn` → detached sender;
`select` → `let_value` composition; `parallel` → `bulk` or a custom
partitioner. A big phase — probably 2–3 sessions.

### 13. `Channel[T]` + `parallel` library (§11)

Depends on async. A typed bounded queue: `send` is sink, `recv`
returns Optional. `parallel(over: xs, by: chunks)` takes a
non-escaping closure and runs it over partitions.

### 14. Quote / splice / declaration macros (§12.4) (multi-session)

The parser already accepts `quote { ... }`. Wiring it through to
declaration-position macros (with `$splice` and `expand`) is the
metaprogramming pillar of §12.

### 15. Ownership / exclusivity phase 2 (multi-session)

Phase 1 is single-pass and linear. Phase 2 needs:
branch-aware flow merging; cross-statement borrow liveness;
partition primitives (`chunks(n)` / `split(at:)`); linear types
(`linear struct Foo { ... }`).

### 16. Capability narrowing + audit trail

Row polymorphism over a generic capability variable; narrowing
(`Mmio.narrowed(to:)`); the `// Safety:` audit-comment trail for
`unsafe` discharges.

### 17. SIMD `[N]T` lowering (§13)

Map fixed-length vector types to `std::experimental::simd` (or target
intrinsics) where available, clean scalar fallback elsewhere. Needs a
target-detection layer the codegen doesn't have yet.

### 18. Content-hashed `@embed` manifest

Today `@embed("path")` reads files relative to the source dir at fold
time. The spec wants paths resolved against a content-hashed
manifest so two builds of the same source under the same config are
byte-identical. Needs a manifest file format + a hash compare in the
driver.

### 19. `Soa[T]` — struct-of-arrays library type (§13)

Inverts AoS storage for SIMD-friendly traversal. Library on top of
generic structs + a small `@layout(soa)` attribute. Design-heavy;
waits for generics phase 2.

---

## Annex track — kernel completeness

A parallel, ordered track for the [`VESTRA_ANNEX_A.md`](VESTRA_ANNEX_A.md)
items. Independent of the language-polish list above — the user can
interleave. Each annex section is roughly one session unless flagged.

### A1. `@noinit static` + section / symbol / weak / alias / visibility (§4.5, §6.7)

The foundation for kernel `.bss` and link-time symbol control. Two
chapters but one phase — both are attribute-only at sema, link-data
at codegen.

### A2. `@inline` directive + layout completeness (§7.8, §6.8)

`@inline(.always | .never | .hint)`; `@repr(union)` untagged
overlay; explicit `@padding`; anonymous `embed`; flexible array
members; comptime layout reflection (`Type.size`, `.alignment`,
`Field.offset`).

### A3. Raw-mint primitives (§10.5)

`Ptr.unchecked(fromAddress:)` / `MutPtr.unchecked(fromAddress:)` +
`Span.raw` / `MutSpan.raw`, with lifetime anchored to the
`RawMemory` discharge. **Depends on roadmap #2 (Span[T])** for the
non-escapable view target.

### A4. `Atomic[T]` memory model (§14.9)

Highest-leverage single annex chunk. Five orderings, four operation
families, strong/weak CAS with the `@retry_loop` shape rule, wide
atomics with `+lse` / `+cx16` feature gates. Every lock-free data
structure in the kernel rests on this.

### A5. Sync intrinsics (§14.10)

Compiler fences, CPU memory barriers, data-cache management,
instruction-cache management, TLB management, pipeline-power hints
(`waitForInterrupt`, `waitForEvent`, `relax`, …). All gated on `Asm`.

### A6. MMIO + sysreg (§14.11, §14.12)

Typed `MmioView[T]` / `MmioRegion[T]` / `MmioWireView[T]` with
implicit volatile + width + endianness discipline; target-knobbed
`Sysreg` namespace with auto-emitted post-write barriers. The two
chapters together remove every single-instruction `asm` wrapper.

### A7. `InterruptsOff` scope + context-switch primitive (§14.13, §14.14)

`with InterruptsOff { … }` with rule 7 (`waitForInterrupt`/Event
rejected) statically checked; `Scheduler.swapContext(saving:,
loading:)` with opaque `Context` (includes mask state) + separate
`FpContext` for lazy-FP.

### A8. `@interrupt(...)` handlers + vector tables (§14.5)

Trap-frame-typed handlers, vector-table arrays, compiler-emitted
prologue/epilogue, the 3.5 lowering rule (slot stride, in-slot vs
trampoline). Closes the ISR-trampoline `.S` files.

### A9. `@boot` + `@kernel_init` (§14.7) (multi-session)

The pre-MMU sub-dialect (seven restrictions) and the post-`@boot`
pre-scheduler regime (five restrictions). Likely two phases: parse +
sema first, then codegen with the runtime-model gates.

### A10. Compiler-emitted intrinsics + `@panic_handler` + `no_libc` (§15.4, §15.5)

`memcpy` / `memset` / `memmove` / stack canaries / overflow traps as
compiler-emitted with Vestra-supplied bodies; the `@panic_handler`
binding; `profile.freestanding.no_libc = true` manifest flag that
audits every external symbol. Together they make "zero references to
libc" a build-time guarantee. **Composes with roadmap #1
(`panic` / `abort` / `unreachable` lowering)** — the lowering target
becomes the `@panic_handler` symbol once both are in.

### A11. `PerCpu[T]` + `Padded[T]` (§14.8)

Per-hart slots with cache-line padding. Library on top of the
target-specific current-hart register read; trivial once §A4–§A6 are
in.

### A12. Pluggable calling conventions + `&decl` (§14.6)

`@extern("conv")` per-target convs; `@symbol` on extern; `&decl`
address-of-symbol. Mostly parser + codegen work; small phase.

---

## Already-shipped (back-pointer to `HANDOFF.md`)

§3 opaque type, §4 string interpolation + layout attrs + Display
conformance, §5 for-in + iterator + struct methods, §6 tuples
(literals + destructuring, nested, at param + match-arm positions),
§9 Optional + Result + try (incl. hoisting + mid-expression in
if/match), §10 Box[T] + Alloc check, §12.1 comptime folding through
phase 7 (functions, locals, loops, vectors, math stdlib,
type-as-callable conversions, @embed), §12.2 reflection phases 1–3
(Type, Field, Field.type), §12.3 derives (Eq, Hash, Debug, Clone,
Display, Default), §12.6 conditional compilation (`@when` + `cfg`),
§17.7 expression precedence (Pratt parser), plus the ownership /
exclusivity / capability / generics phase-1 work described in
[`README.md`](README.md). See [`HANDOFF.md`](HANDOFF.md) for the full
chronological phase log with commit hashes.
