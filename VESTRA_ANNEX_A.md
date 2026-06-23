# Vestra Annex A — Bare-metal completeness (consolidated)

*This annex supersedes the standalone Draft 3.3 and Draft 3.4 delta documents. It folds them into one chapter-ordered specification of every addition Vestra needs to write a kernel without a foreign source file. Where the prior drafts left a gap — the raw-pointer mint primitive, the weak-CAS shape vocabulary, the rounding-aware cache helper, the audit surface for atomic/barrier interactions, the vector-table lowering rule — this annex closes it. Where the kernel still needs a feature neither draft named — the interrupt-disabled scope, the context-switch primitive, the panic-handler hook, the `no_libc` build-time guarantee — this annex adds it. The result is a chapter-ordered specification where every line of the kernel's current `freestanding.c` and almost every line of `freestanding.S` has a named Vestra spelling, and where the `vestra audit` surface enumerates the discipline the type system encodes.*

---

## §4.5 — Uninitialized static storage

A kernel reserves fixed regions of `.bss` that the source does not initialize: page tables, capability tables, ring buffers, per-CPU slabs. Vestra admits exactly one form, the **uninitialized static**:

```vestra
@noinit @section(".bss.cap_table")
static cap_table_buf: [4 * 1024 * 1024]UInt8
```

Three rules: no initializer (the symbol is emitted with no initial bytes); the element type must be `Trivial`; the storage is **not in scope as a value** — reach it by `&buf` (§14.6.3) and gate access on `RawMemory`. The recommended pattern is to wrap the buffer once at startup in a `MutSpan` (§10.5) under a long-lived `with RawMemory { … }`.

`vestra audit --bss` enumerates every `@noinit static`, its size, its section, and the discharge that admits its access.

---

## §6.7 — Section placement, linkage, symbol attributes

Five attributes complete §6's layout vocabulary at the symbol level. Every one is link-time data; none changes the program's type or its guarantees.

| Attribute | Meaning |
|---|---|
| `@section(name)` | Place the declaration in linker section `name`. Applies to `func`, `static`, `@noinit static`. |
| `@symbol(name)` | Export under the literal linker name `name`, overriding mangling. |
| `@weak` | Weak symbol — a strong definition overrides; an unresolved reference is null, not an error. |
| `@alias(other)` | A second name for `other`. Emits an alias; no body required. |
| `@visibility(.default \| .hidden \| .protected)` | ELF visibility class. |

---

## §6.8 — Layout completeness

**Untagged unions.** `@repr(union)` on a struct collapses fields into one storage cell sized and aligned by their maximum. The struct must be transitively `Trivial` and `Copyable`. There is no tag; mutation of one field invalidates the others; the compiler treats the cell as opaque between writes. This is the *untagged overlay*, distinct from `enum`, and pairs with `MmioView[T]` to express bit-field register decoding (§14.11.4).

**Explicit padding.** A field declared `_: [N]UInt8 @padding` reserves exactly `N` bytes that the compiler must not coalesce with neighbouring padding.

**Anonymous fields.** An `embed` with no name (`embed _: Header`) flattens fields into the outer namespace without binding the embed itself.

**Flexible array members.** A struct's *last* field may be `var data: [_]T`. The compiler emits the header with no storage for `data`; access is through `withFlexibleArray(count:)`, which yields a caller-asserted-length `MutSpan[T]` gated on `RawMemory`.

**Compile-time layout reflection.** `Type.size`, `Type.alignment`, and on `Field`: `offset`, `size`, `alignment`. The `sizeof`/`alignof`/`offsetof` patterns are ordinary `comptime` expressions; `comptime { assert(Type.of(CapabilityEntry).size == 64) }` is a static check.

---

## §7.8 — The `@inline` directive

One attribute, three levels:

```vestra
@inline(.always) func slot(_ i: Int) -> MutSpan[Entry] { … }
@inline(.never)  func panicReport(_ msg: Str) -> Never { … }
@inline(.hint)   func popcount(_ x: UInt64) -> Int { … }
```

`.always` is a **correctness** directive — failure to inline is a compile error naming the obstructing site. `.never` forbids inlining. `.hint` is advisory. Absence is the compiler's choice. The attribute is for cases where inlining is part of the *meaning* of a function (a `MutSpan` projection that must not introduce a function-call boundary the borrow checker cannot see through), not a performance dial.

---

## §10.5 — Minting views from raw memory

A kernel mints a typed pointer or view from a `UInt64` it computed (physical address + offset; high-VA alias; linker symbol address as integer). Two primitives, both gated on `RawMemory`:

```vestra
extension Ptr {
    static func unchecked(fromAddress: UInt64) using RawMemory -> Ptr[T]
}
extension MutPtr {
    static func unchecked(fromAddress: UInt64) using RawMemory -> MutPtr[T]
}

extension Span {
    static func raw(at: Ptr[T], count: Int) using RawMemory -> Span[T]
}
extension MutSpan {
    static func raw(at: MutPtr[T], count: Int) using RawMemory -> MutSpan[T]
}
```

Three rules:

1. **The pointer is `Trivial` and escapable.** A `Ptr[T]` is just bits; what is non-escapable is anything *derived* from it (`Span`, `MutSpan`, `MmioView`, `MmioRegion`).
2. **The lifetime of a derived view is anchored to the `RawMemory` discharge** — not to a source binding, because there is none. A `MutSpan` minted under `with RawMemory { … }` is non-escapable past the block's end. Storing it in a heap value, returning it past the discharge, or capturing it in a `spawn`ed task is a compile error naming the discharge.
3. **`T`-alignment is the caller's assertion.** Misalignment for non-`UInt8` `T` traps on aarch64 (Device-nGnRnE rules pre-MMU-stable) and silently corrupts on x86. `vestra audit --raw-pointer` enumerates every `unchecked(fromAddress:)` and the discharge that admitted it.

This primitive is the root the kernel's prior `UnsafeMutablePointer<T>(bitPattern: UInt(addr))!` pattern was reaching for. Every other view-minting primitive in this annex (§14.11's `MmioView.at`, §14.11.2's `MmioRegion.at`, §14.12's sysreg handles) calls this internally; the operation is named once, gated once, audited once.

---

## §14.4 — Inline assembly: tightened clobber discipline

Draft 3.2's `asm` block validates operand constraints against the target ISA. Two clauses tighten it:

1. **Implicit clobbers are an error.** An instruction that writes a register or sysreg the operand list does not name is rejected, with a diagnostic naming the implicit write.
2. **The `@extern("conv")` call site assumes the conv's callee-saved set is preserved.** The compiler trusts the foreign code to obey the declared convention. `vestra audit --extern` enumerates every call, its conv, and (where the foreign code is in-tree) a static check that the conv is honored.

The prior `r12`-clobber-from-C-helper hazard the kernel hit becomes a build-time finding when the foreign code is in-tree, and an audit entry otherwise.

