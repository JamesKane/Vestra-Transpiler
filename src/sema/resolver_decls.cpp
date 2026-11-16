// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// Declaration + statement checking: per-decl dispatch, link-attribute
// validation, function bodies, blocks, and statements.
// Split out of resolver.cpp; see resolver_internal.hpp for the shared
// free helpers.

#include "vestra/ast/nodes.hpp"
#include "vestra/sema/builtins.hpp"
#include "vestra/sema/asm_writes.hpp"
#include "vestra/sema/resolver.hpp"
#include "vestra/sema/scope.hpp"
#include "vestra/sema/types.hpp"

#include "resolver_internal.hpp"

#include <algorithm>
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
using detail::struct_is_repr_union;

void Resolver::check_decl(const ast::Decl& d) {
    switch (d.kind) {
    case ast::NodeKind::Func:
        check_func(static_cast<const ast::FuncDecl&>(d));
        break;
    case ast::NodeKind::Const: {
        const auto& c = static_cast<const ast::ConstDecl&>(d);
        const auto* sym = scopes_.global().lookup_local(c.name);
        TypePtr annotated = sym != nullptr ? sym->type : nullptr;
        if (c.value) {
            auto value_type = check_expr(*c.value, annotated);
            // §4 a const is a comptime context: an interpolation whose every
            // fragment and value is known there folds to a `StrConst`, so it
            // allocates nothing.
            if (c.value->kind == ast::NodeKind::InterpStringExpr) {
                if (auto folded = folder_.fold(*c.value, comptime_env_, TypeKind::StrConst);
                    folded && folded->kind == ComptimeValue::Kind::String) {
                    value_type = types_->primitive(TypeKind::StrConst);
                    resolution_.set_type(c.value.get(), value_type);
                }
            }
            if (annotated != nullptr && !TypeArena::assignable(value_type, annotated)) {
                error_at(c.value->range,
                         std::format("const value of type {} does not match annotation {}",
                                     value_type ? value_type->describe() : "?",
                                     annotated->describe()));
            }
            note_string_coercion(*c.value, value_type, annotated);
            // §12.1 fold attempt: if the initializer is a pure constant
            // expression, evaluate it now and record the result. The hint is
            // the annotated TypeKind so an integer literal folds to the
            // declared concrete width (e.g. const X: Int32 = 1 << 8 - 1).
            TypeKind hint = annotated != nullptr ? annotated->kind() : TypeKind::Unit;
            if (auto folded = folder_.fold(*c.value, comptime_env_, hint)) {
                resolution_.set_folded_value(c.value.get(), *folded);
                comptime_env_[c.name] = *folded;
            }
        }
        break;
    }
    case ast::NodeKind::Static: {
        const auto& s = static_cast<const ast::StaticDecl&>(d);
        const auto* sym = scopes_.global().lookup_local(s.name);
        TypePtr annotated = sym != nullptr ? sym->type : nullptr;
        if (s.value) {
            auto value_type = check_expr(*s.value, annotated);
            if (annotated != nullptr && !TypeArena::assignable(value_type, annotated)) {
                error_at(s.value->range,
                         std::format("static value of type {} does not match annotation {}",
                                     value_type ? value_type->describe() : "?",
                                     annotated->describe()));
            }
            note_string_coercion(*s.value, value_type, annotated);
        }
        // §A1 (§4.5): a `@noinit static` reserves uninitialized storage
        // in .bss and *must* have a type annotation (sema can't infer
        // the size of a slot with no initializer). It also can't carry
        // an initializer — the parser already rejects that, but we
        // double-check here in case future parser changes thread a
        // value through anyway.
        if (s.noinit) {
            if (s.type == nullptr) {
                error_at(s.range, "@noinit static requires a type annotation");
            }
            if (s.value) {
                error_at(s.value->range,
                         "@noinit static cannot carry an initializer (its storage lands in .bss)");
            }
            // Annex §4.5 the storage is uninitialized bytes, so it may only
            // hold a Trivial (resource-free, non-linear) type.
            if (s.type != nullptr) {
                if (const auto* nsym = scopes_.current().lookup(s.name)) {
                    if (auto blocker = trivial_blocker(nsym->type); blocker != nullptr) {
                        error_at(s.type->range,
                                 std::format("@noinit static '{}' must have a Trivial type "
                                             "(Annex §4.5); {}",
                                             s.name,
                                             trivial_clause(nsym->type, blocker)));
                    }
                }
            }
        }
        check_link_attributes(s.attributes);
        break;
    }
    case ast::NodeKind::Struct: {
        // Resolve member function bodies, if any. Field types were already
        // resolved on demand by callers; we don't yet type-check defaults.
        const auto& s = static_cast<const ast::StructDecl&>(d);
        // §7 generics phase 2 — bind the struct's type parameters as opaque
        // GenericParam placeholders for the duration of decl checking so a
        // method signature referencing `T` resolves rather than failing
        // with "unknown type". For a non-generic struct this pushes an
        // empty scope and is a no-op.
        ScopeStack::Guard struct_generics_g(scopes_);
        for (const auto& gp : s.generics) {
            if (gp.name.empty()) {
                continue;
            }
            Symbol gs;
            gs.name = gp.name;
            gs.kind = SymbolKind::GenericParam;
            gs.type = types_->make_generic_param(gp.name);
            gs.definition_range = gp.range;
            gs.generic = &gp;
            (void)struct_generics_g.scope().insert(std::move(gs));
        }
        {
            const auto prev_self = current_self_type_;
            current_self_type_ = self_type_of(s, s.name, s.generics);
            for (const auto& m : s.methods) {
                if (m->kind == ast::NodeKind::Func) {
                    check_func(static_cast<const ast::FuncDecl&>(*m));
                }
            }
            current_self_type_ = prev_self;
        }
        // §11.1 a Future can't outlive the scope that spawned it, so no
        // struct stores one.
        for (const auto& f : s.fields) {
            if (f.type != nullptr && f.type->kind == ast::NodeKind::NamedType) {
                const auto& n = static_cast<const ast::NamedType&>(*f.type);
                if (n.path.size() == 1 && n.path[0] == "Future") {
                    error_at(f.type->range,
                             std::format("field '{}' would store a Future, which can't outlive "
                                         "the scope that spawned it",
                                         f.name));
                }
            }
        }
        // §A6 (§6.8 / §14.11.4) `@repr(union)` validation. The struct
        // is re-emitted as a C++ union — its members overlay one
        // storage cell. v0.5 enforces two narrow rules:
        //   (1) at least one field (else the C++ union body is empty
        //       and the type is useless).
        //   (2) `@bits(N)` on a direct member is rejected. Bit-fields
        //       only make sense inside a packed sub-struct, which then
        //       overlays the integer cell. A bit-field on the union
        //       itself can't share storage with the integer view, so
        //       the spec's register-decoding pattern would silently
        //       break.
        // The spec also requires transitively Trivial fields, checked
        // below with the @noinit predicate.
        if (struct_is_repr_union(s)) {
            if (s.fields.empty()) {
                error_at(s.range,
                         std::format("@repr(union) struct '{}' must declare at least one field",
                                     s.name));
            }
            for (const auto& f : s.fields) {
                for (const auto& a : f.attributes) {
                    if (a.name == "bits") {
                        error_at(a.range,
                                 std::format("@bits on a direct member of @repr(union) '{}' is "
                                             "rejected; put bit-fields inside a packed sub-struct "
                                             "and overlay it",
                                             s.name));
                    }
                }
            }
            // Annex §6.8 the fields overlay one untagged cell, so each must
            // be Trivial (no resources, no views, nothing linear).
            for (const auto& f : s.fields) {
                if (f.type == nullptr) {
                    continue;
                }
                auto ft = resolve_type(*f.type);
                if (auto blocker = trivial_blocker(ft); blocker != nullptr) {
                    error_at(f.type->range,
                             std::format("@repr(union) field '{}' must have a Trivial type "
                                         "(Annex §6.8); {}",
                                         f.name,
                                         trivial_clause(ft, blocker)));
                }
            }
        }
        break;
    }
    case ast::NodeKind::Enum: {
        const auto& e = static_cast<const ast::EnumDecl&>(d);
        // §7 generics phase 2 — bind the enum's type parameters as opaque
        // placeholders for the duration of decl checking so a method
        // signature referencing `T` resolves. No-op for a non-generic enum.
        ScopeStack::Guard enum_generics_g(scopes_);
        for (const auto& gp : e.generics) {
            if (gp.name.empty()) {
                continue;
            }
            Symbol gs;
            gs.name = gp.name;
            gs.kind = SymbolKind::GenericParam;
            gs.type = types_->make_generic_param(gp.name);
            gs.definition_range = gp.range;
            gs.generic = &gp;
            (void)enum_generics_g.scope().insert(std::move(gs));
        }
        {
            const auto prev_self = current_self_type_;
            current_self_type_ = self_type_of(e, e.name, e.generics);
            for (const auto& m : e.methods) {
                if (m->kind == ast::NodeKind::Func) {
                    check_func(static_cast<const ast::FuncDecl&>(*m));
                }
            }
            current_self_type_ = prev_self;
        }
        break;
    }
    case ast::NodeKind::Extension: {
        const auto& e = static_cast<const ast::ExtensionDecl&>(d);
        for (const auto& m : e.members) {
            check_decl(*m);
        }
        break;
    }
    case ast::NodeKind::Derive: {
        // §12.3 derive(Default) validation: every field of the target
        // struct must itself be Default-conformant. The other derives
        // (Eq / Hash / Clone / Debug / Display) defer field-conformance
        // to the C++ compiler since the underlying op is defaulted /
        // member-wise; Default is the one where the spec is explicit
        // about field-level checking.
        const auto& dd = static_cast<const ast::DeriveDecl&>(d);
        // §A6 (§6.8 / §14.11.4) `@repr(union)` is an untagged overlay
        // — the compiler can't pick which member to compare or hash
        // structurally, so the auto-derives don't apply. Reject any
        // derive targeting a union struct with a clear message rather
        // than letting the C++ compiler fail on `operator== = default`
        // for a union type.
        if (dd.target != nullptr && dd.target->kind == ast::NodeKind::NamedType) {
            const auto& tt = static_cast<const ast::NamedType&>(*dd.target);
            if (!tt.path.empty()) {
                const auto* tsym = scopes_.global().lookup_local(tt.path.back());
                if (tsym != nullptr && tsym->kind == SymbolKind::Struct && tsym->decl != nullptr) {
                    const auto& tsd = static_cast<const ast::StructDecl&>(*tsym->decl);
                    if (struct_is_repr_union(tsd)) {
                        error_at(dd.target->range,
                                 std::format("derive on @repr(union) struct '{}' is rejected; "
                                             "untagged overlays don't have a structural "
                                             "comparison or hash",
                                             tsd.name));
                        break;
                    }
                }
            }
        }
        check_derive_fields(dd);
        bool derives_default = false;
        for (const auto& proto : dd.protocols) {
            if (proto == nullptr || proto->kind != ast::NodeKind::NamedType) {
                continue;
            }
            const auto& nt = static_cast<const ast::NamedType&>(*proto);
            if (!nt.path.empty() && nt.path.back() == "Default") {
                derives_default = true;
                break;
            }
        }
        if (!derives_default || dd.target == nullptr) {
            break;
        }
        // §3 follow-on: an opaque newtype is admissible as a
        // derive(Default) target. The check is "is the underlying
        // type Default-conformant" — the opaque's `enum class T :
        // Underlying {}` C++ form gives `T()` = `T{0}` automatically,
        // so no codegen change is needed.
        if (dd.target->kind == ast::NodeKind::NamedType) {
            const auto& tt = static_cast<const ast::NamedType&>(*dd.target);
            if (!tt.path.empty()) {
                const auto* osym = scopes_.global().lookup_local(tt.path.back());
                if (osym != nullptr && osym->kind == SymbolKind::OpaqueType
                    && osym->decl != nullptr) {
                    const auto& od = static_cast<const ast::OpaqueDecl&>(*osym->decl);
                    if (od.underlying != nullptr) {
                        auto under = resolve_type(*od.underlying);
                        if (!is_default_conformant(under)) {
                            error_at(dd.target->range,
                                     std::format("derive(Default) for opaque '{}': underlying "
                                                 "type {} is not Default-conformant",
                                                 od.name,
                                                 under != nullptr ? under->describe() : "?"));
                        }
                    }
                    break;
                }
            }
        }
        // Resolve the target into a Struct decl. Anything else (enum,
        // protocol) is rejected with a v0.5 diagnostic.
        if (dd.target->kind != ast::NodeKind::NamedType) {
            error_at(dd.target->range, "derive(Default) target must be a struct");
            break;
        }
        const auto& tt = static_cast<const ast::NamedType&>(*dd.target);
        if (tt.path.empty()) {
            break;
        }
        const auto* sym = scopes_.global().lookup_local(tt.path.back());
        if (sym == nullptr || sym->kind != SymbolKind::Struct || sym->decl == nullptr) {
            error_at(
                dd.target->range,
                std::format("derive(Default) for '{}' requires a struct target", tt.path.back()));
            break;
        }
        const auto& sd = static_cast<const ast::StructDecl&>(*sym->decl);
        // §7 generics phase 2 — bind the target struct's type parameters as
        // placeholders so a field typed `T` resolves to a GenericParam
        // (deferred-conformant) rather than failing with "unknown type T".
        ScopeStack::Guard derive_generics_g(scopes_);
        for (const auto& gp : sd.generics) {
            if (gp.name.empty()) {
                continue;
            }
            Symbol gs;
            gs.name = gp.name;
            gs.kind = SymbolKind::GenericParam;
            gs.type = types_->make_generic_param(gp.name);
            gs.definition_range = gp.range;
            gs.generic = &gp;
            (void)derive_generics_g.scope().insert(std::move(gs));
        }
        for (const auto& f : sd.fields) {
            if (f.kind == ast::StructDecl::Field::Kind::Embed) {
                continue;  // embed fields are checked through the embedded struct's own derive
            }
            if (f.type == nullptr) {
                continue;
            }
            TypePtr ft = resolve_type(*f.type);
            if (!is_default_conformant(ft)) {
                error_at(f.range,
                         std::format("derive(Default) for '{}': field '{}' of type {} is not "
                                     "Default-conformant",
                                     sd.name,
                                     f.name,
                                     ft != nullptr ? ft->describe() : "?"));
            }
        }
        break;
    }
    default:
        break;
    }
}

