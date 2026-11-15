// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

// The runtime prelude every generated header carries, in two profiles:
// hosted (the libc++-backed default) and freestanding (`--no-libc`). The
// sections both profiles share are emitted by one function each, so the two
// stay in step; the hosted profile calls them in its original order, which
// keeps its output byte-for-byte what it was before the split.

#include "cpp_emitter_internal.hpp"

#include "vestra/sema/sysregs.hpp"

#include <array>
#include <ostream>
#include <string_view>
#include <utility>

namespace vestra::codegen::detail {

namespace {

void emit_prelude_never(std::ostream& hdr) {
    hdr << "enum class Endianness { little, big, native };\n\n";

    hdr << "namespace __vstr {\n\n";
    hdr << "struct Never {\n";
    hdr << "    template <class T> [[noreturn]] operator T() const noexcept {\n";
    hdr << "        std::unreachable();\n";
    hdr << "    }\n";
    hdr << "};\n\n";
    // §19.9 `a ?? b` evaluates `b` only when `a` is none. The default comes
    // from a callback given the payload type, so it converts (or diverges,
    // as a `Never`) exactly as `value_or`'s argument would.
    hdr << "template <class T> struct coalesce_tag { using type = T; };\n";
    hdr << "template <class O, class F>\n";
    hdr << "constexpr auto coalesce(O&& o, F&& f) -> std::remove_cvref_t<decltype(*o)> {\n";
    hdr << "    if (o.has_value()) { return *static_cast<O&&>(o); }\n";
    hdr << "    return f(coalesce_tag<std::remove_cvref_t<decltype(*o)>>{});\n";
    hdr << "}\n\n";
    // §4 `&+ &- &*` wrap. The operation runs in an unsigned type at least
    // as wide as the operands and as `unsigned` (so promotion can't
    // overflow), and the conversion back to the operand type is modular. The result type is the
    // narrower operand's, as sema's sized-side-wins rule picks; an integer
    // literal operand arrives as a `wlit` and takes the other side's type.
    // The helpers use no <type_traits>: the freestanding profile has none.
    hdr << "struct wlit {\n";
    hdr << "    unsigned long long v;\n";
    hdr << "    template <class I> constexpr wlit(I x) : v(static_cast<unsigned long long>(x)) {}\n";
    hdr << "};\n";
    hdr << "template <bool, class A, class B> struct wrap_pick { using type = A; };\n";
    hdr << "template <class A, class B> struct wrap_pick<false, A, B> { using type = B; };\n";
    hdr << "template <class A, class B> struct wrap_ty {\n";
    hdr << "    using type = typename wrap_pick<(sizeof(B) < sizeof(A)), B, A>::type;\n";
    hdr << "};\n";
    hdr << "template <class A> struct wrap_ty<A, wlit> { using type = A; };\n";
    hdr << "template <class B> struct wrap_ty<wlit, B> { using type = B; };\n";
    hdr << "template <> struct wrap_ty<wlit, wlit> { using type = long long; };\n";
    hdr << "template <class T> struct wrap_fp { static constexpr bool v = false; };\n";
    hdr << "template <> struct wrap_fp<float> { static constexpr bool v = true; };\n";
    hdr << "template <> struct wrap_fp<double> { static constexpr bool v = true; };\n";
    hdr << "template <> struct wrap_fp<long double> { static constexpr bool v = true; };\n";
    hdr << "template <class T> struct wrap_uw { using type = unsigned; };\n";
    hdr << "template <class T> requires(sizeof(T) > sizeof(unsigned)) struct wrap_uw<T> {\n";
    hdr << "    using type = unsigned long long;\n";
    hdr << "};\n";
    hdr << "template <class T, class V> constexpr T wrap_in(V x) { return static_cast<T>(x); }\n";
    hdr << "template <class T> constexpr T wrap_in(wlit x) {\n";
    hdr << "    if constexpr (wrap_fp<T>::v) { return static_cast<T>(static_cast<long long>(x.v)); }\n";
    hdr << "    else { return static_cast<T>(x.v); }\n";
    hdr << "}\n";
    static constexpr std::pair<const char*, const char*> wrap_ops[] = {
        {"wrap_add", "+"}, {"wrap_sub", "-"}, {"wrap_mul", "*"}};
    for (const auto& [fn, op] : wrap_ops) {
        hdr << "template <class A, class B>\n";
        hdr << "constexpr auto " << fn << "(A a, B b) -> typename wrap_ty<A, B>::type {\n";
        hdr << "    using T = typename wrap_ty<A, B>::type;\n";
        hdr << "    if constexpr (wrap_fp<T>::v) {\n";
        hdr << "        return wrap_in<T>(a) " << op << " wrap_in<T>(b);\n";
        hdr << "    } else {\n";
        hdr << "        using W = typename wrap_uw<T>::type;\n";
        hdr << "        return static_cast<T>(wrap_in<W>(a) " << op << " wrap_in<W>(b));\n";
        hdr << "    }\n";
        hdr << "}\n";
    }
    hdr << "\n";
}

void emit_prelude_str_queries(std::ostream& hdr) {
    // §18.5 String/Str read-only query helpers. All operate on a
    // std::string_view so the owned-String and borrowed-Str receivers share one
    // lowering. str_slice clamps [from, to) into range (an out-of-range or
    // inverted span yields an empty view rather than UB); str_byte_at folds the
    // bounds check into the `UInt8?` the resolver promises; str_find maps the
    // npos sentinel onto `Int?`.
    hdr << "inline std::string_view str_slice(std::string_view s, std::intptr_t from, "
           "std::intptr_t to) {\n";
    hdr << "    auto n = static_cast<std::intptr_t>(s.size());\n";
    hdr << "    if (from < 0) from = 0;\n";
    hdr << "    if (to > n) to = n;\n";
    hdr << "    if (from > to) from = to;\n";
    hdr << "    return s.substr(static_cast<std::size_t>(from), "
           "static_cast<std::size_t>(to - from));\n";
    hdr << "}\n\n";
    hdr << "inline std::optional<std::uint8_t> str_byte_at(std::string_view s, "
           "std::intptr_t i) {\n";
    hdr << "    if (i < 0 || static_cast<std::size_t>(i) >= s.size()) return std::nullopt;\n";
    hdr << "    return static_cast<std::uint8_t>("
           "static_cast<unsigned char>(s[static_cast<std::size_t>(i)]));\n";
    hdr << "}\n\n";
    hdr << "inline std::optional<std::intptr_t> str_find(std::string_view s, "
           "std::string_view needle) {\n";
    hdr << "    auto p = s.find(needle);\n";
    hdr << "    if (p == std::string_view::npos) return std::nullopt;\n";
    hdr << "    return static_cast<std::intptr_t>(p);\n";
    hdr << "}\n\n";
}

void emit_prelude_char_at(std::ostream& hdr) {
    // §18.5 byte/char access for hand-tokenizing. char_at is the bounds-checked
    // i-th byte as a char32_t (the `Char?` the resolver promises); chars
    // materializes one char32_t per byte for `for c in s.chars()`. Both are
    // byte-oriented in v0.5 (a byte maps to one Char; UTF-8 decoding is a
    // carry-forward). The classifiers are ASCII-only and locale-free.
    hdr << "inline std::optional<char32_t> char_at(std::string_view s, std::intptr_t i) {\n";
    hdr << "    if (i < 0 || static_cast<std::size_t>(i) >= s.size()) return std::nullopt;\n";
    hdr << "    return static_cast<char32_t>("
           "static_cast<unsigned char>(s[static_cast<std::size_t>(i)]));\n";
    hdr << "}\n\n";
}

void emit_prelude_char_classes(std::ostream& hdr) {
    hdr << "inline bool is_digit(char32_t c) { return c >= U'0' && c <= U'9'; }\n";
    hdr << "inline bool is_alpha(char32_t c) {\n";
    hdr << "    return (c >= U'a' && c <= U'z') || (c >= U'A' && c <= U'Z');\n";
    hdr << "}\n";
    hdr << "inline bool is_alnum(char32_t c) { return is_digit(c) || is_alpha(c); }\n";
    hdr << "inline bool is_space(char32_t c) {\n";
    hdr << "    return c == U' ' || c == U'\\t' || c == U'\\n' || c == U'\\r' "
           "|| c == U'\\f' || c == U'\\v';\n";
    hdr << "}\n\n";
}

void emit_prelude_vec_ops(std::ostream& hdr) {
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
}

void emit_prelude_unwrap(std::ostream& hdr) {
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
    // §18 `Optional.take()` moves the value out and leaves `.none`;
    // `put(v)` fills an empty Optional and panics on an occupied one (how a
    // linear value leaves and enters storage, §19.6).
    hdr << "template <class T>\n";
    hdr << "[[nodiscard]] inline std::optional<T> opt_take(std::optional<T>& o) {\n";
    hdr << "    std::optional<T> out = std::move(o);\n";
    hdr << "    o.reset();\n";
    hdr << "    return out;\n";
    hdr << "}\n\n";
    hdr << "template <class T, class U>\n";
    hdr << "inline void opt_put(std::optional<T>& o, U&& v) {\n";
    hdr << "    if (o.has_value()) { panic(\"put into an Optional that already holds a value\"); }\n";
    hdr << "    o.emplace(std::forward<U>(v));\n";
    hdr << "}\n\n";
    // §14.9.1 an ordering held in a variable is checked where it's used: one
    // the operation can't carry panics instead of reaching the C++ atomic
    // (undefined behaviour there). A literal ordering is checked at compile
    // time and never comes through here.
    hdr << R"__ord(inline std::memory_order load_order(std::memory_order o) noexcept {
    if (o == std::memory_order_release) { panic("an atomic load can't carry the ordering '.release'"); }
    if (o == std::memory_order_acq_rel) { panic("an atomic load can't carry the ordering '.acqRel'"); }
    return o;
}

inline std::memory_order store_order(std::memory_order o) noexcept {
    if (o == std::memory_order_acquire) { panic("an atomic store can't carry the ordering '.acquire'"); }
    if (o == std::memory_order_acq_rel) { panic("an atomic store can't carry the ordering '.acqRel'"); }
    return o;
}

inline std::memory_order cas_failure_order(std::memory_order success, std::memory_order failure) noexcept {
    if (failure == std::memory_order_release) { panic("a CAS failure ordering can't be '.release'"); }
    if (failure == std::memory_order_acq_rel) { panic("a CAS failure ordering can't be '.acqRel'"); }
    const bool success_acquires = success == std::memory_order_acquire
        || success == std::memory_order_acq_rel || success == std::memory_order_seq_cst;
    if ((failure == std::memory_order_seq_cst && success != std::memory_order_seq_cst)
        || (failure == std::memory_order_acquire && !success_acquires)) {
        panic("the CAS failure ordering is stronger than its success ordering");
    }
    return failure;
}

)__ord";
    // §4 integer division or remainder by zero traps. `MIN / -1` overflows,
    // which is undefined in C++; it wraps here as `&-` would (0 &- MIN is
    // MIN), and `MIN % -1` is 0. A non-integer operand (a float) divides
    // as before. No <type_traits>: the freestanding profile has none.
    hdr << R"__div(template <class T> struct div_int { static constexpr bool v = false; };
template <> struct div_int<signed char> { static constexpr bool v = true; };
template <> struct div_int<unsigned char> { static constexpr bool v = true; };
template <> struct div_int<char> { static constexpr bool v = true; };
template <> struct div_int<short> { static constexpr bool v = true; };
template <> struct div_int<unsigned short> { static constexpr bool v = true; };
template <> struct div_int<int> { static constexpr bool v = true; };
template <> struct div_int<unsigned> { static constexpr bool v = true; };
template <> struct div_int<long> { static constexpr bool v = true; };
template <> struct div_int<unsigned long> { static constexpr bool v = true; };
template <> struct div_int<long long> { static constexpr bool v = true; };
template <> struct div_int<unsigned long long> { static constexpr bool v = true; };
template <class A, class B>
constexpr auto idiv(A a, B b) -> decltype(a / b) {
    if constexpr (div_int<A>::v && div_int<B>::v) {
        using R = decltype(a / b);
        if (b == B(0)) { panic("division by zero"); }
        if constexpr (R(-1) < R(0) && B(-1) < B(0)) {
            if (b == B(-1)) {
                using W = typename wrap_uw<R>::type;
                return static_cast<R>(W(0) - static_cast<W>(a));
            }
        }
    }
    return a / b;
}
template <class A, class B>
constexpr auto irem(A a, B b) -> decltype(a % b) {
    if constexpr (div_int<A>::v && div_int<B>::v) {
        using R = decltype(a % b);
        if (b == B(0)) { panic("remainder by zero"); }
        if constexpr (R(-1) < R(0) && B(-1) < B(0)) {
            if (b == B(-1)) { return R(0); }
        }
    }
    return a % b;
}
template <class A, class B> constexpr void idiv_assign(A& a, B b) { a = static_cast<A>(idiv(a, b)); }
template <class A, class B> constexpr void irem_assign(A& a, B b) { a = static_cast<A>(irem(a, b)); }
// §19.10 indexing is bounds-checked: `a[i]` on a `[N]T`, Span, MutSpan,
// Vec or chunk iterator panics unless 0 <= i < the base's length. It
// returns what the subscript does (a reference, or a chunk's sub-view), so
// `a[i] = v` and `&a[i]` still work.
template <class C>
constexpr auto index_len(const C& c) {
    if constexpr (requires { c.size(); }) { return c.size(); }
    else { return static_cast<decltype(sizeof(0))>(c.count()); }
}
template <class C, class I>
constexpr decltype(auto) at(C&& c, I i) {
    using Z = decltype(sizeof(0));
    if constexpr (I(-1) < I(0)) {
        if (i < I(0)) { panic("index out of range"); }
    }
    if (static_cast<Z>(i) >= static_cast<Z>(index_len(c))) { panic("index out of range"); }
    return c[static_cast<Z>(i)];
}

)__div";
}

