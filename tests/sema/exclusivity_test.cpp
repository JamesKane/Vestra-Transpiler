// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#include "vestra/diag/diagnostic.hpp"
#include "vestra/diag/source_manager.hpp"
#include "vestra/lex/lexer.hpp"
#include "vestra/parse/parser.hpp"
#include "vestra/sema/exclusivity.hpp"
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
    std::string first_note;
};

// Full pipeline: lex → parse → resolver → ownership → exclusivity.
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
        }
    }
    CheckResult cr;
    cr.error_count = rep.error_count();
    if (!rep.diagnostics().empty()) {
        cr.first_message = rep.diagnostics().front().message;
        if (!rep.diagnostics().front().notes.empty()) {
            cr.first_note = rep.diagnostics().front().notes.front().message;
        }
    }
    return cr;
}

constexpr std::string_view Prelude = R"(
struct Cell { var v: Int32 }
struct Pair { var a: Int32; var b: Int32 }
func swap_cells(_ a: inout Cell, _ b: inout Cell) {}
func swap_ints(_ a: inout Int32, _ b: inout Int32) {}
func update_and_peek(_ a: inout Cell, _ b: Cell) -> Int32 { return b.v }
func bump(_ a: inout Cell) {}
func read_cell(_ a: Cell) -> Int32 { return a.v }
func read_pair(_ p: Pair, _ q: Pair) {}
)";

std::string with_prelude(std::string body) {
    return std::string{Prelude} + std::move(body);
}

}  // namespace

// ---- positive cases --------------------------------------------------------

TEST_CASE("two reads on the same binding are fine") {
    CHECK(check(with_prelude("func ok(_ c: Cell) -> Int32 {\n"
                             "    return read_cell(c) + read_cell(c)\n"
                             "}\n"))
              .error_count
          == 0);
}

TEST_CASE("inout on two distinct bindings is fine") {
    CHECK(check(with_prelude("func ok() {\n"
                             "    var a = Cell(v: 1)\n"
                             "    var b = Cell(v: 2)\n"
                             "    swap_cells(&a, &b)\n"
                             "}\n"))
              .error_count
          == 0);
}

TEST_CASE("inout on two distinct struct fields is fine (clause 3)") {
    CHECK(check(with_prelude("func ok() {\n"
                             "    var p = Pair(a: 1, b: 2)\n"
                             "    swap_ints(&p.a, &p.b)\n"
                             "}\n"))
              .error_count
          == 0);
}

TEST_CASE("reading two distinct pairs is fine") {
    CHECK(check(with_prelude("func ok() {\n"
                             "    let p = Pair(a: 1, b: 2)\n"
                             "    let q = Pair(a: 3, b: 4)\n"
                             "    read_pair(p, q)\n"
                             "}\n"))
              .error_count
          == 0);
}

// ---- conflicts: two inouts -------------------------------------------------

TEST_CASE("inout + inout on the same binding is rejected") {
    auto r = check(with_prelude("func bad() {\n"
                                "    var c = Cell(v: 1)\n"
                                "    swap_cells(&c, &c)\n"
                                "}\n"));
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("'c'") != std::string::npos);
    CHECK(r.first_message.find("incompatible") != std::string::npos);
    CHECK(r.first_note.find("first borrow") != std::string::npos);
}

TEST_CASE("inout + inout on the same struct field is rejected") {
    auto r = check(with_prelude("func bad() {\n"
                                "    var p = Pair(a: 1, b: 2)\n"
                                "    swap_ints(&p.a, &p.a)\n"
                                "}\n"));
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("p.a") != std::string::npos);
}

// ---- conflicts: read + inout on overlapping place --------------------------

TEST_CASE("inout + read on the same binding is rejected") {
    auto r = check(with_prelude("func bad() {\n"
                                "    var c = Cell(v: 1)\n"
                                "    let n = update_and_peek(&c, c)\n"
                                "}\n"));
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("'c'") != std::string::npos);
}

// ---- conflicts: parent vs child borrow -------------------------------------

TEST_CASE("inout on parent and read on child overlap (parent contains child)") {
    auto r = check(with_prelude(
        "func read_int(_ x: Int32) {}\n"
        "func bad() {\n"
        "    var p = Pair(a: 1, b: 2)\n"
        "    bump(&p as? Cell as Cell)\n"  // we don't have a direct way to inout-the-whole-pair;
                                           // a simpler form below
        "}\n"));
    // This test deliberately exercises the parent-vs-child branch — but the
    // call above uses casts we don't fully support. Replace with a direct
    // case where the parent itself is inout-borrowed.
    (void)r;

    // Direct, supported form: a function that takes a whole Pair inout, while
    // another arg reads one of its fields.
    auto r2 = check(with_prelude("func touch_pair(_ p: inout Pair, _ x: Int32) {}\n"
                                 "func bad2() {\n"
                                 "    var p = Pair(a: 1, b: 2)\n"
                                 "    touch_pair(&p, p.a)\n"
                                 "}\n"));
    CHECK(r2.error_count >= 1);
    CHECK(r2.first_message.find("overlap") != std::string::npos);
}

// ---- sink arguments don't count as borrows ---------------------------------

