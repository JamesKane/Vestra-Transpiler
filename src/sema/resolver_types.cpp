// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// Type-expression resolution: resolve_type, generic member-type
// substitution, and struct-field lookup.
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

TypePtr Resolver::resolve_type(const ast::Type& t) {
    switch (t.kind) {
    case ast::NodeKind::NamedType: {
        const auto& n = static_cast<const ast::NamedType&>(t);
        if (n.path.size() == 1) {
            // §10 builtin `Box[T]` — the unique-ownership heap pointer.
            // Recognized here before generic-symbol lookup so it never
            // needs to be in the user's symbol table.
            if (n.path[0] == "Box" && n.type_args.size() == 1) {
                return types_->make_box(n.type_args[0] ? resolve_type(*n.type_args[0])
                                                       : types_->error());
            }
            // §11 Future[T] — a `spawn`-produced result handle. Spellable as
            // a type so a future can be bound (`let f: Future[Int32] = …`).
            if (n.path[0] == "Future" && n.type_args.size() == 1) {
                return types_->make_future(n.type_args[0] ? resolve_type(*n.type_args[0])
                                                          : types_->error());
            }
            // §11 Channel[T] — a typed queue (`send` / `recv`).
            if (n.path[0] == "Channel" && n.type_args.size() == 1) {
                return types_->make_channel(n.type_args[0] ? resolve_type(*n.type_args[0])
                                                           : types_->error());
            }
            // §10 builtin `Span[T]` / `MutSpan[T]` — borrowed,
            // non-escapable views over a contiguous range of T. Lower
            // to `std::span<const T>` / `std::span<T>`. The implicit
            // `[N]T → Span[T]` coercion happens via TypeArena::assignable
            // (which the C++ side handles through std::span's
            // std::array-constructible ctor).
            if (n.path[0] == "Span" && n.type_args.size() == 1) {
                return types_->make_span(n.type_args[0] ? resolve_type(*n.type_args[0])
                                                        : types_->error());
            }
            if (n.path[0] == "MutSpan" && n.type_args.size() == 1) {
                return types_->make_mut_span(n.type_args[0] ? resolve_type(*n.type_args[0])
                                                            : types_->error());
            }
            // §A3 (§10.5) raw pointers `Ptr[T]` / `MutPtr[T]`. Both are
            // Trivial bit-pattern wrappers. v0.5 doesn't constrain T
            // — the kernel mints pointers to primitives, structs, and
            // capability tables alike. Dereferencing is the gated
            // operation (handled separately via Span.raw); the
            // pointer itself is freely escapable.
            if (n.path[0] == "Ptr" && n.type_args.size() == 1) {
                return types_->make_ptr(n.type_args[0] ? resolve_type(*n.type_args[0])
                                                       : types_->error());
            }
            if (n.path[0] == "MutPtr" && n.type_args.size() == 1) {
                return types_->make_mut_ptr(n.type_args[0] ? resolve_type(*n.type_args[0])
                                                           : types_->error());
            }
            // §A6 (§14.11) typed MMIO views. T must be primitive.
            if (n.path[0] == "MmioView" && n.type_args.size() == 1) {
                TypePtr inner = n.type_args[0] ? resolve_type(*n.type_args[0]) : types_->error();
                if (inner != nullptr && !inner->is_error() && !inner->is_primitive()) {
                    error_at(t.range,
                             std::format("MmioView[T] requires T to be a primitive, got {}",
                                         inner->describe()));
                    return types_->make_mmio_view(types_->error());
                }
                return types_->make_mmio_view(inner);
            }
            if (n.path[0] == "MmioRegion" && n.type_args.size() == 1) {
                TypePtr inner = n.type_args[0] ? resolve_type(*n.type_args[0]) : types_->error();
                if (inner != nullptr && !inner->is_error() && !inner->is_primitive()) {
                    error_at(t.range,
                             std::format("MmioRegion[T] requires T to be a primitive, got {}",
                                         inner->describe()));
                    return types_->make_mmio_region(types_->error());
                }
                return types_->make_mmio_region(inner);
            }
            // §A6 (§14.11.3) `MmioWireView[T]` — endianness-aware
            // MMIO view. Same T-must-be-primitive constraint as
            // MmioView; the swap happens at the read/write boundary
            // when the device's declared endianness differs from
            // the host's native.
            if (n.path[0] == "MmioWireView" && n.type_args.size() == 1) {
                TypePtr inner = n.type_args[0] ? resolve_type(*n.type_args[0]) : types_->error();
                if (inner != nullptr && !inner->is_error() && !inner->is_primitive()) {
                    error_at(t.range,
                             std::format("MmioWireView[T] requires T to be a primitive, got {}",
                                         inner->describe()));
                    return types_->make_mmio_wire_view(types_->error());
                }
                return types_->make_mmio_wire_view(inner);
            }
            // §A11 (§14.8) `PerCpu[T]` — per-hart slot. The spec
            // requires T: Trivial. v0.5 doesn't have a Trivial
            // protocol but the common kernel pattern is
            // `PerCpu[Atomic[T]]` for lock-free per-hart counters,
            // so primitives + Atomic[primitive] are admitted here;
            // any other shape is accepted defensively (codegen will
            // catch shapes that don't satisfy C++'s implicit
            // requirements).
            if (n.path[0] == "PerCpu" && n.type_args.size() == 1) {
                TypePtr inner = n.type_args[0] ? resolve_type(*n.type_args[0]) : types_->error();
                return types_->make_per_cpu(inner);
            }
            // §A11 (§14.8) `Padded[T]` — cache-line-padded wrapper.
            // Pads sizeof(T) up to cfg.option("cache_line_bytes")
            // (v0.5 hardcodes 64) so adjacent values can't share a
            // cache line. The kernel uses it inside the per-hart
            // array region (`[MAX_HARTS]Padded[T]`); also useful
            // standalone for ring-buffer cells.
            if (n.path[0] == "Padded" && n.type_args.size() == 1) {
                TypePtr inner = n.type_args[0] ? resolve_type(*n.type_args[0]) : types_->error();
                return types_->make_padded(inner);
            }
            // §A4 (§14.9) builtin `Atomic[T]` — compiler-known wrapper
            // over std::atomic<T>. T must be primitive for v0.5;
            // sema enforces that here so codegen never has to think
            // about a non-trivially-atomic T at the C++ layer.
            if (n.path[0] == "Atomic" && n.type_args.size() == 1) {
                TypePtr inner = n.type_args[0] ? resolve_type(*n.type_args[0]) : types_->error();
                if (inner != nullptr && !inner->is_error() && !inner->is_primitive()) {
                    error_at(t.range,
                             std::format("Atomic[T] requires T to be a primitive type, got {}",
                                         inner->describe()));
                    return types_->make_atomic(types_->error());
                }
                // §A4 (§14.9.4) wide-atomic target-feature gate.
                // Atomic[UInt128] / Atomic[Int128] need a native
                // 128-bit CAS — aarch64's +lse2 or x86_64's +cx16.
                // Without the matching feature the host compiler
                // would silently degrade to libatomic locks, which
                // changes the contract (no longer lock-free). The
                // hosted default (arch=host) admits unconditionally
                // since the host's standard library decides.
                if (inner != nullptr
                    && (inner->kind() == TypeKind::UInt128 || inner->kind() == TypeKind::Int128)) {
                    if (auto diag = check_wide_atomic_feature(t.range, "Atomic"); !diag.empty()) {
                        error_at(t.range, std::move(diag));
                        return types_->make_atomic(types_->error());
                    }
                }
                return types_->make_atomic(inner);
            }
            // §A4 (§14.9.5) builtin `AtomicTaggedPointer[T]` — typed
            // wide-atomic wrapper that builds an ABA-safe (Ptr[T],
            // tag) pair on top of Atomic[UInt128]. T is the pointee
            // type; v0.5 admits a nominal Struct (the standard
            // Treiber-stack node shape) or a primitive. Pointer-to-
            // pointer, pointer-to-vector, etc. are rejected — the
            // tagged-pointer wrapper holds a single T* slot, not a
            // structural composition, so the inner has to be the
            // pointee directly.
            if (n.path[0] == "AtomicTaggedPointer" && n.type_args.size() == 1) {
                TypePtr inner = n.type_args[0] ? resolve_type(*n.type_args[0]) : types_->error();
                const bool ok = inner == nullptr || inner->is_error() || inner->is_primitive()
                                || inner->kind() == TypeKind::Struct;
                if (!ok) {
                    error_at(t.range,
                             std::format("AtomicTaggedPointer[T] requires T to be a struct or "
                                         "primitive type, got {}",
                                         inner->describe()));
                    return types_->make_atomic_tagged_pointer(types_->error());
                }
                // §A4 (§14.9.4) same target-feature gate as the wide
                // Atomic[T] above — AtomicTaggedPointer composes a
                // (T*, uint64_t) into 128 bits and shares the same
                // ISA-feature constraint.
                if (auto diag = check_wide_atomic_feature(t.range, "AtomicTaggedPointer");
                    !diag.empty()) {
                    error_at(t.range, std::move(diag));
                    return types_->make_atomic_tagged_pointer(types_->error());
                }
                return types_->make_atomic_tagged_pointer(inner);
            }
            // Try primitives first (Int32, Bool, ...).
            auto prim_kind = TypeArena::primitive_kind_by_name(n.path[0]);
            if (prim_kind != TypeKind::Error) {
                return types_->primitive(prim_kind);
            }
            // Then scope lookup — a nominal type or generic parameter.
            if (auto* sym = scopes_.current().lookup(n.path[0])) {
                if (sym->type != nullptr) {
                    // §7 generics phase 2 — a use of a user-defined generic
                    // struct. `Pair[Int32]` resolves to a struct instance
                    // recording the resolved arguments; field access and
                    // construction substitute them for the struct's params.
                    if (sym->type->kind() == TypeKind::Struct && sym->decl != nullptr
                        && sym->decl->kind == ast::NodeKind::Struct) {
                        const auto& sd = static_cast<const ast::StructDecl&>(*sym->decl);
                        if (auto args = resolve_generic_instance_args(
                                sd.generics, n, "struct", sd.name, t.range)) {
                            return types_->make_struct_instance(sym->type->nominal_decl(),
                                                                std::move(*args));
                        }
                        return sym->type;
                    }
                    // §7 generics phase 2 — same shape for a user-defined
                    // generic enum: `Option[Int32]` is an enum instance.
                    if (sym->type->kind() == TypeKind::Enum && sym->decl != nullptr
                        && sym->decl->kind == ast::NodeKind::Enum) {
                        const auto& ed = static_cast<const ast::EnumDecl&>(*sym->decl);
                        if (auto args = resolve_generic_instance_args(
                                ed.generics, n, "enum", ed.name, t.range)) {
                            return types_->make_enum_instance(sym->type->nominal_decl(),
                                                              std::move(*args));
                        }
                        return sym->type;
                    }
                    return sym->type;
                }
            }
            error_at(t.range, std::format("unknown type '{}'", n.path[0]));
            return types_->error();
        }
        // Dotted paths (modules) are not yet resolved.
        error_at(t.range, "dotted type paths are not yet resolved");
        return types_->error();
    }
    case ast::NodeKind::OptionalType: {
        const auto& o = static_cast<const ast::OptionalType&>(t);
        return types_->make_optional(o.inner ? resolve_type(*o.inner) : types_->error());
    }
    case ast::NodeKind::VectorType: {
        const auto& v = static_cast<const ast::VectorType&>(t);
        TypePtr elem = v.element ? resolve_type(*v.element) : types_->error();
        // §7 generics phase 2 — a symbolic length `[N]T` references a
        // const-generic parameter. If N is bound to a concrete value in
        // scope (an instantiation), produce the concrete `[8]T`; otherwise
        // keep it symbolic (the decl-template / inference case).
        if (!v.length_ident.empty()) {
            if (auto* sym = scopes_.current().lookup(v.length_ident);
                sym != nullptr && sym->type != nullptr
                && sym->type->kind() == TypeKind::ConstValue) {
                return types_->make_vector(sym->type->const_value(), elem);
            }
            return types_->make_vector_symbolic(v.length_ident, elem);
        }
        return types_->make_vector(v.length, elem);
    }
    case ast::NodeKind::TupleType: {
        const auto& tup = static_cast<const ast::TupleType&>(t);
        std::vector<TypePtr> parts;
        parts.reserve(tup.elements.size());
        for (const auto& e : tup.elements) {
            parts.push_back(e ? resolve_type(*e) : types_->error());
        }
        return types_->make_tuple(std::move(parts));
    }
    case ast::NodeKind::FunctionType: {
        const auto& f = static_cast<const ast::FunctionType&>(t);
        std::vector<TypePtr> ps;
        ps.reserve(f.params.size());
        for (const auto& p : f.params) {
            ps.push_back(p ? resolve_type(*p) : types_->error());
        }
        TypePtr r = f.result ? resolve_type(*f.result) : types_->unit();
        return types_->make_function(std::move(ps), r);
    }
    case ast::NodeKind::SomeType: {
        const auto& s = static_cast<const ast::SomeType&>(t);
        return s.inner ? resolve_type(*s.inner) : types_->error();
    }
    case ast::NodeKind::DynType: {
        const auto& d = static_cast<const ast::DynType&>(t);
        return d.inner ? resolve_type(*d.inner) : types_->error();
    }
    case ast::NodeKind::InterruptType: {
        // §A8 (§14.5.3) vector-table slot type. The inner T is the
        // trap-frame struct passed by `inout` to each ISR. Sema
        // type-checks assignments to this slot against the @interrupt
        // ISR-shape rules at the assignment site (see check_expr's
        // AddressOfExpr / IdentExpr path for the actual matching).
        const auto& it = static_cast<const ast::InterruptType&>(t);
        TypePtr T = it.trap_frame ? resolve_type(*it.trap_frame) : types_->error();
        return types_->make_interrupt_handler(T);
    }
    default:
        return types_->error();
    }
}

