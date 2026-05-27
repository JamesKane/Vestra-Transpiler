// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "vestra/sema/capability.hpp"

#include "vestra/ast/nodes.hpp"
#include "vestra/diag/diagnostic.hpp"
#include "vestra/sema/resolver.hpp"

#include <format>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace vestra::sema {

namespace {
constexpr std::string_view AsyncCap = "Async";
}

// ============================================================================
// Entry / dispatch
// ============================================================================

void CapabilityChecker::check() {
    for (const auto& d : unit_->decls) {
        check_decl(*d);
    }
}

void CapabilityChecker::check_decl(const ast::Decl& d) {
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

void CapabilityChecker::check_func(const ast::FuncDecl& f) {
    if (!f.body) {
        return;
    }
    // Push a fresh scope holding the function's declared row + Async if async.
    std::unordered_set<std::string> scope;
    for (const auto& cap : f.effects.using_caps) {
        if (cap) {
            auto name = cap_name(*cap);
            if (!name.empty()) {
                scope.insert(std::move(name));
            }
        }
    }
    if (f.is_async) {
        scope.emplace(AsyncCap);
    }
    scopes_.push_back(std::move(scope));
    check_expr(*f.body);
    scopes_.pop_back();
}

// ============================================================================
// Statements / expressions
// ============================================================================

void CapabilityChecker::check_stmt(const ast::Stmt& s) {
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
    case ast::NodeKind::WithStmt: {
        const auto& w = static_cast<const ast::WithStmt&>(s);
        std::unordered_set<std::string> scope;
        for (const auto& b : w.bindings) {
            if (b.cap_type) {
                auto name = cap_name(*b.cap_type);
                if (!name.empty()) {
                    scope.insert(std::move(name));
                }
            }
            // Phase 1 doesn't type-check the bound value — we just walk it
            // so any nested calls inside its expression are validated.
            if (b.value) {
                check_expr(*b.value);
            }
        }
        scopes_.push_back(std::move(scope));
        if (w.body) {
            check_expr(*w.body);
        }
        scopes_.pop_back();
        break;
    }
    case ast::NodeKind::BreakStmt:
    case ast::NodeKind::ContinueStmt:
        break;
    default:
        break;
    }
}

