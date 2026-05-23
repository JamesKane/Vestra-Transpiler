#include "vestra/lex/lexer.hpp"

#include <cassert>
#include <format>
#include <string>
#include <string_view>
#include <utility>

namespace vestra::lex {

namespace {

// Operators whose presence at end-of-line (or start-of-next-line) suppresses
// the newline (§3). Not every TokenKind belongs here — the rule is "binary
// operator," so unary `!` / `~` / `?` are not included.
bool is_binary_operator(TokenKind k) noexcept {
    switch (k) {
    case TokenKind::Plus:
    case TokenKind::Minus:
    case TokenKind::Star:
    case TokenKind::Slash:
    case TokenKind::Percent:
    case TokenKind::AmpPlus:
    case TokenKind::AmpMinus:
    case TokenKind::AmpStar:
    case TokenKind::Amp:
    case TokenKind::Pipe:
    case TokenKind::Caret:
    case TokenKind::ShiftL:
    case TokenKind::ShiftR:
    case TokenKind::EqEq:
    case TokenKind::BangEq:
    case TokenKind::Lt:
    case TokenKind::Gt:
    case TokenKind::LtEq:
    case TokenKind::GtEq:
    case TokenKind::AmpAmp:
    case TokenKind::PipePipe:
    case TokenKind::QuestionQuestion:
    case TokenKind::Assign:
    case TokenKind::PlusEq:
    case TokenKind::MinusEq:
    case TokenKind::StarEq:
    case TokenKind::SlashEq:
    case TokenKind::PercentEq:
    case TokenKind::AmpEq:
    case TokenKind::PipeEq:
    case TokenKind::CaretEq:
    case TokenKind::ShiftLEq:
    case TokenKind::ShiftREq:
    case TokenKind::Arrow:
    case TokenKind::FatArrow:
    case TokenKind::Comma:
    case TokenKind::Colon:
    case TokenKind::DotDot:
    case TokenKind::DotDotLt:
        return true;
    default:
        return false;
    }
}

}  // namespace

Lexer::Lexer(const diag::SourceManager& sm, diag::FileId file, diag::DiagnosticReporter& reporter)
    : file_(file), reporter_(&reporter), src_(sm.source(file)) {}

bool Lexer::is_id_start(char c) noexcept {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}
bool Lexer::is_id_cont(char c) noexcept {
    return is_id_start(c) || is_digit(c);
}
bool Lexer::is_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}
bool Lexer::is_hex(char c) noexcept {
    return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}
bool Lexer::is_oct(char c) noexcept {
    return c >= '0' && c <= '7';
}
bool Lexer::is_bin(char c) noexcept {
    return c == '0' || c == '1';
}

bool Lexer::at_end() const noexcept {
    return pos_ >= src_.size();
}

char Lexer::peek(std::size_t ahead) const noexcept {
    auto p = pos_ + ahead;
    return p < src_.size() ? src_[p] : '\0';
}

char Lexer::advance() noexcept {
    assert(!at_end());
    return src_[pos_++];
}

bool Lexer::match(char expected) noexcept {
    if (peek() != expected) {
        return false;
    }
    ++pos_;
    return true;
}

diag::SourceLoc Lexer::loc_at(std::size_t off) const noexcept {
    return {file_, off};
}

diag::SourceRange Lexer::range_from(std::size_t start) const noexcept {
    return {loc_at(start), pos_ - start};
}

void Lexer::emit(std::vector<Token>& out, TokenKind k, std::size_t start) {
    Token t;
    t.kind = k;
    t.range = range_from(start);
    t.lexeme = src_.substr(start, pos_ - start);
    out.push_back(t);
    if (k != TokenKind::Newline) {
        prev_was_operator_ = is_binary_operator(k);
    }
}

void Lexer::emit_with_text(std::vector<Token>& out,
                           TokenKind k,
                           std::size_t start,
                           std::string_view text) {
    Token t;
    t.kind = k;
    t.range = range_from(start);
    t.lexeme = text;
    out.push_back(t);
    if (k != TokenKind::Newline) {
        prev_was_operator_ = is_binary_operator(k);
    }
}