void emit_prelude_span_helpers(std::ostream& hdr) {
    hdr << R"__task(// §11.2 parallel: split `data` into `chunks` disjoint contiguous sub-spans
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
    // (the for-loop copies the struct anyway). Vestra's `cs[i]` reaches
    // operator[] through __vstr::at, which checks i against count().
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

)__task";
}

void emit_prelude_views(std::ostream& hdr) {
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

}

}  // namespace

void emit_hosted_prelude(std::ostream& hdr) {
    emit_prelude_never(hdr);
    // §18.5 HashMap[K, V].get(k) → an owning `std::optional<V>` lookup.
    // std::unordered_map has no optional-returning accessor, so this
    // shim folds the find/end check into the `T?` the resolver promises.
    hdr << "template <class M, class K>\n";
    hdr << "std::optional<typename M::mapped_type> map_get(const M& m, const K& k) {\n";
    hdr << "    auto it = m.find(k);\n";
    hdr << "    if (it == m.end()) return std::nullopt;\n";
    hdr << "    return it->second;\n";
    hdr << "}\n\n";
    // §18.5 HashMap iteration → owned Vecs of the keys / values / (key, value)
    // tuples. Order is unspecified (it follows the unordered_map's bucket walk),
    // matching the resolver's `keys()` / `values()` / `entries()` contract.
    hdr << "template <class M>\n";
    hdr << "std::vector<typename M::key_type> map_keys(const M& m) {\n";
    hdr << "    std::vector<typename M::key_type> out;\n";
    hdr << "    out.reserve(m.size());\n";
    hdr << "    for (const auto& kv : m) out.push_back(kv.first);\n";
    hdr << "    return out;\n";
    hdr << "}\n\n";
    hdr << "template <class M>\n";
    hdr << "std::vector<typename M::mapped_type> map_values(const M& m) {\n";
    hdr << "    std::vector<typename M::mapped_type> out;\n";
    hdr << "    out.reserve(m.size());\n";
    hdr << "    for (const auto& kv : m) out.push_back(kv.second);\n";
    hdr << "    return out;\n";
    hdr << "}\n\n";
    hdr << "template <class M>\n";
    hdr << "std::vector<std::tuple<typename M::key_type, typename M::mapped_type>> "
           "map_entries(const M& m) {\n";
    hdr << "    std::vector<std::tuple<typename M::key_type, typename M::mapped_type>> out;\n";
    hdr << "    out.reserve(m.size());\n";
    hdr << "    for (const auto& kv : m) out.emplace_back(kv.first, kv.second);\n";
    hdr << "    return out;\n";
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
    emit_prelude_str_queries(hdr);
    // §18.5 number<->string. to_string renders any standard arithmetic value via
    // std::format (decimal for integers, shortest round-trip for floats),
    // backing the Alloc-gated `n.toString()`. parse_int folds std::from_chars
    // over the whole view into the `Int?` the resolver promises: nil unless the
    // entire string parses (from_chars rejects a leading '+' / whitespace and a
    // trailing remainder, so the parse is strict and total).
    hdr << "template <class T>\n";
    hdr << "inline std::string to_string(T v) { return std::format(\"{}\", v); }\n\n";
    hdr << "inline std::optional<std::intptr_t> parse_int(std::string_view s) {\n";
    hdr << "    std::intptr_t v{};\n";
    hdr << "    auto first = s.data();\n";
    hdr << "    auto last = s.data() + s.size();\n";
    hdr << "    auto [ptr, ec] = std::from_chars(first, last, v);\n";
    hdr << "    if (ec != std::errc{} || ptr != last) return std::nullopt;\n";
    hdr << "    return v;\n";
    hdr << "}\n\n";
    // §18.5 String/Str.split(sep) → the owned Vec[Str] of the pieces between each
    // occurrence of sep (the views borrow the source). Adjacent separators and a
    // leading/trailing separator produce empty pieces, so the piece count is
    // always (occurrences + 1); an empty separator yields the whole string as a
    // single piece (rather than looping forever).
    hdr << "inline std::vector<std::string_view> split(std::string_view s, "
           "std::string_view sep) {\n";
    hdr << "    std::vector<std::string_view> out;\n";
    hdr << "    if (sep.empty()) { out.push_back(s); return out; }\n";
    hdr << "    std::size_t pos = 0;\n";
    hdr << "    while (true) {\n";
    hdr << "        auto next = s.find(sep, pos);\n";
    hdr << "        if (next == std::string_view::npos) { out.push_back(s.substr(pos)); break; }\n";
    hdr << "        out.push_back(s.substr(pos, next - pos));\n";
    hdr << "        pos = next + sep.size();\n";
    hdr << "    }\n";
    hdr << "    return out;\n";
    hdr << "}\n\n";
    emit_prelude_char_at(hdr);
    hdr << "inline std::vector<char32_t> chars(std::string_view s) {\n";
    hdr << "    std::vector<char32_t> out;\n";
    hdr << "    out.reserve(s.size());\n";
    hdr << "    for (unsigned char ch : s) out.push_back(static_cast<char32_t>(ch));\n";
    hdr << "    return out;\n";
    hdr << "}\n\n";
    emit_prelude_char_classes(hdr);
    // §18 filesystem I/O. read_file slurps the whole file into a string
    // (std::nullopt if it can't be opened); write_file truncates + writes,
    // returning whether the stream stayed good. Both back the Fs-gated
    // readFile / writeFile builtins.
    hdr << "inline std::optional<std::string> read_file(std::string_view path) {\n";
    hdr << "    std::ifstream f{std::string(path), std::ios::binary};\n";
    hdr << "    if (!f) return std::nullopt;\n";
    hdr << "    return std::string{std::istreambuf_iterator<char>(f), "
           "std::istreambuf_iterator<char>()};\n";
    hdr << "}\n\n";
    hdr << "inline bool write_file(std::string_view path, std::string_view contents) {\n";
    hdr << "    std::ofstream f{std::string(path), std::ios::binary | std::ios::trunc};\n";
    hdr << "    if (!f) return false;\n";
    hdr << "    f.write(contents.data(), static_cast<std::streamsize>(contents.size()));\n";
    hdr << "    return static_cast<bool>(f);\n";
    hdr << "}\n\n";
    // §18 command-line arguments. The entry `main` calls args_init(argc, argv);
    // args() returns a copy of the captured argv as string views (program
    // lifetime), backing the Alloc-gated `args()` builtin.
    hdr << "inline std::vector<std::string_view>& args_storage() {\n";
    hdr << "    static std::vector<std::string_view> a;\n";
    hdr << "    return a;\n";
    hdr << "}\n\n";
    hdr << "inline void args_init(int argc, char** argv) {\n";
    hdr << "    auto& a = args_storage();\n";
    hdr << "    a.clear();\n";
    hdr << "    for (int i = 0; i < argc; ++i) a.emplace_back(argv[i]);\n";
    hdr << "}\n\n";
    hdr << "inline std::vector<std::string_view> args() { return args_storage(); }\n\n";
    emit_prelude_vec_ops(hdr);
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
    emit_prelude_unwrap(hdr);
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
        return Duration{idiv(nanos_, static_cast<std::int64_t>(k))};  // §4 traps on 0
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

)__task";
    emit_prelude_span_helpers(hdr);
    hdr << R"__task(// §11 Channel<T>: a typed FIFO queue connecting cooperative tasks. The
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
    emit_prelude_views(hdr);
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
    // One cell per register in the target table (vestra/sema/sysregs.hpp).
    // Each instance lives at file scope with `inline` so multi-TU builds
    // share one storage cell per register, zero-initialized at static init.
    // Hosted, a register is only a cell, so e2e tests can check round trips
    // without privileged access; the freestanding prelude lowers each to its
    // architectural instruction instead.
    for (const auto& r : sema::sysreg_table()) {
        hdr << "inline Handle<std::uint64_t> " << r.name << ";\n";
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
    // §14.12.3 the registers whose writes change translation, exception or
    // interrupt-routing context get a `write_<name>` wrapper that pairs the
    // write with the barrier (the table's sync_after_write column).
    for (const auto& r : sema::sysreg_table()) {
        if (!r.sync_after_write) {
            continue;
        }
        hdr << "inline void write_" << r.name << "(std::uint64_t v) noexcept {\n";
        hdr << "    " << r.name << ".write(v);\n";
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
    // compilerFence is a full compiler barrier, as in the freestanding
    // prelude: an atomic_signal_fence may be optimized away, which deletes a
    // delay loop whose only body is a compilerFence.
    hdr << "inline void compilerFence(std::memory_order) noexcept {\n";
    hdr << "    __asm__ volatile(\"\" ::: \"memory\");\n";
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
    hdr << "}\n";
    // §14.14 Context.start: hosted, the slot only records the entry and
    // stack (there is no switching to do with them).
    hdr << "template <class F>\n";
    hdr << "inline Context context_start(F* entry, std::uint64_t stack) noexcept {\n";
    hdr << "    Context c{};\n";
    hdr << "    const auto e = reinterpret_cast<std::uintptr_t>(entry);\n";
    hdr << "    for (int i = 0; i < 8; ++i) {\n";
    hdr << "        c._bytes[i] = static_cast<std::uint8_t>(e >> (8 * i));\n";
    hdr << "        c._bytes[8 + i] = static_cast<std::uint8_t>(stack >> (8 * i));\n";
    hdr << "    }\n";
    hdr << "    return c;\n";
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

}

namespace {

// §15.5 freestanding: the slice of `std::` generated code names, for a build
// with no C++ standard library at all (`-ffreestanding -nostdlibinc`). Only
// <stddef.h> / <stdint.h> are assumed; those ship with the compiler. Each
// template is the smallest shape the emitter's lowerings rely on; none of it
// allocates. It lives in namespace std because the emitter spells these
// names there in both profiles.
void emit_freestanding_std(std::ostream& hdr) {
    hdr << R"__fs(namespace std {
using ::int8_t; using ::int16_t; using ::int32_t; using ::int64_t;
using ::uint8_t; using ::uint16_t; using ::uint32_t; using ::uint64_t;
using ::intptr_t; using ::uintptr_t; using ::size_t; using ::ptrdiff_t;

[[noreturn]] inline void unreachable() { __builtin_unreachable(); }

template <class T> struct remove_reference { using type = T; };
template <class T> struct remove_reference<T&> { using type = T; };
template <class T> struct remove_reference<T&&> { using type = T; };
template <class T> using remove_reference_t = typename remove_reference<T>::type;
template <class T> struct remove_cv { using type = T; };
template <class T> struct remove_cv<const T> { using type = T; };
template <class T> struct remove_cv<volatile T> { using type = T; };
template <class T> struct remove_cv<const volatile T> { using type = T; };
template <class T> using remove_cvref_t = typename remove_cv<remove_reference_t<T>>::type;
template <class T> using decay_t = remove_cvref_t<T>;
template <class A, class B> inline constexpr bool is_same_v = __is_same(A, B);
template <class T> T&& declval() noexcept;
template <class T> constexpr T&& forward(remove_reference_t<T>& t) noexcept { return static_cast<T&&>(t); }
template <class T> constexpr T&& forward(remove_reference_t<T>&& t) noexcept { return static_cast<T&&>(t); }
template <class T> constexpr remove_reference_t<T>&& move(T&& t) noexcept {
    return static_cast<remove_reference_t<T>&&>(t);
}
template <class F, class... A> using invoke_result_t = decltype(declval<F>()(declval<A>()...));

// ---- concepts the emitter's requires-clauses name (§7 generic bounds)
template <class T> concept equality_comparable = requires(const T& a, const T& b) { { a == b }; { a != b }; };
template <class T> concept totally_ordered = equality_comparable<T> && requires(const T& a, const T& b) {
    { a < b }; { a > b }; { a <= b }; { a >= b };
};
template <class T> concept default_initializable = requires { T{}; T(); };

struct in_place_t { explicit constexpr in_place_t() = default; };
inline constexpr in_place_t in_place{};
struct monostate {};
constexpr bool operator==(monostate, monostate) noexcept { return true; }

// ---- string_view: literals and (pointer, length); no strlen (there's no libc).
class string_view {
  public:
    static constexpr size_t npos = static_cast<size_t>(-1);
    constexpr string_view() = default;
    constexpr string_view(const char* p, size_t n) : p_(p), n_(n) {}
    template <size_t N>
    constexpr string_view(const char (&s)[N]) : p_(s), n_(N - 1) {}
    [[nodiscard]] constexpr size_t size() const { return n_; }
    [[nodiscard]] constexpr size_t length() const { return n_; }
    [[nodiscard]] constexpr bool empty() const { return n_ == 0; }
    [[nodiscard]] constexpr const char* data() const { return p_; }
    [[nodiscard]] constexpr const char* begin() const { return p_; }
    [[nodiscard]] constexpr const char* end() const { return p_ + n_; }
    constexpr char operator[](size_t i) const { return p_[i]; }
    [[nodiscard]] constexpr string_view substr(size_t pos, size_t n = npos) const {
        if (pos > n_) { pos = n_; }
        if (n > n_ - pos) { n = n_ - pos; }
        return string_view{p_ + pos, n};
    }
    [[nodiscard]] constexpr size_t find(string_view needle, size_t from = 0) const {
        if (needle.n_ > n_) { return npos; }
        for (size_t i = from; i + needle.n_ <= n_; ++i) {
            size_t j = 0;
            while (j < needle.n_ && p_[i + j] == needle.p_[j]) { ++j; }
            if (j == needle.n_) { return i; }
        }
        return npos;
    }
    [[nodiscard]] constexpr bool starts_with(string_view p) const { return p.n_ <= n_ && substr(0, p.n_) == p; }
    [[nodiscard]] constexpr bool ends_with(string_view p) const { return p.n_ <= n_ && substr(n_ - p.n_) == p; }
    [[nodiscard]] constexpr bool contains(string_view p) const { return find(p) != npos; }
    friend constexpr bool operator==(string_view a, string_view b) {
        if (a.n_ != b.n_) { return false; }
        for (size_t i = 0; i < a.n_; ++i) {
            if (a.p_[i] != b.p_[i]) { return false; }
        }
        return true;
    }
  private:
    const char* p_ = nullptr;
    size_t n_ = 0;
};

// ---- optional
struct nullopt_t { explicit constexpr nullopt_t(int) {} };
inline constexpr nullopt_t nullopt{0};

template <class T>
class optional {
  public:
    using value_type = T;
    constexpr optional() noexcept {}
    constexpr optional(nullopt_t) noexcept {}
    constexpr optional(const T& v) : has_(true) { new (&u_.v) T(v); }
    constexpr optional(T&& v) : has_(true) { new (&u_.v) T(static_cast<T&&>(v)); }
    template <class... A>
    constexpr explicit optional(in_place_t, A&&... a) : has_(true) { new (&u_.v) T(static_cast<A&&>(a)...); }
    // Trivial whenever T is, so an optional in a `static` needs no exit-time
    // destructor (a freestanding image has no __cxa_atexit) and copies as bytes.
    constexpr optional(const optional&) requires __is_trivially_copyable(T) = default;
    constexpr optional(optional&&) requires __is_trivially_copyable(T) = default;
    constexpr optional& operator=(const optional&) requires __is_trivially_copyable(T) = default;
    constexpr optional& operator=(optional&&) requires __is_trivially_copyable(T) = default;
    constexpr ~optional() requires __is_trivially_destructible(T) = default;
    constexpr optional(const optional& o) : has_(o.has_) { if (has_) { new (&u_.v) T(o.u_.v); } }
    constexpr optional(optional&& o) : has_(o.has_) { if (has_) { new (&u_.v) T(static_cast<T&&>(o.u_.v)); } }
    constexpr ~optional() { reset(); }
    constexpr optional& operator=(const optional& o) {
        if (this != &o) { reset(); if (o.has_) { new (&u_.v) T(o.u_.v); has_ = true; } }
        return *this;
    }
    constexpr optional& operator=(optional&& o) {
        if (this != &o) { reset(); if (o.has_) { new (&u_.v) T(static_cast<T&&>(o.u_.v)); has_ = true; } }
        return *this;
    }
    constexpr optional& operator=(nullopt_t) { reset(); return *this; }
    constexpr void reset() {
        if (has_) {
            if constexpr (!__is_trivially_destructible(T)) { u_.v.~T(); }
            has_ = false;
        }
    }
    template <class... A> constexpr T& emplace(A&&... a) {
        reset();
        new (&u_.v) T(static_cast<A&&>(a)...);
        has_ = true;
        return u_.v;
    }
    [[nodiscard]] constexpr bool has_value() const { return has_; }
    constexpr explicit operator bool() const { return has_; }
    constexpr T& operator*() & { return u_.v; }
    constexpr const T& operator*() const& { return u_.v; }
    constexpr T&& operator*() && { return static_cast<T&&>(u_.v); }
    constexpr T* operator->() { return &u_.v; }
    constexpr const T* operator->() const { return &u_.v; }
    constexpr T& value() & { if (!has_) { __builtin_trap(); } return u_.v; }
    constexpr const T& value() const& { if (!has_) { __builtin_trap(); } return u_.v; }
    template <class U> constexpr T value_or(U&& d) const { return has_ ? u_.v : static_cast<T>(static_cast<U&&>(d)); }
    template <class F> constexpr auto and_then(F&& f) const {
        using R = remove_cvref_t<invoke_result_t<F, const T&>>;
        return has_ ? f(u_.v) : R{};
    }
    template <class F> constexpr auto transform(F&& f) const {
        using R = remove_cvref_t<invoke_result_t<F, const T&>>;
        return has_ ? optional<R>{f(u_.v)} : optional<R>{};
    }
    friend constexpr bool operator==(const optional& o, nullopt_t) { return !o.has_; }
    friend constexpr bool operator==(const optional& a, const optional& b) {
        return a.has_ == b.has_ && (!a.has_ || a.u_.v == b.u_.v);
    }
    template <class U> requires(!__is_same(remove_cvref_t<U>, optional) && !__is_same(remove_cvref_t<U>, nullopt_t))
    friend constexpr bool operator==(const optional& o, const U& v) { return o.has_ && o.u_.v == v; }
  private:
    union U {
        constexpr U() {}
        constexpr U(const U&) = default;
        constexpr U(U&&) = default;
        constexpr U& operator=(const U&) = default;
        constexpr U& operator=(U&&) = default;
        constexpr ~U() requires __is_trivially_destructible(T) = default;
        constexpr ~U() {}
        T v;
    } u_;
    bool has_ = false;
};
template <class T> optional(T) -> optional<T>;

// ---- array (an aggregate, like the real one)
template <class T, size_t N>
struct array {
    T _v[N];
    using value_type = T;
    constexpr T& operator[](size_t i) { return _v[i]; }
    constexpr const T& operator[](size_t i) const { return _v[i]; }
    [[nodiscard]] constexpr size_t size() const { return N; }
    constexpr T* data() { return _v; }
    constexpr const T* data() const { return _v; }
    constexpr T* begin() { return _v; }
    constexpr const T* begin() const { return _v; }
    constexpr T* end() { return _v + N; }
    constexpr const T* end() const { return _v + N; }
    friend constexpr bool operator==(const array& a, const array& b) {
        for (size_t i = 0; i < N; ++i) {
            if (!(a._v[i] == b._v[i])) { return false; }
        }
        return true;
    }
};

// ---- span (dynamic extent only)
template <class T>
class span {
  public:
    using element_type = T;
    using value_type = remove_cv<T>::type;
    constexpr span() = default;
    constexpr span(T* p, size_t n) : p_(p), n_(n) {}
    template <size_t N> constexpr span(T (&a)[N]) : p_(a), n_(N) {}
    template <class U, size_t N> requires __is_convertible(U (*)[], T (*)[])
    constexpr span(array<U, N>& a) : p_(a.data()), n_(N) {}
    template <class U, size_t N> requires __is_convertible(const U (*)[], T (*)[])
    constexpr span(const array<U, N>& a) : p_(a.data()), n_(N) {}
    template <class U> requires(!__is_same(U, T) && __is_convertible(U (*)[], T (*)[]))
    constexpr span(const span<U>& o) : p_(o.data()), n_(o.size()) {}
    [[nodiscard]] constexpr size_t size() const { return n_; }
    [[nodiscard]] constexpr bool empty() const { return n_ == 0; }
    constexpr T* data() const { return p_; }
    constexpr T* begin() const { return p_; }
    constexpr T* end() const { return p_ + n_; }
    constexpr T& operator[](size_t i) const { return p_[i]; }
    [[nodiscard]] constexpr span subspan(size_t off, size_t count = static_cast<size_t>(-1)) const {
        if (off > n_) { off = n_; }
        if (count > n_ - off) { count = n_ - off; }
        return span{p_ + off, count};
    }
  private:
    T* p_ = nullptr;
    size_t n_ = 0;
};
template <class T, size_t N> span(array<T, N>&) -> span<T>;
template <class T, size_t N> span(const array<T, N>&) -> span<const T>;

// ---- atomics: the compiler's __atomic builtins, lock-free or a link error
// (there is no libatomic to fall back on).
enum class memory_order : int {
    relaxed = __ATOMIC_RELAXED, consume = __ATOMIC_CONSUME, acquire = __ATOMIC_ACQUIRE,
    release = __ATOMIC_RELEASE, acq_rel = __ATOMIC_ACQ_REL, seq_cst = __ATOMIC_SEQ_CST
};
inline constexpr memory_order memory_order_relaxed = memory_order::relaxed;
inline constexpr memory_order memory_order_consume = memory_order::consume;
inline constexpr memory_order memory_order_acquire = memory_order::acquire;
inline constexpr memory_order memory_order_release = memory_order::release;
inline constexpr memory_order memory_order_acq_rel = memory_order::acq_rel;
inline constexpr memory_order memory_order_seq_cst = memory_order::seq_cst;
inline void atomic_thread_fence(memory_order o) noexcept { __atomic_thread_fence(static_cast<int>(o)); }
inline void atomic_signal_fence(memory_order o) noexcept { __atomic_signal_fence(static_cast<int>(o)); }

template <class T>
struct atomic {
    T v_;
    constexpr atomic() noexcept : v_{} {}
    constexpr atomic(T v) noexcept : v_(v) {}
    atomic(const atomic&) = delete;
    atomic& operator=(const atomic&) = delete;
#define VESTRA_MO(o) static_cast<int>(o)
    T load(memory_order o = memory_order::seq_cst) const noexcept { return __atomic_load_n(&v_, VESTRA_MO(o)); }
    void store(T v, memory_order o = memory_order::seq_cst) noexcept { __atomic_store_n(&v_, v, VESTRA_MO(o)); }
    T exchange(T v, memory_order o = memory_order::seq_cst) noexcept { return __atomic_exchange_n(&v_, v, VESTRA_MO(o)); }
    T fetch_add(T d, memory_order o = memory_order::seq_cst) noexcept { return __atomic_fetch_add(&v_, d, VESTRA_MO(o)); }
    T fetch_sub(T d, memory_order o = memory_order::seq_cst) noexcept { return __atomic_fetch_sub(&v_, d, VESTRA_MO(o)); }
    T fetch_and(T d, memory_order o = memory_order::seq_cst) noexcept { return __atomic_fetch_and(&v_, d, VESTRA_MO(o)); }
    T fetch_or(T d, memory_order o = memory_order::seq_cst) noexcept { return __atomic_fetch_or(&v_, d, VESTRA_MO(o)); }
    T fetch_xor(T d, memory_order o = memory_order::seq_cst) noexcept { return __atomic_fetch_xor(&v_, d, VESTRA_MO(o)); }
    bool compare_exchange_strong(T& exp, T des, memory_order s, memory_order f) noexcept {
        return __atomic_compare_exchange_n(&v_, &exp, des, false, VESTRA_MO(s), VESTRA_MO(f));
    }
    bool compare_exchange_strong(T& exp, T des, memory_order o = memory_order::seq_cst) noexcept {
        return compare_exchange_strong(exp, des, o, o == memory_order::acq_rel ? memory_order::acquire
                                                     : o == memory_order::release ? memory_order::relaxed : o);
    }
    bool compare_exchange_weak(T& exp, T des, memory_order s, memory_order f) noexcept {
        return __atomic_compare_exchange_n(&v_, &exp, des, true, VESTRA_MO(s), VESTRA_MO(f));
    }
    bool compare_exchange_weak(T& exp, T des, memory_order o = memory_order::seq_cst) noexcept {
        return compare_exchange_weak(exp, des, o, o == memory_order::acq_rel ? memory_order::acquire
                                                   : o == memory_order::release ? memory_order::relaxed : o);
    }
#undef VESTRA_MO
};

// ---- bit
enum class endian { little = __ORDER_LITTLE_ENDIAN__, big = __ORDER_BIG_ENDIAN__, native = __BYTE_ORDER__ };
template <class T> constexpr T byteswap(T v) noexcept {
    if constexpr (sizeof(T) == 1) { return v; }
    else if constexpr (sizeof(T) == 2) { return static_cast<T>(__builtin_bswap16(static_cast<uint16_t>(v))); }
    else if constexpr (sizeof(T) == 4) { return static_cast<T>(__builtin_bswap32(static_cast<uint32_t>(v))); }
    else { return static_cast<T>(__builtin_bswap64(static_cast<uint64_t>(v))); }
}

// ---- tuple (and structured bindings)
template <size_t... I> struct index_sequence {};
template <class T, T... I> struct __vstr_seq { using type = index_sequence<static_cast<size_t>(I)...>; };
template <size_t N> using make_index_sequence = typename __make_integer_seq<__vstr_seq, size_t, N>::type;
template <size_t I, class... Ts> struct __vstr_nth;
template <class T, class... Ts> struct __vstr_nth<0, T, Ts...> { using type = T; };
template <size_t I, class T, class... Ts> struct __vstr_nth<I, T, Ts...> { using type = typename __vstr_nth<I - 1, Ts...>::type; };

template <size_t I, class T> struct __vstr_leaf { T v; };
template <class S, class... Ts> struct __vstr_tuple;
template <size_t... I, class... Ts>
struct __vstr_tuple<index_sequence<I...>, Ts...> : __vstr_leaf<I, Ts>... {
    constexpr __vstr_tuple() = default;
    template <class... Us> constexpr __vstr_tuple(Us&&... us) : __vstr_leaf<I, Ts>{static_cast<Ts>(static_cast<Us&&>(us))}... {}
};
template <class... Ts>
struct tuple : __vstr_tuple<make_index_sequence<sizeof...(Ts)>, Ts...> {
    using __base = __vstr_tuple<make_index_sequence<sizeof...(Ts)>, Ts...>;
    constexpr tuple() = default;
    template <class... Us> requires(sizeof...(Us) == sizeof...(Ts) && sizeof...(Ts) > 0
                                    && !(sizeof...(Us) == 1 && (__is_same(remove_cvref_t<Us>, tuple) || ...)))
    constexpr tuple(Us&&... us) : __base(static_cast<Us&&>(us)...) {}
    // Element-wise conversion: `return (0, 0)` into a `(UInt64, UInt64)` result.
    template <class... Us> requires(sizeof...(Us) == sizeof...(Ts) && !__is_same(tuple<Us...>, tuple))
    constexpr tuple(const tuple<Us...>& o) : tuple(o, make_index_sequence<sizeof...(Ts)>{}) {}
  private:
    template <class O, size_t... I>
    constexpr tuple(const O& o, index_sequence<I...>)
        : __base(static_cast<const __vstr_leaf<I, typename O::template __elem<I>>&>(o).v...) {}
  public:
    template <size_t I> using __elem = typename __vstr_nth<I, Ts...>::type;
};
template <class... Us> tuple(Us...) -> tuple<Us...>;
template <size_t I, class... Ts> constexpr auto& get(tuple<Ts...>& t) {
    return static_cast<__vstr_leaf<I, typename __vstr_nth<I, Ts...>::type>&>(t).v;
}
template <size_t I, class... Ts> constexpr const auto& get(const tuple<Ts...>& t) {
    return static_cast<const __vstr_leaf<I, typename __vstr_nth<I, Ts...>::type>&>(t).v;
}
template <size_t I, class... Ts> constexpr auto&& get(tuple<Ts...>&& t) {
    using E = typename __vstr_nth<I, Ts...>::type;
    return static_cast<E&&>(static_cast<__vstr_leaf<I, E>&>(t).v);
}
template <class T> struct tuple_size;
template <class... Ts> struct tuple_size<tuple<Ts...>> { static constexpr size_t value = sizeof...(Ts); };
template <class T> struct tuple_size<const T> { static constexpr size_t value = tuple_size<T>::value; };
template <size_t I, class T> struct tuple_element;
template <size_t I, class... Ts> struct tuple_element<I, tuple<Ts...>> { using type = typename __vstr_nth<I, Ts...>::type; };
template <size_t I, class T> struct tuple_element<I, const T> { using type = const typename tuple_element<I, T>::type; };

// ---- variant / visit / holds_alternative / get
template <class T, class... Ts> constexpr size_t __vstr_index_of() {
    size_t i = 0, found = static_cast<size_t>(-1);
    ((__is_same(T, Ts) && found == static_cast<size_t>(-1) ? (found = i, ++i) : ++i), ...);
    return found;
}
template <class... Ts> constexpr size_t __vstr_max_size() { size_t m = 1; ((m = sizeof(Ts) > m ? sizeof(Ts) : m), ...); return m; }

template <class... Ts>
class variant {
  public:
    constexpr variant() : idx_(0) { new (buf_) typename __vstr_nth<0, Ts...>::type{}; }
    template <class T, size_t I = __vstr_index_of<remove_cvref_t<T>, Ts...>()>
        requires(I != static_cast<size_t>(-1))
    constexpr variant(T&& v) : idx_(I) { new (buf_) remove_cvref_t<T>(static_cast<T&&>(v)); }
    // Trivial whenever every alternative is (see optional).
    variant(const variant&) requires(__is_trivially_copyable(Ts) && ...) = default;
    variant(variant&&) requires(__is_trivially_copyable(Ts) && ...) = default;
    variant& operator=(const variant&) requires(__is_trivially_copyable(Ts) && ...) = default;
    variant& operator=(variant&&) requires(__is_trivially_copyable(Ts) && ...) = default;
    ~variant() requires(__is_trivially_destructible(Ts) && ...) = default;
    variant(const variant& o) : idx_(o.idx_) { o.__each([&](const auto& a) { new (buf_) remove_cvref_t<decltype(a)>(a); }); }
    variant(variant&& o) : idx_(o.idx_) {
        o.__each([&](auto& a) { new (buf_) remove_cvref_t<decltype(a)>(static_cast<remove_cvref_t<decltype(a)>&&>(a)); });
    }
    variant& operator=(const variant& o) {
        if (this != &o) { this->~variant(); new (this) variant(o); }
        return *this;
    }
    variant& operator=(variant&& o) {
        if (this != &o) { this->~variant(); new (this) variant(static_cast<variant&&>(o)); }
        return *this;
    }
    ~variant() { __each([](auto& a) { using A = remove_cvref_t<decltype(a)>; a.~A(); }); }
    [[nodiscard]] constexpr size_t index() const { return idx_; }
    friend bool operator==(const variant& a, const variant& b) { return a.idx_ == b.idx_ && a.__eq_at<0>(b); }
    friend bool operator<(const variant& a, const variant& b) { return a.idx_ != b.idx_ ? a.idx_ < b.idx_ : a.__lt_at<0>(b); }
    template <size_t I> constexpr auto& __get() { return *reinterpret_cast<typename __vstr_nth<I, Ts...>::type*>(buf_); }
    template <size_t I> constexpr const auto& __get() const { return *reinterpret_cast<const typename __vstr_nth<I, Ts...>::type*>(buf_); }
    template <class F> void __each(F&& f) { __each_at<0>(f); }
    template <class F> void __each(F&& f) const { __each_at<0>(f); }
  private:
    template <size_t I> bool __eq_at(const variant& b) const {
        if constexpr (I < sizeof...(Ts)) {
            return idx_ == I ? __get<I>() == b.template __get<I>() : __eq_at<I + 1>(b);
        } else {
            return false;
        }
    }
    template <size_t I> bool __lt_at(const variant& b) const {
        if constexpr (I < sizeof...(Ts)) {
            return idx_ == I ? __get<I>() < b.template __get<I>() : __lt_at<I + 1>(b);
        } else {
            return false;
        }
    }
    template <size_t I, class F> void __each_at(F& f) {
        if constexpr (I < sizeof...(Ts)) { if (idx_ == I) { f(__get<I>()); } else { __each_at<I + 1>(f); } }
    }
    template <size_t I, class F> void __each_at(F& f) const {
        if constexpr (I < sizeof...(Ts)) { if (idx_ == I) { f(__get<I>()); } else { __each_at<I + 1>(f); } }
    }
    alignas(Ts...) unsigned char buf_[__vstr_max_size<Ts...>()];
    size_t idx_;
};
template <class V> struct __vstr_variant_size;
template <class... Ts> struct __vstr_variant_size<variant<Ts...>> { static constexpr size_t value = sizeof...(Ts); };
template <size_t I, size_t N, class F, class V>
constexpr decltype(auto) __vstr_visit(F& f, V& v) {
    if constexpr (I + 1 == N) {
        return f(v.template __get<I>());
    } else {
        if (v.index() == I) { return f(v.template __get<I>()); }
        return __vstr_visit<I + 1, N>(f, v);
    }
}
template <class F, class V>
constexpr decltype(auto) visit(F&& f, V&& v) {
    return __vstr_visit<0, __vstr_variant_size<remove_cvref_t<V>>::value>(f, v);
}
template <class T, class... Ts> constexpr bool holds_alternative(const variant<Ts...>& v) {
    return v.index() == __vstr_index_of<T, Ts...>();
}
template <class T, class... Ts> constexpr T& get(variant<Ts...>& v) { return v.template __get<__vstr_index_of<T, Ts...>()>(); }
template <class T, class... Ts> constexpr const T& get(const variant<Ts...>& v) {
    return v.template __get<__vstr_index_of<T, Ts...>()>();
}

// ---- expected / unexpected
template <class E>
struct unexpected {
    E e_;
    constexpr explicit unexpected(E v) : e_(static_cast<E&&>(v)) {}
    constexpr const E& error() const { return e_; }
};
template <class E> unexpected(E) -> unexpected<E>;

template <class T, class E>
class expected {
  public:
    using value_type = T;
    using error_type = E;
    constexpr expected() : ok_(true), v_{} {}
    template <class U = T>
        requires(!__is_same(remove_cvref_t<U>, expected) && !__is_same(remove_cvref_t<U>, unexpected<E>))
    constexpr expected(U&& v) : ok_(true), v_(static_cast<U&&>(v)) {}
    template <class G> constexpr expected(const unexpected<G>& u) : ok_(false), e_(u.error()) {}
    [[nodiscard]] constexpr bool has_value() const { return ok_; }
    constexpr explicit operator bool() const { return ok_; }
    constexpr T& operator*() & { return v_; }
    constexpr const T& operator*() const& { return v_; }
    constexpr T&& operator*() && { return static_cast<T&&>(v_); }
    constexpr T* operator->() { return &v_; }
    constexpr const T* operator->() const { return &v_; }
    constexpr T& value() & { if (!ok_) { __builtin_trap(); } return v_; }
    constexpr const T& value() const& { if (!ok_) { __builtin_trap(); } return v_; }
    constexpr const E& error() const { return e_; }
    template <class F> constexpr auto transform_error(F&& f) const {
        using G = remove_cvref_t<invoke_result_t<F, const E&>>;
        if (ok_) { return expected<T, G>(v_); }
        return expected<T, G>(unexpected<G>(f(e_)));
    }
  private:
    bool ok_;
    T v_{};
    E e_{};
};
template <class E>
class expected<void, E> {
  public:
    using value_type = void;
    using error_type = E;
    constexpr expected() : ok_(true) {}
    template <class G> constexpr expected(const unexpected<G>& u) : ok_(false), e_(u.error()) {}
    [[nodiscard]] constexpr bool has_value() const { return ok_; }
    constexpr explicit operator bool() const { return ok_; }
    constexpr void operator*() const {}
    constexpr void value() const { if (!ok_) { __builtin_trap(); } }
    constexpr const E& error() const { return e_; }
    template <class F> constexpr auto transform_error(F&& f) const {
        using G = remove_cvref_t<invoke_result_t<F, const E&>>;
        if (ok_) { return expected<void, G>(); }
        return expected<void, G>(unexpected<G>(f(e_)));
    }
  private:
    bool ok_;
    E e_{};
};
// ---- hash (derive(Hash) specializes it; these cover the field types)
template <class T> struct hash;
template <class T> requires(__is_integral(T) || __is_enum(T))
struct hash<T> {
    constexpr size_t operator()(T v) const noexcept { return static_cast<size_t>(v); }
};
template <class T> requires(__is_floating_point(T))
struct hash<T> {
    size_t operator()(T v) const noexcept {
        if (v == T{}) { return 0; }  // +0 and -0 hash alike
        if constexpr (sizeof(T) == 4) { return __builtin_bit_cast(uint32_t, v); }
        else { return static_cast<size_t>(__builtin_bit_cast(uint64_t, v)); }
    }
};
template <class T> struct hash<T*> {
    size_t operator()(T* p) const noexcept { return reinterpret_cast<uintptr_t>(p); }
};
template <> struct hash<string_view> {
    constexpr size_t operator()(string_view s) const noexcept {
        uint64_t h = 1469598103934665603ull;  // FNV-1a
        for (size_t i = 0; i < s.size(); ++i) { h = (h ^ static_cast<unsigned char>(s[i])) * 1099511628211ull; }
        return static_cast<size_t>(h);
    }
};
}  // namespace std

// Placement new, for the templates above (there's no <new>).
[[nodiscard]] inline void* operator new(std::size_t, void* p) noexcept { return p; }

)__fs";
}

