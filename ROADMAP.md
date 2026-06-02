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
templates). Four phase-2 slices shipped: user-defined generic *structs* (`e74f1ad`,
`struct Pair[T] { ... }`), generic *enums* (`1bfff52`, `struct`-of-variant
templates), *const generics* (`8bf316a`, `struct Buffer[T, const N: Int] {
var data: [N]T }`, monomorphized to `template <class T, std::size_t N>`),
and *protocol bounds* (`func eq[T: Eq](...)` / `where T: Comparable`,
enforced at the call site and at struct/enum instantiation, with a C++20
`requires` clause for the concept-mappable bounds). See the HANDOFF top
entries. Remaining pieces: explicit type-args at a construction site
(`Pair[Int32](...)`, where the callee currently parses as an index
expression); a const generic as a runtime/comptime *value* in a body
(`for i in 0..N`; today N is only an array length); const-expression
array lengths (`[N + 1]T`); bound enforcement gating body operations;
transitive bound checking; user-defined-protocol conformance (no general
table yet); and `requires` clauses on generic structs/enums (functions
only today). (Leading-dot payloaded-enum construction, const generics on
functions, and derive emission for generic types were closed as
follow-ons.) The type-parameter surface is now substantially complete —
the remaining items are minor refinements. Explicit construction-site
type-args is the most user-visible one left; otherwise generics phase 2
is effectively done and the next phase is best drawn from the bigger
swings (async, macros, ownership phase 2) or the annex deepening.

### 2. `async` / `spawn` / `select` / `parallel` (§11) (multi-session)

