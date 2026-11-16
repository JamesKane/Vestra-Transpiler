// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include <functional>
#include "vestra/sema/ownership.hpp"

#include <algorithm>

#include "vestra/ast/nodes.hpp"
#include "vestra/diag/diagnostic.hpp"
#include "vestra/sema/resolver.hpp"
#include "vestra/sema/scope.hpp"
#include "vestra/sema/types.hpp"

#include <format>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace vestra::sema {

// ============================================================================
// Entry / dispatch
// ============================================================================

void OwnershipChecker::check() {
    compute_linear_decls();
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
    // use. A `sink` parameter takes ownership of its argument and is itself a
    // terminal sink — the callee may move it onward (to another sink / a
    // return) or simply drop it, which destroys the value. So a sink-linear
    // parameter carries no must-consume obligation inside the body; only
    // *local* linear bindings (let/var) and the linear values they own must
    // be discharged before their scope ends.
    bindings_.clear();
    empty_places_.clear();
    borrows_.clear();
    leak_reported_.clear();
    current_func_ = &f;
    check_expr(*f.body);
    // §5/§19.6 — any linear binding still live at the end of the function
    // body is a leak: a linear value must be consumed before its scope ends.
    for (const auto& [sym, info] : bindings_) {
        if (info.state == State::Live && sym != nullptr && sym->kind != SymbolKind::Parameter
            && is_linear_type(sym->type)
            && !leak_reported_.contains(sym)) {
            reporter_->report(
                diag::Diagnostic::error(
                    std::format("linear value '{}' is never consumed; a linear value must be moved "
                                "to a sink parameter, returned, or destructured before its scope "
                                "ends",
                                sym->name))
                    .at(sym->definition_range));
        }
    }
}

// ============================================================================
// Statements
// ============================================================================

