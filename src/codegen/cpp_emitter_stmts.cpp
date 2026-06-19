// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// Statement emission: blocks, tuple-pattern destructuring helpers,
// emit_stmt, and statement-position expression lowering.
// Split out of cpp_emitter.cpp; see cpp_emitter_internal.hpp for the
// shared free helpers.

#include "vestra/codegen/cpp_emitter.hpp"

#include "cpp_emitter_internal.hpp"

#include <format>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vestra::codegen {

using detail::write_indent;

void CppEmitter::emit_block(std::ostream& os, const ast::BlockExpr& b, int indent) {
    os << "{\n";
    for (const auto& s : b.stmts) {
        emit_stmt(os, *s, indent + 1);
    }
    write_indent(os, indent);
    os << "}";
}

void CppEmitter::collect_tuple_pat_names(
    const ast::TuplePat& tp,
    std::vector<std::string>& names,
    std::vector<std::pair<std::string, const ast::TuplePat*>>& followons) {
    for (std::size_t i = 0; i < tp.elements.size(); ++i) {
        const auto& sub = tp.elements[i];
        if (sub && sub->kind == ast::NodeKind::IdentPat) {
            names.push_back(static_cast<const ast::IdentPat&>(*sub).name);
        } else if (sub && sub->kind == ast::NodeKind::BindPat) {
            names.push_back(static_cast<const ast::BindPat&>(*sub).name);
        } else if (sub && sub->kind == ast::NodeKind::WildcardPat) {
            names.push_back(std::format("__vstr_tp_unused_{}", tuple_pat_counter_++));
        } else if (sub && sub->kind == ast::NodeKind::TuplePat) {
            // Nested sub-tuple: bind a placeholder at this level, then
            // unpack it in a follow-on statement after the parent
            // structured binding.
            auto placeholder = std::format("__vstr_tp{}", tuple_pat_counter_++);
            names.push_back(placeholder);
            followons.emplace_back(std::move(placeholder),
                                   &static_cast<const ast::TuplePat&>(*sub));
        } else {
            names.push_back(std::format("__vstr_tp_bind_{}", tuple_pat_counter_++));
        }
    }
}

void CppEmitter::emit_tuple_pat_followons(
    std::ostream& os,
    const std::vector<std::pair<std::string, const ast::TuplePat*>>& followons,
    int indent) {
    for (const auto& [placeholder, inner_tp] : followons) {
        std::vector<std::string> names;
        std::vector<std::pair<std::string, const ast::TuplePat*>> nested;
        collect_tuple_pat_names(*inner_tp, names, nested);
        write_indent(os, indent);
        os << "auto [";
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (i != 0) {
                os << ", ";
            }
            os << names[i];
        }
        os << "] = " << placeholder << ";\n";
        emit_tuple_pat_followons(os, nested, indent);
    }
}

