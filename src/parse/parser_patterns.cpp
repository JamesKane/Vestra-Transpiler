// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// Pattern parsing: the §17.7 pattern grammar and its single-alternative
// sub-production.
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

ast::PatternPtr Parser::parse_pattern() {
    // §17.7 or-pattern: `pat1 | pat2 | …` is a comma-separated chain
    // of single-pattern alternatives. Parse the first; if `|` follows,
    // accumulate. Single-pattern paths return the alt directly.
    auto first = parse_pattern_alt();
    if (!check(TokenKind::Pipe)) {
        return first;
    }
    auto op = std::make_unique<ast::OrPat>();
    op->range = first->range;
    op->alternatives.push_back(std::move(first));
    while (match(TokenKind::Pipe)) {
        op->alternatives.push_back(parse_pattern_alt());
    }
    op->range = merge(op->range, last_range());
    return op;
}

ast::PatternPtr Parser::parse_pattern_alt() {
    auto start = peek().range;
    if (match(TokenKind::Underscore)) {
        auto w = std::make_unique<ast::WildcardPat>();
        w->range = merge(start, last_range());
        return w;
    }
    if (match(TokenKind::LParen)) {
        // §6 `(p, q, ...)` is a tuple pattern. A single `(p)` is just
        // a parenthesized inner pattern (paren noise), not a 1-tuple.
        std::vector<ast::PatternPtr> elems;
        if (!check(TokenKind::RParen)) {
            elems.push_back(parse_pattern());
            while (match(TokenKind::Comma)) {
                elems.push_back(parse_pattern());
            }
        }
        expect(TokenKind::RParen, "')' closing tuple pattern");
        if (elems.size() == 1) {
            return std::move(elems[0]);
        }
        auto tp = std::make_unique<ast::TuplePat>();
        tp->elements = std::move(elems);
        tp->range = merge(start, last_range());
        return tp;
    }
    if (match(TokenKind::KwLet)) {
        auto b = std::make_unique<ast::BindPat>();
        if (!check(TokenKind::Identifier)) {
            emit_error(peek().range, "expected identifier after 'let' in pattern");
        } else {
            b->name = std::string{advance().lexeme};
        }
        b->range = merge(start, last_range());
        return b;
    }
    if (match(TokenKind::Dot)) {
        auto e = std::make_unique<ast::EnumPat>();
        if (!check(TokenKind::Identifier)) {
            emit_error(peek().range, "expected case name after '.' in pattern");
        } else {
            e->case_name = std::string{advance().lexeme};
        }
        if (match(TokenKind::LParen)) {
            if (!check(TokenKind::RParen)) {
                e->children.push_back(parse_pattern());
                while (match(TokenKind::Comma)) {
                    e->children.push_back(parse_pattern());
                }
            }
            expect(TokenKind::RParen, "')' closing enum-pattern payload");
        }
        e->range = merge(start, last_range());
        return e;
    }
    if (check(TokenKind::Identifier)) {
        auto i = std::make_unique<ast::IdentPat>();
        i->name = std::string{advance().lexeme};
        i->range = merge(start, last_range());
        return i;
    }
    if (check(TokenKind::IntLit) || check(TokenKind::FloatLit) || check(TokenKind::StringLit)
        || check(TokenKind::CharLit) || check(TokenKind::KwTrue) || check(TokenKind::KwFalse)) {
        auto literal = parse_primary();
        // §17.7 range pattern: `low ..< high` (exclusive) or `low .. high`
        // (inclusive). Both bounds are literals at the syntactic surface;
        // sema requires them to fold to integer constants. If no range
        // operator follows, this stays a plain LiteralPat.
        if (check(TokenKind::DotDot) || check(TokenKind::DotDotLt)) {
            auto rp = std::make_unique<ast::RangePat>();
            rp->inclusive = check(TokenKind::DotDot);
            advance();  // consume `..` / `..<`
            rp->low = std::move(literal);
            rp->high = parse_primary();
            rp->range = merge(start, last_range());
            return rp;
        }
        auto lp = std::make_unique<ast::LiteralPat>();
        lp->literal = std::move(literal);
        lp->range = merge(start, last_range());
        return lp;
    }
    emit_error(peek().range, "expected pattern");
    auto stub = std::make_unique<ast::WildcardPat>();
    stub->range = peek().range;
    return stub;
}

}  // namespace vestra::parse
