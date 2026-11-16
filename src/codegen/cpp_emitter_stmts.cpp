// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// Statement emission: blocks, tuple-pattern destructuring helpers,
// emit_stmt, and statement-position expression lowering.
// Split out of cpp_emitter.cpp; see cpp_emitter_internal.hpp for the
// shared free helpers.

#include "vestra/sema/asm_writes.hpp"
#include "vestra/codegen/cpp_emitter.hpp"

#include "cpp_emitter_internal.hpp"

#include <algorithm>
#include <format>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vestra::codegen {

using detail::write_indent;

void CppEmitter::emit_block(std::ostream& os, const ast::BlockExpr& b, int indent) {
    os << "{\n";
    for (const auto& s : b.stmts) {
        emit_stmt(os, *s, indent + 1);
    }
    write_indent(os, indent);
    os << "}";
}

void CppEmitter::collect_tuple_pat_names(
    const ast::TuplePat& tp,
    std::vector<std::string>& names,
    std::vector<std::pair<std::string, const ast::TuplePat*>>& followons) {
    for (std::size_t i = 0; i < tp.elements.size(); ++i) {
        const auto& sub = tp.elements[i];
        if (sub && sub->kind == ast::NodeKind::IdentPat) {
            names.push_back(static_cast<const ast::IdentPat&>(*sub).name);
        } else if (sub && sub->kind == ast::NodeKind::BindPat) {
            names.push_back(static_cast<const ast::BindPat&>(*sub).name);
        } else if (sub && sub->kind == ast::NodeKind::WildcardPat) {
            names.push_back(std::format("__vstr_tp_unused_{}", tuple_pat_counter_++));
        } else if (sub && sub->kind == ast::NodeKind::TuplePat) {
            // Nested sub-tuple: bind a placeholder at this level, then
            // unpack it in a follow-on statement after the parent
            // structured binding.
            auto placeholder = std::format("__vstr_tp{}", tuple_pat_counter_++);
            names.push_back(placeholder);
            followons.emplace_back(std::move(placeholder),
                                   &static_cast<const ast::TuplePat&>(*sub));
        } else {
            names.push_back(std::format("__vstr_tp_bind_{}", tuple_pat_counter_++));
        }
    }
}

void CppEmitter::emit_tuple_pat_followons(
    std::ostream& os,
    const std::vector<std::pair<std::string, const ast::TuplePat*>>& followons,
    int indent) {
    for (const auto& [placeholder, inner_tp] : followons) {
        std::vector<std::string> names;
        std::vector<std::pair<std::string, const ast::TuplePat*>> nested;
        collect_tuple_pat_names(*inner_tp, names, nested);
        write_indent(os, indent);
        os << "auto [";
        for (std::size_t i = 0; i < names.size(); ++i) {
            if (i != 0) {
                os << ", ";
            }
            os << names[i];
        }
        os << "] = " << placeholder << ";\n";
        emit_tuple_pat_followons(os, nested, indent);
    }
}

