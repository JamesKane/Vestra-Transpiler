// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "vestra/codegen/cpp_emitter.hpp"
#include "vestra/diag/diagnostic.hpp"
#include "vestra/diag/source_manager.hpp"
#include "vestra/lex/lexer.hpp"
#include "vestra/parse/parser.hpp"
#include "vestra/sema/resolver.hpp"

#include <doctest/doctest.h>

#include <string>

namespace {

vestra::codegen::EmittedUnit emit(std::string text) {
    vestra::diag::SourceManager sm;
    vestra::diag::DiagnosticReporter rep(sm);
    auto fid = sm.add_in_memory("<test>", std::move(text));
    vestra::lex::Lexer lex(sm, fid, rep);
    auto tokens = lex.tokenize();
    vestra::parse::Parser p(tokens, rep);
    auto unit = p.parse_unit();
    REQUIRE_FALSE(rep.has_errors());
    vestra::codegen::CppEmitter em(rep);
    return em.emit(unit, "test");
}

// Fixture for tests that need sema's Resolution side tables (match
// dispatch, sum-type enum construction, …). Holds the source/diag
// state alongside the parsed unit + resolver so the EmittedUnit
// (which only references std::string content) can be safely returned.
struct SemaEmitFixture {
    vestra::diag::SourceManager sm;
    vestra::diag::DiagnosticReporter rep{sm};
    vestra::sema::TypeArena arena;
    std::unique_ptr<vestra::ast::CompilationUnit> unit;
    std::unique_ptr<vestra::sema::Resolver> resolver;
    vestra::codegen::EmittedUnit out;

    explicit SemaEmitFixture(std::string text) {
        auto fid = sm.add_in_memory("<test>", std::move(text));
        vestra::lex::Lexer lex(sm, fid, rep);
        auto tokens = lex.tokenize();
        vestra::parse::Parser p(tokens, rep);
        unit = std::make_unique<vestra::ast::CompilationUnit>(p.parse_unit());
        REQUIRE_FALSE(rep.has_errors());
        resolver = std::make_unique<vestra::sema::Resolver>(*unit, arena, rep);
        resolver->resolve();
        REQUIRE_FALSE(rep.has_errors());
        vestra::codegen::CppEmitter em(rep, &resolver->resolution());
        out = em.emit(*unit, "test");
    }
};

}  // namespace

TEST_CASE("emitter maps primitives to <cstdint> aliases") {
    auto out = emit("func id(_ x: Int32) -> Int32 { return x }\n");
    CHECK(out.header.find("std::int32_t id(const std::int32_t&") != std::string::npos);
    CHECK(out.source.find("return x;") != std::string::npos);
}

TEST_CASE("emitter wraps the module path in a namespace") {
    auto out = emit("module core.math\nconst zero: Int32 = 0\n");
    CHECK(out.header.find("namespace core::math {") != std::string::npos);
    CHECK(out.header.find("inline constexpr") != std::string::npos);
}

TEST_CASE("match over a bare enum lowers to a switch-case IIFE") {
    // §17.7 extends this shape to stack labels for or-patterns; the
    // `case Color::X:` line now sits on its own row (so it can stack
    // with siblings under one return statement when the user writes
    // `case .red | .green:`). The single-case form still works.
    SemaEmitFixture f("enum Color { case red\n    case green\n    case blue\n}\n"
                      "func ch(_ c: Color) -> Int32 {\n"
                      "    return match c {\n"
                      "        case .red:   1\n"
                      "        case .green: 2\n"
                      "        case .blue:  3\n"
                      "    }\n"
                      "}\n");
    CHECK(f.out.source.find("switch (c)") != std::string::npos);
    CHECK(f.out.source.find("case Color::red:") != std::string::npos);
    CHECK(f.out.source.find("case Color::blue:") != std::string::npos);
    CHECK(f.out.source.find("return 1;") != std::string::npos);
    CHECK(f.out.source.find("return 3;") != std::string::npos);
}

TEST_CASE("match over a payloaded enum lowers to std::visit") {
    SemaEmitFixture f("enum Shape {\n"
                      "    case circle(radius: Float64)\n"
                      "    case rect(width: Float64, height: Float64)\n"
                      "    case point\n"
                      "}\n"
                      "func area(_ s: Shape) -> Float64 {\n"
                      "    return match s {\n"
                      "        case .circle(let r):       r * r\n"
                      "        case .rect(let w, let h):  w * h\n"
                      "        case .point:               0.0\n"
                      "    }\n"
                      "}\n");
    CHECK(f.out.source.find("std::visit") != std::string::npos);
    CHECK(f.out.source.find("std::is_same_v<__vstr_alt_t, Shape::circle_t>") != std::string::npos);
    CHECK(f.out.source.find("auto&& r = __vstr_alt.radius") != std::string::npos);
    CHECK(f.out.source.find("auto&& w = __vstr_alt.width") != std::string::npos);
    CHECK(f.out.source.find("std::unreachable()") != std::string::npos);
}

TEST_CASE("where-guard on a payloaded enum arm wraps the body in a runtime if") {
    SemaEmitFixture f("enum Shape {\n"
                      "    case circle(radius: Int32)\n"
                      "    case square(side: Int32)\n"
                      "}\n"
                      "func dispatch(_ s: Shape) -> Int32 {\n"
                      "    return match s {\n"
                      "        case .circle(let r) where r > 0: r\n"
                      "        case .circle(_):                 -1\n"
                      "        case .square(let s):             s\n"
                      "    }\n"
                      "}\n");
    // Multiple arms for one case fold into a single constexpr-if
    // branch. The guarded arm wraps its return in `if (guard) { ...
    // }`; the unguarded arm runs unconditionally after it.
    CHECK(f.out.source.find("if (r > 0) {") != std::string::npos);
    CHECK(f.out.source.find("return r;") != std::string::npos);
    // Only ONE constexpr-if branch for circle_t — the second arm
    // doesn't open a new one.
    auto src = f.out.source;
    std::size_t first = src.find("std::is_same_v<__vstr_alt_t, Shape::circle_t>");
    REQUIRE(first != std::string::npos);
    CHECK(src.find("std::is_same_v<__vstr_alt_t, Shape::circle_t>", first + 1)
          == std::string::npos);
}

TEST_CASE("where-guard on a bare enum arm forces the if-chain lowering") {
    SemaEmitFixture f("enum Tag {\n"
                      "    case a\n"
                      "    case b\n"
                      "    case c\n"
                      "}\n"
                      "func tag_with_guard(_ t: Tag, _ flag: Bool) -> Int32 {\n"
                      "    return match t {\n"
                      "        case .a where flag: 1\n"
                      "        case .a:            10\n"
                      "        case .b:            2\n"
                      "        case .c:            3\n"
                      "    }\n"
                      "}\n");
    // A guarded arm (or duplicate case label) flips the lowering
    // from `switch` to an if-chain so the guard can run.
    CHECK(f.out.source.find("switch (") == std::string::npos);
    CHECK(f.out.source.find("__vstr_m == Tag::a && (flag)") != std::string::npos);
    CHECK(f.out.source.find("__vstr_m == Tag::a)") != std::string::npos);
}

TEST_CASE("sum-type enum case construction wraps in the variant brace shape") {
    SemaEmitFixture f("enum Shape {\n"
                      "    case circle(radius: Float64)\n"
                      "    case point\n"
                      "}\n"
                      "func mk() -> Shape {\n"
                      "    return Shape.circle(radius: 1.0)\n"
                      "}\n"
                      "func mk2() -> Shape {\n"
                      "    return Shape.point\n"
                      "}\n");
    CHECK(f.out.source.find("Shape{Shape::circle_t{1") != std::string::npos);
    CHECK(f.out.source.find("Shape{Shape::point_t{}}") != std::string::npos);
}

TEST_CASE("bare enums keep their `enum class Name::case` lowering") {
    // No payloaded case → still lowers as enum class, so a case access
    // is plain `Color::red`, not the variant brace wrapper.
    SemaEmitFixture f("enum Color { case red\n    case green\n}\n"
                      "func pick() -> Color {\n"
                      "    return Color.red\n"
                      "}\n");
    CHECK(f.out.source.find("return Color::red;") != std::string::npos);
    CHECK(f.out.source.find("Color{Color::red_t") == std::string::npos);
}

TEST_CASE("derive(Eq) for a struct emits a defaulted operator==") {
    SemaEmitFixture f("struct Point {\n"
                      "    var x: Int32\n"
                      "    var y: Int32\n"
                      "}\n"
                      "derive(Eq) for Point\n");
    CHECK(f.out.header.find("struct Point {") != std::string::npos);
    CHECK(f.out.header.find("bool operator==(const Point&) const noexcept = default;")
          != std::string::npos);
}

TEST_CASE("derive(Eq) for a payloaded enum emits operator== on the wrapper") {
    SemaEmitFixture f("enum Shape {\n"
                      "    case circle(radius: Float64)\n"
                      "    case point\n"
                      "}\n"
                      "derive(Eq) for Shape\n");
    CHECK(f.out.header.find("std::variant<circle_t, point_t> value") != std::string::npos);
    CHECK(f.out.header.find("bool operator==(const Shape&) const noexcept = default;")
          != std::string::npos);
}

TEST_CASE("a struct without derive(Eq) gets no operator==") {
    SemaEmitFixture f("struct Bare {\n"
                      "    var x: Int32\n"
                      "}\n");
    CHECK(f.out.header.find("struct Bare {") != std::string::npos);
    CHECK(f.out.header.find("operator==") == std::string::npos);
}

TEST_CASE("derive(Eq, Hash) emits both operator== and std::hash spec") {
    SemaEmitFixture f("module example.hashed\n"
                      "struct Q {\n"
                      "    var n: Int32\n"
                      "    var m: Int32\n"
                      "}\n"
                      "derive(Eq, Hash) for Q\n");
    // Eq stays inside the namespace as a defaulted operator==.
    CHECK(f.out.header.find("bool operator==(const Q&) const noexcept = default;")
          != std::string::npos);
    // Hash is at global scope, fully-qualified, with the boost-combine
    // body. Position-wise it lands after the namespace close.
    const auto eq_pos = f.out.header.find("operator==(const Q&)");
    const auto ns_close = f.out.header.find("}  // namespace example::hashed");
    const auto hash_pos = f.out.header.find("struct std::hash<example::hashed::Q>");
    REQUIRE(eq_pos != std::string::npos);
    REQUIRE(ns_close != std::string::npos);
    REQUIRE(hash_pos != std::string::npos);
    CHECK(eq_pos < ns_close);
    CHECK(ns_close < hash_pos);
    CHECK(f.out.header.find("__h ^= std::hash<std::int32_t>{}(v.n)") != std::string::npos);
    CHECK(f.out.header.find("__h ^= std::hash<std::int32_t>{}(v.m)") != std::string::npos);
}

TEST_CASE("derive(Hash) on a payloaded enum emits a visit-based std::hash spec") {
    SemaEmitFixture f("enum Shape {\n"
                      "    case circle(radius: Float64)\n"
                      "    case rect(width: Float64, height: Float64)\n"
                      "    case point\n"
                      "}\n"
                      "derive(Eq, Hash) for Shape\n");
    // Wrapper-level hash spec.
    CHECK(f.out.header.find("struct std::hash<Shape>") != std::string::npos);
    // Seed includes the alt index so different cases never collide
    // even when their payload bytes are equal.
    CHECK(f.out.header.find("std::size_t __h = v.value.index();") != std::string::npos);
    CHECK(f.out.header.find("std::is_same_v<__T, Shape::circle_t>") != std::string::npos);
    CHECK(f.out.header.find("__h ^= std::hash<double>{}(__alt.radius)") != std::string::npos);
    CHECK(f.out.header.find("__h ^= std::hash<double>{}(__alt.width)") != std::string::npos);
    CHECK(f.out.header.find("__h ^= std::hash<double>{}(__alt.height)") != std::string::npos);
    // point has no payload — the constexpr-if arm is empty (no
    // per-field combine), and the seeded `__h` carries through.
    CHECK(f.out.header.find("std::is_same_v<__T, Shape::point_t>") != std::string::npos);
}

TEST_CASE("derive(Hash) on a bare enum emits no spec (std::hash<E> is built in)") {
    SemaEmitFixture f("enum Color { case red\n    case green\n}\n"
                      "derive(Hash) for Color\n");
    CHECK(f.out.header.find("struct std::hash<Color>") == std::string::npos);
}

TEST_CASE("derive(Hash) alone (no Eq) still emits std::hash spec") {
    SemaEmitFixture f("struct K { var n: Int32 }\n"
                      "derive(Hash) for K\n");
    CHECK(f.out.header.find("struct std::hash<K>") != std::string::npos);
    CHECK(f.out.header.find("operator==") == std::string::npos);
}

TEST_CASE("a struct without derive(Hash) gets no std::hash spec") {
    SemaEmitFixture f("struct Bare { var n: Int32 }\n");
    CHECK(f.out.header.find("std::hash") == std::string::npos);
}

TEST_CASE("derive(Debug) for a struct emits a std::formatter spec") {
    SemaEmitFixture f("struct Point {\n"
                      "    var x: Int32\n"
                      "    var y: Int32\n"
                      "}\n"
                      "derive(Debug) for Point\n");
    CHECK(f.out.header.find("struct std::formatter<Point>") != std::string::npos);
    CHECK(f.out.header.find("\"Point{{x: {}, y: {}}}\"") != std::string::npos);
}

TEST_CASE("derive(Debug) for a bare enum emits a switch-based formatter") {
    SemaEmitFixture f("enum Color { case red\n    case green\n    case blue\n}\n"
                      "derive(Debug) for Color\n");
    CHECK(f.out.header.find("struct std::formatter<Color>") != std::string::npos);
    CHECK(f.out.header.find("case Color::red: return std::format_to(__ctx.out(), \"Color.red\")")
          != std::string::npos);
}

TEST_CASE("derive(Debug) for a payloaded enum uses std::visit + constexpr-if") {
    SemaEmitFixture f("enum Shape {\n"
                      "    case circle(radius: Float64)\n"
                      "    case point\n"
                      "}\n"
                      "derive(Debug) for Shape\n");
    CHECK(f.out.header.find("struct std::formatter<Shape>") != std::string::npos);
    CHECK(f.out.header.find("std::visit") != std::string::npos);
    CHECK(f.out.header.find("std::is_same_v<__T, Shape::circle_t>") != std::string::npos);
    CHECK(f.out.header.find("\"Shape::circle{{radius: {}}}\"") != std::string::npos);
    CHECK(f.out.header.find("\"Shape::point\"") != std::string::npos);
}

