# Why Vestra? — Vestra and Rust, side by side

*Rust is the language Vestra most resembles in ambition: no garbage collector, no null, errors as values, ownership and move semantics, monomorphized generics, a `Result`/`?` story Vestra borrows almost verbatim. The differences are not in **what** Rust achieves — Rust's safety story is the proof-of-concept that made Vestra possible — but in **how**, and at what cost in comprehensibility, the same outcomes can be reached.*

This document is a fair comparison against **Rust 1.95.0** (April 16, 2026, the current stable), built on the 2024 edition that landed in 1.85, and with the keyword-generics / effect-system work still in initiative status.

---

## 1. The shared premises, the diverging methods

Vestra and Rust agree on more than either disagrees with anything else:

- No tracing GC. No null — absence is `Option<T>` / `T?`. Errors are values — `Result<T, E>` / `Result[T, E]`. The `?` operator is `try`.
- Generics are monomorphized; protocols/traits are the only abstraction over multiple types.
- Memory management is *visible*: heap allocation is a type (`Box`/`Buffer`/`Arc` in both languages), not a hidden default.
- Move semantics are the foundation. A value handed to another owner is *gone* from the sender.
- The implementor — kernel or audio engine — is the target audience, not the application programmer who would prefer a GC.

Where they differ is **how those properties are *expressed* in the source**, and that is the entire content of this document. Rust expresses them through a stack of mechanisms that accumulated over a decade: lifetime parameters, `Send`/`Sync` marker traits, the `unsafe` keyword, two macro systems, an effect set (`async`, `const`, `unsafe`, `gen`, `try`) growing piecewise, function colouring, `Pin`/projection, the orphan rule. Each is *defensible in isolation*. The aggregate is the largest concept inventory of any production systems language.

Vestra's bet is that the same guarantees can be reached with **one row-polymorphic capability mechanism**, **no surface references**, **one metaprogramming facility**, and a roughly thirty-item concept inventory (§2). The lost expressive power is recovered with library types, never with new grammar.

---

## 2. The six questions, applied to both

Vestra's organizing rule (§1) is that *everything that matters is in the signature*. The test (§2): any line can be answered locally for six questions — *Does this allocate? Copy a non-trivial value? Call indirectly? Fail? Suspend? What capabilities does it reach?*

| Question | Rust (stable, 1.95) | Vestra |
|---|---|---|
| Does this allocate? | The global allocator is **ambient** — `#[global_allocator]` is a process-wide static. `Vec::push`, `String::push_str`, `Box::new`, formatting macros, even `panic!` can allocate. Not visible at the call site. | **In the signature** — `using Alloc`. A function without it cannot allocate. The allocator is a *value*, not a global. |
| Copy a non-trivial value? | Implicit `Copy` for `Copy` types, implicit *move* for non-`Copy`, explicit `.clone()` for the rest. The line between *move* and *copy* is in the type, not the operator — `let y = x;` does either. | **In the source** — `copy x` is the only way to duplicate a non-`Trivial` value. The keyword is mandatory. |
| Call indirectly? | `dyn Trait` is explicit (fat pointer, 2 words). `impl Trait` is static. Function pointers are explicit. Aligned with Vestra. | **Same shape** — `dyn Protocol` (two-word fat value), `some Protocol` (opaque), generic bounds (monomorphized). |
| Can this fail? | `Result<T, E>` is typed; the error type is in the signature. `?` propagates. Also matches Vestra. | `throws(E)` ≡ `Result[T, E]`; `try` propagates. **There is no untyped throws** — Rust's idiomatic `Box<dyn Error>` has no Vestra equivalent. |
| Can this suspend? | `async fn` colours the function. `.await` is permitted only inside `async` contexts. `Pin`/`Future`/`Waker` machinery is observable through `Pin<&mut Self>` projection. | `async` *is* the capability `Async`; `await` is well-formed where `Async` resolves. **No `Pin`, no projection** — borrows cannot cross `await` (§11.4), so task state is value-only. |
| What capabilities does it reach? | **There is no concept of a capability.** A function can use anything in scope — `println!`, a global allocator, a logger crate, environment variables. Authority is conventional, not enforced. | **The whole point** — every authority in the `using` clause. `vestra audit --capabilities` prints the set per function. |

The honest reading of this table: **Rust scores well on four of six questions** — dynamic dispatch, failure, and partial credit on copy/move are all in the source. Vestra's wins are concentrated on the *allocate / suspend / what-can-this-reach* axis, and that axis is where kernels and high-performance applications live.

