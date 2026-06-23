# Vestra Draft 3.3 — Bare-metal completeness deltas

*Draft 3.3 closes the gap between the language as specified and a kernel that needs no C source file. Where Draft 3.2 said "C++ interop is out of scope" and stopped, 3.3 says **C is out of scope** — the kernel is Vestra and assembly, period. The additions are narrow and load-bearing: every one of them is forced by a concrete pattern in a working kernel, and none of them touch §1. The cost is paid where it should be — in the systems chapter (§14) and the layout/library chapters (§6, §10, §18) — leaving the §1–§13 surface a Swift- and Scala-3-flavoured language a generalist still reads in an afternoon.*

---

## §4.5 (new) — Uninitialized static storage

A kernel reserves fixed regions of `.bss` — page tables, capability tables, ring buffers, per-CPU slabs — sized by linker, populated by code, and never *initialized* in the source sense. The Draft 3.2 rule that a `static` binding takes either an immutable value or an `Atomic` (§1) is too narrow for this case; the value is mutable but its *publication* is structural, not reachable through `static` syntax.

Vestra adds one form, the **uninitialized static**:

```vestra
@noinit @section(".bss.cap_table")
static cap_table_buf: [4 * 1024 * 1024]UInt8
```

Three rules govern it — every one is load-bearing:

1. **No initializer.** `@noinit` replaces the `= expr` required of an ordinary `static`. The compiler emits a symbol of the declared size and alignment, in the named section (§6.7), with no initial bytes — i.e., `.bss`.
2. **The element type must be `Trivial`.** Resource-owning types have invariants a zero-initialized image would violate; `@noinit` is for register-class data and arrays of it. Constructing a `Box`, `Buffer`, or any type with a `deinit` in a `@noinit` storage is rejected.
3. **The storage is `RawMemory` to reach.** A `@noinit static` does **not** participate in §1's static rule — the binding is *not* in scope as a value. To read or write its bytes, take its address with `&buf` (§14.6) and gate the access on `RawMemory` (§14.1), or — the recommended pattern — wrap it once at startup in a `MutSpan` (§10.5) and discharge `RawMemory` there.

`@noinit` is the **only** form of uninitialized storage in Vestra; an ordinary `static` is always initialized, an ordinary `let`/`var` is always definitely-assigned (§19.2). The escape is named, gated, and audit-visible — `vestra audit --bss` lists every `@noinit static` and its size.

---

## §6.7 (new) — Section placement, linkage, and symbol attributes

Layout in Draft 3.2 covered *field arrangement* — `@repr(C)`, `@repr(packed)`, `@repr(align(N))`, `@repr(wire)` — and stopped at the struct boundary. The kernel needs control of the *symbol* a declaration produces and the *section* it lands in. Vestra adds five attributes, all on declarations, all inert outside link-time:

| Attribute | Meaning |
|---|---|
| `@section(name)` | Place the declaration in linker section `name`. Applies to `func`, `static`, and `@noinit static`. |
| `@symbol(name)` | Export the declaration under the literal linker name `name`, overriding name-mangling. Applies to `func` and `static`. |
| `@weak` | The symbol is weak — a strong definition elsewhere overrides it; an unresolved weak reference is null, not a link error. |
| `@alias(other)` | The declaration is a second name for `other`; emits an alias symbol, no body required. |
| `@visibility(.default \| .hidden \| .protected)` | ELF visibility class. |

```vestra
@section(".text.boot") @symbol("_start") @naked
func bootEntry()

@section(".bss.percpu") @noinit
static perCpuSlab: [MAX_HARTS * 4096]UInt8

@weak @symbol("__stack_chk_guard")
static stackChkGuard: UInt64 = 0xDEADBEEFCAFEBABE
```

These attributes do nothing to the *language* — every type and every guarantee is unchanged. They tell the linker where the artifact lives and what label it carries, replacing the C trampolines a freestanding target currently needs to do the same.

---

## §6.8 (new) — Layout completeness

Five additions to the layout vocabulary, each forced by a real kernel ABI shape.

**Untagged unions.** `@repr(union)` on a `struct` collapses its fields into one storage cell sized and aligned by their maximum:

```vestra
@repr(union)
struct PageTableEntryView {
    var raw:    UInt64           // overlays …
    var fields: PageTableEntryBits  // … this @repr(packed) struct
}
```

