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

### 0. Multi-file modules (§5) (multi-session) — slices 1-11 shipped (complete)

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

**Slice 10 shipped** — **`import c "header.h"`**: a C-header import now lowers to
a global-scope `#include "header.h"` in the generated header (the quoted form
finds both in-tree and system headers). Vestra doesn't parse the C header — the
foreign symbols are still declared in-tree via `@extern` (§14.6) — but pulling
the canonical declarations into the translation unit lets the C++ compiler
cross-check those `@extern` prototypes and makes any types/macros they define
visible. The loader already skips `is_c_header` imports (no `.vst` to load);
this fills in the emitter half. Proven end to end (`examples/cinterop_demo/`:
`import c "clib.h"` + an `@extern` bound to the header's inline `cdemo_value()`,
compiled with the example dir on the include path and run).

**Slice 11 shipped** — **import search roots (`-I DIR`)**: `vestra build` and
`vestra check` take repeatable `-I DIR` flags. An `import a.b.c` now resolves
against the entry file's directory first, then each `-I` root in order, taking
the first existing `a/b/c.vst` — so a build can name shared library modules from
a common root regardless of where the entry file lives. The dependency's output
relpath stays its import path (`a/b/c.{hpp,cpp}`), independent of which root
held the source, so the importer's `#include` still matches. The missing-import
diagnostic now lists every root searched. `check` accepts the same flag so it
resolves identically to `build`. Proven end to end (`examples/searchpath_demo/`:
an entry in `app/` imports `mathx` whose file lives only under a `-I lib` root).

This closes §5 multi-file modules for v0.5: transitive loading, `public`
visibility, output layout, qualified references and types (incl. generics),
shared-arena export harvesting, missing/cyclic diagnostics, C-header imports,
and search roots.

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

