#ifndef ZEPHYR_SQL_AST_HPP
#define ZEPHYR_SQL_AST_HPP

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace zephyr::sql {

enum class ColumnType : uint8_t {
    Null = 0,
    I64 = 1,
    F64 = 2,
    Bool = 3,
    Text = 4,
    Bytea = 5
};

enum class UnaryOp : uint8_t {
    Neg,
    Not
};

enum class BinaryOp : uint8_t {
    Add,
    Sub,
    Mul,
    Div,
    Eq,
    Ne,
    Lt,
    Le,
    Gt,
    Ge,
    And,
    Or
};

enum class AggFunc : uint8_t {
    Count,
    Sum,
    Min,
    Max,
    Avg
};

struct Expr;
using ExprPtr = std::unique_ptr<Expr>;

struct LiteralExpr {
    using Value = std::variant<std::nullptr_t, int64_t, double, bool, std::string>;
    Value value{nullptr};
};

struct ParamExpr {
    uint32_t index{1}; // 1-based ($1, $2, ...)
};

struct ColumnRefExpr {
    std::string table;  // optional qualifier
    std::string column;
};

struct UnaryExpr {
    UnaryOp op{UnaryOp::Neg};
    ExprPtr operand;
};

struct BinaryExpr {
    BinaryOp op{BinaryOp::Eq};
    ExprPtr left;
    ExprPtr right;
};

struct IsNullExpr {
    ExprPtr operand;
    bool is_not{false};
};

struct BetweenExpr {
    ExprPtr expr;
    ExprPtr low;
    ExprPtr high;
    bool is_not{false};
};

struct AggregateExpr {
    AggFunc func{AggFunc::Count};
    bool is_star{false};
    ExprPtr arg;
};

struct Expr {
    std::variant<
        LiteralExpr,
        ParamExpr,
        ColumnRefExpr,
        UnaryExpr,
        BinaryExpr,
        IsNullExpr,
        BetweenExpr,
        AggregateExpr
    > node;
};

struct ColumnDef {
    std::string name;
    ColumnType type{ColumnType::I64};
    bool primary_key{false};
    bool not_null{false};
};

struct CreateTableStmt {
    bool if_not_exists{false};
    std::string table_name;
    std::vector<ColumnDef> columns;
};

struct DropTableStmt {
    bool if_exists{false};
    std::string table_name;
};

struct SelectItem {
    bool is_star{false};
    std::string table_qualifier; // for t.*
    ExprPtr expr;
    std::string alias;
};

struct InsertStmt {
    std::string table_name;
    std::vector<std::string> columns;
    std::vector<std::vector<ExprPtr>> rows;
    std::vector<SelectItem> returning;
};

struct Assignment {
    std::string column;
    ExprPtr value;
};

struct UpdateStmt {
    std::string table_name;
    std::vector<Assignment> assignments;
    ExprPtr where;
    std::vector<SelectItem> returning;
};

struct DeleteStmt {
    std::string table_name;
    ExprPtr where;
    std::vector<SelectItem> returning;
};

struct TableRef {
    std::string table_name;
    std::string alias;
};

enum class JoinType : uint8_t {
    Inner,
    Left
};

struct JoinClause {
    JoinType type{JoinType::Inner};
    TableRef table;
    ExprPtr on;
};

struct OrderByItem {
    ExprPtr expr;
    bool desc{false};
};

struct SelectStmt;

struct CteDef {
    std::string name;
    std::unique_ptr<SelectStmt> query;
};

struct SelectStmt {
    std::vector<CteDef> ctes;
    bool distinct{false};
    std::vector<SelectItem> items;
    std::optional<TableRef> from;
    std::vector<JoinClause> joins;
    ExprPtr where;
    std::vector<ExprPtr> group_by;
    std::vector<OrderByItem> order_by;
    std::optional<int64_t> limit;
    std::optional<int64_t> offset;
};

struct TxnStmt {
    enum class Kind : uint8_t { Begin, Commit, Rollback };
    Kind kind{Kind::Begin};
};

using Stmt = std::variant<
    CreateTableStmt,
    DropTableStmt,
    InsertStmt,
    UpdateStmt,
    DeleteStmt,
    SelectStmt,
    TxnStmt
>;

} // namespace zephyr::sql

#endif // ZEPHYR_SQL_AST_HPP