TypePtr Resolver::lookup_field(TypePtr struct_type,
                               std::string_view name,
                               const ast::StructDecl::Field** out_field) {
    if (struct_type == nullptr) {
        return nullptr;
    }
    // §A4 (§14.9.3) CASResult[T] has two synthetic fields. Served
    // here so the resolver's ordinary `.succeeded` / `.actual`
    // member-access path types correctly.
    if (struct_type->kind() == TypeKind::CasResult && struct_type->inner() != nullptr) {
        if (name == "succeeded") {
            return types_->boolean();
        }
        if (name == "actual") {
            return struct_type->inner();
        }
        return nullptr;
    }
    if (struct_type->kind() != TypeKind::Struct) {
        return nullptr;
    }
    const auto* decl = struct_type->nominal_decl();
    if (decl == nullptr || decl->kind != ast::NodeKind::Struct) {
        return nullptr;
    }
    const auto& s = static_cast<const ast::StructDecl&>(*decl);

    // Direct fields first (an embed shadowed by a direct field — unlikely
    // but the §6 read is that names resolve outward, with the immediate
    // owner winning).
    for (const auto& f : s.fields) {
        if (f.name == name && f.kind != ast::StructDecl::Field::Kind::Embed) {
            if (out_field != nullptr) {
                *out_field = &f;
            }
            // §7 generics phase 2 — substitute the instance's type
            // arguments (struct_type->parts()) for the struct's generic
            // params so `Pair[Int32].first` resolves to Int32, not T.
            return f.type ? resolve_member_type_with_generics(
                                s.generics, *f.type, struct_type->parts())
                          : types_->error();
        }
    }
    // Then recurse through `embed` fields so `entity.position` resolves to
    // `entity.transform.position` per §6.
    for (const auto& f : s.fields) {
        if (f.kind == ast::StructDecl::Field::Kind::Embed && f.type) {
            auto embed_type =
                resolve_member_type_with_generics(s.generics, *f.type, struct_type->parts());
            if (auto t = lookup_field(embed_type, name, out_field)) {
                return t;
            }
        }
    }
    return nullptr;
}

