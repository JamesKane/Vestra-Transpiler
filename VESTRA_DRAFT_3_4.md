# Vestra Draft 3.4 — Atomics and sync-intrinsic completeness

*Draft 3.4 fills four named gaps in Draft 3.3. §14.9 specifies the operations on `Atomic[T]` — orderings, RMW, compare-exchange, wide atomics — that §1 named but left to the library. §14.10 specifies the CPU-side companions: compiler fences, memory barriers, cache management, TLB invalidation, and the pipeline-power hint family. §14.11 admits typed memory-mapped I/O views with the width, volatile, and endianness discipline a driver needs. §14.12 admits the platform's system registers as a target-knobbed table of typed handles. Together they remove every line of `swift-atomics`-style ceremony, every inline `asm` block whose only job is to emit `dmb` / `dsb` / `dc cvac` / `tlbi` / `wfi` / `mrs` / `msr` / `rdmsr` / `wrmsr`, and every `volatile UInt32*` cast in a driver. The remaining `asm` blocks in a 3.4 kernel are the ones that genuinely encode logic — boot bring-up, exception-entry trampolines the platform doesn't fit `@interrupt`'s emitted prologue, ISA-specific multi-instruction sequences — not single-instruction wrappers.*

---

## §14.9 (new) — The atomic memory model

Draft 3.2's §1 named `Atomic[T]` as the one mutable-but-publishable storage form; Draft 3.3 left every operation on it unspecified. A kernel writes hundreds of atomic-update sites — refcounts, lock-free queues, generation counters, the ticket of every spinlock — and each is forced to a specific ordering by *what the program guarantees about reordering across it*. Embedded Swift today routes the vocabulary through a separate `swift-atomics` package; Vestra inlines it into the language and tightens what compiles.

### 14.9.1 Orderings

Five orderings, named the way the hardware reference manuals name them. Each is a value of the compiler-known enum `Ordering`:

```vestra
enum Ordering {
    case relaxed
    case acquire
    case release
    case acqRel
    case seqCst
}
```

The five subsume the six C11 spells: `consume` collapses into `acquire` (every compiler shipping today does the same thing). The choice is not advisory — the compiler emits the precise fence the ordering names on the target ISA, and **rejects an ordering the operation cannot carry**: a pure load with `.release` or `.acqRel`, a pure store with `.acquire` or `.acqRel`, a CAS whose `failure` ordering is stronger than its `success`. The diagnostic names the operation and the unreachable barrier.

### 14.9.2 `Atomic[T]`: core operations

`Atomic[T]` is a compiler-known type for every primitive `T` the target's atomic ISA covers: `UInt8`/`Int8` through `UInt64`/`Int64`, `UInt`/`Int`, `Bool`, and pointer-sized `Ptr[U]` / `MutPtr[U]`. The type is `Trivial`, may sit in a `static`, and is the *only* shape a Vestra program may publish for cross-thread mutation.

```vestra
static refCount: Atomic[UInt32] = Atomic.new(1)
```

The four operation families:

```vestra
extension Atomic where T: Primitive {
    func        load(_ ordering: Ordering = .seqCst) -> T
    inout func  store(_ value: T, _ ordering: Ordering = .seqCst)
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

Three rules govern the surface:

1. **The default is `.seqCst`.** The right answer when the developer has not yet thought about the question. Choosing a weaker ordering is an explicit, audit-visible act — `vestra audit --atomics` lists every call site and its ordering.
2. **`inout` on an `Atomic` method does not require a `var` binding.** A `static x: Atomic[UInt32]` is callable as `x.fetchAdd(1, .relaxed)` — the cell-level mutation is what the type *is for*, and §1's no-mutable-statics rule was written with this exemption in mind. The `inout` exists for the borrow checker's intra-thread exclusivity story (§5), not for the binding-class story (§1).
3. **`T: Primitive` is structural.** Non-primitive payloads — a struct, a `Box`, an enum — cannot be `Atomic[T]`; those are the province of `Mutex` and `RwLock`, which are library, not language. The kernel's tagged-pointer patterns are reachable through `Atomic[UInt64]` plus `UInt64(bitPattern: ptr)`.

### 14.9.3 Compare-exchange

Two forms, named for what they guarantee on failure:

```vestra
extension Atomic where T: Primitive {
    // Strong: never spurious-fails. Use when the call site is not a loop.
    inout func compareExchange(
        expected: T,
        desired:  T,
        success:  Ordering = .seqCst,
        failure:  Ordering = .acquire,
    ) -> CASResult[T]

