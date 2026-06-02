// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// Match / select / pattern checking and tuple-pattern binding.
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

void Resolver::bind_tuple_pattern(const ast::TuplePat& pat, TypePtr value_type) {
    if (value_type == nullptr || value_type->is_error()) {
        return;  // suppress cascading errors
    }
    if (value_type->kind() != TypeKind::Tuple) {
        error_at(
            pat.range,
            std::format("tuple pattern requires a tuple value, got {}", value_type->describe()));
        return;
    }
    const auto& parts = value_type->parts();
    if (parts.size() != pat.elements.size()) {
        error_at(pat.range,
                 std::format("tuple pattern has {} elements, value has {}",
                             pat.elements.size(),
                             parts.size()));
        return;
    }
    for (std::size_t i = 0; i < pat.elements.size(); ++i) {
        const auto& sub = pat.elements[i];
        if (!sub) {
            continue;
        }
        std::string_view name;
        if (sub->kind == ast::NodeKind::IdentPat) {
            name = static_cast<const ast::IdentPat&>(*sub).name;
        } else if (sub->kind == ast::NodeKind::BindPat) {
            name = static_cast<const ast::BindPat&>(*sub).name;
        } else if (sub->kind == ast::NodeKind::WildcardPat) {
            continue;
        } else if (sub->kind == ast::NodeKind::TuplePat) {
            bind_tuple_pattern(static_cast<const ast::TuplePat&>(*sub), parts[i]);
            continue;
        } else if (sub->kind == ast::NodeKind::LiteralPat || sub->kind == ast::NodeKind::RangePat
                   || sub->kind == ast::NodeKind::OrPat) {
            // §17.7 value-shape patterns inside a tuple pattern: each
            // sub-pattern type-checks against the corresponding element
            // type; no binding is introduced.
            check_pattern(*sub, parts[i]);
            continue;
        } else {
            error_at(sub->range,
                     "v0.5 only supports identifier / `_` / nested-tuple / literal / "
                     "range / or-pattern element kinds inside a tuple pattern");
            continue;
        }
        if (name.empty()) {
            continue;
        }
        Symbol sym;
        sym.name = std::string{name};
        sym.kind = SymbolKind::Local;
        sym.type = parts[i];
        sym.definition_range = sub->range;
        if (auto* prev = scopes_.current().insert(std::move(sym))) {
            duplicate_definition(*prev, sym.name, sub->range);
        }
    }
}

// §5/§18.4 partition provenance. Annotates the parent-place provenance of a
// `split(at:)` / `chunks(of:)` binding (or a plain alias of an existing
// sub-view) directly on the bound Symbol, so the exclusivity checker can flag
// a borrow of a sub-view that aliases a borrow of its parent.
void Resolver::record_partition_provenance(const ast::Pattern* pat, const ast::Expr* init) {
    if (pat == nullptr || init == nullptr) {
        return;
    }
    // Unwrap a parenthesized initializer.
    while (init->kind == ast::NodeKind::ParenExpr) {
        init = static_cast<const ast::ParenExpr&>(*init).inner.get();
    }

    // Annotate the binding a single-name pattern introduces. Looked up
    // mutably in the current scope (the caller has just inserted it).
    auto annotate = [&](const ast::Pattern& p, const Symbol* root, std::string segment) {
        std::string_view nm;
        if (p.kind == ast::NodeKind::IdentPat) {
            nm = static_cast<const ast::IdentPat&>(p).name;
        } else if (p.kind == ast::NodeKind::BindPat) {
            nm = static_cast<const ast::BindPat&>(p).name;
        }
        if (nm.empty()) {
            return;
        }
        if (Symbol* leaf = scopes_.current().lookup_mutable(nm)) {
            leaf->provenance_root = root;
            leaf->provenance_segment = std::move(segment);
        }
    };

    // Case 1/2: the initializer is a `split` / `chunks` member-call on a place.
    if (init->kind == ast::NodeKind::CallExpr) {
        const auto& call = static_cast<const ast::CallExpr&>(*init);
        if (call.callee != nullptr && call.callee->kind == ast::NodeKind::MemberExpr) {
            const auto& mem = static_cast<const ast::MemberExpr&>(*call.callee);
            if (mem.base != nullptr) {
                const Symbol* parent = resolution_.symbol_of(mem.base.get());
                if (parent != nullptr
                    && (parent->kind == SymbolKind::Local
                        || parent->kind == SymbolKind::Parameter)) {
                    if (mem.member == "split" && pat->kind == ast::NodeKind::TuplePat) {
                        const auto& tp = static_cast<const ast::TuplePat&>(*pat);
                        for (std::size_t i = 0; i < tp.elements.size(); ++i) {
                            if (tp.elements[i] != nullptr) {
                                annotate(*tp.elements[i], parent, "@split" + std::to_string(i));
                            }
                        }
                        return;
                    }
                    if (mem.member == "chunks") {
                        annotate(*pat, parent, "@chunks");
                        return;
                    }
                }
            }
        }
    }

    // Case 3: a plain alias `let x = derived` where `derived` is itself a
    // tracked sub-view. The alias inherits the same (root, segment), so it
    // conflicts with the parent exactly as `derived` does.
    if (init->kind == ast::NodeKind::IdentExpr) {
        if (const Symbol* src = resolution_.symbol_of(init);
            src != nullptr && src->provenance_root != nullptr) {
            annotate(*pat, src->provenance_root, src->provenance_segment);
        }
    }
}

