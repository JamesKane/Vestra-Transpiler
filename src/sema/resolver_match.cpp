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

#include <algorithm>
#include <array>
#include <cstdlib>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vestra::sema {

using detail::named_type_param_count;

namespace {

// A pattern that matches every value of its type: `_`, a binding, or a
// tuple of such. (nullptr stands for an implied wildcard.)
// A bare name is wild unless it compares against a value (§17.6).
bool pattern_is_wild(const ast::Pattern* p, const Resolution& r) {
    if (p == nullptr) {
        return true;
    }
    switch (p->kind) {
    case ast::NodeKind::WildcardPat:
    case ast::NodeKind::BindPat:
        return true;
    case ast::NodeKind::IdentPat:
        return !r.is_value_pattern(p);
    case ast::NodeKind::TuplePat:
        for (const auto& e : static_cast<const ast::TuplePat&>(*p).elements) {
            if (!pattern_is_wild(e.get(), r)) {
                return false;
            }
        }
        return true;
    default:
        return false;
    }
}

// `true` / `false` for a Bool literal pattern, else nullopt.
std::optional<bool> bool_pattern(const ast::Pattern* p) {
    if (p == nullptr || p->kind != ast::NodeKind::LiteralPat) {
        return std::nullopt;
    }
    const auto& lp = static_cast<const ast::LiteralPat&>(*p);
    if (lp.literal == nullptr || lp.literal->kind != ast::NodeKind::BoolLit) {
        return std::nullopt;
    }
    return static_cast<const ast::BoolLit&>(*lp.literal).value;
}

std::string join_witness(const std::vector<std::string>& w, std::size_t from, std::size_t to) {
    std::string out;
    for (std::size_t i = from; i < to && i < w.size(); ++i) {
        if (i > from) {
            out += ", ";
        }
        out += w[i];
    }
    return out;
}

}  // namespace

