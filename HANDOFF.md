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

- **1576 / 1576 tests pass** (`ctest --test-dir build/debug`), Debug build,
  format-check clean. Of these, **286 are `e2e_selfhost`** (4 lexer, 11 parser,
  74 sema, 193 codegen, 4 end-to-end demo).
- Latest slice: `4b81978` (codegen slice 98, tuple-pattern parameters and
  payloads; corpus burndown 44). **140 commits** carry the `selfhost:` prefix.
- **Corpus burndown: 136 of 150 examples byte-identical, 686 divergent lines**
  (625 of them parked by design or in practice, so 61 live; see
  the ranking below). The baseline when the burndown started was 11 examples
  and 23394 lines.
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
  - **Codegen: 98 slices; the close-out roadmap is complete and the corpus
    burndown is active.** See "Where to pick up" below.

## Where to pick up: codegen

Codegen (`selfhost/codegen.vst`, ~10.2K lines, building `vgen`) is the active
front. It re-uses the lexer + parser, has the **whole sema type checker ported
in** (the `TyEnv`), and emits the generated C++. Two differentials run
byte-for-byte vs `vestra build`:

- **`vgen FILE`** gives the `.cpp` *definitions* (function bodies, etc.).
- **`vgen --hpp FILE`** gives the `.hpp` *per-program tail*: everything after the
  fixed ~1130-line runtime prelude (struct/enum defs, inline methods, forward
  declarations, top-level const/static). The reference tail is extracted with
  `awk 'f{print} index($0,"#endif  // VESTRA_RUNTIME_PRELUDE"){f=1}' FILE.hpp`.

### The next action

The completion standard is the whole `examples/` corpus byte-identical, the same
bar the lexer and parser already met. **136 of 150** examples are there;
**686 divergent lines** remain, of which **625 are parked** (by design or
in practice; see the ranking), leaving **61 live**. Families #1 to #8 and #10 have shipped, and five of the eight family #9
one-offs (slices 75 to 98, session of 2026-11-14), reaching 136 identical.
**Three live examples remain**, each a larger lowering than the ones before
it and each already listed under the known gaps below: `docatch_where` (14,
the do/catch `where` guard as a cond-hoist), `soa_demo` (20, the Soa method
surface) and `midtry` (27, an `if` cond-hoist at an unannotated binding and
mid-expression). Probe each against its bootstrap emitter before sizing it. First re-establish the baseline, since it is also
the regression check:

```
./scripts/sweep_codegen_corpus.sh --build --report build/sweep/before.txt
#   … pick a family below, probe it, implement the slice …
./scripts/sweep_codegen_corpus.sh --build --compare build/sweep/before.txt
```

`--compare` prints the before/after totals and flags any example whose
divergence GREW or that stopped being identical. Run it every slice: in twelve
slices it flagged twice, once on a genuine gap the fixtures would have missed
(slice 63, an unregistered do/catch binding) and once on a false alarm that
turned out to be the metric itself (slice 61, which is why the sweep now counts
with `diff -d`). Both were worth knowing.

**Ranked candidates (full re-rank after slice 74).** Slices 67 to 74 (session
of 2026-09-25) took the count from 38 to 93. Every one of the 54 live
divergent examples was then read and bucketed; lines are the sweep's per-
example totals, "flips" the examples a family would make identical.

