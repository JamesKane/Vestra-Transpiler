// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "vestra/sema/types.hpp"

#include <cassert>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>

namespace vestra::sema {

// ----- Type --------------------------------------------------------------

bool Type::is_primitive() const noexcept {
    switch (kind_) {
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
    case TypeKind::Float32:
    case TypeKind::Float64:
    case TypeKind::Bool:
    case TypeKind::Char:
    case TypeKind::Unit:
    case TypeKind::String:
    case TypeKind::Str:
    case TypeKind::StrConst:
        return true;
    default:
        return false;
    }
}

bool Type::is_integer() const noexcept {
    switch (kind_) {
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

bool Type::is_float() const noexcept {
    return kind_ == TypeKind::Float32 || kind_ == TypeKind::Float64;
}

namespace {

std::string_view primitive_spelling(TypeKind k) noexcept {
    switch (k) {
    case TypeKind::Int8:
        return "Int8";
    case TypeKind::Int16:
        return "Int16";
    case TypeKind::Int32:
        return "Int32";
    case TypeKind::Int64:
        return "Int64";
    case TypeKind::Int:
        return "Int";
    case TypeKind::UInt8:
        return "UInt8";
    case TypeKind::UInt16:
        return "UInt16";
    case TypeKind::UInt32:
        return "UInt32";
    case TypeKind::UInt64:
        return "UInt64";
    case TypeKind::UInt:
        return "UInt";
    case TypeKind::Int128:
        return "Int128";
    case TypeKind::UInt128:
        return "UInt128";
    case TypeKind::Float32:
        return "Float32";
    case TypeKind::Float64:
        return "Float64";
    case TypeKind::Bool:
        return "Bool";
    case TypeKind::Char:
        return "Char";
    case TypeKind::Unit:
        return "Unit";
    case TypeKind::String:
        return "String";
    case TypeKind::Str:
        return "Str";
    case TypeKind::StrConst:
        return "StrConst";
    case TypeKind::Never:
        return "Never";
    case TypeKind::Error:
        return "<error>";
    default:
        return {};
    }
}

}  // namespace

std::string Type::describe() const {
    if (is_primitive() || kind_ == TypeKind::Never || kind_ == TypeKind::Error) {
        return std::string{primitive_spelling(kind_)};
    }
    switch (kind_) {
    case TypeKind::Optional:
        return inner_ ? inner_->describe() + "?" : "?";
    case TypeKind::Box:
        return inner_ ? std::format("Box[{}]", inner_->describe()) : std::string{"Box"};
    case TypeKind::Future:
        return inner_ ? std::format("Future[{}]", inner_->describe()) : std::string{"Future"};
    case TypeKind::Channel:
        return inner_ ? std::format("Channel[{}]", inner_->describe()) : std::string{"Channel"};
    case TypeKind::Duration:
        return "Duration";
    case TypeKind::AstExpr:
        return "Expr";
    case TypeKind::Span:
        return inner_ ? std::format("Span[{}]", inner_->describe()) : std::string{"Span"};
    case TypeKind::MutSpan:
        return inner_ ? std::format("MutSpan[{}]", inner_->describe()) : std::string{"MutSpan"};
    case TypeKind::Ptr:
        return inner_ ? std::format("Ptr[{}]", inner_->describe()) : std::string{"Ptr"};
    case TypeKind::MutPtr:
        return inner_ ? std::format("MutPtr[{}]", inner_->describe()) : std::string{"MutPtr"};
    case TypeKind::MmioView:
        return inner_ ? std::format("MmioView[{}]", inner_->describe()) : std::string{"MmioView"};
    case TypeKind::MmioRegion:
        return inner_ ? std::format("MmioRegion[{}]", inner_->describe())
                      : std::string{"MmioRegion"};
    case TypeKind::MmioWireView:
        return inner_ ? std::format("MmioWireView[{}]", inner_->describe())
                      : std::string{"MmioWireView"};
    case TypeKind::PerCpu:
        return inner_ ? std::format("PerCpu[{}]", inner_->describe()) : std::string{"PerCpu"};
    case TypeKind::InterruptHandler:
        return inner_ ? std::format("@interrupt({})", inner_->describe())
                      : std::string{"@interrupt"};
    case TypeKind::Padded:
        return inner_ ? std::format("Padded[{}]", inner_->describe()) : std::string{"Padded"};
    case TypeKind::SysregHandle:
        return inner_ ? std::format("ReadWriteSysreg[{}]", inner_->describe())
                      : std::string{"ReadWriteSysreg"};
    case TypeKind::SysregHandleRO:
        return inner_ ? std::format("ReadOnlySysreg[{}]", inner_->describe())
                      : std::string{"ReadOnlySysreg"};
    case TypeKind::SysregHandleWO:
        return inner_ ? std::format("WriteOnlySysreg[{}]", inner_->describe())
                      : std::string{"WriteOnlySysreg"};
    case TypeKind::ZipIter: {
        std::string a = (parts_.size() >= 1 && parts_[0]) ? parts_[0]->describe() : "?";
        std::string b = (parts_.size() >= 2 && parts_[1]) ? parts_[1]->describe() : "?";
        return std::format("ZipIter[{}, {}]", a, b);
    }
    case TypeKind::TakeIter:
        return inner_ ? std::format("TakeIter[{}]", inner_->describe()) : std::string{"TakeIter"};
    case TypeKind::MapIter: {
        std::string a = (parts_.size() >= 1 && parts_[0]) ? parts_[0]->describe() : "?";
        std::string b = (parts_.size() >= 2 && parts_[1]) ? parts_[1]->describe() : "?";
        return std::format("MapIter[{} -> {}]", a, b);
    }
    case TypeKind::FilterIter:
        return inner_ ? std::format("FilterIter[{}]", inner_->describe())
                      : std::string{"FilterIter"};
    case TypeKind::ChunkIter:
        return inner_ ? std::format("ChunkIter[{}]", inner_->describe()) : std::string{"ChunkIter"};
    case TypeKind::Atomic:
        return inner_ ? std::format("Atomic[{}]", inner_->describe()) : std::string{"Atomic"};
    case TypeKind::CasResult:
        return inner_ ? std::format("CASResult[{}]", inner_->describe()) : std::string{"CASResult"};
    case TypeKind::AtomicTaggedPointer:
        return inner_ ? std::format("AtomicTaggedPointer[{}]", inner_->describe())
                      : std::string{"AtomicTaggedPointer"};
    case TypeKind::Result: {
        std::string ok = inner_ ? inner_->describe() : "?";
        std::string err = result_ ? result_->describe() : "?";
        return std::format("Result<{}, {}>", ok, err);
    }
    case TypeKind::Vector: {
        std::string elem = inner_ ? inner_->describe() : std::string{"?"};
        // §7 generics phase 2 — a symbolic-length vector renders `[N]T`.
        if (!name_.empty()) {
            return std::format("[{}]{}", name_, elem);
        }
        return std::format("[{}]{}", length_, elem);
    }
    case TypeKind::ConstValue:
        return std::to_string(length_);
    case TypeKind::Tuple: {
        std::string s = "(";
        for (std::size_t i = 0; i < parts_.size(); ++i) {
            if (i != 0) {
                s += ", ";
            }
            s += parts_[i] ? parts_[i]->describe() : "?";
        }
        s += ")";
        return s;
    }
    case TypeKind::Function: {
        std::string s = "(";
        for (std::size_t i = 0; i < parts_.size(); ++i) {
            if (i != 0) {
                s += ", ";
            }
            s += parts_[i] ? parts_[i]->describe() : "?";
        }
        s += ") -> ";
        s += result_ ? result_->describe() : "Unit";
        return s;
    }
    case TypeKind::Struct:
    case TypeKind::Enum:
    case TypeKind::Protocol:
    case TypeKind::OpaqueType:
        if (decl_) {
            // We can't include the decl's name here without ast.hpp dep — but
            // we have it. Use a generic placeholder; callers with the decl in
            // hand can print better.
            switch (decl_->kind) {
            case ast::NodeKind::Struct: {
                std::string s = static_cast<const ast::StructDecl&>(*decl_).name;
                // §7 generics phase 2 — a struct instance records its
                // resolved type arguments in parts_; render `Pair[Int32]`.
                if (!parts_.empty()) {
                    s += "[";
                    for (std::size_t i = 0; i < parts_.size(); ++i) {
                        if (i != 0) {
                            s += ", ";
                        }
                        s += parts_[i] ? parts_[i]->describe() : "?";
                    }
                    s += "]";
                }
                return s;
            }
            case ast::NodeKind::Enum: {
                std::string s = static_cast<const ast::EnumDecl&>(*decl_).name;
                // §7 generics phase 2 — render `Option[Int32]` for an
                // enum instance carrying resolved type arguments.
                if (!parts_.empty()) {
                    s += "[";
                    for (std::size_t i = 0; i < parts_.size(); ++i) {
                        if (i != 0) {
                            s += ", ";
                        }
                        s += parts_[i] ? parts_[i]->describe() : "?";
                    }
                    s += "]";
                }
                return s;
            }
            case ast::NodeKind::Protocol:
                return static_cast<const ast::ProtocolDecl&>(*decl_).name;
            case ast::NodeKind::Opaque:
                return static_cast<const ast::OpaqueDecl&>(*decl_).name;
            default:
                break;
            }
        }
        return "<nominal>";
    case TypeKind::GenericParam:
        return name_;
    default:
        return "<type>";
    }
}

// ----- TypeArena ---------------------------------------------------------

TypeArena::TypeArena() {
    // Pre-intern every primitive so calls to primitive(...) are O(1) hash hits.
    for (auto k : {TypeKind::Int8,     TypeKind::Int16,   TypeKind::Int32,  TypeKind::Int64,
                   TypeKind::Int,      TypeKind::UInt8,   TypeKind::UInt16, TypeKind::UInt32,
                   TypeKind::UInt64,   TypeKind::UInt,    TypeKind::Int128, TypeKind::UInt128,
                   TypeKind::Float32,  TypeKind::Float64, TypeKind::Bool,   TypeKind::Char,
                   TypeKind::Unit,     TypeKind::String,  TypeKind::Str,    TypeKind::StrConst,
                   TypeKind::Duration, TypeKind::AstExpr, TypeKind::Never,  TypeKind::Error}) {
        auto t = std::unique_ptr<Type>(new Type(k));
        primitives_.emplace(k, t.get());
        owned_.push_back(std::move(t));
    }
}

TypePtr TypeArena::primitive(TypeKind k) const {
    auto it = primitives_.find(k);
    assert(it != primitives_.end() && "primitive(...) called with non-primitive kind");
    return it->second;
}

TypePtr TypeArena::make_optional(TypePtr inner) {
    auto t = std::unique_ptr<Type>(new Type(TypeKind::Optional));
    t->inner_ = inner;
    auto* p = t.get();
    owned_.push_back(std::move(t));
    return p;
}

TypePtr TypeArena::make_box(TypePtr inner) {
    auto t = std::unique_ptr<Type>(new Type(TypeKind::Box));
    t->inner_ = inner;
    auto* p = t.get();
    owned_.push_back(std::move(t));
    return p;
}

TypePtr TypeArena::make_future(TypePtr inner) {
    auto t = std::unique_ptr<Type>(new Type(TypeKind::Future));
    t->inner_ = inner;
    auto* p = t.get();
    owned_.push_back(std::move(t));
    return p;
}

TypePtr TypeArena::make_channel(TypePtr inner) {
    auto t = std::unique_ptr<Type>(new Type(TypeKind::Channel));
    t->inner_ = inner;
    auto* p = t.get();
    owned_.push_back(std::move(t));
    return p;
}

TypePtr TypeArena::make_span(TypePtr inner) {
    auto t = std::unique_ptr<Type>(new Type(TypeKind::Span));
    t->inner_ = inner;
    auto* p = t.get();
    owned_.push_back(std::move(t));
    return p;
}

TypePtr TypeArena::make_mut_span(TypePtr inner) {
    auto t = std::unique_ptr<Type>(new Type(TypeKind::MutSpan));
    t->inner_ = inner;
    auto* p = t.get();
    owned_.push_back(std::move(t));
    return p;
}

TypePtr TypeArena::make_ptr(TypePtr inner) {
    auto t = std::unique_ptr<Type>(new Type(TypeKind::Ptr));
    t->inner_ = inner;
    auto* p = t.get();
    owned_.push_back(std::move(t));
    return p;
}

TypePtr TypeArena::make_mut_ptr(TypePtr inner) {
    auto t = std::unique_ptr<Type>(new Type(TypeKind::MutPtr));
    t->inner_ = inner;
    auto* p = t.get();
    owned_.push_back(std::move(t));
    return p;
}

TypePtr TypeArena::make_mmio_view(TypePtr inner) {
    auto t = std::unique_ptr<Type>(new Type(TypeKind::MmioView));
    t->inner_ = inner;
    auto* p = t.get();
    owned_.push_back(std::move(t));
    return p;
}

TypePtr TypeArena::make_mmio_region(TypePtr inner) {
    auto t = std::unique_ptr<Type>(new Type(TypeKind::MmioRegion));
    t->inner_ = inner;
    auto* p = t.get();
    owned_.push_back(std::move(t));
    return p;
}

TypePtr TypeArena::make_mmio_wire_view(TypePtr inner) {
    auto t = std::unique_ptr<Type>(new Type(TypeKind::MmioWireView));
    t->inner_ = inner;
    auto* p = t.get();
    owned_.push_back(std::move(t));
    return p;
}

TypePtr TypeArena::make_interrupt_handler(TypePtr inner) {
    auto t = std::unique_ptr<Type>(new Type(TypeKind::InterruptHandler));
    t->inner_ = inner;
    auto* p = t.get();
    owned_.push_back(std::move(t));
    return p;
}

TypePtr TypeArena::make_padded(TypePtr inner) {
    auto t = std::unique_ptr<Type>(new Type(TypeKind::Padded));
    t->inner_ = inner;
    auto* p = t.get();
    owned_.push_back(std::move(t));
    return p;
}

TypePtr TypeArena::make_sysreg_handle(TypePtr inner) {
    auto t = std::unique_ptr<Type>(new Type(TypeKind::SysregHandle));
    t->inner_ = inner;
    auto* p = t.get();
    owned_.push_back(std::move(t));
    return p;
}

TypePtr TypeArena::make_sysreg_handle_ro(TypePtr inner) {
    auto t = std::unique_ptr<Type>(new Type(TypeKind::SysregHandleRO));
    t->inner_ = inner;
    auto* p = t.get();
    owned_.push_back(std::move(t));
    return p;
}

TypePtr TypeArena::make_sysreg_handle_wo(TypePtr inner) {
    auto t = std::unique_ptr<Type>(new Type(TypeKind::SysregHandleWO));
    t->inner_ = inner;
    auto* p = t.get();
    owned_.push_back(std::move(t));
    return p;
}

TypePtr TypeArena::make_per_cpu(TypePtr inner) {
    auto t = std::unique_ptr<Type>(new Type(TypeKind::PerCpu));
    t->inner_ = inner;
    auto* p = t.get();
    owned_.push_back(std::move(t));
    return p;
}

TypePtr TypeArena::make_zip_iter(TypePtr elem_a, TypePtr elem_b) {
    auto t = std::unique_ptr<Type>(new Type(TypeKind::ZipIter));
    t->parts_ = {elem_a, elem_b};
    auto* p = t.get();
    owned_.push_back(std::move(t));
    return p;
}

TypePtr TypeArena::make_take_iter(TypePtr elem) {
    auto t = std::unique_ptr<Type>(new Type(TypeKind::TakeIter));
    t->inner_ = elem;
    auto* p = t.get();
    owned_.push_back(std::move(t));
    return p;
}

TypePtr TypeArena::make_map_iter(TypePtr elem_in, TypePtr elem_out) {
    auto t = std::unique_ptr<Type>(new Type(TypeKind::MapIter));
    t->parts_ = {elem_in, elem_out};
    auto* p = t.get();
    owned_.push_back(std::move(t));
    return p;
}

TypePtr TypeArena::make_filter_iter(TypePtr elem) {
    auto t = std::unique_ptr<Type>(new Type(TypeKind::FilterIter));
    t->inner_ = elem;
    auto* p = t.get();
    owned_.push_back(std::move(t));
    return p;
}

TypePtr TypeArena::make_chunk_iter(TypePtr elem) {
    auto t = std::unique_ptr<Type>(new Type(TypeKind::ChunkIter));
    t->inner_ = elem;
    auto* p = t.get();
    owned_.push_back(std::move(t));
    return p;
}

TypePtr TypeArena::make_atomic(TypePtr inner) {
    auto t = std::unique_ptr<Type>(new Type(TypeKind::Atomic));
    t->inner_ = inner;
    auto* p = t.get();
    owned_.push_back(std::move(t));
    return p;
}

TypePtr TypeArena::make_cas_result(TypePtr inner) {
    auto t = std::unique_ptr<Type>(new Type(TypeKind::CasResult));
    t->inner_ = inner;
    auto* p = t.get();
    owned_.push_back(std::move(t));
    return p;
}

TypePtr TypeArena::make_atomic_tagged_pointer(TypePtr inner) {
    auto t = std::unique_ptr<Type>(new Type(TypeKind::AtomicTaggedPointer));
    t->inner_ = inner;
    auto* p = t.get();
    owned_.push_back(std::move(t));
    return p;
}

TypePtr TypeArena::make_result(TypePtr success, TypePtr error) {
    auto t = std::unique_ptr<Type>(new Type(TypeKind::Result));
    t->inner_ = success;
    t->result_ = error;
    auto* p = t.get();
    owned_.push_back(std::move(t));
    return p;
}

TypePtr TypeArena::make_vector(std::int64_t length, TypePtr element) {
    auto t = std::unique_ptr<Type>(new Type(TypeKind::Vector));
    t->length_ = length;
    t->inner_ = element;
    auto* p = t.get();
    owned_.push_back(std::move(t));
    return p;
}

TypePtr TypeArena::make_vector_symbolic(std::string length_name, TypePtr element) {
    auto t = std::unique_ptr<Type>(new Type(TypeKind::Vector));
    t->length_ = 0;
    t->name_ = std::move(length_name);
    t->inner_ = element;
    auto* p = t.get();
    owned_.push_back(std::move(t));
    return p;
}

TypePtr TypeArena::make_const_value(std::int64_t value) {
    auto t = std::unique_ptr<Type>(new Type(TypeKind::ConstValue));
    t->length_ = value;
    auto* p = t.get();
    owned_.push_back(std::move(t));
    return p;
}

TypePtr TypeArena::make_function(std::vector<TypePtr> params, TypePtr result) {
    auto t = std::unique_ptr<Type>(new Type(TypeKind::Function));
    t->parts_ = std::move(params);
    t->result_ = result;
    auto* p = t.get();
    owned_.push_back(std::move(t));
    return p;
}

TypePtr TypeArena::make_tuple(std::vector<TypePtr> elements) {
    auto t = std::unique_ptr<Type>(new Type(TypeKind::Tuple));
    t->parts_ = std::move(elements);
    auto* p = t.get();
    owned_.push_back(std::move(t));
    return p;
}

TypePtr TypeArena::make_nominal(TypeKind k, const ast::Decl* decl) {
    auto t = std::unique_ptr<Type>(new Type(k));
    t->decl_ = decl;
    auto* p = t.get();
    owned_.push_back(std::move(t));
    return p;
}

TypePtr TypeArena::make_struct_instance(const ast::Decl* decl, std::vector<TypePtr> args) {
    auto t = std::unique_ptr<Type>(new Type(TypeKind::Struct));
    t->decl_ = decl;
    t->parts_ = std::move(args);
    auto* p = t.get();
    owned_.push_back(std::move(t));
    return p;
}

TypePtr TypeArena::make_enum_instance(const ast::Decl* decl, std::vector<TypePtr> args) {
    auto t = std::unique_ptr<Type>(new Type(TypeKind::Enum));
    t->decl_ = decl;
    t->parts_ = std::move(args);
    auto* p = t.get();
    owned_.push_back(std::move(t));
    return p;
}

TypePtr TypeArena::make_generic_param(std::string name) {
    auto t = std::unique_ptr<Type>(new Type(TypeKind::GenericParam));
    t->name_ = std::move(name);
    auto* p = t.get();
    owned_.push_back(std::move(t));
    return p;
}

TypeKind TypeArena::primitive_kind_by_name(std::string_view name) noexcept {
    static const std::unordered_map<std::string_view, TypeKind> m = {
        {"Int8", TypeKind::Int8},
        {"Int16", TypeKind::Int16},
        {"Int32", TypeKind::Int32},
        {"Int64", TypeKind::Int64},
        {"Int", TypeKind::Int},
        {"UInt8", TypeKind::UInt8},
        {"UInt16", TypeKind::UInt16},
        {"UInt32", TypeKind::UInt32},
        {"UInt64", TypeKind::UInt64},
        {"UInt", TypeKind::UInt},
        {"Int128", TypeKind::Int128},
        {"UInt128", TypeKind::UInt128},
        {"Float32", TypeKind::Float32},
        {"Float64", TypeKind::Float64},
        {"Bool", TypeKind::Bool},
        {"Char", TypeKind::Char},
        {"Unit", TypeKind::Unit},
        {"String", TypeKind::String},
        {"Str", TypeKind::Str},
        {"StrConst", TypeKind::StrConst},
        {"Duration", TypeKind::Duration},  // §11 Swift-like time span
        {"Expr", TypeKind::AstExpr},       // §12.4 macro AST-value type
        // §10 `Never` is normally inferred from diverging expressions
        // (panic / throw / unreachable), but `-> Never` is admissible
        // as an explicit annotation — the §A10 @panic_handler is the
        // first source-level spelling that needs it.
        {"Never", TypeKind::Never},
    };
    auto it = m.find(name);
    return it == m.end() ? TypeKind::Error : it->second;
}

bool TypeArena::equal(TypePtr a, TypePtr b) noexcept {
    if (a == b) {
        return true;
    }
    if (a == nullptr || b == nullptr) {
        return false;
    }
    if (a->kind() != b->kind()) {
        return false;
    }
    switch (a->kind()) {
    case TypeKind::Optional:
    case TypeKind::Box:
    case TypeKind::Future:
    case TypeKind::Channel:
    case TypeKind::Span:
    case TypeKind::MutSpan:
    case TypeKind::Ptr:
    case TypeKind::MutPtr:
    case TypeKind::MmioView:
    case TypeKind::MmioRegion:
    case TypeKind::MmioWireView:
    case TypeKind::PerCpu:
    case TypeKind::InterruptHandler:
    case TypeKind::Padded:
    case TypeKind::SysregHandle:
    case TypeKind::SysregHandleRO:
    case TypeKind::SysregHandleWO:
    case TypeKind::TakeIter:
    case TypeKind::FilterIter:
    case TypeKind::ChunkIter:
    case TypeKind::Atomic:
    case TypeKind::CasResult:
    case TypeKind::AtomicTaggedPointer:
        return equal(a->inner(), b->inner());
    case TypeKind::ZipIter:
    case TypeKind::MapIter: {
        const auto& pa = a->parts();
        const auto& pb = b->parts();
        return pa.size() == pb.size() && pa.size() == 2 && equal(pa[0], pb[0])
               && equal(pa[1], pb[1]);
    }
    case TypeKind::Result:
        return equal(a->inner(), b->inner()) && equal(a->result(), b->result());
    case TypeKind::Vector:
        // §7 generics phase 2 — symbolic-length vectors compare by name; a
        // symbolic and a concrete-length vector are never equal.
        if (!a->vector_length_name().empty() || !b->vector_length_name().empty()) {
            return a->vector_length_name() == b->vector_length_name()
                   && equal(a->inner(), b->inner());
        }
        return a->vector_length() == b->vector_length() && equal(a->inner(), b->inner());
    case TypeKind::ConstValue:
        return a->const_value() == b->const_value();
    case TypeKind::Function:
        if (!equal(a->result(), b->result())) {
            return false;
        }
        [[fallthrough]];
    case TypeKind::Tuple: {
        const auto& pa = a->parts();
        const auto& pb = b->parts();
        if (pa.size() != pb.size()) {
            return false;
        }
        for (std::size_t i = 0; i < pa.size(); ++i) {
            if (!equal(pa[i], pb[i])) {
                return false;
            }
        }
        return true;
    }
    case TypeKind::Struct: {
        // §7 generics phase 2 — two struct types are equal when they
        // share a decl AND their recorded type arguments match. A bare
        // (non-generic) struct has empty parts on both sides.
        if (a->nominal_decl() != b->nominal_decl()) {
            return false;
        }
        const auto& pa = a->parts();
        const auto& pb = b->parts();
        if (pa.size() != pb.size()) {
            return false;
        }
        for (std::size_t i = 0; i < pa.size(); ++i) {
            if (!equal(pa[i], pb[i])) {
                return false;
            }
        }
        return true;
    }
    case TypeKind::Enum: {
        // §7 generics phase 2 — like Struct, an enum instance compares
        // equal only when the decl AND the recorded type arguments match.
        if (a->nominal_decl() != b->nominal_decl()) {
            return false;
        }
        const auto& pa = a->parts();
        const auto& pb = b->parts();
        if (pa.size() != pb.size()) {
            return false;
        }
        for (std::size_t i = 0; i < pa.size(); ++i) {
            if (!equal(pa[i], pb[i])) {
                return false;
            }
        }
        return true;
    }
    case TypeKind::Protocol:
    case TypeKind::OpaqueType:
        return a->nominal_decl() == b->nominal_decl();
    case TypeKind::GenericParam:
        return a->generic_name() == b->generic_name();
    default:
        return true;  // primitives compared equal by pointer at the top
    }
}

bool TypeArena::assignable(TypePtr from, TypePtr to) noexcept {
    if (from == nullptr || to == nullptr) {
        return true;
    }  // unresolved — don't pile on
    if (from->is_error() || to->is_error()) {
        return true;
    }
    if (from->is_never()) {
        return true;
    }  // diverging fits anywhere
    // §17 `Int` and `UInt` are the platform-natural-width integer
    // types — the right choice when the developer doesn't care about
    // exact width (loop counters, sizes, indices). When such a value
    // flows into a specific-width slot (`Int32`, `UInt64`, …), the
    // intent is clear and the codegen emits the matching static_cast;
    // forcing an explicit `Int32(i)` everywhere would be miserable
    // ergonomics for the most common patterns. Specific-width types
    // remain *mutually* incompatible — `Int8` does not implicitly
    // become `Int32` — so the rule narrowly covers the natural-vs-
    // sized direction without admitting accidental cross-width
    // assignments. The reverse direction (any specific width → `Int`
    // / `UInt`) is symmetric: ordinary widening into the natural type.
    if (from->is_integer() && to->is_integer()) {
        const bool from_natural = from->kind() == TypeKind::Int || from->kind() == TypeKind::UInt;
        const bool to_natural = to->kind() == TypeKind::Int || to->kind() == TypeKind::UInt;
        // Either side is the natural-width type — admit the conversion.
        // Signedness is still tracked: `Int` doesn't flow into `UInt32`
        // implicitly (a sign change is a real concern; require the
        // explicit `UInt32(i)` for that).
        if (from_natural || to_natural) {
            const bool from_signed =
                from->kind() == TypeKind::Int || from->kind() == TypeKind::Int8
                || from->kind() == TypeKind::Int16 || from->kind() == TypeKind::Int32
                || from->kind() == TypeKind::Int64 || from->kind() == TypeKind::Int128;
            const bool to_signed = to->kind() == TypeKind::Int || to->kind() == TypeKind::Int8
                                   || to->kind() == TypeKind::Int16 || to->kind() == TypeKind::Int32
                                   || to->kind() == TypeKind::Int64
                                   || to->kind() == TypeKind::Int128;
            if (from_signed == to_signed) {
                return true;
            }
        }
    }
    // §9 Optional ergonomics. Two implicit conversions:
    //   1. `T → Optional<T>` — passing a value where an optional is
    //      expected wraps in `.some(...)`. std::optional's converting
    //      ctor makes this a no-op in C++.
    //   2. `Optional<Never> → Optional<T>` — the resolver hands out
    //      `Optional<Never>` for an unconstrained `nil` literal, and
    //      that needs to fit any `Optional<T>` slot.
    if (to->kind() == TypeKind::Optional) {
        if (from->kind() == TypeKind::Optional && from->inner() != nullptr
            && from->inner()->is_never()) {
            return true;
        }
        if (from->kind() != TypeKind::Optional && assignable(from, to->inner())) {
            return true;
        }
    }
    // §10 borrowed views: a fixed-length array `[N]T` is implicitly
    // viewable as a Span[T] or MutSpan[T] (C++'s std::span has the
    // matching converting ctor from std::array<T, N>). A MutSpan[T]
    // also widens to Span[T] (mutable → immutable view), which is the
    // standard read-only-borrow pattern.
    if (to->kind() == TypeKind::Span) {
        if (from->kind() == TypeKind::Vector && equal(from->inner(), to->inner())) {
            return true;
        }
        if (from->kind() == TypeKind::MutSpan && equal(from->inner(), to->inner())) {
            return true;
        }
    }
    if (to->kind() == TypeKind::MutSpan) {
        if (from->kind() == TypeKind::Vector && equal(from->inner(), to->inner())) {
            return true;
        }
    }
    // §A4 (§14.9) Atomic[T] accepts an initial-value of T at
    // construction (`static counter: Atomic[UInt32] = 0`). The C++
    // side already supports this via std::atomic<T>'s implicit ctor.
    if (to->kind() == TypeKind::Atomic && to->inner() != nullptr) {
        if (assignable(from, to->inner())) {
            return true;
        }
    }
    // §A8 (§14.5.3) vector-table slot. An InterruptHandler<T> slot
    // accepts any function whose source type is `(T) -> Unit`. The
    // @interrupt attribute and inout-param requirement are validated
    // at the function definition site already; the type system here
    // is the shape gate so a stray helper with the wrong signature
    // fails at the array-literal assignment, not deep in codegen.
    if (to->kind() == TypeKind::InterruptHandler && to->inner() != nullptr
        && from->kind() == TypeKind::Function && from->parts().size() == 1
        && from->result() != nullptr && from->result()->kind() == TypeKind::Unit
        && equal(from->parts()[0], to->inner())) {
        return true;
    }
    // §4 string lattice. A string literal is `StrConst`; the borrowed view
    // `Str` and the owning `String` are wider. Every narrowing→widening step
    // is a safe C++ conversion: StrConst/Str both lower to std::string_view
    // (identity), and a string_view flows into std::string via its converting
    // constructor. So StrConst → Str → String and StrConst → String all hold;
    // the reverse (String → Str) does not, since a string_view borrowing a
    // temporary std::string would dangle.
    {
        auto is_strish = [](TypeKind k) {
            return k == TypeKind::StrConst || k == TypeKind::Str || k == TypeKind::String;
        };
        auto rank = [](TypeKind k) -> int {
            switch (k) {
            case TypeKind::StrConst:
                return 0;
            case TypeKind::Str:
                return 1;
            case TypeKind::String:
                return 2;
            default:
                return -1;
            }
        };
        if (is_strish(from->kind()) && is_strish(to->kind())
            && rank(from->kind()) <= rank(to->kind())) {
            return true;
        }
    }
    return equal(from, to);
}

TypePtr TypeArena::substitute(TypePtr t, const std::unordered_map<std::string, TypePtr>& bindings) {
    if (t == nullptr || bindings.empty()) {
        return t;
    }
    switch (t->kind()) {
    case TypeKind::GenericParam: {
        auto it = bindings.find(std::string{t->generic_name()});
        return it == bindings.end() ? t : it->second;
    }
    case TypeKind::Optional: {
        auto inner = substitute(t->inner(), bindings);
        return inner == t->inner() ? t : make_optional(inner);
    }
    case TypeKind::Box: {
        auto inner = substitute(t->inner(), bindings);
        return inner == t->inner() ? t : make_box(inner);
    }
    case TypeKind::Future: {
        auto inner = substitute(t->inner(), bindings);
        return inner == t->inner() ? t : make_future(inner);
    }
    case TypeKind::Channel: {
        auto inner = substitute(t->inner(), bindings);
        return inner == t->inner() ? t : make_channel(inner);
    }
    case TypeKind::Span: {
        auto inner = substitute(t->inner(), bindings);
        return inner == t->inner() ? t : make_span(inner);
    }
    case TypeKind::MutSpan: {
        auto inner = substitute(t->inner(), bindings);
        return inner == t->inner() ? t : make_mut_span(inner);
    }
    case TypeKind::Ptr: {
        auto inner = substitute(t->inner(), bindings);
        return inner == t->inner() ? t : make_ptr(inner);
    }
    case TypeKind::MutPtr: {
        auto inner = substitute(t->inner(), bindings);
        return inner == t->inner() ? t : make_mut_ptr(inner);
    }
    case TypeKind::MmioView: {
        auto inner = substitute(t->inner(), bindings);
        return inner == t->inner() ? t : make_mmio_view(inner);
    }
    case TypeKind::MmioRegion: {
        auto inner = substitute(t->inner(), bindings);
        return inner == t->inner() ? t : make_mmio_region(inner);
    }
    case TypeKind::MmioWireView: {
        auto inner = substitute(t->inner(), bindings);
        return inner == t->inner() ? t : make_mmio_wire_view(inner);
    }
    case TypeKind::PerCpu: {
        auto inner = substitute(t->inner(), bindings);
        return inner == t->inner() ? t : make_per_cpu(inner);
    }
    case TypeKind::InterruptHandler: {
        auto inner = substitute(t->inner(), bindings);
        return inner == t->inner() ? t : make_interrupt_handler(inner);
    }
    case TypeKind::Padded: {
        auto inner = substitute(t->inner(), bindings);
        return inner == t->inner() ? t : make_padded(inner);
    }
    case TypeKind::SysregHandle: {
        auto inner = substitute(t->inner(), bindings);
        return inner == t->inner() ? t : make_sysreg_handle(inner);
    }
    case TypeKind::SysregHandleRO: {
        auto inner = substitute(t->inner(), bindings);
        return inner == t->inner() ? t : make_sysreg_handle_ro(inner);
    }
    case TypeKind::SysregHandleWO: {
        auto inner = substitute(t->inner(), bindings);
        return inner == t->inner() ? t : make_sysreg_handle_wo(inner);
    }
    case TypeKind::TakeIter: {
        auto inner = substitute(t->inner(), bindings);
        return inner == t->inner() ? t : make_take_iter(inner);
    }
    case TypeKind::FilterIter: {
        auto inner = substitute(t->inner(), bindings);
        return inner == t->inner() ? t : make_filter_iter(inner);
    }
    case TypeKind::ChunkIter: {
        auto inner = substitute(t->inner(), bindings);
        return inner == t->inner() ? t : make_chunk_iter(inner);
    }
    case TypeKind::Atomic: {
        auto inner = substitute(t->inner(), bindings);
        return inner == t->inner() ? t : make_atomic(inner);
    }
    case TypeKind::CasResult: {
        auto inner = substitute(t->inner(), bindings);
        return inner == t->inner() ? t : make_cas_result(inner);
    }
    case TypeKind::AtomicTaggedPointer: {
        auto inner = substitute(t->inner(), bindings);
        return inner == t->inner() ? t : make_atomic_tagged_pointer(inner);
    }
    case TypeKind::ZipIter: {
        if (t->parts().size() != 2) {
            return t;
        }
        auto a = substitute(t->parts()[0], bindings);
        auto b = substitute(t->parts()[1], bindings);
        return (a == t->parts()[0] && b == t->parts()[1]) ? t : make_zip_iter(a, b);
    }
    case TypeKind::MapIter: {
        if (t->parts().size() != 2) {
            return t;
        }
        auto a = substitute(t->parts()[0], bindings);
        auto b = substitute(t->parts()[1], bindings);
        return (a == t->parts()[0] && b == t->parts()[1]) ? t : make_map_iter(a, b);
    }
    case TypeKind::Result: {
        auto ok = substitute(t->inner(), bindings);
        auto err = substitute(t->result(), bindings);
        return (ok == t->inner() && err == t->result()) ? t : make_result(ok, err);
    }
    case TypeKind::Vector: {
        auto elem = substitute(t->inner(), bindings);
        // §7 generics phase 2 — a symbolic-length vector `[N]T` concretizes
        // to `[8]T` once N binds to a ConstValue; otherwise it stays
        // symbolic (with its element possibly substituted).
        if (!t->vector_length_name().empty()) {
            auto it = bindings.find(std::string{t->vector_length_name()});
            if (it != bindings.end() && it->second != nullptr
                && it->second->kind() == TypeKind::ConstValue) {
                return make_vector(it->second->const_value(), elem);
            }
            return elem == t->inner()
                       ? t
                       : make_vector_symbolic(std::string{t->vector_length_name()}, elem);
        }
        return elem == t->inner() ? t : make_vector(t->vector_length(), elem);
    }
    case TypeKind::Tuple: {
        std::vector<TypePtr> elems;
        elems.reserve(t->parts().size());
        bool changed = false;
        for (auto* p : t->parts()) {
            auto s = substitute(p, bindings);
            changed = changed || s != p;
            elems.push_back(s);
        }
        return changed ? make_tuple(std::move(elems)) : t;
    }
    case TypeKind::Function: {
        std::vector<TypePtr> params;
        params.reserve(t->parts().size());
        bool changed = false;
        for (auto* p : t->parts()) {
            auto s = substitute(p, bindings);
            changed = changed || s != p;
            params.push_back(s);
        }
        auto result = substitute(t->result(), bindings);
        changed = changed || result != t->result();
        return changed ? make_function(std::move(params), result) : t;
    }
    case TypeKind::Struct: {
        // §7 generics phase 2 — a struct instance carries its type
        // arguments in parts_; substitute through them so an instance
        // built with a generic param (e.g. a field typed `Inner[T]`)
        // specializes when T is bound. A bare struct (empty parts) is
        // unchanged.
        if (t->parts().empty()) {
            return t;
        }
        std::vector<TypePtr> args;
        args.reserve(t->parts().size());
        bool changed = false;
        for (auto* p : t->parts()) {
            auto s = substitute(p, bindings);
            changed = changed || s != p;
            args.push_back(s);
        }
        return changed ? make_struct_instance(t->nominal_decl(), std::move(args)) : t;
    }
    case TypeKind::Enum: {
        // §7 generics phase 2 — substitute through an enum instance's type
        // arguments (the enum-case-constructor return type is built with
        // the params as placeholders, so this is what specializes
        // `Option[T]` to `Option[Int]` during call inference).
        if (t->parts().empty()) {
            return t;
        }
        std::vector<TypePtr> args;
        args.reserve(t->parts().size());
        bool changed = false;
        for (auto* p : t->parts()) {
            auto s = substitute(p, bindings);
            changed = changed || s != p;
            args.push_back(s);
        }
        return changed ? make_enum_instance(t->nominal_decl(), std::move(args)) : t;
    }
    default:
        // Primitives, nominals, Never, Error — no generic params inside.
        return t;
    }
}

}  // namespace vestra::sema
