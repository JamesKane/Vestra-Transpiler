// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// Expression checking: the main check_expr dispatch plus unary, binary,
// and if-expression typing. (Call typing lives in resolver_call.cpp.)
// Split out of resolver.cpp; see resolver_internal.hpp for the shared
// free helpers.

#include "vestra/ast/nodes.hpp"
#include "vestra/sema/builtins.hpp"
#include "vestra/sema/resolver.hpp"
#include "vestra/sema/scope.hpp"
#include "vestra/sema/types.hpp"

#include "resolver_internal.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace vestra::sema {

using detail::named_type_param_count;

namespace {

using SpliceSubs = std::unordered_map<std::string, const ast::Expr*>;

// §12.4 deep-clone an expression-macro template, substituting each `$param`
// splice with a fresh clone of the matching argument. Covers the expression
// kinds a v0.5 macro template uses; an unsupported kind returns nullptr so the
// caller can report it. Arguments are cloned with no further substitution (they
// are final use-site expressions, never containing splices).
ast::ExprPtr clone_macro_template(const ast::Expr& src, const SpliceSubs& subs, bool tmpl = true) {
    using namespace ast;
    switch (src.kind) {
    case NodeKind::SpliceExpr: {
        const auto& sp = static_cast<const SpliceExpr&>(src);
        if (sp.inner != nullptr && sp.inner->kind == NodeKind::IdentExpr) {
            auto it = subs.find(static_cast<const IdentExpr&>(*sp.inner).name);
            if (it != subs.end()) {
                auto arg = clone_macro_template(*it->second, {}, false);  // splice in the arg
                if (!arg) {
                    return nullptr;
                }
                // Parenthesize so a spliced compound argument keeps its
                // grouping inside the template (e.g. `$a * $a` with a = `p + 1`
                // is `(p + 1) * (p + 1)`, not `p + 1 * p + 1`).
                auto paren = std::make_unique<ParenExpr>();
                paren->inner = std::move(arg);
                paren->range = src.range;
                return paren;
            }
        }
        return nullptr;  // v0.5 templates support only `$param` splices
    }
    case NodeKind::IntLit: {
        auto n = std::make_unique<IntLit>();
        n->text = static_cast<const IntLit&>(src).text;
        n->range = src.range;
        return n;
    }
    case NodeKind::FloatLit: {
        auto n = std::make_unique<FloatLit>();
        n->text = static_cast<const FloatLit&>(src).text;
        n->range = src.range;
        return n;
    }
    case NodeKind::StringLit: {
        auto n = std::make_unique<StringLit>();
        n->text = static_cast<const StringLit&>(src).text;
        n->range = src.range;
        return n;
    }
    case NodeKind::CharLit: {
        auto n = std::make_unique<CharLit>();
        n->text = static_cast<const CharLit&>(src).text;
        n->range = src.range;
        return n;
    }
    case NodeKind::BoolLit: {
        auto n = std::make_unique<BoolLit>();
        n->value = static_cast<const BoolLit&>(src).value;
        n->range = src.range;
        return n;
    }
    case NodeKind::NilLit: {
        auto n = std::make_unique<NilLit>();
        n->range = src.range;
        return n;
    }
    case NodeKind::IdentExpr: {
        auto n = std::make_unique<IdentExpr>();
        n->name = static_cast<const IdentExpr&>(src).name;
        n->definition_site = tmpl;
        n->range = src.range;
        return n;
    }
    case NodeKind::ParenExpr: {
        const auto& p = static_cast<const ParenExpr&>(src);
        if (!p.inner) {
            return nullptr;
        }
        auto inner = clone_macro_template(*p.inner, subs, tmpl);
        if (!inner) {
            return nullptr;
        }
        auto n = std::make_unique<ParenExpr>();
        n->inner = std::move(inner);
        n->range = src.range;
        return n;
    }
    case NodeKind::UnaryExpr: {
        const auto& u = static_cast<const UnaryExpr&>(src);
        if (!u.operand) {
            return nullptr;
        }
        auto operand = clone_macro_template(*u.operand, subs, tmpl);
        if (!operand) {
            return nullptr;
        }
        auto n = std::make_unique<UnaryExpr>();
        n->op = u.op;
        n->operand = std::move(operand);
        n->range = src.range;
        return n;
    }
    case NodeKind::BinaryExpr: {
        const auto& b = static_cast<const BinaryExpr&>(src);
        if (!b.lhs || !b.rhs) {
            return nullptr;
        }
        auto lhs = clone_macro_template(*b.lhs, subs, tmpl);
        auto rhs = clone_macro_template(*b.rhs, subs, tmpl);
        if (!lhs || !rhs) {
            return nullptr;
        }
        auto n = std::make_unique<BinaryExpr>();
        n->op = b.op;
        n->lhs = std::move(lhs);
        n->rhs = std::move(rhs);
        n->range = src.range;
        return n;
    }
    case NodeKind::MemberExpr: {
        const auto& m = static_cast<const MemberExpr&>(src);
        if (!m.base) {
            return nullptr;
        }
        auto base = clone_macro_template(*m.base, subs, tmpl);
        if (!base) {
            return nullptr;
        }
        auto n = std::make_unique<MemberExpr>();
        n->base = std::move(base);
        n->member = m.member;
        n->is_optional_chain = m.is_optional_chain;
        n->range = src.range;
        return n;
    }
    case NodeKind::CallExpr: {
        const auto& c = static_cast<const CallExpr&>(src);
        if (!c.callee || !c.type_args.empty()) {
            return nullptr;  // explicit call type-args unsupported in a v0.5 template
        }
        auto callee = clone_macro_template(*c.callee, subs, tmpl);
        if (!callee) {
            return nullptr;
        }
        auto n = std::make_unique<CallExpr>();
        n->callee = std::move(callee);
        for (const auto& a : c.args) {
            if (!a.value) {
                return nullptr;
            }
            auto v = clone_macro_template(*a.value, subs, tmpl);
            if (!v) {
                return nullptr;
            }
            CallExpr::Arg na;
            na.label = a.label;
            na.is_inout = a.is_inout;
            na.value = std::move(v);
            n->args.push_back(std::move(na));
        }
        n->range = src.range;
        return n;
    }
    default:
        return nullptr;
    }
}

// §12.4 the `quote { … }` template an expression macro returns: the QuoteExpr
// of a `return quote { … }` (or a bare trailing quote) in the macro body.
const ast::QuoteExpr* macro_template(const ast::FuncDecl& fn) {
    if (!fn.body) {
        return nullptr;
    }
    if (fn.body->kind == ast::NodeKind::QuoteExpr) {
        return static_cast<const ast::QuoteExpr*>(fn.body.get());
    }
    if (fn.body->kind == ast::NodeKind::BlockExpr) {
        const auto& blk = static_cast<const ast::BlockExpr&>(*fn.body);
        for (const auto& s : blk.stmts) {
            if (s->kind == ast::NodeKind::ReturnStmt) {
                const auto& r = static_cast<const ast::ReturnStmt&>(*s);
                if (r.value && r.value->kind == ast::NodeKind::QuoteExpr) {
                    return static_cast<const ast::QuoteExpr*>(r.value.get());
                }
            }
        }
    }
    return nullptr;
}

}  // namespace

