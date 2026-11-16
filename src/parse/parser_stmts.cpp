// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// Statement and block parsing.
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

namespace {

// §17.7 assignment operators — recognised by parse_statement to build an
// AssignStmt (the compound forms desugar in sema, not here).
bool is_assign_op(TokenKind k) {
    switch (k) {
    case TokenKind::Assign:
    case TokenKind::PlusEq:
    case TokenKind::MinusEq:
    case TokenKind::StarEq:
    case TokenKind::SlashEq:
    case TokenKind::PercentEq:
    case TokenKind::AmpEq:
    case TokenKind::PipeEq:
    case TokenKind::CaretEq:
    case TokenKind::ShiftLEq:
    case TokenKind::ShiftREq:
        return true;
    default:
        return false;
    }
}

ast::AssignOp token_to_assign(TokenKind k) {
    switch (k) {
    case TokenKind::Assign:
        return ast::AssignOp::Assign;
    case TokenKind::PlusEq:
        return ast::AssignOp::AddAssign;
    case TokenKind::MinusEq:
        return ast::AssignOp::SubAssign;
    case TokenKind::StarEq:
        return ast::AssignOp::MulAssign;
    case TokenKind::SlashEq:
        return ast::AssignOp::DivAssign;
    case TokenKind::PercentEq:
        return ast::AssignOp::ModAssign;
    case TokenKind::AmpEq:
        return ast::AssignOp::BitAndAssign;
    case TokenKind::PipeEq:
        return ast::AssignOp::BitOrAssign;
    case TokenKind::CaretEq:
        return ast::AssignOp::BitXorAssign;
    case TokenKind::ShiftLEq:
        return ast::AssignOp::ShlAssign;
    case TokenKind::ShiftREq:
        return ast::AssignOp::ShrAssign;
    default:
        return ast::AssignOp::Assign;
    }
}

}  // namespace

std::unique_ptr<ast::BlockExpr> Parser::parse_block_expr() {
    auto start = peek().range;
    auto blk = std::make_unique<ast::BlockExpr>();
    expect(TokenKind::LBrace, "'{' to open block");
    skip_newlines();
    while (!check(TokenKind::RBrace) && !at_end()) {
        auto s = parse_statement();
        if (s) {
            blk->stmts.push_back(std::move(s));
        }
        skip_newlines();
    }
    expect(TokenKind::RBrace, "'}' to close block");
    blk->range = merge(start, last_range());
    return blk;
}