A `@repr(union)` struct is necessarily `Trivial` (every field must be `Trivial` *and* `Copyable`) and may be read as any one field at a time. Mutation of one field invalidates the others; the compiler treats the cell as opaque between writes. There is no tag, no discriminant, and no `match` — this is the *untagged* overlay, distinct from §6's `enum`. The kernel uses it for register-views and ABI structures that the silicon presents as both a word and a set of fields.

**Explicit padding.** A field may be declared `_: [N]UInt8 @padding` to reserve exactly `N` bytes that the compiler must not coalesce with neighbouring padding. This pins the offset of fields after it under `@repr(packed)` and `@repr(C)`.

**Anonymous fields.** An `embed` (§6) with no name (`embed _: Header`) flattens its fields into the outer namespace without introducing a name for the embed itself — the pattern C anonymous structs/unions cover.

**Flexible array members.** A struct's *last* field may be declared `var data: [_]T` — the compiler emits the header with no storage for `data` and the field is reachable only through `withFlexibleArray(count:)`, which yields a `MutSpan[T]` of caller-asserted length, gated on `RawMemory`. This expresses the C `struct { … T data[]; }` ABI exactly, without admitting C's silent out-of-bounds access.

**Compile-time layout reflection.** §12.2's `Type` value gains three queries — `Type.size`, `Type.alignment`, and on `Field`: `offset`, `size`, and `alignment`. The familiar `sizeof`/`alignof`/`offsetof` patterns are then ordinary `comptime` expressions:

```vestra
const ESR_OFFSET: Int = comptime { Field.of(\AArch64TrapFrame.esr).offset }

comptime { assert(Type.of(CapabilityEntry).size == 64) }
```

`assert` in a `comptime` block is a static check — failing it is a compile error.

---

## §7.8 (new) — The `@inline` directive

A kernel needs *occasional* explicit control over inlining: the slot-pointer helpers, the trap-frame projections, the volatile-register builders. Draft 3.2 left this implicit. Vestra adds one attribute, three levels:

```vestra
@inline(.always) func slot(_ i: Int) -> MutSpan[Entry] { … }
@inline(.never)  func panicReport(_ msg: Str) -> Never { … }
@inline(.hint)   func popcount(_ x: UInt64) -> Int { … }
```

`.always` is a *correctness* directive — the body inlines at every call site or compilation fails; the compiler reports the call site that defeated it. `.never` forbids inlining (the diagnostic and trap-prologue cases). `.hint` is advisory. Absence of the attribute leaves the choice to the compiler, which is the right default for the >99% of functions where the choice is obvious. The attribute is *not* a fitness function for hot code — `@inline(.always)` is for cases where inlining is part of the *meaning* of the function (e.g., a `MutSpan` projection that must not introduce a function-call boundary the borrow checker cannot see through).

---

## §10.5 (new) — Minting views from raw memory

Draft 3.2's `Span[T]` and `MutSpan[T]` are non-escapable views whose lifetime is derived from a source binding. A kernel mints a view from *raw integers* — a base physical address and a length — and that operation needs both a constructor and a lifetime story.

The constructor is:

```vestra
extension MutSpan {
    static func raw(at: MutPtr[T], count: Int) -> MutSpan[T] using RawMemory
}
```

(and the analogous `Span.raw(at:count:)` for read-only views). The result's lifetime is **tied to the enclosing `with RawMemory` block** — the discharge site is the source binding. A `MutSpan` minted under `with RawMemory { … }` is non-escapable past that block's end; storing it in a heap value, returning it past the discharge, or capturing it in a `spawn`ed task is a compile error naming the discharge.

This is the principled answer to §19.4's open question. When a non-escapable value cannot be tied to a source binding (because it was conjured from integers), it is tied to the **capability discharge** that admitted the conjuring. The kernel typically discharges `RawMemory` *once*, at boot, in an outer `with` that brackets the lifetime of the kernel itself; a `MutSpan` over the `cap_table_buf` lives as long as the kernel does, and the type system records that fact without a runtime cost.

---

## §14.4 — Inline assembly: clobber discipline (tightened)

Draft 3.2's `asm` block validates operand constraints against the target ISA. Two clauses tighten it for the kernel case:

1. **Implicit clobbers are an error, not a warning.** A `mrs` instruction that writes a sysreg the operand list does not name is rejected, with a diagnostic naming the implicit write. The kernel currently relies on grep and convention; the spec must not.
2. **The `@extern("C")` call site assumes the platform-ABI callee-saved set is preserved.** A C blob (or hand-written `.S`) that clobbers `%rbx`/`%rbp`/`%r12–%r15` on SysV-x86_64 is **not** wrapped: the compiler trusts the foreign code to obey its declared convention. The companion `vestra audit --extern` enumerates every `@extern` call and the conv it claims, so a misbehaving FFI target is *findable*, not silent.

The kernel's prior `r12`-clobber incident becomes a build-time finding with the `--extern` audit, not a runtime corruption.

---

## §14.5 — Interrupt service routines (rewritten)

Draft 3.2's `@interrupt func` was a single ISR with no arguments. Real architectures have *vector tables* — an `N`-entry array of handlers indexed by exception class, vector number, or EL transition — and every handler needs the trap frame the hardware pushed. Vestra replaces the one-shot form with three coordinated declarations.

### 14.5.1 The trap-frame type

A trap frame is an ordinary `@repr(C) @repr(packed)` struct named by the platform; the standard library supplies the canonical shapes in `core.systems.trap`:

```vestra
// Compiler-known shape; layout fixed by target.
@repr(C) @repr(packed)
struct AArch64TrapFrame {
    var gprs: [31]UInt64
    var sp:   UInt64
    var elr:  UInt64
    var spsr: UInt64
    var esr:  UInt64
    var far:  UInt64
}
```

The kernel may declare its own frame shape under `@repr(C)`; the platform is *named*, not blessed, and a developer who builds for an unsupported target writes the frame themselves.

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

@interrupt(vector: 0x80)
func x86Syscall(frame: inout X86TrapFrame) using Asm { … }
```

Five rules:

1. **The first parameter is `inout TrapFrame`** — a non-escapable view of the frame the hardware (or the entry trampoline) pushed. Mutating it changes the registers the CPU restores on `eret`/`iretq`. No other parameters are admitted; an ISR has no caller to receive `using`-row authority from.
2. **The body discharges its own capabilities.** `using Asm`, `using RawMemory`, `using Mmio` in the signature mean the handler *mints* those discharges within itself — the body opens a `with Asm { … }` block at its top and closes it at the bottom. The signature row is *informational*, not propagated.
3. **No `Alloc`, no `Async`.** A handler may not name those capabilities; transitive calls to functions that require them are compile errors. An ISR is straight-line work; deferred work is signalled to an ordinary task through a `static Atomic`.
4. **Return type is `Unit`.** `@interrupt` functions do not `throw`. Failures route into the trap frame (e.g., an x86 `#GP` is dispatched as another vector).
5. **The compiler emits the prologue and epilogue.** Register save/restore, stack pivot if configured, and the return instruction (`eret`/`iretq`/`mret`) are emitted from the `@interrupt(...)` parameters — the entry-trampoline `.S` file goes away.

### 14.5.3 Vector tables

A vector table is a typed array of handler references, declared at module scope and placed by section:

```vestra
@section(".text.vec_table") @align(2048)
static aarch64VecTable: [16]@interrupt(AArch64TrapFrame) = [
    syncEL1, irqEL1, fiqEL1, serrorEL1,
    syncEL0Lower64, irqEL0Lower64, fiqEL0Lower64, serrorEL0Lower64,
    syncEL0Lower32, irqEL0Lower32, fiqEL0Lower32, serrorEL0Lower32,
    syncSPSel0, irqSPSel0, fiqSPSel0, serrorSPSel0,
]
```

`@interrupt(F)` is the type of a handler over frame type `F`; the table is an ordinary value, its address taken with `&aarch64VecTable` (§14.6.3) and written to `VBAR_EL1`. The x86 IDT is the same shape with `@interrupt(X86TrapFrame)` and a 256-entry table. The kernel writes the *table* in Vestra and the *frame-pushing trampoline* in assembly only if the platform's prologue cannot be expressed as `@interrupt`'s emitted prologue — which, for aarch64 and x86_64, it can.

---

## §14.6 — FFI, calling conventions, linker symbols (expanded)

Draft 3.2 had `@extern("C")` and one call gate, `Extern`. Three additions complete the chapter.

### 14.6.1 Pluggable calling conventions

`@extern("conv")` accepts any conv name the target backend knows: `"C"` (SysV / Win64 / AAPCS by platform default), `"hvc64"` (aarch64 EL2 HVC), `"smc64"`, `"sysv64"`, `"win64"`, `"ms_abi"`, `"aapcs"`, `"riscv"`, `"interrupt"` (no ABI; bare register frame). Each conv is a record of *which registers carry which arguments*, *what the callee may clobber*, and *what the return path looks like*. The set is fixed by the target description, not by user code — a new conv requires a target patch.