void Lexer::error_at(diag::SourceRange r, std::string msg) {
    reporter_->report(diag::Diagnostic::error(std::move(msg)).at(r));
}

void Lexer::scan_block_comment() {
    // /* ... */ — nests per §3.
    int depth = 1;
    while (depth > 0 && !at_end()) {
        if (peek() == '/' && peek(1) == '*') {
            pos_ += 2;
            ++depth;
        } else if (peek() == '*' && peek(1) == '/') {
            pos_ += 2;
            --depth;
        } else {
            ++pos_;
        }
    }
    if (depth != 0) {
        error_at({loc_at(pos_), 0}, "unterminated block comment");
    }
}

void Lexer::skip_whitespace_and_comments(std::vector<Token>& out) {
    while (!at_end()) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r') {
            ++pos_;
        } else if (c == '\n') {
            std::size_t start = pos_;
            ++pos_;
            // §3 continuation: suppress the newline if we're inside brackets or
            // immediately after a binary operator.
            if (bracket_depth_ > 0 || prev_was_operator_) {
                continue;
            }
            // Peek past horizontal whitespace + comments to see if the *next*
            // significant token starts with a binary operator. If so, this
            // newline also doesn't terminate.
            std::size_t scan = pos_;
            while (scan < src_.size()) {
                char d = src_[scan];
                if (d == ' ' || d == '\t' || d == '\r' || d == '\n') {
                    ++scan;
                    continue;
                }
                // Skip an inline comment so it doesn't fool the lookahead.
                if (d == '/' && scan + 1 < src_.size() && src_[scan + 1] == '/') {
                    while (scan < src_.size() && src_[scan] != '\n') {
                        ++scan;
                    }
                    continue;
                }
                break;
            }
            bool next_starts_with_operator = false;
            if (scan < src_.size()) {
                char d = src_[scan];
                // A conservative list: punctuation that's only ever a binary
                // operator at the start of a line.
                next_starts_with_operator =
                    (d == '+' || d == '-' || d == '*' || d == '/' || d == '%' || d == '|'
                     || d == '^' || d == '?' || d == '.');
                // `&&` and `||` and `==` are also binary-only at line start —
                // a one-character check is enough because the second char
                // doesn't change the leading category.
            }
            if (!next_starts_with_operator) {
                emit_with_text(out, TokenKind::Newline, start, "\n");
            }
        } else if (c == '/' && peek(1) == '/') {
            // Line comment.
            while (!at_end() && peek() != '\n') {
                ++pos_;
            }
        } else if (c == '/' && peek(1) == '*') {
            pos_ += 2;
            scan_block_comment();
        } else {
            return;
        }
    }
}

void Lexer::scan_number(std::vector<Token>& out, std::size_t start) {
    auto eat_digits = [&](bool (*pred)(char)) {
        while (!at_end() && (pred(peek()) || peek() == '_')) {
            ++pos_;
        }
    };

    bool is_float = false;
    if (src_[start] == '0' && (peek() == 'x' || peek() == 'o' || peek() == 'b')) {
        char base = advance();
        if (base == 'x') {
            eat_digits(&Lexer::is_hex);
        } else if (base == 'o') {
            eat_digits(&Lexer::is_oct);
        } else {
            eat_digits(&Lexer::is_bin);
        }
    } else {
        eat_digits(&Lexer::is_digit);
        // Fractional part — but only if not a `.` followed by an identifier
        // (method call) or another `.` (range operator).
        if (peek() == '.' && is_digit(peek(1))) {
            is_float = true;
            advance();
            eat_digits(&Lexer::is_digit);
        }
        // Exponent.
        if (peek() == 'e' || peek() == 'E') {
            is_float = true;
            advance();
            if (peek() == '+' || peek() == '-') {
                advance();
            }
            eat_digits(&Lexer::is_digit);
        }
    }
    emit(out, is_float ? TokenKind::FloatLit : TokenKind::IntLit, start);
}