ast::StmtPtr Parser::parse_statement() {
    auto start = peek().range;
    auto kind = peek().kind;
    // §14.4 `asm` is contextual: an identifier `asm` opening a statement and
    // followed by `(` or `{` (the Annex reserves no new words).
    if (kind == TokenKind::Identifier && peek().lexeme == "asm"
        && (peek(1).kind == TokenKind::LParen || peek(1).kind == TokenKind::LBrace)) {
        return parse_asm_stmt();
    }
    // §A4 (§14.9.3) `repeat { BODY } while COND` is contextual the same way:
    // an identifier `repeat` opening a statement and followed by `{`.
    if (kind == TokenKind::Identifier && peek().lexeme == "repeat"
        && peek(1).kind == TokenKind::LBrace) {
        advance();
        auto w = std::make_unique<ast::WhileStmt>();
        w->repeat = true;
        w->body = parse_block_expr();
        expect(TokenKind::KwWhile, "`while` after a `repeat` block");
        w->cond = parse_expr();
        w->range = merge(start, last_range());
        return w;
    }
    switch (kind) {
    case TokenKind::KwLet: {
        advance();
        auto s = std::make_unique<ast::LetStmt>();
        s->pattern = parse_pattern();
        if (match(TokenKind::Colon)) {
            s->type = parse_type();
        }
        expect(TokenKind::Assign, "'=' in let binding");
        s->value = parse_expr();
        s->range = merge(start, last_range());
        return s;
    }
    case TokenKind::KwVar: {
        advance();
        auto s = std::make_unique<ast::VarStmt>();
        s->pattern = parse_pattern();
        if (match(TokenKind::Colon)) {
            s->type = parse_type();
        }
        expect(TokenKind::Assign, "'=' in var binding");
        s->value = parse_expr();
        s->range = merge(start, last_range());
        return s;
    }
    case TokenKind::KwReturn: {
        advance();
        auto r = std::make_unique<ast::ReturnStmt>();
        if (!check(TokenKind::Newline) && !check(TokenKind::Semicolon) && !check(TokenKind::RBrace)
            && !check(TokenKind::Eof)) {
            r->value = parse_expr();
        }
        r->range = merge(start, last_range());
        return r;
    }
    case TokenKind::KwBreak: {
        advance();
        auto b = std::make_unique<ast::BreakStmt>();
        if (check(TokenKind::Identifier)) {
            b->label = std::string{advance().lexeme};
        }
        b->range = merge(start, last_range());
        return b;
    }
    case TokenKind::KwContinue: {
        advance();
        auto c = std::make_unique<ast::ContinueStmt>();
        if (check(TokenKind::Identifier)) {
            c->label = std::string{advance().lexeme};
        }
        c->range = merge(start, last_range());
        return c;
    }
    case TokenKind::KwWhile: {
        advance();
        // §9 `while let NAME = INIT { BODY }` desugars to
        // `while true { if let NAME = INIT { BODY } else { break } }`: INIT is
        // re-evaluated each time round, and the loop ends at the first nil
        // (F18).
        if (check(TokenKind::KwLet)) {
            advance();
            auto ifx = std::make_unique<ast::IfExpr>();
            if (check(TokenKind::Identifier)) {
                ifx->let_name = std::string{advance().lexeme};
            } else {
                emit_error(peek().range, "expected a name after `while let`");
            }
            expect(TokenKind::Assign, "'=' in while let");
            ifx->let_init = parse_expr();
            ifx->then_branch = parse_block_expr();
            auto brk = std::make_unique<ast::BreakStmt>();
            brk->range = last_range();
            auto els = std::make_unique<ast::BlockExpr>();
            els->range = last_range();
            els->stmts.push_back(std::move(brk));
            ifx->else_branch = std::move(els);
            ifx->range = merge(start, last_range());
            auto es = std::make_unique<ast::ExprStmt>();
            es->range = ifx->range;
            es->expr = std::move(ifx);
            auto body = std::make_unique<ast::BlockExpr>();
            body->range = es->range;
            body->stmts.push_back(std::move(es));
            auto tru = std::make_unique<ast::BoolLit>();
            tru->value = true;
            tru->range = start;
            auto w = std::make_unique<ast::WhileStmt>();
            w->cond = std::move(tru);
            w->body = std::move(body);
            w->range = merge(start, last_range());
            return w;
        }
        auto w = std::make_unique<ast::WhileStmt>();
        w->cond = parse_expr();
        w->body = parse_block_expr();
        w->range = merge(start, last_range());
        return w;
    }
    case TokenKind::KwFor: {
        advance();
        auto f = std::make_unique<ast::ForStmt>();
        f->pattern = parse_pattern();
        expect(TokenKind::KwIn, "'in' in for loop");
        f->iter = parse_expr();
        f->body = parse_block_expr();
        f->range = merge(start, last_range());
        return f;
    }
    case TokenKind::KwWith: {
        // §17.4: `with` w-bind (',' w-bind)* block
        // w-bind admits three shapes:
        //   * `TYPE`          — marker capability (no `=`, no name)
        //   * `TYPE = EXPR`   — capability satisfaction with value
        //   * `name = EXPR`   — value binding (no capability)
        // The first letter of the leading identifier disambiguates the
        // value-binding form from a cap-typed form: lowercase ⇒ name
        // binding, uppercase ⇒ type. Vestra's casing convention
        // (types PascalCase, identifiers camelCase) makes this
        // unambiguous in practice.
        advance();
        auto w = std::make_unique<ast::WithStmt>();
        do {
            ast::WithBinding b;
            // §17.4 disambiguator: a lowercase-leading identifier
            // followed by `=` *or* `:` is the name-binding shape
            // (with or without an explicit type annotation). Vestra
            // casing convention pins types PascalCase and identifiers
            // camelCase, so the leading-letter test is unambiguous.
            const bool name_binding =
                check(TokenKind::Identifier)
                && (peek(1).kind == TokenKind::Assign || peek(1).kind == TokenKind::Colon)
                && !peek().lexeme.empty()
                && (peek().lexeme.front() >= 'a' && peek().lexeme.front() <= 'z');
            if (name_binding) {
                b.name = std::string{advance().lexeme};
                // §17.4 optional type annotation: `with NAME: T = EXPR`.
                // The annotation is useful when the user wants the
                // binding's type spelled out at the source (and at the
                // C++ layer) instead of `auto&&`'s inferred form. Sema
                // type-checks the value against the annotation.
                if (match(TokenKind::Colon)) {
                    b.type_annotation = parse_type();
                }
                expect(TokenKind::Assign, "'=' in `with name = expr` binding");
                b.value = parse_expr();
            } else {
                b.cap_type = parse_type();
                if (match(TokenKind::Assign)) {
                    b.value = parse_expr();
                }
            }
            w->bindings.push_back(std::move(b));
        } while (match(TokenKind::Comma));
        w->body = parse_block_expr();
        w->range = merge(start, last_range());
        return w;
    }
    default:
        break;
    }

    // Expression statement (possibly an assignment).
    auto expr = parse_expr();
    if (is_assign_op(peek().kind)) {
        auto a = std::make_unique<ast::AssignStmt>();
        a->op = token_to_assign(advance().kind);
        a->target = std::move(expr);
        a->value = parse_expr();
        a->range = merge(start, last_range());
        return a;
    }
    auto e = std::make_unique<ast::ExprStmt>();
    e->expr = std::move(expr);
    e->range = merge(start, last_range());
    return e;
}

