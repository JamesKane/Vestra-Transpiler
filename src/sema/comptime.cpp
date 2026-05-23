// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "vestra/sema/comptime.hpp"

#include "vestra/ast/nodes.hpp"
#include "vestra/sema/builtins.hpp"
#include "vestra/sema/types.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>

namespace vestra::sema {

namespace {

// Strip the underscore separators Vestra allows in number literals
// (1_000_000 / 0xFFFF_0000 / 0b1010_0011) before handing the digits to
// std::from_chars, which doesn't understand them.
std::string strip_underscores(std::string_view s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c != '_') {
            out.push_back(c);
        }
    }
    return out;
}

// Parse an integer literal token (matches §17.1: decimal, 0x hex, 0o oct,
// 0b binary, with `_` separators). On overflow or malformed input return
// std::nullopt; the caller propagates that as "not foldable."
std::optional<std::int64_t> parse_int_literal(std::string_view raw) {
    auto s = strip_underscores(raw);
    if (s.empty()) {
        return std::nullopt;
    }
    int base = 10;
    std::string_view digits = s;
    if (s.size() >= 2 && s[0] == '0') {
        switch (s[1]) {
        case 'x':
        case 'X':
            base = 16;
            digits = std::string_view{s}.substr(2);
            break;
        case 'o':
        case 'O':
            base = 8;
            digits = std::string_view{s}.substr(2);
            break;
        case 'b':
        case 'B':
            base = 2;
            digits = std::string_view{s}.substr(2);
            break;
        default:
            break;
        }
    }
    if (digits.empty()) {
        return std::nullopt;
    }
    std::int64_t v = 0;
    auto res = std::from_chars(digits.data(), digits.data() + digits.size(), v, base);
    if (res.ec != std::errc{} || res.ptr != digits.data() + digits.size()) {
        return std::nullopt;
    }
    return v;
}

std::optional<double> parse_float_literal(std::string_view raw) {
    auto s = strip_underscores(raw);
    if (s.empty()) {
        return std::nullopt;
    }
    double v = 0.0;
    auto res = std::from_chars(s.data(), s.data() + s.size(), v);
    if (res.ec != std::errc{} || res.ptr != s.data() + s.size()) {
        return std::nullopt;
    }
    return v;
}

bool is_integer_kind(TypeKind k) noexcept {
    switch (k) {
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
        return true;
    default:
        return false;
    }
}

bool is_unsigned_kind(TypeKind k) noexcept {
    switch (k) {
    case TypeKind::UInt8:
    case TypeKind::UInt16:
    case TypeKind::UInt32:
    case TypeKind::UInt64:
    case TypeKind::UInt:
        return true;
    default:
        return false;
    }
}

bool is_float_kind(TypeKind k) noexcept {
    return k == TypeKind::Float32 || k == TypeKind::Float64;
}

}  // namespace

// ============================================================================
// ComptimeValue::to_cpp_literal
// ============================================================================

std::string ComptimeValue::to_cpp_literal() const {
    // Phase 1 keeps the literal form intentionally bare. We rely on the
    // surrounding context (`inline constexpr T x = ...;` for consts, the
    // function's return type for return statements) to pin the C++ type;
    // the suffix only marks the *literal kind* enough to keep narrowing
    // / sign-conversion warnings quiet under -Wconversion.
    switch (kind) {
    case Kind::Int:
        return std::to_string(i);
    case Kind::UInt:
        return std::to_string(u) + "u";
    case Kind::Float:
        // std::format keeps enough precision for a round-trip. The `f`
        // suffix pins a Float32 result to float instead of double.
        return type == TypeKind::Float32 ? std::format("{}f", f) : std::format("{}", f);
    case Kind::Bool:
        return b ? "true" : "false";
    case Kind::String:
        // Strings only flow into runtime C++ when a non-cfg comptime call
        // ever returns one — which today it can't. For §12.6 the only use
        // is folding @when predicates, which never reach codegen.
        return "std::string_view(\"" + s + "\")";
    case Kind::Vector: {
        // The brace-pair `{{...}}` slots into the surrounding const's
        // `std::array<T, N> = ...` shape (std::array is an aggregate
        // wrapping a C array, so it takes a doubly-braced initializer).
        std::string out;
        out += "{{";
        for (std::size_t k = 0; k < elements.size(); ++k) {
            if (k != 0) {
                out += ", ";
            }
            out += elements[k].to_cpp_literal();
        }
        out += "}}";
        return out;
    }
    case Kind::Unit:
        return "/* unit */ 0";
    }
    return "/* ?comptime */ 0";
}

// ============================================================================
// ComptimeFolder::fold
// ============================================================================

