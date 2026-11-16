// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "vestra/codegen/cpp_emitter.hpp"

#include "cpp_emitter_internal.hpp"
#include "vestra/sema/linearity.hpp"
#include "vestra/sema/resolver.hpp"
#include "vestra/sema/sysregs.hpp"

#include <algorithm>
#include <format>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>

namespace vestra::codegen {

// §6 the struct declares a `deinit`.
bool struct_has_deinit(const ast::StructDecl& s) {
    return s.deinit_body != nullptr;
}


// The broadly-shared emission helpers live in cpp_emitter_internal.hpp
// so the file can be split by theme. Pull them into this TU's scope
// unqualified so existing call sites and the extracted siblings keep
// working.
using detail::enum_is_sum_type;
using detail::primitive_map;
using detail::write_indent;

namespace {

// §4 layout attributes — extract a small struct of what's interesting
// for codegen. The Vestra parser stores each attribute as a name plus
// a single-expression argument; we pattern-match the argument shape.
struct LayoutAttrs {
    bool packed = false;
    bool is_union = false;   // §A6 (§6.8 / §14.11.4) `@repr(union)`
    std::int64_t align = 0;  // 0 = no explicit alignment
};

std::int64_t int_from_attr_arg(const ast::Expr* arg) {
    if (arg == nullptr || arg->kind != ast::NodeKind::IntLit) {
        return 0;
    }
    try {
        return std::stoll(std::string{static_cast<const ast::IntLit&>(*arg).text});
    } catch (...) {
        return 0;
    }
}

LayoutAttrs read_layout_attrs(const std::vector<ast::Attribute>& attrs) {
    LayoutAttrs out;
    for (const auto& a : attrs) {
        if (a.name == "repr") {
            // `@repr(C)` — no-op (C++ default). `@repr(packed)` — adds
            // __attribute__((packed)). `@repr(align(N))` — alignas(N).
            if (a.predicate == nullptr) {
                continue;
            }
            if (a.predicate->kind == ast::NodeKind::IdentExpr) {
                const auto& id = static_cast<const ast::IdentExpr&>(*a.predicate).name;
                if (id == "packed") {
                    out.packed = true;
                } else if (id == "union") {
                    // §A6 (§6.8 / §14.11.4) untagged overlay. The
                    // struct keyword stays in Vestra source; codegen
                    // re-emits as a C++ `union`.
                    out.is_union = true;
                }
                // `C` and any unknown @repr modes are accepted but
                // not given a special emission yet.
            } else if (a.predicate->kind == ast::NodeKind::CallExpr) {
                const auto& call = static_cast<const ast::CallExpr&>(*a.predicate);
                if (call.callee->kind == ast::NodeKind::IdentExpr) {
                    const auto& name = static_cast<const ast::IdentExpr&>(*call.callee).name;
                    if (name == "align" && call.args.size() == 1) {
                        out.align = int_from_attr_arg(call.args[0].value.get());
                    }
                }
            }
        } else if (a.name == "align") {
            // `@align(N)` shorthand for `@repr(align(N))`.
            out.align = int_from_attr_arg(a.predicate.get());
        }
    }
    return out;
}

std::int64_t read_bits_attr(const std::vector<ast::Attribute>& attrs) {
    for (const auto& a : attrs) {
        if (a.name == "bits") {
            return int_from_attr_arg(a.predicate.get());
        }
    }
    return 0;
}

// §A1 (§6.7) / §A2 (§7.8): link-time symbol attributes. Each maps to
// a GNU attribute or the asm-label declarator-trailer. `inline_mode`
// covers `@inline(.always | .never | .hint)`: the C++ lowering is
// [[gnu::always_inline]] / [[gnu::noinline]] / a plain `inline`
// suggestion respectively.
struct LinkAttrs {
    std::string section;      // empty when not set
    std::string symbol;       // asm-name; empty when not set
    std::string alias;        // empty when not set
    std::string visibility;   // "default" / "hidden" / "protected"; empty when not set
    std::string inline_mode;  // "always" / "never" / "hint"; empty when not set
    bool weak = false;
    bool noinit = false;
    // §4 `@align(N)` on a static (a page table, a DMA buffer): `alignas(N)`.
    std::int64_t align = 0;
    // §A8 (§14.5.2) — @interrupt marks a function as an ISR. v0.5
    // emits `[[gnu::used]]` so dead-code elimination doesn't drop a
    // handler that's only referenced from a hand-written vector
    // table or boot-time linker symbol assignment. The kernel-target
    // build replaces this with the matching arch-specific attribute
    // (`__attribute__((interrupt))` on aarch64/x86, etc.) plus the
    // prologue/epilogue the spec describes.
    bool interrupt = false;
    // §A9 (§14.7) — @boot is the pre-MMU regime. @naked is implied:
    // emit `[[gnu::naked]]` so the C++ compiler skips the standard
    // prologue/epilogue and the body is the literal instructions
    // (typically a single asm-block that escapes via &decl to the
    // first ordinary or @kernel_init function).
    bool boot = false;
    // §A10 (§15.4) — `@stack_protector(.none | .strong | .all)`
    // selects per-function canary instrumentation. v0.5 lowers
    // `.none` to `[[gnu::no_stack_protector]]` (Apple Clang + GCC
    // both accept it); `.strong` and `.all` map to the compiler's
    // default since per-function "force-on" attributes don't
    // portably exist (those are toolchain-wide flags).
    std::string stack_protector;  // "none" / "strong" / "all"; empty when not set
};

std::string string_from_attr_arg(const ast::Expr* arg) {
    if (arg == nullptr || arg->kind != ast::NodeKind::StringLit) {
        return {};
    }
    return std::string{static_cast<const ast::StringLit&>(*arg).text};
}

LinkAttrs read_link_attrs(const std::vector<ast::Attribute>& attrs) {
    LinkAttrs out;
    for (const auto& a : attrs) {
        if (a.name == "section") {
            out.section = string_from_attr_arg(a.predicate.get());
        } else if (a.name == "symbol") {
            out.symbol = string_from_attr_arg(a.predicate.get());
        } else if (a.name == "alias") {
            out.alias = string_from_attr_arg(a.predicate.get());
        } else if (a.name == "weak") {
            out.weak = true;
        } else if (a.name == "noinit") {
            out.noinit = true;
        } else if (a.name == "align") {
            out.align = int_from_attr_arg(a.predicate.get());
        } else if (a.name == "interrupt") {
            out.interrupt = true;
        } else if (a.name == "boot" || a.name == "naked") {
            // §14.7 `@naked` on its own, or implied by `@boot`.
            out.boot = true;
        } else if (a.name == "stack_protector" && a.predicate != nullptr
                   && a.predicate->kind == ast::NodeKind::LeadingDotExpr) {
            const auto& d = static_cast<const ast::LeadingDotExpr&>(*a.predicate);
            if (d.name == "none" || d.name == "strong" || d.name == "all") {
                out.stack_protector = d.name;
            }
        } else if (a.name == "visibility" && a.predicate != nullptr
                   && a.predicate->kind == ast::NodeKind::LeadingDotExpr) {
            const auto& d = static_cast<const ast::LeadingDotExpr&>(*a.predicate);
            if (d.name == "default" || d.name == "hidden" || d.name == "protected") {
                out.visibility = d.name;
            }
        } else if (a.name == "inline" && a.predicate != nullptr
                   && a.predicate->kind == ast::NodeKind::LeadingDotExpr) {
            const auto& d = static_cast<const ast::LeadingDotExpr&>(*a.predicate);
            if (d.name == "always" || d.name == "never" || d.name == "hint") {
                out.inline_mode = d.name;
            }
        }
    }
    return out;
}

// Emit the GNU attribute prefix common to func + static: [[gnu::section(...)]]
// [[gnu::weak]] [[gnu::alias(...)]] [[gnu::visibility(...)]]. The
// asm("name") for @symbol lives at the declarator trailer, not the
// prefix; it's emitted separately. Each attribute on its own
// guarantees the order is stable across compilers.
void emit_link_attr_prefix(std::ostream& os, const LinkAttrs& la) {
    if (!la.section.empty()) {
        os << "[[gnu::section(\"" << la.section << "\")]] ";
    }
    if (la.weak) {
        os << "[[gnu::weak]] ";
    }
    if (!la.alias.empty()) {
        os << "[[gnu::alias(\"" << la.alias << "\")]] ";
    }
    if (!la.visibility.empty()) {
        os << "[[gnu::visibility(\"" << la.visibility << "\")]] ";
    }
    // §A8 (§14.5.2) — keep ISR symbols alive across link-time dead-
    // code elimination. A handler referenced only from a hand-written
    // vector table or a kernel boot script would otherwise look
    // unreferenced from the C++ side; `[[gnu::used]]` pins it.
    if (la.interrupt) {
        os << "[[gnu::used]] ";
    }
    // §A9 (§14.7) — @boot implies @naked. The C++ compiler skips
    // the standard prologue/epilogue; the body is exactly the
    // instructions the user writes (typically asm + a single `br`
    // /`jmp` to the next stage's entry point named by &decl).
    if (la.boot) {
        os << "[[gnu::naked]] ";
    }
    // §A10 (§15.4) — `@stack_protector(.none)` suppresses canary
    // instrumentation on this function. `.strong` / `.all` map to
    // the compiler default — per-function "force-on" attributes
    // don't portably exist (they're toolchain-wide flags), so v0.5
    // emits nothing for those; the audit names them anyway.
    if (la.stack_protector == "none") {
        os << "[[gnu::no_stack_protector]] ";
    }
    // §A2 (§7.8) `@inline`. `.always` is a correctness directive —
    // [[gnu::always_inline]] turns failure-to-inline into a compile
    // error naming the obstructing call site, which matches the
    // spec. `.never` is [[gnu::noinline]]. `.hint` is the plain C++
    // `inline` keyword — advisory only. The `inline` decl-specifier
    // itself is emitted *after* the [[nodiscard]] attribute in
    // emit_func, so this prefix sticks to attributes only — putting
    // `inline` between the prefix and `[[nodiscard]]` would have the
    // C++ parser reading `[[nodiscard]]` as part of the type-specifier
    // rather than the function declaration.
    if (la.inline_mode == "always") {
        os << "[[gnu::always_inline]] ";
    } else if (la.inline_mode == "never") {
        os << "[[gnu::noinline]] ";
    }
}

// Renders the `inline` decl-specifier that pairs with the
// [[gnu::always_inline]] attribute (or stands alone for the `.hint`
// case). Called *after* [[nodiscard]] so the attribute attaches to
// the function declaration, not the return type.
void emit_inline_specifier(std::ostream& os, const LinkAttrs& la) {
    if (la.inline_mode == "always" || la.inline_mode == "hint") {
        os << "inline ";
    }
}

}  // namespace

// ----------------------------------------------------------------------------

EmittedUnit CppEmitter::emit(const ast::CompilationUnit& unit, std::string_view output_basename) {
    std::ostringstream hdr;
    std::ostringstream src;

    // §12.3 derive index: collect every `derive(P1, P2, …) for T` at
    // the top level into target-name → {protocol names}. emit_struct
    // and emit_enum read this when injecting the reflective defaults.
    // Each target/protocol path is reduced to its last segment (its
    // simple name) which matches the scope's nominal lookup today.
    // §18 — a module-less `func main()` is the program's real C++ entry point,
    // so its emission captures argc/argv (for args()); a `main` inside a module
    // is just `mod::main`, not the entry.
    unit_has_module_ = unit.module != nullptr;
    module_label_.clear();
    if (unit.module != nullptr) {
        for (const auto& seg : unit.module->path) {
            module_label_ += seg + "_";
        }
    }

    // §13 — index the unit's top-level structs by name so a `Soa[Point]`
    // annotation can reach Point's fields when emitting its tuple-of-vectors.
    // §10 — also index enums (for the move-only fixpoint below).
    structs_by_name_.clear();
    enums_by_name_.clear();
    for (const auto& d : unit.decls) {
        if (d == nullptr) {
            continue;
        }
        if (d->kind == ast::NodeKind::Struct) {
            structs_by_name_[static_cast<const ast::StructDecl&>(*d).name] =
                static_cast<const ast::StructDecl*>(d.get());
        } else if (d->kind == ast::NodeKind::Enum) {
            enums_by_name_[static_cast<const ast::EnumDecl&>(*d).name] =
                static_cast<const ast::EnumDecl*>(d.get());
        }
    }
    // §10 — imported struct/enum decls participate in the move-only fixpoint too,
    // so a let/var/tuple/construction of an *imported* move-only nominal also
    // moves. Local decls win a name clash (emplace does not overwrite).
    for (const auto& [decl, ns] : imported_qualifiers_) {
        if (decl == nullptr) {
            continue;
        }
        if (decl->kind == ast::NodeKind::Struct) {
            structs_by_name_.emplace(static_cast<const ast::StructDecl&>(*decl).name,
                                     static_cast<const ast::StructDecl*>(decl));
        } else if (decl->kind == ast::NodeKind::Enum) {
            enums_by_name_.emplace(static_cast<const ast::EnumDecl&>(*decl).name,
                                   static_cast<const ast::EnumDecl*>(decl));
        }
    }
    compute_move_only_types();
    derives_by_target_.clear();
    for (const auto& d : unit.decls) {
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
        const auto& target_name = tt.path.back();
        for (const auto& p : dd.protocols) {
            if (p == nullptr || p->kind != ast::NodeKind::NamedType) {
                continue;
            }
            const auto& pt = static_cast<const ast::NamedType&>(*p);
            if (pt.path.empty()) {
                continue;
            }
            derives_by_target_[target_name].insert(pt.path.back());
        }
    }

    auto guard = std::string{output_basename};
    for (auto& c : guard) {
        c = (std::isalnum(static_cast<unsigned char>(c)) != 0)
                ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
                : '_';
    }

    hdr << "// Generated by vestra-transpiler. Do not edit.\n";
    // §A10 (§15.5) freestanding profile marker. When `vestra build
    // --no-libc` is set, the comment records the contract so
    // downstream link tooling can refuse to satisfy any libc
    // reference, and so `vestra audit --no-libc` (queued for the
    // audit-surface slice) sees the build's declared profile.
    if (no_libc_) {
        hdr << "// vestra: no_libc = true\n";
    }
    hdr << "#pragma once\n\n";
    // §15.5 the freestanding profile (`--no-libc`) assumes no C++ standard
    // library: only the compiler-supplied C headers, plus the std:: slice the
    // freestanding prelude defines (F1).
    if (no_libc_) {
        hdr << "#include <stddef.h>\n";
        hdr << "#include <stdint.h>\n\n";
    } else {
        hdr << "#include <algorithm>\n";  // §11 scheduler timer queue min_element / erase_if
        hdr << "#include <array>\n";      // §13 [N]T lowers to std::array (+ vector arithmetic)
        hdr << "#include <atomic>\n";     // §A4 Atomic[T] lowers to std::atomic
        hdr << "#include <bit>\n";        // §A6 MmioWireView std::byteswap / std::endian
        hdr << "#include <chrono>\n";     // §11 timeout select arm wall-clock deadlines
        hdr << "#include <concepts>\n";   // §7 generic bounds lower to requires-clauses
        hdr << "#include <coroutine>\n";  // §11 async func / await lower to coroutines
        hdr << "#include <deque>\n";      // §11 Channel[T] backs onto a deque
        hdr << "#include <charconv>\n";   // §18.5 Str.toInt() → std::from_chars
        hdr << "#include <cstdint>\n";
        hdr << "#include <cstdlib>\n";     // §10 panic / abort → std::abort
        hdr << "#include <expected>\n";    // §9 throws(E) → T lowers to std::expected
        hdr << "#include <fstream>\n";     // §18 readFile / writeFile
        hdr << "#include <format>\n";      // §4 interpolated strings lower to std::format
        hdr << "#include <functional>\n";  // §12.3 derive(Hash) std::hash specializations
        hdr << "#include <memory>\n";      // §10 Box[T] lowers to std::unique_ptr
        hdr << "#include <optional>\n";    // §9 T? / nil / if let / ?? / !
        hdr << "#include <print>\n";       // §10 panic shim writes to stderr
        hdr << "#include <span>\n";        // §10 Span[T] / MutSpan[T] views
        hdr << "#include <string>\n";
        hdr << "#include <string_view>\n";
        hdr << "#include <thread>\n";       // §11 timeout select arm sleep_until
        hdr << "#include <tuple>\n";        // §6 tuple types / literals
        hdr << "#include <type_traits>\n";  // match-over-payloaded-enum constexpr-if
        hdr << "#include <utility>\n";      // std::move at sink call sites
        hdr << "#include <variant>\n";
        hdr << "#include <vector>\n\n";
    }

    // §10 runtime shims. `inline` keeps the definitions safe across
    // multiple Vestra-generated headers in one TU (each defines the
    // same function body, which inline tolerates). `Never` is the
    // bottom type — Vestra's `panic` / `abort` / `unreachable` all
    // return Never, which has a templated `[[noreturn]]` conversion
    // op so a call slots into any C++ type the surrounding expression
    // requires (`std::int32_t x = __vstr::panic("oops");` is well-typed
    // because Never→int32_t is a valid implicit conversion that the
    // compiler proves dead via the [[noreturn]] marker on every step).

    // §A6 (§14.11.3) `Endianness` — bus-endianness selector for
    // MmioWireView constructors. Emits at file scope so leading-dot
    // resolution (`.little`, `.big`, `.native`) picks it up
    // unqualified, just like BarrierScope / TlbScope / BarrierKind.
    // §5 multi-file: the runtime prelude below is identical in every generated
    // header. When one module's header `#include`s another's (a cross-module
    // import), both copies land in the same translation unit — guard the prelude
    // so only the first copy defines its (non-inline) types. `#pragma once`
    // dedups a header against itself; this macro dedups the prelude across
    // distinct headers.
    hdr << "#ifndef VESTRA_RUNTIME_PRELUDE\n";
    hdr << "#define VESTRA_RUNTIME_PRELUDE\n\n";

    if (no_libc_) {
        detail::emit_freestanding_prelude(hdr);
    } else {
        detail::emit_hosted_prelude(hdr);
    }

    hdr << "#endif  // VESTRA_RUNTIME_PRELUDE\n\n";

    // §5 C interop: `import c "stdio.h"` lowers to `#include "stdio.h"` at
    // global scope, ahead of the user's namespace. The quoted form finds both
    // in-tree and system headers (compilers fall back to the angle-bracket
    // search path for a quoted include). Vestra doesn't parse the header — the
    // foreign symbols are declared in-tree via `@extern` (§14.6) — but pulling
    // the canonical C declarations in lets the C++ compiler cross-check those
    // `@extern` prototypes and makes any header types/macros they touch visible.
    bool any_c_header = false;
    for (const auto& imp : unit.imports) {
        if (imp == nullptr || !imp->is_c_header || imp->c_header.empty()) {
            continue;
        }
        any_c_header = true;
        hdr << "#include \"" << imp->c_header << "\"\n";
    }
    if (any_c_header) {
        hdr << "\n";
    }

    // §5 multi-file: pull in each imported module's header. Every cross-module
    // reference is now fully qualified — value refs through the resolver's
    // `qualified_name_of` side table, and computed/inferred imported types
    // through emit_sema_type's `imported_qualifiers_` map — so no
    // `using namespace dep;` is emitted; the `#include` alone suffices. The
    // dependency's header is named by its import path as a directory path
    // (`import util.math` → util/math.hpp); -I the output dir to find it.
    bool any_module_import = false;
    for (const auto& imp : unit.imports) {
        if (imp == nullptr || imp->is_c_header || imp->path.empty()) {
            continue;
        }
        any_module_import = true;
        hdr << "#include \"";
        for (std::size_t i = 0; i < imp->path.size(); ++i) {
            if (i != 0) {
                hdr << "/";
            }
            hdr << imp->path[i];
        }
        hdr << ".hpp\"\n";
    }
    if (any_module_import) {
        hdr << "\n";
    }

    auto write_module_path = [&](std::ostream& os) {
        for (std::size_t i = 0; i < unit.module->path.size(); ++i) {
            if (i != 0) {
                os << "::";
            }
            os << unit.module->path[i];
        }
    };

    module_path_ = unit.module ? unit.module->path : std::vector<std::string>{};
    if (unit.module) {
        // Vestra module path → C++ nested namespace.
        hdr << "namespace ";
        write_module_path(hdr);
        hdr << " {\n\n";
    }

    src << "// Generated by vestra-transpiler. Do not edit.\n";
    if (no_libc_) {
        src << "// vestra: no_libc = true\n";
    }
    src << "#include \"" << output_basename << ".hpp\"\n\n";

    if (unit.module) {
        src << "namespace ";
        write_module_path(src);
        src << " {\n\n";
    }

    for (const auto& d : unit.decls) {
        // §12.6: skip @when-gated-out decls. The Resolver populated the
        // Resolution's gated set during its own pass.
        if (resolution_ != nullptr && resolution_->is_gated_out(d.get())) {
            continue;
        }
        emit_decl(hdr, src, *d);
    }

    // §A10 (§15.5) — if the unit declares a @panic_handler, emit a
    // static-init block that registers it with the runtime
    // delegation slot. Goes inside the user's namespace so the
    // handler name resolves unqualified, before the closing brace.
    // Per [basic.start.static] the assignment runs before main, so
    // any panic from user code (or from a static initializer in
    // another TU) sees the handler installed.
    const ast::FuncDecl* panic_handler = nullptr;
    for (const auto& d : unit.decls) {
        if (d->kind != ast::NodeKind::Func) {
            continue;
        }
        const auto& f = static_cast<const ast::FuncDecl&>(*d);
        for (const auto& a : f.attributes) {
            if (a.name == "panic_handler") {
                panic_handler = &f;
                break;
            }
        }
        if (panic_handler != nullptr) {
            break;
        }
    }
    if (panic_handler != nullptr && no_libc_) {
        // §15.5 freestanding: bind the handler at link time (F5), so a panic
        // before (or without) static initialization still reaches it.
        src << "\nextern \"C\" void __vstr_panic_hook(const std::string_view& message, "
               "const std::string_view& file, const std::intptr_t& line) noexcept {\n";
        src << "    (void)" << panic_handler->name << "(message, file, line);\n";
        src << "}\n";
    } else if (panic_handler != nullptr) {
        src << "\n[[maybe_unused]] static const bool __vstr_register_panic_handler = []{\n";
        src << "    __vstr::panic_handler = &" << panic_handler->name << ";\n";
        src << "    return true;\n";
        src << "}();\n";
    }

    if (unit.module) {
        hdr << "\n}  // namespace ";
        write_module_path(hdr);
        hdr << "\n";
        src << "\n}  // namespace ";
        write_module_path(src);
        src << "\n";
    }

    // §12.3 derive(Hash): every `template <> struct std::hash<…>` lives
    // at global scope, so we emit those specializations AFTER the user's
    // namespace closes. We walk the decl list a second time here so the
    // emission order is predictable (declaration order, struct-only for
    // now — payloaded-enum Hash needs an extra std::visit-driven body).
    std::string qual_prefix;
    if (unit.module) {
        for (const auto& seg : unit.module->path) {
            qual_prefix += seg;
            qual_prefix += "::";
        }
    }
    for (const auto& d : unit.decls) {
        if (d->kind == ast::NodeKind::Struct) {
            const auto& sd = static_cast<const ast::StructDecl&>(*d);
            auto it = derives_by_target_.find(sd.name);
            if (it != derives_by_target_.end() && it->second.contains("Hash")) {
                hdr << "\n";
                emit_hash_spec(hdr, sd, qual_prefix);
            }
        } else if (d->kind == ast::NodeKind::Enum) {
            // Sum-type enums need an explicit std::hash spec because the
            // std::variant wrapper isn't hashable by default. Bare enums
            // are handled by the standard library's enum-class hash.
            const auto& ed = static_cast<const ast::EnumDecl&>(*d);
            auto it = derives_by_target_.find(ed.name);
            if (it != derives_by_target_.end() && it->second.contains("Hash")
                && enum_is_sum_type(ed)) {
                hdr << "\n";
                emit_hash_spec_enum(hdr, ed, qual_prefix);
            }
        } else if (d->kind == ast::NodeKind::Opaque) {
            // §3 opaque newtype: enum class `Q : T {}` has no implicit
            // std::hash specialization, so we synthesize one that
            // delegates to std::hash<T> over static_cast<T>(q).
            const auto& od = static_cast<const ast::OpaqueDecl&>(*d);
            auto it = derives_by_target_.find(od.name);
            if (it != derives_by_target_.end() && it->second.contains("Hash")) {
                hdr << "\n";
                emit_hash_spec_opaque(hdr, od, qual_prefix);
            }
        }
    }

    // §12.3 derive(Debug) + §4 derive(Display): both lower to a
    // std::formatter<T> specialization so `std::format("{}", v)` and
    // string interpolation work. For v0.5 the two produce the same
    // structural rendering — Display is meant to be more user-facing,
    // but until we have a customizable `display(into: Sink)` body
    // they share the Debug-flavoured output. Deriving both on the
    // same type emits a single spec (we'd otherwise hit a C++ ODR
    // violation on duplicate template specializations).
    for (const auto& d : unit.decls) {
        if (d->kind == ast::NodeKind::Struct) {
            const auto& sd = static_cast<const ast::StructDecl&>(*d);
            auto it = derives_by_target_.find(sd.name);
            if (it != derives_by_target_.end()
                && (it->second.contains("Debug") || it->second.contains("Display"))) {
                hdr << "\n";
                emit_debug_spec_struct(hdr, sd, qual_prefix);
            }
        } else if (d->kind == ast::NodeKind::Enum) {
            const auto& ed = static_cast<const ast::EnumDecl&>(*d);
            auto it = derives_by_target_.find(ed.name);
            if (it != derives_by_target_.end()
                && (it->second.contains("Debug") || it->second.contains("Display"))) {
                hdr << "\n";
                emit_debug_spec_enum(hdr, ed, qual_prefix);
            }
        } else if (d->kind == ast::NodeKind::Opaque) {
            // §3 opaque newtype Debug/Display: render `Q(value)` via the
            // underlying T's formatter.
            const auto& od = static_cast<const ast::OpaqueDecl&>(*d);
            auto it = derives_by_target_.find(od.name);
            if (it != derives_by_target_.end()
                && (it->second.contains("Debug") || it->second.contains("Display"))) {
                hdr << "\n";
                emit_debug_spec_opaque(hdr, od, qual_prefix);
            }
        }
    }

    return {hdr.str(), src.str()};
}

// ----------------------------------------------------------------------------

void CppEmitter::unsupported(std::ostream& os, std::string_view what, diag::SourceRange r) {
    reporter_->report(
        diag::Diagnostic::warning(std::format("codegen does not yet support: {}", what)).at(r));
    os << "/* unsupported: " << what << " */";
}

// A divisor that can't be zero or -1: a nonzero integer literal (a negated
// one is a UnaryExpr), or any float literal. `/` and `%` by one stay plain.
bool CppEmitter::divisor_is_safe(const ast::Expr& e) {
    if (e.kind == ast::NodeKind::FloatLit) {
        return true;
    }
    if (e.kind != ast::NodeKind::IntLit) {
        return false;
    }
    std::string_view t = static_cast<const ast::IntLit&>(e).text;
    if (t.size() > 2 && t[0] == '0' && (t[1] == 'x' || t[1] == 'b' || t[1] == 'o')) {
        t.remove_prefix(2);
    }
    for (char c : t) {
        if (c != '0' && c != '_') {
            return true;
        }
    }
    return false;
}

// An operand of `&+ &- &*`: an integer literal, or a negated one, has no
// Vestra type of its own (it takes the other side's), so it goes to the
// prelude helper wrapped as a `wlit`.
void CppEmitter::emit_wrap_operand(std::ostream& os, const ast::Expr& e) {
    const ast::Expr* lit = &e;
    if (e.kind == ast::NodeKind::UnaryExpr) {
        const auto& u = static_cast<const ast::UnaryExpr&>(e);
        if (u.op == ast::UnaryOp::Neg) {
            lit = u.operand.get();
        }
    }
    if (lit->kind != ast::NodeKind::IntLit) {
        emit_expr(os, e);
        return;
    }
    os << "__vstr::wlit(";
    emit_expr(os, e);
    os << ")";
}

const char* CppEmitter::binop_text(ast::BinaryOp op) {
    switch (op) {
    case ast::BinaryOp::Add:
        return "+";
    case ast::BinaryOp::Sub:
        return "-";
    case ast::BinaryOp::Mul:
        return "*";
    case ast::BinaryOp::Div:
        return "/";
    case ast::BinaryOp::Mod:
        return "%";
    case ast::BinaryOp::Shl:
        return "<<";
    case ast::BinaryOp::Shr:
        return ">>";
    case ast::BinaryOp::BitAnd:
        return "&";
    case ast::BinaryOp::BitOr:
        return "|";
    case ast::BinaryOp::BitXor:
        return "^";
    case ast::BinaryOp::Eq:
        return "==";
    case ast::BinaryOp::Ne:
        return "!=";
    case ast::BinaryOp::Lt:
        return "<";
    case ast::BinaryOp::Le:
        return "<=";
    case ast::BinaryOp::Gt:
        return ">";
    case ast::BinaryOp::Ge:
        return ">=";
    case ast::BinaryOp::And:
        return "&&";
    case ast::BinaryOp::Or:
        return "||";
    case ast::BinaryOp::WrappingAdd:
        return "+";  // C++ on unsigned wraps; signed UB — TODO
    case ast::BinaryOp::WrappingSub:
        return "-";
    case ast::BinaryOp::WrappingMul:
        return "*";
    default:
        return "/*?op*/";
    }
}

namespace {

// C++ binding strength of an emitted binary operator (higher binds tighter).
// Operators that never lower to infix C++ (`??` -> `.value_or`, ranges,
// `&+` -> `__vstr::wrap_add`) return 0, meaning "an atom": they never need parens as an operand.
int cpp_binop_prec(ast::BinaryOp op) {
    switch (op) {
    case ast::BinaryOp::Mul:
    case ast::BinaryOp::Div:
    case ast::BinaryOp::Mod:
        return 13;
    case ast::BinaryOp::Add:
    case ast::BinaryOp::Sub:
        return 12;
    case ast::BinaryOp::Shl:
    case ast::BinaryOp::Shr:
        return 11;
    case ast::BinaryOp::Lt:
    case ast::BinaryOp::Le:
    case ast::BinaryOp::Gt:
    case ast::BinaryOp::Ge:
        return 9;
    case ast::BinaryOp::Eq:
    case ast::BinaryOp::Ne:
        return 8;
    case ast::BinaryOp::BitAnd:
        return 7;
    case ast::BinaryOp::BitXor:
        return 6;
    case ast::BinaryOp::BitOr:
        return 5;
    case ast::BinaryOp::And:
        return 4;
    case ast::BinaryOp::Or:
        return 3;
    default:
        return 0;
    }
}

}  // namespace

bool CppEmitter::binop_operand_needs_parens(const ast::Expr& child, ast::BinaryOp parent_op,
                                            bool is_rhs) {
    if (child.kind != ast::NodeKind::BinaryExpr) {
        return false;
    }
    const int c = cpp_binop_prec(static_cast<const ast::BinaryExpr&>(child).op);
    const int p = cpp_binop_prec(parent_op);
    if (c == 0 || p == 0) {
        return false;
    }
    // C++ binary operators are left-associative: a right operand at the
    // same level would also regroup.
    return is_rhs ? c <= p : c < p;
}

const char* CppEmitter::unop_text(ast::UnaryOp op) {
    switch (op) {
    case ast::UnaryOp::Neg:
        return "-";
    case ast::UnaryOp::Not:
        return "!";
    case ast::UnaryOp::BitNot:
        return "~";
    case ast::UnaryOp::Unwrap:
        // Handled specially in emit_expr — never reaches the prefix path.
        return "";
    }
    return "?";
}

// ----------------------------------------------------------------------------

void CppEmitter::emit_decl(std::ostream& hdr, std::ostream& src, const ast::Decl& d) {
    switch (d.kind) {
    case ast::NodeKind::Func: {
        const auto& f = static_cast<const ast::FuncDecl&>(d);
        // §12.4 / §12.1: `comptime func`s are evaluated by the folder at
        // compile time only. They have no runtime form to emit (and their
        // bodies often use language constructs — Range, comptime stdlib
        // calls — that don't have a runtime lowering yet). Callers that
        // reach a comptime func via fold get the literal substituted; any
        // hypothetical runtime call sites would be the resolver's job to
        // reject.
        if (f.is_comptime) {
            break;
        }
        emit_func(hdr, src, f);
        break;
    }
    case ast::NodeKind::Struct:
        emit_struct(hdr, static_cast<const ast::StructDecl&>(d));
        break;
    case ast::NodeKind::Enum:
        emit_enum(hdr, static_cast<const ast::EnumDecl&>(d));
        break;
    case ast::NodeKind::Const: {
        const auto& c = static_cast<const ast::ConstDecl&>(d);
        hdr << "inline constexpr ";
        if (c.type) {
            emit_type(hdr, *c.type);
        } else {
            hdr << "auto";
        }
        hdr << " " << c.name << " = ";
        if (c.value) {
            global_init_ = true;
            emit_expr(hdr, *c.value);
            global_init_ = false;
        }
        hdr << ";\n";
        break;
    }
    case ast::NodeKind::Static: {
        // §A1 (§4.5, §6.7): top-level mutable static storage. `inline`
        // keeps the definition safe across multiple includes of the
        // generated header (each TU sees the same definition).
        // @noinit omits the initializer so the symbol lands in .bss
        // — C++ default-initializes a non-trivially-initialized
        // primitive / array to zero anyway, which matches the .bss
        // convention. The link attributes (@section / @weak /
        // @alias / @visibility) attach via gnu attribute syntax; the
        // @symbol custom name is emitted as an asm("name") trailer.
        const auto& s = static_cast<const ast::StaticDecl&>(d);
        const auto la = read_link_attrs(s.attributes);
        const bool vector_table = emit_vector_table(hdr, src, s);
        if (vector_table) {
            hdr << "#else\n";
        }
        emit_link_attr_prefix(hdr, la);
        if (la.align > 0) {
            hdr << "alignas(" << la.align << ") ";
        }
        hdr << "inline ";
        if (s.type) {
            emit_type(hdr, *s.type);
        } else {
            hdr << "auto";
        }
        hdr << " " << s.name;
        if (!la.symbol.empty()) {
            hdr << " asm(\"" << la.symbol << "\")";
        }
        if (!la.noinit && s.value) {
            hdr << " = ";
            global_init_ = true;
            emit_expr(hdr, *s.value);
            global_init_ = false;
        }
        hdr << ";\n";
        if (vector_table) {
            hdr << "#endif\n";
        }
        break;
    }
    case ast::NodeKind::Opaque: {
        const auto& o = static_cast<const ast::OpaqueDecl&>(d);
        hdr << "// opaque type " << o.name << "\n";
        hdr << "enum class " << o.name << " : ";
        emit_type(hdr, *o.underlying);
        hdr << " {};\n";
        break;
    }
    case ast::NodeKind::Derive:
        // §12.3: derive(...) is sugar for an empty extension that
        // adopts the protocol's reflective default. The actual code
        // gets emitted into the target struct/enum body via the
        // derives_by_target_ index built in emit(); there's nothing
        // to write at the DeriveDecl's own position.
        break;
    default:
        unsupported(hdr, std::format("top-level decl kind {}", static_cast<int>(d.kind)), d.range);
        hdr << "\n";
        break;
    }
}

void CppEmitter::emit_func(std::ostream& hdr, std::ostream& src, const ast::FuncDecl& f) {
    // §18 — the program's real C++ entry point: a module-less, parameterless
    // top-level `func main`. We emit it as `main(int argc, char** argv)` and
    // capture argv into the runtime so `args()` can read it; an in-module
    // `main` or one with params is an ordinary function.
    const bool is_entry_main =
        !unit_has_module_ && f.name == "main" && f.params.empty() && f.body != nullptr;
    // §9 propagation: conditional hoists need to write
    // `std::expected<T, E>` as their lambda's return type, where E is
    // this function's `throws(E)`. Stash + restore for nested funcs.
    const ast::Type* prev_throws = current_throws_type_;
    current_throws_type_ = f.effects.throws_type.get();
    struct ThrowsRestore {
        CppEmitter* self;
        const ast::Type* prev;
        ~ThrowsRestore() { self->current_throws_type_ = prev; }
    } _throws_restore{this, prev_throws};

    // §14.12.3 — `@no_auto_barrier` on the enclosing function
    // suppresses the auto-emitted post-write barrier on gated
    // sysreg writes inside its body. Tracked as a per-function
    // flag so emit_expr's `Sysreg.X.write(v)` intercept can
    // decide between the barrier-bearing wrapper and the raw
    // write.
    const bool prev_no_auto_barrier = current_no_auto_barrier_;
    current_no_auto_barrier_ =
        std::any_of(f.attributes.begin(), f.attributes.end(), [](const ast::Attribute& a) {
            return a.name == "no_auto_barrier";
        });
    struct NoAutoBarrierRestore {
        CppEmitter* self;
        bool prev;
        ~NoAutoBarrierRestore() { self->current_no_auto_barrier_ = prev; }
    } _no_auto_barrier_restore{this, prev_no_auto_barrier};

    // §11 async: a `return` in this body must lower to `co_return`.
    const bool prev_is_async = current_func_is_async_;
    current_func_is_async_ = f.is_async;
    struct AsyncRestore {
        CppEmitter* self;
        bool prev;
        ~AsyncRestore() { self->current_func_is_async_ = prev; }
    } _async_restore{this, prev_is_async};

    // §9 Unit-returning throws: see current_func_unit_throws_.
    const bool prev_unit_throws = current_func_unit_throws_;
    current_func_unit_throws_ = f.effects.throws_type != nullptr && is_unit_result(f);
    struct UnitThrowsRestore {
        CppEmitter* self;
        bool prev;
        ~UnitThrowsRestore() { self->current_func_unit_throws_ = prev; }
    } _unit_throws_restore{this, prev_unit_throws};

    // §7 generics: a Vestra generic function lowers to a C++ template. The
    // host compiler then monomorphizes per instantiation, which is what
    // Vestra semantically requires anyway. A const generic ([const N: Int])
    // becomes a `std::size_t N` non-type parameter (the natural fit for the
    // `[N]T` array lengths it indexes), inferred at the call site from a
    // `[N]T` argument's length.
    const bool has_type_generics =
        std::any_of(f.generics.begin(), f.generics.end(), [](const ast::GenericParam& g) {
            return !g.name.empty();
        });
    auto emit_template_prefix = [&](std::ostream& os) {
        if (!has_type_generics) {
            return;
        }
        os << "template <";
        bool first = true;
        for (const auto& g : f.generics) {
            if (g.name.empty()) {
                continue;
            }
            if (!first) {
                os << ", ";
            }
            first = false;
            os << (g.is_const ? "std::size_t " : "class ") << g.name;
        }
        os << ">\n";
        // §7 generics — map each protocol bound that has a clean C++20
        // concept onto a `requires` conjunct. The Vestra call site already
        // enforces conformance, so this is defense-in-depth + documentation;
        // protocols without a standard concept (Hash, Debug, Display, Clone,
        // user-defined) are omitted from the clause.
        std::vector<std::string> constraints;
        for (const auto& g : f.generics) {
            if (g.is_const || g.name.empty()) {
                continue;
            }
            for (const auto& proto : g.bound.protocols) {
                if (proto == nullptr || proto->kind != ast::NodeKind::NamedType) {
                    continue;
                }
                const auto& nt = static_cast<const ast::NamedType&>(*proto);
                if (nt.path.empty()) {
                    continue;
                }
                const std::string& p = nt.path.back();
                std::string concept_name;
                if (p == "Eq") {
                    concept_name = "std::equality_comparable";
                } else if (p == "Comparable" || p == "Ordered") {
                    concept_name = "std::totally_ordered";
                } else if (p == "Default") {
                    concept_name = "std::default_initializable";
                }
                if (!concept_name.empty()) {
                    constraints.push_back(concept_name + "<" + g.name + ">");
                }
            }
        }
        if (!constraints.empty()) {
            os << "requires (";
            for (std::size_t i = 0; i < constraints.size(); ++i) {
                if (i != 0) {
                    os << " && ";
                }
                os << constraints[i];
            }
            os << ")\n";
        }
    };

    // §6 tuple-pattern param: a param whose AST stores a TuplePat (no
    // `name`) gets a synthetic C++ identifier — the structured binding
    // happens via prologue statements at the top of the body. The same
    // name is used for the signature in header + source so the
    // forward-declared decl and the definition agree.
    auto param_cpp_name = [&](std::size_t i, const ast::Param& p) -> std::string {
        if (p.pattern != nullptr) {
            return std::format("__vstr_arg{}", i);
        }
        return p.name;
    };

    // §A1 (§6.7) link-attribute prefix for the function. The
    // attributes attach to every declaration of the symbol — both
    // the header forward-declare and the source definition — so the
    // linker sees a consistent set.
    const auto link_attrs = read_link_attrs(f.attributes);

    // §15 Tier C: an `@extern(conv)` function, declared or defined, follows
    // the foreign ABI, where a scalar argument travels in a register rather
    // than by address. Its `read` parameters therefore pass by value (F8).
    const bool is_foreign_abi = std::ranges::any_of(
        f.attributes, [](const ast::Attribute& a) { return a.name == "extern"; });

    // `is_decl` controls the asm-label trailer: GCC / Clang allow
    // asm("name") on a forward declaration but reject it on a
    // function *definition* (header + body). The header gets the
    // trailer; the source definition omits it.
    auto emit_signature = [&](std::ostream& os, bool is_decl) {
        // §9: a `throws(E)` clause wraps the user-visible result in
        // `std::expected<T, E>` so callers see the fallible type. Inside
        // the body, `return x` works via the converting ctor; `throw e`
        // and `try f()` are lowered to the matching std::expected ops.
        // §11 an `async func` lowers to a C++20 coroutine returning
        // `__vstr::Task<R>` (std::execution senders aren't in the host
        // libc++; coroutines are). The `await` sites become `co_await` and
        // the returns become `co_return`. An `async func` that also
        // `throws(E)` nests as `__vstr::Task<std::expected<T, E>>`; there
        // `throw` / propagating `try` `co_return std::unexpected{…}` (a plain
        // return is ill-formed in a coroutine — see cpp_emitter_stmts.cpp).
        if (f.is_async) {
            os << "__vstr::Task<";
        }
        if (f.effects.throws_type) {
            os << "std::expected<";
            if (f.result) {
                emit_type(os, *f.result);
            } else {
                os << "void";
            }
            os << ", ";
            emit_type(os, *f.effects.throws_type);
            os << ">";
        } else if (f.result && f.result->kind == ast::NodeKind::FunctionType && !f.is_async) {
            // §A12 a function returning a function pointer: C++ can only spell
            // that as a trailing return type (`auto f() -> R(*)(A)`).
            os << "auto";
        } else if (f.result) {
            emit_type(os, *f.result);
        } else {
            os << "void";
        }
        if (f.is_async) {
            os << ">";
        }
        os << " " << f.name << "(";
        if (is_entry_main) {
            os << "int argc, char** argv";  // captured into __vstr for args()
        }
        for (std::size_t i = 0; i < f.params.size(); ++i) {
            if (i != 0) {
                os << ", ";
            }
            const auto& p = f.params[i];
            // Mode mapping (very rough first pass — §5 ownership is unresolved):
            //   read  → const T&
            //   inout → T&
            //   sink  → T&&
            // §A12 (§14.6.3) follow-up: function-pointer types pass
            // by-value. C++ syntax for "ref to function-pointer" is
            // `R(*const &)(T1)`, which disagrees with the way
            // emit_type builds the spelling; function pointers are
            // word-sized so by-value is the natural lowering anyway.
            // The declarator-name-embedding `emit_type_with_name`
            // also handles function pointers properly when the type
            // is used as a parameter (`R(*name)(T1)`).
            const bool is_fn_ptr = p.type != nullptr && p.type->kind == ast::NodeKind::FunctionType;
            // §A3 (§10.5) raw pointers are word-sized Trivial values;
            // passing them by `const T&` is the canonical Vestra
            // read-mode lowering, but the C++ spelling `const Node*&`
            // would mean "reference to pointer-to-const-Node" — the
            // const binds to the pointee, not the pointer. The right
            // fix is to pass pointer types by value, since they have
            // a single-word bit-pattern and can't be reassigned in
            // C++ from a read-mode caller's perspective either way.
            const bool is_raw_ptr =
                p.type != nullptr
                && (p.type->kind == ast::NodeKind::NamedType
                    && !static_cast<const ast::NamedType&>(*p.type).path.empty()
                    && (static_cast<const ast::NamedType&>(*p.type).path[0] == "Ptr"
                        || static_cast<const ast::NamedType&>(*p.type).path[0] == "MutPtr"));
            const std::string pname{param_cpp_name(i, p)};
            switch (p.mode) {
            case ast::ParamMode::Read:
                if (is_fn_ptr) {
                    if (p.type) {
                        emit_type_with_name(os, *p.type, pname);
                    }
                } else if (is_raw_ptr) {
                    if (p.type) {
                        emit_type(os, *p.type);
                    }
                    os << " " << pname;
                } else if (f.is_async || is_foreign_abi) {
                    // §15 Tier C (`is_foreign_abi`): see above.
                    // §11 — a coroutine does NOT copy by-reference parameters
                    // into its frame, so a `const T&` read param bound to a
                    // caller temporary (a literal arg, `a + b`, …) dangles
                    // once the coroutine suspends and the caller's
                    // full-expression ends. Emit read params of an `async
                    // func` by value so the frame owns a copy that survives
                    // suspension. (This is the codegen half of "no borrow
                    // across await": references can't cross a suspension.)
                    if (p.type) {
                        emit_type(os, *p.type);
                    }
                    os << " " << pname;
                } else {
                    os << "const ";
                    if (p.type) {
                        emit_type(os, *p.type);
                    }
                    os << "& " << pname;
                }
                break;
            case ast::ParamMode::Inout:
                if (p.type) {
                    emit_type(os, *p.type);
                }
                os << "& " << pname;
                break;
            case ast::ParamMode::Sink:
                if (p.type) {
                    emit_type(os, *p.type);
                }
                os << "&& " << pname;
                break;
            }
        }
        os << ")";
        if (f.result && f.result->kind == ast::NodeKind::FunctionType && !f.is_async
            && !f.effects.throws_type) {
            os << " -> ";
            emit_type(os, *f.result);
        }
        // §A1 (§6.7) @symbol: GCC / Clang accept the asm-label
        // trailer on a function *declaration* — after the closing
        // paren, before the `;`. A function *definition* (signature
        // followed by `{`) doesn't allow it, so we emit it only when
        // is_decl is true. The forward declaration in the header
        // carries the rename; the source definition still binds at
        // link time via the declared name.
        if (is_decl && !link_attrs.symbol.empty()) {
            os << " asm(\"" << link_attrs.symbol << "\")";
        }
    };

    // §6 prologue for tuple-pattern params: each synthetic arg gets a
    // structured-binding statement at the top of the body, plus any
    // nested follow-on unpacks via the same helpers LetStmt uses.
    auto emit_tuple_param_prologue = [&](std::ostream& os) {
        for (std::size_t i = 0; i < f.params.size(); ++i) {
            const auto& p = f.params[i];
            if (p.pattern == nullptr || p.pattern->kind != ast::NodeKind::TuplePat) {
                continue;
            }
            const auto& tp = static_cast<const ast::TuplePat&>(*p.pattern);
            std::vector<std::string> names;
            std::vector<std::pair<std::string, const ast::TuplePat*>> followons;
            collect_tuple_pat_names(tp, names, followons);
            write_indent(os, 1);
            os << "auto [";
            for (std::size_t j = 0; j < names.size(); ++j) {
                if (j != 0) {
                    os << ", ";
                }
                os << names[j];
            }
            os << "] = " << param_cpp_name(i, p) << ";\n";
            emit_tuple_pat_followons(os, followons, 1);
        }
    };

    // `[[nodiscard]]` is invalid on void-returning functions — emit it
    // only when there's a real result type or a `throws(E)` wrapper.
    const bool has_result = f.result != nullptr || f.effects.throws_type != nullptr;
    // §4 a `-> Never` function never returns: `[[noreturn]]` tells the C++
    // compiler so, which keeps a caller that ends in a Never call from
    // "falling off the end" (F9). `[[nodiscard]]` is dropped for it, since
    // a Never call is almost always a statement.
    const bool never_returns = f.effects.throws_type == nullptr && is_never_result(f);
    const char* result_attr = never_returns ? "[[noreturn]] " : "[[nodiscard]] ";

    // Templates must be visible at every instantiation site, so a generic
    // function lives entirely in the header. Non-generics keep the
    // declaration/definition split.
    if (has_type_generics) {
        emit_template_prefix(hdr);
        emit_link_attr_prefix(hdr, link_attrs);
        if (has_result) {
            hdr << result_attr;
        }
        emit_inline_specifier(hdr, link_attrs);
        // Templates emit body inline in the header, so this is both
        // declaration *and* definition — the asm-label can't ride
        // along here, so is_decl is false. If the user combines
        // @symbol with a generic function, the rename silently does
        // nothing in v0.5; that's an acceptable v0.5 limitation
        // since the kernel-tagged symbols are almost always concrete.
        emit_signature(hdr, /*is_decl=*/false);
        if (!f.body) {
            hdr << ";\n";
            return;
        }
        hdr << " {\n";
        emit_tuple_param_prologue(hdr);
        if (f.body->kind == ast::NodeKind::BlockExpr) {
            const auto& blk = static_cast<const ast::BlockExpr&>(*f.body);
            for (const auto& s : blk.stmts) {
                emit_stmt(hdr, *s, 1);
            }
            emit_async_void_coreturn(hdr, f, blk);
            emit_unit_throws_return(hdr, f, blk);
        } else {
            unsupported(hdr, "non-block function body", f.body->range);
        }
        hdr << "}\n\n";
        return;
    }

    // §14.6 an `@extern` function has C linkage (F21): without a `@symbol`
    // it then links against its plain name, not a mangled (or namespaced)
    // C++ one. A Vestra definition inherits it from this declaration.
    if (is_foreign_abi) {
        hdr << "extern \"C\" ";
    }
    emit_link_attr_prefix(hdr, link_attrs);
    if (has_result) {
        hdr << result_attr;
    }
    emit_inline_specifier(hdr, link_attrs);
    emit_signature(hdr, /*is_decl=*/true);
    hdr << ";\n";

    if (!f.body) {
        return;
    }

    emit_link_attr_prefix(src, link_attrs);
    emit_inline_specifier(src, link_attrs);
    emit_signature(src, /*is_decl=*/false);
    src << " {\n";
    if (is_entry_main) {
        src << "    __vstr::args_init(argc, argv);\n";
    }
    emit_tuple_param_prologue(src);
    if (f.body->kind == ast::NodeKind::BlockExpr) {
        const auto& blk = static_cast<const ast::BlockExpr&>(*f.body);
        for (const auto& s : blk.stmts) {
            emit_stmt(src, *s, 1);
        }
        emit_async_void_coreturn(src, f, blk);
        emit_unit_throws_return(src, f, blk);
    } else {
        unsupported(src, "non-block function body", f.body->range);
    }
    src << "}\n\n";
}

// §11 — a `void`-returning async function must still contain a coroutine
// keyword to be a C++20 coroutine. If its body doesn't already end in a
// `return` (which lowers to `co_return`), append a trailing `co_return;`.
void CppEmitter::emit_async_void_coreturn(std::ostream& os,
                                          const ast::FuncDecl& f,
                                          const ast::BlockExpr& blk) {
    // A throws fn returns Task<std::expected<void, E>>: emit_unit_throws_return
    // supplies its `co_return {};` instead.
    if (!f.is_async || f.result != nullptr || f.effects.throws_type != nullptr) {
        return;
    }
    const bool ends_in_return =
        !blk.stmts.empty() && blk.stmts.back()->kind == ast::NodeKind::ReturnStmt;
    if (!ends_in_return) {
        write_indent(os, 1);
        os << "co_return;\n";
    }
}

// §9 — a Unit-returning `throws(E)` function returns `std::expected<void, E>`,
// so falling off the end of its body is undefined behaviour (F9b). Unless the
// body already ends in a `return`, append the success value.
namespace {

// Size and alignment of a trap-frame field type the vector code can address:
// a primitive integer, or a fixed array of them. 0 when unsupported.
std::pair<std::int64_t, std::int64_t> frame_field_layout(const ast::Type* t) {
    if (t == nullptr) {
        return {0, 0};
    }
    if (t->kind == ast::NodeKind::VectorType) {
        const auto& v = static_cast<const ast::VectorType&>(*t);
        auto [sz, al] = frame_field_layout(v.element.get());
        return {sz * v.length, al};
    }
    if (t->kind != ast::NodeKind::NamedType) {
        return {0, 0};
    }
    const auto& n = static_cast<const ast::NamedType&>(*t);
    if (n.path.size() != 1) {
        return {0, 0};
    }
    const auto& p = n.path[0];
    if (p == "UInt64" || p == "Int64" || p == "UInt" || p == "Int") {
        return {8, 8};
    }
    if (p == "UInt32" || p == "Int32") {
        return {4, 4};
    }
    if (p == "UInt16" || p == "Int16") {
        return {2, 2};
    }
    if (p == "UInt8" || p == "Int8" || p == "Bool") {
        return {1, 1};
    }
    return {0, 0};
}

}  // namespace

bool CppEmitter::emit_vector_table(std::ostream& hdr, std::ostream& src, const ast::StaticDecl& s) {
    // Only the freestanding profile emits code: hosted, the table stays an
    // array of handler pointers (what a host test inspects).
    if (!no_libc_ || s.type == nullptr || s.type->kind != ast::NodeKind::VectorType) {
        return false;
    }
    const auto& vt = static_cast<const ast::VectorType&>(*s.type);
    if (vt.element == nullptr || vt.element->kind != ast::NodeKind::InterruptType) {
        return false;
    }
    const auto& it = static_cast<const ast::InterruptType&>(*vt.element);
    // The frame type and its canonical fields.
    const ast::StructDecl* frame = nullptr;
    if (resolution_ != nullptr && s.value != nullptr && it.trap_frame != nullptr) {
        // Recover the struct from any handler's first parameter type.
        if (auto st = resolution_->type_of(s.value.get());
            st != nullptr && st->inner() != nullptr && st->inner()->kind() == sema::TypeKind::InterruptHandler
            && st->inner()->inner() != nullptr && st->inner()->inner()->nominal_decl() != nullptr
            && st->inner()->inner()->nominal_decl()->kind == ast::NodeKind::Struct) {
            frame = static_cast<const ast::StructDecl*>(st->inner()->inner()->nominal_decl());
        }
    }
    if (frame == nullptr || s.value == nullptr || s.value->kind != ast::NodeKind::VectorLitExpr) {
        reporter_->report(
            diag::Diagnostic::error("a compiler-emitted vector table needs an array literal of "
                                    "@interrupt handlers over a struct trap frame")
                .at(s.range));
        return false;
    }
    // Layout (C rules over the field types), then the canonical fields.
    std::unordered_map<std::string, std::int64_t> off;
    std::vector<std::pair<std::string, std::int64_t>> off_in_order;  // declaration order
    std::int64_t at = 0;
    std::int64_t max_al = 1;
    std::int64_t x_count = 0;
    for (const auto& f : frame->fields) {
        auto [sz, al] = frame_field_layout(f.type.get());
        if (sz == 0) {
            reporter_->report(
                diag::Diagnostic::error(std::format(
                    "trap frame field '{}' needs a fixed-size integer type for the compiler-emitted "
                    "vector table to address it",
                    f.name))
                    .at(f.range));
            return false;
        }
        at = (at + al - 1) / al * al;
        off[f.name] = at;
        off_in_order.emplace_back(f.name, at);
        if (f.name == "x" && f.type->kind == ast::NodeKind::VectorType) {
            x_count = static_cast<const ast::VectorType&>(*f.type).length;
        }
        at += sz;
        max_al = std::max(max_al, al);
    }
    const std::int64_t frame_size = (at + max_al - 1) / max_al * max_al;
    std::vector<std::string> missing;
    if (x_count != 31) {
        missing.emplace_back("x: [31]UInt64");
    }
    for (const char* n : {"sp", "elr", "spsr", "esr", "far"}) {
        if (!off.contains(n)) {
            missing.emplace_back(std::string{n} + ": UInt64");
        }
    }
    if (!missing.empty()) {
        std::string list;
        for (const auto& m : missing) {
            list += (list.empty() ? "" : ", ") + m;
        }
        reporter_->report(diag::Diagnostic::error(
                              std::format("trap frame '{}' lacks the fields the compiler-emitted "
                                          "vector table saves into: {}",
                                          frame->name,
                                          list))
                              .at(frame->range));
        return false;
    }
    const auto& elems = static_cast<const ast::VectorLitExpr&>(*s.value).elements;
    const std::int64_t stack = (frame_size + 15) / 16 * 16;
    const std::int64_t X = off["x"];
    if (X + 30 * 8 > 504 - 8) {
        reporter_->report(diag::Diagnostic::error(
                              "trap frame field 'x' sits too far into the frame for the vector "
                              "code's paired stores; put it first")
                              .at(frame->range));
        return false;
    }
    const auto la = read_link_attrs(s.attributes);
    const std::string section = la.section.empty() ? ".text.vectors" : la.section;
    const std::int64_t align = la.align > 0 ? la.align : 2048;
    const std::string label = "__vstr_vt_" + module_label_ + s.name;
    const std::string handlers = label + "_handlers";
    std::ostringstream fty;
    emit_type(fty, *s.type);
    const std::string frame_name = nominal_prefix(frame) + frame->name;

    // Header: on aarch64 the static names the code.
    hdr << "#if defined(__aarch64__)\n";
    hdr << "extern " << fty.str() << " " << s.name << " asm(\"" << label << "\");\n";

    // Source: the handler pointers (read by the shared path), the layout the
    // asm assumes, and the table code.
    src << "#if defined(__aarch64__)\n";
    src << "static_assert(sizeof(" << frame_name << ") == " << frame_size
        << ", \"trap frame layout\");\n";
    // Field declaration order, so the output doesn't depend on hashing.
    for (const auto& [n, o] : off_in_order) {
        src << "static_assert(offsetof(" << frame_name << ", " << n << ") == " << o
            << ", \"trap frame layout\");\n";
    }
    src << "[[gnu::used]] void (*" << handlers << "[" << elems.size() << "])(" << frame_name
        << "&) asm(\"" << handlers << "\") = {";
    for (std::size_t i = 0; i < elems.size(); ++i) {
        src << (i != 0 ? ", " : "");
        emit_expr(src, *elems[i]);
    }
    src << "};\n";
    auto line = [&](const std::string& l) { src << "    \"" << l << "\\n\"\n"; };
    src << "__asm__(\n";
    line(".pushsection " + section + ", \\\"ax\\\"");
    line(".balign " + std::to_string(align));
    line(".global " + label);
    line(label + ":");
    for (std::size_t i = 0; i < elems.size(); ++i) {
        line(".balign 128");
        line("sub sp, sp, #" + std::to_string(stack));
        line("stp x0, x1, [sp, #" + std::to_string(X) + "]");
        line("mov x0, #" + std::to_string(i));
        line("b " + label + "_common");
    }
    line(label + "_common:");
    for (int r = 2; r < 30; r += 2) {
        line("stp x" + std::to_string(r) + ", x" + std::to_string(r + 1) + ", [sp, #"
             + std::to_string(X + r * 8) + "]");
    }
    line("str x30, [sp, #" + std::to_string(X + 240) + "]");
    const std::pair<const char*, const char*> regs[] = {
        {"sp", "sp_el0"}, {"elr", "elr_el1"}, {"spsr", "spsr_el1"}, {"esr", "esr_el1"}, {"far", "far_el1"}};
    for (const auto& [field, reg] : regs) {
        line(std::string{"mrs x2, "} + reg);
        line("str x2, [sp, #" + std::to_string(off[field]) + "]");
    }
    if (off.contains("vector")) {
        line("str x0, [sp, #" + std::to_string(off["vector"]) + "]");
    }
    line("adrp x1, " + handlers);
    line("add x1, x1, :lo12:" + handlers);
    line("ldr x1, [x1, x0, lsl #3]");
    line("mov x0, sp");
    line("blr x1");
    for (const auto& [field, reg] : {std::pair{"sp", "sp_el0"}, std::pair{"elr", "elr_el1"},
                                     std::pair{"spsr", "spsr_el1"}}) {
        line("ldr x2, [sp, #" + std::to_string(off[field]) + "]");
        line(std::string{"msr "} + reg + ", x2");
    }
    for (int r = 2; r < 30; r += 2) {
        line("ldp x" + std::to_string(r) + ", x" + std::to_string(r + 1) + ", [sp, #"
             + std::to_string(X + r * 8) + "]");
    }
    line("ldr x30, [sp, #" + std::to_string(X + 240) + "]");
    line("ldp x0, x1, [sp, #" + std::to_string(X) + "]");
    line("add sp, sp, #" + std::to_string(stack));
    line("eret");
    line(".popsection");
    src << ");\n";
    src << "#endif\n\n";
    return true;
}

void CppEmitter::emit_unit_throws_return(std::ostream& os,
                                         const ast::FuncDecl& f,
                                         const ast::BlockExpr& blk) {
    if (f.effects.throws_type == nullptr || !is_unit_result(f)) {
        return;
    }
    const bool ends_in_return =
        !blk.stmts.empty() && blk.stmts.back()->kind == ast::NodeKind::ReturnStmt;
    if (!ends_in_return) {
        write_indent(os, 1);
        os << (f.is_async ? "co_return {};\n" : "return {};\n");
    }
}

bool CppEmitter::is_unit_typed(const ast::Expr& e) const {
    if (resolution_ == nullptr) {
        return false;
    }
    auto t = resolution_->type_of(&e);
    if (t == nullptr) {
        return false;
    }
    // A `try` types as its success value; the throws call it wraps types as
    // Result[T, E], so look through that to T.
    if (t->kind() == sema::TypeKind::Result) {
        t = t->inner();
    }
    return t != nullptr && t->kind() == sema::TypeKind::Unit;
}

const ast::FuncDecl* CppEmitter::callee_decl(const ast::CallExpr& c) const {
    if (resolution_ == nullptr) {
        return nullptr;
    }
    const auto* sym = resolution_->symbol_of(c.callee.get());
    if (sym != nullptr && sym->decl != nullptr && sym->decl->kind == ast::NodeKind::Func) {
        return static_cast<const ast::FuncDecl*>(sym->decl);
    }
    if (c.callee->kind != ast::NodeKind::MemberExpr) {
        return nullptr;
    }
    const auto& m = static_cast<const ast::MemberExpr&>(*c.callee);
    auto bt = resolution_->type_of(m.base.get());
    if (bt == nullptr || bt->kind() != sema::TypeKind::Struct || bt->nominal_decl() == nullptr
        || bt->nominal_decl()->kind != ast::NodeKind::Struct) {
        return nullptr;
    }
    for (const auto& md : static_cast<const ast::StructDecl&>(*bt->nominal_decl()).methods) {
        if (md->kind == ast::NodeKind::Func
            && static_cast<const ast::FuncDecl&>(*md).name == m.member) {
            return static_cast<const ast::FuncDecl*>(md.get());
        }
    }
    return nullptr;
}

namespace {

// Does any identifier under `n` resolve to a local or parameter defined
// outside [lo, hi) (the closure's own source)?
bool refers_outside(const ast::Node& n, const sema::Resolution& r, const diag::SourceRange& span);

bool refers_outside_expr(const ast::Expr* e, const sema::Resolution& r, const diag::SourceRange& span) {
    return e != nullptr && refers_outside(*e, r, span);
}

bool refers_outside(const ast::Node& n, const sema::Resolution& r, const diag::SourceRange& span) {
    auto x = [&](const ast::Expr* e) { return refers_outside_expr(e, r, span); };
    switch (n.kind) {
    case ast::NodeKind::IdentExpr: {
        const auto* sym = r.symbol_of(static_cast<const ast::Expr*>(&n));
        if (sym == nullptr
            || (sym->kind != sema::SymbolKind::Local && sym->kind != sema::SymbolKind::Parameter)) {
            return false;
        }
        const auto d = sym->definition_range.begin;
        return d.file != span.begin.file || d.offset < span.begin.offset
               || d.offset >= span.begin.offset + span.length;
    }
    case ast::NodeKind::SelfExpr:
        return true;
    case ast::NodeKind::ParenExpr:
        return x(static_cast<const ast::ParenExpr&>(n).inner.get());
    case ast::NodeKind::UnaryExpr:
        return x(static_cast<const ast::UnaryExpr&>(n).operand.get());
    case ast::NodeKind::BinaryExpr: {
        const auto& b = static_cast<const ast::BinaryExpr&>(n);
        return x(b.lhs.get()) || x(b.rhs.get());
    }
    case ast::NodeKind::MemberExpr:
        return x(static_cast<const ast::MemberExpr&>(n).base.get());
    case ast::NodeKind::IndexExpr: {
        const auto& ix = static_cast<const ast::IndexExpr&>(n);
        if (x(ix.base.get())) {
            return true;
        }
        for (const auto& i : ix.indices) {
            if (x(i.get())) {
                return true;
            }
        }
        return false;
    }
    case ast::NodeKind::CallExpr: {
        const auto& c = static_cast<const ast::CallExpr&>(n);
        if (x(c.callee.get())) {
            return true;
        }
        for (const auto& a : c.args) {
            if (x(a.value.get())) {
                return true;
            }
        }
        return false;
    }
    case ast::NodeKind::IfExpr: {
        const auto& i = static_cast<const ast::IfExpr&>(n);
        return x(i.cond.get()) || x(i.let_init.get()) || x(i.then_branch.get())
               || x(i.else_branch.get());
    }
    case ast::NodeKind::BlockExpr:
        for (const auto& s : static_cast<const ast::BlockExpr&>(n).stmts) {
            if (refers_outside(*s, r, span)) {
                return true;
            }
        }
        return false;
    case ast::NodeKind::ExprStmt:
        return x(static_cast<const ast::ExprStmt&>(n).expr.get());
    case ast::NodeKind::LetStmt:
        return x(static_cast<const ast::LetStmt&>(n).value.get());
    case ast::NodeKind::VarStmt:
        return x(static_cast<const ast::VarStmt&>(n).value.get());
    case ast::NodeKind::ReturnStmt:
        return x(static_cast<const ast::ReturnStmt&>(n).value.get());
    case ast::NodeKind::AssignStmt: {
        const auto& a = static_cast<const ast::AssignStmt&>(n);
        return x(a.target.get()) || x(a.value.get());
    }
    case ast::NodeKind::WhileStmt: {
        const auto& w = static_cast<const ast::WhileStmt&>(n);
        return x(w.cond.get()) || x(w.body.get());
    }
    case ast::NodeKind::ForStmt: {
        const auto& f = static_cast<const ast::ForStmt&>(n);
        return x(f.iter.get()) || x(f.body.get());
    }
    case ast::NodeKind::MatchExpr: {
        const auto& m = static_cast<const ast::MatchExpr&>(n);
        if (x(m.scrutinee.get())) {
            return true;
        }
        for (const auto& arm : m.arms) {
            if (x(arm.guard.get()) || x(arm.body.get())) {
                return true;
            }
        }
        return false;
    }
    case ast::NodeKind::ClosureExpr:
        for (const auto& s : static_cast<const ast::ClosureExpr&>(n).body) {
            if (refers_outside(*s, r, span)) {
                return true;
            }
        }
        return false;
    default:
        return false;
    }
}

}  // namespace

bool CppEmitter::closure_captures(const ast::ClosureExpr& cx) const {
    if (resolution_ == nullptr) {
        return false;
    }
    for (const auto& s : cx.body) {
        if (refers_outside(*s, *resolution_, cx.range)) {
            return true;
        }
    }
    return false;
}

bool CppEmitter::if_let_borrows(const ast::IfExpr& i) const {
    if (resolution_ == nullptr || i.let_init == nullptr) {
        return false;
    }
    const ast::Expr* e = i.let_init.get();
    while (e->kind == ast::NodeKind::ParenExpr) {
        e = static_cast<const ast::ParenExpr&>(*e).inner.get();
    }
    bool place = e->kind == ast::NodeKind::MemberExpr || e->kind == ast::NodeKind::IndexExpr;
    if (e->kind == ast::NodeKind::IdentExpr) {
        const auto* sym = resolution_->symbol_of(e);
        place = sym != nullptr
                && (sym->kind == sema::SymbolKind::Static || sym->kind == sema::SymbolKind::Parameter);
    }
    if (!place) {
        return false;
    }
    sema::Linearity lin(resolution_->types());
    return lin.is_linear(resolution_->type_of(e));
}

bool CppEmitter::is_never_result(const ast::FuncDecl& f) {
    if (f.result == nullptr || f.result->kind != ast::NodeKind::NamedType) {
        return false;
    }
    const auto& nt = static_cast<const ast::NamedType&>(*f.result);
    return nt.path.size() == 1 && nt.path[0] == "Never" && !nt.has_generics;
}

bool CppEmitter::is_unit_result(const ast::FuncDecl& f) {
    if (f.result == nullptr) {
        return true;
    }
    if (f.result->kind != ast::NodeKind::NamedType) {
        return false;
    }
    const auto& nt = static_cast<const ast::NamedType&>(*f.result);
    return nt.path.size() == 1 && nt.path[0] == "Unit" && !nt.has_generics;
}

// §6 a struct with a `deinit`: RAII in C++, guarded by a live flag that a move
// clears, since a moved-from C++ object is still destroyed. Copies are
// deleted (a deinit type moves only as a whole). The user-declared special
// members make it a non-aggregate, so it gets a positional constructor over
// its fields in order, which construction calls.
void CppEmitter::emit_deinit_members(std::ostream& hdr, const ast::StructDecl& s) {
    const std::string& n = s.name;
    std::vector<const ast::StructDecl::Field*> fields;
    for (const auto& f : s.fields) {
        if (f.type != nullptr) {
            fields.push_back(&f);
        }
    }
    write_indent(hdr, 1);
    hdr << "bool __vstr_live = true;\n";
    write_indent(hdr, 1);
    hdr << n << "() = default;\n";
    if (!fields.empty()) {
        write_indent(hdr, 1);
        hdr << n << "(";
        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (i != 0) {
                hdr << ", ";
            }
            emit_type_with_name(hdr, *fields[i]->type, "__vstr_a" + std::to_string(i));
        }
        hdr << ") : ";
        for (std::size_t i = 0; i < fields.size(); ++i) {
            if (i != 0) {
                hdr << ", ";
            }
            hdr << fields[i]->name << "(std::move(__vstr_a" << i << "))";
        }
        hdr << " {}\n";
    }
    write_indent(hdr, 1);
    hdr << n << "(const " << n << "&) = delete;\n";
    write_indent(hdr, 1);
    hdr << n << "& operator=(const " << n << "&) = delete;\n";
    write_indent(hdr, 1);
    hdr << n << "(" << n << "&& __vstr_o) noexcept : ";
    for (const auto* f : fields) {
        hdr << f->name << "(std::move(__vstr_o." << f->name << ")), ";
    }
    hdr << "__vstr_live(__vstr_o.__vstr_live) { __vstr_o.__vstr_live = false; }\n";
    // Assigning over a live value ends it first, as a scope exit would.
    write_indent(hdr, 1);
    hdr << n << "& operator=(" << n << "&& __vstr_o) noexcept {\n";
    write_indent(hdr, 2);
    hdr << "if (this == &__vstr_o) { return *this; }\n";
    write_indent(hdr, 2);
    hdr << "if (__vstr_live) { __vstr_deinit(); }\n";
    for (const auto* f : fields) {
        write_indent(hdr, 2);
        hdr << f->name << " = std::move(__vstr_o." << f->name << ");\n";
    }
    write_indent(hdr, 2);
    hdr << "__vstr_live = __vstr_o.__vstr_live;\n";
    write_indent(hdr, 2);
    hdr << "__vstr_o.__vstr_live = false;\n";
    write_indent(hdr, 2);
    hdr << "return *this;\n";
    write_indent(hdr, 1);
    hdr << "}\n";
    write_indent(hdr, 1);
    hdr << "~" << n << "() { if (__vstr_live) { __vstr_deinit(); } }\n";
    // The body the resolver checked (its synthesized method), or the parsed
    // one when checking was skipped.
    const ast::Expr* body = s.deinit_body.get();
    for (const auto& m : s.methods) {
        if (m->kind == ast::NodeKind::Func && static_cast<const ast::FuncDecl&>(*m).is_deinit) {
            body = static_cast<const ast::FuncDecl&>(*m).body.get();
        }
    }
    write_indent(hdr, 1);
    hdr << "void __vstr_deinit() ";
    if (body != nullptr && body->kind == ast::NodeKind::BlockExpr) {
        emit_block(hdr, static_cast<const ast::BlockExpr&>(*body), 1);
    } else {
        hdr << "{}";
    }
    hdr << "\n";
}

