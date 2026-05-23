#include "vestra/diag/diagnostic.hpp"
#include "vestra/diag/source_manager.hpp"
#include "vestra/lex/lexer.hpp"
#include "vestra/parse/parser.hpp"
#include "vestra/sema/comptime.hpp"
#include "vestra/sema/resolver.hpp"
#include "vestra/sema/types.hpp"

#include <doctest/doctest.h>

#include <optional>
#include <string>

namespace {

// Direct-fold helper: parse a single Vestra expression, fold it against an
// empty environment, return the result. We wrap the expression in a const
// so the parser accepts it as a unit.
std::optional<vestra::sema::ComptimeValue> fold_expr(std::string expr_source) {
    vestra::diag::SourceManager sm;
    vestra::diag::DiagnosticReporter rep(sm);
    auto text = "const _x = " + std::move(expr_source) + "\n";
    auto fid = sm.add_in_memory("<test>", std::move(text));
    vestra::lex::Lexer lex(sm, fid, rep);
    auto tokens = lex.tokenize();
    vestra::parse::Parser p(tokens, rep);
    auto unit = p.parse_unit();
    REQUIRE_FALSE(rep.has_errors());
    REQUIRE(unit.decls.size() == 1);
    auto& cd = static_cast<vestra::ast::ConstDecl&>(*unit.decls[0]);
    REQUIRE(cd.value);

    vestra::sema::ComptimeFolder folder;
    return folder.fold(*cd.value, {});
}

// Pipeline-fold helper: lex + parse + run the resolver (which calls the
// folder for each const initializer), then look up the named const's
// folded value in the resolution side table.
std::optional<vestra::sema::ComptimeValue> resolver_fold(std::string source,
                                                         std::string const_name) {
    vestra::diag::SourceManager sm;
    vestra::diag::DiagnosticReporter rep(sm);
    auto fid = sm.add_in_memory("<test>", std::move(source));
    vestra::lex::Lexer lex(sm, fid, rep);
    auto tokens = lex.tokenize();
    vestra::parse::Parser p(tokens, rep);
    auto unit = p.parse_unit();
    REQUIRE_FALSE(rep.has_errors());

    vestra::sema::TypeArena arena;
    vestra::sema::Resolver res(unit, arena, rep);
    res.resolve();
    REQUIRE_FALSE(rep.has_errors());

    // Find the matching ConstDecl and ask the side table for its folded value.
    for (const auto& d : unit.decls) {
        if (d->kind != vestra::ast::NodeKind::Const) {
            continue;
        }
        const auto& cd = static_cast<const vestra::ast::ConstDecl&>(*d);
        if (cd.name != const_name) {
            continue;
        }
        if (cd.value == nullptr) {
            return std::nullopt;
        }
        if (const auto* v = res.resolution().folded_value(cd.value.get())) {
            return *v;
        }
        return std::nullopt;
    }
    return std::nullopt;
}

}  // namespace

// ---- Direct folder: arithmetic --------------------------------------------

TEST_CASE("fold an integer literal") {
    auto v = fold_expr("42");
    REQUIRE(v);
    CHECK(v->kind == vestra::sema::ComptimeValue::Kind::Int);
    CHECK(v->i == 42);
}

TEST_CASE("fold mixed-arity arithmetic with precedence") {
    auto v = fold_expr("1 + 2 * 3");
    REQUIRE(v);
    CHECK(v->i == 7);
}

TEST_CASE("fold bit operations") {
    auto v = fold_expr("(1 << 8) - 1");
    REQUIRE(v);
    CHECK(v->i == 255);
}

TEST_CASE("fold unary negation") {
    auto v = fold_expr("-(10 - 5)");
    REQUIRE(v);
    CHECK(v->i == -5);
}

TEST_CASE("fold a Float64 product") {
    auto v = fold_expr("1.5 * 2.0");
    REQUIRE(v);
    CHECK(v->kind == vestra::sema::ComptimeValue::Kind::Float);
    CHECK(v->f == doctest::Approx(3.0));
}

// ---- Booleans + comparisons + short-circuit -------------------------------

TEST_CASE("fold comparisons to a bool") {
    auto v = fold_expr("5 < 10");
    REQUIRE(v);
    CHECK(v->kind == vestra::sema::ComptimeValue::Kind::Bool);
    CHECK(v->b == true);
}

TEST_CASE("fold short-circuiting && stops at false") {
    auto v = fold_expr("false && 1 / 0 == 1");
    REQUIRE(v);
    CHECK(v->b == false);
}

TEST_CASE("fold short-circuiting || stops at true") {
    auto v = fold_expr("true || 1 / 0 == 1");
    REQUIRE(v);
    CHECK(v->b == true);
}

// ---- if expressions -------------------------------------------------------

TEST_CASE("fold an if-expression picks the true branch") {
    auto v = fold_expr("if 4 * 100 > 300 { 7 } else { 0 }");
    REQUIRE(v);
    CHECK(v->i == 7);
}

TEST_CASE("fold an if-expression picks the false branch") {
    auto v = fold_expr("if 1 == 2 { 99 } else { 11 + 22 }");
    REQUIRE(v);
    CHECK(v->i == 33);
}

// ---- non-foldable cases ---------------------------------------------------

TEST_CASE("calls are not foldable in phase 1") {
    // Wrap in a function declaration so we can reference foo from inside x.
    vestra::diag::SourceManager sm;
    vestra::diag::DiagnosticReporter rep(sm);
    auto fid = sm.add_in_memory("<test>",
                                "func foo() -> Int { return 1 }\n"
                                "const x: Int = foo()\n");
    vestra::lex::Lexer lex(sm, fid, rep);
    auto tokens = lex.tokenize();
    vestra::parse::Parser p(tokens, rep);
    auto unit = p.parse_unit();
    REQUIRE_FALSE(rep.has_errors());
    vestra::sema::TypeArena arena;
    vestra::sema::Resolver res(unit, arena, rep);
    res.resolve();
    REQUIRE_FALSE(rep.has_errors());
    // x's initializer is a call — folder bails, side table has no entry.
    const auto& cd = static_cast<const vestra::ast::ConstDecl&>(*unit.decls[1]);
    CHECK(res.resolution().folded_value(cd.value.get()) == nullptr);
}

TEST_CASE("division by zero bails out rather than poisoning the result") {
    auto v = fold_expr("10 / 0");
    CHECK_FALSE(v.has_value());
}

// ---- Resolver integration -------------------------------------------------

TEST_CASE("resolver folds a const initializer and routes it through the side table") {
    auto v = resolver_fold("const Page: Int32 = comptime { 1 << 12 }\n", "Page");
    REQUIRE(v);
    CHECK(v->i == 4096);
}

TEST_CASE("a later const can reference an earlier folded const") {
    auto v = resolver_fold("const A: Int32 = 7\n"
                           "const B: Int32 = A * 6\n",
                           "B");
    REQUIRE(v);
    CHECK(v->i == 42);
}

TEST_CASE("folding a Bool comparison threads through to the side table") {
    auto v = resolver_fold("const Big: Bool = 100 > 50\n", "Big");
    REQUIRE(v);
    CHECK(v->kind == vestra::sema::ComptimeValue::Kind::Bool);
    CHECK(v->b == true);
}