    // Weak: may spurious-fail on LL/SC architectures (aarch64, riscv).
    // Cheaper inside a retry loop.
    inout func compareExchangeWeak(
        expected: T,
        desired:  T,
        success:  Ordering = .seqCst,
        failure:  Ordering = .acquire,
    ) -> CASResult[T]
}

struct CASResult[T: Primitive] {
    let succeeded: Bool
    let actual:    T   // value observed on failure, or `desired` on success
}
```

Three rules:

1. **`failure` may not be stronger than `success`.** A compile error names the pair.
2. **`failure` may not be `.release` or `.acqRel`.** It describes a *failed* load.
3. **The weak form is rejected outside a loop.** Sema looks for an enclosing `while` / `for` / `loop` whose continuation reads `result.succeeded` (directly, or through a `let` binding). A free-standing `compareExchangeWeak` is a hard error suggesting `compareExchange`. The escape is `@allow_weak_cas` on the call site, audit-visible.

The retry loop is then the unambiguous shape every architecture compiles cleanly, with no convention layered on top:

```vestra
while true {
    let observed = head.load(.acquire)
    let r = head.compareExchangeWeak(
        expected: observed,
        desired:  newHead(after: observed),
        success:  .acqRel,
        failure:  .acquire,
    )
    if r.succeeded { break }
}
```

### 14.9.4 Wide atomics

`Atomic[U]` where `U` is 128 bits — `UInt128` / `Int128`, themselves a separate 3.4 addition for the targets that natively model them — is supported on aarch64 (`LDXP` / `STXP` / `CASP`) and on x86_64 with the `cx16` feature (`CMPXCHG16B`). On a target lacking native 128-bit atomics, an `Atomic[UInt128]` declaration is a **link-time error** naming the missing feature — never a silent fallback to a lock.

The library supplies the typed tagged-pointer wrapper the lock-free queues, the seqlock hash tables, and the generation-counted handle vocabularies all use:

```vestra
struct AtomicTaggedPointer[T] {
    var bits: Atomic[UInt128]            // { pointer: UInt64, tag: UInt64 }

    func        load           (_ ordering: Ordering = .seqCst)             -> (Ptr[T]?, UInt64)
    inout func  store          (_ ptr: Ptr[T]?, _ tag: UInt64,
                                _ ordering: Ordering = .seqCst)
    inout func  compareExchange(expected: (Ptr[T]?, UInt64),
                                desired:  (Ptr[T]?, UInt64),
                                success:  Ordering = .seqCst,
                                failure:  Ordering = .acquire)               -> CASResult[(Ptr[T]?, UInt64)]
}
```

### 14.9.5 Deliberate omissions

- **No `consume` ordering.** The compilers don't implement it as anything other than `acquire`; the spec follows the implementation. `acquire` is the right place to land.
- **No `volatile` modifier on `Atomic`.** Atomics already imply both ordering and the prohibition on coalesced reads; `volatile` is for MMIO (§14.11), which is a separate construct on a separate capability.
- **No mixed-size atomic operations.** A 4-byte store and an 8-byte load over the same cell is undefined behaviour in every memory model the world ships; Vestra rejects the construct (the cell's `T` is the only width the program may use through this binding).
- **No language-level fences here.** A free-standing fence is a CPU operation independent of any specific cell, and lives in §14.10 alongside the cache / TLB / sync intrinsics — separating *operations on a memory cell* from *operations on the pipeline* keeps the chapters honest.

The atomic model is the smallest one a working kernel can ship: five orderings, four operation families, two CAS forms, one wide-atomic story. The vocabulary is C11's, sharpened — defaults that are safe, rejection of orderings the operation cannot carry, weak-CAS-requires-loop, no consume, no mixed widths. Embedded Swift's `swift-atomics` package becomes a language feature, and the call-site ceremony the package currently demands — `wordValue.atomicLoad(ordering: .acquiring)`, `Atomic<Int>.AtomicRepresentation` — collapses to the `head.load(.acquire)` shape the developer expected the first time.

---

## §14.10 (new) — Sync intrinsics: barriers, cache, TLB, pipeline hints

§14.9 covers *operations on a memory cell*. §14.10 covers *operations on the pipeline* — the CPU-state intrinsics that today live behind one-instruction inline `asm` wrappers: compiler-only fences, hardware memory barriers, cache-line management, TLB invalidation, and the wait-for-interrupt / pause / prefetch family. Every kernel currently spells these as one-instruction `asm` blocks behind hand-rolled helpers; the spec admits the helpers directly, naming each by what it *does to the machine*, and gates the whole family on `Asm` (the capability that already governs the equivalent inline assembly).

The chapter is organised by the pipeline structure each intrinsic addresses: the compiler barrier, the CPU memory barrier, the data cache, the instruction cache, the TLB, and the pipeline-power hints.

### 14.10.1 Compiler fences

A compiler fence emits **no machine code** — it pins the optimizer's view of "what is reachable from here." The kernel needs this in two places: between a write to a `static` cell and a check of an interrupt flag set by an ISR sharing the same cache; and between a sequence of MMIO operations whose order the silicon requires but no atomic spans.

```vestra
core.systems.sync.compilerFence(_ ordering: Ordering) using Asm
```

The `Ordering` is the §14.9 enum; `.relaxed` is rejected (a relaxed compiler fence is the absence of a fence). The compiler emits a sequence point with the requested ordering and no instruction.

### 14.10.2 CPU memory barriers

Three barrier kinds, one per pipeline stage:

```vestra
extension Cpu {
    static func memoryBarrier  (_ scope: BarrierScope, _ kind: BarrierKind) using Asm
    static func syncBarrier    (_ scope: BarrierScope)                      using Asm
    static func instructionBarrier()                                        using Asm
}

