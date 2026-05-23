# Vestra Language Specification — Draft 3.2

*Vestra is a systems language designed so a developer can hold the **whole language** in their head — the language of the VectraOS stack and of high-performance applications. Its backend is LLVM in v1, and the Vellum backend post-v1.*

> This specification supersedes the earlier Vestra proposal (Draft v9, preserved in the project's git history): the language is the design first explored under the working name "Lumen," adopted as Vestra. Drafts 1–2 fixed the language and its grammar; Draft 3 adds the formal semantics.

Vestra has one organizing rule, and every feature in this document earns its place against it:

> **Everything that matters is in the signature. Nothing that matters is hidden.**

A function's type states what it can reach (capabilities), how it can fail (typed errors), what it borrows versus takes, and whether it can suspend. If a behaviour is not written in a type, the compiler guarantees it cannot occur. That single rule is simultaneously the safety model, the security model, and the performance-predictability model — and it is the reason the language is comprehensible.

---

## 1. Vision, priorities, non-goals

Vestra serves two targets, and refuses to compromise either:

- **Operating systems** — VectraOS as the canonical target. No GC, no hidden runtime, no ambient authority, no kernel heap assumption.
- **High-performance applications** — engines, simulation, audio, numerics. No GC pauses, full layout control, first-class vectors, deterministic memory.

It serves both **without a mode switch**. There is one Vestra. Memory management is a *type* you choose (`Arc[T]` when you want sharing), never a *dialect* you opt into. A language with two modes is two languages, and a developer must learn both.

The four priorities, in deliberate order: **safety, performance, security — and, weighted across all of them, comprehensibility.** Where comprehensibility conflicts with expressive power, comprehensibility wins; the lost power is recovered with a library, never with a language feature.

**Non-goals** (design invariants — changing one is a governance decision, not an RFC):

- No tracing GC. No mode-implicit reference counting — sharing is the explicit `Arc[T]` type.
- No null. Absence is `Optional`.
- No exceptions, no stack unwinding. Errors are values.
- No inheritance, no class hierarchies, no virtual-by-default. Composition and protocols only.
- No surface reference types (`&T`) and no lifetime syntax (`'a`). Ownership is parameter modes; lifetimes are inferred.
- No significant whitespace. Blocks are brace-delimited, always. Indentation never changes meaning.
- No macro language. Compile-time metaprogramming is the ordinary language, run earlier.
- No ambient authority. No global mutable state — a `static` binding may hold an immutable value or an `Atomic`, never a plain mutable one. No hidden allocation, copy, or dispatch.
- No function "colour" beyond the one capability mechanism.
- A stable, versioned ABI.

---

## 2. The whole language

The comprehensibility claim is testable. **Pick any line of Vestra; a developer must answer all six questions by local reading alone** — the signatures in scope plus the line itself, nothing further:

1. Does this allocate?
2. Does this copy a non-trivial value?
3. Does this call indirectly (dynamic dispatch)?
4. Can this fail?
5. Can this suspend?
6. What can this code reach (which capabilities)?

If any answer requires reading another file, the feature that hid it is a bug in the language.

**The concept inventory.** This is *everything* in Vestra — there is nothing else to learn:

> modules · packages · visibility · `let`/`var`/`const`/`static` · primitives · `struct` · `enum` · `opaque type` · `embed` · `func` · argument labels · closures · generics `[T]` · protocols · `some` · `dyn` · associated types · the three parameter modes (`read`/`inout`/`sink`) · `copy` · exclusivity · capabilities (`using`/`with`) · `Result`/`throws`/`try` · `Optional` · `String`/`Str`/`StrConst` · `Box`/`Buffer`/`Span`/`MutSpan`/`Arc` · allocators · `Iterator`/`for` · `async`/`await`/`spawn`/`parallel`/`Channel` · `comptime` · reflection · `derive` · macros · conditional compilation (`cfg`/`@when`) · `[N]T` vectors · raw `Ptr`/`asm` · the unsafe capabilities · attributes · linear types.

Roughly thirty concepts, each orthogonal. That is the budget, and this document does not exceed it.

**Compiler-known types.** A short, fixed set of types are *compiler-known* — the compiler has built-in rules about them, and they could not be written as ordinary library code: the integers, floats, `Bool`, and `Char`; `[N]T` vectors; `Ptr`/`MutPtr`; `Result` and `Optional`; `Box`, `Buffer`, `Span`/`MutSpan`/`StridedSpan`, and `Arc`; `String`/`Str`/`StrConst`; `Atomic`, `Channel`, and `Future`; `Volatile`; and `Type` (§12.2). Everything else that looks built-in — `Matrix`, `Quat`, `HashMap`, the allocators, the iterators, every protocol — is ordinary library code with no special status. The blessed set is short on purpose: the smaller it is, the more of the language is just the language.

---

## 3. Lexical structure and syntax

The surface is **Swift- and Scala-3-flavoured**: `func`, `struct`, `enum`, `protocol`, `extension`, `match`, argument labels, `\(…)` interpolation, `let`/`var`. Type parameters use Scala 3 brackets — `[T]`, not `<T>` — because angle brackets are ambiguous with comparison and the bracket form is unambiguous to read and to parse.

**Blocks are always brace-delimited.** Vestra deliberately rejects Scala 3's optional-braces / significant-indentation syntax: a program's structure must be visible in its punctuation, not inferred from a column. Indentation is formatting; it is never grammar.

Statements are newline-terminated; a statement continues across a newline inside `()`/`[]`/`{}` or when a line ends — or the next begins — with a binary operator. `;` is an optional separator. A block evaluates to its final expression (Scala 3 / Rust); `return` is for early exit only. `if` and `match` are expressions.

```vestra
let area = if radius > 0.0 { pi * radius * radius } else { 0.0 }
```

Comments: `//` to end of line; `/* … */` block comments **nest**. Source is UTF-8.

---

## 4. Primitives, literals, bindings

Integers: `Int8`/`16`/`32`/`64`, `UInt8`/`16`/`32`/`64`, and pointer-sized `Int`/`UInt`. Floats: `Float32`, `Float64`. `Bool`. `Char` is a Unicode scalar value.

```vestra
let count   = 1_000_000
let mask    = 0b1010_0011
let address = 0xFFFF_0000
let ratio   = 1.0e-9
```

`let` binds immutably and `var` mutably, both at local scope; at module scope, `const` is an immutable, compile-time-evaluated binding and `static` is a fixed-address binding that — per §1 — may hold only an immutable value or an `Atomic`. A type annotation is optional where the right-hand side fixes the type.

Integer `+`/`-`/`*` **trap on overflow in debug and wrap in release**; the explicit `&+`/`&-`/`&*` always wrap, and `addingChecked` returns `(T, overflow: Bool)`. Integer division or remainder by zero traps. There are **no implicit numeric conversions**: `x.to[Int64]()` converts and is **checked** — it traps if the value will not fit the destination — while `x.toWrapping[UInt8]()` truncates without checking. `Int`↔`Float` conversion uses the same explicit `to`.

`as` reinterprets the bytes of two trivially-copyable types of identical size and alignment (`let f = bits as Float32`). It is rejected into an `enum` — use `Command.from(raw)`, which checks the discriminant and yields `Command?`.

There is **no null**. A possibly-absent value of any type, pointers included, is `T?` (§9).

**Text.** A string literal `"…"` is a **`StrConst`** — an immutable, UTF-8-validated string held in static storage. It owns nothing and allocates nothing, copies freely, and — because its bytes outlive the whole program — **escapes freely**: a `StrConst` may be returned, stored in an escaping value, or bound to a `const`. `Str` is the **borrowed** counterpart: an immutable, UTF-8-validated *view* into text owned elsewhere. A `Str` is non-escapable (§10), and a `StrConst` coerces to a `Str` wherever a borrowed view is wanted — so a `Str` parameter accepts a literal and a `String`'s contents alike, and `Str` is the type to write in a signature. `String` is the owned, growable counterpart, backed by `Buffer[UInt8]`; constructing or growing one needs `using Alloc`. A byte-string literal `b"…"` is a fixed `[N]UInt8` of raw bytes with no UTF-8 guarantee. `Char` is one Unicode scalar value; text is iterated over its `Char`s or its `UInt8`s explicitly — there is no hidden grapheme machinery.

A string literal *with* an interpolation — `"reg = \(value)"` — is **not** a `StrConst`. As a runtime expression it evaluates to a freshly-built **`String`**, so the enclosing function needs `using Alloc`: an interpolation is an allocation the signature names, never a hidden one, and that is the whole rule for its type. It lowers to a sequence of `Display` writes — the literal fragments verbatim, each `\(value)` as `value.display(into:)` — into the new `String`'s sink; every interpolated value must conform to `Display`, an ordinary protocol for which `derive(Display)` supplies the structural default (§12.3). In a `comptime` context where every fragment and interpolated value is compile-time-known, the interpolation folds to a `StrConst` instead — so a `const` may be defined with interpolation and stays allocation-free.

The no-allocator *runtime* path is the `Display`/`Sink` mechanism used directly, not an interpolation literal: `value.display(into: inout Sink)` renders a value into a caller-provided `Sink`, which may wrap a fixed stack buffer. A kernel formats this way — fragments and values written into a stack-backed `Sink`, with no `String` and no `Alloc` — accepting the explicitness in exchange for needing no heap.

---

## 5. Values, ownership, and exclusivity

Vestra has **value semantics with no surface references**. Every binding names a value; there is no `&T`. Mutation of a value owned elsewhere happens only through a parameter, and the parameter's mode is written in the signature. This is the whole memory model — there is no borrow-checker vocabulary to learn, because there are no references to track.

**Three parameter modes**, and a function states one per parameter:

| Mode | Meaning | At the call site |
|---|---|---|
| `read` (default — omit it) | Immutable view. Zero-copy. The caller keeps the value. | `f(x)` |
| `inout` | Exclusive mutable view. The caller keeps the value, mutated. | `f(&x)` |
| `sink` | Ownership transfers in. The caller's binding ends. | `f(x)` |

```vestra
func length(of v: [3]Float32) -> Float32 { … }          // read — borrows v
func normalize(_ v: inout [3]Float32) { … }              // exclusive mutation
func consume(_ buffer: sink Buffer[UInt8]) { … }         // takes ownership

normalize(&direction)
```

Method receivers use the same modes: a plain `func` reads `self`, an `inout func` mutates it, a `sink func` consumes it.

**Bindings are affine.** A value is used at most once as a `sink`; passing it to a `sink` parameter (or `return`ing it) ends the binding. To use a value again afterwards, write `copy` — and `copy` is only legal where the type permits it. The compiler reports a use-after-move with the exact move site. Implicit copies are inserted **only** for `Trivial` types — those the compiler proves are register-sized and resource-free (the integers, floats, `Bool`, small fixed structs of `Trivial` fields). Every other duplication is the written word `copy`, so a non-trivial copy is never a hidden cost.

```vestra
let a = makeBuffer()
let b = copy a       // explicit — Buffer is not Trivial
consume(a)           // a's binding ends here
consume(b)           // b is independent
// consume(a)        // error: `a` was consumed at line above
```

A type that owns a resource is **not copyable** at all (`Box`, `Buffer`, `File`); `copy` on it is a compile error, and it moves or is borrowed, never duplicated.

**The Law of Exclusivity**, checked statically and locally:

1. Any number of `read` accesses to a value may overlap, **or**
2. exactly one `inout` access may be live — never both.
3. Disjoint fields are independent: distinct fields of one struct may be accessed under different modes simultaneously, including through `embed` (§6).
4. The pieces of a **disjoint partition** of a `Buffer` or `Span` are independent in the same way: each piece may be held `inout` simultaneously, because they provably do not overlap. A partition is produced by the language's fixed **partition primitives** — `split(at:)` and `chunks(n)` on the blessed view and buffer types — or by any function whose result is built *by composing* those primitives, a provenance the compiler tracks (so a library's `Matrix.tiles()`, written from `chunks`/`split`, is itself a partitioner, its disjointness *derived* rather than asserted). A function that fabricates sub-views by raw index arithmetic is **not** a partitioner — its views obey the ordinary one-`inout` rule. This is the clause that makes data-parallel mutation (§11.2) expressible.