TypePtr Resolver::check_match(const ast::MatchExpr& m, TypePtr expected) {
    auto scrutinee_type = check_expr(*m.scrutinee);

    // Track which enum cases got covered for the exhaustiveness check.
    const ast::EnumDecl* scrutinee_enum = nullptr;
    if (scrutinee_type != nullptr && scrutinee_type->kind() == TypeKind::Enum
        && scrutinee_type->nominal_decl() != nullptr) {
        scrutinee_enum = &static_cast<const ast::EnumDecl&>(*scrutinee_type->nominal_decl());
    }
    std::vector<bool> case_seen(scrutinee_enum ? scrutinee_enum->cases.size() : 0, false);
    bool saw_default = false;

    // §17.7 enum-coverage walk: drain through OrPat alternatives so a
    // `case .red | .green:` arm marks both cases as covered. Wildcard
    // and unrelated patterns terminate the walk on this arm.
    auto mark_enum_covered = [&](const ast::Pattern& p, auto& self) -> void {
        if (scrutinee_enum == nullptr) {
            return;
        }
        if (p.kind == ast::NodeKind::WildcardPat) {
            for (auto&& s : case_seen) {
                s = true;
            }
            return;
        }
        if (p.kind == ast::NodeKind::EnumPat) {
            const auto& ep = static_cast<const ast::EnumPat&>(p);
            for (std::size_t i = 0; i < scrutinee_enum->cases.size(); ++i) {
                if (scrutinee_enum->cases[i].name == ep.case_name) {
                    case_seen[i] = true;
                    break;
                }
            }
            return;
        }
        if (p.kind == ast::NodeKind::OrPat) {
            for (const auto& alt : static_cast<const ast::OrPat&>(p).alternatives) {
                self(*alt, self);
            }
        }
    };

    // §8 dead-code arm detection. Two flavors of unreachability we
    // can pin at compile time:
    //   - any arm after a default / catch-all wildcard is dead (the
    //     default fires for everything that didn't match earlier).
    //   - an enum-pattern arm whose case_name was already closed by
    //     an earlier *unguarded* arm targeting the same case_name is
    //     dead; the codegen groups arms by case_name and the
    //     unguarded arm dominates anything after it in the group.
    // The warning surfaces the user's mistake at compile time
    // without failing the build; refactoring patterns where this
    // happens silently is a footgun the §8 follow-up commit
    // explicitly flagged for this slice to close.
    bool seen_catchall = false;
    std::unordered_set<std::string> closed_cases;
    auto register_unguarded_cases = [&](const ast::Pattern& p, auto& self) -> void {
        if (p.kind == ast::NodeKind::EnumPat) {
            const auto& ep = static_cast<const ast::EnumPat&>(p);
            closed_cases.insert(ep.case_name);
            return;
        }
        if (p.kind == ast::NodeKind::OrPat) {
            for (const auto& alt : static_cast<const ast::OrPat&>(p).alternatives) {
                if (alt) {
                    self(*alt, self);
                }
            }
        }
    };

    TypePtr result_type = nullptr;
    for (const auto& arm : m.arms) {
        ScopeStack::Guard g(scopes_);
        // §8 dead-arm warning before processing the arm. A previous
        // catch-all dominates everything after it regardless of the
        // current arm's pattern.
        if (seen_catchall) {
            // MatchArm itself has no range; anchor the warning on the
            // best per-arm range available (body, pattern, or the
            // surrounding match's range as the last resort).
            diag::SourceRange anchor = arm.pattern ? arm.pattern->range
                                       : arm.body  ? arm.body->range
                                                   : m.range;
            warn_at(anchor,
                    "match arm is unreachable: a previous default/wildcard arm dominates "
                    "every following case");
        } else if (arm.pattern != nullptr && arm.pattern->kind == ast::NodeKind::EnumPat
                   && !arm.guard) {
            const auto& ep = static_cast<const ast::EnumPat&>(*arm.pattern);
            if (closed_cases.contains(ep.case_name)) {
                warn_at(arm.pattern->range,
                        std::format("match arm is unreachable: a previous unguarded arm for "
                                    "case '.{}' dominates this one",
                                    ep.case_name));
            }
        } else if (arm.pattern != nullptr && arm.pattern->kind == ast::NodeKind::EnumPat
                   && arm.guard) {
            const auto& ep = static_cast<const ast::EnumPat&>(*arm.pattern);
            if (closed_cases.contains(ep.case_name)) {
                warn_at(arm.pattern->range,
                        std::format("match arm is unreachable: a previous unguarded arm for "
                                    "case '.{}' dominates this one (the guard never runs)",
                                    ep.case_name));
            }
        }
        if (arm.is_default) {
            saw_default = true;
            seen_catchall = true;
        } else if (arm.pattern) {
            check_pattern(*arm.pattern, scrutinee_type);
            mark_enum_covered(*arm.pattern, mark_enum_covered);
            // A wildcard arm closes everything (acts as default).
            if (arm.pattern->kind == ast::NodeKind::WildcardPat) {
                seen_catchall = true;
            }
            // §8 close the case_name(s) when the arm is unguarded.
            // The closure walks through OrPat so `case .a | .b` closes
            // both names; a single EnumPat closes its one name.
            if (!arm.guard) {
                register_unguarded_cases(*arm.pattern, register_unguarded_cases);
            }
        }
        if (arm.guard) {
            auto gt = check_expr(*arm.guard, types_->boolean());
            if (gt != nullptr && gt->kind() != TypeKind::Bool && !gt->is_error()) {
                error_at(arm.guard->range,
                         std::format("match guard must be Bool, got {}", gt->describe()));
            }
        }
        TypePtr arm_type = arm.body ? check_expr(*arm.body, expected) : types_->unit();
        if (result_type == nullptr) {
            result_type = arm_type;
        } else if (arm_type != nullptr && !arm_type->is_never() && !result_type->is_never()
                   && !TypeArena::equal(arm_type, result_type)) {
            error_at(arm.body ? arm.body->range : m.range,
                     std::format("match arms have different types: {} vs {}",
                                 result_type->describe(),
                                 arm_type->describe()));
            result_type = types_->error();
        }
    }

    // Exhaustiveness: when the scrutinee is an enum and there's no `default`,
    // every case must be covered by at least one arm.
    if (scrutinee_enum != nullptr && !saw_default) {
        for (std::size_t i = 0; i < scrutinee_enum->cases.size(); ++i) {
            if (!case_seen[i]) {
                error_at(m.range,
                         std::format("match is not exhaustive: case '{}' is not covered",
                                     scrutinee_enum->cases[i].name));
            }
        }
    }

    return result_type != nullptr ? result_type : types_->unit();
}

