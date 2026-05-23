#include "vestra/ast/nodes.hpp"
#include "vestra/diag/diagnostic.hpp"
#include "vestra/diag/source_manager.hpp"
#include "vestra/lex/lexer.hpp"
#include "vestra/parse/parser.hpp"

#include <doctest/doctest.h>

#include <string>

namespace {

vestra::ast::CompilationUnit
parse(vestra::diag::SourceManager& sm, vestra::diag::DiagnosticReporter& rep, std::string text) {
    auto fid = sm.add_in_memory("<test>", std::move(text));
    vestra::lex::Lexer lex(sm, fid, rep);
    auto tokens = lex.tokenize();
    vestra::parse::Parser parser(tokens, rep);
    return parser.parse_unit();
}

}  // namespace

TEST_CASE("parse a minimal func with a return") {
    vestra::diag::SourceManager sm;
    vestra::diag::DiagnosticReporter rep(sm);
    auto unit = parse(sm, rep, "func answer() -> Int { return 42 }\n");
    CHECK_FALSE(rep.has_errors());
    REQUIRE(unit.decls.size() == 1);
    CHECK(unit.decls[0]->kind == vestra::ast::NodeKind::Func);
    auto& f = static_cast<vestra::ast::FuncDecl&>(*unit.decls[0]);
    CHECK(f.name == "answer");
    REQUIRE(f.body);
    CHECK(f.body->kind == vestra::ast::NodeKind::BlockExpr);
}

TEST_CASE("parse a module + import + struct") {
    vestra::diag::SourceManager sm;
    vestra::diag::DiagnosticReporter rep(sm);
    auto unit = parse(sm,
                      rep,
                      "module core.math\n"
                      "import core.io\n"
                      "struct Point { var x: Int32; var y: Int32 }\n");
    CHECK_FALSE(rep.has_errors());
    REQUIRE(unit.module);
    CHECK(unit.module->path.size() == 2);
    CHECK(unit.module->path[0] == "core");
    CHECK(unit.imports.size() == 1);
    REQUIRE(unit.decls.size() == 1);
    CHECK(unit.decls[0]->kind == vestra::ast::NodeKind::Struct);
}

TEST_CASE("Pratt precedence: 1 + 2 * 3 parses as (1 + (2 * 3))") {
    vestra::diag::SourceManager sm;
    vestra::diag::DiagnosticReporter rep(sm);
    auto unit = parse(sm, rep, "const r = 1 + 2 * 3\n");
    CHECK_FALSE(rep.has_errors());
    REQUIRE(unit.decls.size() == 1);
    auto& c = static_cast<vestra::ast::ConstDecl&>(*unit.decls[0]);
    REQUIRE(c.value);
    REQUIRE(c.value->kind == vestra::ast::NodeKind::BinaryExpr);
    auto& top = static_cast<vestra::ast::BinaryExpr&>(*c.value);
    CHECK(top.op == vestra::ast::BinaryOp::Add);
    CHECK(top.rhs->kind == vestra::ast::NodeKind::BinaryExpr);
    auto& rhs = static_cast<vestra::ast::BinaryExpr&>(*top.rhs);
    CHECK(rhs.op == vestra::ast::BinaryOp::Mul);
}

TEST_CASE("enum with bare cases") {
    vestra::diag::SourceManager sm;
    vestra::diag::DiagnosticReporter rep(sm);
    auto unit = parse(sm, rep, "enum Color { case red; case green; case blue }\n");
    CHECK_FALSE(rep.has_errors());
    REQUIRE(unit.decls.size() == 1);
    auto& e = static_cast<vestra::ast::EnumDecl&>(*unit.decls[0]);
    CHECK(e.cases.size() == 3);
    CHECK(e.cases[0].name == "red");
}