void CppEmitter::emit_stmt(std::ostream& os, const ast::Stmt& s, int indent) {
    // Annex §14.12.3 `@no_auto_barrier` on a statement drops the auto-emitted
    // post-write barrier from the sysreg writes in that statement only.
    struct NoAutoBarrierStmt {
        bool* flag;
        bool prev;
        ~NoAutoBarrierStmt() { *flag = prev; }
    } no_barrier{&current_no_auto_barrier_, current_no_auto_barrier_};
    if (s.has_attribute("no_auto_barrier")) {
        current_no_auto_barrier_ = true;
    }
    // §9 try-hoisting: pull every TryExpr-Propagating in this statement's
    // expressions out into a stmt-position let-binding, so propagation
    // really escapes the enclosing throws(E) fn. The active_hoists_
    // pointer lives only for the duration of this call — emit_expr looks
    // it up to substitute the bound name when it later encounters the
    // hoisted TryExpr.
    std::vector<TryHoist> hoists;
    collect_stmt_hoists(s, hoists);
    for (const auto& h : hoists) {
        emit_try_hoist(os, h, indent);
    }
    const auto* prev_hoists = active_hoists_;
    active_hoists_ = &hoists;
    write_indent(os, indent);
    switch (s.kind) {
    case ast::NodeKind::LetStmt: {
        const auto& l = static_cast<const ast::LetStmt&>(s);
        // §6 tuple destructuring: `let (a, b, ...) = expr` lowers to
        // a C++17 structured binding. The annotation is dropped — the
        // element types come from the tuple value's static type.
        // Nested sub-tuples bind a placeholder here and unpack it in
        // a sibling follow-on statement (structured bindings are
        // single-level in C++).
        if (l.pattern && l.pattern->kind == ast::NodeKind::TuplePat) {
            const auto& tp = static_cast<const ast::TuplePat&>(*l.pattern);
            std::vector<std::string> names;
            std::vector<std::pair<std::string, const ast::TuplePat*>> followons;
            collect_tuple_pat_names(tp, names, followons);
            os << "auto [";
            for (std::size_t i = 0; i < names.size(); ++i) {
                if (i != 0) {
                    os << ", ";
                }
                os << names[i];
            }
            os << "] = ";
            if (l.value) {
                emit_expr(os, *l.value);
            }
            os << ";\n";
            emit_tuple_pat_followons(os, followons, indent);
            break;
        }
        // `let _ = e` evaluates and discards: `(void)(e);`. (A named
        // temporary would collide with the next one in the same scope.)
        if (l.pattern && l.pattern->kind == ast::NodeKind::WildcardPat && l.value) {
            os << "(void)(";
            emit_expr(os, *l.value);
            os << ");\n";
            break;
        }
        // Vestra's `let` is "no reassignment" — but it does allow consumption
        // (move). C++'s `const` is stricter (no rebind AND no move-from), so
        // we use plain `auto` / `T` here. Sema rejects assignment to `let`
        // bindings, which is the part C++'s const was buying us.
        //
        // §9 `let NAME = try EXPR` no longer needs a special case: the
        // try-hoisting pass above pre-emits the propagation escape, then
        // emit_expr substitutes the hoisted name when it hits the
        // TryExpr — so `auto NAME = __vstr_t0;` is correct.
        // The pattern is an IdentPat / BindPat in the common case;
        // we materialize the name up front so `emit_type_with_name`
        // can embed it inside the function-pointer parens. Anything
        // else (refutable patterns, etc.) falls back to `_bind` per
        // the prior behavior.
        std::string bind_name;
        if (l.pattern && l.pattern->kind == ast::NodeKind::IdentPat) {
            bind_name = static_cast<const ast::IdentPat&>(*l.pattern).name;
        } else if (l.pattern && l.pattern->kind == ast::NodeKind::BindPat) {
            bind_name = static_cast<const ast::BindPat&>(*l.pattern).name;
        } else {
            bind_name = "_bind";
        }
        if (l.type) {
            emit_type_with_name(os, *l.type, bind_name);
        } else {
            os << "auto " << bind_name;
        }
        os << " = ";
        if (l.value) {
            closure_to_fn_ptr_ = l.type != nullptr && l.type->kind == ast::NodeKind::FunctionType
                                 && l.value->kind == ast::NodeKind::ClosureExpr;
            emit_value(os, *l.value);  // §10 move a move-only lvalue initializer
            closure_to_fn_ptr_ = false;
        }
        os << ";\n";
        break;
    }
    case ast::NodeKind::VarStmt: {
        const auto& v = static_cast<const ast::VarStmt&>(s);
        if (v.pattern && v.pattern->kind == ast::NodeKind::TuplePat) {
            // Same shape as the LetStmt path. `var` becomes a mutable
            // structured binding via `auto [a, b] = ...;` (C++23's
            // mutable [auto&] form would let us bind by ref; for v0.5
            // we always take by-value to match Vestra's `var` move).
            const auto& tp = static_cast<const ast::TuplePat&>(*v.pattern);
            std::vector<std::string> names;
            std::vector<std::pair<std::string, const ast::TuplePat*>> followons;
            collect_tuple_pat_names(tp, names, followons);
            os << "auto [";
            for (std::size_t i = 0; i < names.size(); ++i) {
                if (i != 0) {
                    os << ", ";
                }
                os << names[i];
            }
            os << "] = ";
            if (v.value) {
                emit_expr(os, *v.value);
            }
            os << ";\n";
            emit_tuple_pat_followons(os, followons, indent);
            break;
        }
        if (v.pattern && v.pattern->kind == ast::NodeKind::WildcardPat && v.value) {
            os << "(void)(";
            emit_expr(os, *v.value);
            os << ");\n";
            break;
        }
        if (v.type) {
            emit_type(os, *v.type);
            os << " ";
        } else {
            os << "auto ";
        }
        if (v.pattern && v.pattern->kind == ast::NodeKind::IdentPat) {
            os << static_cast<const ast::IdentPat&>(*v.pattern).name;
        } else if (v.pattern && v.pattern->kind == ast::NodeKind::BindPat) {
            os << static_cast<const ast::BindPat&>(*v.pattern).name;
        } else {
            os << "_bind";
        }
        os << " = ";
        if (v.value) {
            emit_value(os, *v.value);  // §10 move a move-only lvalue initializer
        }
        os << ";\n";
        break;
    }
    case ast::NodeKind::ExprStmt:
        emit_stmt_expr(os, *static_cast<const ast::ExprStmt&>(s).expr, /*return_value=*/false);
        break;
    case ast::NodeKind::ReturnStmt: {
        const auto& r = static_cast<const ast::ReturnStmt&>(s);
        if (r.value) {
            emit_stmt_expr(os, *r.value, /*return_value=*/true);
        } else {
            // §9 a Unit throws fn returns std::expected<void, E>: a bare
            // return must still produce the success value (F9b).
            const char* tail = current_func_unit_throws_ ? " {};\n" : ";\n";
            os << (current_func_is_async_ ? "co_return" : "return") << tail;
        }
        break;
    }
    case ast::NodeKind::AsmStmt:
        emit_asm(os, static_cast<const ast::AsmStmt&>(s), indent);
        break;
    case ast::NodeKind::BreakStmt:
        os << "break;\n";
        break;
    case ast::NodeKind::ContinueStmt:
        os << "continue;\n";
        break;
    case ast::NodeKind::AssignStmt: {
        const auto& a = static_cast<const ast::AssignStmt&>(s);
        // §4 `x /= y` and `x %= y` check the divisor as `/` and `%` do.
        if ((a.op == ast::AssignOp::DivAssign || a.op == ast::AssignOp::ModAssign)
            && !divisor_is_safe(*a.value)) {
            os << (a.op == ast::AssignOp::DivAssign ? "__vstr::idiv_assign("
                                                    : "__vstr::irem_assign(");
            emit_expr(os, *a.target);
            os << ", ";
            emit_expr(os, *a.value);
            os << ");\n";
            break;
        }
        // §4 `x += y`, `x -= y` and `x *= y` trap on integer overflow as
        // `+`, `-` and `*` do.
        if (a.op == ast::AssignOp::AddAssign || a.op == ast::AssignOp::SubAssign
            || a.op == ast::AssignOp::MulAssign) {
            os << (a.op == ast::AssignOp::AddAssign   ? "__vstr::add_assign("
                   : a.op == ast::AssignOp::SubAssign ? "__vstr::sub_assign("
                                                      : "__vstr::mul_assign(");
            emit_expr(os, *a.target);
            os << ", ";
            emit_wrap_operand(os, *a.value);
            os << ");\n";
            break;
        }
        emit_expr(os, *a.target);
        switch (a.op) {
        case ast::AssignOp::Assign:
            os << " = ";
            break;
        case ast::AssignOp::AddAssign:
            os << " += ";
            break;
        case ast::AssignOp::SubAssign:
            os << " -= ";
            break;
        case ast::AssignOp::MulAssign:
            os << " *= ";
            break;
        case ast::AssignOp::DivAssign:
            os << " /= ";
            break;
        case ast::AssignOp::ModAssign:
            os << " %= ";
            break;
        case ast::AssignOp::BitAndAssign:
            os << " &= ";
            break;
        case ast::AssignOp::BitOrAssign:
            os << " |= ";
            break;
        case ast::AssignOp::BitXorAssign:
            os << " ^= ";
            break;
        case ast::AssignOp::ShlAssign:
            os << " <<= ";
            break;
        case ast::AssignOp::ShrAssign:
            os << " >>= ";
            break;
        }
        emit_expr(os, *a.value);
        os << ";\n";
        break;
    }
    case ast::NodeKind::WhileStmt: {
        const auto& w = static_cast<const ast::WhileStmt&>(s);
        if (w.repeat) {
            // `repeat { B } while C` is C++'s do-while: `continue` jumps to
            // the test, as in Vestra.
            os << "do ";
            if (w.body && w.body->kind == ast::NodeKind::BlockExpr) {
                emit_block(os, static_cast<const ast::BlockExpr&>(*w.body), indent);
            } else {
                os << "{}";
            }
            os << " while (";
            emit_expr(os, *w.cond);
            os << ");\n";
            break;
        }
        os << "while (";
        emit_expr(os, *w.cond);
        os << ") ";
        if (w.body && w.body->kind == ast::NodeKind::BlockExpr) {
            emit_block(os, static_cast<const ast::BlockExpr&>(*w.body), indent);
            os << "\n";
        } else {
            os << "{}\n";
        }
        break;
    }
    case ast::NodeKind::ForStmt: {
        // §5 desugaring: two shapes.
        //   * Range / RangeLt: lower to a C++ counted for-loop, with
        //     the end expression captured once in a sibling decl.
        //   * Iterator protocol: lower to a while-true that calls
        //     __vstr_iter.next() and breaks on .none; the loop body
        //     binds the unwrapped value as `auto x`.
        const auto& f = static_cast<const ast::ForStmt&>(s);
        auto bind_name = [&]() -> std::string {
            if (f.pattern && f.pattern->kind == ast::NodeKind::IdentPat) {
                return static_cast<const ast::IdentPat&>(*f.pattern).name;
            }
            if (f.pattern && f.pattern->kind == ast::NodeKind::BindPat) {
                return static_cast<const ast::BindPat&>(*f.pattern).name;
            }
            return "_it";
        };
        const ast::BinaryExpr* range = nullptr;
        if (f.iter && f.iter->kind == ast::NodeKind::BinaryExpr) {
            const auto& b = static_cast<const ast::BinaryExpr&>(*f.iter);
            if (b.op == ast::BinaryOp::Range || b.op == ast::BinaryOp::RangeLt) {
                range = &b;
            }
        }
        if (range != nullptr) {
            const char* cmp = range->op == ast::BinaryOp::RangeLt ? "<" : "<=";
            os << "for (auto " << bind_name() << " = ";
            emit_expr(os, *range->lhs);
            os << ", __vstr_end = ";
            emit_expr(os, *range->rhs);
            os << "; " << bind_name() << " " << cmp << " __vstr_end; ++" << bind_name() << ") ";
            if (f.body && f.body->kind == ast::NodeKind::BlockExpr) {
                emit_block(os, static_cast<const ast::BlockExpr&>(*f.body), indent);
                os << "\n";
            } else {
                os << "{}\n";
            }
            break;
        }
        // §18.5 / §10 / §13 `for x in xs` over a Vec[T], a Span[T]/MutSpan[T]
        // (incl. a Soa column view), lowers to a C++ range-based for. `auto&&`
        // binds each element without copying (free for struct elements); the
        // loop variable is immutable on the Vestra side, so the reference is
        // read-only in practice.
        if (resolution_ != nullptr && f.iter) {
            if (auto it = resolution_->type_of(f.iter.get());
                it != nullptr
                && (it->kind() == sema::TypeKind::Vec || it->kind() == sema::TypeKind::Span
                    || it->kind() == sema::TypeKind::MutSpan)) {
                // §18.5 tuple-pattern binding: `for (k, v) in m.entries()` over a
                // Vec[(K, V)] lowers to a C++ structured binding in the range-for
                // header. Nested sub-tuples bind a placeholder and unpack in
                // sibling follow-on statements (same machinery as let / the
                // iterator-protocol path).
                if (f.pattern && f.pattern->kind == ast::NodeKind::TuplePat) {
                    const auto& tp = static_cast<const ast::TuplePat&>(*f.pattern);
                    std::vector<std::string> names;
                    std::vector<std::pair<std::string, const ast::TuplePat*>> followons;
                    collect_tuple_pat_names(tp, names, followons);
                    os << "for (auto&& [";
                    for (std::size_t i = 0; i < names.size(); ++i) {
                        if (i != 0) {
                            os << ", ";
                        }
                        os << names[i];
                    }
                    os << "] : ";
                    emit_expr(os, *f.iter);
                    os << ") ";
                    if (followons.empty()) {
                        if (f.body && f.body->kind == ast::NodeKind::BlockExpr) {
                            emit_block(os, static_cast<const ast::BlockExpr&>(*f.body), indent);
                            os << "\n";
                        } else {
                            os << "{}\n";
                        }
                    } else {
                        os << "{ ";
                        emit_tuple_pat_followons(os, followons, indent + 1);
                        if (f.body && f.body->kind == ast::NodeKind::BlockExpr) {
                            emit_block(os, static_cast<const ast::BlockExpr&>(*f.body), indent);
                        }
                        os << " }\n";
                    }
                    break;
                }
                os << "for (auto&& " << bind_name() << " : ";
                emit_expr(os, *f.iter);
                os << ") ";
                if (f.body && f.body->kind == ast::NodeKind::BlockExpr) {
                    emit_block(os, static_cast<const ast::BlockExpr&>(*f.body), indent);
                    os << "\n";
                } else {
                    os << "{}\n";
                }
                break;
            }
        }
        // Iterator protocol: capture iter as a mutable local, drive
        // while-true / .next() / break on nullopt.
        //
        // §6 TuplePat support: when the loop variable is a tuple
        // pattern (e.g., `for (a, b) in zip(...)`), bind via C++
        // structured binding over `*__vstr_o`. Sub-patterns reuse the
        // collect_tuple_pat_names / emit_tuple_pat_followons machinery
        // that param-level tuple destructuring uses, so nested tuple
        // patterns lower the same way.
        os << "{ auto __vstr_iter = ";
        emit_expr(os, *f.iter);
        os << "; while (true) { auto __vstr_o = __vstr_iter.next(); "
              "if (!__vstr_o.has_value()) { break; } ";
        if (f.pattern && f.pattern->kind == ast::NodeKind::TuplePat) {
            const auto& tp = static_cast<const ast::TuplePat&>(*f.pattern);
            std::vector<std::string> names;
            std::vector<std::pair<std::string, const ast::TuplePat*>> followons;
            collect_tuple_pat_names(tp, names, followons);
            os << "auto [";
            for (std::size_t i = 0; i < names.size(); ++i) {
                if (i != 0) {
                    os << ", ";
                }
                os << names[i];
            }
            os << "] = std::move(*__vstr_o); ";
            emit_tuple_pat_followons(os, followons, indent + 1);
        } else {
            os << "auto " << bind_name() << " = std::move(*__vstr_o); ";
        }
        if (f.body && f.body->kind == ast::NodeKind::BlockExpr) {
            emit_block(os, static_cast<const ast::BlockExpr&>(*f.body), indent);
        } else {
            os << "{}";
        }
        os << " } }\n";
        break;
    }
    case ast::NodeKind::WithStmt: {
        // §17.4 `with` statement: open a fresh C++ block, declare any
        // named bindings (`with mmio = createRegion() { ... }`) at
        // their value's deduced type, then emit the body's statements
        // at one-deeper indent. Cap-typed bindings (`with Alloc { ... }`)
        // contribute no identifier but still take the sub-scope so
        // their lifetime story is uniform.
        const auto& w = static_cast<const ast::WithStmt&>(s);
        os << "{\n";
        for (const auto& b : w.bindings) {
            if (!b.name.empty() && b.value) {
                write_indent(os, indent + 1);
                // §17.4 type annotation on a name-binding: emit the
                // declared type instead of `auto&&` so the user's
                // intent shows up at the C++ layer. The function-
                // pointer declarator quirk (where the name embeds
                // inside the parens) is handled by
                // emit_type_with_name.
                if (b.type_annotation) {
                    emit_type_with_name(os, *b.type_annotation, b.name);
                    os << " = ";
                } else {
                    os << "auto&& " << b.name << " = ";
                }
                emit_expr(os, *b.value);
                os << ";\n";
            }
            // Cap-typed bindings (TYPE = EXPR shape with no name) are
            // discharged by the capability checker; the value runs but
            // doesn't introduce an identifier. Discard via void-cast
            // so it doesn't trigger -Wunused-result on a [[nodiscard]]
            // expression.
            else if (b.name.empty() && b.value) {
                write_indent(os, indent + 1);
                os << "(void)(";
                emit_expr(os, *b.value);
                os << ");\n";
            }
        }
        if (w.body && w.body->kind == ast::NodeKind::BlockExpr) {
            const auto& blk = static_cast<const ast::BlockExpr&>(*w.body);
            for (const auto& stmt : blk.stmts) {
                emit_stmt(os, *stmt, indent + 1);
            }
        }
        write_indent(os, indent);
        os << "}\n";
        break;
    }
    default:
        unsupported(os, "stmt", s.range);
        os << "\n";
        break;
    }
    active_hoists_ = prev_hoists;
}

