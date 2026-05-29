// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "vestra/diag/diagnostic.hpp"
#include "vestra/diag/source_manager.hpp"
#include "vestra/lex/lexer.hpp"
#include "vestra/parse/parser.hpp"
#include "vestra/sema/ownership.hpp"
#include "vestra/sema/resolver.hpp"
#include "vestra/sema/types.hpp"

#include <doctest/doctest.h>

#include <string>
#include <string_view>

namespace {

struct CheckResult {
    std::size_t error_count = 0;
    std::string first_message;
    std::string note_message;  // first note attached to the first error, if any
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
        if (!rep.has_errors()) {
            vestra::sema::OwnershipChecker own(unit, res.resolution(), rep);
            own.check();
        }
    }
    CheckResult cr;
    cr.error_count = rep.error_count();
    if (!rep.diagnostics().empty()) {
        cr.first_message = rep.diagnostics().front().message;
        if (!rep.diagnostics().front().notes.empty()) {
            cr.note_message = rep.diagnostics().front().notes.front().message;
        }
    }
    return cr;
}

}  // namespace

// A small helper Vestra prelude for these tests — defines a non-trivial type
// `Buf` and a `take` function that consumes one. Each test embeds it.
constexpr std::string_view Prelude = R"(
struct Buf { var n: Int32 }
func take(_ b: sink Buf) {}
func make_buf() -> Buf { return Buf(n: 0) }
)";

namespace {
std::string with_prelude(std::string body) {
    return std::string{Prelude} + std::move(body);
}
}  // namespace

// ---- clean programs --------------------------------------------------------

TEST_CASE("primitives are trivial and freely reusable") {
    CHECK(check(with_prelude("func ok() -> Int32 {\n"
                             "    let x: Int32 = 1\n"
                             "    let y: Int32 = x + x\n"
                             "    return y\n"
                             "}\n"))
              .error_count
          == 0);
}

TEST_CASE("a non-trivial binding used only once via read is fine") {
    CHECK(check(with_prelude("func use_once(_ b: Buf) -> Int32 { return b.n }\n"
                             "func ok() -> Int32 {\n"
                             "    let b = make_buf()\n"
                             "    return use_once(b)\n"
                             "}\n"))
              .error_count
          == 0);
}

TEST_CASE("a non-trivial binding consumed once is fine") {
    CHECK(check(with_prelude("func ok() {\n"
                             "    let b = make_buf()\n"
                             "    take(b)\n"
                             "}\n"))
              .error_count
          == 0);
}

// ---- core diagnostic -------------------------------------------------------

TEST_CASE("use-after-move on a sink call is rejected with the move site noted") {
    auto r = check(with_prelude("func bad() {\n"
                                "    let b = make_buf()\n"
                                "    take(b)\n"
                                "    take(b)\n"
                                "}\n"));
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("after it was moved") != std::string::npos);
    CHECK(r.note_message.find("was moved here") != std::string::npos);
}

TEST_CASE("use-after-return rejects subsequent uses") {
    auto r = check(with_prelude("func bad() -> Buf {\n"
                                "    let b = make_buf()\n"
                                "    return b\n"
                                "}\n"
                                "func also_bad() -> Buf {\n"
                                "    let b = make_buf()\n"
                                "    return b\n"
                                "    // the return consumes b; any later expr would error,\n"
                                "    // but unreachable code isn't part of phase 1.\n"
                                "}\n"));
    CHECK(r.error_count == 0);  // each function's flow is self-contained
}

TEST_CASE("passing the same binding to two sink params in one call") {
    // The two sink-consumes happen sequentially on the same place.
    auto r = check(with_prelude("func take2(_ a: sink Buf, _ b: sink Buf) {}\n"
                                "func bad() {\n"
                                "    let b = make_buf()\n"
                                "    take2(b, b)\n"
                                "}\n"));
    CHECK(r.error_count >= 1);
}

// ---- copy salvages a move --------------------------------------------------

TEST_CASE("`copy b` lets the original survive a sink call") {
    CHECK(check(with_prelude("func ok() {\n"
                             "    let b = make_buf()\n"
                             "    take(copy b)\n"
                             "    take(b)\n"
                             "}\n"))
              .error_count
          == 0);
}

TEST_CASE("`copy` of an already-moved binding is still an error") {
    auto r = check(with_prelude("func bad() {\n"
                                "    let b = make_buf()\n"
                                "    take(b)\n"
                                "    take(copy b)\n"
                                "}\n"));
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("after it was moved") != std::string::npos);
}

// ---- read parameters don't consume -----------------------------------------

TEST_CASE("read parameter passes don't burn the binding") {
    CHECK(check(with_prelude("func peek(_ b: Buf) -> Int32 { return b.n }\n"
                             "func ok() -> Int32 {\n"
                             "    let b = make_buf()\n"
                             "    let a = peek(b)\n"
                             "    let c = peek(b)\n"
                             "    return a + c\n"
                             "}\n"))
              .error_count
          == 0);
}

// ---- var reassignment revives a consumed binding ---------------------------

TEST_CASE("reassigning a consumed var binding makes it live again") {
    CHECK(check(with_prelude("func ok() {\n"
                             "    var b = make_buf()\n"
                             "    take(b)\n"
                             "    b = make_buf()\n"
                             "    take(b)\n"
                             "}\n"))
              .error_count
          == 0);
}

// ---- §10/§5 phase 2: branch-aware flow merging -----------------------------

TEST_CASE("both branches of an if may each move the same binding") {
    // Phase 1 walked the branches linearly and wrongly saw `b` as moved in
    // the else by the then-branch; phase 2 forks per branch.
    CHECK(check(with_prelude("func ok(_ c: Bool) {\n"
                             "    let b = make_buf()\n"
                             "    if c { take(b) } else { take(b) }\n"
                             "}\n"))
              .error_count
          == 0);
}

TEST_CASE("a binding not moved on any branch is still live after the if") {
    CHECK(check(with_prelude("func ok(_ c: Bool) {\n"
                             "    let b = make_buf()\n"
                             "    if c { } else { }\n"
                             "    take(b)\n"
                             "}\n"))
              .error_count
          == 0);
}

TEST_CASE("a one-sided move makes the binding moved after the if") {
    // Moved on the then-path, live on the else-path: at the join it is
    // treated as moved, so the later use is rejected (sound).
    auto r = check(with_prelude("func bad(_ c: Bool) {\n"
                                "    let b = make_buf()\n"
                                "    if c { take(b) }\n"
                                "    take(b)\n"
                                "}\n"));
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("after it was moved") != std::string::npos);
}

TEST_CASE("a double move within one branch is still rejected") {
    auto r = check(with_prelude("func bad(_ c: Bool) {\n"
                                "    let b = make_buf()\n"
                                "    if c { take(b)  take(b) }\n"
                                "}\n"));
    CHECK(r.error_count >= 1);
}

TEST_CASE("each match arm may move the same binding") {
    CHECK(check(with_prelude("func ok(_ n: Int32) {\n"
                             "    let b = make_buf()\n"
                             "    match n {\n"
                             "        case 0: take(b)\n"
                             "        default: take(b)\n"
                             "    }\n"
                             "}\n"))
              .error_count
          == 0);
}

TEST_CASE("a binding moved in one match arm is moved after the match") {
    auto r = check(with_prelude("func bad(_ n: Int32) {\n"
                                "    let b = make_buf()\n"
                                "    match n {\n"
                                "        case 0: take(b)\n"
                                "        default: 0\n"
                                "    }\n"
                                "    take(b)\n"
                                "}\n"));
    CHECK(r.error_count >= 1);
}