void CppEmitter::emit_struct(std::ostream& hdr, const ast::StructDecl& s) {
    // §4 layout attributes on the struct itself: @repr(packed) +
    // @repr(align(N)) / @align(N) / @repr(union). C++ accepts
    // `alignas(N)` between the `struct` keyword and the name;
    // __attribute__((packed)) goes after the closing brace and
    // before the trailing `;`. `@repr(union)` swaps the `struct`
    // keyword for `union`, giving a C/C++ untagged overlay where
    // every member shares one storage cell.
    const auto struct_attrs = read_layout_attrs(s.attributes);
    // §7 generics phase 2 — a Vestra generic struct lowers to a C++ class
    // template. Emit `template <class T, std::size_t N, ...>` ahead of the
    // keyword (a const generic `[const N: Int]` becomes a `std::size_t`
    // non-type parameter, the natural fit for array lengths); the
    // monomorphization is the C++ compiler's job, as for generic functions.
    bool struct_first_generic = true;
    for (const auto& g : s.generics) {
        if (g.name.empty()) {
            continue;
        }
        if (struct_first_generic) {
            hdr << "template <";
            struct_first_generic = false;
        } else {
            hdr << ", ";
        }
        hdr << (g.is_const ? "std::size_t " : "class ") << g.name;
    }
    if (!struct_first_generic) {
        hdr << ">\n";
    }
    hdr << (struct_attrs.is_union ? "union " : "struct ");
    if (struct_attrs.align > 0) {
        hdr << "alignas(" << struct_attrs.align << ") ";
    }
    hdr << s.name << " {\n";
    for (const auto& f : s.fields) {
        write_indent(hdr, 1);
        const auto field_attrs = read_layout_attrs(f.attributes);
        const auto bits = read_bits_attr(f.attributes);
        if (field_attrs.align > 0) {
            hdr << "alignas(" << field_attrs.align << ") ";
        }
        // A function-typed field needs the name inside the declarator
        // (`R(*f)(T1)`), so emit_type_with_name writes both.
        if (f.type) {
            emit_type_with_name(hdr, *f.type, f.name);
        } else {
            hdr << " " << f.name;
        }
        if (bits > 0) {
            // Bit-fields can't have a default brace-init; leave the
            // default value to the zero-init the C++ compiler gives
            // each member when its enclosing struct is default-ctor'd.
            hdr << " : " << bits;
        } else if (struct_attrs.is_union) {
            // §A6 (§6.8 / §14.11.4) C++ unions accept at most one
            // default member initializer (the implicit ctor picks
            // the first non-static member). Skipping `{}` here means
            // every union construction lands through the explicit
            // designated-init the call lowering already emits
            // (`UartLineStatus{.raw = 0}`), so the user picks the
            // active member.
            hdr << "";
        } else {
            hdr << "{}";
        }
        hdr << ";\n";
    }
    // §12.3 derive(Eq): a defaulted operator== gives us field-by-field
    // structural equality for free, with the C++ compiler doing the
    // recursion through any nested derived-Eq members.
    if (auto it = derives_by_target_.find(s.name); it != derives_by_target_.end()) {
        if (it->second.contains("Eq")) {
            write_indent(hdr, 1);
            hdr << "[[nodiscard]] bool operator==(const " << s.name
                << "&) const noexcept = default;\n";
        }
        // §12.3 derive(Ord): a lexicographic `<` over the fields in
        // declaration order, and the other three from it. Spelled out
        // rather than a defaulted `<=>`, which needs <compare> and so is
        // unavailable to the freestanding profile.
        if (it->second.contains("Ord")) {
            std::vector<std::string> names;
            for (const auto& f : s.fields) {
                if (f.kind != ast::StructDecl::Field::Kind::Embed && f.type != nullptr) {
                    names.push_back(f.name);
                }
            }
            emit_ord_ops(hdr, s.name, names);
        }
        // §12.3 derive(Clone): emit an explicit `clone()` that delegates
        // to the implicit C++ copy ctor. For v0.5 every Vestra struct is
        // copy-constructible (we don't have move-only types yet), so the
        // structural traversal the spec calls for happens for free. The
        // method exists so users have an explicit copy site rather than
        // a silent one.
        if (it->second.contains("Clone")) {
            write_indent(hdr, 1);
            hdr << "[[nodiscard]] " << s.name << " clone() const { return *this; }\n";
        }
    }
    if (struct_has_deinit(s)) {
        emit_deinit_members(hdr, s);
    }
    // §5: methods declared inside `struct { ... }` lower to inline C++
    // methods on the same struct. v0.5 is minimal: no const-qualification
    // (so an `inout` method works), no generics, no out-of-line bodies.
    // The implicit `this` carries `self` through; SelfExpr already
    // lowers to `(*this)` so member access works.
    for (const auto& m : s.methods) {
        if (m->kind != ast::NodeKind::Func) {
            continue;
        }
        const auto& fn = static_cast<const ast::FuncDecl&>(*m);
        if (fn.is_deinit) {
            continue;  // emitted as the destructor's body above
        }
        // §7 a generic method is a member template.
        bool any_generic = false;
        for (const auto& g : fn.generics) {
            if (g.name.empty()) {
                continue;
            }
            if (!any_generic) {
                write_indent(hdr, 1);
                hdr << "template <";
            } else {
                hdr << ", ";
            }
            any_generic = true;
            hdr << (g.is_const ? "std::size_t " : "class ") << g.name;
        }
        if (any_generic) {
            hdr << ">\n";
        }
        write_indent(hdr, 1);
        hdr << "[[nodiscard]] ";
        // §11 an async method is a coroutine returning Task<R>, like an async
        // free function.
        if (fn.is_async) {
            hdr << "__vstr::Task<";
        }
        if (fn.result) {
            emit_type(hdr, *fn.result);
        } else {
            hdr << "void";
        }
        if (fn.is_async) {
            hdr << ">";
        }
        hdr << " " << fn.name << "(";
        for (std::size_t i = 0; i < fn.params.size(); ++i) {
            if (i != 0) {
                hdr << ", ";
            }
            const auto& p = fn.params[i];
            // §A12 (§14.6.3) — function-pointer params pass by value in the
            // declarator form (`R(*name)(T1)`), exactly as on free functions:
            // "ref to function pointer" would need `R(*const&)(T1)`, which
            // emit_type cannot spell, and the previous fall-through to
            // `const <fn-ptr>& name` was not valid C++ at all.
            const bool is_fn_ptr = p.type != nullptr && p.type->kind == ast::NodeKind::FunctionType;
            switch (p.mode) {
            case ast::ParamMode::Read:
                // §11 — an async method copies read params into its coroutine
                // frame (by value), since a `const T&` would dangle across a
                // suspension; a plain method keeps the canonical `const T&`.
                if (is_fn_ptr) {
                    if (p.type) {
                        emit_type_with_name(hdr, *p.type, p.name);
                    }
                } else if (fn.is_async) {
                    if (p.type) {
                        emit_type(hdr, *p.type);
                    }
                    hdr << " " << p.name;
                } else {
                    hdr << "const ";
                    if (p.type) {
                        emit_type(hdr, *p.type);
                    }
                    hdr << "& " << p.name;
                }
                break;
            case ast::ParamMode::Inout:
                if (p.type) {
                    emit_type(hdr, *p.type);
                }
                hdr << "& " << p.name;
                break;
            case ast::ParamMode::Sink:
                if (p.type) {
                    emit_type(hdr, *p.type);
                }
                hdr << "&& " << p.name;
                break;
            }
        }
        // §5 a plain method reads its receiver: `const`, so it can be called
        // on a `read` value. `inout func` mutates it; `sink func` consumes it.
        hdr << (fn.recv_mode == ast::FuncDecl::RecvMode::None ? ") const " : ") ");
        if (fn.body && fn.body->kind == ast::NodeKind::BlockExpr) {
            const auto& blk = static_cast<const ast::BlockExpr&>(*fn.body);
            if (fn.is_async) {
                // Returns in the body lower to co_return; a void coroutine
                // still needs one to be a coroutine.
                const bool prev_async = current_func_is_async_;
                current_func_is_async_ = true;
                hdr << "{\n";
                for (const auto& st : blk.stmts) {
                    emit_stmt(hdr, *st, 2);
                }
                const bool ends_in_return =
                    !blk.stmts.empty() && blk.stmts.back()->kind == ast::NodeKind::ReturnStmt;
                if (fn.result == nullptr && !ends_in_return) {
                    write_indent(hdr, 2);
                    hdr << "co_return;\n";
                }
                write_indent(hdr, 1);
                hdr << "}";
                current_func_is_async_ = prev_async;
            } else {
                emit_block(hdr, blk, 1);
            }
        } else {
            hdr << "{}";
        }
        hdr << "\n";
    }
    hdr << "}";
    if (struct_attrs.packed) {
        hdr << " __attribute__((packed))";
    }
    hdr << ";\n\n";
}

