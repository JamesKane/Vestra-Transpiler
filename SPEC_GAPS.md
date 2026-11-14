# Spec gaps: the bootstrap compiler versus the current draft

*As of 2026-11-14, at commit `e40c3d4`.*

This file records where the bootstrap compiler (`vestra`, the C++26
implementation under `src/` and `include/vestra/`) falls short of the
current language definition:

- [`VESTRA_DRAFT.md`](VESTRA_DRAFT.md): Draft 3.2, the core language.
- [`VESTRA_ANNEX_A.md`](VESTRA_ANNEX_A.md): the consolidated bare-metal annex
  (Drafts 3.3, 3.4 and 3.5). It supersedes `VESTRA_DRAFT_3_3.md` and
  `VESTRA_DRAFT_3_4.md`.

It is a snapshot, not a plan. [`ROADMAP.md`](ROADMAP.md) remains the
priority-ordered menu, and [`HANDOFF.md`](HANDOFF.md) tracks the self-hosting
work. The self-hosted compiler is held to byte-identical output with the
bootstrap, so every gap listed here is inherited by `selfhost/` as well.

## How this was produced

Each chapter of both documents was walked rule by rule. Nearly every entry was
confirmed with a small probe program run through `vestra check` and
`vestra build`, and the generated C++ was inspected. The probes were scratch
files and are not kept in the tree. Entries confirmed only by reading the
source are marked *(code)*. Rules that work as written are not listed. At
least 45 core rules from §3 to §7 alone passed.

Each gap has one of four kinds:

| Kind | Meaning |
|---|---|
| **unsound** | Accepts a program the spec rejects, or emits C++ with undefined or different behaviour. The most important kind. |
| **missing** | The feature is rejected, or the name is unknown. |
| **weaker** | Accepted, but lowered to something weaker than the spec requires, such as a no-op, a host fallback, or a runtime check where a compile-time one is required. |
| **divergent** | Works, but with a different surface or semantics from the spec. Some of these are deliberate v0.5 simplifications, and are marked so. |

## Headline findings

1. **Several safety checks the spec makes static are not enforced.**
   - `let` bindings are mutable.
   - `copy` is accepted on any type.
   - A move inside a loop body is not caught.
   - Views (`Span`, `MutSpan`, `Str`), futures and raw-minted spans can escape
     their scope.
   - Capability rows are not checked through method calls, function values
     or closure bodies.
   - Raw-pointer dereference needs no `RawMemory`.
2. **Generated C++ can have undefined or reordered behaviour where Vestra
   defines it.**
   - Signed overflow, including `&+`, which is defined to wrap.
   - Unchecked indexing.
   - Division by zero.
   - Argument evaluation order.
   - `??` evaluates its right side eagerly.
   - An illegal atomic ordering such as `load(.release)` is passed straight
     through to `std::memory_order_release`.
3. **Unknown names are accepted silently.** These include unknown attributes
   (`@bogus`, `@deterministic`, `@retry_loop`), unknown protocol bounds
   (`T: Ord`, `T: Send`), unknown `derive` names, unknown capabilities and
   unknown `cfg.has(...)` features. A misspelling becomes a no-op, and several
   spec features "work" only in the sense that they are ignored.
4. **Unfoldable comptime constructs degrade to a warning.** `build` prints
   "codegen does not yet support: expression" and emits
   `/* unsupported: expression */`. The output then either fails to compile
   or silently drops a statement. `comptime if` / `comptime match` in a body,
   `Type.structOf`, `as` reinterpretation and comptime calls to runtime
   functions all end up here.
5. **Annex A kernel lowerings exist only with `--no-libc`.** Building with
   `--target=aarch64` without `--no-libc` uses the hosted prelude:
   - system registers become memory cells;
   - `swapContext` is a no-op;
   - barriers become `std::atomic_thread_fence`.

   Even with `--no-libc`, the following are weaker than the spec:
   - `with InterruptsOff` never masks interrupts;
   - `PerCpu[T]` is a single shared slot;
   - the vector table is trampoline-only;
   - 10 of the 12 `vestra audit` modes do not exist.

   The Annex A acceptance criterion cannot be met today.
6. **Large parts of the §18 standard library do not exist**, and the builtin
   `Ordering` collides with the spec's comparison `Ordering`.

---

## Core draft (VESTRA_DRAFT.md)

### §3, §4: lexical structure, primitives, literals, bindings

