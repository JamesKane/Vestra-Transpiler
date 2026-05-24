// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "vestra/diag/diagnostic.hpp"
#include "vestra/diag/source_manager.hpp"
#include "vestra/lex/lexer.hpp"
#include "vestra/parse/parser.hpp"
#include "vestra/sema/capability.hpp"
#include "vestra/sema/exclusivity.hpp"
#include "vestra/sema/ownership.hpp"
#include "vestra/sema/resolver.hpp"
#include "vestra/sema/types.hpp"

#include <doctest/doctest.h>

#include <string>

namespace {

struct CheckResult {
    std::size_t error_count = 0;
    std::string first_message;
};

// Full pipeline: lex → parse → resolver → ownership → exclusivity →
// capability. Each layer must be clean before we run the next, otherwise
// noise from one obscures the layer under test.
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
        if (!rep.has_errors()) {
            vestra::sema::OwnershipChecker own(unit, res.resolution(), rep);
            own.check();
            vestra::sema::ExclusivityChecker excl(unit, res.resolution(), rep);
            excl.check();
            vestra::sema::CapabilityChecker cap(unit, res.resolution(), rep);
            cap.check();
        }
    }
    CheckResult cr;
    cr.error_count = rep.error_count();
    if (!rep.diagnostics().empty()) {
        cr.first_message = rep.diagnostics().front().message;
    }
    return cr;
}

}  // namespace

// ---- clean propagation -----------------------------------------------------

TEST_CASE("a function with `using Alloc` can call one that also declares it") {
    CHECK(check("func leaf() using Alloc {}\n"
                "func mid() using Alloc { leaf() }\n")
              .error_count
          == 0);
}

TEST_CASE("calling a non-using function from a using one is fine") {
    CHECK(check("func leaf() {}\n"
                "func mid() using Alloc { leaf() }\n")
              .error_count
          == 0);
}

// ---- missing capability at call site ---------------------------------------

TEST_CASE("calling `using Alloc` from a function without it is rejected") {
    auto r = check("func leaf() using Alloc {}\n"
                   "func bad() { leaf() }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("missing capability 'Alloc'") != std::string::npos);
}

TEST_CASE("calling a multi-cap function flags each missing one") {
    auto r = check("func leaf() using Alloc, Log {}\n"
                   "func bad() { leaf() }\n");
    // The first error should be one of {Alloc, Log}; both should be reported.
    CHECK(r.error_count >= 2);
}

// ---- `with` blocks satisfy a capability ------------------------------------

TEST_CASE("a `with` block supplies the capability for its body") {
    CHECK(check("func leaf() using Alloc {}\n"
                "func make_alloc() -> Int32 { return 0 }\n"
                "func ok() {\n"
                "    with Alloc = make_alloc() {\n"
                "        leaf()\n"
                "    }\n"
                "}\n")
              .error_count
          == 0);
}

TEST_CASE("marker `with Asm { ... }` (no value) is fine") {
    CHECK(check("func raw() using Asm {}\n"
                "func ok() {\n"
                "    with Asm {\n"
                "        raw()\n"
                "    }\n"
                "}\n")
              .error_count
          == 0);
}

TEST_CASE("a `with` block does not leak outside its body") {
    auto r = check("func leaf() using Alloc {}\n"
                   "func bad() {\n"
                   "    with Alloc = 0 {\n"
                   "        leaf()\n"
                   "    }\n"
                   "    leaf()\n"
                   "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("missing capability 'Alloc'") != std::string::npos);
}

// ---- async + await + spawn -------------------------------------------------

TEST_CASE("an async function may await") {
    CHECK(check("async func leaf() -> Int32 { return 1 }\n"
                "async func mid() -> Int32 { return await leaf() }\n")
              .error_count
          == 0);
}

TEST_CASE("await outside an async context is rejected") {
    auto r = check("async func leaf() -> Int32 { return 1 }\n"
                   "func bad() -> Int32 { return await leaf() }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("missing capability 'Async'") != std::string::npos);
}

TEST_CASE("spawn outside an async context is rejected") {
    auto r = check("async func leaf() -> Int32 { return 1 }\n"
                   "func bad() {\n"
                   "    spawn leaf()\n"
                   "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("missing capability 'Async'") != std::string::npos);
}

TEST_CASE("non-async caller of an async function is rejected") {
    // Calling an async function (which has `using Async`) requires Async
    // to be in scope at the call site, even without await.
    auto r = check("async func leaf() {}\n"
                   "func bad() { leaf() }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("missing capability 'Async'") != std::string::npos);
}

// ---- §10 Box.new requires Alloc -------------------------------------------

TEST_CASE("Box.new in a `using Alloc` fn is clean") {
    CHECK(check("func mk(_ x: Int32) using Alloc -> Box[Int32] { return Box.new(x) }\n").error_count
          == 0);
}

TEST_CASE("Box.new without `using Alloc` is reported by the capability checker") {
    auto r = check("func bad(_ x: Int32) -> Box[Int32] { return Box.new(x) }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("missing capability 'Alloc'") != std::string::npos);
}

// ---- existing examples remain clean ----------------------------------------

TEST_CASE("the shapes example checks clean under capabilities") {
    CHECK(check("module examples.shapes\n"
                "struct Point { var x: Int32; var y: Int32 }\n"
                "enum Color { case red; case green; case blue }\n"
                "func channel(_ c: Color) -> Int32 {\n"
                "    return match c { case .red: 1 case .green: 2 case .blue: 3 }\n"
                "}\n"
                "func sum_xy(_ p: Point) -> Int32 { return p.x + p.y }\n"
                "func compute() -> Int32 {\n"
                "    let p = Point(x: 10, y: 32)\n"
                "    let total: Int32 = sum_xy(p) + channel(Color.green)\n"
                "    return total\n"
                "}\n")
              .error_count
          == 0);
}
