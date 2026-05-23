# Why Vestra? — Vestra and C++26, side by side

*C++26 is the most ambitious C++ standard since C++11: static reflection, contracts, std::execution (senders/receivers), pattern matching via `inspect`, erroneous behavior in place of UB for uninitialized reads, a hardened standard library with bounds checks for the common containers, and the Stroustrup/Dos Reis profiles framework as a serious answer to the safety mandate. **It shipped on 28 March 2026 at the London Croydon meeting.** Vestra's transpiler has been retargeted from C++23 to the C++26 features available in current Apple Clang — so the comparison below is not against a standard on paper but against the actual code Vestra emits, against the C++ a Vestra developer's output is read alongside.*

This document treats C++26 generously: every feature the final draft actually carries is acknowledged, and the comparison is on **how the same outcomes are reached**, not on whether modern C++ "is dangerous" — that line is no longer accurate in 2026.

---

## 1. The framing difference

**C++26 is forty-five years of accumulated decisions, frozen.** The grammar contains C, C with classes, templates, exceptions, multiple inheritance, two memory-management eras (raw pointers and smart pointers), three string types in common use (`const char*`, `std::string`, `std::string_view`), two compile-time-evaluation systems (`constexpr`/`consteval` and the new reflection), two error-handling stories (exceptions and `std::expected`), now two metaprogramming systems (templates and reflection), and — newly — *profiles*, the opt-in framework that turns subsets of the language safety-on. The aggregate is breathtaking in capability and uncomfortably large in concept budget.

The C++26 safety story rests on **profiles**: a program is "safe" in the profile sense if it opts in, with the unsafe constructs tactically opted out where needed. Stroustrup's P3984 type safety profile and Dos Reis's general profiles framework are the load-bearing pieces. Hardened-library bounds checks for `vector`, `string`, `string_view`, `span`, `mdspan`, `optional`, `expected`, `bitset`, and `valarray` are now standard. Erroneous behavior replaces UB for uninitialized reads. This is a major step.

**Vestra is one language with the safety choices already made.** §1's non-goals say so explicitly: no exceptions, no inheritance, no null, no implicit copies, no significant whitespace, no GC, no macro DSL, no two error-handling stories, no ambient authority. The thirty-item concept inventory (§2) is the *whole* language; the grammar fits on two pages (§17). There is no "safe profile" to opt into because there is no unsafe default to opt out of.

The difference can be stated in one sentence: **C++26 makes safety achievable by configuration; Vestra makes it the only configuration.**

---

## 2. The six questions, applied to both

Vestra's organizing rule (§1) — *everything that matters is in the signature* — produces six questions answerable from local reading (§2): *Does this allocate? Copy a non-trivial value? Call indirectly? Fail? Suspend? What capabilities does it reach?*