enum BarrierScope { case full, inner, outer, nonShareable }
enum BarrierKind  { case loadLoad, storeStore, loadStore, full }
```

The mapping is fixed by the target description:

| Call | aarch64 | x86_64 |
|---|---|---|
| `memoryBarrier(.full, .full)` | `dmb sy` | `mfence` |
| `memoryBarrier(.full, .loadLoad)` | `dmb ld` | `lfence` |
| `memoryBarrier(.full, .storeStore)` | `dmb st` | `sfence` |
| `memoryBarrier(.inner, .full)` | `dmb ish` | `mfence` |
| `syncBarrier(.full)` | `dsb sy` | `mfence` |
| `syncBarrier(.inner)` | `dsb ish` | `mfence` |
| `instructionBarrier()` | `isb` | (omitted; x86 self-syncs on serializing instructions) |

Two rules:

1. **The function rejects ordering enums.** A barrier is not an atomic operation; mixing the two vocabularies confuses the call site. The barrier kind is its own enum.
2. **`instructionBarrier()` on x86 emits no code and is not an error.** The platform serializes the pipeline on every privileged instruction. The compiler still admits the call so portable kernel code may name the boundary the aarch64 path needs.

### 14.10.3 Data cache management

A kernel manages the data cache for two reasons — DMA coherence and code patching. The vocabulary is line-oriented; the unit is the cache line, sized by `cfg.option("cache_line_bytes")` (§14.8).

```vestra
extension Cache {
    // Clean (write back to memory; line stays valid).
    static func cleanData          (range: Span[UInt8])    using Asm, RawMemory
    // Invalidate (drop the line; future reads miss to memory).
    static func invalidateData     (range: Span[UInt8])    using Asm, RawMemory
    // Clean + invalidate (write back, then drop).
    static func cleanInvalidateData(range: Span[UInt8])    using Asm, RawMemory

    // Zero an aligned, line-multiple range using the platform's
    // dedicated zeroing instruction (`dc zva` on aarch64; rep-stosb
    // fallback on x86). The fast path the page-zeroer wants.
    static func zeroData(range: MutSpan[UInt8])            using Asm, RawMemory
}
```

Three rules:

1. **The `Span` is line-aligned at both ends.** A range whose start or end is not a multiple of `cfg.option("cache_line_bytes")` is a hard error naming the misalignment. The kernel knows its line size; the spec refuses to silently extend it.
2. **`RawMemory` is required.** The intrinsic operates on memory the type system cannot see (raw bytes), and the discharge is the source binding's.
3. **`zeroData`'s range must additionally satisfy the platform's zeroing-block size.** On aarch64 `DCZID_EL0` advertises this; the compiler reads it at boot through a target-config query and pins the rule. A non-block-aligned `zeroData` is rejected; the fallback is `cleanInvalidateData` plus a memset.

### 14.10.4 Instruction cache management

Code patching, JIT, and trampoline writes all need to evict stale instruction-cache lines.

```vestra
extension Cache {
    // Invalidate I-cache lines covering `range` and emit the data-cache
    // clean + barrier the architecture requires to publish the writes.
    // This is the full "I just wrote new code at this address" pattern.
    static func publishInstructions(range: Span[UInt8]) using Asm, RawMemory