Overlapping an `inout` with any other access is a compile error naming both sites. Because there are no references, exclusivity is the *entire* aliasing story — there is nothing else that can alias.

**Lifetimes are inferred, never written.** A function returning a borrowed view (§10) ties the result's lifetime to its inputs, and the compiler rejects any use past that bound. When one non-escapable input could be the source, the result is tied to it; when several could be, the result is tied to their **intersection** — it is valid only as long as *every* non-escapable input is. That rule is a property of the signature, read off it with no annotation. Vestra has no lifetime-parameter syntax, and adds none here: an override naming one input would either restate what inference already proves or, if the function can return another input, be unsound.

**Linear types.** A type may be declared `linear`: a `linear` value *must* be consumed before its scope ends — dropping it is a compile error naming the leak. This is for resources where silent cleanup is wrong and the program must make an explicit decision (a raw capability handle, a half-open transaction). Ordinary resource types are merely affine: they have a `deinit` (§6) and are cleaned up at scope exit.

**Linearity is transitive through composition.** A `struct` with a `linear` field, and a generic container instantiated over a `linear` element — a `Buffer[T]` or `RingBuffer[T]` whose `T` is `linear` — is itself `linear`: it too must be drained or consumed before scope exit, never dropped. There is one rule, applied through every layer of composition, generic or not; a container of linear values cannot quietly leak them.

---

## 6. Structs, enums, opaque types, layout

```vestra
struct Transform {
    var position: [3]Float32
    var rotation: Quat[Float32]
    var scale:    [3]Float32

    func matrix() -> Matrix[4, 4, Float32] { … }
}

let t = Transform(position: [0, 0, 0], rotation: .identity, scale: [1, 1, 1])
```

A `struct` is a value type with a synthesized memberwise initializer. A struct may declare `deinit { … }`, run deterministically at scope exit (RAII). A `deinit` runs with **no capabilities** unless the type stores them — so a destructor cannot allocate or perform I/O it was not explicitly given, which keeps the one piece of compiler-inserted control flow honest. A type that declares a `deinit` is moved and destroyed only as a whole; its fields cannot be moved out individually, because the `deinit` needs the entire value.

**Enums** are sum types; `match` over them is exhaustive and is an expression:

```vestra
enum Shape {
    case circle(radius: Float32)
    case rect(width: Float32, height: Float32)
    case point
}

let area = match shape {
    case .circle(let r):       pi * r * r
    case .rect(let w, let h):  w * h
    case .point:               0.0
}
```

Patterns admit guards (`case .circle(let r) where r > 0:`), nesting, and slice patterns (`case [b'/', b'/', let rest...]:`). A missing case with no `default` is a compile error naming what is unmatched.

**`embed`** composes structs by flattening one struct's fields and methods into another's namespace. It is name-resolution sugar only — no layout change, no subtyping, no protocol pass-through:

```vestra
struct Entity {
    embed transform:  Transform
    embed kinematics: Kinematics
    var   health:     UInt32
}

let p = entity.position    // resolves to entity.transform.position
```

`embed` is a *distinct keyword* from the capability `using` (§8) on purpose: one keyword, one meaning.

**`opaque type`** is a zero-cost newtype — a distinct type over a backing representation, inheriting none of its operations:

```vestra
opaque type EntityId = UInt32
opaque type UserId   = UInt32
// EntityId and UserId never compare or convert without an explicit cast.
```

**Layout** is controlled by attributes: `@repr(C)` pins C-compatible field order and padding; `@repr(packed)` removes padding; `@repr(align(N))` over-aligns; `@repr(wire)` fixes byte order for cross-machine formats. Absent an attribute, the compiler chooses the layout (it may reorder fields to minimize padding). Struct-of-arrays is not an attribute — it is the `Soa[T]` library type (§13).

---

## 7. Functions, generics, protocols

```vestra
func rotate(_ point: [3]Float32, around axis: [3]Float32, by angle: Float32)
    -> [3]Float32 { … }

let p2 = rotate(p, around: [0, 1, 0], by: radians(45))
```

Every parameter has an **argument label**, defaulting to the parameter name; an external label is written before the name (`around axis`), and `_` omits the label. No declared return type means the function returns `Unit`.

**Generics** are monomorphized — always. A generic is parametric substitution and nothing more; there is no specialization, no coherence ceremony, no higher-kinded types. Bounds are protocol constraints; a `where` clause refines them.

```vestra
func max[T: Ord](_ a: T, _ b: T) -> T { if a > b { a } else { b } }

func sum[const N: Int](_ v: [N]Float32) -> Float32 { … }   // const generic
```

A generic parameter may be a **compile-time integer** (`const N: Int`) — this backs the fixed-vector length `[N]T` and library generics such as `Matrix[R, C, T]`.

**Protocols** are behaviour contracts. There is no inheritance; a protocol is the only abstraction over multiple types, and Vestra offers exactly three ways to use one — two static, one dynamic:

- **Generic bound** — `[T: Hashable]`. Monomorphized, statically dispatched, zero cost.
- **`some Protocol`** — an opaque type: one concrete type the compiler knows and the signature hides. Static, no witness table. The default for return-position polymorphism.
- **`dyn Protocol`** — an existential: a fat value that can hold different concrete types at runtime. This is the **only** dynamic dispatch in Vestra, and it is always written. A `dyn Protocol` value is **two words** — a data word plus a witness-table pointer; the data word holds a pointer-sized concrete value (a handle, a `Box`, a raw pointer), and a larger concrete type is named `Box[dyn Protocol]`. There is no inline-buffer existential — `Box` covers it, and one mechanism beats two.

```vestra
protocol Hashable { func hash(into: inout Hasher) }

func makeHasher() -> some Hasher { Fnv1a() }    // static
var sink: dyn Writer = consoleWriter            // dynamic, explicit
```

Conformance is declared in the owning package (`extension EntityId: Hashable { … }`); a conformance for a type and protocol the package owns neither of requires the audited `@retroactive`.

A protocol may declare **associated types** — type members a conforming type must supply:

```vestra
protocol Iterator {
    type Element
    inout func next() -> Element?
}
```

A type conforms once, fixing each associated type; `Self.Element` names it in the protocol's own signatures. Associated types are deliberately plain — no defaults, no bounds beyond a protocol constraint — because their only job is to let a protocol like `Iterator` name a type it does not itself choose. A protocol may also require another (`protocol Ord: Eq`): a conformer must satisfy both.

When a protocol with associated types is used as a `some`/`dyn` type, or as a generic bound, those associated types are **pinned by name** in the generic brackets — `dyn Iterator[Element = UInt8]`, `some Iterator[Element = Frame]`, `[T: Iterator[Element = UInt8]]` — exactly the way a type argument is supplied. A `dyn`/`some` of a protocol with an unpinned associated type is rejected: the compiler could not give `next()` a result type.

Operators are overloaded with a fixed, small set of `static func` declarations (`+ - * / %`, `& | ^ << >>`, `== <`, `subscript`). Operator functions are **pure** — they carry no `using` and no `throws`. Effects belong on named methods.