void Lexer::scan_identifier(std::vector<Token>& out, std::size_t start) {
    while (!at_end() && is_id_cont(peek())) {
        ++pos_;
    }
    auto lex = src_.substr(start, pos_ - start);
    TokenKind kind = keyword_or_identifier(lex);

    // `try`/`as` may be followed by `?` or `!` per §17.5; combine into one
    // token so the parser doesn't have to peek across whitespace.
    if (kind == TokenKind::KwTry) {
        if (peek() == '?') {
            advance();
            kind = TokenKind::KwTryQuestion;
        } else if (peek() == '!') {
            advance();
            kind = TokenKind::KwTryBang;
        }
    } else if (kind == TokenKind::KwAs && peek() == '?') {
        advance();
        kind = TokenKind::KwAsQuestion;
    }

    // A bare `_` is the wildcard, not an identifier.
    if (kind == TokenKind::Identifier && lex == "_") {
        kind = TokenKind::Underscore;
    }
    emit(out, kind, start);
}

void Lexer::scan_string(std::vector<Token>& out, std::size_t start, bool is_byte) {
    // Interpolation splitting (`"x = \(value)"` → fragment + splice + fragment)
    // is a known gap: we currently lex the whole literal as one StringLit and
    // the parser treats it as a runtime String per §4. Add interpolation
    // tokens here when string-interpolation lowering lands.
    while (!at_end()) {
        char c = peek();
        if (c == '"') {
            advance();
            emit(out, is_byte ? TokenKind::ByteStringLit : TokenKind::StringLit, start);
            return;
        }
        if (c == '\\') {
            advance();
            if (!at_end()) {
                advance();  // consume the escape character verbatim for now
            }
            continue;
        }
        if (c == '\n') {
            error_at({loc_at(pos_), 1}, "unterminated string literal");
            emit(out, TokenKind::Error, start);
            return;
        }
        advance();
    }
    error_at({loc_at(pos_), 0}, "unterminated string literal at end of file");
    emit(out, TokenKind::Error, start);
}

void Lexer::scan_char(std::vector<Token>& out, std::size_t start) {
    if (peek() == '\\') {
        advance();
        if (!at_end()) {
            advance();
        }
    } else if (!at_end() && peek() != '\'') {
        advance();
    }
    if (peek() != '\'') {
        error_at(range_from(start), "unterminated character literal");
        emit(out, TokenKind::Error, start);
        return;
    }
    advance();
    emit(out, TokenKind::CharLit, start);
}

