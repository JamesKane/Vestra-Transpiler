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
    SemaEmitFixture f("enum Color { case red\n    case green\n    case blue\n}\n"
                      "func ch(_ c: Color) -> Int32 {\n"
                      "    return match c {\n"
                      "        case .red:   1\n"
                      "        case .green: 2\n"
                      "        case .blue:  3\n"
                      "    }\n"
                      "}\n");
    CHECK(f.out.source.find("switch (c)") != std::string::npos);
    CHECK(f.out.source.find("case Color::red: return 1") != std::string::npos);
    CHECK(f.out.source.find("case Color::blue: return 3") != std::string::npos);
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
    SemaEmitFixture f("struct Bare { var n: Int32 }\n");
    CHECK(f.out.header.find("std::formatter") == std::string::npos);
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

TEST_CASE("postfix ! lowers to std::optional::value()") {
    auto out = emit("func use(_ o: Int32?) -> Int32 { return o! }\n");
    CHECK(out.source.find("o.value()") != std::string::npos);
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

TEST_CASE("try! lowers to std::expected::value()") {
    auto out = emit("enum E { case bad }\n"
                    "func f() throws(E) -> Int32 { return 7 }\n"
                    "func g() -> Int32 { return try! f() }\n");
    CHECK(out.source.find("f().value()") != std::string::npos);
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

TEST_CASE("try inside an if branch keeps the per-branch lowering (not hoisted)") {
    // The hoist walk refuses to descend into IfExpr branches, so a try
    // inside one stays handled by emit_stmt_expr's per-branch recursion.
    auto out = emit("enum E { case bad }\n"
                    "func f(_ x: Int32) throws(E) -> Int32 { return x }\n"
                    "func g(_ a: Int32, _ b: Int32) throws(E) -> Int32 {\n"
                    "    return if a > 0 { try f(a) } else { try f(b) }\n"
                    "}\n");
    // The hoist pass left both tries alone, so there should be NO
    // top-of-function __vstr_t0 binding before the if.
    CHECK(out.source.find("auto __vstr_t0 = ") == std::string::npos);
    // Each branch carries its own canonical escape via the per-branch
    // emit_stmt_expr fallback.
    CHECK(
        out.source.find("if (!__vstr_r.has_value()) { return std::unexpected{__vstr_r.error()}; }")
        != std::string::npos);
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
