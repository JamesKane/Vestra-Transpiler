// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "vestra/codegen/cpp_emitter.hpp"

#include "cpp_emitter_internal.hpp"

#include <algorithm>
#include <format>
#include <ostream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>

namespace vestra::codegen {

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
        } else if (a.name == "interrupt") {
            out.interrupt = true;
        } else if (a.name == "boot") {
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
    // §13 — index the unit's top-level structs by name so a `Soa[Point]`
    // annotation can reach Point's fields when emitting its tuple-of-vectors.
    structs_by_name_.clear();
    for (const auto& d : unit.decls) {
        if (d != nullptr && d->kind == ast::NodeKind::Struct) {
            structs_by_name_[static_cast<const ast::StructDecl&>(*d).name] =
                static_cast<const ast::StructDecl*>(d.get());
        }
    }
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
    hdr << "#include <algorithm>\n";  // §11 scheduler timer queue min_element / erase_if
    hdr << "#include <array>\n";      // §13 [N]T lowers to std::array (+ vector arithmetic)
    hdr << "#include <atomic>\n";     // §A4 Atomic[T] lowers to std::atomic
    hdr << "#include <bit>\n";        // §A6 MmioWireView std::byteswap / std::endian
    hdr << "#include <chrono>\n";     // §11 timeout select arm wall-clock deadlines
    hdr << "#include <concepts>\n";   // §7 generic bounds lower to requires-clauses
    hdr << "#include <coroutine>\n";  // §11 async func / await lower to coroutines
    hdr << "#include <deque>\n";      // §11 Channel[T] backs onto a deque
    hdr << "#include <cstdint>\n";
    hdr << "#include <cstdlib>\n";     // §10 panic / abort → std::abort
    hdr << "#include <expected>\n";    // §9 throws(E) → T lowers to std::expected
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

    hdr << "enum class Endianness { little, big, native };\n\n";

    hdr << "namespace __vstr {\n\n";
    hdr << "struct Never {\n";
    hdr << "    template <class T> [[noreturn]] operator T() const noexcept {\n";
    hdr << "        std::unreachable();\n";
    hdr << "    }\n";
    hdr << "};\n\n";
    // §18.5 HashMap[K, V].get(k) → an owning `std::optional<V>` lookup.
    // std::unordered_map has no optional-returning accessor, so this
    // shim folds the find/end check into the `T?` the resolver promises.
    hdr << "template <class M, class K>\n";
    hdr << "std::optional<typename M::mapped_type> map_get(const M& m, const K& k) {\n";
    hdr << "    auto it = m.find(k);\n";
    hdr << "    if (it == m.end()) return std::nullopt;\n";
    hdr << "    return it->second;\n";
    hdr << "}\n\n";
    // §18.5 Vec[T].get(i) → a bounds-checked `std::optional<T>` read (nil when
    // the signed index is out of range), and Vec[T].pop() → an
    // `std::optional<T>` that removes and returns the last element (nil when
    // empty). std::vector has neither, so both fold the range/empty check into
    // the `T?` the resolver promises.
    hdr << "template <class V>\n";
    hdr << "std::optional<typename V::value_type> vec_get(const V& v, std::intptr_t i) {\n";
    hdr << "    if (i < 0 || static_cast<std::size_t>(i) >= v.size()) return std::nullopt;\n";
    hdr << "    return v[static_cast<std::size_t>(i)];\n";
    hdr << "}\n\n";
    hdr << "template <class V>\n";
    hdr << "std::optional<typename V::value_type> vec_pop(V& v) {\n";
    hdr << "    if (v.empty()) return std::nullopt;\n";
    hdr << "    auto last = std::move(v.back());\n";
    hdr << "    v.pop_back();\n";
    hdr << "    return last;\n";
    hdr << "}\n\n";
    // §13 elementwise fixed-length-vector arithmetic over `[N]T` (→
    // std::array<T, N>). Each op is a lanewise loop returning a fresh array;
    // the C++ compiler auto-vectorizes these small fixed-extent loops to real
    // SIMD where it pays, falling back to scalar otherwise. Explicit
    // std::experimental::simd / target intrinsics can replace the body later
    // behind this same surface.
    struct VecOp {
        const char* fn;
        const char* op;
    };
    static constexpr VecOp vec_ops[] = {
        {"vec_add", "+"}, {"vec_sub", "-"}, {"vec_mul", "*"}, {"vec_div", "/"}};
    for (const auto& vop : vec_ops) {
        hdr << "template <class T, std::size_t N>\n";
        hdr << "std::array<T, N> " << vop.fn
            << "(const std::array<T, N>& a, const std::array<T, N>& b) {\n";
        hdr << "    std::array<T, N> r{};\n";
        hdr << "    for (std::size_t i = 0; i < N; ++i) r[i] = a[i] " << vop.op << " b[i];\n";
        hdr << "    return r;\n";
        hdr << "}\n\n";
    }
    // §A10 (§15.5) — `@panic_handler` delegation. The function-
    // pointer slot is `inline` so multiple translation units agree
    // on one storage location; when the user declares a
    // `@panic_handler` func, codegen emits a static-init block at
    // the end of the unit that writes its address into this slot.
    // The panic shim consults the slot first, falling back to the
    // default `std::println` + `std::abort` path if no handler is
    // registered (the v0.5 hosted CI build never registers one).
    // The slot's return type is `Never` (not void) so the user's
    // handler — which Vestra requires to return Never — assigns
    // through `&handler` without a reinterpret_cast. Per the spec
    // the handler must not actually return; if it does (UB at the
    // Vestra layer), control falls through to the std::abort below.
    // Vestra emits read-mode params as `const T&` on the C++ side,
    // so the slot's signature mirrors that pattern — assigning the
    // user's handler address would otherwise hit an incompatible-
    // function-pointer-type error.
    hdr << "using PanicHandlerFn = Never(*)(const std::string_view& message, const "
           "std::string_view& file, const std::intptr_t& line);\n";
    hdr << "inline PanicHandlerFn panic_handler = nullptr;\n\n";
    hdr << "[[noreturn]] inline Never panic(std::string_view msg) noexcept {\n";
    hdr << "    if (panic_handler != nullptr) {\n";
    hdr << "        const std::string_view empty_file{};\n";
    hdr << "        const std::intptr_t zero_line = 0;\n";
    hdr << "        (void)panic_handler(msg, empty_file, zero_line);\n";
    hdr << "    }\n";
    hdr << "    std::println(stderr, \"vestra panic: {}\", msg);\n";
    hdr << "    std::abort();\n";
    hdr << "}\n\n";
    hdr << "[[noreturn]] inline Never abort_fn() noexcept {\n";
    hdr << "    std::abort();\n";
    hdr << "}\n\n";
    hdr << "[[noreturn]] inline Never unreachable_fn() noexcept {\n";
    hdr << "    std::unreachable();\n";
    hdr << "}\n\n";
    // Forwarding-reference shape: `opt!` may apply to either a value
    // (rvalue) or a binding (lvalue, including the `const &` param
    // read-mode renders into); `Opt&&` binds to both, and the matching
    // `std::forward` keeps the deref's value category.
    hdr << "template <class Opt>\n";
    hdr << "[[nodiscard]] inline auto unwrap_opt(Opt&& o) {\n";
    hdr << "    if (!o.has_value()) { panic(\"force-unwrap of nil Optional\"); }\n";
    hdr << "    return *std::forward<Opt>(o);\n";
    hdr << "}\n\n";
    hdr << "template <class Exp>\n";
    hdr << "[[nodiscard]] inline auto unwrap_exp(Exp&& r) {\n";
    hdr << "    if (!r.has_value()) { panic(\"force-unwrap of Result error\"); }\n";
    hdr << "    return *std::forward<Exp>(r);\n";
    hdr << "}\n\n";
    // §11 async runtime: a Vestra `async func` lowers to a C++20 coroutine
    // returning Task<T>. v0.5 runs on a cooperative single-threaded
    // scheduler — a Task is *lazy* (initial_suspend is suspend_always): it
    // doesn't start until it's awaited or driven. `co_await task` uses
    // symmetric transfer to run the awaited task inline and continue when it
    // completes; `.get()` lets a non-async (C++) caller drive a Task to its
    // result, pumping the scheduler's ready queue so any tasks `spawn`ed
    // along the way also run. Suspension is real now (a task can park itself
    // on the scheduler and be resumed later), which is what blocking channel
    // recv / timeout select build on; with no parking point a task still
    // runs straight through, preserving the earlier sequential semantics.
    hdr << R"__task(// §11 Duration: a Swift-like time span as a signed nanosecond count.
// Built with the static factories (seconds / milliseconds / microseconds /
// nanoseconds); `Duration / Duration` is a dimensionless ratio (double), while
// + / - and comparisons act span-wise. in_milliseconds() is what a `timeout`
// select arm consumes. Trivial and constexpr — a plain value type.
struct Duration {
    std::int64_t nanos_ = 0;
    static constexpr Duration nanoseconds(std::int64_t n) { return Duration{n}; }
    static constexpr Duration microseconds(std::int64_t n) { return Duration{n * 1'000}; }
    static constexpr Duration milliseconds(std::int64_t n) { return Duration{n * 1'000'000}; }
    static constexpr Duration seconds(std::int64_t n) { return Duration{n * 1'000'000'000}; }
    // Total whole count in each unit (truncating toward zero) — what the
    // `.milliseconds` / `.seconds` / … property accessors read.
    [[nodiscard]] constexpr std::int64_t in_nanoseconds() const { return nanos_; }
    [[nodiscard]] constexpr std::int64_t in_microseconds() const { return nanos_ / 1'000; }
    [[nodiscard]] constexpr std::int64_t in_milliseconds() const { return nanos_ / 1'000'000; }
    [[nodiscard]] constexpr std::int64_t in_seconds() const { return nanos_ / 1'000'000'000; }
    constexpr Duration operator+(Duration o) const { return Duration{nanos_ + o.nanos_}; }
    constexpr Duration operator-(Duration o) const { return Duration{nanos_ - o.nanos_}; }
    constexpr double operator/(Duration o) const {
        return static_cast<double>(nanos_) / static_cast<double>(o.nanos_);
    }
    // Scalar scaling by an integer OR a floating-point factor, each yielding a
    // Duration (`Duration * Int/Float`, `Int/Float * Duration`, `Duration /
    // Int/Float`). Constrained templates keep the integral and floating
    // overloads from colliding on an integer literal — `int -> int64` and
    // `int -> double` are equal-rank standard conversions, so plain overloads
    // would make `d * 3` ambiguous. Fractional arithmetic runs in double. None
    // collide with the `Duration / Duration` ratio (no int/float<->Duration
    // conversion exists).
    template <std::integral I>
    constexpr Duration operator*(I k) const {
        return Duration{nanos_ * static_cast<std::int64_t>(k)};
    }
    template <std::floating_point F>
    constexpr Duration operator*(F f) const {
        return Duration{
            static_cast<std::int64_t>(static_cast<double>(nanos_) * static_cast<double>(f))};
    }
    template <std::integral I>
    constexpr Duration operator/(I k) const {
        return Duration{nanos_ / static_cast<std::int64_t>(k)};
    }
    template <std::floating_point F>
    constexpr Duration operator/(F f) const {
        return Duration{
            static_cast<std::int64_t>(static_cast<double>(nanos_) / static_cast<double>(f))};
    }
    template <std::integral I>
    friend constexpr Duration operator*(I k, Duration d) {
        return d * k;
    }
    template <std::floating_point F>
    friend constexpr Duration operator*(F f, Duration d) {
        return d * f;
    }
    // A defaulted <=> implicitly supplies == / != as well.
    constexpr auto operator<=>(const Duration&) const = default;
};

// A parked receiver/selector. Shared (through shared_ptr) between the waiting
// task and every channel it parked on. `fired` makes waking idempotent: the
// first channel to wake it schedules the handle and sets fired; later channels
// (e.g. when several arms of a select become ready at once) see fired and skip,
// so the handle is never scheduled — and hence resumed — more than once.
struct Waiter {
    std::coroutine_handle<> h;
    bool fired = false;
};

// Cooperative single-threaded scheduler: a FIFO ready queue of suspended
// coroutine handles. A task parks itself by enqueueing its handle (e.g. a
// blocking channel recv waiting for a sender) and is resumed when run() pops
// it. run() drains the ready queue, then services timers: when every task is
// parked it sleeps to the earliest live deadline and fires that waiter — this
// is what a §11 timeout select arm waits on.
struct Scheduler {
    std::deque<std::coroutine_handle<>> ready;
    // A timeout select registers a timer tied to its parked Waiter. A timer
    // whose Waiter already fired (a channel arm won the race) is dropped
    // without sleeping.
    struct Timer {
        std::chrono::steady_clock::time_point deadline;
        std::shared_ptr<Waiter> w;
    };
    std::vector<Timer> timers;
    static Scheduler& instance() {
        static thread_local Scheduler s;
        return s;
    }
    void schedule(std::coroutine_handle<> h) { ready.push_back(h); }
    void add_timer(std::chrono::steady_clock::time_point dl, std::shared_ptr<Waiter> w) {
        timers.push_back({dl, std::move(w)});
    }
    void run() {
        for (;;) {
            if (!ready.empty()) {
                auto h = ready.front();
                ready.pop_front();
                if (h && !h.done()) { h.resume(); }
                continue;
            }
            // Ready queue empty: service timers. Drop any already fired (their
            // select resumed via a channel arm), then sleep to the earliest.
            std::erase_if(timers, [](const Timer& t) { return t.w->fired; });
            if (timers.empty()) { return; }
            auto it = std::min_element(
                timers.begin(), timers.end(), [](const Timer& a, const Timer& b) {
                    return a.deadline < b.deadline;
                });
            auto dl = it->deadline;
            auto w = it->w;
            timers.erase(it);
            std::this_thread::sleep_until(dl);
            if (!w->fired) {
                w->fired = true;
                ready.push_back(w->h);
            }
        }
    }
};

// What a `select` can wait on: a channel state or a future. The select arms
// one shared Waiter on each arm; whichever becomes ready first fires it (once,
// via the Waiter's `fired` flag). This non-template interface lets a select
// hold a homogeneous list of arms regardless of element type or arm kind.
struct Selectable {
    virtual ~Selectable() = default;
    [[nodiscard]] virtual bool sel_ready() const = 0;
    virtual void sel_arm(std::shared_ptr<Waiter> w) = 0;
    virtual void sel_disarm(const Waiter* w) = 0;
};

// Non-template base of every Channel's State: the parked-waiter queue, the
// closed flag, and the park/unpark/wake machinery live here so a `select` can
// hold a homogeneous list of channel states regardless of element type. The
// one element-type-dependent query, `has_value()`, is virtual and overridden
// by the typed State to report whether its queue is non-empty. It is a
// Selectable: arming parks a waiter, disarming removes it, readiness is
// `ready()`.
struct SelectableState : Selectable {
    std::deque<std::shared_ptr<Waiter>> waiters;
    bool closed = false;
    [[nodiscard]] virtual bool has_value() const = 0;
    // Ready to receive: a value is queued, or the channel is closed (a closed,
    // drained channel is "ready" so a blocked receiver observes the nil).
    [[nodiscard]] bool ready() const { return has_value() || closed; }
    [[nodiscard]] bool sel_ready() const override { return ready(); }
    void sel_arm(std::shared_ptr<Waiter> w) override { park(std::move(w)); }
    void sel_disarm(const Waiter* w) override { unpark(w); }
    void park(std::shared_ptr<Waiter> w) { waiters.push_back(std::move(w)); }
    void unpark(const Waiter* w) {
        for (auto it = waiters.begin(); it != waiters.end(); ++it) {
            if (it->get() == w) {
                waiters.erase(it);
                return;
            }
        }
    }
    // Wake one parked waiter (the next not-yet-fired one), scheduling it.
    void wake_one() {
        while (!waiters.empty()) {
            auto w = waiters.front();
            waiters.pop_front();
            if (!w->fired) {
                w->fired = true;
                Scheduler::instance().schedule(w->h);
                return;
            }
        }
    }
    // Wake every parked waiter (used by close()).
    void wake_all() {
        while (!waiters.empty()) {
            auto w = waiters.front();
            waiters.pop_front();
            if (!w->fired) {
                w->fired = true;
                Scheduler::instance().schedule(w->h);
            }
        }
    }
};

template <class T>
struct Task {
    using __vstr_task_value = T;  // marks this as a Task for spawn_future
    struct promise_type {
        T value{};
        std::coroutine_handle<> continuation{};  // resumed at final_suspend
        // §11 select: when this task is a future arm of a blocking select, the
        // select parks one shared Waiter here; completing the task fires it
        // (idempotently — the Waiter's `fired` flag dedups against a channel
        // arm that may also have woken the same select).
        std::shared_ptr<Waiter> select_waiter{};
        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        // Symmetric transfer: on completion, hand control to whoever awaited
        // this task (its continuation), or to noop if it was driven directly.
        struct FinalAwaiter {
            [[nodiscard]] bool await_ready() const noexcept { return false; }
            std::coroutine_handle<> await_suspend(
                std::coroutine_handle<promise_type> h) noexcept {
                auto& w = h.promise().select_waiter;
                if (w && !w->fired) {
                    w->fired = true;
                    Scheduler::instance().schedule(w->h);
                }
                auto c = h.promise().continuation;
                return c ? c : std::noop_coroutine();
            }
            void await_resume() const noexcept {}
        };
        FinalAwaiter final_suspend() noexcept { return {}; }
        void return_value(T v) { value = std::move(v); }
        void unhandled_exception() { std::abort(); }
    };
    std::coroutine_handle<promise_type> h_;
    explicit Task(std::coroutine_handle<promise_type> h) : h_(h) {}
    Task(Task&& o) noexcept : h_(std::exchange(o.h_, {})) {}
    Task& operator=(Task&& o) noexcept {
        if (this != &o) { if (h_) { h_.destroy(); } h_ = std::exchange(o.h_, {}); }
        return *this;
    }
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    ~Task() { if (h_) { h_.destroy(); } }
    [[nodiscard]] bool await_ready() const noexcept { return !h_ || h_.done(); }
    // Awaiting a task records the caller as the task's continuation and
    // transfers control into the task (symmetric transfer — no stack growth).
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
        h_.promise().continuation = caller;
        return h_;
    }
    T await_resume() { return std::move(h_.promise().value); }
    // Drive to completion from a non-async caller: start the task if it
    // hasn't begun, then pump the scheduler until it finishes.
    [[nodiscard]] T get() {
        if (h_ && !h_.done()) { h_.resume(); }
        while (h_ && !h_.done()) { Scheduler::instance().run(); }
        return std::move(h_.promise().value);
    }
};

template <>
struct Task<void> {
    using __vstr_task_value = void;  // marks this as a Task for spawn_future
    struct promise_type {
        std::coroutine_handle<> continuation{};
        std::shared_ptr<Waiter> select_waiter{};  // fired on completion (§11 select)
        Task get_return_object() {
            return Task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        struct FinalAwaiter {
            [[nodiscard]] bool await_ready() const noexcept { return false; }
            std::coroutine_handle<> await_suspend(
                std::coroutine_handle<promise_type> h) noexcept {
                auto& w = h.promise().select_waiter;
                if (w && !w->fired) {
                    w->fired = true;
                    Scheduler::instance().schedule(w->h);
                }
                auto c = h.promise().continuation;
                return c ? c : std::noop_coroutine();
            }
            void await_resume() const noexcept {}
        };
        FinalAwaiter final_suspend() noexcept { return {}; }
        void return_void() {}
        void unhandled_exception() { std::abort(); }
    };
    std::coroutine_handle<promise_type> h_;
    explicit Task(std::coroutine_handle<promise_type> h) : h_(h) {}
    Task(Task&& o) noexcept : h_(std::exchange(o.h_, {})) {}
    Task& operator=(Task&& o) noexcept {
        if (this != &o) { if (h_) { h_.destroy(); } h_ = std::exchange(o.h_, {}); }
        return *this;
    }
    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;
    ~Task() { if (h_) { h_.destroy(); } }
    [[nodiscard]] bool await_ready() const noexcept { return !h_ || h_.done(); }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> caller) noexcept {
        h_.promise().continuation = caller;
        return h_;
    }
    void await_resume() const noexcept {}
    void get() {
        if (h_ && !h_.done()) { h_.resume(); }
        while (h_ && !h_.done()) { Scheduler::instance().run(); }
    }
};

// §11 Future<T>: what `spawn` yields. The Future owns the spawned Task
// (keeping its coroutine frame alive). `spawn` schedules the task on the
// run-loop so it makes progress cooperatively — interleaving with the
// spawner and with other spawned tasks — rather than only when awaited; this
// is what lets a parked consumer and a running producer coexist. Forcing the
// future (`await future`, or `.get()` from a non-async caller) pumps the
// scheduler until the task completes and caches its value. A plain (non-Task)
// spawn argument is already a value and is stored directly. The destructor
// drives a scheduled-but-never-awaited task to completion so it can't leave a
// dangling handle in the ready queue (fire-and-forget cleanup).
template <class T>
struct Future {
    std::optional<Task<T>> task_;  // present until forced
    std::optional<T> value_;       // the resolved result, cached once forced
    bool scheduled_ = false;
    explicit Future(Task<T> t) : task_(std::move(t)) {}
    explicit Future(T v) : value_(std::move(v)) {}
    Future(Future&&) = default;
    Future& operator=(Future&&) = default;
    Future(const Future&) = delete;
    Future& operator=(const Future&) = delete;
    void ensure_scheduled() {
        if (task_ && task_->h_ && !task_->h_.done() && !scheduled_) {
            Scheduler::instance().schedule(task_->h_);
            scheduled_ = true;
        }
    }
    T force() {
        if (!value_.has_value()) {
            ensure_scheduled();
            while (task_->h_ && !task_->h_.done()) { Scheduler::instance().run(); }
            value_ = std::move(task_->h_.promise().value);
            task_.reset();
        }
        return *value_;
    }
    [[nodiscard]] bool await_ready() const noexcept { return true; }
    void await_suspend(std::coroutine_handle<>) const noexcept {}
    T await_resume() { return force(); }
    [[nodiscard]] T get() { return force(); }
    // §11 select support: a future arm is ready once its task has completed
    // (or the value is already cached). Arming registers the select's Waiter on
    // the task's promise so completing the task fires it; if the future is
    // already ready, fire immediately so the select doesn't park forever.
    [[nodiscard]] bool sel_ready() const {
        return value_.has_value() || (task_ && task_->h_ && task_->h_.done());
    }
    void sel_arm(std::shared_ptr<Waiter> w) {
        if (sel_ready()) {
            if (w && !w->fired) {
                w->fired = true;
                Scheduler::instance().schedule(w->h);
            }
            return;
        }
        ensure_scheduled();  // make sure the task will run (and complete)
        task_->h_.promise().select_waiter = std::move(w);
    }
    void sel_disarm() {
        if (task_ && task_->h_ && !task_->h_.done()) { task_->h_.promise().select_waiter = nullptr; }
    }
    ~Future() {
        if (task_ && task_->h_ && !task_->h_.done()) {
            ensure_scheduled();
            while (task_->h_ && !task_->h_.done()) { Scheduler::instance().run(); }
        }
    }
};

// §11 Future<void>: what `spawn` of a Unit-returning async fn yields. There is
// no value to cache (std::optional<void> is ill-formed), so a `forced_` flag
// stands in for "completed"; forcing pumps the scheduler to completion. Awaits
// and `.get()` yield void. Mirrors Future<T>'s scheduling, select hooks, and
// fire-and-forget destructor.
template <>
struct Future<void> {
    std::optional<Task<void>> task_;
    bool forced_ = false;
    bool scheduled_ = false;
    explicit Future(Task<void> t) : task_(std::move(t)) {}
    Future(Future&&) = default;
    Future& operator=(Future&&) = default;
    Future(const Future&) = delete;
    Future& operator=(const Future&) = delete;
    void ensure_scheduled() {
        if (task_ && task_->h_ && !task_->h_.done() && !scheduled_) {
            Scheduler::instance().schedule(task_->h_);
            scheduled_ = true;
        }
    }
    void force() {
        if (!forced_) {
            ensure_scheduled();
            while (task_->h_ && !task_->h_.done()) { Scheduler::instance().run(); }
            forced_ = true;
        }
    }
    [[nodiscard]] bool await_ready() const noexcept { return true; }
    void await_suspend(std::coroutine_handle<>) const noexcept {}
    void await_resume() { force(); }
    void get() { force(); }
    [[nodiscard]] bool sel_ready() const {
        return forced_ || (task_ && task_->h_ && task_->h_.done());
    }
    void sel_arm(std::shared_ptr<Waiter> w) {
        if (sel_ready()) {
            if (w && !w->fired) {
                w->fired = true;
                Scheduler::instance().schedule(w->h);
            }
            return;
        }
        ensure_scheduled();
        task_->h_.promise().select_waiter = std::move(w);
    }
    void sel_disarm() {
        if (task_ && task_->h_ && !task_->h_.done()) { task_->h_.promise().select_waiter = nullptr; }
    }
    ~Future() {
        if (task_ && task_->h_ && !task_->h_.done()) {
            ensure_scheduled();
            while (task_->h_ && !task_->h_.done()) { Scheduler::instance().run(); }
        }
    }
};

// §11 a future as a select arm. Type-erases a `Future<T>*` (which lives in the
// select's coroutine frame) behind the Selectable interface so a blocking
// select can hold channel states and futures in one homogeneous list.
template <class T>
struct FutureSelectable : Selectable {
    Future<T>* f;
    explicit FutureSelectable(Future<T>* fp) : f(fp) {}
    [[nodiscard]] bool sel_ready() const override { return f->sel_ready(); }
    void sel_arm(std::shared_ptr<Waiter> w) override { f->sel_arm(std::move(w)); }
    void sel_disarm(const Waiter*) override { f->sel_disarm(); }
};

// `spawn e`: if e is a Task<T> (an async call), hand back a Future<T> owning
// the task and schedule it so it runs cooperatively; otherwise wrap the plain
// value.
template <class E>
auto spawn_future(E&& e) {
    using D = std::decay_t<E>;
    if constexpr (requires { typename D::__vstr_task_value; }) {
        using V = typename D::__vstr_task_value;
        Future<V> f{std::forward<E>(e)};
        f.ensure_scheduled();
        return f;
    } else {
        return Future<D>{std::forward<E>(e)};
    }
}

// §11.2 parallel: split `data` into `chunks` disjoint contiguous sub-spans
// (balanced — the first `total % chunks` get one extra element) and run the
// non-escaping worker on each. v0.5 runs them sequentially (no scheduler);
// the disjoint-MutSpan partition + non-escaping body is the contract a real
// thread pool plugs into later.
template <class T, class F>
void parallel(std::span<T> data, std::intptr_t chunks, F&& body) {
    if (chunks < 1) { chunks = 1; }
    const std::size_t nc = static_cast<std::size_t>(chunks);
    const std::size_t total = data.size();
    const std::size_t base = total / nc;
    const std::size_t rem = total % nc;
    std::size_t off = 0;
    for (std::size_t i = 0; i < nc; ++i) {
        const std::size_t len = base + (i < rem ? 1 : 0);
        body(data.subspan(off, len));
        off += len;
    }
}

// §5/§18.4 split_at: the trusted partitioner behind `s.split(at: i)`. Returns
// the two non-overlapping sub-views ([0, i) and [i, count)); `i` is clamped
// to [0, count] so an out-of-range split yields an empty piece rather than
// undefined behaviour.
template <class T>
std::tuple<std::span<T>, std::span<T>> split_at(std::span<T> s, std::intptr_t at) {
    const std::size_t n = s.size();
    const std::size_t k =
        at < 0 ? 0 : (static_cast<std::size_t>(at) > n ? n : static_cast<std::size_t>(at));
    return {s.subspan(0, k), s.subspan(k)};
}

// §5/§18.4 Chunks: the iterator behind `s.chunks(of: n)`. `next()` yields the
// consecutive, disjoint sub-views of length `n` ([0,n), [n,2n), …); the last
// is shorter when the length isn't a multiple of `n`. A chunk size of 0 yields
// no chunks rather than dividing by zero. S is the span type (std::span<const
// T> for a read-only Span[T], std::span<T> for a MutSpan[T]); CTAD deduces it
// from the first initializer at the call site (`__vstr::Chunks{data, n}`).
template <class S>
struct Chunks {
    S data;
    std::size_t step;
    std::size_t off = 0;
    std::optional<S> next() {
        if (step == 0 || off >= data.size()) { return std::nullopt; }
        const std::size_t len = (off + step <= data.size()) ? step : (data.size() - off);
        S piece = data.subspan(off, len);
        off += len;
        return piece;
    }
    // First-class (non-mutating) accessors: count() is the number of chunks
    // (ceil(size/step)); operator[] returns the i-th sub-view. Neither touches
    // the `off` cursor, so random access and a `for` consume are independent
    // (the for-loop copies the struct anyway). The index is the caller's
    // responsibility, matching std::span's unchecked operator[].
    [[nodiscard]] std::intptr_t count() const {
        if (step == 0) { return 0; }
        return static_cast<std::intptr_t>((data.size() + step - 1) / step);
    }
    [[nodiscard]] S operator[](std::size_t i) const {
        const std::size_t start = i * step;
        const std::size_t len = (start + step <= data.size()) ? step : (data.size() - start);
        return data.subspan(start, len);
    }
};

// §11 Channel<T>: a typed FIFO queue connecting cooperative tasks. The
// shared State (queue + parked receivers + closed flag) is held through a
// shared_ptr so every copy of a channel handle names one channel. v0.5 is
// single-threaded and unbounded.
//
// Two receive flavors:
//   * `recv() -> T?`        — non-blocking: front value, or nil if empty.
//   * `await receive() -> T?` — suspending: parks the task on an empty, open
//     channel until a `send` wakes it (or `close` drains it). nil means the
//     channel is closed *and* drained — the canonical "no more values" signal
//     that lets a `while let v = await ch.receive()` loop terminate.
// `send` wakes one parked receiver (scheduling it on the run-loop); `close`
// wakes all parked receivers so they observe the closed-and-empty nil.
template <class T>
struct Channel {
    struct State : SelectableState {
        std::deque<T> q;
        [[nodiscard]] bool has_value() const override { return !q.empty(); }
    };
    std::shared_ptr<State> s_ = std::make_shared<State>();
    // A channel is a handle: these mutate the shared State through s_, not the
    // handle itself, so they are const (callable on a read-borrowed channel
    // passed as a `const Channel&` parameter — the share-nothing model passes
    // handles between tasks by read-borrow).
    void send(T v) const {
        s_->q.push_back(std::move(v));
        s_->wake_one();  // wake one parked receiver, if any
    }
    void close() const {
        s_->closed = true;
        s_->wake_all();  // every parked receiver observes the closed-and-drained nil
    }
    std::optional<T> recv() const {  // non-blocking poll
        if (s_->q.empty()) { return std::nullopt; }
        T v = std::move(s_->q.front());
        s_->q.pop_front();
        return v;
    }
    // Suspending receive — co_await it. Ready when a value is queued or the
    // channel is closed; otherwise parks the awaiting task via a shared Waiter.
    struct ReceiveAwaiter {
        std::shared_ptr<State> s;
        std::shared_ptr<Waiter> w;  // set on suspend; nil if we never parked
        [[nodiscard]] bool await_ready() const noexcept { return s->ready(); }
        std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) {
            w = std::make_shared<Waiter>(Waiter{h, false});
            s->park(w);
            return std::noop_coroutine();  // park: control returns to the scheduler
        }
        std::optional<T> await_resume() {
            if (w) { s->unpark(w.get()); }
            if (s->q.empty()) { return std::nullopt; }  // closed and drained
            T v = std::move(s->q.front());
            s->q.pop_front();
            return v;
        }
    };
    ReceiveAwaiter receive() const { return ReceiveAwaiter{s_, nullptr}; }
    // §11 select support. sel_state() hands the select its type-erased state so
    // a select over channels of differing element types can park on all of them
    // uniformly; sel_take() pops the front value (the typed step, done in the
    // generated dispatch once the winning arm is known). nil if drained.
    [[nodiscard]] std::shared_ptr<SelectableState> sel_state() const { return s_; }
    std::optional<T> sel_take() const {
        if (s_->q.empty()) { return std::nullopt; }
        T v = std::move(s_->q.front());
        s_->q.pop_front();
        return v;
    }
};

// §11 blocking select: arms one shared Waiter on every arm (channel state or
// future) and resumes when any becomes ready, yielding the index of the first
// ready arm (in source order, so earlier arms win ties). await_ready short-
// circuits when an arm is already ready (no suspension). On resume it disarms
// the Waiter from every arm — the wake fired it on one, but the others still
// hold it — then recomputes the winner. The generated code then takes the
// winning arm's value (sel_take() for a channel, .get() for a future) and runs
// that arm's body. A timeout arm (channels only) registers a wall-clock timer.
struct SelectAwaiter {
    std::vector<std::shared_ptr<Selectable>> arms;
    std::shared_ptr<Waiter> w;  // set on suspend; nil if an arm was already ready
    // §11 timeout arm: when present, also register a wall-clock timer on
    // suspend; if it fires before any arm, the select resumes with
    // `timeout_index` (== arms.size(), the arm past the last channel).
    bool has_timeout = false;
    std::int64_t timeout_ms = 0;
    int timeout_index = 0;
    [[nodiscard]] int first_ready() const {
        for (std::size_t i = 0; i < arms.size(); ++i) {
            if (arms[i]->sel_ready()) { return static_cast<int>(i); }
        }
        return -1;
    }
    [[nodiscard]] bool await_ready() const noexcept { return first_ready() >= 0; }
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> h) {
        w = std::make_shared<Waiter>(Waiter{h, false});
        for (auto& s : arms) { s->sel_arm(w); }
        if (has_timeout) {
            auto dl = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
            Scheduler::instance().add_timer(dl, w);
        }
        return std::noop_coroutine();
    }
    int await_resume() {
        if (w) {
            for (auto& s : arms) { s->sel_disarm(w.get()); }
        }
        const int r = first_ready();
        return r >= 0 ? r : timeout_index;  // no arm ready → the timer fired
    }
};

)__task";
    // §9 iterator combinators. `Zip<A, B>` and `Take<A>` are header-
    // local templates driven by the existing iterator-protocol for-
    // loop machinery — both expose `next()` returning std::optional
    // over the appropriate element shape. CTAD lets the call sites
    // write `__vstr::Zip{a, b}` / `__vstr::Take{xs, n}` without
    // spelling the underlying iterator types.
    hdr << "template <class A, class B>\n";
    hdr << "struct Zip {\n";
    hdr << "    A a;\n";
    hdr << "    B b;\n";
    hdr << "    using ElemA = typename decltype(std::declval<A&>().next())::value_type;\n";
    hdr << "    using ElemB = typename decltype(std::declval<B&>().next())::value_type;\n";
    hdr << "    using Elem = std::tuple<ElemA, ElemB>;\n";
    hdr << "    std::optional<Elem> next() {\n";
    hdr << "        auto xa = a.next();\n";
    hdr << "        if (!xa.has_value()) { return std::nullopt; }\n";
    hdr << "        auto xb = b.next();\n";
    hdr << "        if (!xb.has_value()) { return std::nullopt; }\n";
    hdr << "        return Elem{std::move(*xa), std::move(*xb)};\n";
    hdr << "    }\n";
    hdr << "};\n";
    hdr << "template <class A, class B> Zip(A, B) -> Zip<A, B>;\n\n";
    hdr << "template <class A>\n";
    hdr << "struct Take {\n";
    hdr << "    A a;\n";
    hdr << "    std::int64_t remaining;\n";
    hdr << "    using Elem = typename decltype(std::declval<A&>().next())::value_type;\n";
    hdr << "    std::optional<Elem> next() {\n";
    hdr << "        if (remaining <= 0) { return std::nullopt; }\n";
    hdr << "        --remaining;\n";
    hdr << "        return a.next();\n";
    hdr << "    }\n";
    hdr << "};\n";
    hdr << "template <class A, class N> Take(A, N) -> Take<A>;\n\n";
    // Map<A, F> forwards each yielded element through f; the element
    // type of the resulting iterator is `decltype(f(elem))`. F is
    // captured by value (same as A) so the inner lambda's captures
    // — by-ref into the surrounding fn's locals — stay alive for
    // every call to next().
    hdr << "template <class A, class F>\n";
    hdr << "struct Map {\n";
    hdr << "    A a;\n";
    hdr << "    F f;\n";
    hdr << "    using ElemA = typename decltype(std::declval<A&>().next())::value_type;\n";
    hdr << "    using Elem  = decltype(std::declval<F&>()(std::declval<ElemA&>()));\n";
    hdr << "    std::optional<Elem> next() {\n";
    hdr << "        auto x = a.next();\n";
    hdr << "        if (!x.has_value()) { return std::nullopt; }\n";
    hdr << "        return std::optional<Elem>{f(*x)};\n";
    hdr << "    }\n";
    hdr << "};\n";
    hdr << "template <class A, class F> Map(A, F) -> Map<A, F>;\n\n";
    // Filter<A, P> loops a.next() until p(v) is true (returning v)
    // or a.next() reports nullopt (returning nullopt). The element
    // type matches the source iterator's.
    hdr << "template <class A, class P>\n";
    hdr << "struct Filter {\n";
    hdr << "    A a;\n";
    hdr << "    P p;\n";
    hdr << "    using Elem = typename decltype(std::declval<A&>().next())::value_type;\n";
    hdr << "    std::optional<Elem> next() {\n";
    hdr << "        for (;;) {\n";
    hdr << "            auto x = a.next();\n";
    hdr << "            if (!x.has_value()) { return std::nullopt; }\n";
    hdr << "            if (p(*x)) { return x; }\n";
    hdr << "        }\n";
    hdr << "    }\n";
    hdr << "};\n";
    hdr << "template <class A, class P> Filter(A, P) -> Filter<A, P>;\n\n";
    // §A6 (§14.11) MMIO view templates. `MmioView<T>` wraps a
    // `volatile T*` and exposes read / write. `MmioRegion<T>` adds
    // an indexed view; the index() bounds-checks via __vstr::panic
    // so an out-of-range index lands the same way an out-of-range
    // vector access would. Both are header-local; their `at()` call
    // sites construct them with the volatile-cast pointer.
    hdr << "template <class T>\n";
    hdr << "struct MmioView {\n";
    hdr << "    T volatile* ptr;\n";
    hdr << "    [[nodiscard]] T read() const noexcept { return *ptr; }\n";
    hdr << "    void write(T v) noexcept { *ptr = v; }\n";
    hdr << "};\n\n";
    hdr << "template <class T>\n";
    hdr << "struct MmioRegion {\n";
    hdr << "    T volatile* base;\n";
    hdr << "    std::intptr_t count;\n";
    hdr << "    [[nodiscard]] MmioView<T> index(std::intptr_t i) const noexcept {\n";
    hdr << "        if (i < 0 || i >= count) {\n";
    hdr << "            panic(\"MmioRegion.index out of range\");\n";
    hdr << "        }\n";
    hdr << "        return MmioView<T>{base + i};\n";
    hdr << "    }\n";
    hdr << "};\n\n";

    // §A11 (§14.8) `PerCpu<T>` — per-hart storage. v0.5 hosts a
    // single 64-byte-aligned slot; the kernel target swaps this
    // for `[MAX_HARTS]Padded<T>` reading the hart index from a
    // target-specific register. The 64-byte alignment matches the
    // cache-line convention so adjacent per-CPU values don't share
    // a line — the "no false sharing" property `PerCpu[Atomic[T]]`
    // is the kernel's load-bearing pattern for.
    // §A6 (§14.11.3) `MmioWireView<T>` — endianness-aware MMIO view.
    // The constructor pins the device's bus endianness; read/write
    // byte-swap when host-native differs. We use std::byteswap for
    // multi-byte primitives (C++23) and a no-op for 1-byte T.
    hdr << "template <class T>\n";
    hdr << "struct MmioWireView {\n";
    hdr << "    T volatile* ptr;\n";
    hdr << "    Endianness endian;\n";
    hdr << "    [[nodiscard]] T read() const noexcept {\n";
    hdr << "        T raw = *ptr;\n";
    hdr << "        return needs_swap() ? swap_bytes(raw) : raw;\n";
    hdr << "    }\n";
    hdr << "    void write(T v) noexcept {\n";
    hdr << "        *ptr = needs_swap() ? swap_bytes(v) : v;\n";
    hdr << "    }\n";
    hdr << "  private:\n";
    hdr << "    [[nodiscard]] bool needs_swap() const noexcept {\n";
    hdr << "        if (endian == Endianness::native) return false;\n";
    hdr << "        bool host_little = (std::endian::native == std::endian::little);\n";
    hdr << "        bool dev_little  = (endian == Endianness::little);\n";
    hdr << "        return host_little != dev_little;\n";
    hdr << "    }\n";
    hdr << "    static T swap_bytes(T v) noexcept {\n";
    hdr << "        if constexpr (sizeof(T) <= 1) { return v; }\n";
    hdr << "        else { return std::byteswap(v); }\n";
    hdr << "    }\n";
    hdr << "};\n\n";

    // §14.8 / §12.6 — the cache-line width is exposed at the C++
    // layer as a named comptime constant so user code that needs
    // the value (and the Padded template below) reference one
    // source of truth. The Vestra-side spelling `cfg.option(
    // "cache_line_bytes")` folds to the same number via the
    // existing comptime accessor. v0.5 hardcodes 64; a future
    // build-time override of the option becomes a one-line change
    // to the dispatch table here + in `cfg_option` (sema/resolver
    // intercept).
    hdr << "inline constexpr std::size_t cache_line_bytes = 64;\n\n";

    // §A11 (§14.8) `Padded<T>` — cache-line-padded wrapper. The
    // value sits at offset 0 with tail padding bringing sizeof up
    // to the cache-line width. The `alignas` ensures the storage
    // starts on a cache-line boundary so adjacent values can't
    // share a line — the load-bearing property for per-hart arrays
    // and lock-free ring slots. The trailing `_pad` field is sized
    // via a tiny conditional so a T that already exceeds the line
    // width doesn't get a negative-array-bounds error.
    hdr << "template <class T>\n";
    hdr << "struct alignas(cache_line_bytes) Padded {\n";
    hdr << "    T value;\n";
    hdr << "    std::uint8_t _pad[(cache_line_bytes - sizeof(T) % cache_line_bytes) "
           "% cache_line_bytes];\n";
    hdr << "};\n\n";

    hdr << "template <class T>\n";
    hdr << "struct alignas(64) PerCpu {\n";
    hdr << "    T value;\n";
    hdr << "    [[nodiscard]] T& mine() noexcept { return value; }\n";
    hdr << "    [[nodiscard]] const T& mine() const noexcept { return value; }\n";
    // §A11 (§14.8) `slot(hartId) using RawMemory -> Ptr[T]`. The
    // spec's layout is `[MAX_HARTS]Padded[T]` indexed by hart;
    // v0.5 hosts a single slot, so any hartId resolves to the
    // one and only slot — the unused parameter cast keeps the
    // compiler quiet without warning suppression. The kernel
    // target swaps this for an indexed lookup over the per-CPU
    // array region (the `tpidr_el1` / `gs:[0]` / `tp` register
    // gives the local hart's index; cross-hart calls index the
    // raw region directly).
    hdr << "    [[nodiscard]] T* slot(std::uint16_t hartId) noexcept {\n";
    hdr << "        (void)hartId;\n";
    hdr << "        return &value;\n";
    hdr << "    }\n";
    hdr << "};\n\n";
    // §A4 (§14.9.3) CASResult<T> — the strong-CAS return value. Two
    // fields: `succeeded` (bool) and `actual` (T, the value the
    // compare-exchange observed). The atomic call site lowering
    // (emit_expr / CallExpr branch) packages compare_exchange_strong's
    // ok-flag + in/out expected into this struct.
    hdr << "template <class T>\n";
    hdr << "struct CASResult {\n";
    hdr << "    bool succeeded;\n";
    hdr << "    T actual;\n";
    hdr << "};\n\n";

    // §A4 (§14.9.5) AtomicTaggedPointer<T> — ABA-safe pointer swap
    // built on top of the §A4 wide-atomic primitive. The wrapper
    // holds a single `std::atomic<__uint128_t>` underneath; the low
    // 64 bits store the pointer (as a `T*` bit-pattern via
    // `reinterpret_cast`), the high 64 bits store an unsigned tag
    // that the compare-exchange auto-bumps on the desired side so a
    // concurrent observer can detect a swap-and-restore.
    //
    // The hosted build relies on libatomic when the target ISA
    // doesn't provide a native 128-bit CAS (aarch64 needs +lse2,
    // x86_64 needs +cx16); without those features the operation
    // falls back to a lock-based implementation that's still
    // correct, just not lock-free. The kernel target ships with
    // the required features in its build description so the
    // generated code lands on the native instruction.
    //
    // The load returns a {ptr, tag} snapshot as std::tuple<T*,
    // std::uint64_t> so the Vestra tuple-destructuring path picks
    // it up unchanged. compareExchange takes the expected pointer
    // and tag separately (matches the v0.5 sema shape) and writes
    // (desired_ptr, exp_tag + 1) on success; on failure the CAS
    // updates the local `exp` value, which we unpack back into a
    // snapshot for `CASResult.actual` so the caller can retry.
    hdr << "template <class T>\n";
    hdr << "struct AtomicTaggedPointer {\n";
    hdr << "    std::atomic<__uint128_t> raw_{0};\n";
    hdr << "    static constexpr __uint128_t pack(T* p, std::uint64_t tag) noexcept {\n";
    hdr << "        return (static_cast<__uint128_t>(tag) << 64)\n";
    hdr << "             | static_cast<__uint128_t>(reinterpret_cast<std::uintptr_t>(p));\n";
    hdr << "    }\n";
    hdr << "    static constexpr std::tuple<T*, std::uint64_t> unpack(__uint128_t v) noexcept {\n";
    hdr << "        return {\n";
    hdr << "            reinterpret_cast<T*>(static_cast<std::uintptr_t>(v)),\n";
    hdr << "            static_cast<std::uint64_t>(v >> 64),\n";
    hdr << "        };\n";
    hdr << "    }\n";
    hdr << "    [[nodiscard]] std::tuple<T*, std::uint64_t> load(std::memory_order o) const "
           "noexcept {\n";
    hdr << "        return unpack(raw_.load(o));\n";
    hdr << "    }\n";
    hdr << "    void store(T* p, std::uint64_t tag, std::memory_order o) noexcept {\n";
    hdr << "        raw_.store(pack(p, tag), o);\n";
    hdr << "    }\n";
    hdr << "    [[nodiscard]] CASResult<std::tuple<T*, std::uint64_t>>\n";
    hdr << "    compare_exchange_strong(T* exp_ptr, std::uint64_t exp_tag, T* des_ptr,\n";
    hdr << "                            std::memory_order success,\n";
    hdr << "                            std::memory_order failure) noexcept {\n";
    hdr << "        __uint128_t exp = pack(exp_ptr, exp_tag);\n";
    hdr << "        __uint128_t des = pack(des_ptr, exp_tag + 1);\n";
    hdr << "        bool ok = raw_.compare_exchange_strong(exp, des, success, failure);\n";
    hdr << "        return {ok, unpack(exp)};\n";
    hdr << "    }\n";
    hdr << "};\n\n";

    // §14.12 typed sysreg handles. The runtime template wraps a
    // single backing cell (per-name `inline` storage below) with
    // `.read()` / `.write(v)` methods. v0.5 hosted reads/writes the
    // cell directly so the e2e can verify round-trips without
    // privileged sysreg access; the kernel target swaps this for one
    // inline-asm `mrs` / `msr` per call (aarch64), `rdmsr` / `wrmsr`
    // (x86), or `csrr` / `csrw` (RISC-V). Auto-emitted post-write
    // barriers from §14.12.3 queue for a later slice; the v0.5
    // hosted template is just the typed access surface.
    hdr << "namespace sysreg {\n";
    hdr << "template <class T>\n";
    hdr << "struct Handle {\n";
    hdr << "    T cell;\n";
    hdr << "    [[nodiscard]] T read() const noexcept { return cell; }\n";
    hdr << "    void write(T v) noexcept { cell = v; }\n";
    hdr << "};\n";
    // Canonical aarch64 EL1 subset for v0.5. Each instance lives at
    // file scope with `inline` so multi-TU builds share one storage
    // cell per sysreg. Zero-initialized at static-init; the kernel
    // target replaces these with the architectural register's own
    // power-on / reset value.
    // §14.12.2 — keep this list in lock-step with the resolver's
    // sysregs_ro + sysregs_rw sets. Hosted v0.5 backs each register
    // with a static cell; the kernel target replaces the Handle
    // template's read/write methods with the architectural
    // instruction per arch (mrs/msr on aarch64, rdmsr/wrmsr on
    // x86_64, csrr/csrw on RISC-V).
    static const std::array<std::string_view, 29> sysreg_names = {
        // aarch64 EL1 — read-only
        "midr_el1",
        "mpidr_el1",
        "cntfrq_el0",
        "cntpct_el0",
        "dczid_el0",
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
        // x86_64 MSRs
        "ia32_efer",
        "ia32_lstar",
        "ia32_apic_base",
        "ia32_pat",
        // RISC-V S-mode CSRs
        "sstatus",
        "sie",
        "stvec",
        "sscratch",
        "sepc",
        "scause",
        "stval",
        "satp",
    };
    for (const auto& name : sysreg_names) {
        hdr << "inline Handle<std::uint64_t> " << name << ";\n";
    }

    // §14.12.3 auto-emitted post-write barrier. The aarch64 target
    // table marks SCTLR_EL1 / VBAR_EL1 / TTBR0_EL1 (and others not
    // yet in the v0.5 canonical set) as needing an ISB on the
    // trailing edge so the subsequent instruction stream observes
    // the new control state. On non-aarch64 hosts the shim falls
    // back to a seq_cst thread fence — strong enough for hosted
    // testing without paying the full ISA-specific instruction.
    // The `write_<name>` wrappers below pair the cell update with
    // the barrier call; emit_expr dispatches `Sysreg.X.write(v)` to
    // them when X is in the gated set and the enclosing function
    // doesn't carry `@no_auto_barrier`.
    hdr << "inline void post_write_barrier() noexcept {\n";
    hdr << "#if defined(__aarch64__) || defined(__arm64__)\n";
    hdr << "    __asm__ volatile(\"isb\" ::: \"memory\");\n";
    hdr << "#else\n";
    hdr << "    std::atomic_thread_fence(std::memory_order_seq_cst);\n";
    hdr << "#endif\n";
    hdr << "}\n";
    // §14.12.3 aarch64 EL1 sysregs that require ISB on the trailing
    // edge — control / TLB / vector-base / memory-attribute sysregs
    // whose retire affects fetch / decode state. ttbr1_el1 + tcr_el1
    // + mair_el1 join sctlr_el1 / vbar_el1 / ttbr0_el1 here because
    // they all change the MMU's view of mappings or attributes;
    // subsequent fetches need to see the new state, hence the ISB.
    static const std::array<std::string_view, 6> gated_writes = {
        "sctlr_el1",
        "vbar_el1",
        "ttbr0_el1",
        "ttbr1_el1",
        "tcr_el1",
        "mair_el1",
    };
    for (const auto& name : gated_writes) {
        hdr << "inline void write_" << name << "(std::uint64_t v) noexcept {\n";
        hdr << "    " << name << ".write(v);\n";
        hdr << "    post_write_barrier();\n";
        hdr << "}\n";
    }
    hdr << "}  // namespace sysreg\n\n";

    hdr << "}  // namespace __vstr\n\n";

    // §A5 (§14.10) sync-intrinsic enums + free-function shims. The
    // two enums live at file scope so leading-dot lowering (`.full`,
    // `.loadLoad`) emits as `BarrierScope::full` / `BarrierKind::loadLoad`
    // without needing a namespace prefix. The free-function shims
    // live in __vstr and lower to portable atomic_thread_fence /
    // atomic_signal_fence calls; the pipeline-hint ops use inline
    // asm where the host architecture has a matching instruction and
    // fall back to a no-op on others.
    hdr << "enum class BarrierScope { full, inner, outer, nonShareable };\n";
    hdr << "enum class BarrierKind { loadLoad, storeStore, loadStore, full };\n\n";
    hdr << "namespace __vstr {\n\n";
    hdr << "inline void compilerFence(std::memory_order ord) noexcept {\n";
    hdr << "    std::atomic_signal_fence(ord);\n";
    hdr << "}\n";
    hdr << "inline void memoryBarrier(BarrierScope, BarrierKind kind) noexcept {\n";
    hdr << "    switch (kind) {\n";
    hdr << "    case BarrierKind::loadLoad:\n";
    hdr << "        std::atomic_thread_fence(std::memory_order_acquire);\n";
    hdr << "        break;\n";
    hdr << "    case BarrierKind::storeStore:\n";
    hdr << "        std::atomic_thread_fence(std::memory_order_release);\n";
    hdr << "        break;\n";
    hdr << "    case BarrierKind::loadStore:\n";
    hdr << "        std::atomic_thread_fence(std::memory_order_acq_rel);\n";
    hdr << "        break;\n";
    hdr << "    case BarrierKind::full:\n";
    hdr << "        std::atomic_thread_fence(std::memory_order_seq_cst);\n";
    hdr << "        break;\n";
    hdr << "    }\n";
    hdr << "}\n";
    hdr << "inline void syncBarrier(BarrierScope) noexcept {\n";
    hdr << "    std::atomic_thread_fence(std::memory_order_seq_cst);\n";
    hdr << "}\n";
    hdr << "inline void instructionBarrier() noexcept {\n";
    // ISB on aarch64; on x86 it's mostly self-serializing — the
    // strongest portable equivalent is a seq_cst thread fence plus
    // an asm "memory" clobber so the host compiler treats it as a
    // hard sequence point.
    hdr << "#if defined(__aarch64__) || defined(__arm64__)\n";
    hdr << "    __asm__ volatile(\"isb\" ::: \"memory\");\n";
    hdr << "#else\n";
    hdr << "    std::atomic_thread_fence(std::memory_order_seq_cst);\n";
    hdr << "#endif\n";
    hdr << "}\n";
    hdr << "inline void waitForInterrupt() noexcept { /* hosted no-op */ }\n";
    hdr << "inline void waitForEvent() noexcept { /* hosted no-op */ }\n";
    hdr << "inline void signalEvent() noexcept { /* hosted no-op */ }\n";
    hdr << "inline void relax() noexcept {\n";
    hdr << "#if defined(__x86_64__)\n";
    hdr << "    __builtin_ia32_pause();\n";
    hdr << "#elif defined(__aarch64__) || defined(__arm64__)\n";
    hdr << "    __asm__ volatile(\"yield\" ::: \"memory\");\n";
    hdr << "#endif\n";
    hdr << "}\n";
    hdr << "inline void cpu_nop() noexcept { __asm__ volatile(\"nop\"); }\n\n";
    hdr << "}  // namespace __vstr\n\n";

    // §A5 (§14.10.5) TlbScope: parameter enum for the TLB-
    // invalidation builtins. Like BarrierScope / BarrierKind this
    // emits at file scope so leading-dot lowering picks it up
    // unqualified.
    hdr << "enum class TlbScope { currentEL, allELs, innerShareable, outerShareable };\n\n";

    // §A5 (§14.10.3 / §14.10.4 / §14.10.5) cache + TLB management
    // shims. Hosted builds have no privileged-instruction access;
    // the bodies are no-ops on hosted except `zeroData`, which
    // actually zeroes the span so the e2e can verify the side
    // effect. The kernel build replaces each shim with its real
    // architectural sequence (dc cvac / dc ivac / tlbi vmalle1 /
    // …).
    hdr << "namespace __vstr {\n\n";
    hdr << "inline void cleanData(std::span<const std::uint8_t>) noexcept {}\n";
    hdr << "inline void invalidateData(std::span<const std::uint8_t>) noexcept {}\n";
    hdr << "inline void cleanInvalidateData(std::span<const std::uint8_t>) noexcept {}\n";
    hdr << "inline void zeroData(std::span<std::uint8_t> r) noexcept {\n";
    hdr << "    for (auto& b : r) { b = 0; }\n";
    hdr << "}\n";
    hdr << "inline void publishInstructions(std::span<const std::uint8_t>) noexcept {}\n";
    hdr << "inline void invalidateAllInstructions() noexcept {}\n";
    hdr << "inline void tlbInvalidateAll(TlbScope) noexcept {}\n";
    hdr << "inline void tlbInvalidatePage(std::uint64_t, bool, TlbScope) noexcept {}\n";
    hdr << "inline void tlbInvalidateAsid(std::uint16_t, TlbScope) noexcept {}\n\n";
    // §A7 (§14.14) — Context + Scheduler.swapContext. `Context` is
    // an opaque, target-determined task-saving slot; the spec
    // exposes only `.size` / `.alignment` (via §A2 reflection) and
    // never the field shape. v0.5 hosts emit a 256-byte buffer,
    // which is large enough to cover every architecture's
    // callee-saved register file + interrupt-mask word. The
    // kernel target replaces the struct + the swapContext shim
    // with the matching assembly.
    hdr << "struct alignas(16) Context {\n";
    hdr << "    std::uint8_t _bytes[256];\n";
    hdr << "};\n";
    hdr << "inline void scheduler_swap_context(Context*, const Context*) noexcept {\n";
    hdr << "    // hosted no-op; kernel target emits the save/restore sequence.\n";
    hdr << "}\n\n";
    hdr << "}  // namespace __vstr\n\n";

    // §4 Optional in a Display splice. Vestra renders `"\(opt)"` as
    // `nil` for `.none` and delegates to T's formatter for `.some(v)`.
    // libc++ on Apple Clang 21 doesn't yet ship the C++26 P2585
    // formatter for std::optional, so we provide a partial
    // specialization in namespace std. The constraint on
    // std::formattable<T, char> keeps it composable with anything T's
    // own formatter accepts, and `inline` keeps the spec safe across
    // headers in one TU.
    hdr << "namespace std {\n";
    hdr << "template <class T>\n";
    hdr << "    requires std::formattable<T, char>\n";
    hdr << "struct formatter<std::optional<T>, char> {\n";
    hdr << "    std::formatter<T, char> inner;\n";
    hdr << "    constexpr auto parse(std::format_parse_context& ctx) {\n";
    hdr << "        return inner.parse(ctx);\n";
    hdr << "    }\n";
    hdr << "    template <class FormatContext>\n";
    hdr << "    auto format(const std::optional<T>& opt, FormatContext& ctx) const {\n";
    hdr << "        if (opt.has_value()) { return inner.format(*opt, ctx); }\n";
    hdr << "        return std::format_to(ctx.out(), \"nil\");\n";
    hdr << "    }\n";
    hdr << "};\n";
    hdr << "}  // namespace std\n\n";

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
    if (panic_handler != nullptr) {
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
            emit_expr(hdr, *c.value);
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
        emit_link_attr_prefix(hdr, la);
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
            emit_expr(hdr, *s.value);
        }
        hdr << ";\n";
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
        } else if (f.result) {
            emit_type(os, *f.result);
        } else {
            os << "void";
        }
        if (f.is_async) {
            os << ">";
        }
        os << " " << f.name << "(";
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
                } else if (f.is_async) {
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

    // Templates must be visible at every instantiation site, so a generic
    // function lives entirely in the header. Non-generics keep the
    // declaration/definition split.
    if (has_type_generics) {
        emit_template_prefix(hdr);
        emit_link_attr_prefix(hdr, link_attrs);
        if (has_result) {
            hdr << "[[nodiscard]] ";
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
        } else {
            unsupported(hdr, "non-block function body", f.body->range);
        }
        hdr << "}\n\n";
        return;
    }

    emit_link_attr_prefix(hdr, link_attrs);
    if (has_result) {
        hdr << "[[nodiscard]] ";
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
    emit_tuple_param_prologue(src);
    if (f.body->kind == ast::NodeKind::BlockExpr) {
        const auto& blk = static_cast<const ast::BlockExpr&>(*f.body);
        for (const auto& s : blk.stmts) {
            emit_stmt(src, *s, 1);
        }
        emit_async_void_coreturn(src, f, blk);
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
    if (!f.is_async || f.result != nullptr) {
        return;
    }
    const bool ends_in_return =
        !blk.stmts.empty() && blk.stmts.back()->kind == ast::NodeKind::ReturnStmt;
    if (!ends_in_return) {
        write_indent(os, 1);
        os << "co_return;\n";
    }
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
        if (f.type) {
            emit_type(hdr, *f.type);
        }
        hdr << " " << f.name;
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
        write_indent(hdr, 1);
        hdr << "[[nodiscard]] ";
        if (fn.result) {
            emit_type(hdr, *fn.result);
        } else {
            hdr << "void";
        }
        hdr << " " << fn.name << "(";
        for (std::size_t i = 0; i < fn.params.size(); ++i) {
            if (i != 0) {
                hdr << ", ";
            }
            const auto& p = fn.params[i];
            switch (p.mode) {
            case ast::ParamMode::Read:
                // §11 — an async method copies read params into its coroutine
                // frame (by value), since a `const T&` would dangle across a
                // suspension; a plain method keeps the canonical `const T&`.
                if (fn.is_async) {
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
        hdr << ") ";
        if (fn.body && fn.body->kind == ast::NodeKind::BlockExpr) {
            emit_block(hdr, static_cast<const ast::BlockExpr&>(*fn.body), 1);
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
            if (c.payload[i].second) {
                emit_type(hdr, *c.payload[i].second);
            }
            hdr << " " << (c.payload[i].first.empty() ? std::format("_{}", i) : c.payload[i].first)
                << "{}; ";
        }
        if (wants_eq) {
            hdr << "[[nodiscard]] bool operator==(const " << c.name
                << "_t&) const noexcept = default; ";
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
        // §12.3 derive(Clone): same shape as the struct path.
        if (it->second.contains("Clone")) {
            write_indent(hdr, 1);
            hdr << "[[nodiscard]] " << e.name << " clone() const { return *this; }\n";
        }
    }
    hdr << "};\n\n";
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
    case ast::NodeKind::IdentExpr:
        os << static_cast<const ast::IdentExpr&>(e).name;
        break;
    case ast::NodeKind::SelfExpr:
        os << "(*this)";
        break;
    case ast::NodeKind::ParenExpr:
        os << "(";
        emit_expr(os, *static_cast<const ast::ParenExpr&>(e).inner);
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
                emit_expr(os, *tup.elements[i]);
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
            os << "; return __vstr_r.has_value() ? std::optional{*__vstr_r} : std::nullopt; }())";
            break;
        }
        // Propagating with a hoist registered → emit the hoisted name.
        if (tx.form == ast::TryExpr::Form::Propagating) {
            if (const auto* name = lookup_try_hoist(&tx)) {
                os << *name;
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
            // §9 nil-coalescing: `a ?? b` → `(a).value_or(b)`. value_or
            // eagerly evaluates b (vs the ternary form which short-
            // circuits); for the literal/identifier defaults that are
            // the common case this is fine and reads cleanly.
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
        emit_expr(os, *b.lhs);
        os << " " << binop_text(b.op) << " ";
        emit_expr(os, *b.rhs);
        break;
    }
    case ast::NodeKind::CallExpr: {
        const auto& c = static_cast<const ast::CallExpr&>(e);
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
                    static const std::unordered_set<std::string_view> gated_writes = {
                        "sctlr_el1",
                        "vbar_el1",
                        "ttbr0_el1",
                        "ttbr1_el1",
                        "tcr_el1",
                        "mair_el1",
                    };
                    if (root.name == "Sysreg" && gated_writes.contains(base_mem.member)
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
        // §A7 (§14.14) `Scheduler.swapContext(saving, loading)`
        // lowers to the __vstr runtime shim. v0.5 sema accepts the
        // arguments as `Context` values (because `&decl` from
        // §14.6.3 isn't shipped yet); the codegen wraps each arg
        // in `&` so the shim receives the C++ pointers it expects.
        if (c.callee && c.callee->kind == ast::NodeKind::MemberExpr) {
            const auto& mem = static_cast<const ast::MemberExpr&>(*c.callee);
            if (mem.base != nullptr && mem.base->kind == ast::NodeKind::IdentExpr
                && static_cast<const ast::IdentExpr&>(*mem.base).name == "Scheduler"
                && mem.member == "swapContext" && c.args.size() == 2) {
                os << "__vstr::scheduler_swap_context(&";
                emit_expr(os, *c.args[0].value);
                os << ", &";
                emit_expr(os, *c.args[1].value);
                os << ")";
                break;
            }
        }
        // §10 `Box.new(value)` lowers to `std::make_unique<T>(value)`.
        // The element type T comes from the resolver (the type of the
        // CallExpr itself is Box<T>; we peel that to get T).
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
                os << ">(";
                emit_expr(os, *c.args[0].value);
                os << ")";
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
                os << "; bool __vstr_ok = ";
                emit_expr(os, *mem.base);
                os << (is_strong ? ".compare_exchange_strong(__vstr_e, "
                                 : ".compare_exchange_weak(__vstr_e, ");
                emit_expr(os, *c.args[1].value);
                os << ", ";
                emit_expr(os, *c.args[2].value);
                os << ", ";
                emit_expr(os, *c.args[3].value);
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
            // §18.5 String methods: `append(x)` → `.append(x)` (std::string's
            // string_view overload takes a Str/StrConst arg directly);
            // `len()` → `.size()` as Int.
            if (auto base_t = resolution_->type_of(mem.base.get());
                base_t != nullptr && base_t->kind() == sema::TypeKind::String) {
                if (mem.member == "append" && c.args.size() == 1) {
                    emit_expr(os, *mem.base);
                    os << ".append(";
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
            if (auto it = mem_intrinsics.find(callee_ident.name); it != mem_intrinsics.end()) {
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
            if (mem.base->kind == ast::NodeKind::IdentExpr) {
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
                                    os << ed.name;
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
                                emit_expr(os, *c.args[i].value);
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
                            os << ed.name;
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
                        emit_expr(os, *c.args[i].value);
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
                os << *qn;
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
                os << "{";
                for (std::size_t i = 0; i < c.args.size(); ++i) {
                    if (i != 0) {
                        os << ", ";
                    }
                    if (!c.args[i].label.empty()) {
                        os << "." << c.args[i].label << " = ";
                    }
                    emit_expr(os, *c.args[i].value);
                }
                os << "}";
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
                os << "{";
                for (std::size_t i = 0; i < c.args.size(); ++i) {
                    if (i != 0) {
                        os << ", ";
                    }
                    if (!c.args[i].label.empty()) {
                        os << "." << c.args[i].label << " = ";
                    }
                    emit_expr(os, *c.args[i].value);
                }
                os << "}";
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
                os << "{";
                for (std::size_t i = 0; i < c.args.size(); ++i) {
                    if (i != 0) {
                        os << ", ";
                    }
                    if (!c.args[i].label.empty()) {
                        os << "." << c.args[i].label << " = ";
                    }
                    emit_expr(os, *c.args[i].value);
                }
                os << "}";
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
        const ast::FuncDecl* fn = nullptr;
        if (resolution_ != nullptr) {
            const auto* sym = resolution_->symbol_of(c.callee.get());
            if (sym != nullptr && sym->decl != nullptr && sym->decl->kind == ast::NodeKind::Func) {
                fn = static_cast<const ast::FuncDecl*>(sym->decl);
            }
        }
        emit_expr(os, *c.callee);
        os << "(";
        for (std::size_t i = 0; i < c.args.size(); ++i) {
            if (i != 0) {
                os << ", ";
            }
            const bool is_sink = fn != nullptr && i < fn->params.size()
                                 && fn->params[i].mode == ast::ParamMode::Sink;
            // For a sink param, the value must arrive as an rvalue. The
            // simplest correct lowering: always std::move at the call site.
            // (`std::move(temporary)` is harmless — it just stays an rvalue.)
            if (is_sink) {
                os << "std::move(";
            }
            emit_expr(os, *c.args[i].value);
            if (is_sink) {
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
                os << *qn;
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
        if (resolution_ != nullptr && m.base->kind == ast::NodeKind::IdentExpr) {
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
                            os << enum_decl.name;
                        }
                    };
                    emit_enum_name();
                    os << "{";
                    emit_enum_name();
                    os << "::" << m.member << "_t{}}";
                } else {
                    os << enum_decl.name << "::" << m.member;
                }
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
                if (enum_decl.name == "Ordering") {
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
                            os << enum_decl.name;
                        }
                    };
                    emit_enum_name();
                    os << "{";
                    emit_enum_name();
                    os << "::" << d.name << "_t{}}";
                } else {
                    os << enum_decl.name << "::" << d.name;
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
            os << "([&]{ if (auto __vstr_opt = ";
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
        os << "[&](";
        for (std::size_t i = 0; i < cx.params.size(); ++i) {
            if (i != 0) {
                os << ", ";
            }
            if (fn_type != nullptr && fn_type->kind() == sema::TypeKind::Function
                && i < fn_type->parts().size() && fn_type->parts()[i] != nullptr) {
                emit_sema_type(os, fn_type->parts()[i]);
            } else {
                os << "auto";
            }
            os << " " << cx.params[i];
        }
        os << ") { ";
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
    default:
        unsupported(os, "expression", e.range);
        break;
    }
}

}  // namespace vestra::codegen
