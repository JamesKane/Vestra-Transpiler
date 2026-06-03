// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// Expression parsing: the §17.7 Pratt parser (prefix / infix / postfix)
// and its binding-power and operator-mapping tables.
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

// Binding-power table for §17.7. Each entry is (left, right). Right-associative
// operators encode that by making right < left; left-associative is right > left.
struct BindingPower {
    int left, right;
};

BindingPower infix_bp(TokenKind k) {
    switch (k) {
    case TokenKind::Star:
    case TokenKind::Slash:
    case TokenKind::Percent:
    case TokenKind::AmpStar:
        return {130, 131};
    case TokenKind::Plus:
    case TokenKind::Minus:
    case TokenKind::AmpPlus:
    case TokenKind::AmpMinus:
        return {120, 121};
    case TokenKind::ShiftL:
    case TokenKind::ShiftR:
        return {110, 111};
    case TokenKind::Amp:
        return {100, 101};
    case TokenKind::Caret:
        return {90, 91};
    case TokenKind::Pipe:
        return {80, 81};
    case TokenKind::DotDot:
    case TokenKind::DotDotLt:
        return {70, 71};
    case TokenKind::EqEq:
    case TokenKind::BangEq:
    case TokenKind::Lt:
    case TokenKind::LtEq:
    case TokenKind::Gt:
    case TokenKind::GtEq:
        return {60, 61};
    case TokenKind::AmpAmp:
        return {50, 51};
    case TokenKind::PipePipe:
        return {40, 41};
    case TokenKind::QuestionQuestion:
        return {30, 31};
    default:
        return {0, 0};
    }
}

ast::BinaryOp token_to_binop(TokenKind k) {
    switch (k) {
    case TokenKind::Plus:
        return ast::BinaryOp::Add;
    case TokenKind::Minus:
        return ast::BinaryOp::Sub;
    case TokenKind::Star:
        return ast::BinaryOp::Mul;
    case TokenKind::Slash:
        return ast::BinaryOp::Div;
    case TokenKind::Percent:
        return ast::BinaryOp::Mod;
    case TokenKind::AmpPlus:
        return ast::BinaryOp::WrappingAdd;
    case TokenKind::AmpMinus:
        return ast::BinaryOp::WrappingSub;
    case TokenKind::AmpStar:
        return ast::BinaryOp::WrappingMul;
    case TokenKind::ShiftL:
        return ast::BinaryOp::Shl;
    case TokenKind::ShiftR:
        return ast::BinaryOp::Shr;
    case TokenKind::Amp:
        return ast::BinaryOp::BitAnd;
    case TokenKind::Caret:
        return ast::BinaryOp::BitXor;
    case TokenKind::Pipe:
        return ast::BinaryOp::BitOr;
    case TokenKind::DotDot:
        return ast::BinaryOp::Range;
    case TokenKind::DotDotLt:
        return ast::BinaryOp::RangeLt;
    case TokenKind::EqEq:
        return ast::BinaryOp::Eq;
    case TokenKind::BangEq:
        return ast::BinaryOp::Ne;
    case TokenKind::Lt:
        return ast::BinaryOp::Lt;
    case TokenKind::LtEq:
        return ast::BinaryOp::Le;
    case TokenKind::Gt:
        return ast::BinaryOp::Gt;
    case TokenKind::GtEq:
        return ast::BinaryOp::Ge;
    case TokenKind::AmpAmp:
        return ast::BinaryOp::And;
    case TokenKind::PipePipe:
        return ast::BinaryOp::Or;
    case TokenKind::QuestionQuestion:
        return ast::BinaryOp::Coalesce;
    default:
        return ast::BinaryOp::Add;  // unreachable
    }
}

}  // namespace

ast::ExprPtr Parser::parse_expr() {
    return parse_expr_bp(0);
}

