// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// Match / select control-flow emission: bare-enum switch, payloaded-enum
// if-chain, value-scrutinee if-chain, select, and pattern helpers.
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

// A channel-receive arm event is `<chan>.receive()`; this returns the <chan>
// sub-expression so codegen can call `.sel_state()` / `.sel_take()` on it. Null
// if the event isn't a `.receive()` call (a Future arm).
static const ast::Expr* select_channel_base(const ast::Expr* event) {
    if (event == nullptr || event->kind != ast::NodeKind::CallExpr) {
        return nullptr;
    }
    const auto& call = static_cast<const ast::CallExpr&>(*event);
    if (call.callee == nullptr || call.callee->kind != ast::NodeKind::MemberExpr) {
        return nullptr;
    }
    const auto& mem = static_cast<const ast::MemberExpr&>(*call.callee);
    return mem.member == "receive" ? mem.base.get() : nullptr;
}

void CppEmitter::emit_select(std::ostream& os, const ast::SelectExpr& sel) {
    sema::TypePtr rt = resolution_ != nullptr ? resolution_->type_of(&sel) : nullptr;

    // Two arm flavors (§11). A Future[T] arm is an always-ready awaiter polled
    // with `.await_ready()` / `.get()`. A channel arm is `ch.receive()`, polled
    // via `ch.sel_state()->ready()` and taken with `ch.sel_take()`. A select
    // whose arms are *all* channel receives and has no default *blocks*: it
    // co_awaits a SelectAwaiter that parks on every channel and wakes on the
    // first ready one. Any other shape (a default present, or a mix of future
    // and channel arms) takes the poll lowering below: source-order, first
    // ready arm wins. In a mixed select a future arm is always ready, so a
    // not-yet-ready channel can't win a race it isn't already holding — the
    // future is forced (this is the poll semantics, not a blocking join).
    const bool has_timeout = sel.timeout_body != nullptr;
    bool all_channel = true;  // vacuously true for an arm-less timeout select
    for (const auto& arm : sel.arms) {
        if (select_channel_base(arm.event.get()) == nullptr) {
            all_channel = false;
            break;
        }
    }

    if (all_channel && sel.default_body == nullptr && (!sel.arms.empty() || has_timeout)) {
        // Blocking select: lower to a nested Task<RT> coroutine that the
        // enclosing async function co_awaits. The inner coroutine parks on all
        // channels (co_await SelectAwaiter → winning index), then dispatches to
        // that arm: take its value, bind the pattern, co_return the body. A
        // timeout arm registers a wall-clock timer; if it fires first the
        // awaiter returns the index just past the last channel.
        os << "co_await [&]() -> __vstr::Task<";
        if (rt != nullptr) {
            emit_sema_type(os, rt);
        } else {
            os << "auto";
        }
        os << "> {\n";
        write_indent(os, 2);
        os << "int __vstr_selw = co_await __vstr::SelectAwaiter{{";
        for (std::size_t i = 0; i < sel.arms.size(); ++i) {
            if (i != 0) {
                os << ", ";
            }
            emit_expr(os, *select_channel_base(sel.arms[i].event.get()));
            os << ".sel_state()";
        }
        if (has_timeout) {
            // states, w(nullptr), has_timeout(true), timeout_ms, timeout_index.
            // The delay is a Duration; the awaiter wants whole milliseconds.
            os << "}, nullptr, true, (";
            emit_expr(os, *sel.timeout_delay);
            os << ").in_milliseconds(), " << sel.arms.size() << "};\n";
        } else {
            os << "}, nullptr};\n";
        }
        for (std::size_t i = 0; i < sel.arms.size(); ++i) {
            const auto& arm = sel.arms[i];
            write_indent(os, 2);
            os << "if (__vstr_selw == " << i << ") {\n";
            write_indent(os, 3);
            os << "auto&& __vstr_selv = ";
            emit_expr(os, *select_channel_base(arm.event.get()));
            os << ".sel_take();\n";
            if (arm.pattern) {
                emit_pat_bindings(os, *arm.pattern, "__vstr_selv", 3);
            }
            write_indent(os, 3);
            os << "co_return ";
            if (arm.body) {
                emit_expr(os, *arm.body);
            }
            os << ";\n";
            write_indent(os, 2);
            os << "}\n";
        }
        if (has_timeout) {
            write_indent(os, 2);
            os << "if (__vstr_selw == " << sel.arms.size() << ") {\n";
            write_indent(os, 3);
            os << "co_return ";
            emit_expr(os, *sel.timeout_body);
            os << ";\n";
            write_indent(os, 2);
            os << "}\n";
        }
        write_indent(os, 2);
        os << "std::unreachable();\n";
        write_indent(os, 1);
        os << "}()";
        return;
    }

    // Poll lowering (Future arms, or channel arms with a default): an IIFE that,
    // in source order, takes the first ready arm, binds, and runs its body; the
    // default (or std::unreachable) covers "nothing ready". Plain `return` — the
    // lambda is not itself a coroutine.
    os << "[&]() -> ";
    if (rt != nullptr) {
        emit_sema_type(os, rt);
    } else {
        os << "auto";
    }
    os << " {\n";
    int idx = 0;
    for (const auto& arm : sel.arms) {
        const ast::Expr* chan = select_channel_base(arm.event.get());
        const std::string ev = std::format("__vstr_sel{}", idx++);
        write_indent(os, 2);
        os << "{\n";
        write_indent(os, 3);
        os << "auto&& " << ev << " = ";
        if (chan != nullptr) {
            emit_expr(os, *chan);  // the channel handle
        } else if (arm.event) {
            emit_expr(os, *arm.event);  // the Future awaiter
        }
        os << ";\n";
        write_indent(os, 3);
        os << "if (" << ev
           << (chan != nullptr ? ".sel_state()->ready()) {\n" : ".await_ready()) {\n");
        if (arm.pattern) {
            write_indent(os, 4);
            os << "auto&& __vstr_selv = " << ev
               << (chan != nullptr ? ".sel_take();\n" : ".get();\n");
            emit_pat_bindings(os, *arm.pattern, "__vstr_selv", 4);
        }
        write_indent(os, 4);
        os << "return ";
        if (arm.body) {
            emit_expr(os, *arm.body);
        }
        os << ";\n";
        write_indent(os, 3);
        os << "}\n";
        write_indent(os, 2);
        os << "}\n";
    }
    write_indent(os, 2);
    if (sel.default_body) {
        os << "return ";
        emit_expr(os, *sel.default_body);
        os << ";\n";
    } else {
        os << "std::unreachable();\n";
    }
    write_indent(os, 1);
    os << "}()";
}

