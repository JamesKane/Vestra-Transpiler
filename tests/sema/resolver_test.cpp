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
