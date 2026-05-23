// SPDX-License-Identifier: BSD-2-Clause
// Copyright (c) 2026 James Kane

#pragma once

#include "vestra/diag/diagnostic.hpp"
#include "vestra/diag/source_manager.hpp"
#include "vestra/lex/token.hpp"

#include <cstddef>
#include <string_view>
#include <vector>

namespace vestra::lex {

// Tokenize one source file end-to-end. Emits a `Newline` token only at points
// where it is a statement terminator under §3's continuation rules (newline
// inside ()/[]/{} suppressed; newline at an operator boundary suppressed).
// String literals containing `\(...)` are emitted as a sequence of
// `StringStart`/`StringMid`*/`StringEnd` plus the spliced expressions between.
//
// Lexical errors are pushed onto the reporter and the corresponding token is
// emitted as `TokenKind::Error` so the parser can resync without losing layout.
class Lexer {
public:
    Lexer(const diag::SourceManager& sm, diag::FileId file, diag::DiagnosticReporter& reporter);

    Lexer(const Lexer&) = delete;
    Lexer& operator=(const Lexer&) = delete;

    // Tokenize and return the full token stream, ending with TokenKind::Eof.
    std::vector<Token> tokenize();

private:
    // Single-character predicates over the input alphabet.
    [[nodiscard]] static bool is_id_start(char c) noexcept;
    [[nodiscard]] static bool is_id_cont(char c) noexcept;
    [[nodiscard]] static bool is_digit(char c) noexcept;
    [[nodiscard]] static bool is_hex(char c) noexcept;
    [[nodiscard]] static bool is_oct(char c) noexcept;
    [[nodiscard]] static bool is_bin(char c) noexcept;

    // Position management.
    [[nodiscard]] bool at_end() const noexcept;
    [[nodiscard]] char peek(std::size_t ahead = 0) const noexcept;
    char advance() noexcept;
    bool match(char expected) noexcept;

    [[nodiscard]] diag::SourceLoc loc_at(std::size_t off) const noexcept;
    [[nodiscard]] diag::SourceRange range_from(std::size_t start) const noexcept;

    // Emit a token over [start, pos_).
    void emit(std::vector<Token>& out, TokenKind k, std::size_t start);
    // Same, but the token is a no-payload kind we want to attach a specific lexeme to.
    void
    emit_with_text(std::vector<Token>& out, TokenKind k, std::size_t start, std::string_view text);

    // Skippers / scanners.
    void skip_whitespace_and_comments(std::vector<Token>& out);
    void scan_block_comment();
    void scan_number(std::vector<Token>& out, std::size_t start);
    void scan_identifier(std::vector<Token>& out, std::size_t start);
    void scan_string(std::vector<Token>& out, std::size_t start, bool is_byte);
    // §4 interpolated string continuation: re-enters string-fragment mode
    // after a splice's closing `)`. Emits InterpStringPart/InterpStringEnd
    // tokens and, if it hits another `\(`, returns to splice mode.
    void scan_interp_continue(std::vector<Token>& out);
    void scan_char(std::vector<Token>& out, std::size_t start);

    void error_at(diag::SourceRange r, std::string msg);

    diag::FileId file_;
    diag::DiagnosticReporter* reporter_;
    std::string_view src_;
    std::size_t pos_ = 0;

    // §3 newline-as-terminator state.
    int bracket_depth_ = 0;           // depth of any of ( [ {
    bool prev_was_operator_ = false;  // tracks whether the most-recent emitted token
                                      // ends with a binary operator (§3 continuation)

    // §4 string-interpolation splice stack. Each entry is the
    // bracket_depth_ value *at the moment* a splice `\(` opened. A `)`
    // that would drop bracket_depth_ below the top of this stack closes
    // the splice and re-enters string-fragment mode. A stack (not a
    // single int) so nested splices via inner interpolated strings
    // (`"a\("b\(x)c")d"`) work.
    std::vector<int> splice_depths_;
};

}  // namespace vestra::lex