| # | Family | Examples (lines) | Flips | Effort |
|---|---|---|---|---|
| 1 | ~~**Loop-element / receiver typing**~~ DONE in slice 75: a `for` element or iterable is untyped, so `.len()` / `.isDigit()` / `.split()` / `.toInt()` miss the §18.5 builtin path | `tokenize_demo` 4, `string_split_demo` 4, `hashmap_iter_demo` 4, `selfhost_demo` 2 | 4 | small |
| 2 | ~~**Global without initializer**~~ DONE in slice 76 emits `T x = ;` instead of `inline T x;` | `padded`, `per_cpu`, `per_cpu_extras`, `link_attrs` (1 to 2 lines each) | 0 alone | tiny; unblocks 5 and 6 |
| 3 | ~~**Or-patterns over enum cases**~~ DONE in slice 77 (`case .red, .green:` as stacked `case` labels; `case .a(let n), .b(let n)` in a visit) | `match` 8, `match_patterns` 4 | 2 | small |
| 4 | ~~**Cheap one-offs**~~ DONE in slices 78 to 83 (`addr_of` also fell from 18 to 14): sink arg of a struct not moved; `requires` bounds; typed `with x = e` binding; if-let as a value; `?.` through an optional field (`and_then`); a function name as a value (`&f`) | `ownership` 2, `generic_bounds` 2, `with_binding` 4, `optional` 4, `optchain` 6, `fn_ptr_table` 6 | up to 6 | small each |
| 5 | ~~**Attributes**~~ DONE in slices 88 to 90 (attribute capture on funcs, statics, structs and fields; link attributes and `@panic_handler`; struct layout; `@interrupt` slot types) | | | |
| 6 | ~~**Atomics / sync surface**~~ DONE in slices 84 to 87, including the `atomics` example itself; `panic_handler` moved to #5 | | | |
| 7 | ~~**Raw pointers and MMIO**~~ DONE in slices 80, 84, 86 and 92 (mints, spans, MMIO views, `p.value`, mem intrinsics, digit separators) | | | |
| 8 | ~~**Spans and iterator adapters**~~ DONE in slice 93 (span `.count` / `.isEmpty` / `split` / `chunks`; `zip` / `take` / `map` / `filter` with typed closures; iterator-protocol tuple binders) | | | |
| 9 | **Medium one-offs**: five DONE in slices 94 to 98 (`simd_demo` vector arithmetic, `map_error` typed closures, `const_generic` generic substitution, `parallel_demo`, `tuple_params`); three remain | `docatch_where` 14, `soa_demo` 20, `midtry` 27 (an `if` cond-hoist at an unannotated binding and mid-expression) | 1 each | medium |
| 10 | ~~**Typed system registers**~~ DONE in slice 91 (`__vstr::sysreg::<name>`, barrier-gated `write_<reg>` unless `@no_auto_barrier`) | | | |

**Atomics decision (answered 2026-11-14).** Unparked. The `atomics` example
was only scoped out of slice 39 to keep that slice reviewable, then labelled
"parked by design" in the slice 63 re-rank without a structural reason
(unlike `select` and comptime). Slices 84 to 87 ported the whole surface and
the example is now identical.

**Also parked in practice**, beyond the two below: the **comptime family**
(`comptime` 26, `reflection` 22, `cfg` 20, `sin_table` 11, `vectors` 11,
`embed` 2, `inline_layout` 16 and `spans` 11). The bootstrap folds comptime
functions and drops them from the output, and comptime is already a known
unmirrored gap. `async_demo` (20) is a `select`, the same parked family as
`channel_select_demo`. Counting those, 625 of the 686 remaining lines are
parked and 61 are live (measured from the sweep report after slice 98).

**A pattern worth checking first.** When many examples share the same small
divergence count (44 `interp_*` demos at exactly 4 lines each), the cause is
almost always one missing builtin type. Slice 67 was a one-line fix to
`tyBuiltinRet`.

**Parked by design; expect these to stay divergent.** `quote_demo` (368,
comptime/quote) and `channel_select_demo` (118, select). Together they are
half of the remaining line count, so subtract them before reading the total
as progress.

### Two lessons about the metric

Both were learned by getting them wrong; they cost a slice each.

- **The identical-example count is coarse mid-arc.** A file only flips when its
  LAST divergent family closes, so a slice can remove thousands of lines and
  move it by zero — slices 59–61 cut ~9800 lines at a flat 21/150. Track total
  divergent LINES as the fine-grained signal and treat the example count as a
  milestone.
- **Line counts must come from MINIMAL diffs** (`diff -d`, which the sweep
  uses). Plain diff picks a non-minimal edit script whose size depends on how
  the surrounding text happens to align, so repairing one hunk can inflate the
  count elsewhere and fake a regression. Reports from before commit `9b67d6d`
  read a few lines high and are not comparable.

### A ranking caveat

Slice 64's family was recorded in an earlier handoff as a cosmetic
`} else { if` vs `} else if` spacing difference. It was the reverse: the
reference was the side with the nested braces, and vgen was silently DROPPING
every else-if branch. Before characterising a family from a diff, confirm which
side is the reference (check the `diff` argument order), and prefer reading the
generated output directly over reading the `<`/`>` markers.

### What codegen covers

Ninety-eight slices; the close-out roadmap is complete and the corpus burndown
is active. The per-slice engineering log — every lowering, its bootstrap source
line, and its gotchas — is in the `project_dogfood_compiler.md` memory file.
The capability summary:

- **Declarations.** Free functions, structs (fields + inline methods), enums in
  both the `enum class` and struct-of-variant forms, forward declarations,
  top-level const/static, module namespace wrapping, imports (C headers and
  modules, with cross-module references fully qualified).
- **Statements.** let/var/assign, while, for (ranges, containers, the iterator
  protocol, and tuple destructuring), if / else-if chains / if-let, break,
  continue, `with` blocks, return.
- **Expressions.** The full set — calls, literals, index, `??`, member,
  optional chains, struct construction, force-unwrap, array/tuple literals,
  string interpolation, scalar casts, leading-dot cases, closures, block
  expressions.