TypePtr Resolver::check_expr(const ast::Expr& e, TypePtr expected) {
    TypePtr t = nullptr;
    switch (e.kind) {
    case ast::NodeKind::IntLit:
        // Integer literals adopt the expected type when it is an integer; this
        // is the bidirectional bit that lets `let x: Int32 = 42` typecheck
        // without an explicit conversion. Without context, default to Int.
        // §9 ergonomic: peel one Optional layer so `let x: Int32? = 42`
        // also adopts Int32 instead of falling back to the Int default.
        // §A4 same trick for Atomic[T]: `static x: Atomic[UInt32] = 0`
        // adopts UInt32 for the literal so the initial-value type
        // matches the inner.
        {
            TypePtr int_hint = expected;
            if (int_hint != nullptr && int_hint->kind() == TypeKind::Optional) {
                int_hint = int_hint->inner();
            } else if (int_hint != nullptr && int_hint->kind() == TypeKind::Atomic) {
                int_hint = int_hint->inner();
            }
            t = (int_hint != nullptr && int_hint->is_integer()) ? int_hint
                                                                : types_->primitive(TypeKind::Int);
            check_int_literal_range(static_cast<const ast::IntLit&>(e), t);
        }
        break;
    case ast::NodeKind::FloatLit: {
        TypePtr float_hint = expected;
        if (float_hint != nullptr && float_hint->kind() == TypeKind::Optional) {
            float_hint = float_hint->inner();
        }
        t = (float_hint != nullptr && float_hint->is_float())
                ? float_hint
                : types_->primitive(TypeKind::Float64);
        break;
    }
    case ast::NodeKind::BoolLit:
        t = types_->boolean();
        break;
    case ast::NodeKind::NilLit:
        // `nil` is the §9 absence literal. Its type is the expected
        // Optional<T> when that's known; otherwise we fall back to
        // Optional<Never>, which TypeArena::assignable treats as
        // compatible with every Optional<T> so e.g. `let x: Int32? = nil`
        // still typechecks even though `nil` was checked before the let-
        // initializer ran.
        t = (expected != nullptr && expected->kind() == TypeKind::Optional)
                ? expected
                : types_->make_optional(types_->never());
        break;
    case ast::NodeKind::StringLit:
        t = types_->primitive(TypeKind::StrConst);
        break;
    case ast::NodeKind::CharLit:
        t = types_->primitive(TypeKind::Char);
        break;

    case ast::NodeKind::IdentExpr: {
        const auto& ident = static_cast<const ast::IdentExpr&>(e);
        const auto* sym = ident.definition_site ? scopes_.global().lookup(ident.name)
                                                : scopes_.current().lookup(ident.name);
        if (sym == nullptr && ident.definition_site) {
            error_at(e.range,
                     std::format("undefined name '{}' in a macro template: a template name "
                                 "resolves where the macro is defined; pass a use-site value as "
                                 "an argument and splice it with `$`",
                                 ident.name));
            t = types_->error();
        } else if (sym == nullptr) {
            error_at(e.range, std::format("undefined name '{}'", ident.name));
            t = types_->error();
        } else {
            resolution_.set_symbol(&e, sym);
            check_visibility(*sym, e.range);
            t = sym->type != nullptr ? sym->type : types_->error();
            check_noinit_value_use(*sym, ident.name, e.range);
            check_view_capture(*sym, e.range);
            if (sym->nonescaping && sym->kind == SymbolKind::Parameter
                && !nonesc_ok_.contains(&e) && nonesc_reported_.insert(&e).second) {
                error_at(e.range,
                         std::format("'{}' is a nonescaping closure parameter: it can be called, "
                                     "or passed to another nonescaping parameter, but not stored, "
                                     "returned or captured",
                                     ident.name));
            }
        }
        break;
    }
    case ast::NodeKind::SelfExpr:
        // §7 `self` is the method's receiver, typed as its owner. Outside a
        // method (or where the owner has no nameable type) it stays the
        // error type, which suppresses checks rather than inventing them.
        t = current_self_type_ != nullptr ? current_self_type_ : types_->error();
        break;
    case ast::NodeKind::ParenExpr:
        t = check_expr(*static_cast<const ast::ParenExpr&>(e).inner, expected);
        break;
    case ast::NodeKind::TupleLitExpr: {
        // §6 `(e1, e2, …)` types as Tuple<typeof(e1), typeof(e2), …>.
        // When the surrounding context expects a TupleType of matching
        // arity, push each expected element down so a literal inside
        // the tuple adopts the expected primitive width.
        const auto& tl = static_cast<const ast::TupleLitExpr&>(e);
        std::vector<TypePtr> parts;
        parts.reserve(tl.elements.size());
        const auto* expected_parts = (expected != nullptr && expected->kind() == TypeKind::Tuple
                                      && expected->parts().size() == tl.elements.size())
                                         ? &expected->parts()
                                         : nullptr;
        for (std::size_t i = 0; i < tl.elements.size(); ++i) {
            TypePtr elem_hint = expected_parts != nullptr ? (*expected_parts)[i] : nullptr;
            parts.push_back(check_expr(*tl.elements[i], elem_hint));
        }
        t = types_->make_tuple(std::move(parts));
        break;
    }
    case ast::NodeKind::VectorLitExpr: {
        // §13 `[e1, e2, ...]` literal. Static decls with `[N]T`
        // annotations (including §A8's `[N]@interrupt(T)` vector
        // tables) push T down as the element-type hint so each
        // element type-checks against the slot type — catching
        // mismatched ISRs at the assignment site rather than deep
        // in C++. Without an annotation we fall back to inferring
        // the element type from the first element.
        const auto& vl = static_cast<const ast::VectorLitExpr&>(e);
        TypePtr elem_hint = (expected != nullptr && expected->kind() == TypeKind::Vector)
                                ? expected->inner()
                                : nullptr;
        TypePtr elem_type = elem_hint;
        for (std::size_t i = 0; i < vl.elements.size(); ++i) {
            if (vl.elements[i] == nullptr) {
                continue;
            }
            TypePtr et = check_expr(*vl.elements[i], elem_hint);
            if (elem_type == nullptr) {
                elem_type = et;
            }
            if (elem_hint != nullptr && !TypeArena::assignable(et, elem_hint)) {
                error_at(vl.elements[i]->range,
                         std::format("vector element {} of type {} does not match "
                                     "expected element type {}",
                                     i,
                                     et != nullptr ? et->describe() : "?",
                                     elem_hint->describe()));
            }
        }
        if (elem_type == nullptr) {
            elem_type = types_->error();
        }
        t = types_->make_vector(static_cast<std::int64_t>(vl.elements.size()), elem_type);
        break;
    }
    case ast::NodeKind::UnaryExpr:
        t = check_unary(static_cast<const ast::UnaryExpr&>(e), expected);
        break;
    case ast::NodeKind::BinaryExpr:
        t = check_binary(static_cast<const ast::BinaryExpr&>(e), expected);
        break;
    case ast::NodeKind::CallExpr:
        t = check_call(static_cast<const ast::CallExpr&>(e), expected);
        break;
    case ast::NodeKind::IfExpr:
        t = check_if(static_cast<const ast::IfExpr&>(e), expected);
        break;
    case ast::NodeKind::BlockExpr:
        t = check_block_expr(static_cast<const ast::BlockExpr&>(e), expected);
        break;
    case ast::NodeKind::ClosureExpr: {
        // §16 closure literal. The parser stores param names with no
        // type annotations — types come from a contextual expected
        // Function type supplied by the use site (e.g. .mapError, the
        // map/filter builtins, or a typed local). Without that
        // context the closure can't be typed in v0.5; we diagnose
        // here rather than fall through to Error so the user gets a
        // specific message.
        const auto& cx = static_cast<const ast::ClosureExpr&>(e);
        if (expected == nullptr || expected->kind() != TypeKind::Function
            || expected->parts().size() != cx.params.size()) {
            error_at(cx.range,
                     "closure type cannot be inferred without context; pass it directly to a "
                     "function-typed slot (e.g. .mapError, map, filter) or annotate the binding");
            // Still walk the body so any errors inside it surface,
            // but with each param at Error (so they're permissive).
            ScopeStack::Guard g(scopes_);
            for (const auto& name : cx.params) {
                Symbol sym;
                sym.name = name;
                sym.kind = SymbolKind::Local;
                sym.is_mutable = false;
                sym.type = types_->error();
                sym.definition_range = cx.range;
                (void)g.scope().insert(std::move(sym));
            }
            for (const auto& stmt : cx.body) {
                if (stmt) {
                    check_stmt(*stmt);
                }
            }
            t = types_->error();
            break;
        }
        closures_.push_back(&cx);
        ScopeStack::Guard g(scopes_);
        for (std::size_t i = 0; i < cx.params.size(); ++i) {
            Symbol sym;
            sym.name = cx.params[i];
            sym.kind = SymbolKind::Local;
            sym.is_mutable = false;
            sym.type = expected->parts()[i] != nullptr ? expected->parts()[i] : types_->error();
            sym.definition_range = cx.range;  // a closure parameter is defined by the closure
            (void)g.scope().insert(std::move(sym));
        }
        // The body's trailing ExprStmt yields the closure result —
        // mirror check_block_expr's "last expression types the block"
        // shape so a single-expression closure `{ x => x * 2 }` types
        // cleanly without needing a `return`. Mid-body statements are
        // checked normally.
        TypePtr body_type = nullptr;
        for (std::size_t i = 0; i < cx.body.size(); ++i) {
            const auto& stmt = cx.body[i];
            if (!stmt) {
                continue;
            }
            const bool is_last = (i + 1 == cx.body.size());
            if (is_last && stmt->kind == ast::NodeKind::ExprStmt
                && static_cast<const ast::ExprStmt&>(*stmt).expr != nullptr) {
                body_type =
                    check_expr(*static_cast<const ast::ExprStmt&>(*stmt).expr, expected->result());
            } else {
                check_stmt(*stmt);
            }
        }
        closures_.pop_back();
        if (body_type == nullptr) {
            body_type = types_->unit();
        }
        if (expected->result() != nullptr
            && !TypeArena::assignable(body_type, expected->result())) {
            error_at(cx.range,
                     std::format("closure body types as {} but the expected return type is {}",
                                 body_type->describe(),
                                 expected->result()->describe()));
        }
        t = types_->make_function(expected->parts(), body_type, expected->caps());
        break;
    }
    case ast::NodeKind::CopyExpr:
        t = check_expr(*static_cast<const ast::CopyExpr&>(e).inner);
        break;
    case ast::NodeKind::AddressOfExpr: {
        // §A12 (§14.6.3) `&decl` — address-of a static or function.
        // - Static: result is `Ptr[T]` over the static's value type
        //   (spec calls for `Ptr[T]?` since a weakly-linked extern can
        //   be null, but optional wrapping is deferred).
        // - Func: result is the function-pointer type matching the
        //   callee's signature `(T1, T2, …) -> R`. Vestra's spelling
        //   for function pointers is the same as for closures —
        //   `TypeKind::Function` — so a callee site that expects a
        //   function-pointer parameter accepts both `&some_func` and
        //   an inline closure (the C++ compiler enforces the capture-
        //   less requirement when one of these flows into a function-
        //   pointer slot).
        const auto& a = static_cast<const ast::AddressOfExpr&>(e);
        if (a.inner != nullptr && a.inner->kind == ast::NodeKind::IdentExpr) {
            const auto& id = static_cast<const ast::IdentExpr&>(*a.inner);
            if (const auto* sym = scopes_.current().lookup(id.name)) {
                resolution_.set_symbol(a.inner.get(), sym);
                if (sym->kind == SymbolKind::Static && sym->type != nullptr) {
                    if (!sym->type->is_error()) {
                        resolution_.set_type(a.inner.get(), sym->type);
                    }
                    t = types_->make_ptr(sym->type != nullptr ? sym->type : types_->error());
                    break;
                }
                if (sym->kind == SymbolKind::Func) {
                    // The Func symbol's type already holds the
                    // canonical `(params...) -> result` shape that
                    // collect_func populated via function_type_of(f).
                    if (sym->type != nullptr) {
                        resolution_.set_type(a.inner.get(), sym->type);
                        t = sym->type;
                    } else {
                        t = types_->error();
                    }
                    break;
                }
                error_at(a.inner->range,
                         std::format("&decl operand must be a static or func, got "
                                     "{} '{}'",
                                     sym->kind == SymbolKind::Local ? "local" : "name",
                                     id.name));
            } else {
                error_at(a.inner->range,
                         std::format("undefined name '{}' in &decl operand", id.name));
            }
            t = types_->error();
        } else if (a.inner != nullptr && a.inner->kind == ast::NodeKind::MemberExpr) {
            // §14.12.2 `&Sysreg.X` is rejected by spec: a sysreg is
            // an architectural cell accessed via `mrs` / `msr`, not a
            // memory cell with a linkage symbol. The generic
            // "must be a static or func identifier" diagnostic doesn't
            // make that distinction clear, so we name Sysreg
            // specifically here. Other MemberExpr operands (struct
            // field access, etc.) fall through to the generic message.
            const auto& mem = static_cast<const ast::MemberExpr&>(*a.inner);
            if (mem.base != nullptr && mem.base->kind == ast::NodeKind::IdentExpr
                && static_cast<const ast::IdentExpr&>(*mem.base).name == "Sysreg"
                && scopes_.current().lookup("Sysreg") == nullptr) {
                error_at(e.range,
                         std::format("&Sysreg.{} is rejected: sysregs are architectural "
                                     "cells, not memory cells with a linkage symbol "
                                     "(§14.12.2)",
                                     mem.member));
            } else if (auto qt = check_qualified_module_ref(mem); qt != nullptr) {
                // §5 / §14.6.3 `&module.decl`: the address of an imported
                // static or function, as for a local one.
                const auto* sym = resolution_.symbol_of(a.inner.get());
                if (sym != nullptr && sym->kind == SymbolKind::Static && !qt->is_error()) {
                    resolution_.set_type(a.inner.get(), qt);
                    t = types_->make_ptr(qt);
                    break;
                }
                if (sym != nullptr && sym->kind == SymbolKind::Func && !qt->is_error()) {
                    resolution_.set_type(a.inner.get(), qt);
                    t = qt;
                    break;
                }
                if (!qt->is_error()) {
                    error_at(e.range, "&decl operand must be a static or func identifier");
                }
            } else {
                error_at(e.range, "&decl operand must be a static or func identifier");
            }
            t = types_->error();
        } else {
            error_at(e.range, "&decl operand must be a static or func identifier");
            t = types_->error();
        }
        break;
    }
    case ast::NodeKind::QuoteExpr: {
        const auto& q = static_cast<const ast::QuoteExpr&>(e);
        if (expected != nullptr && expected->kind() == TypeKind::AstExpr) {
            // §12.4 macro context: the quote is a deferred AST template, typed
            // as `Expr`. Its body is *not* checked as a runtime expression here
            // — it is re-checked after the macro expands at each call site (so
            // `quote { $x + $x }` doesn't try to add two `Expr` values).
            t = types_->primitive(TypeKind::AstExpr);
        } else {
            // §12.4 v0.5 identity quote — types as its body expression (with
            // splices resolved against the surrounding scope). The body is
            // checked with quote depth raised so a `$` splice is admitted.
            ++quote_depth_;
            t = q.inner ? check_expr(*q.inner, expected) : types_->error();
            --quote_depth_;
        }
        break;
    }
    case ast::NodeKind::SpliceExpr: {
        // §12.4 — `$x` / `$(expr)` is only legal inside a quote; its type
        // is the spliced inner expression's type (resolved in the
        // surrounding scope).
        const auto& sp = static_cast<const ast::SpliceExpr&>(e);
        if (quote_depth_ == 0) {
            error_at(e.range, "a `$` splice is only valid inside a `quote { … }`");
        }
        t = sp.inner ? check_expr(*sp.inner, expected) : types_->error();
        break;
    }
    case ast::NodeKind::MacroCallExpr: {
        // §12.4 expression macro `@name(args)`. The macro is a comptime func
        // `(Expr, …) -> Expr` whose body is a `quote { … }` template; expand by
        // substituting each argument for the matching `$param` splice, then
        // type-check the expansion as ordinary code (the spec's "re-checked").
        const auto& mc = static_cast<const ast::MacroCallExpr&>(e);
        for (const auto& a : mc.args) {  // check args in the use-site scope
            if (a) {
                check_expr(*a);
            }
        }
        const Symbol* sym = scopes_.current().lookup(mc.name);
        const ast::FuncDecl* fn =
            (sym != nullptr && sym->decl != nullptr && sym->decl->kind == ast::NodeKind::Func)
                ? static_cast<const ast::FuncDecl*>(sym->decl)
                : nullptr;
        if (fn == nullptr) {
            error_at(e.range, std::format("unknown macro '{}'", mc.name));
            t = types_->error();
            break;
        }
        // A macro is a comptime func taking and returning `Expr`.
        TypePtr ft = sym->type;
        bool shape_ok = fn->is_comptime && ft != nullptr && ft->kind() == TypeKind::Function
                        && ft->result() != nullptr && ft->result()->kind() == TypeKind::AstExpr;
        if (shape_ok) {
            for (TypePtr pt : ft->parts()) {
                if (pt == nullptr || pt->kind() != TypeKind::AstExpr) {
                    shape_ok = false;
                    break;
                }
            }
        }
        if (!shape_ok) {
            error_at(e.range,
                     std::format("'{}' is not an expression macro — a macro is a `comptime func` "
                                 "taking and returning `Expr`",
                                 mc.name));
            t = types_->error();
            break;
        }
        if (mc.args.size() != fn->params.size()) {
            error_at(e.range,
                     std::format("macro '{}' expects {} argument(s), got {}",
                                 mc.name,
                                 fn->params.size(),
                                 mc.args.size()));
            t = types_->error();
            break;
        }
        const ast::QuoteExpr* tmpl = macro_template(*fn);
        if (tmpl == nullptr || tmpl->inner == nullptr) {
            error_at(e.range,
                     std::format("macro '{}' must return a `quote {{ … }}` template", mc.name));
            t = types_->error();
            break;
        }
        SpliceSubs subs;
        for (std::size_t i = 0; i < fn->params.size(); ++i) {
            subs[fn->params[i].name] = mc.args[i].get();
        }
        ast::ExprPtr expansion = clone_macro_template(*tmpl->inner, subs);
        if (!expansion) {
            error_at(e.range,
                     std::format("macro '{}' uses an expression form not supported in a v0.5 "
                                 "template",
                                 mc.name));
            t = types_->error();
            break;
        }
        // Type-check the expansion as ordinary code; it becomes what codegen
        // emits in place of the macro call.
        t = check_expr(*expansion, expected);
        const_cast<ast::MacroCallExpr&>(mc).expansion = std::move(expansion);
        break;
    }
    case ast::NodeKind::AwaitExpr: {
        // §11 `await e` consumes its operand for the underlying value: a
        // `Future[T]` (from `spawn`) unwraps to T; a direct async call is
        // already typed T (async results aren't wrapped), so await is
        // transparent there.
        auto inner = check_expr(*static_cast<const ast::AwaitExpr&>(e).inner);
        t = (inner != nullptr && inner->kind() == TypeKind::Future) ? inner->inner() : inner;
        break;
    }
    case ast::NodeKind::SpawnExpr: {
        // §11 `spawn f(args)` runs the call and yields a `Future[T]` where
        // T is the call's result type.
        const auto& sp = static_cast<const ast::SpawnExpr&>(e);
        t = types_->make_future(check_expr(*sp.inner));
        // §7 a spawned call outlives the caller's frame, so a `nonescaping`
        // parameter can't be what it calls.
        if (sp.inner != nullptr && sp.inner->kind == ast::NodeKind::CallExpr) {
            const auto* callee =
                peel_parens(static_cast<const ast::CallExpr&>(*sp.inner).callee.get());
            const auto* cs = callee != nullptr ? resolution_.symbol_of(callee) : nullptr;
            if (cs != nullptr && cs->nonescaping && nonesc_reported_.insert(callee).second) {
                error_at(callee->range,
                         std::format("'{}' is a nonescaping closure parameter, so it can't be "
                                     "spawned: the task could outlive the call",
                                     cs->name));
            }
        }
        // §10/§11 "spawn captures by value": the spawned task copies its read
        // parameters into the coroutine frame, so each must be an owned value.
        // A non-escapable borrowed view (Span / MutSpan) parameter would copy
        // the view, not the data — and the Future may outlive the view's
        // backing — so a spawned call with a view parameter is rejected. (Keyed
        // on the *parameter* type: an array argument coerces to a Span only at
        // the param boundary.) `parallel` is the safe way to run work over a
        // borrowed view — it can't let the view escape.
        if (sp.inner != nullptr && sp.inner->kind == ast::NodeKind::CallExpr) {
            const auto& call = static_cast<const ast::CallExpr&>(*sp.inner);
            TypePtr ft = call.callee ? resolution_.type_of(call.callee.get()) : nullptr;
            if (ft != nullptr && ft->kind() == TypeKind::Function) {
                const auto& params = ft->parts();
                for (std::size_t i = 0; i < params.size(); ++i) {
                    TypePtr pt = params[i];
                    // Any non-escapable type, transitively (a struct holding a
                    // Str, an Optional view, an MMIO view).
                    if (pt == nullptr || !non_escapable(pt)) {
                        continue;
                    }
                    // Point at the matching argument when present, else the call.
                    const diag::SourceRange where =
                        (i < call.args.size() && call.args[i].value != nullptr)
                            ? call.args[i].value->range
                            : sp.inner->range;
                    error_at(where,
                             std::format("cannot `spawn` a call with a {} parameter — a spawned "
                                         "task captures its arguments by value, but a borrowed "
                                         "view can't be owned (use `parallel` to run work over a "
                                         "view)",
                                         pt->describe()));
                }
            }
        }
        break;
    }
    case ast::NodeKind::ThrowExpr: {
        // §9: `throw e` is only valid inside a throws(E) function; e must
        // be assignable to E. The expression's static type is Never.
        const auto& th = static_cast<const ast::ThrowExpr&>(e);
        TypePtr enclosing_err = throws_stack_.empty() ? nullptr : throws_stack_.back();
        auto inner_type = check_expr(*th.inner, enclosing_err);
        if (enclosing_err == nullptr) {
            error_at(e.range, "'throw' is only valid inside a function declared 'throws(E)'");
        } else if (inner_type != nullptr && !TypeArena::assignable(inner_type, enclosing_err)) {
            error_at(th.inner->range,
                     std::format("'throw' value of type {} does not match the enclosing throws({})",
                                 inner_type->describe(),
                                 enclosing_err->describe()));
        }
        t = types_->never();
        break;
    }
    case ast::NodeKind::TryExpr: {
        // §9: `try e` / `try? e` / `try! e` all require `e` to evaluate to
        // a Result<T, E> (i.e. a call to a throws function). The three
        // forms then differ in how they consume the error:
        //   * Propagating: returns the error from the enclosing function
        //     (which must itself be throws-compatible with E); yields T.
        //   * Optional:    converts to Optional<T>; yields Optional<T>.
        //   * Forced:      panics on error; yields T.
        const auto& tx = static_cast<const ast::TryExpr&>(e);
        auto inner_type = check_expr(*tx.inner);
        if (inner_type == nullptr || inner_type->is_error()) {
            t = types_->error();
            break;
        }
        if (inner_type->kind() != TypeKind::Result) {
            // §9 form-specific diagnostic. The most common confusion is
            // applying a try-prefix to an Optional-returning call —
            // each form has a different "right idiom" to suggest, and
            // a vague "needs Result<T, E>" message just sends the user
            // hunting. When the operand is Optional<T>, point them at
            // the matching non-try construct directly.
            const bool inner_opt = inner_type->kind() == TypeKind::Optional;
            std::string_view form_text = "try";
            switch (tx.form) {
            case ast::TryExpr::Form::Propagating:
                form_text = "try";
                break;
            case ast::TryExpr::Form::Optional:
                form_text = "try?";
                break;
            case ast::TryExpr::Form::Forced:
                form_text = "try!";
                break;
            }
            if (inner_opt) {
                std::string_view hint;
                switch (tx.form) {
                case ast::TryExpr::Form::Optional:
                    hint = "this operand already produces an Optional — drop `try?`, or use "
                           "`?.` / `if let` / `match` to consume it";
                    break;
                case ast::TryExpr::Form::Propagating:
                    hint = "Optional doesn't carry an error to propagate — use `?.` for "
                           "chaining, `if let` / `match` to branch, or `!` for an asserted "
                           "unwrap";
                    break;
                case ast::TryExpr::Form::Forced:
                    hint = "use postfix `!` for force-unwrap of an Optional; `try!` is for "
                           "Result<T, E>";
                    break;
                }
                error_at(tx.inner->range,
                         std::format("'{}' requires a Result<T, E> operand, got {}: {}",
                                     form_text,
                                     inner_type->describe(),
                                     hint));
            } else {
                error_at(tx.inner->range,
                         std::format("'{}' operand must produce a Result<T, E>, got {}",
                                     form_text,
                                     inner_type->describe()));
            }
            t = types_->error();
            break;
        }
        TypePtr ok = inner_type->inner();
        TypePtr err = inner_type->result();
        if (tx.form == ast::TryExpr::Form::Propagating) {
            // §9: a Propagating try inside a bare `do { ... } catch
            // NAME { ... }` writes its error type into the do-catch's
            // inference slot. The slot takes precedence over the
            // enclosing function's throws_stack_ — the do-catch is
            // the closer error handler.
            if (!do_catch_infer_stack_.empty()) {
                TypePtr* slot = do_catch_infer_stack_.back();
                if (*slot == nullptr) {
                    *slot = err;
                } else if (!TypeArena::equal(err, *slot)) {
                    error_at(e.range,
                             std::format("do-body tries with mismatched error types: {} vs {}; "
                                         "use 'catch (NAME: E)' to pin the type explicitly",
                                         (*slot)->describe(),
                                         err ? err->describe() : "?"));
                }
            } else {
                TypePtr enclosing_err = throws_stack_.empty() ? nullptr : throws_stack_.back();
                if (enclosing_err == nullptr) {
                    error_at(
                        e.range,
                        "'try' propagation requires the enclosing function to declare 'throws(E)'");
                } else if (!TypeArena::assignable(err, enclosing_err)) {
                    error_at(
                        e.range,
                        std::format(
                            "propagated error of type {} does not match the enclosing throws({})",
                            err ? err->describe() : "?",
                            enclosing_err->describe()));
                }
            }
            t = ok;
        } else if (tx.form == ast::TryExpr::Form::Optional) {
            t = types_->make_optional(ok);
        } else {
            // Forced: `try!` panics on error.
            t = ok;
        }
        break;
    }
    case ast::NodeKind::DoCatchExpr: {
        // §9 `do { body } catch ... { handler }` — two binding shapes:
        //   * Annotated: `catch (NAME: E)` — E is known up front; we
        //     push it onto throws_stack_ and the body's tries check
        //     against it normally.
        //   * Bare: `catch NAME` — E is *inferred* from the body's
        //     try-calls. We push a fresh inference slot onto
        //     do_catch_infer_stack_; check_try writes the first
        //     try's error type there and checks subsequent ones for
        //     match. Throws_stack_ stays untouched in this mode
        //     since check_try prefers the infer slot when present.
        const auto& dc = static_cast<const ast::DoCatchExpr&>(e);
        TypePtr error_type = dc.error_type ? resolve_type(*dc.error_type) : nullptr;
        TypePtr inferred = nullptr;
        const bool inferring = (dc.error_type == nullptr);
        if (inferring) {
            do_catch_infer_stack_.push_back(&inferred);
        } else {
            throws_stack_.push_back(error_type);
        }
        TypePtr body_type = check_expr(*dc.do_body, expected);
        if (inferring) {
            do_catch_infer_stack_.pop_back();
            if (inferred == nullptr) {
                error_at(dc.range,
                         "bare 'catch NAME' requires the do-body to contain at least one 'try'; "
                         "either add a try-call or use 'catch (NAME: E)' to give the error type "
                         "explicitly");
                inferred = types_->error();
            }
            error_type = inferred;
            resolution_.set_do_catch_error_type(&dc, error_type);
        } else {
            throws_stack_.pop_back();
        }

        ScopeStack::Guard g(scopes_);
        Symbol sym;
        sym.name = dc.error_name;
        sym.kind = SymbolKind::Local;
        sym.is_mutable = false;
        sym.type = error_type;
        sym.definition_range = dc.range;
        (void)scopes_.current().insert(std::move(sym));
        // §9 `where guard`: type-check under the binding's scope; the
        // guard must produce Bool. A failed guard at runtime
        // propagates the bound error to the enclosing throws context,
        // so we also require that context exists and matches the
        // do-catch's error type. (throws_stack_'s top here is the
        // enclosing function's throws_type — the do-catch's own push
        // / pop already happened above.)
        if (dc.guard) {
            auto gt = check_expr(*dc.guard, types_->boolean());
            if (gt != nullptr && !gt->is_error() && gt->kind() != TypeKind::Bool) {
                error_at(dc.guard->range,
                         std::format("do/catch where-guard must be Bool, got {}", gt->describe()));
            }
            TypePtr enclosing = throws_stack_.empty() ? nullptr : throws_stack_.back();
            if (enclosing == nullptr) {
                error_at(dc.guard->range,
                         "do/catch where-guard requires the enclosing function to be "
                         "`throws(E)` so the fall-through can propagate the error");
            } else if (error_type != nullptr && !error_type->is_error()
                       && !TypeArena::assignable(error_type, enclosing)) {
                error_at(dc.guard->range,
                         std::format("do/catch where-guard fall-through propagates {}, but "
                                     "the enclosing throws context is {}; either widen with "
                                     "`.mapError(...)` or remove the guard",
                                     error_type->describe(),
                                     enclosing->describe()));
            }
        }
        TypePtr catch_type = check_expr(*dc.catch_body, expected != nullptr ? expected : body_type);

        if (body_type == nullptr || catch_type == nullptr || body_type->is_error()
            || catch_type->is_error()) {
            t = types_->error();
            break;
        }
        if (body_type->is_never()) {
            t = catch_type;
            break;
        }
        if (catch_type->is_never()) {
            t = body_type;
            break;
        }
        if (!TypeArena::equal(body_type, catch_type)) {
            error_at(e.range,
                     std::format("do/catch arms have different types: {} vs {}",
                                 body_type->describe(),
                                 catch_type->describe()));
            t = types_->error();
            break;
        }
        t = body_type;
        break;
    }
    case ast::NodeKind::AsExpr: {
        const auto& a = static_cast<const ast::AsExpr&>(e);
        const auto from = check_expr(*a.value);
        t = a.target ? resolve_type(*a.target) : types_->error();
        if (!a.is_optional) {
            check_reinterpret(a, from, t);
        } else {
            // The draft has `as?` in its grammar (§17) but gives it no
            // meaning, so it is rejected rather than lowered to a guess.
            error_at(a.range,
                     "`as?` is not supported: the draft gives it no semantics yet; use `as` "
                     "for a byte reinterpretation");
        }
        if (a.is_optional && t != nullptr && !t->is_error()) {
            t = types_->make_optional(t);
        }
        break;
    }
    case ast::NodeKind::MemberExpr:
        t = check_member(static_cast<const ast::MemberExpr&>(e), expected);
        break;
    case ast::NodeKind::LeadingDotExpr:
        t = check_leading_dot(static_cast<const ast::LeadingDotExpr&>(e), expected);
        break;
    case ast::NodeKind::MatchExpr:
        t = check_match(static_cast<const ast::MatchExpr&>(e), expected);
        break;
    case ast::NodeKind::SelectExpr:
        t = check_select(static_cast<const ast::SelectExpr&>(e), expected);
        break;
    case ast::NodeKind::InterpStringExpr: {
        // §4: a `"…\(expr)…"` literal is a freshly allocated String.
        // Every splice must be Display-conformant — primitive numerics
        // / Bool / Char / string-likes pass implicitly; user types need
        // either `derive(Display)` or `derive(Debug)`. The `using Alloc`
        // requirement is the capability pass's (capability.cpp).
        const auto& is_ = static_cast<const ast::InterpStringExpr&>(e);
        for (const auto& seg : is_.segments) {
            if (seg.expr == nullptr) {
                continue;
            }
            auto seg_t = check_expr(*seg.expr);
            if (seg_t == nullptr || seg_t->is_error()) {
                continue;
            }
            if (!is_display_conformant(seg_t)) {
                error_at(seg.expr->range,
                         std::format("interpolation splice of type {} is not Display-conformant; "
                                     "add `derive(Display) for {}` (or `derive(Debug)`) to make it "
                                     "printable",
                                     seg_t->describe(),
                                     seg_t->describe()));
            }
        }
        t = types_->primitive(TypeKind::String);
        break;
    }
    case ast::NodeKind::IndexExpr: {
        // `base[i0, ...]`. v0.5 admits indexing on:
        //   * `[N]T`         → T
        //   * `Span[T]` / `MutSpan[T]` → T
        // Indices must be integer-typed. Other base shapes (struct,
        // pointer, etc.) are not yet wired; sema falls through to the
        // error type so downstream consumers don't cascade.
        const auto& ix = static_cast<const ast::IndexExpr&>(e);
        TypePtr base_t = ix.base ? check_expr(*ix.base) : nullptr;
        for (const auto& idx : ix.indices) {
            if (idx) {
                auto it = check_expr(*idx);
                if (it != nullptr && !it->is_error() && !it->is_integer()) {
                    error_at(idx->range,
                             std::format("index must be integer-typed, got {}", it->describe()));
                }
            }
        }
        if (base_t == nullptr || base_t->is_error()) {
            t = types_->error();
            break;
        }
        switch (base_t->kind()) {
        case TypeKind::Vector:
        case TypeKind::Span:
        case TypeKind::MutSpan:
        // §18.5 indexing a Vec[T] yields T, and traps out of range as a Span
        // does (§19.10); `get(i) -> T?` is the read that can't trap.
        case TypeKind::Vec:
            t = base_t->inner();
            break;
        // §5/§18.4 indexing a ChunkIter yields one sub-view (its inner Span /
        // MutSpan type) — the random-access counterpart to `for c in chunks`.
        case TypeKind::ChunkIter:
            t = base_t->inner();
            break;
        default:
            error_at(e.range, std::format("type {} is not indexable", base_t->describe()));
            t = types_->error();
            break;
        }
        break;
    }
    case ast::NodeKind::ComptimeExpr: {
        // §12.1 a `comptime { }` block is checked like any expression, as
        // pure code: see check_comptime_call.
        const auto& ce = static_cast<const ast::ComptimeExpr&>(e);
        ++comptime_depth_;
        t = ce.inner ? check_expr(*ce.inner, expected) : types_->error();
        --comptime_depth_;
        break;
    }
    case ast::NodeKind::EmbedExpr: {
        // §12.1 `@embed("path")` types as `[N]UInt8` where N is the file's
        // size at compile time. We ask the folder to read the file now;
        // the byte-count from the folded vector pins the type's length,
        // and stashing the folded value here also primes the side table
        // so codegen emits the brace-init literal without re-reading.
        const auto& ee = static_cast<const ast::EmbedExpr&>(e);
        auto folded = folder_.fold(ee, comptime_env_);
        if (!folded || folded->kind != ComptimeValue::Kind::Vector) {
            error_at(ee.range,
                     std::format("could not @embed file \"{}\" (path/manifest miss or no reader "
                                 "configured)",
                                 ee.path));
            t = types_->error();
            break;
        }
        resolution_.set_folded_value(&ee, *folded);
        t = types_->make_vector(folded->length, types_->primitive(TypeKind::UInt8));
        break;
    }

    default:
        // Unimplemented: walk children to surface their errors, then assign
        // Error so downstream consumers don't cascade.
        t = types_->error();
        break;
    }
    if (t != nullptr) {
        resolution_.set_type(&e, t);
    }
    return t;
}