    // Invalidate the whole I-cache. Use sparingly — the kernel's boot
    // and CPU-online paths are the canonical call sites.
    static func invalidateAllInstructions() using Asm
}
```

`publishInstructions` is the operation the spec names; the per-architecture sequence (`dc cvau ; dsb ish ; ic ivau ; dsb ish ; isb` on aarch64; a serializing instruction plus a fence on x86_64) is target-table data. The kernel names the *intent*; the compiler emits the sequence.

### 14.10.5 TLB management

Three scopes, three address shapes:

```vestra
extension Tlb {
    // Whole-TLB invalidation, by EL on aarch64 / by current PCID on x86.
    static func invalidateAll(scope: TlbScope) using Asm

    // Single-page invalidation. `va` is a virtual address; the page
    // size is the platform's smallest (4KiB) unless `large:` is named.
    static func invalidatePage(va: UInt64,
                               large: Bool = false,
                               scope: TlbScope) using Asm

    // ASID-scoped invalidation (aarch64) / PCID-scoped (x86 with INVPCID).
    static func invalidateAsid(_ asid: UInt16, scope: TlbScope) using Asm
}

enum TlbScope { case currentEL, allELs, innerShareable, outerShareable }
```

One rule: **`invalidateAsid` on a target without ASID/PCID hardware is a link-time error.** The kernel's address-space teardown path is platform-dependent; the spec refuses to silently degrade it to a whole-TLB flush.

### 14.10.6 Pipeline / power hints

The wait-for-event family, the spinlock-relax instruction, the speculative prefetcher hint, and the privileged halt:

```vestra
extension Cpu {
    static func waitForInterrupt() using Asm  // wfi   / hlt
    static func waitForEvent()     using Asm  // wfe   / mwait (priv)
    static func signalEvent()      using Asm  // sev
    static func relax()            using Asm  // yield / pause
    static func nop()                          // alignment / instr-density helper

    static func prefetch(
        _ at:       Ptr[UInt8],
        intent:     PrefetchIntent     = .read,
        locality:   PrefetchLocality   = .l1,
    ) using Asm
}

enum PrefetchIntent   { case read, write, execute }
enum PrefetchLocality { case l1, l2, l3, nonTemporal }
```

`relax()` is the spinlock body's discipline — the same call lands as `yield` on aarch64 and `pause` on x86, and the autoformatter inserts it as the first statement of any `while true { ... }` loop whose body's only side effect is a `compareExchange` retry. The kernel's prior `cpu_relax()` macro becomes a typed call with no platform `#ifdef`.

### 14.10.7 Deliberate omissions

- **No `wbinvd` / `wbnoinvd` shortcut.** Whole-system writeback is a privileged x86-only instruction with no aarch64 analogue and a brutally large cost. A kernel that needs it writes the asm explicitly; the spec does not bless it.
- **No software-implemented atomic fallback on a target lacking native atomics.** `Atomic[T]` (§14.9) and the barriers here both pin to hardware support; the spec rejects the build instead of generating a lock-based shim.
- **No `monitor` / `mwait` outside `waitForEvent()`.** The kernel's idle path is `waitForEvent()` plus a `signalEvent()` from the ISR; the lower-level `monitor` interface is an `asm` block under `Asm`, audit-visible.

The chapter is the smallest one that lets a freestanding kernel build with zero one-instruction `asm` helpers. Every call here has a per-target lowering pinned by the target description, every call is gated on `Asm` (and where the operation touches memory the type system cannot see, additionally on `RawMemory`), and every misuse the spec names — non-line-aligned cache range, ASID flush on a no-ASID target, `relaxed` compiler fence — is a compile-time or link-time error. The `freestanding.S` files that today wrap `dmb sy`, `dc cvac`, `tlbi vae1is`, `wfi`, and `prfm pldl1keep` shrink to the entries that genuinely encode a multi-instruction sequence the spec leaves to assembly on purpose.

---

## §14.11 (new) — Memory-mapped I/O

A driver reads and writes device registers across a memory map the kernel set up at boot. Today every such access is either inline `asm` with `volatile` constraints or a hand-rolled `volatile UInt32*` cast — both bypass the type system and admit no width, no ordering, and no endianness check. Draft 3.3 named the `Mmio` capability; Draft 3.4 specifies the operations that capability gates.

The shape is a typed *view* over a mapped region — same family as §10.5's `MutSpan.raw(at:count:)` with implicit volatile semantics, a strict width contract, and a per-view endianness story.

### 14.11.1 `MmioView[T]`

A handle for one device register. Every read emits exactly one load of width `sizeof(T)`; every write emits exactly one store. The compiler will not coalesce, duplicate, or reorder a `MmioView` access against any other side-effecting operation.

```vestra
struct MmioView[T: Primitive] {
    static func at(_ ptr: MutPtr[T]) using Mmio -> MmioView[T]

    func        read()                -> T
    inout func  write(_ value: T)
}
```

