// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// Generic unification, protocol-conformance predicates, bound checking,
// derive detection, and visibility checking.
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

bool Resolver::is_display_conformant(TypePtr t) const {
    if (t == nullptr || t->is_error()) {
        return true;  // suppress cascading errors on already-broken types
    }
    if (t->is_primitive()) {
        // Unit / Never / Error aren't usefully Display-printable, but
        // is_primitive() rules out Never/Error already; Unit would
        // surface as `void` in C++ and never reach here.
        return true;
    }
    switch (t->kind()) {
    case TypeKind::Vector:
        return is_display_conformant(t->inner());
    case TypeKind::Optional:
        // `"\(opt)"` renders `.none` as `nil` and `.some(v)` by
        // delegating to T's formatter. Codegen emits a templated
        // `std::formatter<std::optional<T>>` specialization in the
        // runtime preamble (`__vstr::OptionalFormatter`) to carry
        // the C++ side. The inner T still has to be Display-
        // conformant — this rule composes, it doesn't bypass.
        return is_display_conformant(t->inner());
    case TypeKind::Struct:
    case TypeKind::Enum:
    case TypeKind::OpaqueType:
        return decl_derives(t->nominal_decl(), "Display")
               || decl_derives(t->nominal_decl(), "Debug");
    default:
        return false;
    }
}

bool Resolver::is_default_conformant(TypePtr t) const {
    if (t == nullptr || t->is_error()) {
        return false;
    }
    switch (t->kind()) {
    case TypeKind::Int:
    case TypeKind::Int8:
    case TypeKind::Int16:
    case TypeKind::Int32:
    case TypeKind::Int64:
    case TypeKind::UInt:
    case TypeKind::UInt8:
    case TypeKind::UInt16:
    case TypeKind::UInt32:
    case TypeKind::UInt64:
    case TypeKind::Int128:
    case TypeKind::UInt128:
    case TypeKind::Float32:
    case TypeKind::Float64:
    case TypeKind::Bool:
    case TypeKind::Char:
    case TypeKind::Str:
        return true;
    case TypeKind::Optional:
        // `nil` is the zero of every Optional<T>; std::optional<T>{} is
        // value-init to nullopt, no need for T to itself be conformant.
        return true;
    case TypeKind::Vector:
        // Fixed-length array `[N]T` lowers to `std::array<T, N>`; value-
        // init zero-initializes every element. Require the element to be
        // Default-conformant so the chain stays decidable.
        return is_default_conformant(t->inner());
    case TypeKind::Tuple:
        for (const auto& part : t->parts()) {
            if (!is_default_conformant(part)) {
                return false;
            }
        }
        return true;
    case TypeKind::Struct:
        return decl_derives(t->nominal_decl(), "Default");
    case TypeKind::GenericParam:
        // §7 generics phase 2 — a field typed by a generic parameter can't
        // be checked for Default-conformance until the type is
        // instantiated. Defer: accept here, and the C++ value-init at the
        // instantiation site enforces it.
        return true;
    case TypeKind::OpaqueType: {
        // §3 newtype follow-on: an opaque is Default-conformant iff
        // the user has written `derive(Default) for Q` AND the
        // underlying type is itself Default-conformant. The
        // `enum class Q : Underlying {}` C++ form gives `Q{}` = the
        // underlying's zero value automatically, so no codegen
        // change is needed — the conformance check is what gates
        // use of `Q()` as a default-init slot.
        if (!decl_derives(t->nominal_decl(), "Default")) {
            return false;
        }
        if (t->nominal_decl() == nullptr || t->nominal_decl()->kind != ast::NodeKind::Opaque) {
            return false;
        }
        const auto& od = static_cast<const ast::OpaqueDecl&>(*t->nominal_decl());
        if (od.underlying == nullptr) {
            return false;
        }
        // The mutable_this-style const_cast is unavoidable here: the
        // helper signature is const but resolve_type isn't (it
        // memoizes named types into the arena). The check is
        // logically const; the lookup just needs the arena's
        // identity-preserving make_X paths.
        auto under = const_cast<Resolver*>(this)->resolve_type(*od.underlying);
        return is_default_conformant(under);
    }
    default:
        // Box, Result, GenericParam, Enum (bare or sum), Protocol,
        // Function, Reference — none have a v0.5 default. The kernel
        // resort is `var x: T? = nil` plus an explicit init.
        return false;
    }
}

