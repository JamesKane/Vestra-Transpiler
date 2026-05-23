#include "vestra/diag/diagnostic.hpp"
#include "vestra/diag/source_manager.hpp"
#include "vestra/lex/lexer.hpp"
#include "vestra/lex/token.hpp"

#include <doctest/doctest.h>

#include <string>
#include <vector>

namespace {

std::vector<vestra::lex::Token>
lex_text(vestra::diag::SourceManager& sm, vestra::diag::DiagnosticReporter& rep, std::string text) {
    auto fid = sm.add_in_memory("<test>", std::move(text));
    vestra::lex::Lexer lex(sm, fid, rep);
    return lex.tokenize();
}

}  // namespace

TEST_CASE("lexer produces EOF for an empty input") {
    vestra::diag::SourceManager sm;
    vestra::diag::DiagnosticReporter rep(sm);
    auto toks = lex_text(sm, rep, "");
    REQUIRE(toks.size() == 1);
    CHECK(toks[0].kind == vestra::lex::TokenKind::Eof);
    CHECK_FALSE(rep.has_errors());
}

TEST_CASE("lexer recognizes keywords and identifiers") {
    vestra::diag::SourceManager sm;
    vestra::diag::DiagnosticReporter rep(sm);
    auto toks = lex_text(sm, rep, "func main let var foo");
    REQUIRE(toks.size() >= 6);
    CHECK(toks[0].kind == vestra::lex::TokenKind::KwFunc);
    CHECK(toks[1].kind == vestra::lex::TokenKind::Identifier);
    CHECK(toks[1].lexeme == "main");
    CHECK(toks[2].kind == vestra::lex::TokenKind::KwLet);
    CHECK(toks[3].kind == vestra::lex::TokenKind::KwVar);
    CHECK(toks[4].kind == vestra::lex::TokenKind::Identifier);
    CHECK(toks[4].lexeme == "foo");
}

TEST_CASE("lexer handles compound operators and `try?` / `as?`") {
    vestra::diag::SourceManager sm;
    vestra::diag::DiagnosticReporter rep(sm);
    auto toks = lex_text(sm, rep, "== != <= >= && || ?? -> => try? as?");
    std::vector<vestra::lex::TokenKind> kinds;
    for (const auto& t : toks) {
        kinds.push_back(t.kind);
    }
    CHECK(kinds[0] == vestra::lex::TokenKind::EqEq);
    CHECK(kinds[1] == vestra::lex::TokenKind::BangEq);
    CHECK(kinds[2] == vestra::lex::TokenKind::LtEq);
    CHECK(kinds[3] == vestra::lex::TokenKind::GtEq);
    CHECK(kinds[4] == vestra::lex::TokenKind::AmpAmp);
    CHECK(kinds[5] == vestra::lex::TokenKind::PipePipe);
    CHECK(kinds[6] == vestra::lex::TokenKind::QuestionQuestion);
    CHECK(kinds[7] == vestra::lex::TokenKind::Arrow);
    CHECK(kinds[8] == vestra::lex::TokenKind::FatArrow);
    CHECK(kinds[9] == vestra::lex::TokenKind::KwTryQuestion);
    CHECK(kinds[10] == vestra::lex::TokenKind::KwAsQuestion);
}

TEST_CASE("numeric literals: underscores, hex, binary, float, exponent") {
    vestra::diag::SourceManager sm;
    vestra::diag::DiagnosticReporter rep(sm);
    auto toks = lex_text(sm, rep, "1_000_000 0xFFFF_0000 0b1010_0011 1.0e-9 3.14");
    REQUIRE(toks.size() >= 5);
    CHECK(toks[0].kind == vestra::lex::TokenKind::IntLit);
    CHECK(toks[0].lexeme == "1_000_000");
    CHECK(toks[1].kind == vestra::lex::TokenKind::IntLit);
    CHECK(toks[1].lexeme == "0xFFFF_0000");
    CHECK(toks[2].kind == vestra::lex::TokenKind::IntLit);
    CHECK(toks[3].kind == vestra::lex::TokenKind::FloatLit);
    CHECK(toks[4].kind == vestra::lex::TokenKind::FloatLit);
    CHECK_FALSE(rep.has_errors());
}

TEST_CASE("block comments nest") {
    vestra::diag::SourceManager sm;
    vestra::diag::DiagnosticReporter rep(sm);
    auto toks = lex_text(sm, rep, "1 /* a /* b */ c */ 2");
    REQUIRE(toks.size() == 3);  // 1, 2, EOF
    CHECK(toks[0].kind == vestra::lex::TokenKind::IntLit);
    CHECK(toks[1].kind == vestra::lex::TokenKind::IntLit);
    CHECK_FALSE(rep.has_errors());
}

TEST_CASE("newlines inside brackets do not terminate statements") {
    vestra::diag::SourceManager sm;
    vestra::diag::DiagnosticReporter rep(sm);
    auto toks = lex_text(sm, rep, "(\n1,\n2\n)\n");
    // We expect: ( IntLit , IntLit ) Newline EOF — the inner newlines should
    // NOT have produced Newline tokens.
    std::size_t newline_count = 0;
    for (const auto& t : toks) {
        if (t.kind == vestra::lex::TokenKind::Newline) {
            ++newline_count;
        }
    }
    CHECK(newline_count == 1);
}
