#include "vestra/diag/diagnostic.hpp"
#include "vestra/diag/source_manager.hpp"
#include "vestra/lex/lexer.hpp"
#include "vestra/parse/parser.hpp"
#include "vestra/sema/resolver.hpp"
#include "vestra/sema/types.hpp"

#include <doctest/doctest.h>

#include <string>

namespace {

struct CheckResult {
    std::size_t error_count = 0;
    std::string first_message;
};

CheckResult check(std::string source) {
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

// ---- Clean inference -------------------------------------------------------

TEST_CASE("identity function infers T from a single argument") {
    CHECK(check("func id[T](_ x: T) -> T { return x }\n"
                "func ok() -> Int32 { return id(42) }\n")
              .error_count
          == 0);
}

TEST_CASE("generic over two same-typed args binds T once") {
    CHECK(check("func max_of[T](_ a: T, _ b: T) -> T { return if a > b { a } else { b } }\n"
                "func ok() -> Int32 {\n"
                "    let a: Int32 = 1\n"
                "    let b: Int32 = 2\n"
                "    return max_of(a, b)\n"
                "}\n")
              .error_count
          == 0);
}

// ---- Outer-expected propagation --------------------------------------------

TEST_CASE("expected-type propagation: int literal adopts annotated Int32 through identity") {
    // Without `expected` propagation into call-site inference, `id(7)` would
    // resolve T to `Int` (the literal default) and conflict with Int32.
    CHECK(check("func id[T](_ x: T) -> T { return x }\n"
                "func ok() -> Int32 { return id(7) }\n")
              .error_count
          == 0);
}

TEST_CASE("expected-type propagation: max_of(typed, literal) binds both at the typed side") {
    // The literal `35` would default to Int and conflict with the Int32
    // `i` unless we substitute the bound-so-far T into the second arg's
    // expected type during pass 1.
    CHECK(check("func max_of[T](_ a: T, _ b: T) -> T { return if a > b { a } else { b } }\n"
                "func ok() -> Int32 {\n"
                "    let i: Int32 = 1\n"
                "    return max_of(i, 35)\n"
                "}\n")
              .error_count
          == 0);
}

// ---- Conflicting bindings --------------------------------------------------

TEST_CASE("conflicting bindings for a single generic are reported") {
    auto r = check("func same[T](_ a: T, _ b: T) -> T { return a }\n"
                   "func bad() -> Bool {\n"
                   "    let i: Int32 = 0\n"
                   "    let f: Float64 = 0.0\n"
                   "    return same(i, f) == same(i, f)\n"
                   "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("conflicting bindings for generic 'T'") != std::string::npos);
}

// ---- Unbound generics ------------------------------------------------------
//
// Note: in our minimal language, every call expression has *some* expected
// type (functions returning Unit propagate Unit, statement-position calls
// inside a Unit-returning function still see Unit), so the "no context at
// all" case is hard to construct without features we haven't built yet.
// The unbound-generic diagnostic logic is wired up; it would fire if e.g.
// we had a wildcard `_ = make()` form, which the parser does not yet
// accept. Revisit once that lands.

// ---- Existing examples still pass ------------------------------------------

TEST_CASE("the generics example checks clean") {
    CHECK(check("module examples.generics\n"
                "func identity[T](_ x: T) -> T { return x }\n"
                "func max_of[T](_ a: T, _ b: T) -> T {\n"
                "    return if a > b { a } else { b }\n"
                "}\n"
                "func compute() -> Int32 {\n"
                "    let i: Int32 = identity(7)\n"
                "    let m: Int32 = max_of(i, 35)\n"
                "    return i + m\n"
                "}\n")
              .error_count
          == 0);
}