TEST_CASE("a struct without derive(Debug) gets no formatter spec") {
    // The runtime preamble emits a `std::formatter<std::optional<T>>`
    // for the §4 Optional-in-Display story, so a bare `std::formatter`
    // grep no longer works as a proxy. Check that *no struct-targeted*
    // formatter is emitted instead.
    SemaEmitFixture f("struct Bare { var n: Int32 }\n");
    CHECK(f.out.header.find("std::formatter<Bare>") == std::string::npos);
}

// ---- §9 Optional ----------------------------------------------------------

TEST_CASE("nil lowers to std::nullopt") {
    auto out = emit("func mk() -> Int32? { return nil }\n");
    CHECK(out.header.find("std::optional<std::int32_t> mk(") != std::string::npos);
    CHECK(out.source.find("return std::nullopt;") != std::string::npos);
}

TEST_CASE("?? lowers to std::optional::value_or") {
    auto out = emit("func use(_ o: Int32?) -> Int32 { return o ?? 0 }\n");
    CHECK(out.source.find("(o).value_or(0)") != std::string::npos);
}

TEST_CASE("postfix ! lowers to __vstr::unwrap_opt") {
    // §10 force-unwrap routes through the named panic primitive
    // (`__vstr::unwrap_opt` panics via `__vstr::panic` on .none)
    // instead of relying on the std::bad_optional_access exception.
    auto out = emit("func use(_ o: Int32?) -> Int32 { return o! }\n");
    CHECK(out.source.find("__vstr::unwrap_opt(o)") != std::string::npos);
}

TEST_CASE("if let lowers to a C++23 if-with-initializer over std::optional") {
    SemaEmitFixture f("func use(_ o: Int32?) -> Int32 {\n"
                      "    return if let v = o { v } else { 0 }\n"
                      "}\n");
    CHECK(f.out.source.find("if (auto __vstr_opt = o; __vstr_opt.has_value())")
          != std::string::npos);
    CHECK(f.out.source.find("auto&& v = *__vstr_opt") != std::string::npos);
    CHECK(f.out.source.find("return v;") != std::string::npos);
    CHECK(f.out.source.find("return 0;") != std::string::npos);
}

// ---- §9 Result / throws / try / throw -------------------------------------

TEST_CASE("a throws(E) function emits std::expected<T, E> as its return type") {
    auto out = emit("enum E { case bad }\n"
                    "func f() throws(E) -> Int32 { return 7 }\n");
    CHECK(out.header.find("std::expected<std::int32_t, E> f(") != std::string::npos);
    CHECK(out.header.find("#include <expected>") != std::string::npos);
}

TEST_CASE("throw at top of a throws fn lowers to return std::unexpected") {
    SemaEmitFixture f("enum E { case bad }\n"
                      "func bad() throws(E) -> Int32 { throw E.bad }\n");
    CHECK(f.out.source.find("return std::unexpected{E::bad};") != std::string::npos);
}

TEST_CASE("return try propagates via the canonical 3-line escape") {
    auto out = emit("enum E { case bad }\n"
                    "func f() throws(E) -> Int32 { return 7 }\n"
                    "func g() throws(E) -> Int32 { return try f() }\n");
    // The hoist pass pre-emits the canonical 3-line escape with a
    // generated `__vstr_tN` binding name, then the statement itself
    // emits `return <name>;`.
    CHECK(out.source.find("__vstr_t0_r = f();") != std::string::npos);
    CHECK(out.source.find("return std::unexpected{__vstr_t0_r.error()};") != std::string::npos);
    CHECK(out.source.find("auto __vstr_t0 = *__vstr_t0_r;") != std::string::npos);
    CHECK(out.source.find("return __vstr_t0;") != std::string::npos);
}

TEST_CASE("try? lowers to a Result→Optional conversion IIFE") {
    auto out = emit("enum E { case bad }\n"
                    "func f() throws(E) -> Int32 { return 7 }\n"
                    "func g() -> Int32? { return try? f() }\n");
    CHECK(out.source.find("__vstr_r.has_value() ? std::optional{*__vstr_r} : std::nullopt")
          != std::string::npos);
}

TEST_CASE("try! lowers to __vstr::unwrap_exp") {
    // §10 try! routes through the named panic primitive (same shape
    // as the postfix `!` Optional unwrap above).
    auto out = emit("enum E { case bad }\n"
                    "func f() throws(E) -> Int32 { return 7 }\n"
                    "func g() -> Int32 { return try! f() }\n");
    CHECK(out.source.find("__vstr::unwrap_exp(f())") != std::string::npos);
}

// ---- §4 layout attributes (@repr / @align / @bits) -----------------------

TEST_CASE("@repr(packed) appends __attribute__((packed)) to the struct") {
    auto out = emit("@repr(packed)\nstruct W { var v: UInt8 }\n");
    CHECK(out.header.find("struct W {") != std::string::npos);
    CHECK(out.header.find("} __attribute__((packed));") != std::string::npos);
}

TEST_CASE("@repr(align(N)) emits alignas(N) on the struct") {
    auto out = emit("@repr(align(64))\nstruct C { var v: UInt64 }\n");
    CHECK(out.header.find("struct alignas(64) C") != std::string::npos);
}

TEST_CASE("@align(N) is shorthand for @repr(align(N))") {
    auto out = emit("@align(32)\nstruct C { var v: UInt32 }\n");
    CHECK(out.header.find("struct alignas(32) C") != std::string::npos);
}

TEST_CASE("@bits(N) on a field emits a C++ bit-field") {
    auto out = emit("struct F {\n"
                    "    @bits(4) var lo: UInt8\n"
                    "    @bits(4) var hi: UInt8\n"
                    "}\n");
    CHECK(out.header.find("std::uint8_t lo : 4;") != std::string::npos);
    CHECK(out.header.find("std::uint8_t hi : 4;") != std::string::npos);
}

TEST_CASE("a bit-field has no brace-init (would fail to compile)") {
    auto out = emit("struct F { @bits(3) var v: UInt8 }\n");
    CHECK(out.header.find("v : 3;") != std::string::npos);
    CHECK(out.header.find("v : 3{}") == std::string::npos);
}

TEST_CASE("@repr(union) emits a C++ union with no default member initializers") {
    auto out = emit("@repr(packed)\n"
                    "struct Bits {\n"
                    "    @bits(1) var rxReady: UInt32\n"
                    "    @bits(31) var rest:   UInt32\n"
                    "}\n"
                    "@repr(union)\n"
                    "struct Status {\n"
                    "    var raw:    UInt32\n"
                    "    var fields: Bits\n"
                    "}\n");
    // The struct keyword in source becomes `union` in C++.
    CHECK(out.header.find("union Status {") != std::string::npos);
    CHECK(out.header.find("struct Status {") == std::string::npos);
    // Union members lose the `{}` brace-init the struct path adds —
    // C++ forbids more than one default member initializer in a union.
    CHECK(out.header.find("std::uint32_t raw;") != std::string::npos);
    CHECK(out.header.find("Bits fields;") != std::string::npos);
    CHECK(out.header.find("std::uint32_t raw{};") == std::string::npos);
}

TEST_CASE("function-pointer let-binding lowers to R(*name)(T1) and call-through works") {
    // §A12 follow-up (§14.6.3) — `(Int32) -> Int32` lowers to a C++
    // function-pointer spelling with the variable name embedded in
    // the parens. The param-mode lowering mirrors Vestra's default
    // read-mode (`const T&`) so `&helper` is ABI-compatible with the
    // slot. Calls through the slot stay as ordinary call expressions.
    auto out = emit("func helper(_ x: Int32) -> Int32 { return x + 1 }\n"
                    "func wire() -> Int32 {\n"
                    "    let f: (Int32) -> Int32 = &helper\n"
                    "    return f(7)\n"
                    "}\n");
    CHECK(out.source.find("std::int32_t(*f)(const std::int32_t&) = &helper") != std::string::npos);
    CHECK(out.source.find("return f(7);") != std::string::npos);
}

TEST_CASE("vector-table static lowers to std::array<void(*)(T&), N> with brace-init") {
    // §A8 (§14.5.3) — `[N]@interrupt(T)` is a typed vector table.
    // Codegen emits the array type with a `void(*)(T&)` slot and a
    // brace-init list of bare function names; the names decay to
    // function pointers at the C++ layer.
    auto out = emit("struct TrapFrame { var x: UInt64 }\n"
                    "@interrupt\n"
                    "func a(_ f: inout TrapFrame) { f.x = 1 }\n"
                    "@interrupt\n"
                    "func b(_ f: inout TrapFrame) { f.x = 2 }\n"
                    "static vt: [2]@interrupt(TrapFrame) = [a, b]\n");
    CHECK(out.header.find("std::array<void(*)(TrapFrame&), 2> vt = {a, b};") != std::string::npos);
}

TEST_CASE("vector-table @section + @align attributes attach to the static") {
    auto out = emit("struct TrapFrame { var x: UInt64 }\n"
                    "@interrupt\n"
                    "func a(_ f: inout TrapFrame) { f.x = 1 }\n"
                    "@section(\"__TEXT,__vt\") @align(128)\n"
                    "static vt: [1]@interrupt(TrapFrame) = [a]\n");
    // The link-time attributes ride alongside the static; the slot
    // shape stays the same.
    CHECK(out.header.find("gnu::section(\"__TEXT,__vt\")") != std::string::npos);
    CHECK(out.header.find("std::array<void(*)(TrapFrame&), 1> vt") != std::string::npos);
}

TEST_CASE("function-pointer parameter type lowers to R(*name)(T1) on the parameter slot") {
    auto out = emit("func helper(_ x: Int32) -> Int32 { return x + 1 }\n"
                    "func apply(_ f: (Int32) -> Int32, _ v: Int32) -> Int32 { return f(v) }\n"
                    "func wire() -> Int32 { return apply(helper, 41) }\n");
    // The parameter signature lands as `std::int32_t(*f)(const std::int32_t&)`
    // — the name embeds inside the parens so the declarator is
    // well-formed, and the param-mode lowering matches helper's.
    CHECK(out.source.find("std::int32_t(*f)(const std::int32_t&)") != std::string::npos);
    // `helper` decays to its function pointer at the C++ layer; no
    // explicit `&` is needed (and the parser would read `&` in a
    // call-arg position as the inout marker instead).
    CHECK(out.source.find("apply(helper, 41)") != std::string::npos);
}

TEST_CASE("@repr(union) composes with @repr(packed) sub-struct bit-fields") {
    auto out = emit("@repr(packed)\n"
                    "struct Bits {\n"
                    "    @bits(4) var lo: UInt8\n"
                    "    @bits(4) var hi: UInt8\n"
                    "}\n"
                    "@repr(union)\n"
                    "struct Cell {\n"
                    "    var raw:    UInt8\n"
                    "    var nibbles: Bits\n"
                    "}\n");
    // The packed sub-struct still carries its packed attribute and
    // bit-field syntax; the union overlay sits on top.
    CHECK(out.header.find("struct Bits {") != std::string::npos);
    CHECK(out.header.find("std::uint8_t lo : 4;") != std::string::npos);
    CHECK(out.header.find("} __attribute__((packed));") != std::string::npos);
    CHECK(out.header.find("union Cell {") != std::string::npos);
}

// ---- §6 tuple destructuring + literals -----------------------------------

TEST_CASE("tuple literal lowers to std::tuple{...}") {
    auto out = emit("func mk() -> (Int32, Int32) { return (1, 2) }\n");
    CHECK(out.source.find("return std::tuple{1, 2};") != std::string::npos);
}

TEST_CASE("let-tuple destructuring lowers to a structured binding") {
    auto out = emit("func mk() -> (Int32, Int32) { return (1, 2) }\n"
                    "func sum() -> Int32 {\n"
                    "    let (a, b) = mk()\n"
                    "    return a + b\n"
                    "}\n");
    CHECK(out.source.find("auto [a, b] = mk();") != std::string::npos);
}

TEST_CASE("tuple type lowers to std::tuple<...>") {
    auto out = emit("func mk() -> (Int32, Int32) { return (1, 2) }\n");
    CHECK(out.header.find("std::tuple<std::int32_t, std::int32_t>") != std::string::npos);
}

TEST_CASE("nested tuple pattern hoists inner unpacks into sibling stmts") {
    auto out = emit("func mk() -> ((Int32, Int32), Int32) { return ((1, 2), 3) }\n"
                    "func sum() -> Int32 {\n"
                    "    let ((a, b), c) = mk()\n"
                    "    return a + b + c\n"
                    "}\n");
    // The outer level binds a placeholder for the inner tuple plus the
    // leaf `c`; a follow-on statement then unpacks the placeholder.
    CHECK(out.source.find("auto [__vstr_tp0, c] = mk();") != std::string::npos);
    CHECK(out.source.find("auto [a, b] = __vstr_tp0;") != std::string::npos);
}

TEST_CASE("three-deep tuple pattern emits one unpack per level") {
    auto out = emit("func mk() -> (((Int32, Int32), Int32), Int32) {\n"
                    "    return (((1, 2), 3), 4)\n"
                    "}\n"
                    "func sum() -> Int32 {\n"
                    "    let (((x, y), z), w) = mk()\n"
                    "    return x + y + z + w\n"
                    "}\n");
    CHECK(out.source.find("auto [__vstr_tp0, w] = mk();") != std::string::npos);
    CHECK(out.source.find("auto [__vstr_tp1, z] = __vstr_tp0;") != std::string::npos);
    CHECK(out.source.find("auto [x, y] = __vstr_tp1;") != std::string::npos);
}

TEST_CASE("nested tuple pattern in var destructures the same way") {
    auto out = emit("func mk() -> ((Int32, Int32), Int32) { return ((1, 2), 3) }\n"
                    "func bump() -> Int32 {\n"
                    "    var ((a, b), c) = mk()\n"
                    "    a = a + 1\n"
                    "    return a + b + c\n"
                    "}\n");
    CHECK(out.source.find("auto [__vstr_tp0, c] = mk();") != std::string::npos);
    CHECK(out.source.find("auto [a, b] = __vstr_tp0;") != std::string::npos);
}

TEST_CASE("nested tuple pattern with wildcard sub-element drops the slot") {
    auto out = emit("func mk() -> ((Int32, Int32), Int32) { return ((1, 2), 3) }\n"
                    "func pick() -> Int32 {\n"
                    "    let ((a, _), c) = mk()\n"
                    "    return a + c\n"
                    "}\n");
    CHECK(out.source.find("auto [__vstr_tp0, c] = mk();") != std::string::npos);
    // The wildcard slot is given a generated name so the sibling unpack
    // still binds something for `a`.
    CHECK(out.source.find("auto [a, __vstr_tp_unused_") != std::string::npos);
}