namespace {

ComptimeValue make_int(std::int64_t v, TypeKind hint) {
    ComptimeValue cv;
    if (is_unsigned_kind(hint)) {
        cv.kind = ComptimeValue::Kind::UInt;
        cv.u = static_cast<std::uint64_t>(v);
        cv.type = hint;
    } else {
        cv.kind = ComptimeValue::Kind::Int;
        cv.i = v;
        cv.type = is_integer_kind(hint) ? hint : TypeKind::Int;
    }
    return cv;
}

ComptimeValue make_uint(std::uint64_t v, TypeKind hint) {
    ComptimeValue cv;
    cv.kind = ComptimeValue::Kind::UInt;
    cv.u = v;
    cv.type = is_unsigned_kind(hint) ? hint : TypeKind::UInt;
    return cv;
}

ComptimeValue make_float(double v, TypeKind hint) {
    ComptimeValue cv;
    cv.kind = ComptimeValue::Kind::Float;
    cv.f = v;
    cv.type = is_float_kind(hint) ? hint : TypeKind::Float64;
    return cv;
}

ComptimeValue make_bool(bool v) {
    ComptimeValue cv;
    cv.kind = ComptimeValue::Kind::Bool;
    cv.b = v;
    cv.type = TypeKind::Bool;
    return cv;
}

ComptimeValue make_string(std::string v) {
    ComptimeValue cv;
    cv.kind = ComptimeValue::Kind::String;
    cv.s = std::move(v);
    cv.type = TypeKind::String;
    return cv;
}

// Default-constructed scalar value for the given element type. Used to
// fill `var t: [N]T = .zero`.
ComptimeValue zero_of(TypeKind element_kind) {
    if (is_integer_kind(element_kind)) {
        return is_unsigned_kind(element_kind) ? make_uint(0, element_kind)
                                              : make_int(0, element_kind);
    }
    if (is_float_kind(element_kind)) {
        return make_float(0.0, element_kind);
    }
    if (element_kind == TypeKind::Bool) {
        return make_bool(false);
    }
    // Unknown element kind → just zero-init the value's bytes; the codegen
    // path will emit `0` for the element, which is at worst a no-op for
    // most C++ targets.
    ComptimeValue cv;
    cv.kind = ComptimeValue::Kind::Int;
    cv.i = 0;
    cv.type = element_kind;
    return cv;
}

// Extract the element TypeKind from an `ast::VectorType` (e.g. `[8]Int32`
// → TypeKind::Int32). Returns TypeKind::Error if the shape isn't one of
// the primitives we recognize.
TypeKind vector_element_kind(const ast::VectorType& vt) {
    if (vt.element == nullptr || vt.element->kind != ast::NodeKind::NamedType) {
        return TypeKind::Error;
    }
    const auto& nt = static_cast<const ast::NamedType&>(*vt.element);
    if (nt.path.empty()) {
        return TypeKind::Error;
    }
    return TypeArena::primitive_kind_by_name(nt.path.front());
}

// §12.6: the `cfg` value's fields. Phase 1 hardcodes them based on
// build-time host detection; later phases will let the build system
// override via -D flags or a manifest.
std::optional<ComptimeValue> cfg_field(std::string_view name) {
#if defined(__APPLE__)
    constexpr std::string_view OsName = "macos";
#elif defined(__linux__)
    constexpr std::string_view OsName = "linux";
#elif defined(_WIN32)
    constexpr std::string_view OsName = "windows";
#else
    constexpr std::string_view OsName = "freestanding";
#endif
#if defined(__aarch64__) || defined(__arm64__)
    constexpr std::string_view ArchName = "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    constexpr std::string_view ArchName = "x86_64";
#elif defined(__riscv) && (__riscv_xlen == 64)
    constexpr std::string_view ArchName = "riscv64";
#elif defined(__riscv) && (__riscv_xlen == 32)
    constexpr std::string_view ArchName = "riscv32";
#elif defined(__wasm32__)
    constexpr std::string_view ArchName = "wasm32";
#else
    constexpr std::string_view ArchName = "unknown";
#endif

    if (name == "os") {
        return make_string(std::string{OsName});
    }
    if (name == "arch") {
        return make_string(std::string{ArchName});
    }
    if (name == "endian") {
        return make_string("little");
    }
    if (name == "pointerBits") {
        ComptimeValue cv;
        cv.kind = ComptimeValue::Kind::Int;
        cv.i = static_cast<std::int64_t>(sizeof(void*) * 8);
        cv.type = TypeKind::Int;
        return cv;
    }
    if (name == "profile") {
#ifdef NDEBUG
        return make_string("release");
#else
        return make_string("debug");
#endif
    }
    return std::nullopt;
}

// Promote `a` and `b` to a common numeric kind for arithmetic / comparison.
// Phase 1 only deals with same-kind operands; mixed-mode arithmetic isn't
// supported (Vestra doesn't allow implicit conversions anyway).
bool same_numeric_kind(const ComptimeValue& a, const ComptimeValue& b) {
    return a.kind == b.kind
           && (a.kind == ComptimeValue::Kind::Int || a.kind == ComptimeValue::Kind::UInt
               || a.kind == ComptimeValue::Kind::Float);
}

}  // namespace

std::optional<ComptimeValue>
ComptimeFolder::fold(const ast::Expr& e, const Env& env, TypeKind hint, int depth) const {
    // Top-level entry: build a fresh empty Frame (no locals, no early
    // return tripped yet) and delegate to fold_with.
    Frame frame;
    return fold_with(e, env, frame, hint, depth);
}

