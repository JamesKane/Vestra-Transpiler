# Roadmap

Forward-looking menu of candidate phases for the Vestra-to-C++26
transpiler. The *back-looking* counterpart — what's shipped, with
commit references and known v0.5 limitations — is in
[`HANDOFF.md`](HANDOFF.md). The spec is
[`VESTRA_DRAFT.md`](VESTRA_DRAFT.md).

Each candidate below is sized for a single end-to-end session: AST →
parser → sema → codegen → unit tests → e2e. **Tier S** items unblock
several follow-ons or fill obvious gaps in already-shipped phases;
**Tier A** items are clean wins on their own; **Tier B** items are
bigger swings or depend on more groundwork.

The user's working pattern: open with **"let's start the next phase"**;
I respond with `AskUserQuestion` containing **four** options drawn
from this list, plus any fresh candidates the most recent phase opened
up.

---

## Tier S — closes a current gap

### Mid-expression `try` inside if/match arms

Today `(try f() if cond else try g())` falls back to `.value()` (panic
on error) because the hoist pass in `e3ad4ec` refuses to descend into
conditional contexts. The right fix is per-arm hoist contexts: each
branch gets its own pre-emitted let-binding scope, and the IIFE's
return-position handler stitches them together. Already-shipped pieces
to lean on: `emit_stmt_expr`, `collect_try_hoists`.

### Nested tuple destructuring at codegen

`let ((a, b), c) = (...)` already type-checks (`bind_tuple_pattern`
recurses through `TuplePat`) but the codegen only binds the outer
element. C++ structured bindings are single-level — fixing this needs
a sibling-statement hoist:

```cpp
auto [__t0, c] = std::tuple{std::tuple{10, 20}, 30};
auto [a, b]   = __t0;
```

Plumb it into the LetStmt / VarStmt / ForStmt destructuring sites.

### Tuple patterns at function-param / match-arm positions

Tuple destructuring only fires inside `let` / `var` / `for` today.
`func f((a, b): (Int32, Int32)) -> Int32` is a parser-only stretch;
match-arm payload destructuring (`case .pair(let (a, b)):`) is the
deeper one. Both reuse `bind_tuple_pattern` once they land in the
right spot.

### `Optional` in a Display splice

`is_display_conformant` rejects `Optional` today (no standard
`std::format("{}", std::optional<T>)`). The clean fix is a helper
shim in the preamble that delegates to `T`'s formatter on .some and
prints `nil` on .none. Tiny.

### `derive(Default)` + `Default` protocol

§12.3 lists Default alongside Eq / Hash / Clone / Debug. `derive(Default)
for T` would emit a static `T.default() -> T` returning the
zero-initialised value, surfaced through the same `decl_derives`
pathway as Clone. Useful for container ergonomics later.

---

## Tier A — bounded next-phase candidates

### `.mapError(_ f)` on Result

§9 spelling for explicit error-type widening between throws functions:
`try someCall().mapError(toE)`. Sema recognizes `.mapError` as a
built-in Result method; codegen lowers via std::expected's
`transform_error`. Pairs well with multi-typed do/catch.

### `Span[T]` — non-owning view (§10)

The borrowed, non-escapable view type. Lowers to `std::span<T>`.
Letting a function take `Span[T]` over a Box[T] / fixed vector
without copying or transferring ownership is the immediate use. Also
the first **non-escapable** type in the system — opens the door to
the §5 escape rules later.

### `panic` / `abort` / `unreachable` lowering (§10)

§10 says panic is terminal — no unwinding, no `deinit`. Lower
`panic(msg)` to `std::abort()` (or `std::terminate()`), and the spec's
`abort` / `hang` / `reset` manifest setting could pick the lowering.
Today force-unwrap relies on `std::bad_optional_access` — a real
`panic` lowering would unify on a single mechanism.

### `with X = expr { ... }` block satisfying a capability

The capability checker already recognizes `with` as a satisfier, but
there's no actual sema/codegen for opening a `with` block whose value
is bound for the body's duration. Tightening this means proper
capability narrowing later (`Mmio.narrowed(to:)`).

### Newtype `opaque type` follow-ons

Two small wins on top of `d7e7589`:
- A `derive(Eq, Hash, Debug, Clone, Display) for Q` story for opaque
  newtypes. The codegen path is mostly there for the underlying T;
  needs a static_cast through Q to reuse it.
- Conversion-call `T(q)` form (the inverse of `q.value`) so the user
  has both directions written the same way.

### `do { ... } catch e where guard { ... }`

§9 hints at guarded catch arms (`case` already has `where`). Mirror
the syntax + sema in do/catch so the user can dispatch on the error
value without writing an inner match.

### Pattern matching enhancements