TEST_CASE("tuple-pattern param emits a synthetic arg + prologue binding") {
    auto out = emit("func add_pair((a, b): (Int32, Int32)) -> Int32 {\n"
                    "    return a + b\n"
                    "}\n");
    // The C++ signature loses the user's leaf names — the param is a
    // synthetic `__vstr_arg0` of the tuple type.
    CHECK(out.header.find("add_pair(const std::tuple<std::int32_t, std::int32_t>& __vstr_arg0)")
          != std::string::npos);
    // The body opens with the structured-binding unpack so the leaves
    // are in scope for the rest of the function.
    CHECK(out.source.find("auto [a, b] = __vstr_arg0;") != std::string::npos);
}

TEST_CASE("nested tuple-pattern param threads through the prologue") {
    auto out = emit("func sum3(((p, q), r): ((Int32, Int32), Int32)) -> Int32 {\n"
                    "    return p + q + r\n"
                    "}\n");
    CHECK(out.source.find("auto [__vstr_tp0, r] = __vstr_arg0;") != std::string::npos);
    CHECK(out.source.find("auto [p, q] = __vstr_tp0;") != std::string::npos);
}

TEST_CASE("match-arm tuple sub-pattern destructures the enum payload field") {
    SemaEmitFixture f("enum Action {\n"
                      "    case quit\n"
                      "    case point(p: (Int32, Int32))\n"
                      "}\n"
                      "func score(_ a: Action) -> Int32 {\n"
                      "    return match a {\n"
                      "        case .quit: 0\n"
                      "        case .point((let x, let y)): x + y\n"
                      "    }\n"
                      "}\n");
    // The point-case arm binds the payload's tuple field by reference
    // via a structured binding off the variant alternative's `.p`.
    CHECK(f.out.source.find("auto&& [x, y] = __vstr_alt.p;") != std::string::npos);
}

// ---- §10 Box[T] -----------------------------------------------------------

TEST_CASE("Box[T] type lowers to std::unique_ptr<T>") {
    auto out = emit("func id(_ b: Box[Int32]) -> Box[Int32] { return b }\n");
    CHECK(out.header.find("std::unique_ptr<std::int32_t>") != std::string::npos);
}

TEST_CASE("Box.new(v) lowers to std::make_unique<T>(v)") {
    SemaEmitFixture f("func mk(_ x: Int32) using Alloc -> Box[Int32] {\n"
                      "    return Box.new(x)\n"
                      "}\n");
    CHECK(f.out.source.find("std::make_unique<std::int32_t>(x)") != std::string::npos);
}

TEST_CASE("box.value lowers to (*box)") {
    SemaEmitFixture f("func deref(_ b: Box[Int32]) -> Int32 { return b.value }\n");
    CHECK(f.out.source.find("(*b)") != std::string::npos);
}

// ---- §3 opaque type -------------------------------------------------------

TEST_CASE("opaque type lowers to enum class with the underlying as base") {
    auto out = emit("opaque type UserId = Int64\n");
    CHECK(out.header.find("enum class UserId : std::int64_t {}") != std::string::npos);
}

TEST_CASE("opaque constructor lowers to static_cast<Q>(t)") {
    SemaEmitFixture f("opaque type UserId = Int64\n"
                      "func mk(_ x: Int64) -> UserId { return UserId(x) }\n");
    CHECK(f.out.source.find("return static_cast<UserId>(x);") != std::string::npos);
}

TEST_CASE("opaque .value lowers to static_cast<UnderlyingT>(q)") {
    SemaEmitFixture f("opaque type UserId = Int64\n"
                      "func raw(_ id: UserId) -> Int64 { return id.value }\n");
    CHECK(f.out.source.find("return static_cast<std::int64_t>(id);") != std::string::npos);
}

// ---- §4 derive(Display) ---------------------------------------------------

TEST_CASE("derive(Display) on a struct emits a std::formatter spec") {
    SemaEmitFixture f("struct Point { var x: Int32 }\n"
                      "derive(Display) for Point\n");
    CHECK(f.out.header.find("struct std::formatter<Point>") != std::string::npos);
    CHECK(f.out.header.find("\"Point{{x: {}}}\"") != std::string::npos);
}

TEST_CASE("derive(Display) and derive(Debug) on the same type emit one formatter") {
    SemaEmitFixture f("struct Point { var x: Int32 }\n"
                      "derive(Display, Debug) for Point\n");
    // ODR: only one specialization, not two.
    auto first = f.out.header.find("struct std::formatter<Point>");
    REQUIRE(first != std::string::npos);
    auto second = f.out.header.find("struct std::formatter<Point>", first + 1);
    CHECK(second == std::string::npos);
}

// ---- §5 for-in over Iterator ---------------------------------------------

TEST_CASE("for-in over a Range lowers to a C++ counted for-loop") {
    auto out = emit("func sum() -> Int32 {\n"
                    "    var total: Int32 = 0\n"
                    "    for i in Int32(0)..Int32(9) { total = total + i }\n"
                    "    return total\n"
                    "}\n");
    CHECK(out.source.find("for (auto i = static_cast<std::int32_t>(0), __vstr_end = "
                          "static_cast<std::int32_t>(9); i <= __vstr_end; ++i)")
          != std::string::npos);
}

TEST_CASE("for-in over a Range with `..<` uses strict-less-than") {
    auto out = emit("func sum() -> Int32 {\n"
                    "    var total: Int32 = 0\n"
                    "    for i in Int32(0)..<Int32(10) { total = total + i }\n"
                    "    return total\n"
                    "}\n");
    CHECK(out.source.find("i < __vstr_end") != std::string::npos);
}

TEST_CASE("for-in over an iterator value lowers to a while/next/break loop") {
    SemaEmitFixture f("struct Counter {\n"
                      "    var n: Int32\n"
                      "    inout func next() -> Int32? { return nil }\n"
                      "}\n"
                      "func use() -> Int32 {\n"
                      "    var c = Counter(n: 0)\n"
                      "    var t: Int32 = 0\n"
                      "    for v in c { t = t + v }\n"
                      "    return t\n"
                      "}\n");
    CHECK(f.out.source.find("auto __vstr_iter = c;") != std::string::npos);
    CHECK(f.out.source.find("auto __vstr_o = __vstr_iter.next();") != std::string::npos);
    CHECK(f.out.source.find("if (!__vstr_o.has_value()) { break; }") != std::string::npos);
    CHECK(f.out.source.find("auto v = std::move(*__vstr_o);") != std::string::npos);
}

// ---- §12.3 derive(Clone) --------------------------------------------------

TEST_CASE("derive(Clone) on a struct emits an explicit clone() method") {
    SemaEmitFixture f("struct Point { var x: Int32 }\n"
                      "derive(Clone) for Point\n");
    CHECK(f.out.header.find("[[nodiscard]] Point clone() const { return *this; }")
          != std::string::npos);
}

TEST_CASE("derive(Clone) on a payloaded enum emits a clone() on the wrapper") {
    SemaEmitFixture f("enum Shape { case circle(r: Float64); case point }\n"
                      "derive(Clone) for Shape\n");
    CHECK(f.out.header.find("[[nodiscard]] Shape clone() const { return *this; }")
          != std::string::npos);
}

TEST_CASE("a method returning a struct stays a real call, not a fresh struct literal") {
    // Regression: the CallExpr emitter used to lower `p.clone()` to
    // `Point{}` because its check was result-type-driven. Now it's
    // gated on the callee being an IdentExpr resolving to a Struct
    // symbol — so `.clone()` lowers correctly as a method call.
    SemaEmitFixture f("struct Point { var x: Int32 }\n"
                      "derive(Clone) for Point\n"
                      "func dup(_ p: Point) -> Point { return p.clone() }\n");
    CHECK(f.out.source.find("return p.clone();") != std::string::npos);
    CHECK(f.out.source.find("return Point{};") == std::string::npos);
}

// ---- §9 do/catch where guard ---------------------------------------------

TEST_CASE("do/catch where-guard lowers to a cond-hoist with error propagation") {
    SemaEmitFixture f("enum E { case bad }\n"
                      "func f() throws(E) -> Int32 { return 1 }\n"
                      "func isBad(_ e: E) -> Bool { return true }\n"
                      "func go() throws(E) -> Int32 {\n"
                      "    return do { try f() } catch (e: E) where isBad(e) { -1 }\n"
                      "}\n");
    // The do-catch gets lifted to a cond-hoist lambda returning
    // std::expected<T, E>, named __vstr_c…
    CHECK(f.out.source.find("auto __vstr_c0 = [&]() -> std::expected<") != std::string::npos);
    // The handler runs only if the guard says so.
    CHECK(f.out.source.find("if (isBad(e))") != std::string::npos);
    // Guard fall-through propagates the bound error to the enclosing
    // throws context via std::unexpected — no more panic.
    CHECK(f.out.source.find("return std::unexpected{e};") != std::string::npos);
    CHECK(f.out.source.find("__vstr::panic(\"do/catch where-guard fell through")
          == std::string::npos);
    // The outer cond-hoist lambda is unwrapped via the canonical
    // 3-line escape at the use site.
    CHECK(f.out.source.find("if (!__vstr_c0.has_value()) { return std::unexpected{")
          != std::string::npos);
}

// ---- §9 iterator combinators (zip / take) --------------------------------

TEST_CASE("zip(a, b) lowers via CTAD on __vstr::Zip and binds tuples in the for-loop") {
    SemaEmitFixture f("struct It {\n"
                      "    var n: Int32\n"
                      "    inout func next() -> Int32? {\n"
                      "        if self.n <= 0 { return nil }\n"
                      "        let v = self.n\n"
                      "        self.n = self.n - 1\n"
                      "        return v\n"
                      "    }\n"
                      "}\n"
                      "func use() -> Int32 {\n"
                      "    var t: Int32 = 0\n"
                      "    var a = It(n: 3)\n"
                      "    var b = It(n: 5)\n"
                      "    for (x, y) in zip(a, b) { t = t + x + y }\n"
                      "    return t\n"
                      "}\n");
    // The header-local Zip template lands once in the preamble.
    CHECK(f.out.header.find("struct Zip {") != std::string::npos);
    CHECK(f.out.header.find("Zip(A, B) -> Zip<A, B>") != std::string::npos);
    // The call lowers to a CTAD instantiation.
    CHECK(f.out.source.find("__vstr::Zip{a, b}") != std::string::npos);
    // The TuplePat in the for-loop lowers to a structured binding
    // over the yielded std::tuple.
    CHECK(f.out.source.find("auto [x, y] = std::move(*__vstr_o);") != std::string::npos);
}

// ---- §A4 Atomic[T] (§14.9) -----------------------------------------------

// ---- §A10 @panic_handler (§15.5) -----------------------------------------

TEST_CASE("@panic_handler emits a static-init block that registers the handler") {
    SemaEmitFixture f("@panic_handler\n"
                      "func myPanic(_ msg: Str, _ file: StrConst, _ line: Int) -> Never {\n"
                      "    abort()\n"
                      "}\n");
    // The runtime preamble declares the slot.
    CHECK(f.out.header.find("inline PanicHandlerFn panic_handler = nullptr;") != std::string::npos);
    // The panic shim delegates through the slot when non-null.
    CHECK(f.out.header.find("if (panic_handler != nullptr)") != std::string::npos);
    // The static-init block ends up in the source file inside the
    // user's namespace.
    CHECK(f.out.source.find("__vstr::panic_handler = &myPanic;") != std::string::npos);
    // The handler's signature lowers with `__vstr::Never` return.
    CHECK(f.out.source.find("__vstr::Never myPanic(") != std::string::npos);
}

// ---- §A8 @interrupt handlers (§14.5.2) -----------------------------------

TEST_CASE("@interrupt emits [[gnu::used]] on the declaration") {
    SemaEmitFixture f("struct TrapFrame { var x0: UInt64 }\n"
                      "@interrupt(0) @symbol(\"isr_sym\")\n"
                      "func isr(_ frame: inout TrapFrame) {\n"
                      "    frame.x0 = frame.x0 + 1\n"
                      "}\n");
    // [[gnu::used]] keeps the symbol alive across dead-code
    // elimination — required for an ISR whose only references are
    // in a hand-written vector table or linker script.
    CHECK(f.out.header.find("[[gnu::used]]") != std::string::npos);
    // Pairing with @symbol still renames the asm label.
    CHECK(f.out.header.find("isr(TrapFrame& frame) asm(\"isr_sym\")") != std::string::npos);
}

// ---- §A12 &decl (§14.6.3) ------------------------------------------------

TEST_CASE("&static lowers to a plain `&ident` in C++") {
    SemaEmitFixture f("static n: Int32 = 42\n"
                      "func get_addr() -> Ptr[Int32] { return &n }\n");
    // The function returns `const std::int32_t*` (the Ptr[Int32]
    // lowering) and the body is `return &n;`.
    CHECK(f.out.header.find("const std::int32_t* get_addr();") != std::string::npos);
    CHECK(f.out.source.find("return &n;") != std::string::npos);
}

// ---- §A11 PerCpu (§14.8) -------------------------------------------------

TEST_CASE("PerCpu[T] lowers to __vstr::PerCpu<T> with .mine() accessor") {
    SemaEmitFixture f("@noinit static c: PerCpu[Atomic[UInt32]]\n"
                      "func bump() -> UInt32 {\n"
                      "    return c.mine().fetchAdd(1, .seqCst)\n"
                      "}\n");
    // The runtime preamble defines the 64-byte-aligned template.
    CHECK(f.out.header.find("struct alignas(64) PerCpu {") != std::string::npos);
    CHECK(f.out.header.find("inline __vstr::PerCpu<std::atomic<std::uint32_t>> c;")
          != std::string::npos);
    // The call chain `c.mine().fetchAdd` lowers naturally — the
    // .mine() is a plain method call and fetch_add picks up the
    // Atomic rename.
    CHECK(f.out.source.find("c.mine().fetch_add(1, std::memory_order_seq_cst)")
          != std::string::npos);
}

TEST_CASE("--no-libc emits the freestanding profile marker in both files") {
    vestra::diag::SourceManager sm;
    vestra::diag::DiagnosticReporter rep(sm);
    auto fid = sm.add_in_memory("<test>", "func id(_ x: Int32) -> Int32 { return x }\n");
    vestra::lex::Lexer lex(sm, fid, rep);
    auto tokens = lex.tokenize();
    vestra::parse::Parser p(tokens, rep);
    auto unit = p.parse_unit();
    REQUIRE_FALSE(rep.has_errors());
    vestra::codegen::CppEmitter em(rep);
    em.set_no_libc(true);
    auto out = em.emit(unit, "test");
    // The marker rides at the top of both header and source so link
    // tooling and `vestra audit --no-libc` can see the contract on
    // either artifact alone.
    CHECK(out.header.find("// vestra: no_libc = true") != std::string::npos);
    CHECK(out.source.find("// vestra: no_libc = true") != std::string::npos);
}

