// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "vestra/sema/comptime.hpp"

#include "vestra/ast/clone.hpp"
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
    case TypeKind::Int128:
    case TypeKind::UInt128:
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
    case TypeKind::UInt128:
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
    case Kind::Field:
        // Reflection Field values are comptime-only — there's no runtime
        // Field type in the emitted C++ today. Surface the name in a
        // comment so a stray escape is at least debuggable, and emit
        // a placeholder zero that won't compile under most contexts
        // (which is the right outcome: Field shouldn't leak past fold).
        return "/* reflect::Field name=\"" + s + "\" */ 0";
    case Kind::TypeRef:
        // Same comptime-only story as Field — TypeRef has no runtime
        // C++ shape today. Comment includes the type's display name.
        return "/* reflect::Type name=\"" + s + "\" */ 0";
    case Kind::Unit:
        return "/* unit */ 0";
    case Kind::Code:
        // §12.4 Code values are AST, not data — they're materialized back into
        // the program via ast::clone, never rendered as a C++ literal. Reaching
        // here means a Code value leaked past macro expansion.
        return "/* ast::Code */ 0";
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

// §12.4 wrap an owned AST node as a Code value (the result of materializing a
// quote-body item: a freshly cloned, substituted subtree).
ComptimeValue make_code(std::shared_ptr<const ast::Node> node, TypeKind kind) {
    ComptimeValue cv;
    cv.kind = ComptimeValue::Kind::Code;
    cv.code = std::move(node);
    cv.type = kind;
    return cv;
}

// §12.4 the source name of an annotatable declaration, for `$(d.name)`.
std::string_view decl_name_of(const ast::Node& n) {
    switch (n.kind) {
    case ast::NodeKind::Struct:
        return static_cast<const ast::StructDecl&>(n).name;
    case ast::NodeKind::Func:
        return static_cast<const ast::FuncDecl&>(n).name;
    case ast::NodeKind::Enum:
        return static_cast<const ast::EnumDecl&>(n).name;
    default:
        return {};
    }
}

// §12.4 the attribute list of an annotatable decl (struct / func / enum), for
// attribute reflection (`d.hasAttribute`, `d.attribute`) and for stripping the
// macro's own attribute off a `$d`-spliced clone. nullptr for other kinds.
const std::vector<ast::Attribute>* decl_attrs_of(const ast::Node& n) {
    switch (n.kind) {
    case ast::NodeKind::Struct:
        return &static_cast<const ast::StructDecl&>(n).attributes;
    case ast::NodeKind::Func:
        return &static_cast<const ast::FuncDecl&>(n).attributes;
    case ast::NodeKind::Enum:
        return &static_cast<const ast::EnumDecl&>(n).attributes;
    default:
        return nullptr;
    }
}

std::vector<ast::Attribute>* decl_attrs_mut(ast::Node& n) {
    return const_cast<std::vector<ast::Attribute>*>(decl_attrs_of(n));
}

