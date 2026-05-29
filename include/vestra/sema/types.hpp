// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

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
    // §A4 (§14.9.4) wide-integer primitives. The load-bearing purpose is
    // backing `Atomic[UInt128]` / `Atomic[Int128]` for tagged-pointer
    // and seqlock patterns the kernel needs; the underlying integer
    // types also lower to GCC/Clang's `__uint128_t` / `__int128_t` so
    // user code can manipulate them outside the atomic wrapper. The
    // architectures that admit `Atomic[Int128]` (aarch64 +lse2, x86_64
    // +cx16) are gated at the build-feature layer; the primitive
    // itself is universally available.
    Int128,
    UInt128,
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
    Result,
    Box,
    // §11 Future[T] — what `spawn f()` yields; `await` consumes it for the T.
    Future,
    // §11 Channel[T] — a typed queue: `send` is a sink, `recv` -> T?.
    Channel,
    Vector,
    Function,
    Tuple,
    // §10 borrowed views. `Span[T]` is read-only, `MutSpan[T]` permits
    // mutation through the view; both are non-escapable (the type
    // system v0.5 doesn't yet enforce the escape rule, but `describe()`
    // names them so callers can see the contract).
    Span,
    MutSpan,
    // §A3 (§10.5) raw-mint pointers. `Ptr[T]` / `MutPtr[T]` are
    // Trivial bit-pattern wrappers; lower to `const T*` / `T*`.
    // Minted via `Ptr.unchecked(fromAddress:)` etc. under a
    // `RawMemory` discharge.
    Ptr,
    MutPtr,
    // §A6 (§14.11) typed MMIO views. `MmioView[T]` is a non-escapable
    // single-register handle minted via `MmioView.at(ptr:)` under
    // an `Mmio` discharge; `MmioRegion[T]` is the indexed N-register
    // variant. Both lower to small wrapper structs that thread a
    // `volatile T*` through the read/write paths.
    MmioView,
    MmioRegion,
    // §A6 (§14.11.3) endianness-aware MMIO view. Lowers to a
    // template that conditionally byte-swaps read/write based on
    // the device's declared endianness vs the host's native.
    MmioWireView,
    // §A11 (§14.8) per-CPU storage. `PerCpu[T]` is the compiler-
    // known generic struct over Trivial T; v0.5 hosts a single
    // 64-byte-padded slot, the kernel target swaps the storage for
    // `[MAX_HARTS]Padded[T]`.
    PerCpu,
    // §A8 (§14.5.3) vector-table slot type — `@interrupt(T)` in a
    // type position. Equivalent at the C++ layer to a function-
    // pointer with shape `(inout T) -> Unit`, but tracked as a
    // distinct kind so sema can enforce the ISR-shape rules at
    // assignment sites (the only valid value is `&fn` where fn is
    // an @interrupt-attributed function whose first param is `inout
    // T` and whose return is Unit).
    InterruptHandler,
    // §A11 (§14.8) cache-line-padded wrapper. `Padded[T]` rounds
    // sizeof(T) up to cfg.option("cache_line_bytes") (v0.5 defaults
    // to 64) so adjacent values don't share a cache line. The
    // load-bearing kernel use is `[MAX_HARTS]Padded[T]` for the
    // per-hart array region; also useful standalone for ring-buffer
    // slots and lock-free queue cells.
    Padded,
    // §14.12 typed system-register handles. Three flavors per
    // §14.12.1: read-only (`ReadOnlySysreg[T]`, e.g. midr_el1),
    // write-only (`WriteOnlySysreg[T]`, no canonical members in the
    // v0.5 aarch64 EL1 subset but the kernel target uses it for
    // some MSRs and CSRs), and read-write (`ReadWriteSysreg[T]`,
    // e.g. daif / sctlr_el1 / vbar_el1 / ttbr0_el1). Sema gates
    // `.read()` and `.write(T)` per handle kind so a stray
    // `Sysreg.midr_el1.write(v)` fails compile rather than running
    // an undefined operation. Calls discharge Asm.
    SysregHandle,
    SysregHandleRO,
    SysregHandleWO,
    // §9 iterator combinators. `ZipIter[A, B]` yields `(A, B)` tuples;
    // `TakeIter[A]` yields up to N values of A. Both expose a
    // synthetic `next() -> Element?` so the existing iterator-protocol
    // for-loop machinery drives them. Sema tracks the *element* types
    // of the inner iterators (so it can shape next()'s return);
    // codegen lowers the calls with CTAD so the C++ side deduces the
    // underlying iterator types from the argument expressions.
    ZipIter,
    TakeIter,
    MapIter,
    FilterIter,
    // §A4 (§14.9) Atomic[T] — compiler-known wrapper that lowers to
    // std::atomic<T>. v0.5 supports primitive T only; load/store/
    // exchange/fetchAdd/fetchSub synthesized via lookup_method.
    Atomic,
    // §A4 (§14.9.3) CASResult[T] — what Atomic[T].compareExchange()
    // returns. Two fields: `succeeded: Bool`, `actual: T`. Lowers to
    // the __vstr::CASResult<T> template in the runtime preamble.
    CasResult,
    // §A4 (§14.9.5) AtomicTaggedPointer[T] — lock-free ABA-safe
    // pointer swap built on top of §A4 wide atomics. Wraps an
    // `Atomic[UInt128]` underneath but presents a typed surface:
    // `.load(ordering) -> (Ptr[T], UInt64)` returns the (pointer,
    // tag) snapshot; `.compareExchange(exp_ptr, exp_tag, des_ptr,
    // success, failure) -> CASResult[(Ptr[T], UInt64)]` auto-bumps
    // the tag on the desired side so a concurrent observer can
    // detect a swap-and-restore. The classic load-bearing use is
    // Treiber-stack push/pop where ABA via free-list reuse would
    // otherwise allow a stale pointer to look fresh.
    AtomicTaggedPointer,
    // nominal — point back to an ast::Decl
    Struct,
    Enum,
    Protocol,
    OpaqueType,
    // generic parameter — opaque within its declaring scope
    GenericParam,
    // §7 generics phase 2 — a compile-time integer used as a generic
    // argument (a const generic, `[const N: Int]`). Carries the value in
    // length_. Appears in a struct/enum instance's parts_ alongside type
    // arguments and as the binding a symbolic vector length resolves to.
    ConstValue,
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
    // §7 generics phase 2 — for a symbolic-length vector (`[N]T`) this is
    // the const-parameter name "N"; empty for a concrete-length vector.
    [[nodiscard]] std::string_view vector_length_name() const noexcept { return name_; }
    // §7 generics phase 2 — the integer carried by a ConstValue.
    [[nodiscard]] std::int64_t const_value() const noexcept { return length_; }
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
    // §9 Result<T, E> — externally what a `throws(E) -> T` function returns.
    // Stored as inner_=T (success), result_=E (error). The codegen lowers it
    // to `std::expected<T, E>`.
    [[nodiscard]] TypePtr make_result(TypePtr success, TypePtr error);
    // §10 Box<T> — the unique-ownership heap pointer. Lowers to
    // `std::unique_ptr<T>` (move-only, never null after construction).
    [[nodiscard]] TypePtr make_box(TypePtr inner);
    // §11 Future[T] — the result handle a `spawn` yields; `await` unwraps it.
    [[nodiscard]] TypePtr make_future(TypePtr inner);
    // §11 Channel[T] — a typed queue; `send(T)` and `recv() -> T?`.
    [[nodiscard]] TypePtr make_channel(TypePtr inner);
    // §10 Span[T] / MutSpan[T] — borrowed, non-escapable views.
    // Lower to `std::span<const T>` and `std::span<T>` respectively.
    [[nodiscard]] TypePtr make_span(TypePtr inner);
    [[nodiscard]] TypePtr make_mut_span(TypePtr inner);
    // §A3 (§10.5) raw pointers — `Ptr[T]` (read-only address) and
    // `MutPtr[T]` (mutable). Trivial; lower to const T* / T*.
    [[nodiscard]] TypePtr make_ptr(TypePtr inner);
    [[nodiscard]] TypePtr make_mut_ptr(TypePtr inner);
    // §A6 (§14.11) typed MMIO views over primitive T.
    [[nodiscard]] TypePtr make_mmio_view(TypePtr inner);
    [[nodiscard]] TypePtr make_mmio_region(TypePtr inner);
    [[nodiscard]] TypePtr make_mmio_wire_view(TypePtr inner);
    [[nodiscard]] TypePtr make_interrupt_handler(TypePtr inner);
    [[nodiscard]] TypePtr make_padded(TypePtr inner);
    [[nodiscard]] TypePtr make_sysreg_handle(TypePtr inner);
    [[nodiscard]] TypePtr make_sysreg_handle_ro(TypePtr inner);
    [[nodiscard]] TypePtr make_sysreg_handle_wo(TypePtr inner);
    // §A11 (§14.8) per-CPU storage over Trivial T.
    [[nodiscard]] TypePtr make_per_cpu(TypePtr inner);
    // §9 iterator-combinator types. ZipIter[A, B] tracks the *element*
    // types yielded by each side; TakeIter[A] tracks the inner
    // iterator's element type.
    [[nodiscard]] TypePtr make_zip_iter(TypePtr elem_a, TypePtr elem_b);
    [[nodiscard]] TypePtr make_take_iter(TypePtr elem);
    // §9 map / filter combinators. MapIter tracks both the source
    // element type (parts[0]) and the post-transform type (parts[1]);
    // FilterIter just carries the source element (the predicate
    // produces Bool, which lookup_method synthesizes implicitly).
    [[nodiscard]] TypePtr make_map_iter(TypePtr elem_in, TypePtr elem_out);
    [[nodiscard]] TypePtr make_filter_iter(TypePtr elem);
    // §A4 (§14.9) Atomic[T] — wraps primitive T as a std::atomic.
    [[nodiscard]] TypePtr make_atomic(TypePtr inner);
    // §A4 (§14.9.3) CASResult[T] — returned by Atomic[T].compareExchange.
    [[nodiscard]] TypePtr make_cas_result(TypePtr inner);
    // §A4 (§14.9.5) AtomicTaggedPointer[T] — typed wide-atomic
    // tagged-pointer wrapper. Inner is the pointee T; the wrapper
    // composes (Ptr[T], UInt64) tag inside an Atomic[UInt128].
    [[nodiscard]] TypePtr make_atomic_tagged_pointer(TypePtr inner);
    [[nodiscard]] TypePtr make_vector(std::int64_t length, TypePtr element);
    // §7 generics phase 2 — a `[N]T` whose length is a const-generic
    // parameter (name) not yet bound to a concrete value. Substitution
    // concretizes it to make_vector(value, …) once N is bound.
    [[nodiscard]] TypePtr make_vector_symbolic(std::string length_name, TypePtr element);
    // §7 generics phase 2 — a compile-time integer generic argument.
    [[nodiscard]] TypePtr make_const_value(std::int64_t value);
    [[nodiscard]] TypePtr make_function(std::vector<TypePtr> params, TypePtr result);
    [[nodiscard]] TypePtr make_tuple(std::vector<TypePtr> elements);

    [[nodiscard]] TypePtr make_nominal(TypeKind k, const ast::Decl* decl);
    // §7 generics phase 2 — a use-site instantiation of a generic struct,
    // e.g. `Pair[Int32]`. Same nominal decl as the bare struct, but the
    // resolved type arguments are recorded in parts_ so field access and
    // construction can substitute them for the struct's generic params.
    // An empty `args` is equivalent to make_nominal(Struct, decl).
    [[nodiscard]] TypePtr make_struct_instance(const ast::Decl* decl, std::vector<TypePtr> args);
    // §7 generics phase 2 — a use-site instantiation of a generic enum,
    // e.g. `Option[Int32]`. Same nominal decl as the bare enum, with the
    // resolved type arguments in parts_. An empty `args` is equivalent to
    // make_nominal(Enum, decl).
    [[nodiscard]] TypePtr make_enum_instance(const ast::Decl* decl, std::vector<TypePtr> args);
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

    // Substitute generic-parameter occurrences in `t` with the supplied
    // mapping. Walks Optional/Vector/Tuple/Function recursively; leaves
    // primitives and nominals untouched. The result lives in this arena.
    // If `t` contains no generic params (or they're all unmapped), returns
    // `t` unchanged.
    [[nodiscard]] TypePtr substitute(TypePtr t,
                                     const std::unordered_map<std::string, TypePtr>& bindings);

private:
    std::vector<std::unique_ptr<Type>> owned_;
    // Keyed by TypeKind directly — C++14 made std::hash valid for any
    // enum class, so the static_cast<int> the map used to require is gone.
    std::unordered_map<TypeKind, const Type*> primitives_;
};

}  // namespace vestra::sema