**Closures** are function values, written `{ params => body }`; a closure's type is the function type `(A, B) using C -> R`. A closure **captures by value** — copying a `Trivial` capture, moving a non-`Trivial` one (which ends the captured binding, exactly as a `sink` would). There is no by-reference capture, because Vestra has no references; a closure that must mutate caller state takes an `inout` parameter instead. A closure is therefore an ordinary value — move-only if it captured a move-only value — and may be stored, returned, or passed like any other. A closure *parameter* may instead be declared **non-escaping**: the receiving function may call it but never store, return, or spawn it. Because a non-escaping closure provably does not outlive the call, it may receive and capture non-escapable values — the affordance the library `parallel` (§11.2) rests on.

**`for` and iteration.** `for x in xs { … }` desugars to repeatedly calling `xs.next()` until it yields `.none`; `xs` is any value conforming to `Iterator`. An iterator is an ordinary `var` value whose `inout func next()` advances it — the compiler inlines a monomorphic one, so `for` over a concrete iterator *is* the hand-written loop. There is no `yield` and no generator coroutine: an iterator with interesting state is a `struct` you write. Iterating several sequences in step is the library combinator `zip` — `for (a, b) in zip(xs, ys)` — and lazy transformation (`map`, `filter`, `take`) is likewise a library of `Iterator`-wrapping iterators, not language machinery.

---

## 8. Capabilities — the `using` row

A capability is an authority a function needs from its caller: an allocator, a logger, a clock, a GPU queue, the ability to suspend. Every authority a Vestra function consumes flows through one mechanism — the `using` clause — and is **compile-time-resolved within one compilation**. The cross-program security primitive is a different thing — the VectraOS `Capability` (a typed, unforgeable, kernel-mediated handle, delegated across Rings) — and the two layers compose: the language proves authority is threaded correctly within one program; the kernel proves correct hand-off between programs.

```vestra
func parseConfig(_ path: Path)
    using Alloc, Log throws(ConfigError) -> Config
{
    let bytes = try readFile(path)
    Log.info("parsed \(path)")
    return build(bytes)
}

with Alloc = Arena(), Log = Console() {
    let config = try parseConfig(configPath)
}
```

- A `using` clause names every capability the function consumes. The capability name is **in scope as a value** inside the body (`Log.info(…)`) — there is no separate accessor syntax.
- `with Cap = value { … }` binds a capability for a lexical region. Resolution searches outward from the use site; the innermost binding wins; a missing binding is a compile error naming the capability.
- Capabilities propagate through call chains — **but every signature in the chain names what it consumes.** There is no ambient authority and no inheritance of environment. A function's `using` clause *is* its in-program security boundary, and `vestra audit` extracts it mechanically.
- An unused `using` capability is a warning.

**Async is a capability.** `async`/`await` are sugar: `async func` adds `using Async` to the clause, and `await e` is permitted exactly where `Async` is in scope. There is no separate function "colour" — an async function is simply one whose capability row contains `Async`, and it composes with every other capability by the same rule.

```vestra
async func fetch(_ url: Url) using Net throws(NetError) -> Buffer[UInt8] {
    let response = await Net.get(url)
    return await response.body()
}
```

**Row polymorphism** lets a higher-order function abstract over an unknown capability set:

```vestra
func map[T, U, C](_ xs: Span[T], _ f: (T) using C -> U) using C -> Buffer[U]
```

`C` is a compile-time-resolved capability set, inferred from `f` at the call site. This is the same `C` whether it carries `Async`, `Alloc`, or nothing — one mechanism, uniformly.

**Scope: one compilation.** A `using` row is a property of the signatures inside one program. When a value or call leaves the program — into another Task across a Ring, into a file, into a serialized message, into a plugin loaded at runtime — the row does not travel with it; the receiver's signature stands on its own. Authority that must cross the program boundary is carried by the VectraOS `Capability` primitive instead. §1's promise that *everything that matters is in the signature* holds for one program; cross-program composition is a runtime concern.

**No handlers, no `resume`.** This is the row-polymorphic context-passing shape that Koka and Eff popularized, with the algebraic-effects layer — user-defined handlers, `resume`, multi-shot continuations — deliberately cut. A capability is a value the caller supplies; it is not a continuation the callee captures. That is the comprehensibility ceiling, not a theoretical limit.

**Vocabulary.** *Capability* in this chapter is the `using` kind: compile-time-resolved, never stored, named in a signature. The VectraOS corpus uses *Capability* (capitalized) for the runtime, kernel-mediated handle — `RingCap`, `MemCap`, and so on — a different primitive at a different layer. Inside a Vestra program a runtime handle is **not** a second language mechanism: it is an ordinary value, usually `linear` (§5), moved and held like any other. Where this document writes *capability* unqualified, the `using` sense is meant; a runtime authority is always called a *handle*.

---

## 9. Errors and optionals

**Errors are values.** A fallible function returns `Result[T, E]`; `throws(E)` is sugar for it, and there is no untyped `throws` — every fallible function names its error type.

```vestra
enum Result[T, E] { case ok(T); case err(E) }

func readFile(_ path: Path) using Alloc throws(IoError) -> Buffer[UInt8] {
    if !exists(path) { throw IoError.notFound(path) }
    …
}
```

Three operators consume a fallible call: `try e` propagates the error (the enclosing function must declare a compatible `throws`); `try? e` converts to `Optional`; `try! e` panics on error. Inline handling is `do { … } catch e { … }`. A `throws(E)` function may call a `throws(E2)` one only when `E2` *is* `E`; a different error type is converted **explicitly** — `try someCall().mapError(toE)` — never silently. There is no implicit error widening, and no unwinding: `try` is an ordinary early `return` of the error value, visible in control flow.

The error type may itself be non-copyable, which is how a `sink` function hands a resource back on the failure path: the error carries the resource at its true state.

**Optionals.** `T?` is absence — and the *only* form of absence:

```vestra
let top = stack.last() ?? identity            // nil-coalescing
if let top = stack.last() { submit(top) }      // conditional binding
let name = user?.profile?.displayName          // optional chaining
let forced = stack.last()!                      // force-unwrap; panics on nil
```

`Optional` over a pointer-like type is **niche-optimized**: `.none` reuses the all-zero bit pattern, so `Ptr[T]?` is exactly one word and bit-identical to a C `T*`. `panic` is terminal — no unwinding, and therefore **no `deinit`s run**: a panic is an immediate, hard stop, and outstanding resources are reclaimed by the OS, or on bare metal not at all. Its behaviour (`abort`/`hang`/`reset`) is set once, per executable, in the manifest.

---

## 10. Heap and views

Heap allocation is always explicit and always goes through an `Alloc` capability — there is no implicit global heap.

| Type | Role |
|---|---|
| `Box[T]` | Unique-ownership heap pointer. Move-only, never null. `deinit` frees it. |
| `Buffer[T]` | Growable contiguous owned storage. `Buffer[T](fixed: n)` is non-growable. |
| `Span[T]` | `(ptr, len)` — a borrowed, read-only contiguous view. Non-escapable, zero-cost. |
| `MutSpan[T]` | `(ptr, len)` — a borrowed, mutable contiguous view; the `inout`-mode counterpart of `Span`. Non-escapable. |
| `StridedSpan[T]` | `(ptr, len, stride)` — a borrowed non-contiguous view. |
| `Arc[T]` | Atomically reference-counted shared ownership. **A type, not a mode.** |

`Arc[T]` is the entire story of shared ownership in Vestra. There is no managed dialect and no compiler-inserted reference counting: if a program shares ownership, it writes `Arc`, and the cost — a refcount and atomic traffic — is visible at the type. Code that does not write `Arc` pays nothing for it. `Arc` is the niche where reference counting is correct; it is not a language-wide tax.

A `Span` is non-escapable: it cannot be stored on the heap or returned past the lifetime of what it views, and the compiler infers and enforces that without a lifetime annotation. A struct that stores a `Span` is itself non-escapable, transitively. `Str` (§4) is a borrowed view and is non-escapable for the same reason; `StrConst`, whose bytes are in static storage, is *not* — it has unbounded lifetime and escapes freely.

Constructing a `Box` or `Buffer` requires `using Alloc`; dropping one frees through the allocator it was built with. Stdlib allocators — `Arena` (bump; bulk-frees on drop), `ObjectPool[T]` (fixed-size objects), `FixedAllocator` (a fixed backing buffer) — are ordinary capability values.

---

## 11. Concurrency

Vestra's concurrency is **structured and share-nothing**: there is no shared mutable state across tasks, hence no data races, hence no `Send`/`Sync` markers. That safety is not inferred — it falls out of rules already in the language. A value handed to another task is *moved* (§5's affine rule ends the sender's binding, so no alias survives); a borrowed view is *non-escapable* (§10, so it cannot be handed over at all); and shared ownership is the *explicit* `Arc[T]` (§10). Concurrency adds exactly two primitives over that foundation.

### 11.1 `spawn` — independent concurrent work

`spawn f(args)` runs `f` concurrently *with* the calling task and yields a `Future[T]`; `await` consumes the future for its result. `spawn` requires `Async`. A future cannot escape its spawning scope, and an un-awaited one is awaited or cancelled at scope exit.

```vestra
async func render(_ scene: Scene) using Alloc {
    with Async = Pool() {
        let shared = Arc(scene)                 // shared, read-only
        let culled = spawn cull(copy shared)    // each task gets its own handle
        let lit    = spawn light(copy shared)
        draw(await culled, await lit)
    }
}
```

A `spawn`ed task runs *while the parent continues*, so its closure must own everything it touches — it captures by value (§7), moving non-`Trivial` captures in. Data two tasks must both see is shared read-only through `Arc`, as `scene` is here; genuinely independent data is simply moved. A `spawn`ed closure may **not** capture a borrowed view — a `Span` is non-escapable, and the parent, still running, could race or invalidate it.