| Question | C++26 (modern) | Vestra |
|---|---|---|
| Does this allocate? | `new`/`delete` are visible. But `std::string`, `std::vector::push_back`, `std::format`, `std::function`'s small-buffer optimization can all allocate invisibly through the active allocator. Allocator-aware containers thread `std::pmr::polymorphic_allocator` through types, not signatures. | **In the signature** — `using Alloc`. Without it, a function cannot allocate. The allocator is a *value*, not a global or a type-thread. |
| Copy a non-trivial value? | Pervasive. Copy constructors run on assignment, passing-by-value, return-by-value (modulo NRVO), and many implicit conversions. `std::move` opts into a *move* — a destructor still runs. The five (or six) special member functions are a foundational topic. | **In the source** — `copy x` is the *only* way to duplicate a non-`Trivial` value. Move is the default; `Trivial` values copy implicitly. There is no copy constructor concept. |
| Call indirectly? | `virtual` is in the type. Function pointers and `std::function` are explicit. But `virtual` member functions on a base reference dispatch dynamically with no syntactic mark at the call site. | **In the type** — `dyn Protocol` is the *only* dynamic dispatch and is two words. No `virtual` keyword; no inheritance. |
| Can this fail? | **Two systems.** Exceptions (the historic story, with stack unwinding, RAII interaction, and `noexcept` to opt out) and `std::expected<T, E>` (C++23, monadic, exception-free). A library mixes both depending on age and convention. | **One system.** `throws(E)` ≡ `Result[T, E]`; `try` propagates. **No exceptions, no stack unwinding** — `try` is an early `return` of the error value. |
| Can this suspend? | `co_await`/`co_yield` are explicit, and `std::execution` (P2300) brings senders/receivers as the unified async/concurrency model. But the coroutine machinery — promise type, awaitable, awaiter, custom allocator, symmetric transfer — is one of the most complex corners of the language. | `async` *is* the capability `Async`; `await` is well-formed where `Async` is in scope. No `Pin`, no `co_*` machinery, no separate coroutine type system. |
| What capabilities does it reach? | **Anything in scope** — globals, the standard library, the filesystem. C++26 profiles can *deny* some operations, but the default is unrestricted. Authority is conventional. | **The `using` clause** — every authority is named. `vestra audit --capabilities` prints the set per function. |

**C++26 closes ground on questions 1–4** through contracts, `std::expected` discipline, hardened-library bounds checks, and erroneous-behavior elimination of UB. **The capability question** is the one C++ has not answered and is not on track to answer — and that is the question Vestra was designed around.

---

## 3. Memory safety: profiles vs. by construction

C++26's safety story is layered:

- **Erroneous behavior** — uninitialized local variable reads are no longer UB; they yield an erroneous (but defined) value an implementation may diagnose. A whole vulnerability class is recompile-eliminated.
- **Hardened standard library** — bounds checks for the common containers and views, runtime contract violations rather than UB on out-of-bounds access.
- **Profiles** — opt-in subsets that statically deny certain constructs (the `std::bounds` profile, the type-safety profile, the lifetime profile). Multiple profiles can compose.
- **Contracts** — `pre(...)`, `post(...)`, `contract_assert(...)` move runtime invariants into function declarations, visible to callers and to static analysis.

The aggregate is real progress. The aggregate is also a **set of optional disciplines layered onto a language whose default semantics include pointer arithmetic, raw new/delete, undefined-behavior dataflow, and aliasing rules that the borrow checker exists, in another language, to encode**. A program that adopts every profile, hardens its library, and writes contracts everywhere is *substantially* safer than C++23. A program that does not, is exactly as safe as before.

Vestra's safety story is in §19.10 — five guarantees the compiler must uphold for every accepted program:

- **Memory safety** — no use-after-free, no use of uninitialized/consumed storage, no out-of-bounds access, no null dereference (there is no null).
- **Data-race freedom** — structurally, from move + non-escapability + `Atomic`/`Arc`.
- **No use-after-move** — the affine binding rule.
- **No leaked linear resources** — `linear` types must be consumed.
- **Capability safety** — a function performs only effects its `using` row names.

These are **obligations, not aspirations**: a program that *could* violate any of them is not well-formed, and the compiler must reject it. There is no "but only with the right profile flags." The cost is real — programs that idiomatically use C++'s pointer-everywhere style need rewording in Vestra's mode discipline. The win is that the wedge between "C++ as written today" and "C++ as the safety profiles want it" disappears.

C++26's profiles are the right *direction* — they are the language acknowledging that the default needs to be safer. Vestra simply assumes that conclusion and ships.

---

## 4. Two metaprogramming systems vs. one

C++26 has **two compile-time metaprogramming systems** — and they are different languages:

1. **Templates and `constexpr`/`consteval`.** The historic system, with template parameter packs, SFINAE-then-`requires`-clauses, fold expressions, `if constexpr`, `consteval` functions, `std::is_constant_evaluated()`. Concepts (C++20) tamed the worst of template error messages. The full system is a sub-discipline.
2. **Static reflection (new in C++26).** The `^^` reflection operator, `std::meta::*` queries, splicers (`[: e :]`), and code injection via `std::define_aggregate` and friends. This is the *good* metaprogramming — typed, hygienic in design, and the foundation for replacing many template tricks with ordinary compile-time code.

