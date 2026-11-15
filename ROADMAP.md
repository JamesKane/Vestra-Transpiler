# Roadmap

The path forward for the Vestra compiler. It lists what is left to do and in
what order, and nothing about what is already done: `git log` is the record of
shipped work, and an item is deleted from this file in the same commit that
finishes it.

Companion documents:

- [`VESTRA_DRAFT.md`](VESTRA_DRAFT.md) and
  [`VESTRA_ANNEX_A.md`](VESTRA_ANNEX_A.md): the language definition.
- [`SPEC_GAPS.md`](SPEC_GAPS.md): every place the compiler falls short of
  that definition, with the probe that showed it. This roadmap orders that
  list; it does not repeat its evidence. Section references below (for
  example "Gaps §5") point into it.
- [`HANDOFF.md`](HANDOFF.md): orientation for the self-hosting work and how
  a session proceeds.

## Where things stand

- **Bootstrap compiler** (`vestra`, C++26, `src/` and `include/`): parses,
  checks and transpiles the whole working language to C++26, hosted or
  freestanding (`--no-libc`, with aarch64 / x86_64 / riscv64 lowerings). It
  boots a bare-metal aarch64 kernel (the Vestra-POC spike: GICv3, EL0
  domains, IPC capabilities).
- **Self-hosted compiler** (`selfhost/`, written in Vestra and built by the
  bootstrap): the lexer and parser are byte-identical to the bootstrap's; the
  sema covers resolution, capabilities, exclusivity, ownership including the
  linear rules, and a subset of type checking; codegen produces byte-identical
  C++ for 136 of 150 corpus files and for every spike module without inline
  asm.
- **The C++ backend is the ceiling.** The spec names LLVM for v1, and a
  family of guarantees cannot be kept while Vestra lowers to C++ (Gaps,
  "The backend is C++26"): defined overflow, left-to-right evaluation, the
  FP contract, the Tier V calling convention and edition-stamped symbols,
  niche-optimized `Ptr[T]?`, and reproducible output independent of the host
  C++ compiler.

## The next phase: lower to LLVM IR

The next phase replaces the C++ transpile step with direct lowering to LLVM
IR. Vestra stops inheriting C++ semantics and gets the code generation its
spec assumes.

### Shape

1. **A typed, lowered IR inside the compiler.** Today codegen walks the AST
   and the sema side tables and prints C++ text. LLVM wants explicit control
   flow, explicit temporaries and explicit types. The first step is a small
   mid-level IR between sema and the backend: basic blocks, SSA-free
   locals, explicit moves and drops, evaluation order fixed left to right,
   `try` / `match` / `if let` desugared to branches. This is also where the
   ownership facts sema already computes (moves, linear consumption,
   `deinit` points) become drop instructions.
2. **Emit textual LLVM IR** (`.ll`) from that IR and hand it to `llc` /
   `clang` for object code. Textual IR keeps the compiler free of the LLVM
   C++ API, works the same from the bootstrap and from the self-hosted
   compiler, and is easy to diff in tests.
3. **Bring the language over in order of dependency:**
   1. Scalars, arithmetic, control flow, functions, the C ABI (`@extern`,
      `@repr(C)`).
   2. Structs, enums with payloads (tagged unions), `T?` with the pointer
      niche, `throws` as a result pair, `match`.
   3. Ownership: moves, drops, `deinit`, `Box`, `Vec`, `String`, `Span`.
   4. The runtime: panic, the hosted entry point, the freestanding profile,
      and the Annex A intrinsics as LLVM intrinsics or inline asm
      (`Sysreg`, barriers, cache and TLB ops, `Atomic[T]` with real
      orderings, `swapContext`, vector tables, `@section` / `@align` /
      `@symbol`).
   5. Generics by monomorphization, closures, `async` / `spawn` /
      `Channel` / `parallel`.
4. **Retire the C++ backend** once the spike kernel and the example corpus
   build and run through LLVM. Until then both backends stay green.

### Testing once byte-identity no longer applies

The current oracle is byte-identical C++ between the two compilers. That stays
for the front end (tokens, AST dumps, diagnostics), but LLVM output has no
reference to match. The backend is held instead to:

- **Behavioural differential:** each example and e2e program runs under both
  backends and must produce the same output and exit status. The C++ backend
  is the reference until it is retired.
- **IR snapshot tests** for the lowering rules the spec pins (overflow traps,
  evaluation order, `&+` wrapping, vector-table layout, atomic orderings).
- **The spike kernel** booting on QEMU, as now (`tools/run.py`,
  `tools/test.py`, `tools/flaws.py`).

### Gaps the LLVM backend owns

These are listed in SPEC_GAPS as unsound or weaker, and every one comes from
lowering to C++. They are fixed by the new backend, not patched in the C++
emitter, unless a patch is a few lines and closes an unsoundness the spike can
hit:

- Integer overflow traps in debug; `&+ &- &*` wrap; division by zero traps;
  integer literals range-checked (Gaps §4).
