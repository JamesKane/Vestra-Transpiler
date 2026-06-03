// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "vestra/sema/resolver.hpp"

#include "vestra/ast/clone.hpp"
#include "vestra/ast/nodes.hpp"
#include "vestra/sema/builtins.hpp"
#include "vestra/sema/comptime.hpp"
#include "vestra/sema/scope.hpp"
#include "vestra/sema/types.hpp"

#include "resolver_internal.hpp"

#include <array>
#include <cstdlib>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace vestra::sema {

// The broadly-shared free helpers live in resolver_internal.hpp so the
// file can be split by theme. Pull them into this TU's scope unqualified
// so existing call sites and the extracted siblings keep working.
using detail::named_type_param_count;
using detail::struct_is_repr_union;

// ============================================================================
// Resolution side tables
// ============================================================================

TypePtr Resolution::type_of(const ast::Expr* e) const {
    auto it = expr_types_.find(e);
    return it == expr_types_.end() ? nullptr : it->second;
}

const Symbol* Resolution::symbol_of(const ast::Expr* e) const {
    auto it = expr_symbols_.find(e);
    return it == expr_symbols_.end() ? nullptr : it->second;
}

void Resolution::set_type(const ast::Expr* e, TypePtr t) {
    expr_types_[e] = t;
}
void Resolution::set_symbol(const ast::Expr* e, const Symbol* s) {
    expr_symbols_[e] = s;
}

const Symbol* Resolution::binding_symbol(const ast::Stmt* s) const {
    auto it = binding_symbols_.find(s);
    return it == binding_symbols_.end() ? nullptr : it->second;
}

void Resolution::set_binding_symbol(const ast::Stmt* s, const Symbol* sym) {
    binding_symbols_[s] = sym;
}
const std::string* Resolution::qualified_name_of(const ast::Expr* e) const {
    auto it = qualified_names_.find(e);
    return it == qualified_names_.end() ? nullptr : &it->second;
}
void Resolution::set_qualified_name(const ast::Expr* e, std::string name) {
    qualified_names_[e] = std::move(name);
}
const ComptimeValue* Resolution::folded_value(const ast::Expr* e) const {
    auto it = folded_.find(e);
    return it == folded_.end() ? nullptr : &it->second;
}
void Resolution::set_folded_value(const ast::Expr* e, ComptimeValue v) {
    folded_[e] = v;
}
bool Resolution::is_gated_out(const ast::Decl* d) const {
    return gated_decls_.contains(d);
}
void Resolution::mark_gated_out(const ast::Decl* d) {
    gated_decls_.insert(d);
}
TypePtr Resolution::do_catch_error_type(const ast::DoCatchExpr* dc) const {
    auto it = do_catch_error_.find(dc);
    return it == do_catch_error_.end() ? nullptr : it->second;
}
void Resolution::set_do_catch_error_type(const ast::DoCatchExpr* dc, TypePtr t) {
    if (t != nullptr) {
        do_catch_error_[dc] = t;
    }
}

// ============================================================================
// Resolver ctor + entry point
// ============================================================================

Resolver::Resolver(const ast::CompilationUnit& unit,
                   TypeArena& types,
                   diag::DiagnosticReporter& reporter,
                   ComptimeFolder::EmbedReader embed_reader,
                   TargetContext target)
    : unit_(&unit), types_(&types), reporter_(&reporter),
      folder_(&scopes_.global(), std::move(embed_reader)), target_(std::move(target)) {}

void Resolver::resolve() {
    register_builtin_capabilities();
    register_builtin_math();
    register_builtin_reflection();
    register_builtin_panic();
    // §A5 must follow register_builtin_reflection because the sync
    // intrinsics' signatures reference the Ordering / BarrierScope /
    // BarrierKind enums registered there.
    register_builtin_sync();
    collect_top_level();
    for (const auto& d : unit_->decls) {
        // §12.6: same gate as collect_top_level. Skipping here keeps the
        // body-check pass aligned with the symbols actually registered.
        if (gated_out(*d)) {
            continue;
        }
        check_decl(*d);
    }
}

