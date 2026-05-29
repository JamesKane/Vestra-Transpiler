// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "vestra/sema/resolver.hpp"

#include "vestra/ast/nodes.hpp"
#include "vestra/sema/builtins.hpp"
#include "vestra/sema/scope.hpp"
#include "vestra/sema/types.hpp"

#include <array>
#include <cstdlib>
#include <format>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace vestra::sema {

namespace {

// §A6 (§6.8 / §14.11.4) helper. Scans a struct's attributes for the
// `@repr(union)` marker so sema validation and (later) derive-target
// rejection share one detection point. Mirrors the codegen's
// `LayoutAttrs::is_union` reading.
bool struct_is_repr_union(const ast::StructDecl& s) {
    for (const auto& a : s.attributes) {
        if (a.name != "repr" || a.predicate == nullptr) {
            continue;
        }
        if (a.predicate->kind == ast::NodeKind::IdentExpr
            && static_cast<const ast::IdentExpr&>(*a.predicate).name == "union") {
            return true;
        }
    }
    return false;
}

// §7 generics phase 2 — count the named generic parameters of a struct or
// enum, type AND const (`[const N: Int]`) alike, since both consume a
// generic argument at a use site.
std::size_t named_type_param_count(const std::vector<ast::GenericParam>& gens) {
    std::size_t n = 0;
    for (const auto& g : gens) {
        if (!g.name.empty()) {
            ++n;
        }
    }
    return n;
}

}  // namespace

// ============================================================================
// Resolution side tables
// ============================================================================

TypePtr Resolution::type_of(const ast::Expr* e) const {
    auto it = expr_types_.find(e);
    return it == expr_types_.end() ? nullptr : it->second;
}

const Symbol* Resolution::symbol_of(const ast::Expr* e) const {
    auto it = expr_symbols_.find(e);
    return it == expr_symbols_.end() ? nullptr : it->second;
}

void Resolution::set_type(const ast::Expr* e, TypePtr t) {
    expr_types_[e] = t;
}
void Resolution::set_symbol(const ast::Expr* e, const Symbol* s) {
    expr_symbols_[e] = s;
}
const ComptimeValue* Resolution::folded_value(const ast::Expr* e) const {
    auto it = folded_.find(e);
    return it == folded_.end() ? nullptr : &it->second;
}
void Resolution::set_folded_value(const ast::Expr* e, ComptimeValue v) {
    folded_[e] = v;
}
bool Resolution::is_gated_out(const ast::Decl* d) const {
    return gated_decls_.contains(d);
}
void Resolution::mark_gated_out(const ast::Decl* d) {
    gated_decls_.insert(d);
}
TypePtr Resolution::do_catch_error_type(const ast::DoCatchExpr* dc) const {
    auto it = do_catch_error_.find(dc);
    return it == do_catch_error_.end() ? nullptr : it->second;
}
void Resolution::set_do_catch_error_type(const ast::DoCatchExpr* dc, TypePtr t) {
    if (t != nullptr) {
        do_catch_error_[dc] = t;
    }
}

// ============================================================================
// Resolver ctor + entry point
// ============================================================================

Resolver::Resolver(const ast::CompilationUnit& unit,
                   TypeArena& types,
                   diag::DiagnosticReporter& reporter,
                   ComptimeFolder::EmbedReader embed_reader,
                   TargetContext target)
    : unit_(&unit), types_(&types), reporter_(&reporter),
      folder_(&scopes_.global(), std::move(embed_reader)), target_(std::move(target)) {}

void Resolver::resolve() {
    register_builtin_capabilities();
    register_builtin_math();
    register_builtin_reflection();
    register_builtin_panic();
    // §A5 must follow register_builtin_reflection because the sync
    // intrinsics' signatures reference the Ordering / BarrierScope /
    // BarrierKind enums registered there.
    register_builtin_sync();
    collect_top_level();
    for (const auto& d : unit_->decls) {
        // §12.6: same gate as collect_top_level. Skipping here keeps the
        // body-check pass aligned with the symbols actually registered.
        if (gated_out(*d)) {
            continue;
        }
        check_decl(*d);
    }
}

// The standard `using` capabilities from §8 — they aren't user-declarable
// types yet (no real stdlib), but a function that writes `using Alloc` must
// not get an "unknown type" error for what is a load-bearing language
// primitive. We register each as a nominal Protocol-kind symbol with a
// placeholder type; the capability checker matches them by name.
void Resolver::register_builtin_capabilities() {
    static constexpr std::array<std::string_view, 11> Names = {
        "Alloc",
        "Log",
        "Async",
        "Net",
        "Clock",
        "Rand",
        "Mmio",
        "Asm",
        "RawMemory",
        "Extern",
        // §A7 (§14.13) — `InterruptsOff` is a region head, not a
        // capability you can put in a `using` row, but it's
        // registered here so `with InterruptsOff { ... }` resolves
        // through the same markered-with-binding path the existing
        // caps use. The shape rules (no waitForInterrupt /
        // waitForEvent / spawn / Alloc-using / Async-using calls
        // inside) are enforced in check_call.
        "InterruptsOff",
    };
    for (auto name : Names) {
        Symbol s;
        s.name = std::string{name};
        s.kind = SymbolKind::Protocol;
        s.decl = nullptr;
        s.type = types_->make_nominal(TypeKind::Protocol, nullptr);
        s.definition_range = {};
        s.visibility = ast::Visibility::Public;
        (void)scopes_.global().insert(std::move(s));
    }
}

// §12.1 comptime stdlib. The folder dispatches each of these by name at
// fold time (see builtins::call / builtins::value_of); here we only
// register the symbols so the resolver can type-check a `sin(theta)` or
// a `tau` reference without flagging an undeclared identifier. Phase 1
// keeps every signature Float64-only; later phases revisit polymorphism.
// §10 panic primitives. Each one returns `Never` (bottom type), so a
// call type-checks against any expected slot — `let x: Int32 =
// panic("oops")` is fine because Never is assignable to every type.
// Codegen intercepts the three names by IdentExpr in emit_expr(CallExpr)
// and lowers to the C++ runtime shims; the symbols here exist so the
// resolver sees a real Func at the call site and the call's static
// type comes out to Never.
void Resolver::register_builtin_panic() {
    auto never = types_->never();
    // §10 `panic` takes a compile-time string. v0.5 doesn't try to
    // promote `Str` / `String` (the runtime / heap-formatted strings)
    // to the panic message — most kernel and library panic sites use
    // a literal anyway, and the StrConst-only signature keeps the
    // primitive composable with @panic_handler (annex §15.5) whose
    // signature is also `(message: Str, file: StrConst, line: Int)`
    // — the future widening is a one-line change here.
    auto str_const = types_->primitive(TypeKind::StrConst);
    auto insert = [&](std::string name, std::vector<TypePtr> params) {
        Symbol s;
        s.name = std::move(name);
        s.kind = SymbolKind::Func;
        s.decl = nullptr;
        s.type = types_->make_function(std::move(params), never);
        s.definition_range = {};
        s.visibility = ast::Visibility::Public;
        (void)scopes_.global().insert(std::move(s));
    };
    insert("panic", {str_const});
    insert("abort", {});
    insert("unreachable", {});
}

void Resolver::register_builtin_sync() {
    // §A5 (§14.10) sync intrinsics. Free-function builtins surfaced
    // in global scope; codegen recognizes the names and lowers each
    // to its matching __vstr runtime shim. The Asm capability gate
    // the spec defines is not yet enforced — kernel-targeting users
    // get the same surface as hosted-targeting users in v0.5.
    auto unit = types_->unit();
    auto ord = builtin_ordering_decl_ != nullptr
                   ? types_->make_nominal(TypeKind::Enum, builtin_ordering_decl_.get())
                   : types_->error();
    auto bscope = builtin_barrier_scope_decl_ != nullptr
                      ? types_->make_nominal(TypeKind::Enum, builtin_barrier_scope_decl_.get())
                      : types_->error();
    auto bkind = builtin_barrier_kind_decl_ != nullptr
                     ? types_->make_nominal(TypeKind::Enum, builtin_barrier_kind_decl_.get())
                     : types_->error();
    auto insert = [&](std::string name, std::vector<TypePtr> params) {
        Symbol s;
        s.name = std::move(name);
        s.kind = SymbolKind::Func;
        s.decl = nullptr;
        s.type = types_->make_function(std::move(params), unit);
        s.definition_range = {};
        s.visibility = ast::Visibility::Public;
        (void)scopes_.global().insert(std::move(s));
    };
    insert("compilerFence", {ord});
    insert("memoryBarrier", {bscope, bkind});
    insert("syncBarrier", {bscope});
    insert("instructionBarrier", {});
    insert("waitForInterrupt", {});
    insert("waitForEvent", {});
    insert("signalEvent", {});
    insert("relax", {});
    insert("nop", {});

    // §A5 (§14.10.3, §14.10.4, §14.10.5) cache + TLB management.
    // Span / MutSpan over UInt8 ranges for the data/instruction
    // cache ops; the data-cache ones additionally require
    // RawMemory (gated in the capability checker, parallel to the
    // §A3 raw-mint primitives). TLB ops take a TlbScope; some take
    // a virtual address (UInt64) and a large-page flag (Bool).
    auto u8_span = types_->make_span(types_->primitive(TypeKind::UInt8));
    auto u8_mut_span = types_->make_mut_span(types_->primitive(TypeKind::UInt8));
    auto u64 = types_->primitive(TypeKind::UInt64);
    auto u16 = types_->primitive(TypeKind::UInt16);
    auto boolean = types_->boolean();
    auto tlb_scope = builtin_tlb_scope_decl_ != nullptr
                         ? types_->make_nominal(TypeKind::Enum, builtin_tlb_scope_decl_.get())
                         : types_->error();
    insert("cleanData", {u8_span});
    insert("invalidateData", {u8_span});
    insert("cleanInvalidateData", {u8_span});
    insert("zeroData", {u8_mut_span});
    insert("publishInstructions", {u8_span});
    insert("invalidateAllInstructions", {});
    insert("tlbInvalidateAll", {tlb_scope});
    insert("tlbInvalidatePage", {u64, boolean, tlb_scope});
    insert("tlbInvalidateAsid", {u16, tlb_scope});

    // §A10 (§15.4) compiler-emitted intrinsics — `memcpy`, `memset`,
    // `memmove`. The spec lists them as Vestra-supplied bodies behind
    // `@symbol`; v0.5 registers them as builtin free functions whose
    // bodies are the compiler's `__builtin_mem*` shims. A freestanding
    // profile with `no_libc = true` (§15.5) will eventually require an
    // in-tree binding for each; today the compiler's intrinsic
    // expansion is what links, which is also "no libc" on every host
    // we target. All three need RawMemory — they walk byte ranges
    // through MutPtr / Ptr without the slice's escape rules
    // intervening, which is exactly what `RawMemory` gates.
    auto u8 = types_->primitive(TypeKind::UInt8);
    auto i_natural = types_->primitive(TypeKind::Int);
    auto u8_mut_ptr = types_->make_mut_ptr(u8);
    auto u8_ptr = types_->make_ptr(u8);
    insert("memcpy", {u8_mut_ptr, u8_ptr, i_natural});
    insert("memset", {u8_mut_ptr, u8, i_natural});
    insert("memmove", {u8_mut_ptr, u8_ptr, i_natural});
}

void Resolver::register_builtin_math() {
    auto f64 = types_->primitive(TypeKind::Float64);

    // Constants first — they're plain Const symbols, just typed Float64.
    // We don't pre-populate comptime_env_ here: the folder consults
    // builtins::lookup_const directly when an ident lookup misses, so
    // the symbol's only role is satisfying the resolver.
    for (const auto& c : builtins::constants()) {
        Symbol s;
        s.name = std::string{c.name};
        s.kind = SymbolKind::Const;
        s.decl = nullptr;
        s.type = f64;
        s.definition_range = {};
        s.visibility = ast::Visibility::Public;
        (void)scopes_.global().insert(std::move(s));
    }

    // Functions: each registers as a Func symbol whose type is the right
    // (Float64, ...) -> Float64 signature. Calls into these go through
    // the resolver's normal call-typing path because the function type
    // is already correct.
    for (const auto& f : builtins::functions()) {
        std::vector<TypePtr> params(f.arity, f64);
        Symbol s;
        s.name = std::string{f.name};
        s.kind = SymbolKind::Func;
        s.decl = nullptr;
        s.type = types_->make_function(std::move(params), f64);
        s.definition_range = {};
        s.visibility = ast::Visibility::Public;
        (void)scopes_.global().insert(std::move(s));
    }
}

// §12.2 reflection: synthesize the `Type` and `Field` structs so
// `T.fields[i].name` and `T.fields[i].type.name` type-check through
// the resolver's ordinary lookup_field path. Both are comptime-only —
// there's no codegen story for a runtime Type/Field-typed value yet,
// and the spec reads reflection as a fold-time activity. Later
// phases extend Field with `offset` + the attribute list.
//
// Type carries `name: Str` (the type's display name). Field carries
// `name: Str` then `type: Type`, in *that* declaration order — the
// folder reads off the same indices when serving member access on a
// folded Field value (elements[0]=name, elements[1]=type).
void Resolver::register_builtin_reflection() {
    // Build `Type` first so Field's `type: Type` field can resolve
    // against the freshly-registered nominal.
    {
        auto str_type = std::make_unique<ast::NamedType>();
        str_type->path = {"Str"};

        auto decl = std::make_unique<ast::StructDecl>();
        decl->name = "Type";
        decl->visibility = ast::Visibility::Public;
        ast::StructDecl::Field name_field;
        name_field.name = "name";
        name_field.kind = ast::StructDecl::Field::Kind::Let;
        name_field.type = std::move(str_type);
        decl->fields.push_back(std::move(name_field));
        builtin_type_decl_ = std::move(decl);

        Symbol s;
        s.name = "Type";
        s.kind = SymbolKind::Struct;
        s.decl = builtin_type_decl_.get();
        s.type = types_->make_nominal(TypeKind::Struct, builtin_type_decl_.get());
        s.definition_range = {};
        s.visibility = ast::Visibility::Public;
        (void)scopes_.global().insert(std::move(s));
    }

    {
        // §A2 (§6.8) Field reflection: `name`, `type`, plus the three
        // layout knobs `offset`, `size`, `alignment`. The folder
        // computes the latter via the same C-style padding walk that
        // drives `T.size` / `T.alignment`, and stashes them at
        // positional indices 2..4 of each Field comptime value. The
        // declaration's field-list shape is the single source of
        // truth — sema looks up by name through the resolver's
        // ordinary member-access path, the folder reads by index.
        auto str_type = std::make_unique<ast::NamedType>();
        str_type->path = {"Str"};
        auto type_type = std::make_unique<ast::NamedType>();
        type_type->path = {"Type"};
        auto int_offset_t = std::make_unique<ast::NamedType>();
        int_offset_t->path = {"Int"};
        auto int_size_t = std::make_unique<ast::NamedType>();
        int_size_t->path = {"Int"};
        auto int_align_t = std::make_unique<ast::NamedType>();
        int_align_t->path = {"Int"};

        auto decl = std::make_unique<ast::StructDecl>();
        decl->name = "Field";
        decl->visibility = ast::Visibility::Public;
        ast::StructDecl::Field name_field;
        name_field.name = "name";
        name_field.kind = ast::StructDecl::Field::Kind::Let;
        name_field.type = std::move(str_type);
        decl->fields.push_back(std::move(name_field));
        ast::StructDecl::Field type_field;
        type_field.name = "type";
        type_field.kind = ast::StructDecl::Field::Kind::Let;
        type_field.type = std::move(type_type);
        decl->fields.push_back(std::move(type_field));
        ast::StructDecl::Field offset_field;
        offset_field.name = "offset";
        offset_field.kind = ast::StructDecl::Field::Kind::Let;
        offset_field.type = std::move(int_offset_t);
        decl->fields.push_back(std::move(offset_field));
        ast::StructDecl::Field size_field;
        size_field.name = "size";
        size_field.kind = ast::StructDecl::Field::Kind::Let;
        size_field.type = std::move(int_size_t);
        decl->fields.push_back(std::move(size_field));
        ast::StructDecl::Field align_field;
        align_field.name = "alignment";
        align_field.kind = ast::StructDecl::Field::Kind::Let;
        align_field.type = std::move(int_align_t);
        decl->fields.push_back(std::move(align_field));
        builtin_field_decl_ = std::move(decl);

        Symbol s;
        s.name = "Field";
        s.kind = SymbolKind::Struct;
        s.decl = builtin_field_decl_.get();
        s.type = types_->make_nominal(TypeKind::Struct, builtin_field_decl_.get());
        s.definition_range = {};
        s.visibility = ast::Visibility::Public;
        (void)scopes_.global().insert(std::move(s));
    }

    // §A5 (§14.10.2) BarrierScope + BarrierKind: the two enums that
    // parameterize Cpu.memoryBarrier and Cpu.syncBarrier. Both lower
    // to C++ enum classes that emit at file scope in the runtime
    // preamble, so `.full` / `.loadLoad` etc. resolve naturally
    // through the leading-dot-case path.
    {
        auto decl = std::make_unique<ast::EnumDecl>();
        decl->name = "BarrierScope";
        decl->visibility = ast::Visibility::Public;
        for (const char* name : {"full", "inner", "outer", "nonShareable"}) {
            ast::EnumDecl::Case c;
            c.name = name;
            decl->cases.push_back(std::move(c));
        }
        builtin_barrier_scope_decl_ = std::move(decl);

        Symbol s;
        s.name = "BarrierScope";
        s.kind = SymbolKind::Enum;
        s.decl = builtin_barrier_scope_decl_.get();
        s.type = types_->make_nominal(TypeKind::Enum, builtin_barrier_scope_decl_.get());
        s.visibility = ast::Visibility::Public;
        (void)scopes_.global().insert(std::move(s));
    }
    {
        auto decl = std::make_unique<ast::EnumDecl>();
        decl->name = "BarrierKind";
        decl->visibility = ast::Visibility::Public;
        for (const char* name : {"loadLoad", "storeStore", "loadStore", "full"}) {
            ast::EnumDecl::Case c;
            c.name = name;
            decl->cases.push_back(std::move(c));
        }
        builtin_barrier_kind_decl_ = std::move(decl);

        Symbol s;
        s.name = "BarrierKind";
        s.kind = SymbolKind::Enum;
        s.decl = builtin_barrier_kind_decl_.get();
        s.type = types_->make_nominal(TypeKind::Enum, builtin_barrier_kind_decl_.get());
        s.visibility = ast::Visibility::Public;
        (void)scopes_.global().insert(std::move(s));
    }

    // §A7 (§14.14) Context: the opaque task-saving slot Scheduler
    // swaps between. No user-visible fields — the layout is the
    // target description's concern. Codegen emits a `__vstr::Context`
    // wrapper struct with a fixed byte buffer; for v0.5 we just
    // expose the type so user code can declare static slots and
    // pass them to swapContext.
    {
        auto decl = std::make_unique<ast::StructDecl>();
        decl->name = "Context";
        decl->visibility = ast::Visibility::Public;
        builtin_context_decl_ = std::move(decl);

        Symbol s;
        s.name = "Context";
        s.kind = SymbolKind::Struct;
        s.decl = builtin_context_decl_.get();
        s.type = types_->make_nominal(TypeKind::Struct, builtin_context_decl_.get());
        s.visibility = ast::Visibility::Public;
        (void)scopes_.global().insert(std::move(s));
    }

    // §A6 (§14.11.3) Endianness: three cases for the MmioWireView
    // constructor. `native` is the no-swap pass-through; `little` /
    // `big` pin the device's bus endianness so the read/write paths
    // swap when the host's native endianness doesn't match.
    {
        auto decl = std::make_unique<ast::EnumDecl>();
        decl->name = "Endianness";
        decl->visibility = ast::Visibility::Public;
        for (const char* name : {"little", "big", "native"}) {
            ast::EnumDecl::Case c;
            c.name = name;
            decl->cases.push_back(std::move(c));
        }
        builtin_endianness_decl_ = std::move(decl);

        Symbol s;
        s.name = "Endianness";
        s.kind = SymbolKind::Enum;
        s.decl = builtin_endianness_decl_.get();
        s.type = types_->make_nominal(TypeKind::Enum, builtin_endianness_decl_.get());
        s.visibility = ast::Visibility::Public;
        (void)scopes_.global().insert(std::move(s));
    }

    // §A5 (§14.10.5) TlbScope: four cases for TLB-invalidation
    // breadth. Mirrors the spec's aarch64-shaped names; x86 / RISC-V
    // targets translate them to their analogues at codegen.
    {
        auto decl = std::make_unique<ast::EnumDecl>();
        decl->name = "TlbScope";
        decl->visibility = ast::Visibility::Public;
        for (const char* name : {"currentEL", "allELs", "innerShareable", "outerShareable"}) {
            ast::EnumDecl::Case c;
            c.name = name;
            decl->cases.push_back(std::move(c));
        }
        builtin_tlb_scope_decl_ = std::move(decl);

        Symbol s;
        s.name = "TlbScope";
        s.kind = SymbolKind::Enum;
        s.decl = builtin_tlb_scope_decl_.get();
        s.type = types_->make_nominal(TypeKind::Enum, builtin_tlb_scope_decl_.get());
        s.visibility = ast::Visibility::Public;
        (void)scopes_.global().insert(std::move(s));
    }

    // §A4 (§14.9.1) Ordering: a bare enum with five cases. `.relaxed`
    // etc. resolve through the existing leading-dot-case path; sema's
    // method synthesis on Atomic[T] uses Ordering as the param type so
    // `counter.fetchAdd(1, .release)` types automatically. Registered
    // before the user's code so a user-defined `Ordering` would have
    // to shadow it (and produce a duplicate-definition diagnostic).
    {
        auto decl = std::make_unique<ast::EnumDecl>();
        decl->name = "Ordering";
        decl->visibility = ast::Visibility::Public;
        for (const char* name : {"relaxed", "acquire", "release", "acqRel", "seqCst"}) {
            ast::EnumDecl::Case c;
            c.name = name;
            decl->cases.push_back(std::move(c));
        }
        builtin_ordering_decl_ = std::move(decl);

        Symbol s;
        s.name = "Ordering";
        s.kind = SymbolKind::Enum;
        s.decl = builtin_ordering_decl_.get();
        s.type = types_->make_nominal(TypeKind::Enum, builtin_ordering_decl_.get());
        s.definition_range = {};
        s.visibility = ast::Visibility::Public;
        (void)scopes_.global().insert(std::move(s));
    }
}

void Resolver::error_at(diag::SourceRange r, std::string msg) {
    reporter_->report(diag::Diagnostic::error(std::move(msg)).at(r));
}

void Resolver::warn_at(diag::SourceRange r, std::string msg) {
    reporter_->report(diag::Diagnostic::warning(std::move(msg)).at(r));
}

std::string Resolver::check_wide_atomic_feature(diag::SourceRange /*r*/,
                                                std::string_view which) const {
    // §A4 (§14.9.4) — gate wide atomics on the matching architectural
    // feature. The hosted default (arch="host") bypasses the check
    // since the host C++ compiler decides via libatomic at link
    // time; explicit non-host targets need the feature in scope or
    // the user gets a silent lock-based fallback they didn't ask
    // for. The pre-architectural targets named here cover the
    // arches with a 128-bit CAS in v0.5; other arches (RISC-V at
    // its current ratified ISA, etc.) don't admit wide atomics at
    // all.
    if (target_.arch == "host" || target_.arch.empty()) {
        return {};
    }
    if (target_.arch == "aarch64") {
        if (target_.has_feature("lse2")) {
            return {};
        }
        return std::format(
            "{}[T] requires the 'lse2' target feature on aarch64 (the architectural 128-bit "
            "compare-and-swap); pass --target-features=lse2 or pick a target that has it",
            which);
    }
    if (target_.arch == "x86_64") {
        if (target_.has_feature("cx16")) {
            return {};
        }
        return std::format("{}[T] requires the 'cx16' target feature on x86_64 (CMPXCHG16B); pass "
                           "--target-features=cx16 or pick a target that has it",
                           which);
    }
    return std::format(
        "{}[T] is not supported on target '{}'; v0.5 admits wide atomics on aarch64+lse2 "
        "and x86_64+cx16",
        which,
        target_.arch);
}

void Resolver::duplicate_definition(const Symbol& existing,
                                    std::string_view name,
                                    diag::SourceRange new_range) {
    auto d =
        diag::Diagnostic::error(std::format("duplicate definition of '{}'", name)).at(new_range);
    if (existing.definition_range.is_valid()) {
        d.with_note(diag::Diagnostic::note(std::format("previous definition of '{}' here", name))
                        .at(existing.definition_range));
    }
    reporter_->report(std::move(d));
}

// ============================================================================
// Pass 1: collect every top-level declaration into the global scope.
// ============================================================================

