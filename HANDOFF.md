# Handoff

Session-continuity notes for the Vestra project. The language spec lives in
[`VESTRA_DRAFT.md`](VESTRA_DRAFT.md); the implementation strategy and high-level
"what works" are in [`README.md`](README.md); the forward-looking phase menu is
in [`ROADMAP.md`](ROADMAP.md). This file is the *back-looking* counterpart for
the **active arc**: what shipped, how the work proceeds, and the rough edges to
watch for.

> The detailed, per-slice engineering log for the active arc lives in the
> auto-memory file `project_dogfood_compiler.md` (loaded each session). This
> handoff is the human-readable orientation; that memory file is the granular
> record.

## The active arc: the dog-food compiler

Vestra has a complete **C++26 bootstrap compiler** (the `vestra` binary, written
in C++). The current arc is **self-hosting**: re-implementing the Vestra compiler
*in Vestra*, under [`selfhost/`](selfhost/), and proving each phase correct by
**differential testing** against the bootstrap compiler, same input giving
byte-for-byte identical output.

The four phases each live in one `.vst` file and build to their own binary:

| Phase | Source | Binary | Differential (vs the `vestra` bootstrap) |
|---|---|---|---|
| Lexer  | `selfhost/lexer.vst`   | `vlex`   | `vestra build --dump-tokens` token stream |
| Parser | `selfhost/parser.vst`  | `vparse` | `vestra build --dump-ast` canonical AST print |
| Sema   | `selfhost/sema.vst`    | `vsema`  | `vestra check` diagnostic lines (grep-extracted) |
| Codegen| `selfhost/codegen.vst` | `vgen`   | `vestra build` generated `.cpp` + `.hpp` tail |

Each `.vst` is itself transpiled to C++ by the bootstrap compiler, compiled with
`c++ -std=c++26`, then run over a corpus; its output must match the bootstrap's
byte-for-byte. `parser.vst` / `sema.vst` / `codegen.vst` all carry the **same copy
of the lexer + parser front-end** at the top of the file (a later phase is the
earlier phase plus more), so a parser change is mirrored into the downstream files.

## Current state

- **1504 / 1504 tests pass** (`ctest --test-dir build/debug`), Debug build,
  format-check clean. Of these, **214 are `e2e_selfhost`** (4 lexer, 11 parser,
  74 sema, 121 codegen, 4 end-to-end demo).
- Tip commit: `8ede5d4` (codegen slice 62, leading-dot cases — corpus
  burndown 8). Slice 31's probe also produced `62b747b`, a **bootstrap emitter
  fix the differential found** (see below). 104 commits carry the `selfhost:`
  prefix.
- Phase status:
  - **Lexer: done.** Token stream byte-identical on the sample corpus and on
    the self-hosted lexer source itself.
  - **Parser: done.** 142/143 example files dump byte-identical via `--dump-ast`
    (the 1 holdout needs declaration-macro expansion; 7 files segfault the C++
    dump under `--skip-check`, neither being a valid differential target).
  - **Sema: ~30 slices, all diagnostic families done.** Resolution (undefined
    name, duplicate definition, throw-outside-throws), capability, exclusivity,
    ownership (move tracking), the full type-checker (initializer/annotation,
    call arity + arg types, returns, condition-Bool, operators, assignment,
    struct construction, member/index/optional/method typing, enum-case),
    generics tracking (unknown-type, is-not-generic), all match families
    (guard-Bool, exhaustiveness, arm-type, pattern checks), and multi-file
    modules (import resolution, qualified member + type).
  - **Codegen: 62 slices; the close-out roadmap is complete and the corpus
    burndown is active.** See "Where to pick up" below.

## Where to pick up: codegen

Codegen (`selfhost/codegen.vst` building `vgen`) is the active front. It re-uses
the lexer + parser, has the **whole sema type checker ported in** (the `TyEnv`),
and emits the generated C++. Two differentials run byte-for-byte vs
`vestra build`:

- **`vgen FILE`** gives the `.cpp` *definitions* (function bodies, etc.).
- **`vgen --hpp FILE`** gives the `.hpp` *per-program tail*: everything after the
  fixed ~1130-line runtime prelude (struct/enum defs, inline methods, forward
  declarations, top-level const/static). The reference tail is extracted with
  `awk 'f{print} index($0,"#endif  // VESTRA_RUNTIME_PRELUDE"){f=1}' FILE.hpp`.

**Done (slices 1 through 28):** free functions; all statements (let/var/assign/
while/for, statement-position if, break/continue, `with`-blocks); the full
expression set (calls, literals, index, `??`, member, struct construction,
force-unwrap); match expressions in **every statement-level position** — return,
`let`/`var` binding (annotated, or inferred via the new `tyInferMatch`;
unannotated payload-arm bindings out of scope), and bare statement (`-> auto`) —
through the position-agnostic `emitMatchValue` (payload-free `switch`,
payload `std::visit`, guard if/else chain, default/wildcard); payload-enum
construction; scalar + generic + optional types; **function-pointer types**
(`(A)->R` becomes `R(*name)(const A&)`); **closures** both in annotated bindings
(`{p=>body}` becomes a `[&]` lambda, param types from the annotation's function
type) and as **call arguments of named callees** (`apply({ n => n+1 }, 5)` — the
`TyEnv` param rows carry a `pTyNode` type-**node** table, so the callee's
function-typed parameter feeds `emitClosureVal`); structs (fields + inline
methods); enum defs (enum class + payload struct/variant; methods dropped both
sides); forward declarations; enum case access; top-level const/static; **named
payload fields** (`case circle(r: Float64)` puts `r` in the `.hpp` payload
struct and the `.cpp` visit binding; positional stays `_<idx>`); the **print
family** (`print`/`println`/`eprint`/`eprintln` → `std::print`/`std::println`,
text as a format *argument*; the other §18 builtins — `readFile`, `writeFile`,
`args`, `panic` — are still open, see `src/codegen/cpp_emitter.cpp` ~4320).