| § | Gap | Kind | Evidence |
|---|---|---|---|
| §4 | Integer overflow does not trap in debug | unsound | `a + b` lowers to plain C++ `+` on `int32_t`. No checked arithmetic exists anywhere in `src/`. |
| §4 | `&+ &- &*` always wrap | unsound | They lower to plain `+`, which is UB on signed types (`cpp_emitter.cpp:679`, "signed UB, TODO"). |
| §4 | Division by zero traps | unsound | Lowers to a raw `a / b`. |
| §4 | Integer literals are range-checked against the target type | unsound | `return 300` as `Int8` and `return -1` as `UInt8` are both accepted. |
| §4 | `as` byte reinterpretation (size, alignment, `Trivial`) | unsound | Sema returns the target type unchecked (`resolver_exprs.cpp:909`). `UInt64 as Float32` is accepted, and codegen emits `/* unsupported */`. `as?` behaves the same. |
| §4 | `x.to[T]()` / `x.toWrapping[T]()` conversions | divergent | Neither exists. `Int32(c)` is the substitute and is an unchecked `static_cast`. |
| §4 | `addingChecked` and friends | missing | "no field or method 'addingChecked'". |
| §4 | `Enum.from(raw)` | missing | "enum 'C' has no case 'from'". |
| §4 | String interpolation requires `using Alloc` | unsound | Accepted without it. `resolver_exprs.cpp:930` calls it "a follow-on". |
| §4 | `StrConst` copies freely | divergent | Passing a `StrConst` to two `sink` params gives "use of 's' after it was moved". |
| §3 | A block's tail value is its result | missing (partial) | `{ self.n }` in a method, or `{ K }` with an untyped const, gives "can reach the end of its body without returning". |
| §3, §17.1 | `'\u{...}'` char escapes | missing | "unterminated character literal". |
| §3 | Identifiers that are C++ keywords | unsound | A Vestra name such as `mutable` or `class` is emitted verbatim, and the C++ doesn't compile. |
| §6, §17.1 | Byte-char literal `b'/'` | missing | "undefined name 'b'". |
| §17.1 | Unicode identifiers | missing | `café` gives "unexpected character". |

### §5: ownership and exclusivity

| § | Gap | Kind | Evidence |
|---|---|---|---|
| §5, §19.2 | `copy` is rejected on non-copyable types | unsound | `copy` of a `Box`, a linear value or a `deinit` type is accepted. `CopyExpr` is never checked (`ownership.cpp:236`). |
| §5, §19.2, §19.5 | Field-level moves | unsound | `eat(o.i); return o.i.n` is accepted. Moves are tracked per whole binding only. |
| §5 | Small `Trivial` structs copy implicitly | divergent | `is_trivial` covers only primitive scalars (`ownership.cpp:688`). Passing `struct P { var a: Int32 }` to `sink` twice is an error. |
| §5, §19.4 | Inferred lifetimes for returned views | unsound | `return id(localArray)` as `Span[Int32]` is accepted. |
| §19.3 | Exclusivity across an `inout` method receiver | unsound | `p.bump(&p.a)` is accepted. |
| §19.3 | Ownership, exclusivity and linearity inside closure bodies | unsound | A double `close(a)` of a linear value inside a `parallel` body is accepted. No checker has a `Closure` case. |
| §7, §5 | Closures move their non-`Trivial` captures | unsound (partial) | A move-only value captured by a closure is still usable afterwards. Capturing a `var` is rejected outright, because function types are plain function pointers. |

### §6: structs, enums, opaque types, layout

| § | Gap | Kind | Evidence |
|---|---|---|---|
| §6 | `deinit` | missing | Parsed (`parser_decls.cpp:57`) but never checked or emitted, so no C++ destructor is generated. Its no-capability rule goes unchecked (`print` in `deinit` passes). |
| §6 | No moving a field out of a `deinit` type | unsound | `take(r.b)` is accepted. |
| §6 | `embed` fields | missing (partial) | Memberwise init rejects the embed label ("not constructible (yet)", `resolver_call.cpp:1223`). The explicit `e.t.p` path fails. Ambiguous promoted names are not diagnosed. |
| §6 | Opaque types inherit no operations | unsound | `==` between two values of an opaque `EntityId` is accepted. |
| §6 | `@repr(wire)` byte order | weaker | Emits a plain struct. An unknown `@repr(bogus)` is accepted. |
| §6 | Exhaustiveness of nested patterns | unsound | Leaving out `.p(.y)` is accepted. |
| §6 | Guarded arms do not count toward coverage | unsound | `.a(let r) where r > 0` plus `.b` passes as exhaustive. |
| §6, §17.6 | Slice patterns and struct patterns | missing | "expected pattern". |
| §17.6 | A bare identifier pattern matches an existing value | divergent | `case k:` binds a fresh `k`, which always matches. |
| §6 | `match self` inside an enum method | divergent (bug) | "enum pattern '.a' used against non-enum type &lt;error&gt;". |
| §7, §9, §18 | `.none` as the empty optional | missing | "'.none' has no contextual type". Only `nil` works. |

