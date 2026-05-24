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

// ---- Member access ----------------------------------------------------------

TEST_CASE("struct field access types correctly") {
    CHECK(check_errors("struct Point { var x: Int32; var y: Int32 }\n"
                       "func sum(_ p: Point) -> Int32 { return p.x + p.y }\n")
          == 0);
}

TEST_CASE("unknown field on a struct is reported") {
    auto r = check_detail("struct Point { var x: Int32 }\n"
                          "func bad(_ p: Point) -> Int32 { return p.zzz }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("no field or method 'zzz'") != std::string::npos);
}

TEST_CASE("embed flattens fields into the enclosing struct") {
    CHECK(check_errors("struct Inner { var v: Int32 }\n"
                       "struct Outer { embed inner: Inner; var w: Int32 }\n"
                       "func sum(_ o: Outer) -> Int32 { return o.v + o.w }\n")
          == 0);
}

// ---- Struct construction ----------------------------------------------------

TEST_CASE("struct construction with labeled fields") {
    CHECK(check_errors("struct Point { var x: Int32; var y: Int32 }\n"
                       "func make() -> Point { return Point(x: 1, y: 2) }\n")
          == 0);
}

TEST_CASE("struct construction missing a field is reported") {
    auto r = check_detail("struct Point { var x: Int32; var y: Int32 }\n"
                          "func bad() -> Point { return Point(x: 1) }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("missing field 'y'") != std::string::npos);
}

TEST_CASE("struct construction with the wrong field name is reported") {
    auto r = check_detail("struct Point { var x: Int32; var y: Int32 }\n"
                          "func bad() -> Point { return Point(x: 1, z: 2) }\n");
    CHECK(r.error_count >= 1);
}

// ---- Enum case construction -------------------------------------------------

TEST_CASE("enum case construction via Color.red") {
    CHECK(check_errors("enum Color { case red; case green; case blue }\n"
                       "func ok() -> Color { return Color.red }\n")
          == 0);
}

TEST_CASE("leading-dot enum case resolves against expected type") {
    CHECK(check_errors("enum Color { case red; case green; case blue }\n"
                       "func ok() -> Color { return .green }\n")
          == 0);
}

TEST_CASE("unknown enum case is reported") {
    auto r = check_detail("enum Color { case red; case green }\n"
                          "func bad() -> Color { return Color.purple }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("no case 'purple'") != std::string::npos);
}

TEST_CASE("leading-dot without expected enum is reported") {
    auto r = check_detail("func bad() -> Int { return .red }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("no contextual type") != std::string::npos);
}

// ---- Match ------------------------------------------------------------------

TEST_CASE("match over a bare enum is well-formed when exhaustive") {
    CHECK(check_errors("enum Color { case red; case green; case blue }\n"
                       "func channel(_ c: Color) -> Int32 {\n"
                       "    return match c { case .red: 1 case .green: 2 case .blue: 3 }\n"
                       "}\n")
          == 0);
}

TEST_CASE("non-exhaustive match over an enum is reported") {
    auto r = check_detail("enum Color { case red; case green; case blue }\n"
                          "func bad(_ c: Color) -> Int32 {\n"
                          "    return match c { case .red: 1 case .green: 2 }\n"
                          "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("not exhaustive") != std::string::npos);
}

TEST_CASE("match with a default arm avoids the exhaustiveness check") {
    CHECK(check_errors("enum Color { case red; case green; case blue }\n"
                       "func ok(_ c: Color) -> Int32 {\n"
                       "    return match c { case .red: 1 default: 0 }\n"
                       "}\n")
          == 0);
}

TEST_CASE("match arms with mismatched result types are reported") {
    auto r = check_detail("enum Color { case red; case green }\n"
                          "func bad(_ c: Color) -> Bool {\n"
                          "    return match c { case .red: true case .green: 1 }\n"
                          "}\n");
    CHECK(r.error_count >= 1);
}

// ---- §9 Optional ----------------------------------------------------------

TEST_CASE("nil is assignable to any Optional<T> slot") {
    CHECK(check_errors("func mk() -> Int32? { return nil }\n") == 0);
}

TEST_CASE("a T value is assignable to a T? slot (implicit some)") {
    CHECK(check_errors("func mk() -> Int32? { return 7 }\n") == 0);
}

TEST_CASE("?? requires an Optional left operand") {
    auto r = check_detail("func bad(_ x: Int32) -> Int32 { return x ?? 0 }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("Optional") != std::string::npos);
}

TEST_CASE("?? yields the wrapped T type") {
    CHECK(check_errors("func use(_ o: Int32?) -> Int32 { return o ?? 0 }\n") == 0);
}

TEST_CASE("postfix ! requires an Optional operand") {
    auto r = check_detail("func bad(_ x: Int32) -> Int32 { return x! }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("Optional") != std::string::npos);
}

TEST_CASE("postfix ! unwraps to the Optional's inner T") {
    CHECK(check_errors("func use(_ o: Int32?) -> Int32 { return o! }\n") == 0);
}

TEST_CASE("if let binds the unwrapped T in the then-branch") {
    CHECK(check_errors("func use(_ o: Int32?) -> Int32 {\n"
                       "    return if let v = o { v } else { 0 }\n"
                       "}\n")
          == 0);
}

TEST_CASE("if let on a non-Optional initializer is reported") {
    auto r = check_detail("func bad(_ x: Int32) -> Int32 {\n"
                          "    return if let v = x { v } else { 0 }\n"
                          "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("Optional") != std::string::npos);
}

// ---- §9 Result / throws / try / throw -------------------------------------

TEST_CASE("a throws(E) function's external return type is Result<T, E>") {
    CHECK(check_errors("enum E { case bad }\n"
                       "func f() throws(E) -> Int32 { return 7 }\n"
                       "func use() -> Int32 { return try! f() }\n")
          == 0);
}

TEST_CASE("try on a non-throwing call is reported") {
    auto r = check_detail("func g() -> Int32 { return 7 }\n"
                          "enum E { case bad }\n"
                          "func use() throws(E) -> Int32 { return try g() }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("Result") != std::string::npos);
}

TEST_CASE("try? converts a Result<T, E> to Optional<T>") {
    CHECK(check_errors("enum E { case bad }\n"
                       "func f() throws(E) -> Int32 { return 7 }\n"
                       "func use() -> Int32? { return try? f() }\n")
          == 0);
}

TEST_CASE("try! yields T") {
    CHECK(check_errors("enum E { case bad }\n"
                       "func f() throws(E) -> Int32 { return 7 }\n"
                       "func use() -> Int32 { return try! f() }\n")
          == 0);
}

TEST_CASE("try propagation requires the enclosing fn to also declare throws") {
    auto r = check_detail("enum E { case bad }\n"
                          "func f() throws(E) -> Int32 { return 7 }\n"
                          "func use() -> Int32 { return try f() }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("throws") != std::string::npos);
}

TEST_CASE("throw outside a throws function is reported") {
    auto r = check_detail("enum E { case bad }\n"
                          "func bad() -> Int32 { throw E.bad }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("throws") != std::string::npos);
}

// ---- §3 opaque type -------------------------------------------------------

TEST_CASE("opaque type constructor takes one arg of the underlying type") {
    CHECK(check_errors("opaque type UserId = Int64\n"
                       "func mk(_ x: Int64) -> UserId { return UserId(x) }\n")
          == 0);
}

TEST_CASE("opaque type constructor rejects wrong underlying type") {
    auto r = check_detail("opaque type UserId = Int64\n"
                          "func mk() -> UserId { return UserId(true) }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("UserId") != std::string::npos);
}

TEST_CASE("opaque .value extracts the underlying type") {
    CHECK(check_errors("opaque type UserId = Int64\n"
                       "func raw(_ id: UserId) -> Int64 { return id.value }\n")
          == 0);
}

TEST_CASE("opaque inherits no underlying operations") {
    auto r = check_detail("opaque type UserId = Int64\n"
                          "func bad(_ a: UserId, _ b: UserId) -> UserId { return a + b }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("numeric") != std::string::npos);
}

// ---- §4 Display conformance on interpolation splices ---------------------

TEST_CASE("interpolation of a primitive is Display-conformant") {
    CHECK(check_errors("func r(_ n: Int32) -> String { return \"n = \\(n)\" }\n") == 0);
}

TEST_CASE("interpolation of a user struct requires derive(Display) or Debug") {
    auto r = check_detail("struct Point { var x: Int32 }\n"
                          "func r(_ p: Point) -> String { return \"p = \\(p)\" }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("Display") != std::string::npos);
}

TEST_CASE("derive(Display) makes a struct interpolation-OK") {
    CHECK(check_errors("struct Point { var x: Int32 }\n"
                       "derive(Display) for Point\n"
                       "func r(_ p: Point) -> String { return \"p = \\(p)\" }\n")
          == 0);
}

TEST_CASE("derive(Debug) also makes a struct interpolation-OK") {
    CHECK(check_errors("struct Point { var x: Int32 }\n"
                       "derive(Debug) for Point\n"
                       "func r(_ p: Point) -> String { return \"p = \\(p)\" }\n")
          == 0);
}

// ---- §5 for-in over Iterator ---------------------------------------------

TEST_CASE("for-in over a Range types the loop variable as the range element") {
    CHECK(check_errors("func sum() -> Int32 {\n"
                       "    var total: Int32 = 0\n"
                       "    for i in Int32(0)..Int32(9) { total = total + i }\n"
                       "    return total\n"
                       "}\n")
          == 0);
}

TEST_CASE("for-in over a struct with next()->T? uses T as the loop variable type") {
    CHECK(check_errors("struct Counter {\n"
                       "    var n: Int32\n"
                       "    inout func next() -> Int32? { return nil }\n"
                       "}\n"
                       "func use() -> Int32 {\n"
                       "    var c = Counter(n: 0)\n"
                       "    var t: Int32 = 0\n"
                       "    for v in c { t = t + v }\n"
                       "    return t\n"
                       "}\n")
          == 0);
}

TEST_CASE("for-in over a non-iterator without next() is reported") {
    auto r = check_detail("struct S { var x: Int32 }\n"
                          "func bad() -> Int32 {\n"
                          "    var t: Int32 = 0\n"
                          "    for v in S(x: 0) { t = t + v }\n"
                          "    return t\n"
                          "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("next") != std::string::npos);
}

// ---- §12.3 derive(Clone) --------------------------------------------------

TEST_CASE("derive(Clone) surfaces a synthetic .clone() on the struct") {
    CHECK(check_errors("struct Point { var x: Int32 }\n"
                       "derive(Clone) for Point\n"
                       "func dup(_ p: Point) -> Point { return p.clone() }\n")
          == 0);
}

TEST_CASE("derive(Clone) surfaces .clone() on a payloaded enum") {
    CHECK(check_errors("enum Shape { case circle(r: Float64); case point }\n"
                       "derive(Clone) for Shape\n"
                       "func dup(_ s: Shape) -> Shape { return s.clone() }\n")
          == 0);
}

TEST_CASE("without derive(Clone), .clone() is reported as missing") {
    auto r = check_detail("struct Point { var x: Int32 }\n"
                          "func dup(_ p: Point) -> Point { return p.clone() }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("clone") != std::string::npos);
}

TEST_CASE("derive(Clone) on a bare enum does NOT surface .clone() (no method slot)") {
    auto r = check_detail("enum Color { case red; case green }\n"
                          "derive(Clone) for Color\n"
                          "func bad(_ c: Color) -> Color { return c.clone() }\n");
    CHECK(r.error_count >= 1);
}

// ---- §9 do / catch inline error handling ---------------------------------

TEST_CASE("do/catch type-checks: catch binding has the annotated error type") {
    CHECK(check_errors("enum E { case bad }\n"
                       "func f() throws(E) -> Int32 { return 7 }\n"
                       "func g() -> Int32 {\n"
                       "    return do { try f() } catch (e: E) { -1 }\n"
                       "}\n")
          == 0);
}

TEST_CASE("do/catch arms with different types are reported") {
    auto r = check_detail("enum E { case bad }\n"
                          "func f() throws(E) -> Int32 { return 7 }\n"
                          "func bad() -> Int32 {\n"
                          "    return do { try f() } catch (e: E) { true }\n"
                          "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("different types") != std::string::npos);
}

TEST_CASE("bare 'catch NAME' infers the error type from the body's try") {
    CHECK(check_errors("enum E { case bad }\n"
                       "func f() throws(E) -> Int32 { return 7 }\n"
                       "func g() -> Int32 {\n"
                       "    return do { try f() } catch e { -1 }\n"
                       "}\n")
          == 0);
}

TEST_CASE("bare 'catch NAME' with no try in the body is reported") {
    auto r = check_detail("func g() -> Int32 {\n"
                          "    return do { 7 } catch e { -1 }\n"
                          "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("at least one") != std::string::npos);
}

TEST_CASE("bare 'catch NAME' with mismatched try error types is reported") {
    auto r = check_detail("enum E1 { case bad }\n"
                          "enum E2 { case bad }\n"
                          "func f() throws(E1) -> Int32 { return 7 }\n"
                          "func g() throws(E2) -> Int32 { return 7 }\n"
                          "func bad() -> Int32 {\n"
                          "    return do { (try f()) + (try g()) } catch e { 0 }\n"
                          "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("mismatched") != std::string::npos);
}

TEST_CASE("do-body's try uses the annotated error type as its throws context") {
    // `f` throws E; the do-block's `catch (e: E)` matches, so `try f()`
    // is OK even though the enclosing function does not throw.
    CHECK(check_errors("enum E { case bad }\n"
                       "func f() throws(E) -> Int32 { return 7 }\n"
                       "func g() -> Int32 {\n"
                       "    return do { try f() } catch (e: E) { 0 }\n"
                       "}\n")
          == 0);
}

// ---- §9 Optional chaining (`?.`) ------------------------------------------

TEST_CASE("?. requires an Optional base") {
    auto r = check_detail("struct S { var x: Int32 }\n"
                          "func bad(_ s: S) -> Int32? { return s?.x }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("Optional") != std::string::npos);
}

TEST_CASE("?. on an Optional struct wraps the field type") {
    CHECK(check_errors("struct S { var x: Int32 }\n"
                       "func use(_ s: S?) -> Int32? { return s?.x }\n")
          == 0);
}

TEST_CASE("?. flattens an already-Optional field (no nested Optional)") {
    // `u?.profile` is Optional<Profile> (not Optional<Optional<Profile>>);
    // then `?.name` is Optional<Str>. Chain types as Str?.
    CHECK(check_errors("struct Profile { var name: Str }\n"
                       "struct User { var profile: Profile? }\n"
                       "func get_name(_ u: User?) -> Str? { return u?.profile?.name }\n")
          == 0);
}

TEST_CASE("throw inside a throws(E) fn requires the value type to match E") {
    auto r = check_detail("enum E { case bad }\n"
                          "func ok() throws(E) -> Int32 { throw E.bad }\n"
                          "func bad() throws(E) -> Int32 { throw 42 }\n");
    CHECK(r.error_count >= 1);
}
