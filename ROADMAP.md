# Roadmap

Forward-looking menu of candidate phases for the Vestra-to-C++26
transpiler. The *back-looking* counterpart, what's shipped with commit
references and known v0.5 limitations, is in
[`HANDOFF.md`](HANDOFF.md). The spec is
[`VESTRA_DRAFT.md`](VESTRA_DRAFT.md), with the kernel-completeness
annex consolidated in [`VESTRA_ANNEX_A.md`](VESTRA_ANNEX_A.md).

The list is **priority-ordered**: number 1 is the next phase to ship
when nothing else is forcing the choice. The user picks from a 4-item
menu each round; the AskUserQuestion draws four entries from this
list, biased toward the top.

Each candidate is sized for a single end-to-end session (AST, parser,
sema, codegen, unit tests, e2e). Items flagged **(multi-session)** are
bigger swings; for those the per-session goal is a self-contained
first slice.

> **Doc hygiene note (2026-05-29).** The previous version of this file
> listed a "language polish (1-5)" and "bounded follow-ons (6-10)"
> section. Every one of those items had already shipped (commits
> `14de903` panic, `f178251` Span, `d8d4ac2` Optional-in-Display,
> `ec26138` pattern matching, `3075829` opaque follow-ons, `11b3a3a`
> .mapError, plus the with-binding / do-catch-guard / zip-take / try?
> follow-ons). The list below was re-derived from `git log` against
> the annex track and the bigger swings. When you finish a phase,
> move it to the "Already shipped" section the same session so this
> file does not drift again.

---

## Next up (priority 1-9)

### 1. Generics phase 2 (multi-session)

`7e93b0e`'s phase 1 covers function generics (opaque GenericParam
types, unification with `expected` propagation, emit as C++
templates). The first phase-2 slice shipped user-defined generic
*structs* (`struct Pair[T] { ... }`, used as `Pair[Int32]`,
monomorphized to C++ class templates with construction inference; see
the HANDOFF top entry). Remaining pieces: user-defined generic *enums*
(`enum Option[T] { case some(T) case none }`); const generics
(`[const N: Int]`); where-clause refinement
(`func f[T](x: T) where T: Eq -> Bool`); protocol-bound enforcement at
the call site; explicit type-args at a construction site
(`Pair[Int32](...)`, where the callee currently parses as an index
expression); and derive / `std::hash` / `std::formatter` emission for
generic structs (partial specializations over `template <class T>`).
The highest-leverage open swing, since it moves `Span[T]` / `PerCpu[T]`
/ `Padded[T]` toward ordinary library types instead of compiler-known
shapes. Generic enums are the natural next slice.

### 2. `async` / `spawn` / `select` / `parallel` (§11) (multi-session)

Today parsed but emitted as `unsupported` comments. The C++26 target
has `std::execution` and senders/receivers; mapping async/await to
those is the structurally clean path. `spawn` becomes a detached
sender, `select` a `let_value` composition, `parallel` a `bulk` or a
custom partitioner. A big phase, probably 2-3 sessions; first slice is
`async func` + `await` over a single sender.

### 3. `Channel[T]` + `parallel` library (§11)

Depends on async. A typed bounded queue: `send` is a sink, `recv`
returns Optional. `parallel(over: xs, by: chunks)` takes a
non-escaping closure and runs it over partitions. Pairs with the
Span[T] non-escapable work already in.

### 4. Quote / splice / declaration macros (§12.4) (multi-session)

The parser already accepts `quote { ... }`. Wiring it through to
declaration-position macros (with `$splice` and `expand`) is the
metaprogramming pillar of §12. Composes with the comptime folding
that already ships through phase 7.

### 5. Ownership / exclusivity phase 2 (multi-session)

Phase 1 (`b0727b9` / `e77f2e0`) is single-pass and linear. Phase 2
needs branch-aware flow merging; cross-statement borrow liveness;
partition primitives (`chunks(n)` / `split(at:)`); linear types
(`linear struct Foo { ... }`). Partition primitives also unblock the
Channel `parallel` partitioner above.

### 6. Capability narrowing + audit trail

Row polymorphism over a generic capability variable; narrowing
(`Mmio.narrowed(to:)`); the `// Safety:` audit-comment trail for
`unsafe` discharges. The `vestra audit` subcommand already exists
(`--sysreg` / `--no-libc` enumerators from `a2e7438` / `3934f9f`), so
this extends an established surface.

### 7. SIMD `[N]T` lowering (§13)

Map fixed-length vector types to `std::experimental::simd` (or target
intrinsics) where available, with a clean scalar fallback elsewhere.
Needs a target-detection layer the codegen doesn't have yet; the
`--target` / `--target-features` plumbing from `312281c` is a starting
point for that layer.

### 8. Content-hashed `@embed` manifest

Bounded single-session item. Today `@embed("path")` reads files
relative to the source dir at fold time; the spec wants paths resolved
against a content-hashed manifest so two builds of the same source
under the same config are byte-identical. Needs a manifest file format
plus a hash compare in the driver. Self-contained.

### 9. `Soa[T]` struct-of-arrays library type (§13)