### §7: functions, generics, protocols

| § | Gap | Kind | Evidence |
|---|---|---|---|
| §7 | Argument labels are enforced | unsound | `rotate(1, axis: 2)` and `rotate(1, 2)` are both accepted. |
| §17.2 | Default arguments `= expr` | missing | "call expects 2 argument(s), got 1". |
| §17.5 | Explicit type arguments on a function call, `id[Int32](1)` | missing | They work only on struct and enum construction. |
| §7 | A const generic `N` as a value in the body | missing | "undefined name 'N'". |
| §17.3 | Const-expression array lengths `[N + 1]T` | missing | Parse error. |
| §7 | Bound names `Ord` / `Hashable` | divergent | The checker knows `Comparable`/`Ordered`/`Hash`. Unknown bounds are accepted, so `[T: Ord]` admits anything (`resolver_generics.cpp:203`). |
| §7 | Operations on `T` are gated by its bounds | unsound | `a > b` on an unbounded `T` is accepted, and the C++ compiler catches it later, if at all. |
| §7 | Transitive bound checking | unsound | An unbounded `g[T]` calling `h[T: Comparable]` is accepted. |
| §7 | Bounds on generic structs at inferred construction | unsound (partial) | Enforced on an annotation `W[N]`, but not on `W(v: N(...))`. |
| §7 | Protocol conformance | missing | An extension missing a required method is accepted. A protocol method on a bounded `T` gives "no field or method". Extensions on primitive types don't attach. |
| §7 | `some P` | missing | Erased to the protocol type. `-> some Shape` returning `Sq` is a type mismatch. |
| §7 | `dyn P` existentials | missing | `let d: dyn Shape = Sq(...)` is a mismatch. |
| §7 | Associated types (`type Element = ...` in a conformance) | missing | Parse error. The spec's grammar has no production for it either; see [spec holes](#spec-holes-found-along-the-way). |
| §7 | `@retroactive` is required on a foreign conformance | missing | `extension Int32: Hashable {}` is accepted. |
| §7 | Operator overloading via `static func +` | missing | "expected 'let', 'var', 'embed', or 'func' inside struct". |
| §7 | `nonescaping` closure parameters | unsound | Parsed, then ignored: returning or storing one is accepted. |

### §8: capabilities

| § | Gap | Kind | Evidence |
|---|---|---|---|
| §8, §19.7 | The `using` row is checked through method calls | unsound | `s.mk(3)`, where `mk` is `using Alloc`, is accepted in a capability-free function (`capability.cpp:519` checks only `Func` symbols). |
| §8, §19.10 | The row is checked through function values | unsound | `let h = f; h(3)` is accepted. |
| §8 | Closure bodies are capability-checked | unsound | `run({ x => f(x) })` is accepted. |
| §8 | Row polymorphism, `(T) using C -> U ... using C` | missing | "cannot infer generic parameter 'C'". |
| §8, §19.7 | `with Cap = value` binds and passes a real value | divergent (v0.5) | Capabilities are a set of names. The bound value is not type-checked (`with Alloc = 42` passes), and codegen discards it as `(void)(...)`. No hidden parameter is passed. |
| §8 | A capability name is a value in scope (`Log.info(...)`) | missing | "no field or method 'info'". |
| §8 | An unused `using` capability warns | missing | No warning is given. |
| §8 | Unknown capability names | unsound | `using Frobnicator` is accepted. |
| §8, §15 | `vestra audit` extracts each function's row | missing | No `--capabilities` mode exists. |

### §9: errors and optionals

| § | Gap | Kind | Evidence |
|---|---|---|---|
| §19.9 | `??` short-circuits | unsound | `o ?? p(7)` emits `(o).value_or(p(7))`, which always evaluates `p(7)`. |
| §9 | `Ptr[T]?` is niche-optimized to one word | weaker | It lowers to `std::optional<const T*>`. |
| §9 | Panic behaviour (`abort` / `hang` / `reset`) set per executable | missing | A hosted panic is hard-wired to `std::abort` (`cpp_emitter.cpp:3648`). No manifest exists. |
| §17.4 | `do` with several `catch` clauses and pattern catches | missing (partial) | One `catch` only, and it takes a name, not a pattern. A bare `catch {` is rejected. |

The core error model works as specified: typed `throws` only, `try` needs an
enclosing `throws`, `try?`/`try!`, `do`/`catch` and `mapError`.

### §10: heap and views