void CapabilityChecker::check_expr(const ast::Expr& e) {
    switch (e.kind) {
    case ast::NodeKind::CallExpr: {
        const auto& c = static_cast<const ast::CallExpr&>(e);
        // §10: a `Box.new(value)` call is a heap allocation and
        // therefore requires the `Alloc` capability in scope.
        // Detected by the callee's syntactic shape so we don't have
        // to thread a builtin Func into the symbol table.
        if (c.callee && c.callee->kind == ast::NodeKind::MemberExpr) {
            const auto& mem = static_cast<const ast::MemberExpr&>(*c.callee);
            if (mem.base && mem.base->kind == ast::NodeKind::IdentExpr
                && static_cast<const ast::IdentExpr&>(*mem.base).name == "Box"
                && mem.member == "new" && !in_scope("Alloc")) {
                missing_capability("Alloc", c.range);
            }
            // §A3 (§10.5) — minting raw pointers and spans-from-raw
            // requires `RawMemory` in scope. We pattern-match all
            // four call shapes here so the audit surface ("every
            // `unchecked(fromAddress:)` and the discharge that
            // admitted it") has the same gating point.
            if (mem.base && mem.base->kind == ast::NodeKind::IdentExpr) {
                const auto& bi = static_cast<const ast::IdentExpr&>(*mem.base);
                const bool is_unchecked_mint =
                    (bi.name == "Ptr" || bi.name == "MutPtr") && mem.member == "unchecked";
                const bool is_raw_span =
                    (bi.name == "Span" || bi.name == "MutSpan") && mem.member == "raw";
                if ((is_unchecked_mint || is_raw_span) && !in_scope("RawMemory")) {
                    missing_capability("RawMemory", c.range);
                }
                // §A6 (§14.11) MMIO view constructors require the
                // `Mmio` capability. A driver-init region typically
                // nests `with RawMemory { with Mmio { … } }`:
                // RawMemory mints the MutPtr; Mmio admits the view.
                const bool is_mmio_at =
                    (bi.name == "MmioView" || bi.name == "MmioRegion") && mem.member == "at";
                if (is_mmio_at && !in_scope("Mmio")) {
                    missing_capability("Mmio", c.range);
                }
            }
        }
        // §A5 (§14.10.3, §14.10.4) the data-cache management ops
        // (clean / invalidate / cleanInvalidate / zero) and
        // publishInstructions all read or write memory through the
        // user-supplied Span range, so they're gated on RawMemory
        // the same way the §A3 mints are. invalidateAllInstructions
        // and the TLB ops act on architectural state alone, so they
        // need only Asm (which v0.5 doesn't yet enforce).
        if (c.callee && c.callee->kind == ast::NodeKind::IdentExpr) {
            static const std::unordered_set<std::string_view> raw_memory_builtins = {
                "cleanData",
                "invalidateData",
                "cleanInvalidateData",
                "zeroData",
                "publishInstructions",
            };
            const auto& bi = static_cast<const ast::IdentExpr&>(*c.callee);
            if (raw_memory_builtins.contains(bi.name) && !in_scope("RawMemory")) {
                missing_capability("RawMemory", c.range);
            }
        }
        check_expr(*c.callee);
        for (const auto& a : c.args) {
            check_expr(*a.value);
        }
        // The callee may be a Func we know; if so, enforce its using row.
        if (const auto* sym = resolution_->symbol_of(c.callee.get())) {
            if (sym->decl != nullptr && sym->decl->kind == ast::NodeKind::Func) {
                const auto& callee = static_cast<const ast::FuncDecl&>(*sym->decl);
                for (const auto& cap : required_caps(callee)) {
                    if (!in_scope(cap)) {
                        missing_capability(cap, c.range);
                    }
                }
            }
        }
        break;
    }
    case ast::NodeKind::AwaitExpr: {
        const auto& a = static_cast<const ast::AwaitExpr&>(e);
        check_expr(*a.inner);
        if (!in_scope(AsyncCap)) {
            missing_capability(AsyncCap, e.range);
        }
        break;
    }
    case ast::NodeKind::SpawnExpr: {
        const auto& s = static_cast<const ast::SpawnExpr&>(e);
        check_expr(*s.inner);
        if (!in_scope(AsyncCap)) {
            missing_capability(AsyncCap, e.range);
        }
        break;
    }

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
    case ast::NodeKind::BlockExpr:
        for (const auto& s : static_cast<const ast::BlockExpr&>(e).stmts) {
            check_stmt(*s);
        }
        break;
    case ast::NodeKind::CopyExpr:
        check_expr(*static_cast<const ast::CopyExpr&>(e).inner);
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
        break;  // literals / SelfExpr / LeadingDotExpr / closures — no embedded calls in phase 1
    }
}

// ============================================================================
// Helpers
// ============================================================================

std::vector<std::string> CapabilityChecker::required_caps(const ast::FuncDecl& callee) const {
    std::vector<std::string> caps;
    caps.reserve(callee.effects.using_caps.size() + (callee.is_async ? 1 : 0));
    for (const auto& cap : callee.effects.using_caps) {
        if (!cap) {
            continue;
        }
        auto name = cap_name(*cap);
        if (!name.empty()) {
            caps.push_back(std::move(name));
        }
    }
    if (callee.is_async) {
        caps.emplace_back(AsyncCap);
    }
    return caps;
}

std::string CapabilityChecker::cap_name(const ast::Type& t) {
    if (t.kind != ast::NodeKind::NamedType) {
        return {};
    }
    const auto& n = static_cast<const ast::NamedType&>(t);
    if (n.path.empty()) {
        return {};
    }
    return n.path.front();
}

bool CapabilityChecker::in_scope(std::string_view name) const noexcept {
    for (const auto& s : scopes_) {
        if (s.contains(std::string{name})) {
            return true;
        }
    }
    return false;
}

void CapabilityChecker::missing_capability(std::string_view name, diag::SourceRange site) {
    reporter_->report(
        diag::Diagnostic::error(std::format("missing capability '{}' — declare it in the "
                                            "function's `using` row or open a `with {} = ...`",
                                            name,
                                            name))
            .at(site));
}

}  // namespace vestra::sema