Four rules:

1. **`T: Primitive`.** Only `UInt8` / `Int8` / `UInt16` / `Int16` / `UInt32` / `Int32` / `UInt64` / `Int64` — the widths the silicon's bus actually carries. Pointers no; an MMIO register is a hardware word, not a pointer slot. Structs no; the composition for bit-field registers is §14.11.4.
2. **Width is structural.** A `MmioView[UInt32]` emits a 4-byte access at a 4-byte-aligned address; a non-aligned mint is a hard error. The compiler will not synthesize an 8-byte access from two 4-byte ones, nor split a 4-byte access into byte loads.
3. **The lifetime is anchored to the `Mmio` discharge.** A view minted under `with Mmio { … }` is non-escapable past the block's end — same story as §10.5's `MutSpan.raw`. The kernel typically discharges `Mmio` *per device*, in the driver's init path, in a `with` that brackets the device's lifetime.
4. **No layered `volatile`.** The view IS the discipline; an attempt to write `@volatile var x: MmioView[T]` is rejected with a diagnostic pointing at the view's name.

### 14.11.2 `MmioRegion[T]`

For a typed array of registers — UART FIFO words, GIC distributor entries, page-table cells presented as an MMIO array — the region form gives indexed access:

```vestra
struct MmioRegion[T: Primitive] {
    static func at(_ ptr: MutPtr[T], count: Int) using Mmio -> MmioRegion[T]

    var  count: Int
    func index(_ i: Int) -> MmioView[T]
}
```

Out-of-range indexing is the §10.4 trap (overflow / index panic), not silent. The kernel pattern is to mint one `MmioRegion[UInt32]` over the device's register file at probe time and project per-register `MmioView[UInt32]` instances at the offsets the driver knows.

### 14.11.3 Endianness: `MmioWireView[T]`

Some devices present registers in a fixed endianness independent of the CPU's — PCIe legacy regions, NICs with a big-endian DMA descriptor, GPU registers that follow the device's bus convention. The wire form applies the byte swap on each access:

```vestra
struct MmioWireView[T: Primitive] {
    static func at(_ ptr: MutPtr[T], endianness: Endianness) using Mmio -> MmioWireView[T]

    func        read()                -> T   // applies swap if endianness != cpu
    inout func  write(_ value: T)
}

enum Endianness { case little, big, native }
```

The compiler emits the swap only when the device endianness disagrees with the CPU's; `.native` is the zero-cost form for the common case. The `Endianness` enum integrates with §6's `@repr(wire)` — a `@repr(wire) struct` mapped over an `MmioWireView[UInt32]` is the canonical "decode a wire register into named fields" shape.

### 14.11.4 Composition with `@repr(union)` for bit-field registers

A device register is usually one word whose bits encode multiple fields. The natural composition is `MmioView[UInt32]` plus the `@repr(union)` overlay from §6.8:

```vestra
@repr(packed)
struct UartLineStatusBits {
    @bits(1) var rxReady:     UInt32
    @bits(1) var rxOverrun:   UInt32
    @bits(1) var parityError: UInt32
    @bits(1) var framingError:UInt32
    @bits(1) var breakDetect: UInt32
    @bits(1) var txEmpty:     UInt32
    @bits(1) var txIdle:      UInt32
    @bits(1) var rxFifoError: UInt32
    @bits(24) var reserved:   UInt32
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

The read is one 32-bit MMIO load; the bit-field decode is a pure register-level operation on the snapshot — no second MMIO read, no torn-read window. Writes follow the read-modify-write pattern explicitly: mint a snapshot, edit its `fields`, write `snapshot.raw`. The compiler does not insert read-modify-write itself; the kernel chooses when each step happens.

### 14.11.5 Ordering between MMIO and ordinary memory

`MmioView` operations interact with §14.10's barriers per the architecture's rules — on aarch64, an MMIO write followed by a "is the device done?" read needs a `Cpu.syncBarrier(.full)` between them to make the write actually leave the store buffer; on x86 the bus model handles the ordering itself. The spec does **not** insert an implicit barrier on each MMIO access — that would penalize the common batched-writes pattern. The driver names the barriers, and the §14.10 lowering table emits them.

`vestra audit --mmio` enumerates every `MmioView.write` followed by an `MmioView.read` of the same region without an intervening `Cpu.syncBarrier` — a common bug class, now findable at build time.

### 14.11.6 Deliberate omissions

- **No mmap-style "find me a free virtual range" primitive.** The kernel's MMU code constructs the virtual mapping; the spec admits the *typed view over that mapping*, not the mapping operation itself.
- **No "side-effect free read" attribute.** Some MMIO reads have side effects (clear-on-read status bits, FIFO pop). The view treats every read as side-effecting; the developer who needs a "read without consequence" reads a shadow register the driver maintains in ordinary memory.
- **No `MmioView` over a `Ptr[T]?`.** A null-MMIO is a driver bug, not a recoverable state. The constructor takes `MutPtr[T]`, which is non-nullable; the driver init path validates the device's base address before minting any view.

---

## §14.12 (new) — System register access

A kernel reads and writes ~50 architectural registers per platform — exception level, vector base, TLB control, MMU control, current-thread pointer, system tick, debug. Today every access is inline `mrs` / `msr` (aarch64), `rdmsr` / `wrmsr` (x86), or `csrr` / `csrw` (riscv), wrapped in a hand-rolled helper. Draft 3.4 admits the sysregs directly: a target-knobbed table of typed handles, gated on `Asm`, with the architecture-required barriers emitted automatically.

### 14.12.1 Typed sysreg handles

Three compiler-known type constructors capture the read/write capability of each register:

```vestra
struct ReadOnlySysreg [T: Primitive] { func        read()                        -> T using Asm }
struct WriteOnlySysreg[T: Primitive] { static func write(_ value: T)             using Asm }
struct ReadWriteSysreg[T: Primitive] { func        read()                        -> T using Asm
                                       inout func  write(_ value: T)             using Asm }
