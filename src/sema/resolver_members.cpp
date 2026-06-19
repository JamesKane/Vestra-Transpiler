// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// Member access, method synthesis, and enum-case construction:
// lookup_method, enum-case helpers, check_member, check_leading_dot.
// Split out of resolver.cpp; see resolver_internal.hpp for the shared
// free helpers.

#include "vestra/ast/nodes.hpp"
#include "vestra/sema/builtins.hpp"
#include "vestra/sema/resolver.hpp"
#include "vestra/sema/scope.hpp"
#include "vestra/sema/types.hpp"

#include "resolver_internal.hpp"

#include <array>
#include <cstdlib>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vestra::sema {

using detail::named_type_param_count;

TypePtr Resolver::lookup_method(TypePtr owner_type,
                                std::string_view name,
                                const ast::FuncDecl** out_method) {
    if (owner_type == nullptr) {
        return nullptr;
    }
    // §9 iterator combinators: synthesize `next() -> Element?` on the
    // builtin adaptor types. ZipIter[A, B]'s element is the tuple
    // (A, B); TakeIter[A]'s element is just A. The codegen runtime
    // provides the matching member function in __vstr::Zip / __vstr::Take
    // (see emit_runtime_preamble), so this is a pure type-level shim.
    // §A11 (§14.8) `PerCpu[T].mine()` — returns the current hart's
    // slot. v0.5 hosts a single slot so this is just the inner T;
    // the kernel target reads a target-specific hart-index
    // register and indexes the `[MAX_HARTS]Padded[T]` storage. The
    // call's mutating-receiver discipline is handled the same way
    // Atomic gets the inout-on-static exemption — the C++-layer
    // `mine()` returns a reference.
    // §14.12 SysregHandle (RW): `.read() -> T` and `.write(T) ->
    // Unit`. Both methods discharge Asm at the call site (gated in
    // capability.cpp via the SysregHandle base type).
    if (owner_type->kind() == TypeKind::SysregHandle && owner_type->inner() != nullptr) {
        if (name == "read") {
            return types_->make_function({}, owner_type->inner());
        }
        if (name == "write") {
            return types_->make_function({owner_type->inner()}, types_->unit());
        }
    }
    // §14.12.1 ReadOnlySysreg: only `.read()` admits. A `.write(v)`
    // call on a RO handle falls through to the no-such-method error
    // path, which produces a typed diagnostic naming the kind so
    // the user knows why `Sysreg.midr_el1.write(v)` failed compile.
    if (owner_type->kind() == TypeKind::SysregHandleRO && owner_type->inner() != nullptr) {
        if (name == "read") {
            return types_->make_function({}, owner_type->inner());
        }
    }
    // §14.12.1 WriteOnlySysreg: only `.write(v)` admits. The v0.5
    // canonical name set has no WO members but the kind is reachable
    // once the kernel target's table extends the registered set.
    if (owner_type->kind() == TypeKind::SysregHandleWO && owner_type->inner() != nullptr) {
        if (name == "write") {
            return types_->make_function({owner_type->inner()}, types_->unit());
        }
    }
    // §11 Channel[T] — `send(T) -> Unit` (the value is moved into the queue),
    // `recv() -> T?` (non-blocking: front value, or nil when empty),
    // `receive() -> T?` (suspending: `await ch.receive()` parks the task on an
    // empty open channel until a send wakes it; nil means closed-and-drained),
    // and `close() -> Unit` (mark the channel closed and wake parked
    // receivers).
    if (owner_type->kind() == TypeKind::Channel && owner_type->inner() != nullptr) {
        if (name == "send") {
            return types_->make_function({owner_type->inner()}, types_->unit());
        }
        if (name == "recv" || name == "receive") {
            return types_->make_function({}, types_->make_optional(owner_type->inner()));
        }
        if (name == "close") {
            return types_->make_function({}, types_->unit());
        }
    }
    // §18.5 Vec[T] methods (v0.5 core): `push(T)` appends (mutating),
    // `len() -> Int` reports the count, `get(Int) -> T?` is the bounds-checked
    // element read (nil when out of range), `pop() -> T?` removes and returns
    // the last element (nil when empty), `set(Int, T)` overwrites an element in
    // place (unchecked, like indexing), and `clear()` empties the Vec.
    // Unchecked element reads still go through indexing (`xs[i]`, see
    // check_index); `for x in xs` iterates (see the ForStmt resolution).
    if (owner_type->kind() == TypeKind::Vec && owner_type->inner() != nullptr) {
        TypePtr T = owner_type->inner();
        if (name == "push") {
            return types_->make_function({T}, types_->unit());
        }
        if (name == "len") {
            return types_->make_function({}, types_->primitive(TypeKind::Int));
        }
        if (name == "get") {
            return types_->make_function({types_->primitive(TypeKind::Int)},
                                         types_->make_optional(T));
        }
        if (name == "pop") {
            return types_->make_function({}, types_->make_optional(T));
        }
        if (name == "set") {
            return types_->make_function({types_->primitive(TypeKind::Int), T}, types_->unit());
        }
        if (name == "clear") {
            return types_->make_function({}, types_->unit());
        }
    }
    // §13 Soa[T] methods (v0.5 core): `push(T)` scatters a struct value's
    // fields into the parallel columns, `len() -> Int` reports the row count,
    // and `get(Int) -> T` gathers the i-th row back into a struct. Column-view
    // accessors are a follow-on slice.
    if (owner_type->kind() == TypeKind::Soa && owner_type->inner() != nullptr) {
        TypePtr T = owner_type->inner();
        if (name == "push") {
            return types_->make_function({T}, types_->unit());
        }
        if (name == "len") {
            return types_->make_function({}, types_->primitive(TypeKind::Int));
        }
        if (name == "get") {
            return types_->make_function({types_->primitive(TypeKind::Int)}, T);
        }
    }
    // §18.5 String methods (v0.5 core): `append(Str)` concatenates another
    // string in place (a literal or borrowed view; std::string::append takes a
    // string_view), and `len() -> Int` reports the byte length. Built via
    // `String.new()` + append; appending an owned String awaits a Str borrow.
    if (owner_type->kind() == TypeKind::String) {
        if (name == "append") {
            return types_->make_function({types_->primitive(TypeKind::Str)}, types_->unit());
        }
    }
    // §18.5 String/Str read-only queries (v0.5 core): the byte-oriented slicing
    // and search surface, synthesized uniformly on the owned `String` and the
    // borrowed `Str` / `StrConst` views since all three lower through a
    // std::string_view. A returned `Str` from `slice` borrows the receiver's
    // bytes (cross-statement liveness of that borrow is a v0.5 carry-forward,
    // like the Span partitioners). Mutating methods (`append`) stay String-only
    // above; these queries never allocate, so they need no capability gate.
    //   len()            -> Int      byte length
    //   isEmpty()        -> Bool     length == 0
    //   slice(Int, Int)  -> Str      the [from, to) sub-view, clamped into range
    //   byteAt(Int)      -> UInt8?   the i-th byte, nil when out of range
    //   find(Str)        -> Int?     first index of the needle, nil when absent
    //   contains(Str)    -> Bool     needle present
    //   startsWith(Str)  -> Bool     prefix test
    //   endsWith(Str)    -> Bool     suffix test
    //   toInt()          -> Int?     parse the whole string as a decimal Int
    if (owner_type->kind() == TypeKind::String || owner_type->kind() == TypeKind::Str
        || owner_type->kind() == TypeKind::StrConst) {
        TypePtr str = types_->primitive(TypeKind::Str);
        if (name == "len") {
            return types_->make_function({}, types_->primitive(TypeKind::Int));
        }
        if (name == "toInt") {
            return types_->make_function({},
                                         types_->make_optional(types_->primitive(TypeKind::Int)));
        }
        if (name == "isEmpty") {
            return types_->make_function({}, types_->boolean());
        }
        if (name == "slice") {
            return types_->make_function(
                {types_->primitive(TypeKind::Int), types_->primitive(TypeKind::Int)}, str);
        }
        if (name == "byteAt") {
            return types_->make_function({types_->primitive(TypeKind::Int)},
                                         types_->make_optional(types_->primitive(TypeKind::UInt8)));
        }
        if (name == "find") {
            return types_->make_function({str},
                                         types_->make_optional(types_->primitive(TypeKind::Int)));
        }
        if (name == "contains" || name == "startsWith" || name == "endsWith") {
            return types_->make_function({str}, types_->boolean());
        }
    }
    // §18.5 `n.toString() -> String` renders a numeric value into an owned,
    // decimal string (the rendering counterpart of `Str.toInt()`). Synthesized
    // on the standard arithmetic primitives; the 128-bit kinds are excluded
    // because std::format / std::to_string have no portable __int128 formatter
    // yet (a v0.5 carry-forward). Allocates an owned String, so the call is
    // Alloc-gated in the capability checker.
    if (owner_type->is_numeric() && owner_type->kind() != TypeKind::Int128
        && owner_type->kind() != TypeKind::UInt128) {
        if (name == "toString") {
            return types_->make_function({}, types_->primitive(TypeKind::String));
        }
    }
    // §18.5 HashMap[K, V] methods (v0.5 core): `set(K, V)` inserts/updates,
    // `get(K) -> V?` is the lookup, `contains(K) -> Bool`, `len() -> Int`.
    if (owner_type->kind() == TypeKind::HashMap && owner_type->parts().size() == 2) {
        TypePtr K = owner_type->parts()[0];
        TypePtr V = owner_type->parts()[1];
        if (name == "set") {
            return types_->make_function({K, V}, types_->unit());
        }
        if (name == "get") {
            return types_->make_function({K}, types_->make_optional(V));
        }
        if (name == "contains") {
            return types_->make_function({K}, types_->primitive(TypeKind::Bool));
        }
        if (name == "len") {
            return types_->make_function({}, types_->primitive(TypeKind::Int));
        }
    }
    if (owner_type->kind() == TypeKind::PerCpu && owner_type->inner() != nullptr) {
        if (name == "mine") {
            return types_->make_function({}, owner_type->inner());
        }
        // §A11 (§14.8) `slot(hartId: UInt16) using RawMemory -> Ptr[T]`
        // — cross-hart accessor for the kernel's IPI / hart-startup /
        // stats-aggregation loops. v0.5 uses UInt16 as the HartId
        // surface type (proper opaque HartId would land alongside a
        // `core.systems.hart` namespace in a later slice). The
        // RawMemory capability gates the call site.
        if (name == "slot") {
            return types_->make_function({types_->primitive(TypeKind::UInt16)},
                                         types_->make_ptr(owner_type->inner()));
        }
    }

    // §A6 (§14.11) MMIO method synthesis. `MmioView[T]` exposes
    // `.read() -> T` and `.write(T)` (the latter mutates the
    // underlying register, but the inout receiver doesn't need a
    // `var` binding — same exemption Atomic gets, since the view
    // is a thin handle). `MmioRegion[T]` exposes `.index(Int)`
    // returning the indexed view.
    if (owner_type->kind() == TypeKind::MmioView && owner_type->inner() != nullptr) {
        if (name == "read") {
            return types_->make_function({}, owner_type->inner());
        }
        if (name == "write") {
            return types_->make_function({owner_type->inner()}, types_->unit());
        }
    }
    if (owner_type->kind() == TypeKind::MmioRegion && owner_type->inner() != nullptr) {
        if (name == "index") {
            return types_->make_function({types_->primitive(TypeKind::Int)},
                                         types_->make_mmio_view(owner_type->inner()));
        }
    }
    // §A6 (§14.11.3) MmioWireView exposes the same `read` / `write`
    // shape as MmioView; the difference (the conditional byte swap)
    // lives at the C++ template layer.
    if (owner_type->kind() == TypeKind::MmioWireView && owner_type->inner() != nullptr) {
        if (name == "read") {
            return types_->make_function({}, owner_type->inner());
        }
        if (name == "write") {
            return types_->make_function({owner_type->inner()}, types_->unit());
        }
    }

    // §A4 (§14.9.2) Atomic[T] method synthesis. Every method takes an
    // optional trailing `Ordering` argument; sema models them as a
    // single-required, single-optional shape — the default lives in
    // a documentation comment, codegen substitutes .seqCst when the
    // user omits it. fetchAdd/fetchSub take `(delta: T, ordering)`
    // and return T; load returns T; store/exchange take a T value;
    // exchange returns the prior T. The ordering type is the builtin
    // Ordering enum.
    if (owner_type->kind() == TypeKind::Atomic && owner_type->inner() != nullptr) {
        TypePtr T = owner_type->inner();
        TypePtr ordering_type =
            builtin_ordering_decl_ != nullptr
                ? types_->make_nominal(TypeKind::Enum, builtin_ordering_decl_.get())
                : types_->error();
        if (name == "load") {
            return types_->make_function({ordering_type}, T);
        }
        if (name == "store") {
            return types_->make_function({T, ordering_type}, types_->unit());
        }
        if (name == "exchange") {
            return types_->make_function({T, ordering_type}, T);
        }
        if (name == "fetchAdd" || name == "fetchSub" || name == "fetchAnd" || name == "fetchOr"
            || name == "fetchXor") {
            return types_->make_function({T, ordering_type}, T);
        }
        // §A4 (§14.9.3) compareExchange (strong) and compareExchangeWeak
        // share the same shape — (expected, desired, success, failure)
        // → CASResult[T]. The strong form is always admitted; the weak
        // form is admitted only inside one of the §14.9.3 retry-loop
        // shapes (see check_call below + the weak_cas_loop_depth_
        // counter incremented in WhileStmt). All four args are
        // positional in v0.5 (default args + labels on synthetic
        // methods wait on a separate infra phase).
        if (name == "compareExchange" || name == "compareExchangeWeak") {
            return types_->make_function({T, T, ordering_type, ordering_type},
                                         types_->make_cas_result(T));
        }
    }
    // §A4 (§14.9.5) AtomicTaggedPointer[T] method synthesis. The
    // wrapper hides the wide-atomic plumbing behind a typed
    // (MutPtr[T], tag) surface:
    //   .load(ordering) -> (MutPtr[T], UInt64)
    //   .store(ptr, tag, ordering) -> Unit
    //   .compareExchange(exp_ptr, exp_tag, des_ptr, success, failure)
    //       -> CASResult[(MutPtr[T], UInt64)]
    // The CAS auto-bumps the tag on the desired side (the codegen
    // does it), so callers thread only the pointer through; the tag
    // observed on failure threads back via .actual for the retry.
    // MutPtr is the right spelling since the Treiber-stack node's
    // link field is mutable and the CAS itself writes the pointer
    // through the wide-atomic word; users who only want a read-only
    // view can take a Ptr[T] from the MutPtr at the call site.
    if (owner_type->kind() == TypeKind::AtomicTaggedPointer && owner_type->inner() != nullptr) {
        TypePtr T = owner_type->inner();
        TypePtr ptr_t = types_->make_mut_ptr(T);
        TypePtr u64 = types_->primitive(TypeKind::UInt64);
        TypePtr snapshot = types_->make_tuple({ptr_t, u64});
        TypePtr ordering_type =
            builtin_ordering_decl_ != nullptr
                ? types_->make_nominal(TypeKind::Enum, builtin_ordering_decl_.get())
                : types_->error();
        if (name == "load") {
            return types_->make_function({ordering_type}, snapshot);
        }
        if (name == "store") {
            return types_->make_function({ptr_t, u64, ordering_type}, types_->unit());
        }
        if (name == "compareExchange") {
            return types_->make_function({ptr_t, u64, ptr_t, ordering_type, ordering_type},
                                         types_->make_cas_result(snapshot));
        }
    }
    if (name == "next") {
        if (owner_type->kind() == TypeKind::ZipIter && owner_type->parts().size() == 2) {
            auto elem = types_->make_tuple({owner_type->parts()[0], owner_type->parts()[1]});
            return types_->make_function({}, types_->make_optional(elem));
        }
        if (owner_type->kind() == TypeKind::TakeIter && owner_type->inner() != nullptr) {
            return types_->make_function({}, types_->make_optional(owner_type->inner()));
        }
        if (owner_type->kind() == TypeKind::MapIter && owner_type->parts().size() == 2) {
            // The element type is the *output* of f — parts[1].
            return types_->make_function({}, types_->make_optional(owner_type->parts()[1]));
        }
        if (owner_type->kind() == TypeKind::FilterIter && owner_type->inner() != nullptr) {
            return types_->make_function({}, types_->make_optional(owner_type->inner()));
        }
        if (owner_type->kind() == TypeKind::ChunkIter && owner_type->inner() != nullptr) {
            return types_->make_function({}, types_->make_optional(owner_type->inner()));
        }
    }
    const auto* decl = owner_type->nominal_decl();
    if (decl == nullptr) {
        return nullptr;
    }
    // §12.3 derive(Clone): surface a synthetic `clone() -> T` method on
    // any nominal that derives Clone, without materializing it in the
    // AST. The codegen injects the matching `T clone() const { return
    // *this; }` body when it emits the type. Bare enums (no payloaded
    // case) lower to C++ `enum class`, which has no method slot, so we
    // skip the synthetic there — the user can just use the implicit
    // copy on the enum value directly.
    if (name == "clone" && decl_derives(decl, "Clone")) {
        bool is_bare_enum = false;
        if (decl->kind == ast::NodeKind::Enum) {
            const auto& ed = static_cast<const ast::EnumDecl&>(*decl);
            is_bare_enum = true;
            for (const auto& c : ed.cases) {
                if (!c.payload.empty()) {
                    is_bare_enum = false;
                    break;
                }
            }
        }
        if (!is_bare_enum) {
            return types_->make_function({}, owner_type);
        }
    }

    auto search = [&](const std::vector<ast::DeclPtr>& methods) -> TypePtr {
        for (const auto& m : methods) {
            if (m->kind != ast::NodeKind::Func) {
                continue;
            }
            const auto& f = static_cast<const ast::FuncDecl&>(*m);
            if (f.name == name) {
                if (out_method != nullptr) {
                    *out_method = &f;
                }
                return function_type_of(f);
            }
        }
        return nullptr;
    };

    switch (decl->kind) {
    case ast::NodeKind::Struct:
        if (auto t = search(static_cast<const ast::StructDecl&>(*decl).methods)) {
            return t;
        }
        // Walk embeds: a method on the embedded struct surfaces too (§6).
        for (const auto& f : static_cast<const ast::StructDecl&>(*decl).fields) {
            if (f.kind == ast::StructDecl::Field::Kind::Embed && f.type) {
                auto embed_type = resolve_type(*f.type);
                if (auto t = lookup_method(embed_type, name, out_method)) {
                    return t;
                }
            }
        }
        return nullptr;
    case ast::NodeKind::Enum:
        return search(static_cast<const ast::EnumDecl&>(*decl).methods);
    default:
        return nullptr;
    }
}

