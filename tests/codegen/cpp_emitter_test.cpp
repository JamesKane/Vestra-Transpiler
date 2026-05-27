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