void OwnershipChecker::check_stmt(const ast::Stmt& s) {
    switch (s.kind) {
    case ast::NodeKind::LetStmt: {
        const auto& l = static_cast<const ast::LetStmt&>(s);
        if (l.value && l.pattern != nullptr && l.pattern->kind == ast::NodeKind::WildcardPat) {
            check_expr(*l.value);
            reject_dropped_linear(*l.value);  // `let _ = mk()` discards it too
            break;
        }
        if (l.value) {
            check_expr(*l.value);
            // §5 — `let c = b` moves a non-trivial place: ownership transfers
            // to the new binding, so `b` is consumed (use `copy b` to keep
            // it). Symmetric with `return x` and sink-argument passing.
            reject_move_out(*l.value, l.value->range);
            consume_place(*l.value, l.value->range);
        }
        register_linear_binding(s);
        break;
    }
    case ast::NodeKind::VarStmt: {
        const auto& v = static_cast<const ast::VarStmt&>(s);
        if (v.value) {
            check_expr(*v.value);
            reject_move_out(*v.value, v.value->range);
            consume_place(*v.value, v.value->range);
        }
        register_linear_binding(s);
        break;
    }
    case ast::NodeKind::ExprStmt:
        check_expr(*static_cast<const ast::ExprStmt&>(s).expr);
        reject_dropped_linear(*static_cast<const ast::ExprStmt&>(s).expr);
        break;
    case ast::NodeKind::ReturnStmt: {
        const auto& r = static_cast<const ast::ReturnStmt&>(s);
        if (r.value) {
            check_expr(*r.value);
            // `return x` for a non-trivial place consumes the binding — the
            // function is handing ownership out to the caller.
            reject_move_out(*r.value, r.value->range);
            consume_place(*r.value, s.range);
        }
        break;
    }
    case ast::NodeKind::AssignStmt:
        check_place_assign(static_cast<const ast::AssignStmt&>(s));
        break;
    case ast::NodeKind::WhileStmt: {
        const auto& w = static_cast<const ast::WhileStmt&>(s);
        // A loop body runs again after its own writes, so no empty-place fact
        // crosses into it (or out of it).
        empty_places_.clear();
        BindingMap pre = bindings_;
        // `repeat` runs its body before the first test.
        if (!w.repeat) {
            check_expr(*w.cond);
        }
        if (w.body) {
            check_expr(*w.body);
        }
        if (w.repeat) {
            check_expr(*w.cond);
        }
        report_loop_moves(pre, w.body.get());
        empty_places_.clear();
        break;
    }
    case ast::NodeKind::ForStmt: {
        const auto& f = static_cast<const ast::ForStmt&>(s);
        empty_places_.clear();
        check_expr(*f.iter);
        BindingMap pre = bindings_;
        if (f.body) {
            check_expr(*f.body);
        }
        report_loop_moves(pre, f.body.get());
        empty_places_.clear();
        break;
    }
    case ast::NodeKind::WithStmt: {
        // `with` opens a capability or value scope; its body is ordinary
        // code and is checked like any block.
        const auto& w = static_cast<const ast::WithStmt&>(s);
        for (const auto& b : w.bindings) {
            if (b.value) {
                check_expr(*b.value);
            }
        }
        if (w.body) {
            check_expr(*w.body);
        }
        break;
    }
    case ast::NodeKind::AsmStmt: {
        // Operands are read (inputs) or written in place (outputs); none moves.
        const auto& a = static_cast<const ast::AsmStmt&>(s);
        for (const auto* ops : {&a.inputs, &a.outputs, &a.inouts}) {
            for (const auto& op : *ops) {
                if (op.value) {
                    check_expr(*op.value);
                }
            }
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
    case ast::NodeKind::MemberExpr: {
        const Symbol* root = nullptr;
        if (auto path = field_path(e, root)) {
            check_field_use(root, *path, e.range);
            break;
        }
        check_expr(*static_cast<const ast::MemberExpr&>(e).base);
        break;
    }
    case ast::NodeKind::IndexExpr: {
        const auto& ix = static_cast<const ast::IndexExpr&>(e);
        check_expr(*ix.base);
        for (const auto& i : ix.indices) {
            check_expr(*i);
        }
        break;
    }
    case ast::NodeKind::CopyExpr: {
        // `copy x` is a read of x — recurse so we still validate that x is
        // live, but never trigger a consume on it. The copied value itself
        // is a fresh temporary, not tracked.
        const auto& cx = static_cast<const ast::CopyExpr&>(e);
        check_expr(*cx.inner);
        // §19.2 well-formed only where the type is Copyable.
        auto t = resolution_->type_of(cx.inner.get());
        if (auto why = non_copyable_reason(t); !why.empty()) {
            reporter_->report(diag::Diagnostic::error(std::format("can't `copy` a value of type {}: "
                                                                  "it {}, so it is never duplicated",
                                                                  t->describe(),
                                                                  why))
                                  .at(e.range));
        }
        break;
    }
    case ast::NodeKind::AwaitExpr:
        check_expr(*static_cast<const ast::AwaitExpr&>(e).inner);
        break;
    case ast::NodeKind::SpawnExpr:
        check_expr(*static_cast<const ast::SpawnExpr&>(e).inner);
        break;
    case ast::NodeKind::ThrowExpr:
        check_expr(*static_cast<const ast::ThrowExpr&>(e).inner);
        break;
    case ast::NodeKind::TryExpr: {
        const auto& t = static_cast<const ast::TryExpr&>(e);
        check_expr(*t.inner);
        if (t.form == ast::TryExpr::Form::Propagating) {
            report_try_leaks(e.range);
        }
        break;
    }
    case ast::NodeKind::DoCatchExpr: {
        const auto& dc = static_cast<const ast::DoCatchExpr&>(e);
        do_bodies_.push_back(dc.do_body->range);
        check_expr(*dc.do_body);
        do_bodies_.pop_back();
        check_expr(*dc.catch_body);
        break;
    }
    case ast::NodeKind::AsExpr:
        check_expr(*static_cast<const ast::AsExpr&>(e).value);
        break;
    case ast::NodeKind::IfExpr: {
        const auto& i = static_cast<const ast::IfExpr&>(e);
        std::vector<PlaceKey> then_facts;
        std::vector<PlaceKey> else_facts;
        // §9 / §19.6 an `if let` over a linear payload: on a storage place it
        // borrows (the place keeps the value); otherwise the binding owns
        // what it unwraps and must consume it in the then-branch.
        const Symbol* let_sym = nullptr;
        bool let_owned = false;
        if (i.cond) {
            check_expr(*i.cond);
            collect_nil_facts(*i.cond, true, then_facts);
            collect_nil_facts(*i.cond, false, else_facts);
        } else if (i.let_init) {
            check_expr(*i.let_init);
            let_sym = resolution_->let_symbol(&i);
            const bool linear = let_sym != nullptr && is_linear_type(let_sym->type);
            if (linear && storage_place(*i.let_init).has_value()) {
                borrows_.insert(let_sym);
            } else if (linear) {
                if (borrows_.contains(place_symbol(*i.let_init))) {
                    reject_move_out(*i.let_init, i.let_init->range);
                }
                consume_place(*i.let_init, i.let_init->range);
                let_owned = true;
            }
        }
        const auto pre_facts = empty_places_;
        // §10/§5 phase 2 — branch-aware flow merge. Each branch is checked
        // from the same post-condition state, independently, then merged at
        // the join: a move in one branch no longer poisons the other, so
        // `if c { take(x) } else { take(x) }` is accepted. A binding moved
        // on a path reaching the join is treated as moved after the if
        // (a one-sided `if c { take(x) }` still forbids a later use of x),
        // which keeps the analysis sound.
        BindingMap pre = bindings_;
        empty_places_ = pre_facts;
        empty_places_.insert(empty_places_.end(), then_facts.begin(), then_facts.end());
        if (let_owned) {
            bindings_[let_sym].state = State::Live;
        }
        if (i.then_branch) {
            check_expr(*i.then_branch);
        }
        if (let_owned) {
            if (auto it = bindings_.find(let_sym);
                it != bindings_.end() && it->second.state == State::Live
                && !block_diverges(i.then_branch.get())) {
                reporter_->report(
                    diag::Diagnostic::error(
                        std::format("linear value '{}' is never consumed; `if let` moved it out of "
                                    "its Optional, so the then-branch must move it on",
                                    let_sym->name))
                        .at(i.range));
            }
            bindings_.erase(let_sym);
        }
        BindingMap then_state = std::move(bindings_);
        auto then_exit_facts = std::move(empty_places_);
        bindings_ = pre;
        empty_places_ = pre_facts;
        empty_places_.insert(empty_places_.end(), else_facts.begin(), else_facts.end());
        if (i.else_branch) {
            check_expr(*i.else_branch);
        }
        BindingMap else_state = bindings_;
        // Facts after the if: a branch that can't fall through contributes
        // nothing; otherwise only what both exits agree on survives.
        const bool then_out = block_diverges(i.then_branch.get());
        const bool else_out = i.else_branch != nullptr && block_diverges(i.else_branch.get());
        // A branch that leaves the function with a linear value still live
        // leaks it on that path.
        if (then_out && leaves_function(i.then_branch.get())) {
            report_path_leaks(then_state, i.then_branch->range);
        }
        if (else_out && leaves_function(i.else_branch.get())) {
            report_path_leaks(else_state, i.else_branch->range);
        }
        if (then_out && !else_out) {
            // keep the else-exit facts
        } else if (else_out && !then_out) {
            empty_places_ = std::move(then_exit_facts);
        } else {
            std::vector<PlaceKey> both;
            for (const auto& k : then_exit_facts) {
                for (const auto& m : empty_places_) {
                    if (k.render() == m.render()) {
                        both.push_back(k);
                        break;
                    }
                }
            }
            empty_places_ = std::move(both);
        }
        // §5/§19.6 — a linear binding live before the if must be consumed on
        // both paths or neither; consuming it on only one leaks the other.
        // Only paths that reach the join take part: one that returns, throws,
        // breaks or continues never meets the other.
        if (!then_out && !else_out) {
            report_linear_divergence(pre, {&then_state, &else_state}, e.range);
        }
        // bindings_ is now the else-path state (or `pre` when there is no
        // else); union in the then-path's moves if that path reaches here.
        if (else_out && !then_out) {
            bindings_ = std::move(then_state);
        } else if (!then_out) {
            merge_consumed(bindings_, then_state);
        }
        break;
    }
    case ast::NodeKind::MatchExpr: {
        const auto& m = static_cast<const ast::MatchExpr&>(e);
        check_expr(*m.scrutinee);
        // §10/§5 phase 2 — guards run in sequence before arm selection
        // (sound: assume each one ran), then every arm body is checked from
        // the resulting state and the bodies are merged at the join, so one
        // arm moving a binding doesn't poison another.
        for (const auto& arm : m.arms) {
            if (arm.guard) {
                check_expr(*arm.guard);
            }
        }
        BindingMap pre = bindings_;
        BindingMap merged = pre;
        std::vector<BindingMap> arm_states;
        for (const auto& arm : m.arms) {
            if (arm.body) {
                bindings_ = pre;
                check_expr(*arm.body);
                merge_consumed(merged, bindings_);
                arm_states.push_back(bindings_);
            }
        }
        // §5/§19.6 — a linear binding must be consumed in every arm or none.
        if (!arm_states.empty()) {
            std::vector<const BindingMap*> ptrs;
            ptrs.reserve(arm_states.size());
            for (const auto& st : arm_states) {
                ptrs.push_back(&st);
            }
            report_linear_divergence(pre, ptrs, e.range);
        }
        bindings_ = std::move(merged);
        empty_places_.clear();
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
    case ast::NodeKind::ClosureExpr:
        check_closure(static_cast<const ast::ClosureExpr&>(e));
        break;
    default:
        // Literals / SelfExpr / LeadingDotExpr / etc. have no
        // binding uses we model in phase 1.
        break;
    }
}

void OwnershipChecker::check_call(const ast::CallExpr& c) {
    if (check_optional_method(c)) {
        return;
    }
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
    bool is_constructor = false;
    if (callee_sym != nullptr) {
        if (callee_sym->decl != nullptr && callee_sym->decl->kind == ast::NodeKind::Func) {
            fn = static_cast<const ast::FuncDecl*>(callee_sym->decl);
        } else if (callee_sym->kind == SymbolKind::Struct || callee_sym->kind == SymbolKind::Enum
                   || callee_sym->kind == SymbolKind::EnumCase) {
            // §5 — struct/enum construction owns its inputs: each field/payload
            // argument is moved into the constructed value, so a non-trivial
            // place argument is consumed (just like a `sink` param). This is
            // what makes a struct with a linear field transitively linear —
            // the field's consume obligation transfers to the whole value.
            is_constructor = true;
        }
    }

    for (std::size_t i = 0; i < c.args.size(); ++i) {
        const auto& arg = c.args[i];
        check_expr(*arg.value);

        ast::ParamMode mode = ast::ParamMode::Read;
        if (is_constructor || builtin_sink_arg(c, i)) {
            mode = ast::ParamMode::Sink;
        } else if (fn != nullptr && i < fn->params.size()) {
            mode = fn->params[i].mode;
        } else if (arg.is_inout) {
            // An explicit `&x` at the call site signals an inout binding for
            // an unresolved callee. Treat it like an inout param (no consume).
            mode = ast::ParamMode::Inout;
        }
        if (mode == ast::ParamMode::Sink) {
            reject_move_out(*arg.value, arg.value->range);
            consume_place(*arg.value, arg.value->range);
        }
        // A callee handed `&place` may fill or empty anything under it.
        if (mode == ast::ParamMode::Inout || arg.is_inout) {
            if (auto k = storage_place(*arg.value)) {
                kill_rooted_at(k->parts.front().text);
            } else if (arg.value->kind == ast::NodeKind::IdentExpr) {
                kill_rooted_at(static_cast<const ast::IdentExpr&>(*arg.value).name);
            }
        }
    }
    // Any call may write a static.
    std::erase_if(empty_places_, [](const PlaceKey& k) { return k.root_is_static; });
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
    if (consume_field(e, site)) {
        return;
    }
    const auto* sym = place_symbol(e);
    if (sym == nullptr) {
        return;
    }
    auto [it, inserted] = bindings_.try_emplace(sym);
    if (!it->second.moved_fields.empty()) {
        return;  // check_use reported the partially moved value
    }
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

std::optional<std::string> OwnershipChecker::field_path(const ast::Expr& e,
                                                        const Symbol*& root) const {
    const ast::Expr* cur = &e;
    while (cur->kind == ast::NodeKind::ParenExpr) {
        cur = static_cast<const ast::ParenExpr&>(*cur).inner.get();
    }
    std::vector<std::string> parts;
    while (cur->kind == ast::NodeKind::MemberExpr) {
        const auto& m = static_cast<const ast::MemberExpr&>(*cur);
        if (m.is_optional_chain || resolution_->qualified_name_of(cur) != nullptr) {
            return std::nullopt;
        }
        auto t = resolution_->type_of(cur);
        if (t == nullptr || t->kind() == TypeKind::Function) {
            return std::nullopt;  // a method, not a field
        }
        // A raw pointer's pointee is RawMemory's, not a field.
        auto bt = resolution_->type_of(m.base.get());
        if (bt != nullptr && (bt->kind() == TypeKind::Ptr || bt->kind() == TypeKind::MutPtr)) {
            return std::nullopt;
        }
        parts.push_back(m.member);
        cur = m.base.get();
        while (cur->kind == ast::NodeKind::ParenExpr) {
            cur = static_cast<const ast::ParenExpr&>(*cur).inner.get();
        }
    }
    if (parts.empty() || cur->kind != ast::NodeKind::IdentExpr) {
        return std::nullopt;
    }
    const auto* sym = resolution_->symbol_of(cur);
    if (sym == nullptr || !is_trackable_kind(sym->kind)) {
        return std::nullopt;
    }
    root = sym;
    std::string path;
    for (auto it = parts.rbegin(); it != parts.rend(); ++it) {
        path += (path.empty() ? "" : ".") + *it;
    }
    return path;
}

bool OwnershipChecker::check_field_use(const Symbol* root,
                                       const std::string& path,
                                       diag::SourceRange site) {
    note_capture(root, site);
    auto it = bindings_.find(root);
    if (it == bindings_.end()) {
        return false;
    }
    if (it->second.state == State::Consumed) {
        reporter_->report(
            diag::Diagnostic::error(std::format("use of '{}' after it was moved", root->name))
                .at(site));
        return true;
    }
    for (const auto& [moved, at] : it->second.moved_fields) {
        const bool covers = path == moved || path.starts_with(moved + ".");
        const bool under = moved.starts_with(path + ".");
        if (covers) {
            reporter_->report(diag::Diagnostic::error(std::format("use of '{}.{}' after it was "
                                                                  "moved",
                                                                  root->name,
                                                                  path))
                                  .at(site));
            return true;
        }
        if (under) {
            reporter_->report(diag::Diagnostic::error(std::format("use of '{}.{}' after '{}.{}' "
                                                                  "was moved out of it",
                                                                  root->name,
                                                                  path,
                                                                  root->name,
                                                                  moved))
                                  .at(site));
            return true;
        }
    }
    return false;
}

bool OwnershipChecker::consume_field(const ast::Expr& e, diag::SourceRange site) {
    const Symbol* root = nullptr;
    auto path = field_path(e, root);
    if (!path || root == nullptr || is_trivial(resolution_->type_of(&e))
        || is_linear_type(resolution_->type_of(&e))) {
        return false;  // a linear field is the storage-place rules' (reject_move_out)
    }
    // An `if let` on a place only borrows it.
    if (borrows_.contains(root)) {
        reporter_->report(diag::Diagnostic::error(
                              std::format("can't move '{}.{}' out of '{}': it's borrowed from its "
                                          "place (an `if let` on a field or element); use `copy`",
                                          root->name,
                                          *path,
                                          root->name))
                              .at(site));
        return true;
    }
    // §6 a type with a `deinit` moves and is destroyed only as a whole: its
    // `deinit` needs every field. The outermost such container on the path
    // is the one named.
    {
        auto peel = [](const ast::Expr* x) {
            while (x->kind == ast::NodeKind::ParenExpr) {
                x = static_cast<const ast::ParenExpr&>(*x).inner.get();
            }
            return x;
        };
        std::vector<const ast::Expr*> bases;  // innermost container first
        const ast::Expr* cur = peel(&e);
        while (cur->kind == ast::NodeKind::MemberExpr) {
            cur = peel(static_cast<const ast::MemberExpr&>(*cur).base.get());
            bases.push_back(cur);
        }
        std::function<std::string(const ast::Expr*)> spell = [&](const ast::Expr* x) {
            x = peel(x);
            if (x->kind == ast::NodeKind::MemberExpr) {
                const auto& m = static_cast<const ast::MemberExpr&>(*x);
                return spell(m.base.get()) + "." + m.member;
            }
            return root->name;
        };
        for (auto it = bases.rbegin(); it != bases.rend(); ++it) {
            auto bt = resolution_->type_of(*it);
            const auto* d = bt != nullptr ? bt->nominal_decl() : nullptr;
            if (d == nullptr || d->kind != ast::NodeKind::Struct
                || static_cast<const ast::StructDecl*>(d)->deinit_body == nullptr) {
                continue;
            }
            reporter_->report(
                diag::Diagnostic::error(
                    std::format("can't move '{}.{}' out of '{}': type {} has a `deinit`, so it "
                                "moves only as a whole; use `copy`",
                                root->name,
                                *path,
                                spell(*it),
                                static_cast<const ast::StructDecl*>(d)->name))
                    .at(site));
            return true;
        }
    }
    // Moving out of a borrow: the caller still owns the value.
    if (root->kind == SymbolKind::Parameter && current_func_ != nullptr) {
        for (const auto& p : current_func_->params) {
            if (p.name == root->name && p.mode != ast::ParamMode::Sink) {
                reporter_->report(
                    diag::Diagnostic::error(
                        std::format("can't move '{}.{}' out of '{}': it's a `read` or `inout` "
                                    "parameter, so the caller still owns it; use `copy` or take "
                                    "it as `sink`",
                                    root->name,
                                    *path,
                                    root->name))
                        .at(site));
                return true;
            }
        }
    }
    auto& info = bindings_[root];
    for (const auto& [moved, at] : info.moved_fields) {
        if (*path == moved || path->starts_with(moved + ".")) {
            reporter_->report(diag::Diagnostic::error(std::format("'{}.{}' was already moved and "
                                                                  "cannot be moved again",
                                                                  root->name,
                                                                  *path))
                                  .at(site));
            return true;
        }
    }
    info.moved_fields[*path] = site;
    return true;
}

bool OwnershipChecker::inside(diag::SourceRange inner, diag::SourceRange outer) {
    return inner.is_valid() && outer.is_valid() && inner.begin.file == outer.begin.file
           && inner.begin.offset >= outer.begin.offset && inner.begin.offset < outer.end().offset;
}

void OwnershipChecker::note_capture(const Symbol* sym, diag::SourceRange site) {
    if (captures_.empty() || sym == nullptr || !is_trackable_kind(sym->kind)) {
        return;
    }
    auto& top = captures_.back();
    if (inside(sym->definition_range, top.range)) {
        return;  // the closure's own parameter or local
    }
    for (const auto& [s, at] : top.captures) {
        if (s == sym) {
            return;
        }
    }
    top.captures.emplace_back(sym, site);
}

// §7 a closure captures by value: a Trivial capture is copied, a non-Trivial
// one moves into the closure and ends the captured binding. The body is
// checked like a function body of its own: captures start as they are
// outside, its locals are tracked, and a linear local of the body must be
// consumed in it.
void OwnershipChecker::check_closure(const ast::ClosureExpr& cx) {
    BindingMap outer = bindings_;
    auto outer_facts = empty_places_;
    captures_.push_back(CaptureScope{cx.range, {}});
    for (const auto& st : cx.body) {
        check_stmt(*st);
    }
    for (const auto& [sym, info] : bindings_) {
        if (info.state == State::Live && sym != nullptr && sym->kind == SymbolKind::Local
            && inside(sym->definition_range, cx.range) && is_linear_type(sym->type)
            && !leak_reported_.contains(sym)) {
            reporter_->report(
                diag::Diagnostic::error(
                    std::format("linear value '{}' is never consumed; a linear value must be moved "
                                "to a sink parameter, returned, or destructured before its scope "
                                "ends",
                                sym->name))
                    .at(sym->definition_range));
        }
    }
    auto scope = std::move(captures_.back());
    captures_.pop_back();
    bindings_ = std::move(outer);
    empty_places_ = std::move(outer_facts);
    for (const auto& [sym, at] : scope.captures) {
        // A capture from further out is the enclosing closure's capture too.
        note_capture(sym, at);
        if (is_trivial(sym->type)) {
            continue;
        }
        // A borrowed value can't move into the closure.
        if (sym->kind == SymbolKind::Parameter && current_func_ != nullptr) {
            bool owned = true;
            for (const auto& p : current_func_->params) {
                if (p.name == sym->name && p.mode != ast::ParamMode::Sink) {
                    owned = false;
                }
            }
            if (!owned) {
                reporter_->report(
                    diag::Diagnostic::error(
                        std::format("a closure captures by value, and '{}' is a `read` or `inout` "
                                    "parameter, so it can't move in; `copy` it into a local first",
                                    sym->name))
                        .at(at));
                continue;
            }
        }
        if (borrows_.contains(sym)) {
            continue;  // reject_move_out-style borrow rules don't reach captures
        }
        auto& info = bindings_[sym];
        if (info.state == State::Live) {
            info.state = State::Consumed;
            info.consume_site = cx.range;
        }
    }
}

void OwnershipChecker::check_use(const ast::IdentExpr& ident) {
    const auto* sym = resolution_->symbol_of(&ident);
    note_capture(sym, ident.range);
    if (sym == nullptr || !is_trackable_kind(sym->kind) || is_trivial(sym->type)) {
        return;
    }
    auto it = bindings_.find(sym);
    if (it != bindings_.end() && it->second.state == State::Live
        && !it->second.moved_fields.empty()) {
        const auto& [f, at] = *it->second.moved_fields.begin();
        reporter_->report(diag::Diagnostic::error(std::format("use of '{}' after '{}.{}' was moved "
                                                              "out of it",
                                                              sym->name,
                                                              sym->name,
                                                              f))
                              .at(ident.range));
        return;
    }
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

// §10/§5 phase 2 — fold a branch's consumed-set into `dst`. A binding is
// Consumed in the merged state iff it is Consumed on either path, so a join
// reached by any path that moved x sees x as moved.
void OwnershipChecker::merge_consumed(BindingMap& dst, const BindingMap& branch) {
    for (const auto& [sym, info] : branch) {
        if (!info.moved_fields.empty()) {
            auto& slot = dst[sym];
            for (const auto& [f, at] : info.moved_fields) {
                slot.moved_fields.try_emplace(f, at);
            }
        }
        if (info.state != State::Consumed) {
            continue;
        }
        auto& slot = dst[sym];
        if (slot.state != State::Consumed) {
            slot.state = State::Consumed;
            slot.consume_site = info.consume_site;
        }
    }
}

bool OwnershipChecker::is_linear_type(TypePtr t) const noexcept {
    if (t == nullptr) {
        return false;
    }
    // §5 transitive through owning containers and fields, imported structs
    // included (vestra/sema/linearity.hpp).
    if (!linearity_.has_value()) {
        linearity_.emplace(resolution_->types());
    }
    if (linearity_->is_linear(t)) {
        return true;
    }
    // Without the arena's member types, the unit-local field closure.
    const ast::Decl* d = t->kind() == TypeKind::Struct ? t->nominal_decl() : nullptr;
    return d != nullptr && linear_decls_.contains(d);
}

void OwnershipChecker::compute_linear_decls() {
    // Index the unit's struct decls by name so a field type spelled `Foo` can
    // be matched to its declaration. v0.5 has a single flat module namespace,
    // so the last path component is enough; generic linear structs and
    // cross-module references are follow-ons.
    std::unordered_map<std::string_view, const ast::StructDecl*> by_name;
    for (const auto& d : unit_->decls) {
        if (d->kind == ast::NodeKind::Struct) {
            const auto& s = static_cast<const ast::StructDecl&>(*d);
            by_name.emplace(s.name, &s);
            if (s.is_linear) {
                linear_decls_.insert(d.get());
            }
        }
    }

    // The head-name of a field's type, or empty if it isn't a plain named
    // type (Optional / Span / Box / tuple etc. hold their element by value
    // but don't own a linear obligation through the wrapper in v0.5).
    auto field_struct_name = [](const ast::Type* ty) -> std::string_view {
        if (ty != nullptr && ty->kind == ast::NodeKind::NamedType) {
            const auto& n = static_cast<const ast::NamedType&>(*ty);
            if (!n.path.empty()) {
                return n.path.back();
            }
        }
        return {};
    };

    // Close the set under field ownership: a struct that has a field whose
    // type is a (transitively) linear struct is itself linear. Iterate to a
    // fixpoint so a chain Wrapper -> Inner -> Token propagates.
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& d : unit_->decls) {
            if (d->kind != ast::NodeKind::Struct || linear_decls_.contains(d.get())) {
                continue;
            }
            const auto& s = static_cast<const ast::StructDecl&>(*d);
            for (const auto& f : s.fields) {
                auto it = by_name.find(field_struct_name(f.type.get()));
                if (it != by_name.end() && linear_decls_.contains(it->second)) {
                    linear_decls_.insert(d.get());
                    changed = true;
                    break;
                }
            }
        }
    }
}

void OwnershipChecker::register_linear_binding(const ast::Stmt& s) {
    const Symbol* sym = resolution_->binding_symbol(&s);
    if (sym != nullptr && is_linear_type(sym->type)) {
        // `var o: Slot? = nil` holds nothing yet: it starts out as if moved.
        const ast::Expr* init = nullptr;
        if (s.kind == ast::NodeKind::LetStmt) {
            init = static_cast<const ast::LetStmt&>(s).value.get();
        } else if (s.kind == ast::NodeKind::VarStmt) {
            init = static_cast<const ast::VarStmt&>(s).value.get();
        }
        bindings_[sym].state =
            init != nullptr && init->kind == ast::NodeKind::NilLit ? State::Consumed : State::Live;
    }
}

void OwnershipChecker::report_linear_divergence(const BindingMap& pre,
                                                const std::vector<const BindingMap*>& branches,
                                                diag::SourceRange site) {
    for (const auto& [sym, info] : pre) {
        // Only bindings live going into the branch can leak on a path; one
        // already consumed before the branch is fine.
        if (info.state == State::Consumed || sym == nullptr || sym->kind == SymbolKind::Parameter
            || !is_linear_type(sym->type)) {
            continue;
        }
        bool any = false;
        bool all = true;
        for (const auto* b : branches) {
            auto it = b->find(sym);
            const bool consumed = it != b->end() && it->second.state == State::Consumed;
            any = any || consumed;
            all = all && consumed;
        }
        if (any && !all) {
            reporter_->report(
                diag::Diagnostic::error(
                    std::format("linear value '{}' is consumed on some branches but leaks on the "
                                "path(s) that do not consume it",
                                sym->name))
                    .at(site));
        }
    }
}

// ============================================================================
// Type / symbol predicates
// ============================================================================

bool OwnershipChecker::is_trivial(TypePtr t) noexcept {
    if (t == nullptr) {
        return true;  // unresolved type — don't pile on
    }
    // The Trivial set: every fixed-width numeric, Bool, Char, Unit, StrConst,
    // raw pointers and small structs of those. String owns a buffer, and a
    // Str is a view whose source the escape checker tracks.
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
    case TypeKind::Int128:
    case TypeKind::UInt128:
    case TypeKind::Float32:
    case TypeKind::Float64:
    case TypeKind::Bool:
    case TypeKind::Char:
    case TypeKind::Unit:
    case TypeKind::Never:
    case TypeKind::Error:
    // Annex §4 a `StrConst` is a view into .rodata, and a raw pointer is bits.
    case TypeKind::StrConst:
    case TypeKind::Ptr:
    case TypeKind::MutPtr:
        return true;
    case TypeKind::Struct:
        // §5 a small struct of Trivial fields (Resolver::mark_trivial_copy_structs).
        return t->nominal_decl() != nullptr && t->nominal_decl()->kind == ast::NodeKind::Struct
               && static_cast<const ast::StructDecl*>(t->nominal_decl())->trivial_copy;
    default:
        return false;
    }
}

bool OwnershipChecker::is_trackable_kind(SymbolKind k) noexcept {
    return k == SymbolKind::Local || k == SymbolKind::Parameter;
}

// ============================================================================
// §19.6 linear values in storage places (F17)
// ============================================================================

std::string OwnershipChecker::PlaceKey::render() const {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i == 0) {
            out = parts[i].text;
        } else if (parts[i].index) {
            out += "[" + parts[i].text + "]";
        } else {
            out += "." + parts[i].text;
        }
    }
    return out;
}