// A target with no lowering for an operation gets a deleted declaration with
// the reason, so a call fails to compile rather than silently doing nothing.
void emit_unlowered_macro(std::ostream& hdr) {
    hdr << R"__fs(#if defined(__cpp_deleted_function) && __cpp_deleted_function >= 202403L
#define VESTRA_UNLOWERED(why) = delete(why)
#else
#define VESTRA_UNLOWERED(why) = delete
#endif

)__fs";
}

// §10 / §15.5 the freestanding panic path: the @panic_handler if there is
// one, else park the CPU. Nothing to print to, nothing to abort to. The
// handler is bound at link time (F5): the unit that declares it defines
// `__vstr_panic_hook`, and the weak declaration here is null without one, so
// no static initializer (and no .init_array) is involved.
void emit_freestanding_panic(std::ostream& hdr) {
    hdr << R"__fs(extern "C" [[gnu::weak]] void __vstr_panic_hook(const std::string_view& message, const std::string_view& file, const std::intptr_t& line) noexcept;

[[noreturn]] inline void halt() noexcept {
    for (;;) {
#if defined(__aarch64__)
        __asm__ volatile("wfe");
#elif defined(__x86_64__)
        __asm__ volatile("hlt");
#elif defined(__riscv)
        __asm__ volatile("wfi");
#endif
    }
}

[[noreturn]] inline Never panic(std::string_view msg) noexcept {
    if (&__vstr_panic_hook != nullptr) {
        const std::string_view empty_file{};
        const std::intptr_t zero_line = 0;
        __vstr_panic_hook(msg, empty_file, zero_line);
    }
    halt();
}

[[noreturn]] inline Never abort_fn() noexcept {
    halt();
}

[[noreturn]] inline Never unreachable_fn() noexcept {
    std::unreachable();
}

)__fs";
}