Inverts AoS storage for SIMD-friendly traversal. Library on top of
generic structs plus a small `@layout(soa)` attribute. Design-heavy;
waits for generics phase 2 (#1).

---

## Annex deepening

The annex track A1-A12 is shipped through first slices and many
follow-ups (see "Already shipped"). What remains are the deeper slices
flagged in those commits:

### A7 second slice: context-switch completion (§14.14)

The first slice (`14b9482`) ships `Scheduler.swapContext(saving,
loading)` against bare `Context` values. The spec form is
`MutPtr[Context]` / `Ptr[Context]`; now that `&decl` exists
(`08b8744`) the pointer-threaded form can be tightened. Adds the
separate `FpContext` for lazy-FP save/restore.

### A8 follow-up: kernel-target vector-table layout (§14.5.3)

`4747e88` ships typed `@interrupt(T)` arrays. The kernel target still
needs per-slot stride alignment (128 bytes on aarch64 EL1) and
`.text.vec_table.<index>` sub-section emission, plus the in-slot vs
trampoline 3.5 lowering rule. Hosted v0.5 drops these because the host
section format differs and dispatch semantics validate without them.

### A9 deepening: boot / kernel_init runtime-model gates (§14.7) (multi-session)

`e773018` ships the sema gates for `@boot` (using-row restriction, no
throws, `[[gnu::naked]]`) and `@kernel_init` (no Async). The audit-time
rules remain: call-graph reachability from each entry, allocator-init
ordering, and rejection of ordinary functions calling `@kernel_init`.
Needs a call-graph pass.

### Lock-free / atomics deepening (§14.9)

- `@retry_loop` function-level escape + `@allow_weak_cas` call-site
  escape for `compareExchangeWeak` outside the admitted loop shapes
  (`fa5b8d0` ships the shape rule; the escapes are deferred).
- AtomicTaggedPointer (`5beef46`) contention-stress lane (a
  multi-threaded Treiber stack with real ABA-defeating semantics) plus
  a load-link / store-conditional emitter for kernel targets.
- Real link-time `.o`-level wide-atomic feature gate (`312281c` ships
  the compile-time gate; the kernel build needs the ELF-feature
  cross-check).

### Audit-surface deepening (§14.12.3 / §15.5)

Cross-architecture barrier-table verification (each gated sysreg
checked against its architecture-manual paragraph), and a build-mode
that walks the *generated* artifact's external-symbol references
(today's `vestra audit` is source-level, seeing what the user declared
rather than what the build linked).

### Pattern-matching + try carry-forwards (§8 / §10)

- Nested tuple destructuring codegen: `let ((a, b), c) = …` type-checks
  in sema but codegen binds only the outer level (C++ structured
  bindings are single-level; needs a sibling-statement hoist).
- Or-patterns with bindings, payloaded-enum or-patterns, and string
  literal patterns (the `ec26138` carry-forwards).
- Mid-expression `try` inside an if/match arm: general per-branch
  hoist contexts (the `e3ad4ec` walk refuses to descend into
  conditional contexts).

---

## Already shipped (back-pointer to `HANDOFF.md`)

Language: §3 opaque type (+ inverse conversion, derive(Default)), §4
string interpolation + layout attrs + Display conformance (incl.
Optional-in-splice), §5 for-in + iterator + struct methods, §6 tuples
(literals + destructuring, nested, param + match-arm positions), §8 /
§17.7 pattern matching (literal / range / or-patterns, tuple
scrutinee, dead-arm warning), §9 Optional + Result + try (hoisting,
mid-expression, `.mapError`, do/catch incl. where-guard +
fall-through propagation), iterator combinators (zip / take / map /
filter), §10 Box[T] + Alloc, Span[T] / MutSpan[T] non-owning views,
panic / abort / unreachable, §16 closures, §17.4 with-binding
(value + type annotation), §12.1 comptime folding through phase 7,
§12.2 reflection phases 1-3, §12.3 derives (Eq, Hash, Debug, Clone,
Display, Default), §12.6 conditional compilation (`@when` + `cfg`,
incl. `cfg.option`), §17.7 Pratt precedence, plus ownership /
exclusivity / capability / generics phase 1.

Annex (first slices + follow-ups): A1 link attributes, A2 `@inline` +
layout reflection, A3 raw-mint primitives (+ `.value` deref), A4 the
full `Atomic[T]` memory model (orderings, bitwise, strong/weak CAS +
retry-loop rule, wide UInt128/Int128 atomics, AtomicTaggedPointer,
target-feature gate), A5 sync intrinsics + cache/TLB management, A6
MMIO (`MmioView` / `MmioRegion` / `MmioWireView`, `@repr(union)`) +
the typed `Sysreg` namespace with auto-emitted post-write barriers and
RO/WO handles (§14.12), A7 `InterruptsOff` + `swapContext` (first
slice), A8 `@interrupt` handlers + typed vector tables, A9 `@boot` +
`@kernel_init` (sema gates), A10 compiler-emitted intrinsics +
`@panic_handler` + `@stack_protector` + `--no-libc`, A11 `PerCpu[T]` +
`Padded[T]`, A12 pluggable calling conventions + `&decl` +
function-pointer type. The `vestra audit` subcommand ships `--sysreg`
and `--no-libc` enumerators.

See [`HANDOFF.md`](HANDOFF.md) for the full chronological phase log
with commit hashes and per-phase rationale.