std::optional<ComptimeValue> ComptimeFolder::fold_with(
    const ast::Expr& e, const Env& env, Frame& frame, TypeKind hint, int depth) const {
    if (depth > MaxDepth) {
        return std::nullopt;  // bail rather than blow the host C++ stack
    }
    switch (e.kind) {
    case ast::NodeKind::IntLit: {
        const auto& lit = static_cast<const ast::IntLit&>(e);
        auto v = parse_int_literal(lit.text);
        if (!v) {
            return std::nullopt;
        }
        if (is_unsigned_kind(hint)) {
            return make_uint(static_cast<std::uint64_t>(*v), hint);
        }
        return make_int(*v, hint);
    }
    case ast::NodeKind::FloatLit: {
        const auto& lit = static_cast<const ast::FloatLit&>(e);
        auto v = parse_float_literal(lit.text);
        return v ? std::optional{make_float(*v, hint)} : std::nullopt;
    }
    case ast::NodeKind::BoolLit:
        return make_bool(static_cast<const ast::BoolLit&>(e).value);

    case ast::NodeKind::IdentExpr: {
        const auto& ident = static_cast<const ast::IdentExpr&>(e);
        // §12.6: `cfg` is a built-in compile-time value. Returning a
        // sentinel String lets MemberExpr below recognize it and route
        // through cfg_field.
        if (ident.name == "cfg") {
            return make_string("__vestra_cfg__");
        }
        // Phase 3: locals shadow top-level consts. A loop's iteration
        // variable, a let binding inside a comptime func body, and a var
        // updated through assignment all live in frame.locals.
        if (auto it = frame.locals.find(ident.name); it != frame.locals.end()) {
            return it->second;
        }
        if (auto it = env.find(ident.name); it != env.end()) {
            return it->second;
        }
        // §12.1 stdlib: `tau` / `pi` / `e` resolve as built-in Float64
        // constants. The resolver registered them as Const symbols so
        // type-checking already succeeded; we just supply the value.
        if (auto entry = builtins::lookup_const(ident.name)) {
            return builtins::value_of(*entry);
        }
        return std::nullopt;
    }

    case ast::NodeKind::MemberExpr: {
        // Only cfg member access is foldable in phase 1. Walking arbitrary
        // struct field reads waits until we model struct *values* at fold
        // time (we have struct *types* but no values yet).
        const auto& m = static_cast<const ast::MemberExpr&>(e);
        if (m.base->kind == ast::NodeKind::IdentExpr
            && static_cast<const ast::IdentExpr&>(*m.base).name == "cfg") {
            return cfg_field(m.member);
        }
        return std::nullopt;
    }

    case ast::NodeKind::LeadingDotExpr: {
        // `.arm64` in a `cfg.arch == .arm64` predicate. The folder doesn't
        // know which enum the case belongs to, so we encode the case name
        // as a String and rely on the sibling cfg.X side of the comparison
        // also being a String for the equality to be meaningful.
        return make_string(static_cast<const ast::LeadingDotExpr&>(e).name);
    }

    case ast::NodeKind::ParenExpr:
        return fold_with(*static_cast<const ast::ParenExpr&>(e).inner, env, frame, hint, depth);

    case ast::NodeKind::ComptimeExpr:
        return fold_with(*static_cast<const ast::ComptimeExpr&>(e).inner, env, frame, hint, depth);

    case ast::NodeKind::BlockExpr: {
        // Phase 3: walk every statement in order through fold_stmt, which
        // mutates `frame` (binding let/var, updating assignments, iterating
        // loops). Stop early on `return` (frame.returned will be set). The
        // block's value is: frame.returned if set, else the trailing
        // ExprStmt's evaluated value, else nullopt (a block of pure stmts
        // has no value to use as an expression).
        const auto& b = static_cast<const ast::BlockExpr&>(e);
        for (std::size_t i = 0; i < b.stmts.size(); ++i) {
            const auto& s = *b.stmts[i];
            const bool last = (i + 1 == b.stmts.size());
            if (last && s.kind == ast::NodeKind::ExprStmt) {
                // Trailing expression: its value is the block's value.
                auto v =
                    fold_with(*static_cast<const ast::ExprStmt&>(s).expr, env, frame, hint, depth);
                if (frame.returned) {
                    return frame.returned;
                }
                return v;
            }
            // Every other stmt (including any ReturnStmt — even a trailing
            // one) routes through fold_stmt so frame.returned is set, which
            // is what makes an early return from a nested block / loop
            // propagate out to the enclosing function.
            if (!fold_stmt(s, env, frame, depth)) {
                return std::nullopt;
            }
            if (frame.returned) {
                return frame.returned;
            }
        }
        // Block ended without yielding a value.
        return std::nullopt;
    }

    case ast::NodeKind::CallExpr: {
        // §12.4 / §12.1 phase 2: a call to a `comptime func` evaluates at
        // fold time. We bind each argument value to the parameter's name
        // in a fresh frame, then fold the body. The callee gets a clean
        // locals slate — phase 3 doesn't expose the caller's bindings
        // (no closures).
        const auto& c = static_cast<const ast::CallExpr&>(e);
        if (c.callee->kind != ast::NodeKind::IdentExpr) {
            return std::nullopt;
        }
        const auto& ident = static_cast<const ast::IdentExpr&>(*c.callee);

        // Primitive-type-as-callable conversion (§17.x): `Float64(x)`,
        // `Int32(y)` etc. fold to a re-typed ComptimeValue with the
        // appropriate Int↔Float / width retag. The resolver already
        // rejected wrong arity or non-numeric args; we only need the
        // arithmetic.
        if (auto target_kind = TypeArena::primitive_kind_by_name(ident.name);
            target_kind != TypeKind::Error
            && (is_integer_kind(target_kind) || is_float_kind(target_kind)) && c.args.size() == 1) {
            auto av = fold_with(*c.args[0].value, env, frame, TypeKind::Unit, depth + 1);
            if (!av) {
                return std::nullopt;
            }
            if (is_float_kind(target_kind)) {
                double f = 0.0;
                switch (av->kind) {
                case ComptimeValue::Kind::Int:
                    f = static_cast<double>(av->i);
                    break;
                case ComptimeValue::Kind::UInt:
                    f = static_cast<double>(av->u);
                    break;
                case ComptimeValue::Kind::Float:
                    f = av->f;
                    break;
                default:
                    return std::nullopt;
                }
                return make_float(f, target_kind);
            }
            // Integer target: signed or unsigned.
            std::int64_t s = 0;
            std::uint64_t u = 0;
            switch (av->kind) {
            case ComptimeValue::Kind::Int:
                s = av->i;
                u = static_cast<std::uint64_t>(av->i);
                break;
            case ComptimeValue::Kind::UInt:
                s = static_cast<std::int64_t>(av->u);
                u = av->u;
                break;
            case ComptimeValue::Kind::Float:
                // C++ static_cast truncates toward zero; mirror that.
                s = static_cast<std::int64_t>(av->f);
                u = static_cast<std::uint64_t>(av->f);
                break;
            default:
                return std::nullopt;
            }
            return is_unsigned_kind(target_kind) ? make_uint(u, target_kind)
                                                 : make_int(s, target_kind);
        }

        // §12.1 stdlib dispatch: `sin(x)` / `cos(x)` / etc. resolve to
        // a host-side std::sin/std::cos with the folded Float64 arg.
        // We fold each arg with a Float64 hint so a literal like `2.0`
        // adopts Float64 rather than the default Float kind.
        if (auto entry = builtins::lookup_func(ident.name)) {
            if (c.args.size() != entry->arity) {
                return std::nullopt;
            }
            std::vector<ComptimeValue> argv;
            argv.reserve(c.args.size());
            for (const auto& a : c.args) {
                auto v = fold_with(*a.value, env, frame, TypeKind::Float64, depth + 1);
                if (!v) {
                    return std::nullopt;
                }
                argv.push_back(*std::move(v));
            }
            return builtins::call(*entry, argv);
        }

        if (global_scope_ == nullptr) {
            return std::nullopt;
        }
        const auto* sym = global_scope_->lookup(ident.name);
        if (sym == nullptr || sym->decl == nullptr || sym->decl->kind != ast::NodeKind::Func) {
            return std::nullopt;
        }
        const auto& fn = static_cast<const ast::FuncDecl&>(*sym->decl);
        if (!fn.is_comptime || fn.body == nullptr) {
            return std::nullopt;
        }
        if (c.args.size() != fn.params.size()) {
            return std::nullopt;
        }
        // Args are folded against the caller's frame (they're expressions
        // in the caller's scope). Results become the callee's parameter
        // bindings in a fresh frame.
        Frame call_frame;
        for (std::size_t i = 0; i < c.args.size(); ++i) {
            auto v = fold_with(*c.args[i].value, env, frame, TypeKind::Unit, depth + 1);
            if (!v) {
                return std::nullopt;
            }
            call_frame.locals[fn.params[i].name] = *v;
        }
        return fold_with(*fn.body, env, call_frame, hint, depth + 1);
    }

    case ast::NodeKind::VectorLitExpr: {
        // §12.1 phase 4: `[e0, e1, ...]` folds to a Vector value when each
        // element folds. The element TypeKind is taken from the first
        // element's resulting type (homogeneity is assumed; sema's job to
        // verify across-the-board match).
        const auto& v = static_cast<const ast::VectorLitExpr&>(e);
        ComptimeValue cv;
        cv.kind = ComptimeValue::Kind::Vector;
        cv.length = static_cast<std::int64_t>(v.elements.size());
        cv.elements.reserve(v.elements.size());
        for (const auto& el : v.elements) {
            // Each element folds with the surrounding `hint` if it's a
            // recognised numeric kind. This lets `[0, 0, 0]` adopt e.g.
            // Int32 when the context is a Vector<Int32>.
            auto val = fold_with(*el, env, frame, hint, depth);
            if (!val) {
                return std::nullopt;
            }
            cv.elements.push_back(*val);
        }
        cv.type = cv.elements.empty() ? TypeKind::Unit : cv.elements.front().type;
        return cv;
    }

    case ast::NodeKind::IndexExpr: {
        // `vec[i]` — phase 4 only supports a single Int index into a
        // Vector base. Anything more (multi-dim, non-vector indexable,
        // slicing) waits for later phases.
        const auto& ix = static_cast<const ast::IndexExpr&>(e);
        auto base = fold_with(*ix.base, env, frame, TypeKind::Unit, depth);
        if (!base || base->kind != ComptimeValue::Kind::Vector) {
            return std::nullopt;
        }
        if (ix.indices.size() != 1) {
            return std::nullopt;
        }
        auto idx = fold_with(*ix.indices.front(), env, frame, TypeKind::Int, depth);
        if (!idx || idx->kind != ComptimeValue::Kind::Int) {
            return std::nullopt;
        }
        if (idx->i < 0 || static_cast<std::size_t>(idx->i) >= base->elements.size()) {
            return std::nullopt;  // out-of-bounds at fold time bails cleanly
        }
        return base->elements[static_cast<std::size_t>(idx->i)];
    }

    case ast::NodeKind::UnaryExpr: {
        const auto& u = static_cast<const ast::UnaryExpr&>(e);
        auto op = fold_with(*u.operand, env, frame, hint, depth);
        if (!op) {
            return std::nullopt;
        }
        switch (u.op) {
        case ast::UnaryOp::Neg:
            if (op->kind == ComptimeValue::Kind::Int) {
                return make_int(-op->i, op->type);
            }
            if (op->kind == ComptimeValue::Kind::Float) {
                return make_float(-op->f, op->type);
            }
            return std::nullopt;
        case ast::UnaryOp::Not:
            if (op->kind == ComptimeValue::Kind::Bool) {
                return make_bool(!op->b);
            }
            return std::nullopt;
        case ast::UnaryOp::BitNot:
            if (op->kind == ComptimeValue::Kind::Int) {
                return make_int(~op->i, op->type);
            }
            if (op->kind == ComptimeValue::Kind::UInt) {
                return make_uint(~op->u, op->type);
            }
            return std::nullopt;
        }
        return std::nullopt;
    }

    case ast::NodeKind::BinaryExpr: {
        const auto& bin = static_cast<const ast::BinaryExpr&>(e);

        // Short-circuit logicals first so we don't evaluate the unreachable
        // side. Both sides must still fold to bool.
        if (bin.op == ast::BinaryOp::And || bin.op == ast::BinaryOp::Or) {
            auto l = fold_with(*bin.lhs, env, frame, TypeKind::Bool, depth);
            if (!l || l->kind != ComptimeValue::Kind::Bool) {
                return std::nullopt;
            }
            if (bin.op == ast::BinaryOp::And && !l->b) {
                return make_bool(false);
            }
            if (bin.op == ast::BinaryOp::Or && l->b) {
                return make_bool(true);
            }
            auto r = fold_with(*bin.rhs, env, frame, TypeKind::Bool, depth);
            if (!r || r->kind != ComptimeValue::Kind::Bool) {
                return std::nullopt;
            }
            return make_bool(r->b);
        }

        auto lhs = fold_with(*bin.lhs, env, frame, hint, depth);
        if (!lhs) {
            return std::nullopt;
        }
        // For the rhs, push the lhs's resolved Vestra type as the hint so
        // integer literals on the right adopt the same type (matches how
        // sema's bidirectional check works in check_binary).
        auto rhs = fold_with(*bin.rhs, env, frame, lhs->type, depth);
        if (!rhs) {
            return std::nullopt;
        }
        if (!same_numeric_kind(*lhs, *rhs) && bin.op != ast::BinaryOp::Eq
            && bin.op != ast::BinaryOp::Ne) {
            return std::nullopt;
        }

        auto fold_int = [&](auto op) -> std::optional<ComptimeValue> {
            return make_int(op(lhs->i, rhs->i), lhs->type);
        };
        auto fold_uint = [&](auto op) -> std::optional<ComptimeValue> {
            return make_uint(op(lhs->u, rhs->u), lhs->type);
        };
        auto fold_float = [&](auto op) -> std::optional<ComptimeValue> {
            return make_float(op(lhs->f, rhs->f), lhs->type);
        };
        auto fold_cmp = [&](auto op) -> std::optional<ComptimeValue> {
            if (lhs->kind == ComptimeValue::Kind::Int) {
                return make_bool(op(lhs->i, rhs->i));
            }
            if (lhs->kind == ComptimeValue::Kind::UInt) {
                return make_bool(op(lhs->u, rhs->u));
            }
            if (lhs->kind == ComptimeValue::Kind::Float) {
                return make_bool(op(lhs->f, rhs->f));
            }
            if (lhs->kind == ComptimeValue::Kind::Bool) {
                return make_bool(op(lhs->b, rhs->b));
            }
            if (lhs->kind == ComptimeValue::Kind::String) {
                // String comparison covers §12.6 `cfg.arch == .arm64` —
                // both sides fold to a String of the enum case name.
                return make_bool(op(lhs->s, rhs->s));
            }
            return std::nullopt;
        };

        switch (bin.op) {
        case ast::BinaryOp::Add:
            if (lhs->kind == ComptimeValue::Kind::Int) {
                return fold_int([](auto a, auto b) { return a + b; });
            }
            if (lhs->kind == ComptimeValue::Kind::UInt) {
                return fold_uint([](auto a, auto b) { return a + b; });
            }
            if (lhs->kind == ComptimeValue::Kind::Float) {
                return fold_float([](auto a, auto b) { return a + b; });
            }
            return std::nullopt;
        case ast::BinaryOp::Sub:
            if (lhs->kind == ComptimeValue::Kind::Int) {
                return fold_int([](auto a, auto b) { return a - b; });
            }
            if (lhs->kind == ComptimeValue::Kind::UInt) {
                return fold_uint([](auto a, auto b) { return a - b; });
            }
            if (lhs->kind == ComptimeValue::Kind::Float) {
                return fold_float([](auto a, auto b) { return a - b; });
            }
            return std::nullopt;
        case ast::BinaryOp::Mul:
            if (lhs->kind == ComptimeValue::Kind::Int) {
                return fold_int([](auto a, auto b) { return a * b; });
            }
            if (lhs->kind == ComptimeValue::Kind::UInt) {
                return fold_uint([](auto a, auto b) { return a * b; });
            }
            if (lhs->kind == ComptimeValue::Kind::Float) {
                return fold_float([](auto a, auto b) { return a * b; });
            }
            return std::nullopt;
        case ast::BinaryOp::Div:
            // §4 says div-by-zero traps at runtime. At fold time we just
            // refuse to produce a value and let the runtime path stand.
            if (lhs->kind == ComptimeValue::Kind::Int) {
                if (rhs->i == 0) {
                    return std::nullopt;
                }
                return fold_int([](auto a, auto b) { return a / b; });
            }
            if (lhs->kind == ComptimeValue::Kind::UInt) {
                if (rhs->u == 0) {
                    return std::nullopt;
                }
                return fold_uint([](auto a, auto b) { return a / b; });
            }
            if (lhs->kind == ComptimeValue::Kind::Float) {
                return fold_float([](auto a, auto b) { return a / b; });
            }
            return std::nullopt;
        case ast::BinaryOp::Mod:
            if (lhs->kind == ComptimeValue::Kind::Int) {
                if (rhs->i == 0) {
                    return std::nullopt;
                }
                return fold_int([](auto a, auto b) { return a % b; });
            }
            if (lhs->kind == ComptimeValue::Kind::UInt) {
                if (rhs->u == 0) {
                    return std::nullopt;
                }
                return fold_uint([](auto a, auto b) { return a % b; });
            }
            return std::nullopt;

        case ast::BinaryOp::Shl:
            if (lhs->kind == ComptimeValue::Kind::Int) {
                return fold_int([](auto a, auto b) { return a << b; });
            }
            if (lhs->kind == ComptimeValue::Kind::UInt) {
                return fold_uint([](auto a, auto b) { return a << b; });
            }
            return std::nullopt;
        case ast::BinaryOp::Shr:
            if (lhs->kind == ComptimeValue::Kind::Int) {
                return fold_int([](auto a, auto b) { return a >> b; });
            }
            if (lhs->kind == ComptimeValue::Kind::UInt) {
                return fold_uint([](auto a, auto b) { return a >> b; });
            }
            return std::nullopt;
        case ast::BinaryOp::BitAnd:
            if (lhs->kind == ComptimeValue::Kind::Int) {
                return fold_int([](auto a, auto b) { return a & b; });
            }
            if (lhs->kind == ComptimeValue::Kind::UInt) {
                return fold_uint([](auto a, auto b) { return a & b; });
            }
            return std::nullopt;
        case ast::BinaryOp::BitOr:
            if (lhs->kind == ComptimeValue::Kind::Int) {
                return fold_int([](auto a, auto b) { return a | b; });
            }
            if (lhs->kind == ComptimeValue::Kind::UInt) {
                return fold_uint([](auto a, auto b) { return a | b; });
            }
            return std::nullopt;
        case ast::BinaryOp::BitXor:
            if (lhs->kind == ComptimeValue::Kind::Int) {
                return fold_int([](auto a, auto b) { return a ^ b; });
            }
            if (lhs->kind == ComptimeValue::Kind::UInt) {
                return fold_uint([](auto a, auto b) { return a ^ b; });
            }
            return std::nullopt;

        case ast::BinaryOp::Eq:
            return fold_cmp([](auto a, auto b) { return a == b; });
        case ast::BinaryOp::Ne:
            return fold_cmp([](auto a, auto b) { return a != b; });
        case ast::BinaryOp::Lt:
            return fold_cmp([](auto a, auto b) { return a < b; });
        case ast::BinaryOp::Le:
            return fold_cmp([](auto a, auto b) { return a <= b; });
        case ast::BinaryOp::Gt:
            return fold_cmp([](auto a, auto b) { return a > b; });
        case ast::BinaryOp::Ge:
            return fold_cmp([](auto a, auto b) { return a >= b; });

        default:
            return std::nullopt;
        }
    }

    case ast::NodeKind::IfExpr: {
        const auto& i = static_cast<const ast::IfExpr&>(e);
        auto c = fold_with(*i.cond, env, frame, TypeKind::Bool, depth);
        if (!c || c->kind != ComptimeValue::Kind::Bool) {
            return std::nullopt;
        }
        // Fold only the chosen branch — matching §19.9 (if evaluates only
        // the selected branch). The unchosen branch isn't even examined.
        if (c->b) {
            return fold_with(*i.then_branch, env, frame, hint, depth);
        }
        if (i.else_branch) {
            return fold_with(*i.else_branch, env, frame, hint, depth);
        }
        // No `else` and the condition was false. As an expression this is
        // Unit. Returning Unit here (instead of nullopt) lets the stmt-
        // position form `if c { return … }` succeed as "no value, no
        // effects" when c is false — otherwise the enclosing block would
        // think the fold bailed and stop walking.
        return ComptimeValue{};
        // If with no else and a false condition is Unit; phase 1 can't fold
        // that into anything useful at the use site.
        return std::nullopt;
    }

    default:
        // Everything else — calls, struct lit, member access, vector lit,
        // match, await, etc. — is not foldable in phase 1.
        return std::nullopt;
    }
}