void Resolver::collect_top_level() {
    for (const auto& d : unit_->decls) {
        // §12.6: skip `@when`-gated-out decls before they enter any scope.
        // Other passes (check_decl, codegen) will skip them the same way,
        // so a gated-out decl is effectively absent from the program.
        if (gated_out(*d)) {
            continue;
        }
        switch (d->kind) {
        case ast::NodeKind::Func:
            collect_func(static_cast<const ast::FuncDecl&>(*d));
            break;
        case ast::NodeKind::Struct:
            collect_struct(static_cast<const ast::StructDecl&>(*d));
            break;
        case ast::NodeKind::Enum:
            collect_enum(static_cast<const ast::EnumDecl&>(*d));
            break;
        case ast::NodeKind::Protocol:
            collect_protocol(static_cast<const ast::ProtocolDecl&>(*d));
            break;
        case ast::NodeKind::Opaque:
            collect_opaque(static_cast<const ast::OpaqueDecl&>(*d));
            break;
        case ast::NodeKind::Const:
            collect_const(static_cast<const ast::ConstDecl&>(*d));
            break;
        case ast::NodeKind::Static:
            collect_static(static_cast<const ast::StaticDecl&>(*d));
            break;
        case ast::NodeKind::Extension:
            // Extensions don't introduce a new top-level name; their members
            // are surfaced via their target type. We resolve them in pass 2.
            break;
        case ast::NodeKind::Derive:
            // `derive(...)` is sugar for empty extensions; nothing to bind.
            break;
        default:
            break;
        }
    }
}

void Resolver::collect_func(const ast::FuncDecl& f) {
    Symbol s;
    s.name = f.name;
    s.kind = SymbolKind::Func;
    s.decl = &f;
    s.type = function_type_of(f);
    s.definition_range = f.range;
    s.visibility = f.visibility;
    if (auto* prev = scopes_.global().insert(std::move(s))) {
        duplicate_definition(*prev, f.name, f.range);
    }
}

void Resolver::collect_struct(const ast::StructDecl& s_) {
    Symbol s;
    s.name = s_.name;
    s.kind = SymbolKind::Struct;
    s.decl = &s_;
    s.type = types_->make_nominal(TypeKind::Struct, &s_);
    s.definition_range = s_.range;
    s.visibility = s_.visibility;
    if (auto* prev = scopes_.global().insert(std::move(s))) {
        duplicate_definition(*prev, s_.name, s_.range);
    }
}

void Resolver::collect_enum(const ast::EnumDecl& e) {
    Symbol s;
    s.name = e.name;
    s.kind = SymbolKind::Enum;
    s.decl = &e;
    s.type = types_->make_nominal(TypeKind::Enum, &e);
    s.definition_range = e.range;
    s.visibility = e.visibility;
    if (auto* prev = scopes_.global().insert(std::move(s))) {
        duplicate_definition(*prev, e.name, e.range);
    }
}

void Resolver::collect_protocol(const ast::ProtocolDecl& p) {
    Symbol s;
    s.name = p.name;
    s.kind = SymbolKind::Protocol;
    s.decl = &p;
    s.type = types_->make_nominal(TypeKind::Protocol, &p);
    s.definition_range = p.range;
    s.visibility = p.visibility;
    if (auto* prev = scopes_.global().insert(std::move(s))) {
        duplicate_definition(*prev, p.name, p.range);
    }
}

void Resolver::collect_opaque(const ast::OpaqueDecl& o) {
    Symbol s;
    s.name = o.name;
    s.kind = SymbolKind::OpaqueType;
    s.decl = &o;
    s.type = types_->make_nominal(TypeKind::OpaqueType, &o);
    s.definition_range = o.range;
    s.visibility = o.visibility;
    if (auto* prev = scopes_.global().insert(std::move(s))) {
        duplicate_definition(*prev, o.name, o.range);
    }
}

void Resolver::collect_const(const ast::ConstDecl& c) {
    Symbol s;
    s.name = c.name;
    s.kind = SymbolKind::Const;
    s.decl = &c;
    s.type = c.type ? resolve_type(*c.type) : nullptr;
    s.definition_range = c.range;
    s.visibility = c.visibility;
    if (auto* prev = scopes_.global().insert(std::move(s))) {
        duplicate_definition(*prev, c.name, c.range);
    }
}

void Resolver::collect_static(const ast::StaticDecl& s_) {
    Symbol s;
    s.name = s_.name;
    s.kind = SymbolKind::Static;
    s.decl = &s_;
    s.type = s_.type ? resolve_type(*s_.type) : nullptr;
    s.definition_range = s_.range;
    s.visibility = s_.visibility;
    if (auto* prev = scopes_.global().insert(std::move(s))) {
        duplicate_definition(*prev, s_.name, s_.range);
    }
}

// ============================================================================
// Function-type construction
// ============================================================================

TypePtr Resolver::function_type_of(const ast::FuncDecl& f) {
    // §7 generics: the function's own type parameters need to resolve while
    // we walk its signature. We push a temporary scope holding each generic
    // param as an opaque GenericParam type, so `func max[T](a: T) -> T`
    // resolves T to its placeholder rather than failing with "unknown type".
    // The scope is popped at the end of this call.
    ScopeStack::Guard g(scopes_);
    for (const auto& gp : f.generics) {
        if (gp.name.empty() || gp.is_const) {
            continue;  // const generics aren't types; phase 1 ignores them
        }
        Symbol s;
        s.name = gp.name;
        s.kind = SymbolKind::GenericParam;
        s.type = types_->make_generic_param(gp.name);
        s.definition_range = gp.range;
        (void)g.scope().insert(std::move(s));
    }

    std::vector<TypePtr> params;
    params.reserve(f.params.size());
    for (const auto& p : f.params) {
        params.push_back(p.type ? resolve_type(*p.type) : types_->error());
    }
    TypePtr result = f.result ? resolve_type(*f.result) : types_->unit();
    // §9: a `throws(E)` clause widens the *external* result to Result<T, E>
    // so callers see the fallible signature; the body itself still talks
    // about T and E individually (handled in check_func via return_stack_
    // and throws_stack_).
    if (f.effects.throws_type) {
        TypePtr err = resolve_type(*f.effects.throws_type);
        result = types_->make_result(result, err);
    }
    return types_->make_function(std::move(params), result);
}

// ============================================================================
// Pass 2: visit each decl body, resolving names and computing types.
// ============================================================================

void Resolver::check_decl(const ast::Decl& d) {
    switch (d.kind) {
    case ast::NodeKind::Func:
        check_func(static_cast<const ast::FuncDecl&>(d));
        break;
    case ast::NodeKind::Const: {
        const auto& c = static_cast<const ast::ConstDecl&>(d);
        const auto* sym = scopes_.global().lookup_local(c.name);
        TypePtr annotated = sym != nullptr ? sym->type : nullptr;
        if (c.value) {
            auto value_type = check_expr(*c.value, annotated);
            if (annotated != nullptr && !TypeArena::assignable(value_type, annotated)) {
                error_at(c.value->range,
                         std::format("const value of type {} does not match annotation {}",
                                     value_type ? value_type->describe() : "?",
                                     annotated->describe()));
            }
            // §12.1 fold attempt: if the initializer is a pure constant
            // expression, evaluate it now and record the result. The hint is
            // the annotated TypeKind so an integer literal folds to the
            // declared concrete width (e.g. const X: Int32 = 1 << 8 - 1).
            TypeKind hint = annotated != nullptr ? annotated->kind() : TypeKind::Unit;
            if (auto folded = folder_.fold(*c.value, comptime_env_, hint)) {
                resolution_.set_folded_value(c.value.get(), *folded);
                comptime_env_[c.name] = *folded;
            }
        }
        break;
    }
    case ast::NodeKind::Static: {
        const auto& s = static_cast<const ast::StaticDecl&>(d);
        const auto* sym = scopes_.global().lookup_local(s.name);
        TypePtr annotated = sym != nullptr ? sym->type : nullptr;
        if (s.value) {
            auto value_type = check_expr(*s.value, annotated);
            if (annotated != nullptr && !TypeArena::assignable(value_type, annotated)) {
                error_at(s.value->range,
                         std::format("static value of type {} does not match annotation {}",
                                     value_type ? value_type->describe() : "?",
                                     annotated->describe()));
            }
        }
        // §A1 (§4.5): a `@noinit static` reserves uninitialized storage
        // in .bss and *must* have a type annotation (sema can't infer
        // the size of a slot with no initializer). It also can't carry
        // an initializer — the parser already rejects that, but we
        // double-check here in case future parser changes thread a
        // value through anyway.
        if (s.noinit) {
            if (s.type == nullptr) {
                error_at(s.range, "@noinit static requires a type annotation");
            }
            if (s.value) {
                error_at(s.value->range,
                         "@noinit static cannot carry an initializer (its storage lands in .bss)");
            }
        }
        check_link_attributes(s.attributes);
        break;
    }
    case ast::NodeKind::Struct: {
        // Resolve member function bodies, if any. Field types were already
        // resolved on demand by callers; we don't yet type-check defaults.
        const auto& s = static_cast<const ast::StructDecl&>(d);
        // §7 generics phase 2 — bind the struct's type parameters as opaque
        // GenericParam placeholders for the duration of decl checking so a
        // method signature referencing `T` resolves rather than failing
        // with "unknown type". For a non-generic struct this pushes an
        // empty scope and is a no-op.
        ScopeStack::Guard struct_generics_g(scopes_);
        for (const auto& gp : s.generics) {
            if (gp.name.empty()) {
                continue;
            }
            Symbol gs;
            gs.name = gp.name;
            gs.kind = SymbolKind::GenericParam;
            gs.type = types_->make_generic_param(gp.name);
            gs.definition_range = gp.range;
            (void)struct_generics_g.scope().insert(std::move(gs));
        }
        for (const auto& m : s.methods) {
            if (m->kind == ast::NodeKind::Func) {
                check_func(static_cast<const ast::FuncDecl&>(*m));
            }
        }
        // §A6 (§6.8 / §14.11.4) `@repr(union)` validation. The struct
        // is re-emitted as a C++ union — its members overlay one
        // storage cell. v0.5 enforces two narrow rules:
        //   (1) at least one field (else the C++ union body is empty
        //       and the type is useless).
        //   (2) `@bits(N)` on a direct member is rejected. Bit-fields
        //       only make sense inside a packed sub-struct, which then
        //       overlays the integer cell. A bit-field on the union
        //       itself can't share storage with the integer view, so
        //       the spec's register-decoding pattern would silently
        //       break.
        // Spec also requires transitively Trivial + Copyable; we defer
        // that to the C++ compiler, which refuses unions whose members
        // have non-trivial special member functions.
        if (struct_is_repr_union(s)) {
            if (s.fields.empty()) {
                error_at(s.range,
                         std::format("@repr(union) struct '{}' must declare at least one field",
                                     s.name));
            }
            for (const auto& f : s.fields) {
                for (const auto& a : f.attributes) {
                    if (a.name == "bits") {
                        error_at(a.range,
                                 std::format("@bits on a direct member of @repr(union) '{}' is "
                                             "rejected; put bit-fields inside a packed sub-struct "
                                             "and overlay it",
                                             s.name));
                    }
                }
            }
        }
        break;
    }
    case ast::NodeKind::Enum: {
        const auto& e = static_cast<const ast::EnumDecl&>(d);
        // §7 generics phase 2 — bind the enum's type parameters as opaque
        // placeholders for the duration of decl checking so a method
        // signature referencing `T` resolves. No-op for a non-generic enum.
        ScopeStack::Guard enum_generics_g(scopes_);
        for (const auto& gp : e.generics) {
            if (gp.name.empty()) {
                continue;
            }
            Symbol gs;
            gs.name = gp.name;
            gs.kind = SymbolKind::GenericParam;
            gs.type = types_->make_generic_param(gp.name);
            gs.definition_range = gp.range;
            (void)enum_generics_g.scope().insert(std::move(gs));
        }
        for (const auto& m : e.methods) {
            if (m->kind == ast::NodeKind::Func) {
                check_func(static_cast<const ast::FuncDecl&>(*m));
            }
        }
        break;
    }
    case ast::NodeKind::Extension: {
        const auto& e = static_cast<const ast::ExtensionDecl&>(d);
        for (const auto& m : e.members) {
            check_decl(*m);
        }
        break;
    }
    case ast::NodeKind::Derive: {
        // §12.3 derive(Default) validation: every field of the target
        // struct must itself be Default-conformant. The other derives
        // (Eq / Hash / Clone / Debug / Display) defer field-conformance
        // to the C++ compiler since the underlying op is defaulted /
        // member-wise; Default is the one where the spec is explicit
        // about field-level checking.
        const auto& dd = static_cast<const ast::DeriveDecl&>(d);
        // §A6 (§6.8 / §14.11.4) `@repr(union)` is an untagged overlay
        // — the compiler can't pick which member to compare or hash
        // structurally, so the auto-derives don't apply. Reject any
        // derive targeting a union struct with a clear message rather
        // than letting the C++ compiler fail on `operator== = default`
        // for a union type.
        if (dd.target != nullptr && dd.target->kind == ast::NodeKind::NamedType) {
            const auto& tt = static_cast<const ast::NamedType&>(*dd.target);
            if (!tt.path.empty()) {
                const auto* tsym = scopes_.global().lookup_local(tt.path.back());
                if (tsym != nullptr && tsym->kind == SymbolKind::Struct && tsym->decl != nullptr) {
                    const auto& tsd = static_cast<const ast::StructDecl&>(*tsym->decl);
                    if (struct_is_repr_union(tsd)) {
                        error_at(dd.target->range,
                                 std::format("derive on @repr(union) struct '{}' is rejected; "
                                             "untagged overlays don't have a structural "
                                             "comparison or hash",
                                             tsd.name));
                        break;
                    }
                }
            }
        }
        bool derives_default = false;
        for (const auto& proto : dd.protocols) {
            if (proto == nullptr || proto->kind != ast::NodeKind::NamedType) {
                continue;
            }
            const auto& nt = static_cast<const ast::NamedType&>(*proto);
            if (!nt.path.empty() && nt.path.back() == "Default") {
                derives_default = true;
                break;
            }
        }
        if (!derives_default || dd.target == nullptr) {
            break;
        }
        // §3 follow-on: an opaque newtype is admissible as a
        // derive(Default) target. The check is "is the underlying
        // type Default-conformant" — the opaque's `enum class T :
        // Underlying {}` C++ form gives `T()` = `T{0}` automatically,
        // so no codegen change is needed.
        if (dd.target->kind == ast::NodeKind::NamedType) {
            const auto& tt = static_cast<const ast::NamedType&>(*dd.target);
            if (!tt.path.empty()) {
                const auto* osym = scopes_.global().lookup_local(tt.path.back());
                if (osym != nullptr && osym->kind == SymbolKind::OpaqueType
                    && osym->decl != nullptr) {
                    const auto& od = static_cast<const ast::OpaqueDecl&>(*osym->decl);
                    if (od.underlying != nullptr) {
                        auto under = resolve_type(*od.underlying);
                        if (!is_default_conformant(under)) {
                            error_at(dd.target->range,
                                     std::format("derive(Default) for opaque '{}': underlying "
                                                 "type {} is not Default-conformant",
                                                 od.name,
                                                 under != nullptr ? under->describe() : "?"));
                        }
                    }
                    break;
                }
            }
        }
        // Resolve the target into a Struct decl. Anything else (enum,
        // protocol) is rejected with a v0.5 diagnostic.
        if (dd.target->kind != ast::NodeKind::NamedType) {
            error_at(dd.target->range, "derive(Default) target must be a struct");
            break;
        }
        const auto& tt = static_cast<const ast::NamedType&>(*dd.target);
        if (tt.path.empty()) {
            break;
        }
        const auto* sym = scopes_.global().lookup_local(tt.path.back());
        if (sym == nullptr || sym->kind != SymbolKind::Struct || sym->decl == nullptr) {
            error_at(
                dd.target->range,
                std::format("derive(Default) for '{}' requires a struct target", tt.path.back()));
            break;
        }
        const auto& sd = static_cast<const ast::StructDecl&>(*sym->decl);
        // §7 generics phase 2 — bind the target struct's type parameters as
        // placeholders so a field typed `T` resolves to a GenericParam
        // (deferred-conformant) rather than failing with "unknown type T".
        ScopeStack::Guard derive_generics_g(scopes_);
        for (const auto& gp : sd.generics) {
            if (gp.name.empty()) {
                continue;
            }
            Symbol gs;
            gs.name = gp.name;
            gs.kind = SymbolKind::GenericParam;
            gs.type = types_->make_generic_param(gp.name);
            gs.definition_range = gp.range;
            (void)derive_generics_g.scope().insert(std::move(gs));
        }
        for (const auto& f : sd.fields) {
            if (f.kind == ast::StructDecl::Field::Kind::Embed) {
                continue;  // embed fields are checked through the embedded struct's own derive
            }
            if (f.type == nullptr) {
                continue;
            }
            TypePtr ft = resolve_type(*f.type);
            if (!is_default_conformant(ft)) {
                error_at(f.range,
                         std::format("derive(Default) for '{}': field '{}' of type {} is not "
                                     "Default-conformant",
                                     sd.name,
                                     f.name,
                                     ft != nullptr ? ft->describe() : "?"));
            }
        }
        break;
    }
    default:
        break;
    }
}

void Resolver::check_link_attributes(const std::vector<ast::Attribute>& attrs) {
    // §A1 (§6.7): shape-check the symbol-level attributes. We accept
    // the common case for each — string literal for
    // @section/@symbol/@alias; bare for @weak/@noinit; an anonymous
    // member-access (`.default` / `.hidden` / `.protected`) for
    // @visibility — and emit a clear diagnostic otherwise. No type or
    // effect checks here: these are link-time data and don't
    // influence Vestra-side types.
    for (const auto& a : attrs) {
        if (a.name == "section" || a.name == "symbol" || a.name == "alias") {
            if (a.predicate == nullptr || a.predicate->kind != ast::NodeKind::StringLit) {
                error_at(a.range, std::format("@{} expects a string literal argument", a.name));
            }
        } else if (a.name == "extern") {
            // §A12 (§14.6.1) `@extern("conv")` accepts any calling
            // convention the target backend names. The set is
            // target-determined; the spec calls out a baseline
            // every supported target carries — sema validates
            // against it, codegen emits the matching gnu::*-abi
            // attribute or `extern "C"` linkage where available.
            if (a.predicate == nullptr || a.predicate->kind != ast::NodeKind::StringLit) {
                error_at(a.range, "@extern expects a string literal argument naming the conv");
            } else {
                std::string_view conv = static_cast<const ast::StringLit&>(*a.predicate).text;
                static constexpr std::array<std::string_view, 9> known_convs = {
                    "C",
                    "hvc64",
                    "smc64",
                    "sysv64",
                    "win64",
                    "ms_abi",
                    "aapcs",
                    "riscv",
                    "interrupt",
                };
                bool ok = false;
                for (auto k : known_convs) {
                    if (conv == k) {
                        ok = true;
                        break;
                    }
                }
                if (!ok) {
                    error_at(a.range,
                             std::format("@extern(\"{}\") is not a recognized calling convention; "
                                         "expected one of: C, hvc64, smc64, sysv64, win64, "
                                         "ms_abi, aapcs, riscv, interrupt",
                                         conv));
                }
            }
        } else if (a.name == "weak" || a.name == "noinit") {
            if (a.predicate != nullptr) {
                error_at(a.range, std::format("@{} takes no arguments", a.name));
            }
        } else if (a.name == "inline") {
            // §A1 (§7.8) — `@inline(.always | .never | .hint)`. The
            // argument is a leading-dot case-name; sema enforces the
            // three legal spellings here so codegen can dispatch on
            // them without re-validating.
            std::string_view case_name;
            if (a.predicate != nullptr && a.predicate->kind == ast::NodeKind::LeadingDotExpr) {
                case_name = static_cast<const ast::LeadingDotExpr&>(*a.predicate).name;
            }
            if (case_name.empty()) {
                error_at(a.range, "@inline expects one of .always, .never, .hint");
            } else if (case_name != "always" && case_name != "never" && case_name != "hint") {
                error_at(
                    a.range,
                    std::format("@inline(.{}) — expected .always, .never, or .hint", case_name));
            }
        } else if (a.name == "visibility") {
            // The argument is `.default` / `.hidden` / `.protected` —
            // an anonymous case name. The parser produces a
            // LeadingDotExpr here (not MemberExpr — there's no base).
            std::string_view case_name;
            if (a.predicate != nullptr && a.predicate->kind == ast::NodeKind::LeadingDotExpr) {
                case_name = static_cast<const ast::LeadingDotExpr&>(*a.predicate).name;
            }
            if (case_name.empty()) {
                error_at(a.range, "@visibility expects one of .default, .hidden, .protected");
            } else if (case_name != "default" && case_name != "hidden"
                       && case_name != "protected") {
                error_at(a.range,
                         std::format("@visibility(.{}) — expected .default, .hidden, "
                                     "or .protected",
                                     case_name));
            }
        } else if (a.name == "stack_protector") {
            // §A10 (§15.4) `@stack_protector(.none | .strong | .all)`
            // selects canary instrumentation on a per-function basis.
            // The leading-dot case-name pattern matches @inline /
            // @visibility; the legal spellings come straight from the
            // spec.
            std::string_view case_name;
            if (a.predicate != nullptr && a.predicate->kind == ast::NodeKind::LeadingDotExpr) {
                case_name = static_cast<const ast::LeadingDotExpr&>(*a.predicate).name;
            }
            if (case_name.empty()) {
                error_at(a.range, "@stack_protector expects one of .none, .strong, .all");
            } else if (case_name != "none" && case_name != "strong" && case_name != "all") {
                error_at(a.range,
                         std::format("@stack_protector(.{}) — expected .none, .strong, or .all",
                                     case_name));
            }
        }
    }
}