void Resolver::error_at(diag::SourceRange r, std::string msg) {
    reporter_->report(diag::Diagnostic::error(std::move(msg)).at(r));
}

void Resolver::warn_at(diag::SourceRange r, std::string msg) {
    reporter_->report(diag::Diagnostic::warning(std::move(msg)).at(r));
}

std::string Resolver::check_wide_atomic_feature(diag::SourceRange /*r*/,
                                                std::string_view which) const {
    // §A4 (§14.9.4) — gate wide atomics on the matching architectural
    // feature. The hosted default (arch="host") bypasses the check
    // since the host C++ compiler decides via libatomic at link
    // time; explicit non-host targets need the feature in scope or
    // the user gets a silent lock-based fallback they didn't ask
    // for. The pre-architectural targets named here cover the
    // arches with a 128-bit CAS in v0.5; other arches (RISC-V at
    // its current ratified ISA, etc.) don't admit wide atomics at
    // all.
    if (target_.arch == "host" || target_.arch.empty()) {
        return {};
    }
    if (target_.arch == "aarch64") {
        if (target_.has_feature("lse2")) {
            return {};
        }
        return std::format(
            "{}[T] requires the 'lse2' target feature on aarch64 (the architectural 128-bit "
            "compare-and-swap); pass --target-features=lse2 or pick a target that has it",
            which);
    }
    if (target_.arch == "x86_64") {
        if (target_.has_feature("cx16")) {
            return {};
        }
        return std::format("{}[T] requires the 'cx16' target feature on x86_64 (CMPXCHG16B); pass "
                           "--target-features=cx16 or pick a target that has it",
                           which);
    }
    return std::format(
        "{}[T] is not supported on target '{}'; v0.5 admits wide atomics on aarch64+lse2 "
        "and x86_64+cx16",
        which,
        target_.arch);
}

void Resolver::duplicate_definition(const Symbol& existing,
                                    std::string_view name,
                                    diag::SourceRange new_range) {
    auto d =
        diag::Diagnostic::error(std::format("duplicate definition of '{}'", name)).at(new_range);
    if (existing.definition_range.is_valid()) {
        d.with_note(diag::Diagnostic::note(std::format("previous definition of '{}' here", name))
                        .at(existing.definition_range));
    }
    reporter_->report(std::move(d));
}

// ============================================================================
// Pass 1: collect every top-level declaration into the global scope.
// ============================================================================

void Resolver::collect_decl(const ast::Decl& d) {
    switch (d.kind) {
    case ast::NodeKind::Func:
        collect_func(static_cast<const ast::FuncDecl&>(d));
        break;
    case ast::NodeKind::Struct:
        collect_struct(static_cast<const ast::StructDecl&>(d));
        break;
    case ast::NodeKind::Enum:
        collect_enum(static_cast<const ast::EnumDecl&>(d));
        break;
    case ast::NodeKind::Protocol:
        collect_protocol(static_cast<const ast::ProtocolDecl&>(d));
        break;
    case ast::NodeKind::Opaque:
        collect_opaque(static_cast<const ast::OpaqueDecl&>(d));
        break;
    case ast::NodeKind::Const:
        collect_const(static_cast<const ast::ConstDecl&>(d));
        break;
    case ast::NodeKind::Static:
        collect_static(static_cast<const ast::StaticDecl&>(d));
        break;
    case ast::NodeKind::Extension:
        // Extensions don't introduce a new top-level name; their members
        // are surfaced via their target type. We resolve them in pass 2.
        break;
    case ast::NodeKind::Derive:
        // `derive(...)` is sugar for empty extensions; nothing to bind.
        break;
    default:
        break;
    }
}

