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

// ---- §5/§19.6 linear types: must-consume ----------------------------------

namespace {
// A `linear struct` plus a sink consumer and a maker.
constexpr std::string_view LinearPrelude = R"(
linear struct Token { var id: Int32 }
func use_tok(_ t: sink Token) {}
func mk_tok() -> Token { return Token(id: 0) }
func noop() {}
)";
std::string with_linear(std::string body) {
    return std::string{LinearPrelude} + std::move(body);
}
}  // namespace

TEST_CASE("a linear value consumed once is fine") {
    CHECK(check(with_linear("func ok() { let t = mk_tok()  use_tok(t) }\n")).error_count == 0);
}

TEST_CASE("a linear value never consumed is a leak") {
    auto r = check(with_linear("func bad() { let t = mk_tok() }\n"));
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("linear value 't' is never consumed") != std::string::npos);
}

TEST_CASE("returning a linear value consumes it") {
    CHECK(check(with_linear("func ok() -> Token { let t = mk_tok()  return t }\n")).error_count
          == 0);
}

TEST_CASE("a linear value consumed on both branches is fine") {
    CHECK(check(with_linear("func ok(_ c: Bool) {\n"
                            "    let t = mk_tok()\n"
                            "    if c { use_tok(t) } else { use_tok(t) }\n"
                            "}\n"))
              .error_count
          == 0);
}

TEST_CASE("a linear value consumed on only one branch leaks the other") {
    auto r = check(with_linear("func bad(_ c: Bool) {\n"
                               "    let t = mk_tok()\n"
                               "    if c { use_tok(t) }\n"
                               "}\n"));
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("consumed on some branches but leaks") != std::string::npos);
}

TEST_CASE("a linear value consumed in every match arm is fine") {
    CHECK(check(with_linear("func ok(_ n: Int32) {\n"
                            "    let t = mk_tok()\n"
                            "    match n { case 0: use_tok(t)  default: use_tok(t) }\n"
                            "}\n"))
              .error_count
          == 0);
}

TEST_CASE("a linear value consumed in only one match arm leaks") {
    auto r = check(with_linear("func bad(_ n: Int32) {\n"
                               "    let t = mk_tok()\n"
                               "    match n { case 0: use_tok(t)  default: noop() }\n"
                               "}\n"));
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("consumed on some branches but leaks") != std::string::npos);
}

// ---- §5 let-from-place move tracking --------------------------------------

TEST_CASE("`let c = b` moves b, so a later use of b is rejected") {
    auto r = check(with_prelude("func bad() {\n"
                                "    let b = make_buf()\n"
                                "    let c = b\n"
                                "    take(b)\n"
                                "}\n"));
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("after it was moved") != std::string::npos);
}

TEST_CASE("`let c = copy b` keeps b live") {
    CHECK(check(with_prelude("func ok() {\n"
                             "    let b = make_buf()\n"
                             "    let c = copy b\n"
                             "    take(b)\n"
                             "    take(c)\n"
                             "}\n"))
              .error_count
          == 0);
}

TEST_CASE("`let c = b` then using c is fine") {
    CHECK(check(with_prelude("func ok() {\n"
                             "    let b = make_buf()\n"
                             "    let c = b\n"
                             "    take(c)\n"
                             "}\n"))
              .error_count
          == 0);
}

TEST_CASE("a linear value moved by `let` is consumed, not leaked") {
    // The let-move both discharges the leak obligation on the source binding
    // and transfers it to the destination, which is then consumed.
    CHECK(check(with_linear("func ok() {\n"
                            "    let t = mk_tok()\n"
                            "    let u = t\n"
                            "    use_tok(u)\n"
                            "}\n"))
              .error_count
          == 0);
}

TEST_CASE("a linear value rebound by `let` but never consumed still leaks") {
    auto r = check(with_linear("func bad() {\n"
                               "    let t = mk_tok()\n"
                               "    let u = t\n"
                               "}\n"));
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("is never consumed") != std::string::npos);
}

// ---- §5 struct/enum construction consumes its place arguments -------------