bool Resolver::type_satisfies_bound(TypePtr t, std::string_view protocol) const {
    if (t == nullptr || t->is_error()) {
        return true;  // suppress cascading errors on already-broken types
    }
    // A generic parameter standing in for a concrete type can't be checked
    // structurally here; accept it. The enclosing scope's own bound on that
    // parameter is the relevant constraint — transitive bound checking is a
    // follow-on.
    if (t->kind() == TypeKind::GenericParam) {
        return true;
    }
    if (protocol == "Display") {
        return is_display_conformant(t);
    }
    if (protocol == "Default") {
        return is_default_conformant(t);
    }
    if (protocol == "Eq" || protocol == "Hash" || protocol == "Debug" || protocol == "Clone") {
        if (t->is_primitive()) {
            return true;
        }
        switch (t->kind()) {
        case TypeKind::Optional:
        case TypeKind::Vector:
            return type_satisfies_bound(t->inner(), protocol);
        case TypeKind::Tuple:
            for (const auto& p : t->parts()) {
                if (!type_satisfies_bound(p, protocol)) {
                    return false;
                }
            }
            return true;
        case TypeKind::Struct:
        case TypeKind::Enum:
        case TypeKind::OpaqueType:
            return decl_derives(t->nominal_decl(), protocol);
        default:
            return false;
        }
    }
    if (protocol == "Comparable" || protocol == "Ordered") {
        // Numeric, character, and string primitives have a natural ordering.
        // v0.5 has no derive(Comparable), so a struct/enum can't yet declare
        // one.
        if (t->is_numeric()) {
            return true;
        }
        switch (t->kind()) {
        case TypeKind::Char:
        case TypeKind::Str:
        case TypeKind::StrConst:
        case TypeKind::String:
            return true;
        default:
            return false;
        }
    }
    // An unrecognized (user-defined) protocol can't be verified without a
    // general conformance table, which v0.5 doesn't have. Accept rather than
    // reject a construct we can't check.
    return true;
}

void Resolver::check_generic_bounds(const std::vector<ast::GenericParam>& generics,
                                    const std::unordered_map<std::string, TypePtr>& bindings,
                                    diag::SourceRange range) {
    for (const auto& gp : generics) {
        if (gp.is_const || gp.name.empty() || gp.bound.protocols.empty()) {
            continue;
        }
        auto it = bindings.find(gp.name);
        if (it == bindings.end() || it->second == nullptr) {
            continue;  // unbound — reported separately
        }
        for (const auto& proto : gp.bound.protocols) {
            if (proto == nullptr || proto->kind != ast::NodeKind::NamedType) {
                continue;
            }
            const auto& nt = static_cast<const ast::NamedType&>(*proto);
            if (nt.path.empty()) {
                continue;
            }
            std::string_view pname = nt.path.back();
            if (!type_satisfies_bound(it->second, pname)) {
                error_at(range,
                         std::format("type {} does not satisfy the bound '{}: {}'",
                                     it->second->describe(),
                                     gp.name,
                                     pname));
            }
        }
    }
}

bool Resolver::decl_derives(const ast::Decl* decl, std::string_view protocol) const {
    if (decl == nullptr || unit_ == nullptr) {
        return false;
    }
    for (const auto& d : unit_->decls) {
        if (d->kind != ast::NodeKind::Derive) {
            continue;
        }
        const auto& dd = static_cast<const ast::DeriveDecl&>(*d);
        if (dd.target == nullptr || dd.target->kind != ast::NodeKind::NamedType) {
            continue;
        }
        const auto& tt = static_cast<const ast::NamedType&>(*dd.target);
        if (tt.path.empty()) {
            continue;
        }
        // Match by simple name — same convention the codegen derive
        // index uses, since paths today are unqualified for in-unit
        // derive targets.
        std::string_view target_name = tt.path.back();
        std::string_view decl_name;
        if (decl->kind == ast::NodeKind::Struct) {
            decl_name = static_cast<const ast::StructDecl&>(*decl).name;
        } else if (decl->kind == ast::NodeKind::Enum) {
            decl_name = static_cast<const ast::EnumDecl&>(*decl).name;
        } else if (decl->kind == ast::NodeKind::Opaque) {
            decl_name = static_cast<const ast::OpaqueDecl&>(*decl).name;
        } else {
            continue;
        }
        if (target_name != decl_name) {
            continue;
        }
        for (const auto& p : dd.protocols) {
            if (p == nullptr || p->kind != ast::NodeKind::NamedType) {
                continue;
            }
            const auto& pt = static_cast<const ast::NamedType&>(*p);
            if (!pt.path.empty() && pt.path.back() == protocol) {
                return true;
            }
        }
    }
    return false;
}

