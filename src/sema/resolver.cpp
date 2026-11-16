// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "vestra/sema/resolver.hpp"

#include "vestra/ast/clone.hpp"
#include "vestra/ast/nodes.hpp"
#include "vestra/ast/printer.hpp"
#include "vestra/sema/builtins.hpp"
#include "vestra/sema/comptime.hpp"
#include "vestra/sema/scope.hpp"
#include "vestra/sema/types.hpp"

#include "resolver_internal.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <format>
#include <iterator>
#include <memory>
#include <set>
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
std::vector<const Symbol*> Resolution::param_symbols(const ast::FuncDecl* f) const {
    auto it = param_symbols_.find(f);
    return it == param_symbols_.end() ? std::vector<const Symbol*>{} : it->second;
}

void Resolution::add_param_symbol(const ast::FuncDecl* f, const Symbol* sym) {
    auto& v = param_symbols_[f];
    if (std::ranges::find(v, sym) == v.end()) {
        v.push_back(sym);
    }
}

const Symbol* Resolution::let_symbol(const ast::IfExpr* i) const {
    auto it = let_symbols_.find(i);
    return it == let_symbols_.end() ? nullptr : it->second;
}
void Resolution::set_let_symbol(const ast::IfExpr* i, const Symbol* sym) {
    let_symbols_[i] = sym;
}
const std::string* Resolution::qualified_name_of(const ast::Expr* e) const {
    auto it = qualified_names_.find(e);
    return it == qualified_names_.end() ? nullptr : &it->second;
}
void Resolution::set_qualified_name(const ast::Expr* e, std::string name) {
    qualified_names_[e] = std::move(name);
}
const std::vector<const Symbol*>* Resolution::view_captures_of(const ast::Expr* c) const {
    auto it = view_captures_.find(c);
    return it == view_captures_.end() ? nullptr : &it->second;
}
void Resolution::add_view_capture(const ast::Expr* c, const Symbol* sym) {
    auto& v = view_captures_[c];
    if (std::ranges::find(v, sym) == v.end()) {
        v.push_back(sym);
    }
}
Resolution::OrderingGuard Resolution::ordering_guard(const ast::Expr* e) const {
    auto it = ordering_guards_.find(e);
    return it == ordering_guards_.end() ? OrderingGuard::None : it->second;
}
void Resolution::set_ordering_guard(const ast::Expr* e, OrderingGuard g) {
    ordering_guards_[e] = g;
}
bool Resolution::cas_guarded(const ast::CallExpr* c) const {
    return cas_guarded_.contains(c);
}
void Resolution::mark_cas_guarded(const ast::CallExpr* c) {
    cas_guarded_.insert(c);
}

bool Resolution::is_optional_pattern(const ast::Pattern* p) const {
    return optional_patterns_.contains(p);
}

void Resolution::mark_optional_pattern(const ast::Pattern* p) {
    optional_patterns_.insert(p);
}

bool Resolution::is_value_pattern(const ast::Pattern* p) const {
    return value_patterns_.contains(p);
}

void Resolution::mark_value_pattern(const ast::Pattern* p) {
    value_patterns_.insert(p);
}

TypePtr Resolution::pattern_type(const ast::Pattern* p) const {
    auto it = pattern_types_.find(p);
    return it == pattern_types_.end() ? nullptr : it->second;
}

void Resolution::set_pattern_type(const ast::Pattern* p, TypePtr t) {
    pattern_types_[p] = std::move(t);
}

bool Resolution::needs_string_coercion(const ast::Expr* e) const {
    return string_coercions_.contains(e);
}
void Resolution::mark_string_coercion(const ast::Expr* e) {
    string_coercions_.insert(e);
}