bool Resolver::payload_enum(TypePtr t) {
    if (t == nullptr || t->kind() != TypeKind::Enum || t->nominal_decl() == nullptr
        || t->nominal_decl()->kind != ast::NodeKind::Enum) {
        return false;
    }
    const auto& ed = static_cast<const ast::EnumDecl&>(*t->nominal_decl());
    return !ed.generics.empty()
           || std::ranges::any_of(ed.cases, [](const auto& c) { return !c.payload.empty(); });
}

bool Resolver::nominal_cmp_kind(TypePtr t) {
    return t != nullptr && (t->kind() == TypeKind::OpaqueType || t->kind() == TypeKind::Struct);
}

std::string Resolver::opaque_underlying_text(TypePtr t) {
    if (t->nominal_decl() != nullptr && t->nominal_decl()->kind == ast::NodeKind::Opaque) {
        const auto& od = static_cast<const ast::OpaqueDecl&>(*t->nominal_decl());
        if (od.underlying != nullptr) {
            return resolve_type(*od.underlying)->describe();
        }
    }
    return "its representation";
}

void Resolver::check_nominal_comparison(const ast::BinaryExpr& b, TypePtr lhs) {
    const bool equality = b.op == ast::BinaryOp::Eq || b.op == ast::BinaryOp::Ne;
    if (type_satisfies_bound(lhs, equality ? "Eq" : "Ord")) {
        return;
    }
    const std::string name = lhs->describe();
    const std::string need = std::format("`derive({}) for {}`", equality ? "Eq" : "Ord", name);
    std::string why;
    if (lhs->kind() == TypeKind::OpaqueType) {
        why = std::format(": an opaque type inherits none of {}'s operations",
                          opaque_underlying_text(lhs));
    }
    error_at(b.range,
             std::format("operator '{}' on {} needs {}{}",
                         b.op == ast::BinaryOp::Eq   ? "=="
                         : b.op == ast::BinaryOp::Ne ? "!="
                         : b.op == ast::BinaryOp::Lt ? "<"
                         : b.op == ast::BinaryOp::Le ? "<="
                         : b.op == ast::BinaryOp::Gt ? ">"
                                                     : ">=",
                         name,
                         need,
                         why));
}