namespace {

// §7 generics phase 2 — a std::hash / std::formatter specialization for a
// generic type is a *partial* specialization: `template <class T> struct
// std::hash<Pair<T>>` rather than `template <> struct std::hash<Pair>`.
// These two helpers emit the template head and the `<T, N>` argument suffix
// on the specialized type name; for a non-generic type they degrade to
// `template <>` and no suffix, preserving the existing output.
bool spec_decl_is_generic(const std::vector<ast::GenericParam>& gens) {
    for (const auto& g : gens) {
        if (!g.name.empty()) {
            return true;
        }
    }
    return false;
}

void emit_spec_head(std::ostream& os, const std::vector<ast::GenericParam>& gens) {
    if (!spec_decl_is_generic(gens)) {
        os << "template <>\n";
        return;
    }
    os << "template <";
    bool first = true;
    for (const auto& g : gens) {
        if (g.name.empty()) {
            continue;
        }
        if (!first) {
            os << ", ";
        }
        first = false;
        os << (g.is_const ? "std::size_t " : "class ") << g.name;
    }
    os << ">\n";
}

void emit_spec_args(std::ostream& os, const std::vector<ast::GenericParam>& gens) {
    if (!spec_decl_is_generic(gens)) {
        return;
    }
    os << "<";
    bool first = true;
    for (const auto& g : gens) {
        if (g.name.empty()) {
            continue;
        }
        if (!first) {
            os << ", ";
        }
        first = false;
        os << g.name;
    }
    os << ">";
}

}  // namespace