TEST_CASE("Sysreg.<name> lowers to __vstr::sysreg::<name>") {
    SemaEmitFixture f("@kernel_init\n"
                      "@no_auto_barrier\n"
                      "func init_mmu(_ ttbr0: UInt64) using Asm {\n"
                      "    Sysreg.ttbr0_el1.write(ttbr0)\n"
                      "}\n"
                      "func cpu_id() using Asm -> UInt64 {\n"
                      "    return Sysreg.midr_el1.read()\n"
                      "}\n");
    // Runtime preamble defines the typed handle + per-name
    // singletons in `__vstr::sysreg`.
    CHECK(f.out.header.find("namespace sysreg {") != std::string::npos);
    CHECK(f.out.header.find("inline Handle<std::uint64_t> ttbr0_el1;") != std::string::npos);
    CHECK(f.out.header.find("inline Handle<std::uint64_t> midr_el1;") != std::string::npos);
    // User-facing Sysreg.<name> lowers to the namespaced singleton.
    // The init_mmu function carries @no_auto_barrier so the write
    // stays raw; without that attribute it'd route through
    // write_ttbr0_el1 — the dispatch is covered by a sibling test.
    CHECK(f.out.source.find("__vstr::sysreg::ttbr0_el1.write(ttbr0)") != std::string::npos);
    CHECK(f.out.source.find("return __vstr::sysreg::midr_el1.read();") != std::string::npos);
}

TEST_CASE("extended aarch64 EL1 MMU sysregs gate barriers on write") {
    // §14.12.3 — ttbr1_el1 / tcr_el1 / mair_el1 join the gated set
    // because they all change the MMU's view of mappings or memory
    // attributes; the runtime must drain the pipeline so subsequent
    // fetches see the new state.
    SemaEmitFixture f("func wire(_ v: UInt64) using Asm {\n"
                      "    Sysreg.tcr_el1.write(v)\n"
                      "    Sysreg.mair_el1.write(v)\n"
                      "    Sysreg.ttbr1_el1.write(v)\n"
                      "}\n");
    CHECK(f.out.header.find("inline void write_tcr_el1(") != std::string::npos);
    CHECK(f.out.header.find("inline void write_mair_el1(") != std::string::npos);
    CHECK(f.out.header.find("inline void write_ttbr1_el1(") != std::string::npos);
    CHECK(f.out.source.find("__vstr::sysreg::write_tcr_el1(v)") != std::string::npos);
    CHECK(f.out.source.find("__vstr::sysreg::write_mair_el1(v)") != std::string::npos);
    CHECK(f.out.source.find("__vstr::sysreg::write_ttbr1_el1(v)") != std::string::npos);
}

TEST_CASE("x86_64 MSRs and RISC-V CSRs emit Handle instances + raw .write paths") {
    SemaEmitFixture f("func wire(_ v: UInt64) using Asm {\n"
                      "    Sysreg.ia32_efer.write(v)\n"
                      "    Sysreg.satp.write(v)\n"
                      "}\n");
    // Each canonical name has a static Handle backing it.
    CHECK(f.out.header.find("inline Handle<std::uint64_t> ia32_efer;") != std::string::npos);
    CHECK(f.out.header.find("inline Handle<std::uint64_t> satp;") != std::string::npos);
    // Neither is in the v0.5 gated set, so the .write stays raw.
    CHECK(f.out.source.find("__vstr::sysreg::ia32_efer.write(v)") != std::string::npos);
    CHECK(f.out.source.find("__vstr::sysreg::satp.write(v)") != std::string::npos);
    CHECK(f.out.source.find("write_ia32_efer") == std::string::npos);
}

TEST_CASE("gated Sysreg write routes through the barrier-bearing wrapper") {
    // §14.12.3 — sctlr_el1 / vbar_el1 / ttbr0_el1 require an ISB on
    // the trailing edge. Without @no_auto_barrier, the write
    // dispatches to __vstr::sysreg::write_<name>, which pairs the
    // cell update with post_write_barrier().
    SemaEmitFixture f("func wire(_ v: UInt64) using Asm {\n"
                      "    Sysreg.sctlr_el1.write(v)\n"
                      "    Sysreg.vbar_el1.write(v)\n"
                      "}\n");
    CHECK(f.out.header.find("inline void post_write_barrier()") != std::string::npos);
    CHECK(f.out.header.find("inline void write_sctlr_el1(") != std::string::npos);
    CHECK(f.out.header.find("inline void write_vbar_el1(") != std::string::npos);
    CHECK(f.out.source.find("__vstr::sysreg::write_sctlr_el1(v)") != std::string::npos);
    CHECK(f.out.source.find("__vstr::sysreg::write_vbar_el1(v)") != std::string::npos);
    // The raw `.write` form should NOT appear for gated registers
    // in the ordinary-function path.
    CHECK(f.out.source.find("__vstr::sysreg::sctlr_el1.write(v)") == std::string::npos);
}

TEST_CASE("@no_auto_barrier on the enclosing function suppresses the wrapper") {
    // §14.11.5 / §14.12.3 — `@no_auto_barrier` opts the function
    // out of the auto-barrier policy. The gated writes go through
    // the raw .write form instead of write_<name>.
    SemaEmitFixture f("@kernel_init\n"
                      "@no_auto_barrier\n"
                      "func mmu_bringup(_ ttbr0: UInt64, _ sctlr: UInt64) using Asm {\n"
                      "    Sysreg.ttbr0_el1.write(ttbr0)\n"
                      "    Sysreg.sctlr_el1.write(sctlr)\n"
                      "}\n");
    CHECK(f.out.source.find("__vstr::sysreg::ttbr0_el1.write(ttbr0)") != std::string::npos);
    CHECK(f.out.source.find("__vstr::sysreg::sctlr_el1.write(sctlr)") != std::string::npos);
    CHECK(f.out.source.find("write_ttbr0_el1") == std::string::npos);
}

TEST_CASE("non-gated Sysreg write stays raw even outside @no_auto_barrier") {
    // DAIF is intentionally outside the gated set — the mask is a
    // synchronously-checked bit, not a control register that
    // changes fetch / decode state.
    SemaEmitFixture f("func mask() using Asm {\n"
                      "    Sysreg.daif.write(15)\n"
                      "}\n");
    CHECK(f.out.source.find("__vstr::sysreg::daif.write(15)") != std::string::npos);
    CHECK(f.out.source.find("write_daif") == std::string::npos);
}

TEST_CASE("@stack_protector(.none) emits [[gnu::no_stack_protector]]") {
    auto out = emit("@stack_protector(.none)\n"
                    "func crit() -> Int32 { return 42 }\n"
                    "@stack_protector(.strong)\n"
                    "func careful() -> Int32 { return 100 }\n");
    // .none suppresses canary instrumentation on this function.
    CHECK(out.source.find("[[gnu::no_stack_protector]] std::int32_t crit()") != std::string::npos);
    // .strong / .all map to compiler default (no per-function attr).
    CHECK(out.source.find("std::int32_t careful()") != std::string::npos);
    CHECK(out.source.find("no_stack_protector]] std::int32_t careful") == std::string::npos);
}

TEST_CASE("cfg.option(\"cache_line_bytes\") lowers to the folded integer literal") {
    // §14.8 / §12.6 — the option name pins a single integer value
    // (64 in v0.5); sema stashes the fold via Resolution::set_folded_
    // _value, and the existing codegen consults that side table so
    // the call site emits the literal directly instead of a runtime
    // function call. This is the v0.5 path that lets `Padded[T]`
    // and adjacent code drop the magic-number 64 in favor of a
    // named comptime constant. SemaEmitFixture runs the resolver so
    // the folded side table is populated; emit() alone doesn't.
    SemaEmitFixture f("const lineBytes: Int = cfg.option(\"cache_line_bytes\")\n"
                      "func get_it() -> Int { return cfg.option(\"cache_line_bytes\") }\n");
    CHECK(f.out.header.find("inline constexpr std::intptr_t lineBytes = 64;") != std::string::npos);
    CHECK(f.out.source.find("return 64;") != std::string::npos);
    // The runtime callee is never spelled — the fold replaces it
    // with a literal at the call site.
    CHECK(f.out.source.find("cfg.option") == std::string::npos);
}

TEST_CASE("Padded[T] lowers to __vstr::Padded<T> with cache-line alignment") {
    auto out = emit("@noinit static slot: Padded[UInt64]\n"
                    "func read_it() -> UInt64 { return slot.value }\n");
    // §14.8 / §12.6 — the preamble defines a single comptime-known
    // cache_line_bytes constant; the Padded template and its tail-
    // pad expression both reference it, so one source of truth
    // drives the alignas + sizeof rounding. v0.5 hardcodes 64; the
    // build-time override hook lives in the cfg dispatch table.
    CHECK(out.header.find("inline constexpr std::size_t cache_line_bytes = 64;")
          != std::string::npos);
    CHECK(out.header.find("struct alignas(cache_line_bytes) Padded {") != std::string::npos);
    CHECK(out.header.find("std::uint8_t _pad[(cache_line_bytes - sizeof(T) % cache_line_bytes) "
                          "% cache_line_bytes];")
          != std::string::npos);
    // The static decl uses the template at the Vestra-side type.
    CHECK(out.header.find("inline __vstr::Padded<std::uint64_t> slot") != std::string::npos);
    // `.value` reads the inner value through the wrapper.
    CHECK(out.source.find("return slot.value;") != std::string::npos);
}

TEST_CASE("@boot emits [[gnu::naked]]; @kernel_init emits a plain function") {
    auto out = emit("@boot\nfunc _start() {}\n"
                    "@kernel_init\nfunc kinit() {}\n");
    // The naked attribute lands on both the declaration and the
    // definition so the C++ compiler skips the prologue/epilogue.
    CHECK(out.header.find("[[gnu::naked]] void _start();") != std::string::npos);
    CHECK(out.source.find("[[gnu::naked]] void _start()") != std::string::npos);
    // @kernel_init has no codegen artifact — it's a runtime regime,
    // not a layout / link-attribute concern.
    CHECK(out.header.find("void kinit();") != std::string::npos);
    CHECK(out.header.find("[[gnu::naked]] void kinit") == std::string::npos);
}

TEST_CASE("Ptr[T].value lowers to (*p) for read; MutPtr[T].value admits write-through") {
    SemaEmitFixture f("func get_val(_ addr: UInt64) -> UInt32 {\n"
                      "    with RawMemory {\n"
                      "        let p: Ptr[UInt32] = Ptr.unchecked(fromAddress: addr)\n"
                      "        return p.value\n"
                      "    }\n"
                      "}\n"
                      "func set_val(_ addr: UInt64, _ v: UInt32) {\n"
                      "    with RawMemory {\n"
                      "        let p: MutPtr[UInt32] = MutPtr.unchecked(fromAddress: addr)\n"
                      "        p.value = v\n"
                      "    }\n"
                      "}\n");
    CHECK(f.out.source.find("return (*p);") != std::string::npos);
    CHECK(f.out.source.find("(*p) = v;") != std::string::npos);
}

TEST_CASE("PerCpu.new lowers to make_unique<__vstr::PerCpu<T>>") {
    SemaEmitFixture f("func make(_ v: UInt32) -> UInt32 {\n"
                      "    with Alloc {\n"
                      "        let pc = PerCpu.new(v)\n"
                      "        return pc.value.mine()\n"
                      "    }\n"
                      "}\n");
    CHECK(f.out.source.find("std::make_unique<__vstr::PerCpu<std::uint32_t>>(__vstr::PerCpu"
                            "<std::uint32_t>{v})")
          != std::string::npos);
}

TEST_CASE("PerCpu.slot(hartId) lowers to the runtime slot accessor") {
    SemaEmitFixture f("@noinit static c: PerCpu[UInt32]\n"
                      "func at(_ h: UInt16) -> UInt32 {\n"
                      "    with RawMemory {\n"
                      "        let p: Ptr[UInt32] = c.slot(h)\n"
                      "    }\n"
                      "    return 0\n"
                      "}\n");
    // The runtime preamble grows the slot accessor next to mine().
    CHECK(f.out.header.find("T* slot(std::uint16_t hartId)") != std::string::npos);
    CHECK(f.out.source.find("c.slot(h)") != std::string::npos);
}

// ---- §A7 InterruptsOff + Scheduler.swapContext (§14.13, §14.14) ----------

TEST_CASE("InterruptsOff lowers to a brace block; swapContext wraps args in `&`") {
    SemaEmitFixture f("@noinit static a: Context\n"
                      "@noinit static b: Context\n"
                      "static counter: Atomic[UInt32] = 0\n"
                      "func bump() {\n"
                      "    with InterruptsOff {\n"
                      "        counter.store(1, .seqCst)\n"
                      "    }\n"
                      "    Scheduler.swapContext(a, b)\n"
                      "}\n");
    // Context lowers to the opaque __vstr struct.
    CHECK(f.out.header.find("inline __vstr::Context a;") != std::string::npos);
    // swapContext takes addresses of both args.
    CHECK(f.out.source.find("__vstr::scheduler_swap_context(&a, &b)") != std::string::npos);
    // The runtime preamble declares the opaque Context + shim.
    CHECK(f.out.header.find("struct alignas(16) Context {") != std::string::npos);
    CHECK(f.out.header.find("inline void scheduler_swap_context(") != std::string::npos);
}

// ---- §A6 MMIO views (§14.11) ---------------------------------------------

TEST_CASE("MmioView.at + read/write lower through __vstr::MmioView with a volatile cast") {
    SemaEmitFixture f("func touch(_ addr: UInt64) -> UInt32 {\n"
                      "    var observed: UInt32 = 0\n"
                      "    with RawMemory {\n"
                      "        with Mmio {\n"
                      "            let p: MutPtr[UInt32] = MutPtr.unchecked(fromAddress: addr)\n"
                      "            let v = MmioView.at(p)\n"
                      "            v.write(42)\n"
                      "            observed = v.read()\n"
                      "        }\n"
                      "    }\n"
                      "    return observed\n"
                      "}\n");
    CHECK(f.out.header.find("struct MmioView {") != std::string::npos);
    // The constructor wraps the MutPtr with a volatile-cast.
    CHECK(f.out.source.find("__vstr::MmioView<std::uint32_t>{reinterpret_cast<std::uint32_t "
                            "volatile*>(p)}")
          != std::string::npos);
    CHECK(f.out.source.find("v.write(42)") != std::string::npos);
    CHECK(f.out.source.find("v.read()") != std::string::npos);
}