const ast::EnumDecl::Case* Resolver::lookup_enum_case(const ast::EnumDecl& e,
                                                      std::string_view name) {
    for (const auto& c : e.cases) {
        if (c.name == name) {
            return &c;
        }
    }
    return nullptr;
}

TypePtr Resolver::enum_case_constructor_type(TypePtr enum_type, const ast::EnumDecl::Case& c) {
    const auto* decl = enum_type->nominal_decl();
    const auto& ed = static_cast<const ast::EnumDecl&>(*decl);
    const std::size_t arity = named_type_param_count(ed.generics);

    // §7 generics phase 2 — work out the instance arguments and the
    // constructor's result type. For a non-generic enum (arity 0) the
    // result is the bare nominal and payload types resolve plainly. For a
    // generic enum that already carries arguments (enum_type is an
    // instance, e.g. from an expected type or a leading-dot context) the
    // payload types resolve against those concrete arguments. For a generic
    // enum with no arguments yet (an explicit `Enum.case(...)` with no
    // context) the result is a placeholder instance `Enum[T, ...]` and the
    // payload types carry the same placeholders, so the generic-call path
    // in check_call infers the parameters from the argument types.
    std::vector<TypePtr> args = enum_type->parts();
    TypePtr result_type = enum_type;
    if (arity > 0 && args.empty()) {
        std::vector<TypePtr> placeholders;
        for (const auto& gp : ed.generics) {
            if (gp.is_const || gp.name.empty()) {
                continue;
            }
            placeholders.push_back(types_->make_generic_param(gp.name));
        }
        result_type = types_->make_enum_instance(decl, placeholders);
        // args stays empty so payload types resolve to the placeholders.
    }

    if (c.payload.empty()) {
        return result_type;  // bare case is a value of the enum
    }
    std::vector<TypePtr> params;
    params.reserve(c.payload.size());
    for (const auto& p : c.payload) {
        params.push_back(p.second ? resolve_member_type_with_generics(ed.generics, *p.second, args)
                                  : types_->error());
    }
    return types_->make_function(std::move(params), result_type);
}