---

## §14.5 — Interrupt service routines

### 14.5.1 The trap-frame type

A trap frame is an ordinary `@repr(C) @repr(packed)` struct. `core.systems.trap` supplies the canonical platform shapes (`AArch64TrapFrame`, `X86TrapFrame`, `Riscv64TrapFrame`); the kernel may declare its own.

### 14.5.2 `@interrupt(...)` handlers

```vestra
@interrupt(class: .sync, fromEL1: true)
func aarch64SyncEL1(frame: inout AArch64TrapFrame) using Asm {
    match decodeEC(frame.esr) {
        case .svcA64:  handleSyscall(&frame)
        case .dabt:    handleDataAbort(&frame)
        default:       trapReport(&frame)
    }
}
```

Five rules: first parameter is `inout TrapFrame` (mutating it changes the registers `eret`/`iretq` restore); the body discharges its own capabilities (the signature row is informational); no `Alloc`, no `Async`, no `Extern`; return type is `Unit`; the compiler emits the prologue and epilogue.

### 14.5.3 Vector tables — lowering rule

```vestra
@section(".text.vec_table") @align(2048)
static aarch64VecTable: [16]@interrupt(AArch64TrapFrame) = [
    syncEL1, irqEL1, fiqEL1, serrorEL1,
    syncEL0Lower64, irqEL0Lower64, fiqEL0Lower64, serrorEL0Lower64,
    syncEL0Lower32, irqEL0Lower32, fiqEL0Lower32, serrorEL0Lower32,
    syncSPSel0, irqSPSel0, fiqSPSel0, serrorSPSel0,
]
```

The lowering rule, fixed at 3.5 (closing the 3.3 open question):

1. Each slot emits a `.text.vec_table.<index>` sub-section, aligned to the platform's slot stride — **128 bytes (32 × 4-byte instructions) on aarch64** (per the architecture's exception vector format), **16 bytes on x86 IDT** (descriptor format), **4 bytes on RISC-V `stvec` direct mode** (one `JAL`).
2. The slot's body is the `@interrupt` function's compiler-emitted prologue + body + epilogue, **inlined at the slot address** when the body fits the slot stride. A body that overflows the slot stride emits a per-slot trampoline (a single jump to a separately-emitted function); the audit (`vestra audit --vec-table`) flags every trampoline.
3. Cross-slot references use the array index, not the symbol. `&aarch64VecTable[i]` yields the slot's first instruction's address — what `VBAR_EL1` requires.
4. The table itself is a `static` of array type. `&aarch64VecTable` (no index) yields the table's base, which is the table's first slot — the value the kernel writes to `VBAR_EL1` at boot.

The x86 IDT and the RISC-V `stvec`-vectored mode follow the same shape with different slot stride and (on x86) descriptor-format fields the target description fills in.

---

## §14.6 — FFI, calling conventions, linker symbols

### 14.6.1 Pluggable calling conventions

`@extern("conv")` accepts any conv the target backend names: `"C"`, `"hvc64"`, `"smc64"`, `"sysv64"`, `"win64"`, `"ms_abi"`, `"aapcs"`, `"riscv"`, `"interrupt"`. Each conv pins which registers carry arguments, which the callee may clobber, and the return path. The set is target-determined.

### 14.6.2 `@symbol` on an extern

```vestra
@extern("C") @symbol("__stack_chk_guard")
static stackChkGuard: UInt64
```

Replaces `@_silgen_name` wholesale.

### 14.6.3 Address of a linker symbol

`&decl` on a `static`, `func`, or vector-table entry yields a `Ptr[T]?` (for static T), a function-type pointer (for func), or a slot address (for table entry). It is `Trivial`. **Address-taking does not require `RawMemory`**; dereferencing does.

---

## §14.7 — Bare-metal entry (`@boot`)

A function attributed `@boot` is **Vestra in surface, assembly in runtime model** — for pre-MMU code where Tier V's hidden-pointer threading cannot run.

Seven restrictions: no `using` row except `Asm`/`RawMemory`/`Mmio`; no call into ordinary Vestra; `@naked` implied; no throws/Result; no spilled register arguments; no reference to non-`@boot` code except by `&decl`; comptime results must be `Trivial`.

The escape from boot mode is a single `br`/`jmp` (in `asm`) to a non-`@boot` function the `&` operator named. From that jump, the kernel runs under the ordinary runtime model — or, when stage-1 init is still in progress, the **`@kernel_init`** regime that §14.7.1 names.

### 14.7.1 The `@kernel_init` regime

A function attributed `@kernel_init` is **ordinary Vestra in surface, single-threaded and pre-scheduler in runtime model** — the transitional regime between `@boot` (no MMU, no stack, no runtime) and the scheduler's first `swapContext` (no `Async`, but ordinary memory model, full type system, allocator may or may not be live depending on call site).

Five restrictions:

1. **No `using Async`, no `spawn`, no `await`.** The scheduler hasn't dispatched a second task yet; there is no other thread to synchronise with.
2. **No call into a non-`@kernel_init` function whose `using` row names `Async`.** The audit (`vestra audit --kernel-init`) enumerates the call graph reachable from each `@kernel_init` entry.
3. **`using Alloc` is admitted only after the allocator's `@kernel_init` initialiser has returned.** The compiler doesn't statically check the order; the audit names every `@kernel_init` that discharges `Alloc` and the source-order site of the `Alloc` initialiser.
4. **`@no_auto_barrier` (§14.12.3) is admitted here.** The kernel-init path is where the kernel writes the MMU control sysregs in a batch that subsumes the auto-`ISB`; outside this regime, the escape is rejected.
5. **`@kernel_init` functions may call into ordinary code, but ordinary code may not call into `@kernel_init`.** The latter is a one-way regime: once stage-1 init returns and the scheduler dispatches, the regime ends.

A typical aarch64 stage-1 entry chain is `@boot func _start → @kernel_init func kernelInit → ordinary func kernelMain`. The `kernelInit` function brings up paging, configures vector base, initialises the allocator, sets up the first task's `Context`, and tail-calls `swapContext` into it. The single `br` from `_start` to `kernelInit` is the audit-flagged boundary between boot and runtime.

---

## §14.8 — Per-CPU storage

```vestra
struct PerCpu[T: Trivial]                       // compiler-known
    static func new(_ value: T) using Alloc -> Box[PerCpu[T]]
    inout func mine() -> inout T                // current hart's slot
    func        mine() -> T

    func slot(_ hartId: HartId) using RawMemory -> Ptr[T]
}
```

Layout is `[MAX_HARTS]Padded[T]` where `Padded[T]` pads to `cfg.option("cache_line_bytes")`. The current-hart index is read from a target-specific register the compiler emits (`tpidr_el1`, `gs:[0]`, `tp` on RISC-V). `PerCpu[Atomic[UInt64]]` is the kernel's per-hart counter with no false sharing.