void Resolver::check_func(const ast::FuncDecl& f) {
    check_link_attributes(f.attributes);

    // §A10 (§15.5) `@panic_handler` shape rules. Exactly one per
    // unit; signature must be `(message: Str, file: StrConst, line:
    // Int) -> Never`; the `using` row must be empty (the handler
    // runs in a context where the program's capability bindings
    // can't be assumed live). The "exactly one per binary" cross-
    // TU rule is the linker's concern; sema enforces only the
    // per-unit count.
    for (const auto& a : f.attributes) {
        if (a.name != "panic_handler") {
            continue;
        }
        if (panic_handler_decl_ != nullptr && panic_handler_decl_ != &f) {
            auto d = diag::Diagnostic::error("duplicate @panic_handler in this unit").at(a.range);
            if (panic_handler_decl_->range.is_valid()) {
                d.with_note(diag::Diagnostic::note("previous @panic_handler here")
                                .at(panic_handler_decl_->range));
            }
            reporter_->report(std::move(d));
        }
        panic_handler_decl_ = &f;
        // Signature check: 3 params, (Str, StrConst, Int).
        const bool params_ok =
            f.params.size() == 3 && f.params[0].type != nullptr
            && resolve_type(*f.params[0].type) == types_->primitive(TypeKind::Str)
            && f.params[1].type != nullptr
            && resolve_type(*f.params[1].type) == types_->primitive(TypeKind::StrConst)
            && f.params[2].type != nullptr
            && resolve_type(*f.params[2].type) == types_->primitive(TypeKind::Int);
        if (!params_ok) {
            error_at(a.range,
                     "@panic_handler signature must be `(message: Str, file: StrConst, line: "
                     "Int) -> Never`");
        }
        // Return type: must be Never. Never isn't spellable directly
        // in source but the resolver type-checks each `panic` /
        // `throw` body shape; here we approximate by requiring an
        // explicit `-> Never` annotation. v0.5: accept any result
        // type that resolves to Never; sema would otherwise need
        // body-level falls-off-the-end checking which is out of scope.
        if (f.result == nullptr || !resolve_type(*f.result)->is_never()) {
            error_at(a.range,
                     "@panic_handler return type must be `Never` (no falling off the end)");
        }
        if (!f.effects.using_caps.empty()) {
            error_at(a.range,
                     "@panic_handler cannot declare a `using` row — the handler runs in a "
                     "context where the program's capability bindings can't be assumed live");
        }
    }

    // §A8 (§14.5.2) `@interrupt` shape rules. Validated against the
    // declared signature regardless of whether the body is present,
    // since the rules are about the surface kernels write at ISRs:
    //   * first parameter must be `inout T` (the trap frame is what
    //     `eret`/`iretq` write back to);
    //   * return type must be Unit — ISRs can't fail or yield a value;
    //   * the using-row must not include `Alloc`, `Async`, or
    //     `Extern` — those don't make sense inside an interrupt
    //     handler (no heap, no awaiting, no FFI back to userland).
    // The attribute-argument shape (`class:` / `fromEL1:`) is left
    // free in v0.5; later phases will parse the labeled-arg dict.
    for (const auto& a : f.attributes) {
        if (a.name != "interrupt") {
            continue;
        }
        if (f.params.empty()
            || (f.params.size() >= 1 && f.params[0].mode != ast::ParamMode::Inout)) {
            error_at(a.range,
                     "@interrupt requires an `inout TrapFrame` first parameter — the prologue / "
                     "epilogue mutate it to write back the saved registers");
        }
        if (f.result != nullptr) {
            error_at(a.range,
                     "@interrupt functions return Unit (the body's effect is the mutation of "
                     "the trap-frame parameter; eret / iretq restores from there)");
        }
        for (const auto& cap : f.effects.using_caps) {
            if (cap == nullptr) {
                continue;
            }
            std::string name;
            if (cap->kind == ast::NodeKind::NamedType) {
                const auto& nt = static_cast<const ast::NamedType&>(*cap);
                if (!nt.path.empty()) {
                    name = nt.path.back();
                }
            }
            if (name == "Alloc" || name == "Async" || name == "Extern") {
                error_at(a.range,
                         std::format("@interrupt cannot declare `using {}` — interrupt handlers "
                                     "run with the kernel's own discipline (no heap, no awaits, "
                                     "no FFI back across the boundary)",
                                     name));
            }
        }
    }

    // §A9 (§14.7) `@boot` shape rules. Validated independently of
    // body presence — boot stubs declared as forward references stay
    // restricted regardless. v0.5 enforces three of the seven rules
    // here (the others are call-graph / audit-time):
    //   * `using` row restricted to {Asm, RawMemory, Mmio}. Other
    //     caps imply runtime services boot can't depend on.
    //   * No throws / Result. The boot regime has no unwinder.
    //   * @naked is implied — recorded on the attr, applied at
    //     codegen time as the C++ `[[gnu::naked]]` prefix.
    for (const auto& a : f.attributes) {
        if (a.name != "boot") {
            continue;
        }
        for (const auto& cap : f.effects.using_caps) {
            if (cap == nullptr || cap->kind != ast::NodeKind::NamedType) {
                continue;
            }
            const auto& nt = static_cast<const ast::NamedType&>(*cap);
            if (nt.path.empty()) {
                continue;
            }
            const auto& name = nt.path.back();
            if (name != "Asm" && name != "RawMemory" && name != "Mmio") {
                error_at(a.range,
                         std::format("@boot cannot declare `using {}` — the boot regime is "
                                     "limited to Asm / RawMemory / Mmio (pre-MMU code has no "
                                     "runtime services)",
                                     name));
            }
        }
        if (f.effects.throws_type != nullptr) {
            error_at(a.range,
                     "@boot cannot declare `throws(E)` — the boot regime has no unwinder, no "
                     "Result type-layer, and no panic path that could carry the error");
        }
    }

    // §A9 (§14.7.1) `@kernel_init` shape rules. v0.5 enforces:
    //   * No `using Async`. The scheduler hasn't dispatched yet —
    //     there's no other thread to synchronise with.
    // The other four rules (call-graph reachability, allocator
    // initialiser ordering, ordinary-calls-kernel_init rejection)
    // are audit-time and queue for the call-graph pass.
    bool is_kernel_init = false;
    for (const auto& a : f.attributes) {
        if (a.name != "kernel_init") {
            continue;
        }
        is_kernel_init = true;
        for (const auto& cap : f.effects.using_caps) {
            if (cap == nullptr || cap->kind != ast::NodeKind::NamedType) {
                continue;
            }
            const auto& nt = static_cast<const ast::NamedType&>(*cap);
            if (!nt.path.empty() && nt.path.back() == "Async") {
                error_at(a.range,
                         "@kernel_init cannot declare `using Async` — the scheduler hasn't "
                         "dispatched a second task yet, so there's no other thread to "
                         "synchronise with");
            }
        }
    }

    // §A6 last (§14.11.5 / §14.12.3) `@no_auto_barrier` — the
    // function-level escape that disables the compiler-inserted
    // device-barrier / sysreg-post-write barrier auto-emission. The
    // spec admits this only inside a function also attributed
    // `@kernel_init`, where the kernel writes a batch of MMU control
    // sysregs whose final write subsumes the per-write ISB. Anywhere
    // else (ordinary code, @boot, @interrupt) the attribute rejects
    // with a clear diagnostic. v0.5 doesn't yet auto-emit barriers
    // for sysreg writes (the Sysreg namespace itself is queued), so
    // the codegen effect is just a marker for the audit; the
    // sema-side gate is what records the contract today.
    for (const auto& a : f.attributes) {
        if (a.name == "no_auto_barrier" && !is_kernel_init) {
            error_at(a.range,
                     "@no_auto_barrier is admitted only on functions also attributed "
                     "@kernel_init — that's the boot-path subset where a following sysreg "
                     "write subsumes the auto-emitted barrier");
        }
    }

    if (!f.body) {
        return;  // protocol requirement / extern stub — nothing to check
    }

    ScopeStack::Guard g(scopes_);

    // Bind generic parameters as opaque types in the function's scope.
    for (const auto& gp : f.generics) {
        if (gp.name.empty() || gp.is_const) {
            continue;  // const generics live in the value namespace; v0.5 skips them
        }
        Symbol s;
        s.name = gp.name;
        s.kind = SymbolKind::GenericParam;
        s.type = types_->make_generic_param(gp.name);
        s.definition_range = gp.range;
        if (auto* prev = g.scope().insert(std::move(s))) {
            duplicate_definition(*prev, gp.name, gp.range);
        }
    }

    // Bind parameters.
    for (const auto& p : f.params) {
        TypePtr param_type = p.type ? resolve_type(*p.type) : types_->error();
        // §6 tuple-pattern param: `func f((a, b): (Int32, Int32))` —
        // there's no single name to insert; instead each leaf of the
        // pattern binds to its corresponding tuple element.
        if (p.pattern != nullptr) {
            if (p.pattern->kind == ast::NodeKind::TuplePat) {
                bind_tuple_pattern(static_cast<const ast::TuplePat&>(*p.pattern), param_type);
            }
            continue;
        }
        if (p.name.empty()) {
            continue;
        }
        Symbol s;
        s.name = p.name;
        s.kind = SymbolKind::Parameter;
        s.type = param_type;
        s.definition_range = p.range;
        if (auto* prev = g.scope().insert(std::move(s))) {
            duplicate_definition(*prev, p.name, p.range);
        }
    }

    TypePtr expected_result = f.result ? resolve_type(*f.result) : types_->unit();
    return_stack_.push_back(expected_result);
    // §9: separate stack tracks the enclosing throws(E). Push for every
    // function body so the depth matches return_stack_; nullptr means
    // "non-throwing", which is what check_throw / check_try look for.
    throws_stack_.push_back(f.effects.throws_type ? resolve_type(*f.effects.throws_type) : nullptr);
    // §9 bare-catch inference is lexically scoped to the current
    // function — a do-catch nested inside this function must not
    // capture a do-catch from the enclosing scope.
    auto saved_infer = std::move(do_catch_infer_stack_);
    do_catch_infer_stack_.clear();

    // The body is always a BlockExpr; pass the expected result as the context.
    TypePtr body_type = types_->unit();
    if (f.body->kind == ast::NodeKind::BlockExpr) {
        body_type = check_block_expr(static_cast<const ast::BlockExpr&>(*f.body), expected_result);
    }

    // A void-returning function may have a non-Unit body — the final value is
    // dropped. A value-returning function must end with an expression of the
    // right type, OR the value must come back through a `return`. We can't
    // distinguish those cleanly without flow analysis, so we only flag the
    // case where the trailing expression is *present* and clearly wrong.
    if (expected_result != nullptr && expected_result->kind() != TypeKind::Unit
        && body_type != nullptr && !TypeArena::assignable(body_type, expected_result)
        && !body_type->is_never()) {
        // We tolerate the mismatch when the body is empty — the function
        // presumably relies on an explicit return we haven't flow-checked.
        if (body_type->kind() != TypeKind::Unit) {
            error_at(f.body->range,
                     std::format("function '{}' returns {} but body produces {}",
                                 f.name,
                                 expected_result->describe(),
                                 body_type->describe()));
        }
    }

    return_stack_.pop_back();
    throws_stack_.pop_back();
    do_catch_infer_stack_ = std::move(saved_infer);
}

TypePtr Resolver::check_block_expr(const ast::BlockExpr& b, TypePtr expected) {
    ScopeStack::Guard g(scopes_);
    TypePtr last = types_->unit();
    for (std::size_t i = 0; i < b.stmts.size(); ++i) {
        const auto& s = *b.stmts[i];
        if (s.kind == ast::NodeKind::ExprStmt && i + 1 == b.stmts.size()) {
            // Trailing expression: its type is the block's value type, and
            // it inherits the surrounding block's expected type.
            last = check_expr(*static_cast<const ast::ExprStmt&>(s).expr, expected);
        } else {
            check_stmt(s);
        }
    }
    return last;
}

void Resolver::check_stmt(const ast::Stmt& s) {
    switch (s.kind) {
    case ast::NodeKind::LetStmt:
    case ast::NodeKind::VarStmt: {
        std::string_view binding_name;
        diag::SourceRange binding_range = s.range;
        const ast::Type* annotation = nullptr;
        const ast::Expr* initializer = nullptr;

        if (s.kind == ast::NodeKind::LetStmt) {
            const auto& l = static_cast<const ast::LetStmt&>(s);
            if (l.pattern && l.pattern->kind == ast::NodeKind::IdentPat) {
                binding_name = static_cast<const ast::IdentPat&>(*l.pattern).name;
                binding_range = l.pattern->range;
            } else if (l.pattern && l.pattern->kind == ast::NodeKind::BindPat) {
                binding_name = static_cast<const ast::BindPat&>(*l.pattern).name;
                binding_range = l.pattern->range;
            }
            annotation = l.type.get();
            initializer = l.value.get();
        } else {
            const auto& v = static_cast<const ast::VarStmt&>(s);
            if (v.pattern && v.pattern->kind == ast::NodeKind::IdentPat) {
                binding_name = static_cast<const ast::IdentPat&>(*v.pattern).name;
                binding_range = v.pattern->range;
            } else if (v.pattern && v.pattern->kind == ast::NodeKind::BindPat) {
                binding_name = static_cast<const ast::BindPat&>(*v.pattern).name;
                binding_range = v.pattern->range;
            }
            annotation = v.type.get();
            initializer = v.value.get();
        }

        TypePtr annot_type = annotation ? resolve_type(*annotation) : nullptr;
        TypePtr value_type = initializer ? check_expr(*initializer, annot_type) : nullptr;

        if (annot_type != nullptr && value_type != nullptr
            && !TypeArena::assignable(value_type, annot_type)) {
            error_at(initializer->range,
                     std::format("initializer of type {} does not match annotation {}",
                                 value_type->describe(),
                                 annot_type->describe()));
        }

        if (!binding_name.empty()) {
            Symbol sym;
            sym.name = std::string{binding_name};
            sym.kind = SymbolKind::Local;
            sym.type = annot_type != nullptr ? annot_type : value_type;
            sym.definition_range = binding_range;
            if (auto* prev = scopes_.current().insert(std::move(sym))) {
                duplicate_definition(*prev, binding_name, binding_range);
            }
        }
        // §6 tuple destructuring: `let (a, b) = pair` binds each
        // element of the tuple pattern to the corresponding tuple
        // element's type. Arity / shape mismatches are reported.
        const ast::Pattern* pat = nullptr;
        if (s.kind == ast::NodeKind::LetStmt) {
            pat = static_cast<const ast::LetStmt&>(s).pattern.get();
        } else {
            pat = static_cast<const ast::VarStmt&>(s).pattern.get();
        }
        if (pat != nullptr && pat->kind == ast::NodeKind::TuplePat) {
            TypePtr bind_t = annot_type != nullptr ? annot_type : value_type;
            bind_tuple_pattern(static_cast<const ast::TuplePat&>(*pat), bind_t);
        }
        break;
    }
    case ast::NodeKind::ReturnStmt: {
        const auto& r = static_cast<const ast::ReturnStmt&>(s);
        TypePtr expected = return_stack_.empty() ? nullptr : return_stack_.back();
        TypePtr value_type = r.value ? check_expr(*r.value, expected) : types_->unit();
        if (expected != nullptr && !TypeArena::assignable(value_type, expected)) {
            error_at(s.range,
                     std::format("return value of type {} does not match function result type {}",
                                 value_type ? value_type->describe() : "?",
                                 expected->describe()));
        }
        break;
    }
    case ast::NodeKind::ExprStmt:
        (void)check_expr(*static_cast<const ast::ExprStmt&>(s).expr);
        break;
    case ast::NodeKind::AssignStmt: {
        const auto& a = static_cast<const ast::AssignStmt&>(s);
        auto lhs = check_expr(*a.target);
        // Pass the target type as the expected type for the rhs so an
        // int literal (`i + 1`) adapts to the slot width (Int32 vs Int)
        // — same propagation as `let x: Int32 = i + 1` already does.
        auto rhs = check_expr(*a.value, lhs);
        if (lhs != nullptr && rhs != nullptr && !TypeArena::assignable(rhs, lhs)) {
            error_at(a.value->range,
                     std::format("cannot assign value of type {} to target of type {}",
                                 rhs->describe(),
                                 lhs->describe()));
        }
        break;
    }
    case ast::NodeKind::WhileStmt: {
        const auto& w = static_cast<const ast::WhileStmt&>(s);
        auto cond_type = check_expr(*w.cond);
        if (cond_type != nullptr && cond_type->kind() != TypeKind::Bool && !cond_type->is_error()) {
            error_at(w.cond->range,
                     std::format("while condition must be Bool, got {}", cond_type->describe()));
        }
        // §A4 (§14.9.3) admitted retry-loop detection. The two
        // while-shaped variants the spec lists are:
        //   * `while !r.succeeded { … }` — UnaryExpr(Not, MemberExpr("succeeded"))
        //   * `while true { … }`         — BoolLit(true)
        // Either shape opens a window where compareExchangeWeak is
        // admitted; the call-site check below consults this depth.
        bool admits_weak_cas = false;
        if (w.cond) {
            if (w.cond->kind == ast::NodeKind::BoolLit
                && static_cast<const ast::BoolLit&>(*w.cond).value) {
                admits_weak_cas = true;
            } else if (w.cond->kind == ast::NodeKind::UnaryExpr) {
                const auto& u = static_cast<const ast::UnaryExpr&>(*w.cond);
                if (u.op == ast::UnaryOp::Not && u.operand != nullptr
                    && u.operand->kind == ast::NodeKind::MemberExpr) {
                    if (static_cast<const ast::MemberExpr&>(*u.operand).member == "succeeded") {
                        admits_weak_cas = true;
                    }
                }
            }
        }
        if (admits_weak_cas) {
            ++weak_cas_loop_depth_;
        }
        if (w.body) {
            check_expr(*w.body);
        }
        if (admits_weak_cas) {
            --weak_cas_loop_depth_;
        }
        break;
    }
    case ast::NodeKind::ForStmt: {
        // §5 `for x in xs { ... }` — two shapes:
        //   * Range / RangeLt: `for x in 0..10` / `for x in 0..<10` —
        //     loop variable inherits the lhs/rhs numeric type.
        //   * Iterator protocol: `xs` has a `next() -> T?` method;
        //     loop variable is T.
        const auto& f = static_cast<const ast::ForStmt&>(s);
        auto iter_type = check_expr(*f.iter);
        TypePtr elem_type = nullptr;
        if (f.iter->kind == ast::NodeKind::BinaryExpr) {
            const auto& b = static_cast<const ast::BinaryExpr&>(*f.iter);
            if (b.op == ast::BinaryOp::Range || b.op == ast::BinaryOp::RangeLt) {
                // The Range result type is currently the operand type
                // itself (no nominal Range[T] yet); use it directly.
                elem_type = iter_type;
            }
        }
        if (elem_type == nullptr && iter_type != nullptr && !iter_type->is_error()) {
            // Iterator protocol fallback: look for a `next()` method
            // whose result is Optional<T>; the loop variable type is T.
            if (auto next_type = lookup_method(iter_type, "next")) {
                if (next_type->kind() == TypeKind::Function && next_type->result() != nullptr
                    && next_type->result()->kind() == TypeKind::Optional) {
                    elem_type = next_type->result()->inner();
                }
            }
            if (elem_type == nullptr) {
                error_at(f.iter->range,
                         std::format("'for x in xs' requires xs to be a Range or have a "
                                     "'next() -> Element?' method, got {}",
                                     iter_type->describe()));
            }
        }
        ScopeStack::Guard g(scopes_);
        if (f.pattern && f.pattern->kind == ast::NodeKind::IdentPat) {
            Symbol sym;
            sym.name = static_cast<const ast::IdentPat&>(*f.pattern).name;
            sym.kind = SymbolKind::Local;
            sym.type = elem_type != nullptr ? elem_type : types_->error();
            sym.definition_range = f.pattern->range;
            (void)g.scope().insert(std::move(sym));
        } else if (f.pattern && f.pattern->kind == ast::NodeKind::TuplePat) {
            // §6 `for (a, b) in zip(xs, ys)` — destructure each
            // element of the iterator into a tuple pattern.
            bind_tuple_pattern(static_cast<const ast::TuplePat&>(*f.pattern), elem_type);
        }
        if (f.body) {
            check_expr(*f.body);
        }
        break;
    }
    case ast::NodeKind::BreakStmt:
    case ast::NodeKind::ContinueStmt:
        break;
    case ast::NodeKind::WithStmt: {
        // §17.4 `with` block. Three shapes per WithBinding (parser
        // disambiguates):
        //   * cap_type only        — `with Alloc { … }`
        //   * cap_type + value     — `with Alloc = myAllocator { … }`
        //   * name + value         — `with mmio = createRegion() { … }`
        // Push a single scope for all bindings; insert each named
        // binding into the scope at the value's type; then recurse
        // into the body.
        const auto& w = static_cast<const ast::WithStmt&>(s);
        ScopeStack::Guard g(scopes_);
        for (const auto& b : w.bindings) {
            // §17.4 type annotation on a name-binding (`with NAME: T
            // = EXPR`). When present, push T down as the value
            // check's expected type so an integer literal adopts the
            // annotated width, then assignability-check the result.
            TypePtr annotation = b.type_annotation ? resolve_type(*b.type_annotation) : nullptr;
            TypePtr value_t = nullptr;
            if (b.value) {
                value_t = check_expr(*b.value, annotation);
            }
            if (annotation != nullptr && value_t != nullptr
                && !TypeArena::assignable(value_t, annotation)) {
                error_at(b.value ? b.value->range : s.range,
                         std::format("with-binding value of type {} does not match "
                                     "annotation {}",
                                     value_t->describe(),
                                     annotation->describe()));
            }
            if (!b.name.empty()) {
                Symbol sym;
                sym.name = b.name;
                sym.kind = SymbolKind::Local;
                // Honor the annotation as the binding's declared
                // type when present; otherwise fall back to the
                // value's inferred type.
                sym.type = annotation != nullptr ? annotation
                                                 : (value_t != nullptr ? value_t : types_->error());
                sym.definition_range = {};
                if (auto* prev = g.scope().insert(std::move(sym))) {
                    duplicate_definition(*prev, b.name, {});
                }
            }
            // Cap-typed bindings don't introduce a Vestra-side
            // identifier — the capability checker handles their
            // discharge separately (including §A7's InterruptsOff
            // region rules — see capability.cpp).
        }
        if (w.body) {
            check_expr(*w.body);
        }
        break;
    }
    default:
        break;
    }
}

// ============================================================================
// Expressions
// ============================================================================