Tasks exchange owned values over the typed bounded `Channel[T]`: `send` takes its argument `sink`, so a sent value is moved out of the sender and nothing is left to alias.

`select` waits on several events at once — each an arm `on event:`, where `event` is a channel receive, a channel send-when-ready, a future, or a `timeout`. The first arm to fire wins: its body runs, and the *other* arms are re-armed, not consumed — a non-winning channel keeps its message. A receiving arm binds the delivered value `if let`-style — `on let msg = ch.recv(): handle(msg)` — while a `send` or `timeout` arm binds nothing. An optional `default` arm runs when no event is ready.

### 11.2 `parallel` — data parallelism over a partition

The one pattern `spawn` cannot express is *N workers each mutating a disjoint slice of one buffer* — the heart of physics, simulation, and numeric kernels. The concurrency library provides `parallel` for it:

```vestra
func step(_ world: inout Buffer[Particle], dt: Float32) using Async {
    parallel(over: world.chunks(8)) { slice in
        for p in 0 ..< slice.count { integrate(&slice[p], dt) }
    }
    // every worker has joined here; `world` is whole again.
}
```

`parallel` is an **ordinary library function**, not a keyword — it rests on two language affordances and needs nothing else:

- **A disjoint partition.** `world.chunks(8)` (and `split(at:)`) yield non-overlapping `MutSpan[Particle]` sub-views. The Law of Exclusivity (§5, clause 4) admits exactly this: the pieces of a *provably-disjoint partition* may be held `inout` at once, because they cannot alias. Overlapping chunks are not expressible.
- **A non-escaping body.** `parallel`'s `body` is a *non-escaping* closure parameter (§7) — `parallel` may call it, never store it. A non-escaping closure may receive non-escapable values, so each worker legitimately gets a borrowed `MutSpan`; and because the body cannot escape, `parallel` cannot ship it to a task that outlives `parallel`'s return — it is *forced* to be fork-join, joining every worker before returning.

The parent is therefore blocked for the whole parallel region, the workers cannot outlive it, and the sub-views provably do not overlap — and each of those facts is visible at the call site, in `parallel(over: chunks(8))`. Nothing is inferred.

### 11.3 What is deliberately absent

- **No region inference.** An earlier draft proposed inferring each value's reachable "region." It is *unnecessary* — §5's move rule and §10's non-escapability already prove message-passing safety — and *insufficient* — it could not express §11.2 at all. The two explicit primitives replace it.
- **No process or service supervision.** OS-level services, capability bundles, restart policies, and priority bands are VectraOS userland library APIs built from `spawn`, `Channel`, and `parallel`. A language does not grow an operating system inside its grammar.

### 11.4 No borrow across `await`

A borrowed view held live across an `await` is a compile error: values that persist across a suspension are stored in the task's state by value. There is consequently no self-referential task state, no pinning, and no projection machinery. (`parallel`'s workers borrow their slices, but a worker is straight-line work over its slice and never suspends inside the borrow.)

---

## 12. Compile-time execution and metaprogramming

Vestra is a small language, and a small language must give the developer a way to build what it and its libraries do not — or the smallness is a cage. That escape hatch is Vestra's metaprogramming, and it is **one facility**: the ordinary language, evaluated at compile time. There is no macro language, no template sublanguage, and no second way to do any of this.

Metaprogramming has four layers. **Reach for the lowest one that does the job** — each higher layer trades a little comprehensibility for a little more power, and the §2 local-reasoning test degrades gracefully and visibly as you climb.

### 12.1 Compile-time evaluation

A `comptime` block or `comptime func` runs in a sandboxed interpreter inside the compiler. It is **pure**: no environment, clock, or network; none of the unsafe capabilities (§14); the only input from outside the source tree is `@embed("path")`, which reads a manifest-declared, content-hashed file. Two builds of the same sources, under the same configuration (§12.6), are byte-identical.

```vestra
const sinTable: [256]Float32 = comptime {
    var t: [256]Float32 = .zero
    for i in 0 ..< 256 { t[i] = sin(Float32(i) * tau / 256.0) }
    t
}
```

`comptime if` and `comptime match` branch on compile-time values; selecting code *and declarations* by architecture, OS, or build option is the subject of §12.6.

### 12.2 Reflection

At compile time a **type is a value** — of type `Type` — and `comptime` code may introspect it: its kind (struct, enum, …), its fields (each a `Field` carrying `name`, `type`, `offset`, and its attributes), an enum's cases, a function's signature. Reflection is **read-only**.

```vestra
comptime func fieldNames(of t: Type) -> [String] {
    t.fields.map { f => f.name }
}
```

`Type` values are also *constructible* (`Type.structOf(fields:)`), which is the engine of generics: a generic type is, underneath, a `comptime func` returning a `Type`. Everyday generic code never touches this — §7's `[T]` is the comprehensible front door; type construction is the power-user layer behind it.

### 12.3 Reflective generic code — the `derive` layer

Most "metaprogramming" is not code generation at all. The operations a `derive` would generate — equality, hashing, cloning, debug-printing — are each written **once, as an ordinary generic function with a `comptime` body that reflects over the type**. No code is generated; there is nothing hidden to go and read.

A protocol may therefore supply a `comptime` **default implementation** that reflects over the conforming type. Declaring the conformance with an empty body adopts it:

```vestra
protocol Eq {
    func equals(_ other: Self) -> Bool

    // default: structural equality, by reflection over Self's fields
    comptime func equals(_ other: Self) -> Bool {
        for f in Self.fields {
            if !self[f].equals(other[f]) { return false }
        }
        return true
    }
}

extension Point: Eq {}              // empty body — adopts the reflective default
derive(Eq, Hash, Clone) for Point   // sugar for the three empty extensions
```

`derive(...)` is exactly that sugar — it is **not** a macro and emits no code; it states that a type conforms via each protocol's reflective default. `Eq`, `Ord`, `Hash`, `Clone`, `Default`, `Debug` ship this way, and a user protocol supplies its own default the same way. This layer covers the large majority of real metaprogramming, and it stays fully local: the behaviour is a function you can open and read.

### 12.4 Declaration macros — the escape hatch

Some needs genuinely require *generating new declarations*: named accessors over a packed word, boilerplate no generic function can express, a small embedded DSL. For these — and only these — Vestra has the **declaration macro**: a `comptime func` that receives the annotated declaration as a typed AST value and returns the declarations to replace it with.

A macro builds its output with **`quote`** — a typed, hygienic template — splicing values in at `$` holes:

```vestra
comptime func bitField(_ input: Decl) -> [Decl] {
    let backing = input.attribute("bitField").arg(0).asType()
    var body: [Decl] = quote { var raw: $backing }
    var offset = 0
    for f in input.fields {
        let width = (f.attribute("bits") ?? error(at: f, "field needs @bits(N)")).arg(0).asInt()
        let mask  = (1 << width) - 1
        body += quote {
            var $(f.name): $(f.type) {
                get { $(f.type).fromBits((raw >> $offset) & $mask) }
                set { raw = (raw & ~($mask << $offset)) | ((newValue.toBits() & $mask) << $offset) }
            }
        }
        offset += width
    }
    if offset != backing.bitWidth() {
        error(at: input, "@bits widths sum to \(offset); \(backing) holds \(backing.bitWidth())")
    }
    return quote { struct $(input.name) { $body } }
}

@bitField(UInt64)
struct PageTableEntry {
    @bits(1)  var valid: Bool
    @bits(44) var ppn:   UInt64
    // ... widths must sum to 64
}
```

`quote { … }` parses its body as Vestra and yields typed AST — `Expr`, `Stmt`, or `[Decl]` by context. `$x` splices a value into a hole, and the splice is type-checked: `$offset` must be an integer, `$(f.type)` a type, `$body` a `[Decl]`. There is no token-pasting and no string-building. `quote` is **sugar over a builder API** (`Decl.struct(…)`, `Expr.call(…)`, …), which stays directly available for code assembled programmatically rather than written as a shape — the loop above is the common mix: builders accumulate the `[Decl]`, `quote` writes each shape legibly. One mechanism, two faces.

Five rules keep macros comprehensible — every one is load-bearing:

- **A macro is the ordinary language.** It is a `comptime func`; `quote` is the only addition, and a `quote` body is ordinary Vestra parsed as Vestra — not a separate macro DSL.
- **Macros are hygienic.** A name written inside a `quote` resolves at the macro's *definition* site; a use-site name enters generated code only through an explicit `$` splice. A macro cannot accidentally capture or shadow.
- **Invocation is visible.** A macro runs only where its `@attribute` is written. You always see that generation happened, and which macro did it.
- **Output is re-checked.** The compiler type-, exclusivity-, and capability-checks generated declarations exactly as if hand-written. **A macro is a convenience, never a capability escalation — it can only produce code the developer could have written by hand.** It cannot reach past the §1 rule.
- **Output is inspectable.** `vestra expand` materializes a macro's result as ordinary Vestra source.

`quote` is in the language, not deferred, for a reason rooted in the thesis: a macro whose *output* you cannot read is off-thesis for Vestra, however few lines it takes. The danger associated with quasiquotes is the *untyped, token-pasting* kind (the C preprocessor, `macro_rules!` token trees); a *typed, hygienic, explicitly-spliced* quote is its opposite — it makes generated code visible *as code*, which is the comprehensibility win.

This remains the **one place** Vestra's local-reasoning test is not fully local: a line bearing a macro attribute requires reading the macro, or `vestra expand`. The cost is accepted — the alternative, a developer with no way to express what the language lacks, is worse — and bounded by the five rules: visible, hygienic, typed, re-checked, and tool-inspectable.