---

## 3. References and lifetimes: the design Vestra rejects

Rust's borrow checker is the proof that *aliasing + mutation = bugs* can be solved statically — and the engineering achievement that made every subsequent memory-safe systems language possible. It is also Rust's largest concept budget item: lifetime parameters (`'a`), elision rules, higher-ranked trait bounds (`for<'a>`), the orphan rule, GATs, NLL, the still-stabilizing Polonius next-generation borrow checker.

Vestra's wager (§5): **eliminate surface references and the lifetime grammar disappears.** Every binding names a value; there are no `&T`/`&mut T`; mutation of a caller's value happens through parameter modes — `read` (the default, immutable view), `inout` (exclusive mutable view), `sink` (transfer ownership). Lifetimes are *inferred, never written*; a function returning a borrowed view (`Span[T]`, `Str`) ties its lifetime to its inputs, and where several inputs could be the source the result is tied to their *intersection* (§5, §19.4).

The Law of Exclusivity (§5, §19.3) is one rule, checked locally:

> Any number of `read` accesses may overlap, **or** exactly one `inout` may be live — never both. Disjoint fields are independent. The pieces of a provenance-tracked partition of a `Buffer`/`Span` are independent.

This is the same property Rust's borrow checker enforces, expressed without lifetime annotations — and **without** a borrow checker as a separate phase, because without references there is nothing else to alias. The cost is real: some Rust programs that thread `&'a mut T` through complex data structures need rewording in Vestra's mode discipline. The benefit is that the *grammar* of memory safety fits on half a page.

A Rust programmer reading Vestra notices the absence immediately: no `'_`, no `'static`, no `for<'a>`. A Vestra programmer reading Rust spends a quarter of their attention budget on lifetime annotations that infer-or-spell-out the same property Vestra encodes structurally.

---

## 4. The effect zoo: Rust has one, Vestra unifies it

Rust has **unknowingly shipped an effect system** since 1.0 — `async`, `const`, `unsafe`, with `gen` and `try` added since. Each effect interacts with traits, function types, and closures by *duplicating the API surface*: an `Fn` family for each effect combination. Five effects produce, in principle, 96 distinct trait variants. The keyword-generics initiative exists to introduce **effect-generic functions** (row polymorphism over effects) so the API surface need not multiply.

This is the work Vestra did at the design stage. The `using` clause **is** the row, and `Async` is *a value in it*, not a separate function colour. A higher-order function that abstracts over an unknown capability set (§8) writes:

```vestra
func map[T, U, C](_ xs: Span[T], _ f: (T) using C -> U) using C -> Buffer[U]
```

`C` is *the same `C`* whether it carries `Async`, `Alloc`, `Log`, the unsafe `RawMemory`, or nothing. One mechanism, uniformly. There is no `AsyncFn` / `FnMut` / `ConstFn` / `TryFn` zoo because there is no separate function colour to multiply against.

The relevant Rust feature — keyword generics with effect inference — is still in the initiative phase as of May 2026. Vestra's design assumes that work *concludes* and asks what the language would look like if it had been there from the start.

---

## 5. Unsafe: a capability, not a keyword

Rust's `unsafe` is a *keyword*: an `unsafe fn` declaration, an `unsafe { ... }` block, an `unsafe impl`, an `unsafe trait`. Inside the block, the compiler relaxes specific checks (raw pointer deref, calling `unsafe fn`, mutating a `static mut`, implementing an `unsafe trait`). It does **not** relax the aliasing rules — code inside `unsafe { ... }` must still uphold them, and a violation is undefined behaviour the compiler may exploit. Miri (the official UB detector) is *dynamic analysis only*, catching UB on paths actually executed.

This is the source of one of Rust's most subtle failure modes: *unsafe code that compiles, passes tests, and silently breaks under a future optimization pass* because it violated an aliasing rule no static check enforced. The Rust community has built impressive defences — Stacked Borrows, Tree Borrows, Miri integration in CI — but the *primitive* is a block that says "I claim to uphold the rules," not a structural guarantee.

Vestra has **no `unsafe` keyword** (§14). The four operations Rust's `unsafe` gates are instead four capabilities:

| Capability | Gates |
|---|---|
| `RawMemory` | dereferencing a raw `Ptr`/`MutPtr`; unchecked indexing |
| `Mmio` | constructing a `Volatile` register handle |
| `Asm` | an inline-assembly block |
| `Extern` | calling an `@extern("C")` function |

