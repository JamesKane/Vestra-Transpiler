# Why Vestra? — Vestra and Embedded Swift, side by side

*Vestra and Embedded Swift cover overlapping ground: bare-metal targets, microcontrollers, kernels, performance-critical code without garbage collection. They also share surface DNA — `func`, `struct`, `enum`, argument labels, `match`, `let`/`var`. The questions worth answering are: where do they actually differ, and why does Vestra exist as a separate language at all?*

This document is a fair comparison against Embedded Swift as it stands in May 2026 — Swift 6.3 having "finished" the Embedded Swift feature set the prior quarter, with noncopyable types still stabilizing and the typed-throws-on-embedded story still listed as an open question in the Embedded Swift vision.

---

## 1. The framing difference

**Embedded Swift is a *subset* of Swift, not a dialect.** The vision document is explicit: code written for Embedded Swift compiles unchanged in standard Swift, and the embedded compilation mode is selected by a flag (`-embedded`) and gated by a `SWIFT_EMBEDDED` predicate. The new `EmbeddedRestrictions` diagnostic group warns when the source uses something Embedded does not have — untyped throws, generic calls on existentials, the print function, reflection, metatypes-at-runtime, and so on. There are also *two* embedded modes — allocating and non-allocating — with the latter forbidding classes, indirect enums, and escaping closures entirely.

**Vestra is one language.** Its non-goals are explicit on this point (§1): *no second mode, no `strict`/`managed` split, no `Send`/`Sync` markers, no ambient authority, no GC, no mode-implicit reference counting*. Where Embedded Swift's strategy is "remove features the runtime would need," Vestra's is "do not have those features in the first place." A program written for a 16 KB microcontroller and a program written for an audio kernel are the same Vestra — the things a kernel programmer wants gone are gone everywhere, and the things an application programmer wants are recovered with library types (`Arc[T]`, allocators, executors), never with a mode switch.

The practical consequence: a Swift developer reading an embedded codebase must hold two languages in their head — what Swift *is* and what Embedded Swift *is not*. A Vestra developer holds one.

---

## 2. Where the costs live

Vestra's organizing rule (§1) is that *everything that matters is in the signature*, tested by six questions answerable from local reading alone (§2): *Does this allocate? Copy a non-trivial value? Call indirectly? Fail? Suspend? What capabilities does it reach?*

Apply the same six questions to Embedded Swift, mode-by-mode, and the differences fall out:

| Question | Embedded Swift (allocating) | Embedded Swift (non-alloc) | Vestra |
|---|---|---|---|
| Does this allocate? | Implicit on any `class` use, any `String` operation, any closure capture into a closure that escapes. ARC traffic invisible at the call site. | Banned outright (no classes, no escaping closures, no indirect enums). | **In the signature** — `using Alloc`. A function without it allocates nothing. |
| Copy a non-trivial value? | Implicit; value types copy on assignment, classes share by reference. | Same. Noncopyable types are arriving but not yet the default. | **In the source** — `copy x` is the only way to duplicate a non-`Trivial` value. Hidden copies are a compile error. |
| Call indirectly? | Protocol witnesses are specialized away when statically resolvable; class methods use vtables. Not visible at the call site. | Same, with classes restricted. | **In the type** — `dyn Protocol` is the only dynamic dispatch, and it is a two-word fat value. `some` and generic bounds are static. |
| Can this fail? | `throws` is typed *only when typed throws are usable* — and Embedded Swift's vision document still notes typed throws are restricted because errors are existentials, with resolution open. | Same. | **In the type** — `throws(E)` always names the error type. There is no untyped `throws`. |
| Can this suspend? | `async` is a function colour; no firm story for `async` in resource-constrained Embedded. | Same. | **In the row** — `async` is `using Async`, one capability among others. One mechanism for all effects. |
| What capabilities does it reach? | There is no concept of a capability. A function can use anything in scope — allocator, logger, peripheral. | Same. The discipline is conventional, not enforced. | **The whole point** — every authority a function consumes is in its `using` clause. `vestra audit --capabilities` prints the set. |

Embedded Swift's removals — no reflection, no metatypes at runtime, no existentials, no `any Protocol` — are *real wins*. But they are wins by **subtraction from a larger language**, leaving a careful developer to remember what the larger language would have done. Vestra's wins are by *construction*: there is no larger language to subtract from.

---

## 3. Sharing: a type, not a mode