TypePtr Resolver::check_select(const ast::SelectExpr& sel, TypePtr expected) {
    TypePtr result_type = nullptr;
    auto join = [&](TypePtr arm_type, diag::SourceRange r) {
        if (result_type == nullptr) {
            result_type = arm_type;
        } else if (arm_type != nullptr && !arm_type->is_never() && !result_type->is_never()
                   && !TypeArena::equal(arm_type, result_type)) {
            error_at(r,
                     std::format("select arms have different types: {} vs {}",
                                 result_type->describe(),
                                 arm_type->describe()));
            result_type = types_->error();
        }
    };
    // A channel-receive arm event is syntactically `<chan>.receive()` — a call
    // whose callee is a `.receive` member access. Its type is the channel's
    // T? (slice 2), and the arm pattern binds to that whole optional (nil means
    // the channel is closed and drained), matching `await ch.receive()`.
    auto is_channel_receive = [](const ast::Expr& ev) {
        if (ev.kind != ast::NodeKind::CallExpr) {
            return false;
        }
        const auto& call = static_cast<const ast::CallExpr&>(ev);
        return call.callee != nullptr && call.callee->kind == ast::NodeKind::MemberExpr
               && static_cast<const ast::MemberExpr&>(*call.callee).member == "receive";
    };
    bool all_channel_arms = true;  // vacuously true for an arm-less timeout select
    for (const auto& arm : sel.arms) {
        ScopeStack::Guard g(scopes_);
        // §11 — an event is either a Future[T] (from `spawn`) or a blocking
        // channel receive `ch.receive()` (type T?).
        TypePtr ev = arm.event ? check_expr(*arm.event) : types_->error();
        const bool is_future = ev != nullptr && ev->kind() == TypeKind::Future;
        const bool is_chan = ev != nullptr && ev->kind() == TypeKind::Optional && arm.event
                             && is_channel_receive(*arm.event);
        if (!is_chan) {
            all_channel_arms = false;
        }
        if (ev != nullptr && !ev->is_error() && !is_future && !is_chan) {
            error_at(arm.event->range,
                     std::format("select event must be a Future[T] or a channel receive "
                                 "(ch.receive()); got {}",
                                 ev->describe()));
        }
        if (arm.pattern) {
            // Future[T] unwraps to T; a channel receive binds the whole T?.
            TypePtr inner = is_future ? ev->inner() : (is_chan ? ev : types_->error());
            check_pattern(*arm.pattern, inner);
        }
        TypePtr arm_type = arm.body ? check_expr(*arm.body, expected) : types_->unit();
        join(arm_type, arm.body ? arm.body->range : sel.range);
    }
    if (sel.default_body) {
        join(check_expr(*sel.default_body, expected), sel.default_body->range);
    }
    // §11 timeout arm: the delay is an Int (milliseconds); the body joins the
    // arm result type. A timeout blocks, so it only pairs with channel arms and
    // never with a `default` (which makes the select a non-blocking poll).
    if (sel.timeout_body != nullptr) {
        if (sel.timeout_delay != nullptr) {
            TypePtr d = check_expr(*sel.timeout_delay);
            if (d != nullptr && !d->is_error() && !d->is_integer()) {
                error_at(sel.timeout_delay->range,
                         std::format("select timeout delay must be an integer (milliseconds); "
                                     "got {}",
                                     d->describe()));
            }
        }
        if (sel.default_body != nullptr) {
            error_at(sel.timeout_body->range,
                     "a select cannot have both a 'timeout' arm and a 'default' (a default makes "
                     "the select non-blocking, which a timeout contradicts)");
        }
        if (!all_channel_arms) {
            error_at(sel.timeout_body->range,
                     "a 'timeout' arm is only valid on a blocking select whose arms are all "
                     "channel receives (ch.receive())");
        }
        join(check_expr(*sel.timeout_body, expected), sel.timeout_body->range);
    }
    return result_type != nullptr ? result_type : types_->unit();
}

