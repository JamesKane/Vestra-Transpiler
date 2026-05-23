#include "vestra/sema/resolver.hpp"

#include "vestra/ast/nodes.hpp"
#include "vestra/sema/scope.hpp"
#include "vestra/sema/types.hpp"

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

// ============================================================================
// Resolver ctor + entry point
// ============================================================================

Resolver::Resolver(const ast::CompilationUnit& unit,
                   TypeArena& types,
                   diag::DiagnosticReporter& reporter)
    : unit_(&unit), types_(&types), reporter_(&reporter) {}

void Resolver::resolve() {
    collect_top_level();
    for (const auto& d : unit_->decls) {
        check_decl(*d);
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
    if (auto* prev = scopes_.global().insert(std::move(s))) {
        duplicate_definition(*prev, s_.name, s_.range);
    }
}

// ============================================================================
// Function-type construction
// ============================================================================

TypePtr Resolver::function_type_of(const ast::FuncDecl& f) {
    std::vector<TypePtr> params;
    params.reserve(f.params.size());
    for (const auto& p : f.params) {
        params.push_back(p.type ? resolve_type(*p.type) : types_->error());
    }
    TypePtr result = f.result ? resolve_type(*f.result) : types_->unit();
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
        if (p.name.empty()) {
            continue;
        }
        Symbol s;
        s.name = p.name;
        s.kind = SymbolKind::Parameter;
        s.type = p.type ? resolve_type(*p.type) : types_->error();
        s.definition_range = p.range;
        if (auto* prev = g.scope().insert(std::move(s))) {
            duplicate_definition(*prev, p.name, p.range);
        }
    }

    TypePtr expected_result = f.result ? resolve_type(*f.result) : types_->unit();
    return_stack_.push_back(expected_result);

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
}

void Resolver::check_block(const ast::BlockExpr& b) {
    (void)check_block_expr(b, nullptr);
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
            sym.kind = (s.kind == ast::NodeKind::LetStmt) ? SymbolKind::Local : SymbolKind::Local;
            sym.type = annot_type != nullptr ? annot_type : value_type;
            sym.definition_range = binding_range;
            if (auto* prev = scopes_.current().insert(std::move(sym))) {
                duplicate_definition(*prev, binding_name, binding_range);
            }
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
        const auto& f = static_cast<const ast::ForStmt&>(s);
        (void)check_expr(*f.iter);
        ScopeStack::Guard g(scopes_);
        if (f.pattern && f.pattern->kind == ast::NodeKind::IdentPat) {
            Symbol sym;
            sym.name = static_cast<const ast::IdentPat&>(*f.pattern).name;
            sym.kind = SymbolKind::Local;
            sym.type = nullptr;  // element type is the iterator's Element — TODO
            sym.definition_range = f.pattern->range;
            (void)g.scope().insert(std::move(sym));
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
        t = (expected != nullptr && expected->is_integer()) ? expected
                                                            : types_->primitive(TypeKind::Int);
        break;
    case ast::NodeKind::FloatLit:
        t = (expected != nullptr && expected->is_float()) ? expected
                                                          : types_->primitive(TypeKind::Float64);
        break;
    case ast::NodeKind::BoolLit:
        t = types_->boolean();
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
            t = sym->type != nullptr ? sym->type : types_->error();
        }
        break;
    }
    case ast::NodeKind::ParenExpr:
        t = check_expr(*static_cast<const ast::ParenExpr&>(e).inner, expected);
        break;
    case ast::NodeKind::UnaryExpr:
        t = check_unary(static_cast<const ast::UnaryExpr&>(e), expected);
        break;
    case ast::NodeKind::BinaryExpr:
        t = check_binary(static_cast<const ast::BinaryExpr&>(e), expected);
        break;
    case ast::NodeKind::CallExpr:
        t = check_call(static_cast<const ast::CallExpr&>(e));
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
    case ast::NodeKind::ThrowExpr:
        (void)check_expr(*static_cast<const ast::ThrowExpr&>(e).inner);
        t = types_->never();
        break;
    case ast::NodeKind::TryExpr:
        t = check_expr(*static_cast<const ast::TryExpr&>(e).inner);
        break;
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
    case ast::NodeKind::IndexExpr:
    case ast::NodeKind::MatchExpr:
    case ast::NodeKind::SelectExpr:
    case ast::NodeKind::ClosureExpr:
    case ast::NodeKind::ComptimeExpr:
    case ast::NodeKind::QuoteExpr:
    case ast::NodeKind::StructLitExpr:
    case ast::NodeKind::VectorLitExpr:
    case ast::NodeKind::LeadingDotExpr:
    case ast::NodeKind::PathExpr:
    case ast::NodeKind::SelfExpr:
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
    }
    return types_->error();
}

TypePtr Resolver::check_binary(const ast::BinaryExpr& b, TypePtr expected) {
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

TypePtr Resolver::check_call(const ast::CallExpr& c) {
    // Resolve the callee first so we know each parameter's expected type
    // before we type its corresponding argument — this is what lets integer
    // literals adopt the parameter's type without an explicit conversion.
    auto callee_type = check_expr(*c.callee);
    if (callee_type == nullptr || callee_type->is_error()) {
        // Still type the arguments so their internal errors surface.
        for (const auto& a : c.args) {
            (void)check_expr(*a.value);
        }
        return types_->error();
    }
    if (callee_type->kind() != TypeKind::Function) {
        for (const auto& a : c.args) {
            (void)check_expr(*a.value);
        }
        error_at(c.callee->range,
                 std::format("called value is not a function (type {})", callee_type->describe()));
        return types_->error();
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
    for (std::size_t i = 0; i < params.size(); ++i) {
        auto arg_type = check_expr(*c.args[i].value, params[i]);
        if (!TypeArena::assignable(arg_type, params[i])) {
            error_at(c.args[i].value->range,
                     std::format("argument {} of type {} does not match parameter type {}",
                                 i + 1,
                                 arg_type ? arg_type->describe() : "?",
                                 params[i] ? params[i]->describe() : "?"));
        }
    }
    return callee_type->result() != nullptr ? callee_type->result() : types_->unit();
}

TypePtr Resolver::check_if(const ast::IfExpr& i, TypePtr expected) {
    auto cond_type = check_expr(*i.cond, types_->boolean());
    if (cond_type != nullptr && cond_type->kind() != TypeKind::Bool && !cond_type->is_error()) {
        error_at(i.cond->range,
                 std::format("if condition must be Bool, got {}", cond_type->describe()));
    }
    auto then_type = check_expr(*i.then_branch, expected);
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

TypePtr Resolver::resolve_type_opt(const ast::Type* t, TypePtr fallback) {
    return t == nullptr ? fallback : resolve_type(*t);
}

}  // namespace vestra::sema