// §12.3 a structural derive (Eq, Hash, Clone, Debug, Display) needs each field
// (and each enum payload) to conform. A field typed by the target's own
// generic parameter is deferred to the instantiation, as Default's check is.
void Resolver::check_derive_fields(const ast::DeriveDecl& dd) {
    if (dd.target == nullptr || dd.target->kind != ast::NodeKind::NamedType) {
        return;
    }
    const auto& tt = static_cast<const ast::NamedType&>(*dd.target);
    if (tt.path.empty()) {
        return;
    }
    const auto* sym = scopes_.global().lookup_local(tt.path.back());
    if (sym == nullptr || sym->decl == nullptr) {
        return;
    }
    // §18.1 `protocol Ord: Eq`: an ordering is adopted alongside equality.
    for (const auto& proto : dd.protocols) {
        if (proto != nullptr && proto->kind == ast::NodeKind::NamedType
            && static_cast<const ast::NamedType&>(*proto).path.size() == 1
            && static_cast<const ast::NamedType&>(*proto).path.front() == "Ord"
            && !decl_derives(static_cast<const ast::Decl*>(sym->decl), "Eq")) {
            error_at(proto->range,
                     std::format("derive(Ord) for '{}' needs derive(Eq) too: Ord refines Eq",
                                 tt.path.back()));
        }
    }
    const std::vector<ast::GenericParam>* generics = nullptr;
    if (sym->kind == SymbolKind::Struct) {
        generics = &static_cast<const ast::StructDecl&>(*sym->decl).generics;
    } else if (sym->kind == SymbolKind::Enum) {
        generics = &static_cast<const ast::EnumDecl&>(*sym->decl).generics;
    } else {
        return;
    }
    ScopeStack::Guard g(scopes_);
    for (const auto& gp : *generics) {
        if (gp.name.empty()) {
            continue;
        }
        Symbol gs;
        gs.name = gp.name;
        gs.kind = SymbolKind::GenericParam;
        gs.type = types_->make_generic_param(gp.name);
        gs.definition_range = gp.range;
        gs.generic = &gp;
        (void)g.scope().insert(std::move(gs));
    }
    for (const auto& proto : dd.protocols) {
        if (proto == nullptr || proto->kind != ast::NodeKind::NamedType) {
            continue;
        }
        const auto& pn = static_cast<const ast::NamedType&>(*proto);
        if (pn.path.size() != 1) {
            continue;
        }
        const std::string& p = pn.path.front();
        const bool display = p == "Debug" || p == "Display";
        if (!display && p != "Eq" && p != "Ord" && p != "Hash" && p != "Clone") {
            continue;
        }
        auto conforms = [&](TypePtr ft) {
            if (ft == nullptr || ft->is_error() || ft->kind() == TypeKind::GenericParam) {
                return true;
            }
            return display ? is_display_conformant(ft) : type_satisfies_bound(ft, p);
        };
        if (sym->kind == SymbolKind::Struct) {
            const auto& sd = static_cast<const ast::StructDecl&>(*sym->decl);
            for (const auto& f : sd.fields) {
                if (f.kind == ast::StructDecl::Field::Kind::Embed || f.type == nullptr) {
                    continue;
                }
                auto ft = resolve_type(*f.type);
                if (!conforms(ft)) {
                    error_at(f.range,
                             std::format("derive({}) for '{}': field '{}' of type {} is not "
                                         "{}-conformant",
                                         p,
                                         sd.name,
                                         f.name,
                                         ft->describe(),
                                         p));
                }
            }
        } else {
            const auto& ed = static_cast<const ast::EnumDecl&>(*sym->decl);
            for (const auto& c : ed.cases) {
                for (const auto& [n, ty] : c.payload) {
                    if (ty == nullptr) {
                        continue;
                    }
                    auto ft = resolve_type(*ty);
                    if (!conforms(ft)) {
                        error_at(ty->range,
                                 std::format("derive({}) for '{}': case '{}' carries {}, which "
                                             "is not {}-conformant",
                                             p,
                                             ed.name,
                                             c.name,
                                             ft->describe(),
                                             p));
                    }
                }
            }
        }
    }
}

namespace {

// `X.compareExchangeWeak(…)`, or nullptr.
const ast::Expr* weak_cas_call(const ast::Expr* e) {
    if (e == nullptr || e->kind != ast::NodeKind::CallExpr) {
        return nullptr;
    }
    const auto& c = static_cast<const ast::CallExpr&>(*e);
    if (c.callee == nullptr || c.callee->kind != ast::NodeKind::MemberExpr
        || static_cast<const ast::MemberExpr&>(*c.callee).member != "compareExchangeWeak") {
        return nullptr;
    }
    return e;
}

// `NAME.succeeded`?
bool is_succeeded_of(const ast::Expr* e, std::string_view name) {
    if (e == nullptr || e->kind != ast::NodeKind::MemberExpr) {
        return false;
    }
    const auto& m = static_cast<const ast::MemberExpr&>(*e);
    return m.member == "succeeded" && m.base != nullptr && m.base->kind == ast::NodeKind::IdentExpr
           && static_cast<const ast::IdentExpr&>(*m.base).name == name;
}

// `if NAME.succeeded { … break | return … }` (no else).
bool is_exit_on_success(const ast::Stmt& s, std::string_view name) {
    if (s.kind != ast::NodeKind::ExprStmt) {
        return false;
    }
    const auto* e = static_cast<const ast::ExprStmt&>(s).expr.get();
    if (e == nullptr || e->kind != ast::NodeKind::IfExpr) {
        return false;
    }
    const auto& i = static_cast<const ast::IfExpr&>(*e);
    if (!i.let_name.empty() || i.else_branch != nullptr || !is_succeeded_of(i.cond.get(), name)
        || i.then_branch == nullptr || i.then_branch->kind != ast::NodeKind::BlockExpr) {
        return false;
    }
    return std::ranges::any_of(static_cast<const ast::BlockExpr&>(*i.then_branch).stmts,
                               [](const ast::StmtPtr& t) {
                                   return t != nullptr
                                          && (t->kind == ast::NodeKind::BreakStmt
                                              || t->kind == ast::NodeKind::ReturnStmt);
                               });
}

}  // namespace

