// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// Error-propagation lowering: the try-hoisting machinery (collect/emit
// hoists, cond-hoist IIFEs, match/do-catch-in-lambda) and the
// propagating-try predicates.
// Split out of cpp_emitter.cpp; see cpp_emitter_internal.hpp for the
// shared free helpers.

#include "vestra/codegen/cpp_emitter.hpp"

#include "cpp_emitter_internal.hpp"

#include <algorithm>
#include <format>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vestra::codegen {

using detail::enum_is_sum_type;
using detail::write_indent;

void CppEmitter::collect_try_hoists(const ast::Expr& e,
                                    std::vector<TryHoist>& out,
                                    const std::vector<TryHoist>* exclude) {
    auto already_hoisted = [&](const ast::Expr* node) {
        if (exclude == nullptr) {
            return false;
        }
        for (const auto& h : *exclude) {
            if (h.node == node) {
                return true;
            }
        }
        return false;
    };
    switch (e.kind) {
    case ast::NodeKind::TryExpr: {
        const auto& tx = static_cast<const ast::TryExpr&>(e);
        if (tx.form == ast::TryExpr::Form::Propagating) {
            // Post-order: inner hoists go first so they're emitted before
            // the outer one consumes their values.
            collect_try_hoists(*tx.inner, out, exclude);
            if (!already_hoisted(&tx)) {
                out.push_back({&tx, std::format("__vstr_t{}", hoist_counter_++)});
            }
        }
        // try? / try! don't propagate, so they don't need a hoist.
        break;
    }
    case ast::NodeKind::CallExpr: {
        const auto& c = static_cast<const ast::CallExpr&>(e);
        collect_try_hoists(*c.callee, out, exclude);
        for (const auto& a : c.args) {
            if (a.value) {
                collect_try_hoists(*a.value, out, exclude);
            }
        }
        break;
    }
    case ast::NodeKind::BinaryExpr: {
        const auto& b = static_cast<const ast::BinaryExpr&>(e);
        collect_try_hoists(*b.lhs, out, exclude);
        // Skip RHS of short-circuit logicals — it might not evaluate.
        if (b.op == ast::BinaryOp::And || b.op == ast::BinaryOp::Or) {
            break;
        }
        collect_try_hoists(*b.rhs, out, exclude);
        break;
    }
    case ast::NodeKind::UnaryExpr:
        collect_try_hoists(*static_cast<const ast::UnaryExpr&>(e).operand, out, exclude);
        break;
    case ast::NodeKind::MemberExpr:
        collect_try_hoists(*static_cast<const ast::MemberExpr&>(e).base, out, exclude);
        break;
    case ast::NodeKind::IndexExpr: {
        const auto& ix = static_cast<const ast::IndexExpr&>(e);
        collect_try_hoists(*ix.base, out, exclude);
        for (const auto& idx : ix.indices) {
            if (idx) {
                collect_try_hoists(*idx, out, exclude);
            }
        }
        break;
    }
    case ast::NodeKind::ParenExpr:
        collect_try_hoists(*static_cast<const ast::ParenExpr&>(e).inner, out, exclude);
        break;
    case ast::NodeKind::AsExpr:
        collect_try_hoists(*static_cast<const ast::AsExpr&>(e).value, out, exclude);
        break;
    case ast::NodeKind::IfExpr: {
        const auto& i = static_cast<const ast::IfExpr&>(e);
        // Walk the unconditionally-evaluated bits: the cond and the
        // let-init. Branches stay un-walked here — each branch's tries
        // would otherwise evaluate eagerly.
        if (i.cond) {
            collect_try_hoists(*i.cond, out, exclude);
        }
        if (i.let_init) {
            collect_try_hoists(*i.let_init, out, exclude);
        }
        // §9 mid-expression `try` inside a branch: when a propagating
        // try sits in either branch (or deeper), hoist the IfExpr
        // itself. emit_cond_hoist lowers it as a lambda returning
        // std::expected<T, E>; the parent scope unwraps via the same
        // canonical 3-line escape used for TryExpr-hoists.
        const bool branches_have_try =
            (i.then_branch != nullptr && expr_contains_propagating_try(*i.then_branch))
            || (i.else_branch != nullptr && expr_contains_propagating_try(*i.else_branch));
        if (branches_have_try && !already_hoisted(&e)) {
            out.push_back({&e, std::format("__vstr_c{}", hoist_counter_++)});
        }
        break;
    }
    case ast::NodeKind::MatchExpr: {
        const auto& m = static_cast<const ast::MatchExpr&>(e);
        // Scrutinee is always evaluated; arm bodies aren't. Walk the
        // scrutinee unconditionally for inner tries, then — if any arm
        // body carries a propagating try — register the whole MatchExpr
        // as a conditional hoist (parallel to the IfExpr case above).
        // emit_cond_hoist lowers the match as a statement-form lambda
        // returning std::expected<T, E>; the outer scope unwraps via
        // the canonical 3-line escape, same as for IfExpr.
        if (m.scrutinee) {
            collect_try_hoists(*m.scrutinee, out, exclude);
        }
        bool arms_have_try = false;
        for (const auto& arm : m.arms) {
            if (arm.guard && expr_contains_propagating_try(*arm.guard)) {
                arms_have_try = true;
                break;
            }
            if (arm.body && expr_contains_propagating_try(*arm.body)) {
                arms_have_try = true;
                break;
            }
        }
        if (arms_have_try && !already_hoisted(&e)) {
            out.push_back({&e, std::format("__vstr_c{}", hoist_counter_++)});
        }
        break;
    }
    case ast::NodeKind::DoCatchExpr: {
        // §9 `do { … } catch (e: E) where guard { … }`: failed guard
        // re-throws E to the enclosing throws context. Register the
        // whole DoCatchExpr as a cond-hoist when a guard is present;
        // emit_cond_hoist will lift it to an IIFE returning
        // std::expected<T, E>, and the outer scope unwraps via the
        // canonical 3-line escape.
        //
        // The do-body's and catch-body's inner tries are handled by
        // the do-catch's own emit machinery (the inner lambda already
        // collects its own hoists scoped to its return-type
        // expected<T, E>), so we deliberately do *not* walk into them
        // here — that'd double-hoist.
        //
        // Do-catches without a guard keep their existing T-returning
        // shape: they're total over the error path, no propagation
        // needed.
        const auto& dc = static_cast<const ast::DoCatchExpr&>(e);
        if (dc.guard != nullptr && !already_hoisted(&e)) {
            out.push_back({&e, std::format("__vstr_c{}", hoist_counter_++)});
        }
        break;
    }
    case ast::NodeKind::CopyExpr:
        collect_try_hoists(*static_cast<const ast::CopyExpr&>(e).inner, out, exclude);
        break;
    case ast::NodeKind::VectorLitExpr: {
        const auto& v = static_cast<const ast::VectorLitExpr&>(e);
        for (const auto& el : v.elements) {
            if (el) {
                collect_try_hoists(*el, out, exclude);
            }
        }
        break;
    }
    case ast::NodeKind::TupleLitExpr: {
        const auto& tup = static_cast<const ast::TupleLitExpr&>(e);
        for (const auto& el : tup.elements) {
            if (el) {
                collect_try_hoists(*el, out, exclude);
            }
        }
        break;
    }
    case ast::NodeKind::InterpStringExpr: {
        const auto& is_ = static_cast<const ast::InterpStringExpr&>(e);
        for (const auto& seg : is_.segments) {
            if (seg.expr) {
                collect_try_hoists(*seg.expr, out, exclude);
            }
        }
        break;
    }
    // BlockExpr, ClosureExpr, AwaitExpr, SpawnExpr, ThrowExpr — leave
    // alone. BlockExpr has its own statement scope (each inner stmt does
    // its own hoist collection); ThrowExpr's inner is the error value,
    // which doesn't carry try semantics here; the concurrency forms have
    // their own escape semantics and aren't hoisted in v0.5.
    default:
        break;
    }
}