TypePtr Resolver::check_qualified_module_ref(const ast::MemberExpr& m) {
    if (module_exports_.empty()) {
        return nullptr;
    }
    // Flatten the chain `((a.b).c)` into head-first segments [a, b, c]. Bail if
    // any link is an optional-chain or the head isn't a plain identifier (then
    // it's an ordinary member access, not a module path).
    std::vector<std::string> tail;  // outer-to-inner member names: [c, b]
    const ast::Expr* cur = &m;
    while (cur->kind == ast::NodeKind::MemberExpr) {
        const auto& me = static_cast<const ast::MemberExpr&>(*cur);
        if (me.is_optional_chain) {
            return nullptr;
        }
        tail.push_back(me.member);
        cur = me.base.get();
    }
    if (cur == nullptr || cur->kind != ast::NodeKind::IdentExpr) {
        return nullptr;
    }
    std::vector<std::string> segs;
    segs.push_back(static_cast<const ast::IdentExpr&>(*cur).name);
    for (auto it = tail.rbegin(); it != tail.rend(); ++it) {
        segs.push_back(*it);
    }
    if (segs.size() < 2) {
        return nullptr;
    }
    // A real value/type of the head's name shadows any module — that's an
    // ordinary member access, not a qualified module reference.
    if (scopes_.current().lookup(segs.front()) != nullptr) {
        return nullptr;
    }
    // The module path is all-but-last; the final segment names the export.
    // (v0.5: exactly one trailing segment — nested access on the export through
    // a qualified head is a later slice.)
    std::string dotted;
    for (std::size_t i = 0; i + 1 < segs.size(); ++i) {
        if (i != 0) {
            dotted += '.';
        }
        dotted += segs[i];
    }
    auto it = module_exports_.find(dotted);
    if (it == module_exports_.end()) {
        return nullptr;  // not an imported module path → ordinary member access
    }
    const std::string& exported = segs.back();
    // The export was resolved by the dependency's own resolver (so a signature
    // mentioning the module's own types is already correct) and handed to us in
    // module_exports_; the map is node-based, so the Symbol's address is stable
    // for the Resolution to point at.
    auto exp = it->second.find(exported);
    if (exp == it->second.end()) {
        error_at(m.range, std::format("module '{}' has no public member '{}'", dotted, exported));
        return types_->error();
    }
    const Symbol* sym = &exp->second;

    // Record the resolved symbol and the fully-qualified C++ name so codegen
    // emits `util::math::add` rather than a member access.
    std::string qualified;
    for (const auto& seg : segs) {
        if (!qualified.empty()) {
            qualified += "::";
        }
        qualified += seg;
    }
    resolution_.set_symbol(&m, sym);
    resolution_.set_qualified_name(&m, std::move(qualified));
    return sym->type;
}