// Annex §14.9.3 a function may carry `@retry_loop` iff (1) it is
// `@inline(.always)`, (2) its body holds exactly one compareExchangeWeak
// site, and (3) that site is the weak CAS of an admitted retry-loop shape.
// A violation is rejected at the definition, naming the clause.
void Resolver::check_retry_loop(const ast::FuncDecl& f, int sites, int loop_sites) {
    const auto* attr = static_cast<const ast::Attribute*>(nullptr);
    for (const auto& a : f.attributes) {
        if (a.name == "retry_loop") {
            attr = &a;
        }
    }
    if (attr == nullptr) {
        return;
    }
    const bool always = std::ranges::any_of(f.attributes, [](const ast::Attribute& a) {
        return a.name == "inline" && a.predicate != nullptr
               && a.predicate->kind == ast::NodeKind::LeadingDotExpr
               && static_cast<const ast::LeadingDotExpr&>(*a.predicate).name == "always";
    });
    if (!always) {
        error_at(attr->range,
                 std::format("@retry_loop function '{}' must be @inline(.always) (Annex §14.9.3 "
                             "clause 1): its body has to reach the call site",
                             f.name));
    }
    if (sites != 1) {
        error_at(attr->range,
                 std::format("@retry_loop function '{}' has {} compareExchangeWeak sites, not "
                             "exactly one (Annex §14.9.3 clause 2)",
                             f.name,
                             sites));
    } else if (loop_sites != 1) {
        error_at(attr->range,
                 std::format("@retry_loop function '{}': its compareExchangeWeak is not the CAS "
                             "of an admitted retry loop (Annex §14.9.3 clause 3)",
                             f.name));
    }
}

const ast::Expr* Resolver::weak_cas_of_loop(const ast::WhileStmt& w) {
    if (w.cond == nullptr || w.body == nullptr || w.body->kind != ast::NodeKind::BlockExpr) {
        return nullptr;
    }
    const auto& stmts = static_cast<const ast::BlockExpr&>(*w.body).stmts;
    // `while !r.succeeded { … r = x.compareExchangeWeak(…) … }`
    if (w.cond->kind == ast::NodeKind::UnaryExpr) {
        const auto& u = static_cast<const ast::UnaryExpr&>(*w.cond);
        if (u.op != ast::UnaryOp::Not || u.operand == nullptr
            || u.operand->kind != ast::NodeKind::MemberExpr) {
            return nullptr;
        }
        const auto& m = static_cast<const ast::MemberExpr&>(*u.operand);
        if (m.member != "succeeded" || m.base == nullptr
            || m.base->kind != ast::NodeKind::IdentExpr) {
            return nullptr;
        }
        const auto& name = static_cast<const ast::IdentExpr&>(*m.base).name;
        for (const auto& s : stmts) {
            if (s == nullptr || s->kind != ast::NodeKind::AssignStmt) {
                continue;
            }
            const auto& a = static_cast<const ast::AssignStmt&>(*s);
            if (a.op == ast::AssignOp::Assign && a.target != nullptr
                && a.target->kind == ast::NodeKind::IdentExpr
                && static_cast<const ast::IdentExpr&>(*a.target).name == name) {
                if (const auto* call = weak_cas_call(a.value.get())) {
                    return call;
                }
            }
        }
        return nullptr;
    }
    // `while true { … let r = x.compareExchangeWeak(…) … if r.succeeded { break } … }`
    // (the `repeat` form admits only the `!r.succeeded` shape above).
    if (w.repeat || w.cond->kind != ast::NodeKind::BoolLit || !static_cast<const ast::BoolLit&>(*w.cond).value) {
        return nullptr;
    }
    for (std::size_t k = 0; k < stmts.size(); ++k) {
        const auto* s = stmts[k].get();
        if (s == nullptr || s->kind != ast::NodeKind::LetStmt) {
            continue;
        }
        const auto& l = static_cast<const ast::LetStmt&>(*s);
        const auto* call = weak_cas_call(l.value.get());
        if (call == nullptr || l.pattern == nullptr || l.pattern->kind != ast::NodeKind::IdentPat) {
            continue;
        }
        const auto& name = static_cast<const ast::IdentPat&>(*l.pattern).name;
        for (std::size_t q = k + 1; q < stmts.size(); ++q) {
            if (stmts[q] != nullptr && is_exit_on_success(*stmts[q], name)) {
                return call;
            }
        }
    }
    return nullptr;
}

void Resolver::check_link_attributes(const std::vector<ast::Attribute>& attrs) {
    // §A1 (§6.7): shape-check the symbol-level attributes. We accept
    // the common case for each — string literal for
    // @section/@symbol/@alias; bare for @weak/@noinit; an anonymous
    // member-access (`.default` / `.hidden` / `.protected`) for
    // @visibility — and emit a clear diagnostic otherwise. No type or
    // effect checks here: these are link-time data and don't
    // influence Vestra-side types.
    for (const auto& a : attrs) {
        if (a.name == "section" || a.name == "symbol" || a.name == "alias") {
            if (a.predicate == nullptr || a.predicate->kind != ast::NodeKind::StringLit) {
                error_at(a.range, std::format("@{} expects a string literal argument", a.name));
            }
        } else if (a.name == "extern") {
            // §A12 (§14.6.1) `@extern("conv")` accepts any calling
            // convention the target backend names. The set is
            // target-determined; the spec calls out a baseline
            // every supported target carries — sema validates
            // against it, codegen emits the matching gnu::*-abi
            // attribute or `extern "C"` linkage where available.
            if (a.predicate == nullptr || a.predicate->kind != ast::NodeKind::StringLit) {
                error_at(a.range, "@extern expects a string literal argument naming the conv");
            } else {
                std::string_view conv = static_cast<const ast::StringLit&>(*a.predicate).text;
                static constexpr std::array<std::string_view, 9> known_convs = {
                    "C",
                    "hvc64",
                    "smc64",
                    "sysv64",
                    "win64",
                    "ms_abi",
                    "aapcs",
                    "riscv",
                    "interrupt",
                };
                bool ok = false;
                for (auto k : known_convs) {
                    if (conv == k) {
                        ok = true;
                        break;
                    }
                }
                if (!ok) {
                    error_at(a.range,
                             std::format("@extern(\"{}\") is not a recognized calling convention; "
                                         "expected one of: C, hvc64, smc64, sysv64, win64, "
                                         "ms_abi, aapcs, riscv, interrupt",
                                         conv));
                }
            }
        } else if (a.name == "weak" || a.name == "noinit") {
            if (a.predicate != nullptr) {
                error_at(a.range, std::format("@{} takes no arguments", a.name));
            }
        } else if (a.name == "inline") {
            // §A1 (§7.8) — `@inline(.always | .never | .hint)`. The
            // argument is a leading-dot case-name; sema enforces the
            // three legal spellings here so codegen can dispatch on
            // them without re-validating.
            std::string_view case_name;
            if (a.predicate != nullptr && a.predicate->kind == ast::NodeKind::LeadingDotExpr) {
                case_name = static_cast<const ast::LeadingDotExpr&>(*a.predicate).name;
            }
            if (case_name.empty()) {
                error_at(a.range, "@inline expects one of .always, .never, .hint");
            } else if (case_name != "always" && case_name != "never" && case_name != "hint") {
                error_at(
                    a.range,
                    std::format("@inline(.{}) — expected .always, .never, or .hint", case_name));
            }
        } else if (a.name == "visibility") {
            // The argument is `.default` / `.hidden` / `.protected` —
            // an anonymous case name. The parser produces a
            // LeadingDotExpr here (not MemberExpr — there's no base).
            std::string_view case_name;
            if (a.predicate != nullptr && a.predicate->kind == ast::NodeKind::LeadingDotExpr) {
                case_name = static_cast<const ast::LeadingDotExpr&>(*a.predicate).name;
            }
            if (case_name.empty()) {
                error_at(a.range, "@visibility expects one of .default, .hidden, .protected");
            } else if (case_name != "default" && case_name != "hidden"
                       && case_name != "protected") {
                error_at(a.range,
                         std::format("@visibility(.{}) — expected .default, .hidden, "
                                     "or .protected",
                                     case_name));
            }
        } else if (a.name == "stack_protector") {
            // §A10 (§15.4) `@stack_protector(.none | .strong | .all)`
            // selects canary instrumentation on a per-function basis.
            // The leading-dot case-name pattern matches @inline /
            // @visibility; the legal spellings come straight from the
            // spec.
            std::string_view case_name;
            if (a.predicate != nullptr && a.predicate->kind == ast::NodeKind::LeadingDotExpr) {
                case_name = static_cast<const ast::LeadingDotExpr&>(*a.predicate).name;
            }
            if (case_name.empty()) {
                error_at(a.range, "@stack_protector expects one of .none, .strong, .all");
            } else if (case_name != "none" && case_name != "strong" && case_name != "all") {
                error_at(a.range,
                         std::format("@stack_protector(.{}) — expected .none, .strong, or .all",
                                     case_name));
            }
        }
    }
}

