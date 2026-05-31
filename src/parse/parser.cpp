// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "vestra/parse/parser.hpp"

#include "vestra/lex/token.hpp"

#include "parser_internal.hpp"

#include <cassert>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace vestra::parse {

// `merge` (range-spanning helper) lives in parser_internal.hpp so the
// file can be split by theme. Pull it into this TU's scope unqualified so
// existing call sites and the extracted siblings keep working.
using detail::merge;

namespace {

using lex::TokenKind;

}  // namespace

// ============================================================================
// ctor + cursor
// ============================================================================

Parser::Parser(std::span<const lex::Token> tokens, diag::DiagnosticReporter& reporter)
    : tokens_(tokens), reporter_(&reporter) {}

const lex::Token& Parser::peek(std::size_t ahead) const noexcept {
    auto idx = pos_ + ahead;
    if (idx >= tokens_.size()) {
        return tokens_.back();  // EOF — the lexer always emits it
    }
    return tokens_[idx];
}

const lex::Token& Parser::advance() noexcept {
    const auto& t = tokens_[pos_];
    if (pos_ + 1 < tokens_.size()) {
        ++pos_;
    }
    return t;
}

bool Parser::check(TokenKind k) const noexcept {
    return peek().kind == k;
}

bool Parser::match(TokenKind k) noexcept {
    if (!check(k)) {
        return false;
    }
    advance();
    return true;
}

bool Parser::expect(TokenKind k, std::string_view what) {
    if (match(k)) {
        return true;
    }
    emit_error(peek().range,
               std::format(
                   "expected {} ({}), got {}", what, lex::spelling(k), lex::spelling(peek().kind)));
    return false;
}

void Parser::skip_newlines() noexcept {
    while (check(TokenKind::Newline) || check(TokenKind::Semicolon)) {
        advance();
    }
}

void Parser::sync_to_decl() noexcept {
    // Drop tokens until we find a plausible declaration start.
    while (!at_end()) {
        switch (peek().kind) {
        case TokenKind::Newline:
            advance();
            return;
        case TokenKind::KwFunc:
        case TokenKind::KwStruct:
        case TokenKind::KwEnum:
        case TokenKind::KwProtocol:
        case TokenKind::KwExtension:
        case TokenKind::KwOpaque:
        case TokenKind::KwConst:
        case TokenKind::KwStatic:
        case TokenKind::KwImport:
        case TokenKind::KwModule:
        case TokenKind::KwDerive:
        case TokenKind::At:
        case TokenKind::KwPublic:
        case TokenKind::KwPrivate:
        case TokenKind::KwPackage:
        case TokenKind::KwInternal:
        case TokenKind::Eof:
            return;
        default:
            advance();
        }
    }
}

bool Parser::at_end() const noexcept {
    return peek().kind == TokenKind::Eof;
}

void Parser::emit_error(diag::SourceRange r, std::string msg) {
    reporter_->report(diag::Diagnostic::error(std::move(msg)).at(r));
}

diag::SourceRange Parser::last_range() const noexcept {
    if (pos_ == 0) {
        return tokens_.front().range;
    }
    return tokens_[pos_ - 1].range;
}

// ============================================================================
// top-level
// ============================================================================

ast::CompilationUnit Parser::parse_unit() {
    ast::CompilationUnit unit;
    skip_newlines();

    if (check(TokenKind::KwModule)) {
        unit.module = parse_module();
        skip_newlines();
    }
    while (check(TokenKind::KwImport)) {
        unit.imports.push_back(parse_import());
        skip_newlines();
    }
    while (!at_end()) {
        if (reporter_->should_stop()) {
            break;
        }
        auto attrs = parse_attributes();
        if (at_end()) {
            break;
        }
        auto decl = parse_decl(std::move(attrs));
        if (decl) {
            unit.decls.push_back(std::move(decl));
        } else {
            sync_to_decl();
        }
        skip_newlines();
    }
    return unit;
}

std::unique_ptr<ast::ModuleDecl> Parser::parse_module() {
    auto start = peek().range;
    advance();  // 'module'
    auto m = std::make_unique<ast::ModuleDecl>();
    if (!check(TokenKind::Identifier)) {
        emit_error(peek().range, "expected module name after 'module'");
        return m;
    }
    m->path.push_back(std::string{advance().lexeme});
    while (match(TokenKind::Dot)) {
        if (!check(TokenKind::Identifier)) {
            emit_error(peek().range, "expected identifier after '.' in module path");
            break;
        }
        m->path.push_back(std::string{advance().lexeme});
    }
    m->range = merge(start, last_range());
    return m;
}

std::unique_ptr<ast::ImportDecl> Parser::parse_import() {
    auto start = peek().range;
    advance();  // 'import'
    auto imp = std::make_unique<ast::ImportDecl>();
    if (check(TokenKind::Identifier) && peek().lexeme == "c") {
        advance();
        if (!check(TokenKind::StringLit)) {
            emit_error(peek().range, "expected string literal after 'import c'");
        } else {
            auto t = advance();
            // Drop surrounding quotes.
            imp->is_c_header = true;
            imp->c_header = std::string{t.lexeme.substr(1, t.lexeme.size() - 2)};
        }
    } else {
        if (!check(TokenKind::Identifier)) {
            emit_error(peek().range, "expected identifier after 'import'");
        } else {
            imp->path.push_back(std::string{advance().lexeme});
            while (match(TokenKind::Dot)) {
                if (!check(TokenKind::Identifier)) {
                    emit_error(peek().range, "expected identifier after '.' in import");
                    break;
                }
                imp->path.push_back(std::string{advance().lexeme});
            }
        }
    }
    imp->range = merge(start, last_range());
    return imp;
}