void CppEmitter::emit_match(std::ostream& os, const ast::MatchExpr& m) {
    // We need sema's findings to know what the scrutinee is. Without them we
    // can't tell which `Enum::case` to write per arm.
    if (resolution_ == nullptr) {
        unsupported(os, "match expression without resolved scrutinee", m.range);
        return;
    }
    auto scrutinee_type = resolution_->type_of(m.scrutinee.get());
    // Non-enum scrutinees (integer / bool / string / tuple) go through
    // the value-scrutinee if-chain path. Enum dispatch stays on the
    // switch / std::visit shapes below.
    if (scrutinee_type == nullptr || (scrutinee_type->kind() != sema::TypeKind::Enum)) {
        emit_match_value_scrutinee(os, m);
        return;
    }
    if (scrutinee_type->nominal_decl() == nullptr) {
        unsupported(os, "match over enum without resolved decl", m.range);
        return;
    }
    const auto& enum_decl = static_cast<const ast::EnumDecl&>(*scrutinee_type->nominal_decl());

    // Two lowerings depending on whether any case carries a payload. A
    // bare enum lowers to a plain `switch (e) { case … }` IIFE; a
    // payloaded enum lowers to `std::visit(...)` with a constexpr-if
    // chain dispatching on each alternative's type.
    bool has_payload = false;
    for (const auto& c : enum_decl.cases) {
        if (!c.payload.empty()) {
            has_payload = true;
            break;
        }
    }

    if (!has_payload) {
        // §8 bare-enum match: pick between a switch lowering (simple
        // case, no guards, each case label distinct) and an if-chain
        // lowering (any arm has a guard, OR multiple arms target the
        // same case — both situations a switch can't express). The
        // if-chain mirrors the value-scrutinee path: each arm
        // contributes one `if (scrutinee == Enum::case && guard) {
        // return body; }`.
        bool any_guard = false;
        std::unordered_set<std::string> seen;
        bool any_duplicate = false;
        for (const auto& arm : m.arms) {
            if (arm.guard) {
                any_guard = true;
            }
            if (arm.pattern != nullptr && arm.pattern->kind == ast::NodeKind::EnumPat) {
                const auto& ep = static_cast<const ast::EnumPat&>(*arm.pattern);
                if (!seen.insert(ep.case_name).second) {
                    any_duplicate = true;
                }
            }
        }
        const bool use_if_chain = any_guard || any_duplicate;

        if (use_if_chain) {
            os << "[&]() -> auto {\n";
            os << "        auto&& __vstr_m = ";
            emit_expr(os, *m.scrutinee);
            os << ";\n";
            bool first = true;
            const ast::MatchArm* default_arm_local = nullptr;
            for (const auto& arm : m.arms) {
                if (arm.is_default
                    || (arm.pattern != nullptr
                        && arm.pattern->kind == ast::NodeKind::WildcardPat)) {
                    default_arm_local = &arm;
                    continue;
                }
                if (arm.pattern == nullptr
                    || (arm.pattern->kind != ast::NodeKind::EnumPat
                        && arm.pattern->kind != ast::NodeKind::OrPat)) {
                    unsupported(os, "match arm pattern", m.range);
                    continue;
                }
                write_indent(os, 2);
                os << (first ? "if (" : "else if (");
                // Build the case predicate: a single EnumPat or an
                // OrPat folded into a chain of `||`s.
                if (arm.pattern->kind == ast::NodeKind::EnumPat) {
                    const auto& ep = static_cast<const ast::EnumPat&>(*arm.pattern);
                    os << "__vstr_m == " << enum_decl.name << "::" << ep.case_name;
                } else {
                    const auto& op = static_cast<const ast::OrPat&>(*arm.pattern);
                    os << "(";
                    for (std::size_t i = 0; i < op.alternatives.size(); ++i) {
                        if (i != 0) {
                            os << " || ";
                        }
                        if (op.alternatives[i] != nullptr
                            && op.alternatives[i]->kind == ast::NodeKind::EnumPat) {
                            const auto& ep = static_cast<const ast::EnumPat&>(*op.alternatives[i]);
                            os << "__vstr_m == " << enum_decl.name << "::" << ep.case_name;
                        } else {
                            os << "false";
                        }
                    }
                    os << ")";
                }
                if (arm.guard) {
                    os << " && (";
                    emit_expr(os, *arm.guard);
                    os << ")";
                }
                os << ") {\n";
                write_indent(os, 3);
                os << "return ";
                if (arm.body) {
                    emit_expr(os, *arm.body);
                }
                os << ";\n";
                write_indent(os, 2);
                os << "}\n";
                first = false;
            }
            if (default_arm_local != nullptr) {
                write_indent(os, 2);
                os << (first ? "{ " : "else { ");
                os << "return ";
                if (default_arm_local->body) {
                    emit_expr(os, *default_arm_local->body);
                }
                os << "; }\n";
            } else {
                write_indent(os, 2);
                os << (first ? "std::unreachable();\n" : "else { std::unreachable(); }\n");
            }
            os << "    }()";
            return;
        }

        // Plain switch path — simple, no guards, distinct case labels.
        // Wildcard patterns map to `default:`; `case .a | .b:` stacks
        // both labels above one return statement.
        os << "[&]{ switch (";
        emit_expr(os, *m.scrutinee);
        os << ") {\n";
        auto emit_enum_labels = [&](const ast::Pattern& p, auto& self) -> bool {
            if (p.kind == ast::NodeKind::EnumPat) {
                const auto& ep = static_cast<const ast::EnumPat&>(p);
                os << "        case " << enum_decl.name << "::" << ep.case_name << ":\n";
                return true;
            }
            if (p.kind == ast::NodeKind::OrPat) {
                bool ok = true;
                for (const auto& alt : static_cast<const ast::OrPat&>(p).alternatives) {
                    if (alt == nullptr || !self(*alt, self)) {
                        ok = false;
                    }
                }
                return ok;
            }
            return false;
        };
        for (const auto& arm : m.arms) {
            if (arm.is_default
                || (arm.pattern != nullptr && arm.pattern->kind == ast::NodeKind::WildcardPat)) {
                os << "        default:\n            return ";
            } else if (arm.pattern != nullptr
                       && (arm.pattern->kind == ast::NodeKind::EnumPat
                           || arm.pattern->kind == ast::NodeKind::OrPat)) {
                if (!emit_enum_labels(*arm.pattern, emit_enum_labels)) {
                    unsupported(os, "match arm pattern", m.range);
                    os << ";\n";
                    continue;
                }
                os << "            return ";
            } else {
                unsupported(os, "match arm pattern", m.range);
                os << ";\n";
                continue;
            }
            emit_expr(os, *arm.body);
            os << ";\n";
        }
        os << "    } }()";
        return;
    }

    // Payloaded enum: lower to std::visit with constexpr-if dispatch.
    //
    //   std::visit([&](auto&& __vstr_alt) -> auto {
    //       using __vstr_alt_t = std::decay_t<decltype(__vstr_alt)>;
    //       if constexpr (std::is_same_v<__vstr_alt_t, Enum::case_t>) {
    //           auto&& binding = __vstr_alt.field;   // per BindPat child
    //           return <arm body>;
    //       } else if constexpr (…) { … }
    //       else { return <default body>; }                  // optional
    //       else { std::unreachable(); }                     // safety net
    //   }, scrutinee.value)
    //
    // Sema verifies exhaustiveness so the std::unreachable() arm only
    // exists to satisfy C++'s "all paths must return" rule when there's
    // no user-supplied default.
    os << "std::visit([&](auto&& __vstr_alt) -> auto {\n";
    os << "        using __vstr_alt_t = std::decay_t<decltype(__vstr_alt)>;\n";

    // §8 group arms by case name so multiple arms for one case (the
    // canonical "guarded arm + unguarded catch" pattern) land in one
    // constexpr-if branch. C++'s std::visit dispatches on the
    // variant's active alternative; two separate `if constexpr`
    // branches for the same case_t leave the second dead. Grouping
    // up front lets each case's arm list run as an ordinary
    // if-else chain at runtime.
    // Each case group entry pairs the source arm (for its body + guard) with
    // the specific EnumPat it matched. For a plain `case .a(x):` the pattern
    // *is* the EnumPat; for an or-pattern `case .a(x) | .b(x):` each
    // alternative is its own EnumPat that shares the arm's body, so the arm
    // appears once per case it covers (each binding its own payload field to
    // the common name sema validated).
    struct ArmCase {
        const ast::MatchArm* arm;
        const ast::EnumPat* ep;
    };
    std::vector<std::pair<std::string, std::vector<ArmCase>>> groups;
    const ast::MatchArm* default_arm = nullptr;
    auto add_case = [&](const ast::MatchArm& arm, const ast::EnumPat& ep) {
        auto it = std::find_if(
            groups.begin(), groups.end(), [&](const auto& g) { return g.first == ep.case_name; });
        if (it == groups.end()) {
            groups.push_back({ep.case_name, {ArmCase{&arm, &ep}}});
        } else {
            it->second.push_back(ArmCase{&arm, &ep});
        }
    };
    for (const auto& arm : m.arms) {
        if (arm.is_default
            || (arm.pattern != nullptr && arm.pattern->kind == ast::NodeKind::WildcardPat)) {
            default_arm = &arm;
            continue;
        }
        if (arm.pattern != nullptr && arm.pattern->kind == ast::NodeKind::EnumPat) {
            add_case(arm, static_cast<const ast::EnumPat&>(*arm.pattern));
            continue;
        }
        // §17.7 or-pattern over payloaded-enum cases: expand each alternative
        // into its own case group, all sharing this arm's body.
        if (arm.pattern != nullptr && arm.pattern->kind == ast::NodeKind::OrPat) {
            const auto& op = static_cast<const ast::OrPat&>(*arm.pattern);
            bool all_enum = !op.alternatives.empty();
            for (const auto& alt : op.alternatives) {
                if (alt == nullptr || alt->kind != ast::NodeKind::EnumPat) {
                    all_enum = false;
                    break;
                }
            }
            if (all_enum) {
                for (const auto& alt : op.alternatives) {
                    add_case(arm, static_cast<const ast::EnumPat&>(*alt));
                }
                continue;
            }
        }
        unsupported(os, "match arm pattern over payloaded enum", m.range);
    }

    auto emit_payload_bindings = [&](const ast::EnumPat& ep, const ast::EnumDecl::Case& case_decl) {
        for (std::size_t i = 0; i < ep.children.size() && i < case_decl.payload.size(); ++i) {
            const auto& child = *ep.children[i];
            std::string field_name = case_decl.payload[i].first.empty()
                                         ? std::format("_{}", i)
                                         : case_decl.payload[i].first;
            if (child.kind == ast::NodeKind::BindPat) {
                write_indent(os, 4);
                os << "auto&& " << static_cast<const ast::BindPat&>(child).name << " = __vstr_alt."
                   << field_name << ";\n";
            } else if (child.kind == ast::NodeKind::IdentPat) {
                write_indent(os, 4);
                os << "auto&& " << static_cast<const ast::IdentPat&>(child).name << " = __vstr_alt."
                   << field_name << ";\n";
            } else if (child.kind == ast::NodeKind::TuplePat) {
                // §6 tuple-pattern payload child:
                //   case .pair((a, b)): ...
                // Lower to a structured binding off the payload field,
                // with the existing tuple-pat helpers handling any
                // nested sub-tuple via sibling follow-on statements.
                const auto& sub_tp = static_cast<const ast::TuplePat&>(child);
                std::vector<std::string> sub_names;
                std::vector<std::pair<std::string, const ast::TuplePat*>> sub_followons;
                collect_tuple_pat_names(sub_tp, sub_names, sub_followons);
                write_indent(os, 4);
                os << "auto&& [";
                for (std::size_t k = 0; k < sub_names.size(); ++k) {
                    if (k != 0) {
                        os << ", ";
                    }
                    os << sub_names[k];
                }
                os << "] = __vstr_alt." << field_name << ";\n";
                emit_tuple_pat_followons(os, sub_followons, 4);
            }
            // Wildcard: nothing to bind.
        }
    };

    bool first = true;
    for (const auto& [case_name, arms] : groups) {
        const ast::EnumDecl::Case* case_decl = nullptr;
        for (const auto& c : enum_decl.cases) {
            if (c.name == case_name) {
                case_decl = &c;
                break;
            }
        }
        if (case_decl == nullptr) {
            unsupported(os, "match arm references unknown enum case", arms.front().ep->range);
            continue;
        }

        if (first) {
            write_indent(os, 2);
            os << "if";
        } else {
            os << " else if";  // chained — previous arm ended with bare "}"
        }
        // §7 generics phase 2 — for a generic enum the alternative type is
        // `Option<Int32>::some_t`, so qualify with the scrutinee instance's
        // type arguments rather than the bare decl name.
        os << " constexpr (std::is_same_v<__vstr_alt_t, ";
        if (!scrutinee_type->parts().empty()) {
            emit_sema_type(os, scrutinee_type);
        } else {
            os << enum_decl.name;
        }
        os << "::" << case_name << "_t>) {\n";

        // §8 multiple arms per case: emit each in source order, scoping
        // per-arm bindings inside a `{}` block so different arms can
        // bind different names. A guarded arm wraps its return in
        // `if (guard) { return body; }`; an unguarded arm fires its
        // return unconditionally and dominates anything after it.
        bool seen_unguarded = false;
        for (const auto& ac : arms) {
            const ast::MatchArm* arm = ac.arm;
            const ast::EnumPat& ep = *ac.ep;
            write_indent(os, 3);
            os << "{\n";
            emit_payload_bindings(ep, *case_decl);
            if (arm->guard) {
                write_indent(os, 4);
                os << "if (";
                emit_expr(os, *arm->guard);
                os << ") {\n";
                write_indent(os, 5);
                os << "return ";
                emit_expr(os, *arm->body);
                os << ";\n";
                write_indent(os, 4);
                os << "}\n";
            } else {
                write_indent(os, 4);
                os << "return ";
                emit_expr(os, *arm->body);
                os << ";\n";
                seen_unguarded = true;
            }
            write_indent(os, 3);
            os << "}\n";
            if (seen_unguarded) {
                break;  // anything after an unguarded arm is dead code
            }
        }
        // If every arm in the group was guarded, the constexpr-if
        // branch can fall off the end without returning — C++ refuses
        // to compile that. Terminate with the default arm or
        // std::unreachable so the branch is well-formed.
        if (!seen_unguarded) {
            write_indent(os, 3);
            if (default_arm != nullptr && default_arm->body != nullptr) {
                os << "return ";
                emit_expr(os, *default_arm->body);
                os << ";\n";
            } else {
                os << "std::unreachable();\n";
            }
        }
        write_indent(os, 2);
        os << "}";
        first = false;
    }

    if (default_arm != nullptr) {
        os << " else {\n";
        write_indent(os, 3);
        os << "return ";
        emit_expr(os, *default_arm->body);
        os << ";\n";
        write_indent(os, 2);
        os << "}";
    } else {
        // Sema's exhaustiveness check should have caught a real miss; this
        // branch only exists to satisfy the C++ compiler's "no return"
        // diagnostic on the lambda.
        os << " else { std::unreachable(); }";
    }
    os << "\n    }, ";
    emit_expr(os, *m.scrutinee);
    os << ".value)";
}