void Resolver::check_func(const ast::FuncDecl& f) {
    check_link_attributes(f.attributes);

    // §A10 (§15.5) `@panic_handler` shape rules. Exactly one per
    // unit; signature must be `(message: Str, file: StrConst, line:
    // Int) -> Never`; the `using` row must be empty (the handler
    // runs in a context where the program's capability bindings
    // can't be assumed live). The "exactly one per binary" cross-
    // TU rule is the linker's concern; sema enforces only the
    // per-unit count.
    for (const auto& a : f.attributes) {
        if (a.name != "panic_handler") {
            continue;
        }
        if (panic_handler_decl_ != nullptr && panic_handler_decl_ != &f) {
            auto d = diag::Diagnostic::error("duplicate @panic_handler in this unit").at(a.range);
            if (panic_handler_decl_->range.is_valid()) {
                d.with_note(diag::Diagnostic::note("previous @panic_handler here")
                                .at(panic_handler_decl_->range));
            }
            reporter_->report(std::move(d));
        }
        panic_handler_decl_ = &f;
        // Signature check: 3 params, (Str, StrConst, Int).
        const bool params_ok =
            f.params.size() == 3 && f.params[0].type != nullptr
            && resolve_type(*f.params[0].type) == types_->primitive(TypeKind::Str)
            && f.params[1].type != nullptr
            && resolve_type(*f.params[1].type) == types_->primitive(TypeKind::StrConst)
            && f.params[2].type != nullptr
            && resolve_type(*f.params[2].type) == types_->primitive(TypeKind::Int);
        if (!params_ok) {
            error_at(a.range,
                     "@panic_handler signature must be `(message: Str, file: StrConst, line: "
                     "Int) -> Never`");
        }
        // Return type: must be Never. Never isn't spellable directly
        // in source but the resolver type-checks each `panic` /
        // `throw` body shape; here we approximate by requiring an
        // explicit `-> Never` annotation. v0.5: accept any result
        // type that resolves to Never; sema would otherwise need
        // body-level falls-off-the-end checking which is out of scope.
        if (f.result == nullptr || !resolve_type(*f.result)->is_never()) {
            error_at(a.range,
                     "@panic_handler return type must be `Never` (no falling off the end)");
        }
        if (!f.effects.using_caps.empty()) {
            error_at(a.range,
                     "@panic_handler cannot declare a `using` row — the handler runs in a "
                     "context where the program's capability bindings can't be assumed live");
        }
    }

    // §A8 (§14.5.2) `@interrupt` shape rules. Validated against the
    // declared signature regardless of whether the body is present,
    // since the rules are about the surface kernels write at ISRs:
    //   * first parameter must be `inout T` (the trap frame is what
    //     `eret`/`iretq` write back to);
    //   * return type must be Unit — ISRs can't fail or yield a value;
    //   * the using-row must not include `Alloc`, `Async`, or
    //     `Extern` — those don't make sense inside an interrupt
    //     handler (no heap, no awaiting, no FFI back to userland).
    // The attribute-argument shape (`class:` / `fromEL1:`) is left
    // free in v0.5; later phases will parse the labeled-arg dict.
    for (const auto& a : f.attributes) {
        if (a.name != "interrupt") {
            continue;
        }
        if (f.params.empty()
            || (f.params.size() >= 1 && f.params[0].mode != ast::ParamMode::Inout)) {
            error_at(a.range,
                     "@interrupt requires an `inout TrapFrame` first parameter — the prologue / "
                     "epilogue mutate it to write back the saved registers");
        }
        if (f.result != nullptr) {
            error_at(a.range,
                     "@interrupt functions return Unit (the body's effect is the mutation of "
                     "the trap-frame parameter; eret / iretq restores from there)");
        }
        for (const auto& cap : f.effects.using_caps) {
            if (cap == nullptr) {
                continue;
            }
            std::string name;
            if (cap->kind == ast::NodeKind::NamedType) {
                const auto& nt = static_cast<const ast::NamedType&>(*cap);
                if (!nt.path.empty()) {
                    name = nt.path.back();
                }
            }
            if (name == "Alloc" || name == "Async" || name == "Extern") {
                error_at(a.range,
                         std::format("@interrupt cannot declare `using {}` — interrupt handlers "
                                     "run with the kernel's own discipline (no heap, no awaits, "
                                     "no FFI back across the boundary)",
                                     name));
            }
        }
    }

    // §A9 (§14.7) `@boot` shape rules. Validated independently of
    // body presence — boot stubs declared as forward references stay
    // restricted regardless. v0.5 enforces three of the seven rules
    // here (the others are call-graph / audit-time):
    //   * `using` row restricted to {Asm, RawMemory, Mmio}. Other
    //     caps imply runtime services boot can't depend on.
    //   * No throws / Result. The boot regime has no unwinder.
    //   * @naked is implied — recorded on the attr, applied at
    //     codegen time as the C++ `[[gnu::naked]]` prefix.
    for (const auto& a : f.attributes) {
        if (a.name != "boot") {
            continue;
        }
        for (const auto& cap : f.effects.using_caps) {
            if (cap == nullptr || cap->kind != ast::NodeKind::NamedType) {
                continue;
            }
            const auto& nt = static_cast<const ast::NamedType&>(*cap);
            if (nt.path.empty()) {
                continue;
            }
            const auto& name = nt.path.back();
            if (name != "Asm" && name != "RawMemory" && name != "Mmio") {
                error_at(a.range,
                         std::format("@boot cannot declare `using {}` — the boot regime is "
                                     "limited to Asm / RawMemory / Mmio (pre-MMU code has no "
                                     "runtime services)",
                                     name));
            }
        }
        if (f.effects.throws_type != nullptr) {
            error_at(a.range,
                     "@boot cannot declare `throws(E)` — the boot regime has no unwinder, no "
                     "Result type-layer, and no panic path that could carry the error");
        }
    }

    // §A9 (§14.7.1) `@kernel_init` shape rules. v0.5 enforces:
    //   * No `using Async`. The scheduler hasn't dispatched yet —
    //     there's no other thread to synchronise with.
    // The other four rules (call-graph reachability, allocator
    // initialiser ordering, ordinary-calls-kernel_init rejection)
    // are audit-time and queue for the call-graph pass.
    bool is_kernel_init = false;
    for (const auto& a : f.attributes) {
        if (a.name != "kernel_init") {
            continue;
        }
        is_kernel_init = true;
        for (const auto& cap : f.effects.using_caps) {
            if (cap == nullptr || cap->kind != ast::NodeKind::NamedType) {
                continue;
            }
            const auto& nt = static_cast<const ast::NamedType&>(*cap);
            if (!nt.path.empty() && nt.path.back() == "Async") {
                error_at(a.range,
                         "@kernel_init cannot declare `using Async` — the scheduler hasn't "
                         "dispatched a second task yet, so there's no other thread to "
                         "synchronise with");
            }
        }
    }

    // §A6 last (§14.11.5 / §14.12.3) `@no_auto_barrier` — the
    // function-level escape that disables the compiler-inserted
    // device-barrier / sysreg-post-write barrier auto-emission. The
    // spec admits this only inside a function also attributed
    // `@kernel_init`, where the kernel writes a batch of MMU control
    // sysregs whose final write subsumes the per-write ISB. Anywhere
    // else (ordinary code, @boot, @interrupt) the attribute rejects
    // with a clear diagnostic. v0.5 doesn't yet auto-emit barriers
    // for sysreg writes (the Sysreg namespace itself is queued), so
    // the codegen effect is just a marker for the audit; the
    // sema-side gate is what records the contract today.
    for (const auto& a : f.attributes) {
        if (a.name == "no_auto_barrier" && !is_kernel_init) {
            error_at(a.range,
                     "@no_auto_barrier is admitted only on functions also attributed "
                     "@kernel_init — that's the boot-path subset where a following sysreg "
                     "write subsumes the auto-emitted barrier");
        }
    }

    // §12.1 comptime is pure: no environment, clock, network or unsafe
    // capability, so a `comptime func` declares none.
    if (f.is_comptime) {
        for (const auto& cap : f.effects.using_caps) {
            if (cap != nullptr && cap->kind == ast::NodeKind::NamedType
                && !static_cast<const ast::NamedType&>(*cap).path.empty()) {
                error_at(cap->range,
                         std::format("comptime function '{}' can't use the capability '{}'; "
                                     "comptime code is pure",
                                     f.name,
                                     static_cast<const ast::NamedType&>(*cap).path.back()));
            }
        }
    }

    if (!f.body) {
        return;  // protocol requirement / extern stub — nothing to check
    }

    ScopeStack::Guard g(scopes_);
    struct ComptimeRestore {
        int& depth;
        int prev;
        ~ComptimeRestore() { depth = prev; }
    } restore_comptime{comptime_depth_, comptime_depth_};
    comptime_depth_ = f.is_comptime ? 1 : 0;

    // Bind generic parameters as opaque types in the function's scope.
    for (const auto& gp : f.generics) {
        if (gp.name.empty()) {
            continue;  // a const generic is a value in the body (see IdentExpr)
        }
        Symbol s;
        s.name = gp.name;
        s.kind = SymbolKind::GenericParam;
        s.type = types_->make_generic_param(gp.name);
        s.definition_range = gp.range;
        s.generic = &gp;
        if (auto* prev = g.scope().insert(std::move(s))) {
            duplicate_definition(*prev, gp.name, gp.range);
        }
    }

    check_param_defaults(f);
    // Bind parameters.
    for (const auto& p : f.params) {
        TypePtr param_type = p.type ? resolve_type(*p.type) : types_->error();
        // §6 tuple-pattern param: `func f((a, b): (Int32, Int32))` —
        // there's no single name to insert; instead each leaf of the
        // pattern binds to its corresponding tuple element.
        if (p.pattern != nullptr) {
            if (p.pattern->kind == ast::NodeKind::TuplePat) {
                bind_tuple_pattern(static_cast<const ast::TuplePat&>(*p.pattern), param_type);
            }
            continue;
        }
        if (p.name.empty()) {
            continue;
        }
        Symbol s;
        s.name = p.name;
        s.kind = SymbolKind::Parameter;
        // §7 a `read` parameter is an immutable borrow; `inout` and `sink` own
        // (or exclusively borrow) a value the body may change.
        s.is_mutable = p.mode == ast::ParamMode::Inout || p.mode == ast::ParamMode::Sink;
        s.nonescaping = p.nonescaping;
        s.type = param_type;
        s.definition_range = p.range;
        if (auto* prev = g.scope().insert(std::move(s))) {
            duplicate_definition(*prev, p.name, p.range);
        } else if (const auto* sym = g.scope().lookup_local(p.name)) {
            resolution_.add_param_symbol(&f, sym);
        }
    }

    TypePtr expected_result = f.result ? resolve_type(*f.result) : types_->unit();
    return_stack_.push_back(expected_result);
    // §9: separate stack tracks the enclosing throws(E). Push for every
    // function body so the depth matches return_stack_; nullptr means
    // "non-throwing", which is what check_throw / check_try look for.
    throws_stack_.push_back(f.effects.throws_type ? resolve_type(*f.effects.throws_type) : nullptr);
    // §9 bare-catch inference is lexically scoped to the current
    // function — a do-catch nested inside this function must not
    // capture a do-catch from the enclosing scope.
    auto saved_infer = std::move(do_catch_infer_stack_);
    do_catch_infer_stack_.clear();

    check_naked_body(f);
    // §8 a `using` row names capabilities: a builtin one, or a declared type.
    for (const auto& cap : f.effects.using_caps) {
        if (cap != nullptr && cap->kind == ast::NodeKind::NamedType) {
            const auto& nt = static_cast<const ast::NamedType&>(*cap);
            if (nt.path.size() == 1 && scopes_.current().lookup(nt.path[0]) == nullptr) {
                error_at(cap->range, std::format("unknown capability '{}'", nt.path[0]));
            }
            // Annex §14.13 rule 7: InterruptsOff is a region, not a capability.
            if (nt.path.size() == 1 && nt.path[0] == "InterruptsOff") {
                const auto* sym = scopes_.current().lookup(nt.path[0]);
                if (sym != nullptr && sym->decl == nullptr) {
                    error_at(cap->range,
                             "'InterruptsOff' is a region, not a capability: there is no "
                             "`using InterruptsOff`; open a `with InterruptsOff` block "
                             "(Annex §14.13 rule 7)");
                }
            }
        }
    }
    if (target_.freestanding) {
        for (const auto& cap : f.effects.using_caps) {
            if (cap != nullptr && cap->kind == ast::NodeKind::NamedType) {
                const auto& nt = static_cast<const ast::NamedType&>(*cap);
                if (!nt.path.empty()) {
                    reject_hosted_cap(nt.path.back(), cap->range);
                }
            }
        }
        if (f.is_async) {
            reject_hosted_cap("Async", f.range);
        }
    }
    const auto* prev_func = current_func_;
    current_func_ = &f;
    struct FuncRestore {
        Resolver* self;
        const ast::FuncDecl* prev;
        ~FuncRestore() { self->current_func_ = prev; }
    } restore_func{this, prev_func};

    // The body is always a BlockExpr; pass the expected result as the context.
    TypePtr body_type = types_->unit();
    const int sites_before = weak_cas_sites_;
    const int loop_sites_before = weak_cas_loop_sites_;
    if (f.body->kind == ast::NodeKind::BlockExpr) {
        body_type = check_block_expr(static_cast<const ast::BlockExpr&>(*f.body), expected_result);
    }
    check_retry_loop(f, weak_cas_sites_ - sites_before, weak_cas_loop_sites_ - loop_sites_before);

    // A void-returning function may have a non-Unit body — the final value is
    // dropped. A value-returning function must end with an expression of the
    // right type, OR the value must come back through a `return`. We can't
    // distinguish those cleanly without flow analysis, so we only flag the
    // case where the trailing expression is *present* and clearly wrong.
    if (expected_result != nullptr && expected_result->kind() != TypeKind::Unit
        && body_type != nullptr && !TypeArena::assignable(body_type, expected_result)
        && !body_type->is_never()) {
        // We tolerate the mismatch when the body is empty — the function
        // presumably relies on an explicit return we haven't flow-checked.
        if (body_type->kind() != TypeKind::Unit) {
            error_at(f.body->range,
                     std::format("function '{}' returns {} but body produces {}",
                                 f.name,
                                 expected_result->describe(),
                                 body_type->describe()));
        }
    }

    // §4 a `-> Never` function must not return at all: an explicit `return`
    // is a type error above, and falling off the end of the body is caught
    // here (F9). The C++ side is `[[noreturn]]`, so this is UB otherwise.
    // A @naked / @boot body is asm the compiler can't see through (its final
    // branch is how it leaves), so it isn't held to this.
    const bool naked_body = std::ranges::any_of(f.attributes, [](const ast::Attribute& at) {
        return at.name == "naked" || at.name == "boot";
    });
    if (expected_result != nullptr && expected_result->is_never() && !naked_body
        && !expr_diverges(*f.body)) {
        error_at(f.body->range,
                 std::format("function '{}' returns Never but its body can complete normally; "
                             "end it with a call to a Never function, `panic(...)`, or a loop "
                             "that doesn't exit",
                             f.name));
    }

    // §19 a value-returning function returns on every path: its body either
    // ends in a value of the result type or can't complete normally (F22).
    // Falling off the end is undefined behaviour in the C++ it lowers to.
    if (expected_result != nullptr && !expected_result->is_error()
        && expected_result->kind() != TypeKind::Unit && !expected_result->is_never() && !naked_body
        && f.body->kind == ast::NodeKind::BlockExpr) {
        const bool produces = body_type != nullptr && !body_type->is_error()
                              && body_type->kind() != TypeKind::Unit
                              && TypeArena::assignable(body_type, expected_result);
        if (!produces && !expr_diverges(*f.body)) {
            error_at(f.body->range,
                     std::format("function '{}' can reach the end of its body without returning "
                                 "its {} result; return on every path",
                                 f.name,
                                 expected_result->describe()));
        }
    }

    return_stack_.pop_back();
    throws_stack_.pop_back();
    do_catch_infer_stack_ = std::move(saved_infer);
}