Slices shipped: `async func` + `await` over a `__vstr::Task<T>` coroutine
shim (the host libc++ ships `<execution>` policies but not P2300
senders), `spawn` → `Future[T]` consumed by `await`, `select` over future
arms (an `await_ready` IIFE), `parallel` (a builtin that splits a
`MutSpan[T]` into N disjoint sub-views and runs a non-escaping worker on
each), and the cooperative scheduler:

  * **slice 1** (`6cb4df6`) — a real single-threaded scheduler with *lazy*
    Task (initial_suspend == suspend_always) and symmetric-transfer
    completion, replacing the earlier eager/suspend_never model. Codegen
    lowering unchanged; the inversion lives in the runtime preamble.
  * **slice 2** (`e981743`) — **blocking channel receive**, the first
    observable use of real suspension: `await ch.receive()` parks the task
    on an empty open channel; `send` wakes one parked receiver, `close`
    wakes all (nil == closed-and-drained). `spawn` now schedules its task
    on the run-loop so a parked consumer and a running producer coexist.
    Also the foundational **coroutine param-lifetime fix** (caught by
    ASan): an `async func` emits its read params *by value* (not `const
    T&`), since a coroutine doesn't copy reference params into its frame —
    the codegen half of "no borrow across await".
  * **slice 3** — **blocking channel select**: a `select` whose arms are
    channel receives (`on let v = ch.receive(): …`) and has *no* default
    parks on *every* arm's channel and resumes on the first to deliver,
    binding that arm's `T?` (nil == closed-and-drained); ties go to the
    earlier arm. Lowers to a nested `Task<RT>` coroutine the caller
    co_awaits, parking one shared `Waiter` (a `fired` flag makes a
    multi-arm wake idempotent — no double-resume) via a `SelectAwaiter`
    over a non-template `SelectableState` base, so arms over channels of
    differing element types park uniformly. A select *with* a default
    stays a non-blocking poll (now `sel_state()->ready()` for channel
    arms, `await_ready()` for future arms).
  * **slice 6** — **mixed channel/future select arms**, with *blocking
    join* semantics. A no-default select mixing `ch.receive()` arms and
    future arms parks until any arm fires — a channel delivers *or* a
    future's spawned task completes — so a channel can win a race over a
    still-pending future. Built on a non-template `Selectable` base shared
    by `SelectableState` (channels) and `FutureSelectable<T>` (futures),
    plus a `select_waiter` slot on the `Task` promise that `final_suspend`
    fires (idempotently, via the same `fired` flag, so a channel arm and a
    future completing can't double-resume). The generalized `SelectAwaiter`
    arms one shared `Waiter` on every arm; the lowering materializes each
    future arm as a stable local and dispatches with `sel_take()` (channel)
    or `.get()` (future). A `timeout` arm still requires the all-channel
    form (rejected on a mixed select); a pure-future no-default select
    stays a poll (futures are always ready).
  * **slice 4** — **timeout select arm**: `timeout <ms>: body` (a
    contextual keyword) on a blocking channel select fires its body if no
    channel arm delivers within the wall-clock delay. The scheduler gains
    a timer queue; when the ready queue drains (every task parked) `run()`
    sleeps (`std::this_thread::sleep_until`) to the earliest live deadline
    and fires that waiter. The `SelectAwaiter` registers the timer on
    suspend tied to the same shared `Waiter`, so a channel arm winning the
    race marks the timer fired (dropped without sleeping); if the timer
    fires first the awaiter returns the index past the last channel arm.
    Sema rejects `timeout` + `default` (contradictory), `timeout` on a
    future select, and a non-Duration delay.
  * **slice 5** — **`Duration` type** (Swift-like). A non-generic value
    type lowered to `__vstr::Duration` (a signed nanosecond count).
    Construction via the typed leading-dot factories `.seconds(n)` /
    `.milliseconds(n)` / `.microseconds(n)` / `.nanoseconds(n)` (resolved
    against the expected type) and the explicit `Duration.seconds(n)`
    form. Arithmetic mirrors Swift: `Duration / Duration -> Float64` (a
    dimensionless ratio), `Duration +/- Duration -> Duration`, and
    comparisons -> `Bool`, plus scalar scaling `Duration * (Int|Float)` /
    `(Int|Float) * Duration` / `Duration / (Int|Float) -> Duration` — all
    riding the C++ `__vstr::Duration` operator overloads, so codegen still
    emits a straight `a <op> b`. The scalar overloads are constrained
    templates (`std::integral` vs `std::floating_point`) so an integer
    literal stays unambiguous against the floating overload (`int->int64`
    and `int->double` are equal-rank conversions); none collide with the
    `Duration / Duration` ratio (no int/float<->Duration conversion
    exists). The `timeout` arm now takes a `Duration` (e.g. `timeout
    .milliseconds(250):`), extracting whole milliseconds via
    `.in_milliseconds()`. Property accessors `.nanoseconds` /
    `.microseconds` / `.milliseconds` / `.seconds` read the total whole
    count in that unit as `Int` (truncating; accessed, not called —
    distinct from the same-named factory), lowering to
    `static_cast<std::intptr_t>(d.in_<unit>())` like Span's `.count`.
  * **slice 7** — **async + throws**: an `async func … throws(E) -> T`
    lowers to `__vstr::Task<std::expected<T, E>>` (the return-type nesting
    was already in place). The body is a coroutine, so every function-level
    error exit co_returns: `throw e` → `co_return std::unexpected{e}`, and a
    propagating `try` (statement-position and the try-hoist prelude) →
    `co_return std::unexpected{…}` / `co_return *r`. Escapes inside a
    cond-hoist's IIFE lambda stay plain `return` (the lambda isn't a
    coroutine) — handled by suppressing the async flag while emitting that
    lambda body. Sema already typed `await f()` of an async throws fn as
    `Result<T, E>` (await is transparent on non-Future), which a `try`
    unwraps; no sema change was needed.
  * **slice 8** — **`Future[void]` for void spawn**: `spawn` of a
    Unit-returning async fn now yields a `Future[Unit]`, lowered to a
    `__vstr::Future<void>` specialization (no `std::optional<void>`; a
    `forced_` flag stands in for "completed", forcing pumps to completion,
    awaits/`.get()` yield void). `Task<void>` now advertises
    `__vstr_task_value = void` so `spawn_future` recognizes it (previously
    it mis-wrapped as `Future<Task<void>>`) and carries a `select_waiter`
    fired at `final_suspend` for parity. (A void future as a *select* arm
    is still unsupported — the dispatch would bind `auto&& v = f.get()` to a
    void — but that combination is nonsensical and out of scope.)
  * **slice 9** — **async-context gate for a blocking select**: a
    no-default select with a channel (or timeout) arm co_awaits a
    SelectAwaiter internally, so the capability checker now requires the
    `Async` capability for it — the same gate `await` / `spawn` carry. This
    turns a confusing downstream C++ "co_await in a non-coroutine" error
    into a source-level `missing capability 'Async'` diagnostic. A select
    *with* a default (or a pure-future select) polls and is still allowed
    in a sync function.
  * **slice 10** — **`using Async` gate on `parallel`**: `parallel(data,
    chunks, body)` dispatches its worker onto the async runtime (v0.5 runs
    the chunks sequentially, but the surface commits to the concurrent
    contract), so the capability checker now requires the `Async`
    capability at the call — the same gate `spawn` / a blocking `select`
    carry. Callers declare `using Async` (or open `with Async = …`); the
    bare-ident builtin shape only gates when nothing shadows `parallel`.
  * **slice 11** — **no borrow across await** (the sema half; codegen's
    by-copy read params were the runtime half). Vestra borrows are
    transient (call-argument boundaries), so the only borrow that can span
    a suspension is an `inout` parameter of the async fn itself — and
    unlike a read param (copied into the frame) an `inout` must stay a
    write-back reference into the caller, which can't survive the
    coroutine's suspension. The exclusivity checker now rejects an `inout`
    parameter on an `async func` at its signature.