- **Match.** Every lowering family: payload-free `switch`, payload
  `std::visit` (grouped by case, guarded arms falling through), guard chains,
  non-enum value scrutinees with predicate patterns, and match in every
  position including nested inside an arm body.
- **Types.** Scalars, generics (functions, structs, enums, const-generics,
  arg-driven ctor inference), optionals, function pointers, param modes
  (`inout` / `sink` with call-site `std::move`), the Str/String lattice,
  opaque newtypes (`enum class Q : T {}`, `Q(t)`, `q.value`), and `Box`
  (`std::make_unique`, `(*b)`), and tuple types (`(A, B)` spellings, so
  destructured names keep their element types).
- **Ownership.** The bootstrap's move-only rule: a type that transitively owns
  a `Box` is found by fixpoint, and lvalues of it move at bindings, tuple
  elements, payload and field initializers, and try-hoist unwraps.
- **Effects.** `throws(E)` as `std::expected` with the full try family
  (propagating hoists, `try!`, `try?`, cond-hoists, `.mapError`, do/catch in
  both the annotated and bare `catch e` forms),
  and async as `__vstr::Task` with `await` / `spawn` / channels.
- **Builtins and derives.** The §18 builtins, the §18.5 builtin-method surface
  (Vec / HashMap / Str family / Char / toString), and §12.3 derives
  (Eq, Clone, Default, Hash, Debug, Display), on structs, enums, and opaque
  newtypes.

**Known gaps, deliberately unmirrored.** `select` (the bootstrap's ~800-line
resolver-driven `emit_select` plus a missing `SelectExpr` parser capture make
it disproportionate); comptime / quote / splice / `@`-intrinsics; the do/catch
leftovers (multi-statement bodies, where-guard, and a bare `catch e` whose
first `try` calls a method rather than a plain function); unannotated
bindings of if cond-hoists (unannotated match bindings hoist since slice 72);
nested tuple patterns outside `let`/`var` (for loops, match arms, do/catch
still take the flat form); the bare-enum if-chain for two UNGUARDED arms
naming the same case (the bootstrap's `any_duplicate` trigger; vgen only
takes the chain on a guard, and no example hits it); an if-EXPRESSION as
the sole statement of a return-position `else { … }` block (the bootstrap
flattens it into nested returning ifs, vgen emits a value lambda; the
`else if` spelling is fine); `PerCpu.new` takes its T from the
argument rather than the call's expected type, so a literal argument under
a wider `Box[PerCpu[T]]` annotation would differ; `Ptr.unchecked` /
`MutPtr.unchecked` know T only as the whole initializer of an annotated
binding or a returned value (elsewhere they spell the bootstrap's
defensive `void`); `map(xs, f)` types its element as unknown (it would
need the closure's result type), so a loop variable over a map is untyped;
an else-less `if` as the LAST statement of a closure body (the bootstrap
lowers it as a value if with a `decltype(...){}` else branch; vgen prints
`/*?expr*/`); Soa methods.

**Known SHARED bootstrap gaps**, mirrored rather than fixed — both produce
non-compiling C++ on BOTH sides, so the differential stays textual:
closure→fn-ptr conversion (a `[&]` lambda cannot convert to a raw function
pointer), a METHOD's read-mode raw-pointer parameter (`const const T*& p`:
only free functions pass `Ptr` / `MutPtr` by value; compiles with a
duplicate-const warning), template-less generic methods, and destructuring a move-only tuple
LVALUE (`let (c, k) = q` emits `auto [c, k] = q;`, a copy; found in slice 73). Separately, methods are not
const-qualified in v0.5, so calling one on a read-mode (`const T&`) parameter
does not compile; fixtures call methods through `var` locals.

**One bootstrap bug the differential found**, fixed in `62b747b`: a method's
function-typed parameter emitted the ill-formed `const R(*)(A)& f` instead of
the declarator form `R(*f)(A)`. Probing a lowering against the reference is
worth doing even when vgen looks wrong.

## How the work proceeds

A tight, one-slice-at-a-time loop:

1. Pick the next gap (the ranked list above, or the audit in the memory file).
2. **Sweep first** — `scripts/sweep_codegen_corpus.sh --build --report
   build/sweep/before.txt` — so there is a baseline to compare against.
3. **Probe the target lowering**: run `vestra build` (or `check`) on a tiny
   hand-written program and read the reference output byte-exactly
   (`cat -e`, `od -c`, `diff`), since spacing and punctuation must match.
   Also read the bootstrap emitter that produces it; `src/codegen/` is the
   spec, and every lowering has one function that owns it.
4. Implement the slice in the relevant `.vst` file (mirror parser-front-end
   changes into the downstream files).
