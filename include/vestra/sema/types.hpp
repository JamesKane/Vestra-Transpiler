#pragma once

#include "vestra/ast/nodes.hpp"

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace vestra::sema {

// The kinds of semantic types we model in v0.5. Distinct from
// ast::NodeKind: ast::Type is a parsed type expression, sema::Type is a
// resolved, canonicalized type.
//
// `Never` is the type of a diverging expression (`throw`, `panic`); `Error` is
// the result of a failed lookup or invalid construction — it's compatible with
// everything to suppress cascading diagnostics.
//
// NOLINTNEXTLINE(performance-enum-size)
enum class TypeKind : std::uint16_t {
    // primitives
    Int8,
    Int16,
    Int32,
    Int64,
    Int,
    UInt8,
    UInt16,
    UInt32,
    UInt64,
    UInt,
    Float32,
    Float64,
    Bool,
    Char,
    Unit,
    String,
    Str,
    StrConst,
    // bookkeeping
    Never,
    Error,
    // structural
    Optional,
    Vector,
    Function,
    Tuple,
    // nominal — point back to an ast::Decl
    Struct,
    Enum,
    Protocol,
    OpaqueType,
    // generic parameter — opaque within its declaring scope
    GenericParam,
};

class Type;
using TypePtr = const Type*;

// Canonicalized type value. Owned by TypeArena; passed around as TypePtr.
class Type {
public:
    Type(const Type&) = delete;
    Type& operator=(const Type&) = delete;
    Type(Type&&) = delete;
    Type& operator=(Type&&) = delete;
    ~Type() = default;

    [[nodiscard]] TypeKind kind() const noexcept { return kind_; }

    // Field accessors — only meaningful for the kinds that use them.
    [[nodiscard]] TypePtr inner() const noexcept { return inner_; }
    [[nodiscard]] std::int64_t vector_length() const noexcept { return length_; }
    [[nodiscard]] const std::vector<TypePtr>& parts() const noexcept { return parts_; }
    [[nodiscard]] TypePtr result() const noexcept { return result_; }
    [[nodiscard]] const ast::Decl* nominal_decl() const noexcept { return decl_; }
    [[nodiscard]] std::string_view generic_name() const noexcept { return name_; }

    [[nodiscard]] bool is_primitive() const noexcept;
    [[nodiscard]] bool is_integer() const noexcept;
    [[nodiscard]] bool is_float() const noexcept;
    [[nodiscard]] bool is_numeric() const noexcept { return is_integer() || is_float(); }
    [[nodiscard]] bool is_error() const noexcept { return kind_ == TypeKind::Error; }
    [[nodiscard]] bool is_never() const noexcept { return kind_ == TypeKind::Never; }

    [[nodiscard]] std::string describe() const;  // human-readable spelling

private:
    friend class TypeArena;

    explicit Type(TypeKind k) : kind_(k) {}

    TypeKind kind_;
    TypePtr inner_ = nullptr;          // Optional / Vector element
    std::int64_t length_ = 0;          // Vector length (0 if symbolic)
    std::vector<TypePtr> parts_;       // Function params / Tuple elements
    TypePtr result_ = nullptr;         // Function result
    const ast::Decl* decl_ = nullptr;  // Nominal target
    std::string name_;                 // GenericParam name
};

// Owns Type storage. Primitive types are interned (one canonical instance per
// kind) so equality is pointer-equality. Compound types are *not* interned in
// v0.5 — the cost of a hash table over them isn't justified yet.
class TypeArena {
public:
    TypeArena();
    TypeArena(const TypeArena&) = delete;
    TypeArena& operator=(const TypeArena&) = delete;
    TypeArena(TypeArena&&) = default;
    TypeArena& operator=(TypeArena&&) = default;
    ~TypeArena() = default;

    // Interned primitive lookups.
    [[nodiscard]] TypePtr primitive(TypeKind k) const;
    [[nodiscard]] TypePtr error() const { return primitive(TypeKind::Error); }
    [[nodiscard]] TypePtr never() const { return primitive(TypeKind::Never); }
    [[nodiscard]] TypePtr unit() const { return primitive(TypeKind::Unit); }
    [[nodiscard]] TypePtr boolean() const { return primitive(TypeKind::Bool); }

    // Structural constructors. Each call yields a fresh Type — equality is by
    // structural compare via equal() below, not pointer compare.
    [[nodiscard]] TypePtr make_optional(TypePtr inner);
    [[nodiscard]] TypePtr make_vector(std::int64_t length, TypePtr element);
    [[nodiscard]] TypePtr make_function(std::vector<TypePtr> params, TypePtr result);
    [[nodiscard]] TypePtr make_tuple(std::vector<TypePtr> elements);

    [[nodiscard]] TypePtr make_nominal(TypeKind k, const ast::Decl* decl);
    [[nodiscard]] TypePtr make_generic_param(std::string name);

    // Look up a primitive by its Vestra spelling — used by the resolver when
    // it sees `Int32` etc. in a type position.
    [[nodiscard]] static TypeKind primitive_kind_by_name(std::string_view name) noexcept;

    // Structural equality. Two types are equal when their kinds and all their
    // children match recursively; nominal types match by decl pointer.
    [[nodiscard]] static bool equal(TypePtr a, TypePtr b) noexcept;

    // True when a value of `from` may be supplied where `to` is expected — for
    // now this is just `equal(from, to) || either is Error/Never`.
    [[nodiscard]] static bool assignable(TypePtr from, TypePtr to) noexcept;

private:
    std::vector<std::unique_ptr<Type>> owned_;
    std::unordered_map<int, const Type*> primitives_;  // keyed by TypeKind value
};

}  // namespace vestra::sema