namespace {

// True when `body` holds a `break` that leaves the loop it belongs to: an
// unlabeled break outside any nested loop, or a break naming `label`.
// Closures are their own control context and are not entered.
bool breaks_out(const ast::Node& n, std::string_view label, bool nested);

bool expr_breaks_out(const ast::Expr* e, std::string_view label, bool nested) {
    return e != nullptr && breaks_out(*e, label, nested);
}

bool breaks_out(const ast::Node& n, std::string_view label, bool nested) {
    switch (n.kind) {
    case ast::NodeKind::BreakStmt: {
        const auto& b = static_cast<const ast::BreakStmt&>(n);
        return b.label.empty() ? !nested : b.label == label;
    }
    case ast::NodeKind::BlockExpr:
        for (const auto& s : static_cast<const ast::BlockExpr&>(n).stmts) {
            if (breaks_out(*s, label, nested)) {
                return true;
            }
        }
        return false;
    case ast::NodeKind::ExprStmt:
        return expr_breaks_out(static_cast<const ast::ExprStmt&>(n).expr.get(), label, nested);
    case ast::NodeKind::IfExpr: {
        const auto& i = static_cast<const ast::IfExpr&>(n);
        return expr_breaks_out(i.then_branch.get(), label, nested)
               || expr_breaks_out(i.else_branch.get(), label, nested);
    }
    case ast::NodeKind::MatchExpr:
        for (const auto& a : static_cast<const ast::MatchExpr&>(n).arms) {
            if (expr_breaks_out(a.body.get(), label, nested)) {
                return true;
            }
        }
        return false;
    case ast::NodeKind::WhileStmt:
        return expr_breaks_out(static_cast<const ast::WhileStmt&>(n).body.get(), label, true);
    case ast::NodeKind::ForStmt:
        return expr_breaks_out(static_cast<const ast::ForStmt&>(n).body.get(), label, true);
    case ast::NodeKind::WithStmt:
        return expr_breaks_out(static_cast<const ast::WithStmt&>(n).body.get(), label, nested);
    default:
        return false;
    }
}

}  // namespace

bool Resolver::expr_diverges(const ast::Expr& e) const {
    if (auto t = resolution_.type_of(&e); t != nullptr && t->is_never()) {
        return true;
    }
    switch (e.kind) {
    case ast::NodeKind::BlockExpr:
        for (const auto& s : static_cast<const ast::BlockExpr&>(e).stmts) {
            if (stmt_diverges(*s)) {
                return true;
            }
        }
        return false;
    case ast::NodeKind::IfExpr: {
        const auto& i = static_cast<const ast::IfExpr&>(e);
        return i.then_branch != nullptr && i.else_branch != nullptr
               && expr_diverges(*i.then_branch) && expr_diverges(*i.else_branch);
    }
    case ast::NodeKind::MatchExpr: {
        const auto& m = static_cast<const ast::MatchExpr&>(e);
        if (m.arms.empty()) {
            return false;
        }
        for (const auto& a : m.arms) {
            if (a.body == nullptr || !expr_diverges(*a.body)) {
                return false;
            }
        }
        return true;
    }
    default:
        return false;
    }
}

bool Resolver::stmt_diverges(const ast::Stmt& s) const {
    switch (s.kind) {
    case ast::NodeKind::ReturnStmt:
        return true;
    case ast::NodeKind::ExprStmt: {
        const auto& es = static_cast<const ast::ExprStmt&>(s);
        return es.expr != nullptr && expr_diverges(*es.expr);
    }
    case ast::NodeKind::LetStmt: {
        const auto& l = static_cast<const ast::LetStmt&>(s);
        return l.value != nullptr && expr_diverges(*l.value);
    }
    case ast::NodeKind::VarStmt: {
        const auto& v = static_cast<const ast::VarStmt&>(s);
        return v.value != nullptr && expr_diverges(*v.value);
    }
    case ast::NodeKind::WhileStmt: {
        const auto& w = static_cast<const ast::WhileStmt&>(s);
        const bool forever = w.cond != nullptr && w.cond->kind == ast::NodeKind::BoolLit
                             && static_cast<const ast::BoolLit&>(*w.cond).value;
        return forever && !expr_breaks_out(w.body.get(), w.label, false);
    }
    case ast::NodeKind::WithStmt: {
        const auto& ws = static_cast<const ast::WithStmt&>(s);
        return ws.body != nullptr && expr_diverges(*ws.body);
    }
    default:
        return false;
    }
}

TypePtr Resolver::check_block_expr(const ast::BlockExpr& b, TypePtr expected) {
    ScopeStack::Guard g(scopes_);
    TypePtr last = types_->unit();
    for (std::size_t i = 0; i < b.stmts.size(); ++i) {
        const auto& s = *b.stmts[i];
        if (s.kind == ast::NodeKind::ExprStmt && i + 1 == b.stmts.size()) {
            // Trailing expression: its type is the block's value type, and
            // it inherits the surrounding block's expected type.
            const bool allow_weak = enter_stmt_attributes(s);
            last = check_expr(*static_cast<const ast::ExprStmt&>(s).expr, expected);
            if (allow_weak) {
                --allow_weak_cas_depth_;
            }
        } else {
            check_stmt(s);
        }
    }
    return last;
}