Slice 31 added **closures as method-call arguments** (per-method-parameter
type-node rows `mpOwner`/`mpName`/`mpIdx`/`mpTyNode`; the member callee's owner
comes from inferring the base expression's type). Its probe **found a bootstrap
bug**, fixed in `62b747b`: a method's function-typed parameter emitted
ill-formed `const R(*)(A)& f` instead of the declarator form `R(*f)(A)`.

Slice 32 added **if-as-value**: value position is a parenthesized IIFE with
per-branch lambdas (`([&]{ if (c) { return [&]{ … }(); } else … }())`, the
branch body sharing the closure-body shape via `emitClosureStmts`), while
**return position** is a plain statement `if` with returning branch tails.
If-let expressions stay stubbed.

Slice 33 added **guarded-payload match**, closing the last match-lowering
family: the visit emitter now GROUPS arms by case — one if-constexpr branch
per distinct case, each arm its own payload-binding scope, guarded arms
returning inside a runtime `if` that falls through to the next scope, and a
group ending in a guarded arm falling back to the default body. Unguarded
matches are the degenerate one-scope case (output-neutral rewrite).

Slice 34 added the **§18 builtins**: `panic`/`abort`/`unreachable`
(unconditional on name + arity) and `readFile`/`writeFile`/`args` (yielding to
unit-defined functions) lower to their `__vstr` shims, and `tyBuiltinRet`
teaches the call inference that `readFile` yields `String?` so `readFile(p)!`
force-unwraps rather than reading as logical-not. New Vestra-in-Vestra gotcha:
`copy x` cannot appear in transpiled code (the bootstrap stubs
copy-expressions) — restructure around conditional `return x` instead.

Slice 35 added **array/tuple literals**: `[e…]` lowers to `std::array<elemT,
N>{…}` with the element type threaded from the annotation or return type (else
first-element inference via the recursive `cppOfSpelling`); tuples are
context-free `std::tuple{…}` plus structured-binding destructuring, and
`emitType` gained `vecT`/`tupleT`. Notably the **index cast is
type-dependent**: only container bases (Vec/Span/MutSpan) get
`static_cast<std::size_t>`; `[N]T` arrays index plainly.

Slice 36 added **optional-chain**: `p?.x` lowers to
`p.transform([](auto&& __vstr_o) { return __vstr_o.x; })`, context-free, with
`??` composing through the existing `.value_or`. Probing verified sema only
permits a single field hop through `?.` (no method calls, no deeper members),
so that's the entire family.

Slice 37 added **Str→String coercion** (the §18.5 string lattice): a Str /
StrConst value flowing into a String slot wraps in `std::string(...)`, at all
five slots the bootstrap's resolver marks — return, annotated initializer,
assignment, call argument, struct-construction field. `tySetRetNode` now also
records `env.curRet` during emission. Exclusivity idiom worth remembering:
never pass `env.field` in the same call that takes `env` inout — bind the
derived value to a local first.

Slice 38 added **string interpolation**: the `interpE` capture was reshaped to
`interpE(Vec[String], Vec[Nd])` — literal fragment texts alongside the splice
exprs (`parts.len() == exprs.len() + 1`, empties included) — mirrored into
`parser.vst`/`sema.vst` with the parser differential staying byte-identical.
`"a \(x) b"` emits `std::format("a {} b", x)` with `{`/`}` doubled in
fragments.

Two **known bootstrap gaps** the text differential does not police (documented,
not yet fixed; discovered while compiling probe output):

- The closure lowering family (slices 26/27/31) emits `[&]` lambdas where the
  target type is a raw function pointer — C++ forbids that conversion for a
  lambda with a capture-default, and the lambda's by-value params mismatch the
  pointer's `const T&` params. The generated text matches byte-for-byte but
  does not compile. A real fix is bootstrap-side (captureless lambdas with
  matching params, or `std::function`).
- Methods are not const-qualified (v0.5 minimalism), so calling any method on
  a read-mode (`const T&`) parameter does not compile; fixtures call methods
  through `var` locals.

Slice 39 added the **§18.5 builtin methods** — the last big single-file gap.
`tryEmitBuiltinMethod` dispatches on the base's inferred type spelling: Vec
(`push`→`.push_back`, `len`→`.size()` as Int, `get`/`pop`→`__vstr` optional
helpers, `set`, `clear`), HashMap (`set`/`get`/`contains`/`len`/`keys`/
`values`/`entries`), the string families (append/len/isEmpty/slice/byteAt/
charAt/find/toInt/split/chars/contains/startsWith/endsWith), numeric
`toString`, and the Char predicates. `Type.new()` annotated bindings construct
the annotation's C++ type; `tyBuiltinMethodRet` feeds builtin return types
into the inference so chained calls, unwrap, and string coercion keep working
on builtin results. Still out (as bootstrap-side special cases): Soa methods,
span `split`/`chunks`, atomics, `Result.mapError`.

Slices 40–41 delivered the core **generics arc**. Functions: a generic
function lowers to a header-resident template (`template <class T, class U>` +
`[[nodiscard]]` + the same `emitFunc` body emitter; nothing in the `.cpp`,
plain call sites). Types: generic struct/enum definitions get template heads;
`Pair[Int32]` references lower to `Pair<std::int32_t>` (node-level via
`emitGenericType`'s fixed fallback, spelling-level via `cppOfSpelling` +
`genBaseOf`); and CONSTRUCTION recovers the instantiated C++ name from the
expected type (binding annotation or result type) via `TyEnv.genTypes` +
`emitGenericCtor` — including bare enum cases and generic-enum matches.
Still open in generics: const-generics, bounds, generic methods, and
arg-driven ctor inference (a generic construction with no expected type in
hand, e.g. as a call argument).

Slice 42 added **param modes**: `inout` → `T&`, `sink` → `T&&` (one dispatch
in `emitParam` covers every param surface), with call-site `&x` markers
dropping and move-only lvalues into sink params wrapping in `std::move` (new
`TyEnv.pMode` rows; move-only judged conservatively by spelling — owned
String/Vec/HashMap/Box only, user nominals not moved).

Slice 43 added the **throws core**: `throws(E)` wraps the declared result in
`std::expected<T, E>` (`void` when omitted) in both the `.cpp` definition and
the `.hpp` forward declaration (`emitExpectedRet`; the `[[nodiscard]]` rule
widened to cover throws-void). `throw e` at statement position lowers to
`return std::unexpected{e};`. A propagating `try` gets the canonical 3-line
hoist (`__vstr_tN_r` / early-return / unwrap into `__vstr_tN`) collected per
statement in the bootstrap's walk order (post-order at a try; if/match visit
only cond/scrutinee; `&&`/`||` skip the rhs), with the use-site consuming
names in collection order — `TyEnv.tryN` is file-global and never resets,
matching the bootstrap's `hoist_counter_`. `try!` → `__vstr::unwrap_exp`,
`try?` → the optional-converting IIFE. Out of scope: **cond-hoists** (a try
inside if/match branches lifts to a `__vstr_cN` lambda in the bootstrap),
do/catch, `.mapError`, throws methods, async+throws.

