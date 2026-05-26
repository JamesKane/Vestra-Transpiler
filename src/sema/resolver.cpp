// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "vestra/sema/resolver.hpp"

#include "vestra/ast/nodes.hpp"
#include "vestra/sema/builtins.hpp"
#include "vestra/sema/scope.hpp"
#include "vestra/sema/types.hpp"

#include <array>
#include <cstdlib>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vestra::sema {

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
                   ComptimeFolder::EmbedReader embed_reader)
    : unit_(&unit), types_(&types), reporter_(&reporter),
      folder_(&scopes_.global(), std::move(embed_reader)) {}

void Resolver::resolve() {
    register_builtin_capabilities();
    register_builtin_math();
    register_builtin_reflection();
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

// The standard `using` capabilities from §8 — they aren't user-declarable
// types yet (no real stdlib), but a function that writes `using Alloc` must
// not get an "unknown type" error for what is a load-bearing language
// primitive. We register each as a nominal Protocol-kind symbol with a
// placeholder type; the capability checker matches them by name.
void Resolver::register_builtin_capabilities() {
    static constexpr std::array<std::string_view, 10> Names = {
        "Alloc",
        "Log",
        "Async",
        "Net",
        "Clock",
        "Rand",
        "Mmio",
        "Asm",
        "RawMemory",
        "Extern",
    };
    for (auto name : Names) {
        Symbol s;
        s.name = std::string{name};
        s.kind = SymbolKind::Protocol;
        s.decl = nullptr;
        s.type = types_->make_nominal(TypeKind::Protocol, nullptr);
        s.definition_range = {};
        s.visibility = ast::Visibility::Public;
        (void)scopes_.global().insert(std::move(s));
    }
}

// §12.1 comptime stdlib. The folder dispatches each of these by name at
// fold time (see builtins::call / builtins::value_of); here we only
// register the symbols so the resolver can type-check a `sin(theta)` or
// a `tau` reference without flagging an undeclared identifier. Phase 1
// keeps every signature Float64-only; later phases revisit polymorphism.
void Resolver::register_builtin_math() {
    auto f64 = types_->primitive(TypeKind::Float64);

    // Constants first — they're plain Const symbols, just typed Float64.
    // We don't pre-populate comptime_env_ here: the folder consults
    // builtins::lookup_const directly when an ident lookup misses, so
    // the symbol's only role is satisfying the resolver.
    for (const auto& c : builtins::constants()) {
        Symbol s;
        s.name = std::string{c.name};
        s.kind = SymbolKind::Const;
        s.decl = nullptr;
        s.type = f64;
        s.definition_range = {};
        s.visibility = ast::Visibility::Public;
        (void)scopes_.global().insert(std::move(s));
    }

    // Functions: each registers as a Func symbol whose type is the right
    // (Float64, ...) -> Float64 signature. Calls into these go through
    // the resolver's normal call-typing path because the function type
    // is already correct.
    for (const auto& f : builtins::functions()) {
        std::vector<TypePtr> params(f.arity, f64);
        Symbol s;
        s.name = std::string{f.name};
        s.kind = SymbolKind::Func;
        s.decl = nullptr;
        s.type = types_->make_function(std::move(params), f64);
        s.definition_range = {};
        s.visibility = ast::Visibility::Public;
        (void)scopes_.global().insert(std::move(s));
    }
}

// §12.2 reflection: synthesize the `Type` and `Field` structs so
// `T.fields[i].name` and `T.fields[i].type.name` type-check through
// the resolver's ordinary lookup_field path. Both are comptime-only —
// there's no codegen story for a runtime Type/Field-typed value yet,
// and the spec reads reflection as a fold-time activity. Later
// phases extend Field with `offset` + the attribute list.
//
// Type carries `name: Str` (the type's display name). Field carries
// `name: Str` then `type: Type`, in *that* declaration order — the
// folder reads off the same indices when serving member access on a
// folded Field value (elements[0]=name, elements[1]=type).
void Resolver::register_builtin_reflection() {
    // Build `Type` first so Field's `type: Type` field can resolve
    // against the freshly-registered nominal.
    {
        auto str_type = std::make_unique<ast::NamedType>();
        str_type->path = {"Str"};

        auto decl = std::make_unique<ast::StructDecl>();
        decl->name = "Type";
        decl->visibility = ast::Visibility::Public;
        ast::StructDecl::Field name_field;
        name_field.name = "name";
        name_field.kind = ast::StructDecl::Field::Kind::Let;
        name_field.type = std::move(str_type);
        decl->fields.push_back(std::move(name_field));
        builtin_type_decl_ = std::move(decl);

        Symbol s;
        s.name = "Type";
        s.kind = SymbolKind::Struct;
        s.decl = builtin_type_decl_.get();
        s.type = types_->make_nominal(TypeKind::Struct, builtin_type_decl_.get());
        s.definition_range = {};
        s.visibility = ast::Visibility::Public;
        (void)scopes_.global().insert(std::move(s));
    }

    {
        auto str_type = std::make_unique<ast::NamedType>();
        str_type->path = {"Str"};
        auto type_type = std::make_unique<ast::NamedType>();
        type_type->path = {"Type"};

        auto decl = std::make_unique<ast::StructDecl>();
        decl->name = "Field";
        decl->visibility = ast::Visibility::Public;
        ast::StructDecl::Field name_field;
        name_field.name = "name";
        name_field.kind = ast::StructDecl::Field::Kind::Let;
        name_field.type = std::move(str_type);
        decl->fields.push_back(std::move(name_field));
        ast::StructDecl::Field type_field;
        type_field.name = "type";
        type_field.kind = ast::StructDecl::Field::Kind::Let;
        type_field.type = std::move(type_type);
        decl->fields.push_back(std::move(type_field));
        builtin_field_decl_ = std::move(decl);

        Symbol s;
        s.name = "Field";
        s.kind = SymbolKind::Struct;
        s.decl = builtin_field_decl_.get();
        s.type = types_->make_nominal(TypeKind::Struct, builtin_field_decl_.get());
        s.definition_range = {};
        s.visibility = ast::Visibility::Public;
        (void)scopes_.global().insert(std::move(s));
    }
}

void Resolver::error_at(diag::SourceRange r, std::string msg) {
    reporter_->report(diag::Diagnostic::error(std::move(msg)).at(r));
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

void Resolver::collect_top_level() {
    for (const auto& d : unit_->decls) {
        // §12.6: skip `@when`-gated-out decls before they enter any scope.
        // Other passes (check_decl, codegen) will skip them the same way,
        // so a gated-out decl is effectively absent from the program.
        if (gated_out(*d)) {
            continue;
        }
        switch (d->kind) {
        case ast::NodeKind::Func:
            collect_func(static_cast<const ast::FuncDecl&>(*d));
            break;
        case ast::NodeKind::Struct:
            collect_struct(static_cast<const ast::StructDecl&>(*d));
            break;
        case ast::NodeKind::Enum:
            collect_enum(static_cast<const ast::EnumDecl&>(*d));
            break;
        case ast::NodeKind::Protocol:
            collect_protocol(static_cast<const ast::ProtocolDecl&>(*d));
            break;
        case ast::NodeKind::Opaque:
            collect_opaque(static_cast<const ast::OpaqueDecl&>(*d));
            break;
        case ast::NodeKind::Const:
            collect_const(static_cast<const ast::ConstDecl&>(*d));
            break;
        case ast::NodeKind::Static:
            collect_static(static_cast<const ast::StaticDecl&>(*d));
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
// Pass 2: visit each decl body, resolving names and computing types.
// ============================================================================

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
            if (annotated != nullptr && !TypeArena::assignable(value_type, annotated)) {
                error_at(c.value->range,
                         std::format("const value of type {} does not match annotation {}",
                                     value_type ? value_type->describe() : "?",
                                     annotated->describe()));
            }
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
        }
        break;
    }
    case ast::NodeKind::Struct: {
        // Resolve member function bodies, if any. Field types were already
        // resolved on demand by callers; we don't yet type-check defaults.
        const auto& s = static_cast<const ast::StructDecl&>(d);
        for (const auto& m : s.methods) {
            if (m->kind == ast::NodeKind::Func) {
                check_func(static_cast<const ast::FuncDecl&>(*m));
            }
        }
        break;
    }
    case ast::NodeKind::Enum: {
        const auto& e = static_cast<const ast::EnumDecl&>(d);
        for (const auto& m : e.methods) {
            if (m->kind == ast::NodeKind::Func) {
                check_func(static_cast<const ast::FuncDecl&>(*m));
            }
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
        // Resolve the target into a Struct decl. Anything else (enum,
        // protocol, opaque) is rejected with a v0.5 diagnostic.
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

void Resolver::check_func(const ast::FuncDecl& f) {
    if (!f.body) {
        return;  // protocol requirement / extern stub — nothing to check
    }

    ScopeStack::Guard g(scopes_);

    // Bind generic parameters as opaque types in the function's scope.
    for (const auto& gp : f.generics) {
        if (gp.name.empty() || gp.is_const) {
            continue;  // const generics live in the value namespace; v0.5 skips them
        }
        Symbol s;
        s.name = gp.name;
        s.kind = SymbolKind::GenericParam;
        s.type = types_->make_generic_param(gp.name);
        s.definition_range = gp.range;
        if (auto* prev = g.scope().insert(std::move(s))) {
            duplicate_definition(*prev, gp.name, gp.range);
        }
    }

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
        s.type = param_type;
        s.definition_range = p.range;
        if (auto* prev = g.scope().insert(std::move(s))) {
            duplicate_definition(*prev, p.name, p.range);
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

    // The body is always a BlockExpr; pass the expected result as the context.
    TypePtr body_type = types_->unit();
    if (f.body->kind == ast::NodeKind::BlockExpr) {
        body_type = check_block_expr(static_cast<const ast::BlockExpr&>(*f.body), expected_result);
    }

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

    return_stack_.pop_back();
    throws_stack_.pop_back();
    do_catch_infer_stack_ = std::move(saved_infer);
}

TypePtr Resolver::check_block_expr(const ast::BlockExpr& b, TypePtr expected) {
    ScopeStack::Guard g(scopes_);
    TypePtr last = types_->unit();
    for (std::size_t i = 0; i < b.stmts.size(); ++i) {
        const auto& s = *b.stmts[i];
        if (s.kind == ast::NodeKind::ExprStmt && i + 1 == b.stmts.size()) {
            // Trailing expression: its type is the block's value type, and
            // it inherits the surrounding block's expected type.
            last = check_expr(*static_cast<const ast::ExprStmt&>(s).expr, expected);
        } else {
            check_stmt(s);
        }
    }
    return last;
}

void Resolver::check_stmt(const ast::Stmt& s) {
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

        if (!binding_name.empty()) {
            Symbol sym;
            sym.name = std::string{binding_name};
            sym.kind = SymbolKind::Local;
            sym.type = annot_type != nullptr ? annot_type : value_type;
            sym.definition_range = binding_range;
            if (auto* prev = scopes_.current().insert(std::move(sym))) {
                duplicate_definition(*prev, binding_name, binding_range);
            }
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
            bind_tuple_pattern(static_cast<const ast::TuplePat&>(*pat), bind_t);
        }
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
        break;
    }
    case ast::NodeKind::ExprStmt:
        (void)check_expr(*static_cast<const ast::ExprStmt&>(s).expr);
        break;
    case ast::NodeKind::AssignStmt: {
        const auto& a = static_cast<const ast::AssignStmt&>(s);
        auto lhs = check_expr(*a.target);
        auto rhs = check_expr(*a.value);
        if (lhs != nullptr && rhs != nullptr && !TypeArena::assignable(rhs, lhs)) {
            error_at(a.value->range,
                     std::format("cannot assign value of type {} to target of type {}",
                                 rhs->describe(),
                                 lhs->describe()));
        }
        break;
    }
    case ast::NodeKind::WhileStmt: {
        const auto& w = static_cast<const ast::WhileStmt&>(s);
        auto cond_type = check_expr(*w.cond);
        if (cond_type != nullptr && cond_type->kind() != TypeKind::Bool && !cond_type->is_error()) {
            error_at(w.cond->range,
                     std::format("while condition must be Bool, got {}", cond_type->describe()));
        }
        if (w.body) {
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
                         std::format("'for x in xs' requires xs to be a Range or have a "
                                     "'next() -> Element?' method, got {}",
                                     iter_type->describe()));
            }
        }
        ScopeStack::Guard g(scopes_);
        if (f.pattern && f.pattern->kind == ast::NodeKind::IdentPat) {
            Symbol sym;
            sym.name = static_cast<const ast::IdentPat&>(*f.pattern).name;
            sym.kind = SymbolKind::Local;
            sym.type = elem_type != nullptr ? elem_type : types_->error();
            sym.definition_range = f.pattern->range;
            (void)g.scope().insert(std::move(sym));
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
    default:
        break;
    }
}

// ============================================================================
// Expressions
// ============================================================================

TypePtr Resolver::check_expr(const ast::Expr& e, TypePtr expected) {
    TypePtr t = nullptr;
    switch (e.kind) {
    case ast::NodeKind::IntLit:
        // Integer literals adopt the expected type when it is an integer; this
        // is the bidirectional bit that lets `let x: Int32 = 42` typecheck
        // without an explicit conversion. Without context, default to Int.
        // §9 ergonomic: peel one Optional layer so `let x: Int32? = 42`
        // also adopts Int32 instead of falling back to the Int default.
        {
            TypePtr int_hint = expected;
            if (int_hint != nullptr && int_hint->kind() == TypeKind::Optional) {
                int_hint = int_hint->inner();
            }
            t = (int_hint != nullptr && int_hint->is_integer()) ? int_hint
                                                                : types_->primitive(TypeKind::Int);
        }
        break;
    case ast::NodeKind::FloatLit: {
        TypePtr float_hint = expected;
        if (float_hint != nullptr && float_hint->kind() == TypeKind::Optional) {
            float_hint = float_hint->inner();
        }
        t = (float_hint != nullptr && float_hint->is_float())
                ? float_hint
                : types_->primitive(TypeKind::Float64);
        break;
    }
    case ast::NodeKind::BoolLit:
        t = types_->boolean();
        break;
    case ast::NodeKind::NilLit:
        // `nil` is the §9 absence literal. Its type is the expected
        // Optional<T> when that's known; otherwise we fall back to
        // Optional<Never>, which TypeArena::assignable treats as
        // compatible with every Optional<T> so e.g. `let x: Int32? = nil`
        // still typechecks even though `nil` was checked before the let-
        // initializer ran.
        t = (expected != nullptr && expected->kind() == TypeKind::Optional)
                ? expected
                : types_->make_optional(types_->never());
        break;
    case ast::NodeKind::StringLit:
        t = types_->primitive(TypeKind::StrConst);
        break;
    case ast::NodeKind::CharLit:
        t = types_->primitive(TypeKind::Char);
        break;

    case ast::NodeKind::IdentExpr: {
        const auto& ident = static_cast<const ast::IdentExpr&>(e);
        const auto* sym = scopes_.current().lookup(ident.name);
        if (sym == nullptr) {
            error_at(e.range, std::format("undefined name '{}'", ident.name));
            t = types_->error();
        } else {
            resolution_.set_symbol(&e, sym);
            check_visibility(*sym, e.range);
            t = sym->type != nullptr ? sym->type : types_->error();
        }
        break;
    }
    case ast::NodeKind::ParenExpr:
        t = check_expr(*static_cast<const ast::ParenExpr&>(e).inner, expected);
        break;
    case ast::NodeKind::TupleLitExpr: {
        // §6 `(e1, e2, …)` types as Tuple<typeof(e1), typeof(e2), …>.
        // When the surrounding context expects a TupleType of matching
        // arity, push each expected element down so a literal inside
        // the tuple adopts the expected primitive width.
        const auto& tl = static_cast<const ast::TupleLitExpr&>(e);
        std::vector<TypePtr> parts;
        parts.reserve(tl.elements.size());
        const auto* expected_parts = (expected != nullptr && expected->kind() == TypeKind::Tuple
                                      && expected->parts().size() == tl.elements.size())
                                         ? &expected->parts()
                                         : nullptr;
        for (std::size_t i = 0; i < tl.elements.size(); ++i) {
            TypePtr elem_hint = expected_parts != nullptr ? (*expected_parts)[i] : nullptr;
            parts.push_back(check_expr(*tl.elements[i], elem_hint));
        }
        t = types_->make_tuple(std::move(parts));
        break;
    }
    case ast::NodeKind::UnaryExpr:
        t = check_unary(static_cast<const ast::UnaryExpr&>(e), expected);
        break;
    case ast::NodeKind::BinaryExpr:
        t = check_binary(static_cast<const ast::BinaryExpr&>(e), expected);
        break;
    case ast::NodeKind::CallExpr:
        t = check_call(static_cast<const ast::CallExpr&>(e), expected);
        break;
    case ast::NodeKind::IfExpr:
        t = check_if(static_cast<const ast::IfExpr&>(e), expected);
        break;
    case ast::NodeKind::BlockExpr:
        t = check_block_expr(static_cast<const ast::BlockExpr&>(e), expected);
        break;
    case ast::NodeKind::CopyExpr:
        t = check_expr(*static_cast<const ast::CopyExpr&>(e).inner);
        break;
    case ast::NodeKind::AwaitExpr:
        t = check_expr(*static_cast<const ast::AwaitExpr&>(e).inner);
        break;
    case ast::NodeKind::SpawnExpr:
        t = check_expr(*static_cast<const ast::SpawnExpr&>(e).inner);
        break;
    case ast::NodeKind::ThrowExpr: {
        // §9: `throw e` is only valid inside a throws(E) function; e must
        // be assignable to E. The expression's static type is Never.
        const auto& th = static_cast<const ast::ThrowExpr&>(e);
        TypePtr enclosing_err = throws_stack_.empty() ? nullptr : throws_stack_.back();
        auto inner_type = check_expr(*th.inner, enclosing_err);
        if (enclosing_err == nullptr) {
            error_at(e.range, "'throw' is only valid inside a function declared 'throws(E)'");
        } else if (inner_type != nullptr && !TypeArena::assignable(inner_type, enclosing_err)) {
            error_at(th.inner->range,
                     std::format("'throw' value of type {} does not match the enclosing throws({})",
                                 inner_type->describe(),
                                 enclosing_err->describe()));
        }
        t = types_->never();
        break;
    }
    case ast::NodeKind::TryExpr: {
        // §9: `try e` / `try? e` / `try! e` all require `e` to evaluate to
        // a Result<T, E> (i.e. a call to a throws function). The three
        // forms then differ in how they consume the error:
        //   * Propagating: returns the error from the enclosing function
        //     (which must itself be throws-compatible with E); yields T.
        //   * Optional:    converts to Optional<T>; yields Optional<T>.
        //   * Forced:      panics on error; yields T.
        const auto& tx = static_cast<const ast::TryExpr&>(e);
        auto inner_type = check_expr(*tx.inner);
        if (inner_type == nullptr || inner_type->is_error()) {
            t = types_->error();
            break;
        }
        if (inner_type->kind() != TypeKind::Result) {
            error_at(tx.inner->range,
                     std::format("'try' operand must produce a Result<T, E>, got {}",
                                 inner_type->describe()));
            t = types_->error();
            break;
        }
        TypePtr ok = inner_type->inner();
        TypePtr err = inner_type->result();
        if (tx.form == ast::TryExpr::Form::Propagating) {
            // §9: a Propagating try inside a bare `do { ... } catch
            // NAME { ... }` writes its error type into the do-catch's
            // inference slot. The slot takes precedence over the
            // enclosing function's throws_stack_ — the do-catch is
            // the closer error handler.
            if (!do_catch_infer_stack_.empty()) {
                TypePtr* slot = do_catch_infer_stack_.back();
                if (*slot == nullptr) {
                    *slot = err;
                } else if (!TypeArena::equal(err, *slot)) {
                    error_at(e.range,
                             std::format("do-body tries with mismatched error types: {} vs {}; "
                                         "use 'catch (NAME: E)' to pin the type explicitly",
                                         (*slot)->describe(),
                                         err ? err->describe() : "?"));
                }
            } else {
                TypePtr enclosing_err = throws_stack_.empty() ? nullptr : throws_stack_.back();
                if (enclosing_err == nullptr) {
                    error_at(
                        e.range,
                        "'try' propagation requires the enclosing function to declare 'throws(E)'");
                } else if (!TypeArena::assignable(err, enclosing_err)) {
                    error_at(
                        e.range,
                        std::format(
                            "propagated error of type {} does not match the enclosing throws({})",
                            err ? err->describe() : "?",
                            enclosing_err->describe()));
                }
            }
            t = ok;
        } else if (tx.form == ast::TryExpr::Form::Optional) {
            t = types_->make_optional(ok);
        } else {
            // Forced: `try!` panics on error.
            t = ok;
        }
        break;
    }
    case ast::NodeKind::DoCatchExpr: {
        // §9 `do { body } catch ... { handler }` — two binding shapes:
        //   * Annotated: `catch (NAME: E)` — E is known up front; we
        //     push it onto throws_stack_ and the body's tries check
        //     against it normally.
        //   * Bare: `catch NAME` — E is *inferred* from the body's
        //     try-calls. We push a fresh inference slot onto
        //     do_catch_infer_stack_; check_try writes the first
        //     try's error type there and checks subsequent ones for
        //     match. Throws_stack_ stays untouched in this mode
        //     since check_try prefers the infer slot when present.
        const auto& dc = static_cast<const ast::DoCatchExpr&>(e);
        TypePtr error_type = dc.error_type ? resolve_type(*dc.error_type) : nullptr;
        TypePtr inferred = nullptr;
        const bool inferring = (dc.error_type == nullptr);
        if (inferring) {
            do_catch_infer_stack_.push_back(&inferred);
        } else {
            throws_stack_.push_back(error_type);
        }
        TypePtr body_type = check_expr(*dc.do_body, expected);
        if (inferring) {
            do_catch_infer_stack_.pop_back();
            if (inferred == nullptr) {
                error_at(dc.range,
                         "bare 'catch NAME' requires the do-body to contain at least one 'try'; "
                         "either add a try-call or use 'catch (NAME: E)' to give the error type "
                         "explicitly");
                inferred = types_->error();
            }
            error_type = inferred;
            resolution_.set_do_catch_error_type(&dc, error_type);
        } else {
            throws_stack_.pop_back();
        }

        ScopeStack::Guard g(scopes_);
        Symbol sym;
        sym.name = dc.error_name;
        sym.kind = SymbolKind::Local;
        sym.type = error_type;
        sym.definition_range = dc.range;
        (void)scopes_.current().insert(std::move(sym));
        TypePtr catch_type = check_expr(*dc.catch_body, expected != nullptr ? expected : body_type);

        if (body_type == nullptr || catch_type == nullptr || body_type->is_error()
            || catch_type->is_error()) {
            t = types_->error();
            break;
        }
        if (body_type->is_never()) {
            t = catch_type;
            break;
        }
        if (catch_type->is_never()) {
            t = body_type;
            break;
        }
        if (!TypeArena::equal(body_type, catch_type)) {
            error_at(e.range,
                     std::format("do/catch arms have different types: {} vs {}",
                                 body_type->describe(),
                                 catch_type->describe()));
            t = types_->error();
            break;
        }
        t = body_type;
        break;
    }
    case ast::NodeKind::AsExpr: {
        const auto& a = static_cast<const ast::AsExpr&>(e);
        (void)check_expr(*a.value);
        t = a.target ? resolve_type(*a.target) : types_->error();
        if (a.is_optional && t != nullptr && !t->is_error()) {
            t = types_->make_optional(t);
        }
        break;
    }
    case ast::NodeKind::MemberExpr:
        t = check_member(static_cast<const ast::MemberExpr&>(e));
        break;
    case ast::NodeKind::LeadingDotExpr:
        t = check_leading_dot(static_cast<const ast::LeadingDotExpr&>(e), expected);
        break;
    case ast::NodeKind::MatchExpr:
        t = check_match(static_cast<const ast::MatchExpr&>(e), expected);
        break;
    case ast::NodeKind::InterpStringExpr: {
        // §4: a `"…\(expr)…"` literal is a freshly allocated String.
        // Every splice must be Display-conformant — primitive numerics
        // / Bool / Char / string-likes pass implicitly; user types need
        // either `derive(Display)` or `derive(Debug)`. The `using Alloc`
        // capability requirement (§4) is still a follow-on; the runtime
        // lowering uses std::format which assumes a default heap.
        const auto& is_ = static_cast<const ast::InterpStringExpr&>(e);
        for (const auto& seg : is_.segments) {
            if (seg.expr == nullptr) {
                continue;
            }
            auto seg_t = check_expr(*seg.expr);
            if (seg_t == nullptr || seg_t->is_error()) {
                continue;
            }
            if (!is_display_conformant(seg_t)) {
                error_at(seg.expr->range,
                         std::format("interpolation splice of type {} is not Display-conformant; "
                                     "add `derive(Display) for {}` (or `derive(Debug)`) to make it "
                                     "printable",
                                     seg_t->describe(),
                                     seg_t->describe()));
            }
        }
        t = types_->primitive(TypeKind::String);
        break;
    }
    case ast::NodeKind::EmbedExpr: {
        // §12.1 `@embed("path")` types as `[N]UInt8` where N is the file's
        // size at compile time. We ask the folder to read the file now;
        // the byte-count from the folded vector pins the type's length,
        // and stashing the folded value here also primes the side table
        // so codegen emits the brace-init literal without re-reading.
        const auto& ee = static_cast<const ast::EmbedExpr&>(e);
        auto folded = folder_.fold(ee, comptime_env_);
        if (!folded || folded->kind != ComptimeValue::Kind::Vector) {
            error_at(ee.range,
                     std::format("could not @embed file \"{}\" (path/manifest miss or no reader "
                                 "configured)",
                                 ee.path));
            t = types_->error();
            break;
        }
        resolution_.set_folded_value(&ee, *folded);
        t = types_->make_vector(folded->length, types_->primitive(TypeKind::UInt8));
        break;
    }

    default:
        // Unimplemented: walk children to surface their errors, then assign
        // Error so downstream consumers don't cascade.
        t = types_->error();
        break;
    }
    if (t != nullptr) {
        resolution_.set_type(&e, t);
    }
    return t;
}

TypePtr Resolver::check_unary(const ast::UnaryExpr& u, TypePtr expected) {
    // Negation / bit-not preserve the operand's type, so the expected type
    // can be propagated inward.
    auto operand = check_expr(*u.operand, expected);
    if (operand == nullptr || operand->is_error()) {
        return types_->error();
    }
    switch (u.op) {
    case ast::UnaryOp::Neg:
        if (!operand->is_numeric()) {
            error_at(u.range,
                     std::format("unary '-' requires a numeric type, got {}", operand->describe()));
            return types_->error();
        }
        return operand;
    case ast::UnaryOp::Not:
        if (operand->kind() != TypeKind::Bool) {
            error_at(u.range, std::format("unary '!' requires Bool, got {}", operand->describe()));
            return types_->error();
        }
        return operand;
    case ast::UnaryOp::BitNot:
        if (!operand->is_integer()) {
            error_at(
                u.range,
                std::format("unary '~' requires an integer type, got {}", operand->describe()));
            return types_->error();
        }
        return operand;
    case ast::UnaryOp::Unwrap:
        // §9 postfix `!` panics on `.none` and yields the wrapped T.
        if (operand->kind() != TypeKind::Optional) {
            error_at(u.range,
                     std::format("postfix '!' requires an Optional, got {}", operand->describe()));
            return types_->error();
        }
        return operand->inner();
    }
    return types_->error();
}

TypePtr Resolver::check_binary(const ast::BinaryExpr& b, TypePtr expected) {
    // §9 `lhs ?? rhs` — the result type is T, so the surrounding `expected`
    // (when present) is T. Propagate T as the lhs's expected-Optional<T>
    // and as the rhs's expected-T so literal defaulting picks the right
    // primitive on both sides.
    if (b.op == ast::BinaryOp::Coalesce) {
        TypePtr lhs_expected = expected != nullptr ? types_->make_optional(expected) : nullptr;
        auto lhs = check_expr(*b.lhs, lhs_expected);
        TypePtr rhs_expected =
            (lhs != nullptr && lhs->kind() == TypeKind::Optional) ? lhs->inner() : expected;
        auto rhs = check_expr(*b.rhs, rhs_expected);
        if (lhs == nullptr || rhs == nullptr || lhs->is_error() || rhs->is_error()) {
            return types_->error();
        }
        if (lhs->kind() != TypeKind::Optional) {
            error_at(
                b.lhs->range,
                std::format("'?\\?' requires an Optional left operand, got {}", lhs->describe()));
            return types_->error();
        }
        if (!TypeArena::assignable(rhs, lhs->inner())) {
            error_at(b.rhs->range,
                     std::format("'?\\?' right operand of type {} does not match Optional<{}>",
                                 rhs->describe(),
                                 lhs->inner() ? lhs->inner()->describe() : "?"));
            return types_->error();
        }
        return lhs->inner();
    }

    // For arithmetic/bitwise/comparison, both operands ought to share a
    // single numeric type. Propagate `expected` to the lhs first; then use
    // the lhs's concrete type as the expected type for the rhs (so a literal
    // on either side adapts).
    auto lhs = check_expr(*b.lhs, expected);
    auto rhs = check_expr(*b.rhs, lhs);
    if (lhs == nullptr || rhs == nullptr) {
        return types_->error();
    }
    if (lhs->is_error() || rhs->is_error()) {
        return types_->error();
    }

    auto require_match = [&] {
        if (!TypeArena::equal(lhs, rhs)) {
            error_at(b.range,
                     std::format("binary operator operands of different types: {} vs {}",
                                 lhs->describe(),
                                 rhs->describe()));
            return false;
        }
        return true;
    };

    switch (b.op) {
    case ast::BinaryOp::Add:
    case ast::BinaryOp::Sub:
    case ast::BinaryOp::Mul:
    case ast::BinaryOp::Div:
    case ast::BinaryOp::Mod:
    case ast::BinaryOp::WrappingAdd:
    case ast::BinaryOp::WrappingSub:
    case ast::BinaryOp::WrappingMul:
        if (!lhs->is_numeric()) {
            error_at(b.lhs->range,
                     std::format("arithmetic operator requires numeric operands, got {}",
                                 lhs->describe()));
            return types_->error();
        }
        if (!require_match()) {
            return types_->error();
        }
        return lhs;

    case ast::BinaryOp::Shl:
    case ast::BinaryOp::Shr:
    case ast::BinaryOp::BitAnd:
    case ast::BinaryOp::BitOr:
    case ast::BinaryOp::BitXor:
        if (!lhs->is_integer()) {
            error_at(
                b.lhs->range,
                std::format("bitwise operator requires integer operands, got {}", lhs->describe()));
            return types_->error();
        }
        if (!require_match()) {
            return types_->error();
        }
        return lhs;

    case ast::BinaryOp::Eq:
    case ast::BinaryOp::Ne:
    case ast::BinaryOp::Lt:
    case ast::BinaryOp::Le:
    case ast::BinaryOp::Gt:
    case ast::BinaryOp::Ge:
        if (!require_match()) {
            return types_->error();
        }
        return types_->boolean();

    case ast::BinaryOp::And:
    case ast::BinaryOp::Or:
        if (lhs->kind() != TypeKind::Bool || rhs->kind() != TypeKind::Bool) {
            error_at(b.range, "logical && / || require Bool operands");
            return types_->error();
        }
        return types_->boolean();

    case ast::BinaryOp::Coalesce:
        // `lhs ?? rhs` — lhs should be Optional<T>, rhs should be T.
        if (lhs->kind() != TypeKind::Optional) {
            error_at(
                b.lhs->range,
                std::format("'?\\?' requires an Optional left operand, got {}", lhs->describe()));
            return types_->error();
        }
        if (!TypeArena::assignable(rhs, lhs->inner())) {
            error_at(b.rhs->range,
                     std::format("'?\\?' right operand of type {} does not match Optional<{}>",
                                 rhs->describe(),
                                 lhs->inner() ? lhs->inner()->describe() : "?"));
            return types_->error();
        }
        return lhs->inner();

    case ast::BinaryOp::Range:
    case ast::BinaryOp::RangeLt:
        if (!require_match()) {
            return types_->error();
        }
        // TODO: a real Range[T] nominal type. For now we just pass through.
        return lhs;
    }
    return types_->error();
}

TypePtr Resolver::check_call(const ast::CallExpr& c, TypePtr expected) {
    // §17.x conversion-call syntax: `Float64(i)` / `Int32(x)` — a bare
    // primitive numeric type name in callee position is an explicit
    // conversion. We handle it before normal callee resolution so the
    // type name doesn't trip "undefined name 'Float64'" against the
    // value scope (where it intentionally isn't registered — primitives
    // are types, not symbols). Requires exactly one positional numeric
    // argument; the result type is the target primitive.
    if (c.callee->kind == ast::NodeKind::IdentExpr) {
        const auto& callee_ident = static_cast<const ast::IdentExpr&>(*c.callee);
        auto kind = TypeArena::primitive_kind_by_name(callee_ident.name);
        if (kind != TypeKind::Error) {
            auto target = types_->primitive(kind);
            if (target != nullptr && target->is_numeric()) {
                if (c.args.size() != 1) {
                    for (const auto& a : c.args) {
                        (void)check_expr(*a.value);
                    }
                    error_at(c.range,
                             std::format("conversion to {} takes exactly one argument",
                                         target->describe()));
                    return target;
                }
                if (!c.args[0].label.empty()) {
                    error_at(c.args[0].value->range,
                             "conversion call argument cannot have a label");
                }
                auto arg_type = check_expr(*c.args[0].value);
                if (arg_type != nullptr && !arg_type->is_numeric() && !arg_type->is_error()) {
                    error_at(c.args[0].value->range,
                             std::format("cannot convert non-numeric {} to {}",
                                         arg_type->describe(),
                                         target->describe()));
                }
                return target;
            }
        }
    }

    // §10 builtin `Box.new(value)` — the unique-ownership heap pointer
    // constructor. The callee is a `MemberExpr(IdentExpr("Box"), "new")`;
    // sema doesn't have a symbol for the Box "value" so we intercept
    // the call shape directly. The result type is Box<typeof(value)>.
    if (c.callee->kind == ast::NodeKind::MemberExpr) {
        const auto& mem = static_cast<const ast::MemberExpr&>(*c.callee);
        if (mem.base->kind == ast::NodeKind::IdentExpr
            && static_cast<const ast::IdentExpr&>(*mem.base).name == "Box" && mem.member == "new"
            && scopes_.current().lookup("Box") == nullptr) {
            if (c.args.size() != 1) {
                for (const auto& a : c.args) {
                    (void)check_expr(*a.value);
                }
                error_at(c.range, "Box.new(value) takes exactly one argument");
                return types_->make_box(types_->error());
            }
            if (!c.args[0].label.empty()) {
                error_at(c.args[0].value->range, "Box.new argument cannot have a label");
            }
            auto inner = check_expr(*c.args[0].value);
            return types_->make_box(inner != nullptr ? inner : types_->error());
        }
    }

    // §3 opaque-type construction: `Q(t)` where Q is a newtype over T —
    // one positional arg, type-checked against T, result is Q. The
    // codegen lowers this to `static_cast<Q>(t)` over the `enum class
    // Q : T {}` emission.
    if (c.callee->kind == ast::NodeKind::IdentExpr) {
        const auto& callee_ident = static_cast<const ast::IdentExpr&>(*c.callee);
        if (const auto* sym = scopes_.current().lookup(callee_ident.name);
            sym != nullptr && sym->kind == SymbolKind::OpaqueType && sym->decl != nullptr) {
            resolution_.set_symbol(c.callee.get(), sym);
            check_visibility(*sym, c.callee->range);
            const auto& od = static_cast<const ast::OpaqueDecl&>(*sym->decl);
            auto underlying = od.underlying ? resolve_type(*od.underlying) : types_->error();
            if (c.args.size() != 1) {
                for (const auto& a : c.args) {
                    (void)check_expr(*a.value);
                }
                error_at(c.range,
                         std::format("opaque-type constructor {}(...) takes exactly one argument",
                                     od.name));
                return sym->type;
            }
            if (!c.args[0].label.empty()) {
                error_at(c.args[0].value->range,
                         "opaque-type constructor argument cannot have a label");
            }
            auto arg_type = check_expr(*c.args[0].value, underlying);
            if (!TypeArena::assignable(arg_type, underlying)) {
                error_at(c.args[0].value->range,
                         std::format("opaque-type {}({}) requires a {} argument, got {}",
                                     od.name,
                                     underlying ? underlying->describe() : "?",
                                     underlying ? underlying->describe() : "?",
                                     arg_type ? arg_type->describe() : "?"));
            }
            return sym->type;
        }
    }

    // Resolve the callee first so we know each parameter's expected type
    // before we type its corresponding argument — this is what lets integer
    // literals adopt the parameter's type without an explicit conversion.
    //
    // We pass `expected` through to the callee so a leading-dot enum case in
    // call position (`.circle(radius: 1.0)`) can resolve against the expected
    // enum type.
    auto callee_type = check_expr(*c.callee, expected);
    if (callee_type == nullptr || callee_type->is_error()) {
        // Still type the arguments so their internal errors surface.
        for (const auto& a : c.args) {
            (void)check_expr(*a.value);
        }
        return types_->error();
    }

    // Struct construction: `Point(x: 10, y: 32)` parses as a CallExpr whose
    // callee evaluates to the nominal Struct type. Dispatch by-label against
    // the struct's fields rather than treating it as a function call.
    if (callee_type->kind() == TypeKind::Struct && callee_type->nominal_decl() != nullptr) {
        const auto& s_decl = static_cast<const ast::StructDecl&>(*callee_type->nominal_decl());
        // Build a label → (field, resolved type) index once.
        std::unordered_map<std::string, std::pair<const ast::StructDecl::Field*, TypePtr>> by_label;
        for (const auto& f : s_decl.fields) {
            if (f.kind == ast::StructDecl::Field::Kind::Embed) {
                continue;  // embeds aren't constructible by label (yet)
            }
            by_label.emplace(f.name,
                             std::make_pair(&f, f.type ? resolve_type(*f.type) : types_->error()));
        }
        std::vector<bool> seen(s_decl.fields.size(), false);
        for (std::size_t i = 0; i < c.args.size(); ++i) {
            const auto& arg = c.args[i];
            if (arg.label.empty()) {
                error_at(
                    arg.value->range,
                    std::format("struct constructor argument {} requires a field label", i + 1));
                (void)check_expr(*arg.value);
                continue;
            }
            auto it = by_label.find(arg.label);
            if (it == by_label.end()) {
                error_at(arg.value->range,
                         std::format("'{}' has no field '{}'", s_decl.name, arg.label));
                (void)check_expr(*arg.value);
                continue;
            }
            auto field_type = it->second.second;
            auto arg_type = check_expr(*arg.value, field_type);
            if (!TypeArena::assignable(arg_type, field_type)) {
                error_at(arg.value->range,
                         std::format("field '{}' of type {} cannot accept value of type {}",
                                     arg.label,
                                     field_type ? field_type->describe() : "?",
                                     arg_type ? arg_type->describe() : "?"));
            }
            // Track coverage for "missing field" diagnostics.
            for (std::size_t k = 0; k < s_decl.fields.size(); ++k) {
                if (&s_decl.fields[k] == it->second.first) {
                    seen[k] = true;
                    break;
                }
            }
        }
        // Missing fields are an error — Vestra has no implicit defaulting.
        for (std::size_t k = 0; k < s_decl.fields.size(); ++k) {
            if (s_decl.fields[k].kind == ast::StructDecl::Field::Kind::Embed) {
                continue;
            }
            if (!seen[k]) {
                error_at(c.range,
                         std::format("struct '{}' constructor is missing field '{}'",
                                     s_decl.name,
                                     s_decl.fields[k].name));
            }
        }
        return callee_type;
    }

    if (callee_type->kind() != TypeKind::Function) {
        for (const auto& a : c.args) {
            (void)check_expr(*a.value);
        }
        error_at(c.callee->range,
                 std::format("called value is not a function (type {})", callee_type->describe()));
        return types_->error();
    }

    // Generic-aware call dispatch (§7): if the callee is a generic Vestra
    // function, build a substitution map by unifying its declared parameter
    // types against the argument types, then apply the substitution to each
    // param (for arg checking) and to the return type (for the call's
    // expression type).
    //
    // The non-generic path stays a strict structural match — the generic
    // path is a strict superset, so we go through it for every call where
    // we have a known callee FuncDecl. Calls into opaque function values
    // fall through to the non-generic check below.
    const ast::FuncDecl* fn = nullptr;
    if (const auto* sym = resolution_.symbol_of(c.callee.get())) {
        if (sym->decl != nullptr && sym->decl->kind == ast::NodeKind::Func) {
            fn = static_cast<const ast::FuncDecl*>(sym->decl);
        }
    }

    const auto& params = callee_type->parts();
    if (params.size() != c.args.size()) {
        for (const auto& a : c.args) {
            (void)check_expr(*a.value);
        }
        error_at(c.range,
                 std::format("call expects {} argument(s), got {}", params.size(), c.args.size()));
        return callee_type->result() != nullptr ? callee_type->result() : types_->error();
    }

    // Seed bindings from the surrounding context: if the call's result type
    // is a generic placeholder (or contains one) and we know the type the
    // caller expects, bind from that. Lets `let x: Int32 = identity(7)` work
    // even though the literal `7` alone would default to Int.
    std::unordered_map<std::string, TypePtr> bindings;
    if (expected != nullptr && callee_type->result() != nullptr) {
        unify_generic(callee_type->result(), expected, bindings, c.range);
    }

    // Pass 1: collect bindings from concrete arg types. We don't enforce
    // type matches yet — that happens after substitution. As we walk left
    // to right, we substitute each parameter's expected type with the
    // bindings discovered so far, so an integer literal on the right side
    // of `max_of(int32_value, 35)` adopts Int32 (already inferred from the
    // first argument) instead of defaulting to Int and then conflicting.
    for (std::size_t i = 0; i < params.size(); ++i) {
        auto pty = types_->substitute(params[i], bindings);
        auto arg_type = check_expr(*c.args[i].value, pty);
        unify_generic(params[i], arg_type, bindings, c.args[i].value->range);
    }

    // Pass 2: enforce each (substituted_param, arg) pair. We re-check the
    // arg with the substituted expected type so bidirectional literal
    // adaptation can pick up the generic's inferred concrete type.
    for (std::size_t i = 0; i < params.size(); ++i) {
        auto pty = types_->substitute(params[i], bindings);
        auto arg_type = check_expr(*c.args[i].value, pty);
        if (!TypeArena::assignable(arg_type, pty)) {
            error_at(c.args[i].value->range,
                     std::format("argument {} of type {} does not match parameter type {}",
                                 i + 1,
                                 arg_type ? arg_type->describe() : "?",
                                 pty ? pty->describe() : "?"));
        }
    }

    // Verify every generic parameter ended up bound. An unbound generic at a
    // call site means inference couldn't pin it down — usually because the
    // generic only appears in the return position with no contextual type.
    if (fn != nullptr) {
        for (const auto& gp : fn->generics) {
            if (gp.is_const || gp.name.empty()) {
                continue;
            }
            if (!bindings.contains(gp.name)) {
                error_at(c.range,
                         std::format("cannot infer generic parameter '{}' for call to '{}'",
                                     gp.name,
                                     fn->name));
            }
        }
    }

    auto result = callee_type->result() != nullptr ? callee_type->result() : types_->unit();
    return types_->substitute(result, bindings);
}

TypePtr Resolver::check_if(const ast::IfExpr& i, TypePtr expected) {
    TypePtr then_type;
    if (!i.let_name.empty()) {
        // §9 `if let NAME = INIT { ... }` — INIT must be Optional<T>; the
        // then-branch is checked in a scope where NAME is bound to T.
        auto init_type = check_expr(*i.let_init);
        TypePtr bound;
        if (init_type == nullptr || init_type->is_error()) {
            bound = types_->error();
        } else if (init_type->kind() != TypeKind::Optional) {
            error_at(i.let_init->range,
                     std::format("'if let' requires an Optional initializer, got {}",
                                 init_type->describe()));
            bound = types_->error();
        } else {
            bound = init_type->inner();
        }
        ScopeStack::Guard g(scopes_);
        Symbol sym;
        sym.name = i.let_name;
        sym.kind = SymbolKind::Local;
        sym.type = bound;
        sym.definition_range = i.range;
        (void)scopes_.current().insert(std::move(sym));
        then_type = check_expr(*i.then_branch, expected);
    } else {
        auto cond_type = check_expr(*i.cond, types_->boolean());
        if (cond_type != nullptr && cond_type->kind() != TypeKind::Bool && !cond_type->is_error()) {
            error_at(i.cond->range,
                     std::format("if condition must be Bool, got {}", cond_type->describe()));
        }
        then_type = check_expr(*i.then_branch, expected);
    }
    if (!i.else_branch) {
        return types_->unit();
    }
    auto else_type = check_expr(*i.else_branch, expected != nullptr ? expected : then_type);
    if (then_type == nullptr || else_type == nullptr) {
        return types_->error();
    }
    if (then_type->is_never()) {
        return else_type;
    }
    if (else_type->is_never()) {
        return then_type;
    }
    if (!TypeArena::equal(then_type, else_type)) {
        error_at(i.range,
                 std::format("if branches have different types: {} vs {}",
                             then_type->describe(),
                             else_type->describe()));
        return types_->error();
    }
    return then_type;
}

// ============================================================================
// Type-expression resolution
// ============================================================================

TypePtr Resolver::resolve_type(const ast::Type& t) {
    switch (t.kind) {
    case ast::NodeKind::NamedType: {
        const auto& n = static_cast<const ast::NamedType&>(t);
        if (n.path.size() == 1) {
            // §10 builtin `Box[T]` — the unique-ownership heap pointer.
            // Recognized here before generic-symbol lookup so it never
            // needs to be in the user's symbol table.
            if (n.path[0] == "Box" && n.type_args.size() == 1) {
                return types_->make_box(n.type_args[0] ? resolve_type(*n.type_args[0])
                                                       : types_->error());
            }
            // Try primitives first (Int32, Bool, ...).
            auto prim_kind = TypeArena::primitive_kind_by_name(n.path[0]);
            if (prim_kind != TypeKind::Error) {
                return types_->primitive(prim_kind);
            }
            // Then scope lookup — a nominal type or generic parameter.
            if (auto* sym = scopes_.current().lookup(n.path[0])) {
                if (sym->type != nullptr) {
                    return sym->type;
                }
            }
            error_at(t.range, std::format("unknown type '{}'", n.path[0]));
            return types_->error();
        }
        // Dotted paths (modules) are not yet resolved.
        error_at(t.range, "dotted type paths are not yet resolved");
        return types_->error();
    }
    case ast::NodeKind::OptionalType: {
        const auto& o = static_cast<const ast::OptionalType&>(t);
        return types_->make_optional(o.inner ? resolve_type(*o.inner) : types_->error());
    }
    case ast::NodeKind::VectorType: {
        const auto& v = static_cast<const ast::VectorType&>(t);
        return types_->make_vector(v.length,
                                   v.element ? resolve_type(*v.element) : types_->error());
    }
    case ast::NodeKind::TupleType: {
        const auto& tup = static_cast<const ast::TupleType&>(t);
        std::vector<TypePtr> parts;
        parts.reserve(tup.elements.size());
        for (const auto& e : tup.elements) {
            parts.push_back(e ? resolve_type(*e) : types_->error());
        }
        return types_->make_tuple(std::move(parts));
    }
    case ast::NodeKind::FunctionType: {
        const auto& f = static_cast<const ast::FunctionType&>(t);
        std::vector<TypePtr> ps;
        ps.reserve(f.params.size());
        for (const auto& p : f.params) {
            ps.push_back(p ? resolve_type(*p) : types_->error());
        }
        TypePtr r = f.result ? resolve_type(*f.result) : types_->unit();
        return types_->make_function(std::move(ps), r);
    }
    case ast::NodeKind::SomeType: {
        const auto& s = static_cast<const ast::SomeType&>(t);
        return s.inner ? resolve_type(*s.inner) : types_->error();
    }
    case ast::NodeKind::DynType: {
        const auto& d = static_cast<const ast::DynType&>(t);
        return d.inner ? resolve_type(*d.inner) : types_->error();
    }
    default:
        return types_->error();
    }
}

// ============================================================================
// Member access, methods, enum cases, match
// ============================================================================

TypePtr Resolver::lookup_field(TypePtr struct_type,
                               std::string_view name,
                               const ast::StructDecl::Field** out_field) {
    if (struct_type == nullptr || struct_type->kind() != TypeKind::Struct) {
        return nullptr;
    }
    const auto* decl = struct_type->nominal_decl();
    if (decl == nullptr || decl->kind != ast::NodeKind::Struct) {
        return nullptr;
    }
    const auto& s = static_cast<const ast::StructDecl&>(*decl);

    // Direct fields first (an embed shadowed by a direct field — unlikely
    // but the §6 read is that names resolve outward, with the immediate
    // owner winning).
    for (const auto& f : s.fields) {
        if (f.name == name && f.kind != ast::StructDecl::Field::Kind::Embed) {
            if (out_field != nullptr) {
                *out_field = &f;
            }
            return f.type ? resolve_type(*f.type) : types_->error();
        }
    }
    // Then recurse through `embed` fields so `entity.position` resolves to
    // `entity.transform.position` per §6.
    for (const auto& f : s.fields) {
        if (f.kind == ast::StructDecl::Field::Kind::Embed && f.type) {
            auto embed_type = resolve_type(*f.type);
            if (auto t = lookup_field(embed_type, name, out_field)) {
                return t;
            }
        }
    }
    return nullptr;
}

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
        // `std::format("{}", std::optional<T>{})` isn't standard, so
        // require an explicit `?? default` / `if let` at the splice
        // site rather than silently rendering "nullopt".
        return false;
    case TypeKind::Struct:
    case TypeKind::Enum:
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
    default:
        // Box, Result, GenericParam, Enum (bare or sum), Protocol,
        // Function, Reference — none have a v0.5 default. The kernel
        // resort is `var x: T? = nil` plus an explicit init.
        return false;
    }
}

void Resolver::bind_tuple_pattern(const ast::TuplePat& pat, TypePtr value_type) {
    if (value_type == nullptr || value_type->is_error()) {
        return;  // suppress cascading errors
    }
    if (value_type->kind() != TypeKind::Tuple) {
        error_at(
            pat.range,
            std::format("tuple pattern requires a tuple value, got {}", value_type->describe()));
        return;
    }
    const auto& parts = value_type->parts();
    if (parts.size() != pat.elements.size()) {
        error_at(pat.range,
                 std::format("tuple pattern has {} elements, value has {}",
                             pat.elements.size(),
                             parts.size()));
        return;
    }
    for (std::size_t i = 0; i < pat.elements.size(); ++i) {
        const auto& sub = pat.elements[i];
        if (!sub) {
            continue;
        }
        std::string_view name;
        if (sub->kind == ast::NodeKind::IdentPat) {
            name = static_cast<const ast::IdentPat&>(*sub).name;
        } else if (sub->kind == ast::NodeKind::BindPat) {
            name = static_cast<const ast::BindPat&>(*sub).name;
        } else if (sub->kind == ast::NodeKind::WildcardPat) {
            continue;
        } else if (sub->kind == ast::NodeKind::TuplePat) {
            bind_tuple_pattern(static_cast<const ast::TuplePat&>(*sub), parts[i]);
            continue;
        } else {
            error_at(sub->range,
                     "v0.5 only supports identifier / `_` / nested-tuple element "
                     "patterns inside a tuple pattern");
            continue;
        }
        if (name.empty()) {
            continue;
        }
        Symbol sym;
        sym.name = std::string{name};
        sym.kind = SymbolKind::Local;
        sym.type = parts[i];
        sym.definition_range = sub->range;
        if (auto* prev = scopes_.current().insert(std::move(sym))) {
            duplicate_definition(*prev, sym.name, sub->range);
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

TypePtr Resolver::lookup_method(TypePtr owner_type,
                                std::string_view name,
                                const ast::FuncDecl** out_method) {
    if (owner_type == nullptr) {
        return nullptr;
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
    if (c.payload.empty()) {
        return enum_type;  // bare case is a value of the enum
    }
    std::vector<TypePtr> params;
    params.reserve(c.payload.size());
    for (const auto& p : c.payload) {
        params.push_back(p.second ? resolve_type(*p.second) : types_->error());
    }
    return types_->make_function(std::move(params), enum_type);
}

TypePtr Resolver::check_member(const ast::MemberExpr& m) {
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
                    auto t = enum_case_constructor_type(sym->type, *c);
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

    // Field on a struct.
    if (auto field_type = lookup_field(lookup_base, m.member)) {
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

TypePtr Resolver::check_match(const ast::MatchExpr& m, TypePtr expected) {
    auto scrutinee_type = check_expr(*m.scrutinee);

    // Track which enum cases got covered for the exhaustiveness check.
    const ast::EnumDecl* scrutinee_enum = nullptr;
    if (scrutinee_type != nullptr && scrutinee_type->kind() == TypeKind::Enum
        && scrutinee_type->nominal_decl() != nullptr) {
        scrutinee_enum = &static_cast<const ast::EnumDecl&>(*scrutinee_type->nominal_decl());
    }
    std::vector<bool> case_seen(scrutinee_enum ? scrutinee_enum->cases.size() : 0, false);
    bool saw_default = false;

    TypePtr result_type = nullptr;
    for (const auto& arm : m.arms) {
        ScopeStack::Guard g(scopes_);
        if (arm.is_default) {
            saw_default = true;
        } else if (arm.pattern) {
            check_pattern(*arm.pattern, scrutinee_type);
            if (scrutinee_enum != nullptr && arm.pattern->kind == ast::NodeKind::EnumPat) {
                const auto& ep = static_cast<const ast::EnumPat&>(*arm.pattern);
                for (std::size_t i = 0; i < scrutinee_enum->cases.size(); ++i) {
                    if (scrutinee_enum->cases[i].name == ep.case_name) {
                        case_seen[i] = true;
                        break;
                    }
                }
            }
        }
        if (arm.guard) {
            auto gt = check_expr(*arm.guard, types_->boolean());
            if (gt != nullptr && gt->kind() != TypeKind::Bool && !gt->is_error()) {
                error_at(arm.guard->range,
                         std::format("match guard must be Bool, got {}", gt->describe()));
            }
        }
        TypePtr arm_type = arm.body ? check_expr(*arm.body, expected) : types_->unit();
        if (result_type == nullptr) {
            result_type = arm_type;
        } else if (arm_type != nullptr && !arm_type->is_never() && !result_type->is_never()
                   && !TypeArena::equal(arm_type, result_type)) {
            error_at(arm.body ? arm.body->range : m.range,
                     std::format("match arms have different types: {} vs {}",
                                 result_type->describe(),
                                 arm_type->describe()));
            result_type = types_->error();
        }
    }

    // Exhaustiveness: when the scrutinee is an enum and there's no `default`,
    // every case must be covered by at least one arm.
    if (scrutinee_enum != nullptr && !saw_default) {
        for (std::size_t i = 0; i < scrutinee_enum->cases.size(); ++i) {
            if (!case_seen[i]) {
                error_at(m.range,
                         std::format("match is not exhaustive: case '{}' is not covered",
                                     scrutinee_enum->cases[i].name));
            }
        }
    }

    return result_type != nullptr ? result_type : types_->unit();
}

void Resolver::check_pattern(const ast::Pattern& p, TypePtr scrutinee) {
    switch (p.kind) {
    case ast::NodeKind::WildcardPat:
    case ast::NodeKind::LiteralPat:
        break;
    case ast::NodeKind::IdentPat: {
        // Plain identifier patterns match by name; in match-arm position we
        // treat them as a binding to the scrutinee's type.
        const auto& ip = static_cast<const ast::IdentPat&>(p);
        Symbol sym;
        sym.name = ip.name;
        sym.kind = SymbolKind::Local;
        sym.type = scrutinee;
        sym.definition_range = p.range;
        (void)scopes_.current().insert(std::move(sym));
        break;
    }
    case ast::NodeKind::BindPat: {
        const auto& bp = static_cast<const ast::BindPat&>(p);
        Symbol sym;
        sym.name = bp.name;
        sym.kind = SymbolKind::Local;
        sym.type = scrutinee;
        sym.definition_range = p.range;
        (void)scopes_.current().insert(std::move(sym));
        break;
    }
    case ast::NodeKind::TuplePat: {
        // §6 tuple pattern in match-arm position. Two shapes:
        //   * `match tup { case (a, b): ... }` — the scrutinee is a
        //     tuple, the pattern destructures it directly.
        //   * As an EnumPat child: `case .pair((a, b)):` — the enclosing
        //     EnumPat case threads the payload field's type in as
        //     `scrutinee` for this recursive call.
        // Either way, bind_tuple_pattern walks the elements in parallel
        // with the scrutinee's TupleType and reports arity / non-tuple
        // mismatches.
        bind_tuple_pattern(static_cast<const ast::TuplePat&>(p), scrutinee);
        break;
    }
    case ast::NodeKind::EnumPat: {
        const auto& ep = static_cast<const ast::EnumPat&>(p);
        if (scrutinee == nullptr || scrutinee->kind() != TypeKind::Enum
            || scrutinee->nominal_decl() == nullptr) {
            error_at(p.range,
                     std::format("enum pattern '.{}' used against non-enum type {}",
                                 ep.case_name,
                                 scrutinee ? scrutinee->describe() : "?"));
            return;
        }
        const auto& enum_decl = static_cast<const ast::EnumDecl&>(*scrutinee->nominal_decl());
        const auto* c = lookup_enum_case(enum_decl, ep.case_name);
        if (c == nullptr) {
            error_at(p.range,
                     std::format("enum '{}' has no case '{}'", enum_decl.name, ep.case_name));
            return;
        }
        if (ep.children.size() != c->payload.size()) {
            error_at(p.range,
                     std::format("case '{}' has {} payload field(s), pattern binds {}",
                                 c->name,
                                 c->payload.size(),
                                 ep.children.size()));
            return;
        }
        for (std::size_t i = 0; i < ep.children.size(); ++i) {
            TypePtr payload_type =
                c->payload[i].second ? resolve_type(*c->payload[i].second) : types_->error();
            check_pattern(*ep.children[i], payload_type);
        }
        break;
    }
    default:
        // Tuple/struct/slice patterns: not yet typed against `scrutinee`.
        break;
    }
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

// ============================================================================
// Generic unification helper
// ============================================================================

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
        return;
    }
    if (ptype->kind() == TypeKind::Tuple && atype->kind() == TypeKind::Tuple
        && ptype->parts().size() == atype->parts().size()) {
        for (std::size_t k = 0; k < ptype->parts().size(); ++k) {
            unify_generic(ptype->parts()[k], atype->parts()[k], bindings, site);
        }
        return;
    }
    // Other kinds: structural rules elsewhere already verify the call, so no
    // binding is contributed.
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

}  // namespace vestra::sema