TypePtr Resolver::check_expr(const ast::Expr& e, TypePtr expected) {
    TypePtr t = nullptr;
    switch (e.kind) {
    case ast::NodeKind::IntLit:
        // Integer literals adopt the expected type when it is an integer; this
        // is the bidirectional bit that lets `let x: Int32 = 42` typecheck
        // without an explicit conversion. Without context, default to Int.
        // §9 ergonomic: peel one Optional layer so `let x: Int32? = 42`
        // also adopts Int32 instead of falling back to the Int default.
        // §A4 same trick for Atomic[T]: `static x: Atomic[UInt32] = 0`
        // adopts UInt32 for the literal so the initial-value type
        // matches the inner.
        {
            TypePtr int_hint = expected;
            if (int_hint != nullptr && int_hint->kind() == TypeKind::Optional) {
                int_hint = int_hint->inner();
            } else if (int_hint != nullptr && int_hint->kind() == TypeKind::Atomic) {
                int_hint = int_hint->inner();
            }
            t = (int_hint != nullptr && int_hint->is_integer()) ? int_hint
                                                                : types_->primitive(TypeKind::Int);
        }
        break;
    case ast::NodeKind::FloatLit: {
        TypePtr float_hint = expected;
        if (float_hint != nullptr && float_hint->kind() == TypeKind::Optional) {
            float_hint = float_hint->inner();
        }
        t = (float_hint != nullptr && float_hint->is_float())
                ? float_hint
                : types_->primitive(TypeKind::Float64);
        break;
    }
    case ast::NodeKind::BoolLit:
        t = types_->boolean();
        break;
    case ast::NodeKind::NilLit:
        // `nil` is the §9 absence literal. Its type is the expected
        // Optional<T> when that's known; otherwise we fall back to
        // Optional<Never>, which TypeArena::assignable treats as
        // compatible with every Optional<T> so e.g. `let x: Int32? = nil`
        // still typechecks even though `nil` was checked before the let-
        // initializer ran.
        t = (expected != nullptr && expected->kind() == TypeKind::Optional)
                ? expected
                : types_->make_optional(types_->never());
        break;
    case ast::NodeKind::StringLit:
        t = types_->primitive(TypeKind::StrConst);
        break;
    case ast::NodeKind::CharLit:
        t = types_->primitive(TypeKind::Char);
        break;

    case ast::NodeKind::IdentExpr: {
        const auto& ident = static_cast<const ast::IdentExpr&>(e);
        const auto* sym = scopes_.current().lookup(ident.name);
        if (sym == nullptr) {
            error_at(e.range, std::format("undefined name '{}'", ident.name));
            t = types_->error();
        } else {
            resolution_.set_symbol(&e, sym);
            check_visibility(*sym, e.range);
            t = sym->type != nullptr ? sym->type : types_->error();
        }
        break;
    }
    case ast::NodeKind::ParenExpr:
        t = check_expr(*static_cast<const ast::ParenExpr&>(e).inner, expected);
        break;
    case ast::NodeKind::TupleLitExpr: {
        // §6 `(e1, e2, …)` types as Tuple<typeof(e1), typeof(e2), …>.
        // When the surrounding context expects a TupleType of matching
        // arity, push each expected element down so a literal inside
        // the tuple adopts the expected primitive width.
        const auto& tl = static_cast<const ast::TupleLitExpr&>(e);
        std::vector<TypePtr> parts;
        parts.reserve(tl.elements.size());
        const auto* expected_parts = (expected != nullptr && expected->kind() == TypeKind::Tuple
                                      && expected->parts().size() == tl.elements.size())
                                         ? &expected->parts()
                                         : nullptr;
        for (std::size_t i = 0; i < tl.elements.size(); ++i) {
            TypePtr elem_hint = expected_parts != nullptr ? (*expected_parts)[i] : nullptr;
            parts.push_back(check_expr(*tl.elements[i], elem_hint));
        }
        t = types_->make_tuple(std::move(parts));
        break;
    }
    case ast::NodeKind::VectorLitExpr: {
        // §13 `[e1, e2, ...]` literal. Static decls with `[N]T`
        // annotations (including §A8's `[N]@interrupt(T)` vector
        // tables) push T down as the element-type hint so each
        // element type-checks against the slot type — catching
        // mismatched ISRs at the assignment site rather than deep
        // in C++. Without an annotation we fall back to inferring
        // the element type from the first element.
        const auto& vl = static_cast<const ast::VectorLitExpr&>(e);
        TypePtr elem_hint = (expected != nullptr && expected->kind() == TypeKind::Vector)
                                ? expected->inner()
                                : nullptr;
        TypePtr elem_type = elem_hint;
        for (std::size_t i = 0; i < vl.elements.size(); ++i) {
            if (vl.elements[i] == nullptr) {
                continue;
            }
            TypePtr et = check_expr(*vl.elements[i], elem_hint);
            if (elem_type == nullptr) {
                elem_type = et;
            }
            if (elem_hint != nullptr && !TypeArena::assignable(et, elem_hint)) {
                error_at(vl.elements[i]->range,
                         std::format("vector element {} of type {} does not match "
                                     "expected element type {}",
                                     i,
                                     et != nullptr ? et->describe() : "?",
                                     elem_hint->describe()));
            }
        }
        if (elem_type == nullptr) {
            elem_type = types_->error();
        }
        t = types_->make_vector(static_cast<std::int64_t>(vl.elements.size()), elem_type);
        break;
    }
    case ast::NodeKind::UnaryExpr:
        t = check_unary(static_cast<const ast::UnaryExpr&>(e), expected);
        break;
    case ast::NodeKind::BinaryExpr:
        t = check_binary(static_cast<const ast::BinaryExpr&>(e), expected);
        break;
    case ast::NodeKind::CallExpr:
        t = check_call(static_cast<const ast::CallExpr&>(e), expected);
        break;
    case ast::NodeKind::IfExpr:
        t = check_if(static_cast<const ast::IfExpr&>(e), expected);
        break;
    case ast::NodeKind::BlockExpr:
        t = check_block_expr(static_cast<const ast::BlockExpr&>(e), expected);
        break;
    case ast::NodeKind::ClosureExpr: {
        // §16 closure literal. The parser stores param names with no
        // type annotations — types come from a contextual expected
        // Function type supplied by the use site (e.g. .mapError, the
        // map/filter builtins, or a typed local). Without that
        // context the closure can't be typed in v0.5; we diagnose
        // here rather than fall through to Error so the user gets a
        // specific message.
        const auto& cx = static_cast<const ast::ClosureExpr&>(e);
        if (expected == nullptr || expected->kind() != TypeKind::Function
            || expected->parts().size() != cx.params.size()) {
            error_at(cx.range,
                     "closure type cannot be inferred without context; pass it directly to a "
                     "function-typed slot (e.g. .mapError, map, filter) or annotate the binding");
            // Still walk the body so any errors inside it surface,
            // but with each param at Error (so they're permissive).
            ScopeStack::Guard g(scopes_);
            for (const auto& name : cx.params) {
                Symbol sym;
                sym.name = name;
                sym.kind = SymbolKind::Local;
                sym.type = types_->error();
                (void)g.scope().insert(std::move(sym));
            }
            for (const auto& stmt : cx.body) {
                if (stmt) {
                    check_stmt(*stmt);
                }
            }
            t = types_->error();
            break;
        }
        ScopeStack::Guard g(scopes_);
        for (std::size_t i = 0; i < cx.params.size(); ++i) {
            Symbol sym;
            sym.name = cx.params[i];
            sym.kind = SymbolKind::Local;
            sym.type = expected->parts()[i] != nullptr ? expected->parts()[i] : types_->error();
            (void)g.scope().insert(std::move(sym));
        }
        // The body's trailing ExprStmt yields the closure result —
        // mirror check_block_expr's "last expression types the block"
        // shape so a single-expression closure `{ x => x * 2 }` types
        // cleanly without needing a `return`. Mid-body statements are
        // checked normally.
        TypePtr body_type = nullptr;
        for (std::size_t i = 0; i < cx.body.size(); ++i) {
            const auto& stmt = cx.body[i];
            if (!stmt) {
                continue;
            }
            const bool is_last = (i + 1 == cx.body.size());
            if (is_last && stmt->kind == ast::NodeKind::ExprStmt
                && static_cast<const ast::ExprStmt&>(*stmt).expr != nullptr) {
                body_type =
                    check_expr(*static_cast<const ast::ExprStmt&>(*stmt).expr, expected->result());
            } else {
                check_stmt(*stmt);
            }
        }
        if (body_type == nullptr) {
            body_type = types_->unit();
        }
        if (expected->result() != nullptr
            && !TypeArena::assignable(body_type, expected->result())) {
            error_at(cx.range,
                     std::format("closure body types as {} but the expected return type is {}",
                                 body_type->describe(),
                                 expected->result()->describe()));
        }
        t = types_->make_function(expected->parts(), body_type);
        break;
    }
    case ast::NodeKind::CopyExpr:
        t = check_expr(*static_cast<const ast::CopyExpr&>(e).inner);
        break;
    case ast::NodeKind::AddressOfExpr: {
        // §A12 (§14.6.3) `&decl` — address-of a static or function.
        // - Static: result is `Ptr[T]` over the static's value type
        //   (spec calls for `Ptr[T]?` since a weakly-linked extern can
        //   be null, but optional wrapping is deferred).
        // - Func: result is the function-pointer type matching the
        //   callee's signature `(T1, T2, …) -> R`. Vestra's spelling
        //   for function pointers is the same as for closures —
        //   `TypeKind::Function` — so a callee site that expects a
        //   function-pointer parameter accepts both `&some_func` and
        //   an inline closure (the C++ compiler enforces the capture-
        //   less requirement when one of these flows into a function-
        //   pointer slot).
        const auto& a = static_cast<const ast::AddressOfExpr&>(e);
        if (a.inner != nullptr && a.inner->kind == ast::NodeKind::IdentExpr) {
            const auto& id = static_cast<const ast::IdentExpr&>(*a.inner);
            if (const auto* sym = scopes_.current().lookup(id.name)) {
                resolution_.set_symbol(a.inner.get(), sym);
                if (sym->kind == SymbolKind::Static && sym->type != nullptr) {
                    if (!sym->type->is_error()) {
                        resolution_.set_type(a.inner.get(), sym->type);
                    }
                    t = types_->make_ptr(sym->type != nullptr ? sym->type : types_->error());
                    break;
                }
                if (sym->kind == SymbolKind::Func) {
                    // The Func symbol's type already holds the
                    // canonical `(params...) -> result` shape that
                    // collect_func populated via function_type_of(f).
                    if (sym->type != nullptr) {
                        resolution_.set_type(a.inner.get(), sym->type);
                        t = sym->type;
                    } else {
                        t = types_->error();
                    }
                    break;
                }
                error_at(a.inner->range,
                         std::format("&decl operand must be a static or func, got "
                                     "{} '{}'",
                                     sym->kind == SymbolKind::Local ? "local" : "name",
                                     id.name));
            } else {
                error_at(a.inner->range,
                         std::format("undefined name '{}' in &decl operand", id.name));
            }
            t = types_->error();
        } else if (a.inner != nullptr && a.inner->kind == ast::NodeKind::MemberExpr) {
            // §14.12.2 `&Sysreg.X` is rejected by spec: a sysreg is
            // an architectural cell accessed via `mrs` / `msr`, not a
            // memory cell with a linkage symbol. The generic
            // "must be a static or func identifier" diagnostic doesn't
            // make that distinction clear, so we name Sysreg
            // specifically here. Other MemberExpr operands (struct
            // field access, etc.) fall through to the generic message.
            const auto& mem = static_cast<const ast::MemberExpr&>(*a.inner);
            if (mem.base != nullptr && mem.base->kind == ast::NodeKind::IdentExpr
                && static_cast<const ast::IdentExpr&>(*mem.base).name == "Sysreg"
                && scopes_.current().lookup("Sysreg") == nullptr) {
                error_at(e.range,
                         std::format("&Sysreg.{} is rejected: sysregs are architectural "
                                     "cells, not memory cells with a linkage symbol "
                                     "(§14.12.2)",
                                     mem.member));
            } else {
                error_at(e.range, "&decl operand must be a static or func identifier");
            }
            t = types_->error();
        } else {
            error_at(e.range, "&decl operand must be a static or func identifier");
            t = types_->error();
        }
        break;
    }
    case ast::NodeKind::AwaitExpr: {
        // §11 `await e` consumes its operand for the underlying value: a
        // `Future[T]` (from `spawn`) unwraps to T; a direct async call is
        // already typed T (async results aren't wrapped), so await is
        // transparent there.
        auto inner = check_expr(*static_cast<const ast::AwaitExpr&>(e).inner);
        t = (inner != nullptr && inner->kind() == TypeKind::Future) ? inner->inner() : inner;
        break;
    }
    case ast::NodeKind::SpawnExpr:
        // §11 `spawn f(args)` runs the call and yields a `Future[T]` where
        // T is the call's result type.
        t = types_->make_future(check_expr(*static_cast<const ast::SpawnExpr&>(e).inner));
        break;
    case ast::NodeKind::ThrowExpr: {
        // §9: `throw e` is only valid inside a throws(E) function; e must
        // be assignable to E. The expression's static type is Never.
        const auto& th = static_cast<const ast::ThrowExpr&>(e);
        TypePtr enclosing_err = throws_stack_.empty() ? nullptr : throws_stack_.back();
        auto inner_type = check_expr(*th.inner, enclosing_err);
        if (enclosing_err == nullptr) {
            error_at(e.range, "'throw' is only valid inside a function declared 'throws(E)'");
        } else if (inner_type != nullptr && !TypeArena::assignable(inner_type, enclosing_err)) {
            error_at(th.inner->range,
                     std::format("'throw' value of type {} does not match the enclosing throws({})",
                                 inner_type->describe(),
                                 enclosing_err->describe()));
        }
        t = types_->never();
        break;
    }
    case ast::NodeKind::TryExpr: {
        // §9: `try e` / `try? e` / `try! e` all require `e` to evaluate to
        // a Result<T, E> (i.e. a call to a throws function). The three
        // forms then differ in how they consume the error:
        //   * Propagating: returns the error from the enclosing function
        //     (which must itself be throws-compatible with E); yields T.
        //   * Optional:    converts to Optional<T>; yields Optional<T>.
        //   * Forced:      panics on error; yields T.
        const auto& tx = static_cast<const ast::TryExpr&>(e);
        auto inner_type = check_expr(*tx.inner);
        if (inner_type == nullptr || inner_type->is_error()) {
            t = types_->error();
            break;
        }
        if (inner_type->kind() != TypeKind::Result) {
            // §9 form-specific diagnostic. The most common confusion is
            // applying a try-prefix to an Optional-returning call —
            // each form has a different "right idiom" to suggest, and
            // a vague "needs Result<T, E>" message just sends the user
            // hunting. When the operand is Optional<T>, point them at
            // the matching non-try construct directly.
            const bool inner_opt = inner_type->kind() == TypeKind::Optional;
            std::string_view form_text = "try";
            switch (tx.form) {
            case ast::TryExpr::Form::Propagating:
                form_text = "try";
                break;
            case ast::TryExpr::Form::Optional:
                form_text = "try?";
                break;
            case ast::TryExpr::Form::Forced:
                form_text = "try!";
                break;
            }
            if (inner_opt) {
                std::string_view hint;
                switch (tx.form) {
                case ast::TryExpr::Form::Optional:
                    hint = "this operand already produces an Optional — drop `try?`, or use "
                           "`?.` / `if let` / `match` to consume it";
                    break;
                case ast::TryExpr::Form::Propagating:
                    hint = "Optional doesn't carry an error to propagate — use `?.` for "
                           "chaining, `if let` / `match` to branch, or `!` for an asserted "
                           "unwrap";
                    break;
                case ast::TryExpr::Form::Forced:
                    hint = "use postfix `!` for force-unwrap of an Optional; `try!` is for "
                           "Result<T, E>";
                    break;
                }
                error_at(tx.inner->range,
                         std::format("'{}' requires a Result<T, E> operand, got {}: {}",
                                     form_text,
                                     inner_type->describe(),
                                     hint));
            } else {
                error_at(tx.inner->range,
                         std::format("'{}' operand must produce a Result<T, E>, got {}",
                                     form_text,
                                     inner_type->describe()));
            }
            t = types_->error();
            break;
        }
        TypePtr ok = inner_type->inner();
        TypePtr err = inner_type->result();
        if (tx.form == ast::TryExpr::Form::Propagating) {
            // §9: a Propagating try inside a bare `do { ... } catch
            // NAME { ... }` writes its error type into the do-catch's
            // inference slot. The slot takes precedence over the
            // enclosing function's throws_stack_ — the do-catch is
            // the closer error handler.
            if (!do_catch_infer_stack_.empty()) {
                TypePtr* slot = do_catch_infer_stack_.back();
                if (*slot == nullptr) {
                    *slot = err;
                } else if (!TypeArena::equal(err, *slot)) {
                    error_at(e.range,
                             std::format("do-body tries with mismatched error types: {} vs {}; "
                                         "use 'catch (NAME: E)' to pin the type explicitly",
                                         (*slot)->describe(),
                                         err ? err->describe() : "?"));
                }
            } else {
                TypePtr enclosing_err = throws_stack_.empty() ? nullptr : throws_stack_.back();
                if (enclosing_err == nullptr) {
                    error_at(
                        e.range,
                        "'try' propagation requires the enclosing function to declare 'throws(E)'");
                } else if (!TypeArena::assignable(err, enclosing_err)) {
                    error_at(
                        e.range,
                        std::format(
                            "propagated error of type {} does not match the enclosing throws({})",
                            err ? err->describe() : "?",
                            enclosing_err->describe()));
                }
            }
            t = ok;
        } else if (tx.form == ast::TryExpr::Form::Optional) {
            t = types_->make_optional(ok);
        } else {
            // Forced: `try!` panics on error.
            t = ok;
        }
        break;
    }
    case ast::NodeKind::DoCatchExpr: {
        // §9 `do { body } catch ... { handler }` — two binding shapes:
        //   * Annotated: `catch (NAME: E)` — E is known up front; we
        //     push it onto throws_stack_ and the body's tries check
        //     against it normally.
        //   * Bare: `catch NAME` — E is *inferred* from the body's
        //     try-calls. We push a fresh inference slot onto
        //     do_catch_infer_stack_; check_try writes the first
        //     try's error type there and checks subsequent ones for
        //     match. Throws_stack_ stays untouched in this mode
        //     since check_try prefers the infer slot when present.
        const auto& dc = static_cast<const ast::DoCatchExpr&>(e);
        TypePtr error_type = dc.error_type ? resolve_type(*dc.error_type) : nullptr;
        TypePtr inferred = nullptr;
        const bool inferring = (dc.error_type == nullptr);
        if (inferring) {
            do_catch_infer_stack_.push_back(&inferred);
        } else {
            throws_stack_.push_back(error_type);
        }
        TypePtr body_type = check_expr(*dc.do_body, expected);
        if (inferring) {
            do_catch_infer_stack_.pop_back();
            if (inferred == nullptr) {
                error_at(dc.range,
                         "bare 'catch NAME' requires the do-body to contain at least one 'try'; "
                         "either add a try-call or use 'catch (NAME: E)' to give the error type "
                         "explicitly");
                inferred = types_->error();
            }
            error_type = inferred;
            resolution_.set_do_catch_error_type(&dc, error_type);
        } else {
            throws_stack_.pop_back();
        }

        ScopeStack::Guard g(scopes_);
        Symbol sym;
        sym.name = dc.error_name;
        sym.kind = SymbolKind::Local;
        sym.type = error_type;
        sym.definition_range = dc.range;
        (void)scopes_.current().insert(std::move(sym));
        // §9 `where guard`: type-check under the binding's scope; the
        // guard must produce Bool. A failed guard at runtime
        // propagates the bound error to the enclosing throws context,
        // so we also require that context exists and matches the
        // do-catch's error type. (throws_stack_'s top here is the
        // enclosing function's throws_type — the do-catch's own push
        // / pop already happened above.)
        if (dc.guard) {
            auto gt = check_expr(*dc.guard, types_->boolean());
            if (gt != nullptr && !gt->is_error() && gt->kind() != TypeKind::Bool) {
                error_at(dc.guard->range,
                         std::format("do/catch where-guard must be Bool, got {}", gt->describe()));
            }
            TypePtr enclosing = throws_stack_.empty() ? nullptr : throws_stack_.back();
            if (enclosing == nullptr) {
                error_at(dc.guard->range,
                         "do/catch where-guard requires the enclosing function to be "
                         "`throws(E)` so the fall-through can propagate the error");
            } else if (error_type != nullptr && !error_type->is_error()
                       && !TypeArena::assignable(error_type, enclosing)) {
                error_at(dc.guard->range,
                         std::format("do/catch where-guard fall-through propagates {}, but "
                                     "the enclosing throws context is {}; either widen with "
                                     "`.mapError(...)` or remove the guard",
                                     error_type->describe(),
                                     enclosing->describe()));
            }
        }
        TypePtr catch_type = check_expr(*dc.catch_body, expected != nullptr ? expected : body_type);

        if (body_type == nullptr || catch_type == nullptr || body_type->is_error()
            || catch_type->is_error()) {
            t = types_->error();
            break;
        }
        if (body_type->is_never()) {
            t = catch_type;
            break;
        }
        if (catch_type->is_never()) {
            t = body_type;
            break;
        }
        if (!TypeArena::equal(body_type, catch_type)) {
            error_at(e.range,
                     std::format("do/catch arms have different types: {} vs {}",
                                 body_type->describe(),
                                 catch_type->describe()));
            t = types_->error();
            break;
        }
        t = body_type;
        break;
    }
    case ast::NodeKind::AsExpr: {
        const auto& a = static_cast<const ast::AsExpr&>(e);
        (void)check_expr(*a.value);
        t = a.target ? resolve_type(*a.target) : types_->error();
        if (a.is_optional && t != nullptr && !t->is_error()) {
            t = types_->make_optional(t);
        }
        break;
    }
    case ast::NodeKind::MemberExpr:
        t = check_member(static_cast<const ast::MemberExpr&>(e), expected);
        break;
    case ast::NodeKind::LeadingDotExpr:
        t = check_leading_dot(static_cast<const ast::LeadingDotExpr&>(e), expected);
        break;
    case ast::NodeKind::MatchExpr:
        t = check_match(static_cast<const ast::MatchExpr&>(e), expected);
        break;
    case ast::NodeKind::InterpStringExpr: {
        // §4: a `"…\(expr)…"` literal is a freshly allocated String.
        // Every splice must be Display-conformant — primitive numerics
        // / Bool / Char / string-likes pass implicitly; user types need
        // either `derive(Display)` or `derive(Debug)`. The `using Alloc`
        // capability requirement (§4) is still a follow-on; the runtime
        // lowering uses std::format which assumes a default heap.
        const auto& is_ = static_cast<const ast::InterpStringExpr&>(e);
        for (const auto& seg : is_.segments) {
            if (seg.expr == nullptr) {
                continue;
            }
            auto seg_t = check_expr(*seg.expr);
            if (seg_t == nullptr || seg_t->is_error()) {
                continue;
            }
            if (!is_display_conformant(seg_t)) {
                error_at(seg.expr->range,
                         std::format("interpolation splice of type {} is not Display-conformant; "
                                     "add `derive(Display) for {}` (or `derive(Debug)`) to make it "
                                     "printable",
                                     seg_t->describe(),
                                     seg_t->describe()));
            }
        }
        t = types_->primitive(TypeKind::String);
        break;
    }
    case ast::NodeKind::IndexExpr: {
        // `base[i0, ...]`. v0.5 admits indexing on:
        //   * `[N]T`         → T
        //   * `Span[T]` / `MutSpan[T]` → T
        // Indices must be integer-typed. Other base shapes (struct,
        // pointer, etc.) are not yet wired; sema falls through to the
        // error type so downstream consumers don't cascade.
        const auto& ix = static_cast<const ast::IndexExpr&>(e);
        TypePtr base_t = ix.base ? check_expr(*ix.base) : nullptr;
        for (const auto& idx : ix.indices) {
            if (idx) {
                auto it = check_expr(*idx);
                if (it != nullptr && !it->is_error() && !it->is_integer()) {
                    error_at(idx->range,
                             std::format("index must be integer-typed, got {}", it->describe()));
                }
            }
        }
        if (base_t == nullptr || base_t->is_error()) {
            t = types_->error();
            break;
        }
        switch (base_t->kind()) {
        case TypeKind::Vector:
        case TypeKind::Span:
        case TypeKind::MutSpan:
            t = base_t->inner();
            break;
        default:
            error_at(e.range, std::format("type {} is not indexable", base_t->describe()));
            t = types_->error();
            break;
        }
        break;
    }
    case ast::NodeKind::EmbedExpr: {
        // §12.1 `@embed("path")` types as `[N]UInt8` where N is the file's
        // size at compile time. We ask the folder to read the file now;
        // the byte-count from the folded vector pins the type's length,
        // and stashing the folded value here also primes the side table
        // so codegen emits the brace-init literal without re-reading.
        const auto& ee = static_cast<const ast::EmbedExpr&>(e);
        auto folded = folder_.fold(ee, comptime_env_);
        if (!folded || folded->kind != ComptimeValue::Kind::Vector) {
            error_at(ee.range,
                     std::format("could not @embed file \"{}\" (path/manifest miss or no reader "
                                 "configured)",
                                 ee.path));
            t = types_->error();
            break;
        }
        resolution_.set_folded_value(&ee, *folded);
        t = types_->make_vector(folded->length, types_->primitive(TypeKind::UInt8));
        break;
    }

    default:
        // Unimplemented: walk children to surface their errors, then assign
        // Error so downstream consumers don't cascade.
        t = types_->error();
        break;
    }
    if (t != nullptr) {
        resolution_.set_type(&e, t);
    }
    return t;
}

TypePtr Resolver::check_unary(const ast::UnaryExpr& u, TypePtr expected) {
    // Negation / bit-not preserve the operand's type, so the expected type
    // can be propagated inward.
    auto operand = check_expr(*u.operand, expected);
    if (operand == nullptr || operand->is_error()) {
        return types_->error();
    }
    switch (u.op) {
    case ast::UnaryOp::Neg:
        if (!operand->is_numeric()) {
            error_at(u.range,
                     std::format("unary '-' requires a numeric type, got {}", operand->describe()));
            return types_->error();
        }
        return operand;
    case ast::UnaryOp::Not:
        if (operand->kind() != TypeKind::Bool) {
            error_at(u.range, std::format("unary '!' requires Bool, got {}", operand->describe()));
            return types_->error();
        }
        return operand;
    case ast::UnaryOp::BitNot:
        if (!operand->is_integer()) {
            error_at(
                u.range,
                std::format("unary '~' requires an integer type, got {}", operand->describe()));
            return types_->error();
        }
        return operand;
    case ast::UnaryOp::Unwrap:
        // §9 postfix `!` panics on `.none` and yields the wrapped T.
        if (operand->kind() != TypeKind::Optional) {
            error_at(u.range,
                     std::format("postfix '!' requires an Optional, got {}", operand->describe()));
            return types_->error();
        }
        return operand->inner();
    }
    return types_->error();
}

TypePtr Resolver::check_binary(const ast::BinaryExpr& b, TypePtr expected) {
    // §9 `lhs ?? rhs` — the result type is T, so the surrounding `expected`
    // (when present) is T. Propagate T as the lhs's expected-Optional<T>
    // and as the rhs's expected-T so literal defaulting picks the right
    // primitive on both sides.
    if (b.op == ast::BinaryOp::Coalesce) {
        TypePtr lhs_expected = expected != nullptr ? types_->make_optional(expected) : nullptr;
        auto lhs = check_expr(*b.lhs, lhs_expected);
        TypePtr rhs_expected =
            (lhs != nullptr && lhs->kind() == TypeKind::Optional) ? lhs->inner() : expected;
        auto rhs = check_expr(*b.rhs, rhs_expected);
        if (lhs == nullptr || rhs == nullptr || lhs->is_error() || rhs->is_error()) {
            return types_->error();
        }
        if (lhs->kind() != TypeKind::Optional) {
            error_at(
                b.lhs->range,
                std::format("'?\\?' requires an Optional left operand, got {}", lhs->describe()));
            return types_->error();
        }
        if (!TypeArena::assignable(rhs, lhs->inner())) {
            error_at(b.rhs->range,
                     std::format("'?\\?' right operand of type {} does not match Optional<{}>",
                                 rhs->describe(),
                                 lhs->inner() ? lhs->inner()->describe() : "?"));
            return types_->error();
        }
        return lhs->inner();
    }

    // For arithmetic/bitwise/comparison, both operands ought to share a
    // single numeric type. Propagate `expected` to the lhs first; then use
    // the lhs's concrete type as the expected type for the rhs (so a literal
    // on either side adapts).
    auto lhs = check_expr(*b.lhs, expected);
    auto rhs = check_expr(*b.rhs, lhs);
    if (lhs == nullptr || rhs == nullptr) {
        return types_->error();
    }
    if (lhs->is_error() || rhs->is_error()) {
        return types_->error();
    }

    // For arithmetic / bitwise / comparison, the operands must share a
    // numeric type. The §17 natural-width rule (see TypeArena::assignable)
    // applies symmetrically — `Int + Int32` yields Int32 (the sized side
    // wins). Two sized widths that disagree (`Int8 + Int32`) are still an
    // error; only the natural-width side flexes.
    auto require_match = [&] {
        if (TypeArena::equal(lhs, rhs)) {
            return true;
        }
        if (lhs->is_integer() && rhs->is_integer()
            && (TypeArena::assignable(lhs, rhs) || TypeArena::assignable(rhs, lhs))) {
            // Pick the sized operand as the result type. If both are
            // natural-width (Int + Int), assignable returns true both
            // ways and the result stays Int.
            const bool lhs_natural = lhs->kind() == TypeKind::Int || lhs->kind() == TypeKind::UInt;
            if (lhs_natural) {
                lhs = rhs;
            }
            return true;
        }
        error_at(b.range,
                 std::format("binary operator operands of different types: {} vs {}",
                             lhs->describe(),
                             rhs->describe()));
        return false;
    };

    switch (b.op) {
    case ast::BinaryOp::Add:
    case ast::BinaryOp::Sub:
    case ast::BinaryOp::Mul:
    case ast::BinaryOp::Div:
    case ast::BinaryOp::Mod:
    case ast::BinaryOp::WrappingAdd:
    case ast::BinaryOp::WrappingSub:
    case ast::BinaryOp::WrappingMul:
        if (!lhs->is_numeric()) {
            error_at(b.lhs->range,
                     std::format("arithmetic operator requires numeric operands, got {}",
                                 lhs->describe()));
            return types_->error();
        }
        if (!require_match()) {
            return types_->error();
        }
        return lhs;

    case ast::BinaryOp::Shl:
    case ast::BinaryOp::Shr:
    case ast::BinaryOp::BitAnd:
    case ast::BinaryOp::BitOr:
    case ast::BinaryOp::BitXor:
        if (!lhs->is_integer()) {
            error_at(
                b.lhs->range,
                std::format("bitwise operator requires integer operands, got {}", lhs->describe()));
            return types_->error();
        }
        if (!require_match()) {
            return types_->error();
        }
        return lhs;

    case ast::BinaryOp::Eq:
    case ast::BinaryOp::Ne:
    case ast::BinaryOp::Lt:
    case ast::BinaryOp::Le:
    case ast::BinaryOp::Gt:
    case ast::BinaryOp::Ge:
        if (!require_match()) {
            return types_->error();
        }
        return types_->boolean();

    case ast::BinaryOp::And:
    case ast::BinaryOp::Or:
        if (lhs->kind() != TypeKind::Bool || rhs->kind() != TypeKind::Bool) {
            error_at(b.range, "logical && / || require Bool operands");
            return types_->error();
        }
        return types_->boolean();

    case ast::BinaryOp::Coalesce:
        // `lhs ?? rhs` — lhs should be Optional<T>, rhs should be T.
        if (lhs->kind() != TypeKind::Optional) {
            error_at(
                b.lhs->range,
                std::format("'?\\?' requires an Optional left operand, got {}", lhs->describe()));
            return types_->error();
        }
        if (!TypeArena::assignable(rhs, lhs->inner())) {
            error_at(b.rhs->range,
                     std::format("'?\\?' right operand of type {} does not match Optional<{}>",
                                 rhs->describe(),
                                 lhs->inner() ? lhs->inner()->describe() : "?"));
            return types_->error();
        }
        return lhs->inner();

    case ast::BinaryOp::Range:
    case ast::BinaryOp::RangeLt:
        if (!require_match()) {
            return types_->error();
        }
        // TODO: a real Range[T] nominal type. For now we just pass through.
        return lhs;
    }
    return types_->error();
}