void Resolver::collect_top_level() {
    // §5 imported modules: collect each imported unit's top-level decls into
    // the global scope first, so references to them resolve in this unit. Their
    // bodies are not re-checked (resolve() only walks this unit's decls) and
    // they are not re-emitted (codegen iterates this unit's decls). Signatures
    // are re-derived here in this resolver's arena, so no cross-arena TypePtr
    // sharing is needed.
    for (const auto* imp : imported_units_) {
        if (imp == nullptr || imp->module == nullptr || imp->module->path.empty()) {
            continue;
        }
        // Index each imported module by its dotted `module` path. Imports are
        // referenced only through a qualified path (`util.math.add`), resolved
        // against this map in check_qualified_module_ref — they are *not*
        // collected into the unqualified global scope, so an imported name never
        // collides with (or shadows) a local one.
        std::string dotted;
        for (const auto& seg : imp->module->path) {
            if (!dotted.empty()) {
                dotted += '.';
            }
            dotted += seg;
        }
        imported_modules_[dotted] = imp;
    }
    for (const auto& d : unit_->decls) {
        // §12.6: skip `@when`-gated-out decls before they enter any scope.
        // Other passes (check_decl, codegen) will skip them the same way,
        // so a gated-out decl is effectively absent from the program.
        if (gated_out(*d)) {
            continue;
        }
        collect_decl(*d);
    }
}

void Resolver::collect_func(const ast::FuncDecl& f) {
    Symbol s;
    s.name = f.name;
    s.kind = SymbolKind::Func;
    s.decl = &f;
    s.type = function_type_of(f);
    s.definition_range = f.range;
    s.visibility = f.visibility;
    if (auto* prev = scopes_.global().insert(std::move(s))) {
        duplicate_definition(*prev, f.name, f.range);
    }
}

void Resolver::collect_struct(const ast::StructDecl& s_) {
    Symbol s;
    s.name = s_.name;
    s.kind = SymbolKind::Struct;
    s.decl = &s_;
    s.type = types_->make_nominal(TypeKind::Struct, &s_);
    s.definition_range = s_.range;
    s.visibility = s_.visibility;
    if (auto* prev = scopes_.global().insert(std::move(s))) {
        duplicate_definition(*prev, s_.name, s_.range);
    }
}

void Resolver::collect_enum(const ast::EnumDecl& e) {
    Symbol s;
    s.name = e.name;
    s.kind = SymbolKind::Enum;
    s.decl = &e;
    s.type = types_->make_nominal(TypeKind::Enum, &e);
    s.definition_range = e.range;
    s.visibility = e.visibility;
    if (auto* prev = scopes_.global().insert(std::move(s))) {
        duplicate_definition(*prev, e.name, e.range);
    }
}

void Resolver::collect_protocol(const ast::ProtocolDecl& p) {
    Symbol s;
    s.name = p.name;
    s.kind = SymbolKind::Protocol;
    s.decl = &p;
    s.type = types_->make_nominal(TypeKind::Protocol, &p);
    s.definition_range = p.range;
    s.visibility = p.visibility;
    if (auto* prev = scopes_.global().insert(std::move(s))) {
        duplicate_definition(*prev, p.name, p.range);
    }
}

void Resolver::collect_opaque(const ast::OpaqueDecl& o) {
    Symbol s;
    s.name = o.name;
    s.kind = SymbolKind::OpaqueType;
    s.decl = &o;
    s.type = types_->make_nominal(TypeKind::OpaqueType, &o);
    s.definition_range = o.range;
    s.visibility = o.visibility;
    if (auto* prev = scopes_.global().insert(std::move(s))) {
        duplicate_definition(*prev, o.name, o.range);
    }
}

void Resolver::collect_const(const ast::ConstDecl& c) {
    Symbol s;
    s.name = c.name;
    s.kind = SymbolKind::Const;
    s.decl = &c;
    s.type = c.type ? resolve_type(*c.type) : nullptr;
    s.definition_range = c.range;
    s.visibility = c.visibility;
    if (auto* prev = scopes_.global().insert(std::move(s))) {
        duplicate_definition(*prev, c.name, c.range);
    }
}

