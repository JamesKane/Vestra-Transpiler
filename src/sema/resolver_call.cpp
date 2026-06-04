// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// Call-expression typing: numeric conversions, the builtin-intercept
// table (split/chunks/Box/Channel/Atomic/Mmio/Sysreg/parallel/…),
// struct construction, and generic function-call dispatch.
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
using detail::struct_is_repr_union;

namespace {

// §18.5 read-borrow at a call argument: an owned `String` argument flowing into
// a `Str` parameter borrows as a view for the duration of the call (std::string
// → std::string_view, a zero-copy conversion). This is the call-site
// counterpart of the §4 string lattice, which forbids String → Str at binding
// and return positions — there the view would outlive the owner and dangle —
// but not here, where the argument is guaranteed to outlive the call it is
// passed into. Distinct from `assignable` so the dangling cases stay rejected.
bool read_borrows_as(TypePtr arg, TypePtr param) noexcept {
    return arg != nullptr && param != nullptr && arg->kind() == TypeKind::String
           && param->kind() == TypeKind::Str;
}

}  // namespace

TypePtr Resolver::type_from_index_expr(const ast::Expr* e) {
    if (e == nullptr || e->kind != ast::NodeKind::IdentExpr) {
        return nullptr;  // only a bare type name today; nested generics defer
    }
    const auto& id = static_cast<const ast::IdentExpr&>(*e);
    if (auto k = TypeArena::primitive_kind_by_name(id.name); k != TypeKind::Error) {
        return types_->primitive(k);
    }
    const auto* sym = scopes_.current().lookup(id.name);
    if (sym == nullptr || sym->type == nullptr) {
        return nullptr;
    }
    switch (sym->kind) {
    case SymbolKind::Struct:
    case SymbolKind::Enum:
    case SymbolKind::Protocol:
    case SymbolKind::OpaqueType:
    case SymbolKind::GenericParam:
        return sym->type;
    default:
        return nullptr;
    }
}

