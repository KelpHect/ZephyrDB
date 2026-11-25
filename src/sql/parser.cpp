#include "zephyr/sql/parser.hpp"

#include <charconv>
#include <cstdlib>

namespace zephyr::sql {

Parser::Parser(std::string_view sql) {
    Lexer lex(sql);
    tokens_ = lex.tokenize_all();
    if (!tokens_.empty() && tokens_.back().kind == TokenKind::Error) {
        set_error(std::string("lexer error: ") + std::string(tokens_.back().text));
    }
}

const Token& Parser::peek(size_t ahead) const noexcept {
    static const Token kEof{TokenKind::Eof, {}, 0, 0, 0};
    if (pos_ + ahead >= tokens_.size()) return kEof;
    return tokens_[pos_ + ahead];
}

const Token& Parser::advance() noexcept {
    const Token& cur = peek(0);
    if (pos_ + 1 < tokens_.size()) ++pos_;
    return cur;
}

bool Parser::check(TokenKind kind) const noexcept {
    return peek().kind == kind;
}

bool Parser::match(TokenKind kind) noexcept {
    if (check(kind)) {
        advance();
        return true;
    }
    return false;
}

void Parser::set_error(const std::string& msg) {
    if (error_.empty()) {
        const Token& t = peek();
        error_ = "line " + std::to_string(t.line) + ":" + std::to_string(t.col) + ": " + msg;
    }
}

bool Parser::expect(TokenKind kind, const char* msg) {
    if (match(kind)) return true;
    set_error(msg);
    return false;
}

bool Parser::is_ident_token(TokenKind kind) const noexcept {
    return kind == TokenKind::Ident || kind == TokenKind::QuotedIdent;
}

std::string Parser::extract_ident(const Token& tok) const {
    if (tok.kind == TokenKind::QuotedIdent) {
        return Lexer::unescape_double_quoted(tok.text);
    }
    return std::string(tok.text);
}

ParseResult Parser::parse() {
    ParseResult res;
    if (has_error()) {
        res.error = error_;
        return res;
    }

    while (!check(TokenKind::Eof)) {
        while (match(TokenKind::Semicolon)) {}
        if (check(TokenKind::Eof)) break;

        auto stmt = parse_statement();
        if (!stmt || has_error()) {
            res.error = error_.empty() ? "syntax error" : error_;
            return res;
        }
        res.stmts.push_back(std::move(*stmt));

        if (!check(TokenKind::Eof) && !match(TokenKind::Semicolon)) {
            set_error("expected ';' or end of input");
            res.error = error_;
            return res;
        }
    }
    return res;
}

std::optional<Stmt> Parser::parse_statement() {
    switch (peek().kind) {
        case TokenKind::KwCreate: return parse_create_table();
        case TokenKind::KwDrop:   return parse_drop_table();
        case TokenKind::KwInsert: return parse_insert();
        case TokenKind::KwUpdate: return parse_update();
        case TokenKind::KwDelete: return parse_delete();
        case TokenKind::KwWith:
        case TokenKind::KwSelect: return parse_select(true);
        case TokenKind::KwBegin:
        case TokenKind::KwCommit:
        case TokenKind::KwRollback: return parse_txn();
        default:
            set_error("unexpected token at start of statement");
            return std::nullopt;
    }
}

std::optional<TxnStmt> Parser::parse_txn() {
    TxnStmt stmt;
    if (match(TokenKind::KwBegin)) {
        match(TokenKind::KwTransaction);
        stmt.kind = TxnStmt::Kind::Begin;
        return stmt;
    }
    if (match(TokenKind::KwCommit)) {
        stmt.kind = TxnStmt::Kind::Commit;
        return stmt;
    }
    if (match(TokenKind::KwRollback)) {
        stmt.kind = TxnStmt::Kind::Rollback;
        return stmt;
    }
    set_error("expected transaction statement");
    return std::nullopt;
}

std::optional<ColumnType> Parser::parse_column_type() {
    if (match(TokenKind::KwBigint) || match(TokenKind::KwInt) || match(TokenKind::KwInteger)) {
        return ColumnType::I64;
    }
    if (match(TokenKind::KwFloat8)) {
        return ColumnType::F64;
    }
    if (match(TokenKind::KwDouble)) {
        match(TokenKind::KwPrecision); // optional PRECISION
        return ColumnType::F64;
    }
    if (match(TokenKind::KwBoolean) || match(TokenKind::KwBool)) {
        return ColumnType::Bool;
    }
    if (match(TokenKind::KwText)) {
        return ColumnType::Text;
    }
    if (match(TokenKind::KwVarchar)) {
        if (match(TokenKind::LParen)) {
            if (!match(TokenKind::IntLit)) {
                set_error("expected length inside VARCHAR(...)");
                return std::nullopt;
            }
            if (!expect(TokenKind::RParen, "expected ')' after VARCHAR length")) {
                return std::nullopt;
            }
        }
        return ColumnType::Text;
    }
    if (match(TokenKind::KwBytea)) {
        return ColumnType::Bytea;
    }
    set_error("expected column type");
    return std::nullopt;
}

std::optional<CreateTableStmt> Parser::parse_create_table() {
    expect(TokenKind::KwCreate, "expected CREATE");
    if (!expect(TokenKind::KwTable, "expected TABLE")) return std::nullopt;

    CreateTableStmt stmt;
    if (match(TokenKind::KwIf)) {
        if (!expect(TokenKind::KwNot, "expected NOT after IF")) return std::nullopt;
        if (!expect(TokenKind::KwExists, "expected EXISTS after IF NOT")) return std::nullopt;
        stmt.if_not_exists = true;
    }

    if (!is_ident_token(peek().kind)) {
        set_error("expected table name");
        return std::nullopt;
    }
    stmt.table_name = extract_ident(advance());

    if (!expect(TokenKind::LParen, "expected '(' after table name")) return std::nullopt;

    while (true) {
        if (!is_ident_token(peek().kind)) {
            set_error("expected column name");
            return std::nullopt;
        }
        ColumnDef col;
        col.name = extract_ident(advance());
        auto ty = parse_column_type();
        if (!ty) return std::nullopt;
        col.type = *ty;

        while (true) {
            if (match(TokenKind::KwPrimary)) {
                if (!expect(TokenKind::KwKey, "expected KEY after PRIMARY")) return std::nullopt;
                col.primary_key = true;
                col.not_null = true;
            } else if (match(TokenKind::KwNot)) {
                if (!expect(TokenKind::KwNull, "expected NULL after NOT")) return std::nullopt;
                col.not_null = true;
            } else {
                break;
            }
        }
        stmt.columns.push_back(std::move(col));
        if (!match(TokenKind::Comma)) break;
    }

    if (!expect(TokenKind::RParen, "expected ')' after column definitions")) return std::nullopt;
    return stmt;
}

std::optional<DropTableStmt> Parser::parse_drop_table() {
    expect(TokenKind::KwDrop, "expected DROP");
    if (!expect(TokenKind::KwTable, "expected TABLE")) return std::nullopt;

    DropTableStmt stmt;
    if (match(TokenKind::KwIf)) {
        if (!expect(TokenKind::KwExists, "expected EXISTS after IF")) return std::nullopt;
        stmt.if_exists = true;
    }
    if (!is_ident_token(peek().kind)) {
        set_error("expected table name");
        return std::nullopt;
    }
    stmt.table_name = extract_ident(advance());
    return stmt;
}

std::optional<InsertStmt> Parser::parse_insert() {
    expect(TokenKind::KwInsert, "expected INSERT");
    if (!expect(TokenKind::KwInto, "expected INTO")) return std::nullopt;
    if (!is_ident_token(peek().kind)) {
        set_error("expected table name");
        return std::nullopt;
    }

    InsertStmt stmt;
    stmt.table_name = extract_ident(advance());

    if (match(TokenKind::LParen)) {
        while (true) {
            if (!is_ident_token(peek().kind)) {
                set_error("expected column name");
                return std::nullopt;
            }
            stmt.columns.push_back(extract_ident(advance()));
            if (!match(TokenKind::Comma)) break;
        }
        if (!expect(TokenKind::RParen, "expected ')' after column list")) return std::nullopt;
    }

    if (!expect(TokenKind::KwValues, "expected VALUES")) return std::nullopt;

    while (true) {
        if (!expect(TokenKind::LParen, "expected '(' for VALUES tuple")) return std::nullopt;
        std::vector<ExprPtr> row;
        while (true) {
            auto e = parse_expr();
            if (!e) return std::nullopt;
            row.push_back(std::move(e));
            if (!match(TokenKind::Comma)) break;
        }
        if (!expect(TokenKind::RParen, "expected ')' after VALUES tuple")) return std::nullopt;
        stmt.rows.push_back(std::move(row));
        if (!match(TokenKind::Comma)) break;
    }

    if (match(TokenKind::KwReturning)) {
        auto ret = parse_select_list();
        if (!ret) return std::nullopt;
        stmt.returning = std::move(*ret);
    }

    return stmt;
}

std::optional<UpdateStmt> Parser::parse_update() {
    expect(TokenKind::KwUpdate, "expected UPDATE");
    if (!is_ident_token(peek().kind)) {
        set_error("expected table name");
        return std::nullopt;
    }

    UpdateStmt stmt;
    stmt.table_name = extract_ident(advance());

    if (!expect(TokenKind::KwSet, "expected SET")) return std::nullopt;

    while (true) {
        if (!is_ident_token(peek().kind)) {
            set_error("expected column name in SET");
            return std::nullopt;
        }
        Assignment asgn;
        asgn.column = extract_ident(advance());
        if (!expect(TokenKind::Eq, "expected '=' in SET")) return std::nullopt;
        asgn.value = parse_expr();
        if (!asgn.value) return std::nullopt;
        stmt.assignments.push_back(std::move(asgn));
        if (!match(TokenKind::Comma)) break;
    }

    if (match(TokenKind::KwWhere)) {
        stmt.where = parse_expr();
        if (!stmt.where) return std::nullopt;
    }

    if (match(TokenKind::KwReturning)) {
        auto ret = parse_select_list();
        if (!ret) return std::nullopt;
        stmt.returning = std::move(*ret);
    }

    return stmt;
}

std::optional<DeleteStmt> Parser::parse_delete() {
    expect(TokenKind::KwDelete, "expected DELETE");
    if (!expect(TokenKind::KwFrom, "expected FROM")) return std::nullopt;
    if (!is_ident_token(peek().kind)) {
        set_error("expected table name");
        return std::nullopt;
    }

    DeleteStmt stmt;
    stmt.table_name = extract_ident(advance());

    if (match(TokenKind::KwWhere)) {
        stmt.where = parse_expr();
        if (!stmt.where) return std::nullopt;
    }

    if (match(TokenKind::KwReturning)) {
        auto ret = parse_select_list();
        if (!ret) return std::nullopt;
        stmt.returning = std::move(*ret);
    }

    return stmt;
}

std::optional<SelectItem> Parser::parse_select_item() {
    SelectItem item;
    if (match(TokenKind::Star)) {
        item.is_star = true;
        return item;
    }
    if (is_ident_token(peek(0).kind) && peek(1).kind == TokenKind::Dot && peek(2).kind == TokenKind::Star) {
        item.is_star = true;
        item.table_qualifier = extract_ident(advance());
        advance(); // '.'
        advance(); // '*'
        return item;
    }

    item.expr = parse_expr();
    if (!item.expr) return std::nullopt;

    if (match(TokenKind::KwAs)) {
        if (!is_ident_token(peek().kind)) {
            set_error("expected alias after AS");
            return std::nullopt;
        }
        item.alias = extract_ident(advance());
    } else if (is_ident_token(peek().kind)) {
        item.alias = extract_ident(advance());
    }
    return item;
}

std::optional<std::vector<SelectItem>> Parser::parse_select_list() {
    std::vector<SelectItem> items;
    while (true) {
        auto it = parse_select_item();
        if (!it) return std::nullopt;
        items.push_back(std::move(*it));
        if (!match(TokenKind::Comma)) break;
    }
    return items;
}

std::optional<TableRef> Parser::parse_table_ref() {
    if (!is_ident_token(peek().kind)) {
        set_error("expected table name");
        return std::nullopt;
    }
    TableRef ref;
    ref.table_name = extract_ident(advance());
    if (match(TokenKind::Dot)) {
        if (!is_ident_token(peek().kind)) {
            set_error("expected table name after '.'");
            return std::nullopt;
        }
        ref.table_name += ".";
        ref.table_name += extract_ident(advance());
    }
    if (match(TokenKind::KwAs)) {
        if (!is_ident_token(peek().kind)) {
            set_error("expected table alias after AS");
            return std::nullopt;
        }
        ref.alias = extract_ident(advance());
    } else if (is_ident_token(peek().kind)) {
        ref.alias = extract_ident(advance());
    }
    return ref;
}

std::optional<SelectStmt> Parser::parse_select(bool allow_with) {
    SelectStmt stmt;

    if (allow_with && match(TokenKind::KwWith)) {
        while (true) {
            if (!is_ident_token(peek().kind)) {
                set_error("expected CTE name");
                return std::nullopt;
            }
            CteDef cte;
            cte.name = extract_ident(advance());
            if (!expect(TokenKind::KwAs, "expected AS in CTE")) return std::nullopt;
            if (!expect(TokenKind::LParen, "expected '(' before CTE query")) return std::nullopt;
            auto sub = parse_select(false);
            if (!sub) return std::nullopt;
            if (!expect(TokenKind::RParen, "expected ')' after CTE query")) return std::nullopt;
            cte.query = std::make_unique<SelectStmt>(std::move(*sub));
            stmt.ctes.push_back(std::move(cte));
            if (!match(TokenKind::Comma)) break;
        }
    }

    if (!expect(TokenKind::KwSelect, "expected SELECT")) return std::nullopt;
    stmt.distinct = match(TokenKind::KwDistinct);

    auto items = parse_select_list();
    if (!items) return std::nullopt;
    stmt.items = std::move(*items);

    if (match(TokenKind::KwFrom)) {
        auto from = parse_table_ref();
        if (!from) return std::nullopt;
        stmt.from = std::move(*from);

        while (check(TokenKind::KwJoin) || check(TokenKind::KwInner) || check(TokenKind::KwLeft)) {
            JoinClause jc;
            if (match(TokenKind::KwLeft)) {
                jc.type = JoinType::Left;
                if (!expect(TokenKind::KwJoin, "expected JOIN after LEFT")) return std::nullopt;
            } else {
                match(TokenKind::KwInner);
                if (!expect(TokenKind::KwJoin, "expected JOIN")) return std::nullopt;
                jc.type = JoinType::Inner;
            }
            auto jt = parse_table_ref();
            if (!jt) return std::nullopt;
            jc.table = std::move(*jt);
            if (!expect(TokenKind::KwOn, "expected ON after JOIN table")) return std::nullopt;
            jc.on = parse_expr();
            if (!jc.on) return std::nullopt;
            stmt.joins.push_back(std::move(jc));
        }
    }

    if (match(TokenKind::KwWhere)) {
        stmt.where = parse_expr();
        if (!stmt.where) return std::nullopt;
    }

    if (match(TokenKind::KwGroup)) {
        if (!expect(TokenKind::KwBy, "expected BY after GROUP")) return std::nullopt;
        while (true) {
            auto e = parse_expr();
            if (!e) return std::nullopt;
            stmt.group_by.push_back(std::move(e));
            if (!match(TokenKind::Comma)) break;
        }
    }

    if (match(TokenKind::KwOrder)) {
        if (!expect(TokenKind::KwBy, "expected BY after ORDER")) return std::nullopt;
        while (true) {
            OrderByItem ob;
            ob.expr = parse_expr();
            if (!ob.expr) return std::nullopt;
            if (match(TokenKind::KwDesc)) {
                ob.desc = true;
            } else {
                match(TokenKind::KwAsc);
                ob.desc = false;
            }
            stmt.order_by.push_back(std::move(ob));
            if (!match(TokenKind::Comma)) break;
        }
    }

    if (match(TokenKind::KwLimit)) {
        if (!check(TokenKind::IntLit)) {
            set_error("expected integer after LIMIT");
            return std::nullopt;
        }
        int64_t val = 0;
        const Token& t = advance();
        std::from_chars(t.text.data(), t.text.data() + t.text.size(), val);
        stmt.limit = val;
    }

    if (match(TokenKind::KwOffset)) {
        if (!check(TokenKind::IntLit)) {
            set_error("expected integer after OFFSET");
            return std::nullopt;
        }
        int64_t val = 0;
        const Token& t = advance();
        std::from_chars(t.text.data(), t.text.data() + t.text.size(), val);
        stmt.offset = val;
    }

    return stmt;
}

ExprPtr Parser::parse_expr() {
    return parse_or();
}

ExprPtr Parser::parse_or() {
    auto left = parse_and();
    if (!left) return nullptr;
    while (match(TokenKind::KwOr)) {
        auto right = parse_and();
        if (!right) return nullptr;
        auto e = std::make_unique<Expr>();
        e->node = BinaryExpr{BinaryOp::Or, std::move(left), std::move(right)};
        left = std::move(e);
    }
    return left;
}

ExprPtr Parser::parse_and() {
    auto left = parse_not();
    if (!left) return nullptr;
    while (match(TokenKind::KwAnd)) {
        auto right = parse_not();
        if (!right) return nullptr;
        auto e = std::make_unique<Expr>();
        e->node = BinaryExpr{BinaryOp::And, std::move(left), std::move(right)};
        left = std::move(e);
    }
    return left;
}

ExprPtr Parser::parse_not() {
    if (match(TokenKind::KwNot)) {
        auto op = parse_not();
        if (!op) return nullptr;
        auto e = std::make_unique<Expr>();
        e->node = UnaryExpr{UnaryOp::Not, std::move(op)};
        return e;
    }
    return parse_comparison();
}

ExprPtr Parser::parse_comparison() {
    auto left = parse_additive();
    if (!left) return nullptr;

    if (match(TokenKind::KwIs)) {
        bool is_not = match(TokenKind::KwNot);
        if (!expect(TokenKind::KwNull, "expected NULL after IS")) return nullptr;
        auto e = std::make_unique<Expr>();
        e->node = IsNullExpr{std::move(left), is_not};
        return e;
    }

    bool not_between = false;
    if (peek(0).kind == TokenKind::KwNot && peek(1).kind == TokenKind::KwBetween) {
        advance(); // NOT
        not_between = true;
    }
    if (match(TokenKind::KwBetween)) {
        auto low = parse_additive();
        if (!low) return nullptr;
        if (!expect(TokenKind::KwAnd, "expected AND in BETWEEN")) return nullptr;
        auto high = parse_additive();
        if (!high) return nullptr;
        auto e = std::make_unique<Expr>();
        e->node = BetweenExpr{std::move(left), std::move(low), std::move(high), not_between};
        return e;
    }

    BinaryOp op = BinaryOp::Eq;
    bool has_cmp = true;
    switch (peek().kind) {
        case TokenKind::Eq: op = BinaryOp::Eq; break;
        case TokenKind::Ne: op = BinaryOp::Ne; break;
        case TokenKind::Lt: op = BinaryOp::Lt; break;
        case TokenKind::Le: op = BinaryOp::Le; break;
        case TokenKind::Gt: op = BinaryOp::Gt; break;
        case TokenKind::Ge: op = BinaryOp::Ge; break;
        default: has_cmp = false; break;
    }
    if (has_cmp) {
        advance();
        auto right = parse_additive();
        if (!right) return nullptr;
        auto e = std::make_unique<Expr>();
        e->node = BinaryExpr{op, std::move(left), std::move(right)};
        return e;
    }

    return left;
}

ExprPtr Parser::parse_additive() {
    auto left = parse_multiplicative();
    if (!left) return nullptr;
    while (check(TokenKind::Plus) || check(TokenKind::Minus)) {
        BinaryOp op = match(TokenKind::Plus) ? BinaryOp::Add : (advance(), BinaryOp::Sub);
        auto right = parse_multiplicative();
        if (!right) return nullptr;
        auto e = std::make_unique<Expr>();
        e->node = BinaryExpr{op, std::move(left), std::move(right)};
        left = std::move(e);
    }
    return left;
}

ExprPtr Parser::parse_multiplicative() {
    auto left = parse_unary();
    if (!left) return nullptr;
    while (check(TokenKind::Star) || check(TokenKind::Slash)) {
        BinaryOp op = match(TokenKind::Star) ? BinaryOp::Mul : (advance(), BinaryOp::Div);
        auto right = parse_unary();
        if (!right) return nullptr;
        auto e = std::make_unique<Expr>();
        e->node = BinaryExpr{op, std::move(left), std::move(right)};
        left = std::move(e);
    }
    return left;
}

ExprPtr Parser::parse_unary() {
    if (match(TokenKind::Minus)) {
        auto op = parse_unary();
        if (!op) return nullptr;
        auto e = std::make_unique<Expr>();
        e->node = UnaryExpr{UnaryOp::Neg, std::move(op)};
        return e;
    }
    return parse_primary();
}

ExprPtr Parser::parse_primary() {
    const Token& t = peek();

    if (match(TokenKind::LParen)) {
        auto inner = parse_expr();
        if (!inner) return nullptr;
        if (!expect(TokenKind::RParen, "expected ')'")) return nullptr;
        return inner;
    }

    if (match(TokenKind::KwNull)) {
        auto e = std::make_unique<Expr>();
        e->node = LiteralExpr{nullptr};
        return e;
    }
    if (match(TokenKind::KwTrue)) {
        auto e = std::make_unique<Expr>();
        e->node = LiteralExpr{true};
        return e;
    }
    if (match(TokenKind::KwFalse)) {
        auto e = std::make_unique<Expr>();
        e->node = LiteralExpr{false};
        return e;
    }
    if (match(TokenKind::IntLit)) {
        int64_t val = 0;
        auto [ptr, ec] = std::from_chars(t.text.data(), t.text.data() + t.text.size(), val);
        if (ec != std::errc{}) {
            set_error("integer literal out of range");
            return nullptr;
        }
        auto e = std::make_unique<Expr>();
        e->node = LiteralExpr{val};
        return e;
    }
    if (match(TokenKind::FloatLit)) {
        std::string tmp(t.text);
        char* endp = nullptr;
        double val = std::strtod(tmp.c_str(), &endp);
        auto e = std::make_unique<Expr>();
        e->node = LiteralExpr{val};
        return e;
    }
    if (match(TokenKind::StringLit)) {
        auto e = std::make_unique<Expr>();
        e->node = LiteralExpr{Lexer::unescape_single_quoted(t.text)};
        return e;
    }
    if (match(TokenKind::Param)) {
        uint32_t idx = 0;
        std::from_chars(t.text.data() + 1, t.text.data() + t.text.size(), idx);
        if (idx == 0) {
            set_error("parameter index must be >= 1");
            return nullptr;
        }
        auto e = std::make_unique<Expr>();
        e->node = ParamExpr{idx};
        return e;
    }

    // Aggregates: COUNT, SUM, MIN, MAX, AVG
    std::optional<AggFunc> agg;
    if (match(TokenKind::KwCount)) agg = AggFunc::Count;
    else if (match(TokenKind::KwSum)) agg = AggFunc::Sum;
    else if (match(TokenKind::KwMin)) agg = AggFunc::Min;
    else if (match(TokenKind::KwMax)) agg = AggFunc::Max;
    else if (match(TokenKind::KwAvg)) agg = AggFunc::Avg;

    if (agg.has_value()) {
        if (!expect(TokenKind::LParen, "expected '(' after aggregate function")) return nullptr;
        AggregateExpr ae;
        ae.func = *agg;
        if (match(TokenKind::Star)) {
            if (*agg != AggFunc::Count) {
                set_error("'*' is only valid with COUNT(*)");
                return nullptr;
            }
            ae.is_star = true;
        } else {
            ae.arg = parse_expr();
            if (!ae.arg) return nullptr;
        }
        if (!expect(TokenKind::RParen, "expected ')' after aggregate argument")) return nullptr;
        auto e = std::make_unique<Expr>();
        e->node = std::move(ae);
        return e;
    }

    if (is_ident_token(t.kind)) {
        std::string first = extract_ident(advance());
        if (match(TokenKind::Dot)) {
            if (!is_ident_token(peek().kind)) {
                set_error("expected column name after '.'");
                return nullptr;
            }
            std::string col = extract_ident(advance());
            auto e = std::make_unique<Expr>();
            e->node = ColumnRefExpr{std::move(first), std::move(col)};
            return e;
        }
        auto e = std::make_unique<Expr>();
        e->node = ColumnRefExpr{{}, std::move(first)};
        return e;
    }

    set_error("unexpected token in expression");
    return nullptr;
}

} // namespace zephyr::sql