void CppEmitter::collect_stmt_hoists(const ast::Stmt& s, std::vector<TryHoist>& out) {
    switch (s.kind) {
    case ast::NodeKind::LetStmt:
        if (const auto& l = static_cast<const ast::LetStmt&>(s); l.value) {
            collect_try_hoists(*l.value, out);
        }
        break;
    case ast::NodeKind::VarStmt:
        if (const auto& v = static_cast<const ast::VarStmt&>(s); v.value) {
            collect_try_hoists(*v.value, out);
        }
        break;
    case ast::NodeKind::ExprStmt:
        collect_try_hoists(*static_cast<const ast::ExprStmt&>(s).expr, out);
        break;
    case ast::NodeKind::ReturnStmt:
        if (const auto& r = static_cast<const ast::ReturnStmt&>(s); r.value) {
            collect_try_hoists(*r.value, out);
        }
        break;
    case ast::NodeKind::AssignStmt: {
        const auto& a = static_cast<const ast::AssignStmt&>(s);
        collect_try_hoists(*a.target, out);
        collect_try_hoists(*a.value, out);
        break;
    }
    case ast::NodeKind::WhileStmt:
        collect_try_hoists(*static_cast<const ast::WhileStmt&>(s).cond, out);
        break;
    case ast::NodeKind::ForStmt:
        if (const auto& fs = static_cast<const ast::ForStmt&>(s); fs.iter) {
            collect_try_hoists(*fs.iter, out);
        }
        break;
    default:
        break;
    }
}