TypePtr Resolver::check_call(const ast::CallExpr& c, TypePtr expected) {
    // §17.x conversion-call syntax: `Float64(i)` / `Int32(x)` — a bare
    // primitive numeric type name in callee position is an explicit
    // conversion. We handle it before normal callee resolution so the
    // type name doesn't trip "undefined name 'Float64'" against the
    // value scope (where it intentionally isn't registered — primitives
    // are types, not symbols). Requires exactly one positional numeric
    // argument; the result type is the target primitive.
    if (c.callee->kind == ast::NodeKind::IdentExpr) {
        const auto& callee_ident = static_cast<const ast::IdentExpr&>(*c.callee);
        auto kind = TypeArena::primitive_kind_by_name(callee_ident.name);
        if (kind != TypeKind::Error) {
            auto target = types_->primitive(kind);
            if (target != nullptr && target->is_numeric()) {
                if (c.args.size() != 1) {
                    for (const auto& a : c.args) {
                        (void)check_expr(*a.value);
                    }
                    error_at(c.range,
                             std::format("conversion to {} takes exactly one argument",
                                         target->describe()));
                    return target;
                }
                if (!c.args[0].label.empty()) {
                    error_at(c.args[0].value->range,
                             "conversion call argument cannot have a label");
                }
                auto arg_type = check_expr(*c.args[0].value);
                // §3 inverse opaque conversion: `T(q)` where q is an
                // opaque newtype admits the call iff the underlying
                // is numeric (the C++ side does `static_cast<T>(q)`
                // through `enum class Q : T {}`). Symmetric with the
                // `Q(t)` forward conversion below.
                bool ok = false;
                if (arg_type == nullptr || arg_type->is_error()) {
                    ok = true;
                } else if (arg_type->is_numeric()) {
                    ok = true;
                } else if (arg_type->kind() == TypeKind::OpaqueType
                           && arg_type->nominal_decl() != nullptr) {
                    const auto& od = static_cast<const ast::OpaqueDecl&>(*arg_type->nominal_decl());
                    if (od.underlying != nullptr) {
                        auto under = resolve_type(*od.underlying);
                        if (under != nullptr && under->is_numeric()) {
                            ok = true;
                        }
                    }
                }
                if (!ok) {
                    error_at(c.args[0].value->range,
                             std::format("cannot convert non-numeric {} to {}",
                                         arg_type->describe(),
                                         target->describe()));
                }
                return target;
            }
        }
    }

    // §9 `result.mapError(f)` — explicit error-type widening between
    // throws functions. `result` must be Result<T, E>; `f` must be
    // a callable `(E) -> E'`. The call types as Result<T, E'>; the
    // codegen lowers it to std::expected's `.transform_error(f)`.
    // Recognized as a MemberExpr callee shape (parallel to Box.new).
    if (c.callee->kind == ast::NodeKind::MemberExpr) {
        const auto& mem = static_cast<const ast::MemberExpr&>(*c.callee);
        if (mem.member == "mapError") {
            auto base_t = check_expr(*mem.base);
            if (base_t != nullptr && base_t->kind() == TypeKind::Result) {
                if (c.args.size() != 1) {
                    for (const auto& a : c.args) {
                        (void)check_expr(*a.value);
                    }
                    error_at(c.range,
                             ".mapError takes exactly one argument (the mapping function)");
                    return types_->make_result(base_t->inner(), base_t->result());
                }
                if (!c.args[0].label.empty()) {
                    error_at(c.args[0].value->range, ".mapError argument cannot have a label");
                }
                // §9 / §16 — when the argument is a closure literal,
                // it needs a contextual Function type so its single
                // param adopts the Result's error type. Push down
                // `(E) -> ?` (result left unconstrained; the body's
                // last expression types the new error). For named-
                // function arguments the expected type is harmless:
                // function symbols ignore the hint and their types
                // are checked against the (E) -> E' shape below.
                auto expected_fn = types_->make_function({base_t->result()}, nullptr);
                auto arg_t = check_expr(*c.args[0].value, expected_fn);
                if (arg_t == nullptr || arg_t->is_error()) {
                    return types_->error();
                }
                if (arg_t->kind() != TypeKind::Function || arg_t->parts().size() != 1) {
                    error_at(c.args[0].value->range,
                             std::format(".mapError argument must be a function (E) -> E', got {}",
                                         arg_t->describe()));
                    return types_->make_result(base_t->inner(), base_t->result());
                }
                if (!TypeArena::assignable(base_t->result(), arg_t->parts()[0])) {
                    error_at(c.args[0].value->range,
                             std::format(".mapError function expects {} but Result error is {}",
                                         arg_t->parts()[0]->describe(),
                                         base_t->result()->describe()));
                    return types_->make_result(base_t->inner(), base_t->result());
                }
                return types_->make_result(base_t->inner(), arg_t->result());
            }
        }
    }

    // §9 iterator combinators: `zip(a, b)` and `take(xs, n)` are
    // free-function builtins. The user can shadow them with their own
    // `zip` / `take` (the scope check below defers to a user symbol if
    // present); otherwise we recognize the call shape directly. Both
    // require their iterable argument(s) to expose a `next() -> T?`
    // method.
    if (c.callee->kind == ast::NodeKind::IdentExpr) {
        const auto& ci = static_cast<const ast::IdentExpr&>(*c.callee);
        if ((ci.name == "zip" || ci.name == "take" || ci.name == "map" || ci.name == "filter")
            && scopes_.current().lookup(ci.name) == nullptr) {
            auto iterator_element = [&](TypePtr iter_t) -> TypePtr {
                if (iter_t == nullptr || iter_t->is_error()) {
                    return nullptr;
                }
                auto nt = lookup_method(iter_t, "next");
                if (nt == nullptr || nt->kind() != TypeKind::Function || nt->result() == nullptr
                    || nt->result()->kind() != TypeKind::Optional) {
                    return nullptr;
                }
                return nt->result()->inner();
            };
            if (ci.name == "zip") {
                if (c.args.size() != 2) {
                    for (const auto& a : c.args) {
                        (void)check_expr(*a.value);
                    }
                    error_at(c.range, "zip(a, b) takes exactly two iterator arguments");
                    return types_->make_zip_iter(types_->error(), types_->error());
                }
                for (const auto& a : c.args) {
                    if (!a.label.empty()) {
                        error_at(a.value->range, "zip arguments cannot have labels");
                    }
                }
                auto ta = check_expr(*c.args[0].value);
                auto tb = check_expr(*c.args[1].value);
                auto ea = iterator_element(ta);
                auto eb = iterator_element(tb);
                if (ea == nullptr && ta != nullptr && !ta->is_error()) {
                    error_at(c.args[0].value->range,
                             std::format("zip's first argument must be an iterator (have a "
                                         "'next() -> Element?' method), got {}",
                                         ta->describe()));
                }
                if (eb == nullptr && tb != nullptr && !tb->is_error()) {
                    error_at(c.args[1].value->range,
                             std::format("zip's second argument must be an iterator (have a "
                                         "'next() -> Element?' method), got {}",
                                         tb->describe()));
                }
                return types_->make_zip_iter(ea != nullptr ? ea : types_->error(),
                                             eb != nullptr ? eb : types_->error());
            }
            if (ci.name == "take") {
                if (c.args.size() != 2) {
                    for (const auto& a : c.args) {
                        (void)check_expr(*a.value);
                    }
                    error_at(c.range, "take(xs, n) takes exactly two arguments");
                    return types_->make_take_iter(types_->error());
                }
                for (const auto& a : c.args) {
                    if (!a.label.empty()) {
                        error_at(a.value->range, "take arguments cannot have labels");
                    }
                }
                auto txs = check_expr(*c.args[0].value);
                auto tn = check_expr(*c.args[1].value);
                auto exs = iterator_element(txs);
                if (exs == nullptr && txs != nullptr && !txs->is_error()) {
                    error_at(c.args[0].value->range,
                             std::format("take's first argument must be an iterator (have a "
                                         "'next() -> Element?' method), got {}",
                                         txs->describe()));
                }
                if (tn != nullptr && !tn->is_error() && !tn->is_integer()) {
                    error_at(c.args[1].value->range,
                             std::format("take's second argument must be an integer count, got {}",
                                         tn->describe()));
                }
                return types_->make_take_iter(exs != nullptr ? exs : types_->error());
            }
            // §9 `map(xs, f)` / `filter(xs, p)` — both type-check the
            // closure against an expected function type derived from
            // xs's element type. For map, f is (T) -> U and the
            // result's element type is U; for filter, p is (T) -> Bool
            // and the element type is preserved. The closure must be
            // passed directly so the expected-type flow can shape its
            // params (sema rejects a free closure with no context).
            if (ci.name == "map" || ci.name == "filter") {
                if (c.args.size() != 2) {
                    for (const auto& a : c.args) {
                        (void)check_expr(*a.value);
                    }
                    error_at(c.range,
                             std::format("{}(xs, {}) takes exactly two arguments",
                                         ci.name,
                                         ci.name == "map" ? "f" : "p"));
                    if (ci.name == "map") {
                        return types_->make_map_iter(types_->error(), types_->error());
                    }
                    return types_->make_filter_iter(types_->error());
                }
                for (const auto& a : c.args) {
                    if (!a.label.empty()) {
                        error_at(a.value->range,
                                 std::format("{} arguments cannot have labels", ci.name));
                    }
                }
                auto txs = check_expr(*c.args[0].value);
                auto exs = iterator_element(txs);
                if (exs == nullptr && txs != nullptr && !txs->is_error()) {
                    error_at(c.args[0].value->range,
                             std::format("{}'s first argument must be an iterator (have a "
                                         "'next() -> Element?' method), got {}",
                                         ci.name,
                                         txs->describe()));
                    exs = types_->error();
                }
                if (exs == nullptr) {
                    exs = types_->error();
                }
                if (ci.name == "filter") {
                    // Predicate: shape (T) -> Bool. Flow the expected
                    // type so a closure literal types its param.
                    auto pred_expected = types_->make_function({exs}, types_->boolean());
                    auto tp = check_expr(*c.args[1].value, pred_expected);
                    if (tp == nullptr || tp->is_error()) {
                        return types_->make_filter_iter(exs);
                    }
                    if (tp->kind() != TypeKind::Function || tp->parts().size() != 1
                        || tp->result() == nullptr || tp->result()->kind() != TypeKind::Bool) {
                        error_at(c.args[1].value->range,
                                 std::format("filter's predicate must be (T) -> Bool, got {}",
                                             tp->describe()));
                        return types_->make_filter_iter(exs);
                    }
                    if (!TypeArena::assignable(exs, tp->parts()[0])) {
                        error_at(c.args[1].value->range,
                                 std::format("filter predicate expects {} but iterator yields {}",
                                             tp->parts()[0]->describe(),
                                             exs->describe()));
                    }
                    return types_->make_filter_iter(exs);
                }
                // map: shape (T) -> U. Flow only the param type into
                // the closure context; the return type is whatever
                // the closure body produces.
                auto fn_expected = types_->make_function({exs}, /*result=*/nullptr);
                auto tf = check_expr(*c.args[1].value, fn_expected);
                if (tf == nullptr || tf->is_error()) {
                    return types_->make_map_iter(exs, types_->error());
                }
                if (tf->kind() != TypeKind::Function || tf->parts().size() != 1) {
                    error_at(c.args[1].value->range,
                             std::format("map's transform must be a function (T) -> U, got {}",
                                         tf->describe()));
                    return types_->make_map_iter(exs, types_->error());
                }
                if (!TypeArena::assignable(exs, tf->parts()[0])) {
                    error_at(c.args[1].value->range,
                             std::format("map transform expects {} but iterator yields {}",
                                         tf->parts()[0]->describe(),
                                         exs->describe()));
                }
                return types_->make_map_iter(
                    exs, tf->result() != nullptr ? tf->result() : types_->error());
            }
        }
    }

    // §A7 (§14.14) `Scheduler.swapContext(saving:, loading:)` —
    // the context-switch primitive. The spec signature uses
    // `MutPtr[Context]` / `Ptr[Context]` arguments derived from
    // `&decl` (§14.6.3). v0.5 doesn't yet have the `&decl`
    // address-of operator, so we accept `Context` values directly
    // — the codegen wraps each argument in `&` when lowering. When
    // `&decl` lands, this intercept tightens to the pointer form.
    if (c.callee->kind == ast::NodeKind::MemberExpr) {
        const auto& mem = static_cast<const ast::MemberExpr&>(*c.callee);
        if (mem.base != nullptr && mem.base->kind == ast::NodeKind::IdentExpr) {
            const auto& bi = static_cast<const ast::IdentExpr&>(*mem.base);
            if (bi.name == "Scheduler" && mem.member == "swapContext"
                && scopes_.current().lookup("Scheduler") == nullptr) {
                if (c.args.size() != 2) {
                    for (const auto& a : c.args) {
                        (void)check_expr(*a.value);
                    }
                    error_at(c.range,
                             "Scheduler.swapContext takes exactly two arguments "
                             "(saving: Context, loading: Context)");
                    return types_->unit();
                }
                TypePtr context_t =
                    builtin_context_decl_ != nullptr
                        ? types_->make_nominal(TypeKind::Struct, builtin_context_decl_.get())
                        : types_->error();
                auto saving_t = check_expr(*c.args[0].value, context_t);
                auto loading_t = check_expr(*c.args[1].value, context_t);
                if (saving_t != nullptr && !saving_t->is_error()
                    && !TypeArena::equal(saving_t, context_t)) {
                    error_at(c.args[0].value->range,
                             std::format("Scheduler.swapContext saving argument must be "
                                         "Context, got {}",
                                         saving_t->describe()));
                }
                if (loading_t != nullptr && !loading_t->is_error()
                    && !TypeArena::equal(loading_t, context_t)) {
                    error_at(c.args[1].value->range,
                             std::format("Scheduler.swapContext loading argument must be "
                                         "Context, got {}",
                                         loading_t->describe()));
                }
                return types_->unit();
            }
        }
    }

    // §A3 (§10.5) raw-mint primitives. Four static-call shapes:
    //   * Ptr.unchecked(fromAddress: addr)    -> Ptr[T]
    //   * MutPtr.unchecked(fromAddress: addr) -> MutPtr[T]
    //   * Span.raw(at: ptr, count: n)         -> Span[T]
    //   * MutSpan.raw(at: ptr, count: n)      -> MutSpan[T]
    //
    // T for the .unchecked forms comes from the expected type at the
    // call site (the let-binding's annotation, the function's
    // return type, etc.). T for the .raw forms comes from the
    // pointer argument itself. All four require `RawMemory` in
    // scope; the capability checker enforces that separately.
    if (c.callee->kind == ast::NodeKind::MemberExpr) {
        const auto& mem = static_cast<const ast::MemberExpr&>(*c.callee);
        if (mem.base != nullptr && mem.base->kind == ast::NodeKind::IdentExpr) {
            const auto& bi = static_cast<const ast::IdentExpr&>(*mem.base);
            const bool is_ptr = bi.name == "Ptr";
            const bool is_mut_ptr = bi.name == "MutPtr";
            const bool is_span = bi.name == "Span";
            const bool is_mut_span = bi.name == "MutSpan";
            if ((is_ptr || is_mut_ptr) && mem.member == "unchecked"
                && scopes_.current().lookup(bi.name) == nullptr) {
                if (c.args.size() != 1) {
                    for (const auto& a : c.args) {
                        (void)check_expr(*a.value);
                    }
                    error_at(c.range,
                             std::format("{}.unchecked takes exactly one argument "
                                         "(fromAddress: UInt64)",
                                         bi.name));
                    return is_ptr ? types_->make_ptr(types_->error())
                                  : types_->make_mut_ptr(types_->error());
                }
                if (!c.args[0].label.empty() && c.args[0].label != "fromAddress") {
                    error_at(c.args[0].value->range,
                             std::format("{}.unchecked expects label 'fromAddress'", bi.name));
                }
                auto addr_t = check_expr(*c.args[0].value, types_->primitive(TypeKind::UInt64));
                if (addr_t != nullptr && !addr_t->is_error() && !addr_t->is_integer()) {
                    error_at(c.args[0].value->range,
                             std::format("{}.unchecked address must be an integer, got {}",
                                         bi.name,
                                         addr_t->describe()));
                }
                // T comes from the expected type. Without it, we
                // can't infer — surface that as a diagnostic so the
                // user adds an annotation.
                TypePtr T = nullptr;
                if (expected != nullptr) {
                    if (is_ptr && expected->kind() == TypeKind::Ptr) {
                        T = expected->inner();
                    } else if (is_mut_ptr && expected->kind() == TypeKind::MutPtr) {
                        T = expected->inner();
                    }
                }
                if (T == nullptr) {
                    error_at(c.range,
                             std::format("{}.unchecked needs an expected {}[T] type at the call "
                                         "site (annotate the let-binding or return slot)",
                                         bi.name,
                                         bi.name));
                    T = types_->error();
                }
                return is_ptr ? types_->make_ptr(T) : types_->make_mut_ptr(T);
            }
            // §A6 (§14.11) `MmioWireView.at(ptr, endianness)` —
            // parallel to the bare-view constructor but takes the
            // device's endianness as a second arg. Capability check
            // is the same `Mmio` gate.
            if (bi.name == "MmioWireView" && mem.member == "at"
                && scopes_.current().lookup(bi.name) == nullptr) {
                if (c.args.size() != 2) {
                    for (const auto& a : c.args) {
                        (void)check_expr(*a.value);
                    }
                    error_at(c.range,
                             "MmioWireView.at takes exactly two arguments "
                             "(_ ptr: MutPtr[T], endianness: Endianness)");
                    return types_->make_mmio_wire_view(types_->error());
                }
                auto ptr_t = check_expr(*c.args[0].value);
                TypePtr endian_t =
                    builtin_endianness_decl_ != nullptr
                        ? types_->make_nominal(TypeKind::Enum, builtin_endianness_decl_.get())
                        : types_->error();
                auto endian_arg_t = check_expr(*c.args[1].value, endian_t);
                if (ptr_t != nullptr && !ptr_t->is_error() && ptr_t->kind() != TypeKind::MutPtr) {
                    error_at(c.args[0].value->range,
                             std::format("MmioWireView.at first argument must be MutPtr[T], got {}",
                                         ptr_t->describe()));
                }
                TypePtr T = (ptr_t != nullptr && ptr_t->kind() == TypeKind::MutPtr)
                                ? ptr_t->inner()
                                : types_->error();
                if (endian_arg_t != nullptr && !endian_arg_t->is_error()
                    && !TypeArena::equal(endian_arg_t, endian_t)) {
                    error_at(c.args[1].value->range,
                             std::format("MmioWireView.at endianness must be Endianness, got {}",
                                         endian_arg_t->describe()));
                }
                return types_->make_mmio_wire_view(T);
            }
            // §A6 (§14.11) MmioView.at(_ ptr: MutPtr[T]) → MmioView[T]
            // and MmioRegion.at(_ ptr: MutPtr[T], count: Int) →
            // MmioRegion[T]. Both require the `Mmio` capability,
            // gated by the capability checker. T comes from the
            // MutPtr arg, not from the call-site expected type —
            // unlike .unchecked, this constructor is total over T.
            if ((bi.name == "MmioView" || bi.name == "MmioRegion") && mem.member == "at"
                && scopes_.current().lookup(bi.name) == nullptr) {
                const bool is_view = bi.name == "MmioView";
                const std::size_t expected_arity = is_view ? 1 : 2;
                if (c.args.size() != expected_arity) {
                    for (const auto& a : c.args) {
                        (void)check_expr(*a.value);
                    }
                    error_at(c.range,
                             std::format("{}.at takes exactly {} argument{}",
                                         bi.name,
                                         expected_arity,
                                         expected_arity == 1 ? "" : "s"));
                    return is_view ? types_->make_mmio_view(types_->error())
                                   : types_->make_mmio_region(types_->error());
                }
                auto ptr_t = check_expr(*c.args[0].value);
                if (ptr_t != nullptr && !ptr_t->is_error() && ptr_t->kind() != TypeKind::MutPtr) {
                    error_at(c.args[0].value->range,
                             std::format("{}.at first argument must be MutPtr[T], got {}",
                                         bi.name,
                                         ptr_t->describe()));
                }
                TypePtr T = (ptr_t != nullptr && ptr_t->kind() == TypeKind::MutPtr)
                                ? ptr_t->inner()
                                : types_->error();
                if (!is_view) {
                    auto ct = check_expr(*c.args[1].value, types_->primitive(TypeKind::Int));
                    if (ct != nullptr && !ct->is_error() && !ct->is_integer()) {
                        error_at(c.args[1].value->range,
                                 std::format("MmioRegion.at count must be an integer, got {}",
                                             ct->describe()));
                    }
                }
                return is_view ? types_->make_mmio_view(T) : types_->make_mmio_region(T);
            }
            if ((is_span || is_mut_span) && mem.member == "raw"
                && scopes_.current().lookup(bi.name) == nullptr) {
                if (c.args.size() != 2) {
                    for (const auto& a : c.args) {
                        (void)check_expr(*a.value);
                    }
                    error_at(c.range,
                             std::format("{}.raw takes exactly two arguments (at: ..., count: ...)",
                                         bi.name));
                    return is_span ? types_->make_span(types_->error())
                                   : types_->make_mut_span(types_->error());
                }
                auto pt = check_expr(*c.args[0].value);
                auto ct = check_expr(*c.args[1].value, types_->primitive(TypeKind::Int));
                TypePtr T = types_->error();
                if (is_span) {
                    if (pt != nullptr && pt->kind() == TypeKind::Ptr) {
                        T = pt->inner();
                    } else if (pt != nullptr && !pt->is_error()) {
                        error_at(
                            c.args[0].value->range,
                            std::format("Span.raw 'at' must be a Ptr[T], got {}", pt->describe()));
                    }
                } else {
                    if (pt != nullptr && pt->kind() == TypeKind::MutPtr) {
                        T = pt->inner();
                    } else if (pt != nullptr && !pt->is_error()) {
                        error_at(c.args[0].value->range,
                                 std::format("MutSpan.raw 'at' must be a MutPtr[T], got {}",
                                             pt->describe()));
                    }
                }
                if (ct != nullptr && !ct->is_error() && !ct->is_integer()) {
                    error_at(c.args[1].value->range,
                             std::format("{}.raw count must be an integer, got {}",
                                         bi.name,
                                         ct->describe()));
                }
                return is_span ? types_->make_span(T) : types_->make_mut_span(T);
            }
        }
    }

    // §14.8 / §12.6 `cfg.option("name")` — comptime-folded build
    // option accessor. v0.5 admits one name: "cache_line_bytes"
    // (returns 64 — the default that §A11's Padded[T] template
    // hardcodes today). Other names error with the admitted set
    // listed so the user sees v0.5's scope. The folded value lands
    // via Resolution::set_folded_value so codegen emits the integer
    // literal directly without re-folding.
    if (c.callee->kind == ast::NodeKind::MemberExpr) {
        const auto& mem = static_cast<const ast::MemberExpr&>(*c.callee);
        if (mem.base->kind == ast::NodeKind::IdentExpr
            && static_cast<const ast::IdentExpr&>(*mem.base).name == "cfg" && mem.member == "option"
            && scopes_.current().lookup("cfg") == nullptr) {
            if (c.args.size() != 1) {
                for (const auto& a : c.args) {
                    (void)check_expr(*a.value);
                }
                error_at(c.range, "cfg.option(name) takes exactly one string-literal argument");
                return types_->primitive(TypeKind::Int);
            }
            if (!c.args[0].label.empty()) {
                error_at(c.args[0].value->range, "cfg.option argument cannot have a label");
            }
            if (c.args[0].value == nullptr || c.args[0].value->kind != ast::NodeKind::StringLit) {
                (void)check_expr(*c.args[0].value);
                error_at(c.range, "cfg.option argument must be a literal string");
                return types_->primitive(TypeKind::Int);
            }
            const auto& arg = static_cast<const ast::StringLit&>(*c.args[0].value);
            std::int64_t value = 0;
            if (arg.text == "cache_line_bytes") {
                value = 64;
            } else {
                error_at(arg.range,
                         std::format("cfg.option({:?}) — unknown option; v0.5 admits "
                                     "\"cache_line_bytes\"",
                                     std::string{arg.text}));
                return types_->primitive(TypeKind::Int);
            }
            ComptimeValue folded;
            folded.kind = ComptimeValue::Kind::Int;
            folded.i = value;
            folded.type = TypeKind::Int;
            resolution_.set_folded_value(&c, std::move(folded));
            return types_->primitive(TypeKind::Int);
        }
    }

    // §10 builtin `Box.new(value)` — the unique-ownership heap pointer
    // constructor. The callee is a `MemberExpr(IdentExpr("Box"), "new")`;
    // sema doesn't have a symbol for the Box "value" so we intercept
    // the call shape directly. The result type is Box<typeof(value)>.
    if (c.callee->kind == ast::NodeKind::MemberExpr) {
        const auto& mem = static_cast<const ast::MemberExpr&>(*c.callee);
        if (mem.base->kind == ast::NodeKind::IdentExpr
            && static_cast<const ast::IdentExpr&>(*mem.base).name == "Box" && mem.member == "new"
            && scopes_.current().lookup("Box") == nullptr) {
            if (c.args.size() != 1) {
                for (const auto& a : c.args) {
                    (void)check_expr(*a.value);
                }
                error_at(c.range, "Box.new(value) takes exactly one argument");
                return types_->make_box(types_->error());
            }
            if (!c.args[0].label.empty()) {
                error_at(c.args[0].value->range, "Box.new argument cannot have a label");
            }
            auto inner = check_expr(*c.args[0].value);
            return types_->make_box(inner != nullptr ? inner : types_->error());
        }
    }

    // §A11 (§14.8) `PerCpu.new(value)` — the heap factory for per-hart
    // storage. Returns `Box[PerCpu[T]]`; the C++ side allocates a 64-
    // byte-aligned `__vstr::PerCpu<T>` and wraps it in a unique_ptr.
    // When the surrounding context names `Box[PerCpu[T]]`, T is pushed
    // down to the value check so an integer literal adopts the
    // expected width; otherwise the value's type pins T. Capability
    // gate is Alloc, parallel to Box.new.
    if (c.callee->kind == ast::NodeKind::MemberExpr) {
        const auto& mem = static_cast<const ast::MemberExpr&>(*c.callee);
        if (mem.base->kind == ast::NodeKind::IdentExpr
            && static_cast<const ast::IdentExpr&>(*mem.base).name == "PerCpu" && mem.member == "new"
            && scopes_.current().lookup("PerCpu") == nullptr) {
            if (c.args.size() != 1) {
                for (const auto& a : c.args) {
                    (void)check_expr(*a.value);
                }
                error_at(c.range, "PerCpu.new(value) takes exactly one argument");
                return types_->make_box(types_->make_per_cpu(types_->error()));
            }
            if (!c.args[0].label.empty()) {
                error_at(c.args[0].value->range, "PerCpu.new argument cannot have a label");
            }
            TypePtr value_hint = nullptr;
            if (expected != nullptr && expected->kind() == TypeKind::Box
                && expected->inner() != nullptr && expected->inner()->kind() == TypeKind::PerCpu) {
                value_hint = expected->inner()->inner();
            }
            auto inner = check_expr(*c.args[0].value, value_hint);
            return types_->make_box(
                types_->make_per_cpu(inner != nullptr ? inner : types_->error()));
        }
    }

    // §3 opaque-type construction: `Q(t)` where Q is a newtype over T —
    // one positional arg, type-checked against T, result is Q. The
    // codegen lowers this to `static_cast<Q>(t)` over the `enum class
    // Q : T {}` emission.
    if (c.callee->kind == ast::NodeKind::IdentExpr) {
        const auto& callee_ident = static_cast<const ast::IdentExpr&>(*c.callee);
        if (const auto* sym = scopes_.current().lookup(callee_ident.name);
            sym != nullptr && sym->kind == SymbolKind::OpaqueType && sym->decl != nullptr) {
            resolution_.set_symbol(c.callee.get(), sym);
            check_visibility(*sym, c.callee->range);
            const auto& od = static_cast<const ast::OpaqueDecl&>(*sym->decl);
            auto underlying = od.underlying ? resolve_type(*od.underlying) : types_->error();
            if (c.args.size() != 1) {
                for (const auto& a : c.args) {
                    (void)check_expr(*a.value);
                }
                error_at(c.range,
                         std::format("opaque-type constructor {}(...) takes exactly one argument",
                                     od.name));
                return sym->type;
            }
            if (!c.args[0].label.empty()) {
                error_at(c.args[0].value->range,
                         "opaque-type constructor argument cannot have a label");
            }
            auto arg_type = check_expr(*c.args[0].value, underlying);
            if (!TypeArena::assignable(arg_type, underlying)) {
                error_at(c.args[0].value->range,
                         std::format("opaque-type {}({}) requires a {} argument, got {}",
                                     od.name,
                                     underlying ? underlying->describe() : "?",
                                     underlying ? underlying->describe() : "?",
                                     arg_type ? arg_type->describe() : "?"));
            }
            return sym->type;
        }
    }

    // Resolve the callee first so we know each parameter's expected type
    // before we type its corresponding argument — this is what lets integer
    // literals adopt the parameter's type without an explicit conversion.
    //
    // We pass `expected` through to the callee so a leading-dot enum case in
    // call position (`.circle(radius: 1.0)`) can resolve against the expected
    // enum type.
    auto callee_type = check_expr(*c.callee, expected);
    if (callee_type == nullptr || callee_type->is_error()) {
        // Still type the arguments so their internal errors surface.
        for (const auto& a : c.args) {
            (void)check_expr(*a.value);
        }
        return types_->error();
    }

    // §A4 (§14.9.3) free-standing compareExchangeWeak rejection. The
    // weak form is admitted only inside one of the spec's retry-loop
    // shapes (tracked via weak_cas_loop_depth_). When we see a weak
    // CAS call site outside that range, emit a diagnostic pointing
    // the user at the strong form. Doing the check here — after
    // callee resolution — keeps the test simple: we know the
    // synthesized method type and the call's range.
    if (c.callee->kind == ast::NodeKind::MemberExpr && weak_cas_loop_depth_ == 0) {
        const auto& mem = static_cast<const ast::MemberExpr&>(*c.callee);
        if (mem.member == "compareExchangeWeak") {
            auto base_t = resolution_.type_of(mem.base.get());
            if (base_t != nullptr && base_t->kind() == TypeKind::Atomic) {
                error_at(c.range,
                         "compareExchangeWeak is admitted only inside a recognized retry "
                         "loop (`while !r.succeeded { … }` or `while true { … if r.succeeded "
                         "{ break } }`). Use compareExchange for a one-shot CAS.");
            }
        }
    }

    // Struct construction: `Point(x: 10, y: 32)` parses as a CallExpr whose
    // callee evaluates to the nominal Struct type. Dispatch by-label against
    // the struct's fields rather than treating it as a function call.
    if (callee_type->kind() == TypeKind::Struct && callee_type->nominal_decl() != nullptr) {
        const auto& s_decl = static_cast<const ast::StructDecl&>(*callee_type->nominal_decl());
        // §A6 (§6.8 / §14.11.4) `@repr(union)`: members overlay one
        // storage cell, so exactly one labeled argument picks the
        // active member. Fall through to label/type checks, but the
        // "missing field" sweep at the end is skipped for unions —
        // the unseen members are by design, not an error.
        const bool is_union = struct_is_repr_union(s_decl);
        if (is_union && c.args.size() != 1) {
            error_at(c.range,
                     std::format("@repr(union) struct '{}' constructor requires exactly one "
                                 "labeled argument naming the active member (got {})",
                                 s_decl.name,
                                 c.args.size()));
        }
        // §7 generics phase 2 — a generic struct infers its type
        // parameters from the labeled arguments, exactly like a generic
        // function call. The field types are resolved with the params as
        // opaque placeholders (`resolve_member_type_with_generics(.., {})`);
        // unifying each against its argument's type builds the binding map.
        // The expected type, when it's an instance of this same struct,
        // seeds the map so an annotated binding pins the width.
        const bool is_generic = named_type_param_count(s_decl.generics) > 0;
        std::unordered_map<std::string, TypePtr> gbindings;
        if (is_generic && expected != nullptr && expected->kind() == TypeKind::Struct
            && expected->nominal_decl() == &s_decl) {
            const auto& ep = expected->parts();
            std::size_t gi = 0;
            for (const auto& gp : s_decl.generics) {
                if (gp.name.empty()) {
                    continue;
                }
                if (gi < ep.size() && ep[gi] != nullptr) {
                    gbindings.emplace(gp.name, ep[gi]);
                }
                ++gi;
            }
        }
        // Build a label → (field, generic field type) index once.
        std::unordered_map<std::string, std::pair<const ast::StructDecl::Field*, TypePtr>> by_label;
        for (const auto& f : s_decl.fields) {
            if (f.kind == ast::StructDecl::Field::Kind::Embed) {
                continue;  // embeds aren't constructible by label (yet)
            }
            by_label.emplace(f.name,
                             std::make_pair(&f,
                                            f.type ? resolve_member_type_with_generics(
                                                         s_decl.generics, *f.type, {})
                                                   : types_->error()));
        }
        std::vector<bool> seen(s_decl.fields.size(), false);
        for (std::size_t i = 0; i < c.args.size(); ++i) {
            const auto& arg = c.args[i];
            if (arg.label.empty()) {
                error_at(
                    arg.value->range,
                    std::format("struct constructor argument {} requires a field label", i + 1));
                (void)check_expr(*arg.value);
                continue;
            }
            auto it = by_label.find(arg.label);
            if (it == by_label.end()) {
                error_at(arg.value->range,
                         std::format("'{}' has no field '{}'", s_decl.name, arg.label));
                (void)check_expr(*arg.value);
                continue;
            }
            auto generic_ft = it->second.second;
            auto field_type = types_->substitute(generic_ft, gbindings);
            auto arg_type = check_expr(*arg.value, field_type);
            if (is_generic) {
                unify_generic(generic_ft, arg_type, gbindings, arg.value->range);
                field_type = types_->substitute(generic_ft, gbindings);
            }
            if (!TypeArena::assignable(arg_type, field_type)) {
                error_at(arg.value->range,
                         std::format("field '{}' of type {} cannot accept value of type {}",
                                     arg.label,
                                     field_type ? field_type->describe() : "?",
                                     arg_type ? arg_type->describe() : "?"));
            }
            // Track coverage for "missing field" diagnostics.
            for (std::size_t k = 0; k < s_decl.fields.size(); ++k) {
                if (&s_decl.fields[k] == it->second.first) {
                    seen[k] = true;
                    break;
                }
            }
        }
        // Missing fields are an error — Vestra has no implicit defaulting.
        // Exception: @repr(union) overlay structs activate exactly one
        // member per construction, so unmentioned fields are not
        // missing in any meaningful sense.
        if (!is_union) {
            for (std::size_t k = 0; k < s_decl.fields.size(); ++k) {
                if (s_decl.fields[k].kind == ast::StructDecl::Field::Kind::Embed) {
                    continue;
                }
                if (!seen[k]) {
                    error_at(c.range,
                             std::format("struct '{}' constructor is missing field '{}'",
                                         s_decl.name,
                                         s_decl.fields[k].name));
                }
            }
        }
        if (!is_generic) {
            return callee_type;
        }
        // Assemble the inferred instance type. An unbound parameter (no
        // argument mentioned it and no expected type seeded it) is a
        // diagnostic — the user should annotate the binding's type.
        std::vector<TypePtr> inst_args;
        for (const auto& gp : s_decl.generics) {
            if (gp.name.empty()) {
                continue;
            }
            auto bit = gbindings.find(gp.name);
            if (bit == gbindings.end()) {
                error_at(c.range,
                         std::format("cannot infer {} argument '{}' for generic struct '{}'; "
                                     "annotate the binding's type (e.g. `let x: {}[...] = ...`)",
                                     gp.is_const ? "const" : "type",
                                     gp.name,
                                     s_decl.name,
                                     s_decl.name));
                inst_args.push_back(types_->error());
            } else {
                inst_args.push_back(bit->second);
            }
        }
        return types_->make_struct_instance(callee_type->nominal_decl(), std::move(inst_args));
    }

    if (callee_type->kind() != TypeKind::Function) {
        for (const auto& a : c.args) {
            (void)check_expr(*a.value);
        }
        error_at(c.callee->range,
                 std::format("called value is not a function (type {})", callee_type->describe()));
        return types_->error();
    }

    // Generic-aware call dispatch (§7): if the callee is a generic Vestra
    // function, build a substitution map by unifying its declared parameter
    // types against the argument types, then apply the substitution to each
    // param (for arg checking) and to the return type (for the call's
    // expression type).
    //
    // The non-generic path stays a strict structural match — the generic
    // path is a strict superset, so we go through it for every call where
    // we have a known callee FuncDecl. Calls into opaque function values
    // fall through to the non-generic check below.
    const ast::FuncDecl* fn = nullptr;
    if (const auto* sym = resolution_.symbol_of(c.callee.get())) {
        if (sym->decl != nullptr && sym->decl->kind == ast::NodeKind::Func) {
            fn = static_cast<const ast::FuncDecl*>(sym->decl);
        }
    }

    const auto& params = callee_type->parts();
    if (params.size() != c.args.size()) {
        for (const auto& a : c.args) {
            (void)check_expr(*a.value);
        }
        error_at(c.range,
                 std::format("call expects {} argument(s), got {}", params.size(), c.args.size()));
        return callee_type->result() != nullptr ? callee_type->result() : types_->error();
    }

    // Seed bindings from the surrounding context: if the call's result type
    // is a generic placeholder (or contains one) and we know the type the
    // caller expects, bind from that. Lets `let x: Int32 = identity(7)` work
    // even though the literal `7` alone would default to Int.
    std::unordered_map<std::string, TypePtr> bindings;
    if (expected != nullptr && callee_type->result() != nullptr) {
        unify_generic(callee_type->result(), expected, bindings, c.range);
    }

    // Pass 1: collect bindings from concrete arg types. We don't enforce
    // type matches yet — that happens after substitution. As we walk left
    // to right, we substitute each parameter's expected type with the
    // bindings discovered so far, so an integer literal on the right side
    // of `max_of(int32_value, 35)` adopts Int32 (already inferred from the
    // first argument) instead of defaulting to Int and then conflicting.
    for (std::size_t i = 0; i < params.size(); ++i) {
        auto pty = types_->substitute(params[i], bindings);
        auto arg_type = check_expr(*c.args[i].value, pty);
        unify_generic(params[i], arg_type, bindings, c.args[i].value->range);
    }

    // Pass 2: enforce each (substituted_param, arg) pair. We re-check the
    // arg with the substituted expected type so bidirectional literal
    // adaptation can pick up the generic's inferred concrete type.
    for (std::size_t i = 0; i < params.size(); ++i) {
        auto pty = types_->substitute(params[i], bindings);
        auto arg_type = check_expr(*c.args[i].value, pty);
        if (!TypeArena::assignable(arg_type, pty)) {
            error_at(c.args[i].value->range,
                     std::format("argument {} of type {} does not match parameter type {}",
                                 i + 1,
                                 arg_type ? arg_type->describe() : "?",
                                 pty ? pty->describe() : "?"));
        }
    }

    // Verify every generic parameter ended up bound. An unbound generic at a
    // call site means inference couldn't pin it down — usually because the
    // generic only appears in the return position with no contextual type.
    if (fn != nullptr) {
        for (const auto& gp : fn->generics) {
            if (gp.name.empty()) {
                continue;
            }
            // A const generic is inferred the same way (unify_generic binds
            // a `[N]T` parameter's length from the argument), so an unbound
            // one is reported here too.
            if (!bindings.contains(gp.name)) {
                error_at(c.range,
                         std::format("cannot infer {} '{}' for call to '{}'",
                                     gp.is_const ? "const generic" : "generic parameter",
                                     gp.name,
                                     fn->name));
            }
        }
        // §7 generics — every `where T: P` / `[T: P]` bound on the callee
        // must be satisfied by the inferred argument type.
        check_generic_bounds(fn->generics, bindings, c.range);
    }

    auto result = callee_type->result() != nullptr ? callee_type->result() : types_->unit();
    return types_->substitute(result, bindings);
}

