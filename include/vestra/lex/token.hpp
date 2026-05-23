#pragma once

#include "vestra/diag/source_manager.hpp"

#include <cstdint>
#include <string_view>

namespace vestra::lex {

// Token kinds — every terminal in §17.1, plus the keywords listed under the
// "Keywords" paragraph there. The ordering here is not load-bearing; we group
// by category for readability.
//
// NOLINTNEXTLINE(performance-enum-size)
enum class TokenKind : std::uint16_t {
    // ---- bookkeeping ----
    Eof,
    Error,    // a lexer error; the diagnostic was reported, parsing should resync
    Newline,  // §3: a newline that *terminates* a statement; the lexer suppresses
              // the ones that don't (inside ()/[]/{} or at operator boundaries).

    // ---- literals ----
    IntLit,
    FloatLit,
    StringLit,      // "..."
    StringStart,    // "...\(   — opens an interpolated string fragment
    StringMid,      // )...\(   — middle fragment between two splices
    StringEnd,      // )..."    — final fragment of an interpolated string
    ByteStringLit,  // b"..."
    CharLit,        // '.'
    Identifier,

    // ---- punctuation ----
    LParen,
    RParen,
    LBrace,
    RBrace,
    LBracket,
    RBracket,
    Comma,
    Semicolon,
    Colon,
    ColonColon,
    Dot,
    DotDot,
    DotDotLt,    // ., .., ..<
    Ellipsis,    // ...
    Arrow,       // ->
    FatArrow,    // =>
    At,          // @
    Dollar,      // $   (only legal inside `quote`; lexer still produces it)
    Question,    // ?
    Underscore,  // _   (only meaningful as a parameter name / pattern)
    BackTick,    // `   (reserved, unused for now)

    // ---- operators (single, then compound) ----
    Plus,
    Minus,
    Star,
    Slash,
    Percent,
    AmpPlus,
    AmpMinus,
    AmpStar,  // &+ &- &*  (wrapping arithmetic, §4)
    Amp,
    Pipe,
    Caret,
    Tilde,
    Bang,  // & | ^ ~ !
    ShiftL,
    ShiftR,  // << >>

    Assign,  // =
    PlusEq,
    MinusEq,
    StarEq,
    SlashEq,
    PercentEq,
    AmpEq,
    PipeEq,
    CaretEq,
    ShiftLEq,
    ShiftREq,

    EqEq,
    BangEq,
    Lt,
    Gt,
    LtEq,
    GtEq,
    AmpAmp,
    PipePipe,
    QuestionQuestion,  // ??

    // ---- keywords (sorted by category — bookkeeping, then declarations,
    //                then statements, then expressions, then modifiers) ----
    KwModule,
    KwImport,
    KwFunc,
    KwStruct,
    KwEnum,
    KwProtocol,
    KwExtension,
    KwOpaque,
    KwType,
    KwEmbed,
    KwDeinit,
    KwLet,
    KwVar,
    KwConst,
    KwStatic,
    KwReturn,
    KwIf,
    KwElse,
    KwMatch,
    KwCase,
    KwDefault,
    KwFor,
    KwIn,
    KwWhile,
    KwBreak,
    KwContinue,
    KwWhere,
    KwDo,
    KwCatch,
    KwThrow,
    KwThrows,
    KwWith,
    KwUsing,
    KwAsync,
    KwAwait,
    KwSpawn,
    KwSelect,
    KwOn,
    KwComptime,
    KwQuote,
    KwCopy,
    KwRead,
    KwInout,
    KwSink,
    KwNonescaping,
    KwSome,
    KwDyn,
    KwLinear,
    KwTry,
    KwTryQuestion,
    KwTryBang,  // try, try?, try!
    KwAs,
    KwAsQuestion,  // as, as?
    KwDerive,
    KwTrue,
    KwFalse,
    KwSelfLower,
    KwSelfUpper,  // self, Self
    KwPrivate,
    KwInternal,
    KwPackage,
    KwPublic,
};

// A lexed token: kind + the source range it covers + (for literals/identifiers)
// the raw lexeme. The lexeme is a view into the source buffer the lexer was
// fed, so it lives exactly as long as that buffer does.
struct Token {
    TokenKind kind = TokenKind::Eof;
    diag::SourceRange range{};
    std::string_view lexeme{};

    [[nodiscard]] constexpr bool is(TokenKind k) const noexcept { return kind == k; }
};

// Human-readable spelling of a kind — for diagnostics, never for codegen.
[[nodiscard]] std::string_view spelling(TokenKind k) noexcept;

// If `lexeme` matches one of the keywords from §17.1, return the matching
// TokenKind; otherwise return TokenKind::Identifier.
[[nodiscard]] TokenKind keyword_or_identifier(std::string_view lexeme) noexcept;

}  // namespace vestra::lex