namespace {

// §14.4 the register names a pinned operand or clobber may use, per target.
// Only checked when --target names an architecture; a host build can't know
// which assembler it will meet.
bool is_register_name(std::string_view arch, std::string_view r) {
    auto numbered = [&](std::string_view prefix, int lo, int hi) {
        if (!r.starts_with(prefix) || r.size() == prefix.size()) {
            return false;
        }
        int n = 0;
        for (char c : r.substr(prefix.size())) {
            if (c < '0' || c > '9') {
                return false;
            }
            n = n * 10 + (c - '0');
        }
        return n >= lo && n <= hi && !(r.size() > prefix.size() + 1 && r[prefix.size()] == '0');
    };
    auto one_of = [&](std::initializer_list<std::string_view> names) {
        for (auto n : names) {
            if (r == n) {
                return true;
            }
        }
        return false;
    };
    if (arch == "aarch64") {
        return numbered("x", 0, 30) || numbered("w", 0, 30) || numbered("v", 0, 31)
               || numbered("q", 0, 31) || numbered("d", 0, 31) || numbered("s", 0, 31)
               || numbered("h", 0, 31) || numbered("b", 0, 31) || one_of({"sp", "fp", "lr"});
    }
    if (arch == "x86_64") {
        return one_of({"rax", "rbx", "rcx", "rdx", "rsi", "rdi", "rbp", "rsp", "eax", "ebx",
                       "ecx", "edx", "esi", "edi", "ebp", "esp", "ax", "bx", "cx", "dx", "al",
                       "bl", "cl", "dl"})
               || numbered("r", 8, 15) || numbered("xmm", 0, 31) || numbered("ymm", 0, 31)
               || numbered("zmm", 0, 31) || numbered("k", 0, 7);
    }
    if (arch == "riscv64") {
        return numbered("x", 0, 31) || numbered("f", 0, 31) || numbered("t", 0, 6)
               || numbered("s", 0, 11) || numbered("a", 0, 7) || numbered("ft", 0, 11)
               || numbered("fs", 0, 11) || numbered("fa", 0, 7)
               || one_of({"zero", "ra", "sp", "gp", "tp", "fp"});
    }
    return true;
}

bool is_asm_operand_type(const TypePtr& t) {
    if (t == nullptr || t->is_error()) {
        return true;  // already diagnosed
    }
    if (t->kind() == TypeKind::Int128 || t->kind() == TypeKind::UInt128) {
        return false;  // wider than any one general register
    }
    return t->is_integer() || t->kind() == TypeKind::Bool || t->kind() == TypeKind::Ptr
           || t->kind() == TypeKind::MutPtr;
}

bool is_place_expr(const ast::Expr& e) {
    switch (e.kind) {
    case ast::NodeKind::IdentExpr:
        return true;
    case ast::NodeKind::MemberExpr:
        return is_place_expr(*static_cast<const ast::MemberExpr&>(e).base);
    case ast::NodeKind::IndexExpr:
        return is_place_expr(*static_cast<const ast::IndexExpr&>(e).base);
    default:
        return false;
    }
}

}  // namespace

void Resolver::check_asm(const ast::AsmStmt& a) {
    std::vector<std::string_view> names;
    std::vector<std::string_view> in_regs;
    std::vector<std::string_view> out_regs;
    const std::string_view arch = target_.arch;
    auto check_reg = [&](std::string_view reg, diag::SourceRange r) {
        if (!is_register_name(arch, reg)) {
            error_at(r, std::format("'{}' is not a register on {}", reg, arch));
        }
    };
    auto add_name = [&](const ast::AsmOperand& op) {
        for (auto n : names) {
            if (n == op.name) {
                error_at(op.range, std::format("duplicate asm operand name '{}'", op.name));
                return;
            }
        }
        names.push_back(op.name);
    };
    auto check_operand = [&](const ast::AsmOperand& op, bool place, std::vector<std::string_view>& regs) {
        TypePtr t = op.value ? check_expr(*op.value) : nullptr;
        if (!is_asm_operand_type(t)) {
            error_at(op.range,
                     std::format("an asm operand is an integer, Bool or pointer value; got {}",
                                 t->describe()));
        }
        if (place && op.value && !is_place_expr(*op.value)) {
            error_at(op.range, "an asm output must be a place (a variable, field or element)");
        }
        if (!op.reg.empty()) {
            check_reg(op.reg, op.range);
            for (auto r : regs) {
                if (r == op.reg) {
                    error_at(op.range, std::format("register '{}' is pinned twice", op.reg));
                }
            }
            regs.push_back(op.reg);
        } else if (!op.name.empty()) {
            add_name(op);
        }
    };
    for (const auto& op : a.inputs) {
        check_operand(op, false, in_regs);
    }
    for (const auto& op : a.outputs) {
        check_operand(op, true, out_regs);
    }
    for (const auto& op : a.inouts) {
        check_operand(op, true, out_regs);
        // An inout pin is an input pin too.
        if (!op.reg.empty()) {
            for (auto r : in_regs) {
                if (r == op.reg) {
                    error_at(op.range, std::format("register '{}' is pinned twice", op.reg));
                }
            }
        }
    }
    for (std::size_t i = 0; i < a.clobbers.size(); ++i) {
        const auto& c = a.clobbers[i];
        if (c == "memory" || c == "cc" || (arch == "aarch64" && is_a64_sysreg_name(c))) {
            continue;
        }
        check_reg(c, a.clobber_ranges[i]);
        for (const auto* regs : {&in_regs, &out_regs}) {
            for (auto r : *regs) {
                if (r == c) {
                    error_at(a.clobber_ranges[i],
                             std::format("register '{}' is both an operand and a clobber", c));
                }
            }
        }
    }
    if (arch == "aarch64" || arch == "x86_64" || arch == "riscv64") {
        check_asm_writes(a, arch);
    }
    if (a.volatile_written && !a.is_volatile && a.outputs.empty() && a.inouts.empty()) {
        error_at(a.range,
                 "a non-volatile asm with no outputs has no effect the compiler can see and may "
                 "be deleted; drop `volatile: false` or give it an output");
    }
    // Template placeholders: `{name}` names an operand; `{{` / `}}` are braces.
    for (std::size_t li = 0; li < a.lines.size(); ++li) {
        const std::string_view line = a.lines[li];
        for (std::size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '{') {
                if (i + 1 < line.size() && line[i + 1] == '{') {
                    ++i;
                    continue;
                }
                auto close = line.find('}', i + 1);
                if (close == std::string_view::npos) {
                    error_at(a.line_ranges[li], "unclosed '{' in asm template; write '{{' for a literal brace");
                    break;
                }
                const auto name = line.substr(i + 1, close - i - 1);
                bool found = false;
                for (auto n : names) {
                    found = found || n == name;
                }
                if (!found) {
                    error_at(a.line_ranges[li],
                             std::format("asm template names '{{{}}}', which is not a named operand "
                                         "(write '{{{{' for a literal brace)",
                                         name));
                }
                i = close;
            } else if (line[i] == '}') {
                if (i + 1 < line.size() && line[i + 1] == '}') {
                    ++i;
                    continue;
                }
                error_at(a.line_ranges[li], "stray '}' in asm template; write '}}' for a literal brace");
            }
        }
    }
}

// Annex §14.4 rule 1 an instruction that writes a register, the flags or a
// sysreg must name it: an output or inout pin, or a clobber. Asm in a
// `@naked` function is the whole function, so nothing is compiled around it
// to protect and it is exempt.
void Resolver::check_asm_writes(const ast::AsmStmt& a, std::string_view arch) {
    if (current_func_ != nullptr
        && std::ranges::any_of(current_func_->attributes,
                               [](const ast::Attribute& at) { return at.name == "naked"; })) {
        return;
    }
    std::vector<std::string> named;
    for (const auto* ops : {&a.outputs, &a.inouts}) {
        for (const auto& op : *ops) {
            if (!op.reg.empty()) {
                named.push_back(asm_reg_key(arch, op.reg));
            }
        }
    }
    bool flags_named = false;
    std::vector<std::string> sysregs;
    for (const auto& c : a.clobbers) {
        if (c == "cc") {
            flags_named = true;
        } else if (arch == "aarch64" && is_a64_sysreg_name(c)) {
            sysregs.push_back(a64_sysreg_key(c));
            flags_named = flags_named || a64_sysreg_key(c) == "nzcv";
        } else {
            named.push_back(asm_reg_key(arch, c));
        }
    }
    std::vector<std::string> reported;
    auto once = [&](const std::string& key) {
        if (std::ranges::find(reported, key) != reported.end()) {
            return false;
        }
        reported.push_back(key);
        return true;
    };
    for (std::size_t li = 0; li < a.lines.size(); ++li) {
        const std::string_view line = a.lines[li];
        for (const auto& w : asm_line_writes(arch, line)) {
            switch (w.kind) {
            case AsmWrite::Kind::Register:
                if (std::ranges::find(named, w.name) == named.end() && once(w.name)) {
                    error_at(a.line_ranges[li],
                             std::format("asm writes {} ('{}'), which isn't an output, an inout "
                                         "or a clobber: add \"{}\" to `clobbers:` (Annex §14.4)",
                                         w.name,
                                         line,
                                         w.name));
                }
                break;
            case AsmWrite::Kind::Flags:
                if (!flags_named && once("cc")) {
                    error_at(a.line_ranges[li],
                             std::format("asm sets the condition flags ('{}'): add \"cc\" to "
                                         "`clobbers:` (Annex §14.4)",
                                         line));
                }
                break;
            case AsmWrite::Kind::Sysreg:
                if (std::ranges::find(sysregs, w.name) == sysregs.end() && once(w.name)) {
                    error_at(a.line_ranges[li],
                             std::format("asm writes the sysreg {} ('{}'): add \"{}\" to "
                                         "`clobbers:` (Annex §14.4)",
                                         w.name,
                                         line,
                                         w.name));
                }
                break;
            }
        }
    }
}

bool Resolver::needs_hosted_runtime(std::string_view cap) {
    return cap == "Alloc" || cap == "Log" || cap == "Async" || cap == "Fs" || cap == "Net"
           || cap == "Clock" || cap == "Rand";
}

void Resolver::reject_hosted_cap(std::string_view cap, diag::SourceRange r) {
    if (!needs_hosted_runtime(cap) || !hosted_reported_.insert(r.begin.offset).second) {
        return;
    }
    error_at(r,
             std::format("`{}` needs the hosted runtime, which a --no-libc build doesn't have "
                         "(no heap, logging, files, async, clock, network or randomness)",
                         cap));
}

bool Resolver::in_interrupt_handler() const {
    return current_func_ != nullptr
           && std::ranges::any_of(current_func_->attributes,
                                  [](const ast::Attribute& at) { return at.name == "interrupt"; });
}

// §14.5 an ISR shares state with the rest of the kernel only through a
// `static Atomic`: writing any other static from the handler's own body (by
// assignment, or by lending it `inout`) is an error.
void Resolver::check_interrupt_static_write(const ast::Expr& target, diag::SourceRange site) {
    const ast::Expr* root = &target;
    while (root->kind == ast::NodeKind::MemberExpr || root->kind == ast::NodeKind::IndexExpr
           || root->kind == ast::NodeKind::ParenExpr) {
        if (root->kind == ast::NodeKind::MemberExpr) {
            root = static_cast<const ast::MemberExpr&>(*root).base.get();
        } else if (root->kind == ast::NodeKind::IndexExpr) {
            root = static_cast<const ast::IndexExpr&>(*root).base.get();
        } else {
            root = static_cast<const ast::ParenExpr&>(*root).inner.get();
        }
    }
    if (root->kind != ast::NodeKind::IdentExpr) {
        return;
    }
    const auto* sym = scopes_.current().lookup(static_cast<const ast::IdentExpr&>(*root).name);
    if (sym == nullptr || sym->kind != SymbolKind::Static || sym->type == nullptr) {
        return;
    }
    if (sym->type->kind() == TypeKind::Atomic || sym->type->kind() == TypeKind::AtomicTaggedPointer) {
        return;
    }
    error_at(site,
             std::format("an @interrupt handler shares state only through a `static Atomic`; "
                         "'{}' is a plain static ({}), so writing it here races the code it "
                         "interrupted",
                         sym->name,
                         sym->type->describe()));
}

