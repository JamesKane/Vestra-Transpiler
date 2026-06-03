// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#pragma once

#include "vestra/ast/nodes.hpp"
#include "vestra/diag/diagnostic.hpp"
#include "vestra/lex/token.hpp"

#include <cstddef>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace vestra::parse {

// Recursive-descent parser for §17's grammar. Expressions use Pratt parsing to
// honor §17.7's precedence table cleanly. The parser is intentionally
// non-throwing: every error pushes a Diagnostic onto the reporter and the
// parser resyncs at the next statement/declaration boundary.
class Parser {
public:
    Parser(std::span<const lex::Token> tokens, diag::DiagnosticReporter& reporter);

    Parser(const Parser&) = delete;
    Parser& operator=(const Parser&) = delete;

    // Parse a whole compilation unit. Even on errors the returned unit is
    // populated with whatever parsed successfully — the reporter is the
    // source of truth for "did we succeed."
    ast::CompilationUnit parse_unit();

private:
    // ---- token cursor ----------------------------------------------------
    [[nodiscard]] const lex::Token& peek(std::size_t ahead = 0) const noexcept;
    const lex::Token& advance() noexcept;
    bool check(lex::TokenKind k) const noexcept;
    bool match(lex::TokenKind k) noexcept;
    bool expect(lex::TokenKind k, std::string_view what);
    void skip_newlines() noexcept;
    void sync_to_decl() noexcept;
    [[nodiscard]] bool at_end() const noexcept;

    // ---- declarations ----------------------------------------------------
    std::unique_ptr<ast::ModuleDecl> parse_module();
    std::unique_ptr<ast::ImportDecl> parse_import();
    ast::DeclPtr parse_decl(std::vector<ast::Attribute> attrs);
    std::unique_ptr<ast::FuncDecl> parse_func(std::vector<ast::Attribute> attrs,
                                              ast::Visibility vis,
                                              bool is_async,
                                              ast::FuncDecl::RecvMode recv,
                                              bool is_comptime = false);
    std::unique_ptr<ast::StructDecl>
    parse_struct(std::vector<ast::Attribute> attrs, ast::Visibility vis, bool is_linear = false);
    std::unique_ptr<ast::EnumDecl> parse_enum(std::vector<ast::Attribute> attrs,
                                              ast::Visibility vis);
    std::unique_ptr<ast::ProtocolDecl> parse_protocol(ast::Visibility vis);
    std::unique_ptr<ast::ExtensionDecl> parse_extension(std::vector<ast::Attribute> attrs);
    std::unique_ptr<ast::OpaqueDecl> parse_opaque(ast::Visibility vis);
    std::unique_ptr<ast::ConstDecl> parse_const(ast::Visibility vis);
    std::unique_ptr<ast::StaticDecl> parse_static(std::vector<ast::Attribute> attrs,
                                                  ast::Visibility vis);
    std::unique_ptr<ast::DeriveDecl> parse_derive();

    // ---- shared sub-productions ------------------------------------------
    std::vector<ast::Attribute> parse_attributes();
    ast::Visibility parse_visibility();
    std::vector<ast::GenericParam> parse_generics_opt();
    // §7 generics: parse an optional `where T: P, U: P & Q` clause and merge
    // each refinement onto the matching already-parsed generic parameter.
    void parse_where_opt(std::vector<ast::GenericParam>& generics);
    std::vector<ast::Param> parse_params();
    ast::Effects parse_effects_opt();

    // ---- types / patterns / statements / expressions ---------------------
    ast::TypePtr parse_type();
    ast::PatternPtr parse_pattern();
    // §17.7 single-pattern alternative (no top-level or-chaining).
    // Recursive sub-pattern sites (tuple elements, enum-payload
    // children, range bounds) use this so a nested `|` doesn't change
    // the surrounding grouping's meaning.
    ast::PatternPtr parse_pattern_alt();

    std::unique_ptr<ast::BlockExpr> parse_block_expr();
    ast::StmtPtr parse_statement();

    // Pratt expression parser.
    ast::ExprPtr parse_expr();
    ast::ExprPtr parse_expr_bp(int min_bp);
    ast::ExprPtr parse_prefix();
    ast::ExprPtr parse_primary();
    ast::ExprPtr parse_postfix(ast::ExprPtr lhs);
    // §12.4 parse just a `$ident` / `$(expr)` splice (no postfix), for splices
    // in name / member / type position where the surrounding `()`/`.` must not
    // be consumed as a call or member of the splice.
    ast::ExprPtr parse_splice();

    // ---- helpers ---------------------------------------------------------
    void emit_error(diag::SourceRange r, std::string msg);
    [[nodiscard]] diag::SourceRange last_range() const noexcept;

    std::span<const lex::Token> tokens_;
    std::size_t pos_ = 0;
    diag::DiagnosticReporter* reporter_ = nullptr;
};

}  // namespace vestra::parse
