# Handoff

Session-continuity notes for the Vestra-to-C++26 transpiler. The
spec lives in [`VESTRA_DRAFT.md`](VESTRA_DRAFT.md); the implementation
strategy and high-level "what works" are in [`README.md`](README.md);
the forward-looking phase menu is in [`ROADMAP.md`](ROADMAP.md). This
file is the *back-looking* counterpart — what shipped, how the work
proceeds, and the rough edges to watch out for.

## Current state

- **705 / 705 tests pass** (`ctest --test-dir build/debug`), Debug
  build with ASan + UBSan via the `asan` preset, format-check clean.
- Tip commit: `655dbb2 parse+sema+codegen: §17.4 with-binding type annotation`.
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
| (this branch) | **§9 `.mapError` closure-literal context propagation** — closes a usability gap on the existing `.mapError(_ f)` surface. Both sema and codegen for `.mapError` were already shipped (the function-arg form `parseInt(x).mapError(toAppErr)` worked end-to-end), but the closure-literal form `parseInt(x).mapError({ pe => ... })` errored with "closure type cannot be inferred without context" because the .mapError intercept didn't push the expected Function type down to the closure-arg check. One-line fix in check_call: build an expected `(E) -> ?` Function from the base Result's error type and pass it as the `expected` parameter to check_expr on the closure argument. The closure's single param then adopts E from `expected->parts()[0]`; its body's last expression types E' (the new error); the result is `Result[T, E']` per the existing .mapError logic. Named-function arguments are unaffected — they ignore the expected hint and their types are checked against the `(E) -> E'` shape just like before. The example's new `parseInlineWrap` function shows the inline-mapping shape with a match-over-enum body; the e2e adds three round-trip checks confirming the closure-literal path produces the same error widening as the named-function path. |
| `655dbb2` | **§17.4 with-binding type annotation** — extends the existing `with NAME = EXPR { ... }` value-binding form with an optional type annotation: `with NAME: T = EXPR { ... }`. The existing surface (parser, sema, capability, codegen) already handled all three §17.4 shapes — cap-only (`with TYPE { ... }`), cap-with-value (`with TYPE = EXPR { ... }`), and name-binding (`with name = EXPR { ... }`) — but the user couldn't pin the binding's declared type at the source level; the C++ side defaulted to `auto&&` and inferred from the RHS. Parser: extends the WithBinding shape disambiguator (lowercase-leading ident followed by `=` *or* `:` ⇒ name-binding) and accepts the type expression between `:` and `=`. AST: `WithBinding` gains a `TypePtr type_annotation` slot. Sema: when the annotation is present, push T down as the value-check's expected type so an integer literal adopts the annotated width; then assignability-check the result and reject mismatches with a typed diagnostic (`"with-binding value of type StrConst does not match annotation Int32"`). The binding's declared symbol type uses the annotation when present, the inferred value type otherwise. Codegen: emits the annotation type via `emit_type_with_name` (which handles the function-pointer-declarator quirk from §A12) instead of `auto&&`. The existing capability checker's WithStmt handler is untouched — annotations don't change the cap-discharge contract. The example's `annotated()` shape covers integer-width pinning across nested bindings, and the e2e verifies the runtime behavior. v0.5 doesn't yet allow annotations on the cap-with-value shape (`with TYPE = NAME: T = EXPR`); the spec syntax there would need a parser disambiguator that the current single-line shape doesn't support. |
| `9e26cc2` | **§14.12 third slice: read-only / write-only sysreg handles** — tightens §14.12.1 by splitting the single SysregHandle (now the RW form) into three TypeKinds: `SysregHandle` (read-write), `SysregHandleRO` (read-only), `SysregHandleWO` (write-only). The v0.5 target-description table maps each canonical name to its kind: midr_el1 + cntfrq_el0 are RO (CPU model ID + system counter freq, no architectural write path); daif / sctlr_el1 / vbar_el1 / ttbr0_el1 are RW. The WO set is empty in the canonical subset but the TypeKind is reachable for the wider name set the kernel target's build extends to. The resolver's Sysreg.NAME intercept picks the right kind from the table; lookup_method admits `.read()` only on RW + RO and `.write(T)` only on RW + WO, so `Sysreg.midr_el1.write(0)` now rejects with "no field or method 'write' on type ReadOnlySysreg[UInt64]" instead of running an undefined operation. The capability checker extends its Asm gate to all three kinds. Codegen emission stays uniform — the runtime `Handle<T>` template carries both methods structurally so the sema-time gate is what enforces the rule; lowering `Sysreg.midr_el1` to `__vstr::sysreg::midr_el1` works the same regardless of kind. The describe() format names each kind (`ReadOnlySysreg[T]` / `WriteOnlySysreg[T]` / `ReadWriteSysreg[T]`) so the diagnostics surface tells the user which kind they hit. Three sema tests cover the three paths: write-on-RO rejection, read-on-RO acceptance, RW admits both. §14.12 is now spec-complete at the type-system level. |
| `12882e7` | **§14.12 second slice: auto-emitted post-write barriers** — activates the `@no_auto_barrier` attribute that's been parked since the §A9 / §A6 slices. The runtime preamble adds `__vstr::sysreg::post_write_barrier()` — `__asm__ volatile("isb" ::: "memory")` on aarch64, `std::atomic_thread_fence(seq_cst)` everywhere else — plus per-gated-sysreg wrappers `write_sctlr_el1` / `write_vbar_el1` / `write_ttbr0_el1` that pair the cell update with the barrier call. The aarch64 target table gates writes to SCTLR_EL1, VBAR_EL1, TTBR0_EL1 (the control-register set whose retire affects fetch / decode state); DAIF / MIDR_EL1 / CNTFRQ_EL0 stay out — DAIF's mask is a synchronously-checked bit, MIDR/CNTFRQ are read-mostly identification registers. CppEmitter gains a `current_no_auto_barrier_` flag set per-function in emit_func (with a RAII restore for nested funcs), letting the per-call-site lowering see whether the enclosing function carries `@no_auto_barrier`. emit_expr's CallExpr branch grows a new intercept matching the `MemberExpr(MemberExpr(IdentExpr("Sysreg"), name), "write")` shape: when the inner name is in the gated set and the enclosing function isn't carrying `@no_auto_barrier`, route to `__vstr::sysreg::write_<name>(v)` instead of `__vstr::sysreg::<name>.write(v)`. Codegen tests cover all three paths: gated write outside opt-out (routes to wrapper), gated write inside `@no_auto_barrier` (stays raw), non-gated write (stays raw regardless). The §14.12 surface now matches the spec's contract end-to-end: `vestra audit --sysreg` (queued for the audit-surface slice) will be able to enumerate every auto-emission and verify the target table against the architecture manuals. The read-only / write-only handle distinction from §14.12.1 and the per-target name set extension queue for the third §14.12 slice. |
| `359efab` | **§14.12 first slice: typed Sysreg access** — opens the typed system-register surface that `@no_auto_barrier` was anchoring against. New TypeKind::SysregHandle wrapping inner T (always UInt64 in v0.5); `make_sysreg_handle` factory + describe/equal/substitute branches all parallel the existing per-builtin pattern. Resolver's `check_member` recognizes the `Sysreg.<name>` shape: when the base ident is "Sysreg" and no Sysreg symbol is in scope, validate <name> against a canonical aarch64 EL1 subset (`midr_el1`, `daif`, `sctlr_el1`, `vbar_el1`, `ttbr0_el1`, `cntfrq_el0`) and return a SysregHandle<UInt64>. Unknown names reject with a clear diagnostic naming the admitted set. `lookup_method` admits `.read() -> T` and `.write(T) -> Unit`; the capability checker detects the syntactic shape `<SysregHandle>.read()` / `.write(v)` and discharges the Asm capability at the call site. Codegen extends the `__vstr` namespace with a nested `sysreg` sub-namespace containing a `Handle<T>` template (read/write methods over a `cell` field) plus one `inline Handle<std::uint64_t>` instance per canonical name; v0.5 hosted backs each register with a static cell so the e2e can verify round-trips without privileged sysreg access. The kernel target replaces the cell-backed Handle with one inline-asm `mrs` / `msr` per call (aarch64), `rdmsr` / `wrmsr` (x86), or `csrr` / `csrw` (RISC-V). User-facing `Sysreg.<name>` lowers to `__vstr::sysreg::<name>`; `.read()` / `.write(v)` are then ordinary method calls on the template. The e2e drives daif / vbar_el1 / sctlr_el1 / ttbr0_el1 through writes and read-back checks, plus exercises the `@kernel_init @no_auto_barrier` MMU bring-up shape end-to-end now that both halves exist. Auto-emitted post-write barriers from §14.12.3 (the thing `@no_auto_barrier` opts out of) queue for the second slice; the per-target name set extension queues for the target-description hardening pass §14.12 calls out. |
| `5a905fe` | **§A6 last: `@no_auto_barrier`** — closes §A6 by adding the barrier opt-out from §14.11.5 / §14.12.3. The attribute disables the compiler's auto-emitted post-write barrier (an ISB on aarch64 sysreg writes, DSB on device-MMIO writes, SFENCE.VMA on RISC-V) for the kernel-init MMU bring-up batch where the final write in the sequence is itself a context-establishing instruction whose retire subsumes every prior barrier. Sema admits the attribute only on functions also attributed `@kernel_init` — that's the boot-path subset where the pattern is admissible per spec. The check_func pass records `is_kernel_init` from the @kernel_init validator pass and then walks attributes a second time; a `@no_auto_barrier` without the partner attribute rejects with a diagnostic pointing at the @kernel_init-only rule. v0.5 codegen accepts the attribute silently — the Sysreg auto-barrier insertion machinery itself queues for a later slice (the typed Sysreg namespace from §14.12 isn't shipped), so the attribute is a contract marker the audit will read once the auto-emission table lands. The sema-side gate today records the boot-path-only constraint so misuse fails at compile time, not at audit time when it's harder to fix. The e2e exercises the canonical "two-sysreg MMU bring-up" function shape (the body returns `ttbr0 ^ sctlr` as a stand-in for the real Sysreg writes); the sema unit tests cover @kernel_init pairing, bare rejection, and the @boot-without-@kernel_init rejection. |
| `fc0512b` | **§A10 follow-up: `@stack_protector` + `--no-libc`** — closes §15.5's freestanding pieces alongside the memcpy/memset/memmove intrinsics from §A10's earlier slice. `@stack_protector(.none / .strong / .all)` is a per-function attribute that picks canary instrumentation; sema validates the leading-dot case-name argument against the three legal spellings (parallel to `@inline` / `@visibility`). Codegen extends LinkAttrs with a `stack_protector` string field and `emit_link_attr_prefix` writes `[[gnu::no_stack_protector]]` for `.none`; `.strong` and `.all` map to the compiler default since per-function "force-on" canary attributes don't portably exist (they're toolchain-wide flags), and the audit names them either way. New `vestra build --no-libc` CLI flag is v0.5's equivalent of the spec's `profile.freestanding.no_libc = true` manifest entry; when set, the emitter writes a `// vestra: no_libc = true` marker at the top of both the generated header and source so downstream link tooling (and the v0.6 `vestra audit --no-libc` enumerator) sees the contract on either artifact alone. The e2e transpiles with `--no-libc`, asserts the marker survived via a separate `grep` test, and runs both stack_protector-decorated functions. The deeper `no_libc` enforcement (refusing to emit references to undeclared external symbols, audit-side enumeration) queues for a build-tooling slice; the marker is the contract anchor downstream tools attach to. |
| `e068f67` | **§8 follow-up: pattern-matching enhancements** — closes three match-arm shapes from the spec that prior slices left partial. (1) Where-guards on enum-payload binds now fire correctly: codegen wraps the arm's body return in `if (guard) { return body; }` and groups multiple arms-per-case into a single constexpr-if branch so the unguarded "catch" arm runs when the guard fails. The grouping is by case_name, source-order-preserving; multiple guarded arms per case work, and an unguarded arm dominates anything after it within a group. If every arm in a group is guarded, the constexpr-if branch terminates with the default arm's body or `std::unreachable()` so C++ doesn't refuse to compile a may-fall-off-lambda path. (2) Tuple patterns with wildcards (`case (1, _)`) already worked via the value-scrutinee path; the e2e verifies first-match-wins ordering and default fall-through end-to-end. (3) Bare enum + guard now switches from a `switch`-IIFE to an if-chain lowering when any arm has a guard OR multiple arms share a case label — `switch` can't fall through cleanly when guards gate cases, so the if-chain is the natural shape. The bare-enum + or-pattern + guard combination folds into the same predicate (`__vstr_m == Tag::a || __vstr_m == Tag::b`). The e2e exercises all three shapes plus the bare-enum guard path; the test suite runs 673 tests across Debug + ASan with format-check clean. The remaining v0.6 polish item flagged in the codegen comment: an unguarded arm in the middle of a payloaded-enum case group dominates subsequent arms in the same group, which is correct but should warn at sema time (dead-code arm). |
| `594662f` | **§A11 follow-on: `Padded[T]` cache-line wrapper** — closes one of the §14.8 loose ends. New `TypeKind::Padded` with `make_padded(inner)` factory; describe / equal / substitute all parallel the existing per-builtin pattern. `resolve_type` recognizes the `Padded[T]` NamedType shape; `lookup_field` admits `.value` on `TypeKind::Padded`, returning the inner T (mirrors Box.value / Ptr.value from prior slices). Codegen extends the runtime preamble with a `__vstr::Padded<T>` template: `T value;` followed by `std::uint8_t _pad[(64 - sizeof(T) % 64) % 64]` and `alignas(64)` on the struct itself. The cache-line width is hardcoded to 64 in v0.5; spec routes this through `cfg.option("cache_line_bytes")`, which queues for the cfg-option slice. `Padded[T]` lowers to `__vstr::Padded<T>` in both emit_sema_type and emit_type. The e2e drives a `@noinit static cell: Padded[UInt64]` round-trip via `.value`, plus a `Padded[Atomic[UInt32]]` counter through three fetchAdds; the load-bearing alignment + sizeof property is enforced via static_assert in the driver so a silent fallback to plain T would fail compile time, not slip through to runtime. PerCpu's internal storage layout still uses `alignas(64)` directly rather than routing through Padded — the spec implies it should, but the swap is functionally equivalent at v0.5 (both produce a 64-byte-aligned single slot) and queues for cleanup alongside the cfg-option work. |
| `e773018` | **§A9 first slice: `@boot` + `@kernel_init`** — two decl-level attributes from §14.7 covering the kernel's stage-1 entry chain. `@boot` is the pre-MMU regime (Vestra surface, assembly runtime model); v0.5 sema enforces three of the seven rules: `using` row restricted to `{Asm, RawMemory, Mmio}` (no Alloc / Async / Extern / Mmio in pre-MMU code), no `throws(E)` (the boot regime has no unwinder), and `@naked` implied via the codegen layer. Codegen extends `LinkAttrs` with a `boot` flag and emits `[[gnu::naked]]` on the function — the C++ compiler skips the standard prologue/epilogue so the body is exactly the instructions the user writes (typically asm + a single `br` to the next stage). `@kernel_init` is the transitional regime (ordinary Vestra surface, single-threaded and pre-scheduler in runtime model); v0.5 sema enforces `using Async` rejection. `using Alloc` is admitted statically (rule 3 — "after the allocator's initialiser returns" — is audit-time). The audit-time rules (call-graph reachability from each entry, ordering of allocator init, ordinary-calls-kernel_init rejection) queue for the call-graph pass. The e2e demonstrates the stage-1 chain shape `@boot func _start → @kernel_init func kernel_init → ordinary func kernel_main`; @boot's body is empty on hosted (real kernels emit asm there) but the `[[gnu::naked]]` attribute survives to link time, and the kernel_init → kernel_main marker round-trip stands in for any subsystem-init side effect. |
| `0e0a548` | **§A3 follow-up: Ptr / MutPtr `.value` dereference** — closes the v0.5 gap that bit the §A11 slot() e2e: Vestra source had no form for reading or writing through a raw pointer, so cross-hart reads had to route through `Span.raw(at:count:1)` or a foreign-function shim. The spelling is `.value`, the same shape `Box[T]` already uses. Sema lookup_field admits the access on Ptr[T] (returns T read-only) and MutPtr[T] (returns T, assignable via `p.value = expr`). Codegen lowers both forms to plain `(*p)` at the C++ layer — `Ptr[T]` emits as `const T*` so the read-only side is enforced by the C++ compiler; `MutPtr[T]` (`T*`) admits the assign-through. Dereferencing itself doesn't require a separate capability discharge — the `.unchecked(fromAddress:)` mint that produced the pointer already discharged `RawMemory`, and the audit lane covering the mint covers the reads / writes alongside it. The e2e drives load_word / store_word / bump_word (RMW) over a stack-resident UInt32 cell, verifying the read / write / read-modify-write path. With this slice the `.slot(hartId).value` cross-hart read pattern from the §A11 e2e is now expressible in Vestra source. |
| `e6883e5` | **§A11 follow-up: PerCpu.new + slot(hartId)** — two extensions that close §14.8 around the existing `.mine()`-on-static-PerCpu shape. `PerCpu.new(value)` is a static-call intercept matching `Box.new`'s pattern; result is `Box[PerCpu[T]]` and T flows from either the value's type or the surrounding `Box[PerCpu[T]]` annotation (so an integer literal at the call site adopts the annotated width). `.slot(hartId: UInt16)` joins `lookup_method` for `TypeKind::PerCpu` next to `.mine()`, returning `Ptr[T]`. v0.5 uses bare UInt16 as the HartId surface type; a proper opaque HartId would land alongside a `core.systems.hart` namespace. Capability gates: `PerCpu.new` joins Box.new's Alloc gate via its syntactic shape, `.slot()` joins the RawMemory audit lane (the cross-hart pointer aliases another hart's storage without the borrow-tracking `.mine()` provides). Codegen: `PerCpu.new(v)` lowers to `std::make_unique<__vstr::PerCpu<T>>(__vstr::PerCpu<T>{v})`; the runtime preamble grows a `slot(hartId)` member on `__vstr::PerCpu<T>` that returns `&value` on hosted (one slot) — the kernel target swaps this for an indexed lookup over the per-hart array region. The e2e drives a `@noinit static counter: PerCpu[UInt64]` through bump/mine() and exercises `PerCpu.new` on the heap; the slot() cross-hart accessor's dereference syntax routes through a higher-level helper that v0.5 doesn't yet ship, so its full round-trip lives in the codegen + capability unit tests. |
| `1e3b812` | **§A10 follow-up: memcpy / memset / memmove intrinsics** — three new free-function builtins from §15.4, each registered in `register_builtin_sync` alongside the existing cache + TLB ops. Signatures match the spec: `memcpy(MutPtr[UInt8], Ptr[UInt8], Int)`, `memset(MutPtr[UInt8], UInt8, Int)`, `memmove(MutPtr[UInt8], Ptr[UInt8], Int)`. All three are gated on the RawMemory capability via the existing `raw_memory_builtins` set in the capability checker (same audit lane as `cleanData` / `MutPtr.unchecked`). Codegen lowers each to its `__builtin_mem*` form so the host compiler resolves the call to an inline expansion or a freestanding-safe runtime entry — never libc — which is the property `no_libc = true` from §15.5 will eventually pin at build time. The trailing length argument widens to `std::size_t` via an explicit `static_cast` so Vestra's `Int` (intptr_t) flows through without a narrowing diagnostic on platforms where the two disagree on signedness. The e2e exercises all three over a stack-resident byte buffer plus an overlapping range to verify `memmove` handles the in-place shift the kernel queue-compact path depends on; the overlap case (dst = ov+1, src = ov, n = 7) would corrupt under plain memcpy. The §15.5 `no_libc` manifest flag itself, stack-canary instrumentation (`__stack_chk_guard` / `__stack_chk_fail`), and the `__vestra_overflow_trap` binding queue for follow-ups. |
| `4747e88` | **§A8 follow-up: typed vector tables** — new ast::InterruptType + TypeKind::InterruptHandler for `@interrupt(T)` in a type position; only `@interrupt` is admitted as a type, every other attribute stays decl-level. The vector-table spelling `[N]@interrupt(T)` from §14.5.3 lowers to `std::array<void(*)(T&), N>` with a brace-init list of bare function names; the function-name-decays-to-pointer rule means `static vt: [4]@interrupt(T) = [a, b, c, d]` works without explicit `&`. Sema gains a VectorLitExpr branch that pushes the expected element type down so a mismatched ISR shape gets caught at the array literal, not deep in C++ ("vector element 0 of type … does not match expected element type …"). Type assignability admits any `(T) -> Unit` function into an `@interrupt(T)` slot; the @interrupt-attribute + inout-param shape rules from §A8's first slice already validate each ISR at its definition site. The e2e dispatches manually through a 4-slot table by index — mimicking what hardware does on a real exception vector entry — and verifies each slot fires its own sentinel ISR plus that the table-base pointer matches `&first_slot`. The per-slot stride alignment (128 bytes on aarch64 EL1) and `.text.vec_table.<index>` sub-section emission are kernel-target concerns; hosted v0.5 drops them since the host platform's section format (mach-o `__SEG,__SECT` vs ELF `.section`) differs and the hosted build doesn't need them to validate dispatch semantics. |
| `6bd6dd8` | **§A12 follow-up: function-pointer type** — `&func` now produces a value of the callee's function-pointer type (was a "not yet supported" error in §A12's first slice). The `(T1, T2) -> R` source spelling lowers to the C++ function-pointer `R(*)(const T1&, const T2&)`; the `const T&` wrapping mirrors func decls' read-mode parameter lowering so `&helper` is ABI-compatible with a `(T) -> R` slot. New `emit_type_with_name` helper handles the C++ function-pointer declarator (`R(*name)(T1)` with the name embedded inside the parens) at every site that declares a variable / parameter / field of that type — let-bindings, function parameters, and (eventually) struct fields and vector-table entries. Function-pointer parameters skip the read-mode `const T&` outer wrap because C++ "ref-to-function-pointer" syntax disagrees with the way emit_type composes the spelling, and function pointers are word-sized so by-value is the natural ABI. The e2e exercises both the let-binding form (`let a: (Int32) -> Int32 = &handler_a`) and the by-parameter form (`apply(handler_b, arg)` — the bare callee decays at the C++ layer; the parser reads `&` in a call-arg position as the inout marker, so the explicit `&` only appears in non-call positions). Unlocks ISR vector arrays in Vestra and tightens the @panic_handler slot. |
| `71ec804` | **§A6 follow-up: `@repr(union)` bit-field composition** — new `is_union` flag on the codegen `LayoutAttrs`; `@repr(union)` swaps the C++ `struct` keyword for `union` and drops the default member initializers (only one allowed per union; the existing call-site designated-init `Status{.raw = x}` covers the active-member init path). Sema rejects `@bits` on a direct union member (bit-fields belong in a packed sub-struct that overlays the integer view) and rejects any `derive(...)` targeting a union struct (untagged overlays have no structural comparison or hash). The struct-construction check now allows a union to be built with exactly one labeled argument instead of requiring coverage of every member, since unmentioned members alias the active one. The e2e composes a `@repr(packed)` UART bit view + a `@repr(union)` outer struct overlaying the raw `UInt32` and the bit view, then probes the bit ordering with seven crafted words to verify each field decodes through the union to the expected mask. `@no_auto_barrier` queues for the next §A6 slice. |
| `824946c` | **§A6 follow-up: `MmioWireView[T]` (endianness)** — new TypeKind::MmioWireView wraps primitive T; constructor `.at(ptr, endian)` takes a MutPtr[T] + an `Endianness` argument (compiler-known enum with cases `.little` / `.big` / `.native`) and is gated on the Mmio capability via the existing CapabilityChecker. `.read()` returns the host-order value, `.write(v)` accepts a host-order value; the runtime template byteswaps via `std::byteswap` (requires `<bit>`) when the device endianness differs from `std::endian::native`. Codegen emits the Endianness enum at file scope (parallel to BarrierScope / BarrierKind / TlbScope) so leading-dot resolution picks up `.big` unqualified. The e2e drives a big-endian wire over a stack-resident UInt32 cell and asserts the raw bytes after write are byteswapped on LE hosts, then verifies the round-trip recovers the original. `@repr(union)` bit-field composition and `@no_auto_barrier` queue for follow-ups. |
| `08b8744` | **§A12 first slice: `&decl` + `@extern("conv")`** — new `AddressOfExpr` AST node; parser recognizes prefix `&` (disambiguated from the binary `&` by position). Sema types `&static_T` as `Ptr[T]`; `&func` rejected with a clear "not yet supported" hint (the Vestra function-pointer type is queued for a follow-up). Codegen lowers `&ident` to the plain C++ `&ident`. `@extern("conv")` validates the string against the spec's set (C / hvc64 / smc64 / sysv64 / win64 / ms_abi / aapcs / riscv / interrupt); v0.5 hosted emits no extra C++ attribute (the kernel target adds the matching `[[gnu::*_abi]]`). Drive-by: IntLit codegen strips the `_` digit separators per §17.1 — `0xCAFE_BABE` had been leaking into C++ as a user-defined-literal suffix. |
| `450de25` | **§A11 first slice: `PerCpu[T]`** — new TypeKind::PerCpu wraps Trivial T; `c.mine()` synthesized as a method returning T. Codegen lowers to a 64-byte-aligned `__vstr::PerCpu<T>` template (single-slot v0.5 hosted; kernel target swaps to `[MAX_HARTS]Padded<T>` with a target-specific current-hart register read). Pairs naturally with §A4 atomics — `@noinit static c: PerCpu[Atomic[UInt32]]` is the canonical per-hart counter with no false sharing. `PerCpu.new(value) using Alloc -> Box[PerCpu[T]]` (heap factory) and `slot(hartId) using RawMemory -> Ptr[T]` (cross-hart accessor) queue for follow-ups. |
| `14b9482` | **§A7 InterruptsOff scope + Scheduler.swapContext (first slice)** — `InterruptsOff` joins `register_builtin_capabilities` so `with InterruptsOff { ... }` resolves through the existing markered-with path. The CapabilityChecker enforces five of the §14.13 rules: `waitForInterrupt` / `waitForEvent` rejected (rule 5 deadlock), `await` rejected (rule 2), `spawn` rejected (rule 4), and any callee with `using Alloc` or `using Async` in its row rejected (rule 3). Rule 1 (mask save/restore) is target-table data — the hosted build emits a brace block, the kernel target swaps the prologue/epilogue. New compiler-known `Context` opaque struct + `Scheduler.swapContext(saving, loading)` intercept; codegen wraps args in `&` and routes to a `__vstr::scheduler_swap_context` no-op shim with a 256-byte 16-aligned Context. Spec form is `MutPtr[Context]` / `Ptr[Context]`; v0.5 accepts the bare Context values since `&decl` from §14.6.3 isn't shipped yet. |
| `35802ab` | **§A10 first slice: `@panic_handler` delegation** — sema validates the canonical `(Str, StrConst, Int) -> Never` signature, empty using row, and at-most-one-per-unit rule. Codegen extends the runtime preamble with an inline `__vstr::panic_handler` function-pointer slot; the `__vstr::panic` shim consults it and delegates when non-null, falling back to the existing `std::println(stderr) + std::abort()` path. When a unit declares a `@panic_handler` function, a static-init lambda after its namespace block registers the handler's address with the slot — pre-main per [basic.start.static]. `Never` becomes admissible as a source-level return-type spelling (looked up by `primitive_kind_by_name` and lowered to `__vstr::Never` via `primitive_map`). `memcpy`/`memset`/`memmove` as in-tree intrinsics (§15.4) and `no_libc = true` (§15.5's manifest flag) wait on follow-ups. |
| `6dc24ca` | **§A8 first slice: `@interrupt` handler shape + [[gnu::used]]** — sema validates ISR-shaped functions (exactly one `inout TrapFrame` param; Unit return; no Alloc/Async/Extern in the using row). Codegen extends LinkAttrs with an `interrupt` flag; the matching `[[gnu::used]]` prefix lands on every @interrupt-attributed function so dead-code elimination doesn't drop a handler whose only references live in a hand-written vector table or linker script. Pairs naturally with §A1's `@symbol` for the canonical `extern "C"`-style ISR-symbol pattern. The full §14.5.3 vector-table lowering (typed `@interrupt(T)` arrays + per-slot stride alignment + trampoline fallback) and labeled-arg attribute parsing wait on follow-ups. |
| `43638c1` | **§A6 first slice: typed MMIO views** — two new type kinds (`MmioView[T]` / `MmioRegion[T]`) restricted to primitive T. Constructors `.at(ptr)` / `.at(ptr, count)` take a MutPtr[T] and are gated on the `Mmio` capability via the CapabilityChecker (composing with §A3's RawMemory for the pointer mint). Codegen wraps a `volatile T*` in __vstr::MmioView<T> / __vstr::MmioRegion<T> templates so every read/write touches the bus; `.index(i)` bounds-checks via __vstr::panic. Parser accepts `.read` as a member name (alongside `.type` / `.default` / `.embed`) so MMIO call sites don't trip the ParamMode::Read keyword. MmioWireView (endianness), `@repr(union)` bit-field composition, and `@no_auto_barrier` queue for follow-ups. |
| `9053c69` | **§A5 follow-up: cache + TLB management** — nine more free-function builtins (cleanData / invalidateData / cleanInvalidateData / zeroData / publishInstructions / invalidateAllInstructions / tlbInvalidateAll / tlbInvalidatePage / tlbInvalidateAsid) plus the TlbScope enum (currentEL / allELs / innerShareable / outerShareable). The data-cache + publishInstructions ops are gated on the §A3 RawMemory capability via the existing CapabilityChecker; I-cache global + TLB ops act on architectural state alone and don't require RawMemory. Hosted runtime shims are no-ops except `zeroData` which actually zeros the span (kernel target replaces with real architectural sequences). |
| `4839cbd` | **§A3 raw-mint primitives — Ptr[T] / MutPtr[T] + Span.raw / MutSpan.raw** — new TypeKind::Ptr / MutPtr lower to `const T* / T*`; four builtin mint shapes (Ptr.unchecked / MutPtr.unchecked / Span.raw / MutSpan.raw) all gated on the RawMemory capability. T for the .unchecked forms is inferred from the call-site expected type; T for .raw comes from the pointer argument. Lowers to `reinterpret_cast<T*>(addr)` and `std::span<T>(ptr, static_cast<size_t>(count))`. The non-escape rule on derived views isn't yet enforced (waits on §10's escape-rule landing); pointers themselves are Trivial and escapable per the spec. Unlocks §A5 cache management, §A6 MMIO, §A8 vector tables. |
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