void CppEmitter::emit_try_hoist(std::ostream& os, const TryHoist& h, int indent) {
    // §9 dispatch by hoisted node kind. TryExpr → canonical 3-line
    // escape; IfExpr → IIFE returning std::expected (a "conditional
    // hoist") via emit_cond_hoist.
    if (h.node->kind == ast::NodeKind::TryExpr) {
        const auto& tx = static_cast<const ast::TryExpr&>(*h.node);
        const std::string r = h.name + "_r";
        write_indent(os, indent);
        os << "auto " << r << " = ";
        emit_expr(os, *tx.inner);
        os << ";\n";
        write_indent(os, indent);
        // Function-level escape: co_return inside an async throws coroutine.
        os << "if (!" << r << ".has_value()) { "
           << (current_func_is_async_ ? "co_return" : "return") << " std::unexpected{" << r
           << ".error()}; }\n";
        write_indent(os, indent);
        os << "auto " << h.name << " = *" << r << ";\n";
        return;
    }
    if (h.node->kind == ast::NodeKind::IfExpr || h.node->kind == ast::NodeKind::MatchExpr
        || h.node->kind == ast::NodeKind::DoCatchExpr) {
        emit_cond_hoist(os, h, indent);
        return;
    }
}

void CppEmitter::emit_cond_hoist(std::ostream& os, const TryHoist& h, int indent) {
    // §9 conditional hoist: an IfExpr or MatchExpr whose branches /
    // arms contain a propagating try lifts to a lambda returning
    // std::expected<T, E>. Each branch's local try-hoists fire inside
    // the branch's brace, escaping the lambda via `return
    // std::unexpected{...}`. The outer scope then unwraps the lambda's
    // result and re-propagates on error — same shape as the TryExpr
    // hoist.
    sema::TypePtr value_type = resolution_ != nullptr ? resolution_->type_of(h.node) : nullptr;
    write_indent(os, indent);
    os << "auto " << h.name << " = [&]() -> std::expected<";
    if (value_type != nullptr) {
        emit_sema_type(os, value_type);
    } else {
        os << "auto";
    }
    os << ", ";
    if (current_throws_type_ != nullptr) {
        emit_type(os, *current_throws_type_);
    } else {
        // Defensive: sema should reject `try` outside a throws(E) fn,
        // but the codegen still needs to write something legal here.
        os << "std::monostate";
    }
    os << "> {\n";

    // While emitting the hoisted node's own body, suppress
    // self-substitution — `emit_expr` would otherwise see the hoist for
    // this very node and write `*name` instead of the if/match shape.
    // The lambda is an ordinary (non-coroutine) callable returning
    // std::expected, so escapes inside it are plain `return` even when the
    // enclosing function is an async coroutine — suppress co_return here.
    const auto* prev_skip = skip_hoist_;
    skip_hoist_ = h.node;
    const bool prev_async = current_func_is_async_;
    current_func_is_async_ = false;
    if (h.node->kind == ast::NodeKind::IfExpr) {
        write_indent(os, indent + 1);
        emit_stmt_expr(os, *h.node, /*return_value=*/true);
    } else if (h.node->kind == ast::NodeKind::MatchExpr) {
        emit_match_in_lambda(os, static_cast<const ast::MatchExpr&>(*h.node), indent + 1);
    } else if (h.node->kind == ast::NodeKind::DoCatchExpr) {
        emit_do_catch_in_lambda(os, static_cast<const ast::DoCatchExpr&>(*h.node), indent + 1);
    }
    current_func_is_async_ = prev_async;
    skip_hoist_ = prev_skip;

    write_indent(os, indent);
    os << "}();\n";
    write_indent(os, indent);
    // Function-level re-propagation: co_return inside an async throws coroutine.
    os << "if (!" << h.name << ".has_value()) { "
       << (current_func_is_async_ ? "co_return" : "return") << " std::unexpected{" << h.name
       << ".error()}; }\n";
}

