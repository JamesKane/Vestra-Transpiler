// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "vestra/sema/comptime.hpp"

#include "vestra/ast/nodes.hpp"
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
        // Look up another const by name. If env doesn't have it, we can't
        // fold — the value might exist at runtime but isn't compile-time-known.
        auto it = env.find(ident.name);
        if (it == env.end()) {
            return std::nullopt;
        }
        return it->second;
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
        return fold(*static_cast<const ast::ParenExpr&>(e).inner, env, hint, depth);

    case ast::NodeKind::ComptimeExpr:
        return fold(*static_cast<const ast::ComptimeExpr&>(e).inner, env, hint, depth);

    case ast::NodeKind::BlockExpr: {
        // Phase 1: only fold a block whose contents are a single trailing
        // ExprStmt. Phase 2 also accepts a single trailing ReturnStmt with
        // a value so a function body like `{ return n * factorial(n-1) }`
        // collapses to its value. Anything more (let bindings, while loops,
        // multiple statements) still bails — those need real interpreter
        // state we don't model yet.
        const auto& b = static_cast<const ast::BlockExpr&>(e);
        if (b.stmts.size() != 1) {
            return std::nullopt;
        }
        const auto& s = *b.stmts[0];
        if (s.kind == ast::NodeKind::ExprStmt) {
            return fold(*static_cast<const ast::ExprStmt&>(s).expr, env, hint, depth);
        }
        if (s.kind == ast::NodeKind::ReturnStmt) {
            const auto& r = static_cast<const ast::ReturnStmt&>(s);
            if (!r.value) {
                return std::nullopt;
            }
            return fold(*r.value, env, hint, depth);
        }
        return std::nullopt;
    }

    case ast::NodeKind::CallExpr: {
        // §12.4 / §12.1 phase 2: a call to a `comptime func` evaluates at
        // fold time. We bind each argument value to the parameter's name
        // in a fresh environment, then fold the body.
        if (global_scope_ == nullptr) {
            return std::nullopt;
        }
        const auto& c = static_cast<const ast::CallExpr&>(e);
        // Phase 2 only resolves bare-identifier callees. Method calls and
        // higher-order forms wait for phase 3.
        if (c.callee->kind != ast::NodeKind::IdentExpr) {
            return std::nullopt;
        }
        const auto& ident = static_cast<const ast::IdentExpr&>(*c.callee);
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
        // Each argument folds against the caller's env. The result becomes
        // the parameter binding the body sees. Phase 2 doesn't propagate
        // the param's declared Vestra type as a hint — that would require
        // resolving the param's ast::Type from inside the folder, which
        // currently belongs to the resolver. Hint stays Unit; the literals
        // adopt the caller's bidirectional context only.
        Env call_env;
        for (std::size_t i = 0; i < c.args.size(); ++i) {
            auto v = fold(*c.args[i].value, env, TypeKind::Unit, depth + 1);
            if (!v) {
                return std::nullopt;
            }
            call_env[fn.params[i].name] = *v;
        }
        return fold(*fn.body, call_env, hint, depth + 1);
    }

    case ast::NodeKind::UnaryExpr: {
        const auto& u = static_cast<const ast::UnaryExpr&>(e);
        auto op = fold(*u.operand, env, hint, depth);
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
            auto l = fold(*bin.lhs, env, TypeKind::Bool, depth);
            if (!l || l->kind != ComptimeValue::Kind::Bool) {
                return std::nullopt;
            }
            if (bin.op == ast::BinaryOp::And && !l->b) {
                return make_bool(false);
            }
            if (bin.op == ast::BinaryOp::Or && l->b) {
                return make_bool(true);
            }
            auto r = fold(*bin.rhs, env, TypeKind::Bool, depth);
            if (!r || r->kind != ComptimeValue::Kind::Bool) {
                return std::nullopt;
            }
            return make_bool(r->b);
        }

        auto lhs = fold(*bin.lhs, env, hint, depth);
        if (!lhs) {
            return std::nullopt;
        }
        // For the rhs, push the lhs's resolved Vestra type as the hint so
        // integer literals on the right adopt the same type (matches how
        // sema's bidirectional check works in check_binary).
        auto rhs = fold(*bin.rhs, env, lhs->type, depth);
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
        auto c = fold(*i.cond, env, TypeKind::Bool, depth);
        if (!c || c->kind != ComptimeValue::Kind::Bool) {
            return std::nullopt;
        }
        // Fold only the chosen branch — matching §19.9 (if evaluates only
        // the selected branch). The unchosen branch isn't even examined.
        if (c->b) {
            return fold(*i.then_branch, env, hint, depth);
        }
        if (i.else_branch) {
            return fold(*i.else_branch, env, hint, depth);
        }
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

}  // namespace vestra::sema