| § | Gap | Kind | Evidence |
|---|---|---|---|
| §10, §19.4 | `Span`/`MutSpan` cannot escape | unsound | Returning a span over a local array, storing one in a struct, and `Box[Span[Int32]]` are all accepted. The only escape check is on `spawn` params (`resolver_exprs.cpp:654`). |
| §10, §19.4 | `Str` cannot escape, transitively through structs | unsound | `Box.new(Hold(t: t))` with `t: Str` is accepted. |
| §10 | `Buffer[T]`, `Buffer(fixed:)` | missing | "undefined name". `Vec` is the substitute. |
| §10, §11 | `Arc[T]` | missing | "undefined name". |
| §10 | `StridedSpan[T]` | missing | "unknown type". |
| §10 | Library allocators, and drop freeing through the allocator that built the value | missing | No `Arena`, `ObjectPool` or `FixedAllocator`. `Box.new` lowers to `std::make_unique`, the global heap. |

### §11: concurrency

| § | Gap | Kind | Evidence |
|---|---|---|---|
| §11 | A real concurrent runtime | divergent (v0.5) | Single-threaded cooperative scheduler. `parallel` runs its chunks sequentially (`cpp_emitter_prelude.cpp:144`). |
| §11.2 | `parallel(over: world.chunks(8)) { slice in ... }` is an ordinary library function | divergent | It is a builtin, `parallel(data, chunks, body)`. The spec's trailing-closure form fails to parse. |
| §11.2, §19.4 | The `parallel` body cannot leak its slice | unsound | `ch.send(slice)` inside the body is accepted. |
| §11.1, §19.8 | `send` takes its argument by `sink` | unsound | `ch.send(b); return b.n` is accepted. A linear value sent on a channel still reports "never consumed". The prelude copies. |
| §11.1 | Bounded channels | weaker | Unbounded. `Channel.new(capacity: 4)` is accepted, and the capacity is silently dropped. |
| §19.4 | A non-escapable value in a channel | unsound | `Channel[Span[Int32]]` plus `send(s)` is accepted. |
| §11.1 | A future cannot escape its spawning scope | unsound | Returning a `Future` or storing one in a struct is accepted. Awaiting at scope exit does work (`~Future` pumps). |
| §11.1 | `spawn` rejects non-escapable captures, transitively | unsound (partial) | A direct `Span` param is rejected. A struct holding a `Span` is not. |
| §11.4 | No borrowed view live across `await` | unsound (partial) | Only `inout` params on an `async func` are rejected. A `Span` param or local used after an `await` passes. |
| §11.1 | `select` send-when-ready arm | missing | "select event must be a Future[T] or a channel receive". |
| §11.1 | `timeout` on a mixed channel/future select | missing | Rejected by design (ROADMAP §2, slice 6). *(docs)* |

### §12: compile-time execution and metaprogramming

| § | Gap | Kind | Evidence |
|---|---|---|---|
| §12.1 | `comptime` is pure (no capabilities, no environment) | unsound | `comptime func f() using Clock` folds, and so does `using RawMemory`. `print()` inside `comptime {}` passes. |
| §12.1 | A comptime call to a runtime function is an error | unsound | `const X = comptime { rt() }` checks, then emits `X = /* unsupported */;` with only a warning. |
| §12.1, §12.6 | `comptime if` / `comptime match` | missing | Rejected at module scope. The statement-arm `match` fails to parse. In a body it checks OK, then lowers to `/* unsupported */`, which silently dropped an assignment in one probe. |
| §12.6 rule 1 | Every `@when` branch is type-checked | unsound | An unselected `@when(linux)` body that references an undefined name checks OK. |
| §12.6 | `cfg` follows the build target | unsound | `cfg` comes from the compiler's host `#if`s (`comptime.cpp:648`): `endian` is always little, and `profile` follows the compiler's own `NDEBUG`. `--target=x86_64` on an arm64 host selects the arm64 branch. |
| §12.6 | `cfg.has` with manifest-declared features, and `cfg` as a value | missing | `cfg.has("nonsense")` and `cfg.archh` are accepted silently. `cfg` is undefined outside `@when`. `cfg.option` knows only `cache_line_bytes` = 64 (`resolver_call.cpp:833`). |
| §12.6 rule 3 | The diagnostic names the missing configuration | missing (partial) | Only a plain "undefined name". |
| §12.2 | `Type` as a value (`t.fields` on a `Type` param) | missing | Only `Point.fields`, with a concrete name, works. |
| §12.2 | `Type.structOf(fields:)` | missing | Checks, then emits nothing. |
| §12.2 | Reflection of enum cases, function signatures and attributes | missing | Field `offset` is 0 at macro-expansion time. *(code)* |
| §12.3 | Protocol `comptime` default implementations (`Self.fields`, `self[f]`) | missing | "expected expression, got Self". |
| §12.3 | `derive` as adoption of a reflective default | divergent | Each derive is hand-written C++ emission (`cpp_emitter.cpp:260`, `:547`). |
| §12.3 | `derive` validation | unsound | `derive(Bogus)` is accepted. `derive(Eq)` over a non-`Eq` field passes sema and fails in clang. Only `Default` is validated. |
| §12.3, §18.1 | `derive(Ord)` and `compare()` | missing | "no field or method 'compare'". |
| §12.4 | Hygiene (template names resolve at the definition site) | unsound | A quote that references a global `k` captures a local `k` at the use site. `gensym()` is opt-in only. |
| §12.4 | Compound field types in generated declarations | unsound | A field typed `[4]Int32` or `Int32?` makes the per-field `quote` silently vanish from `expand` and `build`. |
| §12.4 | `.asType()`, splices in struct-name position | missing | "expected struct name". |
| §12.4 | Builder API (`Decl.struct(...)`, `Expr.call(...)`) | missing | Parse error. |
| §12.4 | `error(at:, msg)` from a macro | missing | The macro fails with "did not expand to a declaration list". |
| §12.4 | `quote` yielding a `Stmt` | missing | "expected expression, got let". |
| §12.4 | `quote` only inside a comptime func | divergent (extension) | A plain function may use `quote` as an identity template. |

