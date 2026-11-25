#ifndef ZEPHYR_SQL_PARSER_HPP
#define ZEPHYR_SQL_PARSER_HPP

#include "zephyr/sql/ast.hpp"
#include "zephyr/sql/lexer.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace zephyr::sql {

struct ParseResult {
    std::vector<Stmt> stmts;
    std::string error;

    bool ok() const noexcept { return error.empty(); }
};

class Parser {
public:
    explicit Parser(std::string_view sql);

    ParseResult parse();

private:
    const Token& peek(size_t ahead = 0) const noexcept;
    const Token& advance() noexcept;
    bool check(TokenKind kind) const noexcept;
    bool match(TokenKind kind) noexcept;
    bool expect(TokenKind kind, const char* msg);
    void set_error(const std::string& msg);
    bool has_error() const noexcept { return !error_.empty(); }

    std::optional<Stmt> parse_statement();
    std::optional<CreateTableStmt> parse_create_table();
    std::optional<DropTableStmt> parse_drop_table();
    std::optional<InsertStmt> parse_insert();
    std::optional<UpdateStmt> parse_update();
    std::optional<DeleteStmt> parse_delete();
    std::optional<SelectStmt> parse_select(bool allow_with = true);
    std::optional<TxnStmt> parse_txn();

    std::optional<ColumnType> parse_column_type();
    std::optional<std::vector<SelectItem>> parse_select_list();
    std::optional<SelectItem> parse_select_item();
    std::optional<TableRef> parse_table_ref();
    std::optional<std::vector<SelectItem>> parse_returning();

    ExprPtr parse_expr();
    ExprPtr parse_or();
    ExprPtr parse_and();
    ExprPtr parse_not();
    ExprPtr parse_comparison();
    ExprPtr parse_additive();
    ExprPtr parse_multiplicative();
    ExprPtr parse_unary();
    ExprPtr parse_primary();

    std::string extract_ident(const Token& tok) const;
    bool is_ident_token(TokenKind kind) const noexcept;

    std::vector<Token> tokens_;
    size_t pos_{0};
    std::string error_;
};

inline ParseResult parse_sql(std::string_view sql) {
    return Parser(sql).parse();
}

} // namespace zephyr::sql

#endif // ZEPHYR_SQL_PARSER_HPP