// §14.4 `asm(...) { "…" }` lowers to GCC/Clang inline asm. With no operands or
// clobbers it is basic asm (the only kind a naked function may hold), and the
// template passes through but for `{{` / `}}`. Otherwise it is extended asm:
//   * `{name}` -> `%[name]`, and a literal `%` doubles to `%%`;
//   * a named output is early-clobber (`=&r`), so it never shares a register
//     with an input still to be read; a named inout is `+r`;
//   * a pinned operand rides in a `register T v __asm__("reg")` local in a
//     block of its own, copied back to its place after the asm. A register
//     pinned as both input and output is one `+r` operand.
void CppEmitter::emit_asm(std::ostream& os, const ast::AsmStmt& a, int indent) {
    const bool extended =
        !a.inputs.empty() || !a.outputs.empty() || !a.inouts.empty() || !a.clobbers.empty();
    std::string tmpl;
    for (std::size_t li = 0; li < a.lines.size(); ++li) {
        if (li != 0) {
            tmpl += "\\n\\t";
        }
        const std::string_view line = a.lines[li];
        for (std::size_t i = 0; i < line.size(); ++i) {
            const char c = line[i];
            if ((c == '{' || c == '}') && i + 1 < line.size() && line[i + 1] == c) {
                tmpl += c;
                ++i;
            } else if (c == '{' && extended) {
                const auto close = line.find('}', i + 1);
                tmpl += "%[";
                tmpl += line.substr(i + 1, close - i - 1);
                tmpl += "]";
                i = close;
            } else if (c == '%' && extended) {
                tmpl += "%%";
            } else {
                tmpl += c;
            }
        }
    }
    const char* kw = a.is_volatile ? "__asm__ volatile(\"" : "__asm__(\"";
    if (!extended) {
        os << kw << tmpl << "\");\n";
        return;
    }

    // Pinned registers, in first-seen order: the input that seeds each, the
    // place an output copies back to, and whether it is read-write.
    struct Pin {
        std::string reg;
        const ast::Expr* in = nullptr;
        const ast::Expr* out = nullptr;
        bool inout = false;
    };
    std::vector<Pin> pins;
    auto pin = [&](const std::string& reg) -> Pin& {
        for (auto& p : pins) {
            if (p.reg == reg) {
                return p;
            }
        }
        pins.push_back(Pin{reg});
        return pins.back();
    };
    for (const auto& op : a.inputs) {
        if (!op.reg.empty()) {
            pin(op.reg).in = op.value.get();
        }
    }
    for (const auto& op : a.outputs) {
        if (!op.reg.empty()) {
            pin(op.reg).out = op.value.get();
        }
    }
    for (const auto& op : a.inouts) {
        if (!op.reg.empty()) {
            auto& p = pin(op.reg);
            p.in = op.value.get();
            p.out = op.value.get();
            p.inout = true;
        }
    }
    auto pin_var = [](const std::string& reg) { return "__vstr_asm_" + reg; };
    auto emit_type_of = [&](const ast::Expr& e) {
        auto t = resolution_ != nullptr ? resolution_->type_of(&e) : nullptr;
        if (t != nullptr) {
            emit_sema_type(os, t);
        } else {
            os << "std::uint64_t";
        }
    };

    if (!pins.empty()) {
        os << "{\n";
        for (const auto& p : pins) {
            detail::write_indent(os, indent + 1);
            os << "register ";
            emit_type_of(p.out != nullptr ? *p.out : *p.in);
            os << " " << pin_var(p.reg) << " __asm__(\"" << p.reg << "\")";
            if (p.in != nullptr) {
                os << " = ";
                emit_expr(os, *p.in);
            }
            os << ";\n";
        }
        detail::write_indent(os, indent + 1);
    }
    // Build the three operand sections, then drop empty trailing ones: GCC
    // syntax needs a `:` only up to the last section in use.
    std::ostringstream outs;
    std::ostringstream ins;
    std::ostringstream clob;
    auto sep = [](std::ostringstream& o) {
        if (o.tellp() > 0) {
            o << ", ";
        }
    };
    for (const auto& op : a.outputs) {
        if (op.reg.empty()) {
            sep(outs);
            outs << "[" << op.name << "] \"=&r\"(";
            emit_expr(outs, *op.value);
            outs << ")";
        }
    }
    for (const auto& op : a.inouts) {
        if (op.reg.empty()) {
            sep(outs);
            outs << "[" << op.name << "] \"+r\"(";
            emit_expr(outs, *op.value);
            outs << ")";
        }
    }
    for (const auto& p : pins) {
        if (p.out != nullptr) {
            sep(outs);
            outs << (p.in != nullptr ? "\"+r\"(" : "\"=r\"(") << pin_var(p.reg) << ")";
        }
    }
    for (const auto& op : a.inputs) {
        if (op.reg.empty()) {
            sep(ins);
            ins << "[" << op.name << "] \"r\"(static_cast<";
            auto t = resolution_ != nullptr ? resolution_->type_of(op.value.get()) : nullptr;
            if (t != nullptr) {
                emit_sema_type(ins, t);
            } else {
                ins << "std::uint64_t";
            }
            ins << ">(";
            emit_expr(ins, *op.value);
            ins << "))";
        }
    }
    for (const auto& p : pins) {
        if (p.out == nullptr) {
            sep(ins);
            ins << "\"r\"(" << pin_var(p.reg) << ")";
        }
    }
    for (const auto& c : a.clobbers) {
        // Annex §14.4 a sysreg clobber records the write for the checker;
        // the C++ compiler has no such register, and the asm is volatile.
        if (c != "memory" && c != "cc" && sema::is_a64_sysreg_name(c)) {
            continue;
        }
        sep(clob);
        clob << "\"" << c << "\"";
    }
    const std::string sections[3] = {outs.str(), ins.str(), clob.str()};
    int last = 2;
    while (last >= 0 && sections[last].empty()) {
        --last;
    }
    os << kw << tmpl << "\"";
    for (int i = 0; i <= last; ++i) {
        os << " :" << (sections[i].empty() ? "" : " ") << sections[i];
    }
    os << ");\n";

    if (!pins.empty()) {
        for (const auto& p : pins) {
            if (p.out != nullptr) {
                detail::write_indent(os, indent + 1);
                emit_expr(os, *p.out);
                os << " = " << pin_var(p.reg) << ";\n";
            }
        }
        detail::write_indent(os, indent);
        os << "}\n";
    }
}