void CppEmitter::emit_match_in_lambda(std::ostream& os, const ast::MatchExpr& m, int indent) {
    // §9 statement-form match for the conditional hoist's lambda body.
    // Mirrors emit_match's two lowerings — switch for bare enum,
    // std::holds_alternative chain for payloaded enum — but every arm
    // body is emit_stmt_expr'd with return_value=true so a propagating
    // try inside an arm body escapes via `return std::unexpected{...}`
    // from the enclosing cond-hoist lambda. The std::visit-with-
    // constexpr-if shape that emit_match uses can't be reused here
    // because std::visit's lambda is its own return scope: a
    // `return std::unexpected{...}` inside that lambda would return
    // expected<T,E> from the visit's lambda rather than from the outer
    // cond-hoist lambda, breaking propagation. Using
    // std::holds_alternative + std::get instead keeps `return` rooted
    // in the cond-hoist lambda.
    if (resolution_ == nullptr) {
        unsupported(os, "match expression without resolved scrutinee in cond-hoist", m.range);
        return;
    }
    auto scrutinee_type = resolution_->type_of(m.scrutinee.get());
    if (scrutinee_type == nullptr || scrutinee_type->kind() != sema::TypeKind::Enum
        || scrutinee_type->nominal_decl() == nullptr) {
        // §8/§9 value-scrutinee match in a cond-hoist: an if-else-if chain
        // over the per-arm predicates, binding the scrutinee to a uniquely
        // named local so a `try` in an arm body returns from the enclosing
        // cond-hoist lambda (return_value=true), mirroring the enum forms
        // below. Reuses the same predicate / binding helpers as the ordinary
        // value-scrutinee lowering (emit_match_value_scrutinee).
        write_indent(os, indent);
        os << "auto&& __vstr_mh = ";
        emit_expr(os, *m.scrutinee);
        os << ";\n";
        bool first = true;
        const ast::MatchArm* default_arm = nullptr;
        for (const auto& arm : m.arms) {
            if (arm.is_default
                || (arm.pattern != nullptr && arm.pattern->kind == ast::NodeKind::WildcardPat)) {
                default_arm = &arm;
                continue;
            }
            if (arm.pattern == nullptr || arm.body == nullptr) {
                continue;
            }
            write_indent(os, indent);
            os << (first ? "if (" : "else if (");
            emit_pat_predicate(os, *arm.pattern, "__vstr_mh");
            if (arm.guard) {
                os << " && (";
                emit_expr(os, *arm.guard);
                os << ")";
            }
            os << ") {\n";
            emit_pat_bindings(os, *arm.pattern, "__vstr_mh", indent + 1);
            write_indent(os, indent + 1);
            emit_stmt_expr(os, *arm.body, /*return_value=*/true);
            write_indent(os, indent);
            os << "}\n";
            first = false;
        }
        write_indent(os, indent);
        if (default_arm != nullptr && default_arm->body != nullptr) {
            os << (first ? "{\n" : "else {\n");
            write_indent(os, indent + 1);
            emit_stmt_expr(os, *default_arm->body, /*return_value=*/true);
            write_indent(os, indent);
            os << "}\n";
        } else {
            os << (first ? "std::unreachable();\n" : "else { std::unreachable(); }\n");
        }
        return;
    }
    const auto& enum_decl = static_cast<const ast::EnumDecl&>(*scrutinee_type->nominal_decl());

    bool has_payload = false;
    for (const auto& c : enum_decl.cases) {
        if (!c.payload.empty()) {
            has_payload = true;
            break;
        }
    }

    if (!has_payload) {
        write_indent(os, indent);
        os << "switch (";
        emit_expr(os, *m.scrutinee);
        os << ") {\n";
        const ast::MatchArm* default_arm = nullptr;
        for (const auto& arm : m.arms) {
            if (arm.is_default
                || (arm.pattern != nullptr && arm.pattern->kind == ast::NodeKind::WildcardPat)) {
                default_arm = &arm;
                continue;
            }
            if (arm.pattern == nullptr || arm.pattern->kind != ast::NodeKind::EnumPat) {
                unsupported(os, "match arm pattern in cond-hoist", m.range);
                continue;
            }
            const auto& ep = static_cast<const ast::EnumPat&>(*arm.pattern);
            write_indent(os, indent);
            os << "case " << enum_decl.name << "::" << ep.case_name << ": {\n";
            write_indent(os, indent + 1);
            emit_stmt_expr(os, *arm.body, /*return_value=*/true);
            write_indent(os, indent);
            os << "}\n";
        }
        write_indent(os, indent);
        os << "default: ";
        if (default_arm != nullptr) {
            os << "{\n";
            write_indent(os, indent + 1);
            emit_stmt_expr(os, *default_arm->body, /*return_value=*/true);
            write_indent(os, indent);
            os << "}\n";
        } else {
            os << "std::unreachable();\n";
        }
        write_indent(os, indent);
        os << "}\n";
        return;
    }

    // Payloaded enum: emit an if-chain over std::holds_alternative so
    // every `return` inside an arm body returns from the enclosing
    // cond-hoist lambda. The payload-field binding mirrors
    // emit_match's std::visit path.
    write_indent(os, indent);
    os << "auto&& __vstr_alt_ref = (";
    emit_expr(os, *m.scrutinee);
    os << ").value;\n";

    bool first = true;
    const ast::MatchArm* default_arm = nullptr;
    for (const auto& arm : m.arms) {
        if (arm.is_default
            || (arm.pattern != nullptr && arm.pattern->kind == ast::NodeKind::WildcardPat)) {
            default_arm = &arm;
            continue;
        }
        if (arm.pattern == nullptr || arm.pattern->kind != ast::NodeKind::EnumPat) {
            unsupported(os, "match arm pattern in cond-hoist over payloaded enum", m.range);
            continue;
        }
        const auto& ep = static_cast<const ast::EnumPat&>(*arm.pattern);

        const ast::EnumDecl::Case* case_decl = nullptr;
        for (const auto& c : enum_decl.cases) {
            if (c.name == ep.case_name) {
                case_decl = &c;
                break;
            }
        }
        if (case_decl == nullptr) {
            unsupported(os, "match arm references unknown enum case", arm.pattern->range);
            continue;
        }

        write_indent(os, indent);
        if (first) {
            os << "if";
        } else {
            os << "else if";
        }
        os << " (std::holds_alternative<" << enum_decl.name << "::" << ep.case_name
           << "_t>(__vstr_alt_ref)) {\n";

        // Bind payload fields. The alt is `std::get<case_t>(__vstr_alt_ref)`;
        // only mint the named handle when there are children to read off it,
        // otherwise `-Werror=unused-variable` rejects the empty-payload arm.
        const bool has_child_binding =
            std::any_of(ep.children.begin(), ep.children.end(), [](const ast::PatternPtr& child) {
                if (!child) {
                    return false;
                }
                switch (child->kind) {
                case ast::NodeKind::BindPat:
                case ast::NodeKind::IdentPat:
                case ast::NodeKind::TuplePat:
                    return true;
                default:
                    return false;
                }
            });
        if (has_child_binding) {
            write_indent(os, indent + 1);
            os << "auto&& __vstr_alt = std::get<" << enum_decl.name << "::" << ep.case_name
               << "_t>(__vstr_alt_ref);\n";
        }

        for (std::size_t i = 0; i < ep.children.size() && i < case_decl->payload.size(); ++i) {
            const auto& child = *ep.children[i];
            std::string field_name = case_decl->payload[i].first.empty()
                                         ? std::format("_{}", i)
                                         : case_decl->payload[i].first;
            if (child.kind == ast::NodeKind::BindPat) {
                write_indent(os, indent + 1);
                os << "auto&& " << static_cast<const ast::BindPat&>(child).name << " = __vstr_alt."
                   << field_name << ";\n";
            } else if (child.kind == ast::NodeKind::IdentPat) {
                write_indent(os, indent + 1);
                os << "auto&& " << static_cast<const ast::IdentPat&>(child).name << " = __vstr_alt."
                   << field_name << ";\n";
            } else if (child.kind == ast::NodeKind::TuplePat) {
                const auto& sub_tp = static_cast<const ast::TuplePat&>(child);
                std::vector<std::string> sub_names;
                std::vector<std::pair<std::string, const ast::TuplePat*>> sub_followons;
                collect_tuple_pat_names(sub_tp, sub_names, sub_followons);
                write_indent(os, indent + 1);
                os << "auto&& [";
                for (std::size_t k = 0; k < sub_names.size(); ++k) {
                    if (k != 0) {
                        os << ", ";
                    }
                    os << sub_names[k];
                }
                os << "] = __vstr_alt." << field_name << ";\n";
                emit_tuple_pat_followons(os, sub_followons, indent + 1);
            }
        }

        write_indent(os, indent + 1);
        emit_stmt_expr(os, *arm.body, /*return_value=*/true);
        write_indent(os, indent);
        os << "}\n";
        first = false;
    }

    write_indent(os, indent);
    if (default_arm != nullptr) {
        os << "else {\n";
        write_indent(os, indent + 1);
        emit_stmt_expr(os, *default_arm->body, /*return_value=*/true);
        write_indent(os, indent);
        os << "}\n";
    } else {
        os << "else { std::unreachable(); }\n";
    }
}