### §13: numerics and SIMD

| § | Gap | Kind | Evidence |
|---|---|---|---|
| §13.1 | `%`, bitwise ops and shifts on `[N]T` | missing | "supports only elementwise + - * /". |
| §13.1 | Scalar broadcast (`v * 2`) | missing | Rejected. |
| §13.1 | Comparisons to `[N]Bool` masks, and vector `select` | missing | `select(...)` parses as the concurrency form. |
| §13.1 | Swizzles, reductions, `any`/`all`, saturating ops, `shuffle` | missing | "no field or method". |
| §13 | Explicit vector lowering, no reliance on the auto-vectorizer | divergent | A loop over `std::array` relies on the C++ compiler's auto-vectorizer. |
| §13.3 | `@deterministic` FP contract | weaker | Accepted, then ignored: plain `a * b + 1.0`, with no FMA or reassociation control. |
| §13, §18.8 | `core.math`, `Matrix`, `Quat` | missing | Unknown module or type. |
| §13 | Math functions over `Float32` | missing (partial) | `Float64` only. |
| §13 | `Soa[T]` | missing (partial) | Works. The open items are in ROADMAP §9. *(docs)* |

### §14 (core draft): systems programming and FFI

| § | Gap | Kind | Evidence |
|---|---|---|---|
| §14.1, Annex §14.6.3 | Raw-pointer dereference requires `RawMemory` | unsound | `p.value` on `Ptr` and `p.value = 3` on `MutPtr` both pass without it. Only minting is gated, and the exemption is deliberate (`resolver_members.cpp:852`). |
| §14.1 | `Ptr.load()`, unchecked indexing | missing | |
| §14.2 | `Mmio.narrowed(to:)`, `MmioRegion(base:size:)` minting | missing | The `Mmio` capability carries no value (`capability.hpp:32`). |
| §14.3 | `Volatile[T]`, `Volatile(in:at:)` | missing | "unknown type". The annex's `MmioView` is the substitute. |
| §14.3 | `@volatile` fields | unsound | Accepted and emitted as a plain non-volatile field. |
| §14.5 | `@interrupt` with no arguments | divergent | It requires an `inout TrapFrame` first param, which is the annex's model. |
| §14.6 | `import c` synthesizes declarations, `T*` becomes `Ptr[T]?` | missing | It lowers only to `#include`, and `@extern` prototypes are hand-written. *(code)* |

### §15: modules, ABI, tooling

| § | Gap | Kind | Evidence |
|---|---|---|---|
| §15 | Packages and a manifest | missing | One file per module (`driver.cpp:463`). There is no package concept. The `@embed` manifest is a CLI flag. |
| §15 | `package` / `internal` / `private` visibility | missing (partial) | Only `public` resolves across modules. `private` is never enforced (`resolver_generics.cpp:283`). |
| §15 | Tier V ABI (capabilities as params, editions, link check) | divergent | Capabilities are erased. `throws` is `std::expected`. Symbols are Itanium-mangled in a namespace, with no edition tag. |
| §15 | Tier C `@repr(C)` | weaker | Plain C++ standard-layout struct. Probably correct, but not checked against the platform ABI. |
| §15 | `vestra new` / `test` / `doc` | missing | `fmt` prints to stdout only. |
| §15 | `audit --capabilities` / `--unsafe` / `--abi` | missing | `--safety` stands in for `--unsafe`. |