void Resolver::collect_static(const ast::StaticDecl& s_) {
    Symbol s;
    s.name = s_.name;
    s.kind = SymbolKind::Static;
    s.decl = &s_;
    s.type = s_.type ? resolve_type(*s_.type) : nullptr;
    s.definition_range = s_.range;
    s.visibility = s_.visibility;
    if (auto* prev = scopes_.global().insert(std::move(s))) {
        duplicate_definition(*prev, s_.name, s_.range);
    }
}

// ============================================================================
// Function-type construction
// ============================================================================

TypePtr Resolver::function_type_of(const ast::FuncDecl& f) {
    // §7 generics: the function's own type parameters need to resolve while
    // we walk its signature. We push a temporary scope holding each generic
    // param as an opaque GenericParam type, so `func max[T](a: T) -> T`
    // resolves T to its placeholder rather than failing with "unknown type".
    // The scope is popped at the end of this call.
    ScopeStack::Guard g(scopes_);
    for (const auto& gp : f.generics) {
        if (gp.name.empty() || gp.is_const) {
            continue;  // const generics aren't types; phase 1 ignores them
        }
        Symbol s;
        s.name = gp.name;
        s.kind = SymbolKind::GenericParam;
        s.type = types_->make_generic_param(gp.name);
        s.definition_range = gp.range;
        (void)g.scope().insert(std::move(s));
    }

    std::vector<TypePtr> params;
    params.reserve(f.params.size());
    for (const auto& p : f.params) {
        params.push_back(p.type ? resolve_type(*p.type) : types_->error());
    }
    TypePtr result = f.result ? resolve_type(*f.result) : types_->unit();
    // §9: a `throws(E)` clause widens the *external* result to Result<T, E>
    // so callers see the fallible signature; the body itself still talks
    // about T and E individually (handled in check_func via return_stack_
    // and throws_stack_).
    if (f.effects.throws_type) {
        TypePtr err = resolve_type(*f.effects.throws_type);
        result = types_->make_result(result, err);
    }
    return types_->make_function(std::move(params), result);
}

// ============================================================================
// Type-expression resolution (generic-instance argument binding)
// ============================================================================

std::optional<std::vector<TypePtr>>
Resolver::resolve_generic_instance_args(const std::vector<ast::GenericParam>& generics,
                                        const ast::NamedType& n,
                                        std::string_view kind,
                                        std::string_view name,
                                        diag::SourceRange range) {
    const std::size_t arity = named_type_param_count(generics);
    if (arity == 0) {
        if (n.has_generics) {
            error_at(range, std::format("'{}' is not a generic type", name));
        }
        return std::nullopt;
    }
    const std::size_t provided = n.type_args.size() + n.const_args.size();
    if (!n.has_generics || provided == 0) {
        error_at(range,
                 std::format("generic {} '{}' requires {} generic argument(s) written as '{}[...]'",
                             kind,
                             name,
                             arity,
                             name));
        return std::nullopt;
    }
    if (provided != arity) {
        error_at(range,
                 std::format("generic {} '{}' expects {} generic argument(s), got {}",
                             kind,
                             name,
                             arity,
                             provided));
    }
    // Walk the parameters in declaration order, drawing each from the
    // matching by-kind argument sequence: a type parameter consumes the
    // next type_arg, a const parameter the next const_arg (as a ConstValue).
    std::vector<TypePtr> args;
    args.reserve(arity);
    std::size_t ti = 0;
    std::size_t ci = 0;
    for (const auto& gp : generics) {
        if (gp.name.empty()) {
            continue;
        }
        if (gp.is_const) {
            args.push_back(ci < n.const_args.size() ? types_->make_const_value(n.const_args[ci++])
                                                    : types_->error());
        } else {
            args.push_back(ti < n.type_args.size() && n.type_args[ti] != nullptr
                               ? resolve_type(*n.type_args[ti++])
                               : types_->error());
        }
    }
    // §7 generics — enforce any `[T: P]` bounds on the type parameters
    // against the supplied arguments at the instantiation site.
    std::unordered_map<std::string, TypePtr> bindings;
    std::size_t bi = 0;
    for (const auto& gp : generics) {
        if (gp.name.empty()) {
            continue;
        }
        if (bi < args.size()) {
            bindings.emplace(gp.name, args[bi]);
        }
        ++bi;
    }
    check_generic_bounds(generics, bindings, range);
    return args;
}

