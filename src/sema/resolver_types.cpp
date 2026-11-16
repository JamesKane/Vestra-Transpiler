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

#include <algorithm>

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
        // §15.5 the heap-backed library types need the hosted runtime.
        if (target_.freestanding && n.path.size() == 1
            && (n.path[0] == "String" || n.path[0] == "Vec" || n.path[0] == "Box"
                || n.path[0] == "HashMap" || n.path[0] == "Channel")
            && scopes_.current().lookup(n.path[0]) == nullptr) {
            if (hosted_reported_.insert(t.range.begin.offset).second) {
                error_at(t.range,
                         std::format("`{}` needs the hosted runtime (a heap), which a --no-libc "
                                     "build doesn't have",
                                     n.path[0]));
            }
            return types_->error();
        }
        if (n.path.size() == 1) {
            // §10 builtin `Box[T]` — the unique-ownership heap pointer.
            // Recognized here before generic-symbol lookup so it never
            // needs to be in the user's symbol table.
            if (n.path[0] == "Box" && n.type_args.size() == 1) {
                auto bx = types_->make_box(n.type_args[0] ? resolve_type(*n.type_args[0])
                                                          : types_->error());
                check_slot_holds(bx, t.range);
                return bx;
            }
            // §11 Future[T] — a `spawn`-produced result handle. Spellable as
            // a type so a future can be bound (`let f: Future[Int32] = …`).
            if (n.path[0] == "Future" && n.type_args.size() == 1) {
                return types_->make_future(n.type_args[0] ? resolve_type(*n.type_args[0])
                                                          : types_->error());
            }
            // §11 Channel[T] — a typed queue (`send` / `recv`).
            if (n.path[0] == "Channel" && n.type_args.size() == 1) {
                auto ch = types_->make_channel(n.type_args[0] ? resolve_type(*n.type_args[0])
                                                              : types_->error());
                check_slot_holds(ch, t.range);
                return ch;
            }
            // §18.5 Vec[T] — a growable, owned sequence (→ std::vector<T>).
            if (n.path[0] == "Vec" && n.type_args.size() == 1) {
                return types_->make_vec(n.type_args[0] ? resolve_type(*n.type_args[0])
                                                       : types_->error());
            }
            // §18.5 HashMap[K, V] — an owned hash map (→ std::unordered_map).
            if (n.path[0] == "HashMap" && n.type_args.size() == 2) {
                return types_->make_hashmap(
                    n.type_args[0] ? resolve_type(*n.type_args[0]) : types_->error(),
                    n.type_args[1] ? resolve_type(*n.type_args[1]) : types_->error());
            }
            // §13 Soa[T] — struct-of-arrays over a struct T. The element must be
            // a (non-generic) struct: SoA stores one column per field, so the
            // field set has to be known. Lowers to a tuple-of-vectors.
            if (n.path[0] == "Soa" && n.type_args.size() == 1) {
                TypePtr elem = n.type_args[0] ? resolve_type(*n.type_args[0]) : types_->error();
                if (elem != nullptr && !elem->is_error()) {
                    const ast::Decl* nd = elem->nominal_decl();
                    if (elem->kind() != TypeKind::Struct || nd == nullptr
                        || nd->kind != ast::NodeKind::Struct) {
                        error_at(t.range,
                                 std::format("Soa[T] requires T to be a struct, got {}",
                                             elem->describe()));
                        return types_->error();
                    }
                    if (!elem->parts().empty()) {
                        error_at(t.range,
                                 "Soa[T] over a generic struct instance is not yet supported");
                        return types_->error();
                    }
                    const auto& sd = static_cast<const ast::StructDecl&>(*nd);
                    std::size_t cols = 0;
                    for (const auto& f : sd.fields) {
                        if (f.kind != ast::StructDecl::Field::Kind::Embed) {
                            ++cols;
                        }
                    }
                    if (cols == 0) {
                        error_at(t.range, "Soa[T] requires T to have at least one stored field");
                        return types_->error();
                    }
                }
                return types_->make_soa(elem);
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
                if (inner != nullptr && !inner->is_error() && !is_mmio_payload(inner)) {
                    error_at(t.range,
                             std::format("MmioView[T] requires T to be a primitive, got {}",
                                         inner->describe()));
                    return types_->make_mmio_view(types_->error());
                }
                return types_->make_mmio_view(inner);
            }
            if (n.path[0] == "MmioRegion" && n.type_args.size() == 1) {
                TypePtr inner = n.type_args[0] ? resolve_type(*n.type_args[0]) : types_->error();
                if (inner != nullptr && !inner->is_error() && !is_mmio_payload(inner)) {
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
                if (inner != nullptr && !inner->is_error() && !is_mmio_payload(inner)) {
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
                if (inner != nullptr && !inner->is_error() && !is_atomic_payload(inner)) {
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
        // §5 a dotted type path is a qualified reference into an imported
        // module: `util.geom.Point`. The all-but-last segments name the module;
        // the last names a public struct/enum export. emit_type renders the
        // full path with `::`, so codegen needs no extra qualification.
        std::string dotted;
        for (std::size_t i = 0; i + 1 < n.path.size(); ++i) {
            if (i != 0) {
                dotted += '.';
            }
            dotted += n.path[i];
        }
        if (auto mit = module_exports_.find(dotted); mit != module_exports_.end()) {
            const std::string& last = n.path.back();
            auto exp = mit->second.find(last);
            if (exp != mit->second.end() && exp->second.type != nullptr
                && (exp->second.kind == SymbolKind::Struct
                    || exp->second.kind == SymbolKind::Enum)) {
                // The dependency's resolver already built this nominal type
                // (in the shared arena); reuse it so the decl identity matches
                // values constructed/returned across the boundary.
                //
                // §7/§5 generic imported type — `coll.box.Holder[Int32]`: bind
                // the type arguments onto the imported nominal exactly as the
                // local case does, so field access and construction substitute
                // them. Non-generic exports (no args) fall through to the bare
                // nominal, matching the prior behavior for `util.math.Pair`.
                const ast::Decl* nd = exp->second.type->nominal_decl();
                if (exp->second.kind == SymbolKind::Struct && nd != nullptr
                    && nd->kind == ast::NodeKind::Struct) {
                    const auto& sd = static_cast<const ast::StructDecl&>(*nd);
                    if (auto args = resolve_generic_instance_args(
                            sd.generics, n, "struct", sd.name, t.range)) {
                        return types_->make_struct_instance(nd, std::move(*args));
                    }
                } else if (exp->second.kind == SymbolKind::Enum && nd != nullptr
                           && nd->kind == ast::NodeKind::Enum) {
                    const auto& ed = static_cast<const ast::EnumDecl&>(*nd);
                    if (auto args = resolve_generic_instance_args(
                            ed.generics, n, "enum", ed.name, t.range)) {
                        return types_->make_enum_instance(nd, std::move(*args));
                    }
                }
                return exp->second.type;
            }
            error_at(t.range, std::format("module '{}' has no public type '{}'", dotted, last));
            return types_->error();
        }
        error_at(t.range, std::format("'{}' is not an imported module", dotted));
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
        // §17.3 a const-expression length folds to a fixed one when every name
        // in it is a const (or a const generic bound by an instantiation);
        // otherwise it stays symbolic, spelled by its canonical text.
        if (v.length_expr) {
            ComptimeFolder::Env env = comptime_env_;
            bind_const_generics_in(*v.length_expr, env);
            if (auto c = folder_.fold(*v.length_expr, env, TypeKind::Int)) {
                if (c->kind == ComptimeValue::Kind::Int && c->i >= 0) {
                    return types_->make_vector(c->i, elem);
                }
                if (c->kind == ComptimeValue::Kind::UInt) {
                    return types_->make_vector(static_cast<std::int64_t>(c->u), elem);
                }
                error_at(v.length_expr->range, "an array length must be a non-negative integer");
                return types_->error();
            }
            if (auto bad = non_const_length_name(*v.length_expr); !bad.empty()) {
                error_at(v.length_expr->range,
                         std::format("array length names '{}', which is not a compile-time "
                                     "constant",
                                     bad));
                return types_->error();
            }
            return types_->make_vector_symbolic(v.length_ident, elem);
        }
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
        return types_->make_function(std::move(ps), r, row_of(f.effects, false));
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

bool Resolver::non_escapable(TypePtr t, int depth) const {
    if (t == nullptr || depth > 16) {
        return false;
    }
    switch (t->kind()) {
    case TypeKind::Span:
    case TypeKind::MutSpan:
    case TypeKind::Str:
    case TypeKind::MmioView:    // Annex §10.5 / §14.11: bound by the Mmio discharge
    case TypeKind::MmioRegion:
        return true;
    // §10 a lazy combinator over a view is a view; `chunks(of:)` yields
    // sub-views of its span.
    case TypeKind::ZipIter:
    case TypeKind::TakeIter:
    case TypeKind::MapIter:
    case TypeKind::FilterIter:
        return t->holds_view();
    case TypeKind::ChunkIter:
        return non_escapable(t->inner(), depth + 1);
    case TypeKind::Optional:
    case TypeKind::Vector:
        return non_escapable(t->inner(), depth + 1);
    case TypeKind::Tuple:
        for (auto p : t->parts()) {
            if (non_escapable(p, depth + 1)) {
                return true;
            }
        }
        return false;
    case TypeKind::Struct: {
        for (auto p : t->parts()) {  // a generic instance over a view
            if (non_escapable(p, depth + 1)) {
                return true;
            }
        }
        const auto* d = t->nominal_decl();
        if (d == nullptr || d->kind != ast::NodeKind::Struct) {
            return false;
        }
        for (const auto& f : static_cast<const ast::StructDecl*>(d)->fields) {
            if (auto ft = types_->member_type(f.type.get())) {
                if (non_escapable(ft, depth + 1)) {
                    return true;
                }
            } else if (ast_non_escapable(f.type.get(), depth + 1)) {
                return true;
            }
        }
        return false;
    }
    default:
        return false;
    }
}

// A field's declared type, judged from its spelling (the unit's own structs
// before their member types are recorded).
bool Resolver::ast_non_escapable(const ast::Type* t, int depth) const {
    if (t == nullptr || depth > 16) {
        return false;
    }
    switch (t->kind) {
    case ast::NodeKind::NamedType: {
        const auto& n = static_cast<const ast::NamedType&>(*t);
        if (n.path.size() == 1
            && (n.path[0] == "Span" || n.path[0] == "MutSpan" || n.path[0] == "Str"
                || n.path[0] == "MmioView" || n.path[0] == "MmioRegion")) {
            return true;
        }
        for (const auto& a : n.type_args) {
            if (ast_non_escapable(a.get(), depth + 1)) {
                return true;
            }
        }
        if (n.path.size() == 1) {
            if (const auto* sym = const_cast<ScopeStack&>(scopes_).global().lookup_local(n.path[0]);
                sym != nullptr && sym->kind == SymbolKind::Struct && sym->decl != nullptr) {
                for (const auto& f : static_cast<const ast::StructDecl*>(sym->decl)->fields) {
                    if (ast_non_escapable(f.type.get(), depth + 1)) {
                        return true;
                    }
                }
            }
        }
        return false;
    }
    case ast::NodeKind::OptionalType:
        return ast_non_escapable(static_cast<const ast::OptionalType&>(*t).inner.get(), depth + 1);
    case ast::NodeKind::VectorType:
        return ast_non_escapable(static_cast<const ast::VectorType&>(*t).element.get(), depth + 1);
    case ast::NodeKind::TupleType:
        for (const auto& el : static_cast<const ast::TupleType&>(*t).elements) {
            if (ast_non_escapable(el.get(), depth + 1)) {
                return true;
            }
        }
        return false;
    default:
        return false;
    }
}

void Resolver::check_slot_holds(TypePtr slot, diag::SourceRange r) {
    if (slot == nullptr || slot->inner() == nullptr || !r.is_valid()
        || !slot_reported_.insert(r.begin.offset).second) {
        return;
    }
    auto inner = slot->inner();
    const bool chan = slot->kind() == TypeKind::Channel;
    if (inner->kind() == TypeKind::Future) {
        error_at(r,
                 std::format("{} would hold a Future, which can't outlive the scope that spawned "
                             "it",
                             slot->describe()));
        return;
    }
    if (non_escapable(inner)) {
        error_at(r,
                 std::format("{} would {}: a Span, MutSpan or Str (or anything holding one) "
                             "can't outlive what it views",
                             slot->describe(),
                             chan ? "carry a non-escapable value" : "put a non-escapable value on the heap"));
    }
}

TypePtr Resolver::resolve_member_type_with_generics(const std::vector<ast::GenericParam>& generics,
                                                    const ast::Type& member_type,
                                                    const std::vector<TypePtr>& args) {
    // §5 a member of an imported type: use the type its own module resolved
    // (F19), binding this instance's type arguments by name. A const generic
    // needs the scope-binding path below (it concretizes `[N]T`), so a type
    // with one falls through.
    if (auto declared = types_->member_type(&member_type)) {
        const bool has_const = std::ranges::any_of(
            generics, [](const ast::GenericParam& g) { return !g.name.empty() && g.is_const; });
        if (named_type_param_count(generics) == 0) {
            return declared;
        }
        if (!has_const) {
            std::unordered_map<std::string, TypePtr> bindings;
            std::size_t idx = 0;
            for (const auto& gp : generics) {
                if (gp.name.empty()) {
                    continue;
                }
                if (idx < args.size() && args[idx] != nullptr) {
                    bindings[gp.name] = args[idx];
                }
                ++idx;
            }
            return types_->substitute(declared, bindings);
        }
    }
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

std::vector<std::string> Resolver::row_of(const ast::Effects& e, bool is_async) {
    std::vector<std::string> row;
    for (const auto& c : e.using_caps) {
        if (c != nullptr && c->kind == ast::NodeKind::NamedType) {
            const auto& nt = static_cast<const ast::NamedType&>(*c);
            if (!nt.path.empty()) {
                row.push_back(nt.path.front());
            }
        }
    }
    if (is_async && std::ranges::find(row, "Async") == row.end()) {
        row.emplace_back("Async");
    }
    return row;
}

}  // namespace vestra::sema

namespace vestra::sema {

// Annex §4.5 / §19 the innermost type that keeps `t` from being Trivial
// (register-class bits, no resources, not linear), or null when it is.
// Resource-free wrappers over Trivial bits (Atomic, PerCpu, Padded, an
// optional, a fixed array) are Trivial when their contents are.
// A cell reached only through its compiler-known methods: an atomic, a
// per-hart slot, a task Context, or one of those padded to a cache line.
bool Resolver::is_method_cell(TypePtr t) {
    switch (t->kind()) {
    case TypeKind::Atomic:
    case TypeKind::AtomicTaggedPointer:
    case TypeKind::PerCpu:
        return true;
    case TypeKind::Padded:
        return t->inner() != nullptr && is_method_cell(t->inner());
    default:
        return false;
    }
}

void Resolver::check_noinit_value_use(const Symbol& sym, std::string_view name,
                                      diag::SourceRange r) {
    if (sym.kind != SymbolKind::Static || sym.decl == nullptr
        || sym.decl->kind != ast::NodeKind::Static
        || !static_cast<const ast::StaticDecl&>(*sym.decl).noinit) {
        return;
    }
    if (sym.type == nullptr || sym.type->is_error() || is_method_cell(sym.type)) {
        return;
    }
    error_at(r,
             std::format("'{}' is @noinit storage, which isn't in scope as a value: reach it "
                         "through `&{}` under `RawMemory` (Annex §4.5)",
                         name,
                         name));
}

bool Resolver::is_mmio_payload(TypePtr t) {
    if (t == nullptr) {
        return false;
    }
    switch (t->kind()) {
    case TypeKind::Int128:
    case TypeKind::UInt128:
        return false;
    case TypeKind::Bool:
    case TypeKind::Char:
        return true;
    default:
        return t->is_integer() || t->is_float();
    }
}

// §14.9.2 an atomic cell holds a §4 primitive: an integer (the 128-bit ones
// gated on the target's wide CAS), a float, Bool or Char. String, Str and
// Unit are not cells (is_primitive admits them for other rules).
bool Resolver::is_atomic_payload(TypePtr t) {
    if (t == nullptr) {
        return false;
    }
    if (t->kind() == TypeKind::Int128 || t->kind() == TypeKind::UInt128) {
        return true;
    }
    return is_mmio_payload(t);
}

std::string Resolver::trivial_clause(TypePtr held, TypePtr blocker) {
    const char* why = trivial_blocker_is_linear(blocker) ? "is linear"
                      : non_escapable(blocker)           ? "is a view"
                      : owns_resources(blocker)          ? "owns resources"
                                                         : "isn't Trivial";
    const auto h = held->describe();
    const auto b = blocker->describe();
    return h == b ? std::format("{} {}", b, why) : std::format("{} holds {}, which {}", h, b, why);
}

ByteLayout Resolver::byte_layout(TypePtr t) {
    if (t == nullptr) {
        return {};
    }
    switch (t->kind()) {
    case TypeKind::Vector: {
        const auto e = byte_layout(t->inner());
        if (e.size == 0 || t->vector_length() <= 0) {
            return {};
        }
        return {t->vector_length() * e.size, e.align};
    }
    case TypeKind::Struct:
        if (t->nominal_decl() != nullptr && t->nominal_decl()->kind == ast::NodeKind::Struct) {
            return struct_byte_layout(static_cast<const ast::StructDecl&>(*t->nominal_decl()),
                                      scopes_.global());
        }
        return {};
    case TypeKind::OpaqueType:
        if (t->nominal_decl() != nullptr && t->nominal_decl()->kind == ast::NodeKind::Opaque) {
            const auto& od = static_cast<const ast::OpaqueDecl&>(*t->nominal_decl());
            if (od.underlying != nullptr) {
                return byte_layout(resolve_type(*od.underlying));
            }
        }
        return {};
    case TypeKind::Str:
    case TypeKind::StrConst:
        return {};  // views of text, not bytes to reinterpret
    default:
        return primitive_byte_layout(t->kind());
    }
}

void Resolver::check_reinterpret(const ast::AsExpr& a, TypePtr from, TypePtr to) {
    if (from == nullptr || to == nullptr || from->is_error() || to->is_error()) {
        return;
    }
    if (to->kind() == TypeKind::Enum) {
        error_at(a.range,
                 std::format("`as` can't reinterpret bytes as the enum {}: use `{}.from(raw)`, "
                             "which checks the discriminant",
                             to->describe(),
                             to->describe()));
        return;
    }
    for (auto side : {from, to}) {
        if (auto blocker = trivial_blocker(side); blocker != nullptr) {
            error_at(a.range,
                     std::format("`as` reinterprets bytes, so both types must be Trivial; {}",
                                 trivial_clause(side, blocker)));
            return;
        }
    }
    const auto fl = byte_layout(from);
    const auto tl = byte_layout(to);
    for (auto [side, l] : {std::pair{from, fl}, std::pair{to, tl}}) {
        if (l.size == 0) {
            error_at(a.range,
                     std::format("`as` can't size {} yet: only primitives, vectors of them and "
                                 "structs of those have a layout",
                                 side->describe()));
            return;
        }
    }
    if (fl.size != tl.size || fl.align != tl.align) {
        error_at(a.range,
                 std::format("`as` needs types of one size and alignment: {} is {} bytes "
                             "(align {}), {} is {} bytes (align {})",
                             from->describe(),
                             fl.size,
                             fl.align,
                             to->describe(),
                             tl.size,
                             tl.align));
    }
}

TypePtr Resolver::trivial_blocker(TypePtr t, int depth) {
    if (t == nullptr || t->is_error() || depth > 32) {
        return nullptr;
    }
    switch (t->kind()) {
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
    case TypeKind::StrConst:
    case TypeKind::Duration:
    case TypeKind::Never:
    case TypeKind::Ptr:
    case TypeKind::MutPtr:
    case TypeKind::AtomicTaggedPointer:
        return nullptr;
    case TypeKind::Optional:
    case TypeKind::Vector:
    case TypeKind::Atomic:
    case TypeKind::PerCpu:
    case TypeKind::Padded:
        return trivial_blocker(t->inner(), depth + 1);
    case TypeKind::Tuple:
        for (auto p : t->parts()) {
            if (auto b = trivial_blocker(p, depth + 1)) {
                return b;
            }
        }
        return nullptr;
    case TypeKind::OpaqueType: {
        // §6 a newtype is its representation at run time.
        const auto* d = t->nominal_decl();
        if (d == nullptr || d->kind != ast::NodeKind::Opaque) {
            return nullptr;
        }
        const auto& od = static_cast<const ast::OpaqueDecl&>(*d);
        return od.underlying != nullptr ? trivial_blocker(resolve_type(*od.underlying), depth + 1)
                                        : nullptr;
    }
    case TypeKind::Struct: {
        const auto* d = t->nominal_decl();
        if (d == nullptr || d->kind != ast::NodeKind::Struct) {
            return nullptr;
        }
        const auto* sd = static_cast<const ast::StructDecl*>(d);
        if (sd->is_linear) {
            return t;
        }
        for (const auto& f : sd->fields) {
            if (f.type == nullptr) {
                continue;
            }
            auto ft = resolve_member_type_with_generics(sd->generics, *f.type, t->parts());
            if (auto b = trivial_blocker(ft, depth + 1)) {
                return b;
            }
        }
        return nullptr;
    }
    case TypeKind::Enum: {
        const auto* d = t->nominal_decl();
        if (d == nullptr || d->kind != ast::NodeKind::Enum) {
            return nullptr;
        }
        const auto* ed = static_cast<const ast::EnumDecl*>(d);
        for (const auto& c : ed->cases) {
            for (const auto& [n, ty] : c.payload) {
                if (ty == nullptr) {
                    continue;
                }
                auto pt = resolve_member_type_with_generics(ed->generics, *ty, t->parts());
                if (auto b = trivial_blocker(pt, depth + 1)) {
                    return b;
                }
            }
        }
        return nullptr;
    }
    default:
        return t;
    }
}

bool Resolver::trivial_blocker_is_linear(TypePtr t) {
    if (t == nullptr || t->kind() != TypeKind::Struct) {
        return false;
    }
    const auto* d = t->nominal_decl();
    return d != nullptr && d->kind == ast::NodeKind::Struct
           && static_cast<const ast::StructDecl*>(d)->is_linear;
}

bool Resolver::owns_resources(TypePtr t) {
    if (t == nullptr) {
        return false;
    }
    switch (t->kind()) {
    case TypeKind::String:
    case TypeKind::Vec:
    case TypeKind::HashMap:
    case TypeKind::Box:
    case TypeKind::Channel:
    case TypeKind::Future:
    case TypeKind::Soa:
    case TypeKind::Function:
        return true;
    default:
        return false;
    }
}

}  // namespace vestra::sema

namespace vestra::sema {

// §17.3 a const generic that the current scope binds to a value (an
// instantiation's `N = 4`) is visible to a length expression's fold.
void Resolver::bind_const_generics_in(const ast::Expr& e, ComptimeFolder::Env& env) {
    switch (e.kind) {
    case ast::NodeKind::IdentExpr: {
        const auto& id = static_cast<const ast::IdentExpr&>(e);
        if (auto* sym = scopes_.current().lookup(id.name);
            sym != nullptr && sym->type != nullptr && sym->type->kind() == TypeKind::ConstValue) {
            ComptimeValue cv;
            cv.kind = ComptimeValue::Kind::Int;
            cv.type = TypeKind::Int;
            cv.i = sym->type->const_value();
            env[id.name] = cv;
        }
        break;
    }
    case ast::NodeKind::BinaryExpr: {
        const auto& b = static_cast<const ast::BinaryExpr&>(e);
        if (b.lhs) {
            bind_const_generics_in(*b.lhs, env);
        }
        if (b.rhs) {
            bind_const_generics_in(*b.rhs, env);
        }
        break;
    }
    case ast::NodeKind::UnaryExpr: {
        const auto& u = static_cast<const ast::UnaryExpr&>(e);
        if (u.operand) {
            bind_const_generics_in(*u.operand, env);
        }
        break;
    }
    case ast::NodeKind::ParenExpr: {
        const auto& p = static_cast<const ast::ParenExpr&>(e);
        if (p.inner) {
            bind_const_generics_in(*p.inner, env);
        }
        break;
    }
    default:
        break;
    }
}

// §17.3 the first name in a length expression that is neither a const nor a
// const generic parameter ("" if none): such a length can't be a C++ bound.
std::string Resolver::non_const_length_name(const ast::Expr& e) {
    switch (e.kind) {
    case ast::NodeKind::IdentExpr: {
        const auto& id = static_cast<const ast::IdentExpr&>(e);
        // A name not in scope here is left to C++: a struct field's type is
        // resolved where its own const generics aren't bound.
        const auto* sym = scopes_.current().lookup(id.name);
        if (sym == nullptr) {
            return {};
        }
        if (sym->kind == SymbolKind::Local || sym->kind == SymbolKind::Parameter
            || sym->kind == SymbolKind::Static || sym->kind == SymbolKind::Func) {
            return id.name;
        }
        return {};
    }
    case ast::NodeKind::BinaryExpr: {
        const auto& b = static_cast<const ast::BinaryExpr&>(e);
        auto l = b.lhs ? non_const_length_name(*b.lhs) : std::string{};
        return !l.empty() ? l : (b.rhs ? non_const_length_name(*b.rhs) : std::string{});
    }
    case ast::NodeKind::UnaryExpr: {
        const auto& u = static_cast<const ast::UnaryExpr&>(e);
        return u.operand ? non_const_length_name(*u.operand) : std::string{};
    }
    case ast::NodeKind::ParenExpr: {
        const auto& p = static_cast<const ast::ParenExpr&>(e);
        return p.inner ? non_const_length_name(*p.inner) : std::string{};
    }
    default:
        return {};
    }
}

}  // namespace vestra::sema