void Resolver::check_pattern(const ast::Pattern& p, TypePtr scrutinee) {
    switch (p.kind) {
    case ast::NodeKind::WildcardPat:
        break;
    case ast::NodeKind::LiteralPat: {
        // §17.7 literal pattern: the literal must adapt to the
        // scrutinee's type (Int literal in an Int32 slot, etc.).
        // check_expr with the scrutinee as the expected hint does the
        // adaptation, and assignable carries the natural-width rule.
        const auto& lp = static_cast<const ast::LiteralPat&>(p);
        if (lp.literal != nullptr) {
            auto lt = check_expr(*lp.literal, scrutinee);
            if (lt != nullptr && !lt->is_error() && scrutinee != nullptr && !scrutinee->is_error()
                && !TypeArena::assignable(lt, scrutinee)) {
                error_at(p.range,
                         std::format("literal of type {} does not match scrutinee of type {}",
                                     lt->describe(),
                                     scrutinee->describe()));
            }
        }
        break;
    }
    case ast::NodeKind::RangePat: {
        // §17.7 range pattern: scrutinee must be integer; both bounds
        // adapt to the scrutinee's width. Inclusive vs exclusive is a
        // codegen concern — sema just types the bounds.
        const auto& rp = static_cast<const ast::RangePat&>(p);
        if (scrutinee != nullptr && !scrutinee->is_error() && !scrutinee->is_integer()) {
            error_at(p.range,
                     std::format("range pattern requires an integer scrutinee, got {}",
                                 scrutinee->describe()));
        }
        auto check_bound = [&](const ast::Expr* b) {
            if (b == nullptr) {
                return;
            }
            auto bt = check_expr(*b, scrutinee);
            if (bt != nullptr && !bt->is_error() && !bt->is_integer()) {
                error_at(b->range,
                         std::format("range bound must be integer, got {}", bt->describe()));
            }
        };
        check_bound(rp.low.get());
        check_bound(rp.high.get());
        break;
    }
    case ast::NodeKind::OrPat: {
        // §17.7 or-pattern: each alternative is checked against the
        // scrutinee independently. v0.5 doesn't enforce the
        // same-bindings-across-alts rule — a follow-on pass adds the
        // diagnostic for `case .a(let x) | .b(let y):` style mismatch.
        const auto& op = static_cast<const ast::OrPat&>(p);
        for (const auto& alt : op.alternatives) {
            if (alt) {
                check_pattern(*alt, scrutinee);
            }
        }
        break;
    }
    case ast::NodeKind::IdentPat: {
        // Plain identifier patterns match by name; in match-arm position we
        // treat them as a binding to the scrutinee's type.
        const auto& ip = static_cast<const ast::IdentPat&>(p);
        Symbol sym;
        sym.name = ip.name;
        sym.kind = SymbolKind::Local;
        sym.type = scrutinee;
        sym.definition_range = p.range;
        (void)scopes_.current().insert(std::move(sym));
        break;
    }
    case ast::NodeKind::BindPat: {
        const auto& bp = static_cast<const ast::BindPat&>(p);
        Symbol sym;
        sym.name = bp.name;
        sym.kind = SymbolKind::Local;
        sym.type = scrutinee;
        sym.definition_range = p.range;
        (void)scopes_.current().insert(std::move(sym));
        break;
    }
    case ast::NodeKind::TuplePat: {
        // §6 tuple pattern in match-arm position. Two shapes:
        //   * `match tup { case (a, b): ... }` — the scrutinee is a
        //     tuple, the pattern destructures it directly.
        //   * As an EnumPat child: `case .pair((a, b)):` — the enclosing
        //     EnumPat case threads the payload field's type in as
        //     `scrutinee` for this recursive call.
        // Either way, bind_tuple_pattern walks the elements in parallel
        // with the scrutinee's TupleType and reports arity / non-tuple
        // mismatches.
        bind_tuple_pattern(static_cast<const ast::TuplePat&>(p), scrutinee);
        break;
    }
    case ast::NodeKind::EnumPat: {
        const auto& ep = static_cast<const ast::EnumPat&>(p);
        if (scrutinee == nullptr || scrutinee->kind() != TypeKind::Enum
            || scrutinee->nominal_decl() == nullptr) {
            error_at(p.range,
                     std::format("enum pattern '.{}' used against non-enum type {}",
                                 ep.case_name,
                                 scrutinee ? scrutinee->describe() : "?"));
            return;
        }
        const auto& enum_decl = static_cast<const ast::EnumDecl&>(*scrutinee->nominal_decl());
        const auto* c = lookup_enum_case(enum_decl, ep.case_name);
        if (c == nullptr) {
            error_at(p.range,
                     std::format("enum '{}' has no case '{}'", enum_decl.name, ep.case_name));
            return;
        }
        if (ep.children.size() != c->payload.size()) {
            error_at(p.range,
                     std::format("case '{}' has {} payload field(s), pattern binds {}",
                                 c->name,
                                 c->payload.size(),
                                 ep.children.size()));
            return;
        }
        for (std::size_t i = 0; i < ep.children.size(); ++i) {
            // §7 generics phase 2 — substitute the scrutinee instance's
            // type arguments (scrutinee->parts()) for the enum's params so
            // `case .some(let x):` over an `Option[Int32]` binds x as Int32.
            TypePtr payload_type = c->payload[i].second
                                       ? resolve_member_type_with_generics(enum_decl.generics,
                                                                           *c->payload[i].second,
                                                                           scrutinee->parts())
                                       : types_->error();
            check_pattern(*ep.children[i], payload_type);
        }
        break;
    }
    default:
        // Tuple/struct/slice patterns: not yet typed against `scrutinee`.
        break;
    }
}

}  // namespace vestra::sema
