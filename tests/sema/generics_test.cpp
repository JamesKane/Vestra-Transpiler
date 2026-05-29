// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

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

// ---- §7 generics phase 2: user-defined generic structs ---------------------

TEST_CASE("generic struct field access substitutes the type argument") {
    // `p.first` / `p.second` resolve to Int32 (T bound to Int32 by the
    // Pair[Int32] parameter), so the addition type-checks.
    CHECK(check("struct Pair[T] { var first: T  var second: T }\n"
                "func sum(_ p: Pair[Int32]) -> Int32 { return p.first + p.second }\n")
              .error_count
          == 0);
}

TEST_CASE("generic struct construction infers the type argument from arguments") {
    CHECK(check("struct Pair[T] { var first: T  var second: T }\n"
                "func make(_ a: Int32, _ b: Int32) -> Pair[Int32] {\n"
                "    return Pair(first: a, second: b)\n"
                "}\n")
              .error_count
          == 0);
}

TEST_CASE("generic struct construction is seeded by the annotated binding type") {
    // The integer literals adopt Int32 from the annotated `Pair[Int32]`;
    // without expected-type seeding they would default to Int and the
    // field assignment would still pass (Int->Int32 natural-width), so the
    // real check is that the binding's element type is honored end-to-end.
    CHECK(check("struct Pair[T] { var first: T  var second: T }\n"
                "func use() -> Int32 {\n"
                "    let p: Pair[Int32] = Pair(first: 10, second: 20)\n"
                "    return p.first + p.second\n"
                "}\n")
              .error_count
          == 0);
}

TEST_CASE("two-parameter generic struct resolves each field independently") {
    CHECK(check("struct KeyValue[K, V] { var key: K  var value: V }\n"
                "func k(_ kv: KeyValue[Int32, Bool]) -> Int32 { return kv.key }\n"
                "func v(_ kv: KeyValue[Int32, Bool]) -> Bool { return kv.value }\n")
              .error_count
          == 0);
}