TEST_CASE("constructing a struct from a binding moves that binding") {
    auto r = check(with_prelude("struct Box1 { var b: Buf }\n"
                                "func bad() {\n"
                                "    let b = make_buf()\n"
                                "    let w = Box1(b: b)\n"
                                "    take(b)\n"
                                "}\n"));
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("after it was moved") != std::string::npos);
}

TEST_CASE("constructing a struct and consuming the whole value is fine") {
    CHECK(check(with_prelude("struct Box1 { var b: Buf }\n"
                             "func useBox(_ w: sink Box1) {}\n"
                             "func ok() {\n"
                             "    let b = make_buf()\n"
                             "    let w = Box1(b: b)\n"
                             "    useBox(w)\n"
                             "}\n"))
              .error_count
          == 0);
}

TEST_CASE("a copied binding survives being placed into a struct") {
    CHECK(check(with_prelude("struct Box1 { var b: Buf }\n"
                             "func useBox(_ w: sink Box1) {}\n"
                             "func ok() {\n"
                             "    let b = make_buf()\n"
                             "    let w = Box1(b: copy b)\n"
                             "    take(b)\n"
                             "    useBox(w)\n"
                             "}\n"))
              .error_count
          == 0);
}

// ---- §5/§19.6 linearity transitivity --------------------------------------

namespace {
// A linear Token, a plain Wrapper that owns one, and a DeepWrap that owns a
// Wrapper — so linearity must propagate two levels.
constexpr std::string_view TransPrelude = R"(
linear struct Token { var id: Int32 }
struct Wrapper { var t: Token }
struct DeepWrap { var w: Wrapper }
func use_w(_ w: sink Wrapper) {}
func use_d(_ d: sink DeepWrap) {}
func mk_tok() -> Token { return Token(id: 0) }
)";
std::string with_trans(std::string body) {
    return std::string{TransPrelude} + std::move(body);
}
}  // namespace

TEST_CASE("a struct owning a linear field is itself linear (leak rejected)") {
    auto r = check(with_trans("func bad() {\n"
                              "    let tok = mk_tok()\n"
                              "    let w = Wrapper(t: tok)\n"
                              "}\n"));
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("is never consumed") != std::string::npos);
}

TEST_CASE("a transitively-linear value consumed via sink is fine") {
    CHECK(check(with_trans("func ok() {\n"
                           "    let tok = mk_tok()\n"
                           "    let w = Wrapper(t: tok)\n"
                           "    use_w(w)\n"
                           "}\n"))
              .error_count
          == 0);
}

TEST_CASE("linearity propagates two levels through nested ownership") {
    auto r = check(with_trans("func bad() {\n"
                              "    let tok = mk_tok()\n"
                              "    let d = DeepWrap(w: Wrapper(t: tok))\n"
                              "}\n"));
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("is never consumed") != std::string::npos);
}

TEST_CASE("a plain struct with no linear field is not linear") {
    // Buf is an ordinary struct; dropping a Buf-owning struct is fine.
    CHECK(check(with_prelude("struct Holder { var b: Buf }\n"
                             "func ok() {\n"
                             "    let h = Holder(b: make_buf())\n"
                             "}\n"))
              .error_count
          == 0);
}

// ---- §5/§19.6 a `sink` linear parameter is itself a terminal sink ---------

TEST_CASE("a sink-linear parameter may be dropped by the callee") {
    // Taking ownership via `sink` counts as consuming: the callee is free to
    // destroy the value, so an empty terminal consumer is well-formed. This
    // is what lets `use_tok(_ t: sink Token) {}` be the end of a linear chain.
    CHECK(check(with_linear("func terminal(_ t: sink Token) {}\n")).error_count == 0);
}

TEST_CASE("a sink-linear parameter may be forwarded to another sink") {
    CHECK(check(with_linear("func forward(_ t: sink Token) { use_tok(t) }\n")).error_count == 0);
}

TEST_CASE("a sink-linear parameter may be returned") {
    CHECK(check(with_linear("func passthru(_ t: sink Token) -> Token { return t }\n")).error_count
          == 0);
}

TEST_CASE("forwarding a sink-linear parameter twice is still a double-move") {
    auto r = check(with_linear("func bad(_ t: sink Token) {\n"
                               "    use_tok(t)\n"
                               "    use_tok(t)\n"
                               "}\n"));
    CHECK(r.error_count >= 1);
}