TypePtr Resolver::check_if(const ast::IfExpr& i, TypePtr expected) {
    TypePtr then_type;
    if (!i.let_name.empty()) {
        // §9 `if let NAME = INIT { ... }` — INIT must be Optional<T>; the
        // then-branch is checked in a scope where NAME is bound to T.
        auto init_type = check_expr(*i.let_init);
        TypePtr bound;
        if (init_type == nullptr || init_type->is_error()) {
            bound = types_->error();
        } else if (init_type->kind() != TypeKind::Optional) {
            error_at(i.let_init->range,
                     std::format("'if let' requires an Optional initializer, got {}",
                                 init_type->describe()));
            bound = types_->error();
        } else {
            bound = init_type->inner();
        }
        ScopeStack::Guard g(scopes_);
        Symbol sym;
        sym.name = i.let_name;
        sym.kind = SymbolKind::Local;
        sym.type = bound;
        sym.definition_range = i.range;
        (void)scopes_.current().insert(std::move(sym));
        then_type = check_expr(*i.then_branch, expected);
    } else {
        auto cond_type = check_expr(*i.cond, types_->boolean());
        if (cond_type != nullptr && cond_type->kind() != TypeKind::Bool && !cond_type->is_error()) {
            error_at(i.cond->range,
                     std::format("if condition must be Bool, got {}", cond_type->describe()));
        }
        then_type = check_expr(*i.then_branch, expected);
    }
    if (!i.else_branch) {
        return types_->unit();
    }
    auto else_type = check_expr(*i.else_branch, expected != nullptr ? expected : then_type);
    if (then_type == nullptr || else_type == nullptr) {
        return types_->error();
    }
    if (then_type->is_never()) {
        return else_type;
    }
    if (else_type->is_never()) {
        return then_type;
    }
    if (!TypeArena::equal(then_type, else_type)) {
        error_at(i.range,
                 std::format("if branches have different types: {} vs {}",
                             then_type->describe(),
                             else_type->describe()));
        return types_->error();
    }
    return then_type;
}

// ============================================================================
// Type-expression resolution
// ============================================================================

std::optional<std::vector<TypePtr>>
Resolver::resolve_generic_instance_args(const std::vector<ast::GenericParam>& generics,
                                        const ast::NamedType& n,
                                        std::string_view kind,
                                        std::string_view name,
                                        diag::SourceRange range) {
    const std::size_t arity = named_type_param_count(generics);
    if (arity == 0) {
        if (n.has_generics) {
            error_at(range, std::format("'{}' is not a generic type", name));
        }
        return std::nullopt;
    }
    const std::size_t provided = n.type_args.size() + n.const_args.size();
    if (!n.has_generics || provided == 0) {
        error_at(range,
                 std::format("generic {} '{}' requires {} generic argument(s) written as '{}[...]'",
                             kind,
                             name,
                             arity,
                             name));
        return std::nullopt;
    }
    if (provided != arity) {
        error_at(range,
                 std::format("generic {} '{}' expects {} generic argument(s), got {}",
                             kind,
                             name,
                             arity,
                             provided));
    }
    // Walk the parameters in declaration order, drawing each from the
    // matching by-kind argument sequence: a type parameter consumes the
    // next type_arg, a const parameter the next const_arg (as a ConstValue).
    std::vector<TypePtr> args;
    args.reserve(arity);
    std::size_t ti = 0;
    std::size_t ci = 0;
    for (const auto& gp : generics) {
        if (gp.name.empty()) {
            continue;
        }
        if (gp.is_const) {
            args.push_back(ci < n.const_args.size() ? types_->make_const_value(n.const_args[ci++])
                                                    : types_->error());
        } else {
            args.push_back(ti < n.type_args.size() && n.type_args[ti] != nullptr
                               ? resolve_type(*n.type_args[ti++])
                               : types_->error());
        }
    }
    // §7 generics — enforce any `[T: P]` bounds on the type parameters
    // against the supplied arguments at the instantiation site.
    std::unordered_map<std::string, TypePtr> bindings;
    std::size_t bi = 0;
    for (const auto& gp : generics) {
        if (gp.name.empty()) {
            continue;
        }
        if (bi < args.size()) {
            bindings.emplace(gp.name, args[bi]);
        }
        ++bi;
    }
    check_generic_bounds(generics, bindings, range);
    return args;
}

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

// ============================================================================
// Member access, methods, enum cases, match
// ============================================================================

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

bool Resolver::is_display_conformant(TypePtr t) const {
    if (t == nullptr || t->is_error()) {
        return true;  // suppress cascading errors on already-broken types
    }
    if (t->is_primitive()) {
        // Unit / Never / Error aren't usefully Display-printable, but
        // is_primitive() rules out Never/Error already; Unit would
        // surface as `void` in C++ and never reach here.
        return true;
    }
    switch (t->kind()) {
    case TypeKind::Vector:
        return is_display_conformant(t->inner());
    case TypeKind::Optional:
        // `"\(opt)"` renders `.none` as `nil` and `.some(v)` by
        // delegating to T's formatter. Codegen emits a templated
        // `std::formatter<std::optional<T>>` specialization in the
        // runtime preamble (`__vstr::OptionalFormatter`) to carry
        // the C++ side. The inner T still has to be Display-
        // conformant — this rule composes, it doesn't bypass.
        return is_display_conformant(t->inner());
    case TypeKind::Struct:
    case TypeKind::Enum:
    case TypeKind::OpaqueType:
        return decl_derives(t->nominal_decl(), "Display")
               || decl_derives(t->nominal_decl(), "Debug");
    default:
        return false;
    }
}

bool Resolver::is_default_conformant(TypePtr t) const {
    if (t == nullptr || t->is_error()) {
        return false;
    }
    switch (t->kind()) {
    case TypeKind::Int:
    case TypeKind::Int8:
    case TypeKind::Int16:
    case TypeKind::Int32:
    case TypeKind::Int64:
    case TypeKind::UInt:
    case TypeKind::UInt8:
    case TypeKind::UInt16:
    case TypeKind::UInt32:
    case TypeKind::UInt64:
    case TypeKind::Int128:
    case TypeKind::UInt128:
    case TypeKind::Float32:
    case TypeKind::Float64:
    case TypeKind::Bool:
    case TypeKind::Char:
    case TypeKind::Str:
        return true;
    case TypeKind::Optional:
        // `nil` is the zero of every Optional<T>; std::optional<T>{} is
        // value-init to nullopt, no need for T to itself be conformant.
        return true;
    case TypeKind::Vector:
        // Fixed-length array `[N]T` lowers to `std::array<T, N>`; value-
        // init zero-initializes every element. Require the element to be
        // Default-conformant so the chain stays decidable.
        return is_default_conformant(t->inner());
    case TypeKind::Tuple:
        for (const auto& part : t->parts()) {
            if (!is_default_conformant(part)) {
                return false;
            }
        }
        return true;
    case TypeKind::Struct:
        return decl_derives(t->nominal_decl(), "Default");
    case TypeKind::GenericParam:
        // §7 generics phase 2 — a field typed by a generic parameter can't
        // be checked for Default-conformance until the type is
        // instantiated. Defer: accept here, and the C++ value-init at the
        // instantiation site enforces it.
        return true;
    case TypeKind::OpaqueType: {
        // §3 newtype follow-on: an opaque is Default-conformant iff
        // the user has written `derive(Default) for Q` AND the
        // underlying type is itself Default-conformant. The
        // `enum class Q : Underlying {}` C++ form gives `Q{}` = the
        // underlying's zero value automatically, so no codegen
        // change is needed — the conformance check is what gates
        // use of `Q()` as a default-init slot.
        if (!decl_derives(t->nominal_decl(), "Default")) {
            return false;
        }
        if (t->nominal_decl() == nullptr || t->nominal_decl()->kind != ast::NodeKind::Opaque) {
            return false;
        }
        const auto& od = static_cast<const ast::OpaqueDecl&>(*t->nominal_decl());
        if (od.underlying == nullptr) {
            return false;
        }
        // The mutable_this-style const_cast is unavoidable here: the
        // helper signature is const but resolve_type isn't (it
        // memoizes named types into the arena). The check is
        // logically const; the lookup just needs the arena's
        // identity-preserving make_X paths.
        auto under = const_cast<Resolver*>(this)->resolve_type(*od.underlying);
        return is_default_conformant(under);
    }
    default:
        // Box, Result, GenericParam, Enum (bare or sum), Protocol,
        // Function, Reference — none have a v0.5 default. The kernel
        // resort is `var x: T? = nil` plus an explicit init.
        return false;
    }
}