TypePtr Resolver::resolve_member_type_with_generics(const std::vector<ast::GenericParam>& generics,
                                                    const ast::Type& member_type,
                                                    const std::vector<TypePtr>& args) {
    if (named_type_param_count(generics) == 0) {
        return resolve_type(member_type);
    }
    // Bind each parameter (type and const alike) for the duration of this
    // resolution. With instance arguments present each binds to its
    // concrete argument — a type param to a type, a const param to a
    // ConstValue, which is what lets a `[N]T` field concretize. With no
    // arguments each binds to an opaque GenericParam placeholder, so a
    // `[N]T` stays a symbolic-length vector for the decl-template /
    // inference case. `args` interleaves type + const arguments in
    // declaration order, matching this walk.
    ScopeStack::Guard g(scopes_);
    std::size_t idx = 0;
    for (const auto& gp : generics) {
        if (gp.name.empty()) {
            continue;
        }
        Symbol s;
        s.name = gp.name;
        s.kind = SymbolKind::GenericParam;
        s.type = (idx < args.size() && args[idx] != nullptr) ? args[idx]
                                                             : types_->make_generic_param(gp.name);
        s.definition_range = gp.range;
        (void)g.scope().insert(std::move(s));
        ++idx;
    }
    return resolve_type(member_type);
}

}  // namespace vestra::sema