namespace {

// Does `n` hold a `return`, or a `break` / `continue` not inside a loop of
// its own? Closures are their own control context.
bool escapes(const ast::Node& n, bool in_loop) {
    auto x = [&](const ast::Node* c, bool loop) { return c != nullptr && escapes(*c, loop); };
    switch (n.kind) {
    case ast::NodeKind::ReturnStmt:
        return true;
    case ast::NodeKind::BreakStmt:
    case ast::NodeKind::ContinueStmt:
        return !in_loop;
    case ast::NodeKind::BlockExpr:
        for (const auto& st : static_cast<const ast::BlockExpr&>(n).stmts) {
            if (escapes(*st, in_loop)) {
                return true;
            }
        }
        return false;
    case ast::NodeKind::ExprStmt:
        return x(static_cast<const ast::ExprStmt&>(n).expr.get(), in_loop);
    case ast::NodeKind::IfExpr: {
        const auto& i = static_cast<const ast::IfExpr&>(n);
        return x(i.then_branch.get(), in_loop) || x(i.else_branch.get(), in_loop);
    }
    case ast::NodeKind::MatchExpr:
        for (const auto& arm : static_cast<const ast::MatchExpr&>(n).arms) {
            if (x(arm.body.get(), in_loop)) {
                return true;
            }
        }
        return false;
    case ast::NodeKind::WhileStmt:
        return x(static_cast<const ast::WhileStmt&>(n).body.get(), true);
    case ast::NodeKind::ForStmt:
        return x(static_cast<const ast::ForStmt&>(n).body.get(), true);
    case ast::NodeKind::WithStmt:
        return x(static_cast<const ast::WithStmt&>(n).body.get(), in_loop);
    default:
        return false;
    }
}

}  // namespace

