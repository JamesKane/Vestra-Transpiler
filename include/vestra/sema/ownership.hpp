// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#pragma once

#include "vestra/ast/nodes.hpp"
#include "vestra/diag/diagnostic.hpp"
#include "vestra/diag/source_manager.hpp"
#include "vestra/sema/resolver.hpp"
#include "vestra/sema/scope.hpp"
#include "vestra/sema/types.hpp"

#include <cstdint>
#include <unordered_map>

namespace vestra::sema {

// Ownership / move tracking — phase 1 of §5 and §19.2.
//
// What this pass enforces today:
//   * Every non-trivial binding (Local or Parameter) has a state: Live or
//     Consumed.
//   * A binding is consumed by passing it to a `sink` parameter or by
//     `return`ing it.
//   * Using a consumed binding is rejected, with the original move site
//     attached as a `note:` for the developer.
//   * `copy x` reads x but does not consume it; the copy is the consumed
//     value when the surrounding context demands ownership.
//
// What this pass deliberately does not (yet) do:
//   * Flow-merge across `if` / `match` branches — analysis is linear.
//   * Field-level tracking — moving `s.field` is approximated as moving `s`.
//   * Linearity (`linear` keyword) — there is no must-consume check yet.
//   * Mutability enforcement on `let` — that belongs in a separate check.
//   * Trivial detection beyond the built-in numeric / Bool / Char primitives.
class OwnershipChecker {
public:
    OwnershipChecker(const ast::CompilationUnit& unit,
                     const Resolution& resolution,
                     diag::DiagnosticReporter& reporter)
        : unit_(&unit), resolution_(&resolution), reporter_(&reporter) {}

    OwnershipChecker(const OwnershipChecker&) = delete;
    OwnershipChecker& operator=(const OwnershipChecker&) = delete;

    void check();

private:
    // NOLINTNEXTLINE(performance-enum-size)
    enum class State : std::uint8_t { Live, Consumed };

    struct BindingInfo {
        State state = State::Live;
        diag::SourceRange consume_site{};
    };

    void check_decl(const ast::Decl& d);
    void check_func(const ast::FuncDecl& f);

    void check_stmt(const ast::Stmt& s);
    void check_expr(const ast::Expr& e);
    void check_call(const ast::CallExpr& c);

    // If `e` is a bare IdentExpr whose Symbol we're tracking, mark that
    // binding Consumed at `site` (or report a double-consume).
    void consume_place(const ast::Expr& e, diag::SourceRange site);

    // The Symbol referenced by `e` if `e` is a bare IdentExpr/CopyExpr-stripped
    // path that we track in this function; nullptr otherwise.
    [[nodiscard]] const Symbol* place_symbol(const ast::Expr& e) const;

    // Treat `e` as a use — if it references a tracked, consumed binding,
    // report use-after-move with the original move site as a note.
    void check_use(const ast::IdentExpr& ident);

    using BindingMap = std::unordered_map<const Symbol*, BindingInfo>;

    // §10/§5 phase 2 — branch-aware flow merge. Fold `branch` into `dst`
    // so a binding is Consumed in `dst` iff it is Consumed on *either*
    // path (a join's state is the union of the moves on the paths reaching
    // it). Keeps the consume site for diagnostics.
    static void merge_consumed(BindingMap& dst, const BindingMap& branch);

    // §5/§19.6 — does this type require linear consumption? True for a
    // `linear struct` (transitivity through linear fields is a follow-on).
    [[nodiscard]] static bool is_linear_type(TypePtr t) noexcept;

    // §5/§19.6 — if this let/var statement binds a linear value, register
    // it Live so the end-of-scope leak check sees it even when it is never
    // otherwise touched.
    void register_linear_binding(const ast::Stmt& s);

    // §5/§19.6 — at a branch join, a linear binding that was live before
    // the branch must be consumed on *every* path or *none*. If `pre`'s
    // linear bindings are consumed on some `branches` but not all, the
    // unconsumed paths leak — report each.
    void report_linear_divergence(const BindingMap& pre,
                                  const std::vector<const BindingMap*>& branches,
                                  diag::SourceRange site);

    // Is this type a candidate for ownership tracking? Trivial types are
    // freely copied and never tracked.
    [[nodiscard]] static bool is_trivial(TypePtr t) noexcept;

    // Only Local / Parameter symbols are tracked. (Func/Const/Struct/... are
    // not values that can be consumed.)
    [[nodiscard]] static bool is_trackable_kind(SymbolKind k) noexcept;

    const ast::CompilationUnit* unit_;
    const Resolution* resolution_;
    diag::DiagnosticReporter* reporter_;

    // Per-function: state of every tracked binding encountered so far. Reset
    // at the start of each function.
    std::unordered_map<const Symbol*, BindingInfo> bindings_;
};

}  // namespace vestra::sema