void CppEmitter::emit_do_catch_in_lambda(std::ostream& os, const ast::DoCatchExpr& dc, int indent) {
    // §9 statement-form do/catch-with-where-guard for the cond-hoist
    // lambda body. The outer cond-hoist lambda returns
    // std::expected<T, E_outer>; we render the inner do-body lambda
    // (returning std::expected<T, E_inner>), dispatch on it, gate the
    // handler on the guard, and on guard-fail emit `return
    // std::unexpected{bound}` — that escapes through the cond-hoist
    // lambda to the enclosing throws context. Sema requires
    // `E_inner` to be assignable to `E_outer` (typically the same
    // enum), so the implicit std::unexpected conversion works.
    sema::TypePtr value_type = resolution_ != nullptr ? resolution_->type_of(&dc) : nullptr;
    write_indent(os, indent);
    os << "auto __vstr_do = [&]() -> std::expected<";
    if (value_type != nullptr) {
        emit_sema_type(os, value_type);
    } else {
        os << "auto";
    }
    os << ", ";
    if (dc.error_type) {
        emit_type(os, *dc.error_type);
    } else if (resolution_ != nullptr) {
        emit_sema_type(os, resolution_->do_catch_error_type(&dc));
    }
    os << "> { ";
    // Inner hoist scope for the do-body's trailing expression — same
    // shape as the non-hoisted DoCatchExpr emit path. Without this,
    // mid-expression tries inside `do { … }` would fall back to
    // `.value()` panics.
    std::vector<TryHoist> body_hoists;
    if (dc.do_body->kind == ast::NodeKind::BlockExpr) {
        const auto& b = static_cast<const ast::BlockExpr&>(*dc.do_body);
        if (!b.stmts.empty() && b.stmts.back()->kind == ast::NodeKind::ExprStmt) {
            const auto& trailing = static_cast<const ast::ExprStmt&>(*b.stmts.back());
            if (trailing.expr) {
                collect_try_hoists(*trailing.expr, body_hoists);
            }
        }
    } else {
        collect_try_hoists(*dc.do_body, body_hoists);
    }
    for (const auto& bh : body_hoists) {
        emit_try_hoist(os, bh, 0);
    }
    const auto* prev_hoists = active_hoists_;
    active_hoists_ = &body_hoists;
    emit_stmt_expr(os, *dc.do_body, /*return_value=*/true);
    active_hoists_ = prev_hoists;
    os << " }(); if (__vstr_do.has_value()) { return *__vstr_do; } [[maybe_unused]] auto "
       << dc.error_name << " = __vstr_do.error(); if (";
    emit_expr(os, *dc.guard);
    os << ") { return ";
    emit_expr(os, *dc.catch_body);
    os << "; } return std::unexpected{" << dc.error_name << "};\n";
}