TypePtr Resolver::self_type_of(const ast::Decl& owner, const std::string& name,
                               const std::vector<ast::GenericParam>& generics) {
    const auto* sym = scopes_.current().lookup(name);
    if (sym == nullptr || sym->decl != &owner || sym->type == nullptr
        || (sym->type->kind() != TypeKind::Struct && sym->type->kind() != TypeKind::Enum)) {
        return nullptr;
    }
    if (generics.empty()) {
        return sym->type;
    }
    std::vector<TypePtr> args;
    for (const auto& gp : generics) {
        if (gp.is_const || gp.name.empty()) {
            return nullptr;  // a value parameter has no type to stand in for it
        }
        const auto* g = scopes_.current().lookup(gp.name);
        if (g == nullptr || g->kind != SymbolKind::GenericParam || g->type == nullptr) {
            return nullptr;
        }
        args.push_back(g->type);
    }
    return sym->type->kind() == TypeKind::Struct
               ? types_->make_struct_instance(sym->type->nominal_decl(), std::move(args))
               : types_->make_enum_instance(sym->type->nominal_decl(), std::move(args));
}

void Resolver::check_mutable_place(const ast::Expr& target, diag::SourceRange site) {
    // Walk to the root binding. Writing through a view (an element of a
    // MutSpan, the pointee of a MutPtr) changes the memory it views, not the
    // binding holding the view, so the walk stops there.
    const ast::Expr* root = &target;
    while (true) {
        if (root->kind == ast::NodeKind::ParenExpr) {
            root = static_cast<const ast::ParenExpr&>(*root).inner.get();
        } else if (root->kind == ast::NodeKind::MemberExpr) {
            const auto& m = static_cast<const ast::MemberExpr&>(*root);
            auto bt = resolution_.type_of(m.base.get());
            if (bt != nullptr && m.member == "value"
                && (bt->kind() == TypeKind::MutPtr || bt->kind() == TypeKind::Ptr)) {
                return;
            }
            root = m.base.get();
        } else if (root->kind == ast::NodeKind::IndexExpr) {
            const auto& ix = static_cast<const ast::IndexExpr&>(*root);
            auto bt = resolution_.type_of(ix.base.get());
            if (bt != nullptr && (bt->kind() == TypeKind::MutSpan || bt->kind() == TypeKind::Span)) {
                return;
            }
            root = ix.base.get();
        } else {
            break;
        }
        if (root == nullptr) {
            return;
        }
    }
    // §5 a plain method reads its receiver; only an `inout func` (or a
    // `sink` one, which owns it) may change `self` (codegen makes a plain
    // method `const`).
    if (root->kind == ast::NodeKind::SelfExpr) {
        if (current_func_ != nullptr && current_self_type_ != nullptr
            && current_func_->recv_mode == ast::FuncDecl::RecvMode::None) {
            error_at(site,
                     std::format("'{}' only reads its receiver, so it can't change `self`; "
                                 "declare it `inout func {}` to mutate it",
                                 current_func_->name,
                                 current_func_->name));
        }
        return;
    }
    if (root->kind != ast::NodeKind::IdentExpr) {
        return;
    }
    const auto& name = static_cast<const ast::IdentExpr&>(*root).name;
    const auto* sym = scopes_.current().lookup(name);
    if (sym == nullptr) {
        return;
    }
    // §1 / §4 a `static` holds an immutable value or an `Atomic`; changing
    // shared state goes through the atomic's methods.
    if (sym->kind == SymbolKind::Static) {
        if (sym->type != nullptr
            && (sym->type->kind() == TypeKind::Atomic
                || sym->type->kind() == TypeKind::AtomicTaggedPointer)) {
            return;
        }
        if (in_interrupt_handler()) {
            return;  // check_interrupt_static_write names the ISR rule instead
        }
        error_at(site,
                 std::format("'{}' is a static, which holds an immutable value; keep shared "
                             "mutable state in an `Atomic`, or in `@noinit` storage reached "
                             "through `&{}` under `RawMemory`",
                             name,
                             name));
        return;
    }
    if (sym->is_mutable) {
        return;
    }
    if (sym->kind == SymbolKind::Parameter) {
        error_at(site,
                 std::format("'{}' is a `read` parameter, so it can't be changed here; take it "
                             "`inout` to change the caller's value",
                             name));
    } else if (sym->kind == SymbolKind::Local) {
        error_at(site,
                 std::format("'{}' is bound with `let`, so it can't be changed; declare it `var` "
                             "to change it",
                             name));
    }
}

void Resolver::check_naked_body(const ast::FuncDecl& f) {
    const bool naked = std::ranges::any_of(f.attributes, [](const ast::Attribute& at) {
        return at.name == "naked" || at.name == "boot";
    });
    if (!naked || f.body == nullptr || f.body->kind != ast::NodeKind::BlockExpr) {
        return;
    }
    for (const auto& s : static_cast<const ast::BlockExpr&>(*f.body).stmts) {
        if (s->kind == ast::NodeKind::AsmStmt) {
            const auto& a = static_cast<const ast::AsmStmt&>(*s);
            if (a.inputs.empty() && a.outputs.empty() && a.inouts.empty() && a.clobbers.empty()) {
                continue;
            }
            error_at(s->range,
                     std::format("'{}' is @naked: its asm can't take operands or clobbers (there "
                                 "is no prologue to set them up); name registers directly",
                                 f.name));
            continue;
        }
        error_at(s->range,
                 std::format("'{}' is @naked: its body may contain only asm (it has no prologue, "
                             "stack frame or return)",
                             f.name));
    }
}

// Annex §14.9.3 `@allow_weak_cas` admits the weak CAS sites of its
// statement (the caller drops the count once the statement is checked);
// Annex §14.12.3 `@no_auto_barrier` is held to the function-level rule: only
// inside `@kernel_init`.
bool Resolver::enter_stmt_attributes(const ast::Stmt& s) {
    if (s.has_attribute("no_auto_barrier")) {
        const bool kinit = current_func_ != nullptr
                           && std::ranges::any_of(current_func_->attributes,
                                                  [](const ast::Attribute& a) {
                                                      return a.name == "kernel_init";
                                                  });
        if (!kinit) {
            error_at(s.range,
                     "@no_auto_barrier is admitted only on functions also attributed "
                     "@kernel_init — that's the boot-path subset where a following sysreg "
                     "write subsumes the auto-emitted barrier");
        }
    }
    if (!s.has_attribute("allow_weak_cas")) {
        return false;
    }
    ++allow_weak_cas_depth_;
    return true;
}

