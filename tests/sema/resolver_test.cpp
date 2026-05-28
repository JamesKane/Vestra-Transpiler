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

TEST_CASE("try? on an Optional points the user at the Optional idioms") {
    // The diagnostic should mention `try?` specifically and suggest
    // the non-try idioms — `?.`, `if let`, `match`. The user is
    // probably reaching for `try?` by analogy with Result and just
    // needs to drop the prefix.
    auto r = check_detail("func opt() -> Int32? { return 7 }\n"
                          "func use() -> Int32? { return try? opt() }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("try?") != std::string::npos);
    CHECK(r.first_message.find("?.") != std::string::npos);
}

TEST_CASE("try on an Optional suggests ?. / match / ! instead of propagation") {
    auto r = check_detail("func opt() -> Int32? { return 7 }\n"
                          "func use() -> Int32? { return try opt() }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("Optional doesn't carry an error") != std::string::npos);
}

TEST_CASE("try! on an Optional suggests postfix ! instead") {
    auto r = check_detail("func opt() -> Int32? { return 7 }\n"
                          "func use() -> Int32 { return try! opt() }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("postfix `!`") != std::string::npos);
}

TEST_CASE("try over a non-Result non-Optional keeps the generic message") {
    // Sanity: the old shape (try over a plain Int32) still emits the
    // "must produce a Result<T, E>" diagnostic with no Optional hint.
    auto r = check_detail("func use() -> Int32 { return try 7 }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("must produce a Result") != std::string::npos);
    CHECK(r.first_message.find("Optional") == std::string::npos);
}

// ---- §6 tuple destructuring -----------------------------------------------

TEST_CASE("let (a, b) = pair binds each element at its tuple-element type") {
    CHECK(check_errors("func mk() -> (Int32, Int32) { return (1, 2) }\n"
                       "func sum() -> Int32 {\n"
                       "    let (a, b) = mk()\n"
                       "    return a + b\n"
                       "}\n")
          == 0);
}

TEST_CASE("tuple-pattern arity mismatch is reported") {
    auto r = check_detail("func mk() -> (Int32, Int32) { return (1, 2) }\n"
                          "func bad() -> Int32 {\n"
                          "    let (a, b, c) = mk()\n"
                          "    return a\n"
                          "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("elements") != std::string::npos);
}

TEST_CASE("tuple pattern on a non-tuple value is reported") {
    auto r = check_detail("func bad(_ x: Int32) -> Int32 {\n"
                          "    let (a, b) = x\n"
                          "    return a\n"
                          "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("tuple") != std::string::npos);
}

TEST_CASE("wildcards inside a tuple pattern are accepted") {
    CHECK(check_errors("func mk() -> (Int32, Int32) { return (1, 2) }\n"
                       "func only_first() -> Int32 {\n"
                       "    let (a, _) = mk()\n"
                       "    return a\n"
                       "}\n")
          == 0);
}

TEST_CASE("function-param tuple pattern binds leaves into the body scope") {
    CHECK(check_errors("func add_pair((a, b): (Int32, Int32)) -> Int32 {\n"
                       "    return a + b\n"
                       "}\n")
          == 0);
}

TEST_CASE("nested tuple-pattern param destructures all the way down") {
    CHECK(check_errors("func sum3(((p, q), r): ((Int32, Int32), Int32)) -> Int32 {\n"
                       "    return p + q + r\n"
                       "}\n")
          == 0);
}

TEST_CASE("tuple-pattern param against a non-tuple type is reported") {
    auto r = check_detail("func bad((a, b): Int32) -> Int32 { return a + b }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("tuple") != std::string::npos);
}

TEST_CASE("match arm with tuple pattern inside enum payload binds leaves") {
    CHECK(check_errors("enum Action {\n"
                       "    case quit\n"
                       "    case point(p: (Int32, Int32))\n"
                       "}\n"
                       "func score(_ a: Action) -> Int32 {\n"
                       "    return match a {\n"
                       "        case .quit: 0\n"
                       "        case .point((let x, let y)): x + y\n"
                       "    }\n"
                       "}\n")
          == 0);
}

// ---- §10 Box[T] + Alloc capability ---------------------------------------

TEST_CASE("Box.new in a function with `using Alloc` type-checks") {
    CHECK(check_errors("func mk(_ x: Int32) using Alloc -> Box[Int32] {\n"
                       "    return Box.new(x)\n"
                       "}\n")
          == 0);
}

TEST_CASE("box.value reads the heap-pointee at type T") {
    CHECK(check_errors("func deref(_ b: Box[Int32]) -> Int32 { return b.value }\n") == 0);
}

TEST_CASE("Box.new with wrong arity is reported") {
    auto r = check_detail("func bad() using Alloc -> Box[Int32] { return Box.new(1, 2) }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("Box.new") != std::string::npos);
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

// ---- §9 iterator combinators (zip / take) --------------------------------

static constexpr const char* kIter = "struct It {\n"
                                     "    var n: Int32\n"
                                     "    inout func next() -> Int32? {\n"
                                     "        if self.n <= 0 { return nil }\n"
                                     "        let v = self.n\n"
                                     "        self.n = self.n - 1\n"
                                     "        return v\n"
                                     "    }\n"
                                     "}\n";

TEST_CASE("zip(a, b) types as a ZipIter and the for-loop binds tuple elements") {
    CHECK(check_errors(std::string{kIter}
                       + "func use() -> Int32 {\n"
                         "    var t: Int32 = 0\n"
                         "    var a = It(n: 3)\n"
                         "    var b = It(n: 5)\n"
                         "    for (x, y) in zip(a, b) { t = t + x + y }\n"
                         "    return t\n"
                         "}\n")
          == 0);
}

TEST_CASE("take(xs, n) types as a TakeIter and the loop variable is the inner element") {
    CHECK(check_errors(std::string{kIter}
                       + "func use() -> Int32 {\n"
                         "    var t: Int32 = 0\n"
                         "    var a = It(n: 100)\n"
                         "    for v in take(a, 3) { t = t + v }\n"
                         "    return t\n"
                         "}\n")
          == 0);
}

TEST_CASE("zip rejects a non-iterator argument with a clear diagnostic") {
    auto r = check_detail("struct NotIter { var x: Int32 }\n"
                          "func use() -> Int32 {\n"
                          "    var n = NotIter(x: 0)\n"
                          "    var t: Int32 = 0\n"
                          "    for v in zip(n, n) { t = t + v }\n"
                          "    return t\n"
                          "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("zip") != std::string::npos);
    CHECK(r.first_message.find("next") != std::string::npos);
}

TEST_CASE("map(xs, closure) types as a MapIter with the closure's return as element type") {
    CHECK(check_errors(std::string{kIter}
                       + "func use() -> Int32 {\n"
                         "    var t: Int32 = 0\n"
                         "    var a = It(n: 5)\n"
                         "    for v in map(a, { x => x * 2 }) { t = t + v }\n"
                         "    return t\n"
                         "}\n")
          == 0);
}

TEST_CASE("filter(xs, closure) types as a FilterIter keeping the source element type") {
    CHECK(check_errors(std::string{kIter}
                       + "func use() -> Int32 {\n"
                         "    var t: Int32 = 0\n"
                         "    var a = It(n: 10)\n"
                         "    for v in filter(a, { x => (x % 2) == 0 }) { t = t + v }\n"
                         "    return t\n"
                         "}\n")
          == 0);
}

TEST_CASE("a closure used outside a function-typed slot is rejected") {
    // No contextual function type means the closure can't be typed
    // in v0.5; sema points the user at the function-slot use site.
    auto r = check_detail("func use() -> Int32 {\n"
                          "    let f = { x => x + 1 }\n"
                          "    return 0\n"
                          "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("closure type cannot be inferred") != std::string::npos);
}

TEST_CASE("filter rejects a closure whose return type isn't Bool") {
    // The closure's expected result type is Bool (filter sets that as
    // its predicate shape); the closure-body diagnostic fires first
    // because the closure's own return-type mismatch is caught before
    // we get back to filter's outer check. Either message is fine
    // for the user — we pin the closure-level one since it's the
    // first thing they see.
    auto r = check_detail(std::string{kIter}
                          + "func use() -> Int32 {\n"
                            "    var a = It(n: 5)\n"
                            "    var t: Int32 = 0\n"
                            "    for v in filter(a, { x => x + 1 }) { t = t + v }\n"
                            "    return t\n"
                            "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("closure body types as") != std::string::npos);
    CHECK(r.first_message.find("Bool") != std::string::npos);
}

// ---- §A4 Atomic[T] (§14.9) -----------------------------------------------

TEST_CASE("Atomic[T] type-checks for primitive T and exposes the core ops") {
    CHECK(check_errors("static c: Atomic[UInt32] = 0\n"
                       "func use() -> UInt32 {\n"
                       "    c.store(0, .release)\n"
                       "    let a = c.fetchAdd(1, .acqRel)\n"
                       "    let b = c.fetchSub(1, .release)\n"
                       "    let prev = c.exchange(7, .seqCst)\n"
                       "    return c.load(.acquire) + a + b + prev\n"
                       "}\n")
          == 0);
}

TEST_CASE("Atomic[T] rejects a non-primitive T") {
    auto r = check_detail("struct S { var x: Int32 }\n"
                          "static c: Atomic[S] = S(x: 0)\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("Atomic[T] requires T to be a primitive type") != std::string::npos);
}

TEST_CASE("Atomic[T] surfaces the bitwise fetch ops + compareExchange") {
    CHECK(check_errors("static c: Atomic[UInt32] = 0\n"
                       "func use() -> UInt32 {\n"
                       "    let a = c.fetchAnd(0xFF, .relaxed)\n"
                       "    let b = c.fetchOr(0x100, .relaxed)\n"
                       "    let d = c.fetchXor(0xAA, .relaxed)\n"
                       "    let r = c.compareExchange(a, b, .seqCst, .acquire)\n"
                       "    if r.succeeded {\n"
                       "        return r.actual\n"
                       "    }\n"
                       "    return r.actual + d\n"
                       "}\n")
          == 0);
}

// ---- §A10 @panic_handler (§15.5) -----------------------------------------

TEST_CASE("@panic_handler accepts the canonical signature") {
    CHECK(check_errors("@panic_handler\n"
                       "func myPanic(_ msg: Str, _ file: StrConst, _ line: Int) -> Never {\n"
                       "    abort()\n"
                       "}\n")
          == 0);
}

TEST_CASE("@panic_handler rejects a mistyped parameter") {
    auto r = check_detail("@panic_handler\n"
                          "func bad(_ msg: Int32, _ file: StrConst, _ line: Int) -> Never {\n"
                          "    abort()\n"
                          "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("(message: Str, file: StrConst, line: Int) -> Never")
          != std::string::npos);
}

TEST_CASE("@panic_handler rejects a non-Never return") {
    auto r = check_detail("@panic_handler\n"
                          "func bad(_ msg: Str, _ file: StrConst, _ line: Int) -> Int32 {\n"
                          "    return 0\n"
                          "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("return type must be `Never`") != std::string::npos);
}

TEST_CASE("@panic_handler rejects a using row") {
    auto r =
        check_detail("@panic_handler\n"
                     "func bad(_ msg: Str, _ file: StrConst, _ line: Int) using Asm -> Never {\n"
                     "    abort()\n"
                     "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("cannot declare a `using` row") != std::string::npos);
}

TEST_CASE("duplicate @panic_handler in one unit is diagnosed") {
    auto r = check_detail("@panic_handler\n"
                          "func first(_ m: Str, _ f: StrConst, _ l: Int) -> Never { abort() }\n"
                          "@panic_handler\n"
                          "func second(_ m: Str, _ f: StrConst, _ l: Int) -> Never { abort() }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("duplicate @panic_handler") != std::string::npos);
}

// ---- §A8 @interrupt handlers (§14.5.2) -----------------------------------

TEST_CASE("@interrupt accepts the canonical inout-TrapFrame shape") {
    CHECK(check_errors("struct TrapFrame { var x0: UInt64 }\n"
                       "@interrupt(0)\n"
                       "func isr(_ frame: inout TrapFrame) {\n"
                       "    frame.x0 = frame.x0 + 1\n"
                       "}\n")
          == 0);
}

TEST_CASE("@interrupt requires the first parameter to be inout") {
    auto r = check_detail("struct TrapFrame { var x0: UInt64 }\n"
                          "@interrupt\n"
                          "func bad(_ frame: TrapFrame) {}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("@interrupt requires an `inout TrapFrame`") != std::string::npos);
}

TEST_CASE("@interrupt rejects a non-Unit return") {
    auto r = check_detail("struct TrapFrame { var x0: UInt64 }\n"
                          "@interrupt\n"
                          "func bad(_ frame: inout TrapFrame) -> Int32 { return 0 }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("@interrupt functions return Unit") != std::string::npos);
}

TEST_CASE("@interrupt rejects Alloc / Async / Extern in the using row") {
    auto r = check_detail("struct TrapFrame { var x0: UInt64 }\n"
                          "@interrupt\n"
                          "func bad(_ frame: inout TrapFrame) using Alloc {}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("@interrupt cannot declare `using Alloc`") != std::string::npos);
}

// ---- §A12 &decl + @extern("conv") (§14.6.1 + §14.6.3) --------------------

TEST_CASE("&static yields Ptr[T]") {
    CHECK(check_errors("static n: Int32 = 42\n"
                       "func get_addr() -> Ptr[Int32] { return &n }\n")
          == 0);
}

TEST_CASE("&decl on a non-static identifier is rejected") {
    auto r = check_detail("func bad() -> Ptr[Int32] {\n"
                          "    let x: Int32 = 0\n"
                          "    return &x\n"
                          "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("&decl operand must be a static or func") != std::string::npos);
}

TEST_CASE("&func types as the callee's function-pointer signature") {
    // §A12 follow-up (§14.6.3) — `&some_func` has the function-pointer
    // type matching the callee's signature. The result flows into a
    // typed slot spelled `(T1, ...) -> R`; mismatched arities or
    // result types are caught at the assignment site.
    CHECK(check_errors("func helper(_ x: Int32) -> Int32 { return x }\n"
                       "func wire() -> Int32 {\n"
                       "    let f: (Int32) -> Int32 = &helper\n"
                       "    return f(7)\n"
                       "}\n")
          == 0);
}

TEST_CASE("&func with mismatched signature is rejected at the slot") {
    auto r = check_detail("func helper(_ x: Int32) -> Int32 { return x }\n"
                          "func bad() -> Int32 {\n"
                          "    let f: (Int32, Int32) -> Int32 = &helper\n"
                          "    return f(1, 2)\n"
                          "}\n");
    CHECK(r.error_count >= 1);
}

TEST_CASE("@extern admits the known calling conventions") {
    CHECK(check_errors("@extern(\"C\") @symbol(\"a\") func a() -> Int32 { return 0 }\n"
                       "@extern(\"sysv64\") @symbol(\"b\") func b() -> Int32 { return 0 }\n"
                       "@extern(\"interrupt\") @symbol(\"c\") func c() -> Int32 { return 0 }\n")
          == 0);
}

TEST_CASE("@extern rejects an unknown calling convention") {
    auto r = check_detail("@extern(\"fastcall\") @symbol(\"f\") func f() -> Int32 { return 0 }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("@extern(\"fastcall\") is not a recognized") != std::string::npos);
}

// ---- §A11 PerCpu (§14.8) -------------------------------------------------

TEST_CASE("PerCpu[T] type-checks and .mine() returns T") {
    CHECK(check_errors("@noinit static c: PerCpu[Atomic[UInt32]]\n"
                       "func bump() -> UInt32 {\n"
                       "    return c.mine().fetchAdd(1, .seqCst)\n"
                       "}\n")
          == 0);
}

TEST_CASE("PerCpu[T] with a primitive inner") {
    CHECK(check_errors("@noinit static c: PerCpu[UInt32]\n"
                       "func get() -> UInt32 { return c.mine() }\n")
          == 0);
}

// ---- §14.12.2 &Sysreg.X is a compile error -------------------------------

TEST_CASE("&Sysreg.X is rejected with a targeted spec-rule diagnostic") {
    // §14.12.2: "&Sysreg.daif is a compile error (a sysreg is not a
    // memory cell with a linkage symbol)." The diagnostic names
    // Sysreg specifically + the spec section so the user sees it's
    // a rule about architectural cells, not a parser quirk.
    auto r = check_detail("func bad() -> Int32 {\n"
                          "    let p = &Sysreg.daif\n"
                          "    return 0\n"
                          "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("&Sysreg.daif is rejected") != std::string::npos);
    CHECK(r.first_message.find("§14.12.2") != std::string::npos);
}

TEST_CASE("&otherStruct.field still gets the generic non-identifier diagnostic") {
    // Other MemberExpr operands (a field access, etc.) fall through
    // to the generic "must be a static or func identifier" rejection.
    auto r = check_detail("struct Foo { var x: Int32 }\n"
                          "func bad(_ f: Foo) -> Int32 {\n"
                          "    let p = &f.x\n"
                          "    return 0\n"
                          "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("must be a static or func identifier") != std::string::npos);
}

// ---- §14.12 typed sysreg access (first slice) ----------------------------

TEST_CASE("Sysreg.<name>.read / .write type-check under Asm") {
    CHECK(check_errors("func wire(_ v: UInt64) using Asm -> UInt64 {\n"
                       "    Sysreg.ttbr0_el1.write(v)\n"
                       "    return Sysreg.midr_el1.read()\n"
                       "}\n")
          == 0);
}

TEST_CASE("Sysreg.midr_el1.write is rejected (read-only handle)") {
    // §14.12.1 — midr_el1 is the CPU model identification register
    // with no architectural write path. The RO handle's
    // lookup_method has no `write` entry, so the call falls
    // through to the no-such-method error, which names the
    // ReadOnlySysreg kind so the user knows why.
    auto r = check_detail("func bad() using Asm {\n"
                          "    Sysreg.midr_el1.write(0)\n"
                          "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("'write' on type ReadOnlySysreg[UInt64]") != std::string::npos);
}

TEST_CASE("Sysreg.midr_el1.read type-checks (read-only handle admits read)") {
    CHECK(check_errors("func cpu_id() using Asm -> UInt64 {\n"
                       "    return Sysreg.midr_el1.read()\n"
                       "}\n")
          == 0);
}

TEST_CASE("Sysreg.daif admits both read and write (RW handle)") {
    CHECK(check_errors("func wire(_ v: UInt64) using Asm -> UInt64 {\n"
                       "    Sysreg.daif.write(v)\n"
                       "    return Sysreg.daif.read()\n"
                       "}\n")
          == 0);
}

TEST_CASE("Sysreg.<unknown> is rejected with the canonical-set diagnostic") {
    auto r = check_detail("func bad() using Asm -> UInt64 {\n"
                          "    return Sysreg.nonsense_reg.read()\n"
                          "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("Sysreg.nonsense_reg") != std::string::npos);
}

// ---- §A6 last: @no_auto_barrier (§14.11.5 / §14.12.3) --------------------

TEST_CASE("@no_auto_barrier on a @kernel_init function type-checks") {
    CHECK(check_errors("@kernel_init\n@no_auto_barrier\nfunc mmu_setup() {}\n") == 0);
}

TEST_CASE("@no_auto_barrier outside @kernel_init is rejected") {
    auto r = check_detail("@no_auto_barrier\nfunc bad() {}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("@no_auto_barrier is admitted only") != std::string::npos);
}

TEST_CASE("@no_auto_barrier on @boot (without @kernel_init) is rejected") {
    auto r = check_detail("@boot\n@no_auto_barrier\nfunc bad() {}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("@no_auto_barrier is admitted only") != std::string::npos);
}

// ---- §A10 follow-up: @stack_protector (§15.4) ----------------------------

TEST_CASE("@stack_protector admits .none / .strong / .all") {
    CHECK(check_errors("@stack_protector(.none)\nfunc a() -> Int32 { return 0 }\n"
                       "@stack_protector(.strong)\nfunc b() -> Int32 { return 0 }\n"
                       "@stack_protector(.all)\nfunc c() -> Int32 { return 0 }\n")
          == 0);
}

TEST_CASE("@stack_protector rejects unknown cases") {
    auto r = check_detail("@stack_protector(.fast)\nfunc bad() -> Int32 { return 0 }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("@stack_protector(.fast)") != std::string::npos);
}

TEST_CASE("@stack_protector with no argument is rejected") {
    auto r = check_detail("@stack_protector\nfunc bad() -> Int32 { return 0 }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("@stack_protector expects one of") != std::string::npos);
}

// ---- §A11 Padded[T] (§14.8) ----------------------------------------------

TEST_CASE("Padded[T] resolves and .value returns T") {
    CHECK(check_errors("@noinit static slot: Padded[UInt64]\n"
                       "func read_it() -> UInt64 { return slot.value }\n"
                       "func write_it(_ v: UInt64) { slot.value = v }\n")
          == 0);
}

TEST_CASE("Padded[T] composes with Atomic for the per-hart counter shape") {
    // Padded[Atomic[UInt32]] is the load-bearing shape for a single
    // cell of the kernel's per-hart array region.
    CHECK(check_errors("@noinit static c: Padded[Atomic[UInt32]]\n"
                       "func bump() -> UInt32 { return c.value.fetchAdd(1, .seqCst) }\n")
          == 0);
}

// ---- §A9 @boot + @kernel_init (§14.7) ------------------------------------

TEST_CASE("@boot with Asm/RawMemory/Mmio using row + no throws type-checks") {
    CHECK(check_errors("@boot\nfunc _start() using Asm {}\n"
                       "@boot\nfunc _start_raw() using RawMemory {}\n"
                       "@boot\nfunc _start_mmio() using Mmio {}\n")
          == 0);
}

TEST_CASE("@boot rejects `using Alloc`") {
    auto r = check_detail("@boot\nfunc bad() using Alloc {}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("@boot cannot declare `using Alloc`") != std::string::npos);
}

TEST_CASE("@boot rejects `throws(E)`") {
    auto r = check_detail("@boot\nfunc bad() throws(Int32) {}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("@boot cannot declare `throws(E)`") != std::string::npos);
}

TEST_CASE("@kernel_init rejects `using Async`") {
    auto r = check_detail("@kernel_init\nfunc bad() using Async {}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("@kernel_init cannot declare `using Async`") != std::string::npos);
}

TEST_CASE("@kernel_init with `using Alloc` is admitted (audit-time, not static)") {
    // Spec rule 3 — `using Alloc` is admitted only after the
    // allocator's @kernel_init initialiser has returned. The compiler
    // doesn't statically check the order; that's audit-time.
    CHECK(check_errors("@kernel_init\nfunc ok() using Alloc {}\n") == 0);
}

TEST_CASE("Ptr[T].value reads through to T (read-only)") {
    // §A3 follow-up (§10.5) deref via .value. Mirrors Box[T].value.
    CHECK(check_errors("func touch(_ addr: UInt64) -> UInt32 {\n"
                       "    with RawMemory {\n"
                       "        let p: Ptr[UInt32] = Ptr.unchecked(fromAddress: addr)\n"
                       "        return p.value\n"
                       "    }\n"
                       "}\n")
          == 0);
}

TEST_CASE("MutPtr[T].value admits assignment-through") {
    CHECK(check_errors("func touch(_ addr: UInt64, _ v: UInt32) {\n"
                       "    with RawMemory {\n"
                       "        let p: MutPtr[UInt32] = MutPtr.unchecked(fromAddress: addr)\n"
                       "        p.value = v\n"
                       "    }\n"
                       "}\n")
          == 0);
}

TEST_CASE("PerCpu.new(value) returns Box[PerCpu[T]]") {
    // §A11 (§14.8) heap factory. The Vestra-side call needs Alloc in
    // scope; the resulting Box wraps a PerCpu[T] whose inner T matches
    // the value's type.
    CHECK(check_errors("func make() -> UInt32 {\n"
                       "    with Alloc {\n"
                       "        let pc: Box[PerCpu[UInt32]] = PerCpu.new(7)\n"
                       "        return pc.value.mine()\n"
                       "    }\n"
                       "}\n")
          == 0);
}

TEST_CASE("PerCpu[T].slot(hartId) returns Ptr[T] under RawMemory") {
    // §A11 (§14.8) cross-hart accessor.
    CHECK(check_errors("@noinit static c: PerCpu[UInt32]\n"
                       "func at(_ h: UInt16) -> UInt32 {\n"
                       "    with RawMemory {\n"
                       "        let p: Ptr[UInt32] = c.slot(h)\n"
                       "    }\n"
                       "    return 0\n"
                       "}\n")
          == 0);
}

// ---- §A7 InterruptsOff + Scheduler.swapContext (§14.13, §14.14) ----------

TEST_CASE("`with InterruptsOff { ... }` resolves cleanly") {
    CHECK(check_errors("func ok() {\n"
                       "    with InterruptsOff {\n"
                       "        let x: Int32 = 7\n"
                       "    }\n"
                       "}\n")
          == 0);
}

TEST_CASE("Scheduler.swapContext accepts two Context arguments") {
    CHECK(check_errors("@noinit static a: Context\n"
                       "@noinit static b: Context\n"
                       "func ok() { Scheduler.swapContext(a, b) }\n")
          == 0);
}

TEST_CASE("Scheduler.swapContext rejects a non-Context argument") {
    auto r = check_detail("@noinit static a: Context\n"
                          "func bad(_ x: Int32) { Scheduler.swapContext(a, x) }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("Scheduler.swapContext loading argument must be Context")
          != std::string::npos);
}

// ---- §A6 MMIO views (§14.11) ---------------------------------------------

TEST_CASE("MmioView + MmioRegion type-check under RawMemory + Mmio") {
    CHECK(check_errors("func touch(_ addr: UInt64) -> UInt32 {\n"
                       "    var observed: UInt32 = 0\n"
                       "    with RawMemory {\n"
                       "        with Mmio {\n"
                       "            let p: MutPtr[UInt32] = MutPtr.unchecked(fromAddress: addr)\n"
                       "            let view = MmioView.at(p)\n"
                       "            view.write(42)\n"
                       "            observed = view.read()\n"
                       "            let r = MmioRegion.at(p, 4)\n"
                       "            observed = r.index(0).read() + UInt32(0)\n"
                       "        }\n"
                       "    }\n"
                       "    return observed\n"
                       "}\n")
          == 0);
}

TEST_CASE("MmioView.at rejects a non-MutPtr argument") {
    auto r = check_detail("func bad(_ addr: UInt64) {\n"
                          "    with RawMemory {\n"
                          "        with Mmio {\n"
                          "            let view = MmioView.at(addr)\n"
                          "        }\n"
                          "    }\n"
                          "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("MmioView.at first argument must be MutPtr[T]")
          != std::string::npos);
}

TEST_CASE("MmioView[T] requires T to be a primitive") {
    auto r = check_detail("struct S { var x: Int32 }\n"
                          "func bad() -> Int32 {\n"
                          "    let v: MmioView[S] = MmioView.at(MutPtr.unchecked(fromAddress: 0))\n"
                          "    return 0\n"
                          "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("MmioView[T] requires T to be a primitive") != std::string::npos);
}

TEST_CASE("MmioWireView[T].at + .read / .write type-check under RawMemory + Mmio") {
    CHECK(check_errors("func talk(_ addr: UInt64) -> UInt32 {\n"
                       "    var observed: UInt32 = 0\n"
                       "    with RawMemory {\n"
                       "        with Mmio {\n"
                       "            let p: MutPtr[UInt32] = MutPtr.unchecked(fromAddress: addr)\n"
                       "            let w = MmioWireView.at(p, .big)\n"
                       "            w.write(42)\n"
                       "            observed = w.read()\n"
                       "        }\n"
                       "    }\n"
                       "    return observed\n"
                       "}\n")
          == 0);
}

TEST_CASE("MmioWireView.at rejects a non-MutPtr first argument") {
    auto r = check_detail("func bad(_ addr: UInt64) {\n"
                          "    with RawMemory {\n"
                          "        with Mmio {\n"
                          "            let w = MmioWireView.at(addr, .little)\n"
                          "        }\n"
                          "    }\n"
                          "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("MmioWireView.at first argument must be MutPtr[T]")
          != std::string::npos);
}

TEST_CASE("MmioWireView[T] requires T to be a primitive") {
    auto r = check_detail(
        "struct S { var x: Int32 }\n"
        "func bad() -> Int32 {\n"
        "    let w: MmioWireView[S] = MmioWireView.at(MutPtr.unchecked(fromAddress: 0), .native)\n"
        "    return 0\n"
        "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("MmioWireView[T] requires T to be a primitive")
          != std::string::npos);
}

// ---- §A6 follow-up: @repr(union) (§6.8 / §14.11.4) -----------------------

TEST_CASE("@repr(union) struct with overlay members type-checks") {
    CHECK(check_errors("@repr(packed)\n"
                       "struct Bits {\n"
                       "    @bits(1) var rxReady: UInt32\n"
                       "    @bits(31) var rest:   UInt32\n"
                       "}\n"
                       "@repr(union)\n"
                       "struct Status {\n"
                       "    var raw:    UInt32\n"
                       "    var fields: Bits\n"
                       "}\n"
                       "func peek() -> UInt32 {\n"
                       "    let s = Status(raw: 5)\n"
                       "    return s.raw\n"
                       "}\n")
          == 0);
}

TEST_CASE("@bits on a direct @repr(union) member is rejected") {
    auto r = check_detail("@repr(union)\n"
                          "struct Bad {\n"
                          "    @bits(4) var lo: UInt8\n"
                          "    var raw:         UInt8\n"
                          "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("@bits on a direct member of @repr(union)") != std::string::npos);
}

// ---- §A8 follow-up: typed vector tables (§14.5.3) ------------------------

TEST_CASE("vector-table static of @interrupt(T) accepts ISR-shaped functions") {
    CHECK(check_errors("struct TrapFrame { var x: UInt64 }\n"
                       "@interrupt\n"
                       "func a(_ f: inout TrapFrame) { f.x = 1 }\n"
                       "@interrupt\n"
                       "func b(_ f: inout TrapFrame) { f.x = 2 }\n"
                       "@interrupt\n"
                       "func c(_ f: inout TrapFrame) { f.x = 3 }\n"
                       "@interrupt\n"
                       "func d(_ f: inout TrapFrame) { f.x = 4 }\n"
                       "static vt: [4]@interrupt(TrapFrame) = [a, b, c, d]\n")
          == 0);
}

TEST_CASE("vector-table slot rejects a function with mismatched trap-frame type") {
    auto r = check_detail("struct TrapFrame { var x: UInt64 }\n"
                          "struct OtherFrame { var y: UInt64 }\n"
                          "@interrupt\n"
                          "func a(_ f: inout OtherFrame) { f.y = 1 }\n"
                          "static vt: [1]@interrupt(TrapFrame) = [a]\n");
    CHECK(r.error_count >= 1);
}

TEST_CASE("@interrupt(T) in a non-type position is rejected") {
    auto r = check_detail("func bad() -> Int32 { let x: @interrupt = 0; return 0 }\n");
    CHECK(r.error_count >= 1);
}

TEST_CASE("derive(Eq) on a @repr(union) struct is rejected") {
    auto r = check_detail("@repr(union)\n"
                          "struct Cell {\n"
                          "    var raw:    UInt32\n"
                          "    var halves: UInt32\n"
                          "}\n"
                          "derive(Eq) for Cell\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("derive on @repr(union) struct 'Cell'") != std::string::npos);
}

// ---- §A5 cache + TLB management (§14.10.3, §14.10.4, §14.10.5) -----------

// ---- §A10 follow-up: memcpy / memset / memmove intrinsics (§15.4) --------

TEST_CASE("memcpy / memset / memmove type-check under RawMemory") {
    CHECK(check_errors("func use(_ d_addr: UInt64, _ s_addr: UInt64) {\n"
                       "    with RawMemory {\n"
                       "        let d: MutPtr[UInt8] = MutPtr.unchecked(fromAddress: d_addr)\n"
                       "        let s: Ptr[UInt8]    = Ptr.unchecked(fromAddress: s_addr)\n"
                       "        memcpy(d, s, 4)\n"
                       "        memset(d, 0xFF, 2)\n"
                       "        memmove(d, s, 4)\n"
                       "    }\n"
                       "}\n")
          == 0);
}

TEST_CASE("memcpy rejects a Ptr[UInt8] as the destination") {
    // memcpy's destination must be MutPtr[UInt8] — a read-only Ptr is
    // an argument-type mismatch (mirrors zeroData's read-only-arg
    // rejection in §A5).
    auto r = check_detail("func bad(_ a: UInt64) {\n"
                          "    with RawMemory {\n"
                          "        let p: Ptr[UInt8] = Ptr.unchecked(fromAddress: a)\n"
                          "        memcpy(p, p, 4)\n"
                          "    }\n"
                          "}\n");
    CHECK(r.error_count >= 1);
}

TEST_CASE("cache + TLB builtins type-check with the right argument shapes") {
    CHECK(check_errors("func use(_ addr: UInt64, _ n: Int) {\n"
                       "    with RawMemory {\n"
                       "        let mp: MutPtr[UInt8] = MutPtr.unchecked(fromAddress: addr)\n"
                       "        let m = MutSpan.raw(at: mp, count: n)\n"
                       "        zeroData(m)\n"
                       "        let p: Ptr[UInt8] = Ptr.unchecked(fromAddress: addr)\n"
                       "        let s = Span.raw(at: p, count: n)\n"
                       "        cleanData(s)\n"
                       "        invalidateData(s)\n"
                       "        cleanInvalidateData(s)\n"
                       "        publishInstructions(s)\n"
                       "    }\n"
                       "    invalidateAllInstructions()\n"
                       "    tlbInvalidateAll(.innerShareable)\n"
                       "    tlbInvalidatePage(0x1000, false, .currentEL)\n"
                       "    tlbInvalidateAsid(7, .innerShareable)\n"
                       "}\n")
          == 0);
}

TEST_CASE("zeroData rejects a Span (read-only) argument") {
    // zeroData mutates; passing a Span[UInt8] (read-only) instead of
    // MutSpan[UInt8] is an argument-type mismatch.
    auto r = check_detail("func bad(_ addr: UInt64, _ n: Int) {\n"
                          "    with RawMemory {\n"
                          "        let p: Ptr[UInt8] = Ptr.unchecked(fromAddress: addr)\n"
                          "        let s = Span.raw(at: p, count: n)\n"
                          "        zeroData(s)\n"
                          "    }\n"
                          "}\n");
    CHECK(r.error_count >= 1);
}

// ---- §A3 raw-mint primitives (§10.5) -------------------------------------

TEST_CASE("MutPtr.unchecked + MutSpan.raw under RawMemory type-checks") {
    CHECK(check_errors("func touch(_ addr: UInt64) {\n"
                       "    with RawMemory {\n"
                       "        let p: MutPtr[UInt32] = MutPtr.unchecked(fromAddress: addr)\n"
                       "        let s = MutSpan.raw(at: p, count: 4)\n"
                       "        s[0] = 42\n"
                       "    }\n"
                       "}\n")
          == 0);
}

TEST_CASE("Ptr.unchecked needs an expected Ptr[T] at the call site") {
    auto r = check_detail("func bad(_ addr: UInt64) {\n"
                          "    with RawMemory {\n"
                          "        let p = Ptr.unchecked(fromAddress: addr)\n"
                          "    }\n"
                          "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("needs an expected Ptr[T] type") != std::string::npos);
}

TEST_CASE("Span.raw 'at' arg must be a Ptr[T]") {
    auto r = check_detail(
        "func bad(_ addr: UInt64) {\n"
        "    with RawMemory {\n"
        "        let p: MutPtr[UInt32] = MutPtr.unchecked(fromAddress: addr)\n"
        "        let s: Span[UInt32] = Span.raw(at: p, count: 4)\n"  // MutPtr to Span — mismatch
        "    }\n"
        "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("Span.raw 'at' must be a Ptr[T]") != std::string::npos);
}

// ---- §A5 sync intrinsics (§14.10) ----------------------------------------

TEST_CASE("sync-intrinsic builtins are registered and type-check") {
    CHECK(check_errors("func use() {\n"
                       "    compilerFence(.acquire)\n"
                       "    memoryBarrier(.full, .storeStore)\n"
                       "    syncBarrier(.inner)\n"
                       "    instructionBarrier()\n"
                       "    waitForInterrupt()\n"
                       "    waitForEvent()\n"
                       "    signalEvent()\n"
                       "    relax()\n"
                       "    nop()\n"
                       "}\n")
          == 0);
}

TEST_CASE("memoryBarrier rejects a swap of its enum arguments") {
    // The first param is BarrierScope, the second BarrierKind. Mixing
    // them up should fail because the leading-dot resolution flows the
    // expected enum type from each slot.
    auto r = check_detail("func bad() { memoryBarrier(.loadLoad, .full) }\n");
    CHECK(r.error_count >= 1);
}

TEST_CASE("compareExchangeWeak admitted inside `while true { ... }`") {
    CHECK(check_errors("static c: Atomic[UInt32] = 0\n"
                       "func use(_ desired: UInt32) -> UInt32 {\n"
                       "    while true {\n"
                       "        let r = c.compareExchangeWeak(0, desired, .seqCst, .acquire)\n"
                       "        if r.succeeded { return r.actual }\n"
                       "    }\n"
                       "    return 0\n"
                       "}\n")
          == 0);
}

TEST_CASE("compareExchangeWeak admitted inside `while !r.succeeded`") {
    CHECK(check_errors("static c: Atomic[UInt32] = 0\n"
                       "func use(_ exp: UInt32, _ desired: UInt32) -> UInt32 {\n"
                       "    var r = c.compareExchange(exp, desired, .seqCst, .acquire)\n"
                       "    while !r.succeeded {\n"
                       "        r = c.compareExchangeWeak(r.actual, desired, .seqCst, .acquire)\n"
                       "    }\n"
                       "    return r.actual\n"
                       "}\n")
          == 0);
}

TEST_CASE("free-standing compareExchangeWeak is rejected with a clear hint") {
    auto r = check_detail("static c: Atomic[UInt32] = 0\n"
                          "func bad() -> UInt32 {\n"
                          "    let r = c.compareExchangeWeak(0, 1, .seqCst, .acquire)\n"
                          "    return r.actual\n"
                          "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("compareExchangeWeak is admitted only inside") != std::string::npos);
    CHECK(r.first_message.find("compareExchange") != std::string::npos);
}

TEST_CASE("compareExchangeWeak in a `while cond { ... }` with a non-admitted cond is rejected") {
    // `while x > 0 { ... }` isn't one of the two admitted shapes —
    // the loop doesn't terminate on the CAS's succeeded flag, so
    // the loop guarantee the spec relies on doesn't hold.
    auto r = check_detail("static c: Atomic[UInt32] = 0\n"
                          "func bad(_ x: UInt32) -> UInt32 {\n"
                          "    var i = x\n"
                          "    while i > 0 {\n"
                          "        let r = c.compareExchangeWeak(0, 1, .seqCst, .acquire)\n"
                          "        if r.succeeded { return r.actual }\n"
                          "        i = i - 1\n"
                          "    }\n"
                          "    return 0\n"
                          "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("compareExchangeWeak is admitted only inside") != std::string::npos);
}

TEST_CASE("Atomic[T] initial value adopts the inner T from context") {
    // The Int literal `7` would otherwise default to Int and fail
    // assignment to Atomic[UInt32]. The hint-peel through Atomic in
    // IntLit makes it type as UInt32.
    CHECK(check_errors("static c: Atomic[UInt32] = 7\n") == 0);
}

// ---- §A2 @inline + layout reflection (§6.8, §7.8) ------------------------

TEST_CASE("@inline accepts .always / .never / .hint") {
    CHECK(check_errors("@inline(.always)\n"
                       "func a() -> Int32 { return 1 }\n"
                       "@inline(.never)\n"
                       "func b() -> Int32 { return 2 }\n"
                       "@inline(.hint)\n"
                       "func c() -> Int32 { return 3 }\n")
          == 0);
}

TEST_CASE("@inline rejects an unknown case") {
    auto r = check_detail("@inline(.maybe)\n"
                          "func f() -> Int32 { return 0 }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("@inline(.maybe)") != std::string::npos);
}

TEST_CASE("T.size and T.alignment type as Int") {
    CHECK(check_errors("struct S { var x: UInt32; var y: UInt16 }\n"
                       "func size() -> Int { return S.size }\n"
                       "func align() -> Int { return S.alignment }\n")
          == 0);
}

TEST_CASE("T.fields[i] carries offset / size / alignment as Int members") {
    CHECK(check_errors("struct S {\n"
                       "    var id: UInt64\n"
                       "    var flag: UInt8\n"
                       "}\n"
                       "func at_off() -> Int { return S.fields[0].offset }\n"
                       "func at_size() -> Int { return S.fields[0].size }\n"
                       "func at_align() -> Int { return S.fields[0].alignment }\n")
          == 0);
}

TEST_CASE("layout reflection fails on a struct with no v0.5 layout for a field") {
    // Optional<T> isn't sized by the v0.5 fold (its layout depends on
    // std::optional's host implementation). Sema diagnoses cleanly
    // instead of returning a wrong number.
    auto r = check_detail("struct S { var maybe: Int32? }\n"
                          "func size() -> Int { return S.size }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("can't compute") != std::string::npos);
}

// ---- §A1 link attributes (§4.5, §6.7) ------------------------------------

TEST_CASE("@noinit static type-checks with no initializer") {
    CHECK(check_errors("@noinit @section(\"__BSS,__buf\")\n"
                       "static buf: [16]UInt8\n")
          == 0);
}

TEST_CASE("@section / @symbol / @alias require a string-literal argument") {
    auto r = check_detail("@section(42)\n"
                          "func f() -> Int32 { return 0 }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("@section") != std::string::npos);
    CHECK(r.first_message.find("string literal") != std::string::npos);
}

TEST_CASE("@weak takes no arguments") {
    auto r = check_detail("@weak(\"oops\")\n"
                          "func f() -> Int32 { return 0 }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("@weak takes no arguments") != std::string::npos);
}

TEST_CASE("@visibility rejects an unknown case") {
    auto r = check_detail("@visibility(.invisible)\n"
                          "func f() -> Int32 { return 0 }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("@visibility(.invisible)") != std::string::npos);
}

TEST_CASE("take rejects a non-integer count") {
    auto r = check_detail(std::string{kIter}
                          + "func use() -> Int32 {\n"
                            "    var a = It(n: 10)\n"
                            "    var t: Int32 = 0\n"
                            "    for v in take(a, true) { t = t + v }\n"
                            "    return t\n"
                            "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("integer count") != std::string::npos);
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

// ---- §9 do/catch where guard ---------------------------------------------

TEST_CASE("do/catch with a where guard type-checks under the bound error name") {
    // §9 the guard runs after the catch binding is established, so
    // `e` is visible. The enclosing fn carries `throws(E)` because
    // the guard's fall-through propagates the bound error.
    CHECK(check_errors("enum E { case bad; case worse }\n"
                       "func f() throws(E) -> Int32 { return 1 }\n"
                       "func isBad(_ e: E) -> Bool { return match e {\n"
                       "    case .bad: true\n"
                       "    case .worse: false\n"
                       "} }\n"
                       "func go() throws(E) -> Int32 {\n"
                       "    return do { try f() } catch (e: E) where isBad(e) { -1 }\n"
                       "}\n")
          == 0);
}

TEST_CASE("non-Bool guard is rejected with a clear diagnostic") {
    auto r = check_detail("enum E { case bad }\n"
                          "func f() throws(E) -> Int32 { return 1 }\n"
                          "func go() throws(E) -> Int32 {\n"
                          "    return do { try f() } catch (e: E) where 42 { -1 }\n"
                          "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("where-guard must be Bool") != std::string::npos);
}

TEST_CASE("where-guard requires an enclosing throws(E) context") {
    // No `throws` on the enclosing fn means the failed-guard
    // propagation has nowhere to go — sema rejects with a clear
    // diagnostic pointing at the guard.
    auto r = check_detail("enum E { case bad }\n"
                          "func f() throws(E) -> Int32 { return 1 }\n"
                          "func isBad(_ e: E) -> Bool { return true }\n"
                          "func go() -> Int32 {\n"
                          "    return do { try f() } catch (e: E) where isBad(e) { -1 }\n"
                          "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("where-guard requires the enclosing function to be")
          != std::string::npos);
}

TEST_CASE("bare-form catch with a where guard infers E from the do-body's try sites") {
    // §9 catch binding without `: E` annotation — sema infers the
    // error type from the do-body's try-expressions and routes it
    // into the guard's scope. Same propagation semantic as the
    // annotated form: a failed guard propagates the inferred E to
    // the enclosing throws(E) context.
    CHECK(check_errors("enum E { case bad }\n"
                       "func f() throws(E) -> Int32 { return 1 }\n"
                       "func go() throws(E) -> Int32 {\n"
                       "    return do { try f() } catch e where e == E.bad { -1 }\n"
                       "}\n")
          == 0);
}

TEST_CASE("where-guard rejects a throws context whose E differs from the caught E") {
    // The do-catch catches NetErr; the enclosing fn throws DbErr.
    // The fall-through can't propagate NetErr through a DbErr slot,
    // so sema rejects.
    auto r = check_detail("enum NetErr { case timeout }\n"
                          "enum DbErr  { case lost }\n"
                          "func f() throws(NetErr) -> Int32 { return 1 }\n"
                          "func isBad(_ e: NetErr) -> Bool { return true }\n"
                          "func go() throws(DbErr) -> Int32 {\n"
                          "    return do { try f() } catch (e: NetErr) where isBad(e) { -1 }\n"
                          "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("fall-through propagates") != std::string::npos);
}

// ---- §17.4 with name = expr { ... } binding ------------------------------

TEST_CASE("with-binding name is visible inside the block body") {
    CHECK(check_errors("struct R { var x: Int32 }\n"
                       "func mk() -> R { return R(x: 42) }\n"
                       "func use() -> Int32 {\n"
                       "    var total: Int32 = 0\n"
                       "    with r = mk() {\n"
                       "        total = r.x\n"
                       "    }\n"
                       "    return total\n"
                       "}\n")
          == 0);
}

TEST_CASE("with-binding admits a type annotation: `with x: T = expr`") {
    CHECK(check_errors("func mk() -> Int32 { return 7 }\n"
                       "func wire() -> Int32 {\n"
                       "    with x: Int32 = mk() {\n"
                       "        return x * 2\n"
                       "    }\n"
                       "}\n")
          == 0);
}

TEST_CASE("with-binding type annotation rejects a value of the wrong type") {
    auto r = check_detail("func wire() -> Int32 {\n"
                          "    with x: Int32 = \"wrong\" {\n"
                          "        return 0\n"
                          "    }\n"
                          "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("with-binding value of type") != std::string::npos);
    CHECK(r.first_message.find("does not match annotation Int32") != std::string::npos);
}

TEST_CASE("with-binding name does not leak past the block") {
    auto r = check_detail("struct R { var x: Int32 }\n"
                          "func mk() -> R { return R(x: 42) }\n"
                          "func bad() -> Int32 {\n"
                          "    with r = mk() {\n"
                          "        let _ = r.x\n"
                          "    }\n"
                          "    return r.x\n"
                          "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("undefined name 'r'") != std::string::npos);
}

TEST_CASE("cap-only with form still type-checks (no name introduced)") {
    CHECK(check_errors("func mk(_ x: Int32) using Alloc -> Box[Int32] {\n"
                       "    return Box.new(x)\n"
                       "}\n"
                       "func use() {\n"
                       "    with Alloc {\n"
                       "        let b = mk(7)\n"
                       "        let _ = b\n"
                       "    }\n"
                       "}\n")
          == 0);
}

// ---- §9 .mapError(_ f) on Result -----------------------------------------

TEST_CASE("mapError widens the error type via a (E) -> E' function") {
    CHECK(check_errors("enum A { case bad }\n"
                       "enum B { case bad }\n"
                       "func f(_ x: Int32) throws(A) -> Int32 { return x }\n"
                       "func toB(_ e: A) -> B { return B.bad }\n"
                       "func g(_ x: Int32) throws(B) -> Int32 {\n"
                       "    return try f(x).mapError(toB)\n"
                       "}\n")
          == 0);
}

TEST_CASE("mapError rejects a closure whose input doesn't match the Result error") {
    auto r = check_detail("enum A { case bad }\n"
                          "enum B { case bad }\n"
                          "func f(_ x: Int32) throws(A) -> Int32 { return x }\n"
                          "func wrongIn(_ e: B) -> B { return e }\n"
                          "func bad(_ x: Int32) throws(B) -> Int32 {\n"
                          "    return try f(x).mapError(wrongIn)\n"
                          "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("mapError function expects") != std::string::npos);
}

TEST_CASE("mapError accepts a closure literal whose param adopts the Result's error") {
    // §9 + §16 — when the .mapError argument is a closure literal,
    // sema pushes `(E) -> ?` down so the closure's param adopts E
    // without an explicit annotation. The body's last expression
    // types the new error E'; the result is Result[T, E'].
    CHECK(check_errors("enum ParseErr { case bad }\n"
                       "enum AppErr {\n"
                       "    case parseFailed\n"
                       "    case other\n"
                       "}\n"
                       "func parse() throws(ParseErr) -> Int32 { throw ParseErr.bad }\n"
                       "func wrap() throws(AppErr) -> Int32 {\n"
                       "    return try parse().mapError({ pe => AppErr.parseFailed })\n"
                       "}\n")
          == 0);
}

TEST_CASE("mapError requires exactly one argument") {
    auto r = check_detail("enum A { case bad }\n"
                          "func f() throws(A) -> Int32 { return 0 }\n"
                          "func toB(_ e: A) -> A { return e }\n"
                          "func bad() throws(A) -> Int32 { return try f().mapError(toB, 1) }\n");
    CHECK(r.error_count >= 1);
}

// ---- §3 opaque newtype follow-ons ----------------------------------------

TEST_CASE("opaque with derive(Debug) is Display-conformant in an interpolation splice") {
    CHECK(check_errors("opaque type UserId = UInt32\n"
                       "derive(Debug) for UserId\n"
                       "func render(_ u: UserId) -> String { return \"u=\\(u)\" }\n")
          == 0);
}

TEST_CASE("derive(Default) admits an opaque whose underlying is Default-conformant") {
    // §3 follow-on: an opaque wrapping a primitive (which is always
    // Default-conformant) becomes Default-conformant itself, so
    // `derive(Default) for UserId` is admitted. The C++ side's
    // `enum class UserId : UInt32 {}` already provides `UserId{}`
    // = `UserId(0)` for free, so no codegen change was needed.
    CHECK(check_errors("opaque type UserId = UInt32\n"
                       "derive(Default) for UserId\n"
                       "func mk() -> UserId { return UserId(0) }\n")
          == 0);
}

TEST_CASE("derive(Default) for opaque rejects a non-Default-conformant underlying") {
    auto r = check_detail("struct NoDefault { var f: Int32 }\n"
                          "opaque type Wrap = NoDefault\n"
                          "derive(Default) for Wrap\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("derive(Default) for opaque 'Wrap'") != std::string::npos);
    CHECK(r.first_message.find("not Default-conformant") != std::string::npos);
}

TEST_CASE("opaque without derive(Debug|Display) is still rejected in a splice") {
    auto r = check_detail("opaque type UserId = UInt32\n"
                          "func render(_ u: UserId) -> String { return \"u=\\(u)\" }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("Display-conformant") != std::string::npos);
}

TEST_CASE("inverse conversion T(q) accepts opaque arg with numeric underlying") {
    CHECK(check_errors("opaque type UserId = UInt32\n"
                       "func unwrap(_ u: UserId) -> UInt32 { return UInt32(u) }\n")
          == 0);
}

TEST_CASE("forward conversion Q(t) still type-checks the underlying") {
    CHECK(check_errors("opaque type UserId = UInt32\n"
                       "func wrap(_ n: UInt32) -> UserId { return UserId(n) }\n")
          == 0);
}

// ---- §17.7 pattern matching enhancements ---------------------------------

TEST_CASE("literal patterns type-check against the scrutinee") {
    CHECK(check_errors("func test(_ n: Int32) -> Int32 {\n"
                       "    return match n {\n"
                       "        case 0: 1\n"
                       "        case 1: 2\n"
                       "        default: 3\n"
                       "    }\n"
                       "}\n")
          == 0);
}

TEST_CASE("range patterns require an integer scrutinee") {
    CHECK(check_errors("func test(_ n: Int32) -> Int32 {\n"
                       "    return match n {\n"
                       "        case 0..<10: 1\n"
                       "        case 10..20: 2\n"
                       "        default: 0\n"
                       "    }\n"
                       "}\n")
          == 0);
}

TEST_CASE("or-patterns stacking enum cases are accepted") {
    CHECK(check_errors("enum Color { case red\n    case green\n    case blue\n}\n"
                       "func is_warm(_ c: Color) -> Int32 {\n"
                       "    return match c {\n"
                       "        case .red | .green: 1\n"
                       "        case .blue: 0\n"
                       "    }\n"
                       "}\n")
          == 0);
}

TEST_CASE("or-patterns drain exhaustiveness coverage") {
    // Without `case .blue`, the or-pattern still covers red and green;
    // a single un-covered case fires the diagnostic.
    auto r = check_detail("enum Color { case red\n    case green\n    case blue\n}\n"
                          "func partial(_ c: Color) -> Int32 {\n"
                          "    return match c {\n"
                          "        case .red | .green: 1\n"
                          "    }\n"
                          "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("'blue' is not covered") != std::string::npos);
}

TEST_CASE("match over a tuple scrutinee accepts a tuple pattern") {
    CHECK(check_errors("func classify(_ p: (Int32, Int32)) -> Int32 {\n"
                       "    return match p {\n"
                       "        case (0, 0): 1\n"
                       "        case (0, _): 2\n"
                       "        case (_, 0): 3\n"
                       "        default: 4\n"
                       "    }\n"
                       "}\n")
          == 0);
}

// ---- §4 Optional in a Display splice -------------------------------------

TEST_CASE("Optional<T> is Display-conformant when T is") {
    CHECK(check_errors("func render(_ a: Int32?) -> String { return \"a=\\(a)\" }\n") == 0);
}

TEST_CASE("Optional<T> over a non-conformant T is still rejected") {
    auto r = check_detail("struct Bare { var n: Int32 }\n"
                          "func render(_ b: Bare?) -> String { return \"b=\\(b)\" }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("Display-conformant") != std::string::npos);
}

// ---- §10 Span[T] / MutSpan[T] --------------------------------------------

TEST_CASE("Span[T] type resolution + array-to-Span call-site coercion") {
    CHECK(check_errors("func sum(_ s: Span[Int32]) -> Int32 { return 0 }\n"
                       "const A: [4]Int32 = comptime { [1, 2, 3, 4] }\n"
                       "func use() -> Int32 { return sum(A) }\n")
          == 0);
}

TEST_CASE("MutSpan[T] widens to Span[T] but not the other way") {
    // (`read` is a parameter-mode keyword in Vestra — `consume` is the
    // convention for sink-style test functions.)
    CHECK(check_errors("func consume(_ s: Span[Int32]) -> Int32 { return 0 }\n"
                       "func mutate(_ s: MutSpan[Int32]) -> Int32 { return 0 }\n"
                       "func use(_ m: MutSpan[Int32]) -> Int32 { return consume(m) }\n")
          == 0);
    auto r = check_detail("func mutate(_ s: MutSpan[Int32]) -> Int32 { return 0 }\n"
                          "func use(_ s: Span[Int32]) -> Int32 { return mutate(s) }\n");
    CHECK(r.error_count >= 1);
}

TEST_CASE("Span .count returns Int and .isEmpty returns Bool") {
    CHECK(check_errors("func test(_ s: Span[Int32]) -> Bool {\n"
                       "    let c: Int = s.count\n"
                       "    let _ = c\n"
                       "    return s.isEmpty\n"
                       "}\n")
          == 0);
}

TEST_CASE("Span indexing returns the element type") {
    CHECK(check_errors("func first(_ s: Span[Int32]) -> Int32 {\n"
                       "    return s[0]\n"
                       "}\n")
          == 0);
}

TEST_CASE("non-integer index is rejected") {
    auto r = check_detail("func bad(_ s: Span[Int32]) -> Int32 {\n"
                          "    return s[\"oops\"]\n"
                          "}\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("integer") != std::string::npos);
}

// ---- §17 natural-width integer flow --------------------------------------

TEST_CASE("Int (natural-width) flows implicitly into Int32 slot and back") {
    CHECK(check_errors("func use(_ n: Int, _ m: Int32) -> Int32 {\n"
                       "    let a: Int32 = n\n"
                       "    let b: Int = m\n"
                       "    return a + b\n"
                       "}\n")
          == 0);
}

TEST_CASE("specific widths do not implicitly cross") {
    auto r = check_detail("func bad(_ x: Int8) -> Int32 { return x }\n");
    CHECK(r.error_count >= 1);
}

// ---- §10 panic / abort / unreachable -------------------------------------

TEST_CASE("panic(msg) type-checks against any return slot (Never is bottom)") {
    CHECK(check_errors("func bad() -> Int32 { panic(\"out of range\") }\n") == 0);
}

TEST_CASE("abort() and unreachable() are zero-arg and return Never") {
    CHECK(check_errors("func a() -> Float64 { abort() }\n"
                       "func u() -> Bool { unreachable() }\n")
          == 0);
}

TEST_CASE("panic requires exactly one StrConst arg") {
    auto r0 = check_detail("func bad() -> Int32 { panic() }\n");
    CHECK(r0.error_count >= 1);
    auto r1 = check_detail("func bad() -> Int32 { panic(\"a\", \"b\") }\n");
    CHECK(r1.error_count >= 1);
}

TEST_CASE("panic admits an interpolated message? no — v0.5 needs a literal") {
    // v0.5 panic takes StrConst; an interpolation produces `String`
    // which doesn't assign. The diagnostic guides the user toward a
    // literal until the @panic_handler annex item widens the surface.
    auto r = check_detail("func bad(_ n: Int32) -> Int32 { panic(\"err \\(n)\") }\n");
    CHECK(r.error_count >= 1);
}

// ---- §12.3 derive(Default) ------------------------------------------------

TEST_CASE("derive(Default) surfaces a static T.default() with primitive fields") {
    CHECK(check_errors("struct Point { var x: Int32\n    var y: Int32 }\n"
                       "derive(Default) for Point\n"
                       "func zero() -> Point { return Point.default() }\n")
          == 0);
}

TEST_CASE("derive(Default) composes through nested struct fields") {
    CHECK(check_errors("struct Inner { var n: Int32 }\n"
                       "derive(Default) for Inner\n"
                       "struct Outer { var a: Int32\n    var b: Inner }\n"
                       "derive(Default) for Outer\n"
                       "func z() -> Outer { return Outer.default() }\n")
          == 0);
}

TEST_CASE("derive(Default) accepts Optional fields (default is nil)") {
    CHECK(check_errors("struct Cell { var label: Str?\n    var count: Int32 }\n"
                       "derive(Default) for Cell\n"
                       "func empty() -> Cell { return Cell.default() }\n")
          == 0);
}

TEST_CASE("derive(Default) rejects a struct with a non-conformant field") {
    auto r = check_detail("struct Inner { var n: Int32 }\n"
                          "struct Outer { var a: Int32\n    var b: Inner }\n"
                          "derive(Default) for Outer\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("Default-conformant") != std::string::npos);
    CHECK(r.first_message.find("Inner") != std::string::npos);
}

TEST_CASE("without derive(Default), T.default() is rejected") {
    auto r = check_detail("struct Point { var x: Int32 }\n"
                          "func zero() -> Point { return Point.default() }\n");
    CHECK(r.error_count >= 1);
    CHECK(r.first_message.find("default") != std::string::npos);
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
