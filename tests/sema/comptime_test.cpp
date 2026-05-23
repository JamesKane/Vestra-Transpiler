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

// ---- §12.1 phase 2: comptime function calls -------------------------------

TEST_CASE("phase 2: a simple comptime function call folds") {
    auto v = resolver_fold("comptime func double(_ x: Int) -> Int { return x * 2 }\n"
                           "const Four: Int = comptime { double(2) }\n",
                           "Four");
    REQUIRE(v);
    CHECK(v->i == 4);
}

TEST_CASE("phase 2: recursive comptime call folds (factorial)") {
    auto v = resolver_fold("comptime func factorial(_ n: Int) -> Int {\n"
                           "    return if n <= 1 { 1 } else { n * factorial(n - 1) }\n"
                           "}\n"
                           "const Fact10: Int = comptime { factorial(10) }\n",
                           "Fact10");
    REQUIRE(v);
    CHECK(v->i == 3628800);
}

TEST_CASE("phase 2: comptime call composed with a binary op") {
    auto v = resolver_fold("comptime func sq(_ n: Int) -> Int { return n * n }\n"
                           "const SumOfSquares: Int = comptime { sq(3) + sq(4) }\n",
                           "SumOfSquares");
    REQUIRE(v);
    CHECK(v->i == 25);  // 9 + 16
}

TEST_CASE("phase 2: a regular (non-comptime) func is not foldable") {
    // `double` is missing the `comptime` modifier — the folder must refuse
    // and leave the codegen to emit the runtime call.
    auto v = resolver_fold("func double(_ x: Int) -> Int { return x * 2 }\n"
                           "const Four: Int = double(2)\n",
                           "Four");
    CHECK_FALSE(v.has_value());
}

TEST_CASE("phase 2: arg-count mismatch bails the fold rather than misfolding") {
    auto v = resolver_fold("comptime func add(_ a: Int, _ b: Int) -> Int { return a + b }\n"
                           "const Bad: Int = comptime { add(1) }\n",
                           "Bad");
    // The check_call sema pass will have already flagged this as an arity
    // error; the folder's job here is just not to corrupt anything in the
    // process. Absence from the side table is the contract.
    CHECK_FALSE(v.has_value());
}

TEST_CASE("phase 2: recursion past the depth cap stops cleanly") {
    // factorial recurses linearly. MaxDepth is 64; we call with 200, which
    // would otherwise produce a 200-deep chain. The fold must bail rather
    // than blow up the host stack or loop forever.
    auto v = resolver_fold("comptime func factorial(_ n: Int) -> Int {\n"
                           "    return if n <= 1 { 1 } else { n * factorial(n - 1) }\n"
                           "}\n"
                           "const Big: Int = comptime { factorial(200) }\n",
                           "Big");
    CHECK_FALSE(v.has_value());
}
