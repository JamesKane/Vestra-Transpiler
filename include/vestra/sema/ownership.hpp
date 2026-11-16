// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#pragma once

#include <map>
#include <optional>
#include <string>

#include "vestra/ast/nodes.hpp"
#include "vestra/diag/diagnostic.hpp"
#include "vestra/diag/source_manager.hpp"
#include "vestra/sema/linearity.hpp"
#include "vestra/sema/resolver.hpp"
#include "vestra/sema/scope.hpp"
#include "vestra/sema/types.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>

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
        // §5 fields moved out of a live binding ("f", "f.g"), with the move
        // sites. The binding stays usable except through a moved path.
        std::map<std::string, diag::SourceRange> moved_fields;
    };

    // `o.f.g` as ("f.g", o's symbol) when it is a field path over a tracked
    // binding (no index, no optional chain, not a method); nullopt otherwise.
    [[nodiscard]] std::optional<std::string> field_path(const ast::Expr& e,
                                                        const Symbol*& root) const;
    // Report a use of field path `path` of `root` that touches a moved field.
    bool check_field_use(const Symbol* root, const std::string& path, diag::SourceRange site);
    // Move field path `e` out of its binding (§5), or reject it.
    bool consume_field(const ast::Expr& e, diag::SourceRange site);

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
    // `linear struct` and, transitively, any struct that owns a linear
    // field. Membership is precomputed into linear_decls_ by
    // compute_linear_decls() at the start of check().
    [[nodiscard]] bool is_linear_type(TypePtr t) const noexcept;

    // §5/§19.6 — seed linear_decls_ with every `linear struct` in the unit,
    // then close it under field ownership: a struct that has a field of a
    // linear type is itself linear. Run once, before any function is checked.
    void compute_linear_decls();

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

    // ---- §19.6 linear values in storage places (F17) -------------------------
    // A storage place is a field, an element, a static, `self`'s state, or a
    // `read` / `inout` parameter: storage this function doesn't own outright.
    // A linear value moves out of one only with `.take()`, an `if let` on one
    // borrows, and `place = v` must not overwrite a live linear value.
    struct PlaceComponent {
        bool index = false;      // `[i]` (vs a root name or `.field`)
        std::string text;        // the name, or the rendered index expression
        std::vector<std::string> idents;  // local names an index mentions
    };
    struct PlaceKey {
        std::vector<PlaceComponent> parts;  // parts[0] is the root
        bool root_is_static = false;
        [[nodiscard]] std::string render() const;
    };
    // The storage place `e` names, if it is one we can key (a root plus
    // fields and simply-indexed elements); nullopt otherwise.
    [[nodiscard]] std::optional<PlaceKey> storage_place(const ast::Expr& e) const;
    [[nodiscard]] static bool may_alias(const PlaceKey& a, const PlaceKey& b);
    void kill_aliasing(const PlaceKey& k);
    void kill_mentioning(std::string_view local);
    void kill_rooted_at(std::string_view root);
    [[nodiscard]] bool known_empty(const PlaceKey& k) const;
    [[nodiscard]] bool is_linear_place_value(const ast::Expr& e) const;
    // Rejects moving a linear value out of a storage place or a borrow.
    void reject_move_out(const ast::Expr& e, diag::SourceRange site);
    void check_place_assign(const ast::AssignStmt& a);
    // The Optional-method special cases: `.take()` / `.put(v)`.
    bool check_optional_method(const ast::CallExpr& c);
    // §18.3, §18.9 the builtin entry points that take their value `sink T`:
    // `Channel.send(x)` and `Box.new(x)`. True when argument `i` of `c` is
    // one of them. `Vec.push` (the bootstrap's `Buffer.append`) still copies;
    // see SPEC_GAPS §18.
    [[nodiscard]] bool builtin_sink_arg(const ast::CallExpr& c, std::size_t i) const;
    // `P == nil` conjuncts in `cond` (then-branch facts), or `P != nil`.
    void collect_nil_facts(const ast::Expr& cond, bool want_nil, std::vector<PlaceKey>& out) const;
    [[nodiscard]] bool block_diverges(const ast::Expr* e) const;
    // Does this diverging branch leave the function (vs break / continue)?
    [[nodiscard]] bool leaves_function(const ast::Expr* e) const;
    // §19.5 a path that ends in a `Never` call (`panic`) stops the program:
    // it runs no destructors and owes no linear value a consumer.
    [[nodiscard]] bool stops(const ast::Expr* e) const;
    void report_path_leaks(const BindingMap& state, diag::SourceRange site);
    // §5 / §19.2 a binding from outside a loop, live on entry and still moved
    // at the end of the body, is moved again (or used) by the next iteration.
    void report_loop_moves(const BindingMap& pre, const ast::Expr* body);
    // §19.2 why a type isn't Copyable ("" when it is): it's linear, has a
    // `deinit`, or owns a `Box`, directly or through a field / payload /
    // element.
    [[nodiscard]] std::string non_copyable_reason(TypePtr t, int depth = 0) const;
    // §7 closures capture by value. While a closure body is checked, every
    // outer binding it names is recorded here (innermost closure last).
    struct CaptureScope {
        diag::SourceRange range;
        std::vector<std::pair<const Symbol*, diag::SourceRange>> captures;
    };
    std::vector<CaptureScope> captures_;
    // §19.6 the `do` bodies around the current point (innermost last): a
    // propagating `try` inside one leaves it for the catch, not the function.
    std::vector<diag::SourceRange> do_bodies_;
    void report_try_leaks(diag::SourceRange site);
    // §19.6 a call statement whose linear result nothing consumes drops it.
    void reject_dropped_linear(const ast::Expr& e);
    void note_capture(const Symbol* sym, diag::SourceRange site);
    void check_closure(const ast::ClosureExpr& cx);
    [[nodiscard]] static bool inside(diag::SourceRange inner, diag::SourceRange outer);

    const ast::CompilationUnit* unit_;
    const Resolution* resolution_;
    diag::DiagnosticReporter* reporter_;

    // Places known to hold `.none` at the current program point.
    std::vector<PlaceKey> empty_places_;
    // `if let` bindings that borrow a storage place's value.
    std::unordered_set<const Symbol*> borrows_;
    // Bindings already reported as leaking, so one leak is one error.
    std::unordered_set<const Symbol*> leak_reported_;
    // §19.6 `sink` parameters of a linear struct declared in another module:
    // only that module's functions may drop one, so here it must move on.
    std::unordered_set<const Symbol*> owed_params_;
    [[nodiscard]] bool owes(const Symbol* sym) const;
    // The current function's parameters, for `read` / `inout` mode lookups.
    const ast::FuncDecl* current_func_ = nullptr;
    mutable std::optional<Linearity> linearity_;

    // Per-function: state of every tracked binding encountered so far. Reset
    // at the start of each function.
    std::unordered_map<const Symbol*, BindingInfo> bindings_;

    // §5/§19.6 — every struct decl that requires linear consumption, whether
    // directly (`linear struct`) or transitively (owns a linear field).
    // Filled once by compute_linear_decls() at the start of check().
    std::unordered_set<const ast::Decl*> linear_decls_;
};

}  // namespace vestra::sema