void Resolver::bind_tuple_pattern(const ast::TuplePat& pat, TypePtr value_type) {
    if (value_type == nullptr || value_type->is_error()) {
        return;  // suppress cascading errors
    }
    if (value_type->kind() != TypeKind::Tuple) {
        error_at(
            pat.range,
            std::format("tuple pattern requires a tuple value, got {}", value_type->describe()));
        return;
    }
    const auto& parts = value_type->parts();
    if (parts.size() != pat.elements.size()) {
        error_at(pat.range,
                 std::format("tuple pattern has {} elements, value has {}",
                             pat.elements.size(),
                             parts.size()));
        return;
    }
    for (std::size_t i = 0; i < pat.elements.size(); ++i) {
        const auto& sub = pat.elements[i];
        if (!sub) {
            continue;
        }
        std::string_view name;
        if (sub->kind == ast::NodeKind::IdentPat) {
            name = static_cast<const ast::IdentPat&>(*sub).name;
        } else if (sub->kind == ast::NodeKind::BindPat) {
            name = static_cast<const ast::BindPat&>(*sub).name;
        } else if (sub->kind == ast::NodeKind::WildcardPat) {
            continue;
        } else if (sub->kind == ast::NodeKind::TuplePat) {
            bind_tuple_pattern(static_cast<const ast::TuplePat&>(*sub), parts[i]);
            continue;
        } else if (sub->kind == ast::NodeKind::LiteralPat || sub->kind == ast::NodeKind::RangePat
                   || sub->kind == ast::NodeKind::OrPat) {
            // §17.7 value-shape patterns inside a tuple pattern: each
            // sub-pattern type-checks against the corresponding element
            // type; no binding is introduced.
            check_pattern(*sub, parts[i]);
            continue;
        } else {
            error_at(sub->range,
                     "v0.5 only supports identifier / `_` / nested-tuple / literal / "
                     "range / or-pattern element kinds inside a tuple pattern");
            continue;
        }
        if (name.empty()) {
            continue;
        }
        Symbol sym;
        sym.name = std::string{name};
        sym.kind = SymbolKind::Local;
        sym.type = parts[i];
        sym.definition_range = sub->range;
        if (auto* prev = scopes_.current().insert(std::move(sym))) {
            duplicate_definition(*prev, sym.name, sub->range);
        }
    }
}

bool Resolver::type_satisfies_bound(TypePtr t, std::string_view protocol) const {
    if (t == nullptr || t->is_error()) {
        return true;  // suppress cascading errors on already-broken types
    }
    // A generic parameter standing in for a concrete type can't be checked
    // structurally here; accept it. The enclosing scope's own bound on that
    // parameter is the relevant constraint — transitive bound checking is a
    // follow-on.
    if (t->kind() == TypeKind::GenericParam) {
        return true;
    }
    if (protocol == "Display") {
        return is_display_conformant(t);
    }
    if (protocol == "Default") {
        return is_default_conformant(t);
    }
    if (protocol == "Eq" || protocol == "Hash" || protocol == "Debug" || protocol == "Clone") {
        if (t->is_primitive()) {
            return true;
        }
        switch (t->kind()) {
        case TypeKind::Optional:
        case TypeKind::Vector:
            return type_satisfies_bound(t->inner(), protocol);
        case TypeKind::Tuple:
            for (const auto& p : t->parts()) {
                if (!type_satisfies_bound(p, protocol)) {
                    return false;
                }
            }
            return true;
        case TypeKind::Struct:
        case TypeKind::Enum:
        case TypeKind::OpaqueType:
            return decl_derives(t->nominal_decl(), protocol);
        default:
            return false;
        }
    }
    if (protocol == "Comparable" || protocol == "Ordered") {
        // Numeric, character, and string primitives have a natural ordering.
        // v0.5 has no derive(Comparable), so a struct/enum can't yet declare
        // one.
        if (t->is_numeric()) {
            return true;
        }
        switch (t->kind()) {
        case TypeKind::Char:
        case TypeKind::Str:
        case TypeKind::StrConst:
        case TypeKind::String:
            return true;
        default:
            return false;
        }
    }
    // An unrecognized (user-defined) protocol can't be verified without a
    // general conformance table, which v0.5 doesn't have. Accept rather than
    // reject a construct we can't check.
    return true;
}

void Resolver::check_generic_bounds(const std::vector<ast::GenericParam>& generics,
                                    const std::unordered_map<std::string, TypePtr>& bindings,
                                    diag::SourceRange range) {
    for (const auto& gp : generics) {
        if (gp.is_const || gp.name.empty() || gp.bound.protocols.empty()) {
            continue;
        }
        auto it = bindings.find(gp.name);
        if (it == bindings.end() || it->second == nullptr) {
            continue;  // unbound — reported separately
        }
        for (const auto& proto : gp.bound.protocols) {
            if (proto == nullptr || proto->kind != ast::NodeKind::NamedType) {
                continue;
            }
            const auto& nt = static_cast<const ast::NamedType&>(*proto);
            if (nt.path.empty()) {
                continue;
            }
            std::string_view pname = nt.path.back();
            if (!type_satisfies_bound(it->second, pname)) {
                error_at(range,
                         std::format("type {} does not satisfy the bound '{}: {}'",
                                     it->second->describe(),
                                     gp.name,
                                     pname));
            }
        }
    }
}

bool Resolver::decl_derives(const ast::Decl* decl, std::string_view protocol) const {
    if (decl == nullptr || unit_ == nullptr) {
        return false;
    }
    for (const auto& d : unit_->decls) {
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
        // Match by simple name — same convention the codegen derive
        // index uses, since paths today are unqualified for in-unit
        // derive targets.
        std::string_view target_name = tt.path.back();
        std::string_view decl_name;
        if (decl->kind == ast::NodeKind::Struct) {
            decl_name = static_cast<const ast::StructDecl&>(*decl).name;
        } else if (decl->kind == ast::NodeKind::Enum) {
            decl_name = static_cast<const ast::EnumDecl&>(*decl).name;
        } else if (decl->kind == ast::NodeKind::Opaque) {
            decl_name = static_cast<const ast::OpaqueDecl&>(*decl).name;
        } else {
            continue;
        }
        if (target_name != decl_name) {
            continue;
        }
        for (const auto& p : dd.protocols) {
            if (p == nullptr || p->kind != ast::NodeKind::NamedType) {
                continue;
            }
            const auto& pt = static_cast<const ast::NamedType&>(*p);
            if (!pt.path.empty() && pt.path.back() == protocol) {
                return true;
            }
        }
    }
    return false;
}

TypePtr Resolver::lookup_method(TypePtr owner_type,
                                std::string_view name,
                                const ast::FuncDecl** out_method) {
    if (owner_type == nullptr) {
        return nullptr;
    }
    // §9 iterator combinators: synthesize `next() -> Element?` on the
    // builtin adaptor types. ZipIter[A, B]'s element is the tuple
    // (A, B); TakeIter[A]'s element is just A. The codegen runtime
    // provides the matching member function in __vstr::Zip / __vstr::Take
    // (see emit_runtime_preamble), so this is a pure type-level shim.
    // §A11 (§14.8) `PerCpu[T].mine()` — returns the current hart's
    // slot. v0.5 hosts a single slot so this is just the inner T;
    // the kernel target reads a target-specific hart-index
    // register and indexes the `[MAX_HARTS]Padded[T]` storage. The
    // call's mutating-receiver discipline is handled the same way
    // Atomic gets the inout-on-static exemption — the C++-layer
    // `mine()` returns a reference.
    // §14.12 SysregHandle (RW): `.read() -> T` and `.write(T) ->
    // Unit`. Both methods discharge Asm at the call site (gated in
    // capability.cpp via the SysregHandle base type).
    if (owner_type->kind() == TypeKind::SysregHandle && owner_type->inner() != nullptr) {
        if (name == "read") {
            return types_->make_function({}, owner_type->inner());
        }
        if (name == "write") {
            return types_->make_function({owner_type->inner()}, types_->unit());
        }
    }
    // §14.12.1 ReadOnlySysreg: only `.read()` admits. A `.write(v)`
    // call on a RO handle falls through to the no-such-method error
    // path, which produces a typed diagnostic naming the kind so
    // the user knows why `Sysreg.midr_el1.write(v)` failed compile.
    if (owner_type->kind() == TypeKind::SysregHandleRO && owner_type->inner() != nullptr) {
        if (name == "read") {
            return types_->make_function({}, owner_type->inner());
        }
    }
    // §14.12.1 WriteOnlySysreg: only `.write(v)` admits. The v0.5
    // canonical name set has no WO members but the kind is reachable
    // once the kernel target's table extends the registered set.
    if (owner_type->kind() == TypeKind::SysregHandleWO && owner_type->inner() != nullptr) {
        if (name == "write") {
            return types_->make_function({owner_type->inner()}, types_->unit());
        }
    }
    if (owner_type->kind() == TypeKind::PerCpu && owner_type->inner() != nullptr) {
        if (name == "mine") {
            return types_->make_function({}, owner_type->inner());
        }
        // §A11 (§14.8) `slot(hartId: UInt16) using RawMemory -> Ptr[T]`
        // — cross-hart accessor for the kernel's IPI / hart-startup /
        // stats-aggregation loops. v0.5 uses UInt16 as the HartId
        // surface type (proper opaque HartId would land alongside a
        // `core.systems.hart` namespace in a later slice). The
        // RawMemory capability gates the call site.
        if (name == "slot") {
            return types_->make_function({types_->primitive(TypeKind::UInt16)},
                                         types_->make_ptr(owner_type->inner()));
        }
    }

    // §A6 (§14.11) MMIO method synthesis. `MmioView[T]` exposes
    // `.read() -> T` and `.write(T)` (the latter mutates the
    // underlying register, but the inout receiver doesn't need a
    // `var` binding — same exemption Atomic gets, since the view
    // is a thin handle). `MmioRegion[T]` exposes `.index(Int)`
    // returning the indexed view.
    if (owner_type->kind() == TypeKind::MmioView && owner_type->inner() != nullptr) {
        if (name == "read") {
            return types_->make_function({}, owner_type->inner());
        }
        if (name == "write") {
            return types_->make_function({owner_type->inner()}, types_->unit());
        }
    }
    if (owner_type->kind() == TypeKind::MmioRegion && owner_type->inner() != nullptr) {
        if (name == "index") {
            return types_->make_function({types_->primitive(TypeKind::Int)},
                                         types_->make_mmio_view(owner_type->inner()));
        }
    }
    // §A6 (§14.11.3) MmioWireView exposes the same `read` / `write`
    // shape as MmioView; the difference (the conditional byte swap)
    // lives at the C++ template layer.
    if (owner_type->kind() == TypeKind::MmioWireView && owner_type->inner() != nullptr) {
        if (name == "read") {
            return types_->make_function({}, owner_type->inner());
        }
        if (name == "write") {
            return types_->make_function({owner_type->inner()}, types_->unit());
        }
    }

    // §A4 (§14.9.2) Atomic[T] method synthesis. Every method takes an
    // optional trailing `Ordering` argument; sema models them as a
    // single-required, single-optional shape — the default lives in
    // a documentation comment, codegen substitutes .seqCst when the
    // user omits it. fetchAdd/fetchSub take `(delta: T, ordering)`
    // and return T; load returns T; store/exchange take a T value;
    // exchange returns the prior T. The ordering type is the builtin
    // Ordering enum.
    if (owner_type->kind() == TypeKind::Atomic && owner_type->inner() != nullptr) {
        TypePtr T = owner_type->inner();
        TypePtr ordering_type =
            builtin_ordering_decl_ != nullptr
                ? types_->make_nominal(TypeKind::Enum, builtin_ordering_decl_.get())
                : types_->error();
        if (name == "load") {
            return types_->make_function({ordering_type}, T);
        }
        if (name == "store") {
            return types_->make_function({T, ordering_type}, types_->unit());
        }
        if (name == "exchange") {
            return types_->make_function({T, ordering_type}, T);
        }
        if (name == "fetchAdd" || name == "fetchSub" || name == "fetchAnd" || name == "fetchOr"
            || name == "fetchXor") {
            return types_->make_function({T, ordering_type}, T);
        }
        // §A4 (§14.9.3) compareExchange (strong) and compareExchangeWeak
        // share the same shape — (expected, desired, success, failure)
        // → CASResult[T]. The strong form is always admitted; the weak
        // form is admitted only inside one of the §14.9.3 retry-loop
        // shapes (see check_call below + the weak_cas_loop_depth_
        // counter incremented in WhileStmt). All four args are
        // positional in v0.5 (default args + labels on synthetic
        // methods wait on a separate infra phase).
        if (name == "compareExchange" || name == "compareExchangeWeak") {
            return types_->make_function({T, T, ordering_type, ordering_type},
                                         types_->make_cas_result(T));
        }
    }
    // §A4 (§14.9.5) AtomicTaggedPointer[T] method synthesis. The
    // wrapper hides the wide-atomic plumbing behind a typed
    // (MutPtr[T], tag) surface:
    //   .load(ordering) -> (MutPtr[T], UInt64)
    //   .store(ptr, tag, ordering) -> Unit
    //   .compareExchange(exp_ptr, exp_tag, des_ptr, success, failure)
    //       -> CASResult[(MutPtr[T], UInt64)]
    // The CAS auto-bumps the tag on the desired side (the codegen
    // does it), so callers thread only the pointer through; the tag
    // observed on failure threads back via .actual for the retry.
    // MutPtr is the right spelling since the Treiber-stack node's
    // link field is mutable and the CAS itself writes the pointer
    // through the wide-atomic word; users who only want a read-only
    // view can take a Ptr[T] from the MutPtr at the call site.
    if (owner_type->kind() == TypeKind::AtomicTaggedPointer && owner_type->inner() != nullptr) {
        TypePtr T = owner_type->inner();
        TypePtr ptr_t = types_->make_mut_ptr(T);
        TypePtr u64 = types_->primitive(TypeKind::UInt64);
        TypePtr snapshot = types_->make_tuple({ptr_t, u64});
        TypePtr ordering_type =
            builtin_ordering_decl_ != nullptr
                ? types_->make_nominal(TypeKind::Enum, builtin_ordering_decl_.get())
                : types_->error();
        if (name == "load") {
            return types_->make_function({ordering_type}, snapshot);
        }
        if (name == "store") {
            return types_->make_function({ptr_t, u64, ordering_type}, types_->unit());
        }
        if (name == "compareExchange") {
            return types_->make_function({ptr_t, u64, ptr_t, ordering_type, ordering_type},
                                         types_->make_cas_result(snapshot));
        }
    }
    if (name == "next") {
        if (owner_type->kind() == TypeKind::ZipIter && owner_type->parts().size() == 2) {
            auto elem = types_->make_tuple({owner_type->parts()[0], owner_type->parts()[1]});
            return types_->make_function({}, types_->make_optional(elem));
        }
        if (owner_type->kind() == TypeKind::TakeIter && owner_type->inner() != nullptr) {
            return types_->make_function({}, types_->make_optional(owner_type->inner()));
        }
        if (owner_type->kind() == TypeKind::MapIter && owner_type->parts().size() == 2) {
            // The element type is the *output* of f — parts[1].
            return types_->make_function({}, types_->make_optional(owner_type->parts()[1]));
        }
        if (owner_type->kind() == TypeKind::FilterIter && owner_type->inner() != nullptr) {
            return types_->make_function({}, types_->make_optional(owner_type->inner()));
        }
    }
    const auto* decl = owner_type->nominal_decl();
    if (decl == nullptr) {
        return nullptr;
    }
    // §12.3 derive(Clone): surface a synthetic `clone() -> T` method on
    // any nominal that derives Clone, without materializing it in the
    // AST. The codegen injects the matching `T clone() const { return
    // *this; }` body when it emits the type. Bare enums (no payloaded
    // case) lower to C++ `enum class`, which has no method slot, so we
    // skip the synthetic there — the user can just use the implicit
    // copy on the enum value directly.
    if (name == "clone" && decl_derives(decl, "Clone")) {
        bool is_bare_enum = false;
        if (decl->kind == ast::NodeKind::Enum) {
            const auto& ed = static_cast<const ast::EnumDecl&>(*decl);
            is_bare_enum = true;
            for (const auto& c : ed.cases) {
                if (!c.payload.empty()) {
                    is_bare_enum = false;
                    break;
                }
            }
        }
        if (!is_bare_enum) {
            return types_->make_function({}, owner_type);
        }
    }

    auto search = [&](const std::vector<ast::DeclPtr>& methods) -> TypePtr {
        for (const auto& m : methods) {
            if (m->kind != ast::NodeKind::Func) {
                continue;
            }
            const auto& f = static_cast<const ast::FuncDecl&>(*m);
            if (f.name == name) {
                if (out_method != nullptr) {
                    *out_method = &f;
                }
                return function_type_of(f);
            }
        }
        return nullptr;
    };

    switch (decl->kind) {
    case ast::NodeKind::Struct:
        if (auto t = search(static_cast<const ast::StructDecl&>(*decl).methods)) {
            return t;
        }
        // Walk embeds: a method on the embedded struct surfaces too (§6).
        for (const auto& f : static_cast<const ast::StructDecl&>(*decl).fields) {
            if (f.kind == ast::StructDecl::Field::Kind::Embed && f.type) {
                auto embed_type = resolve_type(*f.type);
                if (auto t = lookup_method(embed_type, name, out_method)) {
                    return t;
                }
            }
        }
        return nullptr;
    case ast::NodeKind::Enum:
        return search(static_cast<const ast::EnumDecl&>(*decl).methods);
    default:
        return nullptr;
    }
}

const ast::EnumDecl::Case* Resolver::lookup_enum_case(const ast::EnumDecl& e,
                                                      std::string_view name) {
    for (const auto& c : e.cases) {
        if (c.name == name) {
            return &c;
        }
    }
    return nullptr;
}

TypePtr Resolver::enum_case_constructor_type(TypePtr enum_type, const ast::EnumDecl::Case& c) {
    const auto* decl = enum_type->nominal_decl();
    const auto& ed = static_cast<const ast::EnumDecl&>(*decl);
    const std::size_t arity = named_type_param_count(ed.generics);

    // §7 generics phase 2 — work out the instance arguments and the
    // constructor's result type. For a non-generic enum (arity 0) the
    // result is the bare nominal and payload types resolve plainly. For a
    // generic enum that already carries arguments (enum_type is an
    // instance, e.g. from an expected type or a leading-dot context) the
    // payload types resolve against those concrete arguments. For a generic
    // enum with no arguments yet (an explicit `Enum.case(...)` with no
    // context) the result is a placeholder instance `Enum[T, ...]` and the
    // payload types carry the same placeholders, so the generic-call path
    // in check_call infers the parameters from the argument types.
    std::vector<TypePtr> args = enum_type->parts();
    TypePtr result_type = enum_type;
    if (arity > 0 && args.empty()) {
        std::vector<TypePtr> placeholders;
        for (const auto& gp : ed.generics) {
            if (gp.is_const || gp.name.empty()) {
                continue;
            }
            placeholders.push_back(types_->make_generic_param(gp.name));
        }
        result_type = types_->make_enum_instance(decl, placeholders);
        // args stays empty so payload types resolve to the placeholders.
    }

    if (c.payload.empty()) {
        return result_type;  // bare case is a value of the enum
    }
    std::vector<TypePtr> params;
    params.reserve(c.payload.size());
    for (const auto& p : c.payload) {
        params.push_back(p.second ? resolve_member_type_with_generics(ed.generics, *p.second, args)
                                  : types_->error());
    }
    return types_->make_function(std::move(params), result_type);
}

TypePtr Resolver::check_member(const ast::MemberExpr& m, TypePtr expected) {
    // §14.12 typed system-register access. `Sysreg.<name>` resolves
    // to a SysregHandle<UInt64> over the architectural register
    // named by <name>. The set is the v0.5 canonical aarch64 EL1
    // subset (`midr_el1`, `daif`, `sctlr_el1`, `vbar_el1`,
    // `ttbr0_el1`, `cntfrq_el0`); the kernel target's build extends
    // this with the full per-target list. Sema returns a handle
    // typed `SysregHandle<UInt64>`; `.read()` / `.write(v)` discharge
    // Asm at the call site.
    if (m.base->kind == ast::NodeKind::IdentExpr) {
        const auto& base_ident = static_cast<const ast::IdentExpr&>(*m.base);
        if (base_ident.name == "Sysreg" && scopes_.current().lookup("Sysreg") == nullptr) {
            // v0.5 target-description table. Read-only entries are
            // identification / clock-frequency reads that have no
            // architectural write path: MIDR_EL1 carries the CPU
            // model ID, CNTFRQ_EL0 is the system counter frequency
            // (firmware-set at boot). The kernel target extends both
            // sets with the full per-architecture lists; the WO set
            // is empty in v0.5's canonical subset but the TypeKind
            // is reachable for the wider name set the kernel needs.
            static const std::unordered_set<std::string_view> sysregs_ro = {
                // aarch64 EL1 — read-only
                "midr_el1",
                "mpidr_el1",
                "cntfrq_el0",
                "cntpct_el0",
                "dczid_el0",
            };
            static const std::unordered_set<std::string_view> sysregs_rw = {
                // aarch64 EL1 — read-write
                "daif",
                "sctlr_el1",
                "vbar_el1",
                "ttbr0_el1",
                "ttbr1_el1",
                "tcr_el1",
                "mair_el1",
                "esr_el1",
                "far_el1",
                "elr_el1",
                "spsr_el1",
                "tpidr_el1",
                // x86_64 MSRs — uniformly read-write at ring 0
                "ia32_efer",
                "ia32_lstar",
                "ia32_apic_base",
                "ia32_pat",
                // RISC-V S-mode CSRs — all read-write through csrr / csrw
                "sstatus",
                "sie",
                "stvec",
                "sscratch",
                "sepc",
                "scause",
                "stval",
                "satp",
            };
            TypeKind kind;
            if (sysregs_ro.contains(m.member)) {
                kind = TypeKind::SysregHandleRO;
            } else if (sysregs_rw.contains(m.member)) {
                kind = TypeKind::SysregHandle;
            } else {
                error_at(m.range,
                         std::format("Sysreg.{} — unknown system register; v0.5 admits the "
                                     "aarch64 EL1 + x86_64 MSR + RISC-V S-mode CSR canonical "
                                     "set (see §14.12.2)",
                                     m.member));
                return types_->error();
            }
            auto inner = types_->primitive(TypeKind::UInt64);
            return kind == TypeKind::SysregHandleRO ? types_->make_sysreg_handle_ro(inner)
                                                    : types_->make_sysreg_handle(inner);
        }
    }
    // If the base is an IdentExpr resolving to a Type symbol (Struct/Enum/...),
    // member access is a static lookup (currently: enum case construction).
    if (m.base->kind == ast::NodeKind::IdentExpr) {
        const auto& ident = static_cast<const ast::IdentExpr&>(*m.base);
        if (auto* sym = scopes_.current().lookup(ident.name)) {
            if (sym->kind == SymbolKind::Enum && sym->decl != nullptr) {
                resolution_.set_symbol(m.base.get(), sym);
                check_visibility(*sym, m.base->range);
                const auto& enum_decl = static_cast<const ast::EnumDecl&>(*sym->decl);
                if (const auto* c = lookup_enum_case(enum_decl, m.member)) {
                    // §7 generics phase 2 — if the surrounding context
                    // expects a concrete instance of this same enum, build
                    // the constructor against that instance so the type
                    // arguments are pinned (e.g. `let o: Option[Int32] =
                    // Option.none`). Otherwise use the bare nominal: a
                    // payloaded case infers its arguments from the call, and
                    // a no-payload case with no context is reported below.
                    TypePtr enum_t = sym->type;
                    if (expected != nullptr && expected->kind() == TypeKind::Enum
                        && expected->nominal_decl() == sym->decl && !expected->parts().empty()) {
                        enum_t = expected;
                    }
                    if (named_type_param_count(enum_decl.generics) > 0 && c->payload.empty()
                        && enum_t->parts().empty()) {
                        error_at(m.range,
                                 std::format("cannot infer type argument(s) for generic enum '{}' "
                                             "case '{}'; use it in a typed context (e.g. the "
                                             "leading-dot form `.{}` against a `{}[...]`)",
                                             enum_decl.name,
                                             m.member,
                                             m.member,
                                             enum_decl.name));
                        return types_->error();
                    }
                    auto t = enum_case_constructor_type(enum_t, *c);
                    return t;
                }
                error_at(m.range,
                         std::format("enum '{}' has no case '{}'", enum_decl.name, m.member));
                return types_->error();
            }
            // §12.2 reflection: `StructName.fields` returns a `[N]Field`
            // — one entry per ordinary field (embed fields per §6 are
            // not yet expanded). Field carries `name: Str` today;
            // later phases add `type`, `offset`, and attributes.
            if (sym->kind == SymbolKind::Struct && sym->decl != nullptr) {
                resolution_.set_symbol(m.base.get(), sym);
                check_visibility(*sym, m.base->range);
                const auto& sd = static_cast<const ast::StructDecl&>(*sym->decl);
                // §A2 (§6.8) compile-time layout reflection: T.size /
                // T.alignment fold to Int values. We invoke the folder
                // right here so codegen has a concrete literal to emit
                // (without the fold, `CapEntry.size` would land in the
                // C++ source verbatim — not valid). The fold may
                // return nullopt if some field uses an unsizeable
                // type; we diagnose so the user knows.
                if (m.member == "size" || m.member == "alignment") {
                    if (auto folded = folder_.fold(m, comptime_env_)) {
                        resolution_.set_folded_value(&m, *folded);
                    } else {
                        error_at(m.range,
                                 std::format("can't compute {}.{} at fold time — some field's "
                                             "type has no v0.5 layout (only primitives, vectors "
                                             "of primitives, and structs of those are supported)",
                                             sd.name,
                                             m.member));
                    }
                    return types_->primitive(TypeKind::Int);
                }
                if (m.member == "fields") {
                    std::int64_t count = 0;
                    for (const auto& f : sd.fields) {
                        if (f.kind != ast::StructDecl::Field::Kind::Embed) {
                            ++count;
                        }
                    }
                    // The Field nominal lives in global scope under that name.
                    auto field_type =
                        builtin_field_decl_ != nullptr
                            ? types_->make_nominal(TypeKind::Struct, builtin_field_decl_.get())
                            : types_->error();
                    return types_->make_vector(count, field_type);
                }
                // §12.3 derive(Default): expose a static `() -> T` so
                // `T.default()` is an ordinary CallExpr that the
                // codegen lowers to `T{}`.
                if (m.member == "default" && decl_derives(&sd, "Default")) {
                    return types_->make_function({}, sym->type);
                }
                error_at(m.range,
                         std::format("struct '{}' has no static member '{}'", sd.name, m.member));
                return types_->error();
            }
        }
    }

    auto base_type = check_expr(*m.base);
    if (base_type == nullptr || base_type->is_error()) {
        return types_->error();
    }

    // §9 optional chaining: `a?.b` requires `a : Optional<T>` and looks
    // `b` up on `T`. The chain wraps the lookup result in Optional, and
    // flattens an already-Optional member so `a?.b?.c` never yields
    // Optional<Optional<U>>.
    TypePtr lookup_base = base_type;
    if (m.is_optional_chain) {
        if (base_type->kind() != TypeKind::Optional) {
            error_at(m.base->range,
                     std::format("'?.' requires an Optional base, got {}", base_type->describe()));
            return types_->error();
        }
        lookup_base = base_type->inner();
        if (lookup_base == nullptr) {
            return types_->error();
        }
    }

    // §12.2 vector ergonomics: `someVector.length` is its element count
    // as Int. Folds at comptime; not yet wired for runtime emission
    // (vectors today are emitted as `std::array<T, N>` whose .size() is
    // a constexpr — wiring runtime `.length` is a follow-on).
    auto finish = [&](TypePtr member_type) {
        if (!m.is_optional_chain) {
            return member_type;
        }
        if (member_type == nullptr || member_type->is_error()) {
            return member_type;
        }
        // Flatten: a member already returning Optional<U> stays Optional<U>.
        if (member_type->kind() == TypeKind::Optional) {
            return member_type;
        }
        return types_->make_optional(member_type);
    };

    if (lookup_base->kind() == TypeKind::Vector && m.member == "length") {
        return finish(types_->primitive(TypeKind::Int));
    }

    // §3 opaque newtype: `q.value` extracts the underlying T. The
    // opaque inherits none of T's operations, so this is the user's
    // single explicit unwrap point.
    if (lookup_base->kind() == TypeKind::OpaqueType && m.member == "value"
        && lookup_base->nominal_decl() != nullptr) {
        const auto& od = static_cast<const ast::OpaqueDecl&>(*lookup_base->nominal_decl());
        if (od.underlying != nullptr) {
            return finish(resolve_type(*od.underlying));
        }
    }

    // §10 Box[T]: `box.value` reads the heap-pointee. Lowers to `*box`
    // at the C++ layer over std::unique_ptr.
    if (lookup_base->kind() == TypeKind::Box && m.member == "value"
        && lookup_base->inner() != nullptr) {
        return finish(lookup_base->inner());
    }

    // §A3 (§10.5) Ptr[T] / MutPtr[T]: `p.value` reads the pointee.
    // Mirrors Box[T].value at both the sema and codegen layers. The
    // MutPtr form is assignable through (`p.value = expr`) at the
    // C++ layer; sema doesn't enforce a separate place-expression
    // discipline here since the C++ compiler refuses an assign-
    // through-const-pointer naturally. Dereferencing itself doesn't
    // require RawMemory — the `.unchecked(fromAddress:)` mint that
    // produced the pointer already discharged the cap. v0.5 trusts
    // that audit lane to cover the read/write side too.
    if ((lookup_base->kind() == TypeKind::Ptr || lookup_base->kind() == TypeKind::MutPtr)
        && m.member == "value" && lookup_base->inner() != nullptr) {
        return finish(lookup_base->inner());
    }

    // §A11 (§14.8) Padded[T]: `.value` exposes the inner T. The
    // wrapper holds T plus tail padding to bring sizeof up to a
    // cache line; reads / writes pass through `.value` so the
    // padding stays invisible at the source level.
    if (lookup_base->kind() == TypeKind::Padded && m.member == "value"
        && lookup_base->inner() != nullptr) {
        return finish(lookup_base->inner());
    }

    // §10 Span[T] / MutSpan[T]: `.count` returns the element count as
    // Int; `.isEmpty` returns Bool. Codegen renders these as
    // `static_cast<std::intptr_t>(s.size())` and `s.empty()`.
    if ((lookup_base->kind() == TypeKind::Span || lookup_base->kind() == TypeKind::MutSpan)
        && lookup_base->inner() != nullptr) {
        if (m.member == "count") {
            return finish(types_->primitive(TypeKind::Int));
        }
        if (m.member == "isEmpty") {
            return finish(types_->boolean());
        }
    }
    // §A6 (§14.11.2) MmioRegion[T] surfaces `.count: Int` directly.
    if (lookup_base->kind() == TypeKind::MmioRegion && m.member == "count") {
        return finish(types_->primitive(TypeKind::Int));
    }

    // Field on a struct.
    if (auto field_type = lookup_field(lookup_base, m.member)) {
        // §A2 (§12.2): when the base is a builtin reflection Field
        // value (from `T.fields[i]`), the member access is comptime-
        // only — there's no runtime layout for Field. Eager-fold here
        // so codegen sees a concrete literal (`return 8;` rather than
        // `return CapEntry.fields[1].offset;`). The fold may still
        // fail for an unsizeable struct, in which case codegen would
        // see the raw spelling — but sema also wouldn't have typed
        // the member as a useful Int anyway.
        if (lookup_base->nominal_decl() != nullptr
            && lookup_base->nominal_decl() == builtin_field_decl_.get()) {
            if (auto folded = folder_.fold(m, comptime_env_)) {
                resolution_.set_folded_value(&m, *folded);
            }
        }
        return finish(field_type);
    }
    // Method on a struct/enum.
    if (auto method_type = lookup_method(lookup_base, m.member)) {
        return finish(method_type);
    }
    error_at(m.range,
             std::format("no field or method '{}' on type {}", m.member, lookup_base->describe()));
    return types_->error();
}