// ============================================================================
// §12.6 @when gate
// ============================================================================

namespace {

// Each Decl subtype has its own `attributes` vector. Return a span into the
// right one, or nullptr if the kind doesn't carry attributes (Module,
// Import, Const, Static, Opaque, Derive in our current AST shape).
const std::vector<ast::Attribute>* decl_attributes(const ast::Decl& d) {
    switch (d.kind) {
    case ast::NodeKind::Func:
        return &static_cast<const ast::FuncDecl&>(d).attributes;
    case ast::NodeKind::Struct:
        return &static_cast<const ast::StructDecl&>(d).attributes;
    case ast::NodeKind::Enum:
        return &static_cast<const ast::EnumDecl&>(d).attributes;
    case ast::NodeKind::Protocol:
        return &static_cast<const ast::ProtocolDecl&>(d).attributes;
    case ast::NodeKind::Extension:
        return &static_cast<const ast::ExtensionDecl&>(d).attributes;
    default:
        return nullptr;
    }
}

}  // namespace

bool Resolver::gated_out(const ast::Decl& decl) {
    // Fast path: if we've already evaluated this decl's @when in an earlier
    // pass and stashed the verdict, reuse it. Otherwise check each `when`
    // attribute's predicate and remember the result.
    if (resolution_.is_gated_out(&decl)) {
        return true;
    }
    const auto* attrs = decl_attributes(decl);
    if (attrs == nullptr) {
        return false;
    }
    for (const auto& a : *attrs) {
        if (a.name != "when" || !a.predicate) {
            continue;
        }
        // The predicate is folded against the comptime env in its current
        // state — for top-level decls this is normally empty (consts haven't
        // been registered yet), so the predicate has to rely on cfg.* or
        // pure literals. That matches the §12.6 model.
        auto v = folder_.fold(*a.predicate, comptime_env_);
        if (v && v->kind == ComptimeValue::Kind::Bool && !v->b) {
            resolution_.mark_gated_out(&decl);
            return true;
        }
    }
    return false;
}

// ----- §12.4 declaration-macro expansion ---------------------------------

namespace {

// A declaration macro is a `comptime func` with a body that returns `[Decl]`
// (a length-less list of declarations). The body is folded by the comptime
// evaluator at expansion, so it can be any shape — a bare `return quote { … }`,
// or a loop accumulating into a `[Decl]` variable — not just a literal quote.
bool is_decl_macro(const ast::FuncDecl& fn) {
    if (!fn.is_comptime || fn.body == nullptr || fn.result == nullptr) {
        return false;
    }
    if (fn.result->kind != ast::NodeKind::VectorType) {
        return false;
    }
    const auto& vt = static_cast<const ast::VectorType&>(*fn.result);
    if (vt.element == nullptr || vt.element->kind != ast::NodeKind::NamedType) {
        return false;
    }
    const auto& nt = static_cast<const ast::NamedType&>(*vt.element);
    return !nt.path.empty() && nt.path.back() == "Decl";
}

// The mutable attribute list of an annotatable top-level decl (struct / func /
// enum), or nullptr for kinds that don't carry attributes.
std::vector<ast::Attribute>* decl_attributes(ast::Decl& d) {
    switch (d.kind) {
    case ast::NodeKind::Struct:
        return &static_cast<ast::StructDecl&>(d).attributes;
    case ast::NodeKind::Func:
        return &static_cast<ast::FuncDecl&>(d).attributes;
    case ast::NodeKind::Enum:
        return &static_cast<ast::EnumDecl&>(d).attributes;
    default:
        return nullptr;
    }
}

}  // namespace