namespace {

const ast::Expr& strip_parens(const ast::Expr& e) {
    const ast::Expr* p = &e;
    while (p->kind == ast::NodeKind::ParenExpr) {
        p = static_cast<const ast::ParenExpr&>(*p).inner.get();
    }
    return *p;
}

// An index we can compare textually: names, integer literals, fields of
// names, and + - * of those. Collects the local names it mentions.
std::optional<std::string> render_index(const ast::Expr& e, std::vector<std::string>& idents) {
    const auto& x = strip_parens(e);
    switch (x.kind) {
    case ast::NodeKind::IdentExpr: {
        const auto& n = static_cast<const ast::IdentExpr&>(x).name;
        idents.push_back(n);
        return n;
    }
    case ast::NodeKind::IntLit:
        return static_cast<const ast::IntLit&>(x).text;
    case ast::NodeKind::MemberExpr: {
        const auto& m = static_cast<const ast::MemberExpr&>(x);
        auto base = render_index(*m.base, idents);
        if (!base) {
            return std::nullopt;
        }
        return *base + "." + m.member;
    }
    case ast::NodeKind::BinaryExpr: {
        const auto& b = static_cast<const ast::BinaryExpr&>(x);
        const char* op = b.op == ast::BinaryOp::Add   ? "+"
                         : b.op == ast::BinaryOp::Sub ? "-"
                         : b.op == ast::BinaryOp::Mul ? "*"
                                                      : nullptr;
        if (op == nullptr) {
            return std::nullopt;
        }
        auto l = render_index(*b.lhs, idents);
        auto r = render_index(*b.rhs, idents);
        if (!l || !r) {
            return std::nullopt;
        }
        return "(" + *l + " " + op + " " + *r + ")";
    }
    case ast::NodeKind::CallExpr: {
        // `Int(x)` and friends: a conversion of a renderable value.
        const auto& c = static_cast<const ast::CallExpr&>(x);
        if (c.callee->kind != ast::NodeKind::IdentExpr || c.args.size() != 1) {
            return std::nullopt;
        }
        auto a = render_index(*c.args[0].value, idents);
        if (!a) {
            return std::nullopt;
        }
        return static_cast<const ast::IdentExpr&>(*c.callee).name + "(" + *a + ")";
    }
    default:
        return std::nullopt;
    }
}

}  // namespace

