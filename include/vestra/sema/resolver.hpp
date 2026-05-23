// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#pragma once

#include "vestra/ast/nodes.hpp"
#include "vestra/diag/diagnostic.hpp"
#include "vestra/sema/comptime.hpp"
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
    // §12.1 fold result — only expressions the comptime folder could
    // evaluate at compile time get an entry here. Codegen uses it to emit
    // a literal in place of the source expression.
    [[nodiscard]] const ComptimeValue* folded_value(const ast::Expr* e) const;

    void set_type(const ast::Expr* e, TypePtr t);
    void set_symbol(const ast::Expr* e, const Symbol* s);
    void set_folded_value(const ast::Expr* e, ComptimeValue v);

private:
    std::unordered_map<const ast::Expr*, TypePtr> expr_types_;
    std::unordered_map<const ast::Expr*, const Symbol*> expr_symbols_;
    std::unordered_map<const ast::Expr*, ComptimeValue> folded_;
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
    void register_builtin_capabilities();
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

    void check_stmt(const ast::Stmt& s);

    // Compute and record an expression's type. `expected` is the type the
    // context wants — used for literal adaptation; pass nullptr when there
    // is no contextual type (the result then defaults).
    TypePtr check_expr(const ast::Expr& e, TypePtr expected = nullptr);
    TypePtr check_binary(const ast::BinaryExpr& b, TypePtr expected);
    TypePtr check_unary(const ast::UnaryExpr& u, TypePtr expected);
    TypePtr check_call(const ast::CallExpr& c, TypePtr expected = nullptr);
    TypePtr check_if(const ast::IfExpr& i, TypePtr expected);
    TypePtr check_match(const ast::MatchExpr& m, TypePtr expected);
    TypePtr check_block_expr(const ast::BlockExpr& b, TypePtr expected);
    TypePtr check_member(const ast::MemberExpr& m);
    TypePtr check_leading_dot(const ast::LeadingDotExpr& d, TypePtr expected);

    // Resolve an `ast::Type` node into a `sema::TypePtr`.
    TypePtr resolve_type(const ast::Type& t);

    // ---- struct / enum lookup helpers ------------------------------------

    // Look up a field by name on the struct type. Walks `embed` fields
    // recursively per §6 so embedded fields surface in the enclosing
    // namespace. `out_field` is populated with the resolved AST field when
    // the lookup succeeds. Returns nullptr if the field is not found.
    [[nodiscard]] TypePtr lookup_field(TypePtr struct_type,
                                       std::string_view name,
                                       const ast::StructDecl::Field** out_field = nullptr);

    // Look up a method by name on a struct/enum type. Returns the method's
    // full function type. `out_method` is set to the resolved FuncDecl.
    [[nodiscard]] TypePtr lookup_method(TypePtr owner_type,
                                        std::string_view name,
                                        const ast::FuncDecl** out_method = nullptr);

    // Unify a generic parameter type against a concrete arg type, growing the
    // bindings map. A clash (same generic implied to be two different types
    // by different args) is reported at `site`.
    void unify_generic(TypePtr ptype,
                       TypePtr atype,
                       std::unordered_map<std::string, TypePtr>& bindings,
                       diag::SourceRange site);

    // Find an enum case by name on the given enum decl.
    [[nodiscard]] const ast::EnumDecl::Case* lookup_enum_case(const ast::EnumDecl& e,
                                                              std::string_view name);

    // The constructor type for an enum case: the nominal enum for a bare
    // case, or a function (T1, T2, ...) -> Enum for a payload case.
    [[nodiscard]] TypePtr enum_case_constructor_type(TypePtr enum_type,
                                                     const ast::EnumDecl::Case& c);

    // Pattern-typing for match arms. `scrutinee` is the type the arm sees.
    // Binds any `let`-pattern names into the current scope.
    void check_pattern(const ast::Pattern& p, TypePtr scrutinee);

    // Visibility check: report if `sym` is referenced from outside its
    // declaring scope when its visibility forbids it.
    void check_visibility(const Symbol& sym, diag::SourceRange use_range);

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
    // Comptime folder + the const environment it folds against. The env
    // accumulates name→value pairs as we successfully fold each top-level
    // const, so later consts can reference earlier ones. The folder gets
    // a pointer to the global scope so it can look comptime funcs up by
    // name when it encounters a call expression (§12.1 phase 2).
    ComptimeFolder folder_{&scopes_.global()};
    ComptimeFolder::Env comptime_env_;
};

}  // namespace vestra::sema