void CppEmitter::emit_hash_spec(std::ostream& os,
                                const ast::StructDecl& s,
                                std::string_view qual_prefix) {
    // Boost-style hash combine: cheap, no <bit> dependency, gives a
    // decent distribution as long as each field has a sensible
    // std::hash. Doesn't need to be cryptographically strong — it's
    // for unordered_map bucket dispatch.
    emit_spec_head(os, s.generics);
    os << "struct std::hash<" << qual_prefix << s.name;
    emit_spec_args(os, s.generics);
    os << "> {\n";
    os << "    [[nodiscard]] std::size_t operator()(const " << qual_prefix << s.name;
    emit_spec_args(os, s.generics);
    os << "& v) const noexcept {\n";
    os << "        std::size_t __h = 0;\n";
    for (const auto& f : s.fields) {
        if (f.kind == ast::StructDecl::Field::Kind::Embed || f.type == nullptr) {
            // Embed flattening for hash is a follow-on; skip for now.
            continue;
        }
        os << "        __h ^= std::hash<";
        emit_type(os, *f.type);
        os << ">{}(v." << f.name << ") + 0x9e3779b9 + (__h << 6) + (__h >> 2);\n";
    }
    os << "        return __h;\n";
    os << "    }\n";
    os << "};\n";
}

void CppEmitter::emit_hash_spec_opaque(std::ostream& os,
                                       const ast::OpaqueDecl& o,
                                       std::string_view qual_prefix) {
    if (o.underlying == nullptr) {
        return;
    }
    os << "template <>\n";
    os << "struct std::hash<" << qual_prefix << o.name << "> {\n";
    os << "    [[nodiscard]] std::size_t operator()(" << qual_prefix << o.name
       << " v) const noexcept {\n";
    os << "        return std::hash<";
    emit_type(os, *o.underlying);
    os << ">{}(static_cast<";
    emit_type(os, *o.underlying);
    os << ">(v));\n";
    os << "    }\n";
    os << "};\n";
}

void CppEmitter::emit_debug_spec_opaque(std::ostream& os,
                                        const ast::OpaqueDecl& o,
                                        std::string_view qual_prefix) {
    if (o.underlying == nullptr) {
        return;
    }
    // Render as `Q(value)` — keeps the nominal name visible alongside
    // the underlying. Delegates to the underlying's std::formatter via
    // the `{}` placeholder so widths, precisions, etc. flow through.
    os << "template <>\n";
    os << "struct std::formatter<" << qual_prefix << o.name << "> {\n";
    os << "    constexpr auto parse(auto& __ctx) { return __ctx.begin(); }\n";
    os << "    auto format(" << qual_prefix << o.name << " v, auto& __ctx) const {\n";
    os << "        return std::format_to(__ctx.out(), \"" << o.name << "({})\", static_cast<";
    emit_type(os, *o.underlying);
    os << ">(v));\n";
    os << "    }\n";
    os << "};\n";
}

void CppEmitter::emit_hash_spec_enum(std::ostream& os,
                                     const ast::EnumDecl& e,
                                     std::string_view qual_prefix) {
    // Bare enums fall back to the standard library's `std::hash<E>`; we
    // only need a spec when the enum lowers to a struct-of-variant. A
    // generic enum always lowers to the variant shape (even all-bare), so
    // it needs the spec too.
    if (!enum_is_sum_type(e) && !spec_decl_is_generic(e.generics)) {
        return;
    }
    emit_spec_head(os, e.generics);
    os << "struct std::hash<" << qual_prefix << e.name;
    emit_spec_args(os, e.generics);
    os << "> {\n";
    os << "    [[nodiscard]] std::size_t operator()(const " << qual_prefix << e.name;
    emit_spec_args(os, e.generics);
    os << "& v) const noexcept {\n";
    os << "        return std::visit([&](auto&& __alt) {\n";
    os << "            using __T = std::decay_t<decltype(__alt)>;\n";
    os << "            std::size_t __h = v.value.index();\n";
    bool first = true;
    for (const auto& c : e.cases) {
        if (first) {
            write_indent(os, 3);
            os << "if";
        } else {
            os << " else if";
        }
        os << " constexpr (std::is_same_v<__T, ";
        // Inside a partial specialization `X<T>::case_t` is a dependent
        // name and needs `typename`; a concrete `X::case_t` does not.
        if (spec_decl_is_generic(e.generics)) {
            os << "typename ";
        }
        os << qual_prefix << e.name;
        emit_spec_args(os, e.generics);
        os << "::" << c.name << "_t>) {\n";
        for (std::size_t i = 0; i < c.payload.size(); ++i) {
            const auto& [pname, ptype] = c.payload[i];
            std::string fname = pname.empty() ? std::format("_{}", i) : pname;
            write_indent(os, 4);
            os << "__h ^= std::hash<";
            emit_type(os, *ptype);
            os << ">{}(__alt." << fname << ") + 0x9e3779b9 + (__h << 6) + (__h >> 2);\n";
        }
        write_indent(os, 3);
        os << "}";
        first = false;
    }
    if (!first) {
        os << "\n";
    }
    os << "            return __h;\n";
    os << "        }, v.value);\n";
    os << "    }\n";
    os << "};\n";
}

void CppEmitter::emit_debug_spec_struct(std::ostream& os,
                                        const ast::StructDecl& s,
                                        std::string_view qual_prefix) {
    // Renders as `Name{field1: <v.field1>, field2: <v.field2>, …}`.
    // Each `{}` in the std::format string is a placeholder; literal
    // braces in the output must be doubled (`{{` / `}}`) to survive
    // std::format's brace syntax.
    emit_spec_head(os, s.generics);
    os << "struct std::formatter<" << qual_prefix << s.name;
    emit_spec_args(os, s.generics);
    os << "> {\n";
    os << "    constexpr auto parse(auto& __ctx) { return __ctx.begin(); }\n";
    os << "    auto format(const " << qual_prefix << s.name;
    emit_spec_args(os, s.generics);
    os << "& v, auto& __ctx) const {\n";
    os << "        return std::format_to(__ctx.out(), \"" << s.name << "{{";
    bool first = true;
    for (const auto& f : s.fields) {
        if (f.kind == ast::StructDecl::Field::Kind::Embed || f.type == nullptr) {
            continue;
        }
        if (!first) {
            os << ", ";
        }
        os << f.name << ": {}";
        first = false;
    }
    os << "}}\"";
    for (const auto& f : s.fields) {
        if (f.kind == ast::StructDecl::Field::Kind::Embed || f.type == nullptr) {
            continue;
        }
        os << ", v." << f.name;
    }
    os << ");\n";
    os << "    }\n";
    os << "};\n";
}

void CppEmitter::emit_debug_spec_enum(std::ostream& os,
                                      const ast::EnumDecl& e,
                                      std::string_view qual_prefix) {
    emit_spec_head(os, e.generics);
    os << "struct std::formatter<" << qual_prefix << e.name;
    emit_spec_args(os, e.generics);
    os << "> {\n";
    os << "    constexpr auto parse(auto& __ctx) { return __ctx.begin(); }\n";
    if (!enum_is_sum_type(e) && !spec_decl_is_generic(e.generics)) {
        // Bare enum class: render `EnumName.caseName` via a switch.
        os << "    auto format(" << qual_prefix << e.name << " v, auto& __ctx) const {\n";
        os << "        switch (v) {\n";
        for (const auto& c : e.cases) {
            os << "            case " << qual_prefix << e.name << "::" << c.name
               << ": return std::format_to(__ctx.out(), \"" << e.name << "." << c.name << "\");\n";
        }
        os << "        }\n";
        os << "        return std::format_to(__ctx.out(), \"?\");\n";
        os << "    }\n";
        os << "};\n";
        return;
    }
    // Sum-type enum: std::visit + constexpr-if chain, mirroring
    // emit_match. Each alternative renders as `EnumName::caseName{…}`
    // for payloaded cases, or just `EnumName::caseName` for bare.
    os << "    auto format(const " << qual_prefix << e.name;
    emit_spec_args(os, e.generics);
    os << "& v, auto& __ctx) const {\n";
    os << "        return std::visit([&](auto&& __alt) {\n";
    os << "            using __T = std::decay_t<decltype(__alt)>;\n";
    bool first = true;
    for (const auto& c : e.cases) {
        if (first) {
            write_indent(os, 3);
            os << "if";
        } else {
            os << " else if";  // chained — previous arm ended with bare "}"
        }
        os << " constexpr (std::is_same_v<__T, ";
        if (spec_decl_is_generic(e.generics)) {
            os << "typename ";
        }
        os << qual_prefix << e.name;
        emit_spec_args(os, e.generics);
        os << "::" << c.name << "_t>) {\n";
        write_indent(os, 4);
        if (c.payload.empty()) {
            os << "return std::format_to(__ctx.out(), \"" << e.name << "::" << c.name << "\");\n";
        } else {
            os << "return std::format_to(__ctx.out(), \"" << e.name << "::" << c.name << "{{";
            for (std::size_t i = 0; i < c.payload.size(); ++i) {
                if (i != 0) {
                    os << ", ";
                }
                std::string fname =
                    c.payload[i].first.empty() ? std::format("_{}", i) : c.payload[i].first;
                os << fname << ": {}";
            }
            os << "}}\"";
            for (std::size_t i = 0; i < c.payload.size(); ++i) {
                std::string fname =
                    c.payload[i].first.empty() ? std::format("_{}", i) : c.payload[i].first;
                os << ", __alt." << fname;
            }
            os << ");\n";
        }
        write_indent(os, 3);
        os << "}";
        first = false;
    }
    os << " else { std::unreachable(); }\n";
    os << "        }, v.value);\n";
    os << "    }\n";
    os << "};\n";
}

void CppEmitter::emit_enum(std::ostream& hdr, const ast::EnumDecl& e) {
    // A Vestra enum with payloads maps cleanly to std::variant; bare enums to
    // a C++ enum class. We detect "all bare" cheaply.
    // §7 generics phase 2 — a generic enum lowers to a C++ class template
    // over the struct-of-variant shape (a C++ `enum class` can't be a
    // template), so a generic enum never takes the bare path even when all
    // its cases happen to be payload-free.
    bool is_generic = false;
    for (const auto& g : e.generics) {
        if (!g.name.empty()) {
            is_generic = true;
            break;
        }
    }
    bool all_bare = !is_generic;
    for (const auto& c : e.cases) {
        if (!c.payload.empty()) {
            all_bare = false;
            break;
        }
    }
    if (all_bare) {
        hdr << "enum class " << e.name << " {\n";
        for (const auto& c : e.cases) {
            write_indent(hdr, 1);
            hdr << c.name << ",\n";
        }
        hdr << "};\n\n";
        // C++ `enum class` already has operator== built in, so
        // derive(Eq) for a bare enum needs no extra code.
        // derive(Clone) on a bare enum has no method slot to fill
        // (C++ enum class can't host methods) — sema's lookup skips
        // clone for bare enums, so the user just uses the implicit
        // copy on the enum value directly.
        return;
    }
    // Sum type — emit a struct-of-variant. TODO: tighter sum-type lowering.
    //
    // Detect derive(Eq) up front: `std::variant::operator==` requires
    // each alternative to be equality-comparable, so we need to add a
    // defaulted operator== to every case_t too (cheap — it's all POD
    // member compares).
    const bool wants_eq =
        derives_by_target_.contains(e.name) && derives_by_target_.at(e.name).contains("Eq");
    // derive(Ord) likewise: `std::variant`'s `<` orders by case index, then
    // by the payload's own `<`, which each case_t spells out field by field.
    const bool wants_ord =
        derives_by_target_.contains(e.name) && derives_by_target_.at(e.name).contains("Ord");
    // §7 generics phase 2 — `template <class T, ...>` ahead of the wrapper
    // struct. The per-case `*_t` structs are nested inside it, so a payload
    // field typed T refers to the enclosing template's parameter; no inner
    // template prefix is needed.
    bool enum_first_generic = true;
    for (const auto& g : e.generics) {
        if (g.name.empty()) {
            continue;
        }
        if (enum_first_generic) {
            hdr << "template <";
            enum_first_generic = false;
        } else {
            hdr << ", ";
        }
        hdr << (g.is_const ? "std::size_t " : "class ") << g.name;
    }
    if (!enum_first_generic) {
        hdr << ">\n";
    }
    hdr << "struct " << e.name << " {\n";
    for (const auto& c : e.cases) {
        write_indent(hdr, 1);
        hdr << "struct " << c.name << "_t {";
        for (std::size_t i = 0; i < c.payload.size(); ++i) {
            if (i != 0) {
                hdr << " ";
            }
            const std::string field =
                c.payload[i].first.empty() ? std::format("_{}", i) : c.payload[i].first;
            if (c.payload[i].second) {
                emit_type_with_name(hdr, *c.payload[i].second, field);
            } else {
                hdr << " " << field;
            }
            hdr << "{}; ";
        }
        if (wants_eq) {
            hdr << "[[nodiscard]] bool operator==(const " << c.name
                << "_t&) const noexcept = default; ";
        }
        if (wants_ord) {
            hdr << "[[nodiscard]] bool operator<(const " << c.name << "_t&"
                << (c.payload.empty() ? "" : " o") << ") const noexcept { ";
            for (std::size_t i = 0; i < c.payload.size(); ++i) {
                const std::string field =
                    c.payload[i].first.empty() ? std::format("_{}", i) : c.payload[i].first;
                hdr << "if (" << field << " < o." << field << ") { return true; } if (o."
                    << field << " < " << field << ") { return false; } ";
            }
            hdr << "return false; } ";
        }
        hdr << "};\n";
    }
    write_indent(hdr, 1);
    hdr << "std::variant<";
    for (std::size_t i = 0; i < e.cases.size(); ++i) {
        if (i != 0) {
            hdr << ", ";
        }
        hdr << e.cases[i].name << "_t";
    }
    hdr << "> value{};\n";
    // §12.3 derive(Eq): sum-type wrappers need a defaulted operator==
    // too; std::variant compares structurally provided every alternative
    // is comparable, and the defaulted operator delegates accordingly.
    if (auto it = derives_by_target_.find(e.name); it != derives_by_target_.end()) {
        if (it->second.contains("Eq")) {
            write_indent(hdr, 1);
            hdr << "[[nodiscard]] bool operator==(const " << e.name
                << "&) const noexcept = default;\n";
        }
        if (wants_ord) {
            write_indent(hdr, 1);
            hdr << "[[nodiscard]] bool operator<(const " << e.name
                << "& o) const noexcept { return value < o.value; }\n";
            emit_ord_derived(hdr, e.name);
        }
        // §12.3 derive(Clone): same shape as the struct path.
        if (it->second.contains("Clone")) {
            write_indent(hdr, 1);
            hdr << "[[nodiscard]] " << e.name << " clone() const { return *this; }\n";
        }
    }
    hdr << "};\n\n";
}

void CppEmitter::emit_ord_ops(std::ostream& hdr,
                              std::string_view type_name,
                              const std::vector<std::string>& fields) {
    write_indent(hdr, 1);
    hdr << "[[nodiscard]] bool operator<(const " << type_name << "&" << (fields.empty() ? "" : " o")
        << ") const noexcept {";
    for (const auto& f : fields) {
        hdr << " if (" << f << " < o." << f << ") { return true; } if (o." << f << " < " << f
            << ") { return false; }";
    }
    hdr << " return false; }\n";
    emit_ord_derived(hdr, type_name);
}

void CppEmitter::emit_ord_derived(std::ostream& hdr, std::string_view type_name) {
    write_indent(hdr, 1);
    hdr << "[[nodiscard]] bool operator>(const " << type_name
        << "& o) const noexcept { return o < *this; }\n";
    write_indent(hdr, 1);
    hdr << "[[nodiscard]] bool operator<=(const " << type_name
        << "& o) const noexcept { return !(o < *this); }\n";
    write_indent(hdr, 1);
    hdr << "[[nodiscard]] bool operator>=(const " << type_name
        << "& o) const noexcept { return !(*this < o); }\n";
}

// §10 move-on-consume helpers ------------------------------------------------

// An AST type annotation lowers to a non-copyable C++ type when it transitively
// reaches a Box (→ std::unique_ptr). Only Box and its containers count: Vec /
// String / HashMap are owning but *copyable* in C++, so they must NOT be treated
// as move-only (a copy of them is legal, just deep). Nominal struct/enum names
// are resolved against `move_only_types_`, which the fixpoint below fills.
bool CppEmitter::ast_type_is_move_only(const ast::Type& t) const {
    switch (t.kind) {
    case ast::NodeKind::OptionalType: {
        const auto& o = static_cast<const ast::OptionalType&>(t);
        return o.inner != nullptr && ast_type_is_move_only(*o.inner);
    }
    case ast::NodeKind::VectorType: {  // `[N]T` fixed array: move-only iff T is
        const auto& v = static_cast<const ast::VectorType&>(t);
        return v.element != nullptr && ast_type_is_move_only(*v.element);
    }
    case ast::NodeKind::TupleType: {
        for (const auto& el : static_cast<const ast::TupleType&>(t).elements) {
            if (el != nullptr && ast_type_is_move_only(*el)) {
                return true;
            }
        }
        return false;
    }
    case ast::NodeKind::NamedType: {
        const auto& nt = static_cast<const ast::NamedType&>(t);
        if (nt.path.empty()) {
            return false;
        }
        const std::string& name = nt.path.back();
        if (name == "Box") {
            return true;
        }
        // A container is move-only exactly when an element type is.
        if (name == "Vec" || name == "Optional" || name == "HashMap" || name == "Soa") {
            for (const auto& a : nt.type_args) {
                if (a != nullptr && ast_type_is_move_only(*a)) {
                    return true;
                }
            }
            return false;
        }
        return move_only_types_.contains(name);
    }
    default:
        return false;
    }
}

// Seed `move_only_types_` with every struct/enum that transitively owns a Box.
// Iterate to a fixpoint: a nominal becomes move-only once any field / case
// payload is move-only given what's already known, which may in turn mark a
// nominal that names it. Monotonic and bounded by the decl count.
void CppEmitter::compute_move_only_types() {
    move_only_types_.clear();
    bool changed = true;
    while (changed) {
        changed = false;
        for (const auto& [name, sd] : structs_by_name_) {
            if (move_only_types_.contains(name)) {
                continue;
            }
            // §6 a type with a `deinit` moves only as a whole (its copies are
            // deleted in the C++).
            if (struct_has_deinit(*sd)) {
                move_only_types_.insert(name);
                changed = true;
                continue;
            }
            for (const auto& f : sd->fields) {
                if (f.type != nullptr && ast_type_is_move_only(*f.type)) {
                    move_only_types_.insert(name);
                    changed = true;
                    break;
                }
            }
        }
        for (const auto& [name, ed] : enums_by_name_) {
            if (move_only_types_.contains(name)) {
                continue;
            }
            bool found = false;
            for (const auto& c : ed->cases) {
                for (const auto& [pn, pt] : c.payload) {
                    if (pt != nullptr && ast_type_is_move_only(*pt)) {
                        found = true;
                        break;
                    }
                }
                if (found) {
                    break;
                }
            }
            if (found) {
                move_only_types_.insert(name);
                changed = true;
            }
        }
    }
}

// A resolved expression type is move-only on the same Box-transitive rule.
bool CppEmitter::is_move_only(sema::TypePtr t) const {
    if (t == nullptr) {
        return false;
    }
    switch (t->kind()) {
    case sema::TypeKind::Box:
        return true;
    case sema::TypeKind::Optional:
    case sema::TypeKind::Vec:
        return is_move_only(t->inner());
    case sema::TypeKind::HashMap:
    case sema::TypeKind::Soa:
        for (const auto& p : t->parts()) {
            if (is_move_only(p)) {
                return true;
            }
        }
        return false;
    case sema::TypeKind::Tuple:
        for (const auto& p : t->parts()) {
            if (is_move_only(p)) {
                return true;
            }
        }
        return false;
    case sema::TypeKind::Struct:
    case sema::TypeKind::Enum: {
        const auto* d = t->nominal_decl();
        if (d == nullptr) {
            return false;
        }
        const std::string& name = d->kind == ast::NodeKind::Struct
                                      ? static_cast<const ast::StructDecl&>(*d).name
                                      : static_cast<const ast::EnumDecl&>(*d).name;
        return move_only_types_.contains(name);
    }
    default:
        return false;
    }
}

// Only an lvalue can be left dangling-but-valid by a move; a prvalue (a call
// result, a literal, a fresh construction) is already an rvalue that binds /
// elides without a copy, so std::move on it would only pessimize.
bool CppEmitter::is_lvalue_expr(const ast::Expr& e) noexcept {
    return e.kind == ast::NodeKind::IdentExpr || e.kind == ast::NodeKind::MemberExpr
           || e.kind == ast::NodeKind::IndexExpr;
}