std::optional<OwnershipChecker::PlaceKey> OwnershipChecker::storage_place(const ast::Expr& e) const {
    const auto& x = strip_parens(e);
    // Walk to the root, recording components outermost-last.
    std::vector<PlaceComponent> rev;
    const ast::Expr* cur = &x;
    while (true) {
        if (cur->kind == ast::NodeKind::MemberExpr) {
            const auto& m = static_cast<const ast::MemberExpr&>(*cur);
            if (m.is_optional_chain || resolution_->qualified_name_of(cur) != nullptr) {
                return std::nullopt;
            }
            // A raw pointer's pointee (`p.value`) is RawMemory's: whether it
            // holds a value is the caller's assertion (Annex §10.5), so the
            // linear-place rules don't track it. Writing a fresh value into
            // uninitialized @noinit storage is the common case.
            if (m.member == "value") {
                auto bt = resolution_->type_of(m.base.get());
                if (bt != nullptr
                    && (bt->kind() == TypeKind::Ptr || bt->kind() == TypeKind::MutPtr)) {
                    return std::nullopt;
                }
            }
            rev.push_back(PlaceComponent{false, m.member, {}});
            cur = &strip_parens(*m.base);
        } else if (cur->kind == ast::NodeKind::IndexExpr) {
            const auto& ix = static_cast<const ast::IndexExpr&>(*cur);
            if (ix.indices.size() != 1) {
                return std::nullopt;
            }
            PlaceComponent pc;
            pc.index = true;
            auto r = render_index(*ix.indices[0], pc.idents);
            if (!r) {
                return std::nullopt;
            }
            pc.text = std::move(*r);
            rev.push_back(std::move(pc));
            cur = &strip_parens(*ix.base);
        } else {
            break;
        }
    }
    PlaceKey key;
    if (cur->kind == ast::NodeKind::SelfExpr) {
        key.parts.push_back(PlaceComponent{false, "self", {}});
    } else if (cur->kind == ast::NodeKind::IdentExpr) {
        const auto* sym = resolution_->symbol_of(cur);
        if (sym == nullptr) {
            return std::nullopt;
        }
        const auto& name = static_cast<const ast::IdentExpr&>(*cur).name;
        if (rev.empty()) {
            // A bare name is storage only when this function doesn't own it:
            // a static, or a `read` / `inout` parameter.
            bool storage = sym->kind == SymbolKind::Static;
            if (sym->kind == SymbolKind::Parameter && current_func_ != nullptr) {
                for (const auto& p : current_func_->params) {
                    if (p.name == name && p.mode != ast::ParamMode::Sink) {
                        storage = true;
                    }
                }
            }
            if (!storage) {
                return std::nullopt;
            }
        }
        key.root_is_static = sym->kind == SymbolKind::Static;
        key.parts.push_back(PlaceComponent{false, name, {}});
    } else {
        return std::nullopt;
    }
    for (auto it = rev.rbegin(); it != rev.rend(); ++it) {
        key.parts.push_back(*it);
    }
    return key;
}