void CppEmitter::emit_stmt(std::ostream& os, const ast::Stmt& s, int indent) {
    // §9 try-hoisting: pull every TryExpr-Propagating in this statement's
    // expressions out into a stmt-position let-binding, so propagation
    // really escapes the enclosing throws(E) fn. The active_hoists_
    // pointer lives only for the duration of this call — emit_expr looks
    // it up to substitute the bound name when it later encounters the
    // hoisted TryExpr.
    std::vector<TryHoist> hoists;
    collect_stmt_hoists(s, hoists);
    for (const auto& h : hoists) {
        emit_try_hoist(os, h, indent);
    }
    const auto* prev_hoists = active_hoists_;
    active_hoists_ = &hoists;
    write_indent(os, indent);
    switch (s.kind) {
    case ast::NodeKind::LetStmt: {
        const auto& l = static_cast<const ast::LetStmt&>(s);
        // §6 tuple destructuring: `let (a, b, ...) = expr` lowers to
        // a C++17 structured binding. The annotation is dropped — the
        // element types come from the tuple value's static type.
        // Nested sub-tuples bind a placeholder here and unpack it in
        // a sibling follow-on statement (structured bindings are
        // single-level in C++).
        if (l.pattern && l.pattern->kind == ast::NodeKind::TuplePat) {
            const auto& tp = static_cast<const ast::TuplePat&>(*l.pattern);
            std::vector<std::string> names;
            std::vector<std::pair<std::string, const ast::TuplePat*>> followons;
            collect_tuple_pat_names(tp, names, followons);
            os << "auto [";
            for (std::size_t i = 0; i < names.size(); ++i) {
                if (i != 0) {
                    os << ", ";
                }
                os << names[i];
            }
            os << "] = ";
            if (l.value) {
                emit_expr(os, *l.value);
            }
            os << ";\n";
            emit_tuple_pat_followons(os, followons, indent);
            break;
        }
        // Vestra's `let` is "no reassignment" — but it does allow consumption
        // (move). C++'s `const` is stricter (no rebind AND no move-from), so
        // we use plain `auto` / `T` here. Sema rejects assignment to `let`
        // bindings, which is the part C++'s const was buying us.
        //
        // §9 `let NAME = try EXPR` no longer needs a special case: the
        // try-hoisting pass above pre-emits the propagation escape, then
        // emit_expr substitutes the hoisted name when it hits the
        // TryExpr — so `auto NAME = __vstr_t0;` is correct.
        // The pattern is an IdentPat / BindPat in the common case;
        // we materialize the name up front so `emit_type_with_name`
        // can embed it inside the function-pointer parens. Anything
        // else (refutable patterns, etc.) falls back to `_bind` per
        // the prior behavior.
        std::string bind_name;
        if (l.pattern && l.pattern->kind == ast::NodeKind::IdentPat) {
            bind_name = static_cast<const ast::IdentPat&>(*l.pattern).name;
        } else if (l.pattern && l.pattern->kind == ast::NodeKind::BindPat) {
            bind_name = static_cast<const ast::BindPat&>(*l.pattern).name;
        } else {
            bind_name = "_bind";
        }
        if (l.type) {
            emit_type_with_name(os, *l.type, bind_name);
        } else {
            os << "auto " << bind_name;
        }
        os << " = ";
        if (l.value) {
            emit_expr(os, *l.value);
        }
        os << ";\n";
        break;
    }
    case ast::NodeKind::VarStmt: {
        const auto& v = static_cast<const ast::VarStmt&>(s);
        if (v.pattern && v.pattern->kind == ast::NodeKind::TuplePat) {
            // Same shape as the LetStmt path. `var` becomes a mutable
            // structured binding via `auto [a, b] = ...;` (C++23's
            // mutable [auto&] form would let us bind by ref; for v0.5
            // we always take by-value to match Vestra's `var` move).
            const auto& tp = static_cast<const ast::TuplePat&>(*v.pattern);
            std::vector<std::string> names;
            std::vector<std::pair<std::string, const ast::TuplePat*>> followons;
            collect_tuple_pat_names(tp, names, followons);
            os << "auto [";
            for (std::size_t i = 0; i < names.size(); ++i) {
                if (i != 0) {
                    os << ", ";
                }
                os << names[i];
            }
            os << "] = ";
            if (v.value) {
                emit_expr(os, *v.value);
            }
            os << ";\n";
            emit_tuple_pat_followons(os, followons, indent);
            break;
        }
        if (v.type) {
            emit_type(os, *v.type);
            os << " ";
        } else {
            os << "auto ";
        }
        if (v.pattern && v.pattern->kind == ast::NodeKind::IdentPat) {
            os << static_cast<const ast::IdentPat&>(*v.pattern).name;
        } else if (v.pattern && v.pattern->kind == ast::NodeKind::BindPat) {
            os << static_cast<const ast::BindPat&>(*v.pattern).name;
        } else {
            os << "_bind";
        }
        os << " = ";
        if (v.value) {
            emit_expr(os, *v.value);
        }
        os << ";\n";
        break;
    }
    case ast::NodeKind::ExprStmt:
        emit_stmt_expr(os, *static_cast<const ast::ExprStmt&>(s).expr, /*return_value=*/false);
        break;
    case ast::NodeKind::ReturnStmt: {
        const auto& r = static_cast<const ast::ReturnStmt&>(s);
        if (r.value) {
            emit_stmt_expr(os, *r.value, /*return_value=*/true);
        } else {
            os << (current_func_is_async_ ? "co_return;\n" : "return;\n");
        }
        break;
    }
    case ast::NodeKind::BreakStmt:
        os << "break;\n";
        break;
    case ast::NodeKind::ContinueStmt:
        os << "continue;\n";
        break;
    case ast::NodeKind::AssignStmt: {
        const auto& a = static_cast<const ast::AssignStmt&>(s);
        emit_expr(os, *a.target);
        switch (a.op) {
        case ast::AssignOp::Assign:
            os << " = ";
            break;
        case ast::AssignOp::AddAssign:
            os << " += ";
            break;
        case ast::AssignOp::SubAssign:
            os << " -= ";
            break;
        case ast::AssignOp::MulAssign:
            os << " *= ";
            break;
        case ast::AssignOp::DivAssign:
            os << " /= ";
            break;
        case ast::AssignOp::ModAssign:
            os << " %= ";
            break;
        case ast::AssignOp::BitAndAssign:
            os << " &= ";
            break;
        case ast::AssignOp::BitOrAssign:
            os << " |= ";
            break;
        case ast::AssignOp::BitXorAssign:
            os << " ^= ";
            break;
        case ast::AssignOp::ShlAssign:
            os << " <<= ";
            break;
        case ast::AssignOp::ShrAssign:
            os << " >>= ";
            break;
        }
        emit_expr(os, *a.value);
        os << ";\n";
        break;
    }
    case ast::NodeKind::WhileStmt: {
        const auto& w = static_cast<const ast::WhileStmt&>(s);
        os << "while (";
        emit_expr(os, *w.cond);
        os << ") ";
        if (w.body && w.body->kind == ast::NodeKind::BlockExpr) {
            emit_block(os, static_cast<const ast::BlockExpr&>(*w.body), indent);
            os << "\n";
        } else {
            os << "{}\n";
        }
        break;
    }
    case ast::NodeKind::ForStmt: {
        // §5 desugaring: two shapes.
        //   * Range / RangeLt: lower to a C++ counted for-loop, with
        //     the end expression captured once in a sibling decl.
        //   * Iterator protocol: lower to a while-true that calls
        //     __vstr_iter.next() and breaks on .none; the loop body
        //     binds the unwrapped value as `auto x`.
        const auto& f = static_cast<const ast::ForStmt&>(s);
        auto bind_name = [&]() -> std::string {
            if (f.pattern && f.pattern->kind == ast::NodeKind::IdentPat) {
                return static_cast<const ast::IdentPat&>(*f.pattern).name;
            }
            if (f.pattern && f.pattern->kind == ast::NodeKind::BindPat) {
                return static_cast<const ast::BindPat&>(*f.pattern).name;
            }
            return "_it";
        };
        const ast::BinaryExpr* range = nullptr;
        if (f.iter && f.iter->kind == ast::NodeKind::BinaryExpr) {
            const auto& b = static_cast<const ast::BinaryExpr&>(*f.iter);
            if (b.op == ast::BinaryOp::Range || b.op == ast::BinaryOp::RangeLt) {
                range = &b;
            }
        }
        if (range != nullptr) {
            const char* cmp = range->op == ast::BinaryOp::RangeLt ? "<" : "<=";
            os << "for (auto " << bind_name() << " = ";
            emit_expr(os, *range->lhs);
            os << ", __vstr_end = ";
            emit_expr(os, *range->rhs);
            os << "; " << bind_name() << " " << cmp << " __vstr_end; ++" << bind_name() << ") ";
            if (f.body && f.body->kind == ast::NodeKind::BlockExpr) {
                emit_block(os, static_cast<const ast::BlockExpr&>(*f.body), indent);
                os << "\n";
            } else {
                os << "{}\n";
            }
            break;
        }
        // §18.5 / §10 / §13 `for x in xs` over a Vec[T], a Span[T]/MutSpan[T]
        // (incl. a Soa column view), lowers to a C++ range-based for. `auto&&`
        // binds each element without copying (free for struct elements); the
        // loop variable is immutable on the Vestra side, so the reference is
        // read-only in practice.
        if (resolution_ != nullptr && f.iter) {
            if (auto it = resolution_->type_of(f.iter.get());
                it != nullptr
                && (it->kind() == sema::TypeKind::Vec || it->kind() == sema::TypeKind::Span
                    || it->kind() == sema::TypeKind::MutSpan)) {
                // §18.5 tuple-pattern binding: `for (k, v) in m.entries()` over a
                // Vec[(K, V)] lowers to a C++ structured binding in the range-for
                // header. Nested sub-tuples bind a placeholder and unpack in
                // sibling follow-on statements (same machinery as let / the
                // iterator-protocol path).
                if (f.pattern && f.pattern->kind == ast::NodeKind::TuplePat) {
                    const auto& tp = static_cast<const ast::TuplePat&>(*f.pattern);
                    std::vector<std::string> names;
                    std::vector<std::pair<std::string, const ast::TuplePat*>> followons;
                    collect_tuple_pat_names(tp, names, followons);
                    os << "for (auto&& [";
                    for (std::size_t i = 0; i < names.size(); ++i) {
                        if (i != 0) {
                            os << ", ";
                        }
                        os << names[i];
                    }
                    os << "] : ";
                    emit_expr(os, *f.iter);
                    os << ") ";
                    if (followons.empty()) {
                        if (f.body && f.body->kind == ast::NodeKind::BlockExpr) {
                            emit_block(os, static_cast<const ast::BlockExpr&>(*f.body), indent);
                            os << "\n";
                        } else {
                            os << "{}\n";
                        }
                    } else {
                        os << "{ ";
                        emit_tuple_pat_followons(os, followons, indent + 1);
                        if (f.body && f.body->kind == ast::NodeKind::BlockExpr) {
                            emit_block(os, static_cast<const ast::BlockExpr&>(*f.body), indent);
                        }
                        os << " }\n";
                    }
                    break;
                }
                os << "for (auto&& " << bind_name() << " : ";
                emit_expr(os, *f.iter);
                os << ") ";
                if (f.body && f.body->kind == ast::NodeKind::BlockExpr) {
                    emit_block(os, static_cast<const ast::BlockExpr&>(*f.body), indent);
                    os << "\n";
                } else {
                    os << "{}\n";
                }
                break;
            }
        }
        // Iterator protocol: capture iter as a mutable local, drive
        // while-true / .next() / break on nullopt.
        //
        // §6 TuplePat support: when the loop variable is a tuple
        // pattern (e.g., `for (a, b) in zip(...)`), bind via C++
        // structured binding over `*__vstr_o`. Sub-patterns reuse the
        // collect_tuple_pat_names / emit_tuple_pat_followons machinery
        // that param-level tuple destructuring uses, so nested tuple
        // patterns lower the same way.
        os << "{ auto __vstr_iter = ";
        emit_expr(os, *f.iter);
        os << "; while (true) { auto __vstr_o = __vstr_iter.next(); "
              "if (!__vstr_o.has_value()) { break; } ";
        if (f.pattern && f.pattern->kind == ast::NodeKind::TuplePat) {
            const auto& tp = static_cast<const ast::TuplePat&>(*f.pattern);
            std::vector<std::string> names;
            std::vector<std::pair<std::string, const ast::TuplePat*>> followons;
            collect_tuple_pat_names(tp, names, followons);
            os << "auto [";
            for (std::size_t i = 0; i < names.size(); ++i) {
                if (i != 0) {
                    os << ", ";
                }
                os << names[i];
            }
            os << "] = std::move(*__vstr_o); ";
            emit_tuple_pat_followons(os, followons, indent + 1);
        } else {
            os << "auto " << bind_name() << " = std::move(*__vstr_o); ";
        }
        if (f.body && f.body->kind == ast::NodeKind::BlockExpr) {
            emit_block(os, static_cast<const ast::BlockExpr&>(*f.body), indent);
        } else {
            os << "{}";
        }
        os << " } }\n";
        break;
    }
    case ast::NodeKind::WithStmt: {
        // §17.4 `with` statement: open a fresh C++ block, declare any
        // named bindings (`with mmio = createRegion() { ... }`) at
        // their value's deduced type, then emit the body's statements
        // at one-deeper indent. Cap-typed bindings (`with Alloc { ... }`)
        // contribute no identifier but still take the sub-scope so
        // their lifetime story is uniform.
        const auto& w = static_cast<const ast::WithStmt&>(s);
        os << "{\n";
        for (const auto& b : w.bindings) {
            if (!b.name.empty() && b.value) {
                write_indent(os, indent + 1);
                // §17.4 type annotation on a name-binding: emit the
                // declared type instead of `auto&&` so the user's
                // intent shows up at the C++ layer. The function-
                // pointer declarator quirk (where the name embeds
                // inside the parens) is handled by
                // emit_type_with_name.
                if (b.type_annotation) {
                    emit_type_with_name(os, *b.type_annotation, b.name);
                    os << " = ";
                } else {
                    os << "auto&& " << b.name << " = ";
                }
                emit_expr(os, *b.value);
                os << ";\n";
            }
            // Cap-typed bindings (TYPE = EXPR shape with no name) are
            // discharged by the capability checker; the value runs but
            // doesn't introduce an identifier. Discard via void-cast
            // so it doesn't trigger -Wunused-result on a [[nodiscard]]
            // expression.
            else if (b.name.empty() && b.value) {
                write_indent(os, indent + 1);
                os << "(void)(";
                emit_expr(os, *b.value);
                os << ");\n";
            }
        }
        if (w.body && w.body->kind == ast::NodeKind::BlockExpr) {
            const auto& blk = static_cast<const ast::BlockExpr&>(*w.body);
            for (const auto& stmt : blk.stmts) {
                emit_stmt(os, *stmt, indent + 1);
            }
        }
        write_indent(os, indent);
        os << "}\n";
        break;
    }
    default:
        unsupported(os, "stmt", s.range);
        os << "\n";
        break;
    }
    active_hoists_ = prev_hoists;
}