void Resolver::check_stmt(const ast::Stmt& s) {
    struct AllowWeakCas {
        int* depth;
        bool on;
        ~AllowWeakCas() {
            if (on) {
                --*depth;
            }
        }
    } allow_weak{&allow_weak_cas_depth_, enter_stmt_attributes(s)};
    switch (s.kind) {
    case ast::NodeKind::LetStmt:
    case ast::NodeKind::VarStmt: {
        std::string_view binding_name;
        diag::SourceRange binding_range = s.range;
        const ast::Type* annotation = nullptr;
        const ast::Expr* initializer = nullptr;

        if (s.kind == ast::NodeKind::LetStmt) {
            const auto& l = static_cast<const ast::LetStmt&>(s);
            if (l.pattern && l.pattern->kind == ast::NodeKind::IdentPat) {
                binding_name = static_cast<const ast::IdentPat&>(*l.pattern).name;
                binding_range = l.pattern->range;
            } else if (l.pattern && l.pattern->kind == ast::NodeKind::BindPat) {
                binding_name = static_cast<const ast::BindPat&>(*l.pattern).name;
                binding_range = l.pattern->range;
            }
            annotation = l.type.get();
            initializer = l.value.get();
        } else {
            const auto& v = static_cast<const ast::VarStmt&>(s);
            if (v.pattern && v.pattern->kind == ast::NodeKind::IdentPat) {
                binding_name = static_cast<const ast::IdentPat&>(*v.pattern).name;
                binding_range = v.pattern->range;
            } else if (v.pattern && v.pattern->kind == ast::NodeKind::BindPat) {
                binding_name = static_cast<const ast::BindPat&>(*v.pattern).name;
                binding_range = v.pattern->range;
            }
            annotation = v.type.get();
            initializer = v.value.get();
        }

        TypePtr annot_type = annotation ? resolve_type(*annotation) : nullptr;
        TypePtr value_type = initializer ? check_expr(*initializer, annot_type) : nullptr;

        if (annot_type != nullptr && value_type != nullptr
            && !TypeArena::assignable(value_type, annot_type)) {
            error_at(initializer->range,
                     std::format("initializer of type {} does not match annotation {}",
                                 value_type->describe(),
                                 annot_type->describe()));
        }
        if (initializer != nullptr) {
            note_string_coercion(*initializer, value_type, annot_type);
        }

        if (!binding_name.empty()) {
            Symbol sym;
            sym.name = std::string{binding_name};
            sym.is_mutable = s.kind == ast::NodeKind::VarStmt;  // §4 `let` binds immutably
            sym.kind = SymbolKind::Local;
            sym.type = annot_type != nullptr ? annot_type : value_type;
            sym.definition_range = binding_range;
            if (auto* prev = scopes_.current().insert(std::move(sym))) {
                duplicate_definition(*prev, binding_name, binding_range);
            }
            // §5/§19.6 — expose the bound Local so the ownership checker can
            // register a linear binding at its declaration site.
            resolution_.set_binding_symbol(&s, scopes_.current().lookup(binding_name));
        }
        // §6 tuple destructuring: `let (a, b) = pair` binds each
        // element of the tuple pattern to the corresponding tuple
        // element's type. Arity / shape mismatches are reported.
        const ast::Pattern* pat = nullptr;
        if (s.kind == ast::NodeKind::LetStmt) {
            pat = static_cast<const ast::LetStmt&>(s).pattern.get();
        } else {
            pat = static_cast<const ast::VarStmt&>(s).pattern.get();
        }
        if (pat != nullptr && pat->kind == ast::NodeKind::TuplePat) {
            TypePtr bind_t = annot_type != nullptr ? annot_type : value_type;
            bind_tuple_pattern(static_cast<const ast::TuplePat&>(*pat), bind_t,
                               s.kind == ast::NodeKind::VarStmt);
        }
        // §5/§18.4 — record sub-view provenance (split / chunks / alias) so the
        // exclusivity checker can flag a borrow that aliases the parent place.
        record_partition_provenance(pat, initializer);
        break;
    }
    case ast::NodeKind::ReturnStmt: {
        const auto& r = static_cast<const ast::ReturnStmt&>(s);
        TypePtr expected = return_stack_.empty() ? nullptr : return_stack_.back();
        TypePtr value_type = r.value ? check_expr(*r.value, expected) : types_->unit();
        if (expected != nullptr && !TypeArena::assignable(value_type, expected)) {
            error_at(s.range,
                     std::format("return value of type {} does not match function result type {}",
                                 value_type ? value_type->describe() : "?",
                                 expected->describe()));
        }
        if (r.value != nullptr) {
            note_string_coercion(*r.value, value_type, expected);
        }
        break;
    }
    case ast::NodeKind::ExprStmt:
        (void)check_expr(*static_cast<const ast::ExprStmt&>(s).expr);
        break;
    case ast::NodeKind::AsmStmt:
        check_asm(static_cast<const ast::AsmStmt&>(s));
        break;
    case ast::NodeKind::AssignStmt: {
        const auto& a = static_cast<const ast::AssignStmt&>(s);
        if (in_interrupt_handler()) {
            check_interrupt_static_write(*a.target, a.range);
        }

        auto lhs = check_expr(*a.target);
        check_mutable_place(*a.target, a.range);  // after typing: views are judged by type
        if (a.op != ast::AssignOp::Assign) {
            check_compound_assign(a, lhs);
        }
        // Pass the target type as the expected type for the rhs so an
        // int literal (`i + 1`) adapts to the slot width (Int32 vs Int)
        // — same propagation as `let x: Int32 = i + 1` already does.
        auto rhs = check_expr(*a.value, lhs);
        if (lhs != nullptr && rhs != nullptr && !TypeArena::assignable(rhs, lhs)) {
            error_at(a.value->range,
                     std::format("cannot assign value of type {} to target of type {}",
                                 rhs->describe(),
                                 lhs->describe()));
        }
        if (a.value != nullptr) {
            note_string_coercion(*a.value, rhs, lhs);
        }
        break;
    }
    case ast::NodeKind::WhileStmt: {
        const auto& w = static_cast<const ast::WhileStmt&>(s);
        // §A4 (§14.9.3) a retry loop admits its one weak CAS.
        if (const auto* admitted = weak_cas_of_loop(w)) {
            admitted_weak_cas_.insert(admitted);
        }
        if (w.repeat && w.body) {
            check_expr(*w.body);
        }
        auto cond_type = check_expr(*w.cond);
        if (cond_type != nullptr && cond_type->kind() != TypeKind::Bool && !cond_type->is_error()) {
            error_at(w.cond->range,
                     std::format("while condition must be Bool, got {}", cond_type->describe()));
        }
        if (!w.repeat && w.body) {
            check_expr(*w.body);
        }
        break;
    }
    case ast::NodeKind::ForStmt: {
        // §5 `for x in xs { ... }` — two shapes:
        //   * Range / RangeLt: `for x in 0..10` / `for x in 0..<10` —
        //     loop variable inherits the lhs/rhs numeric type.
        //   * Iterator protocol: `xs` has a `next() -> T?` method;
        //     loop variable is T.
        const auto& f = static_cast<const ast::ForStmt&>(s);
        auto iter_type = check_expr(*f.iter);
        TypePtr elem_type = nullptr;
        if (f.iter->kind == ast::NodeKind::BinaryExpr) {
            const auto& b = static_cast<const ast::BinaryExpr&>(*f.iter);
            if (b.op == ast::BinaryOp::Range || b.op == ast::BinaryOp::RangeLt) {
                // The Range result type is currently the operand type
                // itself (no nominal Range[T] yet); use it directly.
                elem_type = iter_type;
            }
        }
        if (elem_type == nullptr && iter_type != nullptr
            && (iter_type->kind() == TypeKind::Vec || iter_type->kind() == TypeKind::Span
                || iter_type->kind() == TypeKind::MutSpan)
            && iter_type->inner() != nullptr) {
            // §18.5 / §10 / §13 `for x in xs` over a Vec[T], a Span[T]/MutSpan[T]
            // (incl. a Soa column view), iterates its elements directly (lowered
            // to a C++ range-based for); the loop variable type is T.
            elem_type = iter_type->inner();
        }
        if (elem_type == nullptr && iter_type != nullptr && !iter_type->is_error()) {
            // Iterator protocol fallback: look for a `next()` method
            // whose result is Optional<T>; the loop variable type is T.
            if (auto next_type = lookup_method(iter_type, "next")) {
                if (next_type->kind() == TypeKind::Function && next_type->result() != nullptr
                    && next_type->result()->kind() == TypeKind::Optional) {
                    elem_type = next_type->result()->inner();
                }
            }
            if (elem_type == nullptr) {
                error_at(f.iter->range,
                         std::format("'for x in xs' requires xs to be a Range, a Vec, or have a "
                                     "'next() -> Element?' method, got {}",
                                     iter_type->describe()));
            }
        }
        ScopeStack::Guard g(scopes_);
        if (f.pattern && f.pattern->kind == ast::NodeKind::IdentPat) {
            Symbol sym;
            sym.name = static_cast<const ast::IdentPat&>(*f.pattern).name;
            sym.is_mutable = false;  // a `for` element is a `let`
            sym.kind = SymbolKind::Local;
            sym.type = elem_type != nullptr ? elem_type : types_->error();
            sym.definition_range = f.pattern->range;
            const std::string name = sym.name;
            (void)g.scope().insert(std::move(sym));
            // §19.4 the escape checker tracks a view element back to `xs`.
            resolution_.set_binding_symbol(&s, scopes_.current().lookup(name));
        } else if (f.pattern && f.pattern->kind == ast::NodeKind::TuplePat) {
            // §6 `for (a, b) in zip(xs, ys)` — destructure each
            // element of the iterator into a tuple pattern.
            bind_tuple_pattern(static_cast<const ast::TuplePat&>(*f.pattern), elem_type);
        }
        if (f.body) {
            check_expr(*f.body);
        }
        break;
    }
    case ast::NodeKind::BreakStmt:
    case ast::NodeKind::ContinueStmt:
        break;
    case ast::NodeKind::WithStmt: {
        // §17.4 `with` block. Three shapes per WithBinding (parser
        // disambiguates):
        //   * cap_type only        — `with Alloc { … }`
        //   * cap_type + value     — `with Alloc = myAllocator { … }`
        //   * name + value         — `with mmio = createRegion() { … }`
        // Push a single scope for all bindings; insert each named
        // binding into the scope at the value's type; then recurse
        // into the body.
        const auto& w = static_cast<const ast::WithStmt&>(s);
        for (const auto& b : w.bindings) {
            if (b.cap_type != nullptr && b.cap_type->kind == ast::NodeKind::NamedType) {
                const auto& nt = static_cast<const ast::NamedType&>(*b.cap_type);
                if (nt.path.size() == 1 && scopes_.current().lookup(nt.path[0]) == nullptr) {
                    error_at(b.cap_type->range, std::format("unknown capability '{}'", nt.path[0]));
                }
            }
        }
        if (target_.freestanding) {
            for (const auto& b : w.bindings) {
                if (b.cap_type != nullptr && b.cap_type->kind == ast::NodeKind::NamedType) {
                    const auto& nt = static_cast<const ast::NamedType&>(*b.cap_type);
                    if (!nt.path.empty()) {
                        reject_hosted_cap(nt.path.back(), b.cap_type->range);
                    }
                }
            }
        }
        // §14.5 an interrupt handler has no heap, no awaits and no FFI: it
        // can't open them with `with` any more than declare them with `using`
        // (and a call that needs one then has nothing to resolve it).
        if (in_interrupt_handler()) {
            for (const auto& b : w.bindings) {
                if (b.cap_type != nullptr && b.cap_type->kind == ast::NodeKind::NamedType) {
                    const auto& nt = static_cast<const ast::NamedType&>(*b.cap_type);
                    const std::string cap = nt.path.empty() ? std::string{} : nt.path.back();
                    if (cap == "Alloc" || cap == "Async" || cap == "Extern") {
                        error_at(b.cap_type->range,
                                 std::format("an @interrupt handler can't open `with {}` (no heap, "
                                             "no awaits, no FFI in a handler); defer that work to a "
                                             "task",
                                             cap));
                    }
                }
            }
        }
        ScopeStack::Guard g(scopes_);
        for (const auto& b : w.bindings) {
            // §17.4 type annotation on a name-binding (`with NAME: T
            // = EXPR`). When present, push T down as the value
            // check's expected type so an integer literal adopts the
            // annotated width, then assignability-check the result.
            TypePtr annotation = b.type_annotation ? resolve_type(*b.type_annotation) : nullptr;
            TypePtr value_t = nullptr;
            if (b.value) {
                value_t = check_expr(*b.value, annotation);
            }
            if (annotation != nullptr && value_t != nullptr
                && !TypeArena::assignable(value_t, annotation)) {
                error_at(b.value ? b.value->range : s.range,
                         std::format("with-binding value of type {} does not match "
                                     "annotation {}",
                                     value_t->describe(),
                                     annotation->describe()));
            }
            if (!b.name.empty()) {
                Symbol sym;
                sym.name = b.name;
                sym.kind = SymbolKind::Local;
                // Honor the annotation as the binding's declared
                // type when present; otherwise fall back to the
                // value's inferred type.
                sym.type = annotation != nullptr ? annotation
                                                 : (value_t != nullptr ? value_t : types_->error());
                sym.definition_range = {};
                if (auto* prev = g.scope().insert(std::move(sym))) {
                    duplicate_definition(*prev, b.name, {});
                }
            }
            // Cap-typed bindings don't introduce a Vestra-side
            // identifier — the capability checker handles their
            // discharge separately (including §A7's InterruptsOff
            // region rules — see capability.cpp).
        }
        if (w.body) {
            check_expr(*w.body);
        }
        break;
    }
    default:
        break;
    }
}

}  // namespace vestra::sema