// §14.4 `asm(volatile: B, in: [...], out: [...], inout: [...], clobbers: [...]) {
//     "instruction" ... }`. Each label at most once, in any order; the parens
// are optional when there is nothing to say. Operand entries:
//   name           — a named operand bound to the variable `name`
//   name: expr     — a named operand bound to `expr`
//   "reg": expr    — `expr` pinned to the register `reg`
ast::StmtPtr Parser::parse_asm_stmt() {
    auto start = peek().range;
    advance();  // `asm`
    auto a = std::make_unique<ast::AsmStmt>();
    if (match(TokenKind::LParen)) {
        std::vector<std::string_view> seen;
        skip_newlines();
        while (!check(TokenKind::RParen) && !at_end()) {
            const auto label_tok = peek();
            const std::string_view label = label_tok.lexeme;
            advance();
            expect(TokenKind::Colon, "':' after an asm label");
            for (auto s : seen) {
                if (s == label) {
                    emit_error(label_tok.range, std::format("duplicate asm label '{}'", label));
                }
            }
            seen.push_back(label);
            if (label == "volatile") {
                a->volatile_written = true;
                if (check(TokenKind::KwTrue) || check(TokenKind::KwFalse)) {
                    a->is_volatile = advance().kind == TokenKind::KwTrue;
                } else {
                    emit_error(peek().range, "asm 'volatile:' expects true or false");
                    advance();
                }
            } else if (label == "in") {
                parse_asm_operands(a->inputs, false);
            } else if (label == "out") {
                parse_asm_operands(a->outputs, true);
            } else if (label == "inout") {
                parse_asm_operands(a->inouts, true);
            } else if (label == "clobbers") {
                expect(TokenKind::LBracket, "'[' to open the asm clobber list");
                skip_newlines();
                while (!check(TokenKind::RBracket) && !at_end()) {
                    if (check(TokenKind::StringLit)) {
                        const auto& t = advance();
                        a->clobbers.emplace_back(t.lexeme.substr(1, t.lexeme.size() - 2));
                        a->clobber_ranges.push_back(t.range);
                    } else {
                        emit_error(peek().range, "an asm clobber is a string: a register, \"memory\" or \"cc\"");
                        advance();
                    }
                    skip_newlines();
                    if (!match(TokenKind::Comma)) {
                        break;
                    }
                    skip_newlines();
                }
                skip_newlines();
                expect(TokenKind::RBracket, "']' to close the asm clobber list");
            } else {
                emit_error(label_tok.range,
                         std::format("unknown asm label '{}'; expected volatile, in, out, inout "
                                     "or clobbers",
                                     label));
            }
            skip_newlines();
            if (!match(TokenKind::Comma)) {
                break;
            }
            skip_newlines();
        }
        expect(TokenKind::RParen, "')' to close the asm arguments");
    }
    expect(TokenKind::LBrace, "'{' to open the asm template");
    skip_newlines();
    while (!check(TokenKind::RBrace) && !at_end()) {
        if (check(TokenKind::StringLit)) {
            const auto& t = advance();
            a->lines.emplace_back(t.lexeme.substr(1, t.lexeme.size() - 2));
            a->line_ranges.push_back(t.range);
        } else {
            emit_error(peek().range, "an asm template holds only string literals, one instruction each");
            advance();
        }
        skip_newlines();
    }
    expect(TokenKind::RBrace, "'}' to close the asm template");
    a->range = merge(start, last_range());
    return a;
}

bool Parser::parse_asm_operands(std::vector<ast::AsmOperand>& out, bool /*place*/) {
    expect(TokenKind::LBracket, "'[' to open an asm operand list");
    skip_newlines();
    while (!check(TokenKind::RBracket) && !at_end()) {
        ast::AsmOperand op;
        auto start = peek().range;
        if (check(TokenKind::StringLit) && peek(1).kind == TokenKind::Colon) {
            const auto& t = advance();
            op.reg = std::string{t.lexeme.substr(1, t.lexeme.size() - 2)};
            advance();  // ':'
            op.value = parse_expr();
        } else if (check(TokenKind::Identifier) && peek(1).kind == TokenKind::Colon) {
            op.name = std::string{advance().lexeme};
            advance();  // ':'
            op.value = parse_expr();
        } else {
            op.value = parse_expr();
            if (op.value && op.value->kind == ast::NodeKind::IdentExpr) {
                op.name = static_cast<const ast::IdentExpr&>(*op.value).name;
            } else {
                emit_error(start,
                         "an asm operand needs a name: write `name: expr` (used as {name}), or "
                         "pin it with `\"reg\": expr`");
            }
        }
        op.range = merge(start, last_range());
        out.push_back(std::move(op));
        skip_newlines();
        if (!match(TokenKind::Comma)) {
            break;
        }
        skip_newlines();
    }
    skip_newlines();
    return expect(TokenKind::RBracket, "']' to close an asm operand list");
}

}  // namespace vestra::parse
