#include "zephyr/sql/lexer.hpp"

#include <cctype>

namespace zephyr::sql {

namespace {

inline bool is_ident_start(char c) noexcept {
    unsigned char u = static_cast<unsigned char>(c);
    return std::isalpha(u) || u == '_';
}

inline bool is_ident_cont(char c) noexcept {
    unsigned char u = static_cast<unsigned char>(c);
    return std::isalnum(u) || u == '_' || u == '$';
}

inline bool is_digit(char c) noexcept {
    return c >= '0' && c <= '9';
}

inline bool iequals(std::string_view a, std::string_view b) noexcept {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        char ca = a[i];
        char cb = b[i];
        if (ca >= 'a' && ca <= 'z') ca = static_cast<char>(ca - 'a' + 'A');
        if (cb >= 'a' && cb <= 'z') cb = static_cast<char>(cb - 'a' + 'A');
        if (ca != cb) return false;
    }
    return true;
}

TokenKind lookup_keyword(std::string_view word) noexcept {
    struct KwEntry {
        const char* name;
        size_t len;
        TokenKind kind;
    };
    static constexpr KwEntry kKeywords[] = {
        {"CREATE", 6, TokenKind::KwCreate},
        {"DROP", 4, TokenKind::KwDrop},
        {"TABLE", 5, TokenKind::KwTable},
        {"IF", 2, TokenKind::KwIf},
        {"NOT", 3, TokenKind::KwNot},
        {"EXISTS", 6, TokenKind::KwExists},
        {"PRIMARY", 7, TokenKind::KwPrimary},
        {"KEY", 3, TokenKind::KwKey},
        {"INSERT", 6, TokenKind::KwInsert},
        {"INTO", 4, TokenKind::KwInto},
        {"VALUES", 6, TokenKind::KwValues},
        {"RETURNING", 9, TokenKind::KwReturning},
        {"UPDATE", 6, TokenKind::KwUpdate},
        {"SET", 3, TokenKind::KwSet},
        {"DELETE", 6, TokenKind::KwDelete},
        {"FROM", 4, TokenKind::KwFrom},
        {"WITH", 4, TokenKind::KwWith},
        {"AS", 2, TokenKind::KwAs},
        {"SELECT", 6, TokenKind::KwSelect},
        {"DISTINCT", 8, TokenKind::KwDistinct},
        {"JOIN", 4, TokenKind::KwJoin},
        {"INNER", 5, TokenKind::KwInner},
        {"LEFT", 4, TokenKind::KwLeft},
        {"ON", 2, TokenKind::KwOn},
        {"WHERE", 5, TokenKind::KwWhere},
        {"AND", 3, TokenKind::KwAnd},
        {"OR", 2, TokenKind::KwOr},
        {"IS", 2, TokenKind::KwIs},
        {"NULL", 4, TokenKind::KwNull},
        {"BETWEEN", 7, TokenKind::KwBetween},
        {"GROUP", 5, TokenKind::KwGroup},
        {"BY", 2, TokenKind::KwBy},
        {"ORDER", 5, TokenKind::KwOrder},
        {"ASC", 3, TokenKind::KwAsc},
        {"DESC", 4, TokenKind::KwDesc},
        {"LIMIT", 5, TokenKind::KwLimit},
        {"OFFSET", 6, TokenKind::KwOffset},
        {"BEGIN", 5, TokenKind::KwBegin},
        {"TRANSACTION", 11, TokenKind::KwTransaction},
        {"COMMIT", 6, TokenKind::KwCommit},
        {"ROLLBACK", 8, TokenKind::KwRollback},
        {"BIGINT", 6, TokenKind::KwBigint},
        {"INT", 3, TokenKind::KwInt},
        {"INTEGER", 7, TokenKind::KwInteger},
        {"DOUBLE", 6, TokenKind::KwDouble},
        {"PRECISION", 9, TokenKind::KwPrecision},
        {"FLOAT8", 6, TokenKind::KwFloat8},
        {"BOOLEAN", 7, TokenKind::KwBoolean},
        {"BOOL", 4, TokenKind::KwBool},
        {"TEXT", 4, TokenKind::KwText},
        {"VARCHAR", 7, TokenKind::KwVarchar},
        {"BYTEA", 5, TokenKind::KwBytea},
        {"COUNT", 5, TokenKind::KwCount},
        {"SUM", 3, TokenKind::KwSum},
        {"MIN", 3, TokenKind::KwMin},
        {"MAX", 3, TokenKind::KwMax},
        {"AVG", 3, TokenKind::KwAvg},
        {"TRUE", 4, TokenKind::KwTrue},
        {"FALSE", 5, TokenKind::KwFalse},
    };
    for (const auto& kw : kKeywords) {
        if (word.size() == kw.len && iequals(word, std::string_view(kw.name, kw.len))) {
            return kw.kind;
        }
    }
    return TokenKind::Ident;
}

} // namespace