**The backend is C++26, not LLVM.** The spec names LLVM for v1, and several
spec guarantees follow from that choice:

- Tier V calling conventions and edition-stamped symbols cannot be expressed.
- Reproducible builds depend on the host C++ compiler.
- Evaluation order, overflow and FP-contract semantics are inherited from C++
  unless the emitter overrides them, which today it does not.

### §16: deliberate omissions

`unsafe`, `yield`, `null`, `class`, `#ifdef`, `any(N)` and `try`/`catch` are
all correctly rejected. Some things are admitted only because unknown names are
not validated: the `T: Send` / `T: Sync` bounds and the `@strict` /
`@forExpansion` attributes. `<` on a struct with no ordering passes sema and
fails in clang.

### §18: standard library surface

- **Missing entirely:**
  - Containers: `Buffer`, `Arc`, `StridedSpan`, `HashSet`, `TreeMap`,
    `BitSet`.
  - Sync primitives: `Mutex`, `RwLock`, `MemoryOrder`.
  - Allocators: `Arena`, `ObjectPool`, `FixedAllocator`, `Pool`.
  - Runtime and others: `Cooperative`, `Executor`, `Volatile`, `Hasher`,
    `Sink`, `Matrix`, `Quat`.
  - The spelled-out `Optional[T]` and `Result[T, E]`. Only `T?` and `throws`
    exist.
- **`Ordering` collision:** the builtin `Ordering` is the five-case atomic
  memory order (`resolver_builtins.cpp:488`). The spec's comparison
  `Ordering { less, equal, greater }` gives "no case 'less'", and a user
  redefinition is a duplicate. One of the two needs renaming in the spec or
  the compiler.
- **Protocols:** an `Iterator` with `type Element` does not parse.
  Iteration is structural, via a `next()` method. `Eq`, `Hash`, `Display`
  and the rest exist only as `derive` names, not as protocols.
- **Iteration (§18.7):** `zip`, `take`, `map` and `filter` are free functions,
  not iterator methods (`resolver_call.cpp:253`). `skip`, `enumerate` and
  `chain` are missing. `Range` does not compose with the combinators.
- **Views:** `Span.slice(_:)` is missing, and `count` is a property, not a
  method.
- **Text:** `String.push(Char)` and `asStr()` are missing. `Str.chars()`
  demands `using Alloc`, so it is not a borrowing iterator. Slicing a
  `StrConst` yields `Str`.
- **Concurrency:** `Channel.bounded` is missing, as is `Buffer.chunks`. The
  `parallel` signature differs from the spec.
- **`Box`:** `Box.new(1)` infers `Box[Int]` even where `Box[Int32]` is
  expected.

### §19: formal semantics (rules not covered above)

| § | Gap | Kind | Evidence |
|---|---|---|---|
| §19.6 | Linear leak on a `try` error path | unsound | `let h = H(...); let x = try mayfail(); close(h)` is accepted. A plain early `return` is caught. |
| §19.6 | A discarded linear result | unsound | A bare statement `mk()` that returns a linear value is accepted. |
| §19.6 | Consumed only by the type's consumer or a destructure | divergent | Any `sink` param counts, so an empty `func dropit(_ h: sink H)` drops anything. No destructure form exists. |
| §19.6 | Panic paths need not consume linear values | divergent (over-strict) | `let h = H(...); panic("x")` reports "never consumed". |
| §19.9 | Left-to-right evaluation of arguments and operands | unsound | Emitted as direct C++ calls and operators, so order is whatever the C++ compiler chooses. |
| §19.10 | Bounds-checked indexing | unsound | `[N]T` and `Span` indexing lower to unchecked `a[i]`. Only `Vec.get` and `char_at` check. |

Linear obligations do carry correctly through `Optional`, `Box` and generic
structs, contrary to an older ROADMAP note.

---

## Annex A (VESTRA_ANNEX_A.md)

The real kernel lowerings exist only under `--no-libc`. With
`--target=<arch>` and no `--no-libc`, the hosted prelude applies:

- system registers are memory cells;
- `swapContext` is a no-op;
- barriers and cache operations are fences or no-ops.

The table below describes the `--no-libc` build unless it says otherwise.

