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

#include <array>
#include <cstdlib>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vestra::sema {

using detail::named_type_param_count;

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
        const auto* sym = scopes_.current().lookup(ident.name);
        if (sym == nullptr) {
            error_at(e.range, std::format("undefined name '{}'", ident.name));
            t = types_->error();
        } else {
            resolution_.set_symbol(&e, sym);
            check_visibility(*sym, e.range);
            t = sym->type != nullptr ? sym->type : types_->error();
        }
        break;
    }
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
                sym.type = types_->error();
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
        ScopeStack::Guard g(scopes_);
        for (std::size_t i = 0; i < cx.params.size(); ++i) {
            Symbol sym;
            sym.name = cx.params[i];
            sym.kind = SymbolKind::Local;
            sym.type = expected->parts()[i] != nullptr ? expected->parts()[i] : types_->error();
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
        t = types_->make_function(expected->parts(), body_type);
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
        // §12.4 v0.5 — an expression-context quote types as its body
        // expression (with splices resolved against the surrounding
        // scope). The body is checked with quote depth raised so a `$`
        // splice inside is admitted.
        const auto& q = static_cast<const ast::QuoteExpr&>(e);
        ++quote_depth_;
        t = q.inner ? check_expr(*q.inner, expected) : types_->error();
        --quote_depth_;
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
    case ast::NodeKind::AwaitExpr: {
        // §11 `await e` consumes its operand for the underlying value: a
        // `Future[T]` (from `spawn`) unwraps to T; a direct async call is
        // already typed T (async results aren't wrapped), so await is
        // transparent there.
        auto inner = check_expr(*static_cast<const ast::AwaitExpr&>(e).inner);
        t = (inner != nullptr && inner->kind() == TypeKind::Future) ? inner->inner() : inner;
        break;
    }
    case ast::NodeKind::SpawnExpr:
        // §11 `spawn f(args)` runs the call and yields a `Future[T]` where
        // T is the call's result type.
        t = types_->make_future(check_expr(*static_cast<const ast::SpawnExpr&>(e).inner));
        break;
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
        (void)check_expr(*a.value);
        t = a.target ? resolve_type(*a.target) : types_->error();
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
        // capability requirement (§4) is still a follow-on; the runtime
        // lowering uses std::format which assumes a default heap.
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

TypePtr Resolver::check_unary(const ast::UnaryExpr& u, TypePtr expected) {
    // Negation / bit-not preserve the operand's type, so the expected type
    // can be propagated inward.
    auto operand = check_expr(*u.operand, expected);
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
    auto lhs = check_expr(*b.lhs, expected);
    auto rhs = check_expr(*b.rhs, lhs);
    if (lhs == nullptr || rhs == nullptr) {
        return types_->error();
    }
    if (lhs->is_error() || rhs->is_error()) {
        return types_->error();
    }

    // §11 Duration arithmetic (Swift-like). `Duration +/- Duration -> Duration`;
    // `Duration / Duration -> Float64` (a dimensionless ratio); comparisons ->
    // Bool. The C++ `__vstr::Duration` overloads these operators, so codegen
    // still emits a straight `a <op> b` — only the result type is pinned here.
    // (Scalar scaling `Duration * Int` is a carry-forward.)
    if (lhs->kind() == TypeKind::Duration || rhs->kind() == TypeKind::Duration) {
        const bool both = lhs->kind() == TypeKind::Duration && rhs->kind() == TypeKind::Duration;
        if (!both) {
            error_at(b.range,
                     std::format("Duration operator requires both operands to be Duration, "
                                 "got {} and {}",
                                 lhs->describe(),
                                 rhs->describe()));
            return types_->error();
        }
        switch (b.op) {
        case ast::BinaryOp::Add:
        case ast::BinaryOp::Sub:
            return types_->primitive(TypeKind::Duration);
        case ast::BinaryOp::Div:
            return types_->primitive(TypeKind::Float64);
        case ast::BinaryOp::Eq:
        case ast::BinaryOp::Ne:
        case ast::BinaryOp::Lt:
        case ast::BinaryOp::Le:
        case ast::BinaryOp::Gt:
        case ast::BinaryOp::Ge:
            return types_->boolean();
        default:
            error_at(b.range, "operator not supported on Duration (only + - / and comparisons)");
            return types_->error();
        }
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
        sym.type = bound;
        sym.definition_range = i.range;
        (void)scopes_.current().insert(std::move(sym));
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