```vestra
@extern("hvc64")
func psciCpuOn(_ funcId: UInt64, _ a1: UInt64, _ a2: UInt64, _ a3: UInt64) -> UInt64 using Extern
```

`vestra audit --extern` enumerates every `@extern` with its conv; a misuse is `--extern --strict`'s job to find.

### 14.6.2 `@symbol` on an extern

A foreign symbol that isn't a valid Vestra identifier (a C `__stack_chk_guard`, a `.S` label `aarch64_kernel_wfi`) is named with `@symbol`:

```vestra
@extern("C") @symbol("__stack_chk_guard")
static stackChkGuard: UInt64
```

This replaces the `@_silgen_name` ceremony wholesale: one attribute, target-explicit name, no mangling.

### 14.6.3 Address of a linker symbol

`&decl` is the address-of operator on a `static` (initialized, `@noinit`, or `@extern`), a `func`, or a vector-table entry. It yields `Ptr[T]?` for a `static T`, `(...) using C -> R` for a `func`, and is `Trivial`. It does **not** require `RawMemory` — taking the address of a known symbol is a link-time fact, not an unsafe read. *Dereferencing* the result is gated on `RawMemory` as ever.

```vestra
@extern("C") @symbol("_kimage_start")
static _kimage_start: UInt8

let imageBase: UInt64 = UInt64(bitPattern: &_kimage_start)
```

The dozens of `@_silgen_name("vectra_X_addr")` C wrappers in `freestanding.c` collapse to a single `&decl` per symbol.

---

## §14.7 (new) — Bare-metal entry: the no-runtime sub-dialect

A kernel's first ~200 instructions run before MMU, without a stack pointer the rest of the program agrees on, and (on aarch64) at physical addresses the linker named as virtual. Tier V's "`using` capabilities pass as hidden pointer parameters" (§15) **cannot** run here — there is no stack, no callee-save discipline, no relocation. Draft 3.3 names this regime and bounds it.

A function attributed `@boot` is a **boot-mode function** — Vestra in surface, but with the runtime model of assembly:

```vestra
@section(".text.boot") @boot @naked @symbol("_start")
func _start() {
    with Asm {
        asm(volatile: true) {
            "adr x0, _bss_start"
            "adr x1, _bss_end"
            "1: cmp x0, x1"
            "b.ge 2f"
            "stp xzr, xzr, [x0], #16"
            "b 1b"
            "2:"
            "adr x30, kernelMain"
            "br  x30"
        }
    }
}
```

`@boot` imposes seven restrictions, every one of them a property the boot regime lacks:

1. **No `using` row.** The function may not declare nor discharge any capability except `Asm`, `RawMemory`, and `Mmio` — the three the language has *no* compiled support for anyway.
2. **No `Alloc`, no `Async`, no `Extern` outside other `@boot`.** No call into ordinary Vestra code; transitively, no `print`, no allocation, no `String`.
3. **`@naked` is implied unless overridden.** No prologue/epilogue is emitted.
4. **No throws, no Result-return.** All errors are values written to a known register or memory cell.
5. **No `Trivial` arguments beyond a fixed register set.** Calls between `@boot` functions pass arguments in registers named by the conv; the compiler verifies no spill is required.
6. **No reference to a non-`@boot` declaration except by `&decl`.** Taking an address is fine; *calling* a non-`@boot` function is rejected. Crossing the boundary is one explicit jump.
7. **No comptime allocation in the body.** A `comptime` block may run, but its result must be `Trivial`.

`@boot` is therefore a **subset** of Vestra — every well-formed `@boot` function is a well-formed Vestra function, but the converse fails. The escape from boot mode is a single `br`/`jmp` (in inline asm) to a non-`@boot` function the `&` operator named; from that point the kernel runs under the ordinary runtime model.

The `@boot` regime is not a second language — it is the ordinary language with seven additional rejection rules. A developer reading a `@boot` function still answers §2's six questions locally; the answers are simply *constrained* by which features the boot regime admits.

---

## §14.8 (new) — Per-CPU storage

A kernel needs per-hart slots — runqueue head, idle flag, current task pointer — with cache-line padding to avoid false sharing. Vestra adds one library type and one supporting attribute:

```vestra
struct PerCpu[T: Trivial]            // (compiler-known)
    static func new(_ value: T) using Alloc -> Box[PerCpu[T]]
    inout func mine() -> inout T     // returns the current hart's slot
    func mine() -> T                  // read-only access

    // Cross-hart access is explicit and gated on RawMemory.
    func slot(_ hartId: HartId) using RawMemory -> Ptr[T]
}
```

`PerCpu[T]` is laid out as `[MAX_HARTS]Padded[T]`, where `Padded[T]` is a compiler-known wrapper that pads `T` to a full cache line (`@repr(align(cfg.option("cache_line_bytes")))`). The current-hart index is read from a target-specific register the compiler emits (`tpidr_el1`, `gs:[0]`, …); the developer never spells it.

`PerCpu` is the kernel's per-hart vocabulary, and it composes with `Atomic[T]`: a `PerCpu[Atomic[UInt64]]` is the kernel's per-hart counter pattern, with no false sharing and no extra cost beyond the indexing.

---

## §15.4 (new) — Compiler-emitted intrinsics

Three families of operation the compiler implicitly emits — `memcpy`/`memset`/`memmove` for struct copies and zero-init, stack-canary instrumentation, and unsigned overflow traps — currently land in `freestanding.c` because Embedded Swift expected `libc`. Vestra fixes this by **emitting them itself** from a fixed intrinsic table, parameterized by target.

`core.systems.intrinsics` declares the family:

```vestra
@symbol("memcpy")  @inline(.never) func memcpy(_ dst: MutPtr[UInt8], _ src: Ptr[UInt8], _ n: Int) using RawMemory
@symbol("memset")  @inline(.never) func memset(_ dst: MutPtr[UInt8], _ v: UInt8, _ n: Int) using RawMemory
@symbol("memmove") @inline(.never) func memmove(_ dst: MutPtr[UInt8], _ src: Ptr[UInt8], _ n: Int) using RawMemory
```

— and the compiler links its implicit emissions against *these* symbols. A freestanding profile ships them in Vestra (a 30-line byte loop each is fine; the autovectorizer takes it from there); a hosted profile inherits the platform `libc` versions via `@weak`. **No C source is required to build a freestanding Vestra program.**

**Stack canaries** are similarly target-knobbed. `@stack_protector(.none | .strong | .all)` on a function or module selects the instrumentation level, and the compiler emits references to two well-known symbols — `__stack_chk_guard` and `__stack_chk_fail` — that the kernel supplies in Vestra under `@symbol`. The kernel's current 6-line C stubs become 6 lines of Vestra with no change in shape.

**Overflow traps** in debug builds (§4) call `__vestra_overflow_trap`, again supplied in the standard library and overridable.

---

## §16 — Deliberate omissions (amended)

Three new omissions, each named for the same reason as the existing ones — to mark the line and explain why it sits there.

- **No general-purpose preemptable context switching primitive.** Task switching is a kernel concern. The language supplies `@interrupt`, `@boot`, vector tables, per-CPU storage, and `linear` handles; the *scheduler* is ordinary kernel code on top.
- **No generation-counted handles in the language.** Use-after-revoke is a runtime check the kernel performs against an explicit `generation` field stored next to each capability entry. `linear opaque type` catches *dropped* handles at compile time; *stale* handles are a runtime concern, library-encoded as `Generational[T]`. The language refuses to grow a second affinity story for this.
- **No untyped-pointer arithmetic on `Ptr[T]`.** `p + n` advances by `T`-sized strides, never bytes. Byte arithmetic is `Ptr[UInt8]`, explicitly.

---

## §17.X — Grammar additions

The grammar grows by six tokens and four production additions.

**New keywords**: `static` already exists. **New attribute words** (none reserved, all parsed under `@IDENT`): `bss`, `noinit`, `section`, `symbol`, `weak`, `alias`, `visibility`, `boot`, `naked`, `interrupt`, `inline`, `padding`, `stack_protector`. None are identifiers; an attribute lives in attribute position only.

**Productions added**:

```ebnf
attribute  ::= '@' IDENT ('(' args? ')')?                  -- unchanged, only the IDENT set grows
declaration ::= … | '@noinit' static-decl                  -- the uninitialized form
type        ::= … | '@interrupt' '(' type ')'              -- handler type, frame-parameterized
expr        ::= … | '&' (IDENT | dotted)                   -- address-of-symbol
struct      ::= … '@repr(union)' struct-body               -- untagged union
```