TypePtr Resolver::check_leading_dot(const ast::LeadingDotExpr& d, TypePtr expected) {
    // `.foo` only makes sense when there is a contextual type that can
    // resolve the case. Today that's an enum case, an Optional, or — via
    // §12.1 phase 4 — a vector `.zero` initializer. The folder treats
    // `.zero` against a Vector as a zero-filled vector value; sema's job
    // here is just to give the expression the right *type* so the
    // surrounding let/var's annotation check succeeds.
    if (expected != nullptr && expected->kind() == TypeKind::Vector && d.name == "zero") {
        return expected;
    }
    if (expected == nullptr || expected->kind() != TypeKind::Enum
        || expected->nominal_decl() == nullptr) {
        error_at(d.range, std::format("'.{}' has no contextual type to resolve against", d.name));
        return types_->error();
    }
    const auto& enum_decl = static_cast<const ast::EnumDecl&>(*expected->nominal_decl());
    if (const auto* c = lookup_enum_case(enum_decl, d.name)) {
        return enum_case_constructor_type(expected, *c);
    }
    error_at(d.range, std::format("enum '{}' has no case '{}'", enum_decl.name, d.name));
    return types_->error();
}

TypePtr Resolver::check_match(const ast::MatchExpr& m, TypePtr expected) {
    auto scrutinee_type = check_expr(*m.scrutinee);

    // Track which enum cases got covered for the exhaustiveness check.
    const ast::EnumDecl* scrutinee_enum = nullptr;
    if (scrutinee_type != nullptr && scrutinee_type->kind() == TypeKind::Enum
        && scrutinee_type->nominal_decl() != nullptr) {
        scrutinee_enum = &static_cast<const ast::EnumDecl&>(*scrutinee_type->nominal_decl());
    }
    std::vector<bool> case_seen(scrutinee_enum ? scrutinee_enum->cases.size() : 0, false);
    bool saw_default = false;

    // §17.7 enum-coverage walk: drain through OrPat alternatives so a
    // `case .red | .green:` arm marks both cases as covered. Wildcard
    // and unrelated patterns terminate the walk on this arm.
    auto mark_enum_covered = [&](const ast::Pattern& p, auto& self) -> void {
        if (scrutinee_enum == nullptr) {
            return;
        }
        if (p.kind == ast::NodeKind::WildcardPat) {
            for (auto&& s : case_seen) {
                s = true;
            }
            return;
        }
        if (p.kind == ast::NodeKind::EnumPat) {
            const auto& ep = static_cast<const ast::EnumPat&>(p);
            for (std::size_t i = 0; i < scrutinee_enum->cases.size(); ++i) {
                if (scrutinee_enum->cases[i].name == ep.case_name) {
                    case_seen[i] = true;
                    break;
                }
            }
            return;
        }
        if (p.kind == ast::NodeKind::OrPat) {
            for (const auto& alt : static_cast<const ast::OrPat&>(p).alternatives) {
                self(*alt, self);
            }
        }
    };

    // §8 dead-code arm detection. Two flavors of unreachability we
    // can pin at compile time:
    //   - any arm after a default / catch-all wildcard is dead (the
    //     default fires for everything that didn't match earlier).
    //   - an enum-pattern arm whose case_name was already closed by
    //     an earlier *unguarded* arm targeting the same case_name is
    //     dead; the codegen groups arms by case_name and the
    //     unguarded arm dominates anything after it in the group.
    // The warning surfaces the user's mistake at compile time
    // without failing the build; refactoring patterns where this
    // happens silently is a footgun the §8 follow-up commit
    // explicitly flagged for this slice to close.
    bool seen_catchall = false;
    std::unordered_set<std::string> closed_cases;
    auto register_unguarded_cases = [&](const ast::Pattern& p, auto& self) -> void {
        if (p.kind == ast::NodeKind::EnumPat) {
            const auto& ep = static_cast<const ast::EnumPat&>(p);
            closed_cases.insert(ep.case_name);
            return;
        }
        if (p.kind == ast::NodeKind::OrPat) {
            for (const auto& alt : static_cast<const ast::OrPat&>(p).alternatives) {
                if (alt) {
                    self(*alt, self);
                }
            }
        }
    };

    TypePtr result_type = nullptr;
    for (const auto& arm : m.arms) {
        ScopeStack::Guard g(scopes_);
        // §8 dead-arm warning before processing the arm. A previous
        // catch-all dominates everything after it regardless of the
        // current arm's pattern.
        if (seen_catchall) {
            // MatchArm itself has no range; anchor the warning on the
            // best per-arm range available (body, pattern, or the
            // surrounding match's range as the last resort).
            diag::SourceRange anchor = arm.pattern ? arm.pattern->range
                                       : arm.body  ? arm.body->range
                                                   : m.range;
            warn_at(anchor,
                    "match arm is unreachable: a previous default/wildcard arm dominates "
                    "every following case");
        } else if (arm.pattern != nullptr && arm.pattern->kind == ast::NodeKind::EnumPat
                   && !arm.guard) {
            const auto& ep = static_cast<const ast::EnumPat&>(*arm.pattern);
            if (closed_cases.contains(ep.case_name)) {
                warn_at(arm.pattern->range,
                        std::format("match arm is unreachable: a previous unguarded arm for "
                                    "case '.{}' dominates this one",
                                    ep.case_name));
            }
        } else if (arm.pattern != nullptr && arm.pattern->kind == ast::NodeKind::EnumPat
                   && arm.guard) {
            const auto& ep = static_cast<const ast::EnumPat&>(*arm.pattern);
            if (closed_cases.contains(ep.case_name)) {
                warn_at(arm.pattern->range,
                        std::format("match arm is unreachable: a previous unguarded arm for "
                                    "case '.{}' dominates this one (the guard never runs)",
                                    ep.case_name));
            }
        }
        if (arm.is_default) {
            saw_default = true;
            seen_catchall = true;
        } else if (arm.pattern) {
            check_pattern(*arm.pattern, scrutinee_type);
            mark_enum_covered(*arm.pattern, mark_enum_covered);
            // A wildcard arm closes everything (acts as default).
            if (arm.pattern->kind == ast::NodeKind::WildcardPat) {
                seen_catchall = true;
            }
            // §8 close the case_name(s) when the arm is unguarded.
            // The closure walks through OrPat so `case .a | .b` closes
            // both names; a single EnumPat closes its one name.
            if (!arm.guard) {
                register_unguarded_cases(*arm.pattern, register_unguarded_cases);
            }
        }
        if (arm.guard) {
            auto gt = check_expr(*arm.guard, types_->boolean());
            if (gt != nullptr && gt->kind() != TypeKind::Bool && !gt->is_error()) {
                error_at(arm.guard->range,
                         std::format("match guard must be Bool, got {}", gt->describe()));
            }
        }
        TypePtr arm_type = arm.body ? check_expr(*arm.body, expected) : types_->unit();
        if (result_type == nullptr) {
            result_type = arm_type;
        } else if (arm_type != nullptr && !arm_type->is_never() && !result_type->is_never()
                   && !TypeArena::equal(arm_type, result_type)) {
            error_at(arm.body ? arm.body->range : m.range,
                     std::format("match arms have different types: {} vs {}",
                                 result_type->describe(),
                                 arm_type->describe()));
            result_type = types_->error();
        }
    }

    // Exhaustiveness: when the scrutinee is an enum and there's no `default`,
    // every case must be covered by at least one arm.
    if (scrutinee_enum != nullptr && !saw_default) {
        for (std::size_t i = 0; i < scrutinee_enum->cases.size(); ++i) {
            if (!case_seen[i]) {
                error_at(m.range,
                         std::format("match is not exhaustive: case '{}' is not covered",
                                     scrutinee_enum->cases[i].name));
            }
        }
    }

    return result_type != nullptr ? result_type : types_->unit();
}

void Resolver::check_pattern(const ast::Pattern& p, TypePtr scrutinee) {
    switch (p.kind) {
    case ast::NodeKind::WildcardPat:
        break;
    case ast::NodeKind::LiteralPat: {
        // §17.7 literal pattern: the literal must adapt to the
        // scrutinee's type (Int literal in an Int32 slot, etc.).
        // check_expr with the scrutinee as the expected hint does the
        // adaptation, and assignable carries the natural-width rule.
        const auto& lp = static_cast<const ast::LiteralPat&>(p);
        if (lp.literal != nullptr) {
            auto lt = check_expr(*lp.literal, scrutinee);
            if (lt != nullptr && !lt->is_error() && scrutinee != nullptr && !scrutinee->is_error()
                && !TypeArena::assignable(lt, scrutinee)) {
                error_at(p.range,
                         std::format("literal of type {} does not match scrutinee of type {}",
                                     lt->describe(),
                                     scrutinee->describe()));
            }
        }
        break;
    }
    case ast::NodeKind::RangePat: {
        // §17.7 range pattern: scrutinee must be integer; both bounds
        // adapt to the scrutinee's width. Inclusive vs exclusive is a
        // codegen concern — sema just types the bounds.
        const auto& rp = static_cast<const ast::RangePat&>(p);
        if (scrutinee != nullptr && !scrutinee->is_error() && !scrutinee->is_integer()) {
            error_at(p.range,
                     std::format("range pattern requires an integer scrutinee, got {}",
                                 scrutinee->describe()));
        }
        auto check_bound = [&](const ast::Expr* b) {
            if (b == nullptr) {
                return;
            }
            auto bt = check_expr(*b, scrutinee);
            if (bt != nullptr && !bt->is_error() && !bt->is_integer()) {
                error_at(b->range,
                         std::format("range bound must be integer, got {}", bt->describe()));
            }
        };
        check_bound(rp.low.get());
        check_bound(rp.high.get());
        break;
    }
    case ast::NodeKind::OrPat: {
        // §17.7 or-pattern: each alternative is checked against the
        // scrutinee independently. v0.5 doesn't enforce the
        // same-bindings-across-alts rule — a follow-on pass adds the
        // diagnostic for `case .a(let x) | .b(let y):` style mismatch.
        const auto& op = static_cast<const ast::OrPat&>(p);
        for (const auto& alt : op.alternatives) {
            if (alt) {
                check_pattern(*alt, scrutinee);
            }
        }
        break;
    }
    case ast::NodeKind::IdentPat: {
        // Plain identifier patterns match by name; in match-arm position we
        // treat them as a binding to the scrutinee's type.
        const auto& ip = static_cast<const ast::IdentPat&>(p);
        Symbol sym;
        sym.name = ip.name;
        sym.kind = SymbolKind::Local;
        sym.type = scrutinee;
        sym.definition_range = p.range;
        (void)scopes_.current().insert(std::move(sym));
        break;
    }
    case ast::NodeKind::BindPat: {
        const auto& bp = static_cast<const ast::BindPat&>(p);
        Symbol sym;
        sym.name = bp.name;
        sym.kind = SymbolKind::Local;
        sym.type = scrutinee;
        sym.definition_range = p.range;
        (void)scopes_.current().insert(std::move(sym));
        break;
    }
    case ast::NodeKind::TuplePat: {
        // §6 tuple pattern in match-arm position. Two shapes:
        //   * `match tup { case (a, b): ... }` — the scrutinee is a
        //     tuple, the pattern destructures it directly.
        //   * As an EnumPat child: `case .pair((a, b)):` — the enclosing
        //     EnumPat case threads the payload field's type in as
        //     `scrutinee` for this recursive call.
        // Either way, bind_tuple_pattern walks the elements in parallel
        // with the scrutinee's TupleType and reports arity / non-tuple
        // mismatches.
        bind_tuple_pattern(static_cast<const ast::TuplePat&>(p), scrutinee);
        break;
    }
    case ast::NodeKind::EnumPat: {
        const auto& ep = static_cast<const ast::EnumPat&>(p);
        if (scrutinee == nullptr || scrutinee->kind() != TypeKind::Enum
            || scrutinee->nominal_decl() == nullptr) {
            error_at(p.range,
                     std::format("enum pattern '.{}' used against non-enum type {}",
                                 ep.case_name,
                                 scrutinee ? scrutinee->describe() : "?"));
            return;
        }
        const auto& enum_decl = static_cast<const ast::EnumDecl&>(*scrutinee->nominal_decl());
        const auto* c = lookup_enum_case(enum_decl, ep.case_name);
        if (c == nullptr) {
            error_at(p.range,
                     std::format("enum '{}' has no case '{}'", enum_decl.name, ep.case_name));
            return;
        }
        if (ep.children.size() != c->payload.size()) {
            error_at(p.range,
                     std::format("case '{}' has {} payload field(s), pattern binds {}",
                                 c->name,
                                 c->payload.size(),
                                 ep.children.size()));
            return;
        }
        for (std::size_t i = 0; i < ep.children.size(); ++i) {
            // §7 generics phase 2 — substitute the scrutinee instance's
            // type arguments (scrutinee->parts()) for the enum's params so
            // `case .some(let x):` over an `Option[Int32]` binds x as Int32.
            TypePtr payload_type = c->payload[i].second
                                       ? resolve_member_type_with_generics(enum_decl.generics,
                                                                           *c->payload[i].second,
                                                                           scrutinee->parts())
                                       : types_->error();
            check_pattern(*ep.children[i], payload_type);
        }
        break;
    }
    default:
        // Tuple/struct/slice patterns: not yet typed against `scrutinee`.
        break;
    }
}

void Resolver::check_visibility(const Symbol& sym, diag::SourceRange use_range) {
    // For v0.5 we only enforce `private`. Internal/package/public collapse to
    // the same thing inside a single compilation unit. The simple rule: a
    // `private` symbol is reachable only within its declaring scope chain —
    // and right now every top-level decl lives in the global scope, so all a
    // `private` decl can do is hide from… nobody. We still emit the check so
    // the wiring is in place; tighten it once we have nested scopes that
    // *aren't* the inside of a function body (e.g., struct member scopes).
    if (sym.visibility != ast::Visibility::Private) {
        return;
    }
    // Conservative: we don't yet model "the scope that owns sym", so we never
    // reject. This branch is the hook for the eventual real check; tested by
    // a TODO test in tests/sema/resolver_test.cpp.
    (void)use_range;
}

// ============================================================================
// Generic unification helper
// ============================================================================

void Resolver::unify_generic(TypePtr ptype,
                             TypePtr atype,
                             std::unordered_map<std::string, TypePtr>& bindings,
                             diag::SourceRange site) {
    if (ptype == nullptr || atype == nullptr) {
        return;
    }
    if (ptype->kind() == TypeKind::GenericParam) {
        auto name = std::string{ptype->generic_name()};
        auto it = bindings.find(name);
        if (it == bindings.end()) {
            bindings.emplace(std::move(name), atype);
        } else if (!TypeArena::equal(it->second, atype) && !atype->is_error()) {
            error_at(site,
                     std::format("conflicting bindings for generic '{}': {} vs {}",
                                 ptype->generic_name(),
                                 it->second ? it->second->describe() : "?",
                                 atype->describe()));
        }
        return;
    }
    if (ptype->kind() == TypeKind::Optional && atype->kind() == TypeKind::Optional) {
        unify_generic(ptype->inner(), atype->inner(), bindings, site);
        return;
    }
    if (ptype->kind() == TypeKind::Vector && atype->kind() == TypeKind::Vector) {
        unify_generic(ptype->inner(), atype->inner(), bindings, site);
        // §7 generics phase 2 — a symbolic-length parameter `[N]T` unifies
        // its length against the concrete argument's length, binding the
        // const generic N (e.g. inferring N = 4 from a `[4]Int32` argument).
        if (!ptype->vector_length_name().empty()) {
            auto cname = std::string{ptype->vector_length_name()};
            auto cval = types_->make_const_value(atype->vector_length());
            auto it = bindings.find(cname);
            if (it == bindings.end()) {
                bindings.emplace(std::move(cname), cval);
            } else if (!TypeArena::equal(it->second, cval)) {
                error_at(site,
                         std::format("conflicting bindings for const generic '{}': {} vs {}",
                                     ptype->vector_length_name(),
                                     it->second ? it->second->describe() : "?",
                                     cval->describe()));
            }
        }
        return;
    }
    if (ptype->kind() == TypeKind::Tuple && atype->kind() == TypeKind::Tuple
        && ptype->parts().size() == atype->parts().size()) {
        for (std::size_t k = 0; k < ptype->parts().size(); ++k) {
            unify_generic(ptype->parts()[k], atype->parts()[k], bindings, site);
        }
        return;
    }
    // §7 generics phase 2 — a struct or enum instance unifies argument-wise
    // when both sides share a decl. This is what lets expected-type seeding
    // bind a parameter: unifying `Option[T]` (the case-constructor result)
    // against the expected `Option[Int32]` records T = Int32 before the
    // payload arguments are even checked.
    if ((ptype->kind() == TypeKind::Struct || ptype->kind() == TypeKind::Enum)
        && ptype->kind() == atype->kind() && ptype->nominal_decl() == atype->nominal_decl()
        && ptype->parts().size() == atype->parts().size()) {
        for (std::size_t k = 0; k < ptype->parts().size(); ++k) {
            unify_generic(ptype->parts()[k], atype->parts()[k], bindings, site);
        }
        return;
    }
    // Other kinds: structural rules elsewhere already verify the call, so no
    // binding is contributed.
}

// ============================================================================
// §12.6 @when gate
// ============================================================================

namespace {

// Each Decl subtype has its own `attributes` vector. Return a span into the
// right one, or nullptr if the kind doesn't carry attributes (Module,
// Import, Const, Static, Opaque, Derive in our current AST shape).
const std::vector<ast::Attribute>* decl_attributes(const ast::Decl& d) {
    switch (d.kind) {
    case ast::NodeKind::Func:
        return &static_cast<const ast::FuncDecl&>(d).attributes;
    case ast::NodeKind::Struct:
        return &static_cast<const ast::StructDecl&>(d).attributes;
    case ast::NodeKind::Enum:
        return &static_cast<const ast::EnumDecl&>(d).attributes;
    case ast::NodeKind::Protocol:
        return &static_cast<const ast::ProtocolDecl&>(d).attributes;
    case ast::NodeKind::Extension:
        return &static_cast<const ast::ExtensionDecl&>(d).attributes;
    default:
        return nullptr;
    }
}

}  // namespace

bool Resolver::gated_out(const ast::Decl& decl) {
    // Fast path: if we've already evaluated this decl's @when in an earlier
    // pass and stashed the verdict, reuse it. Otherwise check each `when`
    // attribute's predicate and remember the result.
    if (resolution_.is_gated_out(&decl)) {
        return true;
    }
    const auto* attrs = decl_attributes(decl);
    if (attrs == nullptr) {
        return false;
    }
    for (const auto& a : *attrs) {
        if (a.name != "when" || !a.predicate) {
            continue;
        }
        // The predicate is folded against the comptime env in its current
        // state — for top-level decls this is normally empty (consts haven't
        // been registered yet), so the predicate has to rely on cfg.* or
        // pure literals. That matches the §12.6 model.
        auto v = folder_.fold(*a.predicate, comptime_env_);
        if (v && v->kind == ComptimeValue::Kind::Bool && !v->b) {
            resolution_.mark_gated_out(&decl);
            return true;
        }
    }
    return false;
}

}  // namespace vestra::sema
