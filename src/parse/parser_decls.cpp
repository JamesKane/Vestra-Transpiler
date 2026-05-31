// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// Declaration parsing: the parse_decl dispatch and every declaration
// production (func / enum / protocol / extension / opaque / const /
// static / derive) plus the where-clause refinement.
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

std::unique_ptr<ast::StructDecl>
Parser::parse_struct(std::vector<ast::Attribute> attrs, ast::Visibility vis, bool is_linear) {
    auto start = peek().range;
    advance();  // 'struct'
    auto s = std::make_unique<ast::StructDecl>();
    s->attributes = std::move(attrs);
    s->visibility = vis;
    s->is_linear = is_linear;
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
    if (check(TokenKind::KwLinear)) {
        // §5 `linear struct Foo { … }` — the value must be consumed before
        // its scope ends. v0.5 admits the modifier on structs.
        advance();  // 'linear'
        if (!check(TokenKind::KwStruct)) {
            emit_error(peek().range, "expected 'struct' after 'linear'");
            return nullptr;
        }
        return parse_struct(std::move(attrs), vis, /*is_linear=*/true);
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
    // §7 generics: an optional `where T: P, U: P & Q` clause refines the
    // bounds of the already-parsed generic parameters (the inline
    // `[T: P]` form is the other way to write the same thing).
    parse_where_opt(f->generics);
    if (check(TokenKind::LBrace)) {
        f->body = parse_block_expr();
    }
    f->range = merge(start, last_range());
    return f;
}

void Parser::parse_where_opt(std::vector<ast::GenericParam>& generics) {
    if (!match(TokenKind::KwWhere)) {
        return;
    }
    do {
        if (!check(TokenKind::Identifier)) {
            emit_error(peek().range, "expected a generic parameter name in 'where' clause");
            break;
        }
        auto name_tok = advance();
        std::string name{name_tok.lexeme};
        expect(TokenKind::Colon, "':' after the parameter name in a 'where' clause");
        // Collect the conjunctive bound `P & Q & …`.
        std::vector<ast::TypePtr> protocols;
        protocols.push_back(parse_type());
        while (match(TokenKind::Amp)) {
            protocols.push_back(parse_type());
        }
        // Merge onto the matching generic parameter; an unknown name is a
        // diagnostic (the clause must refer to a declared parameter).
        ast::GenericParam* target = nullptr;
        for (auto& g : generics) {
            if (g.name == name) {
                target = &g;
                break;
            }
        }
        if (target == nullptr) {
            emit_error(
                name_tok.range,
                std::format("'where' clause refers to unknown generic parameter '{}'", name));
        } else if (target->is_const) {
            emit_error(
                name_tok.range,
                std::format("const generic parameter '{}' cannot have a protocol bound", name));
        } else {
            for (auto& p : protocols) {
                target->bound.protocols.push_back(std::move(p));
            }
        }
    } while (match(TokenKind::Comma));
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

}  // namespace vestra::parse