// §6 exhaustiveness: the classic usefulness check over a matrix of pattern
// rows, one column per value still to be matched. Enum and Bool columns split
// by constructor when some row names one; any other type is covered only by
// wildcards. Returns a witness (one pattern spelling per column) that no row
// matches, or nullopt when the rows cover every value.
std::optional<std::vector<std::string>>
Resolver::uncovered_patterns(std::vector<std::vector<const ast::Pattern*>> rows,
                             const std::vector<TypePtr>& cols,
                             int depth) {
    // Expand or-patterns at the head into one row per alternative.
    for (bool again = true; again;) {
        again = false;
        std::vector<std::vector<const ast::Pattern*>> next;
        for (auto& r : rows) {
            if (!r.empty() && r[0] != nullptr && r[0]->kind == ast::NodeKind::OrPat) {
                for (const auto& alt : static_cast<const ast::OrPat&>(*r[0]).alternatives) {
                    auto copy = r;
                    copy[0] = alt.get();
                    next.push_back(std::move(copy));
                }
                again = true;
            } else {
                next.push_back(std::move(r));
            }
        }
        rows = std::move(next);
    }
    if (cols.empty()) {
        if (rows.empty()) {
            return std::vector<std::string>{};
        }
        return std::nullopt;
    }
    if (depth > 32) {
        return std::nullopt;  // give up quietly rather than recurse forever
    }
    TypePtr t = cols[0];
    std::vector<TypePtr> rest(cols.begin() + 1, cols.end());
    auto tail = [](const std::vector<const ast::Pattern*>& r) {
        return std::vector<const ast::Pattern*>(r.begin() + 1, r.end());
    };
    const ast::EnumDecl* ed = nullptr;
    if (t != nullptr && t->kind() == TypeKind::Enum && t->nominal_decl() != nullptr
        && t->nominal_decl()->kind == ast::NodeKind::Enum) {
        ed = &static_cast<const ast::EnumDecl&>(*t->nominal_decl());
    }
    const bool is_bool = t != nullptr && t->kind() == TypeKind::Bool;
    bool any_ctor = false;
    for (const auto& r : rows) {
        if (ed != nullptr && r[0] != nullptr && r[0]->kind == ast::NodeKind::EnumPat) {
            any_ctor = true;
        }
        if (is_bool && bool_pattern(r[0]).has_value()) {
            any_ctor = true;
        }
    }
    if (any_ctor && ed != nullptr) {
        for (const auto& c : ed->cases) {
            const std::size_t arity = c.payload.size();
            std::vector<TypePtr> sub;
            for (const auto& pl : c.payload) {
                sub.push_back(pl.second ? resolve_member_type_with_generics(ed->generics,
                                                                            *pl.second,
                                                                            t->parts())
                                        : types_->error());
            }
            sub.insert(sub.end(), rest.begin(), rest.end());
            std::vector<std::vector<const ast::Pattern*>> spec;
            for (const auto& r : rows) {
                std::vector<const ast::Pattern*> nr;
                if (pattern_is_wild(r[0], resolution_)) {
                    nr.assign(arity, nullptr);
                } else if (r[0]->kind == ast::NodeKind::EnumPat
                           && static_cast<const ast::EnumPat&>(*r[0]).case_name == c.name) {
                    const auto& ch = static_cast<const ast::EnumPat&>(*r[0]).children;
                    if (ch.size() == arity) {
                        for (const auto& x : ch) {
                            nr.push_back(x.get());
                        }
                    } else {
                        nr.assign(arity, nullptr);
                    }
                } else {
                    continue;
                }
                auto t2 = tail(r);
                nr.insert(nr.end(), t2.begin(), t2.end());
                spec.push_back(std::move(nr));
            }
            if (auto w = uncovered_patterns(std::move(spec), sub, depth + 1)) {
                std::string head = "." + c.name;
                if (arity > 0) {
                    head += "(" + join_witness(*w, 0, arity) + ")";
                }
                std::vector<std::string> out{head};
                out.insert(out.end(), w->begin() + static_cast<std::ptrdiff_t>(arity), w->end());
                return out;
            }
        }
        return std::nullopt;
    }
    if (any_ctor && is_bool) {
        for (bool v : {true, false}) {
            std::vector<std::vector<const ast::Pattern*>> spec;
            for (const auto& r : rows) {
                auto b = bool_pattern(r[0]);
                if (pattern_is_wild(r[0], resolution_) || (b.has_value() && *b == v)) {
                    spec.push_back(tail(r));
                }
            }
            if (auto w = uncovered_patterns(std::move(spec), rest, depth + 1)) {
                std::vector<std::string> out{v ? "true" : "false"};
                out.insert(out.end(), w->begin(), w->end());
                return out;
            }
        }
        return std::nullopt;
    }
    std::vector<std::vector<const ast::Pattern*>> def;
    for (const auto& r : rows) {
        if (pattern_is_wild(r[0], resolution_)) {
            def.push_back(tail(r));
        }
    }
    if (auto w = uncovered_patterns(std::move(def), rest, depth + 1)) {
        std::vector<std::string> out{"_"};
        out.insert(out.end(), w->begin(), w->end());
        return out;
    }
    return std::nullopt;
}

