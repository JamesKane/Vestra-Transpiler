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
    // The branch bodies are inner block-expressions, so they each go
    // through the IIFE form — we just check the leaf returns survive.
    CHECK(f.out.source.find("return v;") != std::string::npos);
    CHECK(f.out.source.find("return 0;") != std::string::npos);
}
