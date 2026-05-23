#include "vestra/ast/nodes.hpp"
#include "vestra/ast/printer.hpp"
#include "vestra/diag/diagnostic.hpp"
#include "vestra/diag/source_manager.hpp"
#include "vestra/lex/lexer.hpp"
#include "vestra/parse/parser.hpp"

#include <doctest/doctest.h>

#include <string>

namespace {

std::string round_trip(std::string text) {
    vestra::diag::SourceManager sm;
    vestra::diag::DiagnosticReporter rep(sm);
    auto fid = sm.add_in_memory("<test>", std::move(text));
    vestra::lex::Lexer lex(sm, fid, rep);
    auto tokens = lex.tokenize();
    vestra::parse::Parser p(tokens, rep);
    auto unit = p.parse_unit();
    REQUIRE_FALSE(rep.has_errors());
    vestra::ast::Printer pr;
    return pr.print(unit);
}

}  // namespace

TEST_CASE("printer emits a func declaration") {
    auto out = round_trip("func square(_ x: Int32) -> Int32 { return x * x }\n");
    // We don't pin the exact whitespace — only check the signature survives.
    CHECK(out.find("func square") != std::string::npos);
    CHECK(out.find("Int32") != std::string::npos);
}

TEST_CASE("printer prints struct fields") {
    auto out = round_trip("struct Point { var x: Int32; var y: Int32 }\n");
    CHECK(out.find("struct Point") != std::string::npos);
    CHECK(out.find("var x: Int32") != std::string::npos);
}