Lexer::Lexer(std::string_view input) noexcept : input_(input) {}

char Lexer::peek(size_t ahead) const noexcept {
    if (pos_ + ahead >= input_.size()) return '\0';
    return input_[pos_ + ahead];
}

char Lexer::advance() noexcept {
    if (pos_ >= input_.size()) return '\0';
    char c = input_[pos_++];
    if (c == '\n') {
        ++line_;
        col_ = 1;
    } else {
        ++col_;
    }
    return c;
}

void Lexer::skip_whitespace_and_comments() noexcept {
    while (pos_ < input_.size()) {
        char c = peek();
        if (c == ' ' || c == '\t' || c == '\r' || c == '\n') {
            advance();
            continue;
        }
        // Line comment: -- ...
        if (c == '-' && peek(1) == '-') {
            advance();
            advance();
            while (pos_ < input_.size() && peek() != '\n') {
                advance();
            }
            continue;
        }
        // Block comment: /* ... */
        if (c == '/' && peek(1) == '*') {
            advance();
            advance();
            while (pos_ < input_.size()) {
                if (peek() == '*' && peek(1) == '/') {
                    advance();
                    advance();
                    break;
                }
                advance();
            }
            continue;
        }
        break;
    }
}

Token Lexer::lex_identifier_or_keyword() noexcept {
    uint32_t start_off = static_cast<uint32_t>(pos_);
    uint32_t start_line = line_;
    uint32_t start_col = col_;
    while (pos_ < input_.size() && is_ident_cont(peek())) {
        advance();
    }
    std::string_view word = input_.substr(start_off, pos_ - start_off);
    return Token{lookup_keyword(word), word, start_off, start_line, start_col};
}

Token Lexer::lex_quoted_ident() noexcept {
    uint32_t start_off = static_cast<uint32_t>(pos_);
    uint32_t start_line = line_;
    uint32_t start_col = col_;
    advance(); // consume opening "
    size_t content_start = pos_;
    while (pos_ < input_.size()) {
        if (peek() == '"') {
            if (peek(1) == '"') {
                advance();
                advance();
                continue;
            }
            std::string_view inner = input_.substr(content_start, pos_ - content_start);
            advance(); // consume closing "
            return Token{TokenKind::QuotedIdent, inner, start_off, start_line, start_col};
        }
        advance();
    }
    return Token{TokenKind::Error, "unterminated quoted identifier", start_off, start_line, start_col};
}

Token Lexer::lex_string_lit() noexcept {
    uint32_t start_off = static_cast<uint32_t>(pos_);
    uint32_t start_line = line_;
    uint32_t start_col = col_;
    advance(); // consume opening '
    size_t content_start = pos_;
    while (pos_ < input_.size()) {
        if (peek() == '\'') {
            if (peek(1) == '\'') {
                advance();
                advance();
                continue;
            }
            std::string_view inner = input_.substr(content_start, pos_ - content_start);
            advance(); // consume closing '
            return Token{TokenKind::StringLit, inner, start_off, start_line, start_col};
        }
        advance();
    }
    return Token{TokenKind::Error, "unterminated string literal", start_off, start_line, start_col};
}

Token Lexer::lex_number() noexcept {
    uint32_t start_off = static_cast<uint32_t>(pos_);
    uint32_t start_line = line_;
    uint32_t start_col = col_;
    bool is_float = false;

    while (is_digit(peek())) {
        advance();
    }
    if (peek() == '.' && is_digit(peek(1))) {
        is_float = true;
        advance(); // '.'
        while (is_digit(peek())) {
            advance();
        }
    }
    if (peek() == 'e' || peek() == 'E') {
        char next_c = peek(1);
        if (is_digit(next_c) || ((next_c == '+' || next_c == '-') && is_digit(peek(2)))) {
            is_float = true;
            advance(); // 'e'/'E'
            if (peek() == '+' || peek() == '-') advance();
            while (is_digit(peek())) advance();
        }
    }
    std::string_view text = input_.substr(start_off, pos_ - start_off);
    return Token{is_float ? TokenKind::FloatLit : TokenKind::IntLit, text, start_off, start_line, start_col};
}