// ============================================================================
// Statement-level fold (§12.1 phase 3)
// ============================================================================

namespace {

// Extract a let/var statement's binding name + initializer + (optional)
// declared type. Phase 3 only handles ident-bound let/var (no destructuring).
// Returns true on a recognised shape; false otherwise.
bool extract_binding(const ast::Stmt& s,
                     std::string& out_name,
                     const ast::Expr*& out_value,
                     const ast::Type*& out_type) {
    out_type = nullptr;
    auto bind = [&](const ast::PatternPtr& pat, const ast::ExprPtr& val, const ast::TypePtr& typ) {
        out_value = val.get();
        out_type = typ.get();
        if (pat && pat->kind == ast::NodeKind::IdentPat) {
            out_name = static_cast<const ast::IdentPat&>(*pat).name;
            return true;
        }
        if (pat && pat->kind == ast::NodeKind::BindPat) {
            out_name = static_cast<const ast::BindPat&>(*pat).name;
            return true;
        }
        return false;
    };
    if (s.kind == ast::NodeKind::LetStmt) {
        const auto& l = static_cast<const ast::LetStmt&>(s);
        return bind(l.pattern, l.value, l.type);
    }
    if (s.kind == ast::NodeKind::VarStmt) {
        const auto& v = static_cast<const ast::VarStmt&>(s);
        return bind(v.pattern, v.value, v.type);
    }
    return false;
}

// Build a Vector value of the shape an `ast::VectorType` describes, with
// every element set to its kind-default (0 / false / etc). Returns nullopt
// when the shape isn't one we model (non-literal length, non-primitive
// element, ...).
std::optional<ComptimeValue> make_zero_vector(const ast::VectorType& vt) {
    if (vt.length <= 0) {
        return std::nullopt;  // const-generic lengths aren't foldable yet
    }
    auto elem_kind = vector_element_kind(vt);
    if (elem_kind == TypeKind::Error) {
        return std::nullopt;
    }
    ComptimeValue cv;
    cv.kind = ComptimeValue::Kind::Vector;
    cv.type = elem_kind;
    cv.length = vt.length;
    cv.elements.assign(static_cast<std::size_t>(vt.length), zero_of(elem_kind));
    return cv;
}

// Apply a compound assignment (`+=`, `-=`, etc.) to a current value. Returns
// nullopt if op isn't supported on the value's kind.
std::optional<ComptimeValue>
apply_compound(ast::AssignOp op, const ComptimeValue& cur, const ComptimeValue& rhs) {
    auto on_int = [&](auto f) -> std::optional<ComptimeValue> {
        if (cur.kind != ComptimeValue::Kind::Int || rhs.kind != ComptimeValue::Kind::Int) {
            return std::nullopt;
        }
        ComptimeValue out = cur;
        out.i = f(cur.i, rhs.i);
        return out;
    };
    auto on_float = [&](auto f) -> std::optional<ComptimeValue> {
        if (cur.kind != ComptimeValue::Kind::Float || rhs.kind != ComptimeValue::Kind::Float) {
            return std::nullopt;
        }
        ComptimeValue out = cur;
        out.f = f(cur.f, rhs.f);
        return out;
    };
    switch (op) {
    case ast::AssignOp::AddAssign:
        if (auto v = on_int([](auto a, auto b) { return a + b; })) {
            return v;
        }
        return on_float([](auto a, auto b) { return a + b; });
    case ast::AssignOp::SubAssign:
        if (auto v = on_int([](auto a, auto b) { return a - b; })) {
            return v;
        }
        return on_float([](auto a, auto b) { return a - b; });
    case ast::AssignOp::MulAssign:
        if (auto v = on_int([](auto a, auto b) { return a * b; })) {
            return v;
        }
        return on_float([](auto a, auto b) { return a * b; });
    case ast::AssignOp::DivAssign:
        if (cur.kind == ComptimeValue::Kind::Int) {
            if (rhs.i == 0) {
                return std::nullopt;
            }
            return on_int([](auto a, auto b) { return a / b; });
        }
        return on_float([](auto a, auto b) { return a / b; });
    case ast::AssignOp::ModAssign:
        if (cur.kind == ComptimeValue::Kind::Int && rhs.i != 0) {
            return on_int([](auto a, auto b) { return a % b; });
        }
        return std::nullopt;
    case ast::AssignOp::BitAndAssign:
        return on_int([](auto a, auto b) { return a & b; });
    case ast::AssignOp::BitOrAssign:
        return on_int([](auto a, auto b) { return a | b; });
    case ast::AssignOp::BitXorAssign:
        return on_int([](auto a, auto b) { return a ^ b; });
    case ast::AssignOp::ShlAssign:
        return on_int([](auto a, auto b) { return a << b; });
    case ast::AssignOp::ShrAssign:
        return on_int([](auto a, auto b) { return a >> b; });
    case ast::AssignOp::Assign:
        return rhs;
    }
    return std::nullopt;
}

}  // namespace

