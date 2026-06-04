# Roadmap

Forward-looking menu of candidate phases for the Vestra-to-C++26
transpiler. The *back-looking* counterpart, what's shipped with commit
references and known v0.5 limitations, is in
[`HANDOFF.md`](HANDOFF.md). The spec is
[`VESTRA_DRAFT.md`](VESTRA_DRAFT.md), with the kernel-completeness
annex consolidated in [`VESTRA_ANNEX_A.md`](VESTRA_ANNEX_A.md).

The list is **priority-ordered**: number 1 is the next phase to ship
when nothing else is forcing the choice. The user picks from a 4-item
menu each round; the AskUserQuestion draws four entries from this
list, biased toward the top.

Each candidate is sized for a single end-to-end session (AST, parser,
sema, codegen, unit tests, e2e). Items flagged **(multi-session)** are
bigger swings; for those the per-session goal is a self-contained
first slice.

> **Doc hygiene note (2026-05-29).** The previous version of this file
> listed a "language polish (1-5)" and "bounded follow-ons (6-10)"
> section. Every one of those items had already shipped (commits
> `14de903` panic, `f178251` Span, `d8d4ac2` Optional-in-Display,
> `ec26138` pattern matching, `3075829` opaque follow-ons, `11b3a3a`
> .mapError, plus the with-binding / do-catch-guard / zip-take / try?
> follow-ons). The list below was re-derived from `git log` against
> the annex track and the bigger swings. When you finish a phase,
> move it to the "Already shipped" section the same session so this
> file does not drift again.

---

## Next up (priority 1-9)

### 0. Multi-file modules (§5) (multi-session) — slices 1-9 shipped

The first step toward self-hosting (a compiler is many files). **Slice 1
shipped**: `vestra build entry.vst` is now a transitive module loader.
`import a.b.c` loads `<entry-dir>/a/b/c.vst` (dotted path = directory path),
deduped by canonical path so a diamond imports once. Every loaded unit is
macro-expanded + extension-folded, then resolved and emitted to its own
`.hpp`/`.cpp`. An importing unit's resolver collects its dependencies' public
top-level decls into scope (signatures re-derived in the importer's arena, so
no cross-arena `TypePtr` sharing); those decls are not re-checked or re-emitted.
Codegen: the importer's header `#include`s each dependency's header and opens
`using namespace dep;`, so cross-module references lower without per-site
qualification. The runtime prelude is now wrapped in a `VESTRA_RUNTIME_PRELUDE`
include-guard so two module headers in one TU don't redefine its (non-inline)
types. Proven end to end (`examples/multifile_demo/`: a cross-module call +
const, compiled and run).

**Slice 2 shipped** — **`public` visibility**: only a module's `public`
top-level decls are exported to importers; `internal` (the default), `package`,
and `private` stay module-private. Enforced in the resolver's imported-unit
collection (`decl_visibility` filter), so a reference to a non-public imported
name fails to resolve. The loader is now shared between `build` and `check`
(`load_module_graph`), so both follow imports identically — `check` previously
resolved nothing across files. Negative test: `e2e_multifile_visibility`
(referencing an `internal` import is a hard error).

**Slice 3 shipped** — **output layout**: a dependency is now emitted at its
import path as a directory path (`import util.math` → `<DIR>/util/math.{hpp,cpp}`)
instead of flat by basename, so same-named modules in different packages no
longer overwrite each other. The entry file keeps its file stem (so single-file
builds are byte-for-byte unchanged), and the importer's `#include` mirrors the
import path, all resolved `-I <DIR>`. The output relpath doubles as the
emitter's `output_basename`, so a unit's self-include and an importer's
dependency-include spell the identical path.