Reflection is the C++26 feature most aligned with Vestra's direction. It is the same idea — *at compile time, a type is a value, and code may introspect and generate over it* — and it is genuinely transformative for C++. The cost is that **C++26 ships reflection in addition to templates, not in place of them**. A C++26 codebase will live with both, often in the same translation unit, often expressing variants of the same idea, for decades.

Vestra has **one metaprogramming facility** (§12), graduated in four layers:

1. **`comptime` evaluation** — pure, sandboxed, sources are byte-identical-reproducible.
2. **Reflection over `Type` values** — read-only at compile time, never at runtime.
3. **`derive(Eq, Hash, Clone, …)` via comptime reflective defaults** — the default `equals` is *one ordinary generic with a comptime body*. No code is generated; there is nothing hidden to read.
4. **Declaration macros** — `comptime func`s that take an AST and return one, built with a typed, hygienic `quote { ... }`. Output is re-checked, and `vestra expand` materializes it as ordinary source.

This is structurally the same idea C++26 reflection commits to — and *only* this idea. There is no template parallel system. Generics are parametric substitution (§7) and nothing more. The lost expressive power of templates' Turing-complete pattern matching is recovered with `comptime` functions over `Type` values, written as ordinary code.

For the kinds of metaprogramming people actually do — `derive`, serialization, schema-driven codegen, named accessors over packed fields — Vestra's facility is the same expressive power as C++26 reflection, *without* the template system in the other half of the toolbox.

---

## 5. Senders/receivers vs. async-as-capability

`std::execution` (P2300, in C++26) is the most sophisticated concurrency model any mainstream language has ever standardized. The three abstractions — schedulers, senders, receivers — provide a composable algebra for asynchronous work; `std::execution::task<T>` is the coroutine type that interoperates with senders; the model unifies CPU thread pools, GPUs, accelerators, and structured cancellation under one interface. The arxiv paper on anonymized network sensing on GPUs using C++26 std::execution (2510.14050) is one of the early production-grade demos.

This is a real and impressive achievement. It is also **a library, layered on top of coroutines, layered on top of the existing C++ type system, with a learning curve that experienced C++ engineers describe as substantial**. The coroutine machinery — promise type, awaitable, awaiter, symmetric transfer, custom allocator integration — is the most complex corner of C++'s type system, and senders/receivers compose on top of it.

Vestra's concurrency is two primitives over the affine + non-escapable foundation:

- **`spawn f(args)`** — concurrent task yielding `Future[T]`, requires `Async`. The closure captures by value (§7), moving non-`Trivial` captures.
- **`parallel(over: partition) { slice => ... }`** — the library function for N workers over a disjoint partition. Works because of provenance-tracked disjoint partitions (§5 clause 4) and non-escaping closures (§7).

Plus `select` (§11.1) for waiting on multiple events, `Channel[T]` for typed move-passing, and `Atomic[T]` as the *one* shared-mutable primitive. **No `Pin`. No promise type. No awaitable/awaiter distinction. No symmetric transfer.** A borrowed view held live across an `await` is a compile error, so task state is value-only.

The honest comparison: `std::execution` can express things `parallel` cannot — heterogeneous executors, custom scheduling policies, work-stealing variants composed at the type level. Vestra's design is *deliberately* smaller — the comprehensibility budget says one mechanism per kind of work, and `parallel` is what the kernel/audio/simulation workloads actually need. A specialized executor (GPU, accelerator) is a library that implements the `Executor` protocol; the call-site looks the same.

---

## 6. Pattern matching: arriving in C++26, foundational in Vestra