TEST_CASE("generic struct with the wrong type-argument arity is reported") {
    auto r = check("struct Pair[T] { var first: T  var second: T }\n"
                   "func f(_ p: Pair[Int32, Bool]) -> Int32 { return p.first }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("expects 1 generic argument") != std::string::npos);
}

TEST_CASE("a generic struct used without type arguments is reported") {
    auto r = check("struct Pair[T] { var first: T  var second: T }\n"
                   "func f(_ p: Pair) -> Int32 { return 0 }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("requires 1 generic argument") != std::string::npos);
}

TEST_CASE("a non-generic struct given type arguments is reported") {
    auto r = check("struct Point { var x: Int32 }\n"
                   "func f(_ p: Point[Int32]) -> Int32 { return p.x }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("is not a generic type") != std::string::npos);
}

TEST_CASE("conflicting type-argument inference in construction is reported") {
    auto r = check("struct Pair[T] { var first: T  var second: T }\n"
                   "func bad() -> Int32 {\n"
                   "    let p = Pair(first: true, second: 5)\n"
                   "    return 0\n"
                   "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("conflicting bindings for generic 'T'") != std::string::npos);
}

// ---- §7 generics phase 2: user-defined generic enums -----------------------

TEST_CASE("generic enum match binds the payload at the instance argument type") {
    // `.just(let x)` over a `Maybe[Int32]` binds x as Int32, so returning x
    // from an Int32 function type-checks.
    CHECK(check("enum Maybe[T] { case just(T)  case nothing }\n"
                "func unwrap(_ m: Maybe[Int32], _ d: Int32) -> Int32 {\n"
                "    return match m {\n"
                "        case .just(let x): x\n"
                "        case .nothing:     d\n"
                "    }\n"
                "}\n")
              .error_count
          == 0);
}

TEST_CASE("generic enum payloaded case construction infers the type argument") {
    CHECK(check("enum Maybe[T] { case just(T)  case nothing }\n"
                "func mk(_ v: Int32) -> Maybe[Int32] { return Maybe.just(v) }\n")
              .error_count
          == 0);
}

TEST_CASE("generic enum no-payload case resolves against the expected instance") {
    CHECK(check("enum Maybe[T] { case just(T)  case nothing }\n"
                "func none() -> Maybe[Int32] { return Maybe.nothing }\n")
              .error_count
          == 0);
}

TEST_CASE("two-parameter generic enum matches both arms") {
    CHECK(check("enum Either[L, R] { case left(L)  case right(R) }\n"
                "func pick(_ e: Either[Int32, Bool], _ d: Int32) -> Int32 {\n"
                "    return match e {\n"
                "        case .left(let n):  n\n"
                "        case .right(let b): if b { d } else { 0 }\n"
                "    }\n"
                "}\n")
              .error_count
          == 0);
}

TEST_CASE("generic enum with the wrong type-argument arity is reported") {
    auto r = check("enum Maybe[T] { case just(T)  case nothing }\n"
                   "func f(_ m: Maybe[Int32, Bool]) -> Int32 { return 0 }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("expects 1 generic argument") != std::string::npos);
}

TEST_CASE("a generic enum used without type arguments is reported") {
    auto r = check("enum Maybe[T] { case just(T)  case nothing }\n"
                   "func f(_ m: Maybe) -> Int32 { return 0 }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("requires 1 generic argument") != std::string::npos);
}

TEST_CASE("a generic enum no-payload case with no context is reported") {
    auto r = check("enum Maybe[T] { case just(T)  case nothing }\n"
                   "func f() -> Int32 {\n"
                   "    let x = Maybe.nothing\n"
                   "    return 0\n"
                   "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("cannot infer type argument") != std::string::npos);
}

TEST_CASE("conflicting type-argument inference across an enum payload is reported") {
    auto r = check("enum Pairish[T] { case two(T, T) }\n"
                   "func f() -> Int32 {\n"
                   "    let p = Pairish.two(true, 5)\n"
                   "    return 0\n"
                   "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("conflicting bindings for generic 'T'") != std::string::npos);
}

// ---- §7 generics phase 2: const generics -----------------------------------

TEST_CASE("const generic field access resolves [N]T at the bound length") {
    // `b.data[0]` over a Buffer[Int32, 4] resolves to Int32 (the [N]T field
    // concretizes to [4]Int32), so returning it from Int32 type-checks.
    CHECK(check("struct Buffer[T, const N: Int] { var data: [N]T }\n"
                "func first(_ b: Buffer[Int32, 4]) -> Int32 { return b.data[0] }\n")
              .error_count
          == 0);
}

TEST_CASE("const generic construction infers both the type and const args") {
    CHECK(check("struct Buffer[T, const N: Int] { var data: [N]T }\n"
                "func make() -> Buffer[Int32, 4] {\n"
                "    return Buffer(data: [10, 20, 30, 40])\n"
                "}\n")
              .error_count
          == 0);
}

TEST_CASE("const generic arity counts type and const arguments together") {
    auto r = check("struct Buffer[T, const N: Int] { var data: [N]T }\n"
                   "func f(_ b: Buffer[Int32]) -> Int32 { return 0 }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("expects 2 generic argument") != std::string::npos);
}

TEST_CASE("conflicting const-generic inference is reported") {
    auto r = check("struct Two[const N: Int] { var a: [N]Int32  var b: [N]Int32 }\n"
                   "func bad() -> Int32 {\n"
                   "    let x = Two(a: [1, 2, 3], b: [1, 2])\n"
                   "    return 0\n"
                   "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("conflicting bindings for const generic 'N'") != std::string::npos);
}

// ---- §7 generics phase 2: protocol bounds (where-clauses) ------------------

TEST_CASE("inline Eq bound is satisfied by a primitive") {
    CHECK(check("func eq[T: Eq](_ a: T, _ b: T) -> Bool { return a == b }\n"
                "func u() -> Bool { return eq(1, 2) }\n")
              .error_count
          == 0);
}

TEST_CASE("inline Eq bound is violated by a struct without derive(Eq)") {
    auto r = check("struct P { var x: Int32 }\n"
                   "func eq[T: Eq](_ a: T, _ b: T) -> Bool { return true }\n"
                   "func u(_ p: P) -> Bool { return eq(p, p) }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("does not satisfy the bound 'T: Eq'") != std::string::npos);
}

TEST_CASE("where-clause Eq bound is satisfied by a struct with derive(Eq)") {
    CHECK(check("struct P { var x: Int32 }\n"
                "derive(Eq) for P\n"
                "func eq[T](_ a: T, _ b: T) -> Bool where T: Eq { return true }\n"
                "func u(_ p: P) -> Bool { return eq(p, p) }\n")
              .error_count
          == 0);
}

TEST_CASE("Comparable bound is satisfied by a primitive") {
    CHECK(check("func mx[T](_ a: T, _ b: T) -> T where T: Comparable {\n"
                "    return if a > b { a } else { b }\n"
                "}\n"
                "func u() -> Int32 { return mx(1, 2) }\n")
              .error_count
          == 0);
}

TEST_CASE("a conjunctive bound requires every protocol") {
    // P derives Eq but not Hash, so `T: Eq & Hash` is violated.
    auto r = check("struct P { var x: Int32 }\n"
                   "derive(Eq) for P\n"
                   "func f[T: Eq & Hash](_ a: T) -> Bool { return true }\n"
                   "func u(_ p: P) -> Bool { return f(p) }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("does not satisfy the bound 'T: Hash'") != std::string::npos);
}

TEST_CASE("a where clause naming an unknown parameter is reported") {
    auto r = check("func f[T](_ x: T) -> T where U: Eq { return x }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("unknown generic parameter 'U'") != std::string::npos);
}

TEST_CASE("a struct type-parameter bound is enforced at instantiation") {
    auto r = check("struct Keyed[T: Hash] { var key: T }\n"
                   "struct P { var x: Int32 }\n"
                   "func f(_ k: Keyed[P]) -> Int32 { return 0 }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("does not satisfy the bound 'T: Hash'") != std::string::npos);
}