ast::ExprPtr Parser::parse_expr_bp(int min_bp) {
    auto lhs = parse_prefix();
    while (true) {
        // `as` and `as?` are postfix at the casting precedence.
        if (check(TokenKind::KwAs) || check(TokenKind::KwAsQuestion)) {
            bool is_opt = check(TokenKind::KwAsQuestion);
            advance();
            auto a = std::make_unique<ast::AsExpr>();
            a->value = std::move(lhs);
            a->target = parse_type();
            a->is_optional = is_opt;
            a->range = merge(a->value->range, last_range());
            lhs = std::move(a);
            continue;
        }
        auto bp = infix_bp(peek().kind);
        if (bp.left < min_bp || bp.left == 0) {
            break;
        }

        auto op_kind = advance().kind;
        auto rhs = parse_expr_bp(bp.right);
        auto bin = std::make_unique<ast::BinaryExpr>();
        bin->op = token_to_binop(op_kind);
        bin->range = merge(lhs->range, rhs->range);
        bin->lhs = std::move(lhs);
        bin->rhs = std::move(rhs);
        lhs = std::move(bin);
    }
    return lhs;
}

ast::ExprPtr Parser::parse_prefix() {
    auto start = peek().range;
    if (match(TokenKind::KwCopy)) {
        auto c = std::make_unique<ast::CopyExpr>();
        c->inner = parse_prefix();
        c->range = merge(start, last_range());
        return c;
    }
    if (match(TokenKind::KwAwait)) {
        auto a = std::make_unique<ast::AwaitExpr>();
        a->inner = parse_prefix();
        a->range = merge(start, last_range());
        return a;
    }
    if (match(TokenKind::KwSpawn)) {
        auto s = std::make_unique<ast::SpawnExpr>();
        s->inner = parse_prefix();
        s->range = merge(start, last_range());
        return s;
    }
    if (match(TokenKind::KwThrow)) {
        auto t = std::make_unique<ast::ThrowExpr>();
        t->inner = parse_prefix();
        t->range = merge(start, last_range());
        return t;
    }
    if (check(TokenKind::KwTry) || check(TokenKind::KwTryQuestion) || check(TokenKind::KwTryBang)) {
        auto k = advance().kind;
        auto t = std::make_unique<ast::TryExpr>();
        t->form = (k == TokenKind::KwTryQuestion) ? ast::TryExpr::Form::Optional
                  : (k == TokenKind::KwTryBang)   ? ast::TryExpr::Form::Forced
                                                  : ast::TryExpr::Form::Propagating;
        t->inner = parse_prefix();
        t->range = merge(start, last_range());
        return t;
    }
    if (match(TokenKind::Minus)) {
        auto u = std::make_unique<ast::UnaryExpr>();
        u->op = ast::UnaryOp::Neg;
        u->operand = parse_prefix();
        u->range = merge(start, last_range());
        return u;
    }
    if (match(TokenKind::Bang)) {
        auto u = std::make_unique<ast::UnaryExpr>();
        u->op = ast::UnaryOp::Not;
        u->operand = parse_prefix();
        u->range = merge(start, last_range());
        return u;
    }
    if (match(TokenKind::Tilde)) {
        auto u = std::make_unique<ast::UnaryExpr>();
        u->op = ast::UnaryOp::BitNot;
        u->operand = parse_prefix();
        u->range = merge(start, last_range());
        return u;
    }
    // §A12 (§14.6.3) `&decl` — prefix address-of. Disambiguates
    // from the binary `&` (bit-and) by position: the binary form
    // is consumed at parse_binary's BitAnd precedence, so a `&`
    // landing here is unambiguously the prefix form.
    if (match(TokenKind::Amp)) {
        auto a = std::make_unique<ast::AddressOfExpr>();
        a->inner = parse_prefix();
        a->range = merge(start, last_range());
        return a;
    }
    auto prim = parse_primary();
    return parse_postfix(std::move(prim));
}