void CppEmitter::emit_value(std::ostream& os, const ast::Expr& e) {
    // A propagating `try` is hoisted to a temp before this point, so in a
    // by-value position it reads as an lvalue identifier (`__vstr_tN`) — treat it
    // like one so a move-only `try` result moves rather than copies.
    const bool hoisted_try = e.kind == ast::NodeKind::TryExpr && lookup_try_hoist(&e) != nullptr;
    const bool move = resolution_ != nullptr && (is_lvalue_expr(e) || hoisted_try)
                      && is_move_only(resolution_->type_of(&e));
    if (move) {
        os << "std::move(";
    }
    emit_expr(os, e);
    if (move) {
        os << ")";
    }
}

void CppEmitter::emit_expr(std::ostream& os, const ast::Expr& e) {
    // §12.1 folded literal short-circuit: when sema's comptime folder
    // computed a constant value for this expression, emit that value
    // directly. Skips the whole structure (e.g. `1 << 8 - 1` becomes
    // `255` in the output) while still giving the C++ compiler an
    // exact-width literal.
    if (resolution_ != nullptr) {
        if (const auto* v = resolution_->folded_value(&e)) {
            os << v->to_cpp_literal();
            return;
        }
        // §18.5 string-lattice coercion: this string-view expression flows into
        // a `String` slot; wrap it in `std::string(...)` (the string_view→string
        // ctor is explicit). The guard wraps once — the recursive emit (and its
        // subtree) is emitted un-wrapped inside the parens.
        if (!in_string_coerce_ && resolution_->needs_string_coercion(&e)) {
            os << "std::string(";
            in_string_coerce_ = true;
            emit_expr(os, e);
            in_string_coerce_ = false;
            os << ")";
            return;
        }
        // §14.9.1 a non-literal ordering is checked where it runs.
        if (in_order_guard_ != &e) {
            const auto g = resolution_->ordering_guard(&e);
            if (g != sema::Resolution::OrderingGuard::None) {
                os << (g == sema::Resolution::OrderingGuard::Load ? "__vstr::load_order("
                                                                   : "__vstr::store_order(");
                in_order_guard_ = &e;
                emit_expr(os, e);
                in_order_guard_ = nullptr;
                os << ")";
                return;
            }
        }
    }
    in_string_coerce_ = false;
    switch (e.kind) {
    case ast::NodeKind::IntLit: {
        // §17.1 admits `_` as a digit separator (`1_000_000`,
        // `0xCAFE_BABE`); C++ treats `_BABE` as a user-defined
        // literal suffix, so we strip the underscores when
        // forwarding the text to the C++ side.
        const auto& text = static_cast<const ast::IntLit&>(e).text;
        for (char c : text) {
            if (c != '_') {
                os << c;
            }
        }
        break;
    }
    case ast::NodeKind::FloatLit:
        os << static_cast<const ast::FloatLit&>(e).text;
        break;
    case ast::NodeKind::StringLit:
        os << "std::string_view(\"" << static_cast<const ast::StringLit&>(e).text << "\")";
        break;
    case ast::NodeKind::InterpStringExpr: {
        // §4 lowering: each literal fragment contributes a `{{...}}`-free
        // chunk to the format string, each splice contributes `{}` plus
        // an argument. std::format does the per-type rendering at
        // runtime; for now we trust Display ≈ std::formatter, which
        // works out of the box for the numeric / Bool / string-like
        // primitives. The result is a freshly-allocated std::string,
        // matching §4's "interpolation is an allocation."
        const auto& is_ = static_cast<const ast::InterpStringExpr&>(e);
        os << "std::format(\"";
        for (const auto& seg : is_.segments) {
            if (seg.expr == nullptr) {
                // Literal fragment: escape `{` and `}` for std::format.
                for (char c : seg.literal) {
                    if (c == '{' || c == '}') {
                        os << c << c;
                    } else {
                        os << c;
                    }
                }
            } else {
                os << "{}";
            }
        }
        os << "\"";
        for (const auto& seg : is_.segments) {
            if (seg.expr != nullptr) {
                os << ", ";
                emit_expr(os, *seg.expr);
            }
        }
        os << ")";
        break;
    }
    case ast::NodeKind::CharLit:
        os << "U'" << static_cast<const ast::CharLit&>(e).text << "'";
        break;
    case ast::NodeKind::BoolLit:
        os << (static_cast<const ast::BoolLit&>(e).value ? "true" : "false");
        break;
    case ast::NodeKind::NilLit:
        // §9 nil → std::nullopt; the surrounding std::optional<T> slot
        // narrows it via the implicit converting ctor.
        os << "std::nullopt";
        break;
    case ast::NodeKind::IdentExpr: {
        const auto& id = static_cast<const ast::IdentExpr&>(e);
        // §12.4 a macro-template name is the unit's own declaration even where
        // a local of the same name is in scope: qualify it past the local.
        if (id.definition_site && !id.definition_module.empty() && resolution_ != nullptr) {
            // An imported macro's template name: `::mlib::k`.
            if (const auto* qn = resolution_->qualified_name_of(&e); qn != nullptr) {
                os << "::" << *qn;
                break;
            }
        }
        if (id.definition_site && resolution_ != nullptr) {
            const auto* sym = resolution_->symbol_of(&e);
            if (sym != nullptr && sym->decl != nullptr
                && (sym->kind == sema::SymbolKind::Func || sym->kind == sema::SymbolKind::Const
                    || sym->kind == sema::SymbolKind::Static)) {
                os << "::";
                for (const auto& seg : module_path_) {
                    os << seg << "::";
                }
            }
        }
        os << id.name;
        break;
    }
    case ast::NodeKind::SelfExpr:
        os << "(*this)";
        break;
    case ast::NodeKind::ParenExpr:
        os << "(";
        emit_expr(os, *static_cast<const ast::ParenExpr&>(e).inner);
        os << ")";
        break;
    case ast::NodeKind::CopyExpr:
        // §6 `copy x` is a fresh value, leaving `x` usable: C++23's
        // decay-copy `auto(x)` (F20).
        os << "auto(";
        emit_expr(os, *static_cast<const ast::CopyExpr&>(e).inner);
        os << ")";
        break;
    case ast::NodeKind::TupleLitExpr: {
        // §6 `(e1, e2, …)` → `std::tuple{e1, e2, …}`. CTAD picks the
        // element types from the brace-init list, matching the sema
        // TupleType the resolver gave the expression.
        const auto& tup = static_cast<const ast::TupleLitExpr&>(e);
        os << "std::tuple{";
        for (std::size_t i = 0; i < tup.elements.size(); ++i) {
            if (i != 0) {
                os << ", ";
            }
            if (tup.elements[i]) {
                emit_value(os, *tup.elements[i]);  // §10 move move-only elements in
            }
        }
        os << "}";
        break;
    }
    case ast::NodeKind::VectorLitExpr: {
        // §13 / §A8 `[e1, e2, …]` lowers to a C++ brace-init list. The
        // surrounding declaration provides the target type
        // (`std::array<T, N>` for a `[N]T` decl), so a bare brace-list
        // suffices — list-initialization picks up the aggregate ctor
        // and the function-name-decays-to-pointer rule covers the
        // vector-table case where each element is a bare ISR name.
        const auto& v = static_cast<const ast::VectorLitExpr&>(e);
        // §7 generics — self-describe the literal with its `std::array<E, N>`
        // type when the element is a plain data type, so it can deduce a
        // generic `[N]T` parameter at a call site (template argument
        // deduction can't see through a bare brace list). The vector-table
        // forms (function-pointer / @interrupt elements) keep the bare list,
        // since their declared target type supplies the shape and their
        // element decay relies on it.
        const sema::Type* lit_t = resolution_ != nullptr ? resolution_->type_of(&e) : nullptr;
        if (lit_t != nullptr && lit_t->kind() == sema::TypeKind::Vector
            && lit_t->vector_length_name().empty() && lit_t->inner() != nullptr
            && lit_t->inner()->kind() != sema::TypeKind::Function
            && lit_t->inner()->kind() != sema::TypeKind::InterruptHandler) {
            emit_sema_type(os, lit_t);
        }
        os << "{";
        for (std::size_t i = 0; i < v.elements.size(); ++i) {
            if (i != 0) {
                os << ", ";
            }
            if (v.elements[i]) {
                emit_expr(os, *v.elements[i]);
            }
        }
        os << "}";
        break;
    }
    case ast::NodeKind::UnaryExpr: {
        const auto& u = static_cast<const ast::UnaryExpr&>(e);
        if (u.op == ast::UnaryOp::Unwrap) {
            // §10 force-unwrap: `opt!` lowers to `__vstr::unwrap_opt(opt)`
            // — the shim panics via `__vstr::panic` on `.none` rather
            // than throwing `std::bad_optional_access`. The behavior is
            // identical (process aborts on absent value), but the path
            // routes through the named panic primitive instead of the
            // exception machinery.
            os << "__vstr::unwrap_opt(";
            emit_expr(os, *u.operand);
            os << ")";
            break;
        }
        os << unop_text(u.op);
        emit_expr(os, *u.operand);
        break;
    }
    case ast::NodeKind::TryExpr: {
        // §9 try forms:
        //   * try e   — propagating: the enclosing emit_stmt's hoist
        //               pass has already pre-emitted a let-binding of
        //               the unwrapped value; we output that name here.
        //               If hoisting was skipped (e.g. this try sits
        //               inside a conditional branch the walk refused
        //               to descend into), fall back to .value() so the
        //               code still compiles (panic on error).
        //   * try? e  — converts to Optional<T> via an IIFE.
        //   * try! e  — panics on error (.value()).
        const auto& tx = static_cast<const ast::TryExpr&>(e);
        if (tx.form == ast::TryExpr::Form::Optional) {
            os << "([&]{ auto __vstr_r = ";
            emit_expr(os, *tx.inner);
            // §9 a Unit-returning callee has no value to wrap (`*r` is void),
            // so `try?` yields an engaged `std::optional<std::monostate>`.
            if (is_unit_typed(*tx.inner)) {
                os << "; return __vstr_r.has_value() ? std::optional<std::monostate>{std::in_place}"
                      " : std::nullopt; }())";
            } else {
                os << "; return __vstr_r.has_value() ? std::optional{*__vstr_r} : std::nullopt; }())";
            }
            break;
        }
        // Propagating with a hoist registered → emit the hoisted name.
        if (tx.form == ast::TryExpr::Form::Propagating) {
            if (const auto* name = lookup_try_hoist(&tx)) {
                // A Unit hoist binds no value (see emit_try_hoist): use the
                // void `*r` of the std::expected<void, E> itself.
                if (is_unit_typed(tx)) {
                    os << "*" << *name << "_r";
                } else {
                    os << *name;
                }
                break;
            }
        }
        // §10 try! / un-hoisted propagating try both route through the
        // named panic primitive via `__vstr::unwrap_exp`. The latter
        // shouldn't fire in v0.5 (the IfExpr/MatchExpr cond-hoists +
        // branch-local hoists cover every conditional context the walk
        // used to refuse), but the fallback stays for safety.
        os << "__vstr::unwrap_exp(";
        emit_expr(os, *tx.inner);
        os << ")";
        break;
    }
    case ast::NodeKind::ThrowExpr: {
        // §9: `throw e` ends the enclosing `throws(E)` function with the
        // error value. The clean lowering only works at statement / return
        // position; emit_stmt handles those forms. In any other expression
        // slot we fall back to a `std::unreachable()` IIFE so the program
        // still type-checks (the resolver lets `throw` flow into any slot
        // since its static type is Never).
        const auto& th = static_cast<const ast::ThrowExpr&>(e);
        os << "([&]() -> decltype(auto) { (void)(";
        emit_expr(os, *th.inner);
        os << "); std::unreachable(); }())";
        break;
    }
    case ast::NodeKind::AddressOfExpr: {
        // §A12 (§14.6.3) `&decl` — the C++ side is a plain `&`.
        // The operand resolves to a global / static identifier so
        // its address is meaningful at link time without further
        // qualification.
        const auto& a = static_cast<const ast::AddressOfExpr&>(e);
        os << "&";
        emit_expr(os, *a.inner);
        break;
    }
    case ast::NodeKind::DoCatchExpr: {
        // §9 `do { body } catch (NAME: E) { handler }` lowers to a
        // pair of nested IIFEs: the inner one returns std::expected<T,
        // E> so the body's `try` propagates via std::unexpected; the
        // outer dispatches has_value to the body value or to the catch
        // handler with NAME bound to E. The success type T comes from
        // the resolver — it's the do-catch expression's own type.
        //
        // When the do-catch carries a `where` guard, sema requires the
        // enclosing function to be `throws(E)` and collect_try_hoists
        // lifts the whole DoCatchExpr as a cond-hoist (the inner
        // lambda returns expected<T,E> at the outer scope, the
        // guard-fail propagates as std::unexpected). The substitution
        // below routes the use-site to that hoisted name.
        const auto& dc = static_cast<const ast::DoCatchExpr&>(e);
        if (const auto* name = lookup_try_hoist(&e)) {
            os << "(*" << *name << ")";
            break;
        }
        sema::TypePtr result_type = resolution_ != nullptr ? resolution_->type_of(&e) : nullptr;
        // Pin the outer lambda's return type explicitly so the catch
        // body's expression-typed return doesn't trip C++ "two
        // different return types" deduction. Falls back to `auto`
        // when sema didn't supply a type (resolver-free emit path).
        os << "([&]() -> ";
        if (result_type != nullptr) {
            emit_sema_type(os, result_type);
        } else {
            os << "auto";
        }
        os << " { auto __vstr_do = [&]() -> std::expected<";
        emit_sema_type(os, result_type);
        os << ", ";
        // Annotated form (dc.error_type != null): emit the AST type
        // directly. Bare form: sema inferred E and stashed it on
        // Resolution::do_catch_error_type — render that via the
        // canonical-type emitter.
        if (dc.error_type) {
            emit_type(os, *dc.error_type);
        } else if (resolution_ != nullptr) {
            emit_sema_type(os, resolution_->do_catch_error_type(&dc));
        }
        os << "> { ";
        // The inner lambda is a fresh statement scope, so any mid-
        // expression `try` in the do-body needs its own hoist pass —
        // the outer emit_stmt's hoist pass didn't descend into the
        // do-catch. Without this, `do { (try f()) + (try g()) } …`
        // would fall back to `.value()` panics. The do-body is almost
        // always a BlockExpr whose trailing expression carries the
        // result; collect_try_hoists itself doesn't descend into a
        // BlockExpr (each inner stmt normally gets its own hoist scope
        // via emit_stmt), so we hand-walk the trailing here.
        std::vector<TryHoist> body_hoists;
        if (dc.do_body->kind == ast::NodeKind::BlockExpr) {
            const auto& b = static_cast<const ast::BlockExpr&>(*dc.do_body);
            if (!b.stmts.empty() && b.stmts.back()->kind == ast::NodeKind::ExprStmt) {
                const auto& trailing = static_cast<const ast::ExprStmt&>(*b.stmts.back());
                if (trailing.expr) {
                    collect_try_hoists(*trailing.expr, body_hoists);
                }
            }
        } else {
            collect_try_hoists(*dc.do_body, body_hoists);
        }
        for (const auto& h : body_hoists) {
            emit_try_hoist(os, h, 0);
        }
        const auto* prev_hoists = active_hoists_;
        active_hoists_ = &body_hoists;
        emit_stmt_expr(os, *dc.do_body, /*return_value=*/true);
        active_hoists_ = prev_hoists;
        os << " }(); if (__vstr_do.has_value()) { return *__vstr_do; } [[maybe_unused]] auto "
           << dc.error_name << " = __vstr_do.error(); ";
        if (dc.guard) {
            // §9 `catch (e: E) where guard`: gate the handler on the
            // guard's runtime value. The normal path is the cond-
            // hoist substitution above — sema requires an enclosing
            // throws(E) context and collect_try_hoists registers the
            // DoCatchExpr, so guard-fail propagates via
            // std::unexpected from the lifted lambda. This branch is
            // a defensive fallback: it only fires on the resolver-
            // free emit path (codegen-only tests), where no hoist
            // gets registered. The panic keeps that path safe.
            os << "if (";
            emit_expr(os, *dc.guard);
            os << ") { return ";
            emit_expr(os, *dc.catch_body);
            os << "; } return __vstr::panic(\"do/catch where-guard fell through "
                  "(resolver-free emit path)\");";
        } else {
            os << "return ";
            emit_expr(os, *dc.catch_body);
            os << ";";
        }
        os << " }())";
        break;
    }
    case ast::NodeKind::BinaryExpr: {
        const auto& b = static_cast<const ast::BinaryExpr&>(e);
        if (b.op == ast::BinaryOp::Coalesce) {
            // §9, §19.9 nil-coalescing: `a ?? b` evaluates `b` only when `a`
            // is none. A default that can do something (a call, an index, a
            // division) runs in a callback; a plain one reads as
            // `(a).value_or(b)`. A global's initializer has no block scope
            // to capture from, so it stays eager.
            if (!global_init_ && coalesce_is_lazy(*b.rhs)) {
                os << "__vstr::coalesce(";
                emit_expr(os, *b.lhs);
                os << ", [&](auto __t) -> typename decltype(__t)::type { return ";
                emit_expr(os, *b.rhs);
                os << "; })";
                break;
            }
            os << "(";
            emit_expr(os, *b.lhs);
            os << ").value_or(";
            emit_expr(os, *b.rhs);
            os << ")";
            break;
        }
        // §13 elementwise vector arithmetic: when sema typed the result as a
        // fixed-length vector, `a <op> b` lowers to an `__vstr::vec_<op>` helper
        // that loops lanewise over the std::array (auto-vectorized by the C++
        // compiler — real SIMD where it pays, a scalar fallback otherwise).
        if (resolution_ != nullptr
            && (b.op == ast::BinaryOp::Add || b.op == ast::BinaryOp::Sub
                || b.op == ast::BinaryOp::Mul || b.op == ast::BinaryOp::Div)) {
            if (auto rt = resolution_->type_of(&e);
                rt != nullptr && rt->kind() == sema::TypeKind::Vector) {
                const char* fn = b.op == ast::BinaryOp::Add   ? "vec_add"
                                 : b.op == ast::BinaryOp::Sub ? "vec_sub"
                                 : b.op == ast::BinaryOp::Mul ? "vec_mul"
                                                              : "vec_div";
                os << "__vstr::" << fn << "(";
                emit_expr(os, *b.lhs);
                os << ", ";
                emit_expr(os, *b.rhs);
                os << ")";
                break;
            }
        }
        // §4 `&+ &- &*` wrap: a prelude helper does the operation in
        // unsigned, where C++ wraps too. An integer literal operand goes in
        // as a `wlit`, so the result takes the other operand's type.
        if (b.op == ast::BinaryOp::WrappingAdd || b.op == ast::BinaryOp::WrappingSub
            || b.op == ast::BinaryOp::WrappingMul) {
            os << (b.op == ast::BinaryOp::WrappingAdd   ? "__vstr::wrap_add("
                   : b.op == ast::BinaryOp::WrappingSub ? "__vstr::wrap_sub("
                                                        : "__vstr::wrap_mul(");
            emit_wrap_operand(os, *b.lhs);
            os << ", ";
            emit_wrap_operand(os, *b.rhs);
            os << ")";
            break;
        }
        // §4 integer division or remainder by zero traps: unless the
        // divisor is a nonzero literal, the prelude's idiv / irem check it.
        if ((b.op == ast::BinaryOp::Div || b.op == ast::BinaryOp::Mod)
            && !divisor_is_safe(*b.rhs)) {
            os << (b.op == ast::BinaryOp::Div ? "__vstr::idiv(" : "__vstr::irem(");
            emit_expr(os, *b.lhs);
            os << ", ";
            emit_expr(os, *b.rhs);
            os << ")";
            break;
        }
        // The tree already encodes §17.7 precedence, but C++ re-parses the
        // text with its own table. Parenthesize an operand only where the two
        // disagree, so already-correct output is unchanged.
        const bool lp = binop_operand_needs_parens(*b.lhs, b.op, false);
        const bool rp = binop_operand_needs_parens(*b.rhs, b.op, true);
        os << (lp ? "(" : "");
        emit_expr(os, *b.lhs);
        os << (lp ? ")" : "") << " " << binop_text(b.op) << " " << (rp ? "(" : "");
        emit_expr(os, *b.rhs);
        os << (rp ? ")" : "");
        break;
    }
    case ast::NodeKind::CallExpr: {
        const auto& c = static_cast<const ast::CallExpr&>(e);
        // §4 `x.to[T]()` traps when the value doesn't fit T; `x.toWrapping[T]()`
        // truncates, which a C++ integer conversion already does (modulo 2^N).
        if (const auto* conv = sema::Resolver::numeric_conversion_call(c);
            conv != nullptr && resolution_ != nullptr) {
            const auto& mem = static_cast<const ast::MemberExpr&>(*conv);
            if (mem.member == "to") {
                os << "__vstr::num_to<";
            } else {
                os << "static_cast<";
            }
            emit_sema_type(os, resolution_->type_of(&e));
            os << ">(";
            emit_expr(os, *mem.base);
            os << ")";
            break;
        }
        // §4 `E.from(raw)` → `__vstr::enum_from<E>(raw, <case count>)`: the
        // case at declaration index `raw`, or `.none`.
        if (resolution_ != nullptr && c.callee->kind == ast::NodeKind::MemberExpr
            && c.args.size() == 1) {
            const auto& mem = static_cast<const ast::MemberExpr&>(*c.callee);
            const auto* sym = mem.member == "from" ? resolution_->symbol_of(mem.base.get()) : nullptr;
            auto rt = resolution_->type_of(&e);
            if (sym != nullptr && sym->kind == sema::SymbolKind::Enum && sym->decl != nullptr
                && sym->decl->kind == ast::NodeKind::Enum && rt != nullptr
                && rt->kind() == sema::TypeKind::Optional) {
                const auto& ed = static_cast<const ast::EnumDecl&>(*sym->decl);
                os << "__vstr::enum_from<";
                emit_sema_type(os, rt->inner());
                os << ">(";
                emit_expr(os, *c.args[0].value);
                os << ", " << ed.cases.size() << ")";
                break;
            }
        }
        // §18 `opt.take()` / `opt.put(v)` on an Optional place (F17).
        if (resolution_ != nullptr && c.callee->kind == ast::NodeKind::MemberExpr) {
            const auto& mem = static_cast<const ast::MemberExpr&>(*c.callee);
            auto bt = resolution_->type_of(mem.base.get());
            if (bt != nullptr && bt->kind() == sema::TypeKind::Optional
                && ((mem.member == "take" && c.args.empty())
                    || (mem.member == "put" && c.args.size() == 1))) {
                os << "__vstr::opt_" << mem.member << "(";
                emit_expr(os, *mem.base);
                if (mem.member == "put") {
                    // A named value moves in; a temporary already is an rvalue.
                    os << ", ";
                    if (is_lvalue_expr(*c.args[0].value)) {
                        os << "std::move(";
                        emit_expr(os, *c.args[0].value);
                        os << ")";
                    } else {
                        emit_expr(os, *c.args[0].value);
                    }
                }
                os << ")";
                break;
            }
        }
        // §A3 (§10.5) raw-mint builtins. Four shapes, all dispatched
        // by callee MemberExpr name. T comes from the resolver-typed
        // result for the pointer mints; from the inner-pointer's T
        // for the span-from-raw mints.
        if (c.callee && c.callee->kind == ast::NodeKind::MemberExpr && resolution_ != nullptr) {
            const auto& mem = static_cast<const ast::MemberExpr&>(*c.callee);
            if (mem.base != nullptr && mem.base->kind == ast::NodeKind::IdentExpr
                && mem.member == "unchecked" && c.args.size() == 1) {
                const auto& bi = static_cast<const ast::IdentExpr&>(*mem.base);
                const bool is_ptr = bi.name == "Ptr";
                const bool is_mut_ptr = bi.name == "MutPtr";
                if (is_ptr || is_mut_ptr) {
                    auto rt = resolution_->type_of(&e);
                    sema::TypePtr T = rt != nullptr
                                              && (rt->kind() == sema::TypeKind::Ptr
                                                  || rt->kind() == sema::TypeKind::MutPtr)
                                          ? rt->inner()
                                          : nullptr;
                    os << "reinterpret_cast<";
                    if (is_ptr) {
                        os << "const ";
                    }
                    if (T != nullptr) {
                        emit_sema_type(os, T);
                    } else {
                        os << "void";  // defensive: sema would have diagnosed
                    }
                    os << "*>(";
                    emit_expr(os, *c.args[0].value);
                    os << ")";
                    break;
                }
            }
            // §14.12.3 auto-emitted post-write barrier on gated
            // sysreg writes. The callee shape is `MemberExpr(base=
            // MemberExpr(IdentExpr("Sysreg"), <name>), "write")`;
            // when <name> is in the gated set and the enclosing
            // function isn't carrying `@no_auto_barrier`, route the
            // call to `__vstr::sysreg::write_<name>(v)` which
            // performs the cell update and then issues the
            // platform-appropriate barrier (ISB on aarch64, seq_cst
            // fence elsewhere).
            if (mem.member == "write" && c.args.size() == 1 && mem.base != nullptr
                && mem.base->kind == ast::NodeKind::MemberExpr) {
                const auto& base_mem = static_cast<const ast::MemberExpr&>(*mem.base);
                if (base_mem.base != nullptr && base_mem.base->kind == ast::NodeKind::IdentExpr) {
                    const auto& root = static_cast<const ast::IdentExpr&>(*base_mem.base);
                    if (root.name == "Sysreg" && sema::sysreg_write_is_gated(base_mem.member)
                        && !current_no_auto_barrier_) {
                        os << "__vstr::sysreg::write_" << base_mem.member << "(";
                        emit_expr(os, *c.args[0].value);
                        os << ")";
                        break;
                    }
                }
            }
            // §A6 (§14.11) `MmioView.at(ptr)` / `MmioRegion.at(ptr,
            // count)` / `MmioWireView.at(ptr, endian)` — wrap the MutPtr
            // in the matching __vstr template with a volatile cast on
            // the pointer so every read/write goes through a `volatile
            // T*` load/store. MmioWireView additionally pins the bus
            // endianness so the runtime knows whether to byteswap.
            if (mem.base != nullptr && mem.base->kind == ast::NodeKind::IdentExpr
                && mem.member == "at") {
                const auto& bi = static_cast<const ast::IdentExpr&>(*mem.base);
                const bool is_view = bi.name == "MmioView";
                const bool is_region = bi.name == "MmioRegion";
                const bool is_wire = bi.name == "MmioWireView";
                if (is_wire && c.args.size() == 2) {
                    auto rt = resolution_->type_of(&e);
                    sema::TypePtr T = rt != nullptr && rt->kind() == sema::TypeKind::MmioWireView
                                          ? rt->inner()
                                          : nullptr;
                    os << "__vstr::MmioWireView<";
                    if (T != nullptr) {
                        emit_sema_type(os, T);
                    } else {
                        os << "void";
                    }
                    os << ">{reinterpret_cast<";
                    if (T != nullptr) {
                        emit_sema_type(os, T);
                    }
                    os << " volatile*>(";
                    emit_expr(os, *c.args[0].value);
                    os << "), ";
                    emit_expr(os, *c.args[1].value);
                    os << "}";
                    break;
                }
                if ((is_view && c.args.size() == 1) || (is_region && c.args.size() == 2)) {
                    auto rt = resolution_->type_of(&e);
                    sema::TypePtr T = rt != nullptr
                                              && (rt->kind() == sema::TypeKind::MmioView
                                                  || rt->kind() == sema::TypeKind::MmioRegion)
                                          ? rt->inner()
                                          : nullptr;
                    if (is_view) {
                        os << "__vstr::MmioView<";
                    } else {
                        os << "__vstr::MmioRegion<";
                    }
                    if (T != nullptr) {
                        emit_sema_type(os, T);
                    } else {
                        os << "void";
                    }
                    os << ">{reinterpret_cast<";
                    if (T != nullptr) {
                        emit_sema_type(os, T);
                    }
                    os << " volatile*>(";
                    emit_expr(os, *c.args[0].value);
                    os << ")";
                    if (is_region) {
                        os << ", static_cast<std::intptr_t>(";
                        emit_expr(os, *c.args[1].value);
                        os << ")";
                    }
                    os << "}";
                    break;
                }
            }
            if (mem.base != nullptr && mem.base->kind == ast::NodeKind::IdentExpr
                && mem.member == "raw" && c.args.size() == 2) {
                const auto& bi = static_cast<const ast::IdentExpr&>(*mem.base);
                const bool is_span = bi.name == "Span";
                const bool is_mut_span = bi.name == "MutSpan";
                if (is_span || is_mut_span) {
                    auto rt = resolution_->type_of(&e);
                    sema::TypePtr T = rt != nullptr
                                              && (rt->kind() == sema::TypeKind::Span
                                                  || rt->kind() == sema::TypeKind::MutSpan)
                                          ? rt->inner()
                                          : nullptr;
                    os << "std::span<";
                    if (is_span) {
                        os << "const ";
                    }
                    if (T != nullptr) {
                        emit_sema_type(os, T);
                    } else {
                        os << "void";
                    }
                    os << ">(";
                    emit_expr(os, *c.args[0].value);
                    os << ", static_cast<std::size_t>(";
                    emit_expr(os, *c.args[1].value);
                    os << "))";
                    break;
                }
            }
        }
        // §A7 (§14.14) `Scheduler.swapContext(saving:, loading:)`
        // lowers to the __vstr runtime shim, which takes the two
        // pointers as they are (`Context*`, `const Context*`).
        if (c.callee && c.callee->kind == ast::NodeKind::MemberExpr) {
            const auto& mem = static_cast<const ast::MemberExpr&>(*c.callee);
            if (mem.base != nullptr && mem.base->kind == ast::NodeKind::IdentExpr
                && static_cast<const ast::IdentExpr&>(*mem.base).name == "Scheduler"
                && mem.member == "swapContext" && c.args.size() == 2) {
                os << "__vstr::scheduler_swap_context(";
                emit_expr(os, *c.args[0].value);
                os << ", ";
                emit_expr(os, *c.args[1].value);
                os << ")";
                break;
            }
            // §14.14 `Context.start(entry: f, stack: top)`.
            if (mem.base != nullptr && mem.base->kind == ast::NodeKind::IdentExpr
                && static_cast<const ast::IdentExpr&>(*mem.base).name == "Context"
                && mem.member == "start" && c.args.size() == 2) {
                os << "__vstr::context_start(";
                emit_expr(os, *c.args[0].value);
                os << ", static_cast<std::uint64_t>(";
                emit_expr(os, *c.args[1].value);
                os << "))";
                break;
            }
        }
        // §10 `Box.new(value)` lowers to `std::make_unique<T>(std::move(value))`.
        // The element type T comes from the resolver (the type of the
        // CallExpr itself is Box<T>; we peel that to get T). The argument is
        // always consumed — the box takes sole ownership of the pointee — so it
        // is moved in: required for a move-only T (e.g. a Box-containing
        // recursive enum node), and harmless otherwise (`std::move` on a
        // temporary just stays an rvalue; on a trivially-copyable T it copies).
        if (c.callee && c.callee->kind == ast::NodeKind::MemberExpr) {
            const auto& mem = static_cast<const ast::MemberExpr&>(*c.callee);
            if (mem.base && mem.base->kind == ast::NodeKind::IdentExpr
                && static_cast<const ast::IdentExpr&>(*mem.base).name == "Box"
                && mem.member == "new" && c.args.size() == 1) {
                os << "std::make_unique<";
                if (resolution_ != nullptr) {
                    auto rt = resolution_->type_of(&e);
                    if (rt != nullptr && rt->kind() == sema::TypeKind::Box) {
                        emit_sema_type(os, rt->inner());
                    } else {
                        emit_sema_type(os, resolution_->type_of(c.args[0].value.get()));
                    }
                } else {
                    os << "auto";
                }
                os << ">(std::move(";
                emit_expr(os, *c.args[0].value);
                os << "))";
                break;
            }
            // §11 `Channel.new()` mints a fresh queue: `__vstr::Channel<T>{}`
            // (T from the call's resolved Channel type). Any capacity
            // argument is ignored in v0.5 (the queue is unbounded).
            if (mem.base && mem.base->kind == ast::NodeKind::IdentExpr
                && static_cast<const ast::IdentExpr&>(*mem.base).name == "Channel"
                && mem.member == "new" && resolution_ != nullptr) {
                auto rt = resolution_->type_of(&e);
                if (rt != nullptr && rt->kind() == sema::TypeKind::Channel) {
                    os << "__vstr::Channel<";
                    emit_sema_type(os, rt->inner());
                    os << ">{}";
                    break;
                }
            }
            // §18.5 `Vec.new()` → an empty `std::vector<T>{}` (T from the call's
            // resolved Vec type).
            if (mem.base && mem.base->kind == ast::NodeKind::IdentExpr
                && static_cast<const ast::IdentExpr&>(*mem.base).name == "Vec"
                && mem.member == "new" && resolution_ != nullptr) {
                auto rt = resolution_->type_of(&e);
                if (rt != nullptr && rt->kind() == sema::TypeKind::Vec) {
                    os << "std::vector<";
                    emit_sema_type(os, rt->inner());
                    os << ">{}";
                    break;
                }
            }
            // §18.5 `String.new()` → an empty `std::string{}`.
            if (mem.base && mem.base->kind == ast::NodeKind::IdentExpr
                && static_cast<const ast::IdentExpr&>(*mem.base).name == "String"
                && mem.member == "new") {
                os << "std::string{}";
                break;
            }
            // §18.5 `HashMap.new()` → an empty `std::unordered_map<K, V>{}`.
            if (mem.base && mem.base->kind == ast::NodeKind::IdentExpr
                && static_cast<const ast::IdentExpr&>(*mem.base).name == "HashMap"
                && mem.member == "new" && resolution_ != nullptr) {
                auto rt = resolution_->type_of(&e);
                if (rt != nullptr && rt->kind() == sema::TypeKind::HashMap
                    && rt->parts().size() == 2) {
                    os << "std::unordered_map<";
                    emit_sema_type(os, rt->parts()[0]);
                    os << ", ";
                    emit_sema_type(os, rt->parts()[1]);
                    os << ">{}";
                    break;
                }
            }
            // §13 `Soa.new()` → an empty struct-of-arrays (tuple of empty
            // vectors), spelled by the call's resolved Soa type.
            if (mem.base && mem.base->kind == ast::NodeKind::IdentExpr
                && static_cast<const ast::IdentExpr&>(*mem.base).name == "Soa"
                && mem.member == "new" && resolution_ != nullptr) {
                auto rt = resolution_->type_of(&e);
                if (rt != nullptr && rt->kind() == sema::TypeKind::Soa) {
                    emit_sema_type(os, rt);
                    os << "{}";
                    break;
                }
            }
            // §11 `Duration.seconds(n)` (also milliseconds / microseconds /
            // nanoseconds) lowers to the static factory
            // `__vstr::Duration::seconds(n)`. Guarded on the call resolving to
            // Duration so it can't shadow an unrelated `.member(...)` call.
            if (mem.base && mem.base->kind == ast::NodeKind::IdentExpr
                && static_cast<const ast::IdentExpr&>(*mem.base).name == "Duration"
                && c.args.size() == 1 && resolution_ != nullptr
                && resolution_->type_of(&e) != nullptr
                && resolution_->type_of(&e)->kind() == sema::TypeKind::Duration) {
                os << "__vstr::Duration::" << mem.member << "(";
                emit_expr(os, *c.args[0].value);
                os << ")";
                break;
            }
            // §A11 (§14.8) `PerCpu.new(value)` heap-allocates a
            // 64-byte-aligned __vstr::PerCpu<T> wrapping `value` and
            // returns a unique_ptr. The brace-init around `value`
            // matches the runtime template's aggregate shape; the
            // surrounding make_unique<__vstr::PerCpu<T>>(...) does
            // the alloc + ctor in one step.
            if (mem.base && mem.base->kind == ast::NodeKind::IdentExpr
                && static_cast<const ast::IdentExpr&>(*mem.base).name == "PerCpu"
                && mem.member == "new" && c.args.size() == 1) {
                sema::TypePtr T = nullptr;
                if (resolution_ != nullptr) {
                    auto rt = resolution_->type_of(&e);
                    if (rt != nullptr && rt->kind() == sema::TypeKind::Box && rt->inner() != nullptr
                        && rt->inner()->kind() == sema::TypeKind::PerCpu) {
                        T = rt->inner()->inner();
                    }
                    if (T == nullptr) {
                        T = resolution_->type_of(c.args[0].value.get());
                    }
                }
                os << "std::make_unique<__vstr::PerCpu<";
                emit_sema_type(os, T);
                os << ">>(__vstr::PerCpu<";
                emit_sema_type(os, T);
                os << ">{";
                emit_expr(os, *c.args[0].value);
                os << "})";
                break;
            }
        }
        // §A4 (§14.9.3) `atomic.compareExchange{,Weak}(expected,
        // desired, success, failure)` lowers to an IIFE that mutates
        // a local copy of `expected`, calls std::atomic's
        // `.compare_exchange_strong` / `.compare_exchange_weak`, and
        // bundles the ok-flag plus the post-call `expected` into a
        // `__vstr::CASResult<T>`. Sema gates the weak form to
        // recognized retry-loop shapes; codegen just dispatches by
        // name.
        if (c.callee && c.callee->kind == ast::NodeKind::MemberExpr && resolution_ != nullptr) {
            const auto& mem = static_cast<const ast::MemberExpr&>(*c.callee);
            auto base_t = resolution_->type_of(mem.base.get());
            const bool is_strong = mem.member == "compareExchange";
            const bool is_weak = mem.member == "compareExchangeWeak";
            if (base_t != nullptr && base_t->kind() == sema::TypeKind::Atomic
                && (is_strong || is_weak) && c.args.size() == 4) {
                os << "([&]{ ";
                emit_sema_type(os, base_t->inner());
                os << " __vstr_e = ";
                emit_expr(os, *c.args[0].value);
                // §14.9.1 an ordering the compiler can't see is checked as a
                // pair before the exchange runs.
                const bool guarded = resolution_->cas_guarded(&c);
                if (guarded) {
                    os << "; const std::memory_order __vstr_s = ";
                    emit_expr(os, *c.args[2].value);
                    os << "; const std::memory_order __vstr_f = __vstr::cas_failure_order(__vstr_s, ";
                    emit_expr(os, *c.args[3].value);
                    os << ")";
                }
                os << "; bool __vstr_ok = ";
                emit_expr(os, *mem.base);
                os << (is_strong ? ".compare_exchange_strong(__vstr_e, "
                                 : ".compare_exchange_weak(__vstr_e, ");
                emit_expr(os, *c.args[1].value);
                if (guarded) {
                    os << ", __vstr_s, __vstr_f";
                } else {
                    os << ", ";
                    emit_expr(os, *c.args[2].value);
                    os << ", ";
                    emit_expr(os, *c.args[3].value);
                }
                os << "); return __vstr::CASResult<";
                emit_sema_type(os, base_t->inner());
                os << ">{__vstr_ok, __vstr_e}; }())";
                break;
            }
            // §A4 (§14.9.5) AtomicTaggedPointer compareExchange — the
            // template's member already returns CASResult<std::tuple<T*,
            // uint64_t>> and auto-bumps the tag, so the lowering is a
            // direct member-method call rather than an IIFE.
            if (base_t != nullptr && base_t->kind() == sema::TypeKind::AtomicTaggedPointer
                && is_strong && c.args.size() == 5) {
                if (resolution_->cas_guarded(&c)) {
                    os << "([&]{ const std::memory_order __vstr_s = ";
                    emit_expr(os, *c.args[3].value);
                    os << "; const std::memory_order __vstr_f = __vstr::cas_failure_order(__vstr_s, ";
                    emit_expr(os, *c.args[4].value);
                    os << "); return ";
                    emit_expr(os, *mem.base);
                    os << ".compare_exchange_strong(";
                    for (std::size_t i = 0; i < 3; ++i) {
                        emit_expr(os, *c.args[i].value);
                        os << ", ";
                    }
                    os << "__vstr_s, __vstr_f); }())";
                    break;
                }
                emit_expr(os, *mem.base);
                os << ".compare_exchange_strong(";
                for (std::size_t i = 0; i < c.args.size(); ++i) {
                    if (i != 0) {
                        os << ", ";
                    }
                    emit_expr(os, *c.args[i].value);
                }
                os << ")";
                break;
            }
        }
        // §18.5 Vec[T] methods: `push(x)` → `.push_back(x)`; `len()` →
        // `.size()` retyped to Int (std::intptr_t).
        if (resolution_ != nullptr && c.callee && c.callee->kind == ast::NodeKind::MemberExpr) {
            const auto& mem = static_cast<const ast::MemberExpr&>(*c.callee);
            if (auto base_t = resolution_->type_of(mem.base.get());
                base_t != nullptr && base_t->kind() == sema::TypeKind::Vec) {
                if (mem.member == "push" && c.args.size() == 1) {
                    emit_expr(os, *mem.base);
                    os << ".push_back(";
                    emit_expr(os, *c.args[0].value);
                    os << ")";
                    break;
                }
                if (mem.member == "len" && c.args.empty()) {
                    os << "static_cast<std::intptr_t>(";
                    emit_expr(os, *mem.base);
                    os << ".size())";
                    break;
                }
                if (mem.member == "get" && c.args.size() == 1) {
                    os << "__vstr::vec_get(";
                    emit_expr(os, *mem.base);
                    os << ", ";
                    emit_expr(os, *c.args[0].value);
                    os << ")";
                    break;
                }
                if (mem.member == "pop" && c.args.empty()) {
                    os << "__vstr::vec_pop(";
                    emit_expr(os, *mem.base);
                    os << ")";
                    break;
                }
                if (mem.member == "set" && c.args.size() == 2) {
                    emit_expr(os, *mem.base);
                    os << "[static_cast<std::size_t>(";
                    emit_expr(os, *c.args[0].value);
                    os << ")] = ";
                    emit_expr(os, *c.args[1].value);
                    break;
                }
                if (mem.member == "clear" && c.args.empty()) {
                    emit_expr(os, *mem.base);
                    os << ".clear()";
                    break;
                }
            }
            // §13 Soa[T] methods. The backing is a tuple of column vectors:
            //   push(v) scatters v's fields into the columns (an IIFE binds the
            //     receiver + value once, then push_back per column);
            //   len()   is column 0's size as Int;
            //   get(i)  gathers the i-th of each column back into a T (an IIFE
            //     binds the receiver + index once, then a designated-init).
            if (auto base_t = resolution_->type_of(mem.base.get());
                base_t != nullptr && base_t->kind() == sema::TypeKind::Soa
                && base_t->inner() != nullptr && base_t->inner()->nominal_decl() != nullptr
                && base_t->inner()->nominal_decl()->kind == ast::NodeKind::Struct) {
                const auto& sd =
                    static_cast<const ast::StructDecl&>(*base_t->inner()->nominal_decl());
                std::vector<const ast::StructDecl::Field*> cols;
                for (const auto& f : sd.fields) {
                    if (f.kind != ast::StructDecl::Field::Kind::Embed && f.type != nullptr) {
                        cols.push_back(&f);
                    }
                }
                if (mem.member == "push" && c.args.size() == 1) {
                    os << "[&](auto&& __s, auto&& __v) { ";
                    for (std::size_t i = 0; i < cols.size(); ++i) {
                        os << "std::get<" << i << ">(__s).push_back(__v." << cols[i]->name << "); ";
                    }
                    os << "}(";
                    emit_expr(os, *mem.base);
                    os << ", ";
                    emit_expr(os, *c.args[0].value);
                    os << ")";
                    break;
                }
                if (mem.member == "len" && c.args.empty()) {
                    os << "static_cast<std::intptr_t>(std::get<0>(";
                    emit_expr(os, *mem.base);
                    os << ").size())";
                    break;
                }
                if (mem.member == "column" && c.args.size() == 1
                    && c.args[0].value->kind == ast::NodeKind::LeadingDotExpr) {
                    const auto& sel = static_cast<const ast::LeadingDotExpr&>(*c.args[0].value);
                    std::size_t ci = 0;
                    bool found = false;
                    for (const auto* col : cols) {
                        if (col->name == sel.name) {
                            found = true;
                            break;
                        }
                        ++ci;
                    }
                    if (found) {
                        // MutSpan over the column vector — CTAD deduces
                        // std::span<T> from the (mutable) std::vector<T>.
                        os << "std::span{std::get<" << ci << ">(";
                        emit_expr(os, *mem.base);
                        os << ")}";
                        break;
                    }
                }
                if (mem.member == "get" && c.args.size() == 1) {
                    os << "[&](auto&& __s, std::size_t __i) { return ";
                    emit_sema_type(os, base_t->inner());
                    os << "{";
                    for (std::size_t i = 0; i < cols.size(); ++i) {
                        if (i != 0) {
                            os << ", ";
                        }
                        os << "." << cols[i]->name << " = std::get<" << i << ">(__s)[__i]";
                    }
                    os << "}; }(";
                    emit_expr(os, *mem.base);
                    os << ", static_cast<std::size_t>(";
                    emit_expr(os, *c.args[0].value);
                    os << "))";
                    break;
                }
            }
            // §18.5 String mutator: `append(x)` → `.append(x)` (std::string's
            // string_view overload takes a Str/StrConst arg directly). Owned
            // String receiver only.
            if (auto base_t = resolution_->type_of(mem.base.get());
                base_t != nullptr && base_t->kind() == sema::TypeKind::String) {
                if (mem.member == "append" && c.args.size() == 1) {
                    emit_expr(os, *mem.base);
                    os << ".append(";
                    emit_expr(os, *c.args[0].value);
                    os << ")";
                    break;
                }
            }
            // §18.5 String/Str read-only queries. The owned std::string and the
            // borrowed std::string_view both carry these members, and the
            // `__vstr::str_*` helpers take a string_view (a std::string receiver
            // converts implicitly), so one lowering serves all three string
            // kinds. len/isEmpty/contains/startsWith/endsWith map to the
            // matching member; slice/byteAt/find go through the clamping
            // preamble helpers (returning Str / UInt8? / Int?).
            if (auto base_t = resolution_->type_of(mem.base.get());
                base_t != nullptr
                && (base_t->kind() == sema::TypeKind::String
                    || base_t->kind() == sema::TypeKind::Str
                    || base_t->kind() == sema::TypeKind::StrConst)) {
                if (mem.member == "len" && c.args.empty()) {
                    os << "static_cast<std::intptr_t>(";
                    emit_expr(os, *mem.base);
                    os << ".size())";
                    break;
                }
                if (mem.member == "isEmpty" && c.args.empty()) {
                    emit_expr(os, *mem.base);
                    os << ".empty()";
                    break;
                }
                if (mem.member == "slice" && c.args.size() == 2) {
                    os << "__vstr::str_slice(";
                    emit_expr(os, *mem.base);
                    os << ", static_cast<std::intptr_t>(";
                    emit_expr(os, *c.args[0].value);
                    os << "), static_cast<std::intptr_t>(";
                    emit_expr(os, *c.args[1].value);
                    os << "))";
                    break;
                }
                if (mem.member == "bytes" && c.args.empty()) {
                    // Inline (no prelude helper), so it needs nothing hosted.
                    os << "[](std::string_view __vstr_s) { return std::span<const std::uint8_t>("
                          "reinterpret_cast<const std::uint8_t*>(__vstr_s.data()), "
                          "__vstr_s.size()); }(";
                    emit_expr(os, *mem.base);
                    os << ")";
                    break;
                }
                if (mem.member == "byteAt" && c.args.size() == 1) {
                    os << "__vstr::str_byte_at(";
                    emit_expr(os, *mem.base);
                    os << ", static_cast<std::intptr_t>(";
                    emit_expr(os, *c.args[0].value);
                    os << "))";
                    break;
                }
                if (mem.member == "find" && c.args.size() == 1) {
                    os << "__vstr::str_find(";
                    emit_expr(os, *mem.base);
                    os << ", ";
                    emit_expr(os, *c.args[0].value);
                    os << ")";
                    break;
                }
                if (mem.member == "toInt" && c.args.empty()) {
                    os << "__vstr::parse_int(";
                    emit_expr(os, *mem.base);
                    os << ")";
                    break;
                }
                if (mem.member == "split" && c.args.size() == 1) {
                    os << "__vstr::split(";
                    emit_expr(os, *mem.base);
                    os << ", ";
                    emit_expr(os, *c.args[0].value);
                    os << ")";
                    break;
                }
                if (mem.member == "charAt" && c.args.size() == 1) {
                    os << "__vstr::char_at(";
                    emit_expr(os, *mem.base);
                    os << ", static_cast<std::intptr_t>(";
                    emit_expr(os, *c.args[0].value);
                    os << "))";
                    break;
                }
                if (mem.member == "chars" && c.args.empty()) {
                    os << "__vstr::chars(";
                    emit_expr(os, *mem.base);
                    os << ")";
                    break;
                }
                if ((mem.member == "contains" || mem.member == "startsWith"
                     || mem.member == "endsWith")
                    && c.args.size() == 1) {
                    emit_expr(os, *mem.base);
                    os << (mem.member == "contains"     ? ".contains("
                           : mem.member == "startsWith" ? ".starts_with("
                                                        : ".ends_with(");
                    emit_expr(os, *c.args[0].value);
                    os << ")";
                    break;
                }
            }
            // §18.5 `n.toString()` → `__vstr::to_string(n)` (std::format-backed);
            // the receiver is any standard numeric primitive.
            if (auto base_t = resolution_->type_of(mem.base.get());
                base_t != nullptr && base_t->is_numeric() && mem.member == "toString"
                && c.args.empty()) {
                os << "__vstr::to_string(";
                emit_expr(os, *mem.base);
                os << ")";
                break;
            }
            // §18.1 `a.compare(b)` with no user `compare` → `__vstr::compare(a, b)`.
            if (c.args.size() == 1 && mem.member == "compare" && callee_decl(c) == nullptr) {
                if (auto rt = resolution_->type_of(&c); rt != nullptr
                                                       && rt->kind() == sema::TypeKind::Enum
                                                       && rt->nominal_decl() != nullptr
                                                       && static_cast<const ast::EnumDecl*>(
                                                              rt->nominal_decl())
                                                                  ->name
                                                              == "Ordering") {
                    os << "__vstr::compare(";
                    emit_expr(os, *mem.base);
                    os << ", ";
                    emit_expr(os, *c.args[0].value);
                    os << ")";
                    break;
                }
            }
            // §4 `a.addingChecked(b)` → `__vstr::add_checked(a, b)`, a
            // `std::tuple<T, bool>` of the wrapped result and the overflow.
            if (auto base_t = resolution_->type_of(mem.base.get());
                base_t != nullptr && base_t->is_integer() && c.args.size() == 1
                && (mem.member == "addingChecked" || mem.member == "subtractingChecked"
                    || mem.member == "multipliedChecked")) {
                os << (mem.member == "addingChecked"        ? "__vstr::add_checked("
                       : mem.member == "subtractingChecked" ? "__vstr::sub_checked("
                                                            : "__vstr::mul_checked(");
                emit_expr(os, *mem.base);
                os << ", ";
                emit_expr(os, *c.args[0].value);
                os << ")";
                break;
            }
            // §18.5 Char classification → the matching `__vstr::is_*` ASCII
            // predicate (isDigit / isAlpha / isAlnum / isSpace).
            if (auto base_t = resolution_->type_of(mem.base.get());
                base_t != nullptr && base_t->kind() == sema::TypeKind::Char && c.args.empty()
                && (mem.member == "isDigit" || mem.member == "isAlpha" || mem.member == "isAlnum"
                    || mem.member == "isSpace")) {
                os << (mem.member == "isDigit"   ? "__vstr::is_digit("
                       : mem.member == "isAlpha" ? "__vstr::is_alpha("
                       : mem.member == "isAlnum" ? "__vstr::is_alnum("
                                                 : "__vstr::is_space(");
                emit_expr(os, *mem.base);
                os << ")";
                break;
            }
            // §18.5 HashMap methods: `set(k, v)` → `.insert_or_assign(k, v)`;
            // `get(k)` → `__vstr::map_get(m, k)` yielding `std::optional<V>`;
            // `contains(k)` → `.contains(k)`; `len()` → `.size()` as Int.
            if (auto base_t = resolution_->type_of(mem.base.get());
                base_t != nullptr && base_t->kind() == sema::TypeKind::HashMap) {
                if (mem.member == "set" && c.args.size() == 2) {
                    emit_expr(os, *mem.base);
                    os << ".insert_or_assign(";
                    emit_expr(os, *c.args[0].value);
                    os << ", ";
                    emit_expr(os, *c.args[1].value);
                    os << ")";
                    break;
                }
                if (mem.member == "get" && c.args.size() == 1) {
                    os << "__vstr::map_get(";
                    emit_expr(os, *mem.base);
                    os << ", ";
                    emit_expr(os, *c.args[0].value);
                    os << ")";
                    break;
                }
                if (mem.member == "contains" && c.args.size() == 1) {
                    emit_expr(os, *mem.base);
                    os << ".contains(";
                    emit_expr(os, *c.args[0].value);
                    os << ")";
                    break;
                }
                if (mem.member == "len" && c.args.empty()) {
                    os << "static_cast<std::intptr_t>(";
                    emit_expr(os, *mem.base);
                    os << ".size())";
                    break;
                }
                // §18.5 iteration: keys/values/entries → the matching __vstr
                // helper, each returning an owned Vec (of K / V / tuple<K,V>).
                if ((mem.member == "keys" || mem.member == "values" || mem.member == "entries")
                    && c.args.empty()) {
                    os << (mem.member == "keys"     ? "__vstr::map_keys("
                           : mem.member == "values" ? "__vstr::map_values("
                                                    : "__vstr::map_entries(");
                    emit_expr(os, *mem.base);
                    os << ")";
                    break;
                }
            }
        }
        // §9 `result.mapError(f)` lowers to std::expected's
        // `.transform_error(f)` — the closure runs on the error path
        // and produces a new Result<T, E'>. Symmetric with the sema
        // intercept; sema has already verified the closure has the
        // right `(E) -> E'` shape.
        if (c.callee && c.callee->kind == ast::NodeKind::MemberExpr) {
            const auto& mem = static_cast<const ast::MemberExpr&>(*c.callee);
            if (mem.member == "mapError" && c.args.size() == 1 && resolution_ != nullptr) {
                auto base_t = resolution_->type_of(mem.base.get());
                if (base_t != nullptr && base_t->kind() == sema::TypeKind::Result) {
                    os << "(";
                    emit_expr(os, *mem.base);
                    os << ").transform_error(";
                    emit_expr(os, *c.args[0].value);
                    os << ")";
                    break;
                }
            }
            // §5/§18.4 `s.split(at: i)` → the runtime partitioner, which
            // returns the (prefix, suffix) tuple of std::span sub-views.
            if (mem.member == "split" && c.args.size() == 1 && resolution_ != nullptr) {
                auto base_t = resolution_->type_of(mem.base.get());
                if (base_t != nullptr
                    && (base_t->kind() == sema::TypeKind::Span
                        || base_t->kind() == sema::TypeKind::MutSpan)) {
                    os << "__vstr::split_at(";
                    emit_expr(os, *mem.base);
                    os << ", static_cast<std::intptr_t>(";
                    emit_expr(os, *c.args[0].value);
                    os << "))";
                    break;
                }
            }
            // §5/§18.4 `s.chunks(of: n)` → the chunk iterator, which yields
            // consecutive std::span sub-views (the last shorter than n when
            // the length isn't a multiple). CTAD deduces the span type.
            if (mem.member == "chunks" && c.args.size() == 1 && resolution_ != nullptr) {
                auto base_t = resolution_->type_of(mem.base.get());
                if (base_t != nullptr
                    && (base_t->kind() == sema::TypeKind::Span
                        || base_t->kind() == sema::TypeKind::MutSpan)) {
                    os << "__vstr::Chunks{";
                    emit_expr(os, *mem.base);
                    os << ", static_cast<std::size_t>(";
                    emit_expr(os, *c.args[0].value);
                    os << ")}";
                    break;
                }
            }
        }
        // §10 panic primitives — `panic(msg)` / `abort()` / `unreachable()`
        // intercepted by IdentExpr callee name. The Vestra-side type is
        // `Never`, which is assignable to any slot. The C++ shims at the
        // top of the header return `__vstr::Never`, whose templated
        // `[[noreturn]]` conversion op slots into whatever C++ type the
        // surrounding expression expects.
        if (c.callee && c.callee->kind == ast::NodeKind::IdentExpr) {
            const auto& callee_ident = static_cast<const ast::IdentExpr&>(*c.callee);
            if (callee_ident.name == "panic" && c.args.size() == 1) {
                os << "__vstr::panic(";
                emit_expr(os, *c.args[0].value);
                os << ")";
                break;
            }
            if (callee_ident.name == "abort" && c.args.empty()) {
                os << "__vstr::abort_fn()";
                break;
            }
            if (callee_ident.name == "unreachable" && c.args.empty()) {
                os << "__vstr::unreachable_fn()";
                break;
            }
            // §18 `print` / `println` (stdout) and `eprint` / `eprintln`
            // (stderr) → std::print / std::println, the stderr pair with a
            // leading `stderr` stream argument. The text is passed as a
            // formatting *argument* (not the format string), so a `{` in it is
            // harmless. Guarded on the builtin symbol (decl == nullptr) so a
            // user-defined print isn't intercepted.
            if ((callee_ident.name == "print" || callee_ident.name == "println"
                 || callee_ident.name == "eprint" || callee_ident.name == "eprintln")
                && c.args.size() == 1) {
                const auto* sym =
                    resolution_ != nullptr ? resolution_->symbol_of(c.callee.get()) : nullptr;
                if (sym != nullptr && sym->decl == nullptr) {
                    const bool err =
                        callee_ident.name == "eprint" || callee_ident.name == "eprintln";
                    const bool ln =
                        callee_ident.name == "println" || callee_ident.name == "eprintln";
                    os << (ln ? "std::println(" : "std::print(");
                    if (err) {
                        os << "stderr, ";
                    }
                    os << "\"{}\", ";
                    emit_expr(os, *c.args[0].value);
                    os << ")";
                    break;
                }
            }
            // §18 `readFile(path)` -> __vstr::read_file (std::optional<std::string>);
            // `writeFile(path, contents)` -> __vstr::write_file (bool). Guarded
            // on the builtin symbol so a user definition isn't intercepted.
            if (callee_ident.name == "readFile" && c.args.size() == 1) {
                const auto* sym =
                    resolution_ != nullptr ? resolution_->symbol_of(c.callee.get()) : nullptr;
                if (sym != nullptr && sym->decl == nullptr) {
                    os << "__vstr::read_file(";
                    emit_expr(os, *c.args[0].value);
                    os << ")";
                    break;
                }
            }
            if (callee_ident.name == "writeFile" && c.args.size() == 2) {
                const auto* sym =
                    resolution_ != nullptr ? resolution_->symbol_of(c.callee.get()) : nullptr;
                if (sym != nullptr && sym->decl == nullptr) {
                    os << "__vstr::write_file(";
                    emit_expr(os, *c.args[0].value);
                    os << ", ";
                    emit_expr(os, *c.args[1].value);
                    os << ")";
                    break;
                }
            }
            // §18 `args()` -> __vstr::args() (a copy of the captured argv).
            if (callee_ident.name == "args" && c.args.empty()) {
                const auto* sym =
                    resolution_ != nullptr ? resolution_->symbol_of(c.callee.get()) : nullptr;
                if (sym != nullptr && sym->decl == nullptr) {
                    os << "__vstr::args()";
                    break;
                }
            }
            // §A5 (§14.10) sync-intrinsic builtins. Each one lowers
            // to its matching __vstr runtime shim. The names match
            // the Vestra-side spelling 1:1 except `nop` which
            // collides with NaN literals + a frequent local-variable
            // name on the C++ side; emit it as `cpu_nop` to keep
            // host symbols out of the way.
            static const std::unordered_map<std::string_view, std::string_view> sync_intrinsics = {
                {"compilerFence", "compilerFence"},
                {"memoryBarrier", "memoryBarrier"},
                {"syncBarrier", "syncBarrier"},
                {"instructionBarrier", "instructionBarrier"},
                {"waitForInterrupt", "waitForInterrupt"},
                {"waitForEvent", "waitForEvent"},
                {"signalEvent", "signalEvent"},
                {"relax", "relax"},
                {"nop", "cpu_nop"},
                // §A5 (§14.10.3 / §14.10.4 / §14.10.5) cache + TLB ops.
                {"cleanData", "cleanData"},
                {"invalidateData", "invalidateData"},
                {"cleanInvalidateData", "cleanInvalidateData"},
                {"zeroData", "zeroData"},
                {"publishInstructions", "publishInstructions"},
                {"invalidateAllInstructions", "invalidateAllInstructions"},
                {"tlbInvalidateAll", "tlbInvalidateAll"},
                {"tlbInvalidatePage", "tlbInvalidatePage"},
                {"tlbInvalidateAsid", "tlbInvalidateAsid"},
            };
            if (auto it = sync_intrinsics.find(callee_ident.name); it != sync_intrinsics.end()) {
                os << "__vstr::" << it->second << "(";
                for (std::size_t i = 0; i < c.args.size(); ++i) {
                    if (i != 0) {
                        os << ", ";
                    }
                    emit_expr(os, *c.args[i].value);
                }
                os << ")";
                break;
            }
            // §A10 (§15.4) compiler-emitted intrinsics — `memcpy`,
            // `memset`, `memmove`. Each routes to its `__builtin_*`
            // form, which the host compiler resolves to an inlined
            // expansion or a libc-compatible call (no libc reference
            // is emitted at the freestanding profile when the host
            // compiler picks the inline path, which is the load-
            // bearing property §15.5's `no_libc = true` will
            // eventually enforce). The Vestra-side signature uses
            // typed pointers (MutPtr[UInt8] / Ptr[UInt8]); the
            // intrinsics take `void*` / `const void*` so the
            // implicit C++ pointer decay does the conversion.
            static const std::unordered_map<std::string_view, std::string_view> mem_intrinsics = {
                {"memcpy", "__builtin_memcpy"},
                {"memset", "__builtin_memset"},
                {"memmove", "__builtin_memmove"},
            };
            const auto* mem_sym =
                resolution_ != nullptr ? resolution_->symbol_of(c.callee.get()) : nullptr;
            if (auto it = mem_intrinsics.find(callee_ident.name);
                it != mem_intrinsics.end() && (mem_sym == nullptr || mem_sym->decl == nullptr)) {
                os << it->second << "(";
                for (std::size_t i = 0; i < c.args.size(); ++i) {
                    if (i != 0) {
                        os << ", ";
                    }
                    // The trailing length argument widens to
                    // std::size_t so a Vestra `Int` (intptr_t) flows
                    // through the `size_t` slot without a narrowing
                    // diagnostic when the user passes a negative
                    // literal — sema rejected that already, but the
                    // explicit cast also covers the platform where
                    // intptr_t and size_t disagree on signedness.
                    if (i + 1 == c.args.size()) {
                        os << "static_cast<std::size_t>(";
                        emit_expr(os, *c.args[i].value);
                        os << ")";
                    } else {
                        emit_expr(os, *c.args[i].value);
                    }
                }
                os << ")";
                break;
            }
            // §9 iterator combinators: `zip(a, b)` / `take(xs, n)`
            // recognized by the resolver as builtin types
            // (ZipIter / TakeIter). The lowering uses CTAD so we
            // don't have to spell the underlying iterator types — the
            // C++ side deduces them from the argument expressions.
            // The take count widens to std::int64_t to match Take's
            // `remaining` slot regardless of the Vestra-side integer
            // width.
            if (resolution_ != nullptr) {
                auto result_t = resolution_->type_of(&e);
                if (result_t != nullptr && callee_ident.name == "zip"
                    && result_t->kind() == sema::TypeKind::ZipIter && c.args.size() == 2) {
                    os << "__vstr::Zip{";
                    emit_expr(os, *c.args[0].value);
                    os << ", ";
                    emit_expr(os, *c.args[1].value);
                    os << "}";
                    break;
                }
                if (result_t != nullptr && callee_ident.name == "take"
                    && result_t->kind() == sema::TypeKind::TakeIter && c.args.size() == 2) {
                    os << "__vstr::Take{";
                    emit_expr(os, *c.args[0].value);
                    os << ", static_cast<std::int64_t>(";
                    emit_expr(os, *c.args[1].value);
                    os << ")}";
                    break;
                }
                if (result_t != nullptr && callee_ident.name == "map"
                    && result_t->kind() == sema::TypeKind::MapIter && c.args.size() == 2) {
                    os << "__vstr::Map{";
                    emit_expr(os, *c.args[0].value);
                    os << ", ";
                    emit_expr(os, *c.args[1].value);
                    os << "}";
                    break;
                }
                if (result_t != nullptr && callee_ident.name == "filter"
                    && result_t->kind() == sema::TypeKind::FilterIter && c.args.size() == 2) {
                    os << "__vstr::Filter{";
                    emit_expr(os, *c.args[0].value);
                    os << ", ";
                    emit_expr(os, *c.args[1].value);
                    os << "}";
                    break;
                }
                // §11.2 `parallel(data, chunks, body)` → the runtime helper
                // that splits `data` into `chunks` disjoint sub-spans and
                // calls the worker on each. The chunk count widens to
                // std::intptr_t to match the helper's parameter.
                if (callee_ident.name == "parallel" && c.args.size() == 3
                    && resolution_->symbol_of(c.callee.get()) == nullptr) {
                    os << "__vstr::parallel(";
                    emit_expr(os, *c.args[0].value);
                    os << ", static_cast<std::intptr_t>(";
                    emit_expr(os, *c.args[1].value);
                    os << "), ";
                    emit_expr(os, *c.args[2].value);
                    os << ")";
                    break;
                }
            }
        }
        // §12.3 derive(Default) construction: `T.default()` lowers to
        // `T{}` (C++ value-init), which zero-initialises every field by
        // virtue of each field's own brace-init in emit_struct. Sema
        // gates this on the target deriving Default and on every field
        // being Default-conformant; the codegen just emits the value.
        if (resolution_ != nullptr && c.callee && c.callee->kind == ast::NodeKind::MemberExpr) {
            const auto& mem = static_cast<const ast::MemberExpr&>(*c.callee);
            if (mem.base != nullptr && mem.member == "default" && c.args.empty()) {
                if (const auto* sym = resolution_->symbol_of(mem.base.get());
                    sym != nullptr && sym->kind == sema::SymbolKind::Struct
                    && sym->decl != nullptr) {
                    const auto& sd = static_cast<const ast::StructDecl&>(*sym->decl);
                    os << sd.name << "{}";
                    break;
                }
            }
        }
        // Payloaded-enum case construction: `Shape.circle(radius: 1.0)` →
        // `Shape{Shape::circle_t{1.0}}`. Bare cases on a payloaded enum
        // come through the MemberExpr path above; here we handle the
        // call form so the args slot into the right `case_t` aggregate.
        if (resolution_ != nullptr && c.callee->kind == ast::NodeKind::MemberExpr) {
            const auto& mem = static_cast<const ast::MemberExpr&>(*c.callee);
            // The enum is named locally (`Shape`) or through its module
            // (`geo.Shape`, F19); sema binds the enum's symbol to either.
            if (mem.base->kind == ast::NodeKind::IdentExpr
                || mem.base->kind == ast::NodeKind::MemberExpr) {
                if (const auto* sym = resolution_->symbol_of(mem.base.get())) {
                    if (sym->kind == sema::SymbolKind::Enum && sym->decl != nullptr) {
                        const auto& ed = static_cast<const ast::EnumDecl&>(*sym->decl);
                        if (enum_is_sum_type(ed)) {
                            // §7 generics phase 2 — for a generic enum the
                            // wrapper and the `::case_t` qualifier both need
                            // the `<args>` from the call's resolved instance
                            // type (`Option<Int32>{Option<Int32>::some_t{…}}`).
                            auto inst = resolution_->type_of(&c);
                            const bool generic_inst = inst != nullptr
                                                      && inst->kind() == sema::TypeKind::Enum
                                                      && !inst->parts().empty();
                            auto emit_enum_name = [&]() {
                                if (generic_inst) {
                                    emit_sema_type(os, inst);
                                } else {
                                    os << nominal_prefix(&ed) << ed.name;
                                }
                            };
                            emit_enum_name();
                            os << "{";
                            emit_enum_name();
                            os << "::" << mem.member << "_t{";
                            for (std::size_t i = 0; i < c.args.size(); ++i) {
                                if (i != 0) {
                                    os << ", ";
                                }
                                emit_value(os, *c.args[i].value);  // §10 move move-only payloads
                            }
                            os << "}}";
                            break;
                        }
                    }
                }
            }
        }
        // §11 leading-dot Duration factory: `.seconds(10)` in a Duration
        // context (a `: Duration` binding, a `timeout` arm, …) lowers to the
        // static factory `__vstr::Duration::seconds(10)`.
        if (resolution_ != nullptr && c.callee->kind == ast::NodeKind::LeadingDotExpr) {
            auto inst = resolution_->type_of(&c);
            if (inst != nullptr && inst->kind() == sema::TypeKind::Duration) {
                const auto& d = static_cast<const ast::LeadingDotExpr&>(*c.callee);
                os << "__vstr::Duration::" << d.name << "(";
                for (std::size_t i = 0; i < c.args.size(); ++i) {
                    if (i != 0) {
                        os << ", ";
                    }
                    emit_expr(os, *c.args[i].value);
                }
                os << ")";
                break;
            }
        }
        // Payloaded-enum case construction via the leading-dot form:
        // `.circle(radius: 1.0)` against a contextual enum type lowers the
        // same way as the `Shape.circle(...)` form above. The case name
        // comes from the leading dot; the enum decl + any generic `<args>`
        // come from the call's resolved instance type.
        if (resolution_ != nullptr && c.callee->kind == ast::NodeKind::LeadingDotExpr) {
            const auto& d = static_cast<const ast::LeadingDotExpr&>(*c.callee);
            auto inst = resolution_->type_of(&c);
            if (inst != nullptr && inst->kind() == sema::TypeKind::Enum
                && inst->nominal_decl() != nullptr) {
                const auto& ed = static_cast<const ast::EnumDecl&>(*inst->nominal_decl());
                if (enum_is_sum_type(ed)) {
                    const bool generic_inst = !inst->parts().empty();
                    auto emit_enum_name = [&]() {
                        if (generic_inst) {
                            emit_sema_type(os, inst);
                        } else {
                            os << nominal_prefix(&ed) << ed.name;
                        }
                    };
                    emit_enum_name();
                    os << "{";
                    emit_enum_name();
                    os << "::" << d.name << "_t{";
                    for (std::size_t i = 0; i < c.args.size(); ++i) {
                        if (i != 0) {
                            os << ", ";
                        }
                        emit_value(os, *c.args[i].value);  // §10 move move-only payloads
                    }
                    os << "}}";
                    break;
                }
            }
        }
        // §17.x conversion-call lowering: `Float64(i)` → `static_cast<double>(i)`.
        // Sema accepts a bare numeric-primitive ident in callee position and
        // produces a one-arg call typed at that primitive; here we map it to
        // a C++ static_cast against the same primitive's C++ name from the
        // shared primitive_map().
        if (c.callee->kind == ast::NodeKind::IdentExpr && c.args.size() == 1) {
            const auto& callee_ident = static_cast<const ast::IdentExpr&>(*c.callee);
            const auto& m = primitive_map();
            if (auto it = m.find(callee_ident.name);
                it != m.end() && it->first != "Bool" && it->first != "Char" && it->first != "Unit"
                && it->first != "String" && it->first != "Str" && it->first != "StrConst") {
                os << "static_cast<" << it->second << ">(";
                emit_expr(os, *c.args[0].value);
                os << ")";
                break;
            }
        }
        // §5 qualified struct construction: `util.geom.Point(x: 1)` — the
        // callee is a MemberExpr the resolver tagged as a struct export with a
        // fully-qualified name. Lower to `util::geom::Point{.x = 1}`.
        if (resolution_ != nullptr && c.callee->kind == ast::NodeKind::MemberExpr) {
            const auto* sym = resolution_->symbol_of(c.callee.get());
            const auto* qn = resolution_->qualified_name_of(c.callee.get());
            if (sym != nullptr && sym->kind == sema::SymbolKind::Struct && qn != nullptr) {
                os << global_qualifier() << *qn;
                // §7/§5 generic imported struct — `coll.box.Holder[Int32](...)`
                // constructs as `coll::box::Holder<std::int32_t>{...}`. The
                // call's resolved result type carries the inferred arguments;
                // emit them so the brace-init names the right specialization
                // (the qualified name string alone has no `<args>`). Mirrors the
                // local-struct construction path below.
                if (auto inst = resolution_->type_of(&c); inst != nullptr
                                                          && inst->kind() == sema::TypeKind::Struct
                                                          && !inst->parts().empty()) {
                    os << "<";
                    for (std::size_t i = 0; i < inst->parts().size(); ++i) {
                        if (i != 0) {
                            os << ", ";
                        }
                        emit_sema_type(os, inst->parts()[i]);
                    }
                    os << ">";
                }
                // §6 a type with a `deinit` has user-declared special members,
                // so it is no aggregate: it constructs through its positional
                // constructor (the fields in order) instead.
                const bool by_ctor = sym->decl != nullptr && sym->decl->kind == ast::NodeKind::Struct
                                     && struct_has_deinit(static_cast<const ast::StructDecl&>(*sym->decl));
                os << (by_ctor ? "(" : "{");
                for (std::size_t i = 0; i < c.args.size(); ++i) {
                    if (i != 0) {
                        os << ", ";
                    }
                    if (!by_ctor && !c.args[i].label.empty()) {
                        os << "." << c.args[i].label << " = ";
                    }
                    emit_value(os, *c.args[i].value);  // §10 move move-only fields
                }
                os << (by_ctor ? ")" : "}");
                break;
            }
        }
        // §7 explicit-type-args struct construction: `Pair[Int32](lo: 1, hi: 2)`
        // parses as a call whose callee is an IndexExpr (`Pair[Int32]`); sema
        // typed the whole call as the explicit struct instance. Lower it as the
        // designated-init brace expression `Pair<Int32>{...}`, taking the
        // specialization arguments from the resolved result type — mirroring
        // the bare-identifier construction below.
        if (resolution_ != nullptr && c.callee->kind == ast::NodeKind::IndexExpr) {
            const auto& ix = static_cast<const ast::IndexExpr&>(*c.callee);
            auto inst = resolution_->type_of(&c);
            if (ix.base != nullptr && ix.base->kind == ast::NodeKind::IdentExpr && inst != nullptr
                && inst->kind() == sema::TypeKind::Struct && inst->nominal_decl() != nullptr) {
                const auto& s_decl = static_cast<const ast::StructDecl&>(*inst->nominal_decl());
                os << s_decl.name;
                if (!inst->parts().empty()) {
                    os << "<";
                    for (std::size_t i = 0; i < inst->parts().size(); ++i) {
                        if (i != 0) {
                            os << ", ";
                        }
                        emit_sema_type(os, inst->parts()[i]);
                    }
                    os << ">";
                }
                // §6 a type with a `deinit` has user-declared special members,
                // so it is no aggregate: it constructs through its positional
                // constructor (the fields in order) instead.
                const bool by_ctor = struct_has_deinit(s_decl);
                os << (by_ctor ? "(" : "{");
                for (std::size_t i = 0; i < c.args.size(); ++i) {
                    if (i != 0) {
                        os << ", ";
                    }
                    if (!by_ctor && !c.args[i].label.empty()) {
                        os << "." << c.args[i].label << " = ";
                    }
                    emit_value(os, *c.args[i].value);  // §10 move move-only fields
                }
                os << (by_ctor ? ")" : "}");
                break;
            }
        }
        // Struct construction: if the callee is a bare identifier
        // resolving to a Struct symbol, lower as a C++ designated-init
        // brace expression. Checking the callee (rather than the call's
        // result type) means a method that *returns* a struct — e.g. a
        // derive(Clone) `.clone() -> Point` — stays lowered as a real
        // call instead of a fresh struct literal.
        if (resolution_ != nullptr && c.callee->kind == ast::NodeKind::IdentExpr) {
            const auto* sym = resolution_->symbol_of(c.callee.get());
            if (sym != nullptr && sym->kind == sema::SymbolKind::Struct && sym->decl != nullptr) {
                const auto& s_decl = static_cast<const ast::StructDecl&>(*sym->decl);
                os << s_decl.name;
                // §7 generics phase 2 — a generic struct constructs as
                // `Pair<Int32>{...}`. The call's resolved result type is the
                // struct instance carrying the inferred type arguments; emit
                // them so the brace-init names the right specialization.
                if (resolution_ != nullptr) {
                    auto inst = resolution_->type_of(&c);
                    if (inst != nullptr && inst->kind() == sema::TypeKind::Struct
                        && !inst->parts().empty()) {
                        os << "<";
                        for (std::size_t i = 0; i < inst->parts().size(); ++i) {
                            if (i != 0) {
                                os << ", ";
                            }
                            emit_sema_type(os, inst->parts()[i]);
                        }
                        os << ">";
                    }
                }
                // §6 a type with a `deinit` has user-declared special members,
                // so it is no aggregate: it constructs through its positional
                // constructor (the fields in order) instead.
                const bool by_ctor = struct_has_deinit(s_decl);
                os << (by_ctor ? "(" : "{");
                for (std::size_t i = 0; i < c.args.size(); ++i) {
                    if (i != 0) {
                        os << ", ";
                    }
                    if (!by_ctor && !c.args[i].label.empty()) {
                        os << "." << c.args[i].label << " = ";
                    }
                    emit_value(os, *c.args[i].value);  // §10 move move-only fields
                }
                os << (by_ctor ? ")" : "}");
                break;
            }
            // §3 opaque-type construction: `Q(t)` lowers to a single
            // `static_cast<Q>(t)` over the `enum class Q : T {}`
            // emission. Single positional arg (sema enforces).
            if (sym != nullptr && sym->kind == sema::SymbolKind::OpaqueType && sym->decl != nullptr
                && c.args.size() == 1) {
                const auto& od = static_cast<const ast::OpaqueDecl&>(*sym->decl);
                os << "static_cast<" << od.name << ">(";
                emit_expr(os, *c.args[0].value);
                os << ")";
                break;
            }
        }
        // Recover the callee's parameter modes so we can wrap sink arguments
        // in std::move(). If the callee resolves to a Vestra func declaration,
        // use its params; otherwise treat every param as `read` (the default).
        const ast::FuncDecl* fn = callee_decl(c);
        emit_expr(os, *c.callee);
        os << "(";
        for (std::size_t i = 0; i < c.args.size(); ++i) {
            if (i != 0) {
                os << ", ";
            }
            const bool is_sink = fn != nullptr && i < fn->params.size()
                                 && fn->params[i].mode == ast::ParamMode::Sink;
            closure_to_fn_ptr_ = fn != nullptr && i < fn->params.size()
                                 && fn->params[i].type != nullptr
                                 && fn->params[i].type->kind == ast::NodeKind::FunctionType
                                 && c.args[i].value->kind == ast::NodeKind::ClosureExpr;
            // For a sink param, the value must arrive as an rvalue. The
            // simplest correct lowering: always std::move at the call site.
            // (`std::move(temporary)` is harmless — it just stays an rvalue.)
            // A `copy x` is already a fresh prvalue, and isn't wrapped.
            const bool wrap = is_sink && c.args[i].value->kind != ast::NodeKind::CopyExpr;
            if (wrap) {
                os << "std::move(";
            }
            emit_expr(os, *c.args[i].value);
            if (wrap) {
                os << ")";
            }
        }
        os << ")";
        break;
    }
    case ast::NodeKind::MemberExpr: {
        const auto& m = static_cast<const ast::MemberExpr&>(e);
        // §5 qualified module reference: `util.math.add` resolved to an imported
        // export, which the resolver tagged with its fully-qualified C++ name.
        if (resolution_ != nullptr) {
            if (const auto* qn = resolution_->qualified_name_of(&e); qn != nullptr) {
                os << global_qualifier() << *qn;
                break;
            }
        }
        // §14.12 typed sysreg access. `Sysreg.<name>` lowers to the
        // matching runtime singleton in `__vstr::sysreg::<name>`. The
        // resolver guarantees the name is in the canonical set
        // before sema accepts the access, so codegen can route
        // directly without a fallback check.
        if (m.base != nullptr && m.base->kind == ast::NodeKind::IdentExpr) {
            const auto& bi = static_cast<const ast::IdentExpr&>(*m.base);
            if (bi.name == "Sysreg" && resolution_ != nullptr) {
                if (auto et = resolution_->type_of(&e);
                    et != nullptr
                    && (et->kind() == sema::TypeKind::SysregHandle
                        || et->kind() == sema::TypeKind::SysregHandleRO
                        || et->kind() == sema::TypeKind::SysregHandleWO)) {
                    os << "__vstr::sysreg::" << m.member;
                    break;
                }
            }
        }
        // §9 optional chaining: `a?.b` lowers to either
        // std::optional::transform (member yields T) or and_then (member
        // already yields Optional<U>, which we'd otherwise nest). The
        // distinction is whether the field's AST type is OptionalType.
        if (m.is_optional_chain) {
            bool flatten = false;
            if (resolution_ != nullptr) {
                auto bt = resolution_->type_of(m.base.get());
                if (bt != nullptr && bt->kind() == sema::TypeKind::Optional
                    && bt->inner() != nullptr && bt->inner()->kind() == sema::TypeKind::Struct
                    && bt->inner()->nominal_decl() != nullptr) {
                    const auto& sd =
                        static_cast<const ast::StructDecl&>(*bt->inner()->nominal_decl());
                    for (const auto& f : sd.fields) {
                        if (f.name == m.member && f.type != nullptr
                            && f.type->kind == ast::NodeKind::OptionalType) {
                            flatten = true;
                            break;
                        }
                    }
                }
            }
            emit_expr(os, *m.base);
            os << (flatten ? ".and_then" : ".transform");
            os << "([](auto&& __vstr_o) { return __vstr_o." << m.member << "; })";
            break;
        }
        // If the base is an identifier the resolver bound to an Enum decl,
        // this is a static enum case access. Bare enums emit as
        // `Color::red`; payloaded enums lower as struct-of-variant so a
        // bare-case access needs the `Shape{Shape::point_t{}}` wrap.
        if (resolution_ != nullptr
            && (m.base->kind == ast::NodeKind::IdentExpr
                || m.base->kind == ast::NodeKind::MemberExpr)) {
            const auto* base_sym = resolution_->symbol_of(m.base.get());
            if (base_sym != nullptr && base_sym->kind == sema::SymbolKind::Enum
                && base_sym->decl != nullptr) {
                const auto& enum_decl = static_cast<const ast::EnumDecl&>(*base_sym->decl);
                if (enum_is_sum_type(enum_decl)) {
                    // §7 generics phase 2 — qualify with the resolved
                    // instance args for a generic enum's no-payload case
                    // (`Option<Int32>{Option<Int32>::none_t{}}`).
                    auto inst = resolution_->type_of(&m);
                    const bool generic_inst = inst != nullptr
                                              && inst->kind() == sema::TypeKind::Enum
                                              && !inst->parts().empty();
                    auto emit_enum_name = [&]() {
                        if (generic_inst) {
                            emit_sema_type(os, inst);
                        } else {
                            os << nominal_prefix(&enum_decl) << enum_decl.name;
                        }
                    };
                    emit_enum_name();
                    os << "{";
                    emit_enum_name();
                    os << "::" << m.member << "_t{}}";
                } else {
                    os << nominal_prefix(&enum_decl) << enum_decl.name << "::" << m.member;
                }
                break;
            }
        }
        // §14.6.3 `p.address`: a pointer (or function pointer) as an integer.
        if (resolution_ != nullptr && m.member == "address") {
            auto bt = resolution_->type_of(m.base.get());
            if (bt != nullptr
                && (bt->kind() == sema::TypeKind::Ptr || bt->kind() == sema::TypeKind::MutPtr
                    || bt->kind() == sema::TypeKind::Function)) {
                os << "static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(";
                emit_expr(os, *m.base);
                os << "))";
                break;
            }
        }
        // §3 opaque newtype: `q.value` extracts the underlying T via a
        // single `static_cast` over the `enum class Q : T {}` shape.
        // §10 Box[T]: `box.value` reads the heap-pointee via `*box`.
        if (resolution_ != nullptr && m.member == "value") {
            auto bt = resolution_->type_of(m.base.get());
            if (bt != nullptr && bt->kind() == sema::TypeKind::OpaqueType
                && bt->nominal_decl() != nullptr) {
                const auto& od = static_cast<const ast::OpaqueDecl&>(*bt->nominal_decl());
                os << "static_cast<";
                if (od.underlying) {
                    emit_type(os, *od.underlying);
                }
                os << ">(";
                emit_expr(os, *m.base);
                os << ")";
                break;
            }
            if (bt != nullptr && bt->kind() == sema::TypeKind::Box) {
                os << "(*";
                emit_expr(os, *m.base);
                os << ")";
                break;
            }
            // §A3 (§10.5) Ptr[T] / MutPtr[T]: `p.value` lowers to
            // `(*p)` over the raw pointer. The C++ compiler enforces
            // the read-only side for Ptr[T] (which emits as `const
            // T*`); MutPtr[T] (`T*`) admits assignment-through.
            if (bt != nullptr
                && (bt->kind() == sema::TypeKind::Ptr || bt->kind() == sema::TypeKind::MutPtr)) {
                os << "(*";
                emit_expr(os, *m.base);
                os << ")";
                break;
            }
        }
        // §10 Span[T] / MutSpan[T]: `.count` and `.isEmpty` map to
        // `std::span::size()` (cast to the signed `Int`-equivalent so
        // the result is comparable to ordinary signed indices without
        // tripping -Wsign-compare) and `.empty()`.
        if (resolution_ != nullptr && (m.member == "count" || m.member == "isEmpty")) {
            auto bt = resolution_->type_of(m.base.get());
            if (bt != nullptr
                && (bt->kind() == sema::TypeKind::Span || bt->kind() == sema::TypeKind::MutSpan)) {
                if (m.member == "count") {
                    os << "static_cast<std::intptr_t>(";
                    emit_expr(os, *m.base);
                    os << ".size())";
                } else {
                    emit_expr(os, *m.base);
                    os << ".empty()";
                }
                break;
            }
        }
        // §5/§18.4 ChunkIter `.count` → the runtime struct's count() accessor.
        if (resolution_ != nullptr && m.member == "count") {
            auto bt = resolution_->type_of(m.base.get());
            if (bt != nullptr && bt->kind() == sema::TypeKind::ChunkIter) {
                os << "(";
                emit_expr(os, *m.base);
                os << ").count()";
                break;
            }
        }
        // §11 Duration accessors `.nanoseconds` / `.microseconds` /
        // `.milliseconds` / `.seconds` → the runtime `in_<unit>()` method,
        // cast to the signed `Int`-equivalent (mirrors Span `.count`).
        if (resolution_ != nullptr
            && (m.member == "nanoseconds" || m.member == "microseconds"
                || m.member == "milliseconds" || m.member == "seconds")) {
            auto bt = resolution_->type_of(m.base.get());
            if (bt != nullptr && bt->kind() == sema::TypeKind::Duration) {
                os << "static_cast<std::intptr_t>((";
                emit_expr(os, *m.base);
                os << ").in_" << m.member << "())";
                break;
            }
        }
        // §A4 (§14.9.2 / §14.9.3) Atomic method renames: the Vestra
        // camelCase spellings `fetchAdd` / `fetchSub` / `fetchAnd` /
        // `fetchOr` / `fetchXor` lower to std::atomic's snake_case
        // names. The compareExchange method needs its own IIFE shape
        // (handled in the CallExpr branch) so the bare MemberExpr
        // path here doesn't try to rename it — it falls through to
        // the default which never fires because the CallExpr branch
        // intercepts it first.
        if (resolution_ != nullptr) {
            auto bt = resolution_->type_of(m.base.get());
            if (bt != nullptr && bt->kind() == sema::TypeKind::Atomic) {
                static const std::unordered_map<std::string_view, std::string_view> renames = {
                    {"fetchAdd", "fetch_add"},
                    {"fetchSub", "fetch_sub"},
                    {"fetchAnd", "fetch_and"},
                    {"fetchOr", "fetch_or"},
                    {"fetchXor", "fetch_xor"},
                };
                if (auto it = renames.find(m.member); it != renames.end()) {
                    emit_expr(os, *m.base);
                    os << "." << it->second;
                    break;
                }
            }
        }
        emit_expr(os, *m.base);
        os << "." << m.member;
        break;
    }
    case ast::NodeKind::IndexExpr: {
        // C++ subscript is the same shape as Vestra's: `base[i0, i1, ...]`.
        // The Vestra-side type-check already verified the base is indexable
        // (a vector / array / slice); we just pass through.
        // §10 Span/MutSpan use std::span::operator[] which takes
        // std::size_t — cast the (signed) Int index to keep
        // -Wsign-conversion quiet.
        const auto& ix = static_cast<const ast::IndexExpr&>(e);
        // §19.10 a single index is bounds-checked by the prelude's
        // `__vstr::at`, which also takes the index to std::size_t.
        if (ix.indices.size() == 1) {
            os << "__vstr::at(";
            emit_expr(os, *ix.base);
            os << ", ";
            emit_expr(os, *ix.indices[0]);
            os << ")";
            break;
        }
        bool span_base = false;
        if (resolution_ != nullptr && ix.base != nullptr) {
            auto bt = resolution_->type_of(ix.base.get());
            // Span / MutSpan operator[] and ChunkIter operator[] both take
            // std::size_t — cast the (signed) Int index to keep
            // -Wsign-conversion quiet.
            // §18.5 Vec[T] indexes std::vector::operator[], also size_t-typed.
            span_base =
                bt != nullptr
                && (bt->kind() == sema::TypeKind::Span || bt->kind() == sema::TypeKind::MutSpan
                    || bt->kind() == sema::TypeKind::ChunkIter
                    || bt->kind() == sema::TypeKind::Vec);
        }
        emit_expr(os, *ix.base);
        os << "[";
        for (std::size_t i = 0; i < ix.indices.size(); ++i) {
            if (i != 0) {
                os << ", ";
            }
            if (span_base) {
                os << "static_cast<std::size_t>(";
                emit_expr(os, *ix.indices[i]);
                os << ")";
            } else {
                emit_expr(os, *ix.indices[i]);
            }
        }
        os << "]";
        break;
    }
    case ast::NodeKind::LeadingDotExpr: {
        const auto& d = static_cast<const ast::LeadingDotExpr&>(e);
        // `.red` — resolved by the sema layer; its expression-type is the
        // enum. We need the enum's name to emit `Enum::red`.
        if (resolution_ != nullptr) {
            auto t = resolution_->type_of(&e);
            if (t != nullptr && t->kind() == sema::TypeKind::Enum && t->nominal_decl() != nullptr) {
                const auto& enum_decl = static_cast<const ast::EnumDecl&>(*t->nominal_decl());
                // §A4 (§14.9.1) Vestra's `Ordering` cases lower to
                // `std::memory_order_<name>` literals. Two of the five
                // need a snake-case conversion (acqRel → acq_rel,
                // seqCst → seq_cst); the others spell identically.
                if (enum_decl.name == "MemoryOrder") {
                    const std::string_view name = d.name;
                    os << "std::memory_order_";
                    if (name == "acqRel") {
                        os << "acq_rel";
                    } else if (name == "seqCst") {
                        os << "seq_cst";
                    } else {
                        os << name;  // relaxed / acquire / release
                    }
                    break;
                }
                // A no-payload case of a payloaded (sum-type) enum needs the
                // struct-of-variant wrap — `Maybe<Int32>{Maybe<Int32>::
                // nothing_t{}}` — just like the `Enum.case` member form;
                // a bare enum keeps the plain `Enum::case`.
                if (enum_is_sum_type(enum_decl)) {
                    const bool generic_inst = !t->parts().empty();
                    auto emit_enum_name = [&]() {
                        if (generic_inst) {
                            emit_sema_type(os, t);
                        } else {
                            os << nominal_prefix(&enum_decl) << enum_decl.name;
                        }
                    };
                    emit_enum_name();
                    os << "{";
                    emit_enum_name();
                    os << "::" << d.name << "_t{}}";
                } else {
                    os << nominal_prefix(&enum_decl) << enum_decl.name << "::" << d.name;
                }
                break;
            }
        }
        unsupported(os, "leading-dot expression without resolved context", e.range);
        break;
    }
    case ast::NodeKind::MatchExpr:
        // §9 conditional hoist: when the parent statement registered
        // this MatchExpr as a hoist (because a propagating try lives
        // in an arm body), the lambda was pre-emitted and bound to a
        // name — substitute `*name` here so propagation already
        // happened. Otherwise fall through to the regular IIFE.
        if (auto* name = lookup_try_hoist(&e)) {
            os << "(*" << *name << ")";
        } else {
            emit_match(os, static_cast<const ast::MatchExpr&>(e));
        }
        break;
    case ast::NodeKind::SelectExpr:
        emit_select(os, static_cast<const ast::SelectExpr&>(e));
        break;
    case ast::NodeKind::IfExpr: {
        const auto& i = static_cast<const ast::IfExpr&>(e);
        // §9 conditional hoist: when the parent statement registered
        // this IfExpr as a hoist (because a propagating try lives in a
        // branch), the IIFE was pre-emitted and bound to a name —
        // substitute `*name` here so propagation already happened.
        if (auto* name = lookup_try_hoist(&e)) {
            os << "(*" << *name << ")";
            break;
        }
        // We can't always lower if-expressions; this works for a statement
        // context but produces invalid code when used in expression position.
        // A future pass should hoist into a temporary + statement.
        if (!i.let_name.empty()) {
            // §9 `if let NAME = INIT { THEN } else { ELSE }` lowers to a
            // C++23 if-with-initializer over the std::optional, binding
            // the unwrapped value as a reference for THEN.
            os << (if_let_borrows(i) ? "([&]{ if (auto& __vstr_opt = " : "([&]{ if (auto __vstr_opt = ");
            emit_expr(os, *i.let_init);
            os << "; __vstr_opt.has_value()) { auto&& " << i.let_name << " = *__vstr_opt; return ";
            emit_expr(os, *i.then_branch);
            os << "; } else { return ";
            if (i.else_branch) {
                emit_expr(os, *i.else_branch);
            } else {
                os << "decltype(";
                emit_expr(os, *i.then_branch);
                os << "){}";
            }
            os << "; } }())";
            break;
        }
        os << "([&]{ if (";
        emit_expr(os, *i.cond);
        os << ") { return ";
        emit_expr(os, *i.then_branch);
        os << "; } else { return ";
        if (i.else_branch) {
            emit_expr(os, *i.else_branch);
        } else {
            os << "decltype(";
            emit_expr(os, *i.then_branch);
            os << "){}";
        }
        os << "; } }())";
        break;
    }
    case ast::NodeKind::BlockExpr: {
        // Treat a block expression as an IIFE returning its last statement.
        const auto& b = static_cast<const ast::BlockExpr&>(e);
        os << "[&]{ ";
        for (std::size_t i = 0; i < b.stmts.size(); ++i) {
            const auto& s = *b.stmts[i];
            bool last = (i + 1 == b.stmts.size());
            if (last && s.kind == ast::NodeKind::ExprStmt) {
                os << "return ";
                emit_expr(os, *static_cast<const ast::ExprStmt&>(s).expr);
                os << ";";
            } else {
                std::ostringstream tmp;
                emit_stmt(tmp, s, 0);
                os << tmp.str();
            }
        }
        os << " }()";
        break;
    }
    case ast::NodeKind::ClosureExpr: {
        // §16 closure literal `{ p1, p2, … => body }` lowers to a C++
        // lambda. Param types come from the resolver-typed Function
        // for this closure; without sema the param list emits as
        // `auto` (the resolver-free codegen test path). The capture
        // is `[&]` since closures don't yet have an escape-rule story
        // in v0.5 — every closure is consumed at the call site
        // (.mapError / map / filter / etc.), so by-reference capture
        // is safe.
        const auto& cx = static_cast<const ast::ClosureExpr&>(e);
        sema::TypePtr fn_type = resolution_ != nullptr ? resolution_->type_of(&e) : nullptr;
        // §A12 a closure bound to a function type is a C function pointer:
        // no captures, and the pointer's `const T&` parameters exactly.
        const bool as_fn_ptr = closure_to_fn_ptr_;
        closure_to_fn_ptr_ = false;  // not inherited by closures nested in this one
        if (as_fn_ptr && closure_captures(cx)) {
            reporter_->report(
                diag::Diagnostic::error(
                    "a closure that captures can't be used where a function type is expected: a "
                    "Vestra function type is a plain function pointer, with nowhere to keep the "
                    "captured values; pass them as parameters")
                    .at(e.range));
        }
        os << (as_fn_ptr ? "[](" : "[&](");
        for (std::size_t i = 0; i < cx.params.size(); ++i) {
            if (i != 0) {
                os << ", ";
            }
            if (fn_type != nullptr && fn_type->kind() == sema::TypeKind::Function
                && i < fn_type->parts().size() && fn_type->parts()[i] != nullptr) {
                if (as_fn_ptr) {
                    os << "const ";
                }
                emit_sema_type(os, fn_type->parts()[i]);
                if (as_fn_ptr) {
                    os << "&";
                }
            } else {
                os << "auto";
            }
            os << " " << cx.params[i];
        }
        os << ") { ";
        // A `return` in the closure body returns from the lambda, not from
        // the enclosing (possibly Unit-throws) function.
        const bool prev_unit_throws = current_func_unit_throws_;
        current_func_unit_throws_ = false;
        for (std::size_t i = 0; i < cx.body.size(); ++i) {
            const auto& s = *cx.body[i];
            bool last = (i + 1 == cx.body.size());
            if (last && s.kind == ast::NodeKind::ExprStmt
                && static_cast<const ast::ExprStmt&>(s).expr != nullptr) {
                os << "return ";
                emit_expr(os, *static_cast<const ast::ExprStmt&>(s).expr);
                os << ";";
            } else {
                std::ostringstream tmp;
                emit_stmt(tmp, s, 0);
                os << tmp.str();
            }
        }
        current_func_unit_throws_ = prev_unit_throws;
        os << " }";
        break;
    }
    case ast::NodeKind::AwaitExpr: {
        // §11 — `await e` lowers to `co_await (e)`. The awaited expression
        // is a call to an async function, which returns a `__vstr::Task<T>`;
        // co_awaiting it yields the T. The parens keep it well-formed in any
        // surrounding expression (co_await binds like a unary operator).
        const auto& a = static_cast<const ast::AwaitExpr&>(e);
        os << "co_await (";
        emit_expr(os, *a.inner);
        os << ")";
        break;
    }
    case ast::NodeKind::QuoteExpr: {
        // §12.4 v0.5 — an expression-context quote lowers to its body with
        // splices substituted in place. (A deferred typed-AST value model
        // and the declaration-macro layer are follow-on slices.)
        const auto& q = static_cast<const ast::QuoteExpr&>(e);
        if (q.inner) {
            emit_expr(os, *q.inner);
        }
        break;
    }
    case ast::NodeKind::SpliceExpr: {
        // §12.4 — `$x` / `$(expr)` substitutes the inner expression at the
        // splice site; parenthesized so it composes inside the quoted body.
        const auto& sp = static_cast<const ast::SpliceExpr&>(e);
        os << "(";
        if (sp.inner) {
            emit_expr(os, *sp.inner);
        }
        os << ")";
        break;
    }
    case ast::NodeKind::MacroCallExpr: {
        // §12.4 — an expression macro lowers to its expansion (the macro's
        // quote template with arguments substituted for the `$param` splices),
        // filled in by the resolver and parenthesized so it composes in place.
        const auto& mc = static_cast<const ast::MacroCallExpr&>(e);
        if (mc.expansion) {
            os << "(";
            emit_expr(os, *mc.expansion);
            os << ")";
        } else {
            unsupported(os, "macro call without a resolved expansion", e.range);
        }
        break;
    }
    case ast::NodeKind::SpawnExpr: {
        // §11 — `spawn e` yields a `Future[T]`. v0.5 runs the spawned call
        // eagerly and boxes its result: `__vstr::spawn_future(e)` overloads
        // on whether `e` is an async-call `Task<T>` (driven to its value) or
        // a plain `T`. `await` later consumes the Future for the T.
        const auto& sp = static_cast<const ast::SpawnExpr&>(e);
        os << "__vstr::spawn_future(";
        emit_expr(os, *sp.inner);
        os << ")";
        break;
    }
    case ast::NodeKind::AsExpr: {
        // §4 `v as T` reinterprets the bytes of two Trivial types of one
        // size and alignment (sema checks both). The compiler builtin
        // behind std::bit_cast needs no <bit>, so the freestanding
        // profile takes the same lowering.
        const auto& a = static_cast<const ast::AsExpr&>(e);
        if (a.is_optional || !a.target) {
            unsupported(os, "`as?`", e.range);
            break;
        }
        os << "__builtin_bit_cast(";
        emit_type(os, *a.target);
        os << ", ";
        emit_expr(os, *a.value);
        os << ")";
        break;
    }
    default:
        unsupported(os, "expression", e.range);
        break;
    }
}

}  // namespace vestra::codegen