// ============================================================================
// attributes, visibility, generics, params, effects
// ============================================================================

std::vector<ast::Attribute> Parser::parse_attributes() {
    std::vector<ast::Attribute> out;
    while (check(TokenKind::At)) {
        auto start = peek().range;
        advance();  // '@'
        ast::Attribute a;
        if (!check(TokenKind::Identifier)) {
            emit_error(peek().range, "expected attribute name after '@'");
            break;
        }
        a.name = std::string{advance().lexeme};
        if (match(TokenKind::LParen)) {
            // §12.6 wants attribute arguments to be ordinary comptime
            // expressions (so `@when(cfg.arch == .arm64)` is a Vestra
            // expression we can fold). Phase 1 supports a single expression
            // argument — enough for @when, @bits, @repr, etc. Multi-arg
            // attributes would extend Attribute to a vector and parse with
            // a comma loop here.
            if (!check(TokenKind::RParen)) {
                a.predicate = parse_expr();
            }
            expect(TokenKind::RParen, "closing ')' of attribute arguments");
        }
        a.range = merge(start, last_range());
        out.push_back(std::move(a));
        skip_newlines();
    }
    return out;
}

ast::Visibility Parser::parse_visibility() {
    if (match(TokenKind::KwPrivate)) {
        return ast::Visibility::Private;
    }
    if (match(TokenKind::KwInternal)) {
        return ast::Visibility::Internal;
    }
    if (match(TokenKind::KwPackage)) {
        return ast::Visibility::Package;
    }
    if (match(TokenKind::KwPublic)) {
        return ast::Visibility::Public;
    }
    return ast::Visibility::Internal;
}

std::vector<ast::GenericParam> Parser::parse_generics_opt() {
    std::vector<ast::GenericParam> out;
    if (!match(TokenKind::LBracket)) {
        return out;
    }

    do {
        ast::GenericParam g;
        auto start = peek().range;
        if (match(TokenKind::KwConst)) {
            g.is_const = true;
            if (!check(TokenKind::Identifier)) {
                emit_error(peek().range, "expected name for const generic parameter");
            } else {
                g.name = std::string{advance().lexeme};
            }
            expect(TokenKind::Colon, "':' after const generic parameter name");
            g.const_type = parse_type();
        } else {
            if (!check(TokenKind::Identifier)) {
                emit_error(peek().range, "expected generic parameter name");
            } else {
                g.name = std::string{advance().lexeme};
            }
            if (match(TokenKind::Colon)) {
                g.bound.protocols.push_back(parse_type());
                while (match(TokenKind::Amp)) {
                    g.bound.protocols.push_back(parse_type());
                }
            }
        }
        g.range = merge(start, last_range());
        out.push_back(std::move(g));
    } while (match(TokenKind::Comma));

    expect(TokenKind::RBracket, "closing ']' of generic parameter list");
    return out;
}

std::vector<ast::Param> Parser::parse_params() {
    std::vector<ast::Param> out;
    expect(TokenKind::LParen, "'(' to start parameter list");
    if (check(TokenKind::RParen)) {
        advance();
        return out;
    }
    do {
        ast::Param p;
        auto start = peek().range;
        // Per §17.2: (IDENT|'_')? IDENT ':' 'nonescaping'? pmode? type ('=' expr)?
        // The leading external label is optional; if a single identifier precedes
        // the colon, it is the parameter name and the label defaults to it.
        //
        // §6 tuple-pattern params: `((a, b): (Int32, Int32))` — when the
        // slot starts with `(`, parse a tuple pattern instead of a name.
        // The pattern's leaves become the in-body bindings; the C++
        // signature gets a synthetic argument name (codegen). Label and
        // mode keywords are not allowed here.
        if (check(TokenKind::LParen)) {
            p.label_omitted = true;
            p.pattern = parse_pattern();
        } else if (check(TokenKind::Underscore)) {
            p.label_omitted = true;
            advance();
            if (check(TokenKind::Identifier)) {
                p.name = std::string{advance().lexeme};
            }
        } else if (check(TokenKind::Identifier)) {
            std::string first = std::string{advance().lexeme};
            if (check(TokenKind::Identifier)) {
                p.label = first;
                p.name = std::string{advance().lexeme};
            } else {
                p.label = first;
                p.name = first;
            }
        }
        expect(TokenKind::Colon, "':' after parameter name");
        if (match(TokenKind::KwNonescaping)) {
            p.nonescaping = true;
        }
        if (match(TokenKind::KwInout)) {
            p.mode = ast::ParamMode::Inout;
        } else if (match(TokenKind::KwSink)) {
            p.mode = ast::ParamMode::Sink;
        } else if (match(TokenKind::KwRead)) {
            p.mode = ast::ParamMode::Read;
        }
        p.type = parse_type();
        if (match(TokenKind::Assign)) {
            p.default_value = parse_expr();
        }
        p.range = merge(start, last_range());
        out.push_back(std::move(p));
    } while (match(TokenKind::Comma));
    expect(TokenKind::RParen, "closing ')' of parameter list");
    return out;
}

ast::Effects Parser::parse_effects_opt() {
    ast::Effects e;
    if (match(TokenKind::KwUsing)) {
        e.using_caps.push_back(parse_type());
        while (match(TokenKind::Comma)) {
            e.using_caps.push_back(parse_type());
        }
    }
    if (match(TokenKind::KwThrows)) {
        expect(TokenKind::LParen, "'(' after 'throws'");
        e.throws_type = parse_type();
        expect(TokenKind::RParen, "closing ')' after 'throws(...)'");
    }
    return e;
}

}  // namespace vestra::parse