| § | Gap | Kind | Evidence |
|---|---|---|---|
| Audit table | Twelve `vestra audit` modes | missing (10 of 12) | Only `--sysreg`, `--no-libc` and `--safety` exist (`driver.cpp:215`). `--bss`, `--atomics`, `--extern`, `--vec-table`, `--raw-pointer`, `--mmio`, `--barriers`, `--cache-rounding`, `--interrupts-off`, `--context-switch` and §14.7.1's `--kernel-init` are absent. |
| §14.12.3 | `audit --sysreg` coverage | weaker | The walker skips `while`/`for`/`match` bodies and struct methods (`driver.cpp:894`). One probe with four gated writes reported one. There is no privilege column. |
| §15.5 | `audit --no-libc` lists the symbols the build resolved | weaker | It lists the source `@extern`/`@symbol` declarations only. |
| §4.5 | `@noinit` rules (`Trivial` only, access under `RawMemory`) | unsound | `@noinit static s: String` passes, and `buf[0]` reads directly. |
| §6.7 | `@alias` | weaker | Emits `[[gnu::alias("other")]]` with the unmangled name inside a namespace, and clang rejects it: "alias must point to a defined variable or function". |
| §6.8 | `@padding`, anonymous `embed _:`, flexible array members | missing | Parse errors. |
| §6.8 | A `@repr(union)` must be `Trivial` | unsound | A union with a `String` field passes. |
| §6.8 | `Type.of(T)`, top-level `comptime { assert(...) }` | missing | `T.size` and `T.fields[i].offset` do work. |
| §7.8 | `@inline(.always)` failure is a correctness error | weaker | Left to `[[gnu::always_inline]]`. A recursive `.always` passes sema. |
| §10.5 rule 2 | A raw-minted view's lifetime ends with the `RawMemory` discharge | unsound | A `MutSpan.raw` returned out of `with RawMemory`, stored outside it, or captured by `spawn` is accepted. |
| §14.6.3 | `&table[i]` slot address | missing | "&decl operand must be a static or func identifier". |
| §14.6.1 | Calling conventions are lowered | weaker | `@extern("hvc64")` and `("win64")` are validated, then emitted as plain `extern "C"`. |
| §14.4 rule 1 | An implicit clobber is an error | unsound | An `asm` block that writes an undeclared `x11`, sets flags (`adds`) or writes `daif` passes. Only named registers are validated (`resolver_decls.cpp:1013`). |
| §14.5.2 | `@interrupt(class: .sync, fromEL1: true)` | missing | Labeled arguments are a parse error. |
| §14.5.1 | `core.systems.trap` frame types (`AArch64TrapFrame`, ...) | missing | "unknown type". |
| §14.5.3 | Vector-table lowering rule | weaker | aarch64 only (`cpp_emitter.cpp:1329`). Every slot is a stub that branches to one shared common path, which dispatches through a pointer table: no body inlined in the slot, no `.text.vec_table.<i>` sub-sections, no FP/SIMD save. x86_64 and riscv64 get a plain function-pointer array, not an IDT or `stvec` table. |
| §14.7 | `@boot` is "Vestra in surface" under seven rules | divergent (stricter) | The body may contain only `asm`. The hand-off is a raw `"b kmain"` string, unchecked and unmangled. The spilled-argument and comptime-`Trivial` rules are not checked: a nine-argument `@boot` passes. |
| §14.7.1 rules 2, 3 | Call-graph reachability, `Alloc`-init ordering | missing | Only the `using` rows are enforced. *(code)* |
| §14.7.1 rule 5 | One-way: ordinary code may not reach `@kernel_init` | unsound (partial) | Direct calls are rejected. `let p = &ki` from ordinary code passes. |
| §14.12.3 | `@no_auto_barrier` on a statement | missing | Function level only. The statement form is a parse error. |
| §14.8 | `PerCpu[T]` layout | weaker | One `alignas(64)` slot shared by every hart (`cpp_emitter_prelude.cpp:369`). `slot(hartId)` ignores `hartId`. No `[MAX_HARTS]` array, and no `tpidr_el1` / `gs` / `tp` read. |
| §14.9.1, §14.9.3 | Ordering legality | unsound | `load(.release)`, `store(_, .acquire)` and a CAS failure ordering stronger than success all pass. `load(.release)` emits `std::memory_order_release`, which is UB. |
| §14.9.2 | Orderings default to `.seqCst` | divergent | The ordering argument is mandatory. |
| §14.9.3 | CAS argument labels | unsound | Labels are ignored, so reordered labeled arguments bind positionally: `compareExchange(desired: 1, expected: 0, ...)` emits expected = 1, desired = 0. |
| §14.9.2 | `fetchSet` / `fetchClear` on `Atomic[Bool]` | missing | |
| §14.9.3 | Admitted weak-CAS loop shapes | unsound (partial) | `repeat { } while` is a parse error. Any `while true` admits any number of weak sites (`resolver_decls.cpp:1300`). |
| §14.9.3 | `@retry_loop` predicate, `@allow_weak_cas` | missing | `@retry_loop` is accepted with no clause checks. `@allow_weak_cas` on a statement is a parse error. |
| §14.9.4 | Wide-atomic gate on `aarch64 +lse` / `x86_64 +cx16` | divergent | It requires `lse2` and rejects plain `+lse`. A host or unset target bypasses the gate (`resolver.cpp:203`), so a lock-based libatomic fallback is possible. It is checked at compile time, not at link time. |
| §14.10 | `Cpu.` / `Cache.` / `Tlb.` namespaces, labeled `range:` / `scope:` | divergent | Free functions only. "undefined name 'Cpu'". |
| §14.10.1 | `compilerFence(.relaxed)` is rejected | unsound | Accepted. |
| §14.10.3 | `cleanDataRounding` / `invalidateDataRounding` | missing | |
| §14.10.3 | Strict cache-range alignment | weaker | A runtime `panic` using the `CTR_EL0` line size, not a compile error against `cfg.option`. Hosted builds make every cache op a no-op. |
| §14.10.6 | `prefetch` | missing | |
| §14.11.1 | `MmioView[T]` requires `T: Primitive` | unsound | `MmioView.at(MutPtr[S])` with a struct `S` passes. |
| §14.11.1 | An MMIO view's lifetime ends with the `Mmio` discharge | unsound | Returning a view out of `with Mmio` passes. |
| §14.11.1 | Debug-build alignment check | missing | The prelude `MmioView` is a bare volatile pointer. |
| §14.12.3 | RISC-V auto-barriers (`SFENCE.VMA` after `satp`) | unsound | `satp` has `sync_after_write = false` (`sysregs.hpp`). On non-aarch64 targets the post-write barrier is a plain thread fence. |
| §14.12.2 | Register spellings and typed handles | divergent (minor) | `currentEL` is unknown (the table spells it `currentel`). Every handle is `UInt64`. |
| §14.12.4 | Link-time privilege check | missing | The register table has no privilege data. |
| §14.13 rules 1, 6 | `with InterruptsOff` masks on entry and restores on every exit | weaker (no-op) | Lowers to a bare `{ }` block even with `--no-libc --target=aarch64`. Nothing in `src/codegen/` mentions `InterruptsOff`. |
| §14.13 rule 5 | No wait-for-interrupt inside the region | unsound (partial) | Only a direct `waitForInterrupt()` call is caught (`capability.cpp:500`). A helper that calls it passes. |
| §14.13 rule 7 | `using InterruptsOff` is not admitted | unsound | Accepted as a function's row. |
| §14.14 rule 1 | `Context.size` via reflection | missing | "can't compute Context.size at fold time". |
| §14.14 rule 4 | `FpContext`, `saveFp` / `restoreFp` / `disableFp` / `enableFp` | missing | "unknown type 'FpContext'". |
| §15.4 | Compiler-emitted intrinsics have Vestra-supplied bodies | weaker | `memcpy` and friends lower to `__builtin_mem*`, with no in-tree bodies required. `@stack_protector(.strong / .all)` emits nothing. There is no `__vestra_overflow_trap`, because there is no overflow checking. |
| §15.5 rule 1 | Exactly one `@panic_handler` per binary | weaker | Checked per unit. The hook is `[[gnu::weak]]`, so a binary with no handler silently halts. |
| §15.5 rule 3 | The handler receives file and line | weaker | `panic()` passes an empty file and line 0 (`cpp_emitter_prelude.cpp:1936`). |
| §15.5 | `profile.freestanding.no_libc = true` is a build-time guarantee | weaker | A CLI flag, not a manifest entry. Undeclared externals are not rejected, and the generated artifact is not walked. |

---

## Spec holes found along the way

These are problems in the spec itself, not in the compiler:

- §17's `s-member` grammar has no production for `type Element = ...` inside a
  conformance, although §7 relies on it.
- The hashing protocol is named `Hash` in §18 and `Hashable` in §7.
- `Ordering` names both the comparison result (§18.1) and the atomic memory
  order (Annex §14.9.1).
- §15 does not say whether `internal` is module-scoped or package-scoped.

## Not determined

- Whether freestanding output assembles and links on all three targets. Only
  one aarch64-none-elf unit was compiled.
- Whether the aarch64 vector stub is correct at run time (`sp_el0`, nested
  exceptions).
- Whether `@repr(C)` layout matches the platform ABI on every target.
- How `--target-features` reaches the C++ compiler's `-march`.
- Reverse-order destruction and `deinit` capability rules (moot while `deinit`
  is not emitted).
- Re-checking of macro output for exclusivity and capabilities. Type checking
  and the `Asm` gate are confirmed.
- Cross-module capability enforcement.