namespace vestra::codegen {

// §19.9 whether `a ?? b`'s default must run lazily: it is made only of names,
// literals, members, calls, indexing and operators (anything else, such as a
// `try`, `await` or `if`, stays eager), and it contains a call, an index or a
// division that could do something. The selfhost codegen's coalesceLazy
// mirrors this.
bool CppEmitter::coalesce_is_lazy(const ast::Expr& e) {
    bool effect = false;
    auto walk = [&](auto& self, const ast::Expr& x) -> bool {
        switch (x.kind) {
        case ast::NodeKind::IntLit:
        case ast::NodeKind::FloatLit:
        case ast::NodeKind::StringLit:
        case ast::NodeKind::CharLit:
        case ast::NodeKind::BoolLit:
        case ast::NodeKind::NilLit:
        case ast::NodeKind::IdentExpr:
        case ast::NodeKind::SelfExpr:
        case ast::NodeKind::LeadingDotExpr:
            return true;
        case ast::NodeKind::ParenExpr:
            return self(self, *static_cast<const ast::ParenExpr&>(x).inner);
        case ast::NodeKind::MemberExpr:
            return self(self, *static_cast<const ast::MemberExpr&>(x).base);
        case ast::NodeKind::UnaryExpr:
            return self(self, *static_cast<const ast::UnaryExpr&>(x).operand);
        case ast::NodeKind::BinaryExpr: {
            const auto& b = static_cast<const ast::BinaryExpr&>(x);
            effect = effect || b.op == ast::BinaryOp::Div || b.op == ast::BinaryOp::Mod;
            return self(self, *b.lhs) && self(self, *b.rhs);
        }
        case ast::NodeKind::IndexExpr: {
            const auto& ix = static_cast<const ast::IndexExpr&>(x);
            effect = true;
            if (!self(self, *ix.base)) {
                return false;
            }
            for (const auto& i : ix.indices) {
                if (!self(self, *i)) {
                    return false;
                }
            }
            return true;
        }
        case ast::NodeKind::CallExpr: {
            const auto& c = static_cast<const ast::CallExpr&>(x);
            effect = true;
            if (c.callee == nullptr || !self(self, *c.callee)) {
                return false;
            }
            for (const auto& a : c.args) {
                if (a.value == nullptr || !self(self, *a.value)) {
                    return false;
                }
            }
            return true;
        }
        default:
            return false;
        }
    };
    return walk(walk, e) && effect;
}

}  // namespace vestra::codegen