- Bounds-checked `[N]T` and `Span` indexing (Gaps §19.10).
- Left-to-right evaluation of arguments and operands (Gaps §19.9).
- `??` evaluates its right side lazily (Gaps §9). This one is a cheap fix
  in the C++ emitter too, and should not wait.
- `Ptr[T]?` is one word (Gaps §9).
- The `@deterministic` FP contract (Gaps §13).
- Tier V ABI: capabilities as parameters, editions, the link-time check
  (Gaps §15); lowered calling conventions such as `hvc64` / `win64`
  (Annex §14.6.1).
- Explicit SIMD lowering of `[N]T` rather than reliance on the
  auto-vectorizer (Gaps §13).
- `as` byte reinterpretation, which needs layout knowledge the C++ path
  never had (Gaps §4).

## Before and alongside the backend: front-end soundness

These gaps are in sema and do not depend on the backend. They should be closed
first, in both front ends (bootstrap and `selfhost/sema.vst`, held to
identical diagnostics), because the new backend will trust whatever sema
accepts. Ordered by how likely each is to let a wrong program through:

1. **Smaller unsound items:**
    - `Atomic[T]` requires a §4 primitive (the bootstrap's `is_primitive`
      also admits `String`, `Str` and `Unit`, and the self-hosted checker
      has no `T` rule at all);
    - `Scheduler.swapContext` takes the spec's `MutPtr[Context]` /
      `Ptr[Context]` (it still takes `Context` values, which is why a
      `@noinit` `Context` stays in scope as a value).

    (Annex A table)

## Self-hosting

The self-hosted compiler moves to the LLVM path with the bootstrap; it does
not keep chasing the C++ output.

- **Sema breadth.** Over `examples/`, `vsema` still reports about 470 lines
  that `vestra check` does not: built-in types it doesn't know, string `+`,
  and other inference gaps. Closing these is the main front-end work, and
  it's the precondition for the self-hosted compiler checking real programs
  on its own.
- **Literal typing.** `vsema` types a number literal as `Int` and patches
  that only where a value meets a declared type. A real expected-type pass,
  as the bootstrap has, removes a class of false positives.
- **Codegen to C++.** The corpus burndown (136/150) stops here. vgen keeps
  passing its differentials so it stays usable as a second front-end check,
  but new lowering work goes into the IR and LLVM backend.
- **Imports.** vgen and vsema find imported modules only in the entry file's
  directory; they need `-I` search roots, as the driver has.
- **Inline asm** stays bootstrap-only until the LLVM backend, where it becomes
  LLVM inline asm in one place for both compilers.
- **The self-hosting milestone:** the self-hosted compiler, built by the
  bootstrap, compiles itself to a working binary through the LLVM backend,
  once that backend is ported from the bootstrap.

## Language features still missing

Roughly in order of how much existing or kernel code wants them (details in
SPEC_GAPS):

1. **`deinit`:** checked and emitted, with its no-capability rule and
   reverse-order destruction. This lands naturally with drops in the new IR
   (Gaps §6).
2. **Protocols for real:**
   - conformance checking, and protocol methods on a bounded `T`;
   - extensions on primitive types;
   - associated types, `some P`, `dyn P`;
   - operator overloading via `static func +`;
   - `@retroactive`.

   (Gaps §7)
3. **Functions:** default arguments; explicit type arguments on calls; const
   generics as values and in array lengths (`[N + 1]T`) (Gaps §7, §17).
4. **Errors and optionals:** `.none`; several `catch` clauses and pattern
   catches (Gaps §9).
2. **Comptime and reflection:**
   - `comptime if` / `comptime match`;
   - `Type` as a value and `Type.structOf`;
   - reflection of enum cases, signatures and attributes;
   - protocol `comptime` defaults;
   - macro hygiene, the builder API, `error(at:)`, statement quotes.

   (Gaps §12)
6. **Patterns:** slice patterns and struct patterns; a bare identifier that
   matches an existing value, `case k:` (Gaps §6, §17.6).
7. **Structs:** `embed` construction and promotion; `@repr(wire)` byte order;
   `@padding`, anonymous `embed _:`, flexible array members (Gaps §6, Annex
   §6.8).
8. **Lexical:** `'\u{...}'` escapes, `b'x'` byte chars, Unicode identifiers
   (Gaps §3).
9. **Conversions:** `x.to[T]()` / `x.toWrapping[T]()`, `addingChecked` and
   friends, `Enum.from(raw)` (Gaps §4).
10. **Modules and packages:** a package manifest, `package` / `internal` /
    `private` visibility, the panic-behaviour setting (Gaps §15, §9).
11. **Concurrency:** a real multi-threaded runtime, bounded channels, `send`
    by `sink`, `select` send arms, `parallel` as the spec's library function
    (Gaps §11).
12. **SIMD:** `%`, bitwise ops and shifts, scalar broadcast, masks, swizzles,
    reductions, `shuffle` (Gaps §13).