TEST_CASE("MmioRegion.at + .index / .count lower through __vstr::MmioRegion") {
    SemaEmitFixture f("func sum(_ addr: UInt64) -> UInt32 {\n"
                      "    var total: UInt32 = 0\n"
                      "    with RawMemory {\n"
                      "        with Mmio {\n"
                      "            let p: MutPtr[UInt32] = MutPtr.unchecked(fromAddress: addr)\n"
                      "            let r = MmioRegion.at(p, 4)\n"
                      "            r.index(0).write(7)\n"
                      "            total = r.index(0).read()\n"
                      "        }\n"
                      "    }\n"
                      "    return total\n"
                      "}\n");
    CHECK(f.out.header.find("struct MmioRegion {") != std::string::npos);
    CHECK(f.out.source.find(
              "__vstr::MmioRegion<std::uint32_t>{reinterpret_cast<std::uint32_t volatile*>(p), "
              "static_cast<std::intptr_t>(4)}")
          != std::string::npos);
    CHECK(f.out.source.find("r.index(0).write(7)") != std::string::npos);
}

TEST_CASE("MmioWireView.at + read/write lower through __vstr::MmioWireView with endianness") {
    SemaEmitFixture f("func talk(_ addr: UInt64) -> UInt32 {\n"
                      "    var observed: UInt32 = 0\n"
                      "    with RawMemory {\n"
                      "        with Mmio {\n"
                      "            let p: MutPtr[UInt32] = MutPtr.unchecked(fromAddress: addr)\n"
                      "            let w = MmioWireView.at(p, .big)\n"
                      "            w.write(42)\n"
                      "            observed = w.read()\n"
                      "        }\n"
                      "    }\n"
                      "    return observed\n"
                      "}\n");
    CHECK(f.out.header.find("enum class Endianness { little, big, native };") != std::string::npos);
    CHECK(f.out.header.find("struct MmioWireView {") != std::string::npos);
    CHECK(f.out.header.find("std::byteswap") != std::string::npos);
    CHECK(f.out.source.find("__vstr::MmioWireView<std::uint32_t>{reinterpret_cast<std::uint32_t "
                            "volatile*>(p), Endianness::big}")
          != std::string::npos);
    CHECK(f.out.source.find("w.write(42)") != std::string::npos);
    CHECK(f.out.source.find("w.read()") != std::string::npos);
}

// ---- §A10 follow-up: memcpy / memset / memmove (§15.4) -------------------

TEST_CASE("memcpy / memset / memmove lower to __builtin_mem* with size_t cast") {
    SemaEmitFixture f("func go(_ d_addr: UInt64, _ s_addr: UInt64) {\n"
                      "    with RawMemory {\n"
                      "        let d: MutPtr[UInt8] = MutPtr.unchecked(fromAddress: d_addr)\n"
                      "        let s: Ptr[UInt8]    = Ptr.unchecked(fromAddress: s_addr)\n"
                      "        memcpy(d, s, 16)\n"
                      "        memset(d, 0xAB, 4)\n"
                      "        memmove(d, s, 16)\n"
                      "    }\n"
                      "}\n");
    // Each intrinsic routes to its __builtin_* form. The trailing
    // length argument widens to std::size_t so size_t-typed slots
    // accept the Vestra-side Int (intptr_t).
    CHECK(f.out.source.find("__builtin_memcpy(d, s, static_cast<std::size_t>(16))")
          != std::string::npos);
    CHECK(f.out.source.find("__builtin_memset(d, 0xAB, static_cast<std::size_t>(4))")
          != std::string::npos);
    CHECK(f.out.source.find("__builtin_memmove(d, s, static_cast<std::size_t>(16))")
          != std::string::npos);
}

// ---- §A5 cache + TLB management (§14.10.3, §14.10.4, §14.10.5) -----------

TEST_CASE("cache + TLB builtins lower to __vstr runtime shims") {
    SemaEmitFixture f("func use(_ addr: UInt64, _ n: Int) {\n"
                      "    with RawMemory {\n"
                      "        let mp: MutPtr[UInt8] = MutPtr.unchecked(fromAddress: addr)\n"
                      "        let m = MutSpan.raw(at: mp, count: n)\n"
                      "        zeroData(m)\n"
                      "        let p: Ptr[UInt8] = Ptr.unchecked(fromAddress: addr)\n"
                      "        let s = Span.raw(at: p, count: n)\n"
                      "        cleanData(s)\n"
                      "        publishInstructions(s)\n"
                      "    }\n"
                      "    invalidateAllInstructions()\n"
                      "    tlbInvalidateAll(.innerShareable)\n"
                      "    tlbInvalidatePage(0x1000, false, .currentEL)\n"
                      "    tlbInvalidateAsid(7, .innerShareable)\n"
                      "}\n");
    // Preamble lands TlbScope at file scope so leading-dot lowering
    // works without a namespace prefix.
    CHECK(f.out.header.find("enum class TlbScope {") != std::string::npos);
    // Each builtin dispatches to its matching shim.
    CHECK(f.out.source.find("__vstr::zeroData(m)") != std::string::npos);
    CHECK(f.out.source.find("__vstr::cleanData(s)") != std::string::npos);
    CHECK(f.out.source.find("__vstr::publishInstructions(s)") != std::string::npos);
    CHECK(f.out.source.find("__vstr::invalidateAllInstructions()") != std::string::npos);
    CHECK(f.out.source.find("__vstr::tlbInvalidateAll(TlbScope::innerShareable)")
          != std::string::npos);
    CHECK(f.out.source.find("__vstr::tlbInvalidatePage(0x1000, false, TlbScope::currentEL)")
          != std::string::npos);
    CHECK(f.out.source.find("__vstr::tlbInvalidateAsid(7, TlbScope::innerShareable)")
          != std::string::npos);
}

// ---- §A3 raw-mint primitives (§10.5) -------------------------------------

TEST_CASE("MutPtr.unchecked + MutSpan.raw lower to reinterpret_cast + std::span") {
    SemaEmitFixture f("func touch(_ addr: UInt64) {\n"
                      "    with RawMemory {\n"
                      "        let p: MutPtr[UInt32] = MutPtr.unchecked(fromAddress: addr)\n"
                      "        let s = MutSpan.raw(at: p, count: 4)\n"
                      "        s[0] = 42\n"
                      "    }\n"
                      "}\n");
    // MutPtr[UInt32] lowers to UInt32*; the mint is a reinterpret_cast.
    CHECK(f.out.source.find("std::uint32_t* p = reinterpret_cast<std::uint32_t*>(addr)")
          != std::string::npos);
    // MutSpan.raw → std::span<UInt32>(ptr, static_cast<size_t>(count)).
    CHECK(f.out.source.find("std::span<std::uint32_t>(p, static_cast<std::size_t>(4))")
          != std::string::npos);
}

TEST_CASE("Ptr.unchecked + Span.raw lower to a const-pointer / const-span pair") {
    // `read` is reserved (ParamMode::Read keyword); name the func
    // something else so the parser doesn't take the keyword path.
    SemaEmitFixture f("func read_back(_ addr: UInt64) -> UInt32 {\n"
                      "    var out: UInt32 = 0\n"
                      "    with RawMemory {\n"
                      "        let p: Ptr[UInt32] = Ptr.unchecked(fromAddress: addr)\n"
                      "        let s = Span.raw(at: p, count: 4)\n"
                      "        out = s[0]\n"
                      "    }\n"
                      "    return out\n"
                      "}\n");
    CHECK(f.out.source.find("reinterpret_cast<const std::uint32_t*>(addr)") != std::string::npos);
    CHECK(f.out.source.find("std::span<const std::uint32_t>(p,") != std::string::npos);
}

// ---- §A5 sync intrinsics (§14.10) ----------------------------------------

TEST_CASE("sync-intrinsic builtins lower to __vstr runtime shims") {
    SemaEmitFixture f("func use() {\n"
                      "    compilerFence(.acquire)\n"
                      "    memoryBarrier(.full, .storeStore)\n"
                      "    syncBarrier(.inner)\n"
                      "    instructionBarrier()\n"
                      "    relax()\n"
                      "    nop()\n"
                      "}\n");
    // Enum decls land at file scope so leading-dot lowering works.
    CHECK(f.out.header.find("enum class BarrierScope {") != std::string::npos);
    CHECK(f.out.header.find("enum class BarrierKind {") != std::string::npos);
    // Each builtin call routes to its matching __vstr shim.
    CHECK(f.out.source.find("__vstr::compilerFence(std::memory_order_acquire)")
          != std::string::npos);
    CHECK(f.out.source.find("__vstr::memoryBarrier(BarrierScope::full, BarrierKind::storeStore)")
          != std::string::npos);
    CHECK(f.out.source.find("__vstr::syncBarrier(BarrierScope::inner)") != std::string::npos);
    CHECK(f.out.source.find("__vstr::instructionBarrier()") != std::string::npos);
    CHECK(f.out.source.find("__vstr::relax()") != std::string::npos);
    // `nop` renames to cpu_nop so it doesn't collide with common C++
    // identifiers like preprocessor NaN-handling helpers.
    CHECK(f.out.source.find("__vstr::cpu_nop()") != std::string::npos);
}

TEST_CASE("compareExchangeWeak lowers to compare_exchange_weak inside a retry loop") {
    SemaEmitFixture f("static c: Atomic[UInt32] = 0\n"
                      "func use(_ desired: UInt32) -> UInt32 {\n"
                      "    while true {\n"
                      "        let r = c.compareExchangeWeak(0, desired, .seqCst, .acquire)\n"
                      "        if r.succeeded { return r.actual }\n"
                      "    }\n"
                      "    return 0\n"
                      "}\n");
    CHECK(f.out.source.find("c.compare_exchange_weak(__vstr_e, desired") != std::string::npos);
    // The strong-form name shouldn't appear in this function.
    CHECK(f.out.source.find("c.compare_exchange_strong") == std::string::npos);
}

TEST_CASE("Atomic[T] bitwise fetch ops rename to snake_case and CAS bundles its result") {
    SemaEmitFixture f("static c: Atomic[UInt32] = 0\n"
                      "func use() -> UInt32 {\n"
                      "    let a = c.fetchAnd(0xFF, .relaxed)\n"
                      "    let b = c.fetchOr(0x100, .relaxed)\n"
                      "    let d = c.fetchXor(0xAA, .relaxed)\n"
                      "    let r = c.compareExchange(0, 7, .seqCst, .acquire)\n"
                      "    if r.succeeded { return r.actual }\n"
                      "    return a + b + d + r.actual\n"
                      "}\n");
    CHECK(f.out.header.find("struct CASResult {") != std::string::npos);
    CHECK(f.out.source.find("c.fetch_and(0xFF, std::memory_order_relaxed)") != std::string::npos);
    CHECK(f.out.source.find("c.fetch_or(0x100, std::memory_order_relaxed)") != std::string::npos);
    CHECK(f.out.source.find("c.fetch_xor(0xAA, std::memory_order_relaxed)") != std::string::npos);
    // The CAS IIFE: mutable expected local, compare_exchange_strong,
    // wrap in __vstr::CASResult.
    CHECK(f.out.source.find("c.compare_exchange_strong(__vstr_e, 7") != std::string::npos);
    CHECK(f.out.source.find("__vstr::CASResult<std::uint32_t>{__vstr_ok, __vstr_e}")
          != std::string::npos);
}

TEST_CASE("Atomic[T] type lowers to std::atomic<T> and ops dispatch correctly") {
    SemaEmitFixture f("static c: Atomic[UInt32] = 0\n"
                      "func use() -> UInt32 {\n"
                      "    c.store(1, .release)\n"
                      "    let a = c.fetchAdd(2, .acqRel)\n"
                      "    let b = c.fetchSub(1, .seqCst)\n"
                      "    let e = c.exchange(7, .seqCst)\n"
                      "    return c.load(.acquire) + a + b + e\n"
                      "}\n");
    CHECK(f.out.header.find("inline std::atomic<std::uint32_t> c = 0;") != std::string::npos);
    CHECK(f.out.source.find("c.store(1, std::memory_order_release);") != std::string::npos);
    CHECK(f.out.source.find("c.fetch_add(2, std::memory_order_acq_rel)") != std::string::npos);
    CHECK(f.out.source.find("c.fetch_sub(1, std::memory_order_seq_cst)") != std::string::npos);
    CHECK(f.out.source.find("c.exchange(7, std::memory_order_seq_cst)") != std::string::npos);
    CHECK(f.out.source.find("c.load(std::memory_order_acquire)") != std::string::npos);
}

TEST_CASE("Atomic[UInt128] / Atomic[Int128] lower to std::atomic over __*int128_t") {
    // §A4 (§14.9.4) wide-atomic codegen. The Vestra-level UInt128 /
    // Int128 primitives lower to the GCC / Clang `__uint128_t` /
    // `__int128_t` intrinsics; wrapping them in std::atomic gives the
    // 128-bit atomic surface the kernel needs for tagged-pointer
    // and seqlock patterns. The synthesized method names follow the
    // same .load / .fetch_add / .compare_exchange lowering as the
    // 32-bit case below.
    SemaEmitFixture f("static p: Atomic[UInt128] = 0\n"
                      "static q: Atomic[Int128] = 0\n"
                      "func use() -> UInt128 {\n"
                      "    p.store(0, .release)\n"
                      "    let _e = p.exchange(7, .seqCst)\n"
                      "    let _a = p.fetchAdd(1, .acqRel)\n"
                      "    return p.load(.acquire)\n"
                      "}\n"
                      "func cas() -> Bool {\n"
                      "    let r = q.compareExchange(0, 1, .seqCst, .acquire)\n"
                      "    return r.succeeded\n"
                      "}\n");
    CHECK(f.out.header.find("inline std::atomic<__uint128_t> p = 0;") != std::string::npos);
    CHECK(f.out.header.find("inline std::atomic<__int128_t> q = 0;") != std::string::npos);
    CHECK(f.out.source.find("p.fetch_add(1, std::memory_order_acq_rel)") != std::string::npos);
    CHECK(f.out.source.find("p.exchange(7, std::memory_order_seq_cst)") != std::string::npos);
    CHECK(f.out.source.find("p.load(std::memory_order_acquire)") != std::string::npos);
    CHECK(f.out.source.find("q.compare_exchange_strong") != std::string::npos);
}

TEST_CASE("UInt128 / Int128 conversion calls lower to static_cast") {
    SemaEmitFixture f("func widen(_ x: UInt64) -> UInt128 {\n"
                      "    return UInt128(x)\n"
                      "}\n"
                      "func narrow(_ y: UInt128) -> UInt64 {\n"
                      "    return UInt64(y)\n"
                      "}\n");
    CHECK(f.out.source.find("static_cast<__uint128_t>(x)") != std::string::npos);
    CHECK(f.out.source.find("static_cast<std::uint64_t>(y)") != std::string::npos);
}

