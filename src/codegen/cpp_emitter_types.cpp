// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// Type emission: lowering resolver-canonicalized sema::TypePtr values
// and AST type-expressions to their C++ spelling.
// Split out of cpp_emitter.cpp; see cpp_emitter_internal.hpp for the
// shared free helpers.

#include "vestra/codegen/cpp_emitter.hpp"

#include "cpp_emitter_internal.hpp"

#include <format>
#include <ostream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vestra::codegen {

using detail::primitive_map;

void CppEmitter::emit_nominal_qualifier(std::ostream& os, const ast::Decl* decl) {
    if (auto it = imported_qualifiers_.find(decl); it != imported_qualifiers_.end()) {
        os << it->second << "::";
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
    case TypeKind::Int128:
        os << "__int128_t";
        return;
    case TypeKind::UInt128:
        os << "__uint128_t";
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
    case TypeKind::Future:
        os << "__vstr::Future<";
        emit_sema_type(os, t->inner());
        os << ">";
        return;
    case TypeKind::Channel:
        os << "__vstr::Channel<";
        emit_sema_type(os, t->inner());
        os << ">";
        return;
    case TypeKind::Vec:
        os << "std::vector<";
        emit_sema_type(os, t->inner());
        os << ">";
        return;
    case TypeKind::HashMap:
        os << "std::unordered_map<";
        emit_sema_type(os, t->parts()[0]);
        os << ", ";
        emit_sema_type(os, t->parts()[1]);
        os << ">";
        return;
    case TypeKind::Duration:
        os << "__vstr::Duration";
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
    case TypeKind::Ptr:
        // §A3 raw read-only pointer.
        os << "const ";
        emit_sema_type(os, t->inner());
        os << "*";
        return;
    case TypeKind::MutPtr:
        emit_sema_type(os, t->inner());
        os << "*";
        return;
    case TypeKind::MmioView:
        os << "__vstr::MmioView<";
        emit_sema_type(os, t->inner());
        os << ">";
        return;
    case TypeKind::MmioRegion:
        os << "__vstr::MmioRegion<";
        emit_sema_type(os, t->inner());
        os << ">";
        return;
    case TypeKind::MmioWireView:
        os << "__vstr::MmioWireView<";
        emit_sema_type(os, t->inner());
        os << ">";
        return;
    case TypeKind::PerCpu:
        os << "__vstr::PerCpu<";
        emit_sema_type(os, t->inner());
        os << ">";
        return;
    case TypeKind::Padded:
        os << "__vstr::Padded<";
        emit_sema_type(os, t->inner());
        os << ">";
        return;
    case TypeKind::SysregHandle:
    case TypeKind::SysregHandleRO:
    case TypeKind::SysregHandleWO:
        // §14.12 — all three handle kinds emit the same C++ runtime
        // template. The read-only / write-only distinction lives at
        // sema time (methods absent from lookup_method on the wrong
        // kind); the C++ template carries both operations
        // structurally so the codegen emission is uniform.
        os << "__vstr::sysreg::Handle<";
        emit_sema_type(os, t->inner());
        os << ">";
        return;
    case TypeKind::InterruptHandler:
        // §A8 (§14.5.3) vector-table slot — `void(*)(T&)`. Type-id
        // form; the declarator-name form is emit_type_with_name's
        // job. This branch fires inside `std::array<…, N>` template
        // args at the static decl site, where we want the type-id.
        os << "void(*)(";
        emit_sema_type(os, t->inner());
        os << "&)";
        return;
    case TypeKind::Atomic:
        os << "std::atomic<";
        emit_sema_type(os, t->inner());
        os << ">";
        return;
    case TypeKind::CasResult:
        os << "__vstr::CASResult<";
        emit_sema_type(os, t->inner());
        os << ">";
        return;
    case TypeKind::AtomicTaggedPointer:
        os << "__vstr::AtomicTaggedPointer<";
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
        // §7 generics phase 2 — symbolic-length vectors carry the const
        // generic's name; concrete-length vectors the integer.
        os << ", ";
        if (!t->vector_length_name().empty()) {
            os << t->vector_length_name();
        } else {
            os << t->vector_length();
        }
        os << ">";
        return;
    case TypeKind::ConstValue:
        // §7 generics phase 2 — a const generic argument lowers to its
        // integer literal in template-argument position (`Buffer<T, 8>`).
        os << t->const_value();
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
    case TypeKind::Function:
        // §A12 (§14.6.3) Vestra function-pointer type. The spelling
        // is `(T1, T2, ...) -> R`; the C++ lowering is the function-
        // pointer `R(*)(const T1&, const T2&, ...)`. The `const T&`
        // wrapping mirrors how func decls lower their default `read`
        // mode parameters, so `&helper` types compatibly with a
        // `(T) -> R` slot at the C++ ABI level. Sema gives the same
        // TypeKind::Function to closures, but their codegen path is
        // a lambda with deduced type — this emit_sema_type branch
        // fires only on places where the function type is named
        // (let-bindings, parameters, struct fields, vector tables).
        emit_sema_type(os, t->result());
        os << "(*)(";
        for (std::size_t i = 0; i < t->parts().size(); ++i) {
            if (i != 0) {
                os << ", ";
            }
            os << "const ";
            emit_sema_type(os, t->parts()[i]);
            os << "&";
        }
        os << ")";
        return;
    case TypeKind::Struct:
    case TypeKind::Enum:
    case TypeKind::Protocol:
    case TypeKind::OpaqueType:
        if (const auto* decl = t->nominal_decl()) {
            switch (decl->kind) {
            case ast::NodeKind::Struct: {
                const auto& sd = static_cast<const ast::StructDecl&>(*decl);
                // §A7 (§14.14) — the builtin Context struct lives in
                // the runtime namespace (it has no user-visible
                // fields and its layout is target-determined).
                if (sd.name == "Context" && sd.fields.empty()) {
                    os << "__vstr::Context";
                } else {
                    emit_nominal_qualifier(os, decl);
                    os << sd.name;
                    // §7 generics phase 2 — a struct instance carries its
                    // resolved type arguments in parts_; emit `Pair<Int32>`
                    // so the C++ template name matches the decl emission.
                    if (!t->parts().empty()) {
                        os << "<";
                        for (std::size_t i = 0; i < t->parts().size(); ++i) {
                            if (i != 0) {
                                os << ", ";
                            }
                            emit_sema_type(os, t->parts()[i]);
                        }
                        os << ">";
                    }
                }
                return;
            }
            case ast::NodeKind::Enum: {
                // §A4 (§14.9.1) the builtin Ordering enum lowers to
                // `std::memory_order` at the C++ layer; cases lower
                // through the LeadingDotExpr special-case below.
                const auto& ed = static_cast<const ast::EnumDecl&>(*decl);
                if (ed.name == "Ordering") {
                    os << "std::memory_order";
                } else {
                    emit_nominal_qualifier(os, decl);
                    os << ed.name;
                    // §7 generics phase 2 — append `<args>` for a generic
                    // enum instance so the template name matches the decl.
                    if (!t->parts().empty()) {
                        os << "<";
                        for (std::size_t i = 0; i < t->parts().size(); ++i) {
                            if (i != 0) {
                                os << ", ";
                            }
                            emit_sema_type(os, t->parts()[i]);
                        }
                        os << ">";
                    }
                }
                return;
            }
            case ast::NodeKind::Protocol:
                emit_nominal_qualifier(os, decl);
                os << static_cast<const ast::ProtocolDecl&>(*decl).name;
                return;
            case ast::NodeKind::Opaque:
                emit_nominal_qualifier(os, decl);
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

void CppEmitter::emit_type_with_name(std::ostream& os,
                                     const ast::Type& t,
                                     std::string_view name,
                                     std::string_view trailing_qual) {
    // §A12 (§14.6.3). C++ function-pointer declarators need the name
    // inside the parens (`R(*name)(T1)`). `emit_type` produces the
    // type-id form (`R(*)(T1)`), which is valid in type-alias and
    // template-argument positions but not in variable / parameter
    // declarations. This helper rewrites the function-pointer case
    // so the declarator is well-formed; every other type falls
    // through to `emit_type` + " name".
    if (t.kind == ast::NodeKind::FunctionType) {
        const auto& fn = static_cast<const ast::FunctionType&>(t);
        if (fn.result) {
            emit_type(os, *fn.result);
        } else {
            os << "void";
        }
        os << "(*" << trailing_qual << name << ")(";
        for (std::size_t i = 0; i < fn.params.size(); ++i) {
            if (i != 0) {
                os << ", ";
            }
            if (fn.params[i]) {
                os << "const ";
                emit_type(os, *fn.params[i]);
                os << "&";
            }
        }
        os << ")";
        return;
    }
    if (t.kind == ast::NodeKind::InterruptType) {
        // §A8 (§14.5.3) vector-table slot. Lowers to `void(*name)(T&)`
        // — the trap-frame is passed by mutable reference because
        // the ISR mutates register state in place. The declarator
        // embeds the variable name inside the parens, matching the
        // function-pointer-declarator pattern from §A12.
        const auto& it = static_cast<const ast::InterruptType&>(t);
        os << "void(*" << trailing_qual << name << ")(";
        if (it.trap_frame) {
            emit_type(os, *it.trap_frame);
            os << "&";
        }
        os << ")";
        return;
    }
    emit_type(os, t);
    if (!trailing_qual.empty()) {
        os << " " << trailing_qual;
    }
    os << " " << name;
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
            // §11 Future[T] → the runtime Future shim.
            if (n.path[0] == "Future" && n.type_args.size() == 1) {
                os << "__vstr::Future<";
                emit_type(os, *n.type_args[0]);
                os << ">";
                return;
            }
            // §11 Channel[T] → the runtime Channel shim.
            if (n.path[0] == "Channel" && n.type_args.size() == 1) {
                os << "__vstr::Channel<";
                emit_type(os, *n.type_args[0]);
                os << ">";
                return;
            }
            // §18.5 Vec[T] → std::vector<T>.
            if (n.path[0] == "Vec" && n.type_args.size() == 1) {
                os << "std::vector<";
                emit_type(os, *n.type_args[0]);
                os << ">";
                return;
            }
            // §18.5 HashMap[K, V] → std::unordered_map<K, V>.
            if (n.path[0] == "HashMap" && n.type_args.size() == 2) {
                os << "std::unordered_map<";
                emit_type(os, *n.type_args[0]);
                os << ", ";
                emit_type(os, *n.type_args[1]);
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
            // §A3 (§10.5) `Ptr[T]` / `MutPtr[T]` lower to const T* / T*.
            if (n.path[0] == "Ptr" && n.type_args.size() == 1) {
                os << "const ";
                emit_type(os, *n.type_args[0]);
                os << "*";
                return;
            }
            if (n.path[0] == "MutPtr" && n.type_args.size() == 1) {
                emit_type(os, *n.type_args[0]);
                os << "*";
                return;
            }
            // §A6 MMIO views — `MmioView[T]` and `MmioRegion[T]`.
            if (n.path[0] == "MmioView" && n.type_args.size() == 1) {
                os << "__vstr::MmioView<";
                emit_type(os, *n.type_args[0]);
                os << ">";
                return;
            }
            if (n.path[0] == "MmioRegion" && n.type_args.size() == 1) {
                os << "__vstr::MmioRegion<";
                emit_type(os, *n.type_args[0]);
                os << ">";
                return;
            }
            if (n.path[0] == "MmioWireView" && n.type_args.size() == 1) {
                os << "__vstr::MmioWireView<";
                emit_type(os, *n.type_args[0]);
                os << ">";
                return;
            }
            // §A11 (§14.8) `PerCpu[T]` lowers to __vstr::PerCpu<T>.
            if (n.path[0] == "PerCpu" && n.type_args.size() == 1) {
                os << "__vstr::PerCpu<";
                emit_type(os, *n.type_args[0]);
                os << ">";
                return;
            }
            // §A11 (§14.8) `Padded[T]` lowers to __vstr::Padded<T>.
            if (n.path[0] == "Padded" && n.type_args.size() == 1) {
                os << "__vstr::Padded<";
                emit_type(os, *n.type_args[0]);
                os << ">";
                return;
            }
            // §A7 (§14.14) builtin `Context` (no type args) lowers
            // to __vstr::Context. The user can only spell it bare;
            // shadowing isn't supported in v0.5.
            if (n.path[0] == "Context" && n.type_args.empty()) {
                os << "__vstr::Context";
                return;
            }
            // §11 `Duration` (no type args) → the runtime value type.
            if (n.path[0] == "Duration" && n.type_args.empty()) {
                os << "__vstr::Duration";
                return;
            }
            // §A4 (§14.9) `Atomic[T]` lowers to `std::atomic<T>`.
            if (n.path[0] == "Atomic" && n.type_args.size() == 1) {
                os << "std::atomic<";
                emit_type(os, *n.type_args[0]);
                os << ">";
                return;
            }
            // §A4 (§14.9.5) `AtomicTaggedPointer[T]` lowers to the
            // runtime template. The template holds an
            // `std::atomic<__uint128_t>` underneath and exposes a
            // typed (T*, uint64_t) load / compareExchange surface.
            if (n.path[0] == "AtomicTaggedPointer" && n.type_args.size() == 1) {
                os << "__vstr::AtomicTaggedPointer<";
                emit_type(os, *n.type_args[0]);
                os << ">";
                return;
            }
            // §A4 (§14.9.1) `Ordering` lowers to `std::memory_order`.
            // The Vestra cases `.relaxed`, `.acquire`, etc. emit as
            // `std::memory_order_…` literals via the LeadingDotExpr
            // special-case (see emit_expr).
            if (n.path[0] == "Ordering" && n.type_args.empty()) {
                os << "std::memory_order";
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
        // §7 generics phase 2 — a symbolic length `[N]T` emits the const
        // generic's name (the enclosing template's non-type parameter).
        os << ", ";
        if (!v.length_ident.empty()) {
            os << v.length_ident;
        } else {
            os << v.length;
        }
        os << ">";
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
    case ast::NodeKind::FunctionType: {
        // §A12 (§14.6.3) function-pointer type spelling. Source-level
        // `(T1, T2) -> R` lowers to the C++ function-pointer
        // `R(*)(const T1&, const T2&)`. The `const T&` wrapping
        // mirrors the read-mode lowering of func decl parameters, so
        // `&helper` types compatibly with a `(T) -> R` slot at the
        // C++ ABI level. The using-row + throws-row are dropped here
        // (Vestra effects have no C++ type-level representation in
        // v0.5; they're audit metadata).
        const auto& fn = static_cast<const ast::FunctionType&>(t);
        if (fn.result) {
            emit_type(os, *fn.result);
        } else {
            os << "void";
        }
        os << "(*)(";
        for (std::size_t i = 0; i < fn.params.size(); ++i) {
            if (i != 0) {
                os << ", ";
            }
            if (fn.params[i]) {
                os << "const ";
                emit_type(os, *fn.params[i]);
                os << "&";
            }
        }
        os << ")";
        break;
    }
    case ast::NodeKind::InterruptType: {
        // §A8 (§14.5.3) vector-table slot type — type-id form, no
        // declarator name (`void(*)(T&)`). Useful for `using` aliases
        // and template-argument positions like `std::array<...>`.
        // The declarator form lives in `emit_type_with_name`.
        const auto& it = static_cast<const ast::InterruptType&>(t);
        os << "void(*)(";
        if (it.trap_frame) {
            emit_type(os, *it.trap_frame);
            os << "&";
        }
        os << ")";
        break;
    }
    default:
        os << "/*type*/auto";
        break;
    }
}

}  // namespace vestra::codegen
