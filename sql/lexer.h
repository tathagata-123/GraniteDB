#pragma once

#include <string>
#include <vector>

namespace simpledb {

enum class SqlTokenType {
    END, IDENTIFIER, NUMBER, STRING,
    COMMA, SEMICOLON, DOT, LPAREN, RPAREN, STAR, EQ, NEQ, LT, LTE, GT, GTE,
    CREATE, TABLE, INDEX, UNIQUE, ON, INSERT, INTO, VALUES, SELECT, FROM, WHERE, JOIN, ORDER, BY, GROUP, LIMIT_KW, AS, UPDATE, SET, DELETE_KW, EXPLAIN_KW, AND, OR, NOT, TRUE_KW, FALSE_KW, NULL_KW,
    UNION_KW, INTERSECT_KW, EXCEPT_KW, ALL_KW, DISTINCT_KW,
    COUNT, SUM, MIN, MAX,
    BOOLEAN_T, INT_T, INTEGER_T, BIGINT_T, DOUBLE_T, VARCHAR_T, ASC, DESC
};

struct SqlToken { SqlTokenType type{SqlTokenType::END}; std::string text; std::size_t position{0}; };

class SqlLexer {
public:
    explicit SqlLexer(std::string input) : input_(std::move(input)) {}
    std::vector<SqlToken> Tokenize() const;
private:
    char Peek(std::size_t pos) const;
    static std::string ToUpper(const std::string &s);
    std::string input_;
};

} // namespace simpledb