Embedded Swift, in allocating mode, keeps **Automatic Reference Counting** for every `class`. The minimal Swift runtime supplies `swift_retain` / `swift_release` / `swift_allocObject`, and a class instance pays for refcount traffic whether the program actually shares the object or not. This is *the* feature that makes Embedded Swift comfortable to write — and *the* feature that complicates predictable performance, because retain/release calls are inserted by the compiler at points the source does not show.

Vestra's sharing story is one sentence: `Arc[T]` is a *type*, not a mode (§1, §10). A program that does not write `Arc[T]` pays nothing for reference counting; a program that does, pays exactly at the point the type appears. There is no compiler-inserted retain or release. The cost is in the source.

This is not "Vestra forbids sharing." Sharing is a real, useful pattern; Vestra simply makes the program declare where it lives. The same is true of allocation, of dynamic dispatch, of suspension — every choice that costs something is a choice the developer made on purpose.

---

## 4. Errors: typed always, never existential

Embedded Swift's vision document is candid about typed throws: *"Typed throws are restricted because thrown errors currently use existential types (any Error), which are prohibited."* Resolution is listed as an open question. This is a direct consequence of the subset framing — Swift's error model rests on `any Error`, and removing existentials means rebuilding the error story for the subset.

Vestra has no untyped `throws` (§9). `throws(E)` is sugar for returning `Result[T, E]`, every fallible function names its error type, and error widening is *explicit* — a caller of a `throws(E2)` function whose own signature is `throws(E)` writes `try call().mapError(toE)`. Because the error model never used existentials, there is nothing to retrofit for embedded use.

The same applies to **noncopyable types**. Embedded Swift inherits Swift's gradual rollout of `~Copyable` — the late-2026 expectation is that Swift 7 will *stabilize* non-copyable types. Vestra has **affine bindings by default** (§5): every binding is consumed at most once as `sink`, `copy` is the explicit duplication, and `linear` is the further discipline for values that must be consumed before scope exit. This is not "Vestra adopted noncopyable types early" — it is the language's *only* memory-management story. There is no copyable default to opt out of.

---

## 5. Unsafe: a capability, not a keyword

Swift's `unsafe` API uses naming conventions (`UnsafePointer`, `withUnsafeBytes`, `unsafeBitCast`) and, in 6.x, an `@unsafe` attribute and `unsafe` expression marker. These are *advisory* — they document where the compiler has stopped helping; they do not change what authority a function holds.

Vestra's unsafety is **four capabilities**: `RawMemory`, `Mmio`, `Asm`, `Extern` (§14). They behave like every other capability — a function that does a raw pointer dereference either *propagates* `RawMemory` in its signature (handing the obligation to its caller) or *discharges* it with a `with RawMemory { ... }` block, and `vestra audit --unsafe` flags every discharge that lacks a `// Safety:` justification. The capability narrowing rule means a broad `Mmio` grant can be reduced — never widened — to a single device's address range before being handed to a driver. An interrupt handler has no caller, so it cannot *receive* an unsafe capability; every authority it needs, it discharges in its own body.

The Swift MMIO package (shipping with Swift 6.3) is the closest analogue: a typed wrapper for memory-mapped I/O with an SVD-based code generator. It is a good piece of engineering. But it is a *library that uses `UnsafeMutablePointer` internally*, not a structural guarantee that MMIO authority flows where the program says it does. The audit question — *which functions can poke this peripheral?* — is, in Swift, a grep over conventions. In Vestra, it is the type system.

---

## 6. Metaprogramming and reflection

Embedded Swift removes reflection entirely: no `Mirror`, no `type(of: value)`, no runtime metatypes, no stringification of arbitrary types. The reason is sound — those features cost runtime metadata, and runtime metadata is what Embedded is trying to avoid. The cost is that Swift idioms relying on reflection (e.g. `Codable` synthesis, debug printing) need alternative implementations or simply don't work.

Vestra's metaprogramming (§12) is a graduated stack:

1. **`comptime` evaluation** — pure, sandboxed; the same language run earlier.
2. **Reflection over `Type` values at compile time** — read-only, never at runtime.
3. **`derive(Eq, Hash, Clone, Debug, …)` via comptime reflective defaults** — the equality function is *one ordinary generic with a comptime body*. No code is generated.
4. **Declaration macros** — `comptime func`s that take an AST and return one, built with a typed, hygienic `quote { ... }`. Output is re-checked and `vestra expand`-able.

Crucially, **none of this costs runtime metadata**. `Type` values exist only at compile time, reflection is a property of the comptime interpreter, and `derive(Eq)` lowers to the same field-by-field comparison a hand-written `equals` would. The cost model of an Embedded-Swift-like target is unchanged — Vestra simply ships the comptime reflection layer Embedded Swift removed.