// §14.12 system registers, lowered to the architectural instruction. Each
// register is its own type, so a direct `Sysreg.x.read()` is one inline
// `mrs` / `rdmsr` / `csrr`, and a register from another architecture is a
// deleted function. `Handle<T>` (the type of a SysregHandle value) carries
// the register's id and dispatches on it.
using sema::SysregAccess;
using sema::SysregInfo;

// The preprocessor macro that selects a register's architecture.
std::string_view arch_macro(std::string_view arch) {
    if (arch == "aarch64") {
        return "__aarch64__";
    }
    if (arch == "x86_64") {
        return "__x86_64__";
    }
    return "__riscv";
}

void emit_sysreg_read(std::ostream& hdr, const SysregInfo& r) {
    hdr << "    [[nodiscard]] std::uint64_t read() const noexcept {\n";
    if (r.arch == "aarch64") {
        hdr << "        std::uint64_t v;\n";
        hdr << "        __asm__ volatile(\"mrs %0, " << r.name << "\" : \"=r\"(v));\n";
        hdr << "        return v;\n";
    } else if (r.arch == "x86_64") {
        hdr << "        std::uint32_t lo, hi;\n";
        hdr << "        __asm__ volatile(\"rdmsr\" : \"=a\"(lo), \"=d\"(hi) : \"c\"(" << r.msr << "));\n";
        hdr << "        return (static_cast<std::uint64_t>(hi) << 32) | lo;\n";
    } else {
        hdr << "        std::uint64_t v;\n";
        hdr << "        __asm__ volatile(\"csrr %0, " << r.name << "\" : \"=r\"(v));\n";
        hdr << "        return v;\n";
    }
    hdr << "    }\n";
}