---

## §14.9 — The atomic memory model

### 14.9.1 Orderings

```vestra
enum Ordering { case relaxed, acquire, release, acqRel, seqCst }
```

Five orderings. `consume` collapses into `acquire`. An ordering an operation cannot carry (pure load with `.release`; pure store with `.acquire`; CAS failure stronger than success) is a compile error.

### 14.9.2 `Atomic[T]`: core operations

`Atomic[T]` is compiler-known for primitive `T` the target's atomic ISA covers. Operations:

```vestra
extension Atomic where T: Primitive {
    func        load    (_ ordering: Ordering = .seqCst) -> T
    inout func  store   (_ value: T, _ ordering: Ordering = .seqCst)
    inout func  exchange(_ value: T, _ ordering: Ordering = .seqCst) -> T

    inout func  fetchAdd(_ delta: T, _ ordering: Ordering = .seqCst) -> T
    inout func  fetchSub(_ delta: T, _ ordering: Ordering = .seqCst) -> T
    inout func  fetchAnd(_ mask:  T, _ ordering: Ordering = .seqCst) -> T
    inout func  fetchOr (_ mask:  T, _ ordering: Ordering = .seqCst) -> T
    inout func  fetchXor(_ mask:  T, _ ordering: Ordering = .seqCst) -> T
}

extension Atomic where T == Bool {
    inout func  fetchSet  (_ ordering: Ordering = .seqCst) -> Bool
    inout func  fetchClear(_ ordering: Ordering = .seqCst) -> Bool
}
```