void Resolver::check_compound_assign(const ast::AssignStmt& a, TypePtr lhs) {
    if (lhs == nullptr || lhs->is_error()) {
        return;
    }
    switch (a.op) {
    case ast::AssignOp::AddAssign:
        // `s += "x"` appends to a String; Duration adds to Duration.
        if (lhs->kind() == TypeKind::String || lhs->kind() == TypeKind::Duration) {
            return;
        }
        [[fallthrough]];
    case ast::AssignOp::SubAssign:
    case ast::AssignOp::MulAssign:
    case ast::AssignOp::DivAssign:
    case ast::AssignOp::ModAssign:
        if (lhs->kind() == TypeKind::Duration) {
            return;
        }
        if (!lhs->is_numeric()) {
            error_at(a.target->range,
                     std::format("arithmetic operator requires numeric operands, got {}",
                                 lhs->describe()));
        }
        return;
    case ast::AssignOp::BitAndAssign:
    case ast::AssignOp::BitOrAssign:
    case ast::AssignOp::BitXorAssign:
    case ast::AssignOp::ShlAssign:
    case ast::AssignOp::ShrAssign:
        if (!lhs->is_integer()) {
            error_at(
                a.target->range,
                std::format("bitwise operator requires integer operands, got {}", lhs->describe()));
        }
        return;
    default:
        return;
    }
}

