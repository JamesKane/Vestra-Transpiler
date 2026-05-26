// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "vestra/codegen/cpp_emitter.hpp"

#include <algorithm>
#include <format>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>

namespace vestra::codegen {

namespace {

void write_indent(std::ostream& os, int indent) {
    for (int i = 0; i < indent; ++i) {
        os << "    ";
    }
}

// Any payload at all on any case means the enum lowers to a
// `std::variant<case_t…>` wrapper, and every case construction needs
// the `Enum{Enum::case_t{…}}` brace-shape. A bare enum lowers to plain
// `enum class Enum { … }` and constructs as `Enum::case` directly.
bool enum_is_sum_type(const ast::EnumDecl& e) noexcept {
    for (const auto& c : e.cases) {
        if (!c.payload.empty()) {
            return true;
        }
    }
    return false;
}

// Map Vestra primitive names to C++ equivalents.
const std::unordered_map<std::string, std::string>& primitive_map() {
    static const std::unordered_map<std::string, std::string> m = {
        {"Int8", "std::int8_t"},
        {"Int16", "std::int16_t"},
        {"Int32", "std::int32_t"},
        {"Int64", "std::int64_t"},
        {"UInt8", "std::uint8_t"},
        {"UInt16", "std::uint16_t"},
        {"UInt32", "std::uint32_t"},
        {"UInt64", "std::uint64_t"},
        {"Int", "std::intptr_t"},
        {"UInt", "std::uintptr_t"},
        {"Float32", "float"},
        {"Float64", "double"},
        {"Bool", "bool"},
        {"Char", "char32_t"},
        {"Unit", "void"},
        {"String", "std::string"},
        {"Str", "std::string_view"},
        {"StrConst", "std::string_view"},
    };
    return m;
}

// §4 layout attributes — extract a small struct of what's interesting
// for codegen. The Vestra parser stores each attribute as a name plus
// a single-expression argument; we pattern-match the argument shape.
struct LayoutAttrs {
    bool packed = false;
    std::int64_t align = 0;  // 0 = no explicit alignment
};

std::int64_t int_from_attr_arg(const ast::Expr* arg) {
    if (arg == nullptr || arg->kind != ast::NodeKind::IntLit) {
        return 0;
    }
    try {
        return std::stoll(std::string{static_cast<const ast::IntLit&>(*arg).text});
    } catch (...) {
        return 0;
    }
}

LayoutAttrs read_layout_attrs(const std::vector<ast::Attribute>& attrs) {
    LayoutAttrs out;
    for (const auto& a : attrs) {
        if (a.name == "repr") {
            // `@repr(C)` — no-op (C++ default). `@repr(packed)` — adds
            // __attribute__((packed)). `@repr(align(N))` — alignas(N).
            if (a.predicate == nullptr) {
                continue;
            }
            if (a.predicate->kind == ast::NodeKind::IdentExpr) {
                const auto& id = static_cast<const ast::IdentExpr&>(*a.predicate).name;
                if (id == "packed") {
                    out.packed = true;
                }
                // `C` and any unknown @repr modes are accepted but
                // not given a special emission yet.
            } else if (a.predicate->kind == ast::NodeKind::CallExpr) {
                const auto& call = static_cast<const ast::CallExpr&>(*a.predicate);
                if (call.callee->kind == ast::NodeKind::IdentExpr) {
                    const auto& name = static_cast<const ast::IdentExpr&>(*call.callee).name;
                    if (name == "align" && call.args.size() == 1) {
                        out.align = int_from_attr_arg(call.args[0].value.get());
                    }
                }
            }
        } else if (a.name == "align") {
            // `@align(N)` shorthand for `@repr(align(N))`.
            out.align = int_from_attr_arg(a.predicate.get());
        }
    }
    return out;
}

std::int64_t read_bits_attr(const std::vector<ast::Attribute>& attrs) {
    for (const auto& a : attrs) {
        if (a.name == "bits") {
            return int_from_attr_arg(a.predicate.get());
        }
    }
    return 0;
}

}  // namespace

// ----------------------------------------------------------------------------

EmittedUnit CppEmitter::emit(const ast::CompilationUnit& unit, std::string_view output_basename) {
    std::ostringstream hdr;
    std::ostringstream src;

    // §12.3 derive index: collect every `derive(P1, P2, …) for T` at
    // the top level into target-name → {protocol names}. emit_struct
    // and emit_enum read this when injecting the reflective defaults.
    // Each target/protocol path is reduced to its last segment (its
    // simple name) which matches the scope's nominal lookup today.
    derives_by_target_.clear();
    for (const auto& d : unit.decls) {
        if (d->kind != ast::NodeKind::Derive) {
            continue;
        }
        const auto& dd = static_cast<const ast::DeriveDecl&>(*d);
        if (dd.target == nullptr || dd.target->kind != ast::NodeKind::NamedType) {
            continue;
        }
        const auto& tt = static_cast<const ast::NamedType&>(*dd.target);
        if (tt.path.empty()) {
            continue;
        }
        const auto& target_name = tt.path.back();
        for (const auto& p : dd.protocols) {
            if (p == nullptr || p->kind != ast::NodeKind::NamedType) {
                continue;
            }
            const auto& pt = static_cast<const ast::NamedType&>(*p);
            if (pt.path.empty()) {
                continue;
            }
            derives_by_target_[target_name].insert(pt.path.back());
        }
    }

    auto guard = std::string{output_basename};
    for (auto& c : guard) {
        c = (std::isalnum(static_cast<unsigned char>(c)) != 0)
                ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
                : '_';
    }

    hdr << "// Generated by vestra-transpiler. Do not edit.\n";
    hdr << "#pragma once\n\n";
    hdr << "#include <cstdint>\n";
    hdr << "#include <cstdlib>\n";     // §10 panic / abort → std::abort
    hdr << "#include <expected>\n";    // §9 throws(E) → T lowers to std::expected
    hdr << "#include <format>\n";      // §4 interpolated strings lower to std::format
    hdr << "#include <functional>\n";  // §12.3 derive(Hash) std::hash specializations
    hdr << "#include <memory>\n";      // §10 Box[T] lowers to std::unique_ptr
    hdr << "#include <optional>\n";    // §9 T? / nil / if let / ?? / !
    hdr << "#include <print>\n";       // §10 panic shim writes to stderr
    hdr << "#include <span>\n";        // §10 Span[T] / MutSpan[T] views
    hdr << "#include <string>\n";
    hdr << "#include <string_view>\n";
    hdr << "#include <tuple>\n";        // §6 tuple types / literals
    hdr << "#include <type_traits>\n";  // match-over-payloaded-enum constexpr-if
    hdr << "#include <utility>\n";      // std::move at sink call sites
    hdr << "#include <variant>\n";
    hdr << "#include <vector>\n\n";

    // §10 runtime shims. `inline` keeps the definitions safe across
    // multiple Vestra-generated headers in one TU (each defines the
    // same function body, which inline tolerates). `Never` is the
    // bottom type — Vestra's `panic` / `abort` / `unreachable` all
    // return Never, which has a templated `[[noreturn]]` conversion
    // op so a call slots into any C++ type the surrounding expression
    // requires (`std::int32_t x = __vstr::panic("oops");` is well-typed
    // because Never→int32_t is a valid implicit conversion that the
    // compiler proves dead via the [[noreturn]] marker on every step).
    hdr << "namespace __vstr {\n\n";
    hdr << "struct Never {\n";
    hdr << "    template <class T> [[noreturn]] operator T() const noexcept {\n";
    hdr << "        std::unreachable();\n";
    hdr << "    }\n";
    hdr << "};\n\n";
    hdr << "[[noreturn]] inline Never panic(std::string_view msg) noexcept {\n";
    hdr << "    std::println(stderr, \"vestra panic: {}\", msg);\n";
    hdr << "    std::abort();\n";
    hdr << "}\n\n";
    hdr << "[[noreturn]] inline Never abort_fn() noexcept {\n";
    hdr << "    std::abort();\n";
    hdr << "}\n\n";
    hdr << "[[noreturn]] inline Never unreachable_fn() noexcept {\n";
    hdr << "    std::unreachable();\n";
    hdr << "}\n\n";
    // Forwarding-reference shape: `opt!` may apply to either a value
    // (rvalue) or a binding (lvalue, including the `const &` param
    // read-mode renders into); `Opt&&` binds to both, and the matching
    // `std::forward` keeps the deref's value category.
    hdr << "template <class Opt>\n";
    hdr << "[[nodiscard]] inline auto unwrap_opt(Opt&& o) {\n";
    hdr << "    if (!o.has_value()) { panic(\"force-unwrap of nil Optional\"); }\n";
    hdr << "    return *std::forward<Opt>(o);\n";
    hdr << "}\n\n";
    hdr << "template <class Exp>\n";
    hdr << "[[nodiscard]] inline auto unwrap_exp(Exp&& r) {\n";
    hdr << "    if (!r.has_value()) { panic(\"force-unwrap of Result error\"); }\n";
    hdr << "    return *std::forward<Exp>(r);\n";
    hdr << "}\n\n";
    hdr << "}  // namespace __vstr\n\n";

    // §4 Optional in a Display splice. Vestra renders `"\(opt)"` as
    // `nil` for `.none` and delegates to T's formatter for `.some(v)`.
    // libc++ on Apple Clang 21 doesn't yet ship the C++26 P2585
    // formatter for std::optional, so we provide a partial
    // specialization in namespace std. The constraint on
    // std::formattable<T, char> keeps it composable with anything T's
    // own formatter accepts, and `inline` keeps the spec safe across
    // headers in one TU.
    hdr << "namespace std {\n";
    hdr << "template <class T>\n";
    hdr << "    requires std::formattable<T, char>\n";
    hdr << "struct formatter<std::optional<T>, char> {\n";
    hdr << "    std::formatter<T, char> inner;\n";
    hdr << "    constexpr auto parse(std::format_parse_context& ctx) {\n";
    hdr << "        return inner.parse(ctx);\n";
    hdr << "    }\n";
    hdr << "    template <class FormatContext>\n";
    hdr << "    auto format(const std::optional<T>& opt, FormatContext& ctx) const {\n";
    hdr << "        if (opt.has_value()) { return inner.format(*opt, ctx); }\n";
    hdr << "        return std::format_to(ctx.out(), \"nil\");\n";
    hdr << "    }\n";
    hdr << "};\n";
    hdr << "}  // namespace std\n\n";

    auto write_module_path = [&](std::ostream& os) {
        for (std::size_t i = 0; i < unit.module->path.size(); ++i) {
            if (i != 0) {
                os << "::";
            }
            os << unit.module->path[i];
        }
    };

    if (unit.module) {
        // Vestra module path → C++ nested namespace.
        hdr << "namespace ";
        write_module_path(hdr);
        hdr << " {\n\n";
    }

    src << "// Generated by vestra-transpiler. Do not edit.\n";
    src << "#include \"" << output_basename << ".hpp\"\n\n";

    if (unit.module) {
        src << "namespace ";
        write_module_path(src);
        src << " {\n\n";
    }

    for (const auto& d : unit.decls) {
        // §12.6: skip @when-gated-out decls. The Resolver populated the
        // Resolution's gated set during its own pass.
        if (resolution_ != nullptr && resolution_->is_gated_out(d.get())) {
            continue;
        }
        emit_decl(hdr, src, *d);
    }

    if (unit.module) {
        hdr << "\n}  // namespace ";
        write_module_path(hdr);
        hdr << "\n";
        src << "\n}  // namespace ";
        write_module_path(src);
        src << "\n";
    }

    // §12.3 derive(Hash): every `template <> struct std::hash<…>` lives
    // at global scope, so we emit those specializations AFTER the user's
    // namespace closes. We walk the decl list a second time here so the
    // emission order is predictable (declaration order, struct-only for
    // now — payloaded-enum Hash needs an extra std::visit-driven body).
    std::string qual_prefix;
    if (unit.module) {
        for (const auto& seg : unit.module->path) {
            qual_prefix += seg;
            qual_prefix += "::";
        }
    }
    for (const auto& d : unit.decls) {
        if (d->kind == ast::NodeKind::Struct) {
            const auto& sd = static_cast<const ast::StructDecl&>(*d);
            auto it = derives_by_target_.find(sd.name);
            if (it != derives_by_target_.end() && it->second.contains("Hash")) {
                hdr << "\n";
                emit_hash_spec(hdr, sd, qual_prefix);
            }
        } else if (d->kind == ast::NodeKind::Enum) {
            // Sum-type enums need an explicit std::hash spec because the
            // std::variant wrapper isn't hashable by default. Bare enums
            // are handled by the standard library's enum-class hash.
            const auto& ed = static_cast<const ast::EnumDecl&>(*d);
            auto it = derives_by_target_.find(ed.name);
            if (it != derives_by_target_.end() && it->second.contains("Hash")
                && enum_is_sum_type(ed)) {
                hdr << "\n";
                emit_hash_spec_enum(hdr, ed, qual_prefix);
            }
        } else if (d->kind == ast::NodeKind::Opaque) {
            // §3 opaque newtype: enum class `Q : T {}` has no implicit
            // std::hash specialization, so we synthesize one that
            // delegates to std::hash<T> over static_cast<T>(q).
            const auto& od = static_cast<const ast::OpaqueDecl&>(*d);
            auto it = derives_by_target_.find(od.name);
            if (it != derives_by_target_.end() && it->second.contains("Hash")) {
                hdr << "\n";
                emit_hash_spec_opaque(hdr, od, qual_prefix);
            }
        }
    }

    // §12.3 derive(Debug) + §4 derive(Display): both lower to a
    // std::formatter<T> specialization so `std::format("{}", v)` and
    // string interpolation work. For v0.5 the two produce the same
    // structural rendering — Display is meant to be more user-facing,
    // but until we have a customizable `display(into: Sink)` body
    // they share the Debug-flavoured output. Deriving both on the
    // same type emits a single spec (we'd otherwise hit a C++ ODR
    // violation on duplicate template specializations).
    for (const auto& d : unit.decls) {
        if (d->kind == ast::NodeKind::Struct) {
            const auto& sd = static_cast<const ast::StructDecl&>(*d);
            auto it = derives_by_target_.find(sd.name);
            if (it != derives_by_target_.end()
                && (it->second.contains("Debug") || it->second.contains("Display"))) {
                hdr << "\n";
                emit_debug_spec_struct(hdr, sd, qual_prefix);
            }
        } else if (d->kind == ast::NodeKind::Enum) {
            const auto& ed = static_cast<const ast::EnumDecl&>(*d);
            auto it = derives_by_target_.find(ed.name);
            if (it != derives_by_target_.end()
                && (it->second.contains("Debug") || it->second.contains("Display"))) {
                hdr << "\n";
                emit_debug_spec_enum(hdr, ed, qual_prefix);
            }
        } else if (d->kind == ast::NodeKind::Opaque) {
            // §3 opaque newtype Debug/Display: render `Q(value)` via the
            // underlying T's formatter.
            const auto& od = static_cast<const ast::OpaqueDecl&>(*d);
            auto it = derives_by_target_.find(od.name);
            if (it != derives_by_target_.end()
                && (it->second.contains("Debug") || it->second.contains("Display"))) {
                hdr << "\n";
                emit_debug_spec_opaque(hdr, od, qual_prefix);
            }
        }
    }

    return {hdr.str(), src.str()};
}

// ----------------------------------------------------------------------------

void CppEmitter::unsupported(std::ostream& os, std::string_view what, diag::SourceRange r) {
    reporter_->report(
        diag::Diagnostic::warning(std::format("codegen does not yet support: {}", what)).at(r));
    os << "/* unsupported: " << what << " */";
}

const char* CppEmitter::binop_text(ast::BinaryOp op) {
    switch (op) {
    case ast::BinaryOp::Add:
        return "+";
    case ast::BinaryOp::Sub:
        return "-";
    case ast::BinaryOp::Mul:
        return "*";
    case ast::BinaryOp::Div:
        return "/";
    case ast::BinaryOp::Mod:
        return "%";
    case ast::BinaryOp::Shl:
        return "<<";
    case ast::BinaryOp::Shr:
        return ">>";
    case ast::BinaryOp::BitAnd:
        return "&";
    case ast::BinaryOp::BitOr:
        return "|";
    case ast::BinaryOp::BitXor:
        return "^";
    case ast::BinaryOp::Eq:
        return "==";
    case ast::BinaryOp::Ne:
        return "!=";
    case ast::BinaryOp::Lt:
        return "<";
    case ast::BinaryOp::Le:
        return "<=";
    case ast::BinaryOp::Gt:
        return ">";
    case ast::BinaryOp::Ge:
        return ">=";
    case ast::BinaryOp::And:
        return "&&";
    case ast::BinaryOp::Or:
        return "||";
    case ast::BinaryOp::WrappingAdd:
        return "+";  // C++ on unsigned wraps; signed UB — TODO
    case ast::BinaryOp::WrappingSub:
        return "-";
    case ast::BinaryOp::WrappingMul:
        return "*";
    default:
        return "/*?op*/";
    }
}

const char* CppEmitter::unop_text(ast::UnaryOp op) {
    switch (op) {
    case ast::UnaryOp::Neg:
        return "-";
    case ast::UnaryOp::Not:
        return "!";
    case ast::UnaryOp::BitNot:
        return "~";
    case ast::UnaryOp::Unwrap:
        // Handled specially in emit_expr — never reaches the prefix path.
        return "";
    }
    return "?";
}

// ----------------------------------------------------------------------------

void CppEmitter::emit_decl(std::ostream& hdr, std::ostream& src, const ast::Decl& d) {
    switch (d.kind) {
    case ast::NodeKind::Func: {
        const auto& f = static_cast<const ast::FuncDecl&>(d);
        // §12.4 / §12.1: `comptime func`s are evaluated by the folder at
        // compile time only. They have no runtime form to emit (and their
        // bodies often use language constructs — Range, comptime stdlib
        // calls — that don't have a runtime lowering yet). Callers that
        // reach a comptime func via fold get the literal substituted; any
        // hypothetical runtime call sites would be the resolver's job to
        // reject.
        if (f.is_comptime) {
            break;
        }
        emit_func(hdr, src, f);
        break;
    }
    case ast::NodeKind::Struct:
        emit_struct(hdr, static_cast<const ast::StructDecl&>(d));
        break;
    case ast::NodeKind::Enum:
        emit_enum(hdr, static_cast<const ast::EnumDecl&>(d));
        break;
    case ast::NodeKind::Const: {
        const auto& c = static_cast<const ast::ConstDecl&>(d);
        hdr << "inline constexpr ";
        if (c.type) {
            emit_type(hdr, *c.type);
        } else {
            hdr << "auto";
        }
        hdr << " " << c.name << " = ";
        if (c.value) {
            emit_expr(hdr, *c.value);
        }
        hdr << ";\n";
        break;
    }
    case ast::NodeKind::Opaque: {
        const auto& o = static_cast<const ast::OpaqueDecl&>(d);
        hdr << "// opaque type " << o.name << "\n";
        hdr << "enum class " << o.name << " : ";
        emit_type(hdr, *o.underlying);
        hdr << " {};\n";
        break;
    }
    case ast::NodeKind::Derive:
        // §12.3: derive(...) is sugar for an empty extension that
        // adopts the protocol's reflective default. The actual code
        // gets emitted into the target struct/enum body via the
        // derives_by_target_ index built in emit(); there's nothing
        // to write at the DeriveDecl's own position.
        break;
    default:
        unsupported(hdr, std::format("top-level decl kind {}", static_cast<int>(d.kind)), d.range);
        hdr << "\n";
        break;
    }
}

void CppEmitter::emit_func(std::ostream& hdr, std::ostream& src, const ast::FuncDecl& f) {
    // §9 propagation: conditional hoists need to write
    // `std::expected<T, E>` as their lambda's return type, where E is
    // this function's `throws(E)`. Stash + restore for nested funcs.
    const ast::Type* prev_throws = current_throws_type_;
    current_throws_type_ = f.effects.throws_type.get();
    struct ThrowsRestore {
        CppEmitter* self;
        const ast::Type* prev;
        ~ThrowsRestore() { self->current_throws_type_ = prev; }
    } _throws_restore{this, prev_throws};

    // §7 generics: a Vestra generic function lowers to a C++ template. The
    // host compiler then monomorphizes per instantiation, which is what
    // Vestra semantically requires anyway. Const generics ([const N: Int])
    // are deferred — phase 1 only handles type parameters.
    const bool has_type_generics =
        std::any_of(f.generics.begin(), f.generics.end(), [](const ast::GenericParam& g) {
            return !g.is_const && !g.name.empty();
        });
    auto emit_template_prefix = [&](std::ostream& os) {
        if (!has_type_generics) {
            return;
        }
        os << "template <";
        bool first = true;
        for (const auto& g : f.generics) {
            if (g.is_const || g.name.empty()) {
                continue;
            }
            if (!first) {
                os << ", ";
            }
            first = false;
            os << "class " << g.name;
        }
        os << ">\n";
    };

    // §6 tuple-pattern param: a param whose AST stores a TuplePat (no
    // `name`) gets a synthetic C++ identifier — the structured binding
    // happens via prologue statements at the top of the body. The same
    // name is used for the signature in header + source so the
    // forward-declared decl and the definition agree.
    auto param_cpp_name = [&](std::size_t i, const ast::Param& p) -> std::string {
        if (p.pattern != nullptr) {
            return std::format("__vstr_arg{}", i);
        }
        return p.name;
    };

    auto emit_signature = [&](std::ostream& os) {
        // §9: a `throws(E)` clause wraps the user-visible result in
        // `std::expected<T, E>` so callers see the fallible type. Inside
        // the body, `return x` works via the converting ctor; `throw e`
        // and `try f()` are lowered to the matching std::expected ops.
        if (f.effects.throws_type) {
            os << "std::expected<";
            if (f.result) {
                emit_type(os, *f.result);
            } else {
                os << "void";
            }
            os << ", ";
            emit_type(os, *f.effects.throws_type);
            os << ">";
        } else if (f.result) {
            emit_type(os, *f.result);
        } else {
            os << "void";
        }
        os << " " << f.name << "(";
        for (std::size_t i = 0; i < f.params.size(); ++i) {
            if (i != 0) {
                os << ", ";
            }
            const auto& p = f.params[i];
            // Mode mapping (very rough first pass — §5 ownership is unresolved):
            //   read  → const T&
            //   inout → T&
            //   sink  → T&&
            switch (p.mode) {
            case ast::ParamMode::Read:
                os << "const ";
                if (p.type) {
                    emit_type(os, *p.type);
                }
                os << "& " << param_cpp_name(i, p);
                break;
            case ast::ParamMode::Inout:
                if (p.type) {
                    emit_type(os, *p.type);
                }
                os << "& " << param_cpp_name(i, p);
                break;
            case ast::ParamMode::Sink:
                if (p.type) {
                    emit_type(os, *p.type);
                }
                os << "&& " << param_cpp_name(i, p);
                break;
            }
        }
        os << ")";
    };

    // §6 prologue for tuple-pattern params: each synthetic arg gets a
    // structured-binding statement at the top of the body, plus any
    // nested follow-on unpacks via the same helpers LetStmt uses.
    auto emit_tuple_param_prologue = [&](std::ostream& os) {
        for (std::size_t i = 0; i < f.params.size(); ++i) {
            const auto& p = f.params[i];
            if (p.pattern == nullptr || p.pattern->kind != ast::NodeKind::TuplePat) {
                continue;
            }
            const auto& tp = static_cast<const ast::TuplePat&>(*p.pattern);
            std::vector<std::string> names;
            std::vector<std::pair<std::string, const ast::TuplePat*>> followons;
            collect_tuple_pat_names(tp, names, followons);
            write_indent(os, 1);
            os << "auto [";
            for (std::size_t j = 0; j < names.size(); ++j) {
                if (j != 0) {
                    os << ", ";
                }
                os << names[j];
            }
            os << "] = " << param_cpp_name(i, p) << ";\n";
            emit_tuple_pat_followons(os, followons, 1);
        }
    };

    // `[[nodiscard]]` is invalid on void-returning functions — emit it
    // only when there's a real result type or a `throws(E)` wrapper.
    const bool has_result = f.result != nullptr || f.effects.throws_type != nullptr;

    // Templates must be visible at every instantiation site, so a generic
    // function lives entirely in the header. Non-generics keep the
    // declaration/definition split.
    if (has_type_generics) {
        emit_template_prefix(hdr);
        if (has_result) {
            hdr << "[[nodiscard]] ";
        }
        emit_signature(hdr);
        if (!f.body) {
            hdr << ";\n";
            return;
        }
        hdr << " {\n";
        emit_tuple_param_prologue(hdr);
        if (f.body->kind == ast::NodeKind::BlockExpr) {
            const auto& blk = static_cast<const ast::BlockExpr&>(*f.body);
            for (const auto& s : blk.stmts) {
                emit_stmt(hdr, *s, 1);
            }
        } else {
            unsupported(hdr, "non-block function body", f.body->range);
        }
        hdr << "}\n\n";
        return;
    }

    if (has_result) {
        hdr << "[[nodiscard]] ";
    }
    emit_signature(hdr);
    hdr << ";\n";

    if (!f.body) {
        return;
    }

    emit_signature(src);
    src << " {\n";
    emit_tuple_param_prologue(src);
    if (f.body->kind == ast::NodeKind::BlockExpr) {
        const auto& blk = static_cast<const ast::BlockExpr&>(*f.body);
        for (const auto& s : blk.stmts) {
            emit_stmt(src, *s, 1);
        }
    } else {
        unsupported(src, "non-block function body", f.body->range);
    }
    src << "}\n\n";
}

void CppEmitter::emit_struct(std::ostream& hdr, const ast::StructDecl& s) {
    // §4 layout attributes on the struct itself: @repr(packed) +
    // @repr(align(N)) / @align(N). C++ accepts `alignas(N)` between
    // the `struct` keyword and the name; __attribute__((packed)) goes
    // after the closing brace and before the trailing `;`.
    const auto struct_attrs = read_layout_attrs(s.attributes);
    hdr << "struct ";
    if (struct_attrs.align > 0) {
        hdr << "alignas(" << struct_attrs.align << ") ";
    }
    hdr << s.name << " {\n";
    for (const auto& f : s.fields) {
        write_indent(hdr, 1);
        const auto field_attrs = read_layout_attrs(f.attributes);
        const auto bits = read_bits_attr(f.attributes);
        if (field_attrs.align > 0) {
            hdr << "alignas(" << field_attrs.align << ") ";
        }
        if (f.type) {
            emit_type(hdr, *f.type);
        }
        hdr << " " << f.name;
        if (bits > 0) {
            // Bit-fields can't have a default brace-init; leave the
            // default value to the zero-init the C++ compiler gives
            // each member when its enclosing struct is default-ctor'd.
            hdr << " : " << bits;
        } else {
            hdr << "{}";
        }
        hdr << ";\n";
    }
    // §12.3 derive(Eq): a defaulted operator== gives us field-by-field
    // structural equality for free, with the C++ compiler doing the
    // recursion through any nested derived-Eq members.
    if (auto it = derives_by_target_.find(s.name); it != derives_by_target_.end()) {
        if (it->second.contains("Eq")) {
            write_indent(hdr, 1);
            hdr << "[[nodiscard]] bool operator==(const " << s.name
                << "&) const noexcept = default;\n";
        }
        // §12.3 derive(Clone): emit an explicit `clone()` that delegates
        // to the implicit C++ copy ctor. For v0.5 every Vestra struct is
        // copy-constructible (we don't have move-only types yet), so the
        // structural traversal the spec calls for happens for free. The
        // method exists so users have an explicit copy site rather than
        // a silent one.
        if (it->second.contains("Clone")) {
            write_indent(hdr, 1);
            hdr << "[[nodiscard]] " << s.name << " clone() const { return *this; }\n";
        }
    }
    // §5: methods declared inside `struct { ... }` lower to inline C++
    // methods on the same struct. v0.5 is minimal: no const-qualification
    // (so an `inout` method works), no generics, no out-of-line bodies.
    // The implicit `this` carries `self` through; SelfExpr already
    // lowers to `(*this)` so member access works.
    for (const auto& m : s.methods) {
        if (m->kind != ast::NodeKind::Func) {
            continue;
        }
        const auto& fn = static_cast<const ast::FuncDecl&>(*m);
        write_indent(hdr, 1);
        hdr << "[[nodiscard]] ";
        if (fn.result) {
            emit_type(hdr, *fn.result);
        } else {
            hdr << "void";
        }
        hdr << " " << fn.name << "(";
        for (std::size_t i = 0; i < fn.params.size(); ++i) {
            if (i != 0) {
                hdr << ", ";
            }
            const auto& p = fn.params[i];
            switch (p.mode) {
            case ast::ParamMode::Read:
                hdr << "const ";
                if (p.type) {
                    emit_type(hdr, *p.type);
                }
                hdr << "& " << p.name;
                break;
            case ast::ParamMode::Inout:
                if (p.type) {
                    emit_type(hdr, *p.type);
                }
                hdr << "& " << p.name;
                break;
            case ast::ParamMode::Sink:
                if (p.type) {
                    emit_type(hdr, *p.type);
                }
                hdr << "&& " << p.name;
                break;
            }
        }
        hdr << ") ";
        if (fn.body && fn.body->kind == ast::NodeKind::BlockExpr) {
            emit_block(hdr, static_cast<const ast::BlockExpr&>(*fn.body), 1);
        } else {
            hdr << "{}";
        }
        hdr << "\n";
    }
    hdr << "}";
    if (struct_attrs.packed) {
        hdr << " __attribute__((packed))";
    }
    hdr << ";\n\n";
}

void CppEmitter::emit_hash_spec(std::ostream& os,
                                const ast::StructDecl& s,
                                std::string_view qual_prefix) {
    // Boost-style hash combine: cheap, no <bit> dependency, gives a
    // decent distribution as long as each field has a sensible
    // std::hash. Doesn't need to be cryptographically strong — it's
    // for unordered_map bucket dispatch.
    os << "template <>\n";
    os << "struct std::hash<" << qual_prefix << s.name << "> {\n";
    os << "    [[nodiscard]] std::size_t operator()(const " << qual_prefix << s.name
       << "& v) const noexcept {\n";
    os << "        std::size_t __h = 0;\n";
    for (const auto& f : s.fields) {
        if (f.kind == ast::StructDecl::Field::Kind::Embed || f.type == nullptr) {
            // Embed flattening for hash is a follow-on; skip for now.
            continue;
        }
        os << "        __h ^= std::hash<";
        emit_type(os, *f.type);
        os << ">{}(v." << f.name << ") + 0x9e3779b9 + (__h << 6) + (__h >> 2);\n";
    }
    os << "        return __h;\n";
    os << "    }\n";
    os << "};\n";
}

void CppEmitter::emit_hash_spec_opaque(std::ostream& os,
                                       const ast::OpaqueDecl& o,
                                       std::string_view qual_prefix) {
    if (o.underlying == nullptr) {
        return;
    }
    os << "template <>\n";
    os << "struct std::hash<" << qual_prefix << o.name << "> {\n";
    os << "    [[nodiscard]] std::size_t operator()(" << qual_prefix << o.name
       << " v) const noexcept {\n";
    os << "        return std::hash<";
    emit_type(os, *o.underlying);
    os << ">{}(static_cast<";
    emit_type(os, *o.underlying);
    os << ">(v));\n";
    os << "    }\n";
    os << "};\n";
}

void CppEmitter::emit_debug_spec_opaque(std::ostream& os,
                                        const ast::OpaqueDecl& o,
                                        std::string_view qual_prefix) {
    if (o.underlying == nullptr) {
        return;
    }
    // Render as `Q(value)` — keeps the nominal name visible alongside
    // the underlying. Delegates to the underlying's std::formatter via
    // the `{}` placeholder so widths, precisions, etc. flow through.
    os << "template <>\n";
    os << "struct std::formatter<" << qual_prefix << o.name << "> {\n";
    os << "    constexpr auto parse(auto& __ctx) { return __ctx.begin(); }\n";
    os << "    auto format(" << qual_prefix << o.name << " v, auto& __ctx) const {\n";
    os << "        return std::format_to(__ctx.out(), \"" << o.name << "({})\", static_cast<";
    emit_type(os, *o.underlying);
    os << ">(v));\n";
    os << "    }\n";
    os << "};\n";
}

void CppEmitter::emit_hash_spec_enum(std::ostream& os,
                                     const ast::EnumDecl& e,
                                     std::string_view qual_prefix) {
    // Bare enums fall back to the standard library's `std::hash<E>`;
    // we only need a spec when the enum lowers to a struct-of-variant.
    if (!enum_is_sum_type(e)) {
        return;
    }
    os << "template <>\n";
    os << "struct std::hash<" << qual_prefix << e.name << "> {\n";
    os << "    [[nodiscard]] std::size_t operator()(const " << qual_prefix << e.name
       << "& v) const noexcept {\n";
    os << "        return std::visit([&](auto&& __alt) {\n";
    os << "            using __T = std::decay_t<decltype(__alt)>;\n";
    os << "            std::size_t __h = v.value.index();\n";
    bool first = true;
    for (const auto& c : e.cases) {
        if (first) {
            write_indent(os, 3);
            os << "if";
        } else {
            os << " else if";
        }
        os << " constexpr (std::is_same_v<__T, " << qual_prefix << e.name << "::" << c.name
           << "_t>) {\n";
        for (std::size_t i = 0; i < c.payload.size(); ++i) {
            const auto& [pname, ptype] = c.payload[i];
            std::string fname = pname.empty() ? std::format("_{}", i) : pname;
            write_indent(os, 4);
            os << "__h ^= std::hash<";
            emit_type(os, *ptype);
            os << ">{}(__alt." << fname << ") + 0x9e3779b9 + (__h << 6) + (__h >> 2);\n";
        }
        write_indent(os, 3);
        os << "}";
        first = false;
    }
    if (!first) {
        os << "\n";
    }
    os << "            return __h;\n";
    os << "        }, v.value);\n";
    os << "    }\n";
    os << "};\n";
}

void CppEmitter::emit_debug_spec_struct(std::ostream& os,
                                        const ast::StructDecl& s,
                                        std::string_view qual_prefix) {
    // Renders as `Name{field1: <v.field1>, field2: <v.field2>, …}`.
    // Each `{}` in the std::format string is a placeholder; literal
    // braces in the output must be doubled (`{{` / `}}`) to survive
    // std::format's brace syntax.
    os << "template <>\n";
    os << "struct std::formatter<" << qual_prefix << s.name << "> {\n";
    os << "    constexpr auto parse(auto& __ctx) { return __ctx.begin(); }\n";
    os << "    auto format(const " << qual_prefix << s.name << "& v, auto& __ctx) const {\n";
    os << "        return std::format_to(__ctx.out(), \"" << s.name << "{{";
    bool first = true;
    for (const auto& f : s.fields) {
        if (f.kind == ast::StructDecl::Field::Kind::Embed || f.type == nullptr) {
            continue;
        }
        if (!first) {
            os << ", ";
        }
        os << f.name << ": {}";
        first = false;
    }
    os << "}}\"";
    for (const auto& f : s.fields) {
        if (f.kind == ast::StructDecl::Field::Kind::Embed || f.type == nullptr) {
            continue;
        }
        os << ", v." << f.name;
    }
    os << ");\n";
    os << "    }\n";
    os << "};\n";
}

void CppEmitter::emit_debug_spec_enum(std::ostream& os,
                                      const ast::EnumDecl& e,
                                      std::string_view qual_prefix) {
    os << "template <>\n";
    os << "struct std::formatter<" << qual_prefix << e.name << "> {\n";
    os << "    constexpr auto parse(auto& __ctx) { return __ctx.begin(); }\n";
    if (!enum_is_sum_type(e)) {
        // Bare enum class: render `EnumName.caseName` via a switch.
        os << "    auto format(" << qual_prefix << e.name << " v, auto& __ctx) const {\n";
        os << "        switch (v) {\n";
        for (const auto& c : e.cases) {
            os << "            case " << qual_prefix << e.name << "::" << c.name
               << ": return std::format_to(__ctx.out(), \"" << e.name << "." << c.name << "\");\n";
        }
        os << "        }\n";
        os << "        return std::format_to(__ctx.out(), \"?\");\n";
        os << "    }\n";
        os << "};\n";
        return;
    }
    // Sum-type enum: std::visit + constexpr-if chain, mirroring
    // emit_match. Each alternative renders as `EnumName::caseName{…}`
    // for payloaded cases, or just `EnumName::caseName` for bare.
    os << "    auto format(const " << qual_prefix << e.name << "& v, auto& __ctx) const {\n";
    os << "        return std::visit([&](auto&& __alt) {\n";
    os << "            using __T = std::decay_t<decltype(__alt)>;\n";
    bool first = true;
    for (const auto& c : e.cases) {
        if (first) {
            write_indent(os, 3);
            os << "if";
        } else {
            os << " else if";  // chained — previous arm ended with bare "}"
        }
        os << " constexpr (std::is_same_v<__T, " << qual_prefix << e.name << "::" << c.name
           << "_t>) {\n";
        write_indent(os, 4);
        if (c.payload.empty()) {
            os << "return std::format_to(__ctx.out(), \"" << e.name << "::" << c.name << "\");\n";
        } else {
            os << "return std::format_to(__ctx.out(), \"" << e.name << "::" << c.name << "{{";
            for (std::size_t i = 0; i < c.payload.size(); ++i) {
                if (i != 0) {
                    os << ", ";
                }
                std::string fname =
                    c.payload[i].first.empty() ? std::format("_{}", i) : c.payload[i].first;
                os << fname << ": {}";
            }
            os << "}}\"";
            for (std::size_t i = 0; i < c.payload.size(); ++i) {
                std::string fname =
                    c.payload[i].first.empty() ? std::format("_{}", i) : c.payload[i].first;
                os << ", __alt." << fname;
            }
            os << ");\n";
        }
        write_indent(os, 3);
        os << "}";
        first = false;
    }
    os << " else { std::unreachable(); }\n";
    os << "        }, v.value);\n";
    os << "    }\n";
    os << "};\n";
}

void CppEmitter::emit_enum(std::ostream& hdr, const ast::EnumDecl& e) {
    // A Vestra enum with payloads maps cleanly to std::variant; bare enums to
    // a C++ enum class. We detect "all bare" cheaply.
    bool all_bare = true;
    for (const auto& c : e.cases) {
        if (!c.payload.empty()) {
            all_bare = false;
            break;
        }
    }
    if (all_bare) {
        hdr << "enum class " << e.name << " {\n";
        for (const auto& c : e.cases) {
            write_indent(hdr, 1);
            hdr << c.name << ",\n";
        }
        hdr << "};\n\n";
        // C++ `enum class` already has operator== built in, so
        // derive(Eq) for a bare enum needs no extra code.
        // derive(Clone) on a bare enum has no method slot to fill
        // (C++ enum class can't host methods) — sema's lookup skips
        // clone for bare enums, so the user just uses the implicit
        // copy on the enum value directly.
        return;
    }
    // Sum type — emit a struct-of-variant. TODO: tighter sum-type lowering.
    //
    // Detect derive(Eq) up front: `std::variant::operator==` requires
    // each alternative to be equality-comparable, so we need to add a
    // defaulted operator== to every case_t too (cheap — it's all POD
    // member compares).
    const bool wants_eq =
        derives_by_target_.contains(e.name) && derives_by_target_.at(e.name).contains("Eq");
    hdr << "struct " << e.name << " {\n";
    for (const auto& c : e.cases) {
        write_indent(hdr, 1);
        hdr << "struct " << c.name << "_t {";
        for (std::size_t i = 0; i < c.payload.size(); ++i) {
            if (i != 0) {
                hdr << " ";
            }
            if (c.payload[i].second) {
                emit_type(hdr, *c.payload[i].second);
            }
            hdr << " " << (c.payload[i].first.empty() ? std::format("_{}", i) : c.payload[i].first)
                << "{}; ";
        }
        if (wants_eq) {
            hdr << "[[nodiscard]] bool operator==(const " << c.name
                << "_t&) const noexcept = default; ";
        }
        hdr << "};\n";
    }
    write_indent(hdr, 1);
    hdr << "std::variant<";
    for (std::size_t i = 0; i < e.cases.size(); ++i) {
        if (i != 0) {
            hdr << ", ";
        }
        hdr << e.cases[i].name << "_t";
    }
    hdr << "> value{};\n";
    // §12.3 derive(Eq): sum-type wrappers need a defaulted operator==
    // too; std::variant compares structurally provided every alternative
    // is comparable, and the defaulted operator delegates accordingly.
    if (auto it = derives_by_target_.find(e.name); it != derives_by_target_.end()) {
        if (it->second.contains("Eq")) {
            write_indent(hdr, 1);
            hdr << "[[nodiscard]] bool operator==(const " << e.name
                << "&) const noexcept = default;\n";
        }
        // §12.3 derive(Clone): same shape as the struct path.
        if (it->second.contains("Clone")) {
            write_indent(hdr, 1);
            hdr << "[[nodiscard]] " << e.name << " clone() const { return *this; }\n";
        }
    }
    hdr << "};\n\n";
}

void CppEmitter::collect_try_hoists(const ast::Expr& e,
                                    std::vector<TryHoist>& out,
                                    const std::vector<TryHoist>* exclude) {
    auto already_hoisted = [&](const ast::Expr* node) {
        if (exclude == nullptr) {
            return false;
        }
        for (const auto& h : *exclude) {
            if (h.node == node) {
                return true;
            }
        }
        return false;
    };
    switch (e.kind) {
    case ast::NodeKind::TryExpr: {
        const auto& tx = static_cast<const ast::TryExpr&>(e);
        if (tx.form == ast::TryExpr::Form::Propagating) {
            // Post-order: inner hoists go first so they're emitted before
            // the outer one consumes their values.
            collect_try_hoists(*tx.inner, out, exclude);
            if (!already_hoisted(&tx)) {
                out.push_back({&tx, std::format("__vstr_t{}", hoist_counter_++)});
            }
        }
        // try? / try! don't propagate, so they don't need a hoist.
        break;
    }
    case ast::NodeKind::CallExpr: {
        const auto& c = static_cast<const ast::CallExpr&>(e);
        collect_try_hoists(*c.callee, out, exclude);
        for (const auto& a : c.args) {
            if (a.value) {
                collect_try_hoists(*a.value, out, exclude);
            }
        }
        break;
    }
    case ast::NodeKind::BinaryExpr: {
        const auto& b = static_cast<const ast::BinaryExpr&>(e);
        collect_try_hoists(*b.lhs, out, exclude);
        // Skip RHS of short-circuit logicals — it might not evaluate.
        if (b.op == ast::BinaryOp::And || b.op == ast::BinaryOp::Or) {
            break;
        }
        collect_try_hoists(*b.rhs, out, exclude);
        break;
    }
    case ast::NodeKind::UnaryExpr:
        collect_try_hoists(*static_cast<const ast::UnaryExpr&>(e).operand, out, exclude);
        break;
    case ast::NodeKind::MemberExpr:
        collect_try_hoists(*static_cast<const ast::MemberExpr&>(e).base, out, exclude);
        break;
    case ast::NodeKind::IndexExpr: {
        const auto& ix = static_cast<const ast::IndexExpr&>(e);
        collect_try_hoists(*ix.base, out, exclude);
        for (const auto& idx : ix.indices) {
            if (idx) {
                collect_try_hoists(*idx, out, exclude);
            }
        }
        break;
    }
    case ast::NodeKind::ParenExpr:
        collect_try_hoists(*static_cast<const ast::ParenExpr&>(e).inner, out, exclude);
        break;
    case ast::NodeKind::AsExpr:
        collect_try_hoists(*static_cast<const ast::AsExpr&>(e).value, out, exclude);
        break;
    case ast::NodeKind::IfExpr: {
        const auto& i = static_cast<const ast::IfExpr&>(e);
        // Walk the unconditionally-evaluated bits: the cond and the
        // let-init. Branches stay un-walked here — each branch's tries
        // would otherwise evaluate eagerly.
        if (i.cond) {
            collect_try_hoists(*i.cond, out, exclude);
        }
        if (i.let_init) {
            collect_try_hoists(*i.let_init, out, exclude);
        }
        // §9 mid-expression `try` inside a branch: when a propagating
        // try sits in either branch (or deeper), hoist the IfExpr
        // itself. emit_cond_hoist lowers it as a lambda returning
        // std::expected<T, E>; the parent scope unwraps via the same
        // canonical 3-line escape used for TryExpr-hoists.
        const bool branches_have_try =
            (i.then_branch != nullptr && expr_contains_propagating_try(*i.then_branch))
            || (i.else_branch != nullptr && expr_contains_propagating_try(*i.else_branch));
        if (branches_have_try && !already_hoisted(&e)) {
            out.push_back({&e, std::format("__vstr_c{}", hoist_counter_++)});
        }
        break;
    }
    case ast::NodeKind::MatchExpr: {
        const auto& m = static_cast<const ast::MatchExpr&>(e);
        // Scrutinee is always evaluated; arm bodies aren't. Walk the
        // scrutinee unconditionally for inner tries, then — if any arm
        // body carries a propagating try — register the whole MatchExpr
        // as a conditional hoist (parallel to the IfExpr case above).
        // emit_cond_hoist lowers the match as a statement-form lambda
        // returning std::expected<T, E>; the outer scope unwraps via
        // the canonical 3-line escape, same as for IfExpr.
        if (m.scrutinee) {
            collect_try_hoists(*m.scrutinee, out, exclude);
        }
        bool arms_have_try = false;
        for (const auto& arm : m.arms) {
            if (arm.guard && expr_contains_propagating_try(*arm.guard)) {
                arms_have_try = true;
                break;
            }
            if (arm.body && expr_contains_propagating_try(*arm.body)) {
                arms_have_try = true;
                break;
            }
        }
        if (arms_have_try && !already_hoisted(&e)) {
            out.push_back({&e, std::format("__vstr_c{}", hoist_counter_++)});
        }
        break;
    }
    case ast::NodeKind::CopyExpr:
        collect_try_hoists(*static_cast<const ast::CopyExpr&>(e).inner, out, exclude);
        break;
    case ast::NodeKind::VectorLitExpr: {
        const auto& v = static_cast<const ast::VectorLitExpr&>(e);
        for (const auto& el : v.elements) {
            if (el) {
                collect_try_hoists(*el, out, exclude);
            }
        }
        break;
    }
    case ast::NodeKind::TupleLitExpr: {
        const auto& tup = static_cast<const ast::TupleLitExpr&>(e);
        for (const auto& el : tup.elements) {
            if (el) {
                collect_try_hoists(*el, out, exclude);
            }
        }
        break;
    }
    case ast::NodeKind::InterpStringExpr: {
        const auto& is_ = static_cast<const ast::InterpStringExpr&>(e);
        for (const auto& seg : is_.segments) {
            if (seg.expr) {
                collect_try_hoists(*seg.expr, out, exclude);
            }
        }
        break;
    }
    // BlockExpr, ClosureExpr, AwaitExpr, SpawnExpr, ThrowExpr — leave
    // alone. BlockExpr has its own statement scope (each inner stmt does
    // its own hoist collection); ThrowExpr's inner is the error value,
    // which doesn't carry try semantics here; the concurrency forms have
    // their own escape semantics and aren't hoisted in v0.5.
    default:
        break;
    }
}

void CppEmitter::collect_stmt_hoists(const ast::Stmt& s, std::vector<TryHoist>& out) {
    switch (s.kind) {
    case ast::NodeKind::LetStmt:
        if (const auto& l = static_cast<const ast::LetStmt&>(s); l.value) {
            collect_try_hoists(*l.value, out);
        }
        break;
    case ast::NodeKind::VarStmt:
        if (const auto& v = static_cast<const ast::VarStmt&>(s); v.value) {
            collect_try_hoists(*v.value, out);
        }
        break;
    case ast::NodeKind::ExprStmt:
        collect_try_hoists(*static_cast<const ast::ExprStmt&>(s).expr, out);
        break;
    case ast::NodeKind::ReturnStmt:
        if (const auto& r = static_cast<const ast::ReturnStmt&>(s); r.value) {
            collect_try_hoists(*r.value, out);
        }
        break;
    case ast::NodeKind::AssignStmt: {
        const auto& a = static_cast<const ast::AssignStmt&>(s);
        collect_try_hoists(*a.target, out);
        collect_try_hoists(*a.value, out);
        break;
    }
    case ast::NodeKind::WhileStmt:
        collect_try_hoists(*static_cast<const ast::WhileStmt&>(s).cond, out);
        break;
    case ast::NodeKind::ForStmt:
        if (const auto& fs = static_cast<const ast::ForStmt&>(s); fs.iter) {
            collect_try_hoists(*fs.iter, out);
        }
        break;
    default:
        break;
    }
}

void CppEmitter::emit_try_hoist(std::ostream& os, const TryHoist& h, int indent) {
    // §9 dispatch by hoisted node kind. TryExpr → canonical 3-line
    // escape; IfExpr → IIFE returning std::expected (a "conditional
    // hoist") via emit_cond_hoist.
    if (h.node->kind == ast::NodeKind::TryExpr) {
        const auto& tx = static_cast<const ast::TryExpr&>(*h.node);
        const std::string r = h.name + "_r";
        write_indent(os, indent);
        os << "auto " << r << " = ";
        emit_expr(os, *tx.inner);
        os << ";\n";
        write_indent(os, indent);
        os << "if (!" << r << ".has_value()) { return std::unexpected{" << r << ".error()}; }\n";
        write_indent(os, indent);
        os << "auto " << h.name << " = *" << r << ";\n";
        return;
    }
    if (h.node->kind == ast::NodeKind::IfExpr || h.node->kind == ast::NodeKind::MatchExpr) {
        emit_cond_hoist(os, h, indent);
        return;
    }
}

void CppEmitter::emit_cond_hoist(std::ostream& os, const TryHoist& h, int indent) {
    // §9 conditional hoist: an IfExpr or MatchExpr whose branches /
    // arms contain a propagating try lifts to a lambda returning
    // std::expected<T, E>. Each branch's local try-hoists fire inside
    // the branch's brace, escaping the lambda via `return
    // std::unexpected{...}`. The outer scope then unwraps the lambda's
    // result and re-propagates on error — same shape as the TryExpr
    // hoist.
    sema::TypePtr value_type = resolution_ != nullptr ? resolution_->type_of(h.node) : nullptr;
    write_indent(os, indent);
    os << "auto " << h.name << " = [&]() -> std::expected<";
    if (value_type != nullptr) {
        emit_sema_type(os, value_type);
    } else {
        os << "auto";
    }
    os << ", ";
    if (current_throws_type_ != nullptr) {
        emit_type(os, *current_throws_type_);
    } else {
        // Defensive: sema should reject `try` outside a throws(E) fn,
        // but the codegen still needs to write something legal here.
        os << "std::monostate";
    }
    os << "> {\n";

    // While emitting the hoisted node's own body, suppress
    // self-substitution — `emit_expr` would otherwise see the hoist for
    // this very node and write `*name` instead of the if/match shape.
    const auto* prev_skip = skip_hoist_;
    skip_hoist_ = h.node;
    if (h.node->kind == ast::NodeKind::IfExpr) {
        write_indent(os, indent + 1);
        emit_stmt_expr(os, *h.node, /*return_value=*/true);
    } else if (h.node->kind == ast::NodeKind::MatchExpr) {
        emit_match_in_lambda(os, static_cast<const ast::MatchExpr&>(*h.node), indent + 1);
    }
    skip_hoist_ = prev_skip;

    write_indent(os, indent);
    os << "}();\n";
    write_indent(os, indent);
    os << "if (!" << h.name << ".has_value()) { return std::unexpected{" << h.name
       << ".error()}; }\n";
}

void CppEmitter::emit_match_in_lambda(std::ostream& os, const ast::MatchExpr& m, int indent) {
    // §9 statement-form match for the conditional hoist's lambda body.
    // Mirrors emit_match's two lowerings — switch for bare enum,
    // std::holds_alternative chain for payloaded enum — but every arm
    // body is emit_stmt_expr'd with return_value=true so a propagating
    // try inside an arm body escapes via `return std::unexpected{...}`
    // from the enclosing cond-hoist lambda. The std::visit-with-
    // constexpr-if shape that emit_match uses can't be reused here
    // because std::visit's lambda is its own return scope: a
    // `return std::unexpected{...}` inside that lambda would return
    // expected<T,E> from the visit's lambda rather than from the outer
    // cond-hoist lambda, breaking propagation. Using
    // std::holds_alternative + std::get instead keeps `return` rooted
    // in the cond-hoist lambda.
    if (resolution_ == nullptr) {
        unsupported(os, "match expression without resolved scrutinee in cond-hoist", m.range);
        return;
    }
    auto scrutinee_type = resolution_->type_of(m.scrutinee.get());
    if (scrutinee_type == nullptr || scrutinee_type->kind() != sema::TypeKind::Enum
        || scrutinee_type->nominal_decl() == nullptr) {
        unsupported(os, "match over non-enum scrutinee in cond-hoist", m.range);
        return;
    }
    const auto& enum_decl = static_cast<const ast::EnumDecl&>(*scrutinee_type->nominal_decl());

    bool has_payload = false;
    for (const auto& c : enum_decl.cases) {
        if (!c.payload.empty()) {
            has_payload = true;
            break;
        }
    }

    if (!has_payload) {
        write_indent(os, indent);
        os << "switch (";
        emit_expr(os, *m.scrutinee);
        os << ") {\n";
        const ast::MatchArm* default_arm = nullptr;
        for (const auto& arm : m.arms) {
            if (arm.is_default
                || (arm.pattern != nullptr && arm.pattern->kind == ast::NodeKind::WildcardPat)) {
                default_arm = &arm;
                continue;
            }
            if (arm.pattern == nullptr || arm.pattern->kind != ast::NodeKind::EnumPat) {
                unsupported(os, "match arm pattern in cond-hoist", m.range);
                continue;
            }
            const auto& ep = static_cast<const ast::EnumPat&>(*arm.pattern);
            write_indent(os, indent);
            os << "case " << enum_decl.name << "::" << ep.case_name << ": {\n";
            write_indent(os, indent + 1);
            emit_stmt_expr(os, *arm.body, /*return_value=*/true);
            write_indent(os, indent);
            os << "}\n";
        }
        write_indent(os, indent);
        os << "default: ";
        if (default_arm != nullptr) {
            os << "{\n";
            write_indent(os, indent + 1);
            emit_stmt_expr(os, *default_arm->body, /*return_value=*/true);
            write_indent(os, indent);
            os << "}\n";
        } else {
            os << "std::unreachable();\n";
        }
        write_indent(os, indent);
        os << "}\n";
        return;
    }

    // Payloaded enum: emit an if-chain over std::holds_alternative so
    // every `return` inside an arm body returns from the enclosing
    // cond-hoist lambda. The payload-field binding mirrors
    // emit_match's std::visit path.
    write_indent(os, indent);
    os << "auto&& __vstr_alt_ref = (";
    emit_expr(os, *m.scrutinee);
    os << ").value;\n";

    bool first = true;
    const ast::MatchArm* default_arm = nullptr;
    for (const auto& arm : m.arms) {
        if (arm.is_default
            || (arm.pattern != nullptr && arm.pattern->kind == ast::NodeKind::WildcardPat)) {
            default_arm = &arm;
            continue;
        }
        if (arm.pattern == nullptr || arm.pattern->kind != ast::NodeKind::EnumPat) {
            unsupported(os, "match arm pattern in cond-hoist over payloaded enum", m.range);
            continue;
        }
        const auto& ep = static_cast<const ast::EnumPat&>(*arm.pattern);

        const ast::EnumDecl::Case* case_decl = nullptr;
        for (const auto& c : enum_decl.cases) {
            if (c.name == ep.case_name) {
                case_decl = &c;
                break;
            }
        }
        if (case_decl == nullptr) {
            unsupported(os, "match arm references unknown enum case", arm.pattern->range);
            continue;
        }

        write_indent(os, indent);
        if (first) {
            os << "if";
        } else {
            os << "else if";
        }
        os << " (std::holds_alternative<" << enum_decl.name << "::" << ep.case_name
           << "_t>(__vstr_alt_ref)) {\n";

        // Bind payload fields. The alt is `std::get<case_t>(__vstr_alt_ref)`;
        // only mint the named handle when there are children to read off it,
        // otherwise `-Werror=unused-variable` rejects the empty-payload arm.
        const bool has_child_binding =
            std::any_of(ep.children.begin(), ep.children.end(), [](const ast::PatternPtr& child) {
                if (!child) {
                    return false;
                }
                switch (child->kind) {
                case ast::NodeKind::BindPat:
                case ast::NodeKind::IdentPat:
                case ast::NodeKind::TuplePat:
                    return true;
                default:
                    return false;
                }
            });
        if (has_child_binding) {
            write_indent(os, indent + 1);
            os << "auto&& __vstr_alt = std::get<" << enum_decl.name << "::" << ep.case_name
               << "_t>(__vstr_alt_ref);\n";
        }

        for (std::size_t i = 0; i < ep.children.size() && i < case_decl->payload.size(); ++i) {
            const auto& child = *ep.children[i];
            std::string field_name = case_decl->payload[i].first.empty()
                                         ? std::format("_{}", i)
                                         : case_decl->payload[i].first;
            if (child.kind == ast::NodeKind::BindPat) {
                write_indent(os, indent + 1);
                os << "auto&& " << static_cast<const ast::BindPat&>(child).name << " = __vstr_alt."
                   << field_name << ";\n";
            } else if (child.kind == ast::NodeKind::IdentPat) {
                write_indent(os, indent + 1);
                os << "auto&& " << static_cast<const ast::IdentPat&>(child).name << " = __vstr_alt."
                   << field_name << ";\n";
            } else if (child.kind == ast::NodeKind::TuplePat) {
                const auto& sub_tp = static_cast<const ast::TuplePat&>(child);
                std::vector<std::string> sub_names;
                std::vector<std::pair<std::string, const ast::TuplePat*>> sub_followons;
                collect_tuple_pat_names(sub_tp, sub_names, sub_followons);
                write_indent(os, indent + 1);
                os << "auto&& [";
                for (std::size_t k = 0; k < sub_names.size(); ++k) {
                    if (k != 0) {
                        os << ", ";
                    }
                    os << sub_names[k];
                }
                os << "] = __vstr_alt." << field_name << ";\n";
                emit_tuple_pat_followons(os, sub_followons, indent + 1);
            }
        }

        write_indent(os, indent + 1);
        emit_stmt_expr(os, *arm.body, /*return_value=*/true);
        write_indent(os, indent);
        os << "}\n";
        first = false;
    }

    write_indent(os, indent);
    if (default_arm != nullptr) {
        os << "else {\n";
        write_indent(os, indent + 1);
        emit_stmt_expr(os, *default_arm->body, /*return_value=*/true);
        write_indent(os, indent);
        os << "}\n";
    } else {
        os << "else { std::unreachable(); }\n";
    }
}

const std::string* CppEmitter::lookup_try_hoist(const ast::Expr* node) const {
    if (active_hoists_ == nullptr) {
        return nullptr;
    }
    if (skip_hoist_ != nullptr && skip_hoist_ == node) {
        return nullptr;
    }
    for (const auto& h : *active_hoists_) {
        if (h.node == node) {
            return &h.name;
        }
    }
    return nullptr;
}

bool CppEmitter::expr_contains_propagating_try(const ast::Expr& e) const {
    switch (e.kind) {
    case ast::NodeKind::TryExpr:
        return static_cast<const ast::TryExpr&>(e).form == ast::TryExpr::Form::Propagating;
    case ast::NodeKind::IfExpr: {
        const auto& i = static_cast<const ast::IfExpr&>(e);
        if (i.cond && expr_contains_propagating_try(*i.cond)) {
            return true;
        }
        if (i.let_init && expr_contains_propagating_try(*i.let_init)) {
            return true;
        }
        if (i.then_branch && expr_contains_propagating_try(*i.then_branch)) {
            return true;
        }
        if (i.else_branch && expr_contains_propagating_try(*i.else_branch)) {
            return true;
        }
        return false;
    }
    case ast::NodeKind::MatchExpr: {
        const auto& m = static_cast<const ast::MatchExpr&>(e);
        if (m.scrutinee && expr_contains_propagating_try(*m.scrutinee)) {
            return true;
        }
        for (const auto& arm : m.arms) {
            if (arm.guard && expr_contains_propagating_try(*arm.guard)) {
                return true;
            }
            if (arm.body && expr_contains_propagating_try(*arm.body)) {
                return true;
            }
        }
        return false;
    }
    case ast::NodeKind::BlockExpr: {
        const auto& b = static_cast<const ast::BlockExpr&>(e);
        for (const auto& s : b.stmts) {
            if (stmt_contains_propagating_try(*s)) {
                return true;
            }
        }
        return false;
    }
    case ast::NodeKind::BinaryExpr: {
        const auto& b = static_cast<const ast::BinaryExpr&>(e);
        return (b.lhs && expr_contains_propagating_try(*b.lhs))
               || (b.rhs && expr_contains_propagating_try(*b.rhs));
    }
    case ast::NodeKind::UnaryExpr:
        return expr_contains_propagating_try(*static_cast<const ast::UnaryExpr&>(e).operand);
    case ast::NodeKind::CallExpr: {
        const auto& c = static_cast<const ast::CallExpr&>(e);
        if (c.callee && expr_contains_propagating_try(*c.callee)) {
            return true;
        }
        for (const auto& a : c.args) {
            if (a.value && expr_contains_propagating_try(*a.value)) {
                return true;
            }
        }
        return false;
    }
    case ast::NodeKind::MemberExpr:
        return expr_contains_propagating_try(*static_cast<const ast::MemberExpr&>(e).base);
    case ast::NodeKind::IndexExpr: {
        const auto& ix = static_cast<const ast::IndexExpr&>(e);
        if (ix.base && expr_contains_propagating_try(*ix.base)) {
            return true;
        }
        for (const auto& idx : ix.indices) {
            if (idx && expr_contains_propagating_try(*idx)) {
                return true;
            }
        }
        return false;
    }
    case ast::NodeKind::ParenExpr:
        return expr_contains_propagating_try(*static_cast<const ast::ParenExpr&>(e).inner);
    case ast::NodeKind::AsExpr:
        return expr_contains_propagating_try(*static_cast<const ast::AsExpr&>(e).value);
    case ast::NodeKind::CopyExpr:
        return expr_contains_propagating_try(*static_cast<const ast::CopyExpr&>(e).inner);
    case ast::NodeKind::VectorLitExpr: {
        const auto& v = static_cast<const ast::VectorLitExpr&>(e);
        for (const auto& el : v.elements) {
            if (el && expr_contains_propagating_try(*el)) {
                return true;
            }
        }
        return false;
    }
    case ast::NodeKind::TupleLitExpr: {
        const auto& tup = static_cast<const ast::TupleLitExpr&>(e);
        for (const auto& el : tup.elements) {
            if (el && expr_contains_propagating_try(*el)) {
                return true;
            }
        }
        return false;
    }
    case ast::NodeKind::InterpStringExpr: {
        const auto& is_ = static_cast<const ast::InterpStringExpr&>(e);
        for (const auto& seg : is_.segments) {
            if (seg.expr && expr_contains_propagating_try(*seg.expr)) {
                return true;
            }
        }
        return false;
    }
    default:
        return false;
    }
}

bool CppEmitter::stmt_contains_propagating_try(const ast::Stmt& s) const {
    switch (s.kind) {
    case ast::NodeKind::LetStmt:
        return static_cast<const ast::LetStmt&>(s).value
               && expr_contains_propagating_try(*static_cast<const ast::LetStmt&>(s).value);
    case ast::NodeKind::VarStmt:
        return static_cast<const ast::VarStmt&>(s).value
               && expr_contains_propagating_try(*static_cast<const ast::VarStmt&>(s).value);
    case ast::NodeKind::ExprStmt:
        return expr_contains_propagating_try(*static_cast<const ast::ExprStmt&>(s).expr);
    case ast::NodeKind::ReturnStmt:
        return static_cast<const ast::ReturnStmt&>(s).value
               && expr_contains_propagating_try(*static_cast<const ast::ReturnStmt&>(s).value);
    case ast::NodeKind::AssignStmt: {
        const auto& a = static_cast<const ast::AssignStmt&>(s);
        return (a.target && expr_contains_propagating_try(*a.target))
               || (a.value && expr_contains_propagating_try(*a.value));
    }
    default:
        return false;
    }
}

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
        // Vestra's `let` is "no reassignment" — but it does allow consumption
        // (move). C++'s `const` is stricter (no rebind AND no move-from), so
        // we use plain `auto` / `T` here. Sema rejects assignment to `let`
        // bindings, which is the part C++'s const was buying us.
        //
        // §9 `let NAME = try EXPR` no longer needs a special case: the
        // try-hoisting pass above pre-emits the propagation escape, then
        // emit_expr substitutes the hoisted name when it hits the
        // TryExpr — so `auto NAME = __vstr_t0;` is correct.
        if (l.type) {
            emit_type(os, *l.type);
            os << " ";
        } else {
            os << "auto ";
        }
        if (l.pattern && l.pattern->kind == ast::NodeKind::IdentPat) {
            os << static_cast<const ast::IdentPat&>(*l.pattern).name;
        } else if (l.pattern && l.pattern->kind == ast::NodeKind::BindPat) {
            os << static_cast<const ast::BindPat&>(*l.pattern).name;
        } else {
            os << "_bind";
        }
        os << " = ";
        if (l.value) {
            emit_expr(os, *l.value);
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
            emit_expr(os, *v.value);
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
            os << "return;\n";
        }
        break;
    }
    case ast::NodeKind::BreakStmt:
        os << "break;\n";
        break;
    case ast::NodeKind::ContinueStmt:
        os << "continue;\n";
        break;
    case ast::NodeKind::AssignStmt: {
        const auto& a = static_cast<const ast::AssignStmt&>(s);
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
        // Iterator protocol: capture iter as a mutable local, drive
        // while-true / .next() / break on nullopt.
        os << "{ auto __vstr_iter = ";
        emit_expr(os, *f.iter);
        os << "; while (true) { auto __vstr_o = __vstr_iter.next(); "
              "if (!__vstr_o.has_value()) { break; } auto "
           << bind_name() << " = std::move(*__vstr_o); ";
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
                os << "auto&& " << b.name << " = ";
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
    switch (expr.kind) {
    case ast::NodeKind::ThrowExpr: {
        const auto& th = static_cast<const ast::ThrowExpr&>(expr);
        os << "return std::unexpected{";
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
            os << "{ auto __vstr_r = ";
            emit_expr(os, *tx.inner);
            os << "; if (!__vstr_r.has_value()) { return std::unexpected{__vstr_r.error()}; } ";
            if (return_value) {
                os << "return *__vstr_r;";
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
            os << "if (auto __vstr_opt = ";
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
        os << "return ";
    }
    emit_expr(os, expr);
    os << ";\n";
    if (!local_hoists.empty()) {
        active_hoists_ = prev_active;
    }
}

void CppEmitter::emit_expr(std::ostream& os, const ast::Expr& e) {
    // §12.1 folded literal short-circuit: when sema's comptime folder
    // computed a constant value for this expression, emit that value
    // directly. Skips the whole structure (e.g. `1 << 8 - 1` becomes
    // `255` in the output) while still giving the C++ compiler an
    // exact-width literal.
    if (resolution_ != nullptr) {
        if (const auto* v = resolution_->folded_value(&e)) {
            os << v->to_cpp_literal();
            return;
        }
    }
    switch (e.kind) {
    case ast::NodeKind::IntLit:
        os << static_cast<const ast::IntLit&>(e).text;
        break;
    case ast::NodeKind::FloatLit:
        os << static_cast<const ast::FloatLit&>(e).text;
        break;
    case ast::NodeKind::StringLit:
        os << "std::string_view(\"" << static_cast<const ast::StringLit&>(e).text << "\")";
        break;
    case ast::NodeKind::InterpStringExpr: {
        // §4 lowering: each literal fragment contributes a `{{...}}`-free
        // chunk to the format string, each splice contributes `{}` plus
        // an argument. std::format does the per-type rendering at
        // runtime; for now we trust Display ≈ std::formatter, which
        // works out of the box for the numeric / Bool / string-like
        // primitives. The result is a freshly-allocated std::string,
        // matching §4's "interpolation is an allocation."
        const auto& is_ = static_cast<const ast::InterpStringExpr&>(e);
        os << "std::format(\"";
        for (const auto& seg : is_.segments) {
            if (seg.expr == nullptr) {
                // Literal fragment: escape `{` and `}` for std::format.
                for (char c : seg.literal) {
                    if (c == '{' || c == '}') {
                        os << c << c;
                    } else {
                        os << c;
                    }
                }
            } else {
                os << "{}";
            }
        }
        os << "\"";
        for (const auto& seg : is_.segments) {
            if (seg.expr != nullptr) {
                os << ", ";
                emit_expr(os, *seg.expr);
            }
        }
        os << ")";
        break;
    }
    case ast::NodeKind::CharLit:
        os << "U'" << static_cast<const ast::CharLit&>(e).text << "'";
        break;
    case ast::NodeKind::BoolLit:
        os << (static_cast<const ast::BoolLit&>(e).value ? "true" : "false");
        break;
    case ast::NodeKind::NilLit:
        // §9 nil → std::nullopt; the surrounding std::optional<T> slot
        // narrows it via the implicit converting ctor.
        os << "std::nullopt";
        break;
    case ast::NodeKind::IdentExpr:
        os << static_cast<const ast::IdentExpr&>(e).name;
        break;
    case ast::NodeKind::SelfExpr:
        os << "(*this)";
        break;
    case ast::NodeKind::ParenExpr:
        os << "(";
        emit_expr(os, *static_cast<const ast::ParenExpr&>(e).inner);
        os << ")";
        break;
    case ast::NodeKind::TupleLitExpr: {
        // §6 `(e1, e2, …)` → `std::tuple{e1, e2, …}`. CTAD picks the
        // element types from the brace-init list, matching the sema
        // TupleType the resolver gave the expression.
        const auto& tup = static_cast<const ast::TupleLitExpr&>(e);
        os << "std::tuple{";
        for (std::size_t i = 0; i < tup.elements.size(); ++i) {
            if (i != 0) {
                os << ", ";
            }
            if (tup.elements[i]) {
                emit_expr(os, *tup.elements[i]);
            }
        }
        os << "}";
        break;
    }
    case ast::NodeKind::UnaryExpr: {
        const auto& u = static_cast<const ast::UnaryExpr&>(e);
        if (u.op == ast::UnaryOp::Unwrap) {
            // §10 force-unwrap: `opt!` lowers to `__vstr::unwrap_opt(opt)`
            // — the shim panics via `__vstr::panic` on `.none` rather
            // than throwing `std::bad_optional_access`. The behavior is
            // identical (process aborts on absent value), but the path
            // routes through the named panic primitive instead of the
            // exception machinery.
            os << "__vstr::unwrap_opt(";
            emit_expr(os, *u.operand);
            os << ")";
            break;
        }
        os << unop_text(u.op);
        emit_expr(os, *u.operand);
        break;
    }
    case ast::NodeKind::TryExpr: {
        // §9 try forms:
        //   * try e   — propagating: the enclosing emit_stmt's hoist
        //               pass has already pre-emitted a let-binding of
        //               the unwrapped value; we output that name here.
        //               If hoisting was skipped (e.g. this try sits
        //               inside a conditional branch the walk refused
        //               to descend into), fall back to .value() so the
        //               code still compiles (panic on error).
        //   * try? e  — converts to Optional<T> via an IIFE.
        //   * try! e  — panics on error (.value()).
        const auto& tx = static_cast<const ast::TryExpr&>(e);
        if (tx.form == ast::TryExpr::Form::Optional) {
            os << "([&]{ auto __vstr_r = ";
            emit_expr(os, *tx.inner);
            os << "; return __vstr_r.has_value() ? std::optional{*__vstr_r} : std::nullopt; }())";
            break;
        }
        // Propagating with a hoist registered → emit the hoisted name.
        if (tx.form == ast::TryExpr::Form::Propagating) {
            if (const auto* name = lookup_try_hoist(&tx)) {
                os << *name;
                break;
            }
        }
        // §10 try! / un-hoisted propagating try both route through the
        // named panic primitive via `__vstr::unwrap_exp`. The latter
        // shouldn't fire in v0.5 (the IfExpr/MatchExpr cond-hoists +
        // branch-local hoists cover every conditional context the walk
        // used to refuse), but the fallback stays for safety.
        os << "__vstr::unwrap_exp(";
        emit_expr(os, *tx.inner);
        os << ")";
        break;
    }
    case ast::NodeKind::ThrowExpr: {
        // §9: `throw e` ends the enclosing `throws(E)` function with the
        // error value. The clean lowering only works at statement / return
        // position; emit_stmt handles those forms. In any other expression
        // slot we fall back to a `std::unreachable()` IIFE so the program
        // still type-checks (the resolver lets `throw` flow into any slot
        // since its static type is Never).
        const auto& th = static_cast<const ast::ThrowExpr&>(e);
        os << "([&]() -> decltype(auto) { (void)(";
        emit_expr(os, *th.inner);
        os << "); std::unreachable(); }())";
        break;
    }
    case ast::NodeKind::DoCatchExpr: {
        // §9 `do { body } catch (NAME: E) { handler }` lowers to a
        // pair of nested IIFEs: the inner one returns std::expected<T,
        // E> so the body's `try` propagates via std::unexpected; the
        // outer dispatches has_value to the body value or to the catch
        // handler with NAME bound to E. The success type T comes from
        // the resolver — it's the do-catch expression's own type.
        const auto& dc = static_cast<const ast::DoCatchExpr&>(e);
        sema::TypePtr result_type = resolution_ != nullptr ? resolution_->type_of(&e) : nullptr;
        os << "([&]{ auto __vstr_do = [&]() -> std::expected<";
        emit_sema_type(os, result_type);
        os << ", ";
        // Annotated form (dc.error_type != null): emit the AST type
        // directly. Bare form: sema inferred E and stashed it on
        // Resolution::do_catch_error_type — render that via the
        // canonical-type emitter.
        if (dc.error_type) {
            emit_type(os, *dc.error_type);
        } else if (resolution_ != nullptr) {
            emit_sema_type(os, resolution_->do_catch_error_type(&dc));
        }
        os << "> { ";
        // The inner lambda is a fresh statement scope, so any mid-
        // expression `try` in the do-body needs its own hoist pass —
        // the outer emit_stmt's hoist pass didn't descend into the
        // do-catch. Without this, `do { (try f()) + (try g()) } …`
        // would fall back to `.value()` panics. The do-body is almost
        // always a BlockExpr whose trailing expression carries the
        // result; collect_try_hoists itself doesn't descend into a
        // BlockExpr (each inner stmt normally gets its own hoist scope
        // via emit_stmt), so we hand-walk the trailing here.
        std::vector<TryHoist> body_hoists;
        if (dc.do_body->kind == ast::NodeKind::BlockExpr) {
            const auto& b = static_cast<const ast::BlockExpr&>(*dc.do_body);
            if (!b.stmts.empty() && b.stmts.back()->kind == ast::NodeKind::ExprStmt) {
                const auto& trailing = static_cast<const ast::ExprStmt&>(*b.stmts.back());
                if (trailing.expr) {
                    collect_try_hoists(*trailing.expr, body_hoists);
                }
            }
        } else {
            collect_try_hoists(*dc.do_body, body_hoists);
        }
        for (const auto& h : body_hoists) {
            emit_try_hoist(os, h, 0);
        }
        const auto* prev_hoists = active_hoists_;
        active_hoists_ = &body_hoists;
        emit_stmt_expr(os, *dc.do_body, /*return_value=*/true);
        active_hoists_ = prev_hoists;
        os << " }(); if (__vstr_do.has_value()) { return *__vstr_do; } [[maybe_unused]] auto "
           << dc.error_name << " = __vstr_do.error(); return ";
        emit_expr(os, *dc.catch_body);
        os << "; }())";
        break;
    }
    case ast::NodeKind::BinaryExpr: {
        const auto& b = static_cast<const ast::BinaryExpr&>(e);
        if (b.op == ast::BinaryOp::Coalesce) {
            // §9 nil-coalescing: `a ?? b` → `(a).value_or(b)`. value_or
            // eagerly evaluates b (vs the ternary form which short-
            // circuits); for the literal/identifier defaults that are
            // the common case this is fine and reads cleanly.
            os << "(";
            emit_expr(os, *b.lhs);
            os << ").value_or(";
            emit_expr(os, *b.rhs);
            os << ")";
            break;
        }
        emit_expr(os, *b.lhs);
        os << " " << binop_text(b.op) << " ";
        emit_expr(os, *b.rhs);
        break;
    }
    case ast::NodeKind::CallExpr: {
        const auto& c = static_cast<const ast::CallExpr&>(e);
        // §10 `Box.new(value)` lowers to `std::make_unique<T>(value)`.
        // The element type T comes from the resolver (the type of the
        // CallExpr itself is Box<T>; we peel that to get T).
        if (c.callee && c.callee->kind == ast::NodeKind::MemberExpr) {
            const auto& mem = static_cast<const ast::MemberExpr&>(*c.callee);
            if (mem.base && mem.base->kind == ast::NodeKind::IdentExpr
                && static_cast<const ast::IdentExpr&>(*mem.base).name == "Box"
                && mem.member == "new" && c.args.size() == 1) {
                os << "std::make_unique<";
                if (resolution_ != nullptr) {
                    auto rt = resolution_->type_of(&e);
                    if (rt != nullptr && rt->kind() == sema::TypeKind::Box) {
                        emit_sema_type(os, rt->inner());
                    } else {
                        emit_sema_type(os, resolution_->type_of(c.args[0].value.get()));
                    }
                } else {
                    os << "auto";
                }
                os << ">(";
                emit_expr(os, *c.args[0].value);
                os << ")";
                break;
            }
        }
        // §9 `result.mapError(f)` lowers to std::expected's
        // `.transform_error(f)` — the closure runs on the error path
        // and produces a new Result<T, E'>. Symmetric with the sema
        // intercept; sema has already verified the closure has the
        // right `(E) -> E'` shape.
        if (c.callee && c.callee->kind == ast::NodeKind::MemberExpr) {
            const auto& mem = static_cast<const ast::MemberExpr&>(*c.callee);
            if (mem.member == "mapError" && c.args.size() == 1 && resolution_ != nullptr) {
                auto base_t = resolution_->type_of(mem.base.get());
                if (base_t != nullptr && base_t->kind() == sema::TypeKind::Result) {
                    os << "(";
                    emit_expr(os, *mem.base);
                    os << ").transform_error(";
                    emit_expr(os, *c.args[0].value);
                    os << ")";
                    break;
                }
            }
        }
        // §10 panic primitives — `panic(msg)` / `abort()` / `unreachable()`
        // intercepted by IdentExpr callee name. The Vestra-side type is
        // `Never`, which is assignable to any slot. The C++ shims at the
        // top of the header return `__vstr::Never`, whose templated
        // `[[noreturn]]` conversion op slots into whatever C++ type the
        // surrounding expression expects.
        if (c.callee && c.callee->kind == ast::NodeKind::IdentExpr) {
            const auto& callee_ident = static_cast<const ast::IdentExpr&>(*c.callee);
            if (callee_ident.name == "panic" && c.args.size() == 1) {
                os << "__vstr::panic(";
                emit_expr(os, *c.args[0].value);
                os << ")";
                break;
            }
            if (callee_ident.name == "abort" && c.args.empty()) {
                os << "__vstr::abort_fn()";
                break;
            }
            if (callee_ident.name == "unreachable" && c.args.empty()) {
                os << "__vstr::unreachable_fn()";
                break;
            }
        }
        // §12.3 derive(Default) construction: `T.default()` lowers to
        // `T{}` (C++ value-init), which zero-initialises every field by
        // virtue of each field's own brace-init in emit_struct. Sema
        // gates this on the target deriving Default and on every field
        // being Default-conformant; the codegen just emits the value.
        if (resolution_ != nullptr && c.callee && c.callee->kind == ast::NodeKind::MemberExpr) {
            const auto& mem = static_cast<const ast::MemberExpr&>(*c.callee);
            if (mem.base != nullptr && mem.member == "default" && c.args.empty()) {
                if (const auto* sym = resolution_->symbol_of(mem.base.get());
                    sym != nullptr && sym->kind == sema::SymbolKind::Struct
                    && sym->decl != nullptr) {
                    const auto& sd = static_cast<const ast::StructDecl&>(*sym->decl);
                    os << sd.name << "{}";
                    break;
                }
            }
        }
        // Payloaded-enum case construction: `Shape.circle(radius: 1.0)` →
        // `Shape{Shape::circle_t{1.0}}`. Bare cases on a payloaded enum
        // come through the MemberExpr path above; here we handle the
        // call form so the args slot into the right `case_t` aggregate.
        if (resolution_ != nullptr && c.callee->kind == ast::NodeKind::MemberExpr) {
            const auto& mem = static_cast<const ast::MemberExpr&>(*c.callee);
            if (mem.base->kind == ast::NodeKind::IdentExpr) {
                if (const auto* sym = resolution_->symbol_of(mem.base.get())) {
                    if (sym->kind == sema::SymbolKind::Enum && sym->decl != nullptr) {
                        const auto& ed = static_cast<const ast::EnumDecl&>(*sym->decl);
                        if (enum_is_sum_type(ed)) {
                            os << ed.name << "{" << ed.name << "::" << mem.member << "_t{";
                            for (std::size_t i = 0; i < c.args.size(); ++i) {
                                if (i != 0) {
                                    os << ", ";
                                }
                                emit_expr(os, *c.args[i].value);
                            }
                            os << "}}";
                            break;
                        }
                    }
                }
            }
        }
        // §17.x conversion-call lowering: `Float64(i)` → `static_cast<double>(i)`.
        // Sema accepts a bare numeric-primitive ident in callee position and
        // produces a one-arg call typed at that primitive; here we map it to
        // a C++ static_cast against the same primitive's C++ name from the
        // shared primitive_map().
        if (c.callee->kind == ast::NodeKind::IdentExpr && c.args.size() == 1) {
            const auto& callee_ident = static_cast<const ast::IdentExpr&>(*c.callee);
            const auto& m = primitive_map();
            if (auto it = m.find(callee_ident.name);
                it != m.end() && it->first != "Bool" && it->first != "Char" && it->first != "Unit"
                && it->first != "String" && it->first != "Str" && it->first != "StrConst") {
                os << "static_cast<" << it->second << ">(";
                emit_expr(os, *c.args[0].value);
                os << ")";
                break;
            }
        }
        // Struct construction: if the callee is a bare identifier
        // resolving to a Struct symbol, lower as a C++ designated-init
        // brace expression. Checking the callee (rather than the call's
        // result type) means a method that *returns* a struct — e.g. a
        // derive(Clone) `.clone() -> Point` — stays lowered as a real
        // call instead of a fresh struct literal.
        if (resolution_ != nullptr && c.callee->kind == ast::NodeKind::IdentExpr) {
            const auto* sym = resolution_->symbol_of(c.callee.get());
            if (sym != nullptr && sym->kind == sema::SymbolKind::Struct && sym->decl != nullptr) {
                const auto& s_decl = static_cast<const ast::StructDecl&>(*sym->decl);
                os << s_decl.name << "{";
                for (std::size_t i = 0; i < c.args.size(); ++i) {
                    if (i != 0) {
                        os << ", ";
                    }
                    if (!c.args[i].label.empty()) {
                        os << "." << c.args[i].label << " = ";
                    }
                    emit_expr(os, *c.args[i].value);
                }
                os << "}";
                break;
            }
            // §3 opaque-type construction: `Q(t)` lowers to a single
            // `static_cast<Q>(t)` over the `enum class Q : T {}`
            // emission. Single positional arg (sema enforces).
            if (sym != nullptr && sym->kind == sema::SymbolKind::OpaqueType && sym->decl != nullptr
                && c.args.size() == 1) {
                const auto& od = static_cast<const ast::OpaqueDecl&>(*sym->decl);
                os << "static_cast<" << od.name << ">(";
                emit_expr(os, *c.args[0].value);
                os << ")";
                break;
            }
        }
        // Recover the callee's parameter modes so we can wrap sink arguments
        // in std::move(). If the callee resolves to a Vestra func declaration,
        // use its params; otherwise treat every param as `read` (the default).
        const ast::FuncDecl* fn = nullptr;
        if (resolution_ != nullptr) {
            const auto* sym = resolution_->symbol_of(c.callee.get());
            if (sym != nullptr && sym->decl != nullptr && sym->decl->kind == ast::NodeKind::Func) {
                fn = static_cast<const ast::FuncDecl*>(sym->decl);
            }
        }
        emit_expr(os, *c.callee);
        os << "(";
        for (std::size_t i = 0; i < c.args.size(); ++i) {
            if (i != 0) {
                os << ", ";
            }
            const bool is_sink = fn != nullptr && i < fn->params.size()
                                 && fn->params[i].mode == ast::ParamMode::Sink;
            // For a sink param, the value must arrive as an rvalue. The
            // simplest correct lowering: always std::move at the call site.
            // (`std::move(temporary)` is harmless — it just stays an rvalue.)
            if (is_sink) {
                os << "std::move(";
            }
            emit_expr(os, *c.args[i].value);
            if (is_sink) {
                os << ")";
            }
        }
        os << ")";
        break;
    }
    case ast::NodeKind::MemberExpr: {
        const auto& m = static_cast<const ast::MemberExpr&>(e);
        // §9 optional chaining: `a?.b` lowers to either
        // std::optional::transform (member yields T) or and_then (member
        // already yields Optional<U>, which we'd otherwise nest). The
        // distinction is whether the field's AST type is OptionalType.
        if (m.is_optional_chain) {
            bool flatten = false;
            if (resolution_ != nullptr) {
                auto bt = resolution_->type_of(m.base.get());
                if (bt != nullptr && bt->kind() == sema::TypeKind::Optional
                    && bt->inner() != nullptr && bt->inner()->kind() == sema::TypeKind::Struct
                    && bt->inner()->nominal_decl() != nullptr) {
                    const auto& sd =
                        static_cast<const ast::StructDecl&>(*bt->inner()->nominal_decl());
                    for (const auto& f : sd.fields) {
                        if (f.name == m.member && f.type != nullptr
                            && f.type->kind == ast::NodeKind::OptionalType) {
                            flatten = true;
                            break;
                        }
                    }
                }
            }
            emit_expr(os, *m.base);
            os << (flatten ? ".and_then" : ".transform");
            os << "([](auto&& __vstr_o) { return __vstr_o." << m.member << "; })";
            break;
        }
        // If the base is an identifier the resolver bound to an Enum decl,
        // this is a static enum case access. Bare enums emit as
        // `Color::red`; payloaded enums lower as struct-of-variant so a
        // bare-case access needs the `Shape{Shape::point_t{}}` wrap.
        if (resolution_ != nullptr && m.base->kind == ast::NodeKind::IdentExpr) {
            const auto* base_sym = resolution_->symbol_of(m.base.get());
            if (base_sym != nullptr && base_sym->kind == sema::SymbolKind::Enum
                && base_sym->decl != nullptr) {
                const auto& enum_decl = static_cast<const ast::EnumDecl&>(*base_sym->decl);
                if (enum_is_sum_type(enum_decl)) {
                    os << enum_decl.name << "{" << enum_decl.name << "::" << m.member << "_t{}}";
                } else {
                    os << enum_decl.name << "::" << m.member;
                }
                break;
            }
        }
        // §3 opaque newtype: `q.value` extracts the underlying T via a
        // single `static_cast` over the `enum class Q : T {}` shape.
        // §10 Box[T]: `box.value` reads the heap-pointee via `*box`.
        if (resolution_ != nullptr && m.member == "value") {
            auto bt = resolution_->type_of(m.base.get());
            if (bt != nullptr && bt->kind() == sema::TypeKind::OpaqueType
                && bt->nominal_decl() != nullptr) {
                const auto& od = static_cast<const ast::OpaqueDecl&>(*bt->nominal_decl());
                os << "static_cast<";
                if (od.underlying) {
                    emit_type(os, *od.underlying);
                }
                os << ">(";
                emit_expr(os, *m.base);
                os << ")";
                break;
            }
            if (bt != nullptr && bt->kind() == sema::TypeKind::Box) {
                os << "(*";
                emit_expr(os, *m.base);
                os << ")";
                break;
            }
        }
        // §10 Span[T] / MutSpan[T]: `.count` and `.isEmpty` map to
        // `std::span::size()` (cast to the signed `Int`-equivalent so
        // the result is comparable to ordinary signed indices without
        // tripping -Wsign-compare) and `.empty()`.
        if (resolution_ != nullptr && (m.member == "count" || m.member == "isEmpty")) {
            auto bt = resolution_->type_of(m.base.get());
            if (bt != nullptr
                && (bt->kind() == sema::TypeKind::Span || bt->kind() == sema::TypeKind::MutSpan)) {
                if (m.member == "count") {
                    os << "static_cast<std::intptr_t>(";
                    emit_expr(os, *m.base);
                    os << ".size())";
                } else {
                    emit_expr(os, *m.base);
                    os << ".empty()";
                }
                break;
            }
        }
        emit_expr(os, *m.base);
        os << "." << m.member;
        break;
    }
    case ast::NodeKind::IndexExpr: {
        // C++ subscript is the same shape as Vestra's: `base[i0, i1, ...]`.
        // The Vestra-side type-check already verified the base is indexable
        // (a vector / array / slice); we just pass through.
        // §10 Span/MutSpan use std::span::operator[] which takes
        // std::size_t — cast the (signed) Int index to keep
        // -Wsign-conversion quiet.
        const auto& ix = static_cast<const ast::IndexExpr&>(e);
        bool span_base = false;
        if (resolution_ != nullptr && ix.base != nullptr) {
            auto bt = resolution_->type_of(ix.base.get());
            span_base =
                bt != nullptr
                && (bt->kind() == sema::TypeKind::Span || bt->kind() == sema::TypeKind::MutSpan);
        }
        emit_expr(os, *ix.base);
        os << "[";
        for (std::size_t i = 0; i < ix.indices.size(); ++i) {
            if (i != 0) {
                os << ", ";
            }
            if (span_base) {
                os << "static_cast<std::size_t>(";
                emit_expr(os, *ix.indices[i]);
                os << ")";
            } else {
                emit_expr(os, *ix.indices[i]);
            }
        }
        os << "]";
        break;
    }
    case ast::NodeKind::LeadingDotExpr: {
        const auto& d = static_cast<const ast::LeadingDotExpr&>(e);
        // `.red` — resolved by the sema layer; its expression-type is the
        // enum. We need the enum's name to emit `Enum::red`.
        if (resolution_ != nullptr) {
            auto t = resolution_->type_of(&e);
            if (t != nullptr && t->kind() == sema::TypeKind::Enum && t->nominal_decl() != nullptr) {
                const auto& enum_decl = static_cast<const ast::EnumDecl&>(*t->nominal_decl());
                os << enum_decl.name << "::" << d.name;
                break;
            }
        }
        unsupported(os, "leading-dot expression without resolved context", e.range);
        break;
    }
    case ast::NodeKind::MatchExpr:
        // §9 conditional hoist: when the parent statement registered
        // this MatchExpr as a hoist (because a propagating try lives
        // in an arm body), the lambda was pre-emitted and bound to a
        // name — substitute `*name` here so propagation already
        // happened. Otherwise fall through to the regular IIFE.
        if (auto* name = lookup_try_hoist(&e)) {
            os << "(*" << *name << ")";
        } else {
            emit_match(os, static_cast<const ast::MatchExpr&>(e));
        }
        break;
    case ast::NodeKind::IfExpr: {
        const auto& i = static_cast<const ast::IfExpr&>(e);
        // §9 conditional hoist: when the parent statement registered
        // this IfExpr as a hoist (because a propagating try lives in a
        // branch), the IIFE was pre-emitted and bound to a name —
        // substitute `*name` here so propagation already happened.
        if (auto* name = lookup_try_hoist(&e)) {
            os << "(*" << *name << ")";
            break;
        }
        // We can't always lower if-expressions; this works for a statement
        // context but produces invalid code when used in expression position.
        // A future pass should hoist into a temporary + statement.
        if (!i.let_name.empty()) {
            // §9 `if let NAME = INIT { THEN } else { ELSE }` lowers to a
            // C++23 if-with-initializer over the std::optional, binding
            // the unwrapped value as a reference for THEN.
            os << "([&]{ if (auto __vstr_opt = ";
            emit_expr(os, *i.let_init);
            os << "; __vstr_opt.has_value()) { auto&& " << i.let_name << " = *__vstr_opt; return ";
            emit_expr(os, *i.then_branch);
            os << "; } else { return ";
            if (i.else_branch) {
                emit_expr(os, *i.else_branch);
            } else {
                os << "decltype(";
                emit_expr(os, *i.then_branch);
                os << "){}";
            }
            os << "; } }())";
            break;
        }
        os << "([&]{ if (";
        emit_expr(os, *i.cond);
        os << ") { return ";
        emit_expr(os, *i.then_branch);
        os << "; } else { return ";
        if (i.else_branch) {
            emit_expr(os, *i.else_branch);
        } else {
            os << "decltype(";
            emit_expr(os, *i.then_branch);
            os << "){}";
        }
        os << "; } }())";
        break;
    }
    case ast::NodeKind::BlockExpr: {
        // Treat a block expression as an IIFE returning its last statement.
        const auto& b = static_cast<const ast::BlockExpr&>(e);
        os << "[&]{ ";
        for (std::size_t i = 0; i < b.stmts.size(); ++i) {
            const auto& s = *b.stmts[i];
            bool last = (i + 1 == b.stmts.size());
            if (last && s.kind == ast::NodeKind::ExprStmt) {
                os << "return ";
                emit_expr(os, *static_cast<const ast::ExprStmt&>(s).expr);
                os << ";";
            } else {
                std::ostringstream tmp;
                emit_stmt(tmp, s, 0);
                os << tmp.str();
            }
        }
        os << " }()";
        break;
    }
    default:
        unsupported(os, "expression", e.range);
        break;
    }
}

void CppEmitter::emit_sema_type(std::ostream& os, sema::TypePtr t) {
    using namespace sema;
    if (t == nullptr) {
        os << "/*?*/";
        return;
    }
    switch (t->kind()) {
    case TypeKind::Int8:
        os << "std::int8_t";
        return;
    case TypeKind::Int16:
        os << "std::int16_t";
        return;
    case TypeKind::Int32:
        os << "std::int32_t";
        return;
    case TypeKind::Int64:
        os << "std::int64_t";
        return;
    case TypeKind::Int:
        os << "std::intptr_t";
        return;
    case TypeKind::UInt8:
        os << "std::uint8_t";
        return;
    case TypeKind::UInt16:
        os << "std::uint16_t";
        return;
    case TypeKind::UInt32:
        os << "std::uint32_t";
        return;
    case TypeKind::UInt64:
        os << "std::uint64_t";
        return;
    case TypeKind::UInt:
        os << "std::uintptr_t";
        return;
    case TypeKind::Float32:
        os << "float";
        return;
    case TypeKind::Float64:
        os << "double";
        return;
    case TypeKind::Bool:
        os << "bool";
        return;
    case TypeKind::Char:
        os << "char32_t";
        return;
    case TypeKind::Unit:
        os << "void";
        return;
    case TypeKind::String:
        os << "std::string";
        return;
    case TypeKind::Str:
    case TypeKind::StrConst:
        os << "std::string_view";
        return;
    case TypeKind::Optional:
        os << "std::optional<";
        emit_sema_type(os, t->inner());
        os << ">";
        return;
    case TypeKind::Box:
        os << "std::unique_ptr<";
        emit_sema_type(os, t->inner());
        os << ">";
        return;
    case TypeKind::Span:
        // Read-only borrowed view → `std::span<const T>`. The const on
        // the element pins the read-only side of the type system at
        // the C++ layer; an assignment-through-Span fails to compile.
        os << "std::span<const ";
        emit_sema_type(os, t->inner());
        os << ">";
        return;
    case TypeKind::MutSpan:
        // Mutable borrowed view → `std::span<T>` (no const).
        os << "std::span<";
        emit_sema_type(os, t->inner());
        os << ">";
        return;
    case TypeKind::Result:
        os << "std::expected<";
        emit_sema_type(os, t->inner());
        os << ", ";
        emit_sema_type(os, t->result());
        os << ">";
        return;
    case TypeKind::Vector:
        os << "std::array<";
        emit_sema_type(os, t->inner());
        os << ", " << t->vector_length() << ">";
        return;
    case TypeKind::Tuple:
        os << "std::tuple<";
        for (std::size_t i = 0; i < t->parts().size(); ++i) {
            if (i != 0) {
                os << ", ";
            }
            emit_sema_type(os, t->parts()[i]);
        }
        os << ">";
        return;
    case TypeKind::Struct:
    case TypeKind::Enum:
    case TypeKind::Protocol:
    case TypeKind::OpaqueType:
        if (const auto* decl = t->nominal_decl()) {
            switch (decl->kind) {
            case ast::NodeKind::Struct:
                os << static_cast<const ast::StructDecl&>(*decl).name;
                return;
            case ast::NodeKind::Enum:
                os << static_cast<const ast::EnumDecl&>(*decl).name;
                return;
            case ast::NodeKind::Protocol:
                os << static_cast<const ast::ProtocolDecl&>(*decl).name;
                return;
            case ast::NodeKind::Opaque:
                os << static_cast<const ast::OpaqueDecl&>(*decl).name;
                return;
            default:
                break;
            }
        }
        os << "/*nominal*/";
        return;
    case TypeKind::GenericParam:
        os << t->generic_name();
        return;
    default:
        os << "/*type*/";
        return;
    }
}

void CppEmitter::emit_type(std::ostream& os, const ast::Type& t) {
    switch (t.kind) {
    case ast::NodeKind::NamedType: {
        const auto& n = static_cast<const ast::NamedType&>(t);
        if (n.path.size() == 1) {
            // §10 `Box[T]` lowers to std::unique_ptr<T>.
            if (n.path[0] == "Box" && n.type_args.size() == 1) {
                os << "std::unique_ptr<";
                emit_type(os, *n.type_args[0]);
                os << ">";
                return;
            }
            // §10 borrowed views: `Span[T]` → `std::span<const T>`,
            // `MutSpan[T]` → `std::span<T>`.
            if (n.path[0] == "Span" && n.type_args.size() == 1) {
                os << "std::span<const ";
                emit_type(os, *n.type_args[0]);
                os << ">";
                return;
            }
            if (n.path[0] == "MutSpan" && n.type_args.size() == 1) {
                os << "std::span<";
                emit_type(os, *n.type_args[0]);
                os << ">";
                return;
            }
            const auto& m = primitive_map();
            auto it = m.find(n.path[0]);
            if (it != m.end()) {
                os << it->second;
                return;
            }
        }
        for (std::size_t i = 0; i < n.path.size(); ++i) {
            if (i != 0) {
                os << "::";
            }
            os << n.path[i];
        }
        if (n.has_generics) {
            os << "<";
            bool first = true;
            for (const auto& ta : n.type_args) {
                if (!first) {
                    os << ", ";
                }
                first = false;
                emit_type(os, *ta);
            }
            for (auto ci : n.const_args) {
                if (!first) {
                    os << ", ";
                }
                first = false;
                os << ci;
            }
            os << ">";
        }
        break;
    }
    case ast::NodeKind::OptionalType:
        os << "std::optional<";
        emit_type(os, *static_cast<const ast::OptionalType&>(t).inner);
        os << ">";
        break;
    case ast::NodeKind::VectorType: {
        const auto& v = static_cast<const ast::VectorType&>(t);
        os << "std::array<";
        if (v.element) {
            emit_type(os, *v.element);
        } else {
            os << "void";
        }
        os << ", " << v.length << ">";
        break;
    }
    case ast::NodeKind::TupleType: {
        // §6 tuple type → std::tuple<T1, T2, …>.
        const auto& tup = static_cast<const ast::TupleType&>(t);
        os << "std::tuple<";
        for (std::size_t i = 0; i < tup.elements.size(); ++i) {
            if (i != 0) {
                os << ", ";
            }
            if (tup.elements[i]) {
                emit_type(os, *tup.elements[i]);
            }
        }
        os << ">";
        break;
    }
    default:
        os << "/*type*/auto";
        break;
    }
}

// ============================================================================
// match — bare enums lower to a switch over the matching enum class.
// ============================================================================

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
        // Lower the whole expression as an IIFE returning a switch's value
        // through a tail `return`. Wildcard patterns map to `default:`;
        // `case .a | .b:` stacks both labels above one return statement.
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

    bool first = true;
    const ast::MatchArm* default_arm = nullptr;

    for (const auto& arm : m.arms) {
        if (arm.is_default
            || (arm.pattern != nullptr && arm.pattern->kind == ast::NodeKind::WildcardPat)) {
            default_arm = &arm;
            continue;
        }
        if (arm.pattern == nullptr || arm.pattern->kind != ast::NodeKind::EnumPat) {
            unsupported(os, "match arm pattern over payloaded enum", m.range);
            continue;
        }
        const auto& ep = static_cast<const ast::EnumPat&>(*arm.pattern);

        // Locate the case decl so we can resolve positional vs labeled
        // payload field names (parallels emit_enum's `_{i}` fallback).
        const ast::EnumDecl::Case* case_decl = nullptr;
        for (const auto& c : enum_decl.cases) {
            if (c.name == ep.case_name) {
                case_decl = &c;
                break;
            }
        }
        if (case_decl == nullptr) {
            unsupported(os, "match arm references unknown enum case", arm.pattern->range);
            continue;
        }

        if (first) {
            write_indent(os, 2);
            os << "if";
        } else {
            os << " else if";  // chained — previous arm ended with bare "}"
        }
        os << " constexpr (std::is_same_v<__vstr_alt_t, " << enum_decl.name << "::" << ep.case_name
           << "_t>) {\n";

        for (std::size_t i = 0; i < ep.children.size() && i < case_decl->payload.size(); ++i) {
            const auto& child = *ep.children[i];
            std::string field_name = case_decl->payload[i].first.empty()
                                         ? std::format("_{}", i)
                                         : case_decl->payload[i].first;
            if (child.kind == ast::NodeKind::BindPat) {
                write_indent(os, 3);
                os << "auto&& " << static_cast<const ast::BindPat&>(child).name << " = __vstr_alt."
                   << field_name << ";\n";
            } else if (child.kind == ast::NodeKind::IdentPat) {
                write_indent(os, 3);
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
                write_indent(os, 3);
                os << "auto&& [";
                for (std::size_t k = 0; k < sub_names.size(); ++k) {
                    if (k != 0) {
                        os << ", ";
                    }
                    os << sub_names[k];
                }
                os << "] = __vstr_alt." << field_name << ";\n";
                emit_tuple_pat_followons(os, sub_followons, 3);
            }
            // Wildcard: nothing to bind.
        }

        write_indent(os, 3);
        os << "return ";
        emit_expr(os, *arm.body);
        os << ";\n";
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
