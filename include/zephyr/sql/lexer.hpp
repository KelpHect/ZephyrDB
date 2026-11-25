#ifndef ZEPHYR_SQL_LEXER_HPP
#define ZEPHYR_SQL_LEXER_HPP

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace zephyr::sql {

enum class TokenKind : uint16_t {
    Eof = 0,
    Error,

    // Identifiers & Literals
    Ident,
    QuotedIdent,
    StringLit,
    IntLit,
    FloatLit,
    Param, // $1, $2, ...

    // Punctuation & Operators
    LParen,    // (
    RParen,    // )
    Comma,     // ,
    Semicolon, // ;
    Dot,       // .
    Plus,      // +
    Minus,     // -
    Star,      // *
    Slash,     // /
    Eq,        // =
    Ne,        // != or <>
    Lt,        // <
    Le,        // <=
    Gt,        // >
    Ge,        // >=

    // Keywords (case-insensitive)
    KwCreate,
    KwDrop,
    KwTable,
    KwIf,
    KwNot,
    KwExists,
    KwPrimary,
    KwKey,
    KwInsert,
    KwInto,
    KwValues,
    KwReturning,
    KwUpdate,
    KwSet,
    KwDelete,
    KwFrom,
    KwWith,
    KwAs,
    KwSelect,
    KwDistinct,
    KwJoin,
    KwInner,
    KwLeft,
    KwOn,
    KwWhere,
    KwAnd,
    KwOr,
    KwIs,
    KwNull,
    KwBetween,
    KwGroup,
    KwBy,
    KwOrder,
    KwAsc,
    KwDesc,
    KwLimit,
    KwOffset,
    KwBegin,
    KwTransaction,
    KwCommit,
    KwRollback,

    // Types
    KwBigint,
    KwInt,
    KwInteger,
    KwDouble,
    KwPrecision,
    KwFloat8,
    KwBoolean,
    KwBool,
    KwText,
    KwVarchar,
    KwBytea,

    // Aggregates & Booleans
    KwCount,
    KwSum,
    KwMin,
    KwMax,
    KwAvg,
    KwTrue,
    KwFalse
};

struct Token {
    TokenKind kind{TokenKind::Eof};
    std::string_view text;
    uint32_t offset{0};
    uint32_t line{1};
    uint32_t col{1};
};

class Lexer {
public:
    explicit Lexer(std::string_view input) noexcept;

    Token next();
    std::vector<Token> tokenize_all();

    static std::string unescape_single_quoted(std::string_view raw);
    static std::string unescape_double_quoted(std::string_view raw);

private:
    void skip_whitespace_and_comments() noexcept;
    Token lex_identifier_or_keyword() noexcept;
    Token lex_quoted_ident() noexcept;
    Token lex_string_lit() noexcept;
    Token lex_number() noexcept;
    Token lex_param() noexcept;

    char peek(size_t ahead = 0) const noexcept;
    char advance() noexcept;

    std::string_view input_;
    size_t pos_{0};
    uint32_t line_{1};
    uint32_t col_{1};
};

} // namespace zephyr::sql

#endif // ZEPHYR_SQL_LEXER_HPP