void Resolver::check_int_literal_range(const ast::IntLit& lit, TypePtr t) {
    const bool negated = negating_int_lit_;
    negating_int_lit_ = false;
    if (t == nullptr || !t->is_integer()) {
        return;
    }
    // Parse into 128 bits; a literal wider than that never fits.
    std::string_view raw = lit.text;
    std::string digits;
    for (char c : raw) {
        if (c != '_') {
            digits.push_back(c);
        }
    }
    unsigned base = 10;
    std::size_t at = 0;
    if (digits.size() >= 2 && digits[0] == '0') {
        switch (digits[1]) {
        case 'x': case 'X': base = 16; at = 2; break;
        case 'o': case 'O': base = 8; at = 2; break;
        case 'b': case 'B': base = 2; at = 2; break;
        default: break;
        }
    }
    using U128 = unsigned __int128;
    U128 v = 0;
    bool wide = false;
    for (std::size_t i = at; i < digits.size(); ++i) {
        const char c = digits[i];
        unsigned d = 0;
        if (c >= '0' && c <= '9') {
            d = static_cast<unsigned>(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = static_cast<unsigned>(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            d = static_cast<unsigned>(c - 'A' + 10);
        } else {
            return;  // not a plain integer spelling; the lexer owns that error
        }
        if (d >= base) {
            return;
        }
        if (v > (~U128{0} - d) / base) {
            wide = true;
            break;
        }
        v = v * base + d;
    }
    unsigned bits = 64;
    bool is_signed = true;
    std::string_view lo;
    std::string_view hi;
    switch (t->kind()) {
    case TypeKind::Int8: bits = 8; lo = "-128"; hi = "127"; break;
    case TypeKind::Int16: bits = 16; lo = "-32768"; hi = "32767"; break;
    case TypeKind::Int32: bits = 32; lo = "-2147483648"; hi = "2147483647"; break;
    case TypeKind::Int64:
    case TypeKind::Int: lo = "-9223372036854775808"; hi = "9223372036854775807"; break;
    case TypeKind::Int128:
        bits = 128;
        lo = "-170141183460469231731687303715884105728";
        hi = "170141183460469231731687303715884105727";
        break;
    case TypeKind::UInt8: bits = 8; is_signed = false; hi = "255"; break;
    case TypeKind::UInt16: bits = 16; is_signed = false; hi = "65535"; break;
    case TypeKind::UInt32: bits = 32; is_signed = false; hi = "4294967295"; break;
    case TypeKind::UInt64:
    case TypeKind::UInt: is_signed = false; hi = "18446744073709551615"; break;
    case TypeKind::UInt128:
        bits = 128;
        is_signed = false;
        hi = "340282366920938463463374607431768211455";
        break;
    default: return;
    }
    if (!is_signed) {
        lo = "0";
    }
    bool fits = !wide;
    if (fits) {
        const U128 one = 1;
        if (is_signed) {
            const U128 mag = one << (bits - 1);  // |min|; max is mag - 1
            fits = negated ? v <= mag : v < mag;
        } else {
            const U128 max = bits == 128 ? ~U128{0} : (one << bits) - 1;
            fits = negated ? v == 0 : v <= max;
        }
    }
    if (!fits && lit_range_reported_.insert(&lit).second) {
        error_at(negated ? neg_lit_range_ : lit.range, std::format("integer literal {}{} doesn't fit in {} (range {} to {})",
                                        negated ? "-" : "", lit.text, t->describe(), lo, hi));
    }
}

TypePtr Resolver::check_unary(const ast::UnaryExpr& u, TypePtr expected) {
    // Negation / bit-not preserve the operand's type, so the expected type
    // can be propagated inward.
    const ast::Expr* lit_under = u.operand.get();
    while (lit_under != nullptr && lit_under->kind == ast::NodeKind::ParenExpr) {
        lit_under = static_cast<const ast::ParenExpr*>(lit_under)->inner.get();
    }
    const bool neg_lit = u.op == ast::UnaryOp::Neg && lit_under != nullptr
                         && lit_under->kind == ast::NodeKind::IntLit;
    if (neg_lit) {
        negating_int_lit_ = true;
        neg_lit_range_ = u.range;
    }
    auto operand = check_expr(*u.operand, expected);
    negating_int_lit_ = false;
    if (operand == nullptr || operand->is_error()) {
        return types_->error();
    }
    switch (u.op) {
    case ast::UnaryOp::Neg:
        if (!operand->is_numeric()) {
            error_at(u.range,
                     std::format("unary '-' requires a numeric type, got {}", operand->describe()));
            return types_->error();
        }
        return operand;
    case ast::UnaryOp::Not:
        if (operand->kind() != TypeKind::Bool) {
            error_at(u.range, std::format("unary '!' requires Bool, got {}", operand->describe()));
            return types_->error();
        }
        return operand;
    case ast::UnaryOp::BitNot:
        if (!operand->is_integer()) {
            error_at(
                u.range,
                std::format("unary '~' requires an integer type, got {}", operand->describe()));
            return types_->error();
        }
        return operand;
    case ast::UnaryOp::Unwrap:
        // §9 postfix `!` panics on `.none` and yields the wrapped T.
        if (operand->kind() != TypeKind::Optional) {
            error_at(u.range,
                     std::format("postfix '!' requires an Optional, got {}", operand->describe()));
            return types_->error();
        }
        return operand->inner();
    }
    return types_->error();
}

// An expression of number literals alone: `48`, `-1`, `(2 * 3)`, `1 + 2`.
bool Resolver::is_number_literal_tree(const ast::Expr& e) {
    switch (e.kind) {
    case ast::NodeKind::IntLit:
    case ast::NodeKind::FloatLit:
        return true;
    case ast::NodeKind::ParenExpr:
        return is_number_literal_tree(*static_cast<const ast::ParenExpr&>(e).inner);
    case ast::NodeKind::UnaryExpr: {
        const auto& u = static_cast<const ast::UnaryExpr&>(e);
        return u.op == ast::UnaryOp::Neg && u.operand != nullptr
               && is_number_literal_tree(*u.operand);
    }
    case ast::NodeKind::BinaryExpr: {
        const auto& bx = static_cast<const ast::BinaryExpr&>(e);
        switch (bx.op) {
        case ast::BinaryOp::Add:
        case ast::BinaryOp::Sub:
        case ast::BinaryOp::Mul:
        case ast::BinaryOp::Div:
        case ast::BinaryOp::Mod:
            return is_number_literal_tree(*bx.lhs) && is_number_literal_tree(*bx.rhs);
        default:
            return false;
        }
    }
    default:
        return false;
    }
}

TypePtr Resolver::check_binary(const ast::BinaryExpr& b, TypePtr expected) {
    // §9 `lhs ?? rhs` — the result type is T, so the surrounding `expected`
    // (when present) is T. Propagate T as the lhs's expected-Optional<T>
    // and as the rhs's expected-T so literal defaulting picks the right
    // primitive on both sides.
    if (b.op == ast::BinaryOp::Coalesce) {
        TypePtr lhs_expected = expected != nullptr ? types_->make_optional(expected) : nullptr;
        auto lhs = check_expr(*b.lhs, lhs_expected);
        TypePtr rhs_expected =
            (lhs != nullptr && lhs->kind() == TypeKind::Optional) ? lhs->inner() : expected;
        auto rhs = check_expr(*b.rhs, rhs_expected);
        if (lhs == nullptr || rhs == nullptr || lhs->is_error() || rhs->is_error()) {
            return types_->error();
        }
        if (lhs->kind() != TypeKind::Optional) {
            error_at(
                b.lhs->range,
                std::format("'?\\?' requires an Optional left operand, got {}", lhs->describe()));
            return types_->error();
        }
        if (!TypeArena::assignable(rhs, lhs->inner())) {
            error_at(b.rhs->range,
                     std::format("'?\\?' right operand of type {} does not match Optional<{}>",
                                 rhs->describe(),
                                 lhs->inner() ? lhs->inner()->describe() : "?"));
            return types_->error();
        }
        return lhs->inner();
    }

    // For arithmetic/bitwise/comparison, both operands ought to share a
    // single numeric type. Propagate `expected` to the lhs first; then use
    // the lhs's concrete type as the expected type for the rhs (so a literal
    // on either side adapts).
    // With nothing expected, a left operand made only of number literals
    // (`48 + v % 10`, `1 + 2 * v`) takes the right operand's numeric type,
    // as a literal on the right already takes the left's.
    TypePtr lhs;
    TypePtr rhs;
    if (expected == nullptr && is_number_literal_tree(*b.lhs)
        && !is_number_literal_tree(*b.rhs)) {
        rhs = check_expr(*b.rhs, nullptr);
        const bool numeric = rhs != nullptr && (rhs->is_integer() || rhs->is_float());
        lhs = check_expr(*b.lhs, numeric ? rhs : nullptr);
    } else {
        lhs = check_expr(*b.lhs, expected);
        rhs = check_expr(*b.rhs, lhs);
    }
    if (lhs == nullptr || rhs == nullptr) {
        return types_->error();
    }
    if (lhs->is_error() || rhs->is_error()) {
        return types_->error();
    }

    // §11 Duration arithmetic (Swift-like). `Duration +/- Duration -> Duration`;
    // `Duration / Duration -> Float64` (a dimensionless ratio); comparisons ->
    // Bool; and scalar scaling `Duration * Int` / `Int * Duration` /
    // `Duration / Int -> Duration`. The C++ `__vstr::Duration` overloads all of
    // these, so codegen still emits a straight `a <op> b` — only the result
    // type is pinned here.
    if (lhs->kind() == TypeKind::Duration || rhs->kind() == TypeKind::Duration) {
        const bool l_dur = lhs->kind() == TypeKind::Duration;
        const bool r_dur = rhs->kind() == TypeKind::Duration;
        const bool both = l_dur && r_dur;
        switch (b.op) {
        case ast::BinaryOp::Add:
        case ast::BinaryOp::Sub:
            if (!both) {
                error_at(b.range,
                         std::format("Duration {} requires both operands to be Duration, "
                                     "got {} and {}",
                                     b.op == ast::BinaryOp::Add ? "+" : "-",
                                     lhs->describe(),
                                     rhs->describe()));
                return types_->error();
            }
            return types_->primitive(TypeKind::Duration);
        case ast::BinaryOp::Mul: {
            // Duration * (Int|Float) or (Int|Float) * Duration — scaling. Never
            // Duration * Duration.
            if (both) {
                error_at(b.range, "cannot multiply two Durations");
                return types_->error();
            }
            TypePtr scalar = l_dur ? rhs : lhs;
            if (scalar == nullptr || !(scalar->is_integer() || scalar->is_float())) {
                error_at(b.range,
                         std::format("a Duration may only be scaled by an integer or "
                                     "floating-point factor, got {}",
                                     scalar != nullptr ? scalar->describe() : "?"));
                return types_->error();
            }
            return types_->primitive(TypeKind::Duration);
        }
        case ast::BinaryOp::Div:
            // Duration / Duration is a ratio; Duration / (Int|Float) scales.
            if (both) {
                return types_->primitive(TypeKind::Float64);
            }
            if (!l_dur) {
                error_at(b.range, std::format("cannot divide {} by a Duration", lhs->describe()));
                return types_->error();
            }
            if (!(rhs->is_integer() || rhs->is_float())) {
                error_at(b.range,
                         std::format("a Duration may only be divided by a Duration (ratio) or "
                                     "an integer/floating-point factor (scaling), got {}",
                                     rhs->describe()));
                return types_->error();
            }
            return types_->primitive(TypeKind::Duration);
        case ast::BinaryOp::Eq:
        case ast::BinaryOp::Ne:
        case ast::BinaryOp::Lt:
        case ast::BinaryOp::Le:
        case ast::BinaryOp::Gt:
        case ast::BinaryOp::Ge:
            if (!both) {
                error_at(b.range,
                         std::format("Duration comparison requires both operands to be "
                                     "Duration, got {} and {}",
                                     lhs->describe(),
                                     rhs->describe()));
                return types_->error();
            }
            return types_->boolean();
        default:
            error_at(b.range, "operator not supported on Duration (only + - * / and comparisons)");
            return types_->error();
        }
    }

    // §13 elementwise vector arithmetic: `[N]T <op> [N]T` for + - * / applies
    // the operator lanewise and yields the same `[N]T`, where T is numeric and
    // both lengths match. Lowered to an auto-vectorizable elementwise helper (a
    // clean scalar fallback the C++ compiler maps to real SIMD where it pays).
    if (lhs->kind() == TypeKind::Vector || rhs->kind() == TypeKind::Vector) {
        const bool elementwise = b.op == ast::BinaryOp::Add || b.op == ast::BinaryOp::Sub
                                 || b.op == ast::BinaryOp::Mul || b.op == ast::BinaryOp::Div;
        if (!elementwise) {
            error_at(b.range, "a fixed-length vector supports only elementwise + - * /");
            return types_->error();
        }
        if (lhs->kind() != TypeKind::Vector || rhs->kind() != TypeKind::Vector
            || !TypeArena::equal(lhs, rhs)) {
            error_at(b.range,
                     std::format("elementwise vector arithmetic requires two vectors of the "
                                 "same type, got {} and {}",
                                 lhs->describe(),
                                 rhs->describe()));
            return types_->error();
        }
        if (lhs->inner() == nullptr || !lhs->inner()->is_numeric()) {
            error_at(b.range,
                     std::format("elementwise vector arithmetic requires a numeric element "
                                 "type, got {}",
                                 lhs->describe()));
            return types_->error();
        }
        return lhs;
    }

    // For arithmetic / bitwise / comparison, the operands must share a
    // numeric type. The §17 natural-width rule (see TypeArena::assignable)
    // applies symmetrically — `Int + Int32` yields Int32 (the sized side
    // wins). Two sized widths that disagree (`Int8 + Int32`) are still an
    // error; only the natural-width side flexes.
    auto require_match = [&] {
        if (TypeArena::equal(lhs, rhs)) {
            return true;
        }
        if (lhs->is_integer() && rhs->is_integer()
            && (TypeArena::assignable(lhs, rhs) || TypeArena::assignable(rhs, lhs))) {
            // Pick the sized operand as the result type. If both are
            // natural-width (Int + Int), assignable returns true both
            // ways and the result stays Int.
            const bool lhs_natural = lhs->kind() == TypeKind::Int || lhs->kind() == TypeKind::UInt;
            if (lhs_natural) {
                lhs = rhs;
            }
            return true;
        }
        error_at(b.range,
                 std::format("binary operator operands of different types: {} vs {}",
                             lhs->describe(),
                             rhs->describe()));
        return false;
    };

    switch (b.op) {
    case ast::BinaryOp::Add:
    case ast::BinaryOp::Sub:
    case ast::BinaryOp::Mul:
    case ast::BinaryOp::Div:
    case ast::BinaryOp::Mod:
    case ast::BinaryOp::WrappingAdd:
    case ast::BinaryOp::WrappingSub:
    case ast::BinaryOp::WrappingMul:
        if (!lhs->is_numeric()) {
            error_at(b.lhs->range,
                     std::format("arithmetic operator requires numeric operands, got {}",
                                 lhs->describe()));
            return types_->error();
        }
        if (!require_match()) {
            return types_->error();
        }
        return lhs;

    case ast::BinaryOp::Shl:
    case ast::BinaryOp::Shr:
    case ast::BinaryOp::BitAnd:
    case ast::BinaryOp::BitOr:
    case ast::BinaryOp::BitXor:
        if (!lhs->is_integer()) {
            error_at(
                b.lhs->range,
                std::format("bitwise operator requires integer operands, got {}", lhs->describe()));
            return types_->error();
        }
        if (!require_match()) {
            return types_->error();
        }
        return lhs;

    case ast::BinaryOp::Eq:
    case ast::BinaryOp::Ne:
    case ast::BinaryOp::Lt:
    case ast::BinaryOp::Le:
    case ast::BinaryOp::Gt:
    case ast::BinaryOp::Ge:
        if (!require_match()) {
            return types_->error();
        }
        // §6, §12.3 an opaque type inherits none of its representation's
        // operations, and a struct compares only by a derive: `==`/`!=`
        // need `derive(Eq)`, the orderings `derive(Ord)`. An enum whose
        // cases carry payloads is ordered only by `derive(Ord)` as well.
        // (Plain enum cases compare without one.)
        if (nominal_cmp_kind(lhs) || (b.op != ast::BinaryOp::Eq && b.op != ast::BinaryOp::Ne
                                      && payload_enum(lhs))) {
            check_nominal_comparison(b, lhs);
        }
        // §7 an operation on a generic parameter is gated by its bounds:
        // `==`/`!=` need Eq, the orderings need Ord (Comparable).
        if (lhs->kind() == TypeKind::GenericParam) {
            const bool equality = b.op == ast::BinaryOp::Eq || b.op == ast::BinaryOp::Ne;
            const std::string_view need = equality ? "Eq" : "Comparable";
            const char* op_text = b.op == ast::BinaryOp::Eq   ? "=="
                                  : b.op == ast::BinaryOp::Ne ? "!="
                                  : b.op == ast::BinaryOp::Lt ? "<"
                                  : b.op == ast::BinaryOp::Le ? "<="
                                  : b.op == ast::BinaryOp::Gt ? ">"
                                                              : ">=";
            if (!generic_param_has_bound(lhs, need)) {
                error_at(b.range,
                         std::format("operator '{}' on {} requires the bound '{}: {}'",
                                     op_text,
                                     lhs->describe(),
                                     lhs->describe(),
                                     need));
            }
        }
        return types_->boolean();

    case ast::BinaryOp::And:
    case ast::BinaryOp::Or:
        if (lhs->kind() != TypeKind::Bool || rhs->kind() != TypeKind::Bool) {
            error_at(b.range, "logical && / || require Bool operands");
            return types_->error();
        }
        return types_->boolean();

    case ast::BinaryOp::Coalesce:
        // `lhs ?? rhs` — lhs should be Optional<T>, rhs should be T.
        if (lhs->kind() != TypeKind::Optional) {
            error_at(
                b.lhs->range,
                std::format("'?\\?' requires an Optional left operand, got {}", lhs->describe()));
            return types_->error();
        }
        if (!TypeArena::assignable(rhs, lhs->inner())) {
            error_at(b.rhs->range,
                     std::format("'?\\?' right operand of type {} does not match Optional<{}>",
                                 rhs->describe(),
                                 lhs->inner() ? lhs->inner()->describe() : "?"));
            return types_->error();
        }
        return lhs->inner();

    case ast::BinaryOp::Range:
    case ast::BinaryOp::RangeLt:
        if (!require_match()) {
            return types_->error();
        }
        // TODO: a real Range[T] nominal type. For now we just pass through.
        return lhs;
    }
    return types_->error();
}

TypePtr Resolver::check_if(const ast::IfExpr& i, TypePtr expected) {
    TypePtr then_type;
    if (!i.let_name.empty()) {
        // §9 `if let NAME = INIT { ... }` — INIT must be Optional<T>; the
        // then-branch is checked in a scope where NAME is bound to T.
        auto init_type = check_expr(*i.let_init);
        TypePtr bound;
        if (init_type == nullptr || init_type->is_error()) {
            bound = types_->error();
        } else if (init_type->kind() != TypeKind::Optional) {
            error_at(i.let_init->range,
                     std::format("'if let' requires an Optional initializer, got {}",
                                 init_type->describe()));
            bound = types_->error();
        } else {
            bound = init_type->inner();
        }
        ScopeStack::Guard g(scopes_);
        Symbol sym;
        sym.name = i.let_name;
        sym.kind = SymbolKind::Local;
        sym.is_mutable = false;
        sym.type = bound;
        sym.definition_range = i.range;
        (void)scopes_.current().insert(std::move(sym));
        resolution_.set_let_symbol(&i, scopes_.current().lookup_local(i.let_name));
        then_type = check_expr(*i.then_branch, expected);
    } else {
        auto cond_type = check_expr(*i.cond, types_->boolean());
        if (cond_type != nullptr && cond_type->kind() != TypeKind::Bool && !cond_type->is_error()) {
            error_at(i.cond->range,
                     std::format("if condition must be Bool, got {}", cond_type->describe()));
        }
        then_type = check_expr(*i.then_branch, expected);
    }
    if (!i.else_branch) {
        return types_->unit();
    }
    auto else_type = check_expr(*i.else_branch, expected != nullptr ? expected : then_type);
    if (then_type == nullptr || else_type == nullptr) {
        return types_->error();
    }
    if (then_type->is_never()) {
        return else_type;
    }
    if (else_type->is_never()) {
        return then_type;
    }
    if (!TypeArena::equal(then_type, else_type)) {
        error_at(i.range,
                 std::format("if branches have different types: {} vs {}",
                             then_type->describe(),
                             else_type->describe()));
        return types_->error();
    }
    return then_type;
}

}  // namespace vestra::sema

namespace vestra::sema {

// §10 a non-escapable value "may not be ... captured by an escaping closure":
// a closure's type is a plain function type, so a view it captured would
// leave with it untracked. A closure handed straight to a `nonescaping`
// parameter, or to a builtin that runs it before returning, may capture one.
void Resolver::check_view_capture(const Symbol& sym, diag::SourceRange at) {
    if (closures_.empty()
        || (sym.kind != SymbolKind::Local && sym.kind != SymbolKind::Parameter)) {
        return;
    }
    TypePtr t = sym.type;
    const bool view = non_escapable(t)
                      || (t != nullptr
                          && ((t->kind() == TypeKind::Vec && non_escapable(t->inner()))
                              || (t->kind() == TypeKind::HashMap
                                  && std::ranges::any_of(t->parts(), [&](TypePtr p) {
                                         return non_escapable(p);
                                     }))));
    if (!view) {
        return;
    }
    const auto& d = sym.definition_range;
    for (const auto* cx : closures_) {
        const auto& r = cx->range;
        const bool own = d.begin.file == r.begin.file && d.begin.offset >= r.begin.offset
                         && d.begin.offset + d.length <= r.begin.offset + r.length;
        if (own || nonesc_ok_.contains(cx)) {
            continue;
        }
        if (view_capture_reported_.emplace(cx, &sym).second) {
            error_at(at,
                     std::format("a closure that can escape captures the view '{}' ({}): pass the "
                                 "closure straight to a `nonescaping` parameter, or capture an "
                                 "owned copy (§10)",
                                 sym.name,
                                 t->describe()));
        }
    }
}

}  // namespace vestra::sema