const std::string* CppEmitter::lookup_try_hoist(const ast::Expr* node) const {
    if (active_hoists_ == nullptr) {
        return nullptr;
    }
    if (skip_hoist_ != nullptr && skip_hoist_ == node) {
        return nullptr;
    }
    for (const auto& h : *active_hoists_) {
        if (h.node == node) {
            return &h.name;
        }
    }
    return nullptr;
}

bool CppEmitter::expr_contains_propagating_try(const ast::Expr& e) const {
    switch (e.kind) {
    case ast::NodeKind::TryExpr:
        return static_cast<const ast::TryExpr&>(e).form == ast::TryExpr::Form::Propagating;
    case ast::NodeKind::IfExpr: {
        const auto& i = static_cast<const ast::IfExpr&>(e);
        if (i.cond && expr_contains_propagating_try(*i.cond)) {
            return true;
        }
        if (i.let_init && expr_contains_propagating_try(*i.let_init)) {
            return true;
        }
        if (i.then_branch && expr_contains_propagating_try(*i.then_branch)) {
            return true;
        }
        if (i.else_branch && expr_contains_propagating_try(*i.else_branch)) {
            return true;
        }
        return false;
    }
    case ast::NodeKind::MatchExpr: {
        const auto& m = static_cast<const ast::MatchExpr&>(e);
        if (m.scrutinee && expr_contains_propagating_try(*m.scrutinee)) {
            return true;
        }
        for (const auto& arm : m.arms) {
            if (arm.guard && expr_contains_propagating_try(*arm.guard)) {
                return true;
            }
            if (arm.body && expr_contains_propagating_try(*arm.body)) {
                return true;
            }
        }
        return false;
    }
    case ast::NodeKind::BlockExpr: {
        const auto& b = static_cast<const ast::BlockExpr&>(e);
        for (const auto& s : b.stmts) {
            if (stmt_contains_propagating_try(*s)) {
                return true;
            }
        }
        return false;
    }
    case ast::NodeKind::BinaryExpr: {
        const auto& b = static_cast<const ast::BinaryExpr&>(e);
        return (b.lhs && expr_contains_propagating_try(*b.lhs))
               || (b.rhs && expr_contains_propagating_try(*b.rhs));
    }
    case ast::NodeKind::UnaryExpr:
        return expr_contains_propagating_try(*static_cast<const ast::UnaryExpr&>(e).operand);
    case ast::NodeKind::CallExpr: {
        const auto& c = static_cast<const ast::CallExpr&>(e);
        if (c.callee && expr_contains_propagating_try(*c.callee)) {
            return true;
        }
        for (const auto& a : c.args) {
            if (a.value && expr_contains_propagating_try(*a.value)) {
                return true;
            }
        }
        return false;
    }
    case ast::NodeKind::MemberExpr:
        return expr_contains_propagating_try(*static_cast<const ast::MemberExpr&>(e).base);
    case ast::NodeKind::IndexExpr: {
        const auto& ix = static_cast<const ast::IndexExpr&>(e);
        if (ix.base && expr_contains_propagating_try(*ix.base)) {
            return true;
        }
        for (const auto& idx : ix.indices) {
            if (idx && expr_contains_propagating_try(*idx)) {
                return true;
            }
        }
        return false;
    }
    case ast::NodeKind::ParenExpr:
        return expr_contains_propagating_try(*static_cast<const ast::ParenExpr&>(e).inner);
    case ast::NodeKind::AsExpr:
        return expr_contains_propagating_try(*static_cast<const ast::AsExpr&>(e).value);
    case ast::NodeKind::CopyExpr:
        return expr_contains_propagating_try(*static_cast<const ast::CopyExpr&>(e).inner);
    case ast::NodeKind::VectorLitExpr: {
        const auto& v = static_cast<const ast::VectorLitExpr&>(e);
        for (const auto& el : v.elements) {
            if (el && expr_contains_propagating_try(*el)) {
                return true;
            }
        }
        return false;
    }
    case ast::NodeKind::TupleLitExpr: {
        const auto& tup = static_cast<const ast::TupleLitExpr&>(e);
        for (const auto& el : tup.elements) {
            if (el && expr_contains_propagating_try(*el)) {
                return true;
            }
        }
        return false;
    }
    case ast::NodeKind::InterpStringExpr: {
        const auto& is_ = static_cast<const ast::InterpStringExpr&>(e);
        for (const auto& seg : is_.segments) {
            if (seg.expr && expr_contains_propagating_try(*seg.expr)) {
                return true;
            }
        }
        return false;
    }
    default:
        return false;
    }
}

bool CppEmitter::stmt_contains_propagating_try(const ast::Stmt& s) const {
    switch (s.kind) {
    case ast::NodeKind::LetStmt:
        return static_cast<const ast::LetStmt&>(s).value
               && expr_contains_propagating_try(*static_cast<const ast::LetStmt&>(s).value);
    case ast::NodeKind::VarStmt:
        return static_cast<const ast::VarStmt&>(s).value
               && expr_contains_propagating_try(*static_cast<const ast::VarStmt&>(s).value);
    case ast::NodeKind::ExprStmt:
        return expr_contains_propagating_try(*static_cast<const ast::ExprStmt&>(s).expr);
    case ast::NodeKind::ReturnStmt:
        return static_cast<const ast::ReturnStmt&>(s).value
               && expr_contains_propagating_try(*static_cast<const ast::ReturnStmt&>(s).value);
    case ast::NodeKind::AssignStmt: {
        const auto& a = static_cast<const ast::AssignStmt&>(s);
        return (a.target && expr_contains_propagating_try(*a.target))
               || (a.value && expr_contains_propagating_try(*a.value));
    }
    default:
        return false;
    }
}

}  // namespace vestra::codegen