A function performing a gated operation either **propagates** the capability — names it in `using` — or **discharges** it with `with RawMemory { ... }`, at which point `vestra audit --unsafe` requires a `// Safety:` justification at the discharge site. A capability can be **narrowed** but never widened (§14.2): a broad `Mmio` grant is reduced to a single device's address range before being handed to a driver, and the structural guarantee that authority can never expand is static.

The crucial structural property: an *undischarged* unsafe operation is **in the signature**, so §1's rule holds; a *discharged* one is encapsulated, and its callers are not coloured (which is what lets `Buffer[T]` be raw internally and safe externally). Rust's `unsafe fn` does roughly the same thing — but the `unsafe` keyword does double duty as both *"I do something dangerous"* and *"the caller must uphold an invariant,"* and the language relies on convention to know which.

Aliasing UB *cannot exist* in safe Vestra because **there are no references to alias** (§3). Within `RawMemory`, the aliasing burden lives where it must — and only there.

---

## 6. Macros: one facility vs. two

Rust has **two macro systems**:

1. **`macro_rules!`** — declarative, token-tree pattern matching. Hygienic in limited ways; untyped; the matched tokens are not type-checked until expansion.
2. **Procedural macros** — separate crates (`proc-macro = true`), compiled to `.so` / `.dylib`, run inside the compiler. Receive a `TokenStream`, return a `TokenStream`. Three flavours (function-like, derive, attribute).

Both are real wins for Rust; both are also a meaningful comprehensibility cost. A `derive(Serialize)` or `#[tokio::main]` requires reading another crate to know what was generated, and `cargo expand` is the workaround. The procedural-macro language is *not* Rust — it is Rust *manipulating Rust syntax*, with a different mental model.

Vestra has **one metaprogramming facility** (§12), graduated in four layers:

1. **`comptime` evaluation** — pure, sandboxed.
2. **Reflection at compile time** — `Type` is a value; fields, cases, signatures.
3. **`derive(Eq, Hash, Clone, …)` via comptime reflective defaults** — the protocol's default `equals` is *one ordinary generic with a comptime body* reflecting over the type. **No code is generated**, so there is nothing hidden to read.
4. **Declaration macros** — `comptime func`s that take an AST and return one, built with a typed, hygienic `quote { ... }`. Output is type-, exclusivity-, and capability-checked exactly as if hand-written, and `vestra expand` materializes it as ordinary source.

The thesis is *climb only as high as the job demands* (§12.5). The top layer exists so "the language does not have it" is never a dead end, and the local-reasoning test (§2) degrades gracefully and visibly as you climb: a macro-attributed line is the *one* place Vestra admits the test is not fully local. Rust's two macro systems both fail the test, all the time.

This is the largest single comprehensibility difference between the two languages. Reading an idiomatic Rust crate without reading `tokio`, `serde`, `clap`, and `thiserror`'s procedural-macro output is impossible. Reading an idiomatic Vestra module without reading its declaration macros is possible — there usually aren't any, because `derive` and `comptime` defaults cover what proc macros do in Rust.

---

## 7. Conditional compilation: every branch type-checked

Rust's `#[cfg(...)]` is a small predicate sublanguage — `any`, `all`, `not`, with `target_arch`, `target_os`, `feature`, `target_feature`, custom keys. Rust 1.95 added `cfg_select!`, a compile-time match over `cfg`s that replaces the popular `cfg-if` crate. The mechanism is solid; the *guarantee* is weak: a branch not selected by the active configuration is **not type-checked**. Editing on x86-64 silently breaking the ARM64 build is a familiar Rust bug.

Vestra has no preprocessor and no separate `cfg` predicate language (§12.6). `cfg` is a **`comptime`-known value**, in scope everywhere, with built-in fields (`.arch`, `.os`, `.endian`, `.pointerBits`, `.profile`) and a manifest-declared namespace of `feature` and `option` criteria. A configuration test is an ordinary comptime `Bool` over `cfg` — `cfg.arch == .arm64 && cfg.has("neon")`.

The stronger guarantee: **every branch of every configuration conditional is parsed and fully type-checked**, each in a configuration its predicate selects. Only the matching branch is *lowered*. A reference to a declaration absent from a configuration the conditional spans is a compile error naming that configuration. A misspelled criterion is a compile error, not a silently-false test.

This is a property C's `#ifdef`, Rust's `cfg`, and Zig's `comptime` all give up. Vestra's bet is that it should not be optional for a systems language — the cost in usability is small, the cost in cross-platform reliability is large.

---