5. **Rebuild the binary and diff.** For codegen:
   ```
   build/debug/vestra build selfhost/codegen.vst -o /tmp/vgenbuild
   c++ -std=c++26 -O0 -I/tmp/vgenbuild /tmp/vgenbuild/codegen.cpp -o /tmp/vgenbuild/vgen
   diff -d <(/tmp/vgenbuild/vgen FILE.vst) <(vestra build FILE.vst …)
   ```
6. **Re-sweep with `--compare build/sweep/before.txt`** and read the regression
   lines. Do not skip this — a flagged example is either a genuine gap the
   fixtures would have missed or a problem with the measurement, and both are
   worth stopping for.
7. Add a curated fixture under `tests/e2e/codegen/` (or `tests/e2e/sema/`), wire
   its stem into the `foreach(prog …)` list(s) in `tests/CMakeLists.txt`
   (codegen fixtures go in **both** the `.cpp` and `.hpp` lists), re-run
   `cmake .` + `ctest -R e2e_selfhost`, then the full `ctest`. Each
   differential builds into its own `_codegen/<cpp|hpp>/<prog>` directory
   (since `d4dbfc6`); a shared output path raced under `ctest -j8` and failed
   a random unrelated fixture about one run in five. Keep new test commands
   on per-test paths.
8. One commit per slice; then update the `project_dogfood_compiler.md` memory.
   Keep doc/script changes in their own commits so the slice diff stays
   reviewable.

Standing constraints, honored every commit:

- **Build via system cmake** at `build/debug` (CLion's `cmake-build-debug` fails
  on `cxx_std_26`).
- **Explicit `git add` paths**, never `git add -A`/`.`. The working tree carries
  untracked spec drafts (`VESTRA_DRAFT_3_*.md`, `VESTRA_ANNEX_A.md`) the user
  authors independently and that must never be swept into a commit.
- **No `--no-verify`, no force-push to main.** Slices commit **directly to
  `main`** — confirmed by the user on 2026-07-24, and consistent with all 108
  prior `selfhost:` commits. No need to ask again.
- Commit subject `selfhost: <phase> slice N — <feature>`, a rationale body, and
  a `Co-Authored-By:` trailer naming the ACTIVE Claude model (slices 67 to 74
  credit Opus 5.5, 59 to 66 Opus 5, earlier ones Fable 5 and Opus 4.8).

## Rough edges (writing Vestra-in-Vestra)

- **Types must be defined before use** in a `.vst` file: a new helper type used
  by the emit functions has to sit ahead of them (the `TyEnv` port had to be
  physically relocated above the emitters).
- **A match arm body is a single expression.** No `return`, no assignment, no
  multi-statement body inside an arm; extract a helper, or use `match` itself as
  an expression (`return match x { … }`).
- **Move analysis is flow-insensitive**: returning a value after a later `push`
  to the same vector trips a use-after-move; return a fresh `Vec.new()` on the
  empty path instead. The same applies to two `return nm` lines guarded by
  separate `if`s: fold the conditions into one.
- **Prefix keywords carry a trailing space** in `prefixE`: match `"try "`,
  `"try! "`, `"await "`, not `"try"`.
- A fixture's **reference** output must compile too: a `Box`-owning value
  passed as a read (`const&`) parameter cannot be boxed or moved, so such
  fixtures take `sink` parameters.
- The C++ printer dumps unhandled constructs as `/*?stmt*/` / `/*?expr*/` /
  `/*?type*/`. The self-hosted parser still consumes those tokens precisely (so
  the token stream stays aligned) and carries the captured AST for downstream
  phases, but because no `print*` case is added, the **parser differential stays
  byte-identical**. Closures, `with`-blocks, array/tuple/interp literals are all
  captured this way.
- Closures use **last-expression-as-value**; an explicit `return` inside a
  closure makes it type as `Unit`.
- `pi` is a reserved/builtin name, so don't name a fixture function `pi`.
- **Exclusivity**: never pass `env.<field>` in the same call that takes `env`
  as `inout` — bind the derived value to a local first
  (`let to = own(env.curRet)`, then pass `to`). This bites constantly in the
  emitters, where nearly every helper takes `env inout`.
- **Functions may be declared in any order** (the emitter writes forward
  declarations), so a new helper can sit next to its caller. Only TYPES are
  order-sensitive.
- Match arms must all have the SAME type, which constrains fixtures: a
  `String`-returning arm cannot sit beside a `StrConst` literal arm. Build a
  tiny helper (`func mkString(_ s: Str) -> String`) rather than fighting it.
- `own("")` is the idiom for an empty owned `String`; `own(x)` copies a `Str`
  into one. Example fixtures cannot call the compiler's internal helpers.
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