void expand_declaration_macros(ast::CompilationUnit& unit, diag::DiagnosticReporter& rep) {
    // Collect declaration macros by name: a `comptime func` whose body is a
    // declaration-context quote.
    std::unordered_map<std::string, ast::FuncDecl*> macros;
    for (auto& d : unit.decls) {
        if (d->kind == ast::NodeKind::Func) {
            auto& fn = static_cast<ast::FuncDecl&>(*d);
            if (is_decl_macro(fn)) {
                macros[fn.name] = &fn;
            }
        }
    }
    if (macros.empty()) {
        return;
    }

    std::vector<ast::DeclPtr> out;
    out.reserve(unit.decls.size());
    // One folder for the whole unit so its `gensym()` counter is unique across
    // every macro application (§12.4 hygiene).
    ComptimeFolder folder;
    for (auto& d : unit.decls) {
        // Drop the macro definitions themselves (comptime-only).
        if (d->kind == ast::NodeKind::Func) {
            auto it = macros.find(static_cast<ast::FuncDecl&>(*d).name);
            if (it != macros.end() && it->second == &static_cast<ast::FuncDecl&>(*d)) {
                continue;
            }
        }
        // Find a macro attribute on this declaration.
        const std::vector<ast::Attribute>* attrs = decl_attributes(*d);
        ast::FuncDecl* macro = nullptr;
        if (attrs != nullptr) {
            for (const auto& a : *attrs) {
                auto it = macros.find(a.name);
                if (it != macros.end()) {
                    macro = it->second;
                    break;
                }
            }
        }
        if (macro == nullptr) {
            out.push_back(std::move(d));
            continue;
        }
        // Expand through the comptime folder: it folds the macro body with the
        // annotated decl bound to its `Decl` parameter and materializes the
        // `[Decl]` quote result, resolving `$d`, `$(d.name)`, computed
        // `$(expr)` splices, and `d.attribute(...)` reflection at fold time.
        // The macro's own attribute is left on the decl so reflection can read
        // it; the folder strips it from the `$d` clone. The annotated decl is
        // dropped — `$d` reproduces it as a freshly cloned subtree.
        auto expanded = folder.expand_decl_macro(*macro, *d);
        if (!expanded) {
            rep.report(
                diag::Diagnostic::error(std::format("declaration macro '@{}' did not expand to a "
                                                    "declaration list",
                                                    macro->name))
                    .at(d->range));
            out.push_back(std::move(d));
            continue;
        }
        for (auto& g : *expanded) {
            out.push_back(std::move(g));
        }
    }
    unit.decls = std::move(out);
}

void fold_extensions(ast::CompilationUnit& unit) {
    // Map each unit-local struct name to its decl so an extension's methods can
    // be appended. Built first so an `extension T` may precede `struct T`.
    std::unordered_map<std::string, ast::StructDecl*> structs;
    for (auto& d : unit.decls) {
        if (d->kind == ast::NodeKind::Struct) {
            auto& s = static_cast<ast::StructDecl&>(*d);
            structs[s.name] = &s;
        }
    }
    if (structs.empty()) {
        return;
    }

    std::vector<ast::DeclPtr> out;
    out.reserve(unit.decls.size());
    for (auto& d : unit.decls) {
        if (d->kind == ast::NodeKind::Extension) {
            auto& ext = static_cast<ast::ExtensionDecl&>(*d);
            // Only fold extensions whose target is a plain unit-local struct
            // name. (Conformances and non-struct / external targets are left
            // for their own handling — they pass through unchanged.)
            ast::StructDecl* target = nullptr;
            if (ext.target != nullptr && ext.target->kind == ast::NodeKind::NamedType) {
                const auto& nt = static_cast<const ast::NamedType&>(*ext.target);
                if (!nt.path.empty()) {
                    if (auto it = structs.find(nt.path.back()); it != structs.end()) {
                        target = it->second;
                    }
                }
            }
            if (target != nullptr && ext.conformances.empty()) {
                for (auto& m : ext.members) {
                    target->methods.push_back(std::move(m));
                }
                continue;  // drop the now-empty extension decl
            }
        }
        out.push_back(std::move(d));
    }
    unit.decls = std::move(out);
}

}  // namespace vestra::sema