bool CppEmitter::match_arms_escape(const ast::MatchExpr& m) {
    for (const auto& arm : m.arms) {
        if (arm.body != nullptr && escapes(*arm.body, false)) {
            return true;
        }
    }
    return false;
}

void CppEmitter::emit_match_stmt(std::ostream& os, const ast::MatchExpr& m) {
    const int n = match_stmt_counter_++;
    const std::string var = std::format("__vstr_ms{}", n);
    const std::string end = var + "_end";
    auto st = resolution_ != nullptr ? resolution_->type_of(m.scrutinee.get()) : nullptr;
    const ast::EnumDecl* ed = nullptr;
    if (st != nullptr && st->kind() == sema::TypeKind::Enum && st->nominal_decl() != nullptr) {
        ed = static_cast<const ast::EnumDecl*>(st->nominal_decl());
    }
    const bool payload = ed != nullptr && detail::enum_is_sum_type(*ed);
    auto enum_name = [&]() {
        std::ostringstream o;
        if (st != nullptr && !st->parts().empty()) {
            emit_sema_type(o, st);
        } else {
            o << nominal_prefix(ed) << ed->name;
        }
        return o.str();
    };
    // The test for one (non-or) enum pattern.
    auto enum_test = [&](const ast::EnumPat& ep) {
        if (payload) {
            return std::format("std::holds_alternative<{}::{}_t>({}.value)", enum_name(), ep.case_name, var);
        }
        return std::format("{} == {}::{}", var, enum_name(), ep.case_name);
    };
    os << "{\n";
    os << "auto&& " << var << " = ";
    emit_expr(os, *m.scrutinee);
    os << ";\n";
    for (const auto& arm : m.arms) {
        std::string test = "true";
        const ast::Pattern* p = arm.is_default ? nullptr : arm.pattern.get();
        if (p != nullptr && ed != nullptr) {
            if (p->kind == ast::NodeKind::EnumPat) {
                test = enum_test(static_cast<const ast::EnumPat&>(*p));
            } else if (p->kind == ast::NodeKind::OrPat) {
                test.clear();
                for (const auto& alt : static_cast<const ast::OrPat&>(*p).alternatives) {
                    if (alt != nullptr && alt->kind == ast::NodeKind::EnumPat) {
                        test += (test.empty() ? "" : " || ") + enum_test(static_cast<const ast::EnumPat&>(*alt));
                    }
                }
                if (test.empty()) {
                    test = "true";
                }
            }
        } else if (p != nullptr) {
            std::ostringstream t;
            emit_pat_predicate(t, *p, var);
            test = t.str();
        }
        os << "if (" << test << ") {\n";
        // Bindings, arm-local.
        if (p != nullptr && ed != nullptr && p->kind == ast::NodeKind::EnumPat && payload) {
            const auto& ep = static_cast<const ast::EnumPat&>(*p);
            const auto c = std::ranges::find_if(ed->cases, [&](const auto& cs) { return cs.name == ep.case_name; });
            if (c != ed->cases.end() && !ep.children.empty()) {
                os << "auto&& " << var << "_alt = std::get<" << enum_name() << "::" << ep.case_name
                   << "_t>(" << var << ".value);\n";
                for (std::size_t i = 0; i < ep.children.size() && i < c->payload.size(); ++i) {
                    const auto& child = *ep.children[i];
                    const std::string field = c->payload[i].first.empty() ? std::format("_{}", i) : c->payload[i].first;
                    std::string name;
                    if (child.kind == ast::NodeKind::BindPat) {
                        name = static_cast<const ast::BindPat&>(child).name;
                    } else if (child.kind == ast::NodeKind::IdentPat) {
                        name = static_cast<const ast::IdentPat&>(child).name;
                    }
                    if (!name.empty()) {
                        os << "auto&& " << name << " = " << var << "_alt." << field << ";\n";
                    }
                }
            }
        } else if (p != nullptr && ed == nullptr) {
            emit_pat_bindings(os, *p, var, 0);
        } else if (p != nullptr && (p->kind == ast::NodeKind::IdentPat || p->kind == ast::NodeKind::BindPat)) {
            emit_pat_bindings(os, *p, var, 0);
        }
        if (arm.guard) {
            os << "if (";
            emit_expr(os, *arm.guard);
            os << ") {\n";
        }
        if (arm.body) {
            emit_stmt_expr(os, *arm.body, false);
        }
        os << "goto " << end << ";\n";
        if (arm.guard) {
            os << "}\n";
        }
        os << "}\n";
    }
    os << end << ":;\n";
    os << "}\n";
}