Scheduler / §11 carry-forwards: spawn capture-by-value / move
semantics + non-escapable futures. (senders/receivers can replace the
coroutine shims if/when libc++ ships P2300.)

### 3. `Channel[T]` + `parallel` library (§11) — shipped

Both shipped: `parallel` (under §11 above) and `Channel[T]` — a typed
queue, lowered to a `__vstr::Channel<T>` over a shared deque, with `send`
(a sink), the non-blocking `recv() -> T?` poll, the suspending `await
ch.receive() -> T?` (scheduler slice 2), and `close()` (a closed flag
distinct from empty: nil from `receive` means closed-and-drained). v0.5
is single-threaded and unbounded. A blocking `select` over channel
receives ships under §11 above (slice 3), with a wall-clock `timeout`
arm (slice 4). Remaining (tracked under §11 above): bounded capacity /
back-pressure, and `send` as a true call-site move.

### 4. Quote / splice / declaration macros (§12.4) (multi-session)

First slice shipped: the `$`-splice syntax (`$ident` / `$(expr)`), a
`SpliceExpr` AST, and an expression-context `quote { EXPR }` that
materializes its body with splices substituted in place (an identity
template — the value is the reusable syntax + AST + type rule). The bulk
remains: a deferred typed-AST value model (`Decl` / `Expr` / `[Decl]`
comptime values), declaration / statement quotes, `comptime func`
declaration macros invoked via `@macro` with AST substitution and
re-checking, hygiene, the builder API, and `vestra expand`. The
AST-as-comptime-value layer is the large prerequisite and the natural
next unit; it composes with the comptime folder that ships through
phase 7.

### 5. Ownership / exclusivity phase 2 (multi-session)

Phase 1 (`b0727b9` / `e77f2e0`) is single-pass and linear. Phase 2 is
complete. Shipped: the `split(at:)` partition primitive on
Span/MutSpan; **branch-aware flow merging** (each `if`/`match` branch
forks and merges at the join); the iterator partitioner **`chunks(of: n)`**
(a lazy `ChunkIter` over consecutive disjoint sub-views); and the full
**`linear` types** story — `linear struct` must-consume (a leak error for
any linear binding still live at a scope exit, including one-sided branch
consumption), **let-from-place move tracking** (`let c = b` moves `b`),
**construction consumes its place arguments** (a constructor owns its
inputs), **linearity transitivity** (a struct that owns a linear field is
itself linear, closed to a fixpoint), and **sink parameters as terminal
sinks** (taking ownership counts as consuming, so an empty consumer ends a
linear chain).

Also shipped: **chunks as a first-class value** — a `chunks(of: n)`
result can be bound and accessed randomly (`.count`, `cs[i]`), not only
consumed by `for`.

Also shipped: **partition provenance** in the exclusivity checker — a
`split(at:)` half or `chunks(of:)` iterator records its parent place on
the bound Symbol, so an `inout` borrow of a sub-view that aliases the
parent is rejected while the two halves of a split still borrow
independently.

Ownership/exclusivity phase 2 is **complete**. General cross-statement
borrow liveness was moot (v0.5 has no persistent borrow form — `&x` is
admitted only directly in argument position, so no borrow's lifetime
spans statements). Deferred analysis follow-ons, none blocking: linear-in-
loops soundness (the loop body is walked once), the larger
destructure/deinit terminal form for linear values (sink params cover the
common case today), and generic linear structs / linear obligations
carried through wrapper types (Optional/Span/Box).

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

### Pattern-matching + try carry-forwards (§8 / §10) — complete

All four carry-forwards are shipped:

- ~~Nested tuple destructuring codegen~~ — `let ((a, b), c) = …` lowers via
  a placeholder + sibling `auto [a, b] = __vstr_tpN` structured-binding
  hoist (verified end to end).
- ~~String-literal patterns~~ (`f1a6637`) — the §4 string lattice
  (StrConst → Str → String) lets a literal match a Str/String scrutinee;
  the value-scrutinee if-chain lowering already handled the compare.
- ~~Or-patterns over payloaded-enum cases~~ (`df9ef1d`) — each alternative
  of `case .a(x) | .b(x):` expands into its own constexpr-if branch in the
  std::visit lowering, sharing the arm body and binding its own payload
  field to the common name.
- ~~Mid-expression `try` inside an if/match arm~~ (`9a08a57`) — the if-expr
  and enum-match cond-hoist forms already worked; the last gap was a `try`
  inside a *value-scrutinee* match arm, now lowered as an if-else-if chain
  inside the cond-hoist lambda so propagation escapes correctly.

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