TEST_CASE("sink arg + inout arg on the same binding does not trigger exclusivity") {
    // The sink consumes the binding; ownership catches the resulting move
    // problem if any. Exclusivity (this checker) should stay quiet.
    auto r = check(with_prelude("func take_and_bump(_ a: sink Cell, _ b: inout Cell) {}\n"
                                "func tricky() {\n"
                                "    var a = Cell(v: 1)\n"
                                "    var b = Cell(v: 2)\n"
                                "    take_and_bump(a, &b)\n"
                                "}\n"));
    CHECK(r.error_count == 0);
}

// ---- existing examples remain clean ----------------------------------------

TEST_CASE("the ownership example checks clean under exclusivity too") {
    CHECK(check("module examples.ownership\n"
                "struct Buf { var n: Int32 }\n"
                "func make_buf(_ n: Int32) -> Buf { return Buf(n: n) }\n"
                "func consume(_ b: sink Buf) -> Int32 { return b.n }\n"
                "func peek(_ b: Buf) -> Int32 { return b.n }\n"
                "func compute() -> Int32 {\n"
                "    let b = make_buf(7)\n"
                "    let first = peek(b)\n"
                "    let second = peek(b)\n"
                "    return first + second + consume(b)\n"
                "}\n")
              .error_count
          == 0);
}

// ---- §5/§18.4 partition provenance -----------------------------------------

namespace {
// A MutSpan two-inout consumer plus a sink consumer, for partition tests.
constexpr std::string_view SpanPrelude = R"(
func mut_two(_ a: inout MutSpan[Int32], _ b: inout MutSpan[Int32]) {}
func mut_one(_ a: inout MutSpan[Int32]) {}
)";
std::string with_span(std::string body) {
    return std::string{SpanPrelude} + std::move(body);
}
}  // namespace

TEST_CASE("inout borrow of a split half conflicts with inout borrow of the parent") {
    auto r = check(with_span("func bad(_ s: MutSpan[Int32]) {\n"
                             "    var sv = s\n"
                             "    let (lo, hi) = sv.split(at: 2)\n"
                             "    var lov = lo\n"
                             "    mut_two(&sv, &lov)\n"
                             "}\n"));
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("sub-view of") != std::string::npos);
}

TEST_CASE("the two halves of a split are disjoint and borrow independently") {
    CHECK(check(with_span("func ok(_ s: MutSpan[Int32]) {\n"
                          "    let (lo, hi) = s.split(at: 2)\n"
                          "    var lov = lo\n"
                          "    var hiv = hi\n"
                          "    mut_two(&lov, &hiv)\n"
                          "}\n"))
              .error_count
          == 0);
}

TEST_CASE("an alias of a split half inherits its parent provenance") {
    auto r = check(with_span("func bad(_ s: MutSpan[Int32]) {\n"
                             "    var sv = s\n"
                             "    let (lo, hi) = sv.split(at: 2)\n"
                             "    var alias = lo\n"
                             "    mut_two(&sv, &alias)\n"
                             "}\n"));
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("sub-view of") != std::string::npos);
}

// ---- §11 no borrow across await -------------------------------------------

TEST_CASE("an async func with an inout parameter is rejected") {
    // A mutable borrow can't survive the coroutine's suspension; codegen makes
    // read params safe by copying, but inout must stay a write-back reference.
    auto r = check(with_prelude("async func bad(_ c: inout Cell) {\n"
                                "    c.v = c.v + 1\n"
                                "}\n"));
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("no borrow across await") != std::string::npos);
}

TEST_CASE("an async func with a read parameter is fine") {
    // Read params are copied into the coroutine frame, so they survive
    // suspension — no borrow is held across the await.
    CHECK(check(with_prelude("async func ok(_ c: Cell) -> Int32 {\n"
                             "    return read_cell(c)\n"
                             "}\n"))
              .error_count
          == 0);
}

// ---- §18.5 mutating collection methods borrow their receiver inout ---------

TEST_CASE("a mutating Vec method aliasing its argument is an exclusivity error") {
    // `n.kids.push(n)` borrows the receiver `n.kids` inout while reading `n` as
    // the pushed argument; `n` contains `n.kids`, so the two overlap.
    auto r = check("struct Node { var kids: Vec[Node]\n var id: Int32 }\n"
                   "func grow(_ n: inout Node) using Alloc {\n"
                   "    n.kids.push(n)\n"
                   "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("overlap") != std::string::npos);
}

TEST_CASE("a mutating Vec method with a disjoint argument is clean") {
    // Distinct roots (`n.kids` receiver, `child` argument) — no overlap.
    CHECK(check("struct Node { var kids: Vec[Node]\n var id: Int32 }\n"
                "func grow(_ n: inout Node, _ child: Node) using Alloc {\n"
                "    n.kids.push(child)\n"
                "}\n")
              .error_count
          == 0);
}

TEST_CASE("a non-mutating Vec method does not borrow its receiver inout") {
    // `len()` reads; pushing the count read from the same Vec is fine because
    // the receiver borrow is read, not inout (so two reads, no conflict).
    CHECK(check("struct Holder { var xs: Vec[Int32] }\n"
                "func peek(_ h: Holder) -> Int { return h.xs.len() }\n")
              .error_count
          == 0);
}