std::vector<Token> Lexer::tokenize() {
    std::vector<Token> out;
    out.reserve(src_.size() / 4 + 8);

    while (!at_end()) {
        skip_whitespace_and_comments(out);
        if (at_end()) {
            break;
        }
        std::size_t start = pos_;
        char c = advance();

        switch (c) {
        case '(':
            ++bracket_depth_;
            emit(out, TokenKind::LParen, start);
            break;
        case ')':
            --bracket_depth_;
            emit(out, TokenKind::RParen, start);
            break;
        case '[':
            ++bracket_depth_;
            emit(out, TokenKind::LBracket, start);
            break;
        case ']':
            --bracket_depth_;
            emit(out, TokenKind::RBracket, start);
            break;
        case '{':
            ++bracket_depth_;
            emit(out, TokenKind::LBrace, start);
            break;
        case '}':
            --bracket_depth_;
            emit(out, TokenKind::RBrace, start);
            break;
        case ',':
            emit(out, TokenKind::Comma, start);
            break;
        case ';':
            emit(out, TokenKind::Semicolon, start);
            break;
        case '@':
            emit(out, TokenKind::At, start);
            break;
        case '~':
            emit(out, TokenKind::Tilde, start);
            break;

        case ':':
            emit(out, TokenKind::Colon, start);
            break;

        case '.':
            if (match('.')) {
                if (match('<')) {
                    emit(out, TokenKind::DotDotLt, start);
                } else {
                    emit(out, TokenKind::DotDot, start);
                }
            } else {
                emit(out, TokenKind::Dot, start);
            }
            break;

        case '?':
            if (match('?')) {
                emit(out, TokenKind::QuestionQuestion, start);
            } else {
                emit(out, TokenKind::Question, start);
            }
            break;

        case '+':
            if (match('=')) {
                emit(out, TokenKind::PlusEq, start);
            } else {
                emit(out, TokenKind::Plus, start);
            }
            break;
        case '-':
            if (match('=')) {
                emit(out, TokenKind::MinusEq, start);
            } else if (match('>')) {
                emit(out, TokenKind::Arrow, start);
            } else {
                emit(out, TokenKind::Minus, start);
            }
            break;
        case '*':
            if (match('=')) {
                emit(out, TokenKind::StarEq, start);
            } else {
                emit(out, TokenKind::Star, start);
            }
            break;
        case '/':
            // line/block comments handled by skip_whitespace_and_comments.
            if (match('=')) {
                emit(out, TokenKind::SlashEq, start);
            } else {
                emit(out, TokenKind::Slash, start);
            }
            break;
        case '%':
            if (match('=')) {
                emit(out, TokenKind::PercentEq, start);
            } else {
                emit(out, TokenKind::Percent, start);
            }
            break;
        case '^':
            if (match('=')) {
                emit(out, TokenKind::CaretEq, start);
            } else {
                emit(out, TokenKind::Caret, start);
            }
            break;

        case '&':
            if (match('&')) {
                emit(out, TokenKind::AmpAmp, start);
            } else if (match('=')) {
                emit(out, TokenKind::AmpEq, start);
            } else if (match('+')) {
                emit(out, TokenKind::AmpPlus, start);
            } else if (match('-')) {
                emit(out, TokenKind::AmpMinus, start);
            } else if (match('*')) {
                emit(out, TokenKind::AmpStar, start);
            } else {
                emit(out, TokenKind::Amp, start);
            }
            break;
        case '|':
            if (match('|')) {
                emit(out, TokenKind::PipePipe, start);
            } else if (match('=')) {
                emit(out, TokenKind::PipeEq, start);
            } else {
                emit(out, TokenKind::Pipe, start);
            }
            break;

        case '=':
            if (match('=')) {
                emit(out, TokenKind::EqEq, start);
            } else if (match('>')) {
                emit(out, TokenKind::FatArrow, start);
            } else {
                emit(out, TokenKind::Assign, start);
            }
            break;
        case '!':
            if (match('=')) {
                emit(out, TokenKind::BangEq, start);
            } else {
                emit(out, TokenKind::Bang, start);
            }
            break;
        case '<':
            if (match('<')) {
                if (match('=')) {
                    emit(out, TokenKind::ShiftLEq, start);
                } else {
                    emit(out, TokenKind::ShiftL, start);
                }
            } else if (match('=')) {
                emit(out, TokenKind::LtEq, start);
            } else {
                emit(out, TokenKind::Lt, start);
            }
            break;
        case '>':
            if (match('>')) {
                if (match('=')) {
                    emit(out, TokenKind::ShiftREq, start);
                } else {
                    emit(out, TokenKind::ShiftR, start);
                }
            } else if (match('=')) {
                emit(out, TokenKind::GtEq, start);
            } else {
                emit(out, TokenKind::Gt, start);
            }
            break;

        case '"':
            scan_string(out, start, /*is_byte=*/false);
            break;
        case '\'':
            scan_char(out, start);
            break;

        default:
            if (c == 'b' && peek() == '"') {
                advance();
                scan_string(out, start, /*is_byte=*/true);
            } else if (is_digit(c)) {
                scan_number(out, start);
            } else if (is_id_start(c)) {
                scan_identifier(out, start);
            } else {
                error_at(range_from(start), std::format("unexpected character '{}'", c));
                emit(out, TokenKind::Error, start);
            }
            break;
        }

        if (reporter_->should_stop()) {
            break;
        }
    }

    Token eof;
    eof.kind = TokenKind::Eof;
    eof.range = {loc_at(pos_), 0};
    eof.lexeme = {};
    out.push_back(eof);
    return out;
}

}  // namespace vestra::lex