The "What's new" coverage for Swift 6.3 includes pure-Swift float printing, `@section`/`@used` for linker control, `@c` for C-ABI exports, and SVD-driven MMIO. Each is a real improvement. Each is also additive — features the embedded subset *acquires*, one at a time, on its way to feature parity with what bare-metal C has had for decades. Vestra's bet is that those features are simpler when the language is designed for them from line one.

---

## 7. Concurrency: structural, not annotational

Swift 6's data-race safety rests on the `Sendable` protocol, actor isolation, and `@MainActor` / global-actor annotations. It is a substantial achievement, and a substantial annotation burden — the strict-concurrency migration is one of the largest in Swift's history. Embedded Swift inherits the same model, with the open question of how `async`/`await` integrates with non-allocating environments.

Vestra has **no `Send`/`Sync` markers**, and the data-race-freedom guarantee (§11, §19.8) holds structurally:

- A value handed to another task is **moved** (§5's affine rule leaves no live alias).
- Borrowed views are **non-escapable** (§10, so they cannot leave the call frame).
- Shared mutable state is **`Atomic` or `Arc` over immutable data** — the only kinds.
- Data-parallel mutation is the library function `parallel`, which works because of two language affordances: provenance-tracked disjoint partitions (§5 clause 4) and non-escaping closures (§7).

There is nothing to annotate, because the hazard is absent rather than marked. An async function is one whose `using` row contains `Async` — function colour is not a separate axis but a value in the row, composed with every other capability by the same rule.

---

## 8. ABI and reproducibility

Embedded Swift explicitly **eliminates library evolution and ABI stability** — every binary is statically linked, every generic specialized at use, no resilience boundaries. This makes sense for embedded: a microcontroller flashes one image. It does not generalize to a language whose other half is high-performance applications shipping as packages.

Vestra has a **versioned Tier V ABI** (§15), link-stable within each edition, with the linker rejecting cross-edition mismatches. The same language, the same compiler, ships an embedded binary *and* an application package — the difference is `cfg.os == .freestanding` versus `.linux`, not a separate language tier.

Reproducibility is also a built-in: `comptime` evaluation is pure (no clock, no network, no environment), `cfg` is part of the build's recorded inputs, and two builds of the same sources under the same `cfg` are byte-identical (§19.10). Swift has no equivalent guarantee at the language level.

---

## 9. Conditional compilation: every branch type-checked

Swift uses `#if`/`#elseif`/`#endif` preprocessor-style conditionals: `#if SWIFT_EMBEDDED`, `#if os(macOS)`, `#if arch(arm64)`. An inactive branch is not parsed in the same way an active one is — drift between platforms is a familiar Swift bug.

Vestra has no preprocessor (§12.6). `cfg` is a `comptime`-known value, conditional compilation is `comptime if` / `comptime match` / `@when(predicate)`, and **every branch of every configuration conditional is parsed and fully type-checked, each in a configuration its predicate selects**. Editing on x86-64 cannot silently break the ARM64 branch — a cross-platform file is checked, in one build, for every platform it claims to support. Only the matching branch is *lowered*; the others produce no instructions and no symbols.

This is a stronger guarantee than C's `#ifdef`, Rust's `cfg`, or Swift's `#if` — and it falls out of the same `comptime` machinery that powers reflection and macros. One mechanism, not three.

---

## 10. SIMD: a primitive, not a library

Embedded Swift has SIMD support through standard library types (`SIMD4<Float>` and friends), which lower to platform vector instructions where available. The shapes are reasonable; the operation set varies by platform.

Vestra makes `[N]T` a **language primitive** (§13), with a fixed **portable core** — elementwise arithmetic and logic, swizzles, mask comparisons, branch-free `select`, reductions, saturating arithmetic, compile-time-fixed shuffles — every operation with a defined scalar lowering. The portable core *is* the language; exotic instructions (AVX-512 mask-conflict, hardware AES, matrix accelerators) are ordinary `@when`-gated library functions implemented with `asm`, not blessed intrinsics.

Vestra's bet is that *the developer writes the width*, and what they wrote is what runs — no auto-vectorizer, no hope-it-vectorizes. For audio, simulation, and numerics, this predictability matters more than the convenience of "the compiler might figure it out."

---

## 11. Comprehensibility, measured

Vestra ships an explicit comprehensibility test (§2): pick any line, answer six questions from local reading alone. The concept inventory is roughly thirty orthogonal pieces, and the grammar fits on two pages (§17). The compiler-known type set is short on purpose — the smaller it is, the more of the language is just the language.

Swift's concept inventory is larger by a factor — properties (stored, computed, lazy, property-wrapped), subscripts, classes vs. structs vs. actors, `@MainActor` and friends, key paths, result builders, opaque return types, existentials, value generics, protocol witnesses, async/await/structured concurrency, macros (a separate Swift-Macros system), property wrappers, dynamic member lookup, dynamic callable, `@objc` interop. Each is useful in its niche. The aggregate exceeds what fits comfortably in one head.

This is not a criticism of Swift — it is a different bet. Swift optimizes for *one language across application tiers from iOS to server to embedded*. Vestra optimizes for *one language a person can finish reading in an afternoon*.

---

## 12. Where Embedded Swift wins

A fair document names what Embedded Swift does better:

- **Ecosystem.** Swift Package Manager, an enormous community, Apple's commitment to the toolchain, IDE integration (Xcode, VS Code, JetBrains), LLDB. Vestra is, today, a single transpiler in a single repo.
- **Maturity.** Embedded Swift in Swift 6.3 is shipping production firmware. Vestra is a specification with a transpiler bootstrap.
- **C++ interop.** Swift now has bidirectional C++ interop. Vestra defers C++ interop past v1.
- **Apple silicon.** If the target is an Apple device, Swift is the path of least resistance by a large margin.
- **The migration story.** Swift code that *already exists* gets to keep working when the build flag flips. Vestra has no migration path because there is no Vestra code to migrate.

If the goal is shipping firmware on a microcontroller this quarter, Embedded Swift is a serious, working option. The case for Vestra is not "Swift is bad"; it is "the design space looks different if you start from a clean sheet with these priorities."

---

## 13. The thesis, restated

Embedded Swift solves a real problem by *removing* features from a general-purpose language until the remainder fits. The removals are honest, well-specified, and shipping. The cost is that what remains is *defined relative to* a larger thing — the rules a developer must hold in their head are "what Swift does" *minus* "what Embedded Swift removes."

Vestra solves a different problem by *not having the larger thing in the first place*. There is no implicit allocation to remove, no ARC to disable, no existentials to forbid, no reflection-at-runtime to strip, no untyped `throws` to typedify, no `Send`/`Sync` to retrofit. The thirty-concept inventory is the *whole language*, and the six-questions test is satisfied by any well-formed line.

The wager:

> A language that fits, whole, in one developer's head will produce code that is more correct, more performant, and more auditable than a language that requires the developer to remember which subset they are writing.

This is not a refutation of Embedded Swift. It is a separate experiment, run from a different starting premise. The two languages can both be right — they answer different questions.

---

## Sources

- [Embedded Swift Vision Document — swift-evolution](https://github.com/swiftlang/swift-evolution/blob/main/visions/embedded-swift.md)
- [State of Swift 2026 — devnewsletter.com](https://devnewsletter.com/p/state-of-swift-2026/)
- [Embedded Swift Improvements Coming in Swift 6.3 — Swift.org](https://www.swift.org/blog/embedded-swift-improvements-coming-in-swift-6.3/)
- [What's new in Swift: February 2026 — Swift.org](https://www.swift.org/blog/whats-new-in-swift-february-2026/)
- [What's new in Swift: March 2026 — Swift.org](https://www.swift.org/blog/whats-new-in-swift-march-2026/)
- [Embedded Swift Documentation — docs.swift.org](https://docs.swift.org/embedded/documentation/embedded/)
- [Get Started with Embedded Swift on ARM and RISC-V — Swift.org](https://www.swift.org/blog/embedded-swift-examples/)
- [SE-0390: Noncopyable structs and enums](https://github.com/swiftlang/swift-evolution/blob/main/proposals/0390-noncopyable-structs-and-enums.md)
- [SE-0437: Noncopyable Standard Library Primitives](https://github.com/swiftlang/swift-evolution/blob/main/proposals/0437-noncopyable-stdlib-primitives.md)
- [SE-0528: Noncopyable Continuation (under review, April 2026)](https://github.com/swiftlang/swift-evolution/blob/main/proposals/0528-noncopyable-continuation.md)
- [Swift 6 — strict concurrency, typed throws, migration](https://www.forasoft.com/blog/article/swift-6-must-have-features)
- [Announcing Swift 6 — Swift.org](https://www.swift.org/blog/announcing-swift-6/)