### 12.5 The principle

Climb only as high as the job demands: a `const` before a reflective function, a reflective function before a macro. Each layer down is more comprehensible; the top layer exists so that "the language does not have it" is never a dead end — only an invitation to write a `comptime func`.

### 12.6 Configuration and conditional compilation

A systems language must express that a declaration, a feature, or a branch exists only for certain architectures, operating systems, or build options. Vestra does this with **no preprocessor and no separate `cfg` predicate language** — conditional compilation is ordinary `comptime` branching on one compile-time value.

**The configuration value.** `cfg` is a `comptime`-known value, in scope everywhere, describing the build. The compiler always supplies its built-in criteria:

```vestra
cfg.arch         // Arch:    .x86_64 | .arm64 | .riscv64 | .riscv32 | .wasm32
cfg.os           // Os:      .vectra | .linux | .macos | .freestanding | ...
cfg.endian       // Endian:  .little | .big
cfg.pointerBits  // Int:     32 | 64
cfg.profile      // Profile: .debug | .release | a build-defined profile
```

`cfg` is an immutable compile-time constant — reading it is like reading a `const`, not ambient authority.

**Extensible criteria.** Beyond the built-ins, the package manifest *declares* a namespace of **features** (boolean) and **options** (typed values); the build system selects them, and `cfg.has("simd")` / `cfg.option("page_size")` read them. Because the namespace is declared in the manifest, a misspelled criterion is a compile error, not a silently-false test. This is the extension point: a build system — or a downstream consumer of the package — adds criteria by declaring them in the manifest; the language itself never changes.

**The predicate is an ordinary comptime expression.** A configuration test is a `comptime` `Bool` over `cfg` — `cfg.arch == .arm64 && cfg.has("neon")`. There is no `all`/`any`/`not` predicate vocabulary; those are `&&`/`||`/`!`. A predicate may reference only `cfg` and other compile-time constants.

**Two surfaces over the one mechanism:**

- **`comptime if` / `comptime match`** branch *code*, and at module scope *declarations*:
  ```vestra
  func barrier() {
      comptime match cfg.arch {
          case .arm64:              with Asm { asm(volatile: true) { "dmb sy" } }
          case .x86_64:             with Asm { asm(volatile: true) { "mfence" } }
          case .riscv64, .riscv32:  with Asm { asm(volatile: true) { "fence" } }
          case .wasm32:             {}   // ordering is the platform's
      }
  }
  ```
- **`@when(predicate)`** gates a single declaration — a `func`, `struct`, `extension`, or `import` — and is the findable, audit-able form. `@when(P) decl` is exactly a declaration-position `comptime if P { decl }`:
  ```vestra
  @when(cfg.os == .vectra)
  func mapPage(_ entry: PageTableEntry) using Mmio { ... }

  @when(cfg.arch == .x86_64) @repr(C)
  struct TrapFrame { ... }            // a separate @when(.arm64) TrapFrame exists
  ```

**Every branch is checked; only one is lowered.** This is what makes conditional compilation *sound* rather than rot-prone — and a stronger guarantee than C, Rust's `cfg`, or Zig's `comptime` give:

1. **Every branch of every configuration conditional is parsed and fully type-checked**, each in a configuration its predicate selects. Editing on x86-64 cannot silently break the ARM64 branch — a cross-platform file is checked, in one build, for every platform it claims to support.
2. **Only the branch matching the active build is lowered.** An unselected branch produces no instructions, no symbols, no cost.
3. **A reference is valid only where its target exists.** Un-gated code may name only declarations present in *every* configuration; code under `@when(P)` (or `comptime if P`) may additionally name declarations gated by `P` or weaker. A reference to a declaration absent from a configuration the conditional spans is a compile error naming that configuration.

`cfg` is part of the build's recorded inputs (§15), so conditional compilation does not weaken reproducibility: two builds with the same sources and the same `cfg` are byte-identical.

---

## 13. Numerics and SIMD

The fixed vector `[N]T` is a built-in primitive and *is* the SIMD type — there is no separate annotation. The same source lowers to vector instructions where the target has a unit of that width and to scalar lanes where it does not, so Vestra needs **no auto-vectorizer**: the developer writes the width, and what they wrote is what runs. `Matrix[R, C, T]` and `Quat[T]` are ordinary `core.math` library generics over `const` parameters; a module that does not import them does not pay for them.

### 13.1 The portable core

Every `[N]T` supports a fixed **portable set** — operations with a clean, universal scalar fallback, so identical source is correct on every target, vector unit or not:

- **Elementwise arithmetic and logic** — `+ - * / %`, `& | ^ << >>`, applied lane-wise.
- **Swizzles** — spatial accessors: `v.xyz` projects a `[4]T` to `[3]T`, `v.zyx` reverses, and so on.
- **Comparisons → masks** — `==`, `!=`, `<`, `>`, `<=`, `>=` on `[N]T` yield a lane mask of type `[N]Bool`.
- **Selection** — `select(mask, ifTrue, ifFalse)` picks each lane from one of two vectors by a `[N]Bool` mask: branch-free lane choice.
- **Reductions** — `.sum()`, `.product()`, `.min()`, `.max()` fold a vector to a scalar; `.any()`, `.all()` fold a `[N]Bool` to a `Bool`.
- **Saturating arithmetic** — `.saturatingAdd(other)`, `.saturatingSub(other)` clamp rather than wrap — the DSP and audio primitive.
- **Shuffles** — `v.shuffle[indices]()` permutes lanes, where `indices` is a `const` array of lane positions fixed at compile time.

```vestra
func clampHigh(_ v: [8]Float32, _ limit: [8]Float32) -> [8]Float32 {
    select(v > limit, limit, v)        // where v exceeds limit, take limit
}
```

Every operation in the portable core has a defined scalar lowering, so `[16]Int32` code runs correctly on a target with no 512-bit unit — slower there, never wrong.

### 13.2 The exotic border

An operation with **no clean, universal scalar fallback** — an AVX-512 mask-conflict instruction, hardware AES, a matrix accelerator — is deliberately *not* a `[N]T` method. It is an ordinary function in a library module (by convention `std.sys.intrinsics`, which holds no special compiler status), gated with `@when` (§12.6) and implemented with `asm` discharged under `Asm` (§14.4):

```vestra
@when(cfg.arch == .x86_64 && cfg.has("avx512f"))
func comparedEqualMask(_ a: [16]Int32, _ b: [16]Int32) -> UInt16 {
    with Asm {
        // Safety: vpcmpeqd has no memory effect; operands are register vectors.
        asm(volatile: false) { ... }   // lowers directly to `vpcmpeqd` → mask reg
    }
}
```

This is the deliberate line. The portable core *is the language* — checked, lowered, optimized, target-independent. The exotic border is *ordinary `@when`-gated library code*: there is no blessed intrinsics namespace and no compiler-magic intrinsic table — an exotic op is a normal function the language already lets you write, and a program that needs one names it explicitly, `@when`-gated to the architectures that actually have it.

### 13.3 Determinism

`@deterministic` on a function or module disables FMA contraction, FP reassociation, and reordering; transcendentals route to a vendored, correctly-rounded library. Output is then bit-identical across architectures at a fixed toolchain — required for lockstep simulation.

---

## 14. Systems programming and FFI

Vestra is built to write a kernel, so the metal is first-class. It has **no `unsafe` keyword**. The places where the compiler's guarantees are suspended are instead a small, fixed set of **capabilities** — the mechanism Vestra already uses for every other authority (§8). Unsafety is not a second system bolted onto the language; it is the same `using` mechanism applied to its last class of authority.

### 14.1 The unsafe capabilities

Four capabilities, fixed by the language, gate every operation the compiler cannot vouch for:

| Capability | Gates |
|---|---|
| `RawMemory` | dereferencing a raw `Ptr`/`MutPtr`; unchecked indexing |
| `Mmio` | constructing a `Volatile` register handle (§14.3) |
| `Asm` | an inline-assembly block (§14.4) |
| `Extern` | calling an `@extern("C")` function (§14.6) |

The compiler gates these operations on these capabilities exactly as it gates `await` on `Async` — no new rule-kind, only four more capabilities the compiler knows by name. They behave as every capability does (§8): a function performing a gated operation either **propagates** the capability — names it in its `using` clause, handing the obligation to its caller — or **discharges** it with a `with` block, and `vestra audit` requires a `// Safety:` justification at each discharge site.

```vestra
// Raw pointers carry no ownership, no bounds checks, and are never null —
// an absent pointer is `Ptr[T]?`. Dereferencing one is gated on `RawMemory`.
func loadWord(_ p: Ptr[UInt32]) -> UInt32 {
    with RawMemory {
        // Safety: `p` is a word-aligned address the caller guarantees live.
        p.load()
    }
}
```

The crucial property: an *undischarged* unsafe operation is **in the signature** (`using RawMemory`), so §1's rule — everything that matters is in the signature — holds; a *discharged* one is encapsulated, and its callers are not coloured, which is what lets `Buffer[T]` be raw internally and safe externally. A hidden `unsafe { }` block could express neither distinction.

### 14.2 Capability narrowing

A capability value may be **narrowed** — replaced by one carrying *strictly less* authority. Narrowing is the only non-minting way to obtain a capability, and it can only shrink, so authority can never be regained once given up.

```vestra
// Inside a broad `Mmio` grant, hand a driver one scoped to a single device:
with Mmio = Mmio.narrowed(to: MmioRegion(base: uartBase, size: 0x1000)) {
    let uart = Uart.at()        // reaches this region, and no other
}
```