The match codegen handles bare and payloaded enums; sema's
exhaustiveness check is in. Missing pieces:
- **Range patterns**: `case 0..10: …` in match arms.
- **Or-patterns**: `case .red | .green: …`.
- **Tuple patterns inside `case`**: `case (1, _): …`.
None of these are huge individually — they're all in `parse_pattern`
+ `check_pattern`.

### Generics phase 2

`b9583c8` covers function generics. Missing:
- Const generics (`[const N: Int]`).
- Generic structs / enums (`struct Box[T] { ... }` user-defined).
- Where-clause refinement (`func f[T](x: T) where T: Eq -> Bool`).
- Protocol-bound enforcement (a generic parameter constrained to a
  protocol must actually satisfy it at the call site).

---

## Tier B — bigger or depends on groundwork

### `async` / `spawn` / `select` / `parallel` (§11)

Today parsed but emitted as `unsupported` comments. The C++26 target
has `std::execution` and `senders/receivers` available; mapping
async/await to those is the structurally clean path. `spawn` →
detached sender. `select` → `let_value` composition. `parallel` →
`bulk` or a custom partitioner. This is a big phase — probably 2–3
sessions to do properly.

### `Channel[T]` + `parallel` library (§11)

Once async lands, `Channel[T]` is a typed bounded queue. `send` is
sink, `recv` returns Optional. `parallel(over: xs, by: chunks)` takes
a non-escaping closure and runs it over partitions. Library on top
of the async runtime.

### Ownership / exclusivity phase 2

Phase 1 is single-pass and linear. Phase 2 needs:
- **Branch-aware flow merging** — a binding moved in one if-arm but
  not the other is "maybe-moved" after the if; reading it is an
  error, but reassignment revives.
- **Cross-statement borrow liveness** — exclusivity today is
  per-call; widening to per-statement (and beyond) catches more
  conflicts.
- **Partition primitives** — `chunks(n)` / `split(at:)` need to be
  recognized as producing disjoint slices.
- **Linear types** (`linear struct Foo { ... }`) — types whose
  values can't be discarded without an explicit consumer (e.g. a
  destructor consume).

### Capability narrowing + audit trail

Phase 2 capability work:
- **Row polymorphism** over a generic capability variable so
  `func gen[T, using C](x: T) using C` actually parameterizes over
  capabilities.
- **Narrowing** (`Mmio.narrowed(to:)`) so a wide capability can be
  refined for a sub-call.
- **`// Safety:` audit trail** for `unsafe` discharges — gates real
  unsafe ops once they exist (Ptr.load → RawMemory, asm → Asm,
  etc.).

### SIMD `[N]T` lowering (§13)

Map fixed-length vector types to `std::experimental::simd` (or target
intrinsics) when available, clean scalar fallback elsewhere. Needs a
target-detection layer the codegen doesn't have yet.

### Content-hashed `@embed` manifest

Today `@embed("path")` reads files relative to the source dir at fold
time. The spec wants paths resolved against a content-hashed
manifest so two builds of the same source under the same config are
byte-identical. Needs a manifest file format + a hash compare in the
driver.

### Quote / splice / declaration macros (§12.4)

The parser already accepts `quote { ... }`. Wiring it through to
declaration-position macros (with `$splice` and `expand`) is the
metaprogramming pillar of §12.

### `Iterator` combinators (`zip`, `map`, `filter`, `take`)

These are library, not language — but they need `Iterator` and `for`
to work, which is now done. Each combinator is a struct with a
`next() -> Element?` method. Once one of them ships, the rest follow
a template.

### Result `try?` over a non-Result Optional

Currently `try?` requires Result. Sometimes the user wants
`try? optional_func()` to short-circuit `nil` from a non-throwing
optional-returning call. Either a clean error or a sema-rewrite into
the matching Optional form.

### `Soa[T]` — struct-of-arrays library type (§13)

Inverts AoS storage for SIMD-friendly traversal. Library on top of
generic structs + a small `@layout(soa)` attribute. Lots of design
freedom; could be a session of its own once generics + attributes
have firmed up.

---

## Already-shipped (back-pointer to `HANDOFF.md`)

§3 opaque type, §4 string interpolation + layout attrs + Display
conformance, §5 for-in + iterator + struct methods, §6 tuples
(literals + destructuring), §9 Optional + Result + try (incl.
hoisting) + do/catch (annotated and bare), §10 Box[T] + Alloc check,
§12.1 comptime folding through phase 7 (functions, locals, loops,
vectors, math stdlib, type-as-callable conversions, @embed), §12.2
reflection phases 1–3 (Type, Field, Field.type), §12.3 derives
(Eq, Hash, Debug, Clone, Display), §12.6 conditional compilation
(`@when` + `cfg`), §17.7 expression precedence (Pratt parser), plus
the ownership / exclusivity / capability / generics phase-1 work
described in [`README.md`](README.md). See
[`HANDOFF.md`](HANDOFF.md) for the full chronological phase log with
commit hashes.