Slice 44 added the **async core**: an `async func` returns `__vstr::Task<R>`
(`emitTaskRet` wraps `emitExpectedRet`, so async+throws nests
`Task<std::expected<T, E>>` for free), its read params pass **by value** (the
coroutine frame must own copies across suspension), function-level `return` /
`throw` / the try-hoist escape switch to `co_return` (`TyEnv.curAsync` +
`emitRetKw`; lambda-internal returns untouched), a void async body not ending
in a return gets a trailing `co_return;`, `await e` → `co_await (e)`, and
`spawn e` → `__vstr::spawn_future(e)` (inference round-trips spawn→`Future[T]`
→await→`T`). Out of scope: select, channels/recv, async methods, async
closures.

Slice 45 added **import declarations**, closing the last structural arc: the
`.hpp` tail gains `#include "h"` for `import c "h"` then `#include "m.hpp"`
per module import (dotted path → directory path), each group followed by a
blank line; cross-module references emit fully qualified via `TyEnv.imports`
(`m.f(x)` → `m::f(x)`, `m.CONST` → `m::CONST`, and a Capitalized member call
is a cross-module construction `m.Point(x: a)` → `m::Point{.x = a}`). vgen
needs no import loading — the include and the qualified spelling suffice.
Note: the importED module must carry a `module <name>` decl or the
bootstrap's sema rejects the qualifier. Out of scope: a module decl in the
emitted file (namespace-wrapped output), qualified type annotations, nested
qualifiers (`m.Color.red`).

**Close-out roadmap (slices 46-54): COMPLETE.** All four phases landed, one
slice per commit:

- **Phase A, modules** — 46 namespace wrapping (`module a.b` →
  `namespace a::b { }` in both outputs; `cghelp.vst` became a differential
  target); 47 qualified type annotations (dotted type paths spell `::` via
  the context-free `cppNamed` fallback).