```

The `T` width is the register's architectural width (`UInt8` for `CurrentEL`, `UInt32` for most x86 control registers, `UInt64` for almost every aarch64 sysreg and every MSR). The compiler emits one `mrs` / `msr` / `rdmsr` / `wrmsr` / `csrr` / `csrw` per call; no fallback path exists.

### 14.12.2 The `Sysreg` namespace

The standard library declares one named instance per architectural sysreg the target description knows, in `core.systems.sysreg`:

```vestra
// AArch64 EL1 (target = aarch64-*-elf with EL1 profile)
let midr_el1:    ReadOnlySysreg[UInt64]
let mpidr_el1:   ReadOnlySysreg[UInt64]
let currentEL:   ReadOnlySysreg[UInt8]
let daif:        ReadWriteSysreg[UInt64]
let tpidr_el1:   ReadWriteSysreg[UInt64]
let sctlr_el1:   ReadWriteSysreg[UInt64]     // ISB-on-write (§14.12.3)
let vbar_el1:    ReadWriteSysreg[UInt64]     // ISB-on-write
let ttbr0_el1:   ReadWriteSysreg[UInt64]     // ISB-on-write
let ttbr1_el1:   ReadWriteSysreg[UInt64]     // ISB-on-write
let tcr_el1:     ReadWriteSysreg[UInt64]     // ISB-on-write
let mair_el1:    ReadWriteSysreg[UInt64]     // ISB-on-write
let esr_el1:     ReadOnlySysreg[UInt64]
let far_el1:     ReadOnlySysreg[UInt64]
let elr_el1:     ReadWriteSysreg[UInt64]
let spsr_el1:    ReadWriteSysreg[UInt64]
let cntfrq_el0:  ReadOnlySysreg[UInt64]
let cntpct_el0:  ReadOnlySysreg[UInt64]
let dczid_el0:   ReadOnlySysreg[UInt64]

// x86_64 MSRs (target = x86_64-*-elf)
let ia32_efer:           ReadWriteSysreg[UInt64]
let ia32_star:           ReadWriteSysreg[UInt64]
let ia32_lstar:          ReadWriteSysreg[UInt64]
let ia32_fmask:          ReadWriteSysreg[UInt64]
let ia32_kernel_gs_base: ReadWriteSysreg[UInt64]
let ia32_gs_base:        ReadWriteSysreg[UInt64]
let ia32_fs_base:        ReadWriteSysreg[UInt64]
let ia32_apic_base:      ReadWriteSysreg[UInt64]
let ia32_pat:            ReadWriteSysreg[UInt64]