bool OwnershipChecker::may_alias(const PlaceKey& a, const PlaceKey& b) {
    const std::size_t n = std::min(a.parts.size(), b.parts.size());
    for (std::size_t i = 0; i < n; ++i) {
        const auto& x = a.parts[i];
        const auto& y = b.parts[i];
        if (x.index != y.index) {
            return false;
        }
        if (x.text == y.text) {
            continue;
        }
        // Two elements of one array may be the same element; two different
        // fields (or roots) never are.
        return x.index;
    }
    return true;  // one is a prefix of the other
}

void OwnershipChecker::kill_aliasing(const PlaceKey& k) {
    std::erase_if(empty_places_, [&](const PlaceKey& f) { return may_alias(k, f); });
}

void OwnershipChecker::kill_mentioning(std::string_view local) {
    std::erase_if(empty_places_, [&](const PlaceKey& f) {
        for (const auto& p : f.parts) {
            if ((&p == &f.parts.front() && p.text == local)
                || std::ranges::find(p.idents, local) != p.idents.end()) {
                return true;
            }
        }
        return false;
    });
}

void OwnershipChecker::kill_rooted_at(std::string_view root) {
    std::erase_if(empty_places_, [&](const PlaceKey& f) { return f.parts.front().text == root; });
}

bool OwnershipChecker::known_empty(const PlaceKey& k) const {
    const auto r = k.render();
    return std::ranges::any_of(empty_places_, [&](const PlaceKey& f) { return f.render() == r; });
}