void CppEmitter::emit_stmt_expr(std::ostream& os, const ast::Expr& expr, bool return_value) {
    // §9 + §17.7 statement-position lowering. The kinds we special-case
    // are the ones whose value semantics fight a generic `expr;` /
    // `return expr;` lowering:
    //   * ThrowExpr — needs a real `return std::unexpected{...};`.
    //   * TryExpr (Propagating) — needs the canonical 3-line escape.
    //   * IfExpr — emit if/else statements so each branch's body can
    //     itself contain a throw / propagating-try; also avoids the IIFE
    //     return-type-deduction trap when branches have differing types.
    //   * BlockExpr — flatten its statements; recurse on the trailing
    //     expression so a `throw` at the end of a block fires correctly.
    // Everything else falls back to plain `expr;` / `return expr;`.
    if (expr.kind == ast::NodeKind::MatchExpr && !return_value
        && match_arms_escape(static_cast<const ast::MatchExpr&>(expr))) {
        emit_match_stmt(os, static_cast<const ast::MatchExpr&>(expr));
        return;
    }
    switch (expr.kind) {
    case ast::NodeKind::ThrowExpr: {
        // §11 — inside an `async func` the body is a coroutine, so the error
        // exit must be `co_return std::unexpected{e}` (a plain `return` is
        // ill-formed in a coroutine). A non-async throws fn uses `return`.
        const auto& th = static_cast<const ast::ThrowExpr&>(expr);
        os << (current_func_is_async_ ? "co_return std::unexpected{" : "return std::unexpected{");
        emit_expr(os, *th.inner);
        os << "};\n";
        return;
    }
    case ast::NodeKind::TryExpr: {
        const auto& tx = static_cast<const ast::TryExpr&>(expr);
        if (tx.form == ast::TryExpr::Form::Propagating) {
            // If hoisting registered a binding for this try, emit_expr
            // would substitute the name; just use the regular return /
            // void-cast path.
            if (lookup_try_hoist(&tx) != nullptr) {
                break;  // fall through to generic `<expr>;` / `return <expr>;`
            }
            // Both exits are function-level returns, so they co_return inside
            // an async throws coroutine (and `return` otherwise).
            const char* ret = current_func_is_async_ ? "co_return " : "return ";
            os << "{ auto __vstr_r = ";
            emit_expr(os, *tx.inner);
            os << "; if (!__vstr_r.has_value()) { " << ret
               << "std::unexpected{__vstr_r.error()}; } ";
            if (return_value) {
                os << ret << "*__vstr_r;";
            } else {
                os << "(void)*__vstr_r;";
            }
            os << " }\n";
            return;
        }
        break;  // fall through for try? / try!
    }
    case ast::NodeKind::IfExpr: {
        const auto& i = static_cast<const ast::IfExpr&>(expr);
        // If this IfExpr is the target of a conditional hoist, the
        // hoist pre-emitted its IIFE-returning-expected and bound the
        // result. Fall through to the generic tail; emit_expr will
        // substitute `*<name>` for the IfExpr.
        if (lookup_try_hoist(&expr) != nullptr) {
            break;
        }
        if (!i.let_name.empty()) {
            os << (if_let_borrows(i) ? "if (auto& __vstr_opt = " : "if (auto __vstr_opt = ");
            emit_expr(os, *i.let_init);
            os << "; __vstr_opt.has_value()) { auto&& " << i.let_name << " = *__vstr_opt; ";
            emit_stmt_expr(os, *i.then_branch, return_value);
            os << "}";
        } else {
            os << "if (";
            emit_expr(os, *i.cond);
            os << ") { ";
            emit_stmt_expr(os, *i.then_branch, return_value);
            os << "}";
        }
        if (i.else_branch) {
            os << " else { ";
            emit_stmt_expr(os, *i.else_branch, return_value);
            os << "}";
        }
        os << "\n";
        return;
    }
    case ast::NodeKind::BlockExpr: {
        const auto& b = static_cast<const ast::BlockExpr&>(expr);
        for (std::size_t i = 0; i < b.stmts.size(); ++i) {
            const auto& s = *b.stmts[i];
            bool last = (i + 1 == b.stmts.size());
            if (last && s.kind == ast::NodeKind::ExprStmt) {
                emit_stmt_expr(os, *static_cast<const ast::ExprStmt&>(s).expr, return_value);
            } else {
                emit_stmt(os, s, 0);
            }
        }
        return;
    }
    default:
        break;
    }
    // §9 branch-local hoists: at the trailing-expression position of a
    // statement-form emission (e.g. inside an IfExpr branch's `{ ... }`),
    // the parent statement's collect_try_hoists didn't descend into
    // conditional branches — so any propagating tries here aren't yet
    // in active_hoists_. Collect them locally, pre-emit at this brace
    // scope, and extend active_hoists_ so emit_expr substitutes the
    // names. Exclusion against active_hoists_ avoids duplicating
    // already-emitted hoists.
    std::vector<TryHoist> local_hoists;
    collect_try_hoists(expr, local_hoists, active_hoists_);
    std::vector<TryHoist> combined;
    const auto* prev_active = active_hoists_;
    if (!local_hoists.empty()) {
        for (const auto& h : local_hoists) {
            emit_try_hoist(os, h, 0);
        }
        if (prev_active != nullptr) {
            combined = *prev_active;
        }
        for (const auto& h : local_hoists) {
            combined.push_back(h);
        }
        active_hoists_ = &combined;
    }
    if (return_value) {
        // §11 — inside an async function this statement-position return is
        // a coroutine return. (Value-context IIFE lambdas in emit_expr keep
        // their plain `return`; only the function-level return changes.)
        os << (current_func_is_async_ ? "co_return " : "return ");
    }
    emit_expr(os, expr);
    os << ";\n";
    if (!local_hoists.empty()) {
        active_hoists_ = prev_active;
    }
}

}  // namespace vestra::codegen