## 8. Concurrency: structural, not annotated

Rust's data-race-freedom proof rests on:

- The `Send` marker trait — *can be transferred between threads*.
- The `Sync` marker trait — *can be shared between threads via shared reference*.
- The borrow checker — preventing aliasing+mutation.
- Auto-trait inference — `Send`/`Sync` derived automatically from a type's fields, with explicit `!Send` / `!Sync` for the cases inference gets wrong.

This is, again, a real achievement. It also means a Rust programmer encounters `error[E0277]: ... cannot be sent between threads safely because it does not implement Send` regularly enough that explaining it is a beginner-Rust meme.

Vestra has **no `Send`/`Sync`**, and the data-race-freedom guarantee (§11, §19.8) holds structurally from rules already in the language:

- A value handed to another task is **moved** (§5's affine rule — no live alias survives).
- Borrowed views are **non-escapable** (§10 — they cannot leave the call frame).
- Shared mutable state is **`Atomic`** or **`Arc` over immutable data** — the only kinds.
- Data-parallel mutation is the library function `parallel`, which works because of provenance-tracked disjoint partitions and non-escaping closures (§11.2).

There is nothing to annotate because the hazard is absent rather than marked. This is the most direct illustration of Vestra's thesis: the *same property* Rust enforces with two marker traits, autoderivation, and a borrow checker is enforced in Vestra by the affine move rule, non-escapability, and one library function. No new vocabulary.

---

## 9. Async: a capability in the row

Rust's async is famously complex. `async fn` desugars to a function returning `impl Future`; `await` is permitted only in `async` contexts; self-referential futures require `Pin<&mut Self>`; projection is an unsafe-ish art (the `pin-project` crate exists for this); `Send`/`Sync` analyses must thread through async state machines. The async-trait story has been a multi-year stabilization. Async closures stabilized in 1.85 (Feb 2025); dyn-compatible async traits are still partially nightly-only via `dynosaur`.

Vestra collapses this. `async` is the capability `Async` (§8); `async fn` is sugar for `using Async`; `await e` is permitted exactly where `Async` is in scope. The only async-specific rule is **no borrow across `await`** (§11.4) — a borrowed view held live across a suspension is a compile error, so task state is stored by value. **There is no `Pin`, no projection, no `Unpin`, no `pin-project`**, because there is no self-referential task state to protect. Awaitable closures and awaitable traits are not separate things — they are ordinary closures and protocols whose `using` row contains `Async`.

The Rust async ecosystem is enormously valuable and works. The Vestra design asks: *what would async look like if it had been a capability from day one*? The answer is roughly four pages instead of a small library of language extensions, marker conventions, and helper crates.

---

## 10. Generics, traits, and the orphan rule

Both languages monomorphize. Rust uses **trait coherence** with the **orphan rule** — `impl Trait for Type` is permitted only when the crate owns either the trait or the type — to prevent conflicting blanket impls across the ecosystem. The orphan rule is structurally necessary in an open-world ecosystem; the workaround is the *newtype pattern* (a wrapper struct) when you must add a foreign-trait impl to a foreign type.

Vestra's protocol-conformance rule is the same in shape: *conformance is declared in the owning package* (§7), and adding a conformance for a type and protocol the package owns neither of requires the audited `@retroactive` annotation. This is the orphan rule with a different name and a louder warning.

The differences are smaller:

- **Vestra has no higher-kinded types** — and no `for<'a>` HRTBs, because there are no lifetimes. Both languages support *parametric* generics; neither supports HKT.
- **Associated types are pinned by name in brackets** in Vestra (`some Iterator[Element = UInt8]`), matching how a type argument is supplied. Rust uses `impl Iterator<Item = u8>`.
- **No GATs in Vestra.** Generic associated types are a Rust generalization Vestra deliberately does not pursue — its associated types are *plain*, with no defaults and no bounds beyond a protocol constraint, because their job is to let a protocol name a type it does not choose. Some Rust libraries that rely on GATs (notably for lending iterators) have no Vestra-shaped equivalent; the workaround is the iterator-as-`struct` pattern (§7).

The omission is intentional. GATs are a substantial concept-budget item for a small additional set of expressible programs, and they fail the local-reasoning test sharply.

---

## 11. The standard library shape

Both libraries are designed against the same constraint — *do not require a runtime the kernel does not have*. Rust splits this across `core`, `alloc`, and `std`; `no_std` programs use `core` plus optionally `alloc` once a `#[global_allocator]` is wired up. The `#[global_allocator]` is **ambient authority by design** — once registered, every `Box::new`, every `Vec::push`, every `format!` reaches it.

Vestra's library is a fixed *blessed* set (§2) — the compiler-known types `Box`, `Buffer`, `Span`/`MutSpan`/`StridedSpan`, `Arc`, `String`/`Str`/`StrConst`, `Optional`/`Result`, `Atomic`, `Channel`, `Future`, `Volatile`, `Type` — plus ordinary library types built on top. The blessed set is short on purpose: around two dozen items, every one of them with a compiler-internal rule that could not be written as ordinary library code. Allocators (`Arena`, `ObjectPool`, `FixedAllocator`) and executors (`Pool`, `Cooperative`) are *ordinary library types*, not blessed mechanisms.

Crucially, **there is no global allocator** in Vestra. An allocator is a *value* (§8), passed via `using Alloc`. The same source compiles into a kernel with an arena, an embedded program with a fixed allocator, and a server with a system allocator — by selecting the allocator at the `with` site, never by changing the program. The cost is verbose call sites in some programs; the win is that *which allocator does this function reach?* is always a local-reading question.

---

## 12. SIMD: primitive vs. layered

Rust's portable SIMD (`std::simd`) is still nightly as of May 2026. The stable story is **target-specific intrinsics** (`std::arch::x86_64::*`, `std::arch::aarch64::*`) — pinpoint-accurate, architecture-coupled. The 2026 stabilizations added AVX-512 FP16 and AArch64 NEON FP16 intrinsics (Rust 1.94, March 2026).

Vestra makes `[N]T` a **language primitive** (§13), with a fixed **portable core** — elementwise arithmetic and logic, swizzles, mask comparisons, branch-free `select`, reductions, saturating arithmetic, compile-time-fixed shuffles — every operation with a defined scalar lowering, so `[16]Int32` runs correctly on a target without a 512-bit unit. Exotic instructions (mask-conflict, hardware AES) are ordinary `@when`-gated library functions implemented with `asm` — not blessed intrinsics, not a compiler-magic intrinsic table.

The split is deliberate: the portable core *is* the language; the exotic border is *ordinary configured library code*. Rust will, in time, stabilize its portable layer; Vestra ships with it.

---

## 13. ABI and reproducibility

**Rust has no stable Rust ABI.** Only `extern "C"` is stable across compiler versions; the Rust-native ABI may change between releases, and a Rust library compiled with 1.94 cannot be linked into a 1.95 binary as a Rust library. The workaround is `extern "C"` interfaces or recompilation from source, the latter being the cargo norm.

Vestra has a **versioned Tier V ABI** (§15), link-stable within each edition. Every Tier V symbol carries its edition; the linker rejects cross-edition mismatches. The same compiler ships an embedded binary, an application package, and a kernel module — and a third-party Vestra library can ship as a `.rlib`-equivalent that downstream consumers link against without recompiling from source.

Reproducibility is also a built-in: `comptime` is pure, `cfg` is recorded, two builds of the same sources under the same `cfg` are byte-identical (§19.10). Rust achieves bit-reproducibility through `cargo`'s `--locked` and careful management of build inputs; the *language* makes no such guarantee.

---

## 14. Where Rust wins, decisively

A fair document names what Rust does better — and the list is long:

- **Ecosystem.** crates.io, cargo, ~150,000 packages, an enormous community. Vestra is a specification and a transpiler bootstrap.
- **Tooling.** `rust-analyzer`, `cargo`, `clippy`, `rustfmt`, `miri`, `cargo-audit`, `cargo-deny`. A decade of investment.
- **Production deployment.** Linux kernel, Windows kernel, Cloudflare's Pingora, Firefox, much of the Android stack, large portions of AWS infrastructure. Vestra has none of this.
- **The borrow checker as proof.** Rust *proved* aliasing + mutation could be solved statically. Vestra inherits the lesson; without Rust the experiment wouldn't have run.
- **Industry adoption and hiring.** A Rust job exists, as a category. A Vestra job does not.
- **Documentation.** `The Rust Programming Language`, the rustonomicon, the reference, an enormous body of tutorials.
- **Edition migration.** Rust's edition system is a model for evolving a language without breaking the ecosystem. Vestra has nothing comparable because it has no ecosystem to evolve.
- **HKT-adjacent expressiveness.** GATs, HRTBs, complex lifetime-parameterized libraries — some Rust APIs have no clean Vestra equivalent.
- **Async ecosystem.** `tokio`, `async-std`, `embassy` for embedded async — Vestra has the `Async` capability and the design of `parallel`, but not a tokio-class library.

If the goal is shipping production code this quarter on a well-understood target, **Rust is the right answer**. Vestra's case is the long-term one: *if you were redesigning Rust today, knowing what it learned, what would you cut?*

---

## 15. The thesis, restated

Rust solved the central systems-programming problem — aliasing + mutation = bugs — in 2015, and the language has spent the decade since making the surrounding ergonomics tolerable. Each new feature is a defensible local improvement: NLL, the `?` operator, edition 2018/2021/2024, async, GATs, trait upcasting, let-chains, the keyword-generics initiative still in flight. The aggregate is a language whose specification runs to thousands of pages, whose reference cites multiple still-unstable features as load-bearing, and whose error messages have themselves become a sub-discipline.

Vestra's wager is that **the same safety, the same performance, and a clearer security model** can be reached with a roughly thirty-item concept inventory, a two-page grammar, and one row-polymorphic capability mechanism in place of the accumulated effect zoo. The cost is the entire Rust ecosystem; the prize is a language a developer can read in an afternoon and *thereafter predict, from any line of code, exactly what it does*.

Concretely, Vestra's bet against Rust is:

1. **No lifetime grammar.** Eliminate references; modes encode the same property. (§5)
2. **No `unsafe` keyword.** Four capabilities replace it, and unsafety is in the signature. (§14)
3. **No `Send`/`Sync`.** Structural concurrency safety from rules already in the language. (§11)
4. **One effect/capability row, not five effects multiplying.** Row polymorphism from day one. (§8)
5. **One metaprogramming facility, not `macro_rules!` plus proc macros.** Typed, hygienic, locally inspectable. (§12)
6. **Conditional compilation that type-checks every branch.** No silent platform skew. (§12.6)
7. **No `Pin`, no projection, no `Unpin`.** No borrows across `await`, by rule. (§11.4)
8. **A stable Vestra ABI.** Tier V, versioned per edition. (§15)
9. **No global allocator.** An allocator is a value, passed through `using Alloc`. (§10)
10. **Six questions, answered locally.** Every line, every time. (§2)

If those ten differences are *worth* losing the Rust ecosystem, Vestra exists. If they are not, Rust is the right tool and this document is for documentation rather than persuasion. The wager — that a language a developer can hold whole in their head produces code that is more correct, more performant, and more auditable than a language that requires them to keep adding subsystems — is the entire question. The two languages are the same answer to "no GC, no null, errors as values, move semantics, monomorphized generics," and **divergent answers to "how much language is too much."**

---

## Sources

- [Announcing Rust 1.95.0 — Rust Blog (April 16, 2026)](https://blog.rust-lang.org/2026/04/16/Rust-1.95.0/)
- [Announcing Rust 1.94.0 — Rust Blog (March 5, 2026)](https://blog.rust-lang.org/2026/03/05/Rust-1.94.0/)
- [Announcing Rust 1.88.0 — Rust Blog (June 26, 2025)](https://blog.rust-lang.org/2025/06/26/Rust-1.88.0/)
- [Announcing Rust 1.85.0 and Rust 2024 — Rust Blog (February 20, 2025)](https://blog.rust-lang.org/2025/02/20/Rust-1.85.0/)
- [State of Rust 2026 — devnewsletter.com](https://devnewsletter.com/p/state-of-rust-2026/)
- [Rust Versions — releases.rs](https://releases.rs/)
- [Effect-Generic Bounds and Functions — keyword generics initiative](https://rust-lang.github.io/keyword-generics-initiative/explainer/effect-generic-bounds-and-functions.html)
- [Extending Rust's Effect System — Yoshua Wuyts](https://blog.yoshuawuyts.com/extending-rusts-effect-system/)
- [Miri: Practical Undefined Behavior Detection for Rust — Ralf Jung (POPL 2026)](https://research.ralfj.de/papers/2026-popl-miri.pdf)
- [Unsafe Rust — The Rust Programming Language](https://doc.rust-lang.org/book/ch20-01-unsafe-rust.html)
- [no_std — The Embedded Rust Book](https://docs.rust-embedded.org/book/intro/no-std.html)
- [Rust 1.94 Released With Stable Support For AVX-512 FP16 Intrinsics — Phoronix](https://www.phoronix.com/news/Rust-1.94-Released)
- [Rust 2024 Wrap-Up — rust-dd.com](https://rust-dd.com/post/rust-2024-wrap-up-biggest-changes-and-future-outlook)