bool OwnershipChecker::is_linear_place_value(const ast::Expr& e) const {
    return is_linear_type(resolution_->type_of(&e));
}

void OwnershipChecker::reject_move_out(const ast::Expr& e, diag::SourceRange site) {
    const auto& x = strip_parens(e);
    if (x.kind == ast::NodeKind::IdentExpr) {
        const auto* sym = resolution_->symbol_of(&x);
        if (sym != nullptr && borrows_.contains(sym)) {
            reporter_->report(
                diag::Diagnostic::error(
                    std::format("'{}' is borrowed from its place (an `if let` on a field or "
                                "element doesn't move the value); move it out with "
                                "`if let {} = <place>.take()`",
                                sym->name,
                                sym->name))
                    .at(site));
            return;
        }
    }
    auto k = storage_place(x);
    if (!k || !is_linear_place_value(x)) {
        return;
    }
    auto t = resolution_->type_of(&x);
    const auto place = k->render();
    if (k->parts.size() == 1 && !k->root_is_static) {
        reporter_->report(
            diag::Diagnostic::error(
                std::format("can't move the linear value '{}': it's a `read` or `inout` "
                            "parameter, so the caller still owns it; take it as `sink` to move it",
                            place))
                .at(site));
        return;
    }
    if (t != nullptr && t->kind() == TypeKind::Optional) {
        reporter_->report(
            diag::Diagnostic::error(
                std::format("can't move the linear value out of '{}': the place would still hold "
                            "it; use '{}.take()', which moves it out and leaves the place empty",
                            place,
                            place))
                .at(site));
    } else {
        reporter_->report(
            diag::Diagnostic::error(
                std::format("can't move the linear value out of '{}': the place would still hold "
                            "it; keep it in an Optional and move it out with '.take()', or move "
                            "the value that owns the place",
                            place))
                .at(site));
    }
}

