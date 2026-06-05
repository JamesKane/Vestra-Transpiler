// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// Builtin registration: the synthetic capability / panic / sync /
// math / reflection declarations the resolver injects before user code.
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

// The standard `using` capabilities from §8 — they aren't user-declarable
// types yet (no real stdlib), but a function that writes `using Alloc` must
// not get an "unknown type" error for what is a load-bearing language
// primitive. We register each as a nominal Protocol-kind symbol with a
// placeholder type; the capability checker matches them by name.
void Resolver::register_builtin_capabilities() {
    static constexpr std::array<std::string_view, 12> Names = {
        "Alloc",
        "Log",
        "Async",
        "Net",
        "Clock",
        "Rand",
        // §18 filesystem access (readFile / writeFile). A side-effecting
        // capability like Net/Log; the hosted self-hosting toolchain needs it.
        "Fs",
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

void Resolver::register_builtin_io() {
    // §18 stdout output. `print` / `println` take a `Str` (a literal, a view, or
    // an owned String read-borrowed at the call) and return Unit. Codegen lowers
    // them to std::print / std::println; the `Log` capability gates them
    // (capability.cpp), since writing to stdout is an observable side effect.
    auto unit = types_->unit();
    auto str = types_->primitive(TypeKind::Str);
    auto insert = [&](std::string name) {
        Symbol s;
        s.name = std::move(name);
        s.kind = SymbolKind::Func;
        s.decl = nullptr;
        s.type = types_->make_function({str}, unit);
        s.definition_range = {};
        s.visibility = ast::Visibility::Public;
        (void)scopes_.global().insert(std::move(s));
    };
    insert("print");
    insert("println");

    // §18 filesystem I/O (Fs-gated, see capability.cpp). `readFile(path)` reads
    // a whole file into a `String?` (nil when it can't be read); `writeFile(path,
    // contents)` writes a string and returns whether it succeeded. Codegen
    // lowers them to __vstr::read_file / __vstr::write_file.
    auto str_t = types_->primitive(TypeKind::String);
    {
        Symbol rf;
        rf.name = "readFile";
        rf.kind = SymbolKind::Func;
        rf.type = types_->make_function({str}, types_->make_optional(str_t));
        rf.visibility = ast::Visibility::Public;
        (void)scopes_.global().insert(std::move(rf));

        Symbol wf;
        wf.name = "writeFile";
        wf.kind = SymbolKind::Func;
        wf.type = types_->make_function({str, str}, types_->primitive(TypeKind::Bool));
        wf.visibility = ast::Visibility::Public;
        (void)scopes_.global().insert(std::move(wf));
    }
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

}  // namespace vestra::sema