TEST_CASE("AtomicTaggedPointer[T] lowers to __vstr template + dispatches the typed ops") {
    // §A4 (§14.9.5) — the lock-free wide-atomic wrapper lowers to a
    // __vstr template that internally holds a std::atomic<__uint128_t>;
    // load returns the (T*, uint64_t) snapshot, compareExchange
    // dispatches to the template's compare_exchange_strong member
    // (the auto-tag-bump lives in the template, not the call site).
    SemaEmitFixture f("struct Node { var next: UInt64 }\n"
                      "@noinit static head: AtomicTaggedPointer[Node]\n"
                      "func peek_tag() -> UInt64 {\n"
                      "    let (_, t) = head.load(.acquire)\n"
                      "    return t\n"
                      "}\n"
                      "func push(_ exp: MutPtr[Node], _ exp_tag: UInt64, _ des: MutPtr[Node]) "
                      "-> Bool {\n"
                      "    let r = head.compareExchange(exp, exp_tag, des, .seqCst, .acquire)\n"
                      "    return r.succeeded\n"
                      "}\n");
    // Type lowering — the head static decl uses the wrapped template.
    CHECK(f.out.header.find("__vstr::AtomicTaggedPointer<Node>") != std::string::npos);
    // Method dispatch — load goes through `.load(...)` on the template;
    // compareExchange routes to compare_exchange_strong with the five
    // positional args.
    CHECK(f.out.source.find("head.load(std::memory_order_acquire)") != std::string::npos);
    CHECK(f.out.source.find("head.compare_exchange_strong(exp, exp_tag, des,")
          != std::string::npos);
    // The preamble carries the template definition.
    CHECK(f.out.header.find("struct AtomicTaggedPointer {") != std::string::npos);
    CHECK(f.out.header.find("std::atomic<__uint128_t> raw_") != std::string::npos);
}

// ---- §A2 @inline + layout reflection (§6.8 + §7.8) -----------------------

TEST_CASE("@inline(.always) lowers to [[gnu::always_inline]] + inline") {
    SemaEmitFixture f("@inline(.always)\n"
                      "func one() -> Int32 { return 1 }\n");
    CHECK(f.out.header.find("[[gnu::always_inline]] [[nodiscard]] inline std::int32_t one()")
          != std::string::npos);
}

TEST_CASE("@inline(.never) lowers to [[gnu::noinline]] without inline keyword") {
    SemaEmitFixture f("@inline(.never)\n"
                      "func slow() -> Int32 { return -1 }\n");
    CHECK(f.out.header.find("[[gnu::noinline]] [[nodiscard]] std::int32_t slow();")
          != std::string::npos);
    // .never doesn't add the `inline` decl-specifier — the attribute
    // alone is enough to forbid inlining.
    CHECK(f.out.header.find("[[gnu::noinline]] [[nodiscard]] inline") == std::string::npos);
}

TEST_CASE("@inline(.hint) emits a plain inline (no gnu attribute)") {
    SemaEmitFixture f("@inline(.hint)\n"
                      "func maybe() -> Int32 { return 7 }\n");
    CHECK(f.out.header.find("[[nodiscard]] inline std::int32_t maybe();") != std::string::npos);
    CHECK(f.out.header.find("[[gnu::") == std::string::npos);
}

TEST_CASE("T.fields[i].offset / .size / .alignment fold to literals") {
    SemaEmitFixture f("struct S {\n"
                      "    var id: UInt64\n"
                      "    var flag: UInt8\n"
                      "    var ext: UInt32\n"
                      "}\n"
                      "func at_off() -> Int { return S.fields[2].offset }\n"
                      "func at_size() -> Int { return S.fields[2].size }\n"
                      "func at_align() -> Int { return S.fields[2].alignment }\n");
    // id (8,8) → offset 0. flag (1,1) → offset 8. ext (4,4) → next
    // aligned-up from 9 is 12 → offset 12, size 4, align 4.
    CHECK(f.out.source.find("return 12;") != std::string::npos);
    CHECK(f.out.source.find("return 4;") != std::string::npos);
    // No raw spelling leaks through.
    CHECK(f.out.source.find("S.fields") == std::string::npos);
}

TEST_CASE("T.size and T.alignment fold to integer literals at the use site") {
    SemaEmitFixture f("struct S {\n"
                      "    var id: UInt64\n"
                      "    var flag: UInt8\n"
                      "}\n"
                      "func size() -> Int { return S.size }\n"
                      "func align() -> Int { return S.alignment }\n");
    // id (8,8) + flag (1,1) → offsets 0, 8. Total running 9, struct
    // align 8, padded-up to 16.
    CHECK(f.out.source.find("return 16;") != std::string::npos);
    CHECK(f.out.source.find("return 8;") != std::string::npos);
    // The Vestra-side spelling shouldn't leak into the C++ source —
    // verifies the fold actually fired.
    CHECK(f.out.source.find("S.size") == std::string::npos);
    CHECK(f.out.source.find("S.alignment") == std::string::npos);
}

// ---- §A1 link attributes (§4.5 + §6.7) -----------------------------------

TEST_CASE("@noinit static emits as `inline T name;` with no initializer") {
    SemaEmitFixture f("@noinit @section(\"__DATA,__buf\")\n"
                      "static buf: [8]UInt8\n");
    CHECK(f.out.header.find("[[gnu::section(\"__DATA,__buf\")]] inline std::array<std::uint8_t, "
                            "8> buf;")
          != std::string::npos);
    // No `= ...` follows the name on this declaration.
    CHECK(f.out.header.find("buf =") == std::string::npos);
}

TEST_CASE("static with @section + initializer emits both") {
    SemaEmitFixture f("@section(\"__DATA,__count\")\n"
                      "static count: UInt32 = 16\n");
    CHECK(f.out.header.find("[[gnu::section(\"__DATA,__count\")]] inline std::uint32_t count = 16;")
          != std::string::npos);
}

TEST_CASE("@weak func emits [[gnu::weak]]") {
    SemaEmitFixture f("@weak\n"
                      "func default_trap() -> Int32 { return -1 }\n");
    CHECK(f.out.header.find("[[gnu::weak]]") != std::string::npos);
}

TEST_CASE("@symbol renames the func at the asm-label, on declaration only") {
    SemaEmitFixture f("@symbol(\"real_entry\")\n"
                      "func entry() -> Int32 { return 0 }\n");
    // Declaration in the header carries the asm-label.
    CHECK(f.out.header.find("entry() asm(\"real_entry\");") != std::string::npos);
    // The source-side definition omits it — C++ rejects asm-label on
    // a function definition.
    CHECK(f.out.source.find("entry() {") != std::string::npos);
    CHECK(f.out.source.find("asm(\"real_entry\")") == std::string::npos);
}

TEST_CASE("@visibility lowers to the matching gnu::visibility string") {
    SemaEmitFixture f("@visibility(.hidden)\n"
                      "func tucked_away() -> Int32 { return 0 }\n");
    CHECK(f.out.header.find("[[gnu::visibility(\"hidden\")]]") != std::string::npos);
}

TEST_CASE("map(xs, closure) lowers to __vstr::Map{xs, [&](T x){ return ... }}") {
    SemaEmitFixture f("struct It {\n"
                      "    var n: Int32\n"
                      "    inout func next() -> Int32? {\n"
                      "        if self.n <= 0 { return nil }\n"
                      "        let v = self.n\n"
                      "        self.n = self.n - 1\n"
                      "        return v\n"
                      "    }\n"
                      "}\n"
                      "func use() -> Int32 {\n"
                      "    var t: Int32 = 0\n"
                      "    var a = It(n: 5)\n"
                      "    for v in map(a, { x => x * 2 }) { t = t + v }\n"
                      "    return t\n"
                      "}\n");
    CHECK(f.out.header.find("struct Map {") != std::string::npos);
    CHECK(f.out.header.find("Map(A, F) -> Map<A, F>") != std::string::npos);
    // Closure lowers with the param typed from sema and `[&]` capture.
    CHECK(f.out.source.find("__vstr::Map{a, [&](std::int32_t x) { return x * 2; }}")
          != std::string::npos);
}

TEST_CASE("filter(xs, predicate) lowers to __vstr::Filter and demands a Bool return") {
    SemaEmitFixture f("struct It {\n"
                      "    var n: Int32\n"
                      "    inout func next() -> Int32? {\n"
                      "        if self.n <= 0 { return nil }\n"
                      "        let v = self.n\n"
                      "        self.n = self.n - 1\n"
                      "        return v\n"
                      "    }\n"
                      "}\n"
                      "func use() -> Int32 {\n"
                      "    var t: Int32 = 0\n"
                      "    var a = It(n: 10)\n"
                      "    for v in filter(a, { x => (x % 2) == 0 }) { t = t + v }\n"
                      "    return t\n"
                      "}\n");
    CHECK(f.out.header.find("struct Filter {") != std::string::npos);
    CHECK(f.out.source.find("__vstr::Filter{a, [&](std::int32_t x)") != std::string::npos);
}

TEST_CASE("take(xs, n) lowers via CTAD and casts the count to std::int64_t") {
    SemaEmitFixture f("struct It {\n"
                      "    var n: Int32\n"
                      "    inout func next() -> Int32? {\n"
                      "        if self.n <= 0 { return nil }\n"
                      "        let v = self.n\n"
                      "        self.n = self.n - 1\n"
                      "        return v\n"
                      "    }\n"
                      "}\n"
                      "func use() -> Int32 {\n"
                      "    var t: Int32 = 0\n"
                      "    var a = It(n: 10)\n"
                      "    for v in take(a, 3) { t = t + v }\n"
                      "    return t\n"
                      "}\n");
    CHECK(f.out.header.find("struct Take {") != std::string::npos);
    CHECK(f.out.source.find("__vstr::Take{a, static_cast<std::int64_t>(3)}") != std::string::npos);
}

// ---- §17.4 with name = expr { ... } binding ------------------------------

TEST_CASE("with-binding annotation emits the declared type instead of auto&&") {
    SemaEmitFixture f("func mk() -> Int32 { return 7 }\n"
                      "func wire() -> Int32 {\n"
                      "    with x: Int32 = mk() {\n"
                      "        return x * 2\n"
                      "    }\n"
                      "}\n");
    CHECK(f.out.source.find("std::int32_t x = mk();") != std::string::npos);
    CHECK(f.out.source.find("auto&& x =") == std::string::npos);
}

TEST_CASE("with-binding emits a sub-scope with `auto&& NAME = EXPR;`") {
    SemaEmitFixture f("struct R { var x: Int32 }\n"
                      "func mk() -> R { return R(x: 42) }\n"
                      "func use() -> Int32 {\n"
                      "    var total: Int32 = 0\n"
                      "    with r = mk() {\n"
                      "        total = r.x\n"
                      "    }\n"
                      "    return total\n"
                      "}\n");
    CHECK(f.out.source.find("auto&& r = mk();") != std::string::npos);
    // The binding lives inside a `{ ... }` sub-scope.
    auto auto_pos = f.out.source.find("auto&& r =");
    auto open = f.out.source.rfind("{", auto_pos);
    auto close = f.out.source.find("}", auto_pos);
    REQUIRE(open != std::string::npos);
    REQUIRE(close != std::string::npos);
    CHECK(open < auto_pos);
    CHECK(close > auto_pos);
}

// ---- §9 .mapError(_ f) on Result -----------------------------------------

TEST_CASE("mapError lowers to std::expected::transform_error") {
    SemaEmitFixture f("enum A { case bad }\n"
                      "enum B { case bad }\n"
                      "func source(_ x: Int32) throws(A) -> Int32 { return x }\n"
                      "func toB(_ e: A) -> B { return B.bad }\n"
                      "func wrap(_ x: Int32) throws(B) -> Int32 {\n"
                      "    return try source(x).mapError(toB)\n"
                      "}\n");
    // The mapError call lowers as `(base).transform_error(arg)`, and
    // the surrounding `try` carries the new error type through the
    // canonical 3-line escape.
    CHECK(f.out.source.find("(source(x)).transform_error(toB)") != std::string::npos);
    CHECK(f.out.source.find("std::expected<std::int32_t, B>") != std::string::npos);
}

// ---- §3 opaque newtype follow-ons ----------------------------------------

TEST_CASE("derive(Hash) for an opaque newtype emits a delegating std::hash") {
    SemaEmitFixture f("module example.id\n"
                      "opaque type UserId = UInt32\n"
                      "derive(Hash) for UserId\n");
    CHECK(f.out.header.find("struct std::hash<example::id::UserId>") != std::string::npos);
    CHECK(f.out.header.find("std::hash<std::uint32_t>{}(static_cast<std::uint32_t>(v))")
          != std::string::npos);
}

TEST_CASE("derive(Debug) for an opaque newtype renders `Q(value)`") {
    SemaEmitFixture f("module example.id\n"
                      "opaque type UserId = UInt32\n"
                      "derive(Debug) for UserId\n");
    CHECK(f.out.header.find("struct std::formatter<example::id::UserId>") != std::string::npos);
    CHECK(f.out.header.find("\"UserId({})\", static_cast<std::uint32_t>(v)") != std::string::npos);
}

TEST_CASE("inverse conversion T(q) lowers to static_cast over the opaque value") {
    SemaEmitFixture f("opaque type UserId = UInt32\n"
                      "func unwrap(_ u: UserId) -> UInt32 { return UInt32(u) }\n");
    CHECK(f.out.source.find("static_cast<std::uint32_t>(u)") != std::string::npos);
}

// ---- §17.7 pattern matching enhancements ---------------------------------

TEST_CASE("integer-scrutinee match lowers to an if-else-if chain") {
    SemaEmitFixture f("func bucket(_ n: Int32) -> Int32 {\n"
                      "    return match n {\n"
                      "        case 0: 100\n"
                      "        case 1 | 2 | 3: 200\n"
                      "        case 10..<20: 300\n"
                      "        default: 0\n"
                      "    }\n"
                      "}\n");
    // No switch — non-enum scrutinees take the value-scrutinee path.
    CHECK(f.out.source.find("switch (n)") == std::string::npos);
    // The IIFE binds the scrutinee, then runs the if-chain.
    CHECK(f.out.source.find("auto&& __vstr_m = n;") != std::string::npos);
    // Literal predicate.
    CHECK(f.out.source.find("(__vstr_m == 0)") != std::string::npos);
    // Or-pattern combines per-alt predicates with `||`.
    CHECK(f.out.source.find("((__vstr_m == 1) || (__vstr_m == 2) || (__vstr_m == 3))")
          != std::string::npos);
    // Exclusive range uses `<`.
    CHECK(f.out.source.find("(10 <= __vstr_m && __vstr_m < 20)") != std::string::npos);
}

TEST_CASE("inclusive range pattern uses `<=` on the upper bound") {
    SemaEmitFixture f("func grade(_ n: Int32) -> Int32 {\n"
                      "    return match n {\n"
                      "        case 90..100: 4\n"
                      "        default: 0\n"
                      "    }\n"
                      "}\n");
    CHECK(f.out.source.find("(90 <= __vstr_m && __vstr_m <= 100)") != std::string::npos);
}

