#include "vestra/diag/diagnostic.hpp"
#include "vestra/diag/source_manager.hpp"
#include "vestra/lex/lexer.hpp"
#include "vestra/parse/parser.hpp"
#include "vestra/sema/resolver.hpp"
#include "vestra/sema/types.hpp"

#include <doctest/doctest.h>

#include <string>
#include <string_view>

namespace {

// Helper: lex+parse+sema-check `source`, return how many errors the reporter
// accumulated. `unit` is held inside the call; we only care about the count.
std::size_t check_errors(std::string source) {
    vestra::diag::SourceManager sm;
    vestra::diag::DiagnosticReporter rep(sm);
    auto fid = sm.add_in_memory("<test>", std::move(source));
    vestra::lex::Lexer lex(sm, fid, rep);
    auto tokens = lex.tokenize();
    vestra::parse::Parser p(tokens, rep);
    auto unit = p.parse_unit();
    if (rep.has_errors()) {
        return rep.error_count();
    }
    vestra::sema::TypeArena arena;
    vestra::sema::Resolver res(unit, arena, rep);
    res.resolve();
    return rep.error_count();
}

// Helper: as above but also returns the first error message text — useful for
// asserting on specific diagnostics.
struct CheckResult {
    std::size_t error_count = 0;
    std::string first_message;
};

CheckResult check_detail(std::string source) {
    vestra::diag::SourceManager sm;
    vestra::diag::DiagnosticReporter rep(sm);
    auto fid = sm.add_in_memory("<test>", std::move(source));
    vestra::lex::Lexer lex(sm, fid, rep);
    auto tokens = lex.tokenize();
    vestra::parse::Parser p(tokens, rep);
    auto unit = p.parse_unit();
    if (!rep.has_errors()) {
        vestra::sema::TypeArena arena;
        vestra::sema::Resolver res(unit, arena, rep);
        res.resolve();
    }
    CheckResult cr;
    cr.error_count = rep.error_count();
    if (!rep.diagnostics().empty()) {
        cr.first_message = rep.diagnostics().front().message;
    }
    return cr;
}

}  // namespace

TEST_CASE("a clean program produces no sema errors") {
    CHECK(check_errors("func square(_ x: Int32) -> Int32 { return x * x }\n"
                       "func main() -> Int32 { return square(7) }\n")
          == 0);
}

TEST_CASE("undefined name is reported") {
    auto r = check_detail("func main() -> Int32 { return missing }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("undefined name 'missing'") != std::string::npos);
}

TEST_CASE("duplicate top-level definition is reported with a note") {
    auto r = check_detail("func foo() -> Int { return 1 }\n"
                          "func foo() -> Int { return 2 }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("duplicate definition of 'foo'") != std::string::npos);
}

TEST_CASE("binary operator with mismatched operand types") {
    auto r = check_detail("func bad() -> Bool {\n"
                          "    let a: Int32 = 0\n"
                          "    let b: Int64 = 0\n"
                          "    return a == b\n"
                          "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("different types") != std::string::npos);
}

TEST_CASE("wrong argument count to a call is reported") {
    auto r = check_detail("func one(_ x: Int) -> Int { return x }\n"
                          "func main() -> Int { return one(1, 2) }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("expects 1 argument") != std::string::npos);
}

TEST_CASE("argument type mismatch is reported") {
    auto r = check_detail("func need_bool(_ b: Bool) -> Bool { return b }\n"
                          "func main() -> Bool {\n"
                          "    let i: Int32 = 0\n"
                          "    return need_bool(i)\n"
                          "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("does not match parameter type") != std::string::npos);
}

TEST_CASE("function return type mismatch is reported") {
    auto r = check_detail("func f() -> Int32 {\n"
                          "    let b: Bool = true\n"
                          "    return b\n"
                          "}\n");
    CHECK(r.error_count >= 1);
}

TEST_CASE("integer literal adapts to its annotated type (no error)") {
    // Without bidirectional checking this would fail: `42` would be `Int` and
    // not match `Int32`.
    CHECK(check_errors("const X: Int32 = 42\n") == 0);
    CHECK(check_errors("func f() -> Int32 { return 42 }\n") == 0);
    CHECK(check_errors("func add(_ x: Int32, _ y: Int32) -> Int32 { return x + y }\n"
                       "func main() -> Int32 { return add(1, 2) }\n")
          == 0);
}

TEST_CASE("logical operators require Bool") {
    auto r = check_detail("func bad() -> Bool {\n"
                          "    let i: Int32 = 0\n"
                          "    return i && i\n"
                          "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("require Bool operands") != std::string::npos);
}

TEST_CASE("while condition must be Bool") {
    auto r = check_detail("func bad() {\n"
                          "    var n: Int32 = 0\n"
                          "    while n {\n"
                          "        n = n + 1\n"
                          "    }\n"
                          "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("while condition must be Bool") != std::string::npos);
}

TEST_CASE("if condition must be Bool") {
    auto r = check_detail("func bad() -> Int32 {\n"
                          "    let n: Int32 = 0\n"
                          "    return if n { 1 } else { 0 }\n"
                          "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("if condition must be Bool") != std::string::npos);
}

TEST_CASE("nominal struct types resolve and pass through funcs") {
    CHECK(check_errors("struct Point { var x: Int32; var y: Int32 }\n"
                       "func id(_ p: Point) -> Point { return p }\n")
          == 0);
}

TEST_CASE("unknown type in an annotation is reported") {
    auto r = check_detail("const X: NoSuchType = 0\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("unknown type 'NoSuchType'") != std::string::npos);
}