**Slice 9 shipped** — **String/Str read-only queries** (the byte-oriented
slicing + search surface real string processing and the self-hosting front-end
need). Eight methods, synthesized uniformly on the owned `String` and the
borrowed `Str` / `StrConst` views since all three lower through a
`std::string_view`: `len() -> Int`, `isEmpty() -> Bool`, `slice(Int, Int) ->
Str` (the clamped `[from, to)` sub-view — out-of-range / inverted spans yield an
empty view rather than UB), `byteAt(Int) -> UInt8?` (bounds-checked byte read),
`find(Str) -> Int?` (first index of the needle, `npos` mapped to nil),
`contains(Str) -> Bool`, `startsWith(Str) -> Bool`, `endsWith(Str) -> Bool`.
Sema synthesizes them in `lookup_method` alongside `String.append` (which stays
String-only, since it mutates); the needle args reuse the existing
`read_borrows_as` relaxation so an owned `String` needle borrows as a `Str` at
the call. None allocate (a sliced `Str` borrows the receiver's bytes), so they
need no capability gate. Codegen lowers `len`/`isEmpty`/`contains`/`startsWith`/
`endsWith` to the matching `std::string` / `std::string_view` member, and
`slice`/`byteAt`/`find` to three new clamping `__vstr::str_*` preamble helpers
(returning `string_view` / `optional<uint8_t>` / `optional<intptr_t>`); the
helpers take a `string_view` so a `std::string` receiver converts implicitly and
one lowering serves all three kinds. Five unit tests (the query surface on a Str
and on a String; a non-Str needle rejects; `byteAt` yields `UInt8?` not a bare
byte; the codegen lowering) plus a transpile→compile→run e2e
(`examples/string_query_demo.vst` + `tests/e2e/main_string_query_demo.cpp`)
covering each method, the clamped/out-of-range edges, `if let`/`??` over the
optional results, the `afterDot` file-extension shape, and the owned-String
receiver. v0.5 carry-forwards: cross-statement liveness of a sliced `Str`'s
borrow (today, like the Span partitioners, the borrow isn't tracked past the
call); number↔string conversion (`String(n)` / parse); and `split(Str)` /
char-iteration (needs an iterator or a Vec result) — the next string slices.

**Slice 10 shipped** — **number↔string conversion** (the rendering / parsing
pair a real tool needs — a lexer reads integer literals, diagnostics print line
numbers). `n.toString() -> String` is synthesized in `lookup_method` on the
standard numeric primitives (the 128-bit kinds are excluded — std::format /
std::to_string have no portable `__int128` formatter yet) and lowers to a new
`__vstr::to_string` helper (`std::format("{}", v)`: decimal for integers,
shortest round-trip for floats); it allocates an owned String, so the call is
Alloc-gated in the capability checker (keyed on the receiver's numeric type via
`resolution_->type_of`, the same shape as the `PerCpu.slot` gate). `Str.toInt()
-> Int?` is synthesized beside the other String/Str queries and lowers to a new
`__vstr::parse_int` helper over `std::from_chars` (new `<charconv>` include): nil
unless the *entire* view parses, so a leading `+`/whitespace or any trailing
remainder yields nil — the parse is strict and total. Because `toInt` reads a
borrowed view it never allocates, so it needs no capability. Four unit tests
(toString type-checks + is excluded on Int128; toInt yields `Int?`; the Alloc
gate fires without the capability and is clean with it, and toInt stays
gate-free; the codegen lowering) plus a transpile→compile→run e2e
(`examples/string_convert_demo.vst` + `tests/e2e/main_string_convert_demo.cpp`)
covering rendering across Int / Int32 / Float64, the malformed/partial-input nil
cases, the `??` and `if let` consumers, a render→parse round-trip, and a
`slice(...).toInt()` chain over a `key=value` pair. v0.5 carry-forwards:
`toString` on the 128-bit kinds (awaits a portable formatter); parsing the other
numeric types (`toInt32` / `toFloat`); and a radix argument on both sides.

**Slice 11 shipped** — **`split(sep) -> Vec[Str]`** (the tokenizer primitive a
lexer needs). Synthesized in `lookup_method` beside the other String/Str
queries; it materializes the pieces between each occurrence of the separator
into an owned `Vec[Str]` (the piece views borrow the source), so the result
plugs straight into the existing Vec surface — `for piece in s.split(",")`,
`.len()`, `.get(i)` all work with no new machinery, and it composes with the
previous slice's `.toInt()` for "sum the integer fields" shapes. The v0.5 choice
is eager (a materialized vector) rather than a lazy `SplitIter` combinator — the
result is random-access and reuses all of `Vec[T]`; the zero-allocation iterator
form is a documented follow-on. Because it allocates the Vec, the call is
Alloc-gated in the capability checker, keyed on a *string* receiver so it does
not catch the unrelated, non-allocating Span `split(at:)` partitioner (which
stays clean without Alloc — pinned by a test). Codegen lowers to a new
`__vstr::split(s, sep)` preamble helper returning `std::vector<std::string_view>`
(= `Vec[Str]`): adjacent / leading / trailing separators produce empty pieces so
the piece count is always (occurrences + 1), and an empty separator yields the
whole string as one piece rather than looping forever. Four unit tests (the
Vec-surface use through for-in / len / get; a non-Str separator rejects; the
Alloc gate fires/clears and the Span `split(at:)` stays gate-free; the codegen
lowering + for-in drive) plus a transpile→compile→run e2e
(`examples/string_split_demo.vst` + `tests/e2e/main_string_split_demo.cpp`)
covering piece counts, the empty-piece edges, a multi-char separator, indexed
field access, `split`+`toInt` CSV summing, word counting over non-empty pieces,
and a `key=value` split. v0.5 carry-forwards: the lazy `SplitIter` combinator
(zero-allocation `for`-iteration); a max-splits limit; splitting on any of a set
of separators / on whitespace runs; and cross-statement liveness of the borrowed
piece views (shared with `slice`, like the Span partitioners).

**Slice 12 shipped** — **byte/char access for hand-tokenizing** (past `split` on
fixed separators — the toolkit a real lexer needs to scan character by
character). The enabling discovery: `Char` (→ `char32_t`), char literals
(`'a'`), `while`, and for-in over `Vec` already existed, so the gap was getting
*characters* out of a string and *classifying* them. Three string methods plus a
`Char` classifier set: `s.charAt(Int) -> Char?` (the bounds-checked indexed read,
no allocation — the index-loop workhorse paired with `slice`), `s.chars() ->
Vec[Char]` (one `Char` per byte, for `for c in s.chars()` — allocates, so
Alloc-gated like `split`), and `c.isDigit() / isAlpha() / isAlnum() / isSpace()
-> Bool` on `Char` (ASCII-only, locale-free). Because a `Char` compares directly
against a char literal (`c == '('`, both `Char`), the canonical scanning loop —
`while i < s.len() { let c = s.charAt(i) ?? ' '; if c.isAlpha() { … } }` with
maximal-munch lookahead — reads idiomatically. v0.5 is byte-oriented (each byte
maps to one `Char`); UTF-8 decoding into code points is a carry-forward. Codegen
adds `__vstr::char_at` (`std::optional<char32_t>`), `__vstr::chars`
(`std::vector<char32_t>`), and the four `__vstr::is_*(char32_t)` ASCII predicates;
the classifiers lower on a `Char` receiver, `charAt`/`chars` on a string
receiver. Five unit tests (charAt yields `Char?` / chars a `Vec[Char]`; the
classifiers type-check and `c == 'x'` compares; the Char-only guard rejects
`n.isDigit()` on an Int; the `chars()` Alloc gate fires/clears while charAt + the
classifiers stay gate-free; the codegen lowering) plus a transpile→compile→run
e2e (`examples/tokenize_demo.vst` + `tests/e2e/main_tokenize_demo.cpp`): a
hand-written tokenizer counting identifier and number tokens over a real
expression with maximal munch (so the digits in `bar123` belong to the
identifier, not a separate number), char-counting via the `chars()` iterator,
and char-vs-char equality. v0.5 carry-forwards: UTF-8 code-point decoding;
`isUpper`/`isLower`/`isPunct`; a `Char -> UInt8` / digit-value conversion; and a
zero-copy char iterator (today `chars()` materializes a vector).

That rounds out the bootstrap collection set: `Vec`, `String`, and `HashMap`
each have construction, reads, mutation, iteration where applicable, the string
read-borrow, inout-receiver discipline for their mutators, and (for strings) the
read-only slicing + search query surface, number↔string conversion, separator
splitting, and byte/char access for hand-tokenizing.

### 0c. I/O + entry-point (self-hosting equipment) (multi-session)

The third self-hosting equipment gap from the bootstrap assessment (after
multi-file modules §0 and collections §0b): a compiler must read its input,
write its output, see argv, and print diagnostics. The entry point already
works — `func main() -> Int32` becomes the C++ `main`, so a program with a
`main` compiles to a standalone executable (no driver).

**Slice 1 shipped** — **stdout output**: `print(Str)` / `println(Str)` are
builtin free functions (registered in `register_builtin_io`) that lower to
`std::print("{}", x)` / `std::println("{}", x)` (the argument is a formatting
*argument*, never the format string, so text containing `{` is safe). They take
a `Str`, so a literal, a borrowed view, or an owned `String` (via the slice-7
read-borrow) all work. Writing to stdout is an observable side effect, so they
require the `Log` capability — a printing function declares `using Log` or a
caller opens `with Log` — gated in capability.cpp on the builtin symbol (a
user-defined `print` is unaffected, in both the gate and the codegen intercept).
Proven end to end (`examples/io_demo.vst` compiles standalone and prints
"Hello, Vestra!").

**Slice 2 shipped** — **file I/O**: `readFile(path) -> String?` slurps a whole
file (nil when it can't be opened); `writeFile(path, contents) -> Bool`
truncates and writes, returning success. Both lower to `__vstr::read_file` /
`__vstr::write_file` prelude helpers (over `<fstream>`) and require a new `Fs`
capability — filesystem access is an observable side effect, like Net/Log — so
a file-touching function declares `using Fs`. This is the read/write a
self-hosting transpiler needs. Proven end to end (`examples/fileio_demo.vst`: a
write→read round-trip, printed, compiled standalone and run).

**Slice 3 shipped** — **command-line args**: `args() -> Vec[Str]` returns the
program's argv (argv[0] is the program name) as views with program lifetime. A
module-less `func main()` is the real C++ entry point, so it now emits as
`main(int argc, char** argv)` with an `__vstr::args_init(argc, argv)` prologue
(an in-module or parametered `main` stays an ordinary function); `args()` lowers
to `__vstr::args()`, a copy of the captured argv. It allocates the Vec, so it
carries the `Alloc` gate. This is how a self-hosting CLI (`vestrac input.vst`)
reaches its input path. Proven end to end (`examples/args_demo.vst` echoes its
args; the e2e runs it with `alpha beta`).

With stdout (slice 1), file I/O (slice 2), and argv (slice 3) — plus the
already-working `func main` entry point — the I/O equipment a self-hosting
transpiler needs is in place. Remaining nicety: formatted print over non-`Str`
values (today the caller interpolates to a `String` first, or uses `toString`).

**Slice 4 shipped** — **stderr output**: `eprint(Str)` / `eprintln(Str)` are the
stderr twins of print / println — the diagnostics channel a self-hosting
front-end needs so errors don't pollute the program's real stdout. Same `Str`
argument and same `Log` capability gate (writing to a standard stream is
observable), registered in `register_builtin_io` beside print/println and
sharing the builtin-symbol guard (`decl == nullptr`, so a user-defined `eprint`
isn't hijacked). Codegen mirrors the print lowering with a leading stream
argument: `std::print(stderr, "{}", x)` / `std::println(stderr, "{}", x)` (the
same shape the panic shim already used). Two unit tests (the Log gate fires
without the capability / clears with it; the codegen stderr lowering) plus a
standalone-`func main` e2e (`examples/eprint_demo.vst`) whose two redirected run
steps *prove* the stream routing — keeping only stdout (`2>/dev/null`) shows the
`OUT:` line and not the `ERR:` diagnostics, keeping only stderr (`1>/dev/null`)
shows the reverse (and that `eprint` + `eprintln` join one line). v0.5
carry-forward: formatted variants over non-`Str` values (`eprintln(n)` today
wants `n.toString()` first).

**Self-hosting proof-of-concept shipped** — with §0 (modules), §0b (collections
+ the full string surface: query / number↔string / split) and §0c (I/O + entry
point) all in place, the equipment stack now carries a real standalone program
end to end. `examples/selfhost_demo.vst` is a `func main` word-frequency +
integer-sum reporter that reads a file path from `args()`, `readFile`s it,
tokenizes with `split` (lines then words), tabulates with a `Vec[Str]` of
first-seen words and a `HashMap[Str, Int]` of counts, parses integer tokens with
`toInt`, renders the report with `String.new` + `append` + `Int.toString`, echoes
it with `print`, and `writeFile`s it — the read→tokenize→tabulate→emit shape a
front-end takes. It transpiled clean on the first attempt (no equipment gaps
surfaced), and the e2e (`tests/e2e/selfhost_input.txt` fixture) is a four-step
transpile→compile→run→verify: the run asserts the summary on stdout, and a
`grep` step verifies the written output file. This is the milestone the whole
self-hosting arc was building toward — a non-trivial Vestra program that does
real String/Vec/HashMap work compiles to a standalone executable and runs. Next
real gaps it points at (the actual front-end, not toy I/O): a `Char`/byte
iterator for hand-tokenizing, `HashMap` iteration (today set/get/contains/len
only — the PoC tracks insertion order in a side `Vec` to emit deterministically),
and `eprint`/stderr for diagnostics.

**Char/byte tokenizing shipped** (§0b slice 12) — the first of the PoC's gaps:
`s.charAt(Int) -> Char?`, `s.chars() -> Vec[Char]`, and `Char` ASCII classifiers
(`isDigit`/`isAlpha`/`isAlnum`/`isSpace`), so a hand-written scanning lexer
(comparing `c == '('`, maximal munch) is now expressible. See §0b above.

**HashMap iteration shipped** — the second PoC gap. `m.keys() -> Vec[K]`,
`m.values() -> Vec[V]`, and `m.entries() -> Vec[(K, V)]` (the eager-Vec trio,
mirroring `split` / `chars`), so the map's contents are finally walkable —
`for (k, v) in m.entries()` destructures the 2-tuple element directly. The
enabling codegen change beyond the three methods + their Alloc gate (keyed on a
HashMap receiver) was teaching the **Vec range-for path to emit a C++ structured
binding** (`for (auto&& [k, v] : vec)`) when the loop variable is a tuple
pattern — previously only the iterator-protocol (`.next()`) for-loop handled
tuple patterns, and a Vec has no `.next()` to fall through to; nested sub-tuples
reuse the same `collect_tuple_pat_names` / follow-on machinery as `let`
destructuring. Iteration order is unspecified (the underlying `unordered_map`
bucket walk), so a caller wanting determinism sorts the Vec or keeps a side
insertion-order Vec (as the PoC does). Three unit tests (the Vec[K]/Vec[V]/
Vec[(K,V)] shapes incl. `for (k, v)` destructuring; the Alloc gate fires/clears
while the reads stay gate-free; the codegen `__vstr::map_*` + structured-binding
lowering) plus a transpile→compile→run e2e (`examples/hashmap_iter_demo.vst` +
`tests/e2e/main_hashmap_iter_demo.cpp`) over word-count maps, asserting only
order-independent aggregates (distinct count via `keys`, total via `values`, and
`len(key)*count` / max via destructured `entries`). v0.5 carry-forwards: a
stable/insertion-ordered map (today unordered); direct `for (k, v) in m` without
the `.entries()` call; and a zero-copy entries iterator (today materializes a
Vec). With both gaps closed, the only remaining PoC follow-on is `eprint`/stderr.

**Self-hosting lexer milestone shipped** — the genuine self-hosting step the whole
equipment arc was for: a real lexer for a small expression language, *written in
Vestra*. `examples/lexer_demo.vst` declares a `TokKind` enum and a `Token` struct
(`{ kind, text }`), then `tokenize(src) -> Vec[Token]` scans the input by
character with maximal munch — whitespace skipped, a letter starts an identifier
(`charAt` + `isAlpha`/`isAlnum`), a digit a number, anything else a
single-character punctuation token (`punctKind` dispatching on char literals,
`c == '+'`), each token's `text` a `slice` that borrows the source. `main` reads
the source path from `args()`, `readFile`s it, walks the `Vec[Token]` (matching
each kind to a name and `append`ing a `"<KIND> <lexeme>"` listing), `writeFile`s
the listing, prints the token count, and reports each unexpected character on
stderr via `eprint`. So it exercises the entire stack — enums, structs, `Vec`,
`match`, `while`, char access + classification, `slice`, `String` building,
`toString`, argv, file I/O, stdout, and stderr — to do real front-end work.
Five-step e2e (`tests/e2e/lexer_input.txt` clean fixture, `lexer_bad.txt` with a
stray `@`): transpile → compile → run (12 tokens, summary on stdout) → verify the
written listing → and a bad-input run asserting the `lex error: unexpected '@'`
on stderr. This is the first real phase of a self-hosted front-end running as a
standalone Vestra executable. Natural next steps: a recursive-descent parser over
this token stream (building an AST), then a tiny evaluator / pretty-printer — each
a further self-hosting phase.

**Parser fix — error-recovery forward-progress guard (found by the lexer work).**
Writing the lexer surfaced a real compiler bug: a malformed `struct`/`enum` body
*followed by another declaration* sent the parser into an unbounded-memory hang
(it ballooned to 20 GB before being killed). Root cause: `sync_to_decl()` stops
*at* a declaration keyword (or EOF) without consuming it, so when a body loop's
error recovery landed on a following `struct`/`func`/… (the body's `}` missing),
the `while (!RBrace && !at_end())` loop re-entered at the same token forever,
emitting a diagnostic each pass — a single malformed decl escaped only because
recovery reached EOF. Fix: a forward-progress guard at each of the four body-loop
recovery sites (struct / enum / protocol / extension) — capture the token cursor
before `sync_to_decl()`, and if it didn't advance, `break` out of the body (the
top-level loop, which already has a `should_stop()` error cap, then parses the
next declaration). Two parser regression tests pin it (a malformed enum then a
struct, and a malformed struct then a well-formed one that recovery still finds);
the test merely *returning* is the assertion that the hang is gone. The whole
class of "malformed input → runaway" in these loops is now closed by the
guarantee that every iteration consumes at least one token.

**Self-hosting phase 2 — recursive-descent parser shipped.** The front-end's
next phase after the lexer: a recursive-descent expression grammar with operator
precedence and parentheses, written in Vestra. `examples/calc_demo.vst`
implements `expr = term (('+'|'-') term)*`, `term = factor (('*'|'/') factor)*`,
`factor = number | '(' expr ')'`, with `factor`/`expr` mutually recursive for the
parenthesis rule (the single-unit two-pass resolution handles the forward
reference regardless of definition order). Each parse function takes
`(source, position)` and returns a `(value, nextPosition)` tuple — Vestra tuples
thread the cursor through the recursion functionally, with no shared mutable
parser state, and the returned pairs destructure with `let (v, j) = …`. It
evaluates as it parses (the calculator shape), so it computes a value directly;
a materialized `Box[Expr]` AST is the follow-on. Uses only the char toolkit +
`slice` + `toInt`, so it allocates nothing and needs no capability. The e2e
(`tests/e2e/main_calc_demo.cpp`) pins the things a real grammar must get right —
operator precedence (`2 + 3 * 4 == 14`), left-associativity (`10 - 3 - 2 == 5`),
parentheses (`(2 + 3) * 4 == 20`), deep nesting (`2 * (3 + (4 * 5)) == 46`), and
a larger mixed expression. With the lexer (phase 1) and this parser (phase 2),
two of a front-end's core phases now run as standalone Vestra executables.
Natural next steps: a materialized AST (needs recursive `Box[Expr]`
enum payloads), or wiring the parser to consume the lexer's `Vec[Token]` stream
rather than re-scanning characters.

**Self-hosting phase 3 — materialized AST shipped** (plus the codegen fix it
needed). `examples/calc_ast_demo.vst` builds an explicit tree first and then
walks it twice — `eval` folds it to a value, `render` pretty-prints it fully
parenthesized — over a recursive enum whose `binary` case Boxes its children
(`enum Ast { case num(Int); case binary(Box[Ast], Char, Box[Ast]) }`), the
canonical move-only algebraic-data-type AST shape. **The recursive `Box`-payload
enum *type* already worked** (the type system handles the self-reference through
`Box`, which lowers to `std::unique_ptr<Ast>` and breaks the infinite size);
the blocker was that an `Ast` owns `Box`es so it is **move-only**, and codegen
only emitted `std::move` for explicit `sink` params — so consuming a move-only
value anywhere else copied, a hard compile error. Fix: **`Box.new(value)` now
moves its argument** (`std::make_unique<T>(std::move(value))`) — the box
unconditionally takes ownership, so the move is always correct (required for a
move-only `T`, harmless for a copyable one). With that, the AST builds cleanly:
the parser threads its cursor through an `inout Int` and returns each node by
value (C++ moves a local on return, so no tuple-of-move-only copy is needed),
`Box.new` moves the operands into the heap cells, and `eval`/`render` read each
child through `.value` (a `*unique_ptr` borrow — no move). Two unit tests (the
updated `Box.new` move lowering; a recursive `Box`-payload enum that builds via
`make_unique(std::move(...))` and reads children through `(*l)`) plus a
transpile→compile→run e2e (`tests/e2e/main_calc_ast_demo.cpp`) whose `render`
checks pin the *tree shape* — `2 + 3 * 4` → `(2 + (3 * 4))`, `10 - 3 - 2` →
`((10 - 3) - 2)` — making precedence/associativity structural, not just a number.
Two gotchas worth recording: the enum can't be named `Expr` (reserved §12.4
builtin — it silently shadows to a non-enum, producing confusing
"non-enum type Expr" errors), and at the time this shipped general move-on-consume
was incomplete (only `sink` params and `Box.new` moved) — now closed, see below.
v0.5 carry-forwards: wiring this AST to the lexer's `Vec[Token]` stream; and
parse-error reporting (today a malformed expression just stops).

**Move-on-consume pass shipped** — the general follow-on, so an owned move-only
value can be consumed anywhere it lands by value, not just into a `sink` param.
A new codegen predicate `is_move_only(type)` marks the types whose C++ lowering
is *non-copyable* — `Box` (→ `std::unique_ptr`) and, transitively, any
struct/enum/tuple/`Vec`/`Optional`/`HashMap` that reaches a `Box`; the nominal
cases are precomputed once per unit by a fixpoint over the decls
(`compute_move_only_types`). Crucially it is **only** the Box family: `Vec` /
`String` / `HashMap` are owning but *copyable* in C++, so they are deliberately
left alone — moving them could break a legitimate later use. A helper
`emit_value` wraps an expression in `std::move` exactly when it is a move-only
*lvalue* (an identifier / field / index — never a prvalue, which already
moves/elides), applied at the by-value sites that previously copied: **let/var
initializers and tuple-literal elements**. General call arguments are left
untouched on purpose — a read param is `const&` (binds an lvalue with no copy)
and an `inout` param needs a real lvalue (a moved rvalue won't bind), so the move
belongs only at genuine by-value sites. With this, the **tuple-returning parser
style now works**: `let (rest, c) = parse(...); var acc: Ast = rest; return
(acc, c)` moves the move-only `Ast` at the binding and into the returned tuple,
while the `Int` rides along as a copy. One codegen test pins the precision — a
move-only `Box` binding/tuple-element gets `std::move`, a copyable `Vec` binding
does not, and a call-result element is never wrapped. Full suite green (1079) —
no existing copyable-type lowering changed.

**Move-on-consume follow-ons shipped — construction args + imported nominals.**
Two carry-forwards from the first move slice, now closed. (1) **Struct / enum
construction args**: passing a move-only lvalue into an enum-case payload
(`Ast.bin(a, b)`) or a struct field (`Pair(lo: x, hi: y)`) is a by-value
aggregate position, so it copied — a hard error — until `emit_value` was applied
at all five construction sites (bare / qualified / indexed struct, and the
MemberExpr / leading-dot enum-case forms). Like the tuple/let sites these carry
no `inout` risk (a field / payload is never `inout`), so the move is
unconditionally safe there. Construction from a prvalue (`Box.new(...)`) already
moved; this covers the lvalue case. (2) **Imported move-only nominals**: the
`compute_move_only_types` fixpoint only scanned the importing unit's own decls,
so a `let`/`var`/tuple/construction of an *imported* move-only type (an enum /
struct from another module that owns a `Box`) wasn't recognized and copied. Fix:
seed the by-name maps with the imported nominal decls (the keys of
`imported_qualifiers_`, populated before `emit`) so the fixpoint covers them too;
local decls win a name clash. One codegen test (enum-payload + struct-field
construction from move-only lvalues both `std::move`) and an extension of the §5
multifile e2e — `util.math` now exports a move-only `Tree` (its `node` case Boxes
itself), and `demo.app` binds an imported `util.math.Tree` from one local to
another (a move that only compiles because the importer recognizes the imported
nominal as move-only) and folds it across the module boundary. Full suite green
(1083). The move story is now complete for all by-value sites — let/var, tuples,
struct/enum construction — across the unit boundary; the only remaining
non-`sink` call-arg case is a hypothetical by-value (non-`inout`, non-`const&`)
parameter mode, which v0.5 does not have.

**Match-result-type fix shipped** — the orthogonal bug the move pass surfaced. A
`match` lowers to a lambda (a `std::visit` visitor for a payloaded enum, an IIFE
for a bare enum / value scrutinee) whose return type was deduced (`-> auto`).
When one arm was a bare integer literal (C++ `int`) and a sibling was an `Int`
expression (`intptr_t`), the per-arm deduced types disagreed and `std::visit`'s
single-return-type requirement rejected it — so a fold like `case .empty: 0`
beside `case .cons: head + sum(...)` failed to compile. Fix: emit an explicit
lambda return type — the match's resolved result type — at all four lowering
sites (value-scrutinee IIFE, guarded enum if-chain, bare-enum switch IIFE,
payloaded `std::visit`), via a shared `emit_match_result_type` helper; each arm
then converts to that one type. It falls back to `auto` for a Unit / Never /
unresolved match (a statement-position match, where void deduction is already
consistent). One codegen test (a payloaded-enum match with mixed `int`-literal /
`intptr_t` arms emits `-> std::intptr_t`) and a transpile→compile→run e2e
(`examples/movelist_demo.vst`) — a move-only recursive list built by a
tuple-returning builder (move-on-consume) and folded by mixed-arm matches
(`sum` / `length` / `maxElem`, each with a `case .empty: 0` base) — which
exercises *both* this fix and the move pass together. Full suite green (1083).

**Self-hosting phase 4 — a two-phase front-end shipped.** The capstone: a real
lexer→parser pipeline in one Vestra program, where the parser consumes a *token
stream* rather than re-scanning characters. `examples/frontend_demo.vst` runs
`source --lex→ Vec[Token] --parse→ Box[Ast] --eval→ Int`: phase 1 (`lex`) scans
the source with the char toolkit into a `Vec[Token]` (each `Token` a `{ kind:
Tok, value: Int }` value, so the stream is an ordinary copyable `Vec`); phase 2
(`pExpr`/`pTerm`/`pFactor`) is recursive descent over that stream — `peekKind`/
`peekVal` index into the `Vec[Token]` (via `.get(i)` + field access on the
`Token?`), token dispatch is enum equality (`peekKind(ts, i) == Tok.plus`), and
the cursor threads through an `inout Int` — building a move-only `Box[Ast]` tree;
phase 3 (`eval`) folds the tree with a mixed-arm match. It pulls together nearly
the whole language at once — char access + classification, `Vec` as a stream,
enums (token kind + AST), a recursive `Box` ADT built with move-on-consume, enum
`==` dispatch, `inout` threading, and the match-result-type fold — and it
transpiled + compiled clean on the first attempt (every supporting fix from the
recent slices held). The e2e (`tests/e2e/main_frontend_demo.cpp`) checks the
lexer's token counts and the end-to-end value over precedence / associativity /
parentheses / nesting (`2 + 3 * 4 == 14`, `(2 + 3) * 4 == 20`, `2 * (3 + (4 *
5)) == 46`, a larger mixed expression). Four of a front-end's phases now run as
standalone Vestra executables — lexer, char-level parser, AST builder, and now a
token-stream lexer+parser+evaluator pipeline.

**Parse-error reporting shipped (plus the §9 codegen fixes it needed).**
`examples/parser_errors_demo.vst` is the token-stream parser written with
Vestra's §9 error channel: each parse function `throws(ParseError)` and
propagates with `try`, so a structural error (an unexpected token, an unclosed
`(`, trailing input after a complete expression) unwinds the whole recursive
descent to the entry point as a *typed* error rather than producing silent
garbage. `parse(s) throws(ParseError) -> Int` lowers to a function returning
`std::expected<Int, ParseError>`, so the e2e driver inspects the value path and
the specific error for each malformed input, and `evalOr` recovers in-Vestra via
`do/catch`. Building the **move-only** `Box[Ast]` tree *through* `try` surfaced
three codegen gaps in the §9 machinery, all fixed: (1) the `try`-unwrap
(`auto v = *expected`) now **moves** a move-only result out of the
`std::expected` (a copy was a hard error); (2) `emit_value` treats a hoisted
`try` as a move-only lvalue, so `let x = try f()` moves the hoisted temp into the
binding; and (3) the conditional-hoist lambda for a **Unit-typed** node (an `if`
whose branch holds a propagating `try` but yields no value) now emits a terminal
success `return {};` — without it the `std::expected<void, E>` lambda returned an
indeterminate value and its `has_value()` check was UB (the bug manifested as a
spurious parse error on *valid* input). One restructuring note for parser
authors, recorded in the example: a `try` and a value-`return` in the *same*
`if`-branch still trips the cond-hoist (the branch lifts to an `expected<void>`
lambda the `return` escapes wrongly) — keep the `try` and the `return` at the
function's statement level (a `throw` inside an `if` is fine). One codegen test
(the unwrap-move + the Unit cond-hoist success return) and a transpile→compile→
run e2e (`tests/e2e/main_parser_errors_demo.cpp`) that checks a typed error for
each of unexpected-token / unclosed-paren / trailing-input / unknown-char / empty
input, plus `do/catch` recovery. Full suite green (1087).

**Variable-binding mini-language shipped — a symbol table.**
`examples/calc_vars_demo.vst` grows the front-end into a small language with
variables: a program is `stmt (';' stmt)*`, a statement is `ident '=' expr` (an
assignment that binds the name) or a bare `expr`, and an identifier in an
expression is *looked up* — so later statements use values bound by earlier ones.
The environment is a `HashMap[Str, Int]` over the existing collection surface:
assignment is `env.set(name, v)`, a reference is `env.get(name) ?? 0` (an
undefined name reads as 0). The token stream now carries identifier names as
`Str` slices of the source (`Token { kind, value, name }`, still a copyable value
so `Vec[Token]` stays an ordinary stream), so the map keys borrow the source,
which lives for the whole run. Evaluation happens as the parser descends (the env
threads through `pExpr`/`pTerm`/`pFactor` as a read parameter for lookups; the
statement driver `set`s it between statements), so the slice's focus is the
symbol table rather than another AST. It needed no compiler changes — it composes
the char toolkit, `Vec`, `HashMap`, enums, structs, and `inout` threading that
the recent slices built — and transpiled + compiled clean on the first attempt.
The e2e (`tests/e2e/main_calc_vars_demo.cpp`) checks binding + reference
(`x = 5; x + 3 == 8`), several bindings (`a = 2; b = 3; a * b == 6`), a binding
that uses an earlier one (`x = 10; y = x * 2; x + y == 30`), reassignment that
reads the old value (`x = 1; x = x + 1; x = x + 1; x == 3`), multi-letter names,
and undefined / empty programs. The self-hosting front-end now spans lexing,
token-stream parsing, a move-only AST, evaluation, typed parse errors, and a
variable environment — a real, if small, interpreter.

**Control flow shipped — conditionals + comparisons.**
`examples/calc_if_demo.vst` gives the variable mini-language branching: `if cond
then a else b` is an expression and `<` / `>` compare (yielding 1 / 0), so a
program can compute `abs` / `max` over its variables. `if` / `then` / `else` are
recognized as keyword tokens *at lex time* — a string `match` on each identifier
maps it to a `kwIf` / `kwThen` / `kwElse` token, so the parser dispatches on enum
equality rather than re-comparing strings. Because the expression language is
side-effect free — a reference only reads the environment, and division is made
total (`x / 0 = 0`) — the conditional evaluates *both* branches and selects the
taken one, which keeps the eval-as-parse shape simple without changing results
(the untaken branch, even `10 / 0`, computes harmlessly). It needed no compiler
changes and transpiled + compiled clean on the first attempt. The e2e
(`tests/e2e/main_calc_if_demo.cpp`) checks comparisons, basic selection, a
condition over a comparison + variable, `max(a, b)` and `abs(n)` via conditionals,
a nested `if` in the else branch, and the total-division safety of the untaken
branch. The interpreter now has lexing, token-stream parsing, a move-only AST,
evaluation, typed parse errors, a variable environment, and control flow — the
core of a usable little language.

**User-defined recursive functions shipped — the mini-language is now
(recursively) programmable.** `examples/calc_fn_demo.vst` adds `fn name(p) = expr`
definitions and `name(arg)` calls, so the language can express factorial and
Fibonacci:

    fn fact(n) = if n < 2 then 1 else n * fact(n - 1);  fact(6)     -> 720
    fn fib(n)  = if n < 2 then n else fib(n-1) + fib(n-2);  fib(10) -> 55

Two ideas make recursion work in the eval-as-parse design. (1) A function is
stored as the *start index* of its body in the token stream (the body is one
expression, so re-running the parser from there consumes exactly it); a call
re-runs the parser over that body with a fresh single-binding environment, so
each recursion level has its own cursor and scope. The function is registered
*after* its body is skip-parsed at definition time, so the define-time scan
doesn't fire the (not-yet-registered) self-call. (2) The conditional became
**lazy**: a `live` flag threads through every rule, and the untaken `if` branch
is parsed-to-advance-the-cursor but not evaluated — *no function call fires in a
dead branch* — which is exactly what lets a recursive base case terminate
(eval-both, fine for the non-recursive calc_if, would recurse forever here). The
evaluation context — variable `env`, the `fparam` / `fbody` function table, and
`live` — threads through `pExpr` / `pCmp` / `pSum` / `pTerm` / `pFactor` /
`callFunc`. It needed no compiler changes; the capped run terminated (the lazy
conditional did its job). The e2e (`tests/e2e/main_calc_fn_demo.cpp`) checks
define+call, an expression argument, nested calls (`inc(inc(inc(0)))`), a call
over a variable, and factorial / Fibonacci at several inputs. The self-hosting
interpreter now spans lexing, token-stream parsing, a move-only AST, evaluation,
typed parse errors, variables, control flow, and recursive functions — a small
but genuinely Turing-capable language written in Vestra.

**Multi-argument functions shipped.** `examples/calc_multifn_demo.vst` grows the
single-parameter functions into a comma-separated parameter list, so the language
expresses binary and n-ary operations — max, gcd, power, 3-argument sum:

    fn max(a, b) = if a < b then b else a;            max(3, 7)   -> 7
    fn pow(b, e) = if e < 1 then 1 else b * pow(b, e - 1);  pow(2, 10) -> 1024
    fn gcd(a, b) = if b < 1 then a else gcd(b, a - (a / b) * b);  gcd(48, 36) -> 12

The function table now stores each function's parameters as a `Vec[Str]` — a
*value in a `HashMap[Str, Vec[Str]]`* (which works: the map lowers to
`unordered_map<string_view, vector<string_view>>`) — and a call collects its
argument values into a `Vec[Int]` (parsing the comma-separated list, advancing
the cursor) and binds them positionally into a fresh environment. Everything else
carries over unchanged: the lazy conditional (`live`) still gates recursion, so
two-argument recursion (power, gcd — the latter with integer division for the
modulo) terminates; zero- and one-argument functions still work (an empty list
parses to an empty `Vec`). It needed no compiler changes and transpiled +
compiled clean on the first attempt; the capped run terminated. The e2e
(`tests/e2e/main_calc_multifn_demo.cpp`) checks 2- and 3-parameter functions,
positional/expression arguments, nested cross-function calls, `pow` / `gcd`
recursion at several inputs, and the zero/one-argument cases. The self-hosting
interpreter is now a small applicative language with recursion and n-ary
functions, written entirely in Vestra.

**`while` loops shipped — the language becomes imperative.** A loop needs
*mutable* state to make progress, which is the piece the language lacked: every
prior layer threaded the environment *read-only*. `examples/calc_while_demo.vst`
adds a **statement layer** above the expression layer that threads an **`inout`
environment** — `pStmt` / `pBlock` mutate it (assignment, the loop body), while
the expression rules still read it (passing the `inout` env onward as a read
argument, which Vestra allows). A `while expr do stmt` records its condition's
and body's token positions and, each iteration, re-runs the parser over them
against the shared environment — the same token-range-replay trick a function
call uses — so the body's assignments persist and the condition eventually
flips; a `{ … }` block sequences statements, and a per-loop guard bounds a
runaway. This is the first place the env is mutated *inside* evaluation, so it
also exercises the `inout`-param-passed-onward patterns (probed first: an `inout`
binding flows both to a read parameter and, via `&env`, to another `inout`
parameter). It needed no compiler changes and transpiled + compiled clean on the
first attempt; the capped run terminated (the loops converge). The e2e
(`tests/e2e/main_calc_while_demo.cpp`) checks iterative sum / factorial / power,
a single-statement body, a zero-iteration loop, iterative Fibonacci, a loop that
calls a function each iteration, and **nested loops** — plus that conditionals
and functions still compose with loops. The self-hosting interpreter now has
lexing, token-stream parsing, a move-only AST, evaluation, typed parse errors,
variables, control flow, recursive n-ary functions, and imperative loops — a
genuinely usable little language, written entirely in Vestra.

**The interpreter as a standalone tool shipped.** `examples/interp_demo.vst`
packages the whole mini-language as a `func main` executable: it reads a program
file named on the command line (`args()` + `readFile`, Fs-gated), runs it, and
the program's output appears on stdout. The one new language feature is a `print
expr` statement, which evaluates and emits its value (`println(v.toString())`);
because printing is a side effect, the *statement* layer (`pStmt` / `pBlock` /
`runProgram`) now carries the `Log` capability while the read-only expression
layer does not — a clean illustration of the capability row tracking exactly
where effects happen. So the entire self-hosting arc — lexer, token-stream
parser, variables, conditionals, recursive n-ary functions, while loops — becomes
something you invoke: `interp program.calc`. The e2e runs the built executable on
a committed program (`tests/e2e/interp_program.calc`: a recursive factorial, a
while-loop sum, a max function, each `print`ed) and asserts both the final
`result:` line and — via a `grep` verify step distinct from `main`'s own echo —
that a program `print` (the recursive `fact(5) = 120`) reached stdout. It needed
no compiler changes and transpiled + compiled clean on the first attempt; the
capped run terminated. This is the capstone of the interpreter arc: a self-hosted
interpreter for a small imperative language with recursion, written in Vestra,
that you can run on a file.

**A richer value type shipped — a tagged `Value`.** `examples/calc_value_demo.vst`
makes the interpreter's values an algebraic data type rather than just `Int`:
`enum Value { case int(Int); case boolean(Bool); case str(Str) }`. A literal
carries its type (`42`, `true`, `"hi"` via a string-literal lexer rule), integer
arithmetic produces `int`, `<` / `>` / `==` produce `boolean` (`==` is
structural and cross-type-safe — a nested match per tag, using `Str ==` for
strings), and `if` may pick between branches of *different* types (so `if 1 < 2
then "small" else 99` is a string). `format` renders a value by its tag, so the
e2e's expected output pins the result *type* as well as its value: `"14"` (int),
`"true"` (bool), `"small"` (str). The one subtlety, found and fixed while
writing it: `pTerm` / `pSum` must only collapse to an integer *when an arithmetic
operator actually follows* — otherwise a bare `true` or `"hi"` would be coerced
to `int` (it was, until each rule learned to return its single factor's `Value`
unchanged when no `*`/`+` follows). That restructure also tripped a precise
ownership rule — `if c { return first }` then `asInt(first)` reads as
use-after-move because the checker doesn't model the early `return` skipping the
later use — sidestepped by moving the arithmetic inside the operator branch so
`first` is consumed on exactly one path (a recorded language-ergonomics note, not
a compiler change). No compiler changes; transpiled + compiled clean after that
fix; the e2e covers arithmetic, comparison, structural / cross-type equality,
string + boolean literals, type preservation, and heterogeneous `if` branches.
The interpreter now manipulates integers, booleans, and strings as first-class
tagged values — the data side of a dynamically-typed language, in Vestra.
Natural next steps: string concatenation (an owned `String` in the value), or
variables / `print` over `Value` in the full interpreter.

**The tagged `Value` threaded through the *full* interpreter shipped.**
`examples/interp_value_demo.vst` merges the two prior strands: the focused
evaluator's `Value` (int / bool / str) is now the value type of the *whole*
standalone tool — lexer, token-stream parser, the variable environment,
recursive n-ary functions, `while` loops, and `print`. The environment is now
`HashMap[Str, Value]` (was `HashMap[Str, Int]`), so a variable can hold an
integer, a boolean, or a string; function arguments are a `Vec[Value]` and the
call frame a `HashMap[Str, Value]`; `print` renders any value by its tag via
`format`; comparison / `==` produce booleans; and `if` may pick between branches
of *different* value types. The one thing worth recording: storing a move-only
`Value` in a `HashMap` and reading it back (`env.set(name, v)` then
`env.get(name) ?? Value.int(0)`) just works — every Vestra enum is
copy-constructible at the C++ layer, so the read out of the map copies rather
than moves, and the checker did not flag a use-after-move (no compiler change
needed). The `str` payload is a `Str` view that borrows the program source,
which outlives the run. It transpiled and compiled clean on the first pass; the
e2e runs the built executable on a committed program
(`tests/e2e/interp_value_program.calc`) and asserts a string variable (`hello`),
recursion over `Value` (`fact(6)` = 720), a while-loop accumulation (55), a
structural `==` (`true`), and a heterogeneous `if` (`big`) all flow through the
tagged value type, plus the final `result: done` (the last statement is a
string). The self-hosting interpreter is now a small dynamically-typed language —
variables, recursion, loops, and output, all over a tagged value. Natural next
steps: string concatenation (an owned `String` in the value, the first heap-owned
payload), or comparison operators producing typed errors on bad operands.

**String concatenation shipped, the interpreter's first heap-owned payload.**
`examples/interp_concat_demo.vst` changes the tagged `Value`'s string case from a
borrowed view (`case str(Str)`) to an *owned* `case str(String)`, because a
concatenation result is a fresh string that is not a slice of the program source.
`+` becomes string-led: if the left operand is a string, the whole run of `+`s
concatenates (each operand rendered by its tag via `format`, so `"n=" + 42` is
`n=42`); otherwise `+` stays integer arithmetic, selected by an `isStr(Value)`
test on the first operand. The interesting part is ownership. An owned-`String`
`Value` now rides through the variable environment (`HashMap[Str, Value]`) and the
`Vec[Value]` argument lists, and a read back out (`env.get(name) ?? Value.int(0)`,
`args.get(j) ?? Value.int(0)`) copies the owned value, because Vestra enums are
copy-constructible at the C++ layer, so the heap payload survives being stored and
re-read with no compiler change. This was verified first with a standalone probe
(an owned-String enum into a `HashMap` and back), then built into the full
interpreter; it transpiled and compiled clean. `String == String` in `valEq`
worked directly (owned-String structural equality). The e2e runs the built tool
on a committed program (`tests/e2e/interp_concat_program.calc`) asserting literal
concatenation (`Hello, world`), variable concatenation (`Hi Vestra!`), string-led
mixed concat (`n=42`), a recursion that builds owned strings through `Vec[Value]`
(`rep("ab", 3)` is `ababab`), and an owned `String` round-tripping through the env
in a `while` loop (`acc = acc + "x"` gives `xxx`). The dynamically-typed
interpreter now produces and stores heap-owned strings, not just borrowed views.
Natural next steps: a string-length / substring builtin on string values, or
`print` of multiple comma-separated values.

**String builtins shipped: `len` and `substr` over string values.**
`examples/interp_strlib_demo.vst` gives the mini-language two built-in functions
so it can take strings apart, not just build them up: `len(s)` returns the byte
length of a string value (0 otherwise), and `substr(s, a, b)` returns the
`[a, b)` sub-string. They lower onto the synthesized String/Str query surface
(`s.len()`, `s.slice(a, b)`); `substr` builds a fresh owned String from the
sliced view (the view borrows the receiver, so the result must own its bytes),
and `slice` clamps out-of-range / inverted spans to empty, so the builtins never
fault on bad indices. The builtins are dispatched in `callFunc` after the
user-function lookup, as a fallback for names a program did not define as `fn`s,
so a user can still define a function of any other name. The one thing that bit:
the dispatch was first written as `if name == "len"`, which the checker rejected
(`binary operator operands of different types: Str vs StrConst` — a `Str`
variable cannot be `==`-compared to a string literal); the working idiom is a
`match name { case "len": ...; case "substr": ...; default: ... }`, the same
string-literal matching `wordKind` already uses in the lexer (recorded as a
language-ergonomics note, not a compiler change). The e2e runs the built tool on
a committed program (`tests/e2e/interp_strlib_program.calc`) asserting a length
(`len("hello world")` = 11), slicing (`substr(s, 0, 5)` = `hello`,
`substr(s, 6, 11)` = `world`), the empty case (`len("")` = 0), nested builtins
(`len(substr(s, 0, 5))` = 5), a user function that calls a builtin (`firstFive`),
a substring of a string literal (`substr("Hello, Vestra", 7, 13)` = `Vestra`),
and a clamped out-of-range span (`substr(s, 6, 100)` = `world`). The
dynamically-typed interpreter can now both construct and decompose strings.
Natural next steps: `find` / `contains` builtins (the search half of the string
surface), or `print` of multiple comma-separated values.

**Multi-value `print` shipped.** `examples/interp_print_demo.vst` grows the
`print` statement from a single expression to a comma-separated list, printed on
one line, space-separated (Python-style) and newline-terminated. A single-value
`print` is just the one-element case, so every earlier program prints
identically (the e2e suite for the prior interpreter slices is unchanged). The
`print` arm of `pStmt` parses the first expression, then while a comma follows
parses the next, appending `" "` + its `format` to one line `String`, and
`println`s the line once; each operand is rendered by its tag, so a single
`print` can mix ints, bools, and strings. Building the line is unconditional but
only the `println` is gated on `live`, so re-parsing a body (the while-loop and
untaken-branch skip passes) advances the token cursor identically. The statement
returns the last value. No compiler change. The e2e runs the built tool on a
committed program (`tests/e2e/interp_print_program.calc`) asserting a two-string
line (`hello world`), a labelled int pair (`x = 10 y = 20`), an expression in the
list (`sum = 30`), a three-int line (`1 2 3`), the single-value case (`single`),
builtins in the list (`s, len(s), substr(s, 0, 2)` = `abc 3 ab`), and a mixed
string/bool/string line (`mixed: true end`). The interpreter's output side is now
a real `print`, not a one-value echo. Natural next steps: `find` / `contains`
string builtins (the search half of the surface), or a string-keyed value map as
a first compound data type.

**Search builtins shipped: `find` and `contains`.**
`examples/interp_search_demo.vst` adds the search half of the string surface to
the mini-language, completing the `len` / `substr` set with the two operations
that locate substrings: `find(s, needle)` returns the index of the first
occurrence (or -1 when absent), and `contains(s, needle)` returns whether the
needle occurs at all. Both lower onto the synthesized String/Str search surface
(`s.find(needle)`, `s.contains(needle)`); `find`'s `Int?` result becomes -1 via
`?? -1`. Each operand is a string value matched out to its payload through a
nested `match` (a non-string haystack or needle yields -1 / false rather than
faulting), and the owned-`String` binding coerces to the `Str` the search methods
expect at the call. They join `len` / `substr` in the `callFunc` dispatch
`match`, after the user-function lookup, so a program can still define its own
function of any other name. No compiler change. The e2e runs the built tool on a
committed program (`tests/e2e/interp_search_program.calc`) asserting a found
index (`find("hello world", "world")` = 6), first-occurrence (`find(s, "o")` =
4), the absent case (-1), `contains` true / false, the empty-needle case
(`contains(s, "")` = true), a user function that calls a builtin (`locate`), and
a four-builtin composition that slices from a found index to the end
(`substr(s, find(s, "world"), len(s))` = `world`). The interpreter's string
library is now construct (concat), measure / slice (`len` / `substr`), and search
(`find` / `contains`). Natural next steps: a string-keyed value map as the
interpreter's first compound data type, or `split` returning a list value.

**A string-keyed value map shipped, the interpreter's first compound data type.**
`examples/interp_map_demo.vst` adds a fourth `Value` case,
`case map(HashMap[String, Value])`, which makes `Value` recursive (a map holds
values, a value may be a map) and gives it its first move-only heap-container
payload. Four functional builtins operate on it: `mapNew()` (a fresh empty map),
`put(m, k, v)` (a new map equal to `m` with key `k` bound to `v`), `get(m, k)`
(the value at `k`, or `int(0)` when absent), and `has(m, k)` (a bool). Maps are
value-semantic and functional: `put` copies the backing map, inserts, and returns
the new map, so `m = put(m, k, v)` is the update idiom, which falls out naturally
from the fact that a map-bearing `Value` already deep-copies when read back out of
the environment. Keys are owned `String`s (the map calls `strOf` to copy the key
bytes), not `Str` views, so a computed key such as `"k" + "ey"` is safe rather
than a dangling borrow of a temporary. This was de-risked with two standalone
probes before any interpreter change: the first proved a recursive `enum` through
`HashMap`, the env round-trip of a map value, and a deep-copying functional `put`
(`var dup = m` copies the borrowed map); the second proved owned-`String` keys
with a borrowed-`Str` query (`m.get(k)`). No compiler change. The five existing
`Value` helpers that match exhaustively (`format`, `asInt`, `truthy`, `isStr`,
`valEq`) each gained a `.map` arm: a map renders as a fixed `<map>` placeholder
(its entry order is unspecified, so printing contents would not be
deterministic), is always truthy, coerces to 0, is not a string, and compares
unequal to everything. The e2e runs the built tool on a committed program
(`tests/e2e/interp_map_program.calc`) asserting string / int / bool values stored
and retrieved by key, the absent-key default (`get(m, "nope")` = 0), `has`
true / false, a computed key (`"k" + "ey"` then `get(m, "key")` = 99), a nested
map (`get(get(m, "inner"), "x")` = 42), and a frequency-count idiom
(`put(c, k, get(c, k) + 1)` twice gives `count a: 2`). The interpreter now has a
real compound, recursive, value-semantic data structure. Natural next steps: a
`keys(m)` builtin (needs an ordered list value first), or `split` returning a list
value as the second compound type.

**`keys(m)` and an ordered list value shipped, the second compound type.**
`examples/interp_keys_demo.vst` adds a fifth `Value` case,
`case list(Vec[Value])` (recursive like the map), so that `keys(m)` has somewhere
to return the map's keys. Three builtins land: `keys(m)` (the keys as a list of
string values, built by walking `m.keys()` and copying each into an owned-`String`
value), `at(xs, i)` (the i-th element, or `int(0)` out of range), and `len`
extended to also report a list's element count (`strLen` gained a `.list` arm
alongside `.str`). The five exhaustive `Value` matches each gained a `.list` arm
(a list renders as a `<list>` placeholder, is truthy, coerces to 0, is not a
string, compares unequal). Map key order is unspecified (the backing
`std::unordered_map` walks buckets), so `keys` is used by length and indexed
traversal rather than by printing the list, and the demo asserts only
order-independent aggregates. The idiom this unlocks is iterating a map: walk
`0` up to `len(keys(m))`, `at`-index each key, and `get` its value. No compiler
change. The e2e runs the built tool on a committed program
(`tests/e2e/interp_keys_program.calc`) that builds a 3-entry map and asserts the
key count (`len(keys(m))` = 3), a map-iteration value sum (`total: 60`), a
threshold count using boolean-to-int coercion in arithmetic
(`big = big + (get(m, at(ks, i)) > 15)` gives `big: 2`), the empty-map case
(`len(keys(mapNew()))` = 0), and an out-of-range index (`at(ks, 99)` = 0). The
interpreter now has two compound types (map and list) and can enumerate a map.
Natural next steps: `split(s, sep)` returning a list (a producer for the list
type from strings), or list builtins `push` / `concat` to build lists directly.

**`split(s, sep)` returning a list shipped.** `examples/interp_split_demo.vst`
adds the other natural producer of the list value (the first was `keys`): `split`
cuts a string value into the pieces between each occurrence of a separator and
returns them as a list of string values, which the existing `len` / `at` consume.
It lowers onto the synthesized `Str.split(sep) -> Vec[Str]`, copying each piece
into an owned-`String` value so the list owns its bytes rather than borrowing the
source. The piece count is always occurrences + 1, so adjacent / leading /
trailing separators yield empty pieces, and an empty separator yields the whole
string as one piece. Unlike a `keys` list, a `split` list preserves order, so
indexed access and a rebuild-by-iteration are deterministic. No compiler change.
The e2e runs the built tool on a committed program
(`tests/e2e/interp_split_program.calc`) asserting the piece count
(`len(split("a,b,c,d", ","))` = 4), indexed access (`at(parts, 0)` = `a`,
`at(parts, 3)` = `d`), a deterministic rebuild that concatenates the pieces back
(`joined: abcd`), a space split (`split("the quick brown fox", " ")` has 4
pieces, piece 1 is `quick`), the empty-middle-piece edge case
(`len(split("a,,b", ","))` = 3), and the no-separator case
(`at(split("nosep", ","), 0)` = `nosep`). The interpreter can now turn a string
into a list and walk it. Natural next steps: a `join(list, sep)` builtin (the
inverse of `split`), or a `toInt` builtin so split pieces can be summed
numerically.

**`join(list, sep)` shipped, the inverse of `split`.**
`examples/interp_join_demo.vst` adds the list type's first consumer back to a
string: `join(xs, sep)` renders each element of a list value (via `format`, so a
list of ints or bools joins as readily as a list of strings) into one string with
the separator placed only between elements, so an n-element list yields n-1
separators (an empty list joins to `""`, a singleton to just its element). It is
built directly with a `String` accumulator over the backing `Vec`, so the result
owns its bytes. With the prior slice, `join` and `split` round-trip:
`join(split(s, sep), sep) == s`. No compiler change. The e2e runs the built tool
on a committed program (`tests/e2e/interp_join_program.calc`) asserting a
comma round-trip (`join(parts, ",")` = `a,b,c,d`), other separators
(`-` gives `a-b-c-d`, `""` gives `abcd`), a multi-character separator
(`join(split("x y z", " "), " -> ")` = `x -> y -> z`), the round-trip identity as
a boolean (`join(split("a,b,c,d", ","), ",") == "a,b,c,d"` = true), the singleton
case (`join(split("solo", ","), ",")` = `solo`), and the empty-list case
(`join(keys(mapNew()), ",")` = `""`). The interpreter can now convert freely
between strings and lists in both directions. Natural next steps: a `toInt`
builtin so split pieces can be summed numerically, or list builtins
`push` / `concat` to build lists directly (not only via `keys` / `split`).

**`toInt(v)` shipped, closing the loop on numeric text.**
`examples/interp_toint_demo.vst` adds a builtin that parses a string value's
digits into an integer value, so `split` pieces (always strings) can finally be
summed and compared as numbers. It is deliberately distinct from the interpreter's
internal `asInt` arithmetic coercion (which treats any string as 0): `toInt`
actually parses via the synthesized `Str.toInt() -> Int?`, falling back to 0 when
the text is malformed; ints and bools pass through (a bool as 1 / 0), maps and
lists are 0. No compiler change. The motivating idiom now works end to end: split
a CSV of numbers, `toInt` each piece, accumulate. The e2e runs the built tool on a
committed program (`tests/e2e/interp_toint_program.calc`) asserting a CSV sum via
split + toInt (`split("10,20,30,40", ",")` summed = 100) and its average
(`total / len(nums)` = 25), a plain parse (`toInt("42")` = 42), a negative
(`toInt("-7")` = -7), the malformed fallback (`toInt("abc")` = 0), int passthrough
(`toInt(99)` = 99), a bool conversion (`toInt(true)` = 1), and a two-field numeric
sum (`toInt("100") + toInt("23")` = 123). The interpreter's string library now
spans construct, measure / slice, search, split / join, and numeric parse, with
two compound types (map and list). Natural next steps: list builtins
`push` / `concat` to build lists directly (not only via `keys` / `split`), or a
`toStr` builtin to round-trip numbers back to text explicitly.

**List builders shipped: `listNew` / `push` / `concat`.**
`examples/interp_listbuild_demo.vst` lets a program construct a list value
directly, element by element, where before a list could only come from `keys` or
`split`. `listNew()` is a fresh empty list, `push(xs, v)` returns a new list equal
to `xs` with `v` appended, and `concat(xs, ys)` returns a new list of the elements
of `xs` followed by those of `ys`. Like `put` for maps, they are value-semantic
and functional: `push` / `concat` copy the backing `Vec` (`var dup = xs`, the same
copy-a-borrowed-container move proved for the map), append, and return the new
list, so `xs = push(xs, v)` is the update idiom. With `len` / `at` / `join`, a
program can now accumulate a list in a loop and render it. No compiler change. The
e2e runs the built tool on a committed program
(`tests/e2e/interp_listbuild_program.calc`) asserting a string list built by
`push` (`join` = `a,b,c`, `len` = 3, `at(xs, 1)` = `b`), an int list
(`join(ys, "+")` = `1+2+3`), a `concat` that preserves order across mixed types
(`len` = 6, `join` = `a,b,c,1,2,3`), a list accumulated in a `while` loop
(`push(rs, i * i)` gives `squares: 1 4 9 16`), a `concat` of a `split` result with
an empty list (`x-y`), and the empty-list case (`len(listNew())` = 0). The list
type now has direct constructors to match the map's. Natural next steps: a `toStr`
builtin to round-trip numbers back to text explicitly, or a `set(m, ...)` /
`remove` pair to mutate maps and lists by index/key beyond rebuild-from-scratch.

**`toStr(v)` shipped, the symmetric counterpart of `toInt`.**
`examples/interp_tostr_demo.vst` adds a builtin that renders any value to a string
value by exposing the interpreter's internal `format` (an int or bool becomes its
text, a string is returned verbatim, a map or list as its placeholder). Its real
payoff is producing a string where one is required: a number can now be a *map
key* or a list element directly, which previously needed the string-led concat
hack (`"" + i`). `toStr` and `toInt` round-trip on integers
(`toInt(toStr(n)) == n`). No compiler change (a two-line wrapper over `format`).
The e2e runs the built tool on a committed program
(`tests/e2e/interp_tostr_program.calc`) that builds a map keyed by number strings
in a loop (`put(sq, toStr(i), i * i)`) and reads it back both by literal key and
by `toStr` (`get(sq, "3")` = 9, `get(sq, toStr(4))` = 16, `len(keys(sq))` = 4),
converts an int / bool / string (`toStr(42)` = 42, `toStr(true)` = true,
`toStr("hi")` = hi), builds a stringified list (`push(labels, toStr(i * i))` then
`join` = `1,4,9`), and round-trips (`toInt(toStr(123))` = 123). With numeric text
flowing both directions and two compound types with full build / read surfaces,
the interpreter's value library is broadly complete. Natural next steps: a
`set(xs, i, v)` / `remove(m, k)` pair to modify lists and maps by index / key, or
a `sort(xs)` builtin so a `keys` list can be rendered deterministically.

**`sort(xs)` shipped, closing the keys-order gap.** `examples/interp_sort_demo.vst`
adds a builtin that sorts a list value ascending, which finally makes a `keys`
list deterministic: every map slice noted that `keys(m)` returns an unspecified
order, so `join(sort(keys(m)), ",")` is the first way to print a map in a stable
order. `sort` is functional (copies the backing `Vec`, selection-sorts in place
via `Vec.get` / `Vec.set`, returns the new list) and its comparison is type-aware:
two ints compare numerically and two strings lexicographically (`Str <`), with any
other mix falling back to the integer coercion. Two small capabilities were probed
before building (both already present, no compiler change): `Str` supports `<`, and
`Vec` has `.set(i, v)`. The e2e runs the built tool on a committed program
(`tests/e2e/interp_sort_program.calc`) asserting deterministically sorted map keys
(`sort(keys(m))` joined = `apple,banana,cherry` whatever the map's internal order),
a numeric sort that proves the comparison is by value not lexicographic
(`sort([30, 10, 20, 5])` = `5 10 20 30`, not `10 20 30 5`), an alphabetic string
sort (`sort(split("delta,alpha,charlie,bravo", ","))` = `alpha bravo charlie
delta`), indexed access into the sorted result (`at(words, 0)` = `alpha`), and the
empty / singleton cases. A map can now be enumerated and printed in order. Natural
next steps: a `set(xs, i, v)` / `remove(m, k)` pair to modify lists and maps by
index / key (note `HashMap` has no `remove`, so map removal would rebuild by
iteration), or `min` / `max` / `sum` reductions over a list.

**List reductions shipped: `sum` / `min` / `max`.**
`examples/interp_reduce_demo.vst` adds the aggregate side of the list surface
(after build, read, sort): three builtins fold a list value down to a single
value. `sum(xs)` totals the elements, coercing each with the same parse as the
`toInt` builtin, so a list of numeric strings sums too (the motivating case,
`sum(split("1,2,3", ","))` = 6, with no manual loop); `min(xs)` and `max(xs)`
return the extreme element by the type-aware `lessVal` ordering that `sort`
introduced (ints numerically, strings lexicographically), returning `int(0)` for
an empty list. `min` / `max` reuse `lessVal` directly, so the reductions and the
sort stay consistent. No compiler change. The e2e runs the built tool on a
committed program (`tests/e2e/interp_reduce_program.calc`) asserting an integer
sum / min / max over a built list (`sum` = 110, `min` = 10, `max` = 50), the
average idiom (`sum(nums) / len(nums)` = 27), a CSV sum straight over `split`
pieces (`sum(split("1,2,3,4,5", ","))` = 15), lexicographic `min` / `max` on a
string list (`alpha` / `delta`), and the empty-list cases (`sum` = 0, `max` = 0).
The list type now has build, read, sort, and reduce. Natural next steps: a
`set(xs, i, v)` / `remove(m, k)` pair to modify lists and maps by index / key, or
a `map`-style transform (`mapList(xs, fn)`) applying a user function to each
element.

**`mapList(xs, fn)` shipped, the interpreter's first higher-order operation.**
`examples/interp_maplist_demo.vst` adds a transform that applies a function, named
by a string, to each element of a list value and collects the results into a new
list. The function is resolved through the very same `callFunc` an ordinary call
uses, so it works with a user-defined `fn` *and* with any builtin: `callFunc` and
the new `mapListInto` are mutually recursive (a builtin that calls back into user
code, one argument per element), which transpiled and compiled clean. This is the
first time the interpreter treats a function as data, passed by name. It composes
with the rest of the list surface. No compiler change. The e2e runs the built tool
on a committed program (`tests/e2e/interp_maplist_program.calc`) asserting a user
function mapped over a list (`mapList(nums, "double")` joined = `2,4,6,8`,
`mapList(nums, "square")` = `1 4 9 16`), the result folded by a reduction
(`sum(mapList(nums, "square"))` = 30), a *builtin* mapped over a list
(`mapList(words, "len")` over `split("a,bb,ccc", ",")` = `1,2,3`), a map-then-reduce
(`max(mapList(words, "len"))` = 3), and a map through `toStr` (`1-2-3-4`). The
interpreter is now genuinely higher-order over its list type. Natural next steps:
a `filterList(xs, fn)` companion (keep elements where the named predicate is true),
or a `set(xs, i, v)` / `remove(m, k)` pair to modify lists and maps in place.

**`filterList(xs, fn)` shipped, the higher-order companion to `mapList`.**
`examples/interp_filter_demo.vst` keeps each element of a list value for which a
named predicate returns a truthy value, collecting the survivors into a new list.
Like `mapList`, the predicate is resolved by name through `callFunc` (a user `fn`
or a builtin), and the keep decision uses the same `truthy` rule the interpreter
applies to `if` / `while`, so a predicate returning a boolean, a nonzero int, a
string, or a compound all behave consistently. With `mapList` and the reductions,
the interpreter now has the full map / filter / reduce pipeline. No compiler
change. The e2e runs the built tool on a committed program
(`tests/e2e/interp_filter_program.calc`) building `1..10` and asserting an
even-number filter (`filterList(nums, "isEven")` = `2,4,6,8,10`), a
greater-than-5 filter (`6,7,8,9,10`), filter-then-reduce
(`len(filterList(...))` = 5, `sum(filterList(...))` = 30), a full
filter-then-map-then-join pipeline (`mapList(filterList(nums, "isEven"),
"square")` = `4 16 36 64 100`), and an empty result (`filterList(nums, "huge")`
has length 0). The interpreter is now a small but complete list-processing
language. Natural next steps: a `reduceList(xs, fn, init)` to fold with a
two-argument user function (generalizing `sum`), or a `set(xs, i, v)` /
`remove(m, k)` pair to modify lists and maps in place.

**`reduceList(xs, fn, init)` shipped, completing map / filter / reduce.**
`examples/interp_fold_demo.vst` adds a left fold with a *two-argument* user
function, the general form of `sum` / `min` / `max`. Starting from `init`, it
threads an accumulator through the list, replacing it with `fn(acc, element)` at
each step. The combining function is resolved by name through `callFunc` like
`mapList` / `filterList`, but called with *two* arguments, so this is the first
builtin to drive a binary user function. The accumulator loop pushes `acc` into
the call args and then reassigns `acc` from the result (a move-then-reassign of a
local), which the checker accepts. No compiler change. With one fold a program
expresses many reductions; the e2e runs the built tool on a committed program
(`tests/e2e/interp_fold_program.calc`) expressing sum (`reduceList(nums, "add",
0)` = 14), product (`"mul", 1` = 60), max (`"maxOf", 0` = 5), a count that ignores
the element (`fn inc(a, x) = a + 1` = 5), a string-concatenation fold
(`reduceList(split("a,b,c", ","), "cat", "")` = `abc`), and the empty-list case
returning `init` (42). The interpreter now has the complete map / filter / reduce
trio over user code, plus build / read / sort on two compound types. Natural next
steps: a `set(xs, i, v)` / `remove(m, k)` pair to modify lists and maps in place,
or `range(n)` / `zip(xs, ys)` list producers to round out the comprehension shape.

**`range(n)` shipped, completing the comprehension shape.**
`examples/interp_range_demo.vst` adds the list producer for the integers
`[0, n)` (an empty list when `n <= 0`), the generator that pairs with the existing
higher-order trio: produce a range, then map / filter / reduce over it, instead of
writing a counter loop by hand. With it the interpreter is a small comprehension
engine. No compiler change (a `Vec`-building loop returning a list value). The
e2e runs the built tool on a committed program
(`tests/e2e/interp_range_program.calc`) feeding `range` into the whole pipeline:
the range itself (`join(range(5), ",")` = `0,1,2,3,4`), range then map
(`mapList(range(5), "square")` = `0,1,4,9,16`), range then filter
(`filterList(range(10), "isOdd")` = `1,3,5,7,9`), range then reduce
(`reduceList(range(100), "add", 0)` = 4950), range then map then sum
(`sum(mapList(range(5), "square"))` = 30), a map then filter then len
(`count odd` = 5), and the empty / negative-n cases (both length 0). The
interpreter can now express list comprehensions over a generated range without an
explicit loop. Natural next steps: a `set(xs, i, v)` / `remove(m, k)` pair to
modify lists and maps in place, or a `zipMap(keys, values)` producer that builds a
map from two parallel lists.

**`zipMap(keys, values)` shipped, the list-to-map bridge.**
`examples/interp_zipmap_demo.vst` builds a map from two parallel lists: the i-th
key element is bound to the i-th value element, pairing up to the shorter list. A
map key is always a string, so each key element is rendered with `format` (the
same coercion `toStr` uses), which lets a list of integer keys (e.g. from `range`)
become string keys. This is the bridge from the list type back into the map type:
with `range` / `mapList` it assembles a map out of computed lists without an index
loop. No compiler change. The e2e runs the built tool on a committed program
(`tests/e2e/interp_zipmap_program.calc`) zipping two parallel lists and looking up
a value by key (`zipMap(ids, vals)` then `get(m, "b")` = 2, `len(keys(m))` = 3,
`sort(keys(m))` = `a,b,c`), building a number-to-square table from `range` plus a
mapped `square` (`zipMap(range(4), mapList(range(4), "square"))` then `get(sq, "2")`
= 4, `get(sq, "3")` = 9, the integer keys stringified), and a length mismatch that
pairs to the shorter list (`zipMap(split("x,y,z", ","), range(2))` has 2 keys). The
interpreter can now flow data between its two compound types in both directions
(`keys` / `split` produce lists from a map / string, `zipMap` produces a map from
lists). Natural next steps: a `set(xs, i, v)` / `remove(m, k)` pair to modify lists
and maps in place, or `values(m)` / `entries(m)` to read a map's contents as lists.

**`values(m)` / `entries(m)` shipped, completing map iteration.**
`examples/interp_values_demo.vst` adds the two counterparts to `keys(m)` that round
out reading a map: `values(m)` returns the map's values as a list, and `entries(m)`
returns one `[key, value]` two-element list per binding. With no tuple type an
entry is just a two-element list, read with `at(entry, 0)` / `at(entry, 1)`, which
also exercises nested lists (a list of lists). Both mirror the underlying HashMap
surface (`keys` / `values` / `entries`); order is unspecified, so the demos assert
order-independent aggregates and `sort` the value list where a stable order is
wanted. No compiler change. The e2e runs the built tool on a committed program
(`tests/e2e/interp_values_program.calc`) aggregating over `values(m)`
(`sum(values(m))` = 6, `max(values(m))` = 3, `sort(values(m))` joined = `1,2,3`,
`len(values(m))` = 3) and summing values by walking `entries(m)` with a nested
`at(at(es, i), 1)` (= 6), plus the empty-map case (`len(values(mapNew()))` = 0). A
map's keys, values, and entries are all now readable as lists, so map and list
data flows freely in both directions. Natural next steps: a `set(xs, i, v)` /
`remove(m, k)` pair to modify lists and maps in place, or a `contains`-style
`hasValue(m, v)` / membership test over a list.

**`set(xs, i, v)` / `remove(m, k)` shipped, functional modification.**
`examples/interp_setremove_demo.vst` fills the gap between rebuild-from-scratch and
the read accessors: `set(xs, i, v)` returns a copy of a list with element `i`
replaced (out of range leaves it unchanged), and `remove(m, k)` returns a copy of a
map without key `k` (a missing key is a no-op). Both are functional, like every
other container op here, so the inputs are left intact. `set` copies the backing
`Vec` and overwrites one slot with `Vec.set`; `remove` rebuilds the map by iterating
its entries and copying every binding whose key differs from `k`, because the
backing `HashMap` has no in-place delete. The key comparison needed care: a
`String` key cannot be `==`-compared to the `Str` argument (the same `String` vs
`Str` operand-type rule seen before), so `remove` converts the target to an owned
`String` once and compares `String == String` (the comparison `valEq` already
uses). No compiler change. The e2e runs the built tool on a committed program
(`tests/e2e/interp_setremove_program.calc`) replacing a list element
(`set(xs, 1, 99)` = `10,99,30`) while the original is unchanged (`10,20,30`), an
out-of-range `set` no-op, removing a map key (`remove(m, "b")` leaves keys `a,c`)
while the original still has `b`, the removed map lacking `b`, and a missing-key
`remove` no-op (still 3 keys). Lists and maps can now be modified by index / key,
not only rebuilt. Natural next steps: a `hasValue(m, v)` or list membership test,
or numeric `abs` / `mod` builtins to round out the arithmetic surface.

**`abs` / `mod` shipped, rounding out the arithmetic, plus a FizzBuzz capstone.**
`examples/interp_arith_demo.vst` adds the two numeric builtins the operators
(`+ - * /`) left out: `abs(x)` is the absolute value, and `mod(a, b)` is the
remainder `a - (a / b) * b` (0 when `b` is 0). The mini-language has no `%`, so
`mod` is the one it most lacked; with it an even check is just `mod(x, 2) == 0`
instead of the `x - (x / 2) * 2` workaround the earlier slices used. Both coerce
their operands with `asInt`, like the operators. No compiler change. One honest
finding while writing the demo: the language has no *unary minus*, so a literal
`-7` does not parse as a negative (the `-` is consumed as a stray factor and the
`7` is left dangling); a negative is written as a subtraction, e.g. `abs(0 - 7)`.
The e2e runs the built tool on a committed program
(`tests/e2e/interp_arith_program.calc`) asserting `abs(0 - 7)` = 7, `abs(5)` = 5,
`mod(17, 5)` = 2, the divide-by-zero guard (`mod(10, 0)` = 0), a `mod`-based even
filter (`filterList(range(10), "isEven")` = `0,2,4,6,8`), and the headline: a
**FizzBuzz** over 1..15 written as a `fizzbuzz` function (nested `if` on `mod`,
falling back to `toStr(n)`) mapped over a list and joined
(`1 2 Fizz 4 Buzz Fizz 7 8 Fizz Buzz 11 Fizz 13 14 FizzBuzz`), which exercises
`mod`, nested `if`, `mapList`, `toStr`, `range`/list build, and `join` together.
The interpreter is now a genuinely usable small dynamic language. Natural next
steps: unary minus in the parser (so `-7` is a literal), or a `hasValue(m, v)` /
list membership test.

**Unary minus shipped, closing the negative-literal gap.**
`examples/interp_unaryminus_demo.vst` adds one rule to `pFactor`: a `-` in factor
position consumes the following factor and negates it (`Value.int(0 - asInt(f))`),
the gap the `abs` slice surfaced (`-7` previously left the `-` dangling and forced
`0 - 7`). Handling it at the factor level gives the right precedence for free:
unary minus binds tighter than `* / + -`, so `-3 * 4` is `(-3) * 4`, and because
`pFactor` recurses, `--5` is double negation. It composes everywhere a factor can
appear, with no conflict with binary minus (the binary case only fires once `pSum`
has already consumed a left operand). This is a parser rule in the interpreter, so
no Vestra compiler change. The e2e runs the built tool on a committed program
(`tests/e2e/interp_unaryminus_program.calc`) asserting a negative literal
(`-7`), a negative in an expression (`-7 + 10` = 3), precedence against `*`
(`-3 * 4` = -12), double negation (`--5` = 5), negating a parenthesized expression
(`-(2 + 3)` = -5), a negative literal as a builtin argument (`abs(-9)` = 9, which
the `abs` slice could not write), a negative bound to a variable (`x = -4`), binary
minus then unary minus (`10 - -2` = 12), a negating user function
(`fn neg(n) = -n`, `neg(8)` = -8), and a fold over negated range values
(`sum(mapList(range(4), "neg"))` = -6). Negatives are now first-class in the
language. Natural next steps: a `hasValue(m, v)` / list membership test, or a
`reverse(xs)` list builtin.

**`member(xs, v)` / `hasValue(m, v)` shipped, value membership tests.**
`examples/interp_member_demo.vst` adds the value-search the containers lacked: they
could be searched by key (`has(m, k)`) or string content (`contains`), but not for
a value. `member(xs, v)` walks a list value and `hasValue(m, v)` walks a map's
values, each comparing with the interpreter's structural `valEq` (the same equality
`==` uses), so the test is type-aware: an integer element matches an integer target
but never a string one. No compiler change. The e2e runs the built tool on a
committed program (`tests/e2e/interp_member_program.calc`) asserting list hits and
misses (`member(split("apple,banana,cherry", ","), "banana")` = true, `"grape"`
false, `member(nums, 20)` true, `99` false), the cross-type case
(`member(nums, "20")` = false, since the int `20` is not the string `"20"`), map
value membership (`hasValue(m, 2)` = true, `9` false), and an empty list (false).
The compound types can now be searched by key, by string content, and by value.
Natural next steps: a `reverse(xs)` list builtin, or `take(xs, n)` / `drop(xs, n)`
list slicing.

**`take(xs, n)` / `drop(xs, n)` shipped, list slicing.**
`examples/interp_takedrop_demo.vst` adds the sublist pair: `take(xs, n)` is the
first `n` elements and `drop(xs, n)` is everything after them. Both are functional
and clamp the count, so `take(xs, 0)` is empty, `take(xs, huge)` is the whole list,
and `drop` past the end is empty. They partition a list:
`concat(take(xs, n), drop(xs, n))` reconstructs `xs` for any `n`. Composing them
gives pagination, `take(drop(xs, k), n)` being the `n` elements starting at index
`k`, and with `range` / `mapList` a bounded prefix of a computed list. No compiler
change. The e2e runs the built tool on a committed program
(`tests/e2e/interp_takedrop_program.calc`) asserting a prefix
(`take(xs, 2)` = `a,b`) and suffix (`drop(xs, 2)` = `c,d,e`), the clamps
(`take(xs, 0)` empty, `take(xs, 99)` the whole list, `drop(xs, 99)` empty), the
partition identity (`concat(take(xs, 2), drop(xs, 2))` = `a,b,c,d,e`), a pagination
slice (`take(drop(range(10), 3), 3)` = `3,4,5`), and a bounded prefix of a computed
list (`take(mapList(range(100), "square"), 3)` = `0,1,4`). The list now supports
slicing as well as building, reading, transforming, and searching. Natural next
steps: a `reverse(xs)` list builtin, or a `find`-style `indexOf(xs, v)` returning
the position of a value.

**`reverse(xs)` / `indexOf(xs, v)` shipped, rounding out the list surface.**
`examples/interp_revidx_demo.vst` adds two list operations: `reverse(xs)` returns
the elements in reverse order (built by walking the backing `Vec` from the end, an
involution so `reverse(reverse(xs))` is `xs`), and `indexOf(xs, v)` returns the
index of the first element equal to `v` or -1. `indexOf` is the position
counterpart to the boolean `member` and uses the same structural `valEq`, so it is
type-aware. No compiler change. The e2e runs the built tool on a committed program
(`tests/e2e/interp_revidx_program.calc`) asserting a reversed list
(`reverse(split("a,b,c,d", ","))` = `d,c,b,a`), the double-reverse identity
(`a,b,c,d`), `indexOf` hits and a miss (`indexOf(xs, "c")` = 2, `"a"` = 0,
`"z"` = -1), reversing an int list and a `range` (`30,20,10` and `4,3,2,1,0`),
`indexOf(nums, 20)` = 1, and the empty-list case. The list surface is now broad:
build (`listNew` / `push` / `concat` / `range` / `split` / `keys`), read (`len` /
`at` / `member` / `indexOf`), transform (`mapList` / `filterList` / `reduceList` /
`sort` / `reverse` / `take` / `drop`), and aggregate (`sum` / `min` / `max` /
`join`). Natural next steps: a `zip(xs, ys)` producing a list of `[x, y]` pairs, or
moving the interpreter capstone into a single committed `interp_*` standalone tool
that wires the whole builtin set.

**`zip(xs, ys)` shipped, pairing two lists.**
`examples/interp_zip_demo.vst` pairs two list values element-wise into a list of
two-element lists (no tuple type, so a pair is a `[x, y]` list read with
`at(pair, 0)` / `at(pair, 1)`), pairing up to the shorter input. It is the list
counterpart of `zipMap`: `zip` keeps the pairs as an ordered list of any-typed
values, where `zipMap` keys a map by the stringified first list. No compiler change.
The e2e runs the built tool on a committed program
(`tests/e2e/interp_zip_program.calc`) zipping parallel name / age lists
(`len(zip(names, ages))` = 3, `at(at(pairs, 0), 0)` = `alice`, `at(at(pairs, 0), 1)`
= 30), walking the pairs to build `name=age` strings
(`join` = `alice=30,bob=25,carol=35`), zipping a `range` for indices
(`at(at(zip(range(3), names), 1), 0)` = 1 paired with `bob`), and a length mismatch
that pairs to the shorter (`len(zip(names, range(2)))` = 2). The two compound types
now have matching producers from parallel lists (`zip` to a list of pairs, `zipMap`
to a map). Natural next steps: consolidating the whole builtin set into one
committed standalone `interp` tool (a capstone), or a `groupBy(xs, fn)` that buckets
list elements into a map.

**`groupBy(xs, fn)` shipped, the interpreter's richest builtin.**
`examples/interp_groupby_demo.vst` buckets a list value into a map by a key
function: for each element it computes the key by calling `fn` (a user `fn` or a
builtin) through `callFunc`, stringifies it with `format`, and appends the element
to that key's bucket list (built functionally with `pushVal`). It is the first
builtin to combine all three machineries at once: higher-order calls, the map type
(one bucket per key), and the list type (each bucket a list). The accumulation reads
the bucket with `m.get(key)` (borrow) and writes it back with `m.set(key, updated)`
(move) in the same iteration, which the checker accepts. Buckets preserve input
order, so the demo is deterministic. No compiler change. The e2e runs the built tool
on a committed program (`tests/e2e/interp_groupby_program.calc`) partitioning
`range(10)` by parity (`get(g, "even")` = `0,2,4,6,8`, `"odd"` = `1,3,5,7,9`,
`len(get(g, "even"))` = 5), grouping words by first letter
(`groupBy(words, "firstChar")` gives `a` = `apple,avocado`, `b` =
`banana,blueberry`, `len(keys(...))` = 3), and folding a bucket
(`sum(get(g, "even"))` = 20). The interpreter now expresses real aggregation
pipelines (generate, group, reduce). Natural next steps: consolidating the whole
builtin set into one committed standalone `interp` tool (a capstone with a
runnable program file), or a `update(m, k, fn)` that maps a function over one map
value in place.

**`update(m, k, fn)` shipped, derive-a-map-value from the old.**
`examples/interp_update_demo.vst` applies a one-argument function to a single map
value: it reads the current value at key `k` (using `int(0)` when the key is
absent), calls `fn` on it through `callFunc`, and binds the result back, returning a
new map. It is the derive-from-old counterpart to `put` (which overwrites with a
given value). Because a missing key reads as `int(0)`, the canonical word-frequency
count collapses to `update(counts, word, "inc")` with `fn inc(n) = n + 1`, no
get-or-default dance. No compiler change. The e2e runs the built tool on a committed
program (`tests/e2e/interp_update_program.calc`) counting words by folding `update`
over a split list (`the` = 3, `cat` = 2, `dog` = 1, `len(keys)` = 3), deriving a new
value with another function (`update(counts, "the", "double")` then `get` = 6), the
absent-key case (`update(counts, "new", "inc")` = 1, since `inc(0)`), and a total
over the values (`sum(values(counts))` = 10). The map now supports both overwrite
(`put`) and transform (`update`) of a value. Natural next steps: consolidating the
whole builtin set into one committed standalone `interp` tool with a real program
(a capstone), or a `merge(m1, m2)` that combines two maps.

**`merge(m1, m2)` shipped, combining two maps.**
`examples/interp_merge_demo.vst` overlays two maps: the result has every binding of
`m1` with `m2`'s bindings set on top, so `m2` wins on a shared key. It is functional
(a copy of `m1` with `m2`'s entries `set` over it via an `entries()` walk), so both
inputs are untouched. This completes the map's combiners alongside `put` (one key)
and `update` (transform one key): `merge` folds in a whole second map at once, the
shape config-overlay and record-update need. No compiler change. The e2e runs the
built tool on a committed program (`tests/e2e/interp_merge_program.calc`) merging
`{a:1, b:2}` with `{b:20, c:30}` and asserting the unshared keys (`a` = 1, `c` = 30),
the override (`b` = 20), the merged key count (3) and sorted keys (`a,b,c`), that the
first input is untouched (`get(m1, "b")` = 2), a value total (`sum(values(m))` = 51),
and an empty merge preserving the original (2 keys). The map is now fully featured:
build, read, modify (`put` / `update` / `remove`), iterate (`keys` / `values` /
`entries`), and combine (`merge`). Natural next steps: consolidating the whole
builtin set into one committed standalone `interp` tool with a real program (a
capstone), or a `count(xs, fn)` that tallies how many list elements satisfy a
predicate.

**`any(xs, fn)` / `all(xs, fn)` shipped, boolean quantifiers.**
`examples/interp_anyall_demo.vst` adds the two higher-order quantifiers: `any` is
true when the predicate holds for at least one element, `all` when it holds for
every one. The predicate is resolved by name through `callFunc` (a user `fn` or a
builtin) and judged by the same `truthy` rule as `if` / `while`; a shared `quantify`
loop short-circuits (`any` stops at the first true, `all` at the first false) and
returns the conventional empty-list values (`any` of nothing false, `all` of nothing
true). They are the boolean summaries of `filterList`, distinct from counting or
collecting. No compiler change. The e2e runs the built tool on a committed program
(`tests/e2e/interp_anyall_program.calc`) over an all-even list (`all(nums, "isEven")`
= true, `any(nums, "isBig")` = false), a mixed list (`all(mixed, "isEven")` = false,
`any(mixed, "isEven")` = true, `all(mixed, "isPos")` = true), the empty-list vacuous
cases (`any` = false, `all` = true), and `all(range(5), "isPos")` = false (0 fails).
The list now answers existential and universal questions, not just collects. Natural
next steps: a `count(xs, fn)` tally, or consolidating the whole builtin set into a
standalone `interp` capstone tool with a real program.

**`replace(s, old, new)` shipped, substring replacement.**
`examples/interp_replace_demo.vst` adds the string transform the surface was
missing, and it falls out of the split / join pair already in place: replacing every
`old` with `new` is splitting on `old` and joining the pieces with `new`. It is
implemented directly over `Str.split(old)` (append each piece, with `new` between
pieces), which gives the edge behavior for free: adjacent matches leave empty pieces
(`replace("aaa", "a", "x")` is `xxx`), and an empty `old` matches nothing (the whole
string is one piece, returned unchanged). No compiler change. The e2e runs the built
tool on a committed program (`tests/e2e/interp_replace_program.calc`) covering a
delimiter swap (`replace("a-b-c", "-", "/")` = `a/b/c`), a single-character replace
(`heLLo`), removal with an empty replacement (`abc`), a multi-character word replace
(`replace("the cat sat", "cat", "dog")` = `the dog sat`), a no-match no-op, adjacent
matches (`xxx`), and a two-step round-trip (`a.b.c`). The string surface now spans
build (concat), measure / slice (`len` / `substr`), search (`find` / `contains`),
split / join, parse / render (`toInt` / `toStr`), and rewrite (`replace`). Natural
next steps: a `count(xs, fn)` tally, or consolidating the whole builtin set into a
standalone `interp` capstone tool with a real program.

**Capstone shipped: the complete interpreter running a real word-frequency report.**
After the long builtin arc, this slice proves the whole language solves a genuine
task rather than exercising one feature. It adds no new interpreter code: the e2e
reuses the latest complete interpreter input (`examples/interp_replace_demo.vst`,
all ~33 builtins) and runs it on a committed report program
(`tests/e2e/interp_report_program.calc`) that tokenizes a text with `split`, counts
word frequencies with `update` over a fold, and renders a report with `keys` /
`sort` / `get` / `len` / `max` / `values` / `sum` / `mapList` / `reduceList` / `join`
/ `replace` / concat. Two honest language limits surfaced while writing it, both
recorded as notes (not compiler changes): user functions are not closures (a `fn`
sees only its parameters and the global function table, not the surrounding
variables, so a predicate cannot reference an outer `counts` map), and the
comparison operators are only `<` / `>` / `==` (no `>=` / `<=` / `!=`, so a
length-tiebreak `longerOf` uses `>`). The e2e asserts the analytics: total words
(10), distinct words (6), total characters via `sum(mapList(words, "len"))` (37), the
longest word via a length-reducing fold (`quick`), the most frequent count
(`max(values)` = 3), an alphabetical `word: count` listing, and a `replace`-normalized
sorted key list (`brown_dog_fox_lazy_quick_the`). The interpreter is a small but
genuinely capable dynamic language. Natural next steps: a `count(xs, fn)` tally, a
`zipMap`-based CSV-to-records demo, or `>=` / `<=` / `!=` comparison operators in the
parser (closing the gap this capstone found).

**`<=` / `>=` / `!=` comparison operators shipped, closing the capstone gap.**
`examples/interp_cmpops_demo.vst` adds the three comparison operators the
mini-language lacked (it had only `<` / `>` / `==`, so the capstone's length-tiebreak
had to use `>`). Three new tokens (`le` / `ge` / `ne`) are lexed before the
single-character fallback, mirroring how `==` is already special-cased; `pCmp` gains
`<=` / `>=` arms (comparing with `asInt` like `<` / `>`), and `pEq` gains a `!=` arm
(`!valEq(a, b)`, the negation of the `==` rule, so it inherits the cross-type
behavior). A lexer + parser change in the interpreter, no Vestra compiler change. The
e2e runs the built tool on a committed program
(`tests/e2e/interp_cmpops_program.calc`) checking each operator including the equal
boundary (`5 <= 5` = true, `4 >= 4` = true), a cross-type inequality
(`1 != "1"` = true), the operators inside user predicates
(`filterList(range(8), "atLeast5")` with `fn atLeast5(x) = x >= 5` = `5,6,7`, and a
`longest` fold that can now use `>=`), and that the existing `<` / `==` still work.
The mini-language's relational surface is now complete. Natural next steps: a
`count(xs, fn)` tally, logical `and` / `or` operators (today only `if` chains
combine conditions), or a `zipMap`-based CSV-to-records demo.

**Logical `and` / `or` with short-circuit shipped.**
`examples/interp_logic_demo.vst` adds the two logical connectives, which until now
could be expressed only by nesting `if`s. They are keywords (added to `wordKind`) and
introduce two new precedence levels below the comparisons: the grammar gains `pOr`
(lowest) over `pAnd` over `pEq`, and `pExpr` enters at `pOr` (for both the `if`
condition and a bare expression), so `a < b and c < d` parses as
`(a < b) and (c < d)` and `and` binds tighter than `or`. Crucially they
*short-circuit*, reusing the `live` flag: the right operand is always parsed (the
token cursor must advance) but only evaluated when the left does not already decide
the result, so `true or boom()` never calls `boom`, the behavior a guard recursion
needs. No Vestra compiler change. The e2e runs the built tool on a committed program
(`tests/e2e/interp_logic_program.calc`) checking the truth tables (`and tt` true,
`and tf` false, `or ft` true, `or ff` false), precedence
(`5 < 2 or 1 < 2 and 3 < 4` = true), the short-circuit with an infinitely-recursive
`fn boom(n) = boom(n)` on the skipped side (`1 < 2 or boom(1)` = true and
`5 < 2 and boom(1)` = false, both terminating), and a compound user predicate
(`both(x) = isEven(x) and small(x)` filtering `range(10)` to `0,2,4`). The
mini-language can now combine conditions directly. Natural next steps: a logical
`not` (unary), a `count(xs, fn)` tally, or a `zipMap`-based CSV-to-records demo.

**Unary logical `not` shipped, completing the boolean trio.**
`examples/interp_not_demo.vst` adds `not`, the negation that joins `and` / `or`. It
is a keyword at the conventional precedence (Python's `or` < `and` < `not` <
comparisons): a new `pNot` level sits between `pAnd` and `pEq`, so `not a == b` is
`not (a == b)` while `not a and b` is `(not a) and b`, and `pNot` recurses so
`not not x` double-negates. The result is `Value.boolean(!truthy(operand))`. No
Vestra compiler change. The e2e runs the built tool on a committed program
(`tests/e2e/interp_not_program.calc`) checking negation (`not (1 < 2)` = false,
`not (5 < 2)` = true), double negation (true), precedence with `and` / `or`
(`not 5 < 2 and 1 < 2` = true, `not 1 < 2 or 3 < 4` = true) and with `==`
(`not 3 == 4` = true), composition with a builtin (`not contains("hello", "z")`
= true), `not` on a bare boolean (`not true` = false), and a `not`-based user
predicate (`fn odd(x) = not isEven(x)` filtering `range(8)` to `1,3,5,7`). The
mini-language's boolean logic is now complete (`and` / `or` / `not` plus the six
comparisons). Natural next steps: a `count(xs, fn)` tally, a `zipMap`-based
CSV-to-records demo, or string case builtins (`upper` / `lower`).

**`chars(s)` shipped, exploding a string to characters.**
`examples/interp_chars_demo.vst` adds the bridge from the string type into the list
type at character granularity, which was previously unreachable (`split` cuts on a
separator; there was no way to get the individual characters). `chars(s)` returns a
list with one one-character string per byte, built over `Str.slice(i, i + 1)` (each
character copied into an owned `String`). It joins the string and list surfaces: a
string becomes a list, the whole higher-order machinery applies per character, and
`join(..., "")` rebuilds a string, so `join(chars(s), "")` is `s` and
`join(reverse(chars(s)), "")` reverses a string. Byte-oriented like the rest of the
string surface. No compiler change. The e2e runs the built tool on a committed
program (`tests/e2e/interp_chars_program.calc`) listing the characters of `"hello"`
(`h,e,l,l,o`, length 5), reversing a string via chars + reverse + join (`cba`),
rebuilding via `join(chars("test"), "")` (`test`), and filtering / counting vowels
with a user predicate built from the new `or` (`filterList(chars("education"),
"isVowel")` = `euaio`, count 5; the `a`s in `banana` = 3), plus the empty-string
case. Characters are now first-class list elements. Natural next steps: string case
builtins (`upper` / `lower`), a `count(xs, fn)` tally, or a `zipMap`-based
CSV-to-records demo.

**Second capstone shipped: a CSV-to-records pipeline.**
After the map/list arc, a real structured-data program rather than another feature,
exercising both compound types together. It adds no new interpreter code: the e2e
reuses the complete interpreter (`examples/interp_chars_demo.vst`) and runs a
committed program (`tests/e2e/interp_csv_program.calc`) that parses a small CSV (rows
separated by `;`, fields by `,`, since a `.calc` string literal is one line), zips the
header over each row into a record map with `zipMap`, collects the records into a
list, then queries columns (`get(at(records, 0), "age")` = 30), extracts and sorts a
column (`names` = `alice,bob,carol`), and aggregates numerically (`toInt` each age,
summed = 90, averaged = 30). It was checked against the `upper` / `lower` idea first,
which was dropped: the `Str` surface has no case-conversion method (`toUpper` /
`uppercased` both absent) and only `byteAt -> UInt8?`, so case folding has no clean
primitive yet. The e2e asserts a field query, the extracted sorted column, and the
numeric total. The interpreter handles records (maps) inside a list, the shape real
tabular data needs. Natural next steps: string case builtins via `byteAt` arithmetic
(if a byte-to-string primitive lands), a `count(xs, fn)` tally, or a `sortBy(xs, fn)`
that sorts a list by a key function.

**`sortBy(xs, fn)` shipped, sort by a key function.**
`examples/interp_sortby_demo.vst` generalizes `sort`: where `sort` orders by the
elements themselves, `sortBy` orders by `fn(element)`, the higher-order version. The
key function is resolved by name through `callFunc` (a user `fn` or a builtin) and the
derived keys are compared with the same type-aware `lessVal` as `sort`; it is a
functional selection sort over a copy (recomputing the key per comparison, fine at
this scale). This unlocks the orderings `sort` cannot express: ascending via an
identity key, descending by sorting on a negated key, by string length, or a record
list by one field. No compiler change. The e2e runs the built tool on a committed
program (`tests/e2e/interp_sortby_program.calc`) sorting numbers ascending
(`sortBy(nums, "id")` = `1,2,3,5`), descending via a negated key
(`sortBy(nums, "negate")` = `5,3,2,1`), words by length
(`sortBy(words, "wlen")` = `fig,kiwi,cherry,banana`), and words alphabetically
(`banana,cherry,fig,kiwi`). The list now sorts by any computed key. Natural next
steps: a `count(xs, fn)` tally, string case builtins (if a byte-to-string primitive
lands), or `min`/`max`-by-key variants.

**`minBy(xs, fn)` / `maxBy(xs, fn)` shipped, the by-key extremes.**
`examples/interp_extremeby_demo.vst` adds the reduce-to-extreme counterparts of
`sortBy` (and the by-key generalization of `min` / `max`): they return the element
whose `fn(element)` key is least / greatest. The key function is resolved through
`callFunc` and keys compared with the same `lessVal`; they return the *element* (not
the key) and `int(0)` for an empty list, scanning once and keeping the best element
and its key together (a double conditional-move the checker accepts). This is the
clean way to ask "the longest word" (`maxBy(words, "wlen")`), which the first capstone
had to write as a hand-rolled fold. No compiler change. The e2e runs the built tool
on a committed program (`tests/e2e/interp_extremeby_program.calc`) finding the longest
and shortest word by length (`banana` / `fig`), the max and min number by identity (50
/ 10), the max via a negated key (`minBy(nums, "negate")` = 50), and the empty-list
case. The list's higher-order surface is now broad: transform (`mapList`), filter
(`filterList`), fold (`reduceList`), quantify (`any` / `all`), sort (`sort` /
`sortBy`), and extremes (`min` / `max` / `minBy` / `maxBy`).

**`findFirst(xs, fn)` shipped, the linear-search reducer.**
`examples/interp_findfirst_demo.vst` adds the predicate search: it walks the list and
returns the first element for which `fn(element)` is truthy, or `int(0)` when nothing
matches (and for the empty list). The predicate is resolved through `callFunc` like the
other higher-order builtins, and the scan returns the element itself the moment the
predicate holds rather than folding to the end. This is the clean way to ask "the first
big number" without a hand-rolled loop, and it composes with the generators: it can
search a `range`, a `chars` list, or any built list. No compiler change. The e2e runs
the built tool on a committed program (`tests/e2e/interp_findfirst_program.calc`)
finding the first big number (`findFirst(nums, "isBig")` = 30), the no-match case (0),
the first value over two in a range (`findFirst(range(10), "gt2")` = 3), the first vowel
in a char list (`findFirst(chars("xyzebra"), "isVowel")` = e), and the empty-list case.
The list's higher-order surface now covers transform, filter, fold, quantify, sort,
extremes, and search.

**`count(xs, fn)` shipped, the predicate tally.**
`examples/interp_count_demo.vst` adds the counting counterpart of `findFirst`: where
`findFirst` returns the first matching element and stops, `count` scans the whole list
and returns how many elements make the predicate truthy (`int(0)` for the empty list).
The predicate is resolved through `callFunc` and judged by `truthy`, the same machinery
the other higher-order builtins share. With `any` / `all` (some / every), `findFirst`
(the first), and now `count` (how many), the predicate-query surface is complete: a
program can ask whether, which, and how many in the same vocabulary. No compiler change.
The e2e runs the built tool on a committed program (`tests/e2e/interp_count_program.calc`)
counting the big numbers (`count(nums, "isBig")` = 2), the vowels in a char list
(`count(chars("xyzebra"), "isVowel")` = 2), the even values over a range
(`count(range(10), "even")` = 5), and the empty-list case.

**`product(xs)` shipped, the multiplicative fold.**
`examples/interp_product_demo.vst` adds the companion of `sum`: where `sum` adds the
elements starting from 0, `product` multiplies them starting from 1. Each element is
coerced with `toIntVal` the same way `sum` coerces, and the empty list returns the
multiplicative identity 1 (mirroring sum's additive 0), which keeps it composable. The
list now has both numeric folds, enough to express a factorial as the product of a
built 1..n list or a running total as a sum. No compiler change. The e2e runs the built
tool on a committed program (`tests/e2e/interp_product_program.calc`) summing and
multiplying a small list (`sum` = 9, `product` = 24), computing 5! as the product of a
1..5 list (`factorial5` = 120), and checking the empty-list identity (`empty` = 1).
Natural next steps: an `indexOf`-style position search, an `avg(xs)` mean over `sum` and
`len`, or string case builtins if a byte-to-string primitive lands.

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
entries. A fifth slice shipped: **explicit type-args at a construction site**
(`Pair[Int32](lo: 1, hi: 2)`). It parses as a call of an index expression
(`Pair[Int32]`); when the base names a generic struct/enum, `check_call`
reinterprets the bracket as explicit type arguments (`type_from_index_expr`
converts each bare type name), builds the nominal instance, and funnels into
the ordinary construction path with that instance as the expected type — which
seeds the generic bindings, so a contradicting value argument conflicts at
unification. Arity mismatches and type-args on a non-generic type are
diagnosed (not silently read as a subscript). Codegen emits the named
specialization `Pair<std::int32_t>{...}`. Bare type-name arguments today;
nested generics still defer to inference. Remaining pieces: a const generic as
a runtime/comptime *value* in a body
(`for i in 0..N`; today N is only an array length); const-expression
array lengths (`[N + 1]T`); bound enforcement gating body operations;
transitive bound checking; user-defined-protocol conformance (no general
table yet); and `requires` clauses on generic structs/enums (functions
only today). (Leading-dot payloaded-enum construction, const generics on
functions, and derive emission for generic types were closed as
follow-ons.) The type-parameter surface is now substantially complete —
the remaining items are minor refinements (const generics as body values,
const-expression array lengths, deeper bound enforcement, a conformance
table). With explicit construction-site type-args now shipped, generics
phase 2 is effectively done and the next phase is best drawn from the bigger
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

Fifteenth slice shipped — **multi-argument attributes**: an attribute can now
carry more than one argument (`@mapped(0x80, 16)`), and a macro reads the i-th
with a two-arg `d.attribute("name", i)` (0 = the first, ≥1 = the new
`Attribute::extra_args`). Additive — `predicate` stays the first argument so
every single-arg consumer is untouched; the parser gained a comma loop after
the first arg, `ast::clone` reproduces the extras, and the folder's
attribute-reflection path dispatches on the call arity. Proven end to end
(`quote_demo` `@mapped(addr, width)` bakes both into generated `_base()` /
`_width()` accessors) plus a codegen unit test.

Remaining (full reflection): `.asType()` on a type-valued argument
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

First slice shipped — **`vestra audit --safety`**: a third audit enumerator
(beside `--sysreg` / `--no-libc`) that walks every site granting an *unsafe*
capability — `RawMemory` / `Asm` / `Mmio`, the low-level rows whose discharges
bypass the safe model — through a function's `using` row or a `with` block, and
reports whether a `// Safety:` comment justifies it: one line per site,
`<file>:<line>:<col>: unsafe <Cap> [safety:{yes|no}]`. The justification scan
walks the source lines above the site, skipping blanks, `@attributes`, and
other comments, matching a `// Safety:` line. A zero-`safety:no` pass is the
audit trail the spec calls for — every unsafe grant documented where it is
taken. It's an enumerator, not a build gate, so existing low-level code keeps
building; proven over `examples/safety_demo.vst` (a justified `using
RawMemory, Mmio`, an undocumented `using Asm`, and a mix of `with` blocks).

Remaining: row polymorphism over a generic capability variable; value-level
narrowing (`Mmio.narrowed(to:)`, which needs capabilities-as-region-values, a
model the name-set checker doesn't have yet); and a build-time `// Safety:`
*requirement* (vs. today's audit-time enumeration), which would want real
comment-trivia retention in the lexer rather than the audit's source-line scan.

### 7. SIMD `[N]T` lowering (§13)

First slice shipped — **elementwise arithmetic on `[N]T`**: `+ - * /` apply
lanewise to two same-length, numeric-element fixed vectors and yield the same
`[N]T`. Sema's `check_binary` adds a vector case (matching length via the
structural `Vector` equality, numeric element required, only the four
elementwise ops — `%`/bitwise/comparison are rejected); codegen lowers each to
an `__vstr::vec_add/sub/mul/div` prelude template — a small fixed-extent loop
over the `std::array<T, N>` that the C++ compiler auto-vectorizes to real SIMD
where it pays, with a clean scalar fallback and no target-detection layer.
Compositions chain (`a * x + y` → `vec_add(vec_mul(a, x), y)`). Proven end to
end (`examples/simd_demo.vst`: axpy + a lane-sum dot product) plus a
codegen unit test.

Remaining: scalar broadcast (`v * 2`); reductions / swizzles / lane shuffles;
comparison-to-mask; and the spec's target-feature-gated typed vector lanes
(`Vec[N, T]` in a `core.systems.simd` namespace, ANNEX A §14.10) backed by
`std::experimental::simd` or NEON/SSE/AVX/RVV intrinsics — which is where the
`--target` / `--target-features` plumbing (`312281c`) becomes the
target-detection layer. The current helper bodies can be swapped for those
behind the same `vec_*` surface.

### 8. Content-hashed `@embed` manifest — shipped

`@embed("path")` reads files relative to the source dir at fold time; a
content-hashed manifest now pins exactly which bytes a build embeds.
`vestra build --embed-manifest FILE` enforces it: every `@embed`ed file's
FNV-1a-64 content hash + byte size must match a manifest entry for that
path, or the build fails — a drifted file or an unmanifested embed is a hard
error (so two builds of the same source under the same manifest are
byte-identical). `--emit-embed-manifest FILE` writes that manifest from the
files a clean build embedded (sorted by path, byte-stable). Both live in the
manifest-aware `make_embed_reader` (the documented integration point), keyed
by the path as written; the manifest is emitted only after an error-free
build so a failed build never leaves a stale one. The two flags are mutually
exclusive. Proven end to end (`examples/embed_demo/`: an emit→enforce
round-trip agrees, and a committed wrong-hash `drift.manifest` makes enforce
fail — the `e2e_embed_manifest_drift` WILL_FAIL test). The hash is a fast
non-cryptographic content hash (reproducibility / drift detection, not tamper
resistance).

### 9. `Soa[T]` struct-of-arrays library type (§13)

First slice shipped — **`Soa[T]` over a struct**: a struct-of-arrays container
that inverts AoS storage for SIMD-/cache-friendly per-field traversal. Built as
a compiler-known type (the Vec/HashMap idiom), `Soa[T]` lowers to a
`std::tuple<std::vector<F0>, std::vector<F1>, …>` — one growable column per
field of the element struct. `Soa.new()` mints an empty one (Alloc-gated, T
inferred from the expected type); `push(v)` scatters v's fields into the columns
(an IIFE binds receiver + value once, then `push_back` per column), `len() ->
Int` is column 0's size, and `get(i) -> T` gathers the i-th row back into a
struct (an IIFE + designated-init). T must be a non-generic struct with at least
one stored field (diagnosed otherwise). The tuple backing is shared between
`emit_sema_type` (struct via the sema type's nominal_decl) and `emit_type`
(struct via a unit-local name→struct index). Proven end to end
(`examples/soa_demo.vst`: column fold + row gather, compiled at -O2 and run)
plus a codegen unit test.

Second slice shipped — **per-field column views** (`s.column(.x)`): the
SIMD-traversal payoff. A leading-dot field selector resolved against T's fields
yields a `MutSpan[FieldType]` over that field's contiguous column — lowered to
`std::span{std::get<i>(s)}`. `column` is intercepted in `check_call` (the
selector is a field name, not a value, so the result type is the field's type
statically) and in the emitter's Soa method block. To make a column directly
traversable, `for x in xs` now also iterates a `Span[T]`/`MutSpan[T]` (the same
range-based-for lowering as Vec), so `for v in s.column(.y) { … }` walks one
field's memory. Proven end to end (`soa_demo.sumY` folds a column view) plus a
codegen unit test.

Remaining: the spec's `@layout(soa)` attribute form (vs. today's explicit
`Soa[T]` type); `get(i) -> T?` bounds-checking; Soa over a generic struct
instance; and elementwise ops directly over a column view (pairs with §7).

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
