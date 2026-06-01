// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "vestra/sema/exclusivity.hpp"

#include "vestra/ast/nodes.hpp"
#include "vestra/diag/diagnostic.hpp"
#include "vestra/sema/resolver.hpp"
#include "vestra/sema/scope.hpp"

#include <format>
#include <string>
#include <utility>
#include <vector>

namespace vestra::sema {

// ============================================================================
// Entry / dispatch
// ============================================================================

void ExclusivityChecker::check() {
    for (const auto& d : unit_->decls) {
        check_decl(*d);
    }
}

void ExclusivityChecker::check_decl(const ast::Decl& d) {
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
        break;
    }
}

void ExclusivityChecker::check_func(const ast::FuncDecl& f) {
    if (!f.body) {
        return;
    }
    check_expr(*f.body);
}

// ============================================================================
// Statements / expressions
// ============================================================================

void ExclusivityChecker::check_stmt(const ast::Stmt& s) {
    switch (s.kind) {
    case ast::NodeKind::LetStmt:
        if (auto* v = static_cast<const ast::LetStmt&>(s).value.get()) {
            check_expr(*v);
        }
        break;
    case ast::NodeKind::VarStmt:
        if (auto* v = static_cast<const ast::VarStmt&>(s).value.get()) {
            check_expr(*v);
        }
        break;
    case ast::NodeKind::ExprStmt:
        check_expr(*static_cast<const ast::ExprStmt&>(s).expr);
        break;
    case ast::NodeKind::ReturnStmt:
        if (auto* v = static_cast<const ast::ReturnStmt&>(s).value.get()) {
            check_expr(*v);
        }
        break;
    case ast::NodeKind::AssignStmt: {
        const auto& a = static_cast<const ast::AssignStmt&>(s);
        if (a.target) {
            check_expr(*a.target);
        }
        if (a.value) {
            check_expr(*a.value);
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
    default:
        break;
    }
}

void ExclusivityChecker::check_expr(const ast::Expr& e) {
    switch (e.kind) {
    case ast::NodeKind::CallExpr:
        check_call(static_cast<const ast::CallExpr&>(e));
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
    case ast::NodeKind::IfExpr: {
        const auto& i = static_cast<const ast::IfExpr&>(e);
        if (i.cond) {
            check_expr(*i.cond);
        } else if (i.let_init) {
            check_expr(*i.let_init);
        }
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
    case ast::NodeKind::CopyExpr:
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
    case ast::NodeKind::VectorLitExpr:
        for (const auto& el : static_cast<const ast::VectorLitExpr&>(e).elements) {
            check_expr(*el);
        }
        break;
    default:
        break;  // literals / SelfExpr / LeadingDotExpr / closures — no calls inside
    }
}

// ============================================================================
// The actual check: per-call borrow gathering + pairwise conflict scan.
// ============================================================================

void ExclusivityChecker::check_call(const ast::CallExpr& c) {
    // Recurse into the callee + every argument so nested calls get checked
    // before we look at this one. The borrows of nested calls have ended by
    // the time the outer call evaluates.
    check_expr(*c.callee);
    for (const auto& a : c.args) {
        check_expr(*a.value);
    }

    // Recover the callee's parameter modes. Struct constructors and unresolved
    // callees default every param to read; sink params don't generate borrows.
    const ast::FuncDecl* fn = nullptr;
    if (const auto* sym = resolution_->symbol_of(c.callee.get())) {
        if (sym->decl != nullptr && sym->decl->kind == ast::NodeKind::Func) {
            fn = static_cast<const ast::FuncDecl*>(sym->decl);
        }
    }

    std::vector<Borrow> borrows;
    borrows.reserve(c.args.size());
    for (std::size_t i = 0; i < c.args.size(); ++i) {
        const auto& arg = c.args[i];

        ast::ParamMode mode = ast::ParamMode::Read;
        if (fn != nullptr && i < fn->params.size()) {
            mode = fn->params[i].mode;
        }
        // A call-site `&x` forces inout regardless of what we inferred.
        if (arg.is_inout) {
            mode = ast::ParamMode::Inout;
        }
        if (mode == ast::ParamMode::Sink) {
            continue;  // ownership owns this — it's a consume, not a borrow
        }

        auto place = as_place(*arg.value);
        if (!place) {
            continue;  // temporaries, literals, computed expressions — no place
        }

        borrows.push_back({std::move(*place),
                           mode == ast::ParamMode::Inout ? Access::Inout : Access::Read,
                           arg.value->range});
    }

    // Pairwise conflict scan. The lists are tiny in practice (most calls have
    // ≤ 4 args), so the O(n²) is fine.
    for (std::size_t i = 0; i < borrows.size(); ++i) {
        for (std::size_t j = i + 1; j < borrows.size(); ++j) {
            const auto& a = borrows[i];
            const auto& b = borrows[j];
            if (!overlap(a.place, b.place) || !conflict(a.access, b.access)) {
                continue;
            }
            auto same_place = a.place.path == b.place.path;
            auto reason = same_place
                              ? std::format("{} is borrowed twice with incompatible modes",
                                            render_place(a.place))
                              : std::format("{} and {} overlap with incompatible borrow modes",
                                            render_place(a.place),
                                            render_place(b.place));
            auto d = diag::Diagnostic::error(std::move(reason)).at(b.site);
            d.with_note(
                diag::Diagnostic::note(std::format("first borrow is here ({})",
                                                   a.access == Access::Inout ? "inout" : "read"))
                    .at(a.site));
            d.with_note(
                diag::Diagnostic::note(std::format("second borrow is here ({})",
                                                   b.access == Access::Inout ? "inout" : "read"))
                    .at(b.site));
            reporter_->report(std::move(d));
        }
    }
}

// ============================================================================
// Place extraction + overlap
// ============================================================================

std::optional<ExclusivityChecker::Place> ExclusivityChecker::as_place(const ast::Expr& e) const {
    switch (e.kind) {
    case ast::NodeKind::ParenExpr:
        return as_place(*static_cast<const ast::ParenExpr&>(e).inner);
    case ast::NodeKind::IdentExpr: {
        const auto& ident = static_cast<const ast::IdentExpr&>(e);
        const auto* sym = resolution_->symbol_of(&e);
        if (sym == nullptr) {
            return std::nullopt;
        }
        // Only Local/Parameter symbols are storage we care about for the
        // exclusivity rule. Calls, consts, statics, type symbols — none of
        // those are places a caller can &x into.
        if (sym->kind != SymbolKind::Local && sym->kind != SymbolKind::Parameter) {
            return std::nullopt;
        }
        (void)ident;
        // §5/§18.4 partition provenance: a sub-view binding (a `split` half or
        // a `chunks` iterator) maps to a place rooted at its *parent* with a
        // synthetic discriminator segment. A borrow of the sub-view then
        // overlaps a borrow of the parent (the parent's empty path is a prefix
        // of the segment), while disjoint split halves carry distinct segments
        // and so don't overlap each other.
        if (sym->provenance_root != nullptr) {
            return Place{sym->provenance_root, {sym->provenance_segment}};
        }
        return Place{sym, {}};
    }
    case ast::NodeKind::MemberExpr: {
        const auto& m = static_cast<const ast::MemberExpr&>(e);
        auto base = as_place(*m.base);
        if (!base) {
            return std::nullopt;
        }
        base->path.push_back("." + m.member);
        return base;
    }
    default:
        return std::nullopt;
    }
}

bool ExclusivityChecker::overlap(const Place& a, const Place& b) noexcept {
    if (a.root == nullptr || b.root == nullptr || a.root != b.root) {
        return false;
    }
    // Same root: overlap iff one path is a prefix of the other (which
    // includes the equal-paths case). Differing path elements at any
    // shared depth means they walk into disjoint fields.
    const auto& shorter = a.path.size() < b.path.size() ? a.path : b.path;
    const auto& longer = a.path.size() < b.path.size() ? b.path : a.path;
    for (std::size_t i = 0; i < shorter.size(); ++i) {
        if (shorter[i] != longer[i]) {
            return false;
        }
    }
    return true;
}

bool ExclusivityChecker::conflict(Access a, Access b) noexcept {
    return a == Access::Inout || b == Access::Inout;
}

std::string ExclusivityChecker::render_place(const Place& p) {
    const std::string root = (p.root != nullptr) ? p.root->name : std::string{"?"};
    // §5/§18.4 — a synthetic partition segment ("@split0" / "@chunks") renders
    // as a readable "sub-view of '<root>'" rather than the internal spelling.
    // The returned string is fully display-formatted (quotes included), so the
    // call sites don't wrap it again.
    if (p.path.size() == 1 && !p.path[0].empty() && p.path[0][0] == '@') {
        return std::format("a sub-view of '{}'", root);
    }
    std::string s = "'" + root;
    for (const auto& seg : p.path) {
        s += seg;
    }
    s += "'";
    return s;
}

}  // namespace vestra::sema