**Slice 4 shipped** — **qualified references** (`util.math.add`), which also
changed the import model: imported names are now reached *only* through a
qualified path and are no longer collected into the unqualified global scope.
This eliminates import/local collisions entirely — a local `add` and
`util.math.add` coexist, the local owning the unqualified name (matches the
slice's design preview). `check_qualified_module_ref` flattens a `a.b.c` member
chain, matches the longest prefix against the imported-module map, and
synthesizes a symbol for the named public func/const export (signature
re-derived in this arena); codegen emits the fully-qualified `util::math::add`
via a Resolution side table (`qualified_name_of`). The `using namespace dep;`
emission is now inert (no unqualified imported refs remain) and is left in place
pending the cleanup below.

Note this supersedes slices 1-2's "imports brought into the unqualified scope"
behavior. The `decl_visibility` collection filter is gone (imports aren't
collected at all); visibility is now enforced at the qualified-reference site
(only `public` func/const exports resolve).

**Slice 5 shipped** — **qualified types**: a dotted type path `util.math.Pair`
in `resolve_type` resolves to the imported module's public struct/enum nominal
(emit_type already joins the path → `util::math::Pair`), so it works in param /
return / field / let annotations. `check_qualified_module_ref` also synthesizes
struct/enum export symbols, so `util.math.Pair(lo: 1, hi: 2)` construction
resolves and codegen emits the qualified designated-init. Field access on the
imported value (`p.lo`) works through the nominal's decl. Proven end to end
(multifile_demo `pairSum()`).

**Slice 6 shipped** — **shared arena + export harvesting** (the architecture
that makes modules compiler-grade). The build now uses one `TypeArena`, resolves
units dependency-first (topological order), and **harvests each module's
resolved public-export symbols** into a store handed to its importers. An
importer no longer re-derives an import's signature in its own arena — it reuses
the symbol the dependency's own resolver produced. This fixes the prior wall:
an imported function whose signature mentions the module's own types
(`public func combine(_ p: Pair) -> Int32`) is now callable from an importer
(`util.math.combine(util.math.Pair(...))`), because `Pair` was resolved in
util.math's context and reused. Type identity holds across the boundary because
`TypeArena::equal` is structural (nominals by shared decl pointer). The
`Resolver` lost `set_imported_units` + the per-unit collection/synthesis; it now
takes `set_module_exports(...)` and exposes `public_exports()`.

**Slice 7 shipped** — **missing / cyclic-import diagnostics**: the module loader
now reports both failure modes as located errors at the offending `import`
statement, recorded in the shared reporter so the build aborts (via
`rep.has_errors()`) before resolution. A missing import (no backing `.vst`)
reports "cannot find imported module '<a.b.c>' (no file at <path>)"; a cyclic
import is caught by tracking the units currently on the DFS recursion stack —
an `import` whose target is on the stack closes a back-edge and reports "cyclic
import: '<x>' is already being imported", and the back-edge is dropped so the
dependency graph stays acyclic for the topological resolve. A diamond
re-import (target already loaded but off the stack) is unaffected. Negative
tests `e2e_cyclic_import` / `e2e_missing_import` (both WILL_FAIL).

**Slice 8 shipped** — **`emit_sema_type` qualification + dropping `using
namespace`**: a *computed/inferred* imported nominal (one not spelled via an
annotation — e.g. the `std::vector<util::math::Pair>{}` a `Vec.new()` for a
`Vec[util.math.Pair]` emits) is now spelled fully-qualified instead of relying
on a `using namespace dep;`. The driver hands each emitter an
`imported_qualifiers_` map (every *other* module unit's public struct/enum/
protocol/opaque decl → its C++ namespace, keyed by the same decl pointer a sema
type's `nominal_decl()` carries); `emit_sema_type` prefixes a hit with
`util::math::`. With every cross-module reference now fully qualified — value
refs via the resolver's `qualified_name_of`, computed types via this map — the
`using namespace dep;` emission is gone entirely; the dependency `#include`
alone suffices. Proven end to end (`pairVecSum()` in multifile_demo builds a
`Vec[util.math.Pair]` whose element type lowers through emit_sema_type).

**Slice 9 shipped** — **generic imported types**: a qualified generic type
`util.math.Boxed[Int32]` now binds its type arguments onto the imported nominal
instead of dropping them. `resolve_type`'s dotted-path branch calls the same
`resolve_generic_instance_args` + `make_struct_instance`/`make_enum_instance`
the local case uses (the export symbol carries the dependency's decl, whose
`generics` drive the binding), so the annotation resolves to the instance,
construction infers it, and a field read yields the substituted type (not the
unbound `T`). Codegen: the qualified-construction path now appends the resolved
`<args>` after the `qualified_name_of` string — `coll::box::Holder<std::int32_t>
{...}` — matching the local-struct emission instead of leaning on CTAD. Proven
end to end (`boxedValue()` over a `util.math.Boxed[Int32]`).

Remaining: **`import c "header.h"`** and a **search-path / project-root** notion.

### 0b. Collections / string library (§18.5) (multi-session) — slices 1-8 shipped

The other big self-hosting blocker (a compiler is mostly Vec/HashMap/String
churn). v0.5 had no growable collection; there's no raw-array-alloc primitive
to write one in Vestra, so these follow the established "compiler-known type
backed by a C++ STL container" idiom (like Box→unique_ptr, Channel→deque).

**Slice 1 shipped** — **`Vec[T]` → `std::vector<T>`**: a growable, owned
sequence. `Vec.new()` mints an empty one (gated by `Alloc`, like Box.new);
`v.push(x)` appends, `v.len() -> Int`, and `v[i]` reads an element. New
`TypeKind::Vec` (distinct from the fixed-array `Vector`); resolved in
`resolve_type` / `check_call` (construction) / `lookup_method` (push, len) /
`check_index` (indexing); lowered in the emitter (type, `Vec.new()` → `{}`,
push → `push_back`, len → `size()`-as-Int, index → `[size_t]`). Proven end to
end (`examples/vec_demo.vst`, including a `Vec[Point]`).

**Slice 2 shipped** — **owned growable `String`** (→ std::string). `String`
already lowered to std::string but had no operations (and was untested): now
`String.new()` mints an empty one (Alloc-gated), `s.append(_ other: Str)`
concatenates a literal or borrowed view (via std::string's string_view append
overload — no coercion needed), and `s.len() -> Int`. Resolved in check_call /
lookup_method, lowered in the emitter, gated in capability.cpp; proven end to
end (`examples/string_demo.vst`).

**Slice 3 shipped** — **string-lattice coercion codegen**: a string view
(StrConst/Str) flowing into a `String` slot is now wrapped in `std::string(...)`
at codegen, fixing the pre-existing bug where `var s: String = "literal"`
type-checked but emitted invalid C++ (`std::string = std::string_view(...)`,
explicit ctor). The resolver marks the coerced expr (`note_string_coercion` at
every assignable-to-String site — let/var, return, assign, call arg, struct
field, const/static), recorded in a Resolution side table; emit_expr wraps it
once (a re-entry guard prevents double-wrap). A String can now be seeded
directly from a literal.

**Slice 4 shipped** — **`HashMap[K, V]` → `std::unordered_map<K, V>`**: the
owned hash map that completes the bootstrap collection set (a compiler is mostly
Vec/HashMap/String churn). `HashMap.new()` mints an empty one (Alloc-gated,
inferring K/V from the expected type); `m.set(k, v)` inserts-or-assigns,
`m.contains(k) -> Bool`, `m.len() -> Int`, and `m.get(k) -> V?` reads a value
back as an optional (nil when absent), folding straight into `if let` / `??`.
New `TypeKind::HashMap` (parts = {K, V}); resolved in `resolve_type` /
`check_call` / `lookup_method`; lowered in the emitter (type → unordered_map,
`new` → `{}`, set → `insert_or_assign`, contains → `.contains`, len →
`size()`-as-Int). The only new codegen wrinkle is `get`'s optional return: a
`__vstr::map_get(m, k)` prelude template folds the find/end check into a
`std::optional<V>`. Proven end to end (`examples/hashmap_demo.vst`).

**Slice 5 shipped** — **optional-returning `Vec` reads**: `get(i) -> T?` is the
bounds-checked element read (nil when the signed index is out of range, so a
default falls out of `??`), and `pop() -> T?` removes and returns the last
element (nil when empty). Both reuse the slice-4 optional-helper shape: new
`__vstr::vec_get(v, i)` / `__vstr::vec_pop(v)` prelude templates fold the
range/empty check into a `std::optional<T>`. Resolved in `lookup_method`
(returning `make_optional(T)`), lowered in the emitter; `pop` rides the same
handle-style mutation path as `push`. Proven end to end (`examples/vec_demo.vst`
extended with `getOr`/`drainSum`).

**Slice 6 shipped** — **`Vec` mutation + iteration**: `set(i, x)` overwrites an
element in place (unchecked, lowering to `v[size_t] = x`, the assignment mate of
the unchecked `xs[i]` read), `clear()` empties the Vec (`.clear()`), and
`for x in xs` iterates its elements (a third ForStmt shape alongside Range and
the `next()` iterator protocol, lowered to a C++ range-based `for (auto&& x :
v)`). Resolved in `lookup_method` (set/clear) and the ForStmt resolution
(element type = the Vec's inner). Proven end to end (`examples/vec_demo.vst`
extended with `doubleAndSum`, which sets, iterates, and clears).

**Slice 7 shipped** — **owned-`String` read-borrow at a call argument**: an
owned `String` flowing into a `Str` parameter now borrows as a view for the
duration of the call (std::string → std::string_view, zero-copy), so
`s.append(otherString)` and any `func f(_ v: Str)` called with an owned String
type-check. This is the call-site counterpart of the §4 string lattice: a new
`read_borrows_as(String, Str)` relaxation sits beside `assignable` in
`check_call`'s argument check only, so the dangling cases the lattice guards —
`let v: Str = ownedString` and `return ownedString` from a `-> Str` function —
stay rejected (the view would outlive its owner). No codegen change: the
std::string → string_view conversion and `std::string::append(const string&)`
are both implicit. Proven end to end (`examples/string_demo.vst` extended with
`joined`).

**Slice 8 shipped** — **tracked `inout` receiver for the mutating methods**: the
mutating collection methods (`Vec.push`/`pop`/`set`/`clear`, `String.append`,
`HashMap.set`) now borrow their receiver `inout` in the exclusivity checker
instead of riding the handle-style exemption. `check_call` adds the receiver
place as an inout borrow (keyed on the resolved receiver type and method name)
alongside the argument borrows, so a call that aliases the receiver with one of
its arguments is caught — e.g. `node.kids.push(node)` borrows `node.kids` inout
while reading `node`, which contains it, an overlap. Disjoint roots
(`n.kids.push(child)`) and the non-mutating reads (`len`/`get`/`contains`) stay
clean. Scoped to the builtin collection types (user-defined methods still have
no receiver-mode story). Phase-1 exclusivity remains intra-call, so this fires
when receiver and argument alias within one call.

That rounds out the bootstrap collection set: `Vec`, `String`, and `HashMap`
each have construction, reads, mutation, iteration where applicable, the string
read-borrow, and inout-receiver discipline for their mutators.

### 1. Generics phase 2 (multi-session)

`7e93b0e`'s phase 1 covers function generics (opaque GenericParam
types, unification with `expected` propagation, emit as C++
templates). Four phase-2 slices shipped: user-defined generic *structs* (`e74f1ad`,
`struct Pair[T] { ... }`), generic *enums* (`1bfff52`, `struct`-of-variant
templates), *const generics* (`8bf316a`, `struct Buffer[T, const N: Int] {
var data: [N]T }`, monomorphized to `template <class T, std::size_t N>`),
and *protocol bounds* (`func eq[T: Eq](...)` / `where T: Comparable`,
enforced at the call site and at struct/enum instantiation, with a C++20
`requires` clause for the concept-mappable bounds). See the HANDOFF top
entries. Remaining pieces: explicit type-args at a construction site
(`Pair[Int32](...)`, where the callee currently parses as an index
expression); a const generic as a runtime/comptime *value* in a body
(`for i in 0..N`; today N is only an array length); const-expression
array lengths (`[N + 1]T`); bound enforcement gating body operations;
transitive bound checking; user-defined-protocol conformance (no general
table yet); and `requires` clauses on generic structs/enums (functions
only today). (Leading-dot payloaded-enum construction, const generics on
functions, and derive emission for generic types were closed as
follow-ons.) The type-parameter surface is now substantially complete —
the remaining items are minor refinements. Explicit construction-site
type-args is the most user-visible one left; otherwise generics phase 2
is effectively done and the next phase is best drawn from the bigger
swings (async, macros, ownership phase 2) or the annex deepening.

### 2. `async` / `spawn` / `select` / `parallel` (§11) (multi-session)

Slices shipped: `async func` + `await` over a `__vstr::Task<T>` coroutine
shim (the host libc++ ships `<execution>` policies but not P2300
senders), `spawn` → `Future[T]` consumed by `await`, `select` over future
arms (an `await_ready` IIFE), `parallel` (a builtin that splits a
`MutSpan[T]` into N disjoint sub-views and runs a non-escaping worker on
each), and the cooperative scheduler:

  * **slice 1** (`6cb4df6`) — a real single-threaded scheduler with *lazy*
    Task (initial_suspend == suspend_always) and symmetric-transfer
    completion, replacing the earlier eager/suspend_never model. Codegen
    lowering unchanged; the inversion lives in the runtime preamble.
  * **slice 2** (`e981743`) — **blocking channel receive**, the first
    observable use of real suspension: `await ch.receive()` parks the task
    on an empty open channel; `send` wakes one parked receiver, `close`
    wakes all (nil == closed-and-drained). `spawn` now schedules its task
    on the run-loop so a parked consumer and a running producer coexist.
    Also the foundational **coroutine param-lifetime fix** (caught by
    ASan): an `async func` emits its read params *by value* (not `const
    T&`), since a coroutine doesn't copy reference params into its frame —
    the codegen half of "no borrow across await".
  * **slice 3** — **blocking channel select**: a `select` whose arms are
    channel receives (`on let v = ch.receive(): …`) and has *no* default
    parks on *every* arm's channel and resumes on the first to deliver,
    binding that arm's `T?` (nil == closed-and-drained); ties go to the
    earlier arm. Lowers to a nested `Task<RT>` coroutine the caller
    co_awaits, parking one shared `Waiter` (a `fired` flag makes a
    multi-arm wake idempotent — no double-resume) via a `SelectAwaiter`
    over a non-template `SelectableState` base, so arms over channels of
    differing element types park uniformly. A select *with* a default
    stays a non-blocking poll (now `sel_state()->ready()` for channel
    arms, `await_ready()` for future arms).
  * **slice 6** — **mixed channel/future select arms**, with *blocking
    join* semantics. A no-default select mixing `ch.receive()` arms and
    future arms parks until any arm fires — a channel delivers *or* a
    future's spawned task completes — so a channel can win a race over a
    still-pending future. Built on a non-template `Selectable` base shared
    by `SelectableState` (channels) and `FutureSelectable<T>` (futures),
    plus a `select_waiter` slot on the `Task` promise that `final_suspend`
    fires (idempotently, via the same `fired` flag, so a channel arm and a
    future completing can't double-resume). The generalized `SelectAwaiter`
    arms one shared `Waiter` on every arm; the lowering materializes each
    future arm as a stable local and dispatches with `sel_take()` (channel)
    or `.get()` (future). A `timeout` arm still requires the all-channel
    form (rejected on a mixed select); a pure-future no-default select
    stays a poll (futures are always ready).
  * **slice 4** — **timeout select arm**: `timeout <ms>: body` (a
    contextual keyword) on a blocking channel select fires its body if no
    channel arm delivers within the wall-clock delay. The scheduler gains
    a timer queue; when the ready queue drains (every task parked) `run()`
    sleeps (`std::this_thread::sleep_until`) to the earliest live deadline
    and fires that waiter. The `SelectAwaiter` registers the timer on
    suspend tied to the same shared `Waiter`, so a channel arm winning the
    race marks the timer fired (dropped without sleeping); if the timer
    fires first the awaiter returns the index past the last channel arm.
    Sema rejects `timeout` + `default` (contradictory), `timeout` on a
    future select, and a non-Duration delay.
  * **slice 5** — **`Duration` type** (Swift-like). A non-generic value
    type lowered to `__vstr::Duration` (a signed nanosecond count).
    Construction via the typed leading-dot factories `.seconds(n)` /
    `.milliseconds(n)` / `.microseconds(n)` / `.nanoseconds(n)` (resolved
    against the expected type) and the explicit `Duration.seconds(n)`
    form. Arithmetic mirrors Swift: `Duration / Duration -> Float64` (a
    dimensionless ratio), `Duration +/- Duration -> Duration`, and
    comparisons -> `Bool`, plus scalar scaling `Duration * (Int|Float)` /
    `(Int|Float) * Duration` / `Duration / (Int|Float) -> Duration` — all
    riding the C++ `__vstr::Duration` operator overloads, so codegen still
    emits a straight `a <op> b`. The scalar overloads are constrained
    templates (`std::integral` vs `std::floating_point`) so an integer
    literal stays unambiguous against the floating overload (`int->int64`
    and `int->double` are equal-rank conversions); none collide with the
    `Duration / Duration` ratio (no int/float<->Duration conversion
    exists). The `timeout` arm now takes a `Duration` (e.g. `timeout
    .milliseconds(250):`), extracting whole milliseconds via
    `.in_milliseconds()`. Property accessors `.nanoseconds` /
    `.microseconds` / `.milliseconds` / `.seconds` read the total whole
    count in that unit as `Int` (truncating; accessed, not called —
    distinct from the same-named factory), lowering to
    `static_cast<std::intptr_t>(d.in_<unit>())` like Span's `.count`.
  * **slice 7** — **async + throws**: an `async func … throws(E) -> T`
    lowers to `__vstr::Task<std::expected<T, E>>` (the return-type nesting
    was already in place). The body is a coroutine, so every function-level
    error exit co_returns: `throw e` → `co_return std::unexpected{e}`, and a
    propagating `try` (statement-position and the try-hoist prelude) →
    `co_return std::unexpected{…}` / `co_return *r`. Escapes inside a
    cond-hoist's IIFE lambda stay plain `return` (the lambda isn't a
    coroutine) — handled by suppressing the async flag while emitting that
    lambda body. Sema already typed `await f()` of an async throws fn as
    `Result<T, E>` (await is transparent on non-Future), which a `try`
    unwraps; no sema change was needed.
  * **slice 8** — **`Future[void]` for void spawn**: `spawn` of a
    Unit-returning async fn now yields a `Future[Unit]`, lowered to a
    `__vstr::Future<void>` specialization (no `std::optional<void>`; a
    `forced_` flag stands in for "completed", forcing pumps to completion,
    awaits/`.get()` yield void). `Task<void>` now advertises
    `__vstr_task_value = void` so `spawn_future` recognizes it (previously
    it mis-wrapped as `Future<Task<void>>`) and carries a `select_waiter`
    fired at `final_suspend` for parity. (A void future as a *select* arm
    is still unsupported — the dispatch would bind `auto&& v = f.get()` to a
    void — but that combination is nonsensical and out of scope.)
  * **slice 9** — **async-context gate for a blocking select**: a
    no-default select with a channel (or timeout) arm co_awaits a
    SelectAwaiter internally, so the capability checker now requires the
    `Async` capability for it — the same gate `await` / `spawn` carry. This
    turns a confusing downstream C++ "co_await in a non-coroutine" error
    into a source-level `missing capability 'Async'` diagnostic. A select
    *with* a default (or a pure-future select) polls and is still allowed
    in a sync function.
  * **slice 10** — **`using Async` gate on `parallel`**: `parallel(data,
    chunks, body)` dispatches its worker onto the async runtime (v0.5 runs
    the chunks sequentially, but the surface commits to the concurrent
    contract), so the capability checker now requires the `Async`
    capability at the call — the same gate `spawn` / a blocking `select`
    carry. Callers declare `using Async` (or open `with Async = …`); the
    bare-ident builtin shape only gates when nothing shadows `parallel`.
  * **slice 11** — **no borrow across await** (the sema half; codegen's
    by-copy read params were the runtime half). Vestra borrows are
    transient (call-argument boundaries), so the only borrow that can span
    a suspension is an `inout` parameter of the async fn itself — and
    unlike a read param (copied into the frame) an `inout` must stay a
    write-back reference into the caller, which can't survive the
    coroutine's suspension. The exclusivity checker now rejects an `inout`
    parameter on an `async func` at its signature.
  * **slice 12** — **spawn captures by value / non-escapable futures**.
    Capture-by-value of *owned* args already held (the by-value read-param
    fix copies them into the coroutine frame — a spawned task sees the
    value as of spawn time, not later mutations). The remaining gap was a
    borrowed *view*: a `Span` / `MutSpan` parameter is captured as a view,
    not the data, so a Future that outlives the view's backing would
    dangle. The resolver now rejects `spawn`ing a call with a `Span` /
    `MutSpan` parameter (keyed on the parameter type, since an array arg
    coerces to a view only at the param boundary), pointing at the
    offending argument. With views barred from capture, a Future owns its
    whole frame and is safe to outlive its spawning scope; `parallel`
    remains the way to run work over a borrowed view (it can't let the view
    escape).

§11 is feature-complete for v0.5. (senders/receivers can replace the
coroutine shims if/when libc++ ships P2300; a real thread pool can replace
the cooperative scheduler behind the same surface.)

### 3. `Channel[T]` + `parallel` library (§11) — shipped

Both shipped: `parallel` (under §11 above) and `Channel[T]` — a typed
queue, lowered to a `__vstr::Channel<T>` over a shared deque, with `send`
(a sink), the non-blocking `recv() -> T?` poll, the suspending `await
ch.receive() -> T?` (scheduler slice 2), and `close()` (a closed flag
distinct from empty: nil from `receive` means closed-and-drained). v0.5
is single-threaded and unbounded. A blocking `select` over channel
receives ships under §11 above (slice 3), with a wall-clock `timeout`
arm (slice 4). Remaining (tracked under §11 above): bounded capacity /
back-pressure, and `send` as a true call-site move.

### 4. Quote / splice / declaration macros (§12.4) (multi-session)

First slice shipped: the `$`-splice syntax (`$ident` / `$(expr)`), a
`SpliceExpr` AST, and an expression-context `quote { EXPR }` that
materializes its body with splices substituted in place (an identity
template).

Second slice shipped — **expression macros**: a `comptime func(Expr, …)
-> Expr` whose body is a `quote { … }` template, invoked in expression
position as `@name(args)`. The `Expr` AST-value type (`TypeKind::AstExpr`)
was added; a `quote` types as `Expr` when its expected type is `Expr` (its
body then deferred, not checked as a runtime expression) and otherwise
keeps the slice-1 identity behaviour. A macro call resolves by cloning the
macro's template and substituting each argument for the matching `$param`
splice (spliced args parenthesized to preserve precedence), then the
expansion is type-checked as ordinary code (the spec's "re-checked") and
lowered in place; the comptime macro func itself is never emitted. The
comptime evaluator already executes `comptime func` bodies (loops, locals,
recursion), so the engine was in place; this slice added the AST-as-value
representation and the invoke/expand loop for the template form.

Third slice shipped — **declaration macros** (replace form): a `comptime
func(Decl) -> [Decl]` whose body is a declaration-context `quote { … }`,
applied as `@name` on a top-level struct/func. A pre-resolution pass
(`expand_declaration_macros`, run from the driver before the resolver)
rewrites `unit.decls`: the annotated decl is replaced by the macro's
template declarations, with a `$d` item (a `SpliceDecl`) splicing the
annotated decl back in; the macro func itself is dropped (comptime-only),
and the generated decls are then checked + lowered as ordinary code. The
quote parser disambiguates a declaration-context quote (a decl keyword at
brace depth 1) from the expression quote; `[Decl]` / `[Expr]` parse as
length-less AST-list types. To avoid a full decl cloner, the template is
*moved* into the expansion site, so a macro backs a single application
(applying it twice is a diagnosed v0.5 limitation).

Fourth slice shipped — **`Decl` name reflection** (`$(d.name)`): a
declaration macro's template can splice the annotated declaration's name
(as a String) via `$(d.name)`, where `d` is the macro's `Decl` parameter.
Implemented as a mutating substitution walk over the moved template decls
(`expand_declaration_macros`), replacing each `$(<param>.name)` splice with
a string literal of the annotated decl's name. No comptime evaluation yet —
it stays template-based, which covers name reflection without the heavier
machinery.

Fifth slice shipped — **AST deep-clone foundation** (`ast::clone`,
src/ast/clone.cpp): an owned recursive copy of any decl / stmt / expr /
type / pattern, preserving source ranges (the long pole every remaining
reflection sub-slice needs — for materializing quote bodies and cloning
templates). Declaration-macro expansion now *clones* its template per
application instead of moving it, so one macro can back several sites; a
fixed-name generated decl that collides across sites is caught by the
ordinary duplicate-definition check rather than a special single-use error.

Sixth slice shipped — **folder-evaluated declaration macros** (computed
`$(k)` splices): declaration-macro expansion now runs the macro body through
the comptime folder (`ComptimeFolder::expand_decl_macro`) instead of a fixed
template walk. A copyable `Code` comptime value (`ComptimeValue::Kind::Code`,
holding shared `const ast::Node` — an owning clone for a quote result, a
non-owning alias for the bound `Decl`) lets `quote { … }` fold to an AST
value; `$d`, `$(d.name)`, and now arbitrary computed `$(expr)` splices all
resolve at fold time (`$(k)` where `let k: Int32 = 40 + 2` materializes the
literal `42`). The `d.name` reflection moved from the bespoke
`subst_name_splices` walk onto the folder's MemberExpr path. Deliberately
deferred to the next step: `.fields` iteration and for-over-collection.

Seventh slice shipped — **field reflection over `d.fields`** (comptime
loop): `d.fields` on the bound `Decl` folds to the same `Field` Vector as
`StructName.fields` (sourced from the StructDecl directly, so it works at
pre-resolution expansion time — name/type carry, the layout slots are 0
without a global scope), and the folder's `for` now iterates any iterable
that folds to a Vector, not just ranges. So a macro can derive a value from
a type's shape — `for f in d.fields { total += 1; if f.type.name == "Int64"
{ wide += 1 } }` — and splice the folded totals (`$(total)`, `$(wide)`) into
the generated accessors. `field.name` / `field.type` dispatch by a fixed
positional fallback (`field_member_index`) when no `Field` StructDecl is in
scope. The `.fields` construction is now one shared helper
(`build_fields_value`) used by both the reflection and macro paths.

Eighth slice shipped — **per-field declaration generation**: a macro can
build its `[Decl]` result by iterating `d.fields` and appending one generated
declaration per field. `[Decl] +=` concatenates Code vectors (folder
`apply_compound` / `BinaryOp::Add`), comptime String `+` concatenates
(threaded past the numeric-operand guard), and each generated decl gets a
distinct name from a comptime String splice in name position —
`func $(f.name + "_get")( … )`. The parser accepts a `$(expr)` / `$x` splice
where a function name is expected (`FuncDecl::name_splice`), parsed without
the postfix step so the parameter list's `()` isn't swallowed as a call; the
folder resolves the splice to a String and writes `FuncDecl::name` during
quote materialization (the loop variable is in scope, so each decl is named
distinctly). The quote disambiguator now treats a standalone `$d` item as
declaration-context, so `quote { $d }` is a `[Decl]` (seeding the
accumulator) rather than an expression quote. Macro detection moved from a
literal-`return quote` shape check to the signature (`comptime` + `-> [Decl]`),
since a folder-evaluated body can be any shape.

Ninth slice shipped — **per-field typed accessors** (full reflection into
generated signatures): a generated declaration can now consume the field's
type and value, not just its name. Three new splice positions: a `$(expr)`
splice in **type** position (`ast::SpliceType`, parsed in `parse_type`) for
`_ v: $(d.name)` and `-> $(f.type)`; a **member-name** splice
(`MemberExpr::member_splice`) for `v.$(f.name)`; and TypeRef/String →
`NamedType` materialization (`materialize_type`, simple identifiers only in
v0.5). The folder resolves type splices in a generated decl's params/result
and member splices in its body during quote materialization. So
`func $(f.name + "_of")(_ v: $(d.name)) -> $(f.type) { return v.$(f.name) }`
over a struct emits a typed `field_of(s)` reader per field. The `$`-splice
parse is now one shared `parse_splice()` (no postfix), used by name / member /
type positions.

Tenth slice shipped — **hygiene via `gensym()`**: a comptime builtin that
yields a fresh, collision-free identifier on every call (`gensym("hint")` →
`__vstr_hint_h<n>`), backed by a unit-wide counter on the one expansion folder.
It composes with the name splices — `func $(gensym("helper"))( … )` — so a
macro that introduces a fixed-purpose helper can be applied to many types
without the generated names colliding (previously a duplicate-definition
error). `gensym` is only reachable inside a macro body (never registered as a
resolver builtin), so a normal call site still fails to resolve.

Eleventh slice shipped — **identifier-position splices**: a comptime String
spliced in call-callee or member/index-base position now materializes as an
identifier *reference* (an IdentExpr) rather than a string literal, so a
generated body can *call* a `gensym`'d helper — `$(h)()` invokes the function
named by `h`. Implemented as `subst_splices_ident` (used for the callee and
base positions), where a String folds to an IdentExpr and any other value
falls back to the normal literal materialization; elsewhere `$(s)` stays a
StringLit. Together with gensym this closes the loop: a macro can mint a
private, collision-free helper and reference it from its public surface
(`func $(h)() {…}  func $(d.name + "_seed")() { return $(h)() }`).

Twelfth slice shipped — **`vestra expand`**: a new subcommand that
pretty-prints a source file *after* declaration-macro expansion (it runs
`expand_declaration_macros`, then the AST printer — like `fmt` with the
expansion pass in between, before sema). A macro author can now see exactly
what each `@macro` generated; the comptime macro definitions drop out. An e2e
test runs it over `quote_demo` and asserts the generated `Seed1_seed` wrapper
is present while `comptime func seeded` is gone.

Thirteenth slice shipped — **attribute reflection**: a macro can read the
arguments of an attribute on the annotated decl, including the very attribute
that invoked it. `d.hasAttribute("name")` folds to a Bool; `d.attribute("name")`
folds the named attribute's single argument (`@register(0x40)` → 64). Both are
reflection method-calls dispatched in the folder's CallExpr fold on a Code(Decl)
base. The macro's own attribute is no longer stripped pre-expansion — it stays
on the decl so `d.attribute(…)` can fold its argument — and is instead stripped
from the `$d`-spliced clone (by macro name) so the reproduced decl isn't
re-validated as an unknown attribute; other (legitimate) attributes survive.

Fourteenth slice shipped — **method generation via extension lowering**: a
macro can emit `extension $(d.name) { func … }` blocks whose methods use
`self`. This needed real `extension` support (codegen emitted nothing for them
before): a pre-resolution `fold_extensions` pass folds each `extension T`'s
instance methods into struct `T`'s method list, so they become ordinary struct
methods — resolved for `t.method()` lookup and lowered as inline C++ member
functions (`self` → `(*this)`) by the existing struct-method path. Works for
both hand-written and macro-generated extensions. The folder's
`resolve_decl_splices` recurses into extension members (target type, method
names/types/bodies), and `ast::clone` now reproduces `ExtensionDecl`.

Remaining (full reflection): multi-argument attributes (`Attribute` carries a
single predicate today) and `.asType()` on a type-valued argument
(`@derive(Eq)`); richer type materialization (compound types like `[N]T` /
`T?`). The builder API is the last broad piece. Extension lowering is minimal
(instance methods on a unit-local struct target; conformance-bearing or
external-target extensions still pass through unfolded).

### 5. Ownership / exclusivity phase 2 (multi-session)

Phase 1 (`b0727b9` / `e77f2e0`) is single-pass and linear. Phase 2 is
complete. Shipped: the `split(at:)` partition primitive on
Span/MutSpan; **branch-aware flow merging** (each `if`/`match` branch
forks and merges at the join); the iterator partitioner **`chunks(of: n)`**
(a lazy `ChunkIter` over consecutive disjoint sub-views); and the full
**`linear` types** story — `linear struct` must-consume (a leak error for
any linear binding still live at a scope exit, including one-sided branch
consumption), **let-from-place move tracking** (`let c = b` moves `b`),
**construction consumes its place arguments** (a constructor owns its
inputs), **linearity transitivity** (a struct that owns a linear field is
itself linear, closed to a fixpoint), and **sink parameters as terminal
sinks** (taking ownership counts as consuming, so an empty consumer ends a
linear chain).

Also shipped: **chunks as a first-class value** — a `chunks(of: n)`
result can be bound and accessed randomly (`.count`, `cs[i]`), not only
consumed by `for`.

Also shipped: **partition provenance** in the exclusivity checker — a
`split(at:)` half or `chunks(of:)` iterator records its parent place on
the bound Symbol, so an `inout` borrow of a sub-view that aliases the
parent is rejected while the two halves of a split still borrow
independently.

Ownership/exclusivity phase 2 is **complete**. General cross-statement
borrow liveness was moot (v0.5 has no persistent borrow form — `&x` is
admitted only directly in argument position, so no borrow's lifetime
spans statements). Deferred analysis follow-ons, none blocking: linear-in-
loops soundness (the loop body is walked once), the larger
destructure/deinit terminal form for linear values (sink params cover the
common case today), and generic linear structs / linear obligations
carried through wrapper types (Optional/Span/Box).

### 6. Capability narrowing + audit trail

Row polymorphism over a generic capability variable; narrowing
(`Mmio.narrowed(to:)`); the `// Safety:` audit-comment trail for
`unsafe` discharges. The `vestra audit` subcommand already exists
(`--sysreg` / `--no-libc` enumerators from `a2e7438` / `3934f9f`), so
this extends an established surface.

### 7. SIMD `[N]T` lowering (§13)

Map fixed-length vector types to `std::experimental::simd` (or target
intrinsics) where available, with a clean scalar fallback elsewhere.
Needs a target-detection layer the codegen doesn't have yet; the
`--target` / `--target-features` plumbing from `312281c` is a starting
point for that layer.

### 8. Content-hashed `@embed` manifest

Bounded single-session item. Today `@embed("path")` reads files
relative to the source dir at fold time; the spec wants paths resolved
against a content-hashed manifest so two builds of the same source
under the same config are byte-identical. Needs a manifest file format
plus a hash compare in the driver. Self-contained.

### 9. `Soa[T]` struct-of-arrays library type (§13)

Inverts AoS storage for SIMD-friendly traversal. Library on top of
generic structs plus a small `@layout(soa)` attribute. Design-heavy;
waits for generics phase 2 (#1).

---

## Annex deepening

The annex track A1-A12 is shipped through first slices and many
follow-ups (see "Already shipped"). What remains are the deeper slices
flagged in those commits:

### A7 second slice: context-switch completion (§14.14)

The first slice (`14b9482`) ships `Scheduler.swapContext(saving,
loading)` against bare `Context` values. The spec form is
`MutPtr[Context]` / `Ptr[Context]`; now that `&decl` exists
(`08b8744`) the pointer-threaded form can be tightened. Adds the
separate `FpContext` for lazy-FP save/restore.

### A8 follow-up: kernel-target vector-table layout (§14.5.3)

`4747e88` ships typed `@interrupt(T)` arrays. The kernel target still
needs per-slot stride alignment (128 bytes on aarch64 EL1) and
`.text.vec_table.<index>` sub-section emission, plus the in-slot vs
trampoline 3.5 lowering rule. Hosted v0.5 drops these because the host
section format differs and dispatch semantics validate without them.

### A9 deepening: boot / kernel_init runtime-model gates (§14.7) (multi-session)

`e773018` ships the sema gates for `@boot` (using-row restriction, no
throws, `[[gnu::naked]]`) and `@kernel_init` (no Async). The audit-time
rules remain: call-graph reachability from each entry, allocator-init
ordering, and rejection of ordinary functions calling `@kernel_init`.
Needs a call-graph pass.

### Lock-free / atomics deepening (§14.9)

- `@retry_loop` function-level escape + `@allow_weak_cas` call-site
  escape for `compareExchangeWeak` outside the admitted loop shapes
  (`fa5b8d0` ships the shape rule; the escapes are deferred).
- AtomicTaggedPointer (`5beef46`) contention-stress lane (a
  multi-threaded Treiber stack with real ABA-defeating semantics) plus
  a load-link / store-conditional emitter for kernel targets.
- Real link-time `.o`-level wide-atomic feature gate (`312281c` ships
  the compile-time gate; the kernel build needs the ELF-feature
  cross-check).

### Audit-surface deepening (§14.12.3 / §15.5)

Cross-architecture barrier-table verification (each gated sysreg
checked against its architecture-manual paragraph), and a build-mode
that walks the *generated* artifact's external-symbol references
(today's `vestra audit` is source-level, seeing what the user declared
rather than what the build linked).

### Pattern-matching + try carry-forwards (§8 / §10) — complete

All four carry-forwards are shipped:

- ~~Nested tuple destructuring codegen~~ — `let ((a, b), c) = …` lowers via
  a placeholder + sibling `auto [a, b] = __vstr_tpN` structured-binding
  hoist (verified end to end).
- ~~String-literal patterns~~ (`f1a6637`) — the §4 string lattice
  (StrConst → Str → String) lets a literal match a Str/String scrutinee;
  the value-scrutinee if-chain lowering already handled the compare.
- ~~Or-patterns over payloaded-enum cases~~ (`df9ef1d`) — each alternative
  of `case .a(x) | .b(x):` expands into its own constexpr-if branch in the
  std::visit lowering, sharing the arm body and binding its own payload
  field to the common name.
- ~~Mid-expression `try` inside an if/match arm~~ (`9a08a57`) — the if-expr
  and enum-match cond-hoist forms already worked; the last gap was a `try`
  inside a *value-scrutinee* match arm, now lowered as an if-else-if chain
  inside the cond-hoist lambda so propagation escapes correctly.

---

## Already shipped (back-pointer to `HANDOFF.md`)

Language: §3 opaque type (+ inverse conversion, derive(Default)), §4
string interpolation + layout attrs + Display conformance (incl.
Optional-in-splice), §5 for-in + iterator + struct methods, §6 tuples
(literals + destructuring, nested, param + match-arm positions), §8 /
§17.7 pattern matching (literal / range / or-patterns, tuple
scrutinee, dead-arm warning), §9 Optional + Result + try (hoisting,
mid-expression, `.mapError`, do/catch incl. where-guard +
fall-through propagation), iterator combinators (zip / take / map /
filter), §10 Box[T] + Alloc, Span[T] / MutSpan[T] non-owning views,
panic / abort / unreachable, §16 closures, §17.4 with-binding
(value + type annotation), §12.1 comptime folding through phase 7,
§12.2 reflection phases 1-3, §12.3 derives (Eq, Hash, Debug, Clone,
Display, Default), §12.6 conditional compilation (`@when` + `cfg`,
incl. `cfg.option`), §17.7 Pratt precedence, plus ownership /
exclusivity / capability / generics phase 1.

Annex (first slices + follow-ups): A1 link attributes, A2 `@inline` +
layout reflection, A3 raw-mint primitives (+ `.value` deref), A4 the
full `Atomic[T]` memory model (orderings, bitwise, strong/weak CAS +
retry-loop rule, wide UInt128/Int128 atomics, AtomicTaggedPointer,
target-feature gate), A5 sync intrinsics + cache/TLB management, A6
MMIO (`MmioView` / `MmioRegion` / `MmioWireView`, `@repr(union)`) +
the typed `Sysreg` namespace with auto-emitted post-write barriers and
RO/WO handles (§14.12), A7 `InterruptsOff` + `swapContext` (first
slice), A8 `@interrupt` handlers + typed vector tables, A9 `@boot` +
`@kernel_init` (sema gates), A10 compiler-emitted intrinsics +
`@panic_handler` + `@stack_protector` + `--no-libc`, A11 `PerCpu[T]` +
`Padded[T]`, A12 pluggable calling conventions + `&decl` +
function-pointer type. The `vestra audit` subcommand ships `--sysreg`
and `--no-libc` enumerators.

See [`HANDOFF.md`](HANDOFF.md) for the full chronological phase log
with commit hashes and per-phase rationale.