ast::ExprPtr Parser::parse_primary() {
    auto start = peek().range;
    auto k = peek().kind;
    switch (k) {
    case TokenKind::IntLit: {
        auto n = std::make_unique<ast::IntLit>();
        n->text = std::string{advance().lexeme};
        n->range = merge(start, last_range());
        return n;
    }
    case TokenKind::FloatLit: {
        auto n = std::make_unique<ast::FloatLit>();
        n->text = std::string{advance().lexeme};
        n->range = merge(start, last_range());
        return n;
    }
    case TokenKind::StringLit: {
        auto s = std::make_unique<ast::StringLit>();
        auto t = advance();
        // Strip the surrounding quotes for storage.
        if (t.lexeme.size() >= 2) {
            s->text = std::string{t.lexeme.substr(1, t.lexeme.size() - 2)};
        }
        s->range = merge(start, last_range());
        return s;
    }
    case TokenKind::InterpStringBegin: {
        // §4 interpolated string: lexer emitted
        //   InterpStringBegin (InterpStringPart | LParen <expr> RParen)* InterpStringEnd
        // Walk the sequence, alternating between literal fragments and
        // splice expressions, until we see InterpStringEnd.
        advance();  // consume InterpStringBegin
        auto node = std::make_unique<ast::InterpStringExpr>();
        while (!check(TokenKind::InterpStringEnd) && !at_end()) {
            if (check(TokenKind::InterpStringPart)) {
                auto frag = advance();
                ast::InterpStringExpr::Segment seg;
                seg.literal = std::string{frag.lexeme};
                node->segments.push_back(std::move(seg));
            } else if (check(TokenKind::LParen)) {
                advance();  // '('
                ast::InterpStringExpr::Segment seg;
                seg.expr = parse_expr();
                expect(TokenKind::RParen, "')' closing string interpolation splice");
                node->segments.push_back(std::move(seg));
            } else {
                emit_error(peek().range,
                           std::format("unexpected {} inside interpolated string literal",
                                       lex::spelling(peek().kind)));
                advance();
            }
        }
        if (check(TokenKind::InterpStringEnd)) {
            advance();
        }
        node->range = merge(start, last_range());
        return node;
    }
    case TokenKind::ByteStringLit: {
        auto s = std::make_unique<ast::ByteStringLit>();
        auto t = advance();
        if (t.lexeme.size() >= 3) {
            s->text = std::string{t.lexeme.substr(2, t.lexeme.size() - 3)};
        }
        s->range = merge(start, last_range());
        return s;
    }
    case TokenKind::CharLit: {
        auto c = std::make_unique<ast::CharLit>();
        auto t = advance();
        if (t.lexeme.size() >= 2) {
            c->text = std::string{t.lexeme.substr(1, t.lexeme.size() - 2)};
        }
        c->range = merge(start, last_range());
        return c;
    }
    case TokenKind::KwTrue:
    case TokenKind::KwFalse: {
        auto b = std::make_unique<ast::BoolLit>();
        b->value = (advance().kind == TokenKind::KwTrue);
        b->range = merge(start, last_range());
        return b;
    }
    case TokenKind::KwNil: {
        advance();
        auto n = std::make_unique<ast::NilLit>();
        n->range = merge(start, last_range());
        return n;
    }
    case TokenKind::KwSelfLower: {
        advance();
        auto s = std::make_unique<ast::SelfExpr>();
        s->range = merge(start, last_range());
        return s;
    }
    case TokenKind::Identifier: {
        auto i = std::make_unique<ast::IdentExpr>();
        i->name = std::string{advance().lexeme};
        i->range = merge(start, last_range());
        return i;
    }
    case TokenKind::Dot: {
        // Leading-dot inference: `.foo`
        advance();
        auto l = std::make_unique<ast::LeadingDotExpr>();
        if (!check(TokenKind::Identifier)) {
            emit_error(peek().range, "expected identifier after leading '.'");
        } else {
            l->name = std::string{advance().lexeme};
        }
        l->range = merge(start, last_range());
        return l;
    }
    case TokenKind::LParen: {
        advance();
        auto first = parse_expr();
        // §6 `(e1, e2, …)` is a tuple literal; a single `(e)` is a
        // ParenExpr (grouping). Switch to tuple mode on first comma.
        if (match(TokenKind::Comma)) {
            auto tup = std::make_unique<ast::TupleLitExpr>();
            tup->elements.push_back(std::move(first));
            tup->elements.push_back(parse_expr());
            while (match(TokenKind::Comma)) {
                tup->elements.push_back(parse_expr());
            }
            expect(TokenKind::RParen, "')' to close tuple literal");
            tup->range = merge(start, last_range());
            return tup;
        }
        expect(TokenKind::RParen, "')' to close parenthesized expression");
        auto p = std::make_unique<ast::ParenExpr>();
        p->inner = std::move(first);
        p->range = merge(start, last_range());
        return p;
    }
    case TokenKind::LBracket: {
        advance();
        auto v = std::make_unique<ast::VectorLitExpr>();
        if (!check(TokenKind::RBracket)) {
            v->elements.push_back(parse_expr());
            while (match(TokenKind::Comma)) {
                v->elements.push_back(parse_expr());
            }
        }
        expect(TokenKind::RBracket, "']' to close vector literal");
        v->range = merge(start, last_range());
        return v;
    }
    case TokenKind::LBrace: {
        // A bare `{ ... }` in expression position is either a closure or a
        // block. We distinguish: a closure begins with `params =>` — peek for
        // a `=>` before any newline / statement starts.
        auto save = pos_;
        advance();  // '{'
        // Try to find `=>` before hitting end of file at this depth.
        int depth = 1;
        bool is_closure = false;
        while (!at_end()) {
            auto kk = peek().kind;
            if (kk == TokenKind::LBrace || kk == TokenKind::LParen || kk == TokenKind::LBracket) {
                ++depth;
            }
            if (kk == TokenKind::RBrace || kk == TokenKind::RParen || kk == TokenKind::RBracket) {
                --depth;
                if (depth == 0) {
                    break;
                }
            }
            if (depth == 1 && kk == TokenKind::FatArrow) {
                is_closure = true;
                break;
            }
            if (depth == 1 && kk == TokenKind::Newline) {
                break;
            }
            advance();
        }
        pos_ = save;

        if (is_closure) {
            advance();  // '{'
            auto c = std::make_unique<ast::ClosureExpr>();
            // params, optional. Either `IDENT (, IDENT)*` then `=>`, or just `=>`.
            if (!check(TokenKind::FatArrow)) {
                if (check(TokenKind::Identifier)) {
                    c->params.push_back(std::string{advance().lexeme});
                    while (match(TokenKind::Comma)) {
                        if (!check(TokenKind::Identifier)) {
                            emit_error(peek().range, "expected closure parameter name");
                            break;
                        }
                        c->params.push_back(std::string{advance().lexeme});
                    }
                }
            }
            expect(TokenKind::FatArrow, "'=>' in closure");
            skip_newlines();
            while (!check(TokenKind::RBrace) && !at_end()) {
                auto s = parse_statement();
                if (s) {
                    c->body.push_back(std::move(s));
                }
                skip_newlines();
            }
            expect(TokenKind::RBrace, "'}' closing closure");
            c->range = merge(start, last_range());
            return c;
        }
        // Otherwise it's a block expression.
        return parse_block_expr();
    }
    case TokenKind::KwIf: {
        advance();
        auto i = std::make_unique<ast::IfExpr>();
        // §9 `if let NAME = INIT { ... }` — when the next token is `let`,
        // parse a single-name binding and the optional initializer. Anything
        // more elaborate (pattern destructuring, `var let`, multiple
        // bindings) is left for a later phase.
        if (match(TokenKind::KwLet)) {
            if (!check(TokenKind::Identifier)) {
                emit_error(peek().range, "expected identifier after 'if let'");
            } else {
                i->let_name = std::string{advance().lexeme};
            }
            expect(TokenKind::Assign, "'=' after 'if let NAME'");
            i->let_init = parse_expr();
        } else {
            i->cond = parse_expr();
        }
        i->then_branch = parse_block_expr();
        if (match(TokenKind::KwElse)) {
            if (check(TokenKind::KwIf)) {
                i->else_branch = parse_primary();
            } else {
                i->else_branch = parse_block_expr();
            }
        }
        i->range = merge(start, last_range());
        return i;
    }
    case TokenKind::KwMatch: {
        advance();
        auto m = std::make_unique<ast::MatchExpr>();
        m->scrutinee = parse_expr();
        expect(TokenKind::LBrace, "'{' to open match body");
        skip_newlines();
        while (!check(TokenKind::RBrace) && !at_end()) {
            ast::MatchArm arm;
            if (match(TokenKind::KwDefault)) {
                arm.is_default = true;
                expect(TokenKind::Colon, "':' after 'default'");
                arm.body = parse_expr();
            } else {
                expect(TokenKind::KwCase, "'case' or 'default' in match arm");
                arm.pattern = parse_pattern();
                if (match(TokenKind::KwWhere)) {
                    arm.guard = parse_expr();
                }
                expect(TokenKind::Colon, "':' after case pattern");
                arm.body = parse_expr();
            }
            m->arms.push_back(std::move(arm));
            skip_newlines();
        }
        expect(TokenKind::RBrace, "'}' closing match body");
        m->range = merge(start, last_range());
        return m;
    }
    case TokenKind::KwSelect: {
        // §11 `select { on (let pat =)? event : body  …
        //                 timeout <ms>: body  default: body }`.
        // An event is a `Future[T]` (the binding form `on let msg = fut`
        // unwraps the future's value) or a blocking channel `ch.receive()`.
        // A `timeout <ms>` arm fires after a wall-clock delay; `default`
        // makes the select a non-blocking poll.
        advance();
        auto sel = std::make_unique<ast::SelectExpr>();
        expect(TokenKind::LBrace, "'{' to open select body");
        skip_newlines();
        while (!check(TokenKind::RBrace) && !at_end()) {
            if (match(TokenKind::KwDefault)) {
                expect(TokenKind::Colon, "':' after 'default'");
                sel->default_body = parse_expr();
                skip_newlines();
                continue;
            }
            // §11 `timeout <ms>: body` — a contextual keyword (not reserved
            // elsewhere) at arm-start position, where only `on`/`default`/
            // `timeout` are legal, so a bare `timeout` is unambiguous here.
            if (check(TokenKind::Identifier) && peek().lexeme == "timeout") {
                advance();
                if (sel->timeout_body != nullptr) {
                    emit_error(peek().range, "a select may have at most one 'timeout' arm");
                }
                sel->timeout_delay = parse_expr();
                expect(TokenKind::Colon, "':' after select timeout delay");
                sel->timeout_body = parse_expr();
                skip_newlines();
                continue;
            }
            if (!match(TokenKind::KwOn)) {
                emit_error(peek().range, "expected 'on', 'timeout', or 'default' in select arm");
                break;
            }
            ast::SelectArm arm;
            if (match(TokenKind::KwLet)) {
                arm.pattern = parse_pattern();
                expect(TokenKind::Assign, "'=' after 'let' pattern in select arm");
            }
            arm.event = parse_expr();
            expect(TokenKind::Colon, "':' after select event");
            arm.body = parse_expr();
            sel->arms.push_back(std::move(arm));
            skip_newlines();
        }
        expect(TokenKind::RBrace, "'}' closing select body");
        sel->range = merge(start, last_range());
        return sel;
    }
    case TokenKind::KwDo: {
        // §9 `do { body } catch NAME { handler }` — inline error
        // handling. Two binding shapes:
        //   * `catch (NAME: E) { ... }` — explicit annotation; sema
        //     uses E directly as the do-body's throws context.
        //   * `catch NAME { ... }` — bare binding; sema infers E
        //     from the body's TryExpr error types.
        advance();
        auto dc = std::make_unique<ast::DoCatchExpr>();
        dc->do_body = parse_block_expr();
        expect(TokenKind::KwCatch, "'catch' after 'do { ... }'");
        if (match(TokenKind::LParen)) {
            if (!check(TokenKind::Identifier)) {
                emit_error(peek().range, "expected error binding name after 'catch ('");
            } else {
                dc->error_name = std::string{advance().lexeme};
            }
            expect(TokenKind::Colon, "':' after catch binding name");
            dc->error_type = parse_type();
            expect(TokenKind::RParen, "')' after catch binding annotation");
        } else if (check(TokenKind::Identifier)) {
            // Bare form: sema's inference pass fills in the type.
            dc->error_name = std::string{advance().lexeme};
        } else {
            emit_error(peek().range, "expected '(NAME: E)' or bare 'NAME' after 'catch'");
        }
        // §9 optional `where guard` clause between the catch binding
        // and the body's `{`. The guard sees the bound error value
        // and gates whether this catch arm applies at runtime.
        if (match(TokenKind::KwWhere)) {
            dc->guard = parse_expr();
        }
        dc->catch_body = parse_block_expr();
        dc->range = merge(start, last_range());
        return dc;
    }
    case TokenKind::KwComptime: {
        advance();
        auto c = std::make_unique<ast::ComptimeExpr>();
        if (check(TokenKind::LBrace)) {
            c->inner = parse_block_expr();
        } else {
            c->inner = parse_expr();
        }
        c->range = merge(start, last_range());
        return c;
    }
    case TokenKind::KwQuote: {
        // §12.4 quote. An expression-context quote `quote { EXPR }` carries a
        // single expression; a declaration-context quote `quote { <decl> … }`
        // (a declaration-macro template) carries a list of declarations and may
        // splice the annotated decl as a `$d` item. Disambiguated by scanning
        // the braces for a declaration keyword at brace-depth 1.
        advance();
        // Skip past a `$ident` / `$(expr)` splice that begins at token `i`
        // (which is the Dollar), returning the index just after it. Used to
        // tell a standalone `$d` declaration item from a `$x` inside an
        // expression.
        const auto after_splice = [&](std::size_t i) -> std::size_t {
            std::size_t j = i + 1;
            if (peek(j).kind == TokenKind::LParen) {
                int pd = 0;
                for (; peek(j).kind != TokenKind::Eof; ++j) {
                    if (peek(j).kind == TokenKind::LParen) {
                        ++pd;
                    } else if (peek(j).kind == TokenKind::RParen && --pd == 0) {
                        ++j;
                        break;
                    }
                }
            } else if (peek(j).kind == TokenKind::Identifier) {
                ++j;
            }
            return j;
        };
        const auto is_decl_kw = [](TokenKind k) {
            return k == TokenKind::KwFunc || k == TokenKind::KwStruct || k == TokenKind::KwEnum
                   || k == TokenKind::KwProtocol || k == TokenKind::KwExtension
                   || k == TokenKind::KwConst || k == TokenKind::KwStatic;
        };
        bool is_decl_quote = false;
        {
            int depth = 0;
            // Last non-newline token at the current depth, so we can tell a
            // `$d` that *starts* an item (preceded by the quote's `{` or the
            // `}` of the previous decl) from a `$x` used as an operand inside
            // an expression (preceded by an operator/operand).
            TokenKind prev = TokenKind::Eof;
            for (std::size_t i = 0;; ++i) {
                const auto& tk = peek(i);
                if (tk.kind == TokenKind::Eof) {
                    break;
                }
                if (tk.kind == TokenKind::LBrace) {
                    ++depth;
                } else if (tk.kind == TokenKind::RBrace) {
                    if (--depth == 0) {
                        break;
                    }
                } else if (depth == 1 && is_decl_kw(tk.kind)) {
                    is_decl_quote = true;
                    break;
                } else if (depth == 1 && tk.kind == TokenKind::Dollar
                           && (prev == TokenKind::LBrace || prev == TokenKind::RBrace)) {
                    // A `$d` standing alone as a declaration item: it begins at
                    // an item boundary AND, after the splice (and any
                    // newlines), is followed by the closing `}`, a declaration
                    // keyword, or another `$` item. This is what makes
                    // `quote { $d }` a [Decl] — while `quote { $x + $x }` stays
                    // an expression (its trailing `$x` is preceded by `+`, not
                    // a boundary, so it never triggers here).
                    std::size_t j = after_splice(i);
                    while (peek(j).kind == TokenKind::Newline) {
                        ++j;
                    }
                    const auto nk = peek(j).kind;
                    if (nk == TokenKind::RBrace || nk == TokenKind::Dollar || is_decl_kw(nk)) {
                        is_decl_quote = true;
                        break;
                    }
                }
                if (tk.kind != TokenKind::Newline) {
                    prev = tk.kind;
                }
            }
        }
        expect(TokenKind::LBrace, "'{' to open quote body");
        skip_newlines();
        if (is_decl_quote) {
            // §12.4 declaration-macro template: a sequence of declarations,
            // each either a real decl or a `$d` splice item.
            auto q = std::make_unique<ast::QuoteDeclExpr>();
            while (!check(TokenKind::RBrace) && !at_end()) {
                const std::size_t before = pos_;
                if (check(TokenKind::Dollar)) {
                    auto sd = std::make_unique<ast::SpliceDecl>();
                    sd->splice = parse_prefix();  // the `$d` SpliceExpr
                    sd->range = sd->splice ? sd->splice->range : peek().range;
                    q->decls.push_back(std::move(sd));
                } else {
                    q->decls.push_back(parse_decl(parse_attributes()));
                }
                skip_newlines();
                // Guard against a non-advancing iteration (a token neither the
                // splice nor parse_decl can consume): emit one error and bail
                // rather than spin forever building error nodes.
                if (pos_ == before) {
                    emit_error(peek().range, "unexpected token in declaration quote body");
                    break;
                }
            }
            expect(TokenKind::RBrace, "'}' to close quote body");
            q->range = merge(start, last_range());
            return q;
        }
        auto q = std::make_unique<ast::QuoteExpr>();
        q->inner = parse_expr();
        skip_newlines();
        expect(TokenKind::RBrace, "'}' to close quote body");
        q->range = merge(start, last_range());
        return q;
    }
    case TokenKind::Dollar:
        // §12.4 splice: `$ident` or `$(expr)`. Only meaningful inside a
        // quote (sema enforces); the inner is substituted at the splice site.
        return parse_splice();
    case TokenKind::At: {
        // `@embed("path")` and other intrinsic-call forms in expression
        // position. Today only @embed is recognized — extending this to
        // @bits / @align etc. is a switch on `intr_name` below.
        advance();  // '@'
        // Accept any identifier OR a reserved word as the intrinsic name —
        // `embed` is the struct-embedding keyword (§6) but reuses the same
        // letters here, and future intrinsics may collide similarly.
        // Validation happens against the known-intrinsic list below.
        if (!check(TokenKind::Identifier) && !check(TokenKind::KwEmbed)) {
            emit_error(peek().range, "expected intrinsic name after '@'");
            auto stub = std::make_unique<ast::IdentExpr>();
            stub->name = "<error>";
            stub->range = merge(start, last_range());
            return stub;
        }
        auto intr_name = std::string{advance().lexeme};
        if (intr_name == "embed") {
            expect(TokenKind::LParen, "'(' after @embed");
            auto e = std::make_unique<ast::EmbedExpr>();
            if (check(TokenKind::StringLit)) {
                auto t = advance();
                if (t.lexeme.size() >= 2) {
                    e->path = std::string{t.lexeme.substr(1, t.lexeme.size() - 2)};
                }
            } else {
                emit_error(peek().range, "@embed requires a string literal path");
            }
            expect(TokenKind::RParen, "')' after @embed path");
            e->range = merge(start, last_range());
            return e;
        }
        // §12.4 anything else is an expression macro invocation `@name(args)` —
        // `name` is a `comptime func(Expr, …) -> Expr` resolved + expanded by
        // sema. (Intrinsics like @embed are matched above first.)
        auto m = std::make_unique<ast::MacroCallExpr>();
        m->name = std::move(intr_name);
        expect(TokenKind::LParen, "'(' after macro name");
        if (!check(TokenKind::RParen)) {
            do {
                m->args.push_back(parse_expr());
            } while (match(TokenKind::Comma));
        }
        expect(TokenKind::RParen, "')' closing macro arguments");
        m->range = merge(start, last_range());
        return m;
    }
    default:
        emit_error(peek().range,
                   std::format("expected expression, got {}", lex::spelling(peek().kind)));
        // Move past one token to make progress.
        if (!at_end()) {
            advance();
        }
        auto stub = std::make_unique<ast::IdentExpr>();
        stub->name = "<error>";
        stub->range = start;
        return stub;
    }
}