void Resolver::check_visibility(const Symbol& sym, diag::SourceRange use_range) {
    // For v0.5 we only enforce `private`. Internal/package/public collapse to
    // the same thing inside a single compilation unit. The simple rule: a
    // `private` symbol is reachable only within its declaring scope chain —
    // and right now every top-level decl lives in the global scope, so all a
    // `private` decl can do is hide from… nobody. We still emit the check so
    // the wiring is in place; tighten it once we have nested scopes that
    // *aren't* the inside of a function body (e.g., struct member scopes).
    if (sym.visibility != ast::Visibility::Private) {
        return;
    }
    // Conservative: we don't yet model "the scope that owns sym", so we never
    // reject. This branch is the hook for the eventual real check; tested by
    // a TODO test in tests/sema/resolver_test.cpp.
    (void)use_range;
}

void Resolver::unify_generic(TypePtr ptype,
                             TypePtr atype,
                             std::unordered_map<std::string, TypePtr>& bindings,
                             diag::SourceRange site) {
    if (ptype == nullptr || atype == nullptr) {
        return;
    }
    if (ptype->kind() == TypeKind::GenericParam) {
        auto name = std::string{ptype->generic_name()};
        auto it = bindings.find(name);
        if (it == bindings.end()) {
            bindings.emplace(std::move(name), atype);
        } else if (!TypeArena::equal(it->second, atype) && !atype->is_error()) {
            error_at(site,
                     std::format("conflicting bindings for generic '{}': {} vs {}",
                                 ptype->generic_name(),
                                 it->second ? it->second->describe() : "?",
                                 atype->describe()));
        }
        return;
    }
    if (ptype->kind() == TypeKind::Optional && atype->kind() == TypeKind::Optional) {
        unify_generic(ptype->inner(), atype->inner(), bindings, site);
        return;
    }
    if (ptype->kind() == TypeKind::Vector && atype->kind() == TypeKind::Vector) {
        unify_generic(ptype->inner(), atype->inner(), bindings, site);
        // §7 generics phase 2 — a symbolic-length parameter `[N]T` unifies
        // its length against the concrete argument's length, binding the
        // const generic N (e.g. inferring N = 4 from a `[4]Int32` argument).
        if (!ptype->vector_length_name().empty()) {
            auto cname = std::string{ptype->vector_length_name()};
            auto cval = types_->make_const_value(atype->vector_length());
            auto it = bindings.find(cname);
            if (it == bindings.end()) {
                bindings.emplace(std::move(cname), cval);
            } else if (!TypeArena::equal(it->second, cval)) {
                error_at(site,
                         std::format("conflicting bindings for const generic '{}': {} vs {}",
                                     ptype->vector_length_name(),
                                     it->second ? it->second->describe() : "?",
                                     cval->describe()));
            }
        }
        return;
    }
    if (ptype->kind() == TypeKind::Tuple && atype->kind() == TypeKind::Tuple
        && ptype->parts().size() == atype->parts().size()) {
        for (std::size_t k = 0; k < ptype->parts().size(); ++k) {
            unify_generic(ptype->parts()[k], atype->parts()[k], bindings, site);
        }
        return;
    }
    // §7 generics phase 2 — a struct or enum instance unifies argument-wise
    // when both sides share a decl. This is what lets expected-type seeding
    // bind a parameter: unifying `Option[T]` (the case-constructor result)
    // against the expected `Option[Int32]` records T = Int32 before the
    // payload arguments are even checked.
    if ((ptype->kind() == TypeKind::Struct || ptype->kind() == TypeKind::Enum)
        && ptype->kind() == atype->kind() && ptype->nominal_decl() == atype->nominal_decl()
        && ptype->parts().size() == atype->parts().size()) {
        for (std::size_t k = 0; k < ptype->parts().size(); ++k) {
            unify_generic(ptype->parts()[k], atype->parts()[k], bindings, site);
        }
        return;
    }
    // Other kinds: structural rules elsewhere already verify the call, so no
    // binding is contributed.
}

}  // namespace vestra::sema