bool ComptimeFolder::fold_stmt(const ast::Stmt& s, const Env& env, Frame& frame, int depth) const {
    if (frame.returned) {
        return true;  // a prior statement already short-circuited
    }
    switch (s.kind) {
    case ast::NodeKind::LetStmt:
    case ast::NodeKind::VarStmt: {
        std::string name;
        const ast::Expr* init = nullptr;
        const ast::Type* decl_type = nullptr;
        if (!extract_binding(s, name, init, decl_type) || init == nullptr) {
            return false;
        }
        // §12.1 phase 4: `var t: [N]T = .zero` synthesizes a zero-filled
        // vector of the right shape. The declared type is the only place
        // we can recover the element kind + length from, so we special-
        // case this combination before falling through to the general
        // fold path.
        if (decl_type != nullptr && decl_type->kind == ast::NodeKind::VectorType
            && init->kind == ast::NodeKind::LeadingDotExpr
            && static_cast<const ast::LeadingDotExpr&>(*init).name == "zero") {
            auto v = make_zero_vector(static_cast<const ast::VectorType&>(*decl_type));
            if (!v) {
                return false;
            }
            frame.locals[name] = std::move(*v);
            return true;
        }
        // For vector literals declared with a vector type, push the element
        // type through as a fold hint so `[0, 0, 0]` adopts the right
        // primitive width.
        TypeKind hint = TypeKind::Unit;
        if (decl_type != nullptr && decl_type->kind == ast::NodeKind::VectorType) {
            hint = vector_element_kind(static_cast<const ast::VectorType&>(*decl_type));
        }
        auto v = fold_with(*init, env, frame, hint, depth);
        if (!v) {
            return false;
        }
        frame.locals[name] = *v;
        return true;
    }
    case ast::NodeKind::ExprStmt: {
        // Evaluate for any embedded loop/recursion side effects; discard
        // the value (this isn't the block's trailing expression — the
        // BlockExpr handler dispatches the trailing case directly).
        auto v = fold_with(
            *static_cast<const ast::ExprStmt&>(s).expr, env, frame, TypeKind::Unit, depth);
        return v.has_value();
    }
    case ast::NodeKind::ReturnStmt: {
        const auto& r = static_cast<const ast::ReturnStmt&>(s);
        if (!r.value) {
            frame.returned = ComptimeValue{};  // Unit return
            return true;
        }
        auto v = fold_with(*r.value, env, frame, TypeKind::Unit, depth);
        if (!v) {
            return false;
        }
        frame.returned = *v;
        return true;
    }
    case ast::NodeKind::AssignStmt: {
        const auto& a = static_cast<const ast::AssignStmt&>(s);
        // Phase 4: ident target (whole-binding update) and IndexExpr
        // target whose base is an ident-bound vector (slot update).
        // Anything else (member access, multi-index, base that's another
        // computation) isn't foldable yet.
        if (a.target->kind == ast::NodeKind::IdentExpr) {
            auto name = static_cast<const ast::IdentExpr&>(*a.target).name;
            // Hint the rhs with the current binding's element type when
            // possible so an int literal `42` slots into an Int32 var
            // without resetting its type.
            TypeKind hint = TypeKind::Unit;
            if (auto it = frame.locals.find(name); it != frame.locals.end()) {
                hint = it->second.type;
            }
            auto rhs = fold_with(*a.value, env, frame, hint, depth);
            if (!rhs) {
                return false;
            }
            if (a.op == ast::AssignOp::Assign) {
                frame.locals[name] = *rhs;
                return true;
            }
            auto it = frame.locals.find(name);
            if (it == frame.locals.end()) {
                return false;
            }
            auto combined = apply_compound(a.op, it->second, *rhs);
            if (!combined) {
                return false;
            }
            it->second = *combined;
            return true;
        }
        if (a.target->kind == ast::NodeKind::IndexExpr) {
            const auto& ix = static_cast<const ast::IndexExpr&>(*a.target);
            if (ix.base->kind != ast::NodeKind::IdentExpr || ix.indices.size() != 1) {
                return false;
            }
            auto base_name = static_cast<const ast::IdentExpr&>(*ix.base).name;
            auto vec_it = frame.locals.find(base_name);
            if (vec_it == frame.locals.end()
                || vec_it->second.kind != ComptimeValue::Kind::Vector) {
                return false;
            }
            auto idx = fold_with(*ix.indices.front(), env, frame, TypeKind::Int, depth);
            if (!idx || idx->kind != ComptimeValue::Kind::Int) {
                return false;
            }
            if (idx->i < 0 || static_cast<std::size_t>(idx->i) >= vec_it->second.elements.size()) {
                return false;  // out-of-bounds at fold time bails cleanly
            }
            // Hint the rhs with the vector's element type so an int
            // literal slots into the declared element width.
            auto rhs = fold_with(*a.value, env, frame, vec_it->second.type, depth);
            if (!rhs) {
                return false;
            }
            auto& slot = vec_it->second.elements[static_cast<std::size_t>(idx->i)];
            if (a.op == ast::AssignOp::Assign) {
                slot = *rhs;
                return true;
            }
            auto combined = apply_compound(a.op, slot, *rhs);
            if (!combined) {
                return false;
            }
            slot = *combined;
            return true;
        }
        return false;
    }
    case ast::NodeKind::ForStmt: {
        // Only `for ident in start ..< end` (RangeLt) and `for ident in
        // start .. end` (Range) are foldable. Anything else (iterator
        // protocol, vector iteration, etc.) waits for a later phase.
        const auto& f = static_cast<const ast::ForStmt&>(s);
        if (!f.iter || f.iter->kind != ast::NodeKind::BinaryExpr) {
            return false;
        }
        const auto& binop = static_cast<const ast::BinaryExpr&>(*f.iter);
        if (binop.op != ast::BinaryOp::Range && binop.op != ast::BinaryOp::RangeLt) {
            return false;
        }
        auto lo = fold_with(*binop.lhs, env, frame, TypeKind::Int, depth);
        auto hi = fold_with(*binop.rhs, env, frame, TypeKind::Int, depth);
        if (!lo || !hi || lo->kind != ComptimeValue::Kind::Int
            || hi->kind != ComptimeValue::Kind::Int) {
            return false;
        }
        std::int64_t start = lo->i;
        // `..` is inclusive; `..<` is exclusive.
        std::int64_t end = binop.op == ast::BinaryOp::Range ? hi->i + 1 : hi->i;

        // Determine the loop variable name. IdentPat / BindPat bind into
        // the body's scope; WildcardPat ("for _ in 0..<n") runs the body
        // without binding anything; anything else (destructuring) is not
        // foldable in phase 3.
        std::string var_name;  // empty when the pattern is a wildcard
        if (f.pattern && f.pattern->kind == ast::NodeKind::IdentPat) {
            var_name = static_cast<const ast::IdentPat&>(*f.pattern).name;
        } else if (f.pattern && f.pattern->kind == ast::NodeKind::BindPat) {
            var_name = static_cast<const ast::BindPat&>(*f.pattern).name;
        } else if (!f.pattern || f.pattern->kind != ast::NodeKind::WildcardPat) {
            return false;
        }

        // Save any prior binding of the loop variable so we restore after
        // the loop (the loop variable shouldn't leak out). For a wildcard
        // pattern there's no name to save or restore.
        bool had_prior = false;
        ComptimeValue prior{};
        if (!var_name.empty()) {
            if (auto it = frame.locals.find(var_name); it != frame.locals.end()) {
                had_prior = true;
                prior = it->second;
            }
        }

        std::int64_t iters = 0;
        for (std::int64_t i = start; i < end; ++i) {
            if (++iters > MaxLoopIterations) {
                return false;
            }
            if (!var_name.empty()) {
                ComptimeValue iv;
                iv.kind = ComptimeValue::Kind::Int;
                iv.i = i;
                iv.type = TypeKind::Int;
                frame.locals[var_name] = iv;
            }
            if (f.body) {
                auto v = fold_with(*f.body, env, frame, TypeKind::Unit, depth);
                (void)v;  // body's value is discarded; what we care about is
                          // assignments and frame.returned
                if (frame.returned) {
                    break;
                }
            }
        }

        if (!var_name.empty()) {
            if (had_prior) {
                frame.locals[var_name] = prior;
            } else {
                frame.locals.erase(var_name);
            }
        }
        return true;
    }
    case ast::NodeKind::WhileStmt: {
        const auto& w = static_cast<const ast::WhileStmt&>(s);
        std::int64_t iters = 0;
        while (true) {
            if (++iters > MaxLoopIterations) {
                return false;
            }
            auto c = fold_with(*w.cond, env, frame, TypeKind::Bool, depth);
            if (!c || c->kind != ComptimeValue::Kind::Bool) {
                return false;
            }
            if (!c->b) {
                break;
            }
            if (w.body) {
                auto v = fold_with(*w.body, env, frame, TypeKind::Unit, depth);
                (void)v;
                if (frame.returned) {
                    break;
                }
            }
        }
        return true;
    }
    case ast::NodeKind::BreakStmt:
    case ast::NodeKind::ContinueStmt:
        // Phase 3 doesn't model break/continue control flow inside loops;
        // a body using either bails the fold cleanly instead of misexecuting.
        return false;
    default:
        return false;
    }
}

}  // namespace vestra::sema