// §17.7 pattern predicate. Writes a C++ expression that evaluates to
// `true` when the value spelled by `base` matches the pattern. Used by
// the value-scrutinee if-chain (where the predicate gates the arm's
// body). The base string is composed deeper for tuple elements
// (`std::get<i>(parent_base)`), so the helper stays purely functional.
void CppEmitter::emit_pat_predicate(std::ostream& os,
                                    const ast::Pattern& p,
                                    std::string_view base) {
    switch (p.kind) {
    case ast::NodeKind::WildcardPat:
    case ast::NodeKind::IdentPat:
    case ast::NodeKind::BindPat:
        // Always-match — the binding is emitted separately if any.
        os << "true";
        return;
    case ast::NodeKind::LiteralPat: {
        const auto& lp = static_cast<const ast::LiteralPat&>(p);
        os << "(" << base << " == ";
        if (lp.literal) {
            emit_expr(os, *lp.literal);
        }
        os << ")";
        return;
    }
    case ast::NodeKind::RangePat: {
        const auto& rp = static_cast<const ast::RangePat&>(p);
        os << "(";
        if (rp.low) {
            emit_expr(os, *rp.low);
        }
        os << " <= " << base << " && " << base << (rp.inclusive ? " <= " : " < ");
        if (rp.high) {
            emit_expr(os, *rp.high);
        }
        os << ")";
        return;
    }
    case ast::NodeKind::OrPat: {
        const auto& op = static_cast<const ast::OrPat&>(p);
        os << "(";
        for (std::size_t i = 0; i < op.alternatives.size(); ++i) {
            if (i != 0) {
                os << " || ";
            }
            if (op.alternatives[i]) {
                emit_pat_predicate(os, *op.alternatives[i], base);
            }
        }
        os << ")";
        return;
    }
    case ast::NodeKind::TuplePat: {
        const auto& tp = static_cast<const ast::TuplePat&>(p);
        os << "(";
        bool first = true;
        for (std::size_t i = 0; i < tp.elements.size(); ++i) {
            if (tp.elements[i] == nullptr) {
                continue;
            }
            if (!first) {
                os << " && ";
            }
            first = false;
            std::string sub_base = std::format("std::get<{}>({})", i, base);
            emit_pat_predicate(os, *tp.elements[i], sub_base);
        }
        if (first) {
            os << "true";
        }
        os << ")";
        return;
    }
    default:
        // Unrecognized — return true so the arm is always taken (and
        // the caller's `unsupported` diagnostic surfaces the real
        // issue elsewhere).
        os << "true";
        return;
    }
}

