// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "vestra/parse/parser.hpp"

#include "vestra/lex/token.hpp"

#include <cassert>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace vestra::parse {

namespace {

using lex::TokenKind;

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

diag::SourceRange merge(diag::SourceRange a, diag::SourceRange b) {
    if (!a.is_valid()) {
        return b;
    }
    if (!b.is_valid()) {
        return a;
    }
    auto end = b.begin.offset + b.length;
    return {a.begin, end - a.begin.offset};
}

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

// ============================================================================
// declarations
// ============================================================================

ast::DeclPtr Parser::parse_decl(std::vector<ast::Attribute> attrs) {
    auto vis = parse_visibility();

    // §12.4: `comptime func` runs at fold time. The modifier appears
    // before `async`/recv-mode/`func`. We only consume it when it's
    // unambiguously paired with one of those — otherwise `comptime` is
    // an expression keyword (a comptime { ... } block) that some later
    // production will handle.
    bool is_comptime = false;
    if (check(TokenKind::KwComptime)
        && (peek(1).kind == TokenKind::KwFunc || peek(1).kind == TokenKind::KwAsync
            || peek(1).kind == TokenKind::KwInout || peek(1).kind == TokenKind::KwSink)) {
        advance();
        is_comptime = true;
    }

    // `async`/recv-mode before func.
    bool is_async = match(TokenKind::KwAsync);
    ast::FuncDecl::RecvMode recv = ast::FuncDecl::RecvMode::None;
    if (check(TokenKind::KwInout) && peek(1).kind == TokenKind::KwFunc) {
        advance();
        recv = ast::FuncDecl::RecvMode::Inout;
    } else if (check(TokenKind::KwSink) && peek(1).kind == TokenKind::KwFunc) {
        advance();
        recv = ast::FuncDecl::RecvMode::Sink;
    }

    if (check(TokenKind::KwFunc)) {
        return parse_func(std::move(attrs), vis, is_async, recv, is_comptime);
    }
    if (check(TokenKind::KwStruct)) {
        return parse_struct(std::move(attrs), vis);
    }
    if (check(TokenKind::KwEnum)) {
        return parse_enum(std::move(attrs), vis);
    }
    if (check(TokenKind::KwProtocol)) {
        return parse_protocol(vis);
    }
    if (check(TokenKind::KwExtension)) {
        return parse_extension(std::move(attrs));
    }
    if (check(TokenKind::KwOpaque)) {
        return parse_opaque(vis);
    }
    if (check(TokenKind::KwConst)) {
        return parse_const(vis);
    }
    if (check(TokenKind::KwStatic)) {
        return parse_static(std::move(attrs), vis);
    }
    if (check(TokenKind::KwDerive)) {
        return parse_derive();
    }

    emit_error(peek().range,
               std::format("expected declaration, got {}", lex::spelling(peek().kind)));
    return nullptr;
}

std::unique_ptr<ast::FuncDecl> Parser::parse_func(std::vector<ast::Attribute> attrs,
                                                  ast::Visibility vis,
                                                  bool is_async,
                                                  ast::FuncDecl::RecvMode recv,
                                                  bool is_comptime) {
    auto start = peek().range;
    advance();  // 'func'
    auto f = std::make_unique<ast::FuncDecl>();
    f->attributes = std::move(attrs);
    f->visibility = vis;
    f->is_async = is_async;
    f->is_comptime = is_comptime;
    f->recv_mode = recv;

    if (!check(TokenKind::Identifier)) {
        emit_error(peek().range, "expected function name after 'func'");
    } else {
        f->name = std::string{advance().lexeme};
    }
    f->generics = parse_generics_opt();
    f->params = parse_params();
    f->effects = parse_effects_opt();
    if (match(TokenKind::Arrow)) {
        f->result = parse_type();
    }
    if (check(TokenKind::LBrace)) {
        f->body = parse_block_expr();
    }
    f->range = merge(start, last_range());
    return f;
}

std::unique_ptr<ast::StructDecl> Parser::parse_struct(std::vector<ast::Attribute> attrs,
                                                      ast::Visibility vis) {
    auto start = peek().range;
    advance();  // 'struct'
    auto s = std::make_unique<ast::StructDecl>();
    s->attributes = std::move(attrs);
    s->visibility = vis;
    if (!check(TokenKind::Identifier)) {
        emit_error(peek().range, "expected struct name");
    } else {
        s->name = std::string{advance().lexeme};
    }
    s->generics = parse_generics_opt();
    expect(TokenKind::LBrace, "'{' to open struct body");
    skip_newlines();
    while (!check(TokenKind::RBrace) && !at_end()) {
        auto field_attrs = parse_attributes();
        if (check(TokenKind::KwFunc) || check(TokenKind::KwAsync) || check(TokenKind::KwInout)
            || check(TokenKind::KwSink)) {
            bool method_async = match(TokenKind::KwAsync);
            ast::FuncDecl::RecvMode method_recv = ast::FuncDecl::RecvMode::None;
            if (match(TokenKind::KwInout)) {
                method_recv = ast::FuncDecl::RecvMode::Inout;
            }
            if (match(TokenKind::KwSink)) {
                method_recv = ast::FuncDecl::RecvMode::Sink;
            }
            auto m = parse_func(
                std::move(field_attrs), ast::Visibility::Internal, method_async, method_recv);
            s->methods.push_back(std::move(m));
        } else if (match(TokenKind::KwDeinit)) {
            s->deinit_body = parse_block_expr();
        } else {
            ast::StructDecl::Field f;
            f.attributes = std::move(field_attrs);
            if (match(TokenKind::KwLet)) {
                f.kind = ast::StructDecl::Field::Kind::Let;
            } else if (match(TokenKind::KwVar)) {
                f.kind = ast::StructDecl::Field::Kind::Var;
            } else if (match(TokenKind::KwEmbed)) {
                f.kind = ast::StructDecl::Field::Kind::Embed;
            } else {
                emit_error(peek().range, "expected 'let', 'var', 'embed', or 'func' inside struct");
                sync_to_decl();
                continue;
            }
            auto fstart = last_range();
            if (!check(TokenKind::Identifier)) {
                emit_error(peek().range, "expected field name");
            } else {
                f.name = std::string{advance().lexeme};
            }
            expect(TokenKind::Colon, "':' before field type");
            f.type = parse_type();
            f.range = merge(fstart, last_range());
            s->fields.push_back(std::move(f));
        }
        skip_newlines();
    }
    expect(TokenKind::RBrace, "'}' to close struct body");
    s->range = merge(start, last_range());
    return s;
}

std::unique_ptr<ast::EnumDecl> Parser::parse_enum(std::vector<ast::Attribute> attrs,
                                                  ast::Visibility vis) {
    auto start = peek().range;
    advance();  // 'enum'
    auto e = std::make_unique<ast::EnumDecl>();
    e->attributes = std::move(attrs);
    e->visibility = vis;
    if (!check(TokenKind::Identifier)) {
        emit_error(peek().range, "expected enum name");
    } else {
        e->name = std::string{advance().lexeme};
    }
    e->generics = parse_generics_opt();
    expect(TokenKind::LBrace, "'{' to open enum body");
    skip_newlines();
    while (!check(TokenKind::RBrace) && !at_end()) {
        if (match(TokenKind::KwCase)) {
            ast::EnumDecl::Case c;
            auto cstart = last_range();
            if (!check(TokenKind::Identifier)) {
                emit_error(peek().range, "expected enum case name");
            } else {
                c.name = std::string{advance().lexeme};
            }
            if (match(TokenKind::LParen)) {
                if (!check(TokenKind::RParen)) {
                    do {
                        std::string label;
                        if (check(TokenKind::Identifier) && peek(1).kind == TokenKind::Colon) {
                            label = std::string{advance().lexeme};
                            advance();  // ':'
                        }
                        c.payload.emplace_back(std::move(label), parse_type());
                    } while (match(TokenKind::Comma));
                }
                expect(TokenKind::RParen, "closing ')' of enum case payload");
            }
            c.range = merge(cstart, last_range());
            e->cases.push_back(std::move(c));
        } else {
            // Methods / fields on the enum (uncommon but allowed by §17.2).
            auto member_attrs = parse_attributes();
            if (check(TokenKind::KwFunc)) {
                auto m = parse_func(std::move(member_attrs),
                                    ast::Visibility::Internal,
                                    /*is_async=*/false,
                                    ast::FuncDecl::RecvMode::None);
                e->methods.push_back(std::move(m));
            } else {
                emit_error(peek().range, "expected 'case' or member in enum body");
                sync_to_decl();
            }
        }
        skip_newlines();
    }
    expect(TokenKind::RBrace, "'}' to close enum body");
    e->range = merge(start, last_range());
    return e;
}

std::unique_ptr<ast::ProtocolDecl> Parser::parse_protocol(ast::Visibility vis) {
    auto start = peek().range;
    advance();  // 'protocol'
    auto p = std::make_unique<ast::ProtocolDecl>();
    p->visibility = vis;
    if (!check(TokenKind::Identifier)) {
        emit_error(peek().range, "expected protocol name");
    } else {
        p->name = std::string{advance().lexeme};
    }
    p->generics = parse_generics_opt();
    if (match(TokenKind::Colon)) {
        p->refines.push_back(parse_type());
        while (match(TokenKind::Comma)) {
            p->refines.push_back(parse_type());
        }
    }
    expect(TokenKind::LBrace, "'{' to open protocol body");
    skip_newlines();
    while (!check(TokenKind::RBrace) && !at_end()) {
        if (match(TokenKind::KwType)) {
            ast::ProtocolDecl::AssocType a;
            if (!check(TokenKind::Identifier)) {
                emit_error(peek().range, "expected associated type name");
            } else {
                a.name = std::string{advance().lexeme};
            }
            if (match(TokenKind::Colon)) {
                a.bound.protocols.push_back(parse_type());
                while (match(TokenKind::Amp)) {
                    a.bound.protocols.push_back(parse_type());
                }
            }
            p->assoc_types.push_back(std::move(a));
        } else {
            bool is_async = match(TokenKind::KwAsync);
            ast::FuncDecl::RecvMode recv = ast::FuncDecl::RecvMode::None;
            if (match(TokenKind::KwInout)) {
                recv = ast::FuncDecl::RecvMode::Inout;
            }
            if (match(TokenKind::KwSink)) {
                recv = ast::FuncDecl::RecvMode::Sink;
            }
            if (!check(TokenKind::KwFunc) && !check(TokenKind::KwComptime)) {
                emit_error(peek().range, "expected protocol requirement");
                sync_to_decl();
                skip_newlines();
                continue;
            }
            (void)match(TokenKind::KwComptime);  // accepted but unused at this stub
            auto m = parse_func({}, ast::Visibility::Internal, is_async, recv);
            p->requirements.push_back(std::move(m));
        }
        skip_newlines();
    }
    expect(TokenKind::RBrace, "'}' to close protocol body");
    p->range = merge(start, last_range());
    return p;
}

std::unique_ptr<ast::ExtensionDecl> Parser::parse_extension(std::vector<ast::Attribute> attrs) {
    auto start = peek().range;
    advance();  // 'extension'
    auto e = std::make_unique<ast::ExtensionDecl>();
    e->attributes = std::move(attrs);
    e->target = parse_type();
    if (match(TokenKind::Colon)) {
        e->conformances.push_back(parse_type());
        while (match(TokenKind::Comma)) {
            e->conformances.push_back(parse_type());
        }
    }
    expect(TokenKind::LBrace, "'{' to open extension body");
    skip_newlines();
    while (!check(TokenKind::RBrace) && !at_end()) {
        auto member_attrs = parse_attributes();
        auto decl = parse_decl(std::move(member_attrs));
        if (decl) {
            e->members.push_back(std::move(decl));
        } else {
            sync_to_decl();
        }
        skip_newlines();
    }
    expect(TokenKind::RBrace, "'}' to close extension body");
    e->range = merge(start, last_range());
    return e;
}

std::unique_ptr<ast::OpaqueDecl> Parser::parse_opaque(ast::Visibility vis) {
    auto start = peek().range;
    advance();  // 'opaque'
    expect(TokenKind::KwType, "'type' after 'opaque'");
    auto o = std::make_unique<ast::OpaqueDecl>();
    o->visibility = vis;
    if (!check(TokenKind::Identifier)) {
        emit_error(peek().range, "expected opaque type name");
    } else {
        o->name = std::string{advance().lexeme};
    }
    expect(TokenKind::Assign, "'=' in opaque type declaration");
    o->underlying = parse_type();
    o->range = merge(start, last_range());
    return o;
}

std::unique_ptr<ast::ConstDecl> Parser::parse_const(ast::Visibility vis) {
    auto start = peek().range;
    advance();  // 'const'
    auto c = std::make_unique<ast::ConstDecl>();
    c->visibility = vis;
    if (!check(TokenKind::Identifier)) {
        emit_error(peek().range, "expected const name");
    } else {
        c->name = std::string{advance().lexeme};
    }
    if (match(TokenKind::Colon)) {
        c->type = parse_type();
    }
    expect(TokenKind::Assign, "'=' after const declaration");
    c->value = parse_expr();
    c->range = merge(start, last_range());
    return c;
}

std::unique_ptr<ast::StaticDecl> Parser::parse_static(std::vector<ast::Attribute> attrs,
                                                      ast::Visibility vis) {
    auto start = peek().range;
    advance();  // 'static'
    auto s = std::make_unique<ast::StaticDecl>();
    s->visibility = vis;
    // §A1 (§4.5): `@noinit static name: T` reserves uninitialized
    // storage in .bss — the parser skips the `= value` part entirely
    // for this form so it round-trips cleanly with the spec.
    for (const auto& a : attrs) {
        if (a.name == "noinit") {
            s->noinit = true;
            break;
        }
    }
    s->attributes = std::move(attrs);
    if (!check(TokenKind::Identifier)) {
        emit_error(peek().range, "expected static name");
    } else {
        s->name = std::string{advance().lexeme};
    }
    expect(TokenKind::Colon, "':' after static name");
    s->type = parse_type();
    if (!s->noinit) {
        expect(TokenKind::Assign, "'=' after static type");
        s->value = parse_expr();
    }
    s->range = merge(start, last_range());
    return s;
}

std::unique_ptr<ast::DeriveDecl> Parser::parse_derive() {
    auto start = peek().range;
    advance();  // 'derive'
    auto d = std::make_unique<ast::DeriveDecl>();
    expect(TokenKind::LParen, "'(' after 'derive'");
    if (!check(TokenKind::RParen)) {
        d->protocols.push_back(parse_type());
        while (match(TokenKind::Comma)) {
            d->protocols.push_back(parse_type());
        }
    }
    expect(TokenKind::RParen, "closing ')' in derive(...)");
    expect(TokenKind::KwFor, "'for' in derive(...) for T");
    d->target = parse_type();
    d->range = merge(start, last_range());
    return d;
}

// ============================================================================
// types
// ============================================================================

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

// ============================================================================
// patterns
// ============================================================================

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

// ============================================================================
// statements / blocks
// ============================================================================

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
            const bool name_binding =
                check(TokenKind::Identifier) && peek(1).kind == TokenKind::Assign
                && !peek().lexeme.empty()
                && (peek().lexeme.front() >= 'a' && peek().lexeme.front() <= 'z');
            if (name_binding) {
                b.name = std::string{advance().lexeme};
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

// ============================================================================
// expressions — Pratt
// ============================================================================

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
        advance();
        auto q = std::make_unique<ast::QuoteExpr>();
        q->inner = parse_block_expr();  // we treat `quote { ... }` as a block
        q->range = merge(start, last_range());
        return q;
    }
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
        emit_error(start, std::format("unknown intrinsic '@{}'", intr_name));
        // Best-effort recover: skip an optional (...) so we don't trip the
        // outer expression parser on the leftover paren.
        if (match(TokenKind::LParen)) {
            int depth = 1;
            while (depth > 0 && !at_end()) {
                if (check(TokenKind::LParen)) {
                    ++depth;
                } else if (check(TokenKind::RParen)) {
                    --depth;
                }
                advance();
            }
        }
        auto stub = std::make_unique<ast::IdentExpr>();
        stub->name = "<error>";
        stub->range = merge(start, last_range());
        return stub;
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
            if (check(TokenKind::Identifier) || check(TokenKind::KwType)
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
