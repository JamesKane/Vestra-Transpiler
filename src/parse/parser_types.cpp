// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// Type-expression parsing.
// Split out of parser.cpp; see parser_internal.hpp for the shared
// range-merge helper.

#include "vestra/lex/token.hpp"
#include "vestra/parse/parser.hpp"

#include "parser_internal.hpp"

#include <cassert>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace vestra::parse {

using detail::merge;
using lex::TokenKind;

ast::TypePtr Parser::parse_type() {
    auto start = peek().range;
    ast::TypePtr base;

    if (match(TokenKind::KwSome)) {
        auto s = std::make_unique<ast::SomeType>();
        s->inner = parse_type();
        s->range = merge(start, last_range());
        base = std::move(s);
    } else if (match(TokenKind::KwDyn)) {
        auto d = std::make_unique<ast::DynType>();
        d->inner = parse_type();
        d->range = merge(start, last_range());
        base = std::move(d);
    } else if (check(TokenKind::At)) {
        // §A8 (§14.5.3) `@interrupt(T)` in type position — vector-table
        // slot type. Only `@interrupt` is admitted here in v0.5; every
        // other attribute is a decl-level annotation, not a type form.
        advance();  // '@'
        if (!check(TokenKind::Identifier) || peek().lexeme != "interrupt") {
            emit_error(peek().range,
                       "expected 'interrupt' after '@' in type position "
                       "(only @interrupt(T) is admitted as a type)");
        } else {
            advance();  // 'interrupt'
        }
        expect(TokenKind::LParen, "'(' after @interrupt");
        auto it = std::make_unique<ast::InterruptType>();
        it->trap_frame = parse_type();
        expect(TokenKind::RParen, "')' after @interrupt(T)");
        it->range = merge(start, last_range());
        base = std::move(it);
    } else if (match(TokenKind::LBracket)) {
        // [N]T  — fixed vector
        auto v = std::make_unique<ast::VectorType>();
        if (check(TokenKind::IntLit)) {
            v->length = std::stoll(std::string{advance().lexeme});
        } else if (check(TokenKind::Identifier)) {
            v->length_ident = std::string{advance().lexeme};
        } else {
            emit_error(peek().range, "expected vector length (integer or identifier)");
        }
        expect(TokenKind::RBracket, "']' in vector type");
        v->element = parse_type();
        v->range = merge(start, last_range());
        base = std::move(v);
    } else if (match(TokenKind::LParen)) {
        // Either a tuple type or a function type: `(T,...) using ... throws(...) -> R`.
        std::vector<ast::TypePtr> parts;
        if (!check(TokenKind::RParen)) {
            parts.push_back(parse_type());
            while (match(TokenKind::Comma)) {
                parts.push_back(parse_type());
            }
        }
        expect(TokenKind::RParen, "closing ')' in tuple/function type");
        if (check(TokenKind::KwUsing) || check(TokenKind::KwThrows) || check(TokenKind::Arrow)) {
            auto fn = std::make_unique<ast::FunctionType>();
            fn->params = std::move(parts);
            fn->effects = parse_effects_opt();
            if (match(TokenKind::Arrow)) {
                fn->result = parse_type();
            }
            fn->range = merge(start, last_range());
            base = std::move(fn);
        } else if (parts.size() == 1) {
            base = std::move(parts[0]);  // (T) is just T
        } else {
            auto tup = std::make_unique<ast::TupleType>();
            tup->elements = std::move(parts);
            tup->range = merge(start, last_range());
            base = std::move(tup);
        }
    } else if (check(TokenKind::Identifier) || check(TokenKind::KwSelfUpper)) {
        auto n = std::make_unique<ast::NamedType>();
        n->path.push_back(std::string{advance().lexeme});
        while (match(TokenKind::Dot)) {
            if (!check(TokenKind::Identifier)) {
                emit_error(peek().range, "expected identifier after '.' in type path");
                break;
            }
            n->path.push_back(std::string{advance().lexeme});
        }
        if (match(TokenKind::LBracket)) {
            n->has_generics = true;
            if (!check(TokenKind::RBracket)) {
                do {
                    // Either `IDENT = type` (associated binding), an INT literal
                    // (const arg), or a type.
                    if (check(TokenKind::Identifier) && peek(1).kind == TokenKind::Assign) {
                        std::string nm = std::string{advance().lexeme};
                        advance();  // '='
                        n->assoc_bindings.emplace_back(std::move(nm), parse_type());
                    } else if (check(TokenKind::IntLit)) {
                        n->const_args.push_back(std::stoll(std::string{advance().lexeme}));
                    } else {
                        n->type_args.push_back(parse_type());
                    }
                } while (match(TokenKind::Comma));
            }
            expect(TokenKind::RBracket, "']' closing generic argument list");
        }
        n->range = merge(start, last_range());
        base = std::move(n);
    } else {
        emit_error(peek().range, "expected type");
        auto stub = std::make_unique<ast::NamedType>();
        stub->path.push_back("<error>");
        stub->range = peek().range;
        return stub;
    }

    // Postfix `?` for optional.
    while (match(TokenKind::Question)) {
        auto o = std::make_unique<ast::OptionalType>();
        o->inner = std::move(base);
        o->range = merge(start, last_range());
        base = std::move(o);
    }
    return base;
}

}  // namespace vestra::parse