void OwnershipChecker::check_place_assign(const ast::AssignStmt& a) {
    check_expr(*a.value);
    const bool value_is_nil = strip_parens(*a.value).kind == ast::NodeKind::NilLit;
    // `x = y` moves a linear `y`.
    if (is_linear_place_value(*a.value)) {
        reject_move_out(*a.value, a.value->range);
        consume_place(*a.value, a.value->range);
    }
    if (a.target == nullptr) {
        return;
    }
    const auto& target = strip_parens(*a.target);
    // §5 assigning a moved field (or a whole binding) gives it a value again.
    {
        const Symbol* root = nullptr;
        if (auto path = field_path(target, root); path && root != nullptr) {
            if (auto it = bindings_.find(root); it != bindings_.end()) {
                std::erase_if(it->second.moved_fields, [&](const auto& kv) {
                    return kv.first == *path || kv.first.starts_with(*path + ".");
                });
            }
        } else if (target.kind == ast::NodeKind::IdentExpr) {
            if (const auto* sym = resolution_->symbol_of(&target)) {
                if (auto it = bindings_.find(sym); it != bindings_.end()) {
                    it->second.moved_fields.clear();
                }
            }
        }
    }
    if (auto k = storage_place(target)) {
        check_expr(target);
        if (is_linear_place_value(target)) {
            auto t = resolution_->type_of(&target);
            const bool optional = t != nullptr && t->kind() == TypeKind::Optional;
            if (!optional || !known_empty(*k)) {
                const auto place = k->render();
                reporter_->report(
                    diag::Diagnostic::error(
                        optional
                            ? std::format(
                                  "assigning to '{}' could drop the linear value it holds; assign "
                                  "only where it's known to be empty (inside `if {} == nil`, or "
                                  "after '{}.take()'), or use '{}.put(v)', which checks at run "
                                  "time",
                                  place,
                                  place,
                                  place,
                                  place)
                            : std::format("assigning to '{}' drops the linear value it holds; "
                                          "move the old value out first",
                                          place))
                        .at(a.range));
            }
        }
        kill_aliasing(*k);
        if (value_is_nil) {
            empty_places_.push_back(*k);
        }
        return;
    }
    if (target.kind == ast::NodeKind::IdentExpr) {
        const auto* sym = resolution_->symbol_of(&target);
        kill_mentioning(static_cast<const ast::IdentExpr&>(target).name);
        if (sym == nullptr) {
            return;
        }
        auto it = bindings_.find(sym);
        if (is_linear_type(sym->type)) {
            if (it != bindings_.end() && it->second.state == State::Live) {
                reporter_->report(
                    diag::Diagnostic::error(
                        std::format("assigning to '{}' drops the linear value it still holds; "
                                    "move that value on first",
                                    sym->name))
                        .at(a.range));
            }
            bindings_[sym].state = value_is_nil ? State::Consumed : State::Live;
            bindings_[sym].consume_site = {};
            return;
        }
        // Reassignment to a `var` revives a consumed binding (§19.2).
        if (it != bindings_.end()) {
            it->second.state = State::Live;
            it->second.consume_site = {};
        }
        return;
    }
    check_expr(target);
}

bool OwnershipChecker::builtin_sink_arg(const ast::CallExpr& c, std::size_t i) const {
    if (i != 0 || c.args.size() != 1 || c.callee == nullptr
        || c.callee->kind != ast::NodeKind::MemberExpr) {
        return false;
    }
    const auto& m = static_cast<const ast::MemberExpr&>(*c.callee);
    if (m.base == nullptr) {
        return false;
    }
    if (auto bt = resolution_->type_of(m.base.get())) {
        if (bt->kind() == TypeKind::Channel) {
            return m.member == "send";
        }
        if (bt->kind() == TypeKind::Vec) {
            return m.member == "push";  // §18 `push(_ x: sink T)`
        }
    }
    // `Box.new(x)`: the base names the type, not a value.
    const auto& base = strip_parens(*m.base);
    if (base.kind == ast::NodeKind::IdentExpr && m.member == "new"
        && static_cast<const ast::IdentExpr&>(base).name == "Box") {
        const auto* sym = resolution_->symbol_of(&base);
        return sym == nullptr || sym->kind == SymbolKind::Struct;
    }
    return false;
}

bool OwnershipChecker::check_optional_method(const ast::CallExpr& c) {
    if (c.callee == nullptr || c.callee->kind != ast::NodeKind::MemberExpr) {
        return false;
    }
    const auto& m = static_cast<const ast::MemberExpr&>(*c.callee);
    const bool take = m.member == "take" && c.args.empty();
    const bool put = m.member == "put" && c.args.size() == 1;
    if (!take && !put) {
        return false;
    }
    auto bt = resolution_->type_of(m.base.get());
    if (bt == nullptr || bt->kind() != TypeKind::Optional) {
        return false;
    }
    const auto& base = strip_parens(*m.base);
    auto k = storage_place(base);
    const Symbol* local = nullptr;
    if (!k && base.kind == ast::NodeKind::IdentExpr) {
        local = resolution_->symbol_of(&base);
    }
    if (!k && local == nullptr) {
        reporter_->report(
            diag::Diagnostic::error(std::format("'{}()' needs a place (a variable, field or "
                                                "element) to {}",
                                                m.member,
                                                take ? "empty" : "fill"))
                .at(c.range));
        return true;
    }
    if (k) {
        check_expr(base);
    }
    const bool linear = is_linear_type(bt);
    if (take) {
        if (k) {
            kill_aliasing(*k);
            empty_places_.push_back(*k);
        } else if (linear) {
            auto& b = bindings_[local];
            b.state = State::Consumed;
            b.consume_site = {};
        }
        return true;
    }
    const auto& arg = *c.args[0].value;
    check_expr(arg);
    if (is_linear_place_value(arg)) {
        reject_move_out(arg, arg.range);
        consume_place(arg, arg.range);
    }
    if (k) {
        kill_aliasing(*k);
    } else if (linear) {
        bindings_[local].state = State::Live;
    }
    return true;
}

void OwnershipChecker::collect_nil_facts(const ast::Expr& cond,
                                         bool want_nil,
                                         std::vector<PlaceKey>& out) const {
    const auto& x = strip_parens(cond);
    if (x.kind != ast::NodeKind::BinaryExpr) {
        return;
    }
    const auto& b = static_cast<const ast::BinaryExpr&>(x);
    if (want_nil && b.op == ast::BinaryOp::And) {
        collect_nil_facts(*b.lhs, true, out);
        collect_nil_facts(*b.rhs, true, out);
        return;
    }
    const auto wanted = want_nil ? ast::BinaryOp::Eq : ast::BinaryOp::Ne;
    if (b.op != wanted) {
        return;
    }
    const ast::Expr* other = nullptr;
    if (strip_parens(*b.rhs).kind == ast::NodeKind::NilLit) {
        other = b.lhs.get();
    } else if (strip_parens(*b.lhs).kind == ast::NodeKind::NilLit) {
        other = b.rhs.get();
    }
    if (other != nullptr) {
        if (auto k = storage_place(*other)) {
            out.push_back(std::move(*k));
        }
    }
}

bool OwnershipChecker::block_diverges(const ast::Expr* e) const {
    if (e == nullptr) {
        return false;
    }
    if (auto t = resolution_->type_of(e); t != nullptr && t->is_never()) {
        return true;
    }
    if (e->kind != ast::NodeKind::BlockExpr) {
        return false;
    }
    const auto& b = static_cast<const ast::BlockExpr&>(*e);
    if (b.stmts.empty()) {
        return false;
    }
    const auto& last = *b.stmts.back();
    switch (last.kind) {
    case ast::NodeKind::ReturnStmt:
    case ast::NodeKind::BreakStmt:
    case ast::NodeKind::ContinueStmt:
        return true;
    case ast::NodeKind::ExprStmt: {
        const auto* ex = static_cast<const ast::ExprStmt&>(last).expr.get();
        if (ex == nullptr) {
            return false;
        }
        if (ex->kind == ast::NodeKind::ThrowExpr) {
            return true;
        }
        auto t = resolution_->type_of(ex);
        return t != nullptr && t->is_never();
    }
    default:
        return false;
    }
}