TEST_CASE("bare-enum or-pattern stacks case labels above one return") {
    SemaEmitFixture f("enum Color { case red\n    case green\n    case blue\n}\n"
                      "func warm(_ c: Color) -> Int32 {\n"
                      "    return match c {\n"
                      "        case .red | .green: 1\n"
                      "        case .blue: 0\n"
                      "    }\n"
                      "}\n");
    // The switch path emits stacked labels (newline-separated), then
    // the body return on the next line.
    CHECK(f.out.source.find("case Color::red:") != std::string::npos);
    CHECK(f.out.source.find("case Color::green:") != std::string::npos);
    // Only one `return 1;` (the bodies don't duplicate).
    auto first = f.out.source.find("return 1;");
    auto second =
        first != std::string::npos ? f.out.source.find("return 1;", first + 1) : std::string::npos;
    CHECK(first != std::string::npos);
    CHECK(second == std::string::npos);
}

TEST_CASE("tuple-scrutinee match uses std::get<i> in each predicate") {
    SemaEmitFixture f("func classify(_ p: (Int32, Int32)) -> Int32 {\n"
                      "    return match p {\n"
                      "        case (0, 0): 1\n"
                      "        case (0, _): 2\n"
                      "        default: 4\n"
                      "    }\n"
                      "}\n");
    CHECK(f.out.source.find("std::get<0>(__vstr_m)") != std::string::npos);
    CHECK(f.out.source.find("std::get<1>(__vstr_m)") != std::string::npos);
    CHECK(f.out.source.find("(std::get<0>(__vstr_m) == 0) && (std::get<1>(__vstr_m) == 0)")
          != std::string::npos);
}

// ---- §4 Optional in a Display splice -------------------------------------

TEST_CASE("runtime preamble emits std::formatter<std::optional<T>> spec") {
    auto out = emit("func id(_ x: Int32) -> Int32 { return x }\n");
    CHECK(out.header.find("struct formatter<std::optional<T>, char>") != std::string::npos);
    CHECK(out.header.find("requires std::formattable<T, char>") != std::string::npos);
    CHECK(out.header.find("std::format_to(ctx.out(), \"nil\")") != std::string::npos);
}

TEST_CASE("interpolation accepts an Optional splice and emits std::format normally") {
    auto out = emit("func render(_ a: Int32?) -> String { return \"a=\\(a)\" }\n");
    // The splice goes through std::format with no special-casing at
    // the splice site; the preamble formatter handles `.none → nil`
    // and `.some(v)` → T's formatter.
    CHECK(out.source.find("std::format(\"a={}\", a)") != std::string::npos);
}

// ---- §10 Span[T] / MutSpan[T] --------------------------------------------

TEST_CASE("Span[T] lowers to std::span<const T>") {
    auto out = emit("func sum(_ s: Span[Int32]) -> Int32 { return 0 }\n");
    CHECK(out.header.find("std::span<const std::int32_t>") != std::string::npos);
}

TEST_CASE("MutSpan[T] lowers to std::span<T> (no const)") {
    auto out = emit("func mutate(_ s: MutSpan[Int32]) -> Int32 { return 0 }\n");
    CHECK(out.header.find("std::span<std::int32_t>") != std::string::npos);
    CHECK(out.header.find("std::span<const std::int32_t>") == std::string::npos);
}

TEST_CASE("Span runtime preamble includes <span>") {
    auto out = emit("func id(_ x: Int32) -> Int32 { return x }\n");
    CHECK(out.header.find("#include <span>") != std::string::npos);
}

TEST_CASE("Span .count emits a signed-width cast over std::span::size") {
    SemaEmitFixture f("func len(_ s: Span[Int32]) -> Int {\n"
                      "    return s.count\n"
                      "}\n");
    CHECK(f.out.source.find("static_cast<std::intptr_t>(s.size())") != std::string::npos);
}

TEST_CASE("Span .isEmpty emits std::span::empty") {
    SemaEmitFixture f("func empty(_ s: Span[Int32]) -> Bool {\n"
                      "    return s.isEmpty\n"
                      "}\n");
    CHECK(f.out.source.find("s.empty()") != std::string::npos);
}

TEST_CASE("Span indexing casts the index to size_t") {
    SemaEmitFixture f("func first(_ s: Span[Int32]) -> Int32 {\n"
                      "    return s[0]\n"
                      "}\n");
    CHECK(f.out.source.find("s[static_cast<std::size_t>(0)]") != std::string::npos);
}

// ---- §10 panic / abort / unreachable -------------------------------------

TEST_CASE("panic(msg) lowers to __vstr::panic(...)") {
    auto out = emit("func bad() -> Int32 { panic(\"out of range\") }\n");
    CHECK(out.source.find("__vstr::panic(") != std::string::npos);
    CHECK(out.source.find("\"out of range\"") != std::string::npos);
}

TEST_CASE("abort() and unreachable() lower to the named __vstr shims") {
    auto out = emit("func a() -> Int32 { abort() }\n"
                    "func u() -> Int32 { unreachable() }\n");
    CHECK(out.source.find("__vstr::abort_fn()") != std::string::npos);
    CHECK(out.source.find("__vstr::unreachable_fn()") != std::string::npos);
}

TEST_CASE("runtime preamble emits the __vstr::Never bottom type + shims") {
    // The preamble lives at global scope before the user's namespace,
    // and `inline` keeps the definitions safe across multi-header TUs.
    auto out = emit("func id(_ x: Int32) -> Int32 { return x }\n");
    CHECK(out.header.find("namespace __vstr {") != std::string::npos);
    CHECK(out.header.find("struct Never {") != std::string::npos);
    CHECK(out.header.find("[[noreturn]] inline Never panic(") != std::string::npos);
    CHECK(out.header.find("[[noreturn]] inline Never abort_fn()") != std::string::npos);
    CHECK(out.header.find("[[noreturn]] inline Never unreachable_fn()") != std::string::npos);
    CHECK(out.header.find("inline auto unwrap_opt(Opt&& o)") != std::string::npos);
    CHECK(out.header.find("inline auto unwrap_exp(Exp&& r)") != std::string::npos);
}

TEST_CASE("void-returning function does not carry [[nodiscard]]") {
    // The attribute is invalid on void return; emit_func gates it on
    // `f.result != nullptr || f.effects.throws_type != nullptr`.
    auto out = emit("func bail() { abort() }\n");
    CHECK(out.header.find("[[nodiscard]] void") == std::string::npos);
    CHECK(out.header.find("void bail()") != std::string::npos);
}

// ---- §12.3 derive(Default) -----------------------------------------------

TEST_CASE("derive(Default) lowers T.default() to value-init T{}") {
    // The codegen intercepts the `MemberExpr(<struct symbol>, "default")`
    // call shape and emits `T{}` — C++ value-init zeroes every field via
    // the per-field `{}` brace-init that emit_struct already writes.
    SemaEmitFixture f("struct Point { var x: Int32\n    var y: Int32 }\n"
                      "derive(Default) for Point\n"
                      "func zero() -> Point { return Point.default() }\n");
    CHECK(f.out.source.find("return Point{};") != std::string::npos);
}

TEST_CASE("derive(Default) composes for a struct holding a derived inner") {
    SemaEmitFixture f("struct Inner { var n: Int32 }\n"
                      "derive(Default) for Inner\n"
                      "struct Outer { var a: Int32\n    var b: Inner }\n"
                      "derive(Default) for Outer\n"
                      "func z() -> Outer { return Outer.default() }\n");
    CHECK(f.out.source.find("return Outer{};") != std::string::npos);
}

// ---- §9 do / catch inline error handling ---------------------------------

TEST_CASE("do/catch lowers to nested IIFEs over std::expected") {
    SemaEmitFixture f("enum E { case bad }\n"
                      "func parse(_ x: Int32) throws(E) -> Int32 {\n"
                      "    if x < 0 { throw E.bad }\n"
                      "    return x\n"
                      "}\n"
                      "func safe(_ x: Int32) -> Int32 {\n"
                      "    return do { try parse(x) } catch (e: E) { -1 }\n"
                      "}\n");
    // Inner lambda returns std::expected<T, E>; success T is the
    // do-catch's result type, which is the resolver-inferred Int32.
    CHECK(f.out.source.find("std::expected<std::int32_t, E>") != std::string::npos);
    CHECK(f.out.source.find("if (__vstr_do.has_value())") != std::string::npos);
    CHECK(f.out.source.find("[[maybe_unused]] auto e = __vstr_do.error();") != std::string::npos);
}

// ---- §9 Optional chaining (`?.`) ------------------------------------------

TEST_CASE("?. on a member returning T lowers to std::optional::transform") {
    SemaEmitFixture f("struct S { var x: Int32 }\n"
                      "func use(_ s: S?) -> Int32? { return s?.x }\n");
    CHECK(f.out.source.find("s.transform([](auto&& __vstr_o) { return __vstr_o.x; })")
          != std::string::npos);
}

TEST_CASE("?. on a member already returning Optional lowers to and_then (flatten)") {
    SemaEmitFixture f("struct Profile { var name: Str }\n"
                      "struct User { var profile: Profile? }\n"
                      "func use(_ u: User?) -> Profile? { return u?.profile }\n");
    CHECK(f.out.source.find("u.and_then([](auto&& __vstr_o) { return __vstr_o.profile; })")
          != std::string::npos);
}

TEST_CASE("?. chains compose by stacking transform / and_then") {
    SemaEmitFixture f("struct Profile { var name: Str }\n"
                      "struct User { var profile: Profile? }\n"
                      "func use(_ u: User?) -> Str? { return u?.profile?.name }\n");
    // First link flattens (Profile?), second wraps (Str → Str?).
    CHECK(f.out.source.find(".and_then([](auto&& __vstr_o) { return __vstr_o.profile; })")
          != std::string::npos);
    CHECK(f.out.source.find(".transform([](auto&& __vstr_o) { return __vstr_o.name; })")
          != std::string::npos);
}

TEST_CASE("mid-expression try hoists to a stmt-position let-binding") {
    // `try` in a binary-expression operand should NOT fall through to
    // .value(); the hoist pass pre-emits the escape and the binary
    // expression references the hoisted name.
    auto out = emit("enum E { case bad }\n"
                    "func f(_ x: Int32) throws(E) -> Int32 { return x }\n"
                    "func g(_ a: Int32, _ b: Int32) throws(E) -> Int32 {\n"
                    "    return (try f(a)) + (try f(b))\n"
                    "}\n");
    CHECK(out.source.find("auto __vstr_t0_r = f(a);") != std::string::npos);
    CHECK(out.source.find("auto __vstr_t0 = *__vstr_t0_r;") != std::string::npos);
    CHECK(out.source.find("auto __vstr_t1_r = f(b);") != std::string::npos);
    CHECK(out.source.find("auto __vstr_t1 = *__vstr_t1_r;") != std::string::npos);
    CHECK(out.source.find("(__vstr_t0) + (__vstr_t1)") != std::string::npos);
    // No panic fallback.
    CHECK(out.source.find("f(a).value()") == std::string::npos);
    CHECK(out.source.find("f(b).value()") == std::string::npos);
}

TEST_CASE("if-expr with try-in-branches hoists to a lambda returning expected") {
    // An IfExpr whose branches contain a propagating try lifts to a
    // conditional hoist: a `[&]() -> std::expected<T, E>` lambda whose
    // body emits the if-else in stmt form, then an outer propagation
    // check that re-emits the error to the enclosing function.
    SemaEmitFixture f("enum E { case bad }\n"
                      "func g(_ x: Int32) throws(E) -> Int32 { return x }\n"
                      "func h(_ a: Int32, _ b: Int32) throws(E) -> Int32 {\n"
                      "    return if a > 0 { try g(a) } else { try g(b) }\n"
                      "}\n");
    CHECK(f.out.source.find("auto __vstr_c0 = [&]() -> std::expected<std::int32_t, E>")
          != std::string::npos);
    // Each branch carries its own canonical escape — the `return
    // std::unexpected{...}` returns from the lambda, not the function.
    CHECK(f.out.source.find(
              "if (!__vstr_r.has_value()) { return std::unexpected{__vstr_r.error()}; }")
          != std::string::npos);
    // Outer propagation re-emits the error from the function.
    CHECK(f.out.source.find(
              "if (!__vstr_c0.has_value()) { return std::unexpected{__vstr_c0.error()};")
          != std::string::npos);
    // The if-expr's use-site substitutes the unwrapped value.
    CHECK(f.out.source.find("return (*__vstr_c0);") != std::string::npos);
}

TEST_CASE("let x = if c { try f() } else { try g() } hoists the same way") {
    // Statement-value position (LetStmt) used to fall back to .value()
    // because emit_expr(IfExpr) is the IIFE-returning-T form. The
    // conditional hoist now fires here too.
    SemaEmitFixture f("enum E { case bad }\n"
                      "func g() throws(E) -> Int32 { return 1 }\n"
                      "func h() throws(E) -> Int32 { return 2 }\n"
                      "func pick(_ c: Bool) throws(E) -> Int32 {\n"
                      "    let x = if c { try g() } else { try h() }\n"
                      "    return x\n"
                      "}\n");
    CHECK(f.out.source.find("auto __vstr_c0 = [&]() -> std::expected<std::int32_t, E>")
          != std::string::npos);
    CHECK(f.out.source.find("auto x = (*__vstr_c0);") != std::string::npos);
    // No .value() panic fallback anywhere.
    CHECK(f.out.source.find("g().value()") == std::string::npos);
    CHECK(f.out.source.find("h().value()") == std::string::npos);
}

TEST_CASE("if-expr as a sub-expression of a larger expr hoists") {
    SemaEmitFixture f("enum E { case bad }\n"
                      "func g() throws(E) -> Int32 { return 1 }\n"
                      "func h() throws(E) -> Int32 { return 2 }\n"
                      "func plus_one(_ c: Bool) throws(E) -> Int32 {\n"
                      "    return (if c { try g() } else { try h() }) + 1\n"
                      "}\n");
    CHECK(f.out.source.find("auto __vstr_c0 = [&]() -> std::expected<std::int32_t, E>")
          != std::string::npos);
    // The substituted IfExpr appears inside the outer BinaryExpr.
    CHECK(f.out.source.find("((*__vstr_c0)) + 1") != std::string::npos);
}