The address-of-symbol operator binds at the same precedence as `copy` (prefix, tightest). Operator `&` retains its *call-site* meaning marking an `inout` argument (§17.5 `arg`); the two uses disambiguate by position — `&decl` is an expression, `f(&x)` is an argument marker — and the parser distinguishes them on the LHS of the comma rule.

---

## §18.X — Standard library additions

Six new compiler-known or library types:

```vestra
struct PerCpu[T: Trivial]               // §14.8
struct Padded[T]                         // cache-line-padded wrapper
struct Generational[T]                   // generation-counted handle wrapper (library)

extension MutSpan {
    static func raw(at: MutPtr[T], count: Int) -> MutSpan[T] using RawMemory   // §10.5
    func withFlexibleArray[U](count: Int) -> MutSpan[U] using RawMemory        // §6.8 FAM
}

// core.systems.intrinsics, core.systems.trap — §14.5, §15.4
```

---

## Draft 3.3 changelog

Draft 3.3 adds, in order of the chapter they touch:

- §4.5 — `@noinit static` for uninitialized .bss storage, gated on `Trivial` and `RawMemory`.
- §6.7 — `@section`, `@symbol`, `@weak`, `@alias`, `@visibility` on declarations.
- §6.8 — `@repr(union)`, explicit `_: [N]UInt8 @padding` fields, anonymous `embed`, flexible array members, and `Type.size` / `Type.alignment` / `Field.offset|size|alignment` in reflection.
- §7.8 — `@inline(.always | .never | .hint)` as a principled inlining lever.
- §10.5 — `Span.raw(at:count:)` / `MutSpan.raw(at:count:)`, with lifetime anchored to the `RawMemory` discharge.
- §14.4 — `asm` block clobber discipline tightened; the `@extern("C")` audit names every conv.
- §14.5 — `@interrupt` rewritten: a frame-typed handler, vector-table arrays, compiler-emitted prologue/epilogue. The `.S` entry trampolines for normal exception classes go away.
- §14.6 — pluggable `@extern("conv")` (C / hvc64 / smc64 / sysv64 / win64 / aapcs / riscv / interrupt), `@symbol("name")` on externs, `&decl` for address-of-symbol.
- §14.7 — `@boot` as a seven-rule subset of Vestra for pre-MMU code; the boundary between `@boot` and ordinary Vestra is one named jump.
- §14.8 — `PerCpu[T]` and the cache-line-padded layout it rests on.
- §15.4 — `memcpy`/`memset`/`memmove`, stack canaries, and overflow traps are now compiler-emitted with Vestra-supplied bodies, parameterized by target.

**The acceptance criterion for Draft 3.3 is that `freestanding.c` is empty.** Every function it currently provides — symbol-address getters, fixed-section buffer reservations, intrinsics, stack-canary stubs, MMIO escape hatches — has a Vestra spelling in this draft. The .S files remain, with reduced scope (the exception-entry trampolines for synchronous / IRQ / FIQ / SError on aarch64 collapse into `@interrupt`-emitted prologues; what remains is pre-MMU bring-up, page-table activation, and instructions Vestra's `asm` cannot inline). A working systems programmer still reads §1–§13 in an afternoon; §14 remains the chapter they study when they touch the metal, but that chapter no longer mentions a foreign source language.

---

## Open questions for Draft 3.4

Two pieces of §14 are intentionally underspecified in 3.3 and need a follow-up pass before commit:

1. **Vector-table lowering.** §14.5.3 declares a typed `[N]@interrupt(F)` array placed by `@section`, but the codegen has to emit ISA-specific stubs at each slot — aarch64 vec_table entries are 32-instruction-aligned chunks; x86 IDT entries are 16-byte descriptors with offset/selector/type/dpl. A 3.4 amendment pins the lowering rule (how `@interrupt`'s prologue maps onto each slot) before the form is committed.

2. **C++-style template-heavy advanced layouts.** Variadic struct expansion, layout-dependent template specialization, and the `std::variant` discriminator-elision tricks are mostly reachable through §12.4 declaration macros — but the kernel hasn't hit those patterns yet, so the spec hasn't been forced to think them through. When VectraOS userland starts wanting `std::span`-equivalent generic algebra over layout (and the inevitable `Matrix.tiles().par_map(...)` shapes), §6 and §12 will need another pass.