bool OwnershipChecker::leaves_function(const ast::Expr* e) const {
    if (e == nullptr || e->kind != ast::NodeKind::BlockExpr) {
        return e != nullptr && block_diverges(e);
    }
    const auto& b = static_cast<const ast::BlockExpr&>(*e);
    if (b.stmts.empty()) {
        return false;
    }
    const auto k = b.stmts.back()->kind;
    return k != ast::NodeKind::BreakStmt && k != ast::NodeKind::ContinueStmt;
}

std::string OwnershipChecker::non_copyable_reason(TypePtr t, int depth) const {
    if (t == nullptr || depth > 16) {
        return {};
    }
    if (is_linear_type(t)) {
        return "is linear";
    }
    switch (t->kind()) {
    case TypeKind::Box:
        return "owns a Box";
    case TypeKind::Optional:
    case TypeKind::Vector:
    case TypeKind::Vec:
    case TypeKind::Padded:
        return non_copyable_reason(t->inner(), depth + 1);
    case TypeKind::Tuple:
    case TypeKind::HashMap:
        if (auto r = non_copyable_reason(t->inner(), depth + 1); !r.empty()) {
            return r;
        }
        for (auto p : t->parts()) {
            if (auto r = non_copyable_reason(p, depth + 1); !r.empty()) {
                return r;
            }
        }
        return {};
    case TypeKind::Struct: {
        const auto* d = t->nominal_decl();
        if (d == nullptr || d->kind != ast::NodeKind::Struct) {
            return {};
        }
        const auto& s = static_cast<const ast::StructDecl&>(*d);
        if (s.deinit_body != nullptr) {
            return "has a deinit";
        }
        for (const auto& f : s.fields) {
            if (auto ft = resolution_->types()->member_type(f.type.get())) {
                if (auto r = non_copyable_reason(ft, depth + 1); !r.empty()) {
                    return r;
                }
            }
        }
        return {};
    }
    case TypeKind::Enum: {
        const auto* d = t->nominal_decl();
        if (d == nullptr || d->kind != ast::NodeKind::Enum) {
            return {};
        }
        for (const auto& c : static_cast<const ast::EnumDecl&>(*d).cases) {
            for (const auto& [label, pt] : c.payload) {
                if (auto ct = resolution_->types()->member_type(pt.get())) {
                    if (auto r = non_copyable_reason(ct, depth + 1); !r.empty()) {
                        return r;
                    }
                }
            }
        }
        return {};
    }
    default:
        return {};
    }
}

void OwnershipChecker::report_try_leaks(diag::SourceRange site) {
    const bool in_do = !do_bodies_.empty();
    for (const auto& [sym, info] : bindings_) {
        if (info.state != State::Live || sym == nullptr || sym->kind == SymbolKind::Parameter
            || !is_linear_type(sym->type) || borrows_.contains(sym) || leak_reported_.contains(sym)) {
            continue;
        }
        // Thrown to a catch, only what the `do` body declared is left behind.
        if (in_do && !inside(sym->definition_range, do_bodies_.back())) {
            continue;
        }
        leak_reported_.insert(sym);
        reporter_->report(
            diag::Diagnostic::error(std::format("linear value '{}' leaks if the `try` throws, which "
                                                "leaves the {} without consuming it",
                                                sym->name,
                                                in_do ? "`do` body" : "function"))
                .at(site));
    }
}

void OwnershipChecker::reject_dropped_linear(const ast::Expr& e) {
    const ast::Expr* x = &e;
    while (x->kind == ast::NodeKind::ParenExpr) {
        x = static_cast<const ast::ParenExpr&>(*x).inner.get();
    }
    if (x->kind == ast::NodeKind::TryExpr) {
        x = static_cast<const ast::TryExpr&>(*x).inner.get();
    }
    if (x->kind != ast::NodeKind::CallExpr) {
        return;
    }
    auto t = resolution_->type_of(&e);
    if (t != nullptr && t->kind() == TypeKind::Result) {
        t = t->inner();
    }
    if (!is_linear_type(t)) {
        return;
    }
    reporter_->report(diag::Diagnostic::error(std::format("the linear value this call returns ({}) "
                                                          "is dropped; bind it and consume it",
                                                          t->describe()))
                          .at(e.range));
}

void OwnershipChecker::report_loop_moves(const BindingMap& pre, const ast::Expr* body) {
    if (body == nullptr || block_diverges(body)) {
        return;  // a body that can't fall through never runs a second time
    }
    for (const auto& [sym, info] : bindings_) {
        if (sym == nullptr) {
            continue;
        }
        // Declared inside the body: a fresh binding each iteration.
        if (sym->definition_range.is_valid() && body->range.is_valid()
            && sym->definition_range.begin.offset >= body->range.begin.offset
            && sym->definition_range.begin.offset < body->range.end().offset) {
            continue;
        }
        // A field moved out in the body, not before the loop.
        if (info.state == State::Live && !info.moved_fields.empty()) {
            auto pit = pre.find(sym);
            for (const auto& [f, at] : info.moved_fields) {
                if (pit != pre.end() && pit->second.moved_fields.contains(f)) {
                    continue;
                }
                reporter_->report(
                    diag::Diagnostic::error(std::format("'{}.{}' is moved inside a loop, so the "
                                                        "next iteration would use it after the "
                                                        "move; move it on a path that leaves the "
                                                        "loop, or reassign it before the loop "
                                                        "repeats",
                                                        sym->name,
                                                        f))
                        .at(at));
            }
            continue;
        }
        if (info.state != State::Consumed) {
            continue;
        }
        auto it = pre.find(sym);
        if (it != pre.end() && it->second.state == State::Consumed) {
            continue;  // already moved before the loop: reported as a use
        }
        reporter_->report(
            diag::Diagnostic::error(std::format("'{}' is moved inside a loop, so the next "
                                                "iteration would use it after the move; move it "
                                                "on a path that leaves the loop, or reassign it "
                                                "before the loop repeats",
                                                sym->name))
                .at(info.consume_site.is_valid() ? info.consume_site : body->range));
    }
}

void OwnershipChecker::report_path_leaks(const BindingMap& state, diag::SourceRange site) {
    for (const auto& [sym, info] : state) {
        if (info.state == State::Live && sym != nullptr && sym->kind != SymbolKind::Parameter
            && is_linear_type(sym->type)
            && !borrows_.contains(sym) && leak_reported_.insert(sym).second) {
            reporter_->report(
                diag::Diagnostic::error(
                    std::format("linear value '{}' leaks on this path, which leaves the function "
                                "without consuming it",
                                sym->name))
                    .at(site));
        }
    }
}

}  // namespace vestra::sema