TEST_CASE("match-expr with try-in-arms hoists like an if-expr does") {
    // Symmetric with the IfExpr cond-hoist: a MatchExpr whose arm
    // bodies contain a propagating try lifts to a lambda returning
    // std::expected<T, E>, emitted as a statement-form switch (bare
    // enum) so each arm's `return` returns from the lambda directly.
    SemaEmitFixture f("enum E { case bad }\n"
                      "enum Cmd { case noop\n    case takeFirst\n    case takeSecond\n}\n"
                      "func tryFirst() throws(E) -> Int32 { return 1 }\n"
                      "func trySecond() throws(E) -> Int32 { return 2 }\n"
                      "func defaultVal() -> Int32 { return 10 }\n"
                      "func dispatch(_ c: Cmd) throws(E) -> Int32 {\n"
                      "    return match c {\n"
                      "        case .noop:        defaultVal()\n"
                      "        case .takeFirst:   try tryFirst()\n"
                      "        case .takeSecond:  try trySecond()\n"
                      "    }\n"
                      "}\n");
    // The MatchExpr is hoisted as a lambda returning expected<T, E>.
    CHECK(f.out.source.find("auto __vstr_c0 = [&]() -> std::expected<std::int32_t, E>")
          != std::string::npos);
    // The lambda body uses a statement-form switch (not a nested IIFE),
    // so each arm's `return` returns from the cond-hoist lambda
    // directly — which is what makes propagation work.
    CHECK(f.out.source.find("switch (c) {") != std::string::npos);
    CHECK(f.out.source.find("case Cmd::noop:") != std::string::npos);
    // Each arm with a try carries the canonical 3-line escape.
    CHECK(f.out.source.find(
              "if (!__vstr_r.has_value()) { return std::unexpected{__vstr_r.error()}; }")
          != std::string::npos);
    // Outer propagation re-emits the error from the function.
    CHECK(f.out.source.find(
              "if (!__vstr_c0.has_value()) { return std::unexpected{__vstr_c0.error()};")
          != std::string::npos);
    // The use-site substitutes the unwrapped value.
    CHECK(f.out.source.find("return (*__vstr_c0);") != std::string::npos);
}

TEST_CASE("let x = match { ... try ... } hoists to a sibling lambda") {
    SemaEmitFixture f("enum E { case bad }\n"
                      "enum Cmd { case noop\n    case fetch\n}\n"
                      "func tryFetch() throws(E) -> Int32 { return 7 }\n"
                      "func defaultVal() -> Int32 { return 0 }\n"
                      "func dispatch(_ c: Cmd) throws(E) -> Int32 {\n"
                      "    let x = match c {\n"
                      "        case .noop:  defaultVal()\n"
                      "        case .fetch: try tryFetch()\n"
                      "    }\n"
                      "    return x + 1\n"
                      "}\n");
    CHECK(f.out.source.find("auto __vstr_c0 = [&]() -> std::expected<std::int32_t, E>")
          != std::string::npos);
    CHECK(f.out.source.find("auto x = (*__vstr_c0);") != std::string::npos);
}

TEST_CASE("payloaded match with try-in-arms uses std::holds_alternative chain") {
    // The payloaded path can't reuse emit_match's std::visit lambda
    // because a `return std::unexpected{...}` inside that lambda would
    // return from the visit's lambda rather than the outer cond-hoist
    // lambda. emit_match_in_lambda switches to an if-chain over
    // std::holds_alternative so every `return` is rooted in the
    // cond-hoist scope.
    SemaEmitFixture f("enum E { case bad }\n"
                      "enum Shape {\n"
                      "    case circle(radius: Int32)\n"
                      "    case rect(width: Int32, height: Int32)\n"
                      "    case empty\n"
                      "}\n"
                      "func tryDouble(_ x: Int32) throws(E) -> Int32 { return x * 2 }\n"
                      "func area(_ s: Shape) throws(E) -> Int32 {\n"
                      "    return match s {\n"
                      "        case .circle(let r):       try tryDouble(r)\n"
                      "        case .rect(let w, let h):  try tryDouble(w * h)\n"
                      "        case .empty:               0\n"
                      "    }\n"
                      "}\n");
    CHECK(f.out.source.find("auto __vstr_c0 = [&]() -> std::expected<std::int32_t, E>")
          != std::string::npos);
    // No std::visit on the cond-hoist path.
    CHECK(f.out.source.find("std::visit([&](auto&& __vstr_alt)") == std::string::npos);
    // Instead: if-chain over std::holds_alternative.
    CHECK(f.out.source.find("std::holds_alternative<Shape::circle_t>(__vstr_alt_ref)")
          != std::string::npos);
    CHECK(f.out.source.find("auto&& r = __vstr_alt.radius;") != std::string::npos);
    // Empty-payload arm doesn't mint the unused alt handle.
    auto pos_empty = f.out.source.find("std::holds_alternative<Shape::empty_t>");
    REQUIRE(pos_empty != std::string::npos);
    auto pos_unreach = f.out.source.find("std::unreachable()", pos_empty);
    REQUIRE(pos_unreach != std::string::npos);
    CHECK(f.out.source.find("std::get<Shape::empty_t>", pos_empty) > pos_unreach);
}

TEST_CASE("try inside a sub-expression of an if branch fires the branch-local hoist") {
    // The branch body `(try f()) + 1` is a BinaryExpr — emit_stmt_expr's
    // generic tail now collects local try-hoists and pre-emits them
    // inside the branch's brace, so the try actually escapes the
    // enclosing lambda via `return std::unexpected{...}`.
    SemaEmitFixture f("enum E { case bad }\n"
                      "func g() throws(E) -> Int32 { return 1 }\n"
                      "func with_bump(_ c: Bool, _ fallback: Int32) throws(E) -> Int32 {\n"
                      "    return if c { (try g()) + 1 } else { fallback }\n"
                      "}\n");
    // The conditional hoist still fires.
    CHECK(f.out.source.find("auto __vstr_c0 = [&]() -> std::expected<std::int32_t, E>")
          != std::string::npos);
    // Inside the then-branch, the BinaryExpr's try gets its own
    // local hoist via the canonical 3-line escape.
    CHECK(f.out.source.find("auto __vstr_t1 = *__vstr_t1_r;") != std::string::npos);
    // The trailing expression substitutes the hoisted name.
    CHECK(f.out.source.find("return (__vstr_t1) + 1;") != std::string::npos);
    // No .value() panic fallback.
    CHECK(f.out.source.find("g().value()") == std::string::npos);
}

TEST_CASE("throw inside an if branch lowers as a real return-of-unexpected") {
    SemaEmitFixture f("enum E { case bad }\n"
                      "func d(_ a: Int32, _ b: Int32) throws(E) -> Int32 {\n"
                      "    if b == 0 { throw E.bad }\n"
                      "    return a / b\n"
                      "}\n");
    // No `std::unreachable()` IIFE escape — the throw should sit in a
    // direct `if (...) { return std::unexpected{...}; }` form.
    CHECK(f.out.source.find("if (b == 0) { return std::unexpected{E::bad};") != std::string::npos);
    CHECK(f.out.source.find("std::unreachable") == std::string::npos);
}

// ---- §7 generics phase 2: user-defined generic structs --------------------

TEST_CASE("generic struct lowers to a C++ class template") {
    SemaEmitFixture f("struct Pair[T] { var first: T  var second: T }\n"
                      "func sum(_ p: Pair[Int32]) -> Int32 { return p.first + p.second }\n");
    // Decl emits a class template; the field types are the bare param T.
    CHECK(f.out.header.find("template <class T>") != std::string::npos);
    CHECK(f.out.header.find("struct Pair {") != std::string::npos);
    CHECK(f.out.header.find("T first") != std::string::npos);
    CHECK(f.out.header.find("T second") != std::string::npos);
    // A parameter typed Pair[Int32] lowers to the specialization name.
    CHECK(f.out.source.find("Pair<std::int32_t>") != std::string::npos);
}

TEST_CASE("generic struct construction emits the inferred specialization") {
    SemaEmitFixture f("struct Pair[T] { var first: T  var second: T }\n"
                      "func make(_ a: Int32, _ b: Int32) -> Pair[Int32] {\n"
                      "    return Pair(first: a, second: b)\n"
                      "}\n");
    CHECK(f.out.source.find("Pair<std::int32_t>{.first = a, .second = b}") != std::string::npos);
}

TEST_CASE("two-parameter generic struct emits a two-parameter template") {
    SemaEmitFixture f("struct KeyValue[K, V] { var key: K  var value: V }\n"
                      "func mk(_ k: Int32, _ v: Bool) -> KeyValue[Int32, Bool] {\n"
                      "    return KeyValue(key: k, value: v)\n"
                      "}\n");
    CHECK(f.out.header.find("template <class K, class V>") != std::string::npos);
    CHECK(f.out.header.find("struct KeyValue {") != std::string::npos);
    CHECK(f.out.source.find("KeyValue<std::int32_t, bool>{.key = k, .value = v}")
          != std::string::npos);
}

// ---- §7 generics phase 2: user-defined generic enums ----------------------

TEST_CASE("generic enum lowers to a C++ class template over the variant shape") {
    SemaEmitFixture f("enum Maybe[T] { case just(T)  case nothing }\n"
                      "func unwrap(_ m: Maybe[Int32], _ d: Int32) -> Int32 {\n"
                      "    return match m {\n"
                      "        case .just(let x): x\n"
                      "        case .nothing:     d\n"
                      "    }\n"
                      "}\n");
    CHECK(f.out.header.find("template <class T>") != std::string::npos);
    CHECK(f.out.header.find("struct Maybe {") != std::string::npos);
    CHECK(f.out.header.find("struct just_t {T _0{}; }") != std::string::npos);
    CHECK(f.out.header.find("std::variant<just_t, nothing_t>") != std::string::npos);
    // The match dispatch qualifies the case_t with the instance args.
    CHECK(f.out.source.find("std::is_same_v<__vstr_alt_t, Maybe<std::int32_t>::just_t>")
          != std::string::npos);
}

TEST_CASE("generic enum case construction emits the inferred specialization") {
    SemaEmitFixture f("enum Maybe[T] { case just(T)  case nothing }\n"
                      "func mk(_ v: Int32) -> Maybe[Int32] { return Maybe.just(v) }\n"
                      "func none() -> Maybe[Int32] { return Maybe.nothing }\n");
    CHECK(f.out.source.find("Maybe<std::int32_t>{Maybe<std::int32_t>::just_t{v}}")
          != std::string::npos);
    CHECK(f.out.source.find("Maybe<std::int32_t>{Maybe<std::int32_t>::nothing_t{}}")
          != std::string::npos);
}

TEST_CASE("two-parameter generic enum emits a two-parameter template") {
    SemaEmitFixture f("enum Either[L, R] { case left(L)  case right(R) }\n"
                      "func mk(_ n: Int32) -> Either[Int32, Bool] { return Either.left(n) }\n");
    CHECK(f.out.header.find("template <class L, class R>") != std::string::npos);
    CHECK(f.out.header.find("struct Either {") != std::string::npos);
    CHECK(f.out.source.find("Either<std::int32_t, bool>{Either<std::int32_t, bool>::left_t{n}}")
          != std::string::npos);
}

// ---- §7 generics phase 2: const generics ----------------------------------

TEST_CASE("const generic struct lowers to a class template with std::size_t N") {
    SemaEmitFixture f("struct Buffer[T, const N: Int] { var data: [N]T }\n"
                      "func first(_ b: Buffer[Int32, 4]) -> Int32 { return b.data[0] }\n");
    CHECK(f.out.header.find("template <class T, std::size_t N>") != std::string::npos);
    CHECK(f.out.header.find("struct Buffer {") != std::string::npos);
    // The [N]T field emits the symbolic length name inside the template.
    CHECK(f.out.header.find("std::array<T, N> data") != std::string::npos);
    // A parameter typed Buffer[Int32, 4] lowers to the specialization.
    CHECK(f.out.source.find("Buffer<std::int32_t, 4>") != std::string::npos);
}

TEST_CASE("const generic construction emits the inferred specialization") {
    SemaEmitFixture f("struct Buffer[T, const N: Int] { var data: [N]T }\n"
                      "func make() -> Buffer[Int32, 4] {\n"
                      "    return Buffer(data: [10, 20, 30, 40])\n"
                      "}\n");
    CHECK(f.out.source.find("Buffer<std::int32_t, 4>{.data = {10, 20, 30, 40}}")
          != std::string::npos);
}

// ---- §7 generics phase 2: protocol bounds (requires-clauses) --------------

TEST_CASE("an Eq bound lowers to a std::equality_comparable requires clause") {
    SemaEmitFixture f("func eq[T: Eq](_ a: T, _ b: T) -> Bool { return a == b }\n");
    CHECK(f.out.header.find("template <class T>") != std::string::npos);
    CHECK(f.out.header.find("requires (std::equality_comparable<T>)") != std::string::npos);
}

TEST_CASE("a where Comparable bound lowers to std::totally_ordered") {
    SemaEmitFixture f("func mx[T](_ a: T, _ b: T) -> T where T: Comparable {\n"
                      "    return if a > b { a } else { b }\n"
                      "}\n");
    CHECK(f.out.header.find("requires (std::totally_ordered<T>)") != std::string::npos);
}

TEST_CASE("a bound without a standard concept emits no requires clause") {
    // Hash has no clean C++20 concept, so the template is emitted
    // unconstrained (the Vestra call site still enforces conformance).
    SemaEmitFixture f("func h[T: Hash](_ a: T) -> Bool { return true }\n");
    CHECK(f.out.header.find("template <class T>") != std::string::npos);
    CHECK(f.out.header.find("requires (") == std::string::npos);
}

// ---- §8 leading-dot construction of a payloaded enum ----------------------

TEST_CASE("leading-dot construction of a payloaded enum emits the variant wrap") {
    SemaEmitFixture f("enum Shape { case circle(r: Float64)  case point }\n"
                      "func a() -> Shape { return .point }\n"
                      "func b() -> Shape { return .circle(r: 1.0) }\n");
    // No-payload case via leading dot wraps the variant, not `Shape::point`.
    CHECK(f.out.source.find("Shape{Shape::point_t{}}") != std::string::npos);
    // Payloaded case via leading dot slots the argument into the case_t.
    CHECK(f.out.source.find("Shape{Shape::circle_t{1.0}}") != std::string::npos);
}

TEST_CASE("leading-dot construction of a generic enum qualifies with the instance args") {
    SemaEmitFixture f("enum Maybe[T] { case just(T)  case nothing }\n"
                      "func n() -> Maybe[Int32] { return .nothing }\n"
                      "func j(_ v: Int32) -> Maybe[Int32] { return .just(v) }\n");
    CHECK(f.out.source.find("Maybe<std::int32_t>{Maybe<std::int32_t>::nothing_t{}}")
          != std::string::npos);
    CHECK(f.out.source.find("Maybe<std::int32_t>{Maybe<std::int32_t>::just_t{v}}")
          != std::string::npos);
}