// Emit `auto&& name = base;` for every binding the pattern introduces.
// IdentPat and BindPat bind directly; TuplePat recurses into each
// element with `std::get<i>(base)` as the new base. Literal, range,
// or, and wildcard patterns introduce no bindings.
void CppEmitter::emit_pat_bindings(std::ostream& os,
                                   const ast::Pattern& p,
                                   std::string_view base,
                                   int indent) {
    switch (p.kind) {
    case ast::NodeKind::IdentPat:
        write_indent(os, indent);
        os << "auto&& " << static_cast<const ast::IdentPat&>(p).name << " = " << base << ";\n";
        return;
    case ast::NodeKind::BindPat:
        write_indent(os, indent);
        os << "auto&& " << static_cast<const ast::BindPat&>(p).name << " = " << base << ";\n";
        return;
    case ast::NodeKind::TuplePat: {
        const auto& tp = static_cast<const ast::TuplePat&>(p);
        for (std::size_t i = 0; i < tp.elements.size(); ++i) {
            if (tp.elements[i]) {
                std::string sub_base = std::format("std::get<{}>({})", i, base);
                emit_pat_bindings(os, *tp.elements[i], sub_base, indent);
            }
        }
        return;
    }
    default:
        return;
    }
}

// Match over a non-enum scrutinee. Lowers to an IIFE that binds the
// scrutinee to `__vstr_m` and walks an if/else-if chain over each
// arm's predicate. Bindings (for IdentPat / BindPat / TuplePat
// elements) land inside the matched branch's brace. Missing default
// emits `std::unreachable()` to satisfy the C++ "no return path" rule.
void CppEmitter::emit_match_value_scrutinee(std::ostream& os, const ast::MatchExpr& m) {
    os << "[&]() -> auto {\n";
    os << "        auto&& __vstr_m = ";
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
        if (arm.pattern == nullptr) {
            continue;
        }
        write_indent(os, 2);
        os << (first ? "if (" : "else if (");
        emit_pat_predicate(os, *arm.pattern, "__vstr_m");
        if (arm.guard) {
            os << " && (";
            emit_expr(os, *arm.guard);
            os << ")";
        }
        os << ") {\n";
        emit_pat_bindings(os, *arm.pattern, "__vstr_m", 3);
        write_indent(os, 3);
        os << "return ";
        if (arm.body) {
            emit_expr(os, *arm.body);
        }
        os << ";\n";
        write_indent(os, 2);
        os << "}\n";
        first = false;
    }

    if (default_arm != nullptr) {
        write_indent(os, 2);
        os << (first ? "{ " : "else { ");
        os << "return ";
        if (default_arm->body) {
            emit_expr(os, *default_arm->body);
        }
        os << "; }\n";
    } else {
        write_indent(os, 2);
        os << (first ? "std::unreachable();\n" : "else { std::unreachable(); }\n");
    }
    write_indent(os, 1);
    os << "}()";
}

}  // namespace vestra::codegen