Three rules: `.seqCst` is the default (the right choice when the developer has not yet thought about the question); `inout` on an `Atomic` method does not require a `var` binding (a `static x: Atomic[UInt32]` is callable as `x.fetchAdd(1, .relaxed)` — §1's static rule was written with this exemption in mind); `T: Primitive` is structural (non-primitive payloads belong in `Mutex`/`RwLock`).

### 14.9.3 Compare-exchange — and the loop-shape rule

```vestra
extension Atomic where T: Primitive {
    inout func compareExchange(
        expected: T, desired: T,
        success: Ordering = .seqCst,
        failure: Ordering = .acquire,
    ) -> CASResult[T]

    inout func compareExchangeWeak(
        expected: T, desired: T,
        success: Ordering = .seqCst,
        failure: Ordering = .acquire,
    ) -> CASResult[T]
}

struct CASResult[T: Primitive] { let succeeded: Bool; let actual: T }
```

Three rules: `failure ≤ success` (compile error otherwise); `failure` may not be `.release`/`.acqRel`; **the weak form is admitted only inside a recognized retry loop.**

The admitted retry-loop shapes (the spec lists them, closing the 3.4 vagueness):

- `while !r.succeeded { … r = atomic.compareExchangeWeak(…) }`
- `while true { let r = atomic.compareExchangeWeak(…); if r.succeeded { break } }`
- `repeat { … r = atomic.compareExchangeWeak(…) } while !r.succeeded`
- A function attributed `@retry_loop` whose body matches the predicate below; the call to that function is then admitted as if it were the loop. The `withRetry { … }` standard-library combinator carries this attribute.

The `@retry_loop` predicate is the part 3.4 left open. A function `f` may carry `@retry_loop` iff **all three** of:

1. `f` is `@inline(.always)`. The attribute is a correctness directive (§7.8) — the body must reach the call site inline so the borrow checker and the weak-CAS-shape checker see through it.
2. `f`'s body contains **exactly one** `compareExchangeWeak` call site, syntactically. Multiple weak-CAS sites need their own loops.
3. That call site is the weak-CAS of one of the three admitted loop shapes above, written in `f`'s body. The shape predicate is the same one the inline checker uses on user code.

A `@retry_loop` function that violates any of (1)–(3) is rejected at definition time, naming the failing clause. The check is local to the function — no whole-program analysis needed.

```vestra
@retry_loop
@inline(.always)
func withRetry[T: Primitive](_ atomic: inout Atomic[T],
                             _ update: (T) -> T,
                             success: Ordering = .seqCst,
                             failure: Ordering = .acquire) -> T {
    while true {
        let observed = atomic.load(failure)
        let desired  = update(observed)
        let r = atomic.compareExchangeWeak(
            expected: observed, desired: desired,
            success: success, failure: failure)
        if r.succeeded { return desired }
    }
}
```

A free-standing `compareExchangeWeak` outside these shapes is a hard error suggesting `compareExchange`. The audit-visible escape is `@allow_weak_cas` on the call site.

### 14.9.4 Wide atomics

`Atomic[UInt128]` / `Atomic[Int128]` are admitted only on targets whose ISA carries a native 128-bit atomic instruction set. The spec pins three target-feature gates, one per architecture family:

- **aarch64 with `+lse`** — `CASP` / `LDP`-with-acquire / `STP`-with-release (ARMv8.1 Large System Extensions). The link-time check requires `+lse` in the target triple's feature list.
- **aarch64 without `+lse`** — *rejected*. The ARMv8.0 `LDXP`/`STXP` LL/SC sequence is a software construction, not an atomic instruction; a wide-atomic declaration on a `+lse`-less target is a link-time error citing the missing feature. The kernel may still write the LL/SC sequence by hand in inline `asm` under `Asm`, but the spec does not present it as `Atomic[UInt128]` — the loop's progress guarantee is different from `CASP`'s.
- **x86_64 with `+cx16`** — `CMPXCHG16B`. The link-time check requires `+cx16`. Pre-Nehalem and certain AMD CPUs lack it; the build is rejected.
- **RISC-V** — *not supported* in 3.5. No standard 128-bit atomic exists. A declaration is a link-time error pending a future Zacas-128 extension or equivalent.

On every supported target the declaration is a **link-time error** when the gate fails — never a silent lock-based fallback. The `AtomicTaggedPointer[T]` library type wraps `Atomic[UInt128]` for the DCAS shape and inherits the same feature gating.

### 14.9.5 Audit surface

`vestra audit --atomics` enumerates:

- Every atomic call site, its ordering, and the type's width.
- Every **default-`.seqCst` site** — distinguishing "deliberate" from "default" so a project policy can require explicit `.seqCst` where it's load-bearing.
- Every **adjacent atomic+barrier pair** that may be double-paying (e.g., `x.fetchAdd(1, .acqRel)` immediately followed by `Cpu.memoryBarrier(.full, .full)`).
- Every `@allow_weak_cas` and `@retry_loop`-attributed call site.

### 14.9.6 Deliberate omissions

No `consume` ordering. No `volatile` on `Atomic` (volatile is MMIO, §14.11). No mixed-size atomic operations on one cell. No language-level fences in this section — see §14.10.

---

## §14.10 — Sync intrinsics

### 14.10.1 Compiler fences

```vestra
core.systems.sync.compilerFence(_ ordering: Ordering) using Asm
```

No machine code; an optimizer sequence point with the requested ordering. `.relaxed` is rejected.

### 14.10.2 CPU memory barriers

```vestra
extension Cpu {
    static func memoryBarrier  (_ scope: BarrierScope, _ kind: BarrierKind) using Asm
    static func syncBarrier    (_ scope: BarrierScope)                      using Asm
    static func instructionBarrier()                                        using Asm
}

enum BarrierScope { case full, inner, outer, nonShareable }
enum BarrierKind  { case loadLoad, storeStore, loadStore, full }
```

Target lowering is a fixed table; `instructionBarrier()` is a no-op on platforms that self-serialize. The barrier kind is its own enum, never an `Ordering` — confusing the two would conflate cell-level and pipeline-level vocabulary.

### 14.10.3 Data cache management

```vestra
extension Cache {
    static func cleanData          (range: Span[UInt8])    using Asm, RawMemory
    static func invalidateData     (range: Span[UInt8])    using Asm, RawMemory
    static func cleanInvalidateData(range: Span[UInt8])    using Asm, RawMemory
    static func zeroData           (range: MutSpan[UInt8]) using Asm, RawMemory

    // Rounding variants — the audited slow path for caller convenience.
    static func cleanDataRounding          (range: Span[UInt8])    using Asm, RawMemory
    static func invalidateDataRounding     (range: Span[UInt8])    using Asm, RawMemory
    static func cleanInvalidateDataRounding(range: Span[UInt8])    using Asm, RawMemory
}
```

The strict forms require both ends line-aligned (`cfg.option("cache_line_bytes")`); a misaligned range is a hard error naming the misalignment. The **rounding variants** round outward and emit an audit finding (`vestra audit --cache-rounding`) so callers that didn't *need* to round can be tightened.

`zeroData` additionally requires the platform's zeroing-block size (`DCZID_EL0` on aarch64). A non-block-aligned `zeroData` is rejected; fallback is `cleanInvalidateData` + memset.

### 14.10.4 Instruction cache management

```vestra
extension Cache {
    static func publishInstructions(range: Span[UInt8]) using Asm, RawMemory
    static func invalidateAllInstructions() using Asm
}
```

`publishInstructions` is the operation the spec names; the per-architecture sequence (`dc cvau ; dsb ish ; ic ivau ; dsb ish ; isb` on aarch64) is target-table data.

### 14.10.5 TLB management

```vestra
extension Tlb {
    static func invalidateAll (scope: TlbScope) using Asm
    static func invalidatePage(va: UInt64, large: Bool = false, scope: TlbScope) using Asm
    static func invalidateAsid(_ asid: UInt16, scope: TlbScope) using Asm
}

enum TlbScope { case currentEL, allELs, innerShareable, outerShareable }
```

`invalidateAsid` on a target without ASID/PCID is a link-time error — never a silent degradation to a whole-TLB flush.

### 14.10.6 Pipeline / power hints

```vestra
extension Cpu {
    static func waitForInterrupt() using Asm    // wfi / hlt
    static func waitForEvent()     using Asm    // wfe / mwait (priv)
    static func signalEvent()      using Asm    // sev
    static func relax()            using Asm    // yield / pause
    static func nop()
    static func prefetch(_ at: Ptr[UInt8],
                         intent: PrefetchIntent   = .read,
                         locality: PrefetchLocality = .l1) using Asm
}
```

### 14.10.7 Audit surface

`vestra audit --barriers` enumerates:

- Every barrier emission with its source (manual call / auto-sysreg post-write / cache management).
- Every MMIO write followed by an MMIO read of the same region with no intervening barrier — the bug class §14.11.5 names.
- Every `cleanDataRounding`/`invalidateDataRounding` call.

---

## §14.11 — Memory-mapped I/O

### 14.11.1 `MmioView[T]`

```vestra
struct MmioView[T: Primitive] {
    static func at(_ ptr: MutPtr[T]) using Mmio -> MmioView[T]
    func        read()  -> T
    inout func  write(_ value: T)
}
```

`T: Primitive` (silicon-bus widths only); width is structural; lifetime anchored to the `Mmio` discharge (same family as §10.5's `MutSpan.raw`); the view *is* the volatile discipline — `@volatile var x: MmioView[T]` is rejected.

**Alignment is structural too.** `MmioView[T].at(ptr)` requires `ptr` to be `alignof(T)`-aligned at the architecture's natural boundary (4-byte for `UInt32`, 8-byte for `UInt64`, etc.). The constructor itself is total — sema doesn't track alignment of a `MutPtr[T]` minted from a `UInt64` address. The discipline is:

- **Debug builds** emit a runtime check at every `at()` call (`debug_assert((address & (alignof(T) - 1)) == 0)`). The check is elided in release.
- **Release builds** rely on the architecture's misalignment trap (aarch64 Device-nGnRnE traps; aarch64 Normal memory may trap or split per `SCTLR_EL1.A`; x86 silently splits, which is wrong for MMIO but matches the bus's actual behaviour). A misaligned MMIO access is a driver bug, not a recoverable state.
- `vestra audit --mmio` includes the alignment-check site count, so a release build can confirm the per-call-site check was emitted in debug at least once.

The `MutPtr[T]` is obtained via `MutPtr.unchecked(fromAddress:)` (§10.5), which discharges `RawMemory`. A driver therefore opens its init region as `with RawMemory { with Mmio { … } }` — `RawMemory` to mint the pointer, `Mmio` to admit the view — and the audit (`vestra audit --mmio`) lists both discharges.

### 14.11.2 `MmioRegion[T]`

```vestra
struct MmioRegion[T: Primitive] {
    static func at(_ ptr: MutPtr[T], count: Int) using Mmio -> MmioRegion[T]
    var  count: Int
    func index(_ i: Int) -> MmioView[T]
}
```

Indexed access; out-of-range indexing is the §10.4 panic, not silent.

### 14.11.3 `MmioWireView[T]`

For devices whose register endianness differs from the CPU's:

```vestra
struct MmioWireView[T: Primitive] {
    static func at(_ ptr: MutPtr[T], endianness: Endianness) using Mmio -> MmioWireView[T]
    func        read()  -> T
    inout func  write(_ value: T)
}

enum Endianness { case little, big, native }
```

The swap emits only when the device disagrees with the CPU. Integrates with `@repr(wire)` for decoded structures.

### 14.11.4 Composition with `@repr(union)`

The §6.8 untagged union pairs with `MmioView[UInt32]` for bit-field register decoding:

```vestra
@repr(packed)
struct UartLineStatusBits {
    @bits(1) var rxReady:     UInt32
    @bits(1) var rxOverrun:   UInt32
    @bits(1) var txEmpty:     UInt32
    @bits(29) var reserved:   UInt32
}

@repr(union)
struct UartLineStatus {
    var raw:    UInt32
    var fields: UartLineStatusBits
}

func pollRx(_ statusReg: MmioView[UInt32]) -> Bool {
    let snapshot = UartLineStatus(raw: statusReg.read())
    return snapshot.fields.rxReady != 0
}
```

One 32-bit MMIO load; bit-field decoding on the snapshot; no torn-read window. The kernel chooses when read-modify-write happens; the compiler does not insert it.

### 14.11.5 Ordering against ordinary memory

`MmioView` operations interact with §14.10's barriers per the architecture's rules. The spec does **not** insert implicit barriers per access. `vestra audit --mmio` enumerates write-followed-by-read of the same region without an intervening `Cpu.syncBarrier`.

### 14.11.6 Deliberate omissions

No mmap-style primitive (the MMU code does the mapping; the spec admits the *view* over it). No "side-effect free read" attribute (every read is side-effecting; shadow registers cover the read-without-consequence case). No `MmioView` over nullable pointers.

---

## §14.12 — System register access

### 14.12.1 Typed handles

```vestra
struct ReadOnlySysreg [T: Primitive] { func        read()             -> T using Asm }
struct WriteOnlySysreg[T: Primitive] { static func write(_ value: T)  using Asm }
struct ReadWriteSysreg[T: Primitive] { func        read()             -> T using Asm
                                       inout func  write(_ value: T)  using Asm }
```

One `mrs`/`msr`/`rdmsr`/`wrmsr`/`csrr`/`csrw` per call. No fallback path.

### 14.12.2 The `Sysreg` namespace

`core.systems.sysreg` declares one named instance per architectural sysreg the target description knows: aarch64 EL1 set (`midr_el1`, `mpidr_el1`, `currentEL`, `daif`, `tpidr_el1`, `sctlr_el1`, `vbar_el1`, `ttbr0_el1`, `ttbr1_el1`, `tcr_el1`, `mair_el1`, `esr_el1`, `far_el1`, `elr_el1`, `spsr_el1`, `cntfrq_el0`, `cntpct_el0`, `dczid_el0`); x86_64 MSRs (`ia32_efer`, `ia32_star`, `ia32_lstar`, `ia32_fmask`, `ia32_kernel_gs_base`, `ia32_gs_base`, `ia32_fs_base`, `ia32_apic_base`, `ia32_pat`); RISC-V S-mode CSRs (`sstatus`, `sie`, `stvec`, `sscratch`, `sepc`, `scause`, `stval`, `satp`).

Three rules: the set is target-determined; each handle is a compiler-known singleton (not user-constructible); `&Sysreg.daif` is a compile error (a sysreg is not a memory cell with a linkage symbol).

### 14.12.3 Architecturally-required barriers

The target description names which sysregs require which barriers post-write. The compiler emits them automatically; the call site does not say it. The audit (`vestra audit --sysreg`) enumerates every auto-emitted barrier — required for cross-architecture review.

**Cross-architecture caveat.** The target-description data is the riskiest piece of §14.12. aarch64's "`ISB` after MMU-control writes" pattern does not translate directly to RISC-V's `SFENCE.VMA` discipline, and a misconfigured target table will produce *wrong* barriers, not *missing* ones. A 3.6 target-description hardening pass is scheduled: cross-architecture parity tests, fault-injection coverage on the auto-emission table, and an `audit --sysreg --strict` mode that requires every entry to cite the architecture-manual paragraph that motivates it.

The escape for the kernel-init case where a following sysreg write subsumes the barrier:

```vestra
@no_auto_barrier
Sysreg.ttbr0_el1.write(newPgd)        // following SCTLR write carries the ISB
Sysreg.sctlr_el1.write(mmuEnable)
```

`@no_auto_barrier` is rejected outside a function attributed `@kernel_init` (the boot-path subset where this pattern is admissible).

### 14.12.4 Privilege

A privileged sysreg access from too-low an EL traps to the hardware's handler; the spec does not statically reject it. The build-target check at link time rejects a binary whose target's runtime privilege is lower than any sysreg access it contains.

### 14.12.5 Deliberate omissions

No string-to-sysreg lookup. No per-EL alias bouncing (`Sysreg.daif_el2` is a separate handle). No PMU/debug sub-vocabulary (library on top of raw sysreg handles).

---

## §14.13 (new) — Interrupt-disabled scope

A spinlock body, a per-CPU state update, a critical section of the runqueue manipulation — every kernel has regions that must run with asynchronous exceptions masked. Today these are hand-managed `local_irq_save`/`local_irq_restore` pairs with discipline by convention. Vestra admits the region as a typed scope:

```vestra
with InterruptsOff {
    var slot = currentHart.runqueue.mine()       // PerCpu projection — §14.8
    let head = slot.head
    slot.head = head.next
    // … bounded work
}
```

Seven rules:

1. **Mask on entry, restore on exit.** The compiler emits a target-specific mask-set-and-save on entry (write `DAIF` on aarch64; `CLI` on x86; clear `SIE` on RISC-V) and the matching restore on every exit path — early `return`, `throw`, falling through. `throw` *does* restore: the unwinding path goes through the normal `with`-block exit, so the `@panic_handler` (§15.5) sees the caller's prior mask. `panic` is terminal and does **not** restore — the handler runs under whatever mask the panic occurred under, which is the right semantics for a fault diagnosis path.
2. **No `await`.** The awaitable resolves through an ISR that is now masked. Compile error.
3. **No transitive call requiring `Alloc` or `Async`.** A function whose `using` row names either is rejected inside the scope. The diagnostic names the inner call, its row, and the enclosing `InterruptsOff`.
4. **No `spawn`.** The spawned task cannot synchronise with the masked region.
5. **No `Cpu.waitForInterrupt` / `waitForEvent`.** Either is an immediate deadlock — the wake the instruction blocks on is the asynchronous exception this region masks. Compile error, not an audit finding. `Cpu.relax()` / `Cpu.nop()` / `Cpu.prefetch(…)` stay admitted; they don't depend on an exception delivery.
6. **Nesting is admitted.** The mask save/restore stacks; the inner exit restores to the outer state, not to globally-unmasked. The save site is a per-scope local (per-hart, since the mask is a per-hart register), so cross-hart nesting compositions don't conflict.
7. **InterruptsOff is a region, not a `using`-row capability.** It composes orthogonally with `with` bindings of other capabilities. There is no `using InterruptsOff` shape.

`vestra audit --interrupts-off` enumerates every `InterruptsOff` region and (where the body is bounded by a `comptime`-known loop count or contains no loops) emits an upper bound on its cycle count. A loop whose iteration count depends on a runtime value emits an `Unbounded` finding — the kind that turns into a latency bug when interrupts are masked for too long. A project policy may require zero `Unbounded` findings on hot paths.

The kernel's idle path is **not** an `InterruptsOff` region: the WFI/HLT needs interrupts enabled to wake. The kernel's spinlock body, however, *is* — and rule 5 makes the "wait-for-event inside InterruptsOff" deadlock a compile error rather than a runtime hang.

---

## §14.14 (new) — Context switching primitive

The scheduler is policy; §16 keeps it out of the language. The mechanism — saving one register set and restoring another — is what forces the drop to assembly that this annex is trying to close. Vestra admits the primitive without the policy:

```vestra
struct Context        // compiler-known; opaque; layout fixed by target description

extension Scheduler {
    static func swapContext(
        saving:  MutPtr[Context],
        loading: Ptr[Context],
    ) using Asm
}
```

Seven rules:

1. **`Context` is opaque to user code.** Reflection (§12.2) yields `Type.of(Context).size` and `.alignment` only — never the field shape. The kernel allocates a `Context` slot per task in a `@noinit static` array; the layout is the target description's concern, not the scheduler's.
2. **The compiler emits the save/restore.** Save the callee-saved register set + SP + PC into `*saving`; load the same set from `*loading`; return to the new PC. Caller-saved registers are not preserved (the caller already spilled them per the calling convention).
3. **`Context` includes the interrupt-mask state.** `DAIF` on aarch64, `EFLAGS.IF` on x86, the relevant bits of `sstatus` on RISC-V. A task suspended inside `InterruptsOff` resumes with the same mask it had at its prior `swapContext` — the typical pattern is a scheduler that swaps tasks with interrupts off and lets each task observe whatever mask it had when it was preempted, not the swapping task's. `swapContext` is therefore **admitted inside `InterruptsOff`** (rule 5 of §14.13 does not apply: `swapContext` is not a wait-for-event), and the resumed task's `InterruptsOff` save-stack is what the original entry pushed, not what the swapping task held.
4. **Floating-point/vector state is *not* in `Context`.** A separate `FpContext` type covers the lazy-FP touch-fault path; `swapContext` neither saves nor restores FP/vector. The scheduler decides when to flush FP based on `cfg.option("fp_save_policy") ∈ { .eager, .lazy }`. Three operations carry the FP discipline:

   ```vestra
   extension Scheduler {
       static func saveFp     (into: MutPtr[FpContext]) using Asm
       static func restoreFp  (from: Ptr[FpContext])    using Asm
       static func disableFp  ()                         using Asm   // CPACR_EL1 / CR0.TS
       static func enableFp   ()                         using Asm
   }
   ```

   `disableFp` arms the touch-fault; the first FP use thereafter raises a synchronous exception the kernel handles by `saveFp`-ing the prior owner and `restoreFp`-ing the current task. `eager` mode is `saveFp` + `restoreFp` on every `swapContext`; `lazy` mode is `disableFp` on `swapContext` and on-demand restore via the touch-fault handler.
5. **The function is gated on `Asm`.** It is *not* gated on `RawMemory`: the pointers are typed `Ptr[Context]`/`MutPtr[Context]`, and the compiler knows the access width and alignment from the target description.
6. **`swapContext` returns control to the *loaded* `Context`'s saved PC**, not to the caller's. The next statement after `swapContext(saving:, loading:)` runs when *another* call somewhere swaps *back* into this Context. This is the primitive's semantic — task A's `swapContext` returns to task B's prior `swapContext` call site. The audit (`vestra audit --context-switch`) enumerates every call as a control-flow edge.
7. **`@boot` functions may not call `swapContext`.** The boot regime has no scheduler. Compile error.

This is the smallest primitive a working scheduler can build on, and it is the *only* `Asm`-discharging function in the standard library that the kernel cannot replace with a more typed alternative — because what it does is not type-able above the register level.

---

## §15.4 — Compiler-emitted intrinsics

`memcpy`/`memset`/`memmove` (for struct copies, zero-init, slice operations), stack canaries (`__stack_chk_guard`/`__stack_chk_fail`), and overflow traps (`__vestra_overflow_trap`) are compiler-emitted, with Vestra-supplied bodies via `@symbol`:

```vestra
@symbol("memcpy")  @inline(.never) func memcpy(_ dst: MutPtr[UInt8], _ src: Ptr[UInt8], _ n: Int) using RawMemory
@symbol("memset")  @inline(.never) func memset(_ dst: MutPtr[UInt8], _ v: UInt8,        _ n: Int) using RawMemory
@symbol("memmove") @inline(.never) func memmove(_ dst: MutPtr[UInt8], _ src: Ptr[UInt8], _ n: Int) using RawMemory
```

A freestanding profile ships them in Vestra. `@stack_protector(.none | .strong | .all)` selects canary instrumentation.

---

## §15.5 (new) — Panic-handler hook

A freestanding binary's panic path is target-specific. Vestra admits one binding:

```vestra
@panic_handler
func vectraPanic(message: Str, file: StrConst, line: Int) -> Never {
    uartPuts("VECTRA PANIC: ")
    uartPuts(message.bytes())
    Cpu.waitForInterrupt()
    while true { Cpu.relax() }
}
```

Five rules:

1. **Exactly one `@panic_handler` per binary.** The linker enforces.
2. **Fixed signature.** `(message: Str, file: StrConst, line: Int) -> Never`. `StrConst` is the compiler-known type of a `comptime`-evaluated string literal — `__FILE__`-equivalent expansions land here without an allocation. It's `Trivial`, holds a `(Ptr[UInt8], Int)` pair pointing into the read-only `.rodata` segment, and converts implicitly to `Str` via a non-owning view.
3. **Compiler emits panic/overflow/Never-return calls against this binding** — replacing the implicit `__vestra_panic` and `__vestra_overflow_trap` references.
4. **No `using` row.** The handler runs in a context where the program's capability bindings cannot be assumed live.
5. **Falling off the end is rejected** — return type is `Never`.

### The `no_libc` build guarantee

A manifest flag pins the rest:

```toml
[profile.freestanding]
no_libc = true
```

Effects:

- The compiler **refuses to emit any reference to an undeclared external symbol**. Every external symbol must be declared in-tree under `@extern` + `@symbol`, or supplied by the standard library, or rejected at link time with a per-symbol diagnostic.
- `memcpy`/`memset`/`memmove` must resolve to in-tree bindings (§15.4).
- `__stack_chk_guard` and `__stack_chk_fail` must be in-tree.
- `vestra audit --no-libc` enumerates every external symbol the build resolved and where it was declared. A zero-finding pass is the build-time proof that the binary is libc-free.

Together with §15.4's intrinsics, `no_libc = true` makes "zero references to libc" a **build-time guarantee**, not a code-review concern.

---

## §16 — Deliberate omissions (amended)

Four amendments:

- **No general-purpose preemptable context switching primitive *at the policy layer*.** Task switching is a kernel concern; `swapContext` (§14.14) is the *mechanism* the policy rests on.
- **No generation-counted handles in the language.** `linear opaque type` catches dropped handles; stale-handle detection is library `Generational[T]`.
- **No untyped-pointer arithmetic on `Ptr[T]`.** `p + n` is `T`-strided; byte arithmetic is `Ptr[UInt8]`.
- **No software-implemented atomic fallback** (§14.9.4) and **no degraded TLB flush on a no-ASID target** (§14.10.5). The spec rejects the build instead of silently substituting.

---

## §17 — Grammar additions

New attribute words (none reserved as identifiers): `bss`, `noinit`, `section`, `symbol`, `weak`, `alias`, `visibility`, `boot`, `kernel_init`, `naked`, `interrupt`, `inline`, `padding`, `stack_protector`, `panic_handler`, `no_auto_barrier`, `allow_weak_cas`, `retry_loop`.

`InterruptsOff` is a **with-region head**, not a keyword: it is a compiler-known identifier in the `core.systems.scope` module that the `with`-block parser recognizes by name in the bind position, distinguished from an ordinary capability binding by the absence of `=`.

New productions:

```ebnf
declaration ::= … | '@noinit' static-decl
type        ::= … | '@interrupt' '(' type ')'
expr        ::= … | '&' (IDENT | dotted)             -- address-of-symbol
struct      ::= … '@repr(union)' struct-body
w-bind      ::= … | 'InterruptsOff'                  -- region, no '=' binding
```

`&decl` binds at the precedence of `copy` (prefix, tightest); disambiguates from the call-site `&` argument marker by position.

---

## §18 — Standard library additions

```vestra
// §10.5, §14.11
extension Ptr     { static func unchecked(fromAddress: UInt64) using RawMemory -> Ptr[T] }
extension MutPtr  { static func unchecked(fromAddress: UInt64) using RawMemory -> MutPtr[T] }
extension Span    { static func raw(at: Ptr[T], count: Int)    using RawMemory -> Span[T] }
extension MutSpan { static func raw(at: MutPtr[T], count: Int) using RawMemory -> MutSpan[T] }

// §14.8
struct PerCpu[T: Trivial]
struct Padded[T]

// §14.9
enum  Ordering
struct Atomic[T: Primitive]
struct CASResult[T: Primitive]
struct AtomicTaggedPointer[T]

// §14.10
extension Cpu       { /* memoryBarrier, syncBarrier, instructionBarrier, waitForInterrupt, … */ }
extension Cache     { /* cleanData, …, cleanDataRounding, …, publishInstructions, … */ }
extension Tlb       { /* invalidateAll, invalidatePage, invalidateAsid */ }
core.systems.sync.compilerFence

// §14.11
struct MmioView[T: Primitive]
struct MmioRegion[T: Primitive]
struct MmioWireView[T: Primitive]
enum   Endianness

// §14.12
struct ReadOnlySysreg[T: Primitive]
struct WriteOnlySysreg[T: Primitive]
struct ReadWriteSysreg[T: Primitive]
// core.systems.sysreg — target-knobbed singletons

// §14.14
struct Context                        // opaque; includes saved interrupt-mask state
struct FpContext                      // opaque
extension Scheduler {
    static func swapContext(saving: MutPtr[Context], loading: Ptr[Context]) using Asm
    static func saveFp     (into:   MutPtr[FpContext]) using Asm
    static func restoreFp  (from:   Ptr[FpContext])    using Asm
    static func disableFp  ()                          using Asm   // arm the touch-fault
    static func enableFp   ()                          using Asm
}

// §15.5
// @panic_handler binding; no struct.

// Library combinators
struct Generational[T]                // §16
@retry_loop func withRetry[T](_ body: () -> CASResult[T]) -> T
```

---

## Audit surface — consolidated

The annex adds the following `vestra audit` modes; each is a one-flag, machine-readable enumeration of the discipline the type system encodes.

| Audit | What it enumerates |
|---|---|
| `--bss` | Every `@noinit static`, size, section, access discharge. |
| `--extern` | Every `@extern("conv")` call site and its conv. |
| `--vec-table` | Every vector-table slot and whether it fits in-slot or trampolines. |
| `--raw-pointer` | Every `Ptr.unchecked(fromAddress:)` and the `RawMemory` discharge. |
| `--mmio` | Every `MmioView`/`MmioRegion`/`MmioWireView` mint, with the `Mmio`+`RawMemory` discharges; write-followed-by-read without intervening barrier. |
| `--sysreg` | Every sysreg access, the privilege it requires, every auto-emitted post-write barrier. |
| `--atomics` | Every atomic op, ordering, default-vs-deliberate flag, weak-CAS shape, adjacent atomic+barrier pairs. |
| `--barriers` | Every barrier emission and its source. |
| `--cache-rounding` | Every `cleanDataRounding`/`invalidateDataRounding` call. |
| `--interrupts-off` | Every `InterruptsOff` region, bounded-cycle estimate, `Unbounded` findings. |
| `--context-switch` | Every `swapContext` as a control-flow edge. |
| `--no-libc` | Every external symbol the build resolved and where it was declared (zero findings under `profile.freestanding.no_libc = true`). |

These twelve passes together are the operational form of §1's promise: *every property that matters is in a signature, and `vestra audit` extracts it mechanically*.

---

## Annex A changelog (relative to Draft 3.2)

Drafts 3.3, 3.4, and 3.5 land together as this annex.

- **§4.5** `@noinit static` — uninitialized `.bss` storage, gated on `Trivial`, accessed under `RawMemory`. *(3.3)*
- **§6.7** Section/symbol/linkage attributes — `@section`, `@symbol`, `@weak`, `@alias`, `@visibility`. *(3.3)*
- **§6.8** Layout completeness — `@repr(union)`, explicit `@padding`, anonymous `embed`, flexible array members, `Type.size`/`alignment`/`Field.offset|size|alignment`. *(3.3)*
- **§7.8** `@inline(.always | .never | .hint)` — principled inlining lever. *(3.3)*
- **§10.5** Raw-mint primitives — **`Ptr.unchecked(fromAddress:)`** + `Span.raw`/`MutSpan.raw`, with lifetime anchored to the `RawMemory` discharge. *(3.3, with 3.5 closure of the missing pointer primitive)*
- **§14.4** Inline-asm clobber discipline tightened. *(3.3)*
- **§14.5** Interrupt handlers rewritten: `@interrupt(...)` with trap-frame projection, vector-table arrays, **plus 3.5 vector-table lowering rule** pinning slot stride, sub-section emission, and trampoline fallback. *(3.3 + 3.5)*
- **§14.6** Pluggable calling conventions, `@symbol`-on-extern, `&decl` address-of-symbol. *(3.3)*
- **§14.7** `@boot` — seven-rule subset of Vestra for pre-MMU code. *(3.3)* **§14.7.1** *(new)* — `@kernel_init` regime: ordinary Vestra in surface, single-threaded and pre-scheduler in runtime model; the only place `@no_auto_barrier` is admitted; `using Async`/`spawn`/`await` rejected; one-way (ordinary code may not call into `@kernel_init`).
- **§14.8** `PerCpu[T]` and `Padded[T]`. *(3.3)*
- **§14.9** Atomic memory model — five orderings, four operation families, strong/weak CAS, **with 3.5 enumeration of admitted weak-CAS loop shapes and the `@retry_loop` attribute (predicate now pinned: `@inline(.always)` + exactly one `compareExchangeWeak` site + shape-checked body)**, wide atomics **gated explicitly on `aarch64 +lse` / `x86_64 +cx16` with link-time rejection elsewhere (ARMv8.0 LL/SC and RISC-V left unsupported on purpose)**, **and the consolidated audit surface**. *(3.4 + 3.5)*
- **§14.10** Sync intrinsics — compiler fences, CPU barriers, data-cache (with 3.5 rounding variants), I-cache, TLB, pipeline hints. *(3.4 + 3.5)*
- **§14.11** Typed MMIO — `MmioView`, `MmioRegion`, `MmioWireView`; `@repr(union)` composition for bit-field decoding; **3.5 cross-reference to `MutPtr.unchecked(fromAddress:)`** for the pointer mint; **alignment discipline pinned (debug runtime check + release-time architectural trap)**. *(3.4 + 3.5)*
- **§14.12** Typed sysreg access — target-knobbed `Sysreg` namespace, auto-emitted post-write barriers, `@no_auto_barrier` audit escape, **3.5 cross-architecture caveat** flagging RISC-V `SFENCE.VMA` as the highest-risk target-table data. *(3.4 + 3.5)*
- **§14.13** *(new in 3.5)* — `with InterruptsOff` region: target-specific mask on entry, restore on all exit paths (including `throw`), body forbids `await` / `Async` / `Alloc` / `spawn` **and `Cpu.waitForInterrupt` / `waitForEvent`** (rule 7, statically checked — not an audit finding); audit estimates body cycle count.
- **§14.14** *(new in 3.5)* — `Scheduler.swapContext(saving:, loading:)`: typed opaque `Context`, target-described layout, compiler-emitted save/restore, **interrupt-mask state included in `Context` so a resumed task observes its own prior mask (admitted inside `InterruptsOff`)**; separate `FpContext` for lazy-FP **with named entry points (`saveFp` / `restoreFp` / `disableFp` / `enableFp`)**; gated on `Asm` only.
- **§15.4** Compiler-emitted intrinsics (`memcpy`/`memset`/`memmove`/canaries/overflow traps) — Vestra-supplied bodies, freestanding profile. *(3.3)*
- **§15.5** *(new in 3.5)* — `@panic_handler` binding and `profile.freestanding.no_libc = true` manifest flag; together they make "zero libc references" a build-time guarantee.
- **§16** Omission amendments — policy-vs-mechanism split for context switching; software atomic fallback and degraded TLB flush are rejected, not silently substituted.
- **§17** Grammar additions consolidated.
- **§18** Standard library additions consolidated.

---

## Acceptance criterion

A 3.5 kernel:

1. **`freestanding.c` is empty.** Every function the C bridge currently provides — symbol-address getters, fixed-section buffer reservations, intrinsics, stack-canary stubs, MMIO and sysreg one-instruction wrappers — has a Vestra spelling in this annex. `profile.freestanding.no_libc = true` enforces it at build time.
2. **`freestanding.S` is bounded to multi-instruction sequences the spec leaves to assembly on purpose.** These are: pre-MMU bring-up before `@boot` can name its own data (~30 instructions per arch); exception-entry trampolines for any platform whose vector format doesn't fit `@interrupt`'s emitted prologue *and* whose body doesn't fit the slot stride (§14.5.3 trampoline fallback, audit-flagged); and ISA-specific sequences the spec deliberately doesn't generalize (a `CLREX`-before-CAS-retry under contention; a `WFE`-with-explicit-event-mask).
3. **Every property in §1 is enumerable.** The twelve `vestra audit` modes are the operational form of "everything that matters is in the signature." A kernel passes 3.5 acceptance when all twelve produce zero unexpected findings under the project's policy.

---

## Remaining open questions for Draft 3.6

Six items, in priority order. The first two are genuine 3.6 blockers; the next four are explicit deferrals that do *not* gate kernel completion but accumulate as the kernel grows past stage-1.

1. **`UInt128`/`Int128` primitives.** §14.9.4 names them for wide atomics; the arithmetic surface (signed-overflow trap, unsigned wraparound, division on targets without native 128/64 divide, `Display` and `derive` interaction) needs its own pass.
2. **Cross-architecture target-description hardening for §14.12.3 auto-barriers.** The aarch64/x86/RISC-V parity matrix is the kernel's highest-risk implementation dependency; a 3.6 hardening pass adds fault-injection tests, `audit --sysreg --strict` with paragraph citations, and a third-party review gate before the spec's auto-emission claim becomes load-bearing in production.

3. **SIMD / vector intrinsics.** NEON (aarch64), SSE / AVX / AVX-512 (x86_64), RVV (RISC-V). Page zeroing, AES-NI, ChaCha20, memmove fast paths drop to assembly or compiler-vendor intrinsics today. The 3.6 entry pins a `core.systems.simd` namespace with target-feature-gated typed vector lanes (`Vec[N, T]`) and the load/store/arithmetic/shuffle vocabulary, parallel to §14.10's pipeline-intrinsic shape.
4. **Bit-twiddling intrinsics.** `popcount`, `clz` (leading zeros), `ctz` (trailing zeros), `bswap16/32/64`, `pdep`/`pext` (x86 BMI2). Useful as ordinary builtins with target-feature gating; today they drop to inline `asm` or compiler intrinsics. The 3.6 entry adds them to `core.systems.bit` as free functions on each integer primitive.
5. **DMA-descriptor ownership protocol.** A descriptor ring's ownership bit alternates between CPU and device; writing a CPU-owned descriptor without a barrier before flipping the ownership bit is one of the most common driver-correctness bugs. The 3.6 entry sketches a typed `DmaDescriptor[T]` with `claim()` / `release()` operations that emit the right cache-clean and barrier sequence per direction (CPU→device, device→CPU). Library on top of §14.10 and §14.11; spec needed because the discipline is uniform across drivers.
6. **Performance-counter sub-vocabulary.** Raw sysreg access (§14.12) reads `PMCCNTR_EL0` correctly but doesn't model the saturation, overflow-interrupt, and event-selector discipline the PMU registers have. The 3.6 entry adds a typed `Counter[Event]` wrapper that enforces the start/stop/reset state machine and exposes overflow as a typed Optional rather than a silent wraparound.

One **explicitly out of scope for the kernel track**:

7. **C++-style template-heavy advanced layouts** (variadic struct expansion, layout-dependent template specialization, `std::variant` discriminator-elision). Most patterns reachable through §12.4 declaration macros; the spec waits for userland to force a worked example before another pass on §6 and §12.