C++26 ships pattern matching via the `inspect` keyword (P2688R5). It is a genuine improvement over `std::visit` for `std::variant`:

```cpp
inspect(shape) {
    Circle{r}      => area = pi * r * r;
    Rect{w, h}     => area = w * h;
    _              => area = 0.0;
}
```

Guards with `if`, alternation with `|`, wildcards with `_`. This is the same idea Vestra's `match` (§6) ships with from line one:

```vestra
let area = match shape {
    case .circle(let r):       pi * r * r
    case .rect(let w, let h):  w * h
    case .point:               0.0
}
```

The functional shape is similar. The structural differences:

- **`match` is an expression** in Vestra; `inspect` is being shipped as both an expression and a statement form.
- **Exhaustiveness is mandatory** in Vestra — a missing case with no `default` is a compile error naming what is unmatched. C++26's exhaustiveness story depends on the type (e.g. `std::variant` knows its alternatives; arbitrary class hierarchies do not).
- **Vestra has slice patterns** (`case [b'/', b'/', let rest...]:`) and struct patterns natively; C++26's pattern grammar is rich and growing.

This is not a *win* for Vestra so much as a confirmation that **C++26 is, slowly and carefully, catching up to a feature set that newer languages have shipped from the start**. The same is true of `std::expected` (vs. Vestra's `Result`/`throws(E)`), `std::optional` (vs. `T?`), `std::span` (vs. `Span[T]`), and reflection. The trajectory is right; the cost is that every catch-up arrives *in addition to* what came before.

---

## 7. Contracts vs. capabilities

C++26 contracts are a long-awaited feature: `pre(...)`, `post(...)`, `contract_assert(...)` move runtime invariants into function declarations, visible to callers and to static analysis tools. They are checked at one of several enforcement levels (off / observe / enforce / quick-enforce), with violation handlers customizable.

This addresses one part of "what does this function require?" — *runtime preconditions over values*. It does not address the other part — *which authorities does this function consume?* C++26 has no answer to "can this function allocate?" or "can this function write to the filesystem?" or "can this function send a network packet?" beyond convention.

Vestra's `using` clause **is** the structural answer to authority. A function's `using Alloc, Log, Net throws(NetError)` says, with compiler-enforced precision: *this can allocate (through whatever `Alloc` the caller supplied), it can log, it can do network I/O, and it can fail with `NetError`*. `vestra audit --capabilities` extracts the reachable capability set per function, mechanically. C++26 contracts do nothing of the kind, and the profiles framework — which is the closest C++ analogue — is opt-in subset selection, not authority threading.

Contracts and capabilities are **complementary, not competing**. Vestra has no contract syntax, by deliberate omission — preconditions are expressed by *type* (`Optional`, `Result`, non-`Copyable` resource types, `linear` discipline) rather than by runtime predicate. The cases where contracts are uniquely valuable in C++ — predicates over numeric ranges, structural invariants on aggregates — are handled in Vestra by opaque types (§6) and constructors that fail at construction, returning `T?` or `Result[T, E]`.

---

## 8. RAII vs. deinit-without-capabilities

C++ pioneered RAII, and Vestra inherits it. Both languages run destructors at scope exit (§6, §19.5). The difference is what a destructor *can do*.

In C++, a destructor is an ordinary member function: it can allocate, log, throw (problematically), perform I/O, acquire locks, recursively destroy held resources. The `noexcept` qualifier exists in large part to prevent destructors from throwing in stack-unwinding contexts — a long-running source of subtle bugs.

In Vestra, **a `deinit` runs with no capabilities unless the type stores them** (§6). A destructor that needs `Alloc` to deallocate must hold an `Alloc` value as a field; a destructor that needs `Log` must hold a `Log`. This is the same one-rule-applied-everywhere: *everything that matters is in the signature*, and a `deinit` is a signature too. The result: a destructor cannot perform I/O it was not explicitly given, which keeps the one piece of compiler-inserted control flow honest.

There is also no equivalent of *destructor-throwing-during-unwind* because **Vestra has no unwinding**. Errors are values (§9), `panic` is terminal and runs no destructors. Outstanding resources on a panic are reclaimed by the OS or — on bare metal — not at all. This is harsher than C++'s exception story; it is also a simpler invariant.

---

## 9. Module systems: one, vs. two-in-tension

C++26 has both **modules** (since C++20) and the legacy **`#include`** preprocessor system, and as of May 2026 the adoption of modules remains slow. Tooling support across GCC, Clang, and MSVC is inconsistent; build-system integration is non-trivial; most third-party libraries have not migrated. The 2026 expectation, in honest community surveys, is that the two systems coexist for many years.

Vestra has one module system (§15): files share a module by declaring it (`module kernel.ring`); packages are sets of modules built, versioned, and ABI-editioned together; visibility is four levels (`private` / `internal` / `package` / `public`). There is no preprocessor (§12.6 — conditional compilation is `comptime` over the `cfg` value). There is no `#include`, no header/implementation split, no include-order sensitivity, no macros leaking into included files, no precompiled headers, no forward declarations.

The cost is, again, the ecosystem — Vestra has no third-party libraries to migrate. The benefit is structural: a Vestra project's build is a directed graph of modules, parsed and type-checked once, and `vestra build` is incremental and bit-reproducible by construction.

---

## 10. Conditional compilation: every branch type-checked

C++ uses the **preprocessor**: `#if`, `#ifdef`, `#elif`, `#endif`. Code in an unselected branch is **not parsed**. C++26 retains the preprocessor entirely; the conditional-compilation story has not changed. The familiar bug — editing on Linux silently breaking the Windows branch — remains.

Vestra has no preprocessor (§12.6). `cfg` is a `comptime`-known value, and **every branch of every configuration conditional is parsed and fully type-checked**, each in a configuration its predicate selects. Only the matching branch is *lowered*. A misspelled criterion (`cfg.os == .lunix`) is a compile error, not a silently-false test. A reference to a declaration absent from a configuration the conditional spans is a compile error naming that configuration.

This is the kind of difference that doesn't matter on the day you write the code and matters every time you port it.

---

## 11. References, lifetimes, and the dangling story

C++ has **references** (`T&`, `T&&`), **pointers** (`T*`), **smart pointers** (`unique_ptr`, `shared_ptr`, `weak_ptr`), and **iterators** that may be invalidated by container mutation. The lifetime story is largely *cultural* — there is no compiler-enforced lifetime grammar, and dangling references are runtime UB.

C++26's *lifetime profile* (part of the profiles framework) aims to make some of this statically checkable, drawing on the C++ Core Guidelines work. It is an opt-in static analysis on top of an unchanged language.

Vestra has **no surface references** (§5). Every binding names a value; mutation through a caller is `inout` mode; lifetimes are *inferred, never written*. The Law of Exclusivity (§5, §19.3) — any number of `read` accesses overlapping, *or* exactly one `inout`, never both — is the entire aliasing story, because there is nothing else that can alias. Non-escapable types (`Span`, `MutSpan`, `Str`) cannot be returned past their source's lifetime, and the compiler infers the bound locally (§19.4).

The result: **dangling references are not expressible in safe Vestra.** A pointer-like type (`Ptr[T]`, `MutPtr[T]`) exists only under the `RawMemory` capability (§14.1), exactly where C-style pointer arithmetic is needed and the developer accepts the obligation.

The cost is, as elsewhere, expressiveness: some sophisticated C++ code threads `T&` through deep call stacks in ways Vestra's modes do not directly mirror. The win is that the most common C++ memory-safety bug — *the dangling reference returned from a function* — is structurally impossible.

---

## 12. ABI: a real platform standard vs. a versioned native ABI

C++ has a **platform ABI** (Itanium on every platform except Windows; MSVC's on Windows), which is one of C++'s greatest practical strengths. Code compiled with GCC 13 links against a library compiled with GCC 9 (within the same Itanium ABI). The same `.so` file is consumable from C, from C++, from Rust via `extern "C"` wrappers. This is the foundation of the C++ ecosystem.

Vestra has a **versioned Tier V native ABI** (§15), link-stable within each edition, and a Tier C ABI that *is* the platform C ABI (for `@extern("C")` and `@repr(C)` items). Cross-edition Tier V link is rejected by the linker. This is structurally similar to Swift's resilience model and meaningfully better than Rust's "no stable Rust ABI" position — but it is **not** as battle-tested as the Itanium ABI, and the ecosystem of Vestra binaries does not exist yet.

Honestly: C++'s ABI is one of the things it does straightforwardly better than every successor language, and Vestra's bet on Tier V will be proven over time or fail to be.

---

## 13. Where C++26 wins, decisively

Naming what C++26 does better is essential to honesty:

- **The ecosystem.** Forty-five years of libraries, drivers, engines, kernels, databases, games. Boost, Qt, Eigen, Folly, the entire HPC stack, every AAA game engine, LLVM, every major browser. Vestra has none of this.
- **Compilers.** GCC, Clang, MSVC, ICC. Three production-grade implementations supported by very large teams. Vestra has one transpiler bootstrap.
- **The platform ABI.** Discussed above — a real, working, decade-stable interface.
- **GPU and accelerator support.** CUDA, SYCL, HIP, OpenMP target offload — every accelerator vendor ships a C++ frontend first.
- **Industry presence.** C++ is the lingua franca of systems, games, finance, embedded high-performance, scientific computing. A C++ job exists everywhere; a Vestra job exists nowhere.
- **Reflection and contracts as standardized.** Vestra's reflection design is structurally similar, but C++26 reflection is in compilers, in production use, with a working group iterating on it.
- **`std::execution`.** Genuinely more expressive than Vestra's two-primitive concurrency for heterogeneous-executor / GPU / accelerator scheduling. Vestra optimized for one comprehensibility axis; C++26 optimized for a different one.
- **Standard library breadth.** `<algorithm>`, `<ranges>`, `<execution>`, `<chrono>`, `<filesystem>`, `<regex>`, `<numbers>`, `<bit>`, `<cmath>`, `<numeric>` — vast and battle-tested.
- **Backwards compatibility.** A C++26 compiler will compile most C++98 code. There is no Vestra-1 to be compatible with yet.
- **Cross-platform tooling.** CMake, vcpkg, Conan, Bazel — and the integrations with every IDE, every CI system, every static analyzer.

If the goal is shipping in 2026 on a real platform with a real team, **C++26 is the rational choice**, and the modernization steps (contracts, `std::expected`, hardened library, profiles) make modern C++ a substantially safer language than its reputation suggests. The case for Vestra is the design-from-scratch case, not the migrate-from-C++ case.

---

## 14. The thesis, restated

C++ has spent the past fifteen years carefully, deliberately retrofitting safety, expressiveness, and comprehensibility into a language whose foundational decisions were made in the 1980s. C++11 was a renaissance; C++17 made it pleasant; C++20 brought concepts, modules, ranges, and coroutines; C++23 brought `std::expected`, `std::generator`, and `std::flat_map`; **C++26 brings reflection, contracts, `std::execution`, pattern matching, erroneous behavior, profiles, and hardened library bounds checks**.

Every one of these is a defensible improvement. The aggregate is a language whose specification runs to thousands of pages, whose features overlap (templates *and* reflection, exceptions *and* `std::expected`, raw pointers *and* `unique_ptr` *and* `shared_ptr`, `#include` *and* modules), and whose safety story is *opt-in profiles* — exactly because making it the default would break the ecosystem.

Vestra's wager is different, and exactly the opposite shape:

> **The same safety, the same performance, and a clearer model can be reached by a language that made the safety decision in section 1, and never had a default to break.**

The ten differences against C++26 specifically:

1. **No exceptions, no unwinding.** Errors are values, `try` is an early return. (§9)
2. **No surface references, no lifetime annotations.** Modes (`read`/`inout`/`sink`) encode the same property. (§5)
3. **One metaprogramming facility, not templates + reflection.** Reflection is the *whole* metaprogramming system. (§12)
4. **One module system, not modules + `#include`.** No preprocessor. (§15, §12.6)
5. **One error-handling system.** No `std::expected`-vs-exceptions choice per library. (§9)
6. **One smart-pointer story.** `Box`/`Buffer`/`Arc` are the entire set; sharing is a *type*, not a default. (§10)
7. **Capabilities — what C++ profiles aim at, expressed in every signature.** Not opt-in safety; structural safety. (§8)
8. **Conditional compilation that type-checks every branch.** No preprocessor skew. (§12.6)
9. **Async is a capability, not a separate machinery layer.** No `Pin`, no promise type. (§8, §11)
10. **Six questions, answered locally, on every line.** A test C++26 cannot pass even with full profile adoption. (§2)

This is not a refutation of C++26. C++26 is the most thoughtful, ambitious, and serious systems-language evolution of 2026, and modern C++ written in the C++26 idiom is dramatically safer than the C++ reputation suggests. The bet on Vestra is the *clean-sheet* bet: that a developer should not have to remember which profile, which edition, which library version, which idiom they are writing — they should be able to **read the whole language in an afternoon** and predict, from any line, exactly what it does. That bet is either worth it or it isn't. C++26 has made the case for "carefully retrofitted" more compelling than it has ever been; Vestra makes the case for "designed once, designed small."

---

## Sources

- [C++26 is done! — Trip report: March 2026 ISO C++ standards meeting (Herb Sutter)](https://herbsutter.com/2026/03/29/c26-is-done-trip-report-march-2026-iso-c-standards-meeting-london-croydon-uk/)
- [C++26: Reflection, Memory Safety, Contracts, and a New Async Model — InfoQ](https://www.infoq.com/news/2026/04/cpp-26-reflection-safety-async/)
- [C++26 Draft Finalized with Static Reflection, Contracts, and Sender/Receiver Types — InfoQ](https://www.infoq.com/news/2025/06/cpp-26-feature-complete/)
- [C++26 — Wikipedia](https://en.wikipedia.org/wiki/C++26)
- [C++26 Finalized: Reflection, Contracts, Async Model — byteiota](https://byteiota.com/c26-finalized-reflection-contracts-async-model/)
- [State of C++ 2026 — devnewsletter.com](https://devnewsletter.com/p/state-of-cpp-2026/)
- [Core safety profiles for C++26 (P3081R2)](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2025/p3081r2.pdf)
- [C++26 Standard library hardening — Sandor Dargo's Blog](https://www.sandordargo.com/blog/2026/05/13/cpp26-library-hardening)
- [C++26 Contracts are not a good fit for standard library hardening (P3878R0)](https://isocpp.org/files/papers/P3878R0.html)
- [Senders/Receivers: An Introduction — ACCU / Lucian Radu Teodorescu](https://accu.org/journals/overload/32/184/teodorescu/)
- [Execution control library (since C++26) — cppreference](https://en.cppreference.com/cpp/execution)
- [Pattern Matching: match Expression (P2688R1)](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2024/p2688r1.pdf)
- [Pattern matching using `is` and `as` (P2392R2)](https://isocpp.org/files/papers/P2392R2.pdf)
- [Can we finally use C++ Modules in 2026? — Mathieu Ropert](https://mropert.github.io/2026/04/13/modules_in_2026/)
- [C++26 Memory Safety Is the First Serious Answer to the Rewrite Fantasy — John Farrier](https://johnfarrier.com/c26-memory-safety-is-the-first-serious-answer-to-the-rewrite-fantasy/)