void emit_sysreg_write(std::ostream& hdr, const SysregInfo& r) {
    hdr << "    void write(std::uint64_t v) noexcept {\n";
    if (r.arch == "aarch64") {
        hdr << "        __asm__ volatile(\"msr " << r.name << ", %0\" :: \"r\"(v) : \"memory\");\n";
    } else if (r.arch == "x86_64") {
        hdr << "        __asm__ volatile(\"wrmsr\" :: \"c\"(" << r.msr
            << "), \"a\"(static_cast<std::uint32_t>(v)), \"d\"(static_cast<std::uint32_t>(v >> 32)) : "
               "\"memory\");\n";
    } else {
        hdr << "        __asm__ volatile(\"csrw " << r.name << ", %0\" :: \"r\"(v) : \"memory\");\n";
    }
    hdr << "    }\n";
}

std::string_view arch_label(std::string_view arch) {
    return arch == "riscv64" ? "RISC-V" : arch;
}

bool readable(const SysregInfo& r) {
    return r.access != SysregAccess::WriteOnly;
}
bool writable(const SysregInfo& r) {
    return r.access != SysregAccess::ReadOnly;
}

void emit_freestanding_sysregs(std::ostream& hdr) {
    const auto& table = sema::sysreg_table();
    hdr << "namespace sysreg {\n";
    hdr << "enum class Reg : unsigned {\n";
    for (const auto& r : table) {
        hdr << "    " << r.name << ",\n";
    }
    hdr << "};\n";
    hdr << "template <class T>\n";
    hdr << "struct Handle {\n";
    hdr << "    Reg id;\n";
    hdr << "    [[nodiscard]] T read() const noexcept;\n";
    hdr << "    void write(T v) noexcept;\n";
    hdr << "};\n\n";
    for (const auto& r : table) {
        hdr << "struct " << r.name << "_t {\n";
        hdr << "#if defined(" << arch_macro(r.arch) << ")\n";
        if (readable(r)) {
            emit_sysreg_read(hdr, r);
        } else {
            hdr << "    std::uint64_t read() const noexcept VESTRA_UNLOWERED(\"" << r.name
                << " is write-only\");\n";
        }
        if (writable(r)) {
            emit_sysreg_write(hdr, r);
        }
        hdr << "#else\n";
        hdr << "    std::uint64_t read() const noexcept VESTRA_UNLOWERED(\"" << r.name << " is an "
            << arch_label(r.arch) << " register\");\n";
        if (writable(r)) {
            hdr << "    void write(std::uint64_t) noexcept VESTRA_UNLOWERED(\"" << r.name << " is an "
                << arch_label(r.arch) << " register\");\n";
        }
        hdr << "#endif\n";
        hdr << "    operator Handle<std::uint64_t>() const noexcept { return {Reg::" << r.name
            << "}; }\n";
        hdr << "};\n";
        hdr << "inline " << r.name << "_t " << r.name << ";\n\n";
    }
    // Handle dispatch: only this target's registers have a case.
    for (int w = 0; w < 2; ++w) {
        if (w == 0) {
            hdr << "template <class T>\n";
            hdr << "T Handle<T>::read() const noexcept {\n";
        } else {
            hdr << "template <class T>\n";
            hdr << "void Handle<T>::write(T v) noexcept {\n";
        }
        hdr << "    switch (id) {\n";
        std::string_view open_arch;
        for (const auto& r : table) {
            if (w == 0 ? !readable(r) : !writable(r)) {
                continue;
            }
            if (r.arch != open_arch) {
                if (!open_arch.empty()) {
                    hdr << "#endif\n";
                }
                hdr << "#if defined(" << arch_macro(r.arch) << ")\n";
                open_arch = r.arch;
            }
            if (w == 0) {
                hdr << "    case Reg::" << r.name << ": return static_cast<T>(" << r.name
                    << ".read());\n";
            } else {
                hdr << "    case Reg::" << r.name << ": " << r.name
                    << ".write(static_cast<std::uint64_t>(v)); return;\n";
            }
        }
        hdr << "#endif\n";
        hdr << "    default: break;\n";
        hdr << "    }\n";
        hdr << (w == 0 ? "    panic(\"system register is not readable on this target\");\n"
                       : "    panic(\"system register is not writable on this target\");\n");
        hdr << "}\n";
    }
    // §14.12.3 auto-emitted post-write barriers (the gated set codegen routes
    // `Sysreg.X.write(v)` through unless the function is @no_auto_barrier).
    // On RISC-V the one gated register is satp: a new translation root takes
    // effect for later accesses only after an SFENCE.VMA.
    hdr << R"__fs(inline void post_write_barrier() noexcept {
#if defined(__aarch64__)
    __asm__ volatile("isb" ::: "memory");
#elif defined(__riscv)
    __asm__ volatile("sfence.vma zero, zero" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}
)__fs";
    // On another architecture a wrapper is as unavailable as its register.
    for (const auto& r : table) {
        if (!r.sync_after_write) {
            continue;
        }
        hdr << "#if defined(" << arch_macro(r.arch) << ")\n";
        hdr << "inline void write_" << r.name << "(std::uint64_t v) noexcept {\n";
        hdr << "    " << r.name << ".write(v);\n";
        hdr << "    post_write_barrier();\n";
        hdr << "}\n";
        hdr << "#else\n";
        hdr << "inline void write_" << r.name << "(std::uint64_t) noexcept VESTRA_UNLOWERED(\""
            << r.name << " is an " << arch_label(r.arch) << " register\");\n";
        hdr << "#endif\n";
    }
    hdr << "}  // namespace sysreg\n\n";
}