// RISC-V (target = riscv64-*-elf, S-mode profile)
let sstatus:  ReadWriteSysreg[UInt64]
let sie:      ReadWriteSysreg[UInt64]
let stvec:    ReadWriteSysreg[UInt64]
let sscratch: ReadWriteSysreg[UInt64]
let sepc:     ReadWriteSysreg[UInt64]
let scause:   ReadOnlySysreg[UInt64]
let stval:    ReadOnlySysreg[UInt64]
let satp:     ReadWriteSysreg[UInt64]
```

Use sites are the natural noun form:

```vestra
let el  = Sysreg.currentEL.read()        // UInt8: 0, 1, 2, or 3
let tcb = Sysreg.tpidr_el1.read()        // current thread control block
Sysreg.daif.write(0xF0)                  // mask D/A/I/F asynchronous exceptions
Sysreg.sctlr_el1.write(newSctlr)         // MMU control — compiler emits the ISB
```

Three rules:

1. **The `Sysreg` set is target-determined.** `Sysreg.foo` referring to a register the target does not know is a compile error naming the target. Cross-architecture code lives behind `@when(cfg.arch == .aarch64)` (§12.6).
2. **Each handle is a compiler-known singleton.** The fields are not constructible by the user — the compiler synthesizes them per target, and `vestra audit --sysreg` enumerates the set in the build.
3. **No taking the address.** `&Sysreg.daif` is a compile error. A sysreg is not a memory cell with a linkage symbol; the read/write call is the only operation.

### 14.12.3 Architecturally-required barriers

Several sysregs require an explicit barrier after a write before the new value takes effect — `SCTLR_EL1` (MMU on/off), `TTBR0/1_EL1` (TLB root), `TCR_EL1` (translation control), `MAIR_EL1` (memory attribute), `VBAR_EL1` (vector base) all need an `ISB` after the `msr`. Forgetting it is a class of bug that costs developers a day each.

The target description names which sysregs require which barriers. **The compiler emits the barrier automatically**; the call site does not say it. The audit (`vestra audit --sysreg`) enumerates every required barrier emission so a misconfigured target is findable.

The escape — for the rare case where the kernel knows it's about to issue another sysreg write that subsumes the barrier — is `@no_auto_barrier` on the call site:

```vestra
@no_auto_barrier
Sysreg.ttbr0_el1.write(newPgd)        // I know the following SCTLR write
Sysreg.sctlr_el1.write(mmuEnable)     // will carry an ISB for both
```

`@no_auto_barrier` is audit-visible and rejected outside a function attributed `@kernel_init` (the boot-path subset where this pattern is admissible).

### 14.12.4 Privilege and the trap-on-violation model

A read of `SCTLR_EL1` from EL0 (userland) traps to EL1 — the CPU enforces privilege. The spec does **not** statically reject a privileged sysreg access from a function not gated on a kernel-mode capability; the typed call admits the operation, and the hardware enforces the runtime check. The reason is that the boot-target/runtime-privilege coupling is the build's concern, not the call site's — a `@kernel_init` function calling `Sysreg.sctlr_el1.write` is correct at EL1, and the same source compiled for an EL0 userland profile is rejected by the *build target*, not the spec.

The audit lists every sysreg access and the privilege the architecture requires; a build for a target whose runtime privilege is lower than any access requires is a link-time error.

### 14.12.5 Deliberate omissions

- **No string-to-sysreg lookup.** `Sysreg.byName("daif")` is not provided. The set is closed by the target description; reflection over it is `vestra audit --sysreg`'s job, not a runtime operation.
- **No per-EL alias bouncing.** `Sysreg.daif` is the EL1 form; the EL2 form `Sysreg.daif_el2` is a separate handle, and a function compiled for EL1 may not reference it. The boot path that transitions ELs uses `@when(cfg.exceptionLevel == .el2)` to scope the EL2 references.
- **No PMU / debug register sub-vocabulary.** `PMCCNTR_EL0`, `MDSCR_EL1`, and the debug registers are present in the table but the spec does not bless a higher-level "performance counter" or "single-step" abstraction here — those are library on top of the raw sysreg handles, and `vestra audit --sysreg` finds every direct touch.

The two chapters together — §14.11's typed MMIO views and §14.12's typed sysreg handles — eliminate the last categorical use of inline `asm` for a single-instruction wrapper. The remaining `asm` in a 3.4 kernel is the multi-instruction sequence the spec leaves to assembly on purpose: the pre-MMU bring-up before `@boot` can name its own data, the exception-entry trampolines a platform's vector format doesn't fit `@interrupt`'s prologue, and the rare ISA-specific sequence (a `CLREX` before a CAS retry under contention, a `WFE`-with-explicit-event-mask) that the spec deliberately doesn't generalize.

---

## Draft 3.4 changelog

- §14.9 — `Atomic[T]` memory model: five orderings (`relaxed` / `acquire` / `release` / `acqRel` / `seqCst`); core ops (`load`, `store`, `exchange`, `fetchAdd` / `Sub` / `And` / `Or` / `Xor`, plus `fetchSet` / `Clear` on `Bool`); strong + weak compare-exchange with the `failure ≤ success` ordering check and the weak-only-in-loop rule; 128-bit atomics on `aarch64` / `x86_64+cx16` with link-time rejection elsewhere; `AtomicTaggedPointer[T]` as the typed DCAS shape.
- §14.10 — Sync intrinsics: compiler fences, CPU memory barriers (`Cpu.memoryBarrier` / `syncBarrier` / `instructionBarrier`), data-cache management (`Cache.cleanData` / `invalidateData` / `cleanInvalidateData` / `zeroData`), instruction-cache management (`Cache.publishInstructions` / `invalidateAllInstructions`), TLB management (`Tlb.invalidateAll` / `invalidatePage` / `invalidateAsid`), and the pipeline-power hint family (`Cpu.waitForInterrupt` / `waitForEvent` / `signalEvent` / `relax` / `nop` / `prefetch`). All gated on `Asm`; range-taking entries additionally on `RawMemory`.
- §14.11 — Memory-mapped I/O: typed `MmioView[T]` per register and `MmioRegion[T]` per register file with width-correct, volatile, non-coalescing accesses; `MmioWireView[T]` for device-side endianness; composition with `@repr(union)` for bit-field registers; lifetime anchored to the `Mmio` discharge (same family as §10.5's `MutSpan.raw`); ordering against ordinary memory delegated to §14.10's barriers with a `vestra audit --mmio` finding pass.
- §14.12 — System register access: target-knobbed `Sysreg` namespace populated per platform (aarch64 EL1 set, x86_64 MSRs, riscv S-mode CSRs) over `ReadOnlySysreg[T]` / `WriteOnlySysreg[T]` / `ReadWriteSysreg[T]` handles, each emitting one `mrs` / `msr` / `rdmsr` / `wrmsr` / `csrr` / `csrw`; architecturally-required barriers (ISB after MMU-control writes) emitted automatically with `@no_auto_barrier` as the audited escape; privilege enforcement deferred to the hardware with a `vestra audit --sysreg` enumeration and a link-time build-target check.

The acceptance criterion for Draft 3.4 is that the `freestanding.S` helpers shrink to the multi-instruction sequences the spec leaves to assembly on purpose — exception-entry trampolines that don't fit `@interrupt`'s prologue, MMU bring-up before §14.7's `@boot` can name its own data, and the `eret` / `iret` paths the `@interrupt` epilogue already emits. Single-instruction asm wrappers — `dmb sy`, `dsb ish`, `isb`, `dc cvac`, `tlbi vmalle1is`, `wfi`, `prfm pldl1keep`, `cpu_relax`, `pause`, plus the entire `mrs` / `msr` / `rdmsr` / `wrmsr` / `csrr` / `csrw` family and the per-register MMIO `volatile` casts — go away, replaced by named intrinsics, typed views, and named sysreg handles gated on the same capabilities the asm blocks were.

---

## Open questions for Draft 3.5

1. **`UInt128` / `Int128` as primitives.** §14.9.4 names these for 128-bit atomics but the type itself is not yet specified. The arithmetic surface (signed-overflow trap, unsigned wraparound, division support on targets without native 128/64 divide) needs its own pass before commit. Until then `Atomic[UInt128]` reads the cell only — arithmetic on the unwrapped value is the kernel's responsibility.

2. **Interrupt-disabled discipline (§14.13).** A scoped `with InterruptsOff { … }` block that masks asynchronous exceptions on entry, restores the prior mask on exit, and forbids `Async` / `Alloc` / non-`@inline(.always)` calls inside. The §14.12 sysreg handles for `DAIF` (aarch64) and `EFLAGS.IF` (x86) make the *operation* expressible today; the *discipline* — non-leak, non-yield, bounded — needs its own §14.13 to be enforceable rather than convention.

3. **Stack switching (§14.14).** §16 omits a general-purpose context-switch primitive on purpose, but the scheduler still needs *some* typed `swap_context(saved: MutPtr[Context], target: Ptr[Context]) using Asm` shape — the omission of *policy* is right; the omission of the *primitive* is what forces drop-to-asm. A 3.5 §14.14 names the primitive with the `Context` type whose layout the target description fixes per ISA, and the bounded register set the primitive saves and restores.

4. **Panic-handler hook (§15.5) and `@no_libc` audit (§16 amendment).** A `@panic_handler` attribute that names the function the compiler links its panic / overflow / `Never`-return emissions against, plus a manifest flag (`profile.freestanding.no_libc = true`) whose audit refuses every undeclared external symbol. Together they make "zero references to libc" a build-time guarantee rather than a code-review one.
