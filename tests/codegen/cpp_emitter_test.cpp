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