ast::ExprPtr Parser::parse_splice() {
    auto start = peek().range;
    advance();  // '$'
    auto sp = std::make_unique<ast::SpliceExpr>();
    if (match(TokenKind::LParen)) {
        sp->inner = parse_expr();
        expect(TokenKind::RParen, "')' to close '$( … )' splice");
    } else if (check(TokenKind::Identifier)) {
        auto id = std::make_unique<ast::IdentExpr>();
        auto idtok = advance();
        id->name = std::string{idtok.lexeme};
        id->range = idtok.range;
        sp->inner = std::move(id);
    } else {
        emit_error(peek().range, "expected an identifier or '(' after '$'");
    }
    sp->range = merge(start, last_range());
    return sp;
}

ast::ExprPtr Parser::parse_postfix(ast::ExprPtr lhs) {
    while (true) {
        if (check(TokenKind::LParen)) {
            advance();
            auto c = std::make_unique<ast::CallExpr>();
            c->range = lhs->range;
            c->callee = std::move(lhs);
            if (!check(TokenKind::RParen)) {
                do {
                    ast::CallExpr::Arg arg;
                    if (check(TokenKind::Identifier) && peek(1).kind == TokenKind::Colon) {
                        arg.label = std::string{advance().lexeme};
                        advance();  // ':'
                    }
                    if (match(TokenKind::Amp)) {
                        arg.is_inout = true;
                    }
                    arg.value = parse_expr();
                    c->args.push_back(std::move(arg));
                } while (match(TokenKind::Comma));
            }
            expect(TokenKind::RParen, "')' to close call");
            c->range = merge(c->range, last_range());
            lhs = std::move(c);
        } else if (check(TokenKind::Dot)
                   || (check(TokenKind::Question) && peek(1).kind == TokenKind::Dot)) {
            // §9 `?.` is optional chaining; otherwise plain `.` member
            // access. The lexer hands us `?` then `.` as separate
            // tokens, so we peek one ahead to disambiguate from the
            // postfix `?` of a type position (which can't appear here).
            const bool is_chain = check(TokenKind::Question);
            if (is_chain) {
                advance();  // consume `?`
            }
            advance();  // consume `.`
            auto m = std::make_unique<ast::MemberExpr>();
            m->range = lhs->range;
            m->base = std::move(lhs);
            m->is_optional_chain = is_chain;
            // Member names are usually identifiers, but Vestra reserves
            // a few words (`type`, `embed`, `default`, `read`, …) that
            // legitimately serve as field or static-method names —
            // §12.2 reflection exposes `Field.type`, §12.3
            // derive(Default) exposes `T.default()`, §A6 MMIO uses
            // `view.read()`. Accept those keywords here using their
            // spelled lexeme so we don't force users to escape them.
            if (check(TokenKind::Dollar)) {
                // §12.4 a member-name splice inside a decl quote:
                // `v.$(f.name)`. Resolved to a String when the macro expands.
                m->member_splice = parse_splice();
            } else if (check(TokenKind::Identifier) || check(TokenKind::KwType)
                       || check(TokenKind::KwEmbed) || check(TokenKind::KwDefault)
                       || check(TokenKind::KwRead)) {
                m->member = std::string{advance().lexeme};
            } else {
                emit_error(peek().range, "expected member name after '.'");
            }
            m->range = merge(m->range, last_range());
            lhs = std::move(m);
        } else if (check(TokenKind::LBracket)) {
            advance();
            auto ix = std::make_unique<ast::IndexExpr>();
            ix->range = lhs->range;
            ix->base = std::move(lhs);
            if (!check(TokenKind::RBracket)) {
                ix->indices.push_back(parse_expr());
                while (match(TokenKind::Comma)) {
                    ix->indices.push_back(parse_expr());
                }
            }
            expect(TokenKind::RBracket, "']' to close index");
            ix->range = merge(ix->range, last_range());
            lhs = std::move(ix);
        } else if (check(TokenKind::Bang)) {
            // §9 force-unwrap is postfix `!` on an Optional<T> value; it
            // returns the wrapped T and panics if the operand is `.none`.
            advance();
            auto u = std::make_unique<ast::UnaryExpr>();
            u->op = ast::UnaryOp::Unwrap;
            u->range = lhs->range;
            u->operand = std::move(lhs);
            u->range = merge(u->range, last_range());
            lhs = std::move(u);
        } else {
            break;
        }
    }
    return lhs;
}

}  // namespace vestra::parse