void CppEmitter::emit_stmt_expr(std::ostream& os, const ast::Expr& expr, bool return_value) {
    // §9 + §17.7 statement-position lowering. The kinds we special-case
    // are the ones whose value semantics fight a generic `expr;` /
    // `return expr;` lowering:
    //   * ThrowExpr — needs a real `return std::unexpected{...};`.
    //   * TryExpr (Propagating) — needs the canonical 3-line escape.
    //   * IfExpr — emit if/else statements so each branch's body can
    //     itself contain a throw / propagating-try; also avoids the IIFE
    //     return-type-deduction trap when branches have differing types.
    //   * BlockExpr — flatten its statements; recurse on the trailing
    //     expression so a `throw` at the end of a block fires correctly.
    // Everything else falls back to plain `expr;` / `return expr;`.
    switch (expr.kind) {
    case ast::NodeKind::ThrowExpr: {
        // §11 — inside an `async func` the body is a coroutine, so the error
        // exit must be `co_return std::unexpected{e}` (a plain `return` is
        // ill-formed in a coroutine). A non-async throws fn uses `return`.
        const auto& th = static_cast<const ast::ThrowExpr&>(expr);
        os << (current_func_is_async_ ? "co_return std::unexpected{" : "return std::unexpected{");
        emit_expr(os, *th.inner);
        os << "};\n";
        return;
    }
    case ast::NodeKind::TryExpr: {
        const auto& tx = static_cast<const ast::TryExpr&>(expr);
        if (tx.form == ast::TryExpr::Form::Propagating) {
            // If hoisting registered a binding for this try, emit_expr
            // would substitute the name; just use the regular return /
            // void-cast path.
            if (lookup_try_hoist(&tx) != nullptr) {
                break;  // fall through to generic `<expr>;` / `return <expr>;`
            }
            // Both exits are function-level returns, so they co_return inside
            // an async throws coroutine (and `return` otherwise).
            const char* ret = current_func_is_async_ ? "co_return " : "return ";
            os << "{ auto __vstr_r = ";
            emit_expr(os, *tx.inner);
            os << "; if (!__vstr_r.has_value()) { " << ret
               << "std::unexpected{__vstr_r.error()}; } ";
            if (return_value) {
                os << ret << "*__vstr_r;";
            } else {
                os << "(void)*__vstr_r;";
            }
            os << " }\n";
            return;
        }
        break;  // fall through for try? / try!
    }
    case ast::NodeKind::IfExpr: {
        const auto& i = static_cast<const ast::IfExpr&>(expr);
        // If this IfExpr is the target of a conditional hoist, the
        // hoist pre-emitted its IIFE-returning-expected and bound the
        // result. Fall through to the generic tail; emit_expr will
        // substitute `*<name>` for the IfExpr.
        if (lookup_try_hoist(&expr) != nullptr) {
            break;
        }
        if (!i.let_name.empty()) {
            os << "if (auto __vstr_opt = ";
            emit_expr(os, *i.let_init);
            os << "; __vstr_opt.has_value()) { auto&& " << i.let_name << " = *__vstr_opt; ";
            emit_stmt_expr(os, *i.then_branch, return_value);
            os << "}";
        } else {
            os << "if (";
            emit_expr(os, *i.cond);
            os << ") { ";
            emit_stmt_expr(os, *i.then_branch, return_value);
            os << "}";
        }
        if (i.else_branch) {
            os << " else { ";
            emit_stmt_expr(os, *i.else_branch, return_value);
            os << "}";
        }
        os << "\n";
        return;
    }
    case ast::NodeKind::BlockExpr: {
        const auto& b = static_cast<const ast::BlockExpr&>(expr);
        for (std::size_t i = 0; i < b.stmts.size(); ++i) {
            const auto& s = *b.stmts[i];
            bool last = (i + 1 == b.stmts.size());
            if (last && s.kind == ast::NodeKind::ExprStmt) {
                emit_stmt_expr(os, *static_cast<const ast::ExprStmt&>(s).expr, return_value);
            } else {
                emit_stmt(os, s, 0);
            }
        }
        return;
    }
    default:
        break;
    }
    // §9 branch-local hoists: at the trailing-expression position of a
    // statement-form emission (e.g. inside an IfExpr branch's `{ ... }`),
    // the parent statement's collect_try_hoists didn't descend into
    // conditional branches — so any propagating tries here aren't yet
    // in active_hoists_. Collect them locally, pre-emit at this brace
    // scope, and extend active_hoists_ so emit_expr substitutes the
    // names. Exclusion against active_hoists_ avoids duplicating
    // already-emitted hoists.
    std::vector<TryHoist> local_hoists;
    collect_try_hoists(expr, local_hoists, active_hoists_);
    std::vector<TryHoist> combined;
    const auto* prev_active = active_hoists_;
    if (!local_hoists.empty()) {
        for (const auto& h : local_hoists) {
            emit_try_hoist(os, h, 0);
        }
        if (prev_active != nullptr) {
            combined = *prev_active;
        }
        for (const auto& h : local_hoists) {
            combined.push_back(h);
        }
        active_hoists_ = &combined;
    }
    if (return_value) {
        // §11 — inside an async function this statement-position return is
        // a coroutine return. (Value-context IIFE lambdas in emit_expr keep
        // their plain `return`; only the function-level return changes.)
        os << (current_func_is_async_ ? "co_return " : "return ");
    }
    emit_expr(os, expr);
    os << ";\n";
    if (!local_hoists.empty()) {
        active_hoists_ = prev_active;
    }
}

}  // namespace vestra::codegen