const ast::Attribute* find_decl_attribute(const ast::Node& n, std::string_view name) {
    const auto* attrs = decl_attrs_of(n);
    if (attrs == nullptr) {
        return nullptr;
    }
    for (const auto& a : *attrs) {
        if (a.name == name) {
            return &a;
        }
    }
    return nullptr;
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
    if (element_kind == TypeKind::String || element_kind == TypeKind::Str
        || element_kind == TypeKind::StrConst) {
        return make_string({});  // empty string for `.zero` of [N]Str etc.
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

// Display name for an ast::Type (used by §12.2 reflection so
// `Field.type.name` returns a sensible string). Handles the shapes
// reflection users hit in practice — NamedType for primitives and
// nominals, VectorType / OptionalType / TupleType for composites.
// Falls back to "?" on unrecognized shapes; future phases can grow
// this into a proper type-printer.
std::string type_display_name(const ast::Type& t) {
    switch (t.kind) {
    case ast::NodeKind::NamedType: {
        const auto& n = static_cast<const ast::NamedType&>(t);
        std::string out;
        for (std::size_t i = 0; i < n.path.size(); ++i) {
            if (i != 0) {
                out += '.';
            }
            out += n.path[i];
        }
        return out;
    }
    case ast::NodeKind::VectorType: {
        const auto& v = static_cast<const ast::VectorType&>(t);
        std::string out = "[";
        if (v.length != 0) {
            out += std::to_string(v.length);
        } else if (!v.length_ident.empty()) {
            out += v.length_ident;
        }
        out += "]";
        if (v.element) {
            out += type_display_name(*v.element);
        }
        return out;
    }
    case ast::NodeKind::OptionalType: {
        const auto& o = static_cast<const ast::OptionalType&>(t);
        return (o.inner ? type_display_name(*o.inner) : std::string{"?"}) + "?";
    }
    case ast::NodeKind::TupleType: {
        const auto& tt = static_cast<const ast::TupleType&>(t);
        std::string out = "(";
        for (std::size_t i = 0; i < tt.elements.size(); ++i) {
            if (i != 0) {
                out += ", ";
            }
            if (tt.elements[i]) {
                out += type_display_name(*tt.elements[i]);
            }
        }
        out += ")";
        return out;
    }
    default:
        return "?";
    }
}

// §A2 (§6.8) primitive size/alignment tables. The kernel uses these
// for layout assertions (`comptime { assert(T.size == 64) }`); they
// need to match the LP64 / AArch64 / x86_64 platform ABI the
// transpiler targets. Returns {0,0} on shapes we don't size yet —
// the caller treats that as "not foldable" so the assertion fails
// loudly rather than silently picking a wrong size.
struct PrimLayout {
    std::int64_t size = 0;
    std::int64_t align = 0;
};
PrimLayout primitive_layout(TypeKind k) {
    switch (k) {
    case TypeKind::Bool:
    case TypeKind::UInt8:
    case TypeKind::Int8:
        return {1, 1};
    case TypeKind::UInt16:
    case TypeKind::Int16:
        return {2, 2};
    case TypeKind::UInt32:
    case TypeKind::Int32:
    case TypeKind::Float32:
    case TypeKind::Char:
        return {4, 4};
    case TypeKind::UInt64:
    case TypeKind::Int64:
    case TypeKind::UInt:
    case TypeKind::Int:
    case TypeKind::Float64:
        return {8, 8};
    case TypeKind::UInt128:
    case TypeKind::Int128:
        // GCC / Clang both align __int128 to 16 on the targets v0.5
        // builds for; sizeof is 16. This is the de-facto layout
        // every libatomic / libstdc++ build assumes.
        return {16, 16};
    case TypeKind::Str:
    case TypeKind::StrConst:
        // Str / StrConst lower to std::string_view. The C++ Standard
        // doesn't pin its layout, but every libstdc++/libc++ build the
        // transpiler targets uses two pointer-width words (data
        // pointer + size), so 16-byte size + 8-byte align on the
        // 64-bit hosts. Pin that explicitly here; if a target ever
        // diverges, the static_assert in the e2e fires.
        return {16, 8};
    default:
        return {0, 0};
    }
}

std::int64_t align_up(std::int64_t offset, std::int64_t alignment) {
    if (alignment <= 0) {
        return offset;
    }
    return (offset + alignment - 1) / alignment * alignment;
}

// Forward declaration — struct layout walks back to type_layout for
// each field, and type_layout dispatches into struct_layout when it
// hits a nominal Struct.
PrimLayout type_layout(const ast::Type& t, const Scope& globals);

PrimLayout struct_layout(const ast::StructDecl& sd, const Scope& globals) {
    PrimLayout out;
    for (const auto& f : sd.fields) {
        if (f.kind == ast::StructDecl::Field::Kind::Embed || f.type == nullptr) {
            continue;
        }
        auto fl = type_layout(*f.type, globals);
        if (fl.size == 0 || fl.align == 0) {
            return {0, 0};  // bail: can't size some field
        }
        out.size = align_up(out.size, fl.align) + fl.size;
        if (fl.align > out.align) {
            out.align = fl.align;
        }
    }
    if (out.align > 0) {
        out.size = align_up(out.size, out.align);
    }
    return out;
}

// Per-field layout sentinel. `offset` is where the field starts in
// the struct (after C-style padding to the field's own alignment);
// `size`/`align` are the field's own type metrics. Returned in
// declaration order, one entry per *non-embed* field. An empty
// vector means we couldn't size some field — the caller treats it
// the same as struct_layout's {0, 0} bail.
struct FieldLayout {
    std::int64_t offset = 0;
    std::int64_t size = 0;
    std::int64_t align = 0;
};
std::vector<FieldLayout> struct_field_layouts(const ast::StructDecl& sd, const Scope& globals) {
    std::vector<FieldLayout> out;
    std::int64_t running = 0;
    for (const auto& f : sd.fields) {
        if (f.kind == ast::StructDecl::Field::Kind::Embed || f.type == nullptr) {
            continue;
        }
        auto fl = type_layout(*f.type, globals);
        if (fl.size == 0 || fl.align == 0) {
            return {};
        }
        std::int64_t offset = align_up(running, fl.align);
        out.push_back({offset, fl.size, fl.align});
        running = offset + fl.size;
    }
    return out;
}

PrimLayout type_layout(const ast::Type& t, const Scope& globals) {
    if (t.kind == ast::NodeKind::NamedType) {
        const auto& n = static_cast<const ast::NamedType&>(t);
        if (n.path.empty()) {
            return {0, 0};
        }
        const auto& name = n.path.back();
        // Primitive shorthand: NamedType{path=["Int32"]} → primitive.
        auto prim = TypeArena::primitive_kind_by_name(name);
        if (prim != TypeKind::Error) {
            return primitive_layout(prim);
        }
        // Nominal: look up in scope. Today only Struct gets a layout
        // walk — Enum/Opaque/Protocol layouts are out of scope for
        // v0.5 (enums are tag-only or sum-typed in C++ std::variant,
        // and that layout depends on the host compiler).
        if (const auto* sym = globals.lookup(name);
            sym != nullptr && sym->kind == SymbolKind::Struct && sym->decl != nullptr) {
            return struct_layout(static_cast<const ast::StructDecl&>(*sym->decl), globals);
        }
        return {0, 0};
    }
    if (t.kind == ast::NodeKind::VectorType) {
        const auto& v = static_cast<const ast::VectorType&>(t);
        if (v.element == nullptr || v.length <= 0) {
            return {0, 0};
        }
        auto elem = type_layout(*v.element, globals);
        if (elem.size == 0 || elem.align == 0) {
            return {0, 0};
        }
        return {v.length * elem.size, elem.align};
    }
    return {0, 0};
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

// §12.2 / §12.4 build the reflection Vector for a struct's fields: one
// Kind::Field value per non-embed field, laid out positionally to match the
// synthetic `Field` StructDecl — elements[0]=name (String), [1]=type (TypeRef),
// [2]=offset, [3]=size, [4]=alignment (Int). `globals` supplies the §A2 layout
// walk when available (the resolver path); when it's nullptr (the pre-resolution
// declaration-macro path) the three integer slots stay 0 — callers still get
// name/type, which is what field iteration needs.
ComptimeValue build_fields_value(const ast::StructDecl& sd, const Scope* globals) {
    std::vector<FieldLayout> layouts;
    if (globals != nullptr) {
        layouts = struct_field_layouts(sd, *globals);
    }
    const bool has_layout = !layouts.empty();
    ComptimeValue v;
    v.kind = ComptimeValue::Kind::Vector;
    v.type = TypeKind::Struct;  // element kind is Field (nominal struct)
    std::size_t lay_idx = 0;
    for (const auto& f : sd.fields) {
        if (f.kind == ast::StructDecl::Field::Kind::Embed) {
            continue;
        }
        ComptimeValue field;
        field.kind = ComptimeValue::Kind::Field;
        field.type = TypeKind::Struct;
        field.s = f.name;
        field.elements.push_back(make_string(f.name));
        ComptimeValue type_ref;
        type_ref.kind = ComptimeValue::Kind::TypeRef;
        type_ref.type = TypeKind::Struct;
        type_ref.s = f.type ? type_display_name(*f.type) : "?";
        field.elements.push_back(std::move(type_ref));
        FieldLayout lay{};
        if (has_layout && lay_idx < layouts.size()) {
            lay = layouts[lay_idx];
        }
        field.elements.push_back(make_int(lay.offset, TypeKind::Int));
        field.elements.push_back(make_int(lay.size, TypeKind::Int));
        field.elements.push_back(make_int(lay.align, TypeKind::Int));
        ++lay_idx;
        v.elements.push_back(std::move(field));
    }
    v.length = static_cast<std::int64_t>(v.elements.size());
    return v;
}

// Positional index of a well-known `Field` reflection member, matching the
// layout build_fields_value emits. Used to dispatch `field.name` / `field.type`
// etc. without a global scope (the declaration-macro folder has none).
std::optional<std::size_t> field_member_index(std::string_view member) {
    if (member == "name") {
        return 0;
    }
    if (member == "type") {
        return 1;
    }
    if (member == "offset") {
        return 2;
    }
    if (member == "size") {
        return 3;
    }
    if (member == "alignment") {
        return 4;
    }
    return std::nullopt;
}

// §12.4 materialize a folded value into a type, for `$(d.name)` / `$(f.type)`
// in type position. String and TypeRef both carry the type name in `.s`. Only
// a simple identifier is supported in this slice (a dotted path or a compound
// display like `[8]Int32` / `T?` can't be reconstructed as one NamedType);
// anything else returns nullptr so the macro fails cleanly rather than emitting
// a malformed type.
ast::TypePtr materialize_type(const ComptimeValue& v) {
    if (v.kind != ComptimeValue::Kind::String && v.kind != ComptimeValue::Kind::TypeRef) {
        return nullptr;
    }
    const std::string& name = v.s;
    if (name.empty()) {
        return nullptr;
    }
    const auto ident_char = [](char c, bool first) {
        const bool alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
        const bool digit = c >= '0' && c <= '9';
        return first ? alpha : (alpha || digit);
    };
    for (std::size_t k = 0; k < name.size(); ++k) {
        if (!ident_char(name[k], k == 0)) {
            return nullptr;
        }
    }
    auto nt = std::make_unique<ast::NamedType>();
    nt->path.push_back(name);
    return nt;
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

    case ast::NodeKind::StringLit:
        // String literals fold to their raw inner text. Escape processing
        // stays the codegen's responsibility (it preserves backslashes
        // when emitting `std::string_view("…")`); for fold-time we just
        // need bytes to compose into vectors / reflection outputs.
        return make_string(std::string{static_cast<const ast::StringLit&>(e).text});

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
        // Walking arbitrary struct field reads waits until we model struct
        // *values* at fold time (we have struct *types* but no values
        // yet). What we *do* fold:
        //  - `cfg.X` (§12.6 build-time host detection)
        //  - `StructName.fields` (§12.2 reflection phase 1) — vector of
        //    field-name strings, sourced from the StructDecl in scope.
        const auto& m = static_cast<const ast::MemberExpr&>(e);
        // §12.4 reflection on a `Decl` value (a declaration macro's parameter,
        // bound to a Code value aliasing the annotated decl). `d.name` folds to
        // the declaration's source name as a String; `d.fields` (for a struct)
        // folds to the same field Vector as `StructName.fields`, sourced from
        // the bound StructDecl directly (no global scope at expansion time, so
        // the layout slots are 0 — name/type carry the iteration). An unknown
        // member falls through to nullopt.
        if (auto bv = fold_with(*m.base, env, frame, TypeKind::Unit, depth);
            bv && bv->kind == ComptimeValue::Kind::Code && bv->code) {
            if (m.member == "name") {
                return make_string(std::string{decl_name_of(*bv->code)});
            }
            if (m.member == "fields" && bv->code->kind == ast::NodeKind::Struct) {
                return build_fields_value(static_cast<const ast::StructDecl&>(*bv->code),
                                          global_scope_);
            }
            return std::nullopt;
        }
        if (m.base->kind == ast::NodeKind::IdentExpr) {
            const auto& bi = static_cast<const ast::IdentExpr&>(*m.base);
            if (bi.name == "cfg") {
                return cfg_field(m.member);
            }
            if (global_scope_ != nullptr) {
                if (const auto* sym = global_scope_->lookup(bi.name)) {
                    // §A2 (§6.8) compile-time layout reflection on a
                    // struct: T.size / T.alignment fold to Int values
                    // computed via the C-style padding rules in
                    // struct_layout. Returns nullopt — meaning "not
                    // foldable" — if any field uses a type we don't
                    // know how to size yet; the comptime assertion
                    // surfaces that as a clear "couldn't fold" path
                    // rather than silently picking a wrong size.
                    if (sym->kind == SymbolKind::Struct && sym->decl != nullptr
                        && (m.member == "size" || m.member == "alignment")) {
                        const auto& sd = static_cast<const ast::StructDecl&>(*sym->decl);
                        auto lay = struct_layout(sd, *global_scope_);
                        std::int64_t v = m.member == "size" ? lay.size : lay.align;
                        if (v <= 0) {
                            return std::nullopt;
                        }
                        return make_int(v, TypeKind::Int);
                    }
                    if (sym->kind == SymbolKind::Struct && sym->decl != nullptr
                        && m.member == "fields") {
                        // §12.2 reflection: a Vector of Field values, one per
                        // non-embed field, with the §A2 layout walk filling the
                        // offset/size/alignment slots.
                        return build_fields_value(static_cast<const ast::StructDecl&>(*sym->decl),
                                                  global_scope_);
                    }
                }
            }
        }
        // Reflection member access on a folded Field or TypeRef value.
        // The base might be: (a) an IdentExpr bound to a local Field /
        // TypeRef (`let f = T.fields[0]; f.name`), or (b) a sub-
        // expression that itself folds to one (`T.fields[0].type.name`).
        // Field member dispatch walks the synthetic Field StructDecl in
        // global scope by declaration order so adding a member to the
        // Field decl + populating the matching `elements[i]` is a
        // one-place change.
        if (auto base_val = fold_with(*m.base, env, frame, TypeKind::Unit, depth)) {
            if (base_val->kind == ComptimeValue::Kind::Field) {
                // With a global scope, walk the synthetic `Field` StructDecl by
                // declaration order (authoritative). Without one (the
                // declaration-macro folder), fall back to the fixed positional
                // layout build_fields_value emits, so `field.name`/`.type`
                // resolve at expansion time.
                if (global_scope_ != nullptr) {
                    if (const auto* fsym = global_scope_->lookup("Field")) {
                        if (fsym->kind == SymbolKind::Struct && fsym->decl != nullptr) {
                            const auto& fd = static_cast<const ast::StructDecl&>(*fsym->decl);
                            for (std::size_t i = 0; i < fd.fields.size(); ++i) {
                                if (fd.fields[i].name == m.member
                                    && i < base_val->elements.size()) {
                                    return base_val->elements[i];
                                }
                            }
                        }
                    }
                }
                if (auto idx = field_member_index(m.member);
                    idx && *idx < base_val->elements.size()) {
                    return base_val->elements[*idx];
                }
                return std::nullopt;
            }
            if (base_val->kind == ComptimeValue::Kind::TypeRef && m.member == "name") {
                return make_string(base_val->s);
            }
            // §12.2 / vector ergonomics: `.length` on a folded vector
            // returns its element count. Useful for `for i in 0 ..< T.fields.length`.
            if (base_val->kind == ComptimeValue::Kind::Vector && m.member == "length") {
                return make_int(base_val->length, TypeKind::Int);
            }
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

    case ast::NodeKind::QuoteExpr: {
        // §12.4 an expression quote folds to a Code value: clone the quoted
        // expression and resolve its `$(…)` splices against the current frame.
        const auto& q = static_cast<const ast::QuoteExpr&>(e);
        if (!q.inner) {
            return std::nullopt;
        }
        ast::ExprPtr body = ast::clone(*q.inner);
        subst_splices(body, env, frame, depth);
        return make_code(std::shared_ptr<const ast::Node>(std::move(body)), TypeKind::AstExpr);
    }

    case ast::NodeKind::QuoteDeclExpr: {
        // §12.4 a declaration quote folds to a Vector of Code(Decl) values —
        // the macro's `[Decl]` result. Each item is either `$d` (splice the
        // bound Decl value, materialized by cloning) or a generated decl whose
        // `$(…)` splices are resolved against the current frame.
        const auto& q = static_cast<const ast::QuoteDeclExpr&>(e);
        ComptimeValue vec;
        vec.kind = ComptimeValue::Kind::Vector;
        vec.type = TypeKind::AstDecl;  // element kind
        for (const auto& item : q.decls) {
            if (item->kind == ast::NodeKind::SpliceDecl) {
                // `$d`: sd.splice is a SpliceExpr wrapping the named expression
                // (the IdentExpr `d`); fold that inner to its Code(Decl) value.
                const auto& sd = static_cast<const ast::SpliceDecl&>(*item);
                const ast::Expr* target = sd.splice.get();
                if (target != nullptr && target->kind == ast::NodeKind::SpliceExpr) {
                    target = static_cast<const ast::SpliceExpr&>(*target).inner.get();
                }
                std::optional<ComptimeValue> v;
                if (target != nullptr) {
                    v = fold_with(*target, env, frame, TypeKind::Unit, depth);
                }
                if (!v || v->kind != ComptimeValue::Kind::Code || !v->code) {
                    return std::nullopt;
                }
                ast::DeclPtr cloned = ast::clone(static_cast<const ast::Decl&>(*v->code));
                // Strip the expanding macro's own attribute from the reproduced
                // decl — it was kept on the original so `d.attribute(...)` could
                // read it, but the clone must not re-trip unknown-attribute
                // validation. Other (legitimate) attributes are preserved.
                if (!expanding_macro_name_.empty()) {
                    if (auto* attrs = decl_attrs_mut(*cloned)) {
                        std::erase_if(*attrs, [&](const ast::Attribute& a) {
                            return a.name == expanding_macro_name_;
                        });
                    }
                }
                vec.elements.push_back(make_code(
                    std::shared_ptr<const ast::Node>(std::move(cloned)), TypeKind::AstDecl));
            } else {
                ast::DeclPtr gen = ast::clone(*item);
                if (gen->kind == ast::NodeKind::Func) {
                    auto& fd = static_cast<ast::FuncDecl&>(*gen);
                    // §12.4 resolve a name-composing splice (`func $(expr)( … )`)
                    // by folding it to a comptime String; the loop variable is
                    // in scope here, so each generated decl gets a distinct name.
                    if (fd.name_splice) {
                        const ast::Expr* inner = fd.name_splice.get();
                        if (inner->kind == ast::NodeKind::SpliceExpr) {
                            inner = static_cast<const ast::SpliceExpr&>(*inner).inner.get();
                        }
                        auto nv = inner ? fold_with(*inner, env, frame, TypeKind::Unit, depth)
                                        : std::nullopt;
                        if (!nv || nv->kind != ComptimeValue::Kind::String) {
                            return std::nullopt;
                        }
                        fd.name = nv->s;
                        fd.name_splice = nullptr;
                    }
                    // §12.4 resolve `$(…)` splices in type position (param
                    // types, result type) — `_ v: $(d.name)`, `-> $(f.type)` —
                    // by folding each to a String/TypeRef and materializing a
                    // concrete NamedType.
                    const auto resolve_type_splice = [&](ast::TypePtr& ty) -> bool {
                        if (!ty || ty->kind != ast::NodeKind::SpliceType) {
                            return true;
                        }
                        const ast::Expr* inner = static_cast<ast::SpliceType&>(*ty).splice.get();
                        if (inner != nullptr && inner->kind == ast::NodeKind::SpliceExpr) {
                            inner = static_cast<const ast::SpliceExpr&>(*inner).inner.get();
                        }
                        auto tv = inner ? fold_with(*inner, env, frame, TypeKind::Unit, depth)
                                        : std::nullopt;
                        if (!tv) {
                            return false;
                        }
                        auto mat = materialize_type(*tv);
                        if (!mat) {
                            return false;
                        }
                        mat->range = ty->range;
                        ty = std::move(mat);
                        return true;
                    };
                    for (auto& p : fd.params) {
                        if (!resolve_type_splice(p.type)) {
                            return std::nullopt;
                        }
                    }
                    if (!resolve_type_splice(fd.result)) {
                        return std::nullopt;
                    }
                    subst_splices(fd.body, env, frame, depth);
                }
                vec.elements.push_back(
                    make_code(std::shared_ptr<const ast::Node>(std::move(gen)), TypeKind::AstDecl));
            }
        }
        vec.length = static_cast<std::int64_t>(vec.elements.size());
        return vec;
    }

    case ast::NodeKind::EmbedExpr: {
        // §12.1's documented exception to comptime purity: read the named
        // file at fold time. The reader does path resolution + (eventually)
        // manifest enforcement; we just consume bytes and wrap them as a
        // Vector<UInt8> ComptimeValue. The folded length pins the [N]UInt8
        // type the resolver assigns to this expression.
        const auto& ee = static_cast<const ast::EmbedExpr&>(e);
        if (!embed_reader_) {
            return std::nullopt;
        }
        auto bytes = embed_reader_(ee.path);
        if (!bytes) {
            return std::nullopt;
        }
        ComptimeValue cv;
        cv.kind = ComptimeValue::Kind::Vector;
        cv.type = TypeKind::UInt8;
        cv.length = static_cast<std::int64_t>(bytes->size());
        cv.elements.reserve(bytes->size());
        for (auto b : *bytes) {
            ComptimeValue byte;
            byte.kind = ComptimeValue::Kind::UInt;
            byte.type = TypeKind::UInt8;
            byte.u = b;
            cv.elements.push_back(std::move(byte));
        }
        return cv;
    }

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

        // §12.4 attribute reflection: method calls on a `Decl` value with a
        // single String argument — `d.hasAttribute("x")` (Bool) and
        // `d.attribute("x")` (the folded value of attribute x's single
        // argument, e.g. `@register(0x40)` → 64). The macro's own attribute is
        // still present on the annotated decl during expansion, so a macro can
        // read the argument it was invoked with.
        if (c.callee->kind == ast::NodeKind::MemberExpr && c.args.size() == 1) {
            const auto& m = static_cast<const ast::MemberExpr&>(*c.callee);
            if ((m.member == "hasAttribute" || m.member == "attribute") && m.base) {
                if (auto bv = fold_with(*m.base, env, frame, TypeKind::Unit, depth);
                    bv && bv->kind == ComptimeValue::Kind::Code && bv->code) {
                    auto an = fold_with(*c.args[0].value, env, frame, TypeKind::Unit, depth + 1);
                    if (!an || an->kind != ComptimeValue::Kind::String) {
                        return std::nullopt;
                    }
                    const ast::Attribute* attr = find_decl_attribute(*bv->code, an->s);
                    if (m.member == "hasAttribute") {
                        return make_bool(attr != nullptr);
                    }
                    // `attribute`: fold the attribute's single argument.
                    if (attr == nullptr || !attr->predicate) {
                        return std::nullopt;
                    }
                    return fold_with(*attr->predicate, env, frame, hint, depth + 1);
                }
            }
        }

        if (c.callee->kind != ast::NodeKind::IdentExpr) {
            return std::nullopt;
        }
        const auto& ident = static_cast<const ast::IdentExpr&>(*c.callee);

        // §12.4 hygiene: `gensym()` / `gensym("hint")` yields a fresh, unique
        // identifier on every call — for macro-introduced names that must not
        // collide across applications or with user code. Composes with the name
        // splices: `func $(gensym("helper"))( … )`. Only reachable inside a
        // macro body (the resolver never registers `gensym`, so a normal call
        // site fails to resolve); the counter is unit-wide via the reused
        // expansion folder.
        if (ident.name == "gensym" && c.args.size() <= 1) {
            std::string gs_hint;
            if (c.args.size() == 1) {
                auto gs_pv = fold_with(*c.args[0].value, env, frame, TypeKind::Unit, depth + 1);
                if (!gs_pv || gs_pv->kind != ComptimeValue::Kind::String) {
                    return std::nullopt;
                }
                gs_hint = gs_pv->s;
            }
            const std::int64_t gs_n = gensym_counter_++;
            std::string gs_name = "__vstr_";
            if (!gs_hint.empty()) {
                gs_name += gs_hint + "_";
            }
            gs_name += "h" + std::to_string(gs_n);
            return make_string(std::move(gs_name));
        }

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
        case ast::UnaryOp::Unwrap:
            // §9 force-unwrap is a runtime operation (panics on `.none`);
            // the folder doesn't model Optional values, so bail rather
            // than guess.
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
        // §12.4 String `+` (comptime concatenation, for composed names) and
        // String `==`/`!=` (cfg predicates) are the non-numeric binary ops the
        // folder allows; everything else needs matching numeric operands.
        const bool both_strings =
            lhs->kind == ComptimeValue::Kind::String && rhs->kind == ComptimeValue::Kind::String;
        if (!same_numeric_kind(*lhs, *rhs) && bin.op != ast::BinaryOp::Eq
            && bin.op != ast::BinaryOp::Ne && !(bin.op == ast::BinaryOp::Add && both_strings)) {
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
            if (lhs->kind == ComptimeValue::Kind::String
                && rhs->kind == ComptimeValue::Kind::String) {
                // §12.4 comptime string concatenation — composes generated
                // declaration names like `$(f.name + "_get")`.
                return make_string(lhs->s + rhs->s);
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
        if (!i.cond) {
            // §9 `if let` — the folder doesn't model Optional values,
            // so bail rather than guess which branch fires.
            return std::nullopt;
        }
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
        if (cur.kind == ComptimeValue::Kind::Vector && rhs.kind == ComptimeValue::Kind::Vector) {
            // §12.4 `[Decl] += quote { … }` — append the quote's declarations
            // to the accumulator. The element kind (`type`) is preserved.
            ComptimeValue out = cur;
            out.elements.insert(out.elements.end(), rhs.elements.begin(), rhs.elements.end());
            out.length = static_cast<std::int64_t>(out.elements.size());
            return out;
        }
        if (cur.kind == ComptimeValue::Kind::String && rhs.kind == ComptimeValue::Kind::String) {
            ComptimeValue out = cur;
            out.s = cur.s + rhs.s;
            return out;
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
        // Two foldable iteration shapes:
        //   * a range: `for ident in start ..< end` (RangeLt) / `start .. end`
        //     (Range) — bounds folded to Ints, generated lazily.
        //   * a collection: any other iterable that folds to a Vector value
        //     (e.g. `for f in d.fields` over §12.2/§12.4 reflection) — each
        //     element binds to the loop variable in turn.
        // Anything else (the runtime iterator protocol) waits for a later phase.
        const auto& f = static_cast<const ast::ForStmt&>(s);
        if (!f.iter) {
            return false;
        }

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

        // Run the body once with the loop variable bound to `iv`. Returns
        // false to bail the whole fold, true to continue (the caller checks
        // frame.returned for an early `return`).
        const auto run_iter = [&](const ComptimeValue& iv) -> bool {
            if (!var_name.empty()) {
                frame.locals[var_name] = iv;
            }
            if (f.body) {
                auto v = fold_with(*f.body, env, frame, TypeKind::Unit, depth);
                (void)v;  // body's value is discarded; assignments and
                          // frame.returned are what we propagate
            }
            return true;
        };
        const auto restore = [&]() {
            if (var_name.empty()) {
                return;
            }
            if (had_prior) {
                frame.locals[var_name] = prior;
            } else {
                frame.locals.erase(var_name);
            }
        };

        const bool is_range =
            f.iter->kind == ast::NodeKind::BinaryExpr
            && (static_cast<const ast::BinaryExpr&>(*f.iter).op == ast::BinaryOp::Range
                || static_cast<const ast::BinaryExpr&>(*f.iter).op == ast::BinaryOp::RangeLt);
        if (is_range) {
            const auto& binop = static_cast<const ast::BinaryExpr&>(*f.iter);
            auto lo = fold_with(*binop.lhs, env, frame, TypeKind::Int, depth);
            auto hi = fold_with(*binop.rhs, env, frame, TypeKind::Int, depth);
            if (!lo || !hi || lo->kind != ComptimeValue::Kind::Int
                || hi->kind != ComptimeValue::Kind::Int) {
                return false;
            }
            std::int64_t start = lo->i;
            // `..` is inclusive; `..<` is exclusive.
            std::int64_t end = binop.op == ast::BinaryOp::Range ? hi->i + 1 : hi->i;
            std::int64_t iters = 0;
            for (std::int64_t i = start; i < end; ++i) {
                if (++iters > MaxLoopIterations) {
                    return false;
                }
                ComptimeValue iv;
                iv.kind = ComptimeValue::Kind::Int;
                iv.i = i;
                iv.type = TypeKind::Int;
                if (!run_iter(iv)) {
                    return false;
                }
                if (frame.returned) {
                    break;
                }
            }
            restore();
            return true;
        }

        // Collection iteration: the iterable must fold to a Vector value.
        auto coll = fold_with(*f.iter, env, frame, TypeKind::Unit, depth);
        if (!coll || coll->kind != ComptimeValue::Kind::Vector) {
            return false;
        }
        std::int64_t iters = 0;
        for (const auto& el : coll->elements) {
            if (++iters > MaxLoopIterations) {
                return false;
            }
            if (!run_iter(el)) {
                return false;
            }
            if (frame.returned) {
                break;
            }
        }
        restore();
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

// ============================================================================
// §12.4 macro-body evaluation
// ============================================================================

ast::ExprPtr ComptimeFolder::materialize_value(const ComptimeValue& v) const {
    switch (v.kind) {
    case ComptimeValue::Kind::Int: {
        auto lit = std::make_unique<ast::IntLit>();
        lit->text = std::to_string(v.i);
        return lit;
    }
    case ComptimeValue::Kind::UInt: {
        auto lit = std::make_unique<ast::IntLit>();
        lit->text = std::to_string(v.u);
        return lit;
    }
    case ComptimeValue::Kind::Float: {
        auto lit = std::make_unique<ast::FloatLit>();
        lit->text = std::format("{}", v.f);
        return lit;
    }
    case ComptimeValue::Kind::Bool: {
        auto lit = std::make_unique<ast::BoolLit>();
        lit->value = v.b;
        return lit;
    }
    case ComptimeValue::Kind::String: {
        // StringLit.text is the unquoted content; codegen re-quotes it.
        auto lit = std::make_unique<ast::StringLit>();
        lit->text = v.s;
        return lit;
    }
    case ComptimeValue::Kind::Code:
        return v.code ? ast::clone(static_cast<const ast::Expr&>(*v.code)) : nullptr;
    case ComptimeValue::Kind::Vector:
    case ComptimeValue::Kind::Field:
    case ComptimeValue::Kind::TypeRef:
    case ComptimeValue::Kind::Unit:
        return nullptr;
    }
    return nullptr;
}

void ComptimeFolder::subst_splices_in_stmt(ast::Stmt& s,
                                           const Env& env,
                                           Frame& frame,
                                           int depth) const {
    switch (s.kind) {
    case ast::NodeKind::LetStmt:
        subst_splices(static_cast<ast::LetStmt&>(s).value, env, frame, depth);
        break;
    case ast::NodeKind::VarStmt:
        subst_splices(static_cast<ast::VarStmt&>(s).value, env, frame, depth);
        break;
    case ast::NodeKind::ExprStmt:
        subst_splices(static_cast<ast::ExprStmt&>(s).expr, env, frame, depth);
        break;
    case ast::NodeKind::ReturnStmt:
        subst_splices(static_cast<ast::ReturnStmt&>(s).value, env, frame, depth);
        break;
    case ast::NodeKind::AssignStmt:
        subst_splices(static_cast<ast::AssignStmt&>(s).target, env, frame, depth);
        subst_splices(static_cast<ast::AssignStmt&>(s).value, env, frame, depth);
        break;
    case ast::NodeKind::ForStmt:
        subst_splices(static_cast<ast::ForStmt&>(s).iter, env, frame, depth);
        subst_splices(static_cast<ast::ForStmt&>(s).body, env, frame, depth);
        break;
    case ast::NodeKind::WhileStmt:
        subst_splices(static_cast<ast::WhileStmt&>(s).cond, env, frame, depth);
        subst_splices(static_cast<ast::WhileStmt&>(s).body, env, frame, depth);
        break;
    default:
        break;
    }
}

void ComptimeFolder::subst_splices(ast::ExprPtr& e, const Env& env, Frame& frame, int depth) const {
    if (!e) {
        return;
    }
    if (e->kind == ast::NodeKind::SpliceExpr) {
        auto& sp = static_cast<ast::SpliceExpr&>(*e);
        if (sp.inner) {
            // Fold the spliced expression and drop the resulting constant in
            // place (`$(k)` → IntLit, `$(d.name)` → StringLit). If it doesn't
            // fold, leave the splice's inner walked so nested splices resolve.
            if (auto v = fold_with(*sp.inner, env, frame, TypeKind::Unit, depth)) {
                if (auto mat = materialize_value(*v)) {
                    mat->range = e->range;
                    e = std::move(mat);
                    return;
                }
            }
            subst_splices(sp.inner, env, frame, depth);
        }
        return;
    }
    switch (e->kind) {
    case ast::NodeKind::ParenExpr:
        subst_splices(static_cast<ast::ParenExpr&>(*e).inner, env, frame, depth);
        break;
    case ast::NodeKind::UnaryExpr:
        subst_splices(static_cast<ast::UnaryExpr&>(*e).operand, env, frame, depth);
        break;
    case ast::NodeKind::BinaryExpr:
        subst_splices(static_cast<ast::BinaryExpr&>(*e).lhs, env, frame, depth);
        subst_splices(static_cast<ast::BinaryExpr&>(*e).rhs, env, frame, depth);
        break;
    case ast::NodeKind::MemberExpr: {
        auto& mem = static_cast<ast::MemberExpr&>(*e);
        subst_splices_ident(mem.base, env, frame, depth);
        // §12.4 resolve a member-name splice (`v.$(f.name)`) to its String.
        if (mem.member_splice) {
            const ast::Expr* inner = mem.member_splice.get();
            if (inner->kind == ast::NodeKind::SpliceExpr) {
                inner = static_cast<const ast::SpliceExpr&>(*inner).inner.get();
            }
            if (inner != nullptr) {
                if (auto v = fold_with(*inner, env, frame, TypeKind::Unit, depth);
                    v && v->kind == ComptimeValue::Kind::String) {
                    mem.member = v->s;
                    mem.member_splice = nullptr;
                }
            }
        }
        break;
    }
    case ast::NodeKind::IndexExpr: {
        auto& ix = static_cast<ast::IndexExpr&>(*e);
        subst_splices_ident(ix.base, env, frame, depth);
        for (auto& i : ix.indices) {
            subst_splices(i, env, frame, depth);
        }
        break;
    }
    case ast::NodeKind::CallExpr: {
        auto& c = static_cast<ast::CallExpr&>(*e);
        subst_splices_ident(c.callee, env, frame, depth);
        for (auto& a : c.args) {
            subst_splices(a.value, env, frame, depth);
        }
        break;
    }
    case ast::NodeKind::BlockExpr:
        for (auto& st : static_cast<ast::BlockExpr&>(*e).stmts) {
            subst_splices_in_stmt(*st, env, frame, depth);
        }
        break;
    case ast::NodeKind::IfExpr: {
        auto& i = static_cast<ast::IfExpr&>(*e);
        subst_splices(i.cond, env, frame, depth);
        subst_splices(i.let_init, env, frame, depth);
        subst_splices(i.then_branch, env, frame, depth);
        subst_splices(i.else_branch, env, frame, depth);
        break;
    }
    default:
        break;
    }
}

void ComptimeFolder::subst_splices_ident(ast::ExprPtr& e,
                                         const Env& env,
                                         Frame& frame,
                                         int depth) const {
    // Only a direct `$(…)` splice gets the identifier treatment; anything else
    // (a dotted callee, a parenthesized expression, …) walks normally.
    if (!e || e->kind != ast::NodeKind::SpliceExpr) {
        subst_splices(e, env, frame, depth);
        return;
    }
    auto& sp = static_cast<ast::SpliceExpr&>(*e);
    if (!sp.inner) {
        return;
    }
    if (auto v = fold_with(*sp.inner, env, frame, TypeKind::Unit, depth)) {
        // A String in identifier position is a *reference* to that name
        // (`$(h)()` calls the function named by `h`), not a string literal.
        if (v->kind == ComptimeValue::Kind::String) {
            auto id = std::make_unique<ast::IdentExpr>();
            id->name = v->s;
            id->range = e->range;
            e = std::move(id);
            return;
        }
        if (auto mat = materialize_value(*v)) {
            mat->range = e->range;
            e = std::move(mat);
            return;
        }
    }
    subst_splices(sp.inner, env, frame, depth);
}

std::optional<std::vector<ast::DeclPtr>>
ComptimeFolder::expand_decl_macro(const ast::FuncDecl& macro, const ast::Decl& annotated) const {
    if (!macro.body) {
        return std::nullopt;
    }
    // The macro's own attribute stays on `annotated` so reflection can read it;
    // record the name so the `$d` clone drops it (see the SpliceDecl branch).
    expanding_macro_name_ = macro.name;
    Frame frame;
    Env env;
    // Bind the macro's `Decl` parameter to a Code value *aliasing* the annotated
    // declaration (a non-owning shared_ptr — the unit still owns the node). `$d`
    // and `d.name` read through it; materialization always clones, so the alias
    // never escapes.
    if (!macro.params.empty()) {
        auto alias = std::shared_ptr<const ast::Node>(std::shared_ptr<const ast::Node>{},
                                                      &annotated);  // aliasing ctor: no delete
        frame.locals[macro.params[0].name] = make_code(std::move(alias), TypeKind::AstDecl);
    }
    auto result = fold_with(*macro.body, env, frame, TypeKind::Unit, 0);
    if (!result || result->kind != ComptimeValue::Kind::Vector) {
        return std::nullopt;
    }
    std::vector<ast::DeclPtr> out;
    out.reserve(result->elements.size());
    for (const auto& el : result->elements) {
        if (el.kind != ComptimeValue::Kind::Code || !el.code) {
            return std::nullopt;
        }
        out.push_back(ast::clone(static_cast<const ast::Decl&>(*el.code)));
    }
    return out;
}

}  // namespace vestra::sema