void Resolver::note_string_coercion(const ast::Expr& e, TypePtr from, TypePtr to) {
    if (from == nullptr || to == nullptr || to->kind() != TypeKind::String) {
        return;
    }
    if (from->kind() == TypeKind::StrConst || from->kind() == TypeKind::Str) {
        resolution_.mark_string_coercion(&e);
    }
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
      folder_(&scopes_.global(), std::move(embed_reader)), target_(std::move(target)) {
    resolution_.set_types(types_);
    // §12.6 `cfg` describes the build target, not the compiler's host. A
    // `--target` arch sets arch and pointer width; `--no-libc` makes the OS
    // freestanding; the features are the --target-features list.
    BuildConfig cfg = BuildConfig::host();
    if (target_.arch == "aarch64" || target_.arch == "arm64") {
        cfg.arch = "arm64";
    } else if (target_.arch == "x86_64" || target_.arch == "riscv64") {
        cfg.arch = target_.arch;
    } else if (target_.arch == "riscv32" || target_.arch == "wasm32") {
        cfg.arch = target_.arch;
        cfg.pointer_bits = 32;
    }
    if (target_.arch != "host") {
        cfg.endian = "little";
        if (cfg.arch == "arm64" || cfg.arch == "x86_64" || cfg.arch == "riscv64") {
            cfg.pointer_bits = 64;
        }
    }
    if (target_.freestanding) {
        cfg.os = "freestanding";
    }
    cfg.profile = target_.profile;
    cfg.features = target_.features;
    folder_.set_config(std::move(cfg));
}

void Resolver::resolve() {
    register_builtin_capabilities();
    register_builtin_math();
    register_builtin_reflection();
    register_builtin_panic();
    // §A5 must follow register_builtin_reflection because the sync
    // intrinsics' signatures reference the Ordering / BarrierScope /
    // BarrierKind enums registered there.
    register_builtin_sync();
    register_builtin_io();
    check_attribute_names();
    synthesize_deinits();
    collect_top_level();
    check_bound_names();
    check_derive_names();
    for (const auto& d : unit_->decls) {
        // §12.6: same gate as collect_top_level. Skipping here keeps the
        // body-check pass aligned with the symbols actually registered.
        if (gated_out(*d)) {
            continue;
        }
        check_decl(*d);
    }
    check_unselected_branches();
    check_cpp_keyword_names();
    // Only a unit that resolved cleanly is ever imported (the driver stops on
    // the first error), and only then is re-resolving its member types
    // guaranteed silent.
    if (!reporter_->has_errors()) {
        record_member_types();
        mark_trivial_copy_structs();
    }
}

void Resolver::record_member_types() {
    for (const auto& d : unit_->decls) {
        if (gated_out(*d)) {
            continue;
        }
        if (d->kind == ast::NodeKind::Struct) {
            const auto& s = static_cast<const ast::StructDecl&>(*d);
            for (const auto& f : s.fields) {
                if (f.type != nullptr) {
                    types_->record_member_type(
                        f.type.get(), resolve_member_type_with_generics(s.generics, *f.type, {}));
                }
            }
        } else if (d->kind == ast::NodeKind::Enum) {
            const auto& e = static_cast<const ast::EnumDecl&>(*d);
            for (const auto& c : e.cases) {
                for (const auto& p : c.payload) {
                    if (p.second != nullptr) {
                        types_->record_member_type(
                            p.second.get(),
                            resolve_member_type_with_generics(e.generics, *p.second, {}));
                    }
                }
            }
        }
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
    // §5 imports are not collected into the unqualified scope — they are reached
    // only through a qualified path (`util.math.add`), resolved against
    // module_exports_ (set by set_module_exports before resolve()). So an
    // imported name never collides with or shadows a local one.
    // Nominal types first, so a function or static can name a type declared
    // later in the file (its signature is resolved as it is collected).
    auto is_type_decl = [](const ast::Decl& d) {
        return d.kind == ast::NodeKind::Struct || d.kind == ast::NodeKind::Enum
               || d.kind == ast::NodeKind::Opaque;
    };
    for (const bool types_pass : {true, false}) {
        for (const auto& d : unit_->decls) {
            // §12.6: skip `@when`-gated-out decls before they enter any scope.
            // Other passes (check_decl, codegen) will skip them the same way,
            // so a gated-out decl is effectively absent from the program.
            if (gated_out(*d) || is_type_decl(*d) != types_pass) {
                continue;
            }
            collect_decl(*d);
        }
    }
}

void Resolver::synthesize_deinits() {
    for (const auto& d : unit_->decls) {
        if (d->kind != ast::NodeKind::Struct) {
            continue;
        }
        auto& s = const_cast<ast::StructDecl&>(static_cast<const ast::StructDecl&>(*d));
        if (s.deinit_body == nullptr
            || std::ranges::any_of(s.methods, [](const ast::DeclPtr& m) {
                   return m->kind == ast::NodeKind::Func
                          && static_cast<const ast::FuncDecl&>(*m).is_deinit;
               })) {
            continue;
        }
        auto fn = std::make_unique<ast::FuncDecl>();
        fn->name = "deinit";
        fn->is_deinit = true;
        fn->recv_mode = ast::FuncDecl::RecvMode::Inout;
        fn->body = ast::clone(*s.deinit_body);
        fn->range = s.deinit_body->range;
        s.methods.push_back(std::move(fn));
    }
}

Resolver::ExportMap Resolver::public_exports() {
    // §5 harvest this unit's public top-level decls (after resolve()) so an
    // importer can reference them qualified. The Symbol — including its type,
    // resolved here in the shared arena — is copied; the copy stays valid for
    // importers because the arena outlives the build.
    ExportMap out;
    for (const auto& d : unit_->decls) {
        std::string name;
        ast::Visibility vis = ast::Visibility::Internal;
        switch (d->kind) {
        case ast::NodeKind::Func:
            name = static_cast<const ast::FuncDecl&>(*d).name;
            vis = static_cast<const ast::FuncDecl&>(*d).visibility;
            break;
        case ast::NodeKind::Struct:
            name = static_cast<const ast::StructDecl&>(*d).name;
            vis = static_cast<const ast::StructDecl&>(*d).visibility;
            break;
        case ast::NodeKind::Enum:
            name = static_cast<const ast::EnumDecl&>(*d).name;
            vis = static_cast<const ast::EnumDecl&>(*d).visibility;
            break;
        case ast::NodeKind::Const:
            name = static_cast<const ast::ConstDecl&>(*d).name;
            vis = static_cast<const ast::ConstDecl&>(*d).visibility;
            break;
        case ast::NodeKind::Static:
            // §5 a `public static` is reachable as `module.name` (a vector table
            // an init module installs, a shared atomic counter).
            name = static_cast<const ast::StaticDecl&>(*d).name;
            vis = static_cast<const ast::StaticDecl&>(*d).visibility;
            break;
        default:
            continue;
        }
        if (vis != ast::Visibility::Public || name.empty()) {
            continue;
        }
        if (const Symbol* s = scopes_.global().lookup(name)) {
            out[name] = *s;
        }
    }
    return out;
}

// §5 the builtin math names (`e`, `pi`, `tau`, `sin`, ...) sit in the global
// scope with no declaration; a top-level declaration of the same name replaces
// the builtin rather than colliding with it.
const Symbol* Resolver::insert_top_level(Symbol s) {
    std::string name = s.name;
    Symbol keep = s;
    const Symbol* prev = collect_scope().insert(std::move(s));
    if (prev != nullptr && prev->decl == nullptr
        && (prev->kind == SymbolKind::Const || prev->kind == SymbolKind::Func)
        && (builtins::lookup_const(name) || builtins::lookup_func(name))) {
        if (auto* slot = collect_scope().lookup_mutable(name)) {
            *slot = std::move(keep);
            return nullptr;
        }
    }
    return prev;
}

void Resolver::collect_func(const ast::FuncDecl& f) {
    Symbol s;
    s.name = f.name;
    s.kind = SymbolKind::Func;
    s.decl = &f;
    s.type = function_type_of(f);
    s.definition_range = f.range;
    s.visibility = f.visibility;
    Symbol keep = s;
    if (auto* prev = insert_top_level(std::move(s))) {
        // Annex §15.4: a Vestra `memcpy` / `memset` / `memmove` (the body the
        // compiler's own emitted calls bind to) replaces the builtin.
        if (prev->decl == nullptr && prev->kind == SymbolKind::Func
            && (f.name == "memcpy" || f.name == "memset" || f.name == "memmove")) {
            if (auto* slot = collect_scope().lookup_mutable(f.name)) {
                *slot = std::move(keep);
            }
            return;
        }
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
    if (auto* prev = insert_top_level(std::move(s))) {
        duplicate_definition(*prev, s_.name, s_.range);
    }
    if (target_.freestanding) {
        for (const auto& f : s_.fields) {
            reject_hosted_types(f.type.get());
        }
    }
}

void Resolver::reject_hosted_types(const ast::Type* t) {
    if (t == nullptr) {
        return;
    }
    switch (t->kind) {
    case ast::NodeKind::NamedType: {
        const auto& n = static_cast<const ast::NamedType&>(*t);
        if (n.path.size() == 1
            && (n.path[0] == "String" || n.path[0] == "Vec" || n.path[0] == "Box"
                || n.path[0] == "HashMap" || n.path[0] == "Channel")) {
            if (hosted_reported_.insert(t->range.begin.offset).second) {
                error_at(t->range,
                         std::format("`{}` needs the hosted runtime (a heap), which a --no-libc "
                                     "build doesn't have",
                                     n.path[0]));
            }
            return;
        }
        for (const auto& a : n.type_args) {
            reject_hosted_types(a.get());
        }
        return;
    }
    case ast::NodeKind::VectorType:
        reject_hosted_types(static_cast<const ast::VectorType&>(*t).element.get());
        return;
    case ast::NodeKind::OptionalType:
        reject_hosted_types(static_cast<const ast::OptionalType&>(*t).inner.get());
        return;
    case ast::NodeKind::TupleType:
        for (const auto& el : static_cast<const ast::TupleType&>(*t).elements) {
            reject_hosted_types(el.get());
        }
        return;
    default:
        return;
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
    if (auto* prev = insert_top_level(std::move(s))) {
        duplicate_definition(*prev, e.name, e.range);
    }
    if (target_.freestanding) {
        for (const auto& c : e.cases) {
            for (const auto& [label, ty] : c.payload) {
                reject_hosted_types(ty.get());
            }
        }
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
    if (auto* prev = insert_top_level(std::move(s))) {
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
    if (auto* prev = insert_top_level(std::move(s))) {
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
    if (auto* prev = insert_top_level(std::move(s))) {
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
    if (auto* prev = insert_top_level(std::move(s))) {
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
        s.generic = &gp;
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
    return types_->make_function(std::move(params), result, row_of(f.effects, f.is_async));
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

// §12.6 rule 1: every branch of a configuration conditional is type-checked,
// not only the one the build selects. The unselected declarations are grouped
// by their `@when` predicates (same text, same configuration); each group is
// collected into a scope that shadows the global one, so it sees its own
// siblings (a `@when(x86_64)` helper, the x86 `TrapFrame`) over the selected
// declarations of the same names, and then checked there.
void Resolver::check_unselected_branches() {
    std::vector<std::string> keys;
    std::vector<std::vector<const ast::Decl*>> groups;
    ast::Printer printer;
    for (const auto& d : unit_->decls) {
        if (!resolution_.is_gated_out(d.get())) {
            continue;
        }
        std::string key;
        if (const auto* attrs = decl_attributes(*d)) {
            for (const auto& a : *attrs) {
                if (a.name == "when" && a.predicate) {
                    key += printer.expr_text(*a.predicate);
                    key += ";";
                }
            }
        }
        auto it = std::ranges::find(keys, key);
        if (it == keys.end()) {
            keys.push_back(key);
            groups.emplace_back();
            groups.back().push_back(d.get());
        } else {
            groups[static_cast<std::size_t>(it - keys.begin())].push_back(d.get());
        }
    }
    auto is_type_decl = [](const ast::Decl& d) {
        return d.kind == ast::NodeKind::Struct || d.kind == ast::NodeKind::Enum
               || d.kind == ast::NodeKind::Opaque;
    };
    auto name_of = [](const ast::Decl& d) -> std::string_view {
        switch (d.kind) {
        case ast::NodeKind::Func:
            return static_cast<const ast::FuncDecl&>(d).name;
        case ast::NodeKind::Struct:
            return static_cast<const ast::StructDecl&>(d).name;
        case ast::NodeKind::Enum:
            return static_cast<const ast::EnumDecl&>(d).name;
        case ast::NodeKind::Opaque:
            return static_cast<const ast::OpaqueDecl&>(d).name;
        case ast::NodeKind::Protocol:
            return static_cast<const ast::ProtocolDecl&>(d).name;
        default:
            return {};
        }
    };
    for (const auto& group : groups) {
        ScopeStack::Guard overlay(scopes_);
        collect_scope_ = &overlay.scope();
        for (const bool types_pass : {true, false}) {
            for (const auto* d : group) {
                if (is_type_decl(*d) == types_pass) {
                    collect_decl(*d);
                }
            }
            // Rule 3 admits declarations gated by a weaker predicate; without
            // implication between predicates, any other unselected declaration
            // whose name nothing else defines is visible too.
            for (const auto& other : groups) {
                if (&other == &group) {
                    continue;
                }
                for (const auto* d : other) {
                    auto n = name_of(*d);
                    if (is_type_decl(*d) == types_pass && !n.empty()
                        && scopes_.current().lookup(n) == nullptr) {
                        collect_decl(*d);
                    }
                }
            }
        }
        collect_scope_ = nullptr;
        for (const auto* d : group) {
            check_decl(*d);
        }
    }
}

// ----- attribute names ----------------------------------------------------

namespace {

// The attributes the compiler acts on.
constexpr std::array<std::string_view, 20> kKnownAttributes{
    "when",     "repr",      "inline",    "visibility",      "stack_protector",
    "section",  "symbol",    "weak",      "alias",           "extern",
    "naked",    "boot",      "interrupt", "kernel_init",     "panic_handler",
    "no_auto_barrier",       "noinit",    "align",           "bits",
    "retry_loop"};

// Attributes the spec defines that this compiler does not implement yet. They
// are rejected rather than ignored, so code written against them doesn't
// silently lose its meaning.
constexpr std::array<std::string_view, 5> kUnimplementedAttributes{
    "deterministic", "allow_weak_cas", "retroactive", "volatile", "padding"};

void collect_attributes(const ast::Decl& d, std::vector<const ast::Attribute*>& out) {
    auto add = [&](const std::vector<ast::Attribute>& as) {
        for (const auto& a : as) {
            out.push_back(&a);
        }
    };
    switch (d.kind) {
    case ast::NodeKind::Func:
        add(static_cast<const ast::FuncDecl&>(d).attributes);
        break;
    case ast::NodeKind::Struct: {
        const auto& s = static_cast<const ast::StructDecl&>(d);
        add(s.attributes);
        for (const auto& f : s.fields) {
            add(f.attributes);
        }
        for (const auto& m : s.methods) {
            collect_attributes(*m, out);
        }
        break;
    }
    case ast::NodeKind::Enum: {
        const auto& e = static_cast<const ast::EnumDecl&>(d);
        add(e.attributes);
        for (const auto& m : e.methods) {
            collect_attributes(*m, out);
        }
        break;
    }
    case ast::NodeKind::Protocol:
        add(static_cast<const ast::ProtocolDecl&>(d).attributes);
        break;
    case ast::NodeKind::Extension: {
        const auto& x = static_cast<const ast::ExtensionDecl&>(d);
        add(x.attributes);
        for (const auto& m : x.members) {
            collect_attributes(*m, out);
        }
        break;
    }
    case ast::NodeKind::Static:
        add(static_cast<const ast::StaticDecl&>(d).attributes);
        break;
    default:
        break;
    }
}

// The bounds the checker verifies (`Ord` and `Hashable` are the spec's
// spellings of `Comparable` and `Hash`).
constexpr std::array<std::string_view, 10> kKnownBounds{"Display", "Default", "Eq", "Hash",
                                                        "Hashable", "Debug", "Clone",
                                                        "Comparable", "Ordered", "Ord"};

// Bounds the spec defines that the checker can't verify yet.
constexpr std::array<std::string_view, 4> kUnimplementedBounds{"Primitive", "Trivial",
                                                               "Copyable", "Iterator"};

void collect_generics(const ast::Decl& d, std::vector<const ast::GenericParam*>& out) {
    auto add = [&](const std::vector<ast::GenericParam>& gs) {
        for (const auto& g : gs) {
            out.push_back(&g);
        }
    };
    switch (d.kind) {
    case ast::NodeKind::Func:
        add(static_cast<const ast::FuncDecl&>(d).generics);
        break;
    case ast::NodeKind::Struct: {
        const auto& s = static_cast<const ast::StructDecl&>(d);
        add(s.generics);
        for (const auto& m : s.methods) {
            collect_generics(*m, out);
        }
        break;
    }
    case ast::NodeKind::Enum: {
        const auto& e = static_cast<const ast::EnumDecl&>(d);
        add(e.generics);
        for (const auto& m : e.methods) {
            collect_generics(*m, out);
        }
        break;
    }
    case ast::NodeKind::Protocol: {
        const auto& p = static_cast<const ast::ProtocolDecl&>(d);
        add(p.generics);
        for (const auto& m : p.requirements) {
            collect_generics(*m, out);
        }
        break;
    }
    case ast::NodeKind::Extension:
        for (const auto& m : static_cast<const ast::ExtensionDecl&>(d).members) {
            collect_generics(*m, out);
        }
        break;
    default:
        break;
    }
}

}  // namespace

void Resolver::check_bound_names() {
    std::vector<const ast::GenericParam*> generics;
    for (const auto& d : unit_->decls) {
        collect_generics(*d, generics);
    }
    std::vector<const ast::NamedType*> bounds;
    for (const auto* g : generics) {
        if (g->is_const) {
            continue;
        }
        for (const auto& b : g->bound.protocols) {
            if (b != nullptr && b->kind == ast::NodeKind::NamedType) {
                bounds.push_back(static_cast<const ast::NamedType*>(b.get()));
            }
        }
    }
    // Source order: a `where` clause folds its bounds into the parameter list.
    std::ranges::stable_sort(bounds, {}, [](const ast::NamedType* t) {
        return t->range.begin.offset;
    });
    for (const auto* t : bounds) {
        if (t->path.size() != 1) {
            continue;  // a module-qualified protocol, resolved with the module
        }
        const std::string& name = t->path.front();
        const auto is = [&](std::string_view n) { return n == name; };
        if (std::ranges::any_of(kKnownBounds, is)) {
            continue;
        }
        if (const auto* sym = scopes_.global().lookup(name);
            sym != nullptr && sym->kind == SymbolKind::Protocol) {
            continue;
        }
        if (std::ranges::any_of(kUnimplementedBounds, is)) {
            error_at(t->range, std::format("bound '{}' is not supported yet", name));
        } else {
            error_at(t->range, std::format("unknown protocol '{}'", name));
        }
    }
}

void Resolver::check_derive_names() {
    // The derives the emitter implements.
    constexpr std::array<std::string_view, 7> known{"Eq", "Ord", "Hash", "Clone",
                                                    "Debug", "Display", "Default"};
    for (const auto& d : unit_->decls) {
        if (d->kind != ast::NodeKind::Derive) {
            continue;
        }
        for (const auto& p : static_cast<const ast::DeriveDecl&>(*d).protocols) {
            if (p == nullptr || p->kind != ast::NodeKind::NamedType) {
                continue;
            }
            const auto& nt = static_cast<const ast::NamedType&>(*p);
            if (nt.path.size() != 1) {
                continue;
            }
            const std::string& name = nt.path.front();
            if (std::ranges::any_of(known, [&](std::string_view n) { return n == name; })) {
                continue;
            }
            // A user protocol's reflective default is a spec feature this
            // compiler doesn't have yet.
            const auto* sym = scopes_.global().lookup(name);
            if (sym != nullptr && sym->kind == SymbolKind::Protocol) {
                error_at(p->range, std::format("derive({}) is not supported yet", name));
            } else {
                error_at(p->range, std::format("unknown derive '{}'", name));
            }
        }
    }
}

void Resolver::check_attribute_names() {
    std::vector<const ast::Attribute*> attrs;
    std::vector<std::string_view> macros(unit_->macro_names.begin(), unit_->macro_names.end());
    for (const auto& d : unit_->decls) {
        collect_attributes(*d, attrs);
        // A unit resolved without macro expansion still holds its macros.
        if (d->kind == ast::NodeKind::Func) {
            const auto& fn = static_cast<const ast::FuncDecl&>(*d);
            if (fn.is_comptime) {
                macros.push_back(fn.name);
            }
        }
    }
    // Source order, so a struct's fields and methods report as written.
    std::ranges::stable_sort(attrs, {}, [](const ast::Attribute* a) {
        return a->range.begin.offset;
    });
    for (const auto* a : attrs) {
        const auto is = [&](std::string_view n) { return n == a->name; };
        if (std::ranges::any_of(kKnownAttributes, is)
            || std::ranges::any_of(macros, is)) {
            continue;
        }
        if (std::ranges::any_of(kUnimplementedAttributes, is)) {
            error_at(a->range, std::format("attribute '@{}' is not supported yet", a->name));
        } else {
            error_at(a->range, std::format("unknown attribute '@{}'", a->name));
        }
    }
    for (const auto* a : attrs) {
        if (a->name == "when" && a->predicate != nullptr) {
            check_cfg_names(*a->predicate);
        } else if (a->name == "repr") {
            check_repr_kind(a->predicate.get());
            for (const auto& x : a->extra_args) {
                check_repr_kind(x.get());
            }
        }
    }
}

void Resolver::check_repr_kind(const ast::Expr* e) {
    // §6 layout kinds: `C`, `packed`, `union`, `align(N)`; `wire` (fixed byte
    // order) is spec-defined but would emit a plain struct.
    std::string_view name;
    diag::SourceRange range;
    if (e == nullptr) {
        return;
    }
    if (e->kind == ast::NodeKind::IdentExpr) {
        name = static_cast<const ast::IdentExpr&>(*e).name;
        range = e->range;
    } else if (e->kind == ast::NodeKind::CallExpr) {
        const auto& c = static_cast<const ast::CallExpr&>(*e);
        if (c.callee == nullptr || c.callee->kind != ast::NodeKind::IdentExpr) {
            return;
        }
        name = static_cast<const ast::IdentExpr&>(*c.callee).name;
        range = c.callee->range;
        if (name == "align") {
            return;
        }
    } else {
        return;
    }
    if (name == "C" || name == "packed" || name == "union") {
        return;
    }
    if (name == "wire") {
        error_at(range, "@repr(wire) is not supported yet");
    } else {
        error_at(range, std::format("unknown @repr kind '{}'", name));
    }
}

namespace {

// `cfg.X`, or nullptr.
const ast::MemberExpr* cfg_member(const ast::Expr& e) {
    if (e.kind != ast::NodeKind::MemberExpr) {
        return nullptr;
    }
    const auto& m = static_cast<const ast::MemberExpr&>(e);
    if (m.base == nullptr || m.base->kind != ast::NodeKind::IdentExpr
        || static_cast<const ast::IdentExpr&>(*m.base).name != "cfg") {
        return nullptr;
    }
    return &m;
}

// The cases of a `cfg` field with a closed enumeration (§12.6), or empty.
std::vector<std::string_view> cfg_cases(std::string_view field) {
    if (field == "arch") {
        return {"x86_64", "arm64", "riscv64", "riscv32", "wasm32"};
    }
    if (field == "os") {
        return {"vectra", "linux", "macos", "windows", "freestanding"};
    }
    if (field == "endian") {
        return {"little", "big"};
    }
    return {};
}

}  // namespace

void Resolver::check_cfg_names(const ast::Expr& e) {
    // There is no package manifest yet, so the features `cfg.has` can name are
    // the ISA features the compiler itself acts on (§14.9.4).
    constexpr std::array<std::string_view, 2> features{"lse2", "cx16"};
    constexpr std::array<std::string_view, 7> fields{"arch", "os", "endian", "pointerBits",
                                                     "profile", "has", "option"};
    switch (e.kind) {
    case ast::NodeKind::MemberExpr:
        if (const auto* m = cfg_member(e)) {
            if (!std::ranges::contains(fields, std::string_view{m->member})) {
                error_at(m->range, std::format("unknown cfg field '{}'", m->member));
            }
        }
        return;
    case ast::NodeKind::CallExpr: {
        const auto& c = static_cast<const ast::CallExpr&>(e);
        const auto* m = c.callee != nullptr ? cfg_member(*c.callee) : nullptr;
        if (m != nullptr && m->member == "has" && c.args.size() == 1 && c.args[0].value != nullptr
            && c.args[0].value->kind == ast::NodeKind::StringLit) {
            const auto& lit = static_cast<const ast::StringLit&>(*c.args[0].value);
            if (!std::ranges::contains(features, std::string_view{lit.text})) {
                error_at(lit.range, std::format("unknown cfg feature '{}'", lit.text));
            }
        }
        return;
    }
    case ast::NodeKind::ParenExpr:
        check_cfg_names(*static_cast<const ast::ParenExpr&>(e).inner);
        return;
    case ast::NodeKind::UnaryExpr:
        check_cfg_names(*static_cast<const ast::UnaryExpr&>(e).operand);
        return;
    case ast::NodeKind::BinaryExpr: {
        const auto& b = static_cast<const ast::BinaryExpr&>(e);
        if (b.op == ast::BinaryOp::Eq || b.op == ast::BinaryOp::Ne) {
            // `cfg.os == .linux`, either way round.
            const ast::MemberExpr* m = cfg_member(*b.lhs);
            const ast::Expr* other = b.rhs.get();
            if (m == nullptr) {
                m = cfg_member(*b.rhs);
                other = b.lhs.get();
            }
            if (m != nullptr && other != nullptr && other->kind == ast::NodeKind::LeadingDotExpr) {
                check_cfg_names(*m);
                const auto& dot = static_cast<const ast::LeadingDotExpr&>(*other);
                const auto cases = cfg_cases(m->member);
                if (!cases.empty() && !std::ranges::contains(cases, std::string_view{dot.name})) {
                    error_at(dot.range,
                             std::format("unknown cfg.{} value '.{}'", m->member, dot.name));
                }
                return;
            }
        }
        check_cfg_names(*b.lhs);
        check_cfg_names(*b.rhs);
        return;
    }
    default:
        return;
    }
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
    // A parse error can leave a quote body half-built; running a macro over
    // it would fold a malformed tree. The errors already stop the build.
    if (rep.has_errors()) {
        return;
    }
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
    for (const auto& d : unit.decls) {
        if (d->kind == ast::NodeKind::Func) {
            const auto& fn = static_cast<const ast::FuncDecl&>(*d);
            if (is_decl_macro(fn)) {
                unit.macro_names.push_back(fn.name);
            }
        }
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
        if (auto errs = folder.take_macro_errors(); !errs.empty()) {
            // A splice inside a loop fails once per iteration; say it once.
            std::vector<std::pair<std::size_t, std::string>> said;
            for (const auto& me : errs) {
                std::pair<std::size_t, std::string> key{me.range.begin.offset, me.message};
                if (std::ranges::find(said, key) != said.end()) {
                    continue;
                }
                said.push_back(std::move(key));
                rep.report(diag::Diagnostic::error(std::format("declaration macro '@{}': {}",
                                                               macro->name,
                                                               me.message))
                               .at(me.range));
            }
            out.push_back(std::move(d));
            continue;
        }
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

namespace vestra::sema {

namespace {

// The C++ keywords and alternative tokens (C++26). A Vestra name spelled as one
// lowers verbatim, and the C++ doesn't compile.
constexpr std::string_view kCppKeywords[] = {
    "alignas",      "alignof",      "and",          "and_eq",      "asm",
    "auto",         "bitand",       "bitor",        "bool",        "break",
    "case",         "catch",        "char",         "char8_t",     "char16_t",
    "char32_t",     "class",        "co_await",     "co_return",   "co_yield",
    "compl",        "concept",      "const",        "consteval",   "constexpr",
    "constinit",    "const_cast",   "continue",     "contract_assert", "decltype",
    "default",      "delete",       "do",           "double",      "dynamic_cast",
    "else",         "enum",         "explicit",     "export",      "extern",
    "false",        "float",        "for",          "friend",      "goto",
    "if",           "inline",       "int",          "long",        "mutable",
    "namespace",    "new",          "noexcept",     "not",         "not_eq",
    "nullptr",      "operator",     "or",           "or_eq",       "private",
    "protected",    "public",       "register",     "reinterpret_cast", "requires",
    "return",       "short",        "signed",       "sizeof",      "static",
    "static_assert", "static_cast", "struct",       "switch",      "template",
    "this",         "thread_local", "throw",        "true",        "try",
    "typedef",      "typeid",       "typename",     "union",       "unsigned",
    "using",        "virtual",      "void",         "volatile",    "wchar_t",
    "while",        "xor",          "xor_eq",
};

}  // namespace

bool Resolver::is_cpp_keyword(std::string_view name) {
    return std::ranges::find(kCppKeywords, name) != std::end(kCppKeywords);
}

// §3 a Vestra name may be any identifier, but the C++ backend writes names
// verbatim, so one spelled as a C++ keyword (`class`, `mutable`, `signed`)
// would not compile. Until the backend renames them, say so here, once per
// declaration, in source order.
void Resolver::check_cpp_keyword_names() {
    std::set<diag::FileId> files;
    for (const auto& d : unit_->decls) {
        if (d->range.is_valid()) {
            files.insert(d->range.begin.file);
        }
    }
    std::vector<std::pair<diag::SourceRange, std::string>> hits;
    auto note = [&](std::string_view name, diag::SourceRange r) {
        if (r.is_valid() && files.contains(r.begin.file) && is_cpp_keyword(name)) {
            hits.emplace_back(r, std::string{name});
        }
    };
    scopes_.for_each_symbol([&](const Symbol& s) { note(s.name, s.definition_range); });
    auto methods = [&](const std::vector<ast::DeclPtr>& ms) {
        for (const auto& m : ms) {
            if (m != nullptr && m->kind == ast::NodeKind::Func) {
                note(static_cast<const ast::FuncDecl&>(*m).name, m->range);
            }
        }
    };
    for (const auto& d : unit_->decls) {
        if (gated_out(*d)) {
            continue;
        }
        if (d->kind == ast::NodeKind::Struct) {
            const auto& sd = static_cast<const ast::StructDecl&>(*d);
            for (const auto& f : sd.fields) {
                note(f.name, f.range);
            }
            methods(sd.methods);
        } else if (d->kind == ast::NodeKind::Enum) {
            const auto& ed = static_cast<const ast::EnumDecl&>(*d);
            // A payload enum's cases lower to `<case>_t` structs; only an
            // `enum class` spells a case bare.
            const bool bare = std::ranges::all_of(ed.cases, [](const auto& c) {
                return c.payload.empty();
            });
            for (const auto& c : ed.cases) {
                if (bare) {
                    note(c.name, c.range);
                }
                for (const auto& [label, t] : c.payload) {
                    note(label, c.range);
                }
            }
            methods(ed.methods);
        }
    }
    std::ranges::sort(hits, [](const auto& a, const auto& b) {
        if (a.first.begin.offset != b.first.begin.offset) {
            return a.first.begin.offset < b.first.begin.offset;
        }
        return a.second < b.second;
    });
    std::set<std::pair<std::size_t, std::string>> seen;
    for (const auto& [r, name] : hits) {
        if (seen.emplace(r.begin.offset, name).second) {
            error_at(r,
                     std::format("'{}' is a C++ keyword, and the C++ backend writes names as "
                                 "they are, so it can't name anything yet; rename it",
                                 name));
        }
    }
}

// §5 implicit copies are inserted only for Trivial types: the scalars, and
// "small fixed structs of Trivial fields". A struct qualifies when it is not
// generic, not linear, has no `deinit`, and every field is a scalar, a
// `StrConst`, a raw pointer or another such struct, in 16 bytes or less (two
// registers). Anything else still moves.
void Resolver::mark_trivial_copy_structs() {
    std::vector<const ast::StructDecl*> cands;
    for (const auto& d : unit_->decls) {
        if (d->kind != ast::NodeKind::Struct || gated_out(*d)) {
            continue;
        }
        const auto* sd = static_cast<const ast::StructDecl*>(d.get());
        if (sd->generics.empty() && !sd->is_linear && sd->deinit_body == nullptr) {
            cands.push_back(sd);
        }
    }
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto* sd : cands) {
            if (sd->trivial_copy) {
                continue;
            }
            std::int64_t size = 0;
            std::int64_t align = 1;
            bool ok = true;
            for (const auto& f : sd->fields) {
                const auto l = f.type != nullptr ? trivial_copy_layout(resolve_type(*f.type), 0)
                                                 : ByteLayout{};
                if (l.align == 0) {
                    ok = false;
                    break;
                }
                size = (size + l.align - 1) / l.align * l.align + l.size;
                align = std::max(align, l.align);
            }
            size = (size + align - 1) / align * align;
            if (ok && size <= 16) {
                sd->trivial_copy = true;
                changed = true;
            }
        }
    }
}

// The layout of a field that keeps its struct implicitly copyable, or a zero
// alignment when it doesn't. An empty struct is zero bytes, aligned to one.
ByteLayout Resolver::trivial_copy_layout(TypePtr t, int depth) {
    if (t == nullptr || depth > 32) {
        return {};
    }
    switch (t->kind()) {
    case TypeKind::Int8:
    case TypeKind::Int16:
    case TypeKind::Int32:
    case TypeKind::Int64:
    case TypeKind::Int:
    case TypeKind::UInt8:
    case TypeKind::UInt16:
    case TypeKind::UInt32:
    case TypeKind::UInt64:
    case TypeKind::UInt:
    case TypeKind::Int128:
    case TypeKind::UInt128:
    case TypeKind::Float32:
    case TypeKind::Float64:
    case TypeKind::Bool:
    case TypeKind::Char:
        return primitive_byte_layout(t->kind());
    case TypeKind::StrConst:
        return {16, 8};
    case TypeKind::Ptr:
    case TypeKind::MutPtr:
        return {8, 8};
    case TypeKind::Struct: {
        const auto* d = t->nominal_decl();
        if (d == nullptr || d->kind != ast::NodeKind::Struct) {
            return {};
        }
        const auto& sd = static_cast<const ast::StructDecl&>(*d);
        if (!sd.trivial_copy) {
            return {};
        }
        std::int64_t size = 0;
        std::int64_t align = 1;
        for (const auto& f : sd.fields) {
            const auto l = trivial_copy_layout(resolve_type(*f.type), depth + 1);
            if (l.align == 0) {
                return {};
            }
            size = (size + l.align - 1) / l.align * l.align + l.size;
            align = std::max(align, l.align);
        }
        return {(size + align - 1) / align * align, align};
    }
    default:
        return {};
    }
}

}  // namespace vestra::sema
