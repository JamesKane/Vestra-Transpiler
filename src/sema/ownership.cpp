// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "vestra/sema/ownership.hpp"

#include "vestra/ast/nodes.hpp"
#include "vestra/diag/diagnostic.hpp"
#include "vestra/sema/resolver.hpp"
#include "vestra/sema/scope.hpp"
#include "vestra/sema/types.hpp"

#include <format>
#include <utility>

namespace vestra::sema {

// ============================================================================
// Entry / dispatch
// ============================================================================

void OwnershipChecker::check() {
    for (const auto& d : unit_->decls) {
        check_decl(*d);
    }
}

void OwnershipChecker::check_decl(const ast::Decl& d) {
    switch (d.kind) {
    case ast::NodeKind::Func:
        check_func(static_cast<const ast::FuncDecl&>(d));
        break;
    case ast::NodeKind::Struct: {
        const auto& s = static_cast<const ast::StructDecl&>(d);
        for (const auto& m : s.methods) {
            check_decl(*m);
        }
        break;
    }
    case ast::NodeKind::Enum: {
        const auto& e = static_cast<const ast::EnumDecl&>(d);
        for (const auto& m : e.methods) {
            check_decl(*m);
        }
        break;
    }
    case ast::NodeKind::Extension: {
        const auto& e = static_cast<const ast::ExtensionDecl&>(d);
        for (const auto& m : e.members) {
            check_decl(*m);
        }
        break;
    }
    default:
        break;  // Const / Static / Opaque / Protocol / Module / Import / Derive — no body to
                // flow-check
    }
}

void OwnershipChecker::check_func(const ast::FuncDecl& f) {
    if (!f.body) {
        return;  // protocol requirement / declaration only — nothing to check
    }
    // Reset per-function state. Parameter bindings are added lazily on first
    // use — we don't need to pre-populate them.
    bindings_.clear();
    check_expr(*f.body);
}

// ============================================================================
// Statements
// ============================================================================

void OwnershipChecker::check_stmt(const ast::Stmt& s) {
    switch (s.kind) {
    case ast::NodeKind::LetStmt: {
        const auto& l = static_cast<const ast::LetStmt&>(s);
        if (l.value) {
            check_expr(*l.value);
        }
        break;
    }
    case ast::NodeKind::VarStmt: {
        const auto& v = static_cast<const ast::VarStmt&>(s);
        if (v.value) {
            check_expr(*v.value);
        }
        break;
    }
    case ast::NodeKind::ExprStmt:
        check_expr(*static_cast<const ast::ExprStmt&>(s).expr);
        break;
    case ast::NodeKind::ReturnStmt: {
        const auto& r = static_cast<const ast::ReturnStmt&>(s);
        if (r.value) {
            check_expr(*r.value);
            // `return x` for a non-trivial place consumes the binding — the
            // function is handing ownership out to the caller.
            consume_place(*r.value, s.range);
        }
        break;
    }
    case ast::NodeKind::AssignStmt: {
        const auto& a = static_cast<const ast::AssignStmt&>(s);
        check_expr(*a.value);
        // Reassignment to a `var` revives a consumed binding (§19.2). We
        // approximate "the target is a var binding" by checking the LHS is
        // a tracked Symbol; if so, reset its state to Live.
        if (a.target && a.target->kind == ast::NodeKind::IdentExpr) {
            const auto* sym = resolution_->symbol_of(a.target.get());
            if (sym != nullptr) {
                auto it = bindings_.find(sym);
                if (it != bindings_.end()) {
                    it->second.state = State::Live;
                    it->second.consume_site = {};
                }
            }
        } else if (a.target) {
            check_expr(*a.target);  // member / index access — at least walk it
        }
        break;
    }
    case ast::NodeKind::WhileStmt: {
        const auto& w = static_cast<const ast::WhileStmt&>(s);
        check_expr(*w.cond);
        if (w.body) {
            check_expr(*w.body);
        }
        break;
    }
    case ast::NodeKind::ForStmt: {
        const auto& f = static_cast<const ast::ForStmt&>(s);
        check_expr(*f.iter);
        if (f.body) {
            check_expr(*f.body);
        }
        break;
    }
    case ast::NodeKind::BreakStmt:
    case ast::NodeKind::ContinueStmt:
        break;
    default:
        break;
    }
}

// ============================================================================
// Expressions
// ============================================================================

void OwnershipChecker::check_expr(const ast::Expr& e) {
    switch (e.kind) {
    case ast::NodeKind::IdentExpr:
        check_use(static_cast<const ast::IdentExpr&>(e));
        break;

    case ast::NodeKind::ParenExpr:
        check_expr(*static_cast<const ast::ParenExpr&>(e).inner);
        break;
    case ast::NodeKind::UnaryExpr:
        check_expr(*static_cast<const ast::UnaryExpr&>(e).operand);
        break;
    case ast::NodeKind::BinaryExpr: {
        const auto& b = static_cast<const ast::BinaryExpr&>(e);
        check_expr(*b.lhs);
        check_expr(*b.rhs);
        break;
    }
    case ast::NodeKind::CallExpr:
        check_call(static_cast<const ast::CallExpr&>(e));
        break;
    case ast::NodeKind::MemberExpr:
        check_expr(*static_cast<const ast::MemberExpr&>(e).base);
        break;
    case ast::NodeKind::IndexExpr: {
        const auto& ix = static_cast<const ast::IndexExpr&>(e);
        check_expr(*ix.base);
        for (const auto& i : ix.indices) {
            check_expr(*i);
        }
        break;
    }
    case ast::NodeKind::CopyExpr:
        // `copy x` is a read of x — recurse so we still validate that x is
        // live, but never trigger a consume on it. The copied value itself
        // is a fresh temporary, not tracked.
        check_expr(*static_cast<const ast::CopyExpr&>(e).inner);
        break;
    case ast::NodeKind::AwaitExpr:
        check_expr(*static_cast<const ast::AwaitExpr&>(e).inner);
        break;
    case ast::NodeKind::SpawnExpr:
        check_expr(*static_cast<const ast::SpawnExpr&>(e).inner);
        break;
    case ast::NodeKind::ThrowExpr:
        check_expr(*static_cast<const ast::ThrowExpr&>(e).inner);
        break;
    case ast::NodeKind::TryExpr:
        check_expr(*static_cast<const ast::TryExpr&>(e).inner);
        break;
    case ast::NodeKind::DoCatchExpr: {
        const auto& dc = static_cast<const ast::DoCatchExpr&>(e);
        check_expr(*dc.do_body);
        check_expr(*dc.catch_body);
        break;
    }
    case ast::NodeKind::AsExpr:
        check_expr(*static_cast<const ast::AsExpr&>(e).value);
        break;
    case ast::NodeKind::IfExpr: {
        const auto& i = static_cast<const ast::IfExpr&>(e);
        if (i.cond) {
            check_expr(*i.cond);
        } else if (i.let_init) {
            // §9 `if let NAME = INIT { ... }` — the init expression is the
            // only thing checked here; the let-bound name is fresh inside
            // the then-branch and doesn't participate in ownership flow
            // until/unless it's used there.
            check_expr(*i.let_init);
        }
        // Phase 1 is conservative: we walk both branches in sequence rather
        // than merging branch states. This may over-reject (a binding moved
        // only in one branch is considered moved after the if), but it never
        // under-rejects.
        if (i.then_branch) {
            check_expr(*i.then_branch);
        }
        if (i.else_branch) {
            check_expr(*i.else_branch);
        }
        break;
    }
    case ast::NodeKind::MatchExpr: {
        const auto& m = static_cast<const ast::MatchExpr&>(e);
        check_expr(*m.scrutinee);
        for (const auto& arm : m.arms) {
            if (arm.guard) {
                check_expr(*arm.guard);
            }
            if (arm.body) {
                check_expr(*arm.body);
            }
        }
        break;
    }
    case ast::NodeKind::BlockExpr: {
        const auto& b = static_cast<const ast::BlockExpr&>(e);
        for (const auto& s : b.stmts) {
            check_stmt(*s);
        }
        break;
    }
    case ast::NodeKind::VectorLitExpr: {
        const auto& v = static_cast<const ast::VectorLitExpr&>(e);
        for (const auto& el : v.elements) {
            check_expr(*el);
        }
        break;
    }
    default:
        // Literals / SelfExpr / LeadingDotExpr / closures / etc. have no
        // binding uses we model in phase 1.
        break;
    }
}

void OwnershipChecker::check_call(const ast::CallExpr& c) {
    // Walk the callee. We don't currently consume the callee — function
    // values are first-class but the binding semantics for `var f = ...; f()`
    // need more design.
    check_expr(*c.callee);

    // Determine each parameter's mode so we can decide which args to consume.
    // Sema bound the callee's symbol; from that we recover the FuncDecl's
    // params. If the callee resolves to a struct constructor or enum case,
    // params behave as `sink` (the constructed value owns its inputs).
    const auto* callee_sym = resolution_->symbol_of(c.callee.get());
    const ast::FuncDecl* fn = nullptr;
    if (callee_sym != nullptr && callee_sym->decl != nullptr
        && callee_sym->decl->kind == ast::NodeKind::Func) {
        fn = static_cast<const ast::FuncDecl*>(callee_sym->decl);
    }

    for (std::size_t i = 0; i < c.args.size(); ++i) {
        const auto& arg = c.args[i];
        check_expr(*arg.value);

        ast::ParamMode mode = ast::ParamMode::Read;
        if (fn != nullptr && i < fn->params.size()) {
            mode = fn->params[i].mode;
        } else if (arg.is_inout) {
            // An explicit `&x` at the call site signals an inout binding for
            // an unresolved callee. Treat it like an inout param (no consume).
            mode = ast::ParamMode::Inout;
        }
        if (mode == ast::ParamMode::Sink) {
            consume_place(*arg.value, arg.value->range);
        }
    }
}

// ============================================================================
// Place handling
// ============================================================================

const Symbol* OwnershipChecker::place_symbol(const ast::Expr& e) const {
    // `copy x` is not a place — the consumed value is the fresh copy, not x.
    if (e.kind == ast::NodeKind::ParenExpr) {
        return place_symbol(*static_cast<const ast::ParenExpr&>(e).inner);
    }
    if (e.kind != ast::NodeKind::IdentExpr) {
        return nullptr;
    }
    const auto* sym = resolution_->symbol_of(&e);
    if (sym == nullptr || !is_trackable_kind(sym->kind)) {
        return nullptr;
    }
    if (is_trivial(sym->type)) {
        return nullptr;
    }
    return sym;
}

void OwnershipChecker::consume_place(const ast::Expr& e, diag::SourceRange site) {
    const auto* sym = place_symbol(e);
    if (sym == nullptr) {
        return;
    }
    auto [it, inserted] = bindings_.try_emplace(sym);
    if (it->second.state == State::Consumed) {
        // Double-consume: report the second one as a use-after-move.
        auto d = diag::Diagnostic::error(
                     std::format("'{}' was already moved and cannot be moved again", sym->name))
                     .at(site);
        if (it->second.consume_site.is_valid()) {
            d.with_note(diag::Diagnostic::note(std::format("'{}' was moved here", sym->name))
                            .at(it->second.consume_site));
        }
        reporter_->report(std::move(d));
        return;
    }
    it->second.state = State::Consumed;
    it->second.consume_site = site;
}

void OwnershipChecker::check_use(const ast::IdentExpr& ident) {
    const auto* sym = resolution_->symbol_of(&ident);
    if (sym == nullptr || !is_trackable_kind(sym->kind) || is_trivial(sym->type)) {
        return;
    }
    auto it = bindings_.find(sym);
    if (it == bindings_.end() || it->second.state == State::Live) {
        return;
    }
    auto d = diag::Diagnostic::error(std::format("use of '{}' after it was moved", sym->name))
                 .at(ident.range);
    if (it->second.consume_site.is_valid()) {
        d.with_note(diag::Diagnostic::note(
                        std::format("'{}' was moved here; use `copy {}` if you need a duplicate",
                                    sym->name,
                                    sym->name))
                        .at(it->second.consume_site));
    }
    reporter_->report(std::move(d));
}

// ============================================================================
// Type / symbol predicates
// ============================================================================

bool OwnershipChecker::is_trivial(TypePtr t) noexcept {
    if (t == nullptr) {
        return true;  // unresolved type — don't pile on
    }
    // Phase 1 Trivial set: every fixed-width numeric, Bool, Char, Unit. Strings
    // (String/Str/StrConst) are not Trivial: String owns a buffer and even
    // Str/StrConst can carry borrow semantics worth tracking later.
    switch (t->kind()) {
    case TypeKind::Int8:
    case TypeKind::Int16:
    case TypeKind::Int32:
    case TypeKind::Int64:
    case TypeKind::Int:
    case TypeKind::UInt8:
    case TypeKind::UInt16:
    case TypeKind::UInt32:
    case TypeKind::UInt64:
    case TypeKind::UInt:
    case TypeKind::Float32:
    case TypeKind::Float64:
    case TypeKind::Bool:
    case TypeKind::Char:
    case TypeKind::Unit:
    case TypeKind::Never:
    case TypeKind::Error:
        return true;
    default:
        return false;
    }
}

bool OwnershipChecker::is_trackable_kind(SymbolKind k) noexcept {
    return k == SymbolKind::Local || k == SymbolKind::Parameter;
}

}  // namespace vestra::sema
