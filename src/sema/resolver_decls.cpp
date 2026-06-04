// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// Declaration + statement checking: per-decl dispatch, link-attribute
// validation, function bodies, blocks, and statements.
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
using detail::struct_is_repr_union;

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
            note_string_coercion(*c.value, value_type, annotated);
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
            note_string_coercion(*s.value, value_type, annotated);
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
        if (initializer != nullptr) {
            note_string_coercion(*initializer, value_type, annot_type);
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
            // §5/§19.6 — expose the bound Local so the ownership checker can
            // register a linear binding at its declaration site.
            resolution_.set_binding_symbol(&s, scopes_.current().lookup(binding_name));
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
        // §5/§18.4 — record sub-view provenance (split / chunks / alias) so the
        // exclusivity checker can flag a borrow that aliases the parent place.
        record_partition_provenance(pat, initializer);
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
        if (r.value != nullptr) {
            note_string_coercion(*r.value, value_type, expected);
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
        if (a.value != nullptr) {
            note_string_coercion(*a.value, rhs, lhs);
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
        if (elem_type == nullptr && iter_type != nullptr
            && (iter_type->kind() == TypeKind::Vec || iter_type->kind() == TypeKind::Span
                || iter_type->kind() == TypeKind::MutSpan)
            && iter_type->inner() != nullptr) {
            // §18.5 / §10 / §13 `for x in xs` over a Vec[T], a Span[T]/MutSpan[T]
            // (incl. a Soa column view), iterates its elements directly (lowered
            // to a C++ range-based for); the loop variable type is T.
            elem_type = iter_type->inner();
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
                         std::format("'for x in xs' requires xs to be a Range, a Vec, or have a "
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

}  // namespace vestra::sema