13. **C interop:** `import c` synthesizing declarations, with `T*` as
    `Ptr[T]?` (Gaps §14.6).

## Standard library (§18)

Missing entirely, in rough order of need:

- **Containers:** `Buffer` (with `Buffer(fixed:)` and `chunks`), `Arc`,
  `StridedSpan`, `HashSet`, `TreeMap`, `BitSet`.
- **Sync:** `Mutex`, `RwLock`.
- **Allocators:** `Arena`, `ObjectPool`, `FixedAllocator`, and drops through
  the allocator that built the value.
- **Iteration:** the combinators as `Iterator` methods, plus `skip`,
  `enumerate` and `chain`; `Range` composing with them; a borrowing
  `Str.chars()`.
- **Text:** `String.push(Char)`, `asStr()`; slicing a `StrConst` yields a
  `StrConst`.
- **Protocols as protocols:** `Eq`, `Hash`, `Display` and the rest, not only
  `derive` names; `derive(Ord)` with `compare()`.
- **Spelled-out types:** `Optional[T]`, `Result[T, E]`.
- **Numerics:** `core.math`, `Matrix`, `Quat`, and math over `Float32`.

## Annex A (bare-metal) deepening

Most of this is cheaper to do once, in the LLVM backend, than twice:

- **`with InterruptsOff`** masks on entry and restores on every exit (today a
  bare block).
- **`PerCpu[T]`:** one slot per hart, read through `tpidr_el1` / `gs` / `tp`.
- **Vector tables:**
  - the full §14.5.3 rule: in-slot bodies, `.text.vec_table.<i>`
    sub-sections, FP/SIMD save;
  - x86_64 IDT and RISC-V `stvec` forms;
  - typed trap frames from `core.systems.trap`;
  - `@interrupt(class:, fromEL1:)`.
- **Context switch:** `FpContext` with lazy FP save, `Context.size` by
  reflection.
- **`@boot`:** Vestra statements in the body under the seven rules, a checked
  hand-off to the kernel entry, and the spilled-argument rule.
- **`@kernel_init`:** call-graph reachability and `Alloc`-init ordering.
- **Inline asm:** the implicit-clobber rule (an instruction writing an
  unnamed register is an error), which needs a per-ISA instruction model.
- **Statement-level escapes:** `@no_auto_barrier` and `@allow_weak_cas` on a
  statement; the `@retry_loop` predicate.
- **Atomics:** default orderings, `fetchSet` / `fetchClear` on
  `Atomic[Bool]`, the `+lse` / `+cx16` gates checked at link time.
- **Cache and barriers:** `cleanDataRounding` / `invalidateDataRounding`,
  `prefetch`, compile-time cache-range alignment, the `Cpu.` / `Cache.` /
  `Tlb.` namespaces, RISC-V `SFENCE.VMA` after `satp`.
- **Registers:** a privilege column in the table and a link-time privilege
  check; `currentEL` spelling.
- **`vestra audit`:**
  - the ten missing modes;
  - `--sysreg` coverage of loops, matches and methods;
  - `--no-libc` listing the symbols the build actually resolved.
- **Freestanding guarantees:**
  - exactly one `@panic_handler` per binary;
  - the handler receives file and line;
  - Vestra-supplied `memcpy` bodies required;
  - `@stack_protector` modes;
  - undeclared externals rejected.
- **Not yet verified:** freestanding output linking on all three targets, and
  the aarch64 vector stub under nested exceptions (SPEC_GAPS, "Not
  determined").

## Decisions needed

These block or shape work above and are the user's to make:

Decided (least prework first):

- **The LLVM backend is written in the bootstrap (C++).** It needs no
  self-hosted sema breadth first. The self-hosted compiler reaches LLVM by
  porting the backend once the bootstrap's is proven.
- **Textual `.ll`, not the LLVM C API.** No new library dependency; revisit
  if compile times or debug-info emission demand the API.

Still open:

1. **Spec holes** (SPEC_GAPS, "Spec holes"):
   - `Ordering` names both the comparison result and the atomic memory
     order;
   - `Hash` vs `Hashable`;
   - the `type Element = ...` production;
   - whether `internal` is module- or package-scoped.
2. **Questions the spike raised** (Vestra-POC `FINDINGS.md`):
   - enum raw values, `[x; n]` repeat literals, `case a, b`;
   - `&` on an element of a static array;
   - a pointer accessor on `Span` (for a syscall's `(ptr, len)`);
   - system registers by encoding;
   - whether `@symbol` implies the C calling convention (the Annex
     `memcpy` example);
   - whether a dropped non-Unit result is an error or the generated
     `[[nodiscard]]` goes.

## Working with this file

- Each round, the next phase is picked from the top of the earliest
  section that isn't blocked.
- Items too large for one session ship as self-contained slices; the item
  stays here until its last slice lands.
- Finishing an item deletes it here and, if SPEC_GAPS lists it, there too,
  in the same commit. Nothing moves to an "already shipped" section; `git
  log` holds that.