void Resolver::bind_tuple_pattern(const ast::TuplePat& pat, TypePtr value_type, bool mutable_bindings) {
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
            if (in_match_pattern_) {
                check_bare_name_pattern(static_cast<const ast::IdentPat&>(*sub), parts[i]);
                continue;
            }
        } else if (sub->kind == ast::NodeKind::BindPat) {
            name = static_cast<const ast::BindPat&>(*sub).name;
        } else if (sub->kind == ast::NodeKind::WildcardPat) {
            continue;
        } else if (sub->kind == ast::NodeKind::TuplePat) {
            bind_tuple_pattern(static_cast<const ast::TuplePat&>(*sub), parts[i], mutable_bindings);
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
        sym.is_mutable = mutable_bindings;
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
    bool saw_default = false;

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
            // `.some(k)` or `.some(0)` over an optional can fail, so it
            // closes nothing. (A user enum's payload sub-patterns aren't
            // tested yet, so its case still closes.)
            if (resolution_.is_optional_pattern(&p)
                && !std::ranges::all_of(ep.children,
                                        [this](const auto& c) { return pattern_is_wild(c.get(), resolution_); })) {
                return;
            }
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

    std::vector<std::vector<const ast::Pattern*>> cover_rows;
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
            in_match_pattern_ = true;
            check_pattern(*arm.pattern, scrutinee_type);
            in_match_pattern_ = false;
            // §6 a guarded arm covers nothing: its guard may fail.
            if (!arm.guard) {
                cover_rows.push_back({arm.pattern.get()});
            }
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
        // §9 arms that yield `T` and `nil` (or `T` and `T?`) make a `T?` match,
        // as `if`/`else` does (F18).
        auto optional_join = [&](TypePtr a, TypePtr b) -> TypePtr {
            if (a == nullptr || b == nullptr) {
                return nullptr;
            }
            const bool ao = a->kind() == TypeKind::Optional;
            const bool bo = b->kind() == TypeKind::Optional;
            if (ao && a->inner() != nullptr && a->inner()->is_never()) {
                return bo ? b : types_->make_optional(b);  // nil, then T / T?
            }
            if (bo && b->inner() != nullptr && b->inner()->is_never()) {
                return ao ? a : types_->make_optional(a);  // T / T?, then nil
            }
            if (ao && !bo && TypeArena::equal(a->inner(), b)) {
                return a;
            }
            if (bo && !ao && TypeArena::equal(b->inner(), a)) {
                return b;
            }
            return nullptr;
        };
        if (result_type != nullptr && arm_type != nullptr && !arm_type->is_never()
            && !result_type->is_never() && !TypeArena::equal(arm_type, result_type)) {
            if (auto joined = optional_join(result_type, arm_type)) {
                result_type = joined;
                continue;
            }
        }
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
    // every value of every case must be matched by some unguarded arm,
    // through nested payload patterns. One diagnostic per case, naming the
    // first unmatched shape (`p(.y)`), or just the case when no arm names it.
    if (scrutinee_enum != nullptr && !saw_default) {
        for (const auto& c : scrutinee_enum->cases) {
            const std::size_t arity = c.payload.size();
            std::vector<TypePtr> sub;
            for (const auto& pl : c.payload) {
                sub.push_back(pl.second
                                  ? resolve_member_type_with_generics(scrutinee_enum->generics,
                                                                      *pl.second,
                                                                      scrutinee_type->parts())
                                  : types_->error());
            }
            std::vector<std::vector<const ast::Pattern*>> rows;
            bool named = false;
            std::vector<std::vector<const ast::Pattern*>> heads = cover_rows;
            // Flatten top-level or-patterns.
            for (bool again = true; again;) {
                again = false;
                std::vector<std::vector<const ast::Pattern*>> next;
                for (auto& r : heads) {
                    if (r[0] != nullptr && r[0]->kind == ast::NodeKind::OrPat) {
                        for (const auto& alt : static_cast<const ast::OrPat&>(*r[0]).alternatives) {
                            next.push_back({alt.get()});
                        }
                        again = true;
                    } else {
                        next.push_back(std::move(r));
                    }
                }
                heads = std::move(next);
            }
            for (const auto& r : heads) {
                if (pattern_is_wild(r[0], resolution_)) {
                    rows.emplace_back(arity, nullptr);
                    named = true;
                } else if (r[0]->kind == ast::NodeKind::EnumPat
                           && static_cast<const ast::EnumPat&>(*r[0]).case_name == c.name) {
                    const auto& ch = static_cast<const ast::EnumPat&>(*r[0]).children;
                    std::vector<const ast::Pattern*> nr(arity, nullptr);
                    if (ch.size() == arity) {
                        for (std::size_t k = 0; k < arity; ++k) {
                            nr[k] = ch[k].get();
                        }
                    }
                    rows.push_back(std::move(nr));
                    named = true;
                }
            }
            if (!named) {
                error_at(m.range,
                         std::format("match is not exhaustive: case '{}' is not covered", c.name));
                continue;
            }
            if (auto w = uncovered_patterns(std::move(rows), sub, 0)) {
                error_at(m.range,
                         std::format("match is not exhaustive: case '{}({})' is not covered",
                                     c.name,
                                     join_witness(*w, 0, w->size())));
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
            TypePtr d = check_expr(*sel.timeout_delay, types_->primitive(TypeKind::Duration));
            if (d != nullptr && !d->is_error() && d->kind() != TypeKind::Duration) {
                error_at(sel.timeout_delay->range,
                         std::format("select timeout delay must be a Duration "
                                     "(e.g. `.milliseconds(250)`); got {}",
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

void Resolver::reject_bare_name_pattern(std::string_view name, diag::SourceRange range) {
    error_at(range,
             std::format("a bare name in a pattern matches an existing value, which is not "
                         "supported yet; write 'let {}' to bind '{}'",
                         name,
                         name));
}

// §17.6 a bare name in a match pattern compares the value against an
// existing one (`let x` binds). It lowers to `==`, so it stands where a
// literal could: at the top of an arm, in a tuple, an or-pattern or
// `.some(...)`. A user enum's payload doesn't test its sub-patterns yet, and a
// payloaded enum has no `==`, so both keep the old rejection.
// Binds a rejected bare name anyway, so its uses don't cascade.
void Resolver::bind_pattern_name(const ast::IdentPat& ip, TypePtr type) {
    Symbol sym;
    sym.name = ip.name;
    sym.kind = SymbolKind::Local;
    sym.is_mutable = false;
    sym.type = type;
    sym.definition_range = ip.range;
    (void)scopes_.current().insert(std::move(sym));
}

void Resolver::check_bare_name_pattern(const ast::IdentPat& ip, TypePtr scrutinee) {
    if (in_enum_payload_ > 0
        || (scrutinee != nullptr && scrutinee->kind() == TypeKind::Enum
            && scrutinee->nominal_decl() != nullptr
            && scrutinee->nominal_decl()->kind == ast::NodeKind::Enum
            && std::ranges::any_of(
                static_cast<const ast::EnumDecl&>(*scrutinee->nominal_decl()).cases,
                [](const auto& c) { return !c.payload.empty(); }))) {
        reject_bare_name_pattern(ip.name, ip.range);
        bind_pattern_name(ip, scrutinee);
        return;
    }
    const auto* sym = scopes_.current().lookup(ip.name);
    if (sym == nullptr
        || (sym->kind != SymbolKind::Local && sym->kind != SymbolKind::Parameter
            && sym->kind != SymbolKind::Const && sym->kind != SymbolKind::Static)) {
        error_at(ip.range,
                 std::format("a bare name in a pattern matches an existing value, and '{}' is "
                             "not one in scope; write 'let {}' to bind it",
                             ip.name,
                             ip.name));
        bind_pattern_name(ip, scrutinee);
        return;
    }
    resolution_.mark_value_pattern(&ip);
    TypePtr vt = sym->type;
    if (vt != nullptr && !vt->is_error() && scrutinee != nullptr && !scrutinee->is_error()
        && !TypeArena::assignable(vt, scrutinee)) {
        error_at(ip.range,
                 std::format("pattern value '{}' of type {} does not match scrutinee of type {}",
                             ip.name,
                             vt->describe(),
                             scrutinee->describe()));
    }
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
        if (in_match_pattern_) {
            check_bare_name_pattern(ip, scrutinee);
            break;
        }
        Symbol sym;
        sym.name = ip.name;
        sym.kind = SymbolKind::Local;
        sym.is_mutable = false;  // a pattern binding is a `let`
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
        // §18 Optional is `enum Optional[T] { case some(T); case none }`: its
        // two cases match an optional scrutinee.
        if (scrutinee != nullptr && scrutinee->kind() == TypeKind::Optional
            && (ep.case_name == "some" || ep.case_name == "none")) {
            resolution_.mark_optional_pattern(&p);
            const std::size_t arity = ep.case_name == "some" ? 1 : 0;
            if (!ep.children.empty() && ep.children.size() != arity) {
                error_at(p.range,
                         std::format("case '{}' has {} payload field(s), pattern binds {}",
                                     ep.case_name,
                                     arity,
                                     ep.children.size()));
                return;
            }
            if (!ep.children.empty()) {
                check_pattern(*ep.children[0], scrutinee->inner());
            }
            break;
        }
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
        // §8 `case .x:` with no bindings matches the case and ignores its
        // payload; a pattern that binds must bind every field (F18).
        if (!ep.children.empty() && ep.children.size() != c->payload.size()) {
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
            ++in_enum_payload_;
            check_pattern(*ep.children[i], payload_type);
            --in_enum_payload_;
        }
        break;
    }
    default:
        // Tuple/struct/slice patterns: not yet typed against `scrutinee`.
        break;
    }
}

}  // namespace vestra::sema