Token Lexer::lex_param() noexcept {
    uint32_t start_off = static_cast<uint32_t>(pos_);
    uint32_t start_line = line_;
    uint32_t start_col = col_;
    advance(); // '$'
    if (!is_digit(peek())) {
        return Token{TokenKind::Error, "invalid parameter token '$'", start_off, start_line, start_col};
    }
    while (is_digit(peek())) {
        advance();
    }
    std::string_view text = input_.substr(start_off, pos_ - start_off);
    return Token{TokenKind::Param, text, start_off, start_line, start_col};
}

Token Lexer::next() {
    skip_whitespace_and_comments();
    if (pos_ >= input_.size()) {
        return Token{TokenKind::Eof, {}, static_cast<uint32_t>(pos_), line_, col_};
    }

    char c = peek();
    if (is_ident_start(c)) return lex_identifier_or_keyword();
    if (is_digit(c)) return lex_number();
    if (c == '\'') return lex_string_lit();
    if (c == '"') return lex_quoted_ident();
    if (c == '$') return lex_param();

    uint32_t start_off = static_cast<uint32_t>(pos_);
    uint32_t start_line = line_;
    uint32_t start_col = col_;
    advance();

    switch (c) {
        case '(': return Token{TokenKind::LParen, input_.substr(start_off, 1), start_off, start_line, start_col};
        case ')': return Token{TokenKind::RParen, input_.substr(start_off, 1), start_off, start_line, start_col};
        case ',': return Token{TokenKind::Comma, input_.substr(start_off, 1), start_off, start_line, start_col};
        case ';': return Token{TokenKind::Semicolon, input_.substr(start_off, 1), start_off, start_line, start_col};
        case '.': return Token{TokenKind::Dot, input_.substr(start_off, 1), start_off, start_line, start_col};
        case '+': return Token{TokenKind::Plus, input_.substr(start_off, 1), start_off, start_line, start_col};
        case '-': return Token{TokenKind::Minus, input_.substr(start_off, 1), start_off, start_line, start_col};
        case '*': return Token{TokenKind::Star, input_.substr(start_off, 1), start_off, start_line, start_col};
        case '/': return Token{TokenKind::Slash, input_.substr(start_off, 1), start_off, start_line, start_col};
        case '=': return Token{TokenKind::Eq, input_.substr(start_off, 1), start_off, start_line, start_col};
        case '!':
            if (peek() == '=') {
                advance();
                return Token{TokenKind::Ne, input_.substr(start_off, 2), start_off, start_line, start_col};
            }
            return Token{TokenKind::Error, "unexpected '!'", start_off, start_line, start_col};
        case '<':
            if (peek() == '=') {
                advance();
                return Token{TokenKind::Le, input_.substr(start_off, 2), start_off, start_line, start_col};
            }
            if (peek() == '>') {
                advance();
                return Token{TokenKind::Ne, input_.substr(start_off, 2), start_off, start_line, start_col};
            }
            return Token{TokenKind::Lt, input_.substr(start_off, 1), start_off, start_line, start_col};
        case '>':
            if (peek() == '=') {
                advance();
                return Token{TokenKind::Ge, input_.substr(start_off, 2), start_off, start_line, start_col};
            }
            return Token{TokenKind::Gt, input_.substr(start_off, 1), start_off, start_line, start_col};
        default:
            return Token{TokenKind::Error, "unexpected character", start_off, start_line, start_col};
    }
}

std::vector<Token> Lexer::tokenize_all() {
    std::vector<Token> out;
    while (true) {
        Token t = next();
        out.push_back(t);
        if (t.kind == TokenKind::Eof || t.kind == TokenKind::Error) {
            break;
        }
    }
    return out;
}

std::string Lexer::unescape_single_quoted(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '\'' && i + 1 < raw.size() && raw[i + 1] == '\'') {
            out.push_back('\'');
            ++i;
        } else {
            out.push_back(raw[i]);
        }
    }
    return out;
}

std::string Lexer::unescape_double_quoted(std::string_view raw) {
    std::string out;
    out.reserve(raw.size());
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == '"' && i + 1 < raw.size() && raw[i + 1] == '"') {
            out.push_back('"');
            ++i;
        } else {
            out.push_back(raw[i]);
        }
    }
    return out;
}

} // namespace zephyr::sql