TypePtr Resolver::check_member(const ast::MemberExpr& m, TypePtr expected) {
    // §5 a qualified reference into an imported module (`util.math.add`) is
    // resolved before any value/static member handling.
    if (auto qt = check_qualified_module_ref(m)) {
        return qt;
    }
    // §14.12 typed system-register access. `Sysreg.<name>` resolves
    // to a SysregHandle<UInt64> over the architectural register
    // named by <name>. The set is the v0.5 canonical aarch64 EL1
    // subset (`midr_el1`, `daif`, `sctlr_el1`, `vbar_el1`,
    // `ttbr0_el1`, `cntfrq_el0`); the kernel target's build extends
    // this with the full per-target list. Sema returns a handle
    // typed `SysregHandle<UInt64>`; `.read()` / `.write(v)` discharge
    // Asm at the call site.
    if (m.base->kind == ast::NodeKind::IdentExpr) {
        const auto& base_ident = static_cast<const ast::IdentExpr&>(*m.base);
        if (base_ident.name == "Sysreg" && scopes_.current().lookup("Sysreg") == nullptr) {
            // v0.5 target-description table. Read-only entries are
            // identification / clock-frequency reads that have no
            // architectural write path: MIDR_EL1 carries the CPU
            // model ID, CNTFRQ_EL0 is the system counter frequency
            // (firmware-set at boot). The kernel target extends both
            // sets with the full per-architecture lists; the WO set
            // is empty in v0.5's canonical subset but the TypeKind
            // is reachable for the wider name set the kernel needs.
            static const std::unordered_set<std::string_view> sysregs_ro = {
                // aarch64 EL1 — read-only
                "midr_el1",
                "mpidr_el1",
                "cntfrq_el0",
                "cntpct_el0",
                "dczid_el0",
            };
            static const std::unordered_set<std::string_view> sysregs_rw = {
                // aarch64 EL1 — read-write
                "daif",
                "sctlr_el1",
                "vbar_el1",
                "ttbr0_el1",
                "ttbr1_el1",
                "tcr_el1",
                "mair_el1",
                "esr_el1",
                "far_el1",
                "elr_el1",
                "spsr_el1",
                "tpidr_el1",
                // x86_64 MSRs — uniformly read-write at ring 0
                "ia32_efer",
                "ia32_lstar",
                "ia32_apic_base",
                "ia32_pat",
                // RISC-V S-mode CSRs — all read-write through csrr / csrw
                "sstatus",
                "sie",
                "stvec",
                "sscratch",
                "sepc",
                "scause",
                "stval",
                "satp",
            };
            TypeKind kind;
            if (sysregs_ro.contains(m.member)) {
                kind = TypeKind::SysregHandleRO;
            } else if (sysregs_rw.contains(m.member)) {
                kind = TypeKind::SysregHandle;
            } else {
                error_at(m.range,
                         std::format("Sysreg.{} — unknown system register; v0.5 admits the "
                                     "aarch64 EL1 + x86_64 MSR + RISC-V S-mode CSR canonical "
                                     "set (see §14.12.2)",
                                     m.member));
                return types_->error();
            }
            auto inner = types_->primitive(TypeKind::UInt64);
            return kind == TypeKind::SysregHandleRO ? types_->make_sysreg_handle_ro(inner)
                                                    : types_->make_sysreg_handle(inner);
        }
    }
    // If the base is an IdentExpr resolving to a Type symbol (Struct/Enum/...),
    // member access is a static lookup (currently: enum case construction).
    if (m.base->kind == ast::NodeKind::IdentExpr) {
        const auto& ident = static_cast<const ast::IdentExpr&>(*m.base);
        if (auto* sym = scopes_.current().lookup(ident.name)) {
            if (sym->kind == SymbolKind::Enum && sym->decl != nullptr) {
                resolution_.set_symbol(m.base.get(), sym);
                check_visibility(*sym, m.base->range);
                const auto& enum_decl = static_cast<const ast::EnumDecl&>(*sym->decl);
                if (const auto* c = lookup_enum_case(enum_decl, m.member)) {
                    // §7 generics phase 2 — if the surrounding context
                    // expects a concrete instance of this same enum, build
                    // the constructor against that instance so the type
                    // arguments are pinned (e.g. `let o: Option[Int32] =
                    // Option.none`). Otherwise use the bare nominal: a
                    // payloaded case infers its arguments from the call, and
                    // a no-payload case with no context is reported below.
                    TypePtr enum_t = sym->type;
                    if (expected != nullptr && expected->kind() == TypeKind::Enum
                        && expected->nominal_decl() == sym->decl && !expected->parts().empty()) {
                        enum_t = expected;
                    }
                    if (named_type_param_count(enum_decl.generics) > 0 && c->payload.empty()
                        && enum_t->parts().empty()) {
                        error_at(m.range,
                                 std::format("cannot infer type argument(s) for generic enum '{}' "
                                             "case '{}'; use it in a typed context (e.g. the "
                                             "leading-dot form `.{}` against a `{}[...]`)",
                                             enum_decl.name,
                                             m.member,
                                             m.member,
                                             enum_decl.name));
                        return types_->error();
                    }
                    auto t = enum_case_constructor_type(enum_t, *c);
                    return t;
                }
                error_at(m.range,
                         std::format("enum '{}' has no case '{}'", enum_decl.name, m.member));
                return types_->error();
            }
            // §12.2 reflection: `StructName.fields` returns a `[N]Field`
            // — one entry per ordinary field (embed fields per §6 are
            // not yet expanded). Field carries `name: Str` today;
            // later phases add `type`, `offset`, and attributes.
            if (sym->kind == SymbolKind::Struct && sym->decl != nullptr) {
                resolution_.set_symbol(m.base.get(), sym);
                check_visibility(*sym, m.base->range);
                const auto& sd = static_cast<const ast::StructDecl&>(*sym->decl);
                // §A2 (§6.8) compile-time layout reflection: T.size /
                // T.alignment fold to Int values. We invoke the folder
                // right here so codegen has a concrete literal to emit
                // (without the fold, `CapEntry.size` would land in the
                // C++ source verbatim — not valid). The fold may
                // return nullopt if some field uses an unsizeable
                // type; we diagnose so the user knows.
                if (m.member == "size" || m.member == "alignment") {
                    if (auto folded = folder_.fold(m, comptime_env_)) {
                        resolution_.set_folded_value(&m, *folded);
                    } else {
                        error_at(m.range,
                                 std::format("can't compute {}.{} at fold time — some field's "
                                             "type has no v0.5 layout (only primitives, vectors "
                                             "of primitives, and structs of those are supported)",
                                             sd.name,
                                             m.member));
                    }
                    return types_->primitive(TypeKind::Int);
                }
                if (m.member == "fields") {
                    std::int64_t count = 0;
                    for (const auto& f : sd.fields) {
                        if (f.kind != ast::StructDecl::Field::Kind::Embed) {
                            ++count;
                        }
                    }
                    // The Field nominal lives in global scope under that name.
                    auto field_type =
                        builtin_field_decl_ != nullptr
                            ? types_->make_nominal(TypeKind::Struct, builtin_field_decl_.get())
                            : types_->error();
                    return types_->make_vector(count, field_type);
                }
                // §12.3 derive(Default): expose a static `() -> T` so
                // `T.default()` is an ordinary CallExpr that the
                // codegen lowers to `T{}`.
                if (m.member == "default" && decl_derives(&sd, "Default")) {
                    return types_->make_function({}, sym->type);
                }
                error_at(m.range,
                         std::format("struct '{}' has no static member '{}'", sd.name, m.member));
                return types_->error();
            }
        }
    }

    auto base_type = check_expr(*m.base);
    if (base_type == nullptr || base_type->is_error()) {
        return types_->error();
    }

    // §9 optional chaining: `a?.b` requires `a : Optional<T>` and looks
    // `b` up on `T`. The chain wraps the lookup result in Optional, and
    // flattens an already-Optional member so `a?.b?.c` never yields
    // Optional<Optional<U>>.
    TypePtr lookup_base = base_type;
    if (m.is_optional_chain) {
        if (base_type->kind() != TypeKind::Optional) {
            error_at(m.base->range,
                     std::format("'?.' requires an Optional base, got {}", base_type->describe()));
            return types_->error();
        }
        lookup_base = base_type->inner();
        if (lookup_base == nullptr) {
            return types_->error();
        }
    }

    // §12.2 vector ergonomics: `someVector.length` is its element count
    // as Int. Folds at comptime; not yet wired for runtime emission
    // (vectors today are emitted as `std::array<T, N>` whose .size() is
    // a constexpr — wiring runtime `.length` is a follow-on).
    auto finish = [&](TypePtr member_type) {
        if (!m.is_optional_chain) {
            return member_type;
        }
        if (member_type == nullptr || member_type->is_error()) {
            return member_type;
        }
        // Flatten: a member already returning Optional<U> stays Optional<U>.
        if (member_type->kind() == TypeKind::Optional) {
            return member_type;
        }
        return types_->make_optional(member_type);
    };

    if (lookup_base->kind() == TypeKind::Vector && m.member == "length") {
        return finish(types_->primitive(TypeKind::Int));
    }

    // §3 opaque newtype: `q.value` extracts the underlying T. The
    // opaque inherits none of T's operations, so this is the user's
    // single explicit unwrap point.
    if (lookup_base->kind() == TypeKind::OpaqueType && m.member == "value"
        && lookup_base->nominal_decl() != nullptr) {
        const auto& od = static_cast<const ast::OpaqueDecl&>(*lookup_base->nominal_decl());
        if (od.underlying != nullptr) {
            return finish(resolve_type(*od.underlying));
        }
    }

    // §10 Box[T]: `box.value` reads the heap-pointee. Lowers to `*box`
    // at the C++ layer over std::unique_ptr.
    if (lookup_base->kind() == TypeKind::Box && m.member == "value"
        && lookup_base->inner() != nullptr) {
        return finish(lookup_base->inner());
    }

    // §A3 (§10.5) Ptr[T] / MutPtr[T]: `p.value` reads the pointee.
    // Mirrors Box[T].value at both the sema and codegen layers. The
    // MutPtr form is assignable through (`p.value = expr`) at the
    // C++ layer; sema doesn't enforce a separate place-expression
    // discipline here since the C++ compiler refuses an assign-
    // through-const-pointer naturally. Dereferencing itself doesn't
    // require RawMemory — the `.unchecked(fromAddress:)` mint that
    // produced the pointer already discharged the cap. v0.5 trusts
    // that audit lane to cover the read/write side too.
    if ((lookup_base->kind() == TypeKind::Ptr || lookup_base->kind() == TypeKind::MutPtr)
        && m.member == "value" && lookup_base->inner() != nullptr) {
        return finish(lookup_base->inner());
    }

    // §A11 (§14.8) Padded[T]: `.value` exposes the inner T. The
    // wrapper holds T plus tail padding to bring sizeof up to a
    // cache line; reads / writes pass through `.value` so the
    // padding stays invisible at the source level.
    if (lookup_base->kind() == TypeKind::Padded && m.member == "value"
        && lookup_base->inner() != nullptr) {
        return finish(lookup_base->inner());
    }

    // §10 Span[T] / MutSpan[T]: `.count` returns the element count as
    // Int; `.isEmpty` returns Bool. Codegen renders these as
    // `static_cast<std::intptr_t>(s.size())` and `s.empty()`.
    if ((lookup_base->kind() == TypeKind::Span || lookup_base->kind() == TypeKind::MutSpan)
        && lookup_base->inner() != nullptr) {
        if (m.member == "count") {
            return finish(types_->primitive(TypeKind::Int));
        }
        if (m.member == "isEmpty") {
            return finish(types_->boolean());
        }
    }
    // §A6 (§14.11.2) MmioRegion[T] surfaces `.count: Int` directly.
    if (lookup_base->kind() == TypeKind::MmioRegion && m.member == "count") {
        return finish(types_->primitive(TypeKind::Int));
    }
    // §5/§18.4 ChunkIter as a first-class value: `.count: Int` is the number
    // of chunks, so a `chunks(of:)` result can be indexed in a counted loop,
    // not only consumed by `for`.
    if (lookup_base->kind() == TypeKind::ChunkIter && m.member == "count") {
        return finish(types_->primitive(TypeKind::Int));
    }
    // §11 Duration accessors: `.nanoseconds` / `.microseconds` /
    // `.milliseconds` / `.seconds` read the total whole count in that unit as
    // Int (truncating toward zero). These are accessed, not called — distinct
    // from the same-named `.seconds(n)` *factory*. Codegen renders them as
    // `static_cast<std::intptr_t>(d.in_<unit>())`.
    if (lookup_base->kind() == TypeKind::Duration
        && (m.member == "nanoseconds" || m.member == "microseconds" || m.member == "milliseconds"
            || m.member == "seconds")) {
        return finish(types_->primitive(TypeKind::Int));
    }

    // Field on a struct.
    if (auto field_type = lookup_field(lookup_base, m.member)) {
        // §A2 (§12.2): when the base is a builtin reflection Field
        // value (from `T.fields[i]`), the member access is comptime-
        // only — there's no runtime layout for Field. Eager-fold here
        // so codegen sees a concrete literal (`return 8;` rather than
        // `return CapEntry.fields[1].offset;`). The fold may still
        // fail for an unsizeable struct, in which case codegen would
        // see the raw spelling — but sema also wouldn't have typed
        // the member as a useful Int anyway.
        if (lookup_base->nominal_decl() != nullptr
            && lookup_base->nominal_decl() == builtin_field_decl_.get()) {
            if (auto folded = folder_.fold(m, comptime_env_)) {
                resolution_.set_folded_value(&m, *folded);
            }
        }
        return finish(field_type);
    }
    // Method on a struct/enum.
    if (auto method_type = lookup_method(lookup_base, m.member)) {
        return finish(method_type);
    }
    error_at(m.range,
             std::format("no field or method '{}' on type {}", m.member, lookup_base->describe()));
    return types_->error();
}

TypePtr Resolver::check_leading_dot(const ast::LeadingDotExpr& d, TypePtr expected) {
    // `.foo` only makes sense when there is a contextual type that can
    // resolve the case. Today that's an enum case, an Optional, or — via
    // §12.1 phase 4 — a vector `.zero` initializer. The folder treats
    // `.zero` against a Vector as a zero-filled vector value; sema's job
    // here is just to give the expression the right *type* so the
    // surrounding let/var's annotation check succeeds.
    if (expected != nullptr && expected->kind() == TypeKind::Vector && d.name == "zero") {
        return expected;
    }
    if (expected == nullptr || expected->kind() != TypeKind::Enum
        || expected->nominal_decl() == nullptr) {
        error_at(d.range, std::format("'.{}' has no contextual type to resolve against", d.name));
        return types_->error();
    }
    const auto& enum_decl = static_cast<const ast::EnumDecl&>(*expected->nominal_decl());
    if (const auto* c = lookup_enum_case(enum_decl, d.name)) {
        return enum_case_constructor_type(expected, *c);
    }
    error_at(d.range, std::format("enum '{}' has no case '{}'", enum_decl.name, d.name));
    return types_->error();
}

}  // namespace vestra::sema