// §14.10 barriers, pipeline hints, cache and TLB maintenance: the target
// table. aarch64 is complete; x86_64 and RISC-V lower what has a standard
// instruction and delete the rest. A scope the hardware can't honor from
// here (a broadcast TLB shootdown on x86_64 or RISC-V, `allELs` from EL1)
// panics rather than degrading to a narrower operation (§14.10.5).
void emit_freestanding_machine(std::ostream& hdr) {
    hdr << R"__fs(enum class BarrierScope { full, inner, outer, nonShareable };
enum class BarrierKind { loadLoad, storeStore, loadStore, full };

namespace __vstr {

// A full compiler barrier (the kernel `barrier()`), which satisfies every
// order. Unlike atomic_signal_fence it is never optimized away, so a delay
// loop whose only body is a compilerFence keeps its iterations.
inline void compilerFence(std::memory_order) noexcept {
    __asm__ volatile("" ::: "memory");
}

#if defined(__aarch64__)
inline void memoryBarrier(BarrierScope scope, BarrierKind kind) noexcept {
    // `dmb sy` / `dmb ld` / `dmb st` for the full system; ish / osh / nsh narrow it.
    switch (scope) {
    case BarrierScope::full:
        switch (kind) {
        case BarrierKind::loadLoad:   __asm__ volatile("dmb ld" ::: "memory"); break;
        case BarrierKind::storeStore: __asm__ volatile("dmb st" ::: "memory"); break;
        case BarrierKind::loadStore:  __asm__ volatile("dmb ld" ::: "memory"); break;
        case BarrierKind::full:       __asm__ volatile("dmb sy" ::: "memory"); break;
        }
        break;
    case BarrierScope::inner:
        switch (kind) {
        case BarrierKind::loadLoad:   __asm__ volatile("dmb ishld" ::: "memory"); break;
        case BarrierKind::storeStore: __asm__ volatile("dmb ishst" ::: "memory"); break;
        case BarrierKind::loadStore:  __asm__ volatile("dmb ishld" ::: "memory"); break;
        case BarrierKind::full:       __asm__ volatile("dmb ish" ::: "memory"); break;
        }
        break;
    case BarrierScope::outer:
        switch (kind) {
        case BarrierKind::loadLoad:   __asm__ volatile("dmb oshld" ::: "memory"); break;
        case BarrierKind::storeStore: __asm__ volatile("dmb oshst" ::: "memory"); break;
        case BarrierKind::loadStore:  __asm__ volatile("dmb oshld" ::: "memory"); break;
        case BarrierKind::full:       __asm__ volatile("dmb osh" ::: "memory"); break;
        }
        break;
    case BarrierScope::nonShareable:
        switch (kind) {
        case BarrierKind::loadLoad:   __asm__ volatile("dmb nshld" ::: "memory"); break;
        case BarrierKind::storeStore: __asm__ volatile("dmb nshst" ::: "memory"); break;
        case BarrierKind::loadStore:  __asm__ volatile("dmb nshld" ::: "memory"); break;
        case BarrierKind::full:       __asm__ volatile("dmb nsh" ::: "memory"); break;
        }
        break;
    }
}
inline void syncBarrier(BarrierScope scope) noexcept {
    switch (scope) {
    case BarrierScope::full:         __asm__ volatile("dsb sy" ::: "memory"); break;
    case BarrierScope::inner:        __asm__ volatile("dsb ish" ::: "memory"); break;
    case BarrierScope::outer:        __asm__ volatile("dsb osh" ::: "memory"); break;
    case BarrierScope::nonShareable: __asm__ volatile("dsb nsh" ::: "memory"); break;
    }
}
inline void instructionBarrier() noexcept { __asm__ volatile("isb" ::: "memory"); }
inline void waitForInterrupt() noexcept { __asm__ volatile("wfi" ::: "memory"); }
inline void waitForEvent() noexcept { __asm__ volatile("wfe" ::: "memory"); }
inline void signalEvent() noexcept { __asm__ volatile("sev" ::: "memory"); }
inline void relax() noexcept { __asm__ volatile("yield" ::: "memory"); }
#elif defined(__x86_64__)
inline void memoryBarrier(BarrierScope, BarrierKind kind) noexcept {
    // x86_64 is TSO: only store->load needs a fence for WB memory; lfence /
    // sfence still order weakly-ordered (WC / non-temporal) accesses.
    switch (kind) {
    case BarrierKind::loadLoad:   __asm__ volatile("lfence" ::: "memory"); break;
    case BarrierKind::storeStore: __asm__ volatile("sfence" ::: "memory"); break;
    case BarrierKind::loadStore:  __asm__ volatile("" ::: "memory"); break;
    case BarrierKind::full:       __asm__ volatile("mfence" ::: "memory"); break;
    }
}
inline void syncBarrier(BarrierScope) noexcept { __asm__ volatile("mfence" ::: "memory"); }
inline void instructionBarrier() noexcept {
    // cpuid is architecturally serializing.
    std::uint32_t a = 0, b, c = 0, d;
    __asm__ volatile("cpuid" : "+a"(a), "=b"(b), "+c"(c), "=d"(d) :: "memory");
}
inline void waitForInterrupt() noexcept { __asm__ volatile("hlt" ::: "memory"); }
inline void waitForEvent() noexcept VESTRA_UNLOWERED("waitForEvent needs monitor/mwait setup on x86_64");
inline void signalEvent() noexcept VESTRA_UNLOWERED("x86_64 has no send-event instruction");
inline void relax() noexcept { __asm__ volatile("pause" ::: "memory"); }
#elif defined(__riscv)
inline void memoryBarrier(BarrierScope, BarrierKind kind) noexcept {
    switch (kind) {
    case BarrierKind::loadLoad:   __asm__ volatile("fence r, r" ::: "memory"); break;
    case BarrierKind::storeStore: __asm__ volatile("fence w, w" ::: "memory"); break;
    case BarrierKind::loadStore:  __asm__ volatile("fence r, w" ::: "memory"); break;
    case BarrierKind::full:       __asm__ volatile("fence rw, rw" ::: "memory"); break;
    }
}
inline void syncBarrier(BarrierScope) noexcept { __asm__ volatile("fence iorw, iorw" ::: "memory"); }
inline void instructionBarrier() noexcept { __asm__ volatile("fence.i" ::: "memory"); }
inline void waitForInterrupt() noexcept { __asm__ volatile("wfi" ::: "memory"); }
inline void waitForEvent() noexcept VESTRA_UNLOWERED("RISC-V has no wait-for-event instruction");
inline void signalEvent() noexcept VESTRA_UNLOWERED("RISC-V has no send-event instruction");
// Zihintpause `pause`, encoded directly: a nop on harts without it.
inline void relax() noexcept { __asm__ volatile(".insn i 0x0F, 0, x0, x0, 0x010" ::: "memory"); }
#else
inline void memoryBarrier(BarrierScope, BarrierKind) noexcept VESTRA_UNLOWERED("no barrier lowering for this target");
inline void syncBarrier(BarrierScope) noexcept VESTRA_UNLOWERED("no barrier lowering for this target");
inline void instructionBarrier() noexcept VESTRA_UNLOWERED("no barrier lowering for this target");
inline void waitForInterrupt() noexcept VESTRA_UNLOWERED("no lowering for this target");
inline void waitForEvent() noexcept VESTRA_UNLOWERED("no lowering for this target");
inline void signalEvent() noexcept VESTRA_UNLOWERED("no lowering for this target");
inline void relax() noexcept VESTRA_UNLOWERED("no lowering for this target");
#endif
inline void cpu_nop() noexcept { __asm__ volatile("nop"); }

}  // namespace __vstr

enum class TlbScope { currentEL, allELs, innerShareable, outerShareable };

namespace __vstr {

// The strict cache forms require a line-aligned range (§14.10.3).
inline void require_aligned(const void* p, std::size_t n, std::size_t align, std::string_view what) noexcept {
    if (((reinterpret_cast<std::uintptr_t>(p) | n) & (align - 1)) != 0) {
        panic(what);
    }
}

#if defined(__aarch64__)
inline std::size_t dcache_line_bytes() noexcept {
    std::uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    return std::size_t{4} << ((ctr >> 16) & 0xF);
}
inline std::size_t icache_line_bytes() noexcept {
    std::uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    return std::size_t{4} << (ctr & 0xF);
}
#define VESTRA_DC_RANGE(op, msg)                                                  \
    const std::size_t line = dcache_line_bytes();                                 \
    require_aligned(r.data(), r.size(), line, msg);                               \
    for (std::uintptr_t a = reinterpret_cast<std::uintptr_t>(r.data()),           \
                        e = a + r.size(); a < e; a += line) {                     \
        __asm__ volatile("dc " op ", %0" :: "r"(a) : "memory");                   \
    }                                                                             \
    __asm__ volatile("dsb sy" ::: "memory");
inline void cleanData(std::span<const std::uint8_t> r) noexcept {
    VESTRA_DC_RANGE("cvac", "Cache.cleanData: range is not cache-line aligned")
}
inline void invalidateData(std::span<const std::uint8_t> r) noexcept {
    VESTRA_DC_RANGE("ivac", "Cache.invalidateData: range is not cache-line aligned")
}
inline void cleanInvalidateData(std::span<const std::uint8_t> r) noexcept {
    VESTRA_DC_RANGE("civac", "Cache.cleanInvalidateData: range is not cache-line aligned")
}
#undef VESTRA_DC_RANGE
inline void zeroData(std::span<std::uint8_t> r) noexcept {
    std::uint64_t dczid;
    __asm__ volatile("mrs %0, dczid_el0" : "=r"(dczid));
    if ((dczid & 0x10) != 0) {
        panic("Cache.zeroData: DC ZVA is prohibited (DCZID_EL0.DZP)");
    }
    const std::size_t block = std::size_t{4} << (dczid & 0xF);
    require_aligned(r.data(), r.size(), block, "Cache.zeroData: range is not aligned to the DC ZVA block");
    for (std::uintptr_t a = reinterpret_cast<std::uintptr_t>(r.data()), e = a + r.size(); a < e; a += block) {
        __asm__ volatile("dc zva, %0" :: "r"(a) : "memory");
    }
    __asm__ volatile("dsb ish" ::: "memory");
}
inline void publishInstructions(std::span<const std::uint8_t> r) noexcept {
    // dc cvau ; dsb ish ; ic ivau ; dsb ish ; isb, over the lines the range touches.
    const std::uintptr_t start = reinterpret_cast<std::uintptr_t>(r.data());
    const std::uintptr_t end = start + r.size();
    const std::size_t dl = dcache_line_bytes();
    for (std::uintptr_t a = start & ~(dl - 1); a < end; a += dl) {
        __asm__ volatile("dc cvau, %0" :: "r"(a) : "memory");
    }
    __asm__ volatile("dsb ish" ::: "memory");
    const std::size_t il = icache_line_bytes();
    for (std::uintptr_t a = start & ~(il - 1); a < end; a += il) {
        __asm__ volatile("ic ivau, %0" :: "r"(a) : "memory");
    }
    __asm__ volatile("dsb ish\n isb" ::: "memory");
}
inline void invalidateAllInstructions() noexcept { __asm__ volatile("ic ialluis\n dsb ish\n isb" ::: "memory"); }
inline void tlbInvalidateAll(TlbScope scope) noexcept {
    switch (scope) {
    case TlbScope::currentEL:
        __asm__ volatile("dsb nshst\n tlbi vmalle1\n dsb nsh\n isb" ::: "memory");
        break;
    case TlbScope::innerShareable:
    case TlbScope::outerShareable:
        __asm__ volatile("dsb ishst\n tlbi vmalle1is\n dsb ish\n isb" ::: "memory");
        break;
    case TlbScope::allELs:
        panic("Tlb.invalidateAll(scope: .allELs) needs EL2; EL1 can only invalidate EL1&0");
    }
}
inline void tlbInvalidatePage(std::uint64_t va, bool /*large: vae1 covers any level*/, TlbScope scope) noexcept {
    const std::uint64_t arg = (va >> 12) & ((std::uint64_t{1} << 44) - 1);
    switch (scope) {
    case TlbScope::currentEL:
        __asm__ volatile("dsb nshst\n tlbi vae1, %0\n dsb nsh\n isb" :: "r"(arg) : "memory");
        break;
    case TlbScope::innerShareable:
    case TlbScope::outerShareable:
        __asm__ volatile("dsb ishst\n tlbi vae1is, %0\n dsb ish\n isb" :: "r"(arg) : "memory");
        break;
    case TlbScope::allELs:
        panic("Tlb.invalidatePage(scope: .allELs) needs EL2; EL1 can only invalidate EL1&0");
    }
}
inline void tlbInvalidateAsid(std::uint16_t asid, TlbScope scope) noexcept {
    const std::uint64_t arg = static_cast<std::uint64_t>(asid) << 48;
    switch (scope) {
    case TlbScope::currentEL:
        __asm__ volatile("dsb nshst\n tlbi aside1, %0\n dsb nsh\n isb" :: "r"(arg) : "memory");
        break;
    case TlbScope::innerShareable:
    case TlbScope::outerShareable:
        __asm__ volatile("dsb ishst\n tlbi aside1is, %0\n dsb ish\n isb" :: "r"(arg) : "memory");
        break;
    case TlbScope::allELs:
        panic("Tlb.invalidateAsid(scope: .allELs) needs EL2; EL1 can only invalidate EL1&0");
    }
}
#elif defined(__x86_64__)
// clflush writes a dirty line back and evicts it: it serves clean, invalidate
// and clean+invalidate (the stronger operation is always safe here).
#define VESTRA_CLFLUSH_RANGE(msg)                                                 \
    require_aligned(r.data(), r.size(), 64, msg);                                 \
    for (std::uintptr_t a = reinterpret_cast<std::uintptr_t>(r.data()),           \
                        e = a + r.size(); a < e; a += 64) {                       \
        __asm__ volatile("clflush (%0)" :: "r"(a) : "memory");                    \
    }                                                                             \
    __asm__ volatile("mfence" ::: "memory");
inline void cleanData(std::span<const std::uint8_t> r) noexcept {
    VESTRA_CLFLUSH_RANGE("Cache.cleanData: range is not cache-line aligned")
}
inline void invalidateData(std::span<const std::uint8_t> r) noexcept {
    VESTRA_CLFLUSH_RANGE("Cache.invalidateData: range is not cache-line aligned")
}
inline void cleanInvalidateData(std::span<const std::uint8_t> r) noexcept {
    VESTRA_CLFLUSH_RANGE("Cache.cleanInvalidateData: range is not cache-line aligned")
}
#undef VESTRA_CLFLUSH_RANGE
inline void zeroData(std::span<std::uint8_t> r) noexcept {
    require_aligned(r.data(), r.size(), 64, "Cache.zeroData: range is not cache-line aligned");
    for (auto& b : r) { *static_cast<volatile std::uint8_t*>(&b) = 0; }
}
// x86_64 keeps instruction fetch coherent with stores; a serializing
// instruction is all a code update needs.
inline void publishInstructions(std::span<const std::uint8_t>) noexcept { instructionBarrier(); }
inline void invalidateAllInstructions() noexcept { instructionBarrier(); }
inline void tlbInvalidateAll(TlbScope scope) noexcept {
    if (scope != TlbScope::currentEL) {
        panic("x86_64 has no broadcast TLB invalidation; shoot down other CPUs with IPIs");
    }
    std::uint64_t cr3;
    __asm__ volatile("mov %%cr3, %0\n mov %0, %%cr3" : "=r"(cr3) :: "memory");
}
inline void tlbInvalidatePage(std::uint64_t va, bool, TlbScope scope) noexcept {
    if (scope != TlbScope::currentEL) {
        panic("x86_64 has no broadcast TLB invalidation; shoot down other CPUs with IPIs");
    }
    __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory");
}
#if defined(__INVPCID__)
inline void tlbInvalidateAsid(std::uint16_t asid, TlbScope scope) noexcept {
    if (scope != TlbScope::currentEL) {
        panic("x86_64 has no broadcast TLB invalidation; shoot down other CPUs with IPIs");
    }
    struct { std::uint64_t pcid, addr; } desc{asid, 0};
    __asm__ volatile("invpcid %0, %1" :: "m"(desc), "r"(std::uint64_t{1}) : "memory");
}
#else
inline void tlbInvalidateAsid(std::uint16_t, TlbScope) noexcept
    VESTRA_UNLOWERED("Tlb.invalidateAsid needs INVPCID on x86_64 (build with -minvpcid)");
#endif
#elif defined(__riscv)
#if defined(__riscv_zicbom)
#define VESTRA_CBO_RANGE(op, msg)                                                 \
    require_aligned(r.data(), r.size(), 64, msg);                                 \
    for (std::uintptr_t a = reinterpret_cast<std::uintptr_t>(r.data()),           \
                        e = a + r.size(); a < e; a += 64) {                       \
        __asm__ volatile("cbo." op " (%0)" :: "r"(a) : "memory");                 \
    }                                                                             \
    __asm__ volatile("fence rw, rw" ::: "memory");
inline void cleanData(std::span<const std::uint8_t> r) noexcept {
    VESTRA_CBO_RANGE("clean", "Cache.cleanData: range is not cache-block aligned")
}
inline void invalidateData(std::span<const std::uint8_t> r) noexcept {
    VESTRA_CBO_RANGE("inval", "Cache.invalidateData: range is not cache-block aligned")
}
inline void cleanInvalidateData(std::span<const std::uint8_t> r) noexcept {
    VESTRA_CBO_RANGE("flush", "Cache.cleanInvalidateData: range is not cache-block aligned")
}
#undef VESTRA_CBO_RANGE
#else
inline void cleanData(std::span<const std::uint8_t>) noexcept VESTRA_UNLOWERED("cache maintenance needs Zicbom on RISC-V");
inline void invalidateData(std::span<const std::uint8_t>) noexcept VESTRA_UNLOWERED("cache maintenance needs Zicbom on RISC-V");
inline void cleanInvalidateData(std::span<const std::uint8_t>) noexcept VESTRA_UNLOWERED("cache maintenance needs Zicbom on RISC-V");
#endif
#if defined(__riscv_zicboz)
inline void zeroData(std::span<std::uint8_t> r) noexcept {
    require_aligned(r.data(), r.size(), 64, "Cache.zeroData: range is not cache-block aligned");
    for (std::uintptr_t a = reinterpret_cast<std::uintptr_t>(r.data()), e = a + r.size(); a < e; a += 64) {
        __asm__ volatile("cbo.zero (%0)" :: "r"(a) : "memory");
    }
}
#else
inline void zeroData(std::span<std::uint8_t>) noexcept VESTRA_UNLOWERED("Cache.zeroData needs Zicboz on RISC-V");
#endif
inline void publishInstructions(std::span<const std::uint8_t>) noexcept { __asm__ volatile("fence.i" ::: "memory"); }
inline void invalidateAllInstructions() noexcept { __asm__ volatile("fence.i" ::: "memory"); }
inline void tlbInvalidateAll(TlbScope scope) noexcept {
    if (scope != TlbScope::currentEL) {
        panic("RISC-V has no broadcast TLB invalidation; use an SBI remote fence");
    }
    __asm__ volatile("sfence.vma" ::: "memory");
}
inline void tlbInvalidatePage(std::uint64_t va, bool, TlbScope scope) noexcept {
    if (scope != TlbScope::currentEL) {
        panic("RISC-V has no broadcast TLB invalidation; use an SBI remote fence");
    }
    __asm__ volatile("sfence.vma %0, zero" :: "r"(va) : "memory");
}
inline void tlbInvalidateAsid(std::uint16_t asid, TlbScope scope) noexcept {
    if (scope != TlbScope::currentEL) {
        panic("RISC-V has no broadcast TLB invalidation; use an SBI remote fence");
    }
    __asm__ volatile("sfence.vma zero, %0" :: "r"(static_cast<std::uint64_t>(asid)) : "memory");
}
#else
inline void cleanData(std::span<const std::uint8_t>) noexcept VESTRA_UNLOWERED("no cache lowering for this target");
inline void invalidateData(std::span<const std::uint8_t>) noexcept VESTRA_UNLOWERED("no cache lowering for this target");
inline void cleanInvalidateData(std::span<const std::uint8_t>) noexcept VESTRA_UNLOWERED("no cache lowering for this target");
inline void zeroData(std::span<std::uint8_t>) noexcept VESTRA_UNLOWERED("no cache lowering for this target");
inline void publishInstructions(std::span<const std::uint8_t>) noexcept VESTRA_UNLOWERED("no cache lowering for this target");
inline void invalidateAllInstructions() noexcept VESTRA_UNLOWERED("no cache lowering for this target");
inline void tlbInvalidateAll(TlbScope) noexcept VESTRA_UNLOWERED("no TLB lowering for this target");
inline void tlbInvalidatePage(std::uint64_t, bool, TlbScope) noexcept VESTRA_UNLOWERED("no TLB lowering for this target");
inline void tlbInvalidateAsid(std::uint16_t, TlbScope) noexcept VESTRA_UNLOWERED("no TLB lowering for this target");
#endif

// §14.14 Scheduler.swapContext: save the callee-saved registers, the stack
// pointer, the return address (the resume PC) and the interrupt mask into
// *save; load the same from *load and return into it. Caller-saved and
// FP/vector state are not part of a Context (rules 2-4). Slot layout:
//   aarch64  x19..x28 @0, x29 @80, x30 (resume PC) @88, sp @96, DAIF @104
//   x86_64   rbx rbp r12..r15 @0..40, rsp @48, resume PC @56, RFLAGS @64
//   riscv64  ra @0, sp @8, s0..s11 @16..104, sstatus.SIE @112
struct alignas(16) Context {
    std::uint8_t _bytes[256];
};

[[gnu::naked, gnu::noinline]] inline void scheduler_swap_context(Context* /*save*/, const Context* /*load*/) noexcept {
#if defined(__aarch64__)
    __asm__ volatile(
        "stp x19, x20, [x0, #0]\n"
        "stp x21, x22, [x0, #16]\n"
        "stp x23, x24, [x0, #32]\n"
        "stp x25, x26, [x0, #48]\n"
        "stp x27, x28, [x0, #64]\n"
        "stp x29, x30, [x0, #80]\n"
        "mov x9, sp\n"
        "mrs x10, daif\n"
        "stp x9, x10, [x0, #96]\n"
        "ldp x19, x20, [x1, #0]\n"
        "ldp x21, x22, [x1, #16]\n"
        "ldp x23, x24, [x1, #32]\n"
        "ldp x25, x26, [x1, #48]\n"
        "ldp x27, x28, [x1, #64]\n"
        "ldp x29, x30, [x1, #80]\n"
        "ldp x9, x10, [x1, #96]\n"
        "mov sp, x9\n"
        "msr daif, x10\n"
        "ret\n");
#elif defined(__x86_64__)
    __asm__ volatile(
        "mov %rbx, 0(%rdi)\n"
        "mov %rbp, 8(%rdi)\n"
        "mov %r12, 16(%rdi)\n"
        "mov %r13, 24(%rdi)\n"
        "mov %r14, 32(%rdi)\n"
        "mov %r15, 40(%rdi)\n"
        "lea 8(%rsp), %rax\n"
        "mov %rax, 48(%rdi)\n"
        "mov (%rsp), %rax\n"
        "mov %rax, 56(%rdi)\n"
        "pushfq\n"
        "pop %rax\n"
        "mov %rax, 64(%rdi)\n"
        "mov 0(%rsi), %rbx\n"
        "mov 8(%rsi), %rbp\n"
        "mov 16(%rsi), %r12\n"
        "mov 24(%rsi), %r13\n"
        "mov 32(%rsi), %r14\n"
        "mov 40(%rsi), %r15\n"
        "mov 64(%rsi), %rax\n"
        "push %rax\n"
        "popfq\n"
        "mov 48(%rsi), %rsp\n"
        "jmp *56(%rsi)\n");
#elif defined(__riscv)
    __asm__ volatile(
        "sd ra, 0(a0)\n"
        "sd sp, 8(a0)\n"
        "sd s0, 16(a0)\n"
        "sd s1, 24(a0)\n"
        "sd s2, 32(a0)\n"
        "sd s3, 40(a0)\n"
        "sd s4, 48(a0)\n"
        "sd s5, 56(a0)\n"
        "sd s6, 64(a0)\n"
        "sd s7, 72(a0)\n"
        "sd s8, 80(a0)\n"
        "sd s9, 88(a0)\n"
        "sd s10, 96(a0)\n"
        "sd s11, 104(a0)\n"
        "csrr t0, sstatus\n"
        "andi t0, t0, 2\n"
        "sd t0, 112(a0)\n"
        "ld ra, 0(a1)\n"
        "ld sp, 8(a1)\n"
        "ld s0, 16(a1)\n"
        "ld s1, 24(a1)\n"
        "ld s2, 32(a1)\n"
        "ld s3, 40(a1)\n"
        "ld s4, 48(a1)\n"
        "ld s5, 56(a1)\n"
        "ld s6, 64(a1)\n"
        "ld s7, 72(a1)\n"
        "ld s8, 80(a1)\n"
        "ld s9, 88(a1)\n"
        "ld s10, 96(a1)\n"
        "ld s11, 104(a1)\n"
        "csrci sstatus, 2\n"
        "ld t0, 112(a1)\n"
        "csrs sstatus, t0\n"
        "ret\n");
#else
    __builtin_trap();
#endif
}

inline void context_put64(Context& c, int off, std::uint64_t v) noexcept {
    for (int i = 0; i < 8; ++i) {
        c._bytes[off + i] = static_cast<std::uint8_t>(v >> (8 * i));
    }
}

// §14.14 Context.start: a register image that, swapped in, starts `entry`
// on the stack whose top is `stack`, interrupts unmasked.
template <class F>
inline Context context_start(F* entry, std::uint64_t stack) noexcept {
    Context c{};
    const auto pc = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(entry));
    const std::uint64_t top = stack & ~std::uint64_t{15};
#if defined(__aarch64__)
    context_put64(c, 88, pc);
    context_put64(c, 96, top);
    context_put64(c, 104, 0);              // DAIF clear: interrupts unmasked
#elif defined(__x86_64__)
    context_put64(c, 48, top - 8);          // as if `call` pushed a return address
    context_put64(c, 56, pc);
    context_put64(c, 64, 0x202);            // RFLAGS: IF set, reserved bit 1
#elif defined(__riscv)
    context_put64(c, 0, pc);
    context_put64(c, 8, top);
    context_put64(c, 112, 2);               // sstatus.SIE
#endif
    return c;
}

}  // namespace __vstr

)__fs";
}

}  // namespace

void emit_freestanding_prelude(std::ostream& hdr) {
    emit_freestanding_std(hdr);
    emit_unlowered_macro(hdr);
    emit_prelude_never(hdr);  // also opens namespace __vstr
    emit_prelude_str_queries(hdr);
    emit_prelude_char_at(hdr);
    emit_prelude_char_classes(hdr);
    emit_prelude_vec_ops(hdr);
    emit_freestanding_panic(hdr);
    emit_prelude_unwrap(hdr);
    emit_prelude_span_helpers(hdr);
    emit_prelude_views(hdr);
    emit_freestanding_sysregs(hdr);
    hdr << "}  // namespace __vstr\n\n";
    emit_freestanding_machine(hdr);
}

}  // namespace vestra::codegen::detail