TypePtr Resolver::check_call(const ast::CallExpr& c, TypePtr expected) {
    // §17.x conversion-call syntax: `Float64(i)` / `Int32(x)` — a bare
    // primitive numeric type name in callee position is an explicit
    // conversion. We handle it before normal callee resolution so the
    // type name doesn't trip "undefined name 'Float64'" against the
    // value scope (where it intentionally isn't registered — primitives
    // are types, not symbols). Requires exactly one positional numeric
    // argument; the result type is the target primitive.
    if (c.callee->kind == ast::NodeKind::IdentExpr) {
        const auto& callee_ident = static_cast<const ast::IdentExpr&>(*c.callee);
        auto kind = TypeArena::primitive_kind_by_name(callee_ident.name);
        if (kind != TypeKind::Error) {
            auto target = types_->primitive(kind);
            if (target != nullptr && target->is_numeric()) {
                if (c.args.size() != 1) {
                    for (const auto& a : c.args) {
                        (void)check_expr(*a.value);
                    }
                    error_at(c.range,
                             std::format("conversion to {} takes exactly one argument",
                                         target->describe()));
                    return target;
                }
                if (!c.args[0].label.empty()) {
                    error_at(c.args[0].value->range,
                             "conversion call argument cannot have a label");
                }
                auto arg_type = check_expr(*c.args[0].value);
                // §3 inverse opaque conversion: `T(q)` where q is an
                // opaque newtype admits the call iff the underlying
                // is numeric (the C++ side does `static_cast<T>(q)`
                // through `enum class Q : T {}`). Symmetric with the
                // `Q(t)` forward conversion below.
                bool ok = false;
                if (arg_type == nullptr || arg_type->is_error()) {
                    ok = true;
                } else if (arg_type->is_numeric()) {
                    ok = true;
                } else if (arg_type->kind() == TypeKind::OpaqueType
                           && arg_type->nominal_decl() != nullptr) {
                    const auto& od = static_cast<const ast::OpaqueDecl&>(*arg_type->nominal_decl());
                    if (od.underlying != nullptr) {
                        auto under = resolve_type(*od.underlying);
                        if (under != nullptr && under->is_numeric()) {
                            ok = true;
                        }
                    }
                }
                if (!ok) {
                    error_at(c.args[0].value->range,
                             std::format("cannot convert non-numeric {} to {}",
                                         arg_type->describe(),
                                         target->describe()));
                }
                return target;
            }
        }
    }

    // §11 Duration factories — Swift-like `.seconds(n)` / `.milliseconds(n)` /
    // `.microseconds(n)` / `.nanoseconds(n)`, each taking one integer (a count
    // in that unit) and yielding a Duration. Both the explicit
    // `Duration.<unit>(n)` form and the leading-dot `.<unit>(n)` form (resolved
    // against an expected Duration, e.g. a `: Duration` binding or a `timeout`
    // arm) are accepted; handled before generic callee resolution so the bare
    // `Duration` name isn't looked up as a value.
    {
        std::string_view factory;
        bool is_duration_factory = false;
        if (c.callee->kind == ast::NodeKind::MemberExpr) {
            const auto& mem = static_cast<const ast::MemberExpr&>(*c.callee);
            if (mem.base != nullptr && mem.base->kind == ast::NodeKind::IdentExpr
                && static_cast<const ast::IdentExpr&>(*mem.base).name == "Duration"
                && scopes_.current().lookup("Duration") == nullptr) {
                factory = mem.member;
                is_duration_factory = true;
            }
        } else if (c.callee->kind == ast::NodeKind::LeadingDotExpr && expected != nullptr
                   && expected->kind() == TypeKind::Duration) {
            factory = static_cast<const ast::LeadingDotExpr&>(*c.callee).name;
            is_duration_factory = true;
        }
        if (is_duration_factory) {
            const bool known = factory == "seconds" || factory == "milliseconds"
                               || factory == "microseconds" || factory == "nanoseconds";
            if (!known) {
                for (const auto& a : c.args) {
                    (void)check_expr(*a.value);
                }
                error_at(c.range,
                         std::format("unknown Duration factory '.{}' (expected seconds, "
                                     "milliseconds, microseconds, or nanoseconds)",
                                     factory));
            } else if (c.args.size() != 1 || !c.args[0].label.empty()) {
                for (const auto& a : c.args) {
                    (void)check_expr(*a.value);
                }
                error_at(c.range,
                         std::format("Duration.{}(n) takes exactly one unlabeled integer argument",
                                     factory));
            } else {
                auto at = check_expr(*c.args[0].value);
                if (at != nullptr && !at->is_error() && !at->is_integer()) {
                    error_at(c.args[0].value->range,
                             std::format("Duration.{} expects an integer, got {}",
                                         factory,
                                         at->describe()));
                }
            }
            return types_->primitive(TypeKind::Duration);
        }
    }

    // §9 `result.mapError(f)` — explicit error-type widening between
    // throws functions. `result` must be Result<T, E>; `f` must be
    // a callable `(E) -> E'`. The call types as Result<T, E'>; the
    // codegen lowers it to std::expected's `.transform_error(f)`.
    // Recognized as a MemberExpr callee shape (parallel to Box.new).
    if (c.callee->kind == ast::NodeKind::MemberExpr) {
        const auto& mem = static_cast<const ast::MemberExpr&>(*c.callee);
        if (mem.member == "mapError") {
            auto base_t = check_expr(*mem.base);
            if (base_t != nullptr && base_t->kind() == TypeKind::Result) {
                if (c.args.size() != 1) {
                    for (const auto& a : c.args) {
                        (void)check_expr(*a.value);
                    }
                    error_at(c.range,
                             ".mapError takes exactly one argument (the mapping function)");
                    return types_->make_result(base_t->inner(), base_t->result());
                }
                if (!c.args[0].label.empty()) {
                    error_at(c.args[0].value->range, ".mapError argument cannot have a label");
                }
                // §9 / §16 — when the argument is a closure literal,
                // it needs a contextual Function type so its single
                // param adopts the Result's error type. Push down
                // `(E) -> ?` (result left unconstrained; the body's
                // last expression types the new error). For named-
                // function arguments the expected type is harmless:
                // function symbols ignore the hint and their types
                // are checked against the (E) -> E' shape below.
                auto expected_fn = types_->make_function({base_t->result()}, nullptr);
                auto arg_t = check_expr(*c.args[0].value, expected_fn);
                if (arg_t == nullptr || arg_t->is_error()) {
                    return types_->error();
                }
                if (arg_t->kind() != TypeKind::Function || arg_t->parts().size() != 1) {
                    error_at(c.args[0].value->range,
                             std::format(".mapError argument must be a function (E) -> E', got {}",
                                         arg_t->describe()));
                    return types_->make_result(base_t->inner(), base_t->result());
                }
                if (!TypeArena::assignable(base_t->result(), arg_t->parts()[0])) {
                    error_at(c.args[0].value->range,
                             std::format(".mapError function expects {} but Result error is {}",
                                         arg_t->parts()[0]->describe(),
                                         base_t->result()->describe()));
                    return types_->make_result(base_t->inner(), base_t->result());
                }
                return types_->make_result(base_t->inner(), arg_t->result());
            }
        }
    }

    // §9 iterator combinators: `zip(a, b)` and `take(xs, n)` are
    // free-function builtins. The user can shadow them with their own
    // `zip` / `take` (the scope check below defers to a user symbol if
    // present); otherwise we recognize the call shape directly. Both
    // require their iterable argument(s) to expose a `next() -> T?`
    // method.
    if (c.callee->kind == ast::NodeKind::IdentExpr) {
        const auto& ci = static_cast<const ast::IdentExpr&>(*c.callee);
        if ((ci.name == "zip" || ci.name == "take" || ci.name == "map" || ci.name == "filter")
            && scopes_.current().lookup(ci.name) == nullptr) {
            auto iterator_element = [&](TypePtr iter_t) -> TypePtr {
                if (iter_t == nullptr || iter_t->is_error()) {
                    return nullptr;
                }
                auto nt = lookup_method(iter_t, "next");
                if (nt == nullptr || nt->kind() != TypeKind::Function || nt->result() == nullptr
                    || nt->result()->kind() != TypeKind::Optional) {
                    return nullptr;
                }
                return nt->result()->inner();
            };
            if (ci.name == "zip") {
                if (c.args.size() != 2) {
                    for (const auto& a : c.args) {
                        (void)check_expr(*a.value);
                    }
                    error_at(c.range, "zip(a, b) takes exactly two iterator arguments");
                    return types_->make_zip_iter(types_->error(), types_->error());
                }
                for (const auto& a : c.args) {
                    if (!a.label.empty()) {
                        error_at(a.value->range, "zip arguments cannot have labels");
                    }
                }
                auto ta = check_expr(*c.args[0].value);
                auto tb = check_expr(*c.args[1].value);
                auto ea = iterator_element(ta);
                auto eb = iterator_element(tb);
                if (ea == nullptr && ta != nullptr && !ta->is_error()) {
                    error_at(c.args[0].value->range,
                             std::format("zip's first argument must be an iterator (have a "
                                         "'next() -> Element?' method), got {}",
                                         ta->describe()));
                }
                if (eb == nullptr && tb != nullptr && !tb->is_error()) {
                    error_at(c.args[1].value->range,
                             std::format("zip's second argument must be an iterator (have a "
                                         "'next() -> Element?' method), got {}",
                                         tb->describe()));
                }
                return types_->make_zip_iter(ea != nullptr ? ea : types_->error(),
                                             eb != nullptr ? eb : types_->error());
            }
            if (ci.name == "take") {
                if (c.args.size() != 2) {
                    for (const auto& a : c.args) {
                        (void)check_expr(*a.value);
                    }
                    error_at(c.range, "take(xs, n) takes exactly two arguments");
                    return types_->make_take_iter(types_->error());
                }
                for (const auto& a : c.args) {
                    if (!a.label.empty()) {
                        error_at(a.value->range, "take arguments cannot have labels");
                    }
                }
                auto txs = check_expr(*c.args[0].value);
                auto tn = check_expr(*c.args[1].value);
                auto exs = iterator_element(txs);
                if (exs == nullptr && txs != nullptr && !txs->is_error()) {
                    error_at(c.args[0].value->range,
                             std::format("take's first argument must be an iterator (have a "
                                         "'next() -> Element?' method), got {}",
                                         txs->describe()));
                }
                if (tn != nullptr && !tn->is_error() && !tn->is_integer()) {
                    error_at(c.args[1].value->range,
                             std::format("take's second argument must be an integer count, got {}",
                                         tn->describe()));
                }
                return types_->make_take_iter(exs != nullptr ? exs : types_->error());
            }
            // §9 `map(xs, f)` / `filter(xs, p)` — both type-check the
            // closure against an expected function type derived from
            // xs's element type. For map, f is (T) -> U and the
            // result's element type is U; for filter, p is (T) -> Bool
            // and the element type is preserved. The closure must be
            // passed directly so the expected-type flow can shape its
            // params (sema rejects a free closure with no context).
            if (ci.name == "map" || ci.name == "filter") {
                if (c.args.size() != 2) {
                    for (const auto& a : c.args) {
                        (void)check_expr(*a.value);
                    }
                    error_at(c.range,
                             std::format("{}(xs, {}) takes exactly two arguments",
                                         ci.name,
                                         ci.name == "map" ? "f" : "p"));
                    if (ci.name == "map") {
                        return types_->make_map_iter(types_->error(), types_->error());
                    }
                    return types_->make_filter_iter(types_->error());
                }
                for (const auto& a : c.args) {
                    if (!a.label.empty()) {
                        error_at(a.value->range,
                                 std::format("{} arguments cannot have labels", ci.name));
                    }
                }
                auto txs = check_expr(*c.args[0].value);
                auto exs = iterator_element(txs);
                if (exs == nullptr && txs != nullptr && !txs->is_error()) {
                    error_at(c.args[0].value->range,
                             std::format("{}'s first argument must be an iterator (have a "
                                         "'next() -> Element?' method), got {}",
                                         ci.name,
                                         txs->describe()));
                    exs = types_->error();
                }
                if (exs == nullptr) {
                    exs = types_->error();
                }
                if (ci.name == "filter") {
                    // Predicate: shape (T) -> Bool. Flow the expected
                    // type so a closure literal types its param.
                    auto pred_expected = types_->make_function({exs}, types_->boolean());
                    auto tp = check_expr(*c.args[1].value, pred_expected);
                    if (tp == nullptr || tp->is_error()) {
                        return types_->make_filter_iter(exs);
                    }
                    if (tp->kind() != TypeKind::Function || tp->parts().size() != 1
                        || tp->result() == nullptr || tp->result()->kind() != TypeKind::Bool) {
                        error_at(c.args[1].value->range,
                                 std::format("filter's predicate must be (T) -> Bool, got {}",
                                             tp->describe()));
                        return types_->make_filter_iter(exs);
                    }
                    if (!TypeArena::assignable(exs, tp->parts()[0])) {
                        error_at(c.args[1].value->range,
                                 std::format("filter predicate expects {} but iterator yields {}",
                                             tp->parts()[0]->describe(),
                                             exs->describe()));
                    }
                    return types_->make_filter_iter(exs);
                }
                // map: shape (T) -> U. Flow only the param type into
                // the closure context; the return type is whatever
                // the closure body produces.
                auto fn_expected = types_->make_function({exs}, /*result=*/nullptr);
                auto tf = check_expr(*c.args[1].value, fn_expected);
                if (tf == nullptr || tf->is_error()) {
                    return types_->make_map_iter(exs, types_->error());
                }
                if (tf->kind() != TypeKind::Function || tf->parts().size() != 1) {
                    error_at(c.args[1].value->range,
                             std::format("map's transform must be a function (T) -> U, got {}",
                                         tf->describe()));
                    return types_->make_map_iter(exs, types_->error());
                }
                if (!TypeArena::assignable(exs, tf->parts()[0])) {
                    error_at(c.args[1].value->range,
                             std::format("map transform expects {} but iterator yields {}",
                                         tf->parts()[0]->describe(),
                                         exs->describe()));
                }
                return types_->make_map_iter(
                    exs, tf->result() != nullptr ? tf->result() : types_->error());
            }
        }
        // §11.2 `parallel(data, chunks, body)` — runs the non-escaping
        // closure `body` over disjoint sub-spans of `data`. v0.5 takes a
        // MutSpan[T] plus an explicit chunk count and splits internally
        // (the user-facing `chunks()` / `split(at:)` partition primitives
        // are a separate slice); the worker body is `(MutSpan[T]) -> Unit`.
        if (ci.name == "parallel" && scopes_.current().lookup("parallel") == nullptr) {
            if (c.args.size() != 3) {
                for (const auto& a : c.args) {
                    (void)check_expr(*a.value);
                }
                error_at(c.range, "parallel(data, chunks, body) takes exactly three arguments");
                return types_->unit();
            }
            for (const auto& a : c.args) {
                if (!a.label.empty()) {
                    error_at(a.value->range, "parallel arguments cannot have labels");
                }
            }
            auto td = check_expr(*c.args[0].value);
            if (td != nullptr && !td->is_error() && td->kind() != TypeKind::MutSpan) {
                error_at(c.args[0].value->range,
                         std::format("parallel's first argument must be a MutSpan[T], got {}",
                                     td->describe()));
            }
            auto tn = check_expr(*c.args[1].value);
            if (tn != nullptr && !tn->is_error() && !tn->is_integer()) {
                error_at(c.args[1].value->range,
                         std::format("parallel's chunk count must be an integer, got {}",
                                     tn->describe()));
            }
            TypePtr slice_t =
                (td != nullptr && td->kind() == TypeKind::MutSpan) ? td : types_->error();
            auto body_expected = types_->make_function({slice_t}, types_->unit());
            auto tb = check_expr(*c.args[2].value, body_expected);
            if (tb != nullptr && !tb->is_error()
                && (tb->kind() != TypeKind::Function || tb->parts().size() != 1)) {
                error_at(
                    c.args[2].value->range,
                    std::format("parallel's body must be a closure (MutSpan[T]) -> Unit, got {}",
                                tb->describe()));
            }
            return types_->unit();
        }
    }

    // §A7 (§14.14) `Scheduler.swapContext(saving:, loading:)` —
    // the context-switch primitive. The spec signature uses
    // `MutPtr[Context]` / `Ptr[Context]` arguments derived from
    // `&decl` (§14.6.3). v0.5 doesn't yet have the `&decl`
    // address-of operator, so we accept `Context` values directly
    // — the codegen wraps each argument in `&` when lowering. When
    // `&decl` lands, this intercept tightens to the pointer form.
    if (c.callee->kind == ast::NodeKind::MemberExpr) {
        const auto& mem = static_cast<const ast::MemberExpr&>(*c.callee);
        if (mem.base != nullptr && mem.base->kind == ast::NodeKind::IdentExpr) {
            const auto& bi = static_cast<const ast::IdentExpr&>(*mem.base);
            if (bi.name == "Scheduler" && mem.member == "swapContext"
                && scopes_.current().lookup("Scheduler") == nullptr) {
                if (c.args.size() != 2) {
                    for (const auto& a : c.args) {
                        (void)check_expr(*a.value);
                    }
                    error_at(c.range,
                             "Scheduler.swapContext takes exactly two arguments "
                             "(saving: Context, loading: Context)");
                    return types_->unit();
                }
                TypePtr context_t =
                    builtin_context_decl_ != nullptr
                        ? types_->make_nominal(TypeKind::Struct, builtin_context_decl_.get())
                        : types_->error();
                auto saving_t = check_expr(*c.args[0].value, context_t);
                auto loading_t = check_expr(*c.args[1].value, context_t);
                if (saving_t != nullptr && !saving_t->is_error()
                    && !TypeArena::equal(saving_t, context_t)) {
                    error_at(c.args[0].value->range,
                             std::format("Scheduler.swapContext saving argument must be "
                                         "Context, got {}",
                                         saving_t->describe()));
                }
                if (loading_t != nullptr && !loading_t->is_error()
                    && !TypeArena::equal(loading_t, context_t)) {
                    error_at(c.args[1].value->range,
                             std::format("Scheduler.swapContext loading argument must be "
                                         "Context, got {}",
                                         loading_t->describe()));
                }
                return types_->unit();
            }
        }
    }

    // §A3 (§10.5) raw-mint primitives. Four static-call shapes:
    //   * Ptr.unchecked(fromAddress: addr)    -> Ptr[T]
    //   * MutPtr.unchecked(fromAddress: addr) -> MutPtr[T]
    //   * Span.raw(at: ptr, count: n)         -> Span[T]
    //   * MutSpan.raw(at: ptr, count: n)      -> MutSpan[T]
    //
    // T for the .unchecked forms comes from the expected type at the
    // call site (the let-binding's annotation, the function's
    // return type, etc.). T for the .raw forms comes from the
    // pointer argument itself. All four require `RawMemory` in
    // scope; the capability checker enforces that separately.
    if (c.callee->kind == ast::NodeKind::MemberExpr) {
        const auto& mem = static_cast<const ast::MemberExpr&>(*c.callee);
        if (mem.base != nullptr && mem.base->kind == ast::NodeKind::IdentExpr) {
            const auto& bi = static_cast<const ast::IdentExpr&>(*mem.base);
            const bool is_ptr = bi.name == "Ptr";
            const bool is_mut_ptr = bi.name == "MutPtr";
            const bool is_span = bi.name == "Span";
            const bool is_mut_span = bi.name == "MutSpan";
            if ((is_ptr || is_mut_ptr) && mem.member == "unchecked"
                && scopes_.current().lookup(bi.name) == nullptr) {
                if (c.args.size() != 1) {
                    for (const auto& a : c.args) {
                        (void)check_expr(*a.value);
                    }
                    error_at(c.range,
                             std::format("{}.unchecked takes exactly one argument "
                                         "(fromAddress: UInt64)",
                                         bi.name));
                    return is_ptr ? types_->make_ptr(types_->error())
                                  : types_->make_mut_ptr(types_->error());
                }
                if (!c.args[0].label.empty() && c.args[0].label != "fromAddress") {
                    error_at(c.args[0].value->range,
                             std::format("{}.unchecked expects label 'fromAddress'", bi.name));
                }
                auto addr_t = check_expr(*c.args[0].value, types_->primitive(TypeKind::UInt64));
                if (addr_t != nullptr && !addr_t->is_error() && !addr_t->is_integer()) {
                    error_at(c.args[0].value->range,
                             std::format("{}.unchecked address must be an integer, got {}",
                                         bi.name,
                                         addr_t->describe()));
                }
                // T comes from the expected type. Without it, we
                // can't infer — surface that as a diagnostic so the
                // user adds an annotation.
                TypePtr T = nullptr;
                if (expected != nullptr) {
                    if (is_ptr && expected->kind() == TypeKind::Ptr) {
                        T = expected->inner();
                    } else if (is_mut_ptr && expected->kind() == TypeKind::MutPtr) {
                        T = expected->inner();
                    }
                }
                if (T == nullptr) {
                    error_at(c.range,
                             std::format("{}.unchecked needs an expected {}[T] type at the call "
                                         "site (annotate the let-binding or return slot)",
                                         bi.name,
                                         bi.name));
                    T = types_->error();
                }
                return is_ptr ? types_->make_ptr(T) : types_->make_mut_ptr(T);
            }
            // §A6 (§14.11) `MmioWireView.at(ptr, endianness)` —
            // parallel to the bare-view constructor but takes the
            // device's endianness as a second arg. Capability check
            // is the same `Mmio` gate.
            if (bi.name == "MmioWireView" && mem.member == "at"
                && scopes_.current().lookup(bi.name) == nullptr) {
                if (c.args.size() != 2) {
                    for (const auto& a : c.args) {
                        (void)check_expr(*a.value);
                    }
                    error_at(c.range,
                             "MmioWireView.at takes exactly two arguments "
                             "(_ ptr: MutPtr[T], endianness: Endianness)");
                    return types_->make_mmio_wire_view(types_->error());
                }
                auto ptr_t = check_expr(*c.args[0].value);
                TypePtr endian_t =
                    builtin_endianness_decl_ != nullptr
                        ? types_->make_nominal(TypeKind::Enum, builtin_endianness_decl_.get())
                        : types_->error();
                auto endian_arg_t = check_expr(*c.args[1].value, endian_t);
                if (ptr_t != nullptr && !ptr_t->is_error() && ptr_t->kind() != TypeKind::MutPtr) {
                    error_at(c.args[0].value->range,
                             std::format("MmioWireView.at first argument must be MutPtr[T], got {}",
                                         ptr_t->describe()));
                }
                TypePtr T = (ptr_t != nullptr && ptr_t->kind() == TypeKind::MutPtr)
                                ? ptr_t->inner()
                                : types_->error();
                if (endian_arg_t != nullptr && !endian_arg_t->is_error()
                    && !TypeArena::equal(endian_arg_t, endian_t)) {
                    error_at(c.args[1].value->range,
                             std::format("MmioWireView.at endianness must be Endianness, got {}",
                                         endian_arg_t->describe()));
                }
                return types_->make_mmio_wire_view(T);
            }
            // §A6 (§14.11) MmioView.at(_ ptr: MutPtr[T]) → MmioView[T]
            // and MmioRegion.at(_ ptr: MutPtr[T], count: Int) →
            // MmioRegion[T]. Both require the `Mmio` capability,
            // gated by the capability checker. T comes from the
            // MutPtr arg, not from the call-site expected type —
            // unlike .unchecked, this constructor is total over T.
            if ((bi.name == "MmioView" || bi.name == "MmioRegion") && mem.member == "at"
                && scopes_.current().lookup(bi.name) == nullptr) {
                const bool is_view = bi.name == "MmioView";
                const std::size_t expected_arity = is_view ? 1 : 2;
                if (c.args.size() != expected_arity) {
                    for (const auto& a : c.args) {
                        (void)check_expr(*a.value);
                    }
                    error_at(c.range,
                             std::format("{}.at takes exactly {} argument{}",
                                         bi.name,
                                         expected_arity,
                                         expected_arity == 1 ? "" : "s"));
                    return is_view ? types_->make_mmio_view(types_->error())
                                   : types_->make_mmio_region(types_->error());
                }
                auto ptr_t = check_expr(*c.args[0].value);
                if (ptr_t != nullptr && !ptr_t->is_error() && ptr_t->kind() != TypeKind::MutPtr) {
                    error_at(c.args[0].value->range,
                             std::format("{}.at first argument must be MutPtr[T], got {}",
                                         bi.name,
                                         ptr_t->describe()));
                }
                TypePtr T = (ptr_t != nullptr && ptr_t->kind() == TypeKind::MutPtr)
                                ? ptr_t->inner()
                                : types_->error();
                if (!is_view) {
                    auto ct = check_expr(*c.args[1].value, types_->primitive(TypeKind::Int));
                    if (ct != nullptr && !ct->is_error() && !ct->is_integer()) {
                        error_at(c.args[1].value->range,
                                 std::format("MmioRegion.at count must be an integer, got {}",
                                             ct->describe()));
                    }
                }
                return is_view ? types_->make_mmio_view(T) : types_->make_mmio_region(T);
            }
            if ((is_span || is_mut_span) && mem.member == "raw"
                && scopes_.current().lookup(bi.name) == nullptr) {
                if (c.args.size() != 2) {
                    for (const auto& a : c.args) {
                        (void)check_expr(*a.value);
                    }
                    error_at(c.range,
                             std::format("{}.raw takes exactly two arguments (at: ..., count: ...)",
                                         bi.name));
                    return is_span ? types_->make_span(types_->error())
                                   : types_->make_mut_span(types_->error());
                }
                auto pt = check_expr(*c.args[0].value);
                auto ct = check_expr(*c.args[1].value, types_->primitive(TypeKind::Int));
                TypePtr T = types_->error();
                if (is_span) {
                    if (pt != nullptr && pt->kind() == TypeKind::Ptr) {
                        T = pt->inner();
                    } else if (pt != nullptr && !pt->is_error()) {
                        error_at(
                            c.args[0].value->range,
                            std::format("Span.raw 'at' must be a Ptr[T], got {}", pt->describe()));
                    }
                } else {
                    if (pt != nullptr && pt->kind() == TypeKind::MutPtr) {
                        T = pt->inner();
                    } else if (pt != nullptr && !pt->is_error()) {
                        error_at(c.args[0].value->range,
                                 std::format("MutSpan.raw 'at' must be a MutPtr[T], got {}",
                                             pt->describe()));
                    }
                }
                if (ct != nullptr && !ct->is_error() && !ct->is_integer()) {
                    error_at(c.args[1].value->range,
                             std::format("{}.raw count must be an integer, got {}",
                                         bi.name,
                                         ct->describe()));
                }
                return is_span ? types_->make_span(T) : types_->make_mut_span(T);
            }
        }
    }

    // §13 — `s.column(.field)` on a Soa[T] yields a `MutSpan[FieldType]` over
    // that field's column: the SIMD-friendly per-field view (all the field's
    // values are contiguous in the SoA layout). The argument is a leading-dot
    // field selector resolved against T's fields, not a runtime value, so the
    // result type can be the field's type statically.
    if (c.callee->kind == ast::NodeKind::MemberExpr) {
        const auto& mem = static_cast<const ast::MemberExpr&>(*c.callee);
        if (mem.member == "column" && mem.base != nullptr) {
            auto base_t = check_expr(*mem.base);
            if (base_t != nullptr && base_t->kind() == TypeKind::Soa && base_t->inner() != nullptr
                && base_t->inner()->nominal_decl() != nullptr
                && base_t->inner()->nominal_decl()->kind == ast::NodeKind::Struct) {
                const auto& sd =
                    static_cast<const ast::StructDecl&>(*base_t->inner()->nominal_decl());
                if (c.args.size() != 1 || !c.args[0].label.empty() || c.args[0].value == nullptr
                    || c.args[0].value->kind != ast::NodeKind::LeadingDotExpr) {
                    error_at(c.range,
                             "column(.field) takes one leading-dot field selector "
                             "(e.g. `s.column(.x)`)");
                    return types_->make_mut_span(types_->error());
                }
                const auto& sel = static_cast<const ast::LeadingDotExpr&>(*c.args[0].value);
                for (const auto& f : sd.fields) {
                    if (f.kind != ast::StructDecl::Field::Kind::Embed && f.name == sel.name) {
                        return types_->make_mut_span(f.type != nullptr ? resolve_type(*f.type)
                                                                       : types_->error());
                    }
                }
                error_at(c.args[0].value->range,
                         std::format("Soa element '{}' has no field '{}'", sd.name, sel.name));
                return types_->make_mut_span(types_->error());
            }
        }
    }

    // §5 clause 4 / §18.4 — `s.split(at: i)` on a Span[T] / MutSpan[T] is a
    // trusted partitioner: it yields a provably-disjoint partition, the
    // tuple `(prefix, suffix)` of two non-overlapping sub-views of the same
    // span type ([0, i) and [i, count)). The disjoint provenance lets both
    // halves be borrowed at once; binding them with `let (lo, hi) = …`
    // gives distinct symbols the exclusivity checker already treats as
    // non-overlapping. (chunks(n), the iterator partitioner, and the
    // cross-statement borrow-liveness check on the parent wait on
    // follow-on slices.)
    if (c.callee->kind == ast::NodeKind::MemberExpr) {
        const auto& mem = static_cast<const ast::MemberExpr&>(*c.callee);
        if (mem.member == "split" && mem.base != nullptr) {
            auto base_t = check_expr(*mem.base);
            if (base_t != nullptr
                && (base_t->kind() == TypeKind::Span || base_t->kind() == TypeKind::MutSpan)) {
                if (c.args.size() != 1) {
                    for (const auto& a : c.args) {
                        (void)check_expr(*a.value);
                    }
                    error_at(c.range, "split(at: i) takes exactly one argument");
                    return types_->make_tuple({base_t, base_t});
                }
                if (!c.args[0].label.empty() && c.args[0].label != "at") {
                    error_at(c.args[0].value->range, "split expects the label 'at'");
                }
                auto idx = check_expr(*c.args[0].value, types_->primitive(TypeKind::Int));
                if (idx != nullptr && !idx->is_error() && !idx->is_integer()) {
                    error_at(
                        c.args[0].value->range,
                        std::format("split index must be an integer, got {}", idx->describe()));
                }
                return types_->make_tuple({base_t, base_t});
            }
        }
        // §5/§18.4 — `s.chunks(of: n)` on a Span[T] / MutSpan[T] is the
        // size-based sibling of split(at:): it yields the consecutive,
        // disjoint sub-views of length n (the last shorter when the length
        // isn't a multiple of n). The result is a sequence of sub-views of
        // the receiver's span kind — modelled as a Vector[Span[T]] /
        // Vector[MutSpan[T]] so `for chunk in s.chunks(of: n)` binds
        // chunk: Span[T]; codegen lowers the call directly to a
        // std::vector<std::span<T>> so the runtime count (unknown at
        // compile time) is honoured. Using the result as a first-class
        // value (indexing it, `.count`) is a follow-on; the for-loop form
        // is what v0.5 supports.
        if (mem.member == "chunks" && mem.base != nullptr) {
            auto base_t = check_expr(*mem.base);
            if (base_t != nullptr
                && (base_t->kind() == TypeKind::Span || base_t->kind() == TypeKind::MutSpan)) {
                const auto elem = base_t->inner();
                const auto sub = base_t->kind() == TypeKind::MutSpan ? types_->make_mut_span(elem)
                                                                     : types_->make_span(elem);
                if (c.args.size() != 1) {
                    for (const auto& a : c.args) {
                        (void)check_expr(*a.value);
                    }
                    error_at(c.range, "chunks(of: n) takes exactly one argument");
                    return types_->make_chunk_iter(sub);
                }
                if (!c.args[0].label.empty() && c.args[0].label != "of") {
                    error_at(c.args[0].value->range, "chunks expects the label 'of'");
                }
                auto sz = check_expr(*c.args[0].value, types_->primitive(TypeKind::Int));
                if (sz != nullptr && !sz->is_error() && !sz->is_integer()) {
                    error_at(c.args[0].value->range,
                             std::format("chunks size must be an integer, got {}", sz->describe()));
                }
                return types_->make_chunk_iter(sub);
            }
        }
    }

    // §14.8 / §12.6 `cfg.option("name")` — comptime-folded build
    // option accessor. v0.5 admits one name: "cache_line_bytes"
    // (returns 64 — the default that §A11's Padded[T] template
    // hardcodes today). Other names error with the admitted set
    // listed so the user sees v0.5's scope. The folded value lands
    // via Resolution::set_folded_value so codegen emits the integer
    // literal directly without re-folding.
    if (c.callee->kind == ast::NodeKind::MemberExpr) {
        const auto& mem = static_cast<const ast::MemberExpr&>(*c.callee);
        if (mem.base->kind == ast::NodeKind::IdentExpr
            && static_cast<const ast::IdentExpr&>(*mem.base).name == "cfg" && mem.member == "option"
            && scopes_.current().lookup("cfg") == nullptr) {
            if (c.args.size() != 1) {
                for (const auto& a : c.args) {
                    (void)check_expr(*a.value);
                }
                error_at(c.range, "cfg.option(name) takes exactly one string-literal argument");
                return types_->primitive(TypeKind::Int);
            }
            if (!c.args[0].label.empty()) {
                error_at(c.args[0].value->range, "cfg.option argument cannot have a label");
            }
            if (c.args[0].value == nullptr || c.args[0].value->kind != ast::NodeKind::StringLit) {
                (void)check_expr(*c.args[0].value);
                error_at(c.range, "cfg.option argument must be a literal string");
                return types_->primitive(TypeKind::Int);
            }
            const auto& arg = static_cast<const ast::StringLit&>(*c.args[0].value);
            std::int64_t value = 0;
            if (arg.text == "cache_line_bytes") {
                value = 64;
            } else {
                error_at(arg.range,
                         std::format("cfg.option({:?}) — unknown option; v0.5 admits "
                                     "\"cache_line_bytes\"",
                                     std::string{arg.text}));
                return types_->primitive(TypeKind::Int);
            }
            ComptimeValue folded;
            folded.kind = ComptimeValue::Kind::Int;
            folded.i = value;
            folded.type = TypeKind::Int;
            resolution_.set_folded_value(&c, std::move(folded));
            return types_->primitive(TypeKind::Int);
        }
    }

    // §10 builtin `Box.new(value)` — the unique-ownership heap pointer
    // constructor. The callee is a `MemberExpr(IdentExpr("Box"), "new")`;
    // sema doesn't have a symbol for the Box "value" so we intercept
    // the call shape directly. The result type is Box<typeof(value)>.
    if (c.callee->kind == ast::NodeKind::MemberExpr) {
        const auto& mem = static_cast<const ast::MemberExpr&>(*c.callee);
        if (mem.base->kind == ast::NodeKind::IdentExpr
            && static_cast<const ast::IdentExpr&>(*mem.base).name == "Box" && mem.member == "new"
            && scopes_.current().lookup("Box") == nullptr) {
            if (c.args.size() != 1) {
                for (const auto& a : c.args) {
                    (void)check_expr(*a.value);
                }
                error_at(c.range, "Box.new(value) takes exactly one argument");
                return types_->make_box(types_->error());
            }
            if (!c.args[0].label.empty()) {
                error_at(c.args[0].value->range, "Box.new argument cannot have a label");
            }
            auto inner = check_expr(*c.args[0].value);
            return types_->make_box(inner != nullptr ? inner : types_->error());
        }
        // §11 `Channel.new()` — mints a typed queue. The element type comes
        // from the expected `Channel[T]` (there is no value to infer from),
        // so it must be used in a typed context. An optional capacity
        // argument (Int) is accepted but ignored in v0.5 (the queue is
        // unbounded; bounded back-pressure waits on a scheduler).
        if (mem.base->kind == ast::NodeKind::IdentExpr
            && static_cast<const ast::IdentExpr&>(*mem.base).name == "Channel"
            && mem.member == "new" && scopes_.current().lookup("Channel") == nullptr) {
            if (c.args.size() > 1) {
                for (const auto& a : c.args) {
                    (void)check_expr(*a.value);
                }
                error_at(c.range, "Channel.new() takes at most one argument (an Int capacity)");
            } else if (c.args.size() == 1) {
                auto cap = check_expr(*c.args[0].value);
                if (cap != nullptr && !cap->is_error() && !cap->is_integer()) {
                    error_at(c.args[0].value->range,
                             std::format("Channel.new capacity must be an integer, got {}",
                                         cap->describe()));
                }
            }
            TypePtr inner = (expected != nullptr && expected->kind() == TypeKind::Channel)
                                ? expected->inner()
                                : nullptr;
            if (inner == nullptr) {
                error_at(c.range,
                         "cannot infer Channel element type; annotate the binding (e.g. "
                         "`let ch: Channel[Int32] = Channel.new()`)");
                inner = types_->error();
            }
            return types_->make_channel(inner);
        }
        // §18.5 `Vec.new()` — mints an empty growable sequence. Like Channel,
        // the element type comes from the expected `Vec[T]`. Allocation is
        // gated by `Alloc` (checked in capability.cpp).
        if (mem.base->kind == ast::NodeKind::IdentExpr
            && static_cast<const ast::IdentExpr&>(*mem.base).name == "Vec" && mem.member == "new"
            && scopes_.current().lookup("Vec") == nullptr) {
            if (!c.args.empty()) {
                for (const auto& a : c.args) {
                    (void)check_expr(*a.value);
                }
                error_at(c.range, "Vec.new() takes no arguments");
            }
            TypePtr inner = (expected != nullptr && expected->kind() == TypeKind::Vec)
                                ? expected->inner()
                                : nullptr;
            if (inner == nullptr) {
                error_at(c.range,
                         "cannot infer Vec element type; annotate the binding (e.g. "
                         "`var xs: Vec[Int32] = Vec.new()`)");
                inner = types_->error();
            }
            return types_->make_vec(inner);
        }
        // §18.5 `String.new()` — mints an empty owned, growable string.
        // Alloc-gated (capability.cpp), like Vec.new.
        if (mem.base->kind == ast::NodeKind::IdentExpr
            && static_cast<const ast::IdentExpr&>(*mem.base).name == "String" && mem.member == "new"
            && scopes_.current().lookup("String") == nullptr) {
            if (!c.args.empty()) {
                for (const auto& a : c.args) {
                    (void)check_expr(*a.value);
                }
                error_at(c.range, "String.new() takes no arguments");
            }
            return types_->primitive(TypeKind::String);
        }
        // §18.5 `HashMap.new()` — mints an empty map; K/V come from the expected
        // `HashMap[K, V]`. Alloc-gated, like Vec.new.
        if (mem.base->kind == ast::NodeKind::IdentExpr
            && static_cast<const ast::IdentExpr&>(*mem.base).name == "HashMap"
            && mem.member == "new" && scopes_.current().lookup("HashMap") == nullptr) {
            if (!c.args.empty()) {
                for (const auto& a : c.args) {
                    (void)check_expr(*a.value);
                }
                error_at(c.range, "HashMap.new() takes no arguments");
            }
            if (expected != nullptr && expected->kind() == TypeKind::HashMap
                && expected->parts().size() == 2) {
                return types_->make_hashmap(expected->parts()[0], expected->parts()[1]);
            }
            error_at(c.range,
                     "cannot infer HashMap key/value types; annotate the binding (e.g. "
                     "`var m: HashMap[Str, Int32] = HashMap.new()`)");
            return types_->make_hashmap(types_->error(), types_->error());
        }
        // §13 `Soa.new()` — mints an empty struct-of-arrays; the element struct
        // comes from the expected `Soa[T]`. Alloc-gated, like Vec.new.
        if (mem.base->kind == ast::NodeKind::IdentExpr
            && static_cast<const ast::IdentExpr&>(*mem.base).name == "Soa" && mem.member == "new"
            && scopes_.current().lookup("Soa") == nullptr) {
            if (!c.args.empty()) {
                for (const auto& a : c.args) {
                    (void)check_expr(*a.value);
                }
                error_at(c.range, "Soa.new() takes no arguments");
            }
            if (expected != nullptr && expected->kind() == TypeKind::Soa) {
                return types_->make_soa(expected->inner());
            }
            error_at(c.range,
                     "cannot infer Soa element type; annotate the binding (e.g. "
                     "`var s: Soa[Point] = Soa.new()`)");
            return types_->make_soa(types_->error());
        }
    }

    // §A11 (§14.8) `PerCpu.new(value)` — the heap factory for per-hart
    // storage. Returns `Box[PerCpu[T]]`; the C++ side allocates a 64-
    // byte-aligned `__vstr::PerCpu<T>` and wraps it in a unique_ptr.
    // When the surrounding context names `Box[PerCpu[T]]`, T is pushed
    // down to the value check so an integer literal adopts the
    // expected width; otherwise the value's type pins T. Capability
    // gate is Alloc, parallel to Box.new.
    if (c.callee->kind == ast::NodeKind::MemberExpr) {
        const auto& mem = static_cast<const ast::MemberExpr&>(*c.callee);
        if (mem.base->kind == ast::NodeKind::IdentExpr
            && static_cast<const ast::IdentExpr&>(*mem.base).name == "PerCpu" && mem.member == "new"
            && scopes_.current().lookup("PerCpu") == nullptr) {
            if (c.args.size() != 1) {
                for (const auto& a : c.args) {
                    (void)check_expr(*a.value);
                }
                error_at(c.range, "PerCpu.new(value) takes exactly one argument");
                return types_->make_box(types_->make_per_cpu(types_->error()));
            }
            if (!c.args[0].label.empty()) {
                error_at(c.args[0].value->range, "PerCpu.new argument cannot have a label");
            }
            TypePtr value_hint = nullptr;
            if (expected != nullptr && expected->kind() == TypeKind::Box
                && expected->inner() != nullptr && expected->inner()->kind() == TypeKind::PerCpu) {
                value_hint = expected->inner()->inner();
            }
            auto inner = check_expr(*c.args[0].value, value_hint);
            return types_->make_box(
                types_->make_per_cpu(inner != nullptr ? inner : types_->error()));
        }
    }

    // §3 opaque-type construction: `Q(t)` where Q is a newtype over T —
    // one positional arg, type-checked against T, result is Q. The
    // codegen lowers this to `static_cast<Q>(t)` over the `enum class
    // Q : T {}` emission.
    if (c.callee->kind == ast::NodeKind::IdentExpr) {
        const auto& callee_ident = static_cast<const ast::IdentExpr&>(*c.callee);
        if (const auto* sym = scopes_.current().lookup(callee_ident.name);
            sym != nullptr && sym->kind == SymbolKind::OpaqueType && sym->decl != nullptr) {
            resolution_.set_symbol(c.callee.get(), sym);
            check_visibility(*sym, c.callee->range);
            const auto& od = static_cast<const ast::OpaqueDecl&>(*sym->decl);
            auto underlying = od.underlying ? resolve_type(*od.underlying) : types_->error();
            if (c.args.size() != 1) {
                for (const auto& a : c.args) {
                    (void)check_expr(*a.value);
                }
                error_at(c.range,
                         std::format("opaque-type constructor {}(...) takes exactly one argument",
                                     od.name));
                return sym->type;
            }
            if (!c.args[0].label.empty()) {
                error_at(c.args[0].value->range,
                         "opaque-type constructor argument cannot have a label");
            }
            auto arg_type = check_expr(*c.args[0].value, underlying);
            if (!TypeArena::assignable(arg_type, underlying)) {
                error_at(c.args[0].value->range,
                         std::format("opaque-type {}({}) requires a {} argument, got {}",
                                     od.name,
                                     underlying ? underlying->describe() : "?",
                                     underlying ? underlying->describe() : "?",
                                     arg_type ? arg_type->describe() : "?"));
            }
            return sym->type;
        }
    }

    // §7 explicit type-args at a construction site: `Pair[Int32](lo: 1, hi: 2)`
    // parses as a CallExpr whose callee is the IndexExpr `Pair[Int32]` (the
    // bracket reads as a subscript in expression position). When the base names
    // a generic struct/enum *type*, reinterpret the bracket as explicit type
    // arguments: resolve them, build the nominal instance, and funnel into the
    // ordinary construction path below with that instance as the expected type
    // — which seeds the generic bindings exactly as an annotated binding would.
    TypePtr callee_type = nullptr;
    if (c.callee->kind == ast::NodeKind::IndexExpr) {
        const auto& ix = static_cast<const ast::IndexExpr&>(*c.callee);
        if (ix.base != nullptr && ix.base->kind == ast::NodeKind::IdentExpr) {
            const auto& base_id = static_cast<const ast::IdentExpr&>(*ix.base);
            const auto* sym = scopes_.current().lookup(base_id.name);
            if (sym != nullptr && sym->type != nullptr && sym->decl != nullptr
                && (sym->type->kind() == TypeKind::Struct || sym->type->kind() == TypeKind::Enum)) {
                // The base is a nominal type name, so this is a construction with
                // explicit type arguments, not a subscript. Resolve each bracket
                // entry as a type (a bare type name today; nested generics fall
                // back to inference). Convert IdentExpr indices to types.
                std::vector<TypePtr> targs;
                bool all_types = !ix.indices.empty();
                for (const auto& idx : ix.indices) {
                    TypePtr t = type_from_index_expr(idx.get());
                    if (t == nullptr) {
                        all_types = false;
                        break;
                    }
                    targs.push_back(t);
                }
                const auto& generics =
                    (sym->decl->kind == ast::NodeKind::Struct)
                        ? static_cast<const ast::StructDecl&>(*sym->decl).generics
                        : static_cast<const ast::EnumDecl&>(*sym->decl).generics;
                const std::size_t want = named_type_param_count(generics);
                if (all_types && want > 0 && targs.size() == want) {
                    expected = sym->type->kind() == TypeKind::Struct
                                   ? types_->make_struct_instance(sym->type->nominal_decl(),
                                                                  std::move(targs))
                                   : types_->make_enum_instance(sym->type->nominal_decl(),
                                                                std::move(targs));
                    callee_type = sym->type;
                } else if (want == 0) {
                    error_at(ix.range,
                             std::format("type '{}' is not generic; it takes no type arguments",
                                         base_id.name));
                    callee_type = sym->type;
                } else if (all_types) {
                    error_at(ix.range,
                             std::format("'{}' expects {} type argument(s), got {}",
                                         base_id.name,
                                         want,
                                         ix.indices.size()));
                    callee_type = sym->type;
                }
            }
        }
    }

    // Resolve the callee first so we know each parameter's expected type
    // before we type its corresponding argument — this is what lets integer
    // literals adopt the parameter's type without an explicit conversion.
    //
    // We pass `expected` through to the callee so a leading-dot enum case in
    // call position (`.circle(radius: 1.0)`) can resolve against the expected
    // enum type.
    if (callee_type == nullptr) {
        callee_type = check_expr(*c.callee, expected);
    }
    if (callee_type == nullptr || callee_type->is_error()) {
        // Still type the arguments so their internal errors surface.
        for (const auto& a : c.args) {
            (void)check_expr(*a.value);
        }
        return types_->error();
    }

    // §A4 (§14.9.3) free-standing compareExchangeWeak rejection. The
    // weak form is admitted only inside one of the spec's retry-loop
    // shapes (tracked via weak_cas_loop_depth_). When we see a weak
    // CAS call site outside that range, emit a diagnostic pointing
    // the user at the strong form. Doing the check here — after
    // callee resolution — keeps the test simple: we know the
    // synthesized method type and the call's range.
    if (c.callee->kind == ast::NodeKind::MemberExpr && weak_cas_loop_depth_ == 0) {
        const auto& mem = static_cast<const ast::MemberExpr&>(*c.callee);
        if (mem.member == "compareExchangeWeak") {
            auto base_t = resolution_.type_of(mem.base.get());
            if (base_t != nullptr && base_t->kind() == TypeKind::Atomic) {
                error_at(c.range,
                         "compareExchangeWeak is admitted only inside a recognized retry "
                         "loop (`while !r.succeeded { … }` or `while true { … if r.succeeded "
                         "{ break } }`). Use compareExchange for a one-shot CAS.");
            }
        }
    }

    // Struct construction: `Point(x: 10, y: 32)` parses as a CallExpr whose
    // callee evaluates to the nominal Struct type. Dispatch by-label against
    // the struct's fields rather than treating it as a function call.
    if (callee_type->kind() == TypeKind::Struct && callee_type->nominal_decl() != nullptr) {
        const auto& s_decl = static_cast<const ast::StructDecl&>(*callee_type->nominal_decl());
        // §A6 (§6.8 / §14.11.4) `@repr(union)`: members overlay one
        // storage cell, so exactly one labeled argument picks the
        // active member. Fall through to label/type checks, but the
        // "missing field" sweep at the end is skipped for unions —
        // the unseen members are by design, not an error.
        const bool is_union = struct_is_repr_union(s_decl);
        if (is_union && c.args.size() != 1) {
            error_at(c.range,
                     std::format("@repr(union) struct '{}' constructor requires exactly one "
                                 "labeled argument naming the active member (got {})",
                                 s_decl.name,
                                 c.args.size()));
        }
        // §7 generics phase 2 — a generic struct infers its type
        // parameters from the labeled arguments, exactly like a generic
        // function call. The field types are resolved with the params as
        // opaque placeholders (`resolve_member_type_with_generics(.., {})`);
        // unifying each against its argument's type builds the binding map.
        // The expected type, when it's an instance of this same struct,
        // seeds the map so an annotated binding pins the width.
        const bool is_generic = named_type_param_count(s_decl.generics) > 0;
        std::unordered_map<std::string, TypePtr> gbindings;
        if (is_generic && expected != nullptr && expected->kind() == TypeKind::Struct
            && expected->nominal_decl() == &s_decl) {
            const auto& ep = expected->parts();
            std::size_t gi = 0;
            for (const auto& gp : s_decl.generics) {
                if (gp.name.empty()) {
                    continue;
                }
                if (gi < ep.size() && ep[gi] != nullptr) {
                    gbindings.emplace(gp.name, ep[gi]);
                }
                ++gi;
            }
        }
        // Build a label → (field, generic field type) index once.
        std::unordered_map<std::string, std::pair<const ast::StructDecl::Field*, TypePtr>> by_label;
        for (const auto& f : s_decl.fields) {
            if (f.kind == ast::StructDecl::Field::Kind::Embed) {
                continue;  // embeds aren't constructible by label (yet)
            }
            by_label.emplace(f.name,
                             std::make_pair(&f,
                                            f.type ? resolve_member_type_with_generics(
                                                         s_decl.generics, *f.type, {})
                                                   : types_->error()));
        }
        std::vector<bool> seen(s_decl.fields.size(), false);
        for (std::size_t i = 0; i < c.args.size(); ++i) {
            const auto& arg = c.args[i];
            if (arg.label.empty()) {
                error_at(
                    arg.value->range,
                    std::format("struct constructor argument {} requires a field label", i + 1));
                (void)check_expr(*arg.value);
                continue;
            }
            auto it = by_label.find(arg.label);
            if (it == by_label.end()) {
                error_at(arg.value->range,
                         std::format("'{}' has no field '{}'", s_decl.name, arg.label));
                (void)check_expr(*arg.value);
                continue;
            }
            auto generic_ft = it->second.second;
            auto field_type = types_->substitute(generic_ft, gbindings);
            auto arg_type = check_expr(*arg.value, field_type);
            if (is_generic) {
                unify_generic(generic_ft, arg_type, gbindings, arg.value->range);
                field_type = types_->substitute(generic_ft, gbindings);
            }
            if (!TypeArena::assignable(arg_type, field_type)) {
                error_at(arg.value->range,
                         std::format("field '{}' of type {} cannot accept value of type {}",
                                     arg.label,
                                     field_type ? field_type->describe() : "?",
                                     arg_type ? arg_type->describe() : "?"));
            }
            note_string_coercion(*arg.value, arg_type, field_type);
            // Track coverage for "missing field" diagnostics.
            for (std::size_t k = 0; k < s_decl.fields.size(); ++k) {
                if (&s_decl.fields[k] == it->second.first) {
                    seen[k] = true;
                    break;
                }
            }
        }
        // Missing fields are an error — Vestra has no implicit defaulting.
        // Exception: @repr(union) overlay structs activate exactly one
        // member per construction, so unmentioned fields are not
        // missing in any meaningful sense.
        if (!is_union) {
            for (std::size_t k = 0; k < s_decl.fields.size(); ++k) {
                if (s_decl.fields[k].kind == ast::StructDecl::Field::Kind::Embed) {
                    continue;
                }
                if (!seen[k]) {
                    error_at(c.range,
                             std::format("struct '{}' constructor is missing field '{}'",
                                         s_decl.name,
                                         s_decl.fields[k].name));
                }
            }
        }
        if (!is_generic) {
            return callee_type;
        }
        // Assemble the inferred instance type. An unbound parameter (no
        // argument mentioned it and no expected type seeded it) is a
        // diagnostic — the user should annotate the binding's type.
        std::vector<TypePtr> inst_args;
        for (const auto& gp : s_decl.generics) {
            if (gp.name.empty()) {
                continue;
            }
            auto bit = gbindings.find(gp.name);
            if (bit == gbindings.end()) {
                error_at(c.range,
                         std::format("cannot infer {} argument '{}' for generic struct '{}'; "
                                     "annotate the binding's type (e.g. `let x: {}[...] = ...`)",
                                     gp.is_const ? "const" : "type",
                                     gp.name,
                                     s_decl.name,
                                     s_decl.name));
                inst_args.push_back(types_->error());
            } else {
                inst_args.push_back(bit->second);
            }
        }
        return types_->make_struct_instance(callee_type->nominal_decl(), std::move(inst_args));
    }

    if (callee_type->kind() != TypeKind::Function) {
        for (const auto& a : c.args) {
            (void)check_expr(*a.value);
        }
        error_at(c.callee->range,
                 std::format("called value is not a function (type {})", callee_type->describe()));
        return types_->error();
    }

    // Generic-aware call dispatch (§7): if the callee is a generic Vestra
    // function, build a substitution map by unifying its declared parameter
    // types against the argument types, then apply the substitution to each
    // param (for arg checking) and to the return type (for the call's
    // expression type).
    //
    // The non-generic path stays a strict structural match — the generic
    // path is a strict superset, so we go through it for every call where
    // we have a known callee FuncDecl. Calls into opaque function values
    // fall through to the non-generic check below.
    const ast::FuncDecl* fn = nullptr;
    if (const auto* sym = resolution_.symbol_of(c.callee.get())) {
        if (sym->decl != nullptr && sym->decl->kind == ast::NodeKind::Func) {
            fn = static_cast<const ast::FuncDecl*>(sym->decl);
        }
    }

    const auto& params = callee_type->parts();
    if (params.size() != c.args.size()) {
        for (const auto& a : c.args) {
            (void)check_expr(*a.value);
        }
        error_at(c.range,
                 std::format("call expects {} argument(s), got {}", params.size(), c.args.size()));
        return callee_type->result() != nullptr ? callee_type->result() : types_->error();
    }

    // Seed bindings from the surrounding context: if the call's result type
    // is a generic placeholder (or contains one) and we know the type the
    // caller expects, bind from that. Lets `let x: Int32 = identity(7)` work
    // even though the literal `7` alone would default to Int.
    std::unordered_map<std::string, TypePtr> bindings;
    if (expected != nullptr && callee_type->result() != nullptr) {
        unify_generic(callee_type->result(), expected, bindings, c.range);
    }

    // Pass 1: collect bindings from concrete arg types. We don't enforce
    // type matches yet — that happens after substitution. As we walk left
    // to right, we substitute each parameter's expected type with the
    // bindings discovered so far, so an integer literal on the right side
    // of `max_of(int32_value, 35)` adopts Int32 (already inferred from the
    // first argument) instead of defaulting to Int and then conflicting.
    for (std::size_t i = 0; i < params.size(); ++i) {
        auto pty = types_->substitute(params[i], bindings);
        auto arg_type = check_expr(*c.args[i].value, pty);
        unify_generic(params[i], arg_type, bindings, c.args[i].value->range);
    }

    // Pass 2: enforce each (substituted_param, arg) pair. We re-check the
    // arg with the substituted expected type so bidirectional literal
    // adaptation can pick up the generic's inferred concrete type.
    for (std::size_t i = 0; i < params.size(); ++i) {
        auto pty = types_->substitute(params[i], bindings);
        auto arg_type = check_expr(*c.args[i].value, pty);
        if (!TypeArena::assignable(arg_type, pty) && !read_borrows_as(arg_type, pty)) {
            error_at(c.args[i].value->range,
                     std::format("argument {} of type {} does not match parameter type {}",
                                 i + 1,
                                 arg_type ? arg_type->describe() : "?",
                                 pty ? pty->describe() : "?"));
        }
        note_string_coercion(*c.args[i].value, arg_type, pty);
    }

    // Verify every generic parameter ended up bound. An unbound generic at a
    // call site means inference couldn't pin it down — usually because the
    // generic only appears in the return position with no contextual type.
    if (fn != nullptr) {
        for (const auto& gp : fn->generics) {
            if (gp.name.empty()) {
                continue;
            }
            // A const generic is inferred the same way (unify_generic binds
            // a `[N]T` parameter's length from the argument), so an unbound
            // one is reported here too.
            if (!bindings.contains(gp.name)) {
                error_at(c.range,
                         std::format("cannot infer {} '{}' for call to '{}'",
                                     gp.is_const ? "const generic" : "generic parameter",
                                     gp.name,
                                     fn->name));
            }
        }
        // §7 generics — every `where T: P` / `[T: P]` bound on the callee
        // must be satisfied by the inferred argument type.
        check_generic_bounds(fn->generics, bindings, c.range);
    }

    auto result = callee_type->result() != nullptr ? callee_type->result() : types_->unit();
    return types_->substitute(result, bindings);
}

}  // namespace vestra::sema