- **Phase B, throws tail** — 48 `.mapError` → `transform_error`
  (name-dispatched); 49 if-branch cond-hoists (the `__vstr_cN`
  expected-returning lambda, T from statement context, try tails via the
  fixed `__vstr_r` inline escape, snapshot-hardened hoist emission);
  50 match-arm cond-hoists (holds_alternative chain / braced-case switch /
  `__vstr_mh` if-chain — and a statement-position match types its lambda by
  its own arm type, unlike a statement if); 51 do/catch (annotated,
  value-position nested IIFEs; bare `catch e`, multi-statement bodies, and
  where-guard stay out — the bare form needs sema's inferred error type).
- **Phase C, generics tail** — 52 in one slice: const-generics
  (`template <std::size_t N>`), self typed as the owning struct (plain
  array-field indexing), bounds erasure (already matching), arg-driven ctor
  inference (callee param type node); generic methods documented as a
  SHARED bootstrap gap (no template head, non-compiling output both sides).
- **Phase D, async tail** — 53 async methods (plain non-coroutine shape but
  BY-VALUE read params, mirroring the bootstrap's method emitter);
  54 channels (`Channel[T]`/`Future[T]` → `__vstr::` type mappings;
  send/close/receive/new/await all ride existing machinery). **select is
  parked with rationale**: the bootstrap's ~800-line resolver-driven
  emit_select (channel-vs-Future arm classification, blocking SelectAwaiter
  coroutine vs poll lowering, timeout timers) plus a missing SelectExpr
  parser capture make it disproportionate to mirror.

Closures against **generic** callees (`map`/`filter`) stay out of reach: their
param types need full generic instantiation, not recoverable structurally.

**The corpus burndown is ACTIVE (slices 55+): 22 of 150 examples
byte-identical** (baseline was 11), and **total divergence across the corpus
is down from 23394 lines to 11149** (both under the minimal-diff count the
sweep now uses; see the metric note below). Landed: 55 entry main (`argc/argv` +
`args_init`, non-module files only — module-form mains already worked), 56
nil + statement if-let (62 example files use if-let), 57 scalar-conversion
casts (`Int32(x)` → `static_cast<...>`) + the iterator-protocol for-loop
(known-struct iterables), 58 tuple wildcard bindings (`__vstr_tp_unused_N`),
59 match payload binding TYPES, 60 nested match expressions, 61 tuple
for-loop patterns, 62 leading-dot case references.

Slices 59–60 are one arc and were measured together. **59** gave a payload
arm's binding the case's declared field type (a new `ecfTy` column beside
`ecfName`, `tyPayFieldTy` beside `tyPayFieldName`, registered in
`emitOneBinding`) — until then every type-directed lowering went blind
inside an arm, so `s.len()` never reached `.size()` and Str→String coercion
never fired. **60** routed `matchE` out of `emitExpr`'s `/*?expr*/` default
into `emitMatchValue`: a match arm body is a single expression, so a match
on a second scrutinee can only appear AS that body — the shape every
interpreter demo is built from. Two details the reference pins: the nested
lowering lays out at the FIXED statement indent (the bootstrap indents
absolutely, so depth never shifts the columns), and its lambda takes the
nested match's OWN inferred type, not `curRetNode`. That last part forced a
fix in `tyInferMatch`, which ran before the emitter registered arm bindings
and so typed `x + y` over two payload names as `Int` rather than `Int32`;
it now registers each arm's bindings before typing that arm.

Slice **61** made a tuple for-pattern a C++ structured binding —
`for (k, v) in m.entries()` had been flattening to a single name and
emitting `for (auto&& : …)`, invalid C++, in 26 examples. `emitForHead` and
friends now take the PATTERN rather than a pre-flattened name, reusing slice
58's name list so a `_` element still mints `__vstr_tp_unused_<N>`.

Slice **62** resolved LEADING-DOT cases (`.plus`, `.just(v)`), which name no
enum of their own. The bootstrap reads the resolved type; vgen reads the
SLOT the value flows into — the five coercion slots plus match arm bodies
(a new `TyEnv.armTo`, published by `emitMatchValue` and restored after, so a
nested match cannot leak its arm slot outward). Three shapes: `Ordering`
first (a builtin whose cases ARE the `std::memory_order_*` literals), then
the sum-type variant wrap (instantiated name when generic), then the plain
scoped reference. It moved the example count for the first time in four
slices.

**Two notes on the metric.** First, the identical-example count held at 21
across slices 59–61 even as ~9800 divergent lines disappeared: the
interpreter demos each carry several remaining families, so line count is
the finer-grained signal mid-arc and example count only moves when a file's
LAST family closes. Second, line counts MUST come from minimal diffs
(`diff -d`, which the sweep now uses). Plain diff picks a non-minimal edit
script whose size depends on how surrounding text aligns, so repairing one
hunk can inflate the count elsewhere — slice 61 was falsely reported as a
regression that way. Reports produced before that fix read a few lines high
and are not comparable.

The sweep itself is [`scripts/sweep_codegen_corpus.sh`](scripts/sweep_codegen_corpus.sh):
`--build` rebuilds `vgen` first, and `--compare <earlier-report>` prints the
before/after totals and flags any example whose divergence GREW or that
stopped being identical. Run it before and after every burndown slice —
```
./scripts/sweep_codegen_corpus.sh --build --report build/sweep/before.txt
# … land the slice …
./scripts/sweep_codegen_corpus.sh --build --compare build/sweep/before.txt
```

**Next families, ranked by the post-slice-62 sweep:**
(1) MATCH ON A NON-ENUM SCRUTINEE (a `Str` or numeric matched against
LITERAL patterns) — the bootstrap lowers it to an equality if/else chain
over `auto&& __vstr_m = <scrut>;` (`__vstr_m == std::string_view("fn")`),
where vgen assumes an enum and emits the broken `case Str:::`. It gates the
big keyword-dispatch functions in every `interp_*` demo, and the arm bodies
inside it are leading dots that already work. (2) OR-PATTERNS
(`case .a(n) | .b(n):`) — one if-constexpr
branch per alternative sharing the arm body (see examples/match.vst;
`armCaseName` on an `orP` currently yields the broken `Enum::_t`). Smaller
but cheap: `[[gnu::*]]` attribute passthrough (7 examples), derive-generated
methods `clone`/`operator==`/`default` in the `.hpp` (6), `requires`-clause
bounds (1). After those, re-sweep and re-rank.

**The original burndown proposal (measured 2026-07-24), kept for the
metric's definition:** With the
family roadmap complete, the completion standard shifts to the one the lexer
and parser already met — the whole `examples/` corpus, byte-identical. The
baseline measurement: **11 of 150 examples** already emit identically
(`.cpp` + `.hpp` tail), and **`vgen` on its own 10,807-line source diverges
on only 738 lines (~93% identical)** — self-emission is a reachable
milestone. Sampled divergence families, each a normal slice: `func main`
emission (`int argc, char** argv` + `__vstr::args_init` — touches nearly
every demo), if-let / while-let (`if (auto __vstr_opt = o; …)` — the long-
stubbed family), the `nil` literal (`std::nullopt`), range-for bound casts
(`static_cast<std::int32_t>(0)`, not `Int32(0)`), tuple wildcard bindings
(`__vstr_tp_unused_<i>`), and match payload-pattern case-name capture
(`Event::_t` instead of `Event::keyDown_t`). The loop: pick the divergence
family with the highest example count, slice it, re-measure — the
identical-example count is the arc's progress metric.

**Remaining unmirrored codegen** (full audit in the memory file): select
(parked, see above), the do/catch leftovers (bare `catch e`, multi-statement
bodies, where-guard), unannotated bindings of if/match cond-hoists (vgen
does not infer if-expressions), and the low-value tier (comptime,
quote/splice, @-intrinsics). Known SHARED bootstrap gaps, mirrored rather
than fixed: closure→fn-ptr conversion and template-less generic methods
(both produce non-compiling C++; the differential is textual).

## How the work proceeds

A tight, one-slice-at-a-time loop:

1. Pick the next gap (this handoff or the audit in the memory file).
2. **Probe the target lowering**: run `vestra build` (or `check`) on a tiny
   hand-written program and read the reference output byte-exactly
   (`cat -e`, `od -c`, `diff`), since spacing and punctuation must match.
3. Implement the slice in the relevant `.vst` file (mirror parser-front-end
   changes into the downstream files).
4. **Rebuild the binary and diff.** For codegen:
   ```
   build/debug/vestra build selfhost/codegen.vst -o /tmp/vgenbuild
   c++ -std=c++26 -O0 -I/tmp/vgenbuild /tmp/vgenbuild/codegen.cpp -o /tmp/vgenbuild/vgen
   diff <(/tmp/vgenbuild/vgen FILE.vst) <(vestra build FILE.vst …)
   ```
5. Add a curated fixture under `tests/e2e/codegen/` (or `tests/e2e/sema/`), wire
   its stem into the `foreach(prog …)` list(s) in `tests/CMakeLists.txt`
   (codegen fixtures go in **both** the `.cpp` and `.hpp` lists), re-run
   `cmake .` + `ctest -R e2e_selfhost`.
6. One commit per slice; then update the `project_dogfood_compiler.md` memory.

Standing constraints, honored every commit:

- **Build via system cmake** at `build/debug` (CLion's `cmake-build-debug` fails
  on `cxx_std_26`).
- **Explicit `git add` paths**, never `git add -A`/`.`. The working tree carries
  untracked spec drafts (`VESTRA_DRAFT_3_*.md`, `VESTRA_ANNEX_A.md`) the user
  authors independently and that must never be swept into a commit.
- **No `--no-verify`, no force-push to main.** Branch first if on `main` and the
  user hasn't asked to commit there.
- Commit subject `selfhost: <phase> slice N — <feature>`, a rationale body, and
  the trailer `Co-Authored-By: Claude Fable 5 <noreply@anthropic.com>` (the
  active Claude model; earlier slices credit Opus 4.8).

## Rough edges (writing Vestra-in-Vestra)

- **Types must be defined before use** in a `.vst` file: a new helper type used
  by the emit functions has to sit ahead of them (the `TyEnv` port had to be
  physically relocated above the emitters).
- **A match arm body is a single expression.** No `return`, no assignment, no
  multi-statement body inside an arm; extract a helper, or use `match` itself as
  an expression (`return match x { … }`).
- **Move analysis is flow-insensitive**: returning a value after a later `push`
  to the same vector trips a use-after-move; return a fresh `Vec.new()` on the
  empty path instead.
- The C++ printer dumps unhandled constructs as `/*?stmt*/` / `/*?expr*/` /
  `/*?type*/`. The self-hosted parser still consumes those tokens precisely (so
  the token stream stays aligned) and carries the captured AST for downstream
  phases, but because no `print*` case is added, the **parser differential stays
  byte-identical**. Closures, `with`-blocks, array/tuple/interp literals are all
  captured this way.
- Closures use **last-expression-as-value**; an explicit `return` inside a
  closure makes it type as `Unit`.
- `pi` is a reserved/builtin name, so don't name a fixture function `pi`.
- Commit with `git commit -F <messagefile>` (backtick substitution otherwise
  mangles bodies).

## The bootstrap compiler (the stable substrate)

The C++ `vestra` compiler is the thing being dog-fooded; it is feature-complete
enough to compile the self-hosted compiler. Its pipeline is
`lex → parse → resolver → ownership → exclusivity → capability → comptime fold →
C++ emitter`; every phase reports on a shared `DiagnosticReporter`, and
`vestra build` refuses to emit C++ when sema reports an error (override with
`--skip-check`). Toolchain: Apple Clang (Xcode) at `/usr/bin/c++`, CMake 4,
Ninja; clang-format from Homebrew. The full per-phase history of that arc
(modules, collections, generics phase 2, async/concurrency, SIMD, the kernel/
annex slices, and the rest) is in the git log; search `git log --grep` by
section number (`§5`, `§7`, `§11`, …).
