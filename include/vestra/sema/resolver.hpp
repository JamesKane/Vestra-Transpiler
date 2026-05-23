#pragma once

#include "vestra/ast/nodes.hpp"
#include "vestra/diag/diagnostic.hpp"
#include "vestra/sema/scope.hpp"
#include "vestra/sema/types.hpp"

#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vestra::sema {

// The result of name-resolving + type-checking a compilation unit.
//
// The side tables here are append-only: the resolver fills them and consumers
// (codegen, the future ownership checker) read them. AST nodes are not
// modified.
class Resolution {
public:
    [[nodiscard]] TypePtr type_of(const ast::Expr* e) const;
    [[nodiscard]] const Symbol* symbol_of(const ast::Expr* e) const;

    void set_type(const ast::Expr* e, TypePtr t);
    void set_symbol(const ast::Expr* e, const Symbol* s);

private:
    std::unordered_map<const ast::Expr*, TypePtr> expr_types_;
    std::unordered_map<const ast::Expr*, const Symbol*> expr_symbols_;
};

// The resolver. Run `.resolve()` once; afterwards `resolution()` carries the
// findings and the reporter carries the diagnostics.
class Resolver {
public:
    Resolver(const ast::CompilationUnit& unit,
             TypeArena& types,
             diag::DiagnosticReporter& reporter);

    Resolver(const Resolver&) = delete;
    Resolver& operator=(const Resolver&) = delete;

    void resolve();

    [[nodiscard]] const Resolution& resolution() const noexcept { return resolution_; }

private:
    // ---- pass 1: collect top-level decls ---------------------------------
    void collect_top_level();
    void collect_func(const ast::FuncDecl& f);
    void collect_struct(const ast::StructDecl& s);
    void collect_enum(const ast::EnumDecl& e);
    void collect_protocol(const ast::ProtocolDecl& p);
    void collect_opaque(const ast::OpaqueDecl& o);
    void collect_const(const ast::ConstDecl& c);
    void collect_static(const ast::StaticDecl& s);

    // ---- pass 2: resolve bodies ------------------------------------------
    void check_decl(const ast::Decl& d);
    void check_func(const ast::FuncDecl& f);

    void check_block(const ast::BlockExpr& b);
    void check_stmt(const ast::Stmt& s);

    // Compute and record an expression's type. `expected` is the type the
    // context wants — used for literal adaptation; pass nullptr when there
    // is no contextual type (the result then defaults).
    TypePtr check_expr(const ast::Expr& e, TypePtr expected = nullptr);
    TypePtr check_binary(const ast::BinaryExpr& b, TypePtr expected);
    TypePtr check_unary(const ast::UnaryExpr& u, TypePtr expected);
    TypePtr check_call(const ast::CallExpr& c);
    TypePtr check_if(const ast::IfExpr& i, TypePtr expected);
    TypePtr check_block_expr(const ast::BlockExpr& b, TypePtr expected);

    // Resolve an `ast::Type` node into a `sema::TypePtr`.
    TypePtr resolve_type(const ast::Type& t);
    TypePtr resolve_type_opt(const ast::Type* t, TypePtr fallback);

    // ---- helpers ---------------------------------------------------------
    void error_at(diag::SourceRange r, std::string msg);
    void duplicate_definition(const Symbol& existing,
                              std::string_view name,
                              diag::SourceRange new_range);

    // Map a function decl to its full function type.
    TypePtr function_type_of(const ast::FuncDecl& f);

    const ast::CompilationUnit* unit_;
    TypeArena* types_;
    diag::DiagnosticReporter* reporter_;
    ScopeStack scopes_;
    Resolution resolution_;
    // Stack of expected return types — pushed when entering a function body so
    // a nested return expression can be checked against it.
    std::vector<TypePtr> return_stack_;
};

}  // namespace vestra::sema