`narrowed(to:)` traps if the requested sub-scope is not contained in the current one. The *structural* guarantee — a narrowed capability can never widen — is static; the *containment* check is a runtime precondition of the narrowing call. *Minting* a capability from nothing — `with Mmio = MmioRegion(base:size:)` built from raw integers — is the audited assertion `vestra audit` flags; every grant derived by narrowing needs no further audit. A boot layer thus mints once and hands each driver an un-widenable sub-grant. (Narrowing applies to any capability whose value carries a scope; `Mmio`'s scope is an address range.)

### 14.3 Memory-mapped I/O

`Mmio`'s value is an `MmioRegion` — `{ base, size }`. `Volatile[T]` is a register wrapper whose every read or write emits a real load or store, never elided or reordered. A `Volatile` is constructed within a granted region — `Volatile(in: Mmio, at: offset)` — and the construction checks the offset lies inside the region. Construction is the gated act; a constructed `Volatile` is a proven-valid handle, so `read`/`write` need no capability. `Mmio` therefore gates *minting* register handles, not their later use — a handle is durable, like a file descriptor returned by `open`. A `@volatile` struct field is the same guarantee applied to a field overlaying a device.

### 14.4 Inline assembly

An `asm` block carries explicit inputs, outputs, and clobbers; operand constraints are validated against the target ISA. It is gated on `Asm`.

```vestra
func disableInterrupts() {
    with Asm {
        // Safety: `cli` only clears the interrupt flag — always well-defined.
        asm(volatile: true) { "cli" }
    }
}
```

### 14.5 Interrupt handlers

`@interrupt func` emits the correct ISR prologue/epilogue; the compiler enforces that it takes no arguments, returns `Unit`, and calls nothing that allocates. Because an interrupt handler has **no caller**, it can neither receive a `using` capability nor be granted one from outside — every capability it needs, the unsafe ones included, must be discharged within its own body or the functions it calls. An ISR shares state with the rest of the kernel only through a `static Atomic` — which §1 permits precisely for this case — so a well-formed handler signals through an atomic and returns, leaving the deferred work to an ordinary task with ordinary capabilities.

### 14.6 C interoperability

`@repr(C)` types match the platform C ABI. `import c "header.h"` reads a C header and synthesizes Vestra declarations; a C `T*` imports as `Ptr[T]?`, so a possibly-null C pointer cannot be dereferenced without unwrapping. An `@extern("C")` function passes arguments in C-ABI registers with no marshalling — and **calling one is gated on `Extern`**, because a C function is outside every Vestra guarantee. A thin Vestra wrapper that discharges `Extern` and re-establishes the invariants is how a C library becomes a safe Vestra API. C++ interop is out of scope for v1.

---

## 15. Modules, ABI, and tooling

A **module** is the unit of compilation and namespacing; files share a module by declaring it (`module kernel.ring`). A **package** is a set of modules built, versioned, and ABI-editioned together, described by a manifest — which also declares the package's configuration namespace: the `feature` and `option` criteria that §12.6's `cfg` exposes. Visibility is four levels — `private`, `internal` (the default), `package`, `public`.

**ABI.** Two tiers: **Tier C** is the platform C ABI, for `@extern("C")` and `@repr(C)` items. **Tier V** is Vestra's native ABI for everything else — it extends Tier C with three rules: `using` capabilities append as pointer parameters in declaration order, `throws(E)` returns `Result[T, E]`, and `read`/`inout` parameters pass as pointers with compile-time-only lifetimes. Tier V is versioned per **ABI edition** and link-stable within one; every Tier V symbol carries its edition, and the linker rejects a mismatch.

**Tooling** is one CLI, `vestra`: `new`, `build` (incremental, bit-reproducible), `test`, `doc`, `fmt` (one canonical format — there is no style to argue about), `expand` (materialize a declaration macro's generated code as ordinary source, §12.4), and `audit`. `vestra audit` is the comprehensibility and security multiplier — it surfaces, mechanically, the properties the type system already guarantees: `audit --capabilities` prints each function's reachable capability set; `audit --unsafe` flags any discharge of an unsafe capability (§14) that lacks a `// Safety:` justification; `audit --abi` diffs the ABI against a baseline. The language makes the properties true; the tool makes them *visible*.

---

## 16. Deliberate omissions

What Vestra leaves out is as load-bearing as what it includes. Each of these was considered and cut, because its cost in comprehension exceeded its value, or because a smaller mechanism already covers it:

- **No second mode.** No `strict`/`managed` split. One language; `Arc[T]` is the only "managed" thing and it is a type. This removes mode-neutrality rules, cross-mode borrow rules, and the need to learn two semantics.
- **No rights algebra.** Typestate is valuable, but a union/intersection/subtraction/exclusive/polarity sublanguage is not core. Typestate falls out of the small primitives — `opaque type` + `linear` values + distinct return types express the open/closed-file pattern directly — and a richer typestate library is built on `comptime`, not on grammar.
- **No `@forExpansion`/`yield` iterator coroutines.** `for` iterates an `Iterator` protocol; the compiler inlines monomorphic iterators as ordinary optimization. Zero-cost iteration is a codegen property, not a language feature.
- **No inline-buffer existentials.** `dyn Protocol` and `Box[dyn Protocol]` cover dynamic storage; a byte-budgeted `any(N)` is one mechanism too many.
- **No second metaprogramming system.** One `comptime` facility, four layers (§12) — not Rust's split of declarative macros plus procedural macros, and not a macro DSL distinct from the language.
- **No `unsafe` keyword.** The four places the compiler's guarantees are suspended are ordinary capabilities — `RawMemory`/`Mmio`/`Asm`/`Extern` (§14) — so unsafety sits in the signature, propagates, and is audited like any other authority, never hidden in a block.
- **No preprocessor.** Conditional compilation by architecture, OS, and build option is `comptime` branching on the `cfg` value (§12.6) — typed, checked in every configuration it spans, and the same language as everything else. There is no `#ifdef` and no separate `cfg` predicate sublanguage.
- **No `Send`/`Sync`, and no region inference.** Cross-task safety is structural — values are *moved* between tasks, borrowed views are *non-escapable*, sharing is the explicit `Arc` (§11). There is no marker protocol to apply and no whole-program inference pass; data parallelism is the explicit `parallel` library function (§11.2).
- **No effect handlers, no exceptions, no GC, no inheritance, no null, no significant whitespace, no macro DSL.**

The test for re-admitting any of these is the §2 rule: does it let a developer answer the six questions *more* locally? If not, it stays out.

---

## 17. Grammar

This is the **entire** grammar of Vestra. Notation: `x?` optional, `x*` zero or more, `x+` one or more, `(…)` grouping, `|` alternation, `'…'` a literal token, `UPPERCASE` a lexical class. It fits on two pages by design — §2's budget made concrete.

### 17.1 Lexical

```ebnf
IDENT       ::= (LETTER | '_') (LETTER | DIGIT | '_')*
INT         ::= DIGIT (DIGIT | '_')*
              | '0x' HEX (HEX|'_')*  |  '0o' OCT (OCT|'_')*  |  '0b' BIN (BIN|'_')*
FLOAT       ::= DIGIT (DIGIT|'_')* '.' DIGIT (DIGIT|'_')* EXP?
              | DIGIT (DIGIT|'_')* EXP
STRING      ::= '"'  ( CHAR | ESCAPE | '\(' expr ')' )*  '"'
BYTESTRING  ::= 'b"' ( BYTE | ESCAPE )*  '"'
CHARLIT     ::= "'" ( CHAR | ESCAPE ) "'"
COMMENT     ::= '//' … <newline>   |   '/*' … '*/'          (block comments nest)
```

Source is UTF-8. A **newline ends a statement or declaration** unless it lies inside `()`/`[]`/`{}`, or the line ends — or the next begins — with a binary operator; `;` is an explicit separator. Whitespace and comments are otherwise insignificant.

Keywords: `module import func struct enum protocol extension opaque type embed deinit let var const static return if else match case default for in while break continue do catch throw throws with using async await spawn select on comptime quote copy read inout sink nonescaping some dyn linear try as derive true false self Self private internal package public`.

### 17.2 Compilation unit and declarations

```ebnf
file        ::= module import* declaration*
module      ::= 'module' dotted
import      ::= 'import' dotted  |  'import' 'c' STRING
dotted      ::= IDENT ('.' IDENT)*

declaration ::= attribute* visibility? ( func | struct | enum | protocol
                                       | extension | opaque | const | static | derive )
attribute   ::= '@' IDENT ('(' args? ')')?
visibility  ::= 'private' | 'internal' | 'package' | 'public'

func        ::= 'async'? recv-mode? 'func' IDENT generics? params
                effects? ('->' type)? where? block?
recv-mode   ::= 'inout' | 'sink'
generics    ::= '[' gparam (',' gparam)* ']'
gparam      ::= IDENT (':' bound)?  |  'const' IDENT ':' type
gargs       ::= '[' garg (',' garg)* ']'           -- generic *arguments*, at a use site
garg        ::= type | INT | IDENT '=' type        -- a type, a const, or an associated-type binding
params      ::= '(' (param (',' param)*)? ')'
param       ::= (IDENT | '_')? IDENT ':' 'nonescaping'? pmode? type ('=' expr)?
pmode       ::= 'read' | 'inout' | 'sink'
effects     ::= ('using' type (',' type)*)?  ('throws' '(' type ')')?
where       ::= 'where' expr

struct      ::= 'struct' IDENT generics? where? '{' s-member* '}'
s-member    ::= ('var' | 'let') IDENT ':' type
              | 'embed' IDENT ':' type
              | 'deinit' block
              | attribute* func
enum        ::= 'enum' IDENT generics? '{' e-case* s-member* '}'
e-case      ::= 'case' IDENT ('(' (payload (',' payload)*)? ')')?
payload     ::= (IDENT ':')? type
protocol    ::= 'protocol' IDENT generics? (':' type (',' type)*)? '{' p-member* '}'
p-member    ::= 'type' IDENT (':' bound)?                       -- associated type
              | 'async'? recv-mode? 'func' IDENT generics? params effects? ('->' type)?
              | 'comptime' func                                 -- reflective default
extension   ::= 'extension' type (':' type (',' type)*)? where? '{' s-member* '}'
opaque      ::= 'opaque' 'type' IDENT '=' type
const       ::= 'const'  IDENT (':' type)? '=' expr
static      ::= 'static' IDENT ':' type '=' expr
derive      ::= 'derive' '(' type (',' type)* ')' 'for' type
```

### 17.3 Types

```ebnf
type        ::= dotted gargs?                               -- Buffer[Int], dyn Iterator[Element = UInt8]
              | '[' (INT | IDENT) ']' type                  -- vector  [N]T
              | type '?'                                    -- optional
              | '(' (type (',' type)*)? ')' effects? '->' type   -- function type
              | '(' type (',' type)+ ')'                    -- tuple
              | 'some' type  |  'dyn' type
bound       ::= type ('&' type)*                            -- conjoined protocol bounds
```

### 17.4 Statements

```ebnf
block       ::= '{' statement* '}'
statement   ::= ('let' | 'var') pattern (':' type)? '=' expr
              | expr
              | 'return' expr?
              | 'break' IDENT?  |  'continue' IDENT?
              | (IDENT ':')? 'for'   pattern 'in' expr block
              | (IDENT ':')? 'while' expr block
              | 'with' w-bind (',' w-bind)* block
              | 'do' block ('catch' pattern? block)+
              | lvalue assign-op expr
w-bind      ::= type ('=' expr)?            -- '=' omitted for a marker capability
assign-op   ::= '=' | '+=' | '-=' | '*=' | '/=' | '%=' | '&=' | '|=' | '^=' | '<<=' | '>>='
```

### 17.5 Expressions

```ebnf
expr        ::= literal | path
              | expr gargs? '(' args? ')'                   -- call
              | expr '.' IDENT                              -- member
              | expr gargs? '[' args ']'                    -- subscript / shuffle
              | unop expr  |  expr binop expr
              | expr ('as' | 'as?') type
              | 'if' expr block ('else' (expr | block))?
              | 'match' expr '{' arm+ '}'
              | 'select' '{' select-arm+ ('default' ':' (expr | block))? '}'
              | '{' (IDENT (',' IDENT)* '=>')? statement* '}'    -- closure
              | 'comptime' (block | expr)  |  'quote' block
              | ('try' | 'try?' | 'try!') expr
              | 'await' expr  |  'spawn' expr  |  'copy' expr  |  'throw' expr
              | type '(' (arg (',' arg)*)? ')'              -- struct literal
              | '[' (expr (',' expr)*)? ']'                 -- vector / array literal
              | '(' expr ')'
literal     ::= INT | FLOAT | STRING | BYTESTRING | CHARLIT | 'true' | 'false'
path        ::= IDENT | 'self' | dotted | '.' IDENT         -- '.IDENT' = leading-dot inference
arg         ::= (IDENT ':')? '&'? expr                      -- '&' marks an inout argument
arm         ::= 'case' pattern ('where' expr)? ':' (expr | block)
              | 'default' ':' (expr | block)
select-arm  ::= 'on' ('let' pattern '=')? expr ':' (expr | block)   -- §11.1; binding optional
```

A bare `{ … }` in expression position is a closure; after `if`/`while`/`for`/`with`/`comptime`/`func`/`deinit` it is a block — the keyword disambiguates. Inside a `quote` block, `$IDENT` and `$(expr)` are splice points (§12.4).

### 17.6 Patterns

```ebnf
pattern     ::= '_' | literal | 'let' IDENT | IDENT          -- wildcard, literal, bind, match
              | '.' IDENT ('(' (pattern (',' pattern)*)? ')')?       -- enum case
              | type '(' (IDENT ':' pattern (',' IDENT ':' pattern)*)? ')'  -- struct
              | '(' pattern (',' pattern)+ ')'               -- tuple
              | '[' (pattern (',' pattern)*)? ('let' IDENT '...')? ']'   -- slice
```

### 17.7 Operator precedence (tightest first)

```
copy   -unary  !  ~          (prefix)
as  as?
*  /  %
+  -
<<  >>
&
^
|
..  ..<                      (range)
==  !=  <  <=  >  >=
&&
||
??
```

Assignment is a statement (§17.4), not an expression — it does not nest. `if` and `match` are expressions; an `if`/`match` written as a statement is simply an expression statement.

---

## 18. Standard library surface

The library is **sketched, not fully specified** — the purpose is to fix the shapes the rest of this document refers to. A type tagged *(compiler-known)* is on the §2 blessed list; everything else is ordinary Vestra the bootstrap compiler could have written.

### 18.1 Core protocols

```vestra
protocol Eq         { func equals(_ other: Self) -> Bool }            // comptime default
protocol Ord: Eq    { func compare(_ other: Self) -> Ordering }
protocol Hash       { func hash(into: inout Hasher) }
protocol Clone      { func clone() -> Self }
protocol Default    { static func default() -> Self }
protocol Display    { func display(into: inout Sink) }                // user-facing (§4)
protocol Debug      { func debug(into: inout Sink) }                  // developer-facing
protocol Iterator   { type Element;  inout func next() -> Element? }

enum Ordering { case less, equal, greater }
```

`Eq`, `Ord`, `Hash`, `Clone`, `Default`, `Debug`, `Display` each ship a `comptime` reflective default (§12.3); `derive(Eq, Hash, …) for T` adopts them. `Hasher` accumulates a hash; `Sink` is the byte sink `Display`/`Debug` write into.

### 18.2 `Optional` and `Result` — *(compiler-known)*

```vestra
enum Optional[T]  { case some(T); case none }       // spelled `T?`
enum Result[T, E] { case ok(T); case err(E) }       // `throws(E)` sugar (§9)
```

`Optional`: `?.`, `??`, `if let`, `!`, and `.take()` (moves the value out, leaving `.none`). `Result`: `try`/`try?`/`try!`, and `.mapError(_ f)`.

### 18.3 Owned containers

```vestra
struct Box[T]                                      // (compiler-known) unique heap pointer
    static func new(_ value: sink T) using Alloc -> Box[T]

struct Buffer[T]                                   // (compiler-known) growable storage
    static func empty()               using Alloc -> Buffer[T]
    static func withCapacity(_ n: Int) using Alloc -> Buffer[T]
    static func fixed(_ n: Int, repeating: T) using Alloc -> Buffer[T]
    inout func append(_ x: sink T)
    inout func removeLast() -> T?
    func count() -> Int
    func span() -> Span[T]
    inout func mutSpan() -> MutSpan[T]
    func chunks(_ n: Int) -> some Iterator         // n disjoint MutSpan pieces (§5.4, §11.2)
    subscript(_ i: Int) -> T                       // an `inout subscript` allows mutation

struct Arc[T]                                      // (compiler-known) shared ownership
    static func new(_ value: sink T) using Alloc -> Arc[T]
    // `copy` bumps the refcount; the last drop frees.
```

### 18.4 Views — *(compiler-known)*

```vestra
struct Span[T]        // borrowed, read-only, contiguous; non-escapable
struct MutSpan[T]     // borrowed, mutable,  contiguous; non-escapable
struct StridedSpan[T] // borrowed, non-contiguous; non-escapable
```

Each: `count()`, `subscript`, `slice(_ r: Range)`. `MutSpan` adds `split(at:)` and `chunks(_ n:)`, the trusted partitioners of §5 clause 4.

### 18.5 Collections

```vestra
struct HashMap[K: Hash & Eq, V]      // open-addressed
struct HashSet[T: Hash & Eq]
struct TreeMap[K: Ord, V]            // ordered iteration — for reproducible dumps
struct BitSet                        // backed by Buffer[UInt64]
```

Each sketches `insert`, `get -> V?`, `remove -> V?`, `contains`, `count`, and an `Iterator`.

### 18.6 Text — *(compiler-known)*

```vestra
struct StrConst  // immutable UTF-8 in static storage; the string-literal type (§4); escapes freely
struct Str       // borrowed, UTF-8-validated view; non-escapable; a StrConst coerces to it
struct String    // owned, growable UTF-8; construction/growth needs `using Alloc`
struct Char      // one Unicode scalar value
```

`StrConst` and `Str` share `bytes() -> Span[UInt8]`, `chars() -> some Iterator`, and `len()`; slicing a `StrConst` yields a `StrConst` (still static-backed), slicing a `Str` yields a `Str`. `String`: `push(_ c: Char)`, `append(_ s: Str)`, `asStr() -> Str` — a borrowed, non-escapable view. Interpolation `\()` lowers to `Display` (§4).

### 18.7 Iteration

`Range` (`a..b`, `a..<b`) is an `Iterator`. Combinators are `Iterator`-wrapping iterators — `map`, `filter`, `take`, `skip`, `enumerate`, `zip`, `chain`; `for` consumes any `Iterator` (§7). None is a language feature.

### 18.8 Numerics — `core.math`

```vestra
struct Matrix[const R: Int, const C: Int, T]
struct Quat[T]
```

Library generics over `const` parameters. The `[N]T` vector and its portable operations are the *language* (§13); `Matrix`/`Quat` arithmetic is built from them. A module that imports neither pays for neither.

### 18.9 Concurrency

```vestra
struct Future[T]                                   // (compiler-known) a `spawn` result
struct Channel[T]                                  // (compiler-known) bounded queue
    static func bounded(_ capacity: Int) -> Channel[T]
    inout func send(_ x: sink T) -> Result[Unit, T]
    inout func recv() -> T?

// the §11.2 data-parallel fork-join — an ordinary library function
func parallel[T, C](over: some Iterator,
                    body: nonescaping (MutSpan[T]) using C -> Unit) using Async, C

protocol Executor { … }
struct Pool        : Executor    // work-stealing, multi-core
struct Cooperative : Executor    // single-threaded, cooperative
```

`spawn`, `await`, and `select` are language forms (§11); `Future`, `Channel`, `parallel`, and the executors are library.

### 18.10 Synchronization

```vestra
enum MemoryOrder { case relaxed, acquire, release, acqRel, seqCst }

struct Atomic[T]                                   // (compiler-known) — the one
    func load(order: MemoryOrder) -> T             //   shared-mutable primitive
    func store(_ v: T, order: MemoryOrder)
    func exchange(_ v: T, order: MemoryOrder) -> T
    func compareExchange(expected: T, desired: T, …) -> Bool

struct Mutex[T]      // built on Atomic; `lock` yields exclusive access to the T
struct RwLock[T]
```

### 18.11 Allocators

`Alloc` is a capability (§8); allocators are capability values:

```vestra
struct Arena          // bump allocator; bulk-frees on drop, or reset() to reuse
struct ObjectPool[T]  // fixed-size object pool
struct FixedAllocator // a fixed backing buffer; traps on exhaustion
```

### 18.12 System — *(compiler-known)*

```vestra
struct Ptr[T]         // raw, read-only pointer;  deref gated on `RawMemory` (§14)
struct MutPtr[T]      // raw, read-write pointer; deref gated on `RawMemory`
struct Volatile[T]    // register handle; constructed within an `Mmio` region (§14.3)
struct MmioRegion     // { base, size } — the value of the `Mmio` capability
```

---

## 19. Formal semantics

§17 fixed what is *well-formed*; §19 fixes what well-formed code *means*. It is the dynamic semantics, stated rigorously but not as a mechanized operational calculus — the goal is an unambiguous contract for the implementation, still readable in one sitting. Where §5–§16 give a rule in prose, this section is its precise restatement; where they appear to differ, §19 governs.

### 19.1 The execution model

A Vestra program is **eagerly evaluated** and **value-semantic**. A *binding* — a `let`/`var`, a parameter, a field — names a *value*, and a value is a tree of values: there are no references, so the runtime state is a forest of value trees, never a graph. A *storage location* is anything a value lives in: a binding, a struct field, a vector lane, the cell a `Box` or `Buffer` owns. Mutation reaches a location only through an `inout` access (§19.3).

A value is **trivial** if the compiler proves it register-class and resource-free; otherwise it owns resources. That distinction is the only one the cost model turns on (§5).

### 19.2 Bindings: initialization, move, copy

Every binding has, at each program point, a state — **uninitialized**, **live**, or **consumed** — tracked by a flow analysis:

- A binding is *uninitialized* until assigned; **using an uninitialized binding is rejected** (definite assignment).
- Assignment makes it *live*; reassigning a `var` returns a *consumed* binding to *live*.
- Passing a binding to a `sink` parameter, `return`ing it, or otherwise moving it makes it *consumed*. **Using a consumed binding is rejected**, and the diagnostic names the move site.

`copy x` reads `x` — it does not consume it — and yields a fresh, independent value; it is well-formed only where `x`'s type is `Copyable`. A trivial value is copied implicitly at each use; a non-trivial value is duplicated only by an explicit `copy`; a resource-owning, non-`Copyable` value is never duplicated.

### 19.3 The Law of Exclusivity

For a storage location `L`, consider the accesses to `L` that are *live* at a program point, each classified `read` (shared) or `inout` (exclusive). The law: **at every program point the live accesses to `L` are either all `read`, or exactly one `inout`.** A violation is a compile error naming both access sites.

Distinct fields of a struct are distinct locations; so are the pieces of a *disjoint partition* of a `Buffer`/`Span` produced by the fixed partition primitives or their compositions (§5 clause 4) — the compiler accepts disjointness from that *provenance*, never having to prove arbitrary index arithmetic disjoint. Because there are no references, the set of accesses to `L` is determined statically and locally — exclusivity is decidable without whole-program analysis.

### 19.4 Escapability and lifetime

A value is **non-escapable** if it is a `Span`, `MutSpan`, `StridedSpan`, or `Str`, or a struct that transitively stores one. (`StrConst` is escapable — its bytes are in static storage, so it has unbounded lifetime.) A non-escapable value `v` is *derived from* a source location `s`, and its lifetime is bounded by `s`'s: `v` may not be stored in the heap, returned past `s`'s scope, placed in a `Channel`, or captured by an escaping closure. The source is inferred from the expression that produced `v`; there is no lifetime syntax, and the bound is checked locally at each use. When a function returns a non-escapable value and several non-escapable inputs could be its source, the result's lifetime is the **intersection** of those inputs' — valid only while every one of them is (§5). That bound is fixed by the signature; no annotation selects among the inputs.

### 19.5 Destruction

At a scope's exit, its still-*live* bindings are destroyed in **reverse declaration order**. A *consumed* binding is not destroyed — its value was moved out. Destroying a value runs its `deinit` if it has one (with only the capabilities the type stores, §6), then destroys its fields in reverse declaration order. A type that declares a `deinit` is moved and destroyed only as a whole. A `panic` runs **no** destructors — it is an immediate, terminal stop (§9).

### 19.6 Linearity

A `linear` value must be **consumed on every control-flow path** before the scope holding it ends — moved to a `sink` parameter, returned, or destructured by a `sink` operation into its fields. There is no generic dropper: "destroying" a `linear` value is not a separate mechanism but calling whatever `sink` consumer its type provides. A `linear` type with *no* consumer is a design error in that type, and the language deliberately offers no escape hatch — that is the discipline `linear` exists to enforce. Reaching a scope exit with a `linear` binding still *live* is a compile error naming the leak. Linearity is transitive: a value composing a `linear` value — generic containers included — is itself `linear` (§5).

### 19.7 Capability resolution

A `using` capability is resolved **statically**, at each use site, to the innermost enclosing `with` binding of its type; an unresolved capability is a compile error. Capabilities are not part of a value's runtime representation — Tier V passes them as hidden parameters (§15). An operation gated on an unsafe capability (§14) is well-formed only where that capability resolves; `async` *is* the capability `Async`, and `await` is well-formed only where `Async` resolves.

### 19.8 Concurrency and the memory model

Tasks share no mutable state, so the only inter-task ordering edges are these:

- `Channel.send(v)` **happens-before** the `recv()` that receives `v`.
- `spawn f` happens-before the first statement of `f`'s body; `f`'s body happens-before the `await` that consumes its `Future`.
- A `parallel` call's fork happens-before every worker body; every worker body happens-before the call's return (§11.2).
- `Atomic` operations are ordered by their `MemoryOrder` (§18.10).

**Data-race freedom — the guarantee.** No two tasks ever access one storage location with at least one access a write, unordered by the edges above. It holds by construction: a value handed to another task is *moved* (§19.2 leaves the sender no live binding, hence no alias); `parallel`'s workers receive a *disjoint partition* (§19.3 — distinct locations); and any genuinely shared cell is an `Atomic`, or `Arc` over immutable data. There is nothing to mark `Send`/`Sync` — the hazard is absent, not annotated.

### 19.9 Evaluation order

Expressions evaluate eagerly, left to right; a call's arguments evaluate left to right before the call runs. `if` and `match` evaluate only the selected branch; `&&`, `||`, and `??` short-circuit. A `comptime` expression evaluates at compile time, in the §12 interpreter, before any of the above. Integer overflow traps in debug builds and wraps in release (§4); a trap is a `panic`.

### 19.10 Soundness — the guarantees

For every program Vestra accepts, a conforming implementation must uphold:

- **Memory safety** — no use-after-free; no use of uninitialized or consumed storage (§19.2); no out-of-bounds access (a bound is checked, or the access lies within a verified partition); no null dereference (there is no null).
- **Data-race freedom** — §19.8.
- **No use-after-move** — §19.2.
- **No leaked linear resources** — §19.6.
- **Capability safety** — a function performs only the effects its `using` row names; there is no ambient authority.
- **Reproducibility** — a `comptime` evaluation, and therefore the whole build, is a pure function of the sources and the configuration `cfg` (§12.1, §12.6).

These are obligations, not aspirations: a program that *could* violate any of them is, by definition, not well-formed, and the compiler must reject it. §17 says what Vestra *is*; §19 says what it *guarantees*.

---

*Draft 3 added the formal dynamic semantics (§19); Draft 3.1 closed an external review's findings — associated-type constraints in the type grammar (§7, §17), a precise `select` (§11.1, §17.5), and the rules for multi-input lifetimes (§5, §19.4), linear destruction (§19.6), and trusted partitioners (§5, §19.3). Draft 3.2 split the string-literal type — `StrConst` (immutable text in static storage, escapes freely) is now distinct from the borrowed, non-escapable view `Str` (§4, §10, §18.6, §19.4) — and fixed the result type of an interpolation expression to `String` (§4). With the grammar (§17), the library surface (§18), and the semantics (§19) in place, the specification is complete enough to implement against; the self-hosting bootstrap follows the existing `compiler/` toolchain, not this document. The acceptance test is unchanged from the first line: a working systems programmer can read the whole specification in an afternoon and thereafter predict, from any line of code, exactly what it does — and §19 is the precise statement of that prediction.*
