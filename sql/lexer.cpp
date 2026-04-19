#include "lexer.h"
#include <cctype>
#include <stdexcept>
#include <unordered_map>
namespace simpledb {
char SqlLexer::Peek(std::size_t pos) const { return pos < input_.size() ? input_[pos] : '\0'; }
std::string SqlLexer::ToUpper(const std::string &s) { std::string out=s; for(char &c:out) c=static_cast<char>(std::toupper(static_cast<unsigned char>(c))); return out; }
std::vector<SqlToken> SqlLexer::Tokenize() const {
    static const std::unordered_map<std::string, SqlTokenType> keywords = {
        {"CREATE", SqlTokenType::CREATE},{"TABLE", SqlTokenType::TABLE},{"INDEX", SqlTokenType::INDEX},{"UNIQUE", SqlTokenType::UNIQUE},{"ON", SqlTokenType::ON},
        {"INSERT", SqlTokenType::INSERT},{"INTO", SqlTokenType::INTO},{"VALUES", SqlTokenType::VALUES},{"SELECT", SqlTokenType::SELECT},{"FROM", SqlTokenType::FROM},
        {"WHERE", SqlTokenType::WHERE},{"JOIN", SqlTokenType::JOIN},{"ORDER", SqlTokenType::ORDER},{"BY", SqlTokenType::BY},{"GROUP", SqlTokenType::GROUP},{"LIMIT", SqlTokenType::LIMIT_KW},
        {"AS", SqlTokenType::AS},{"UPDATE", SqlTokenType::UPDATE},{"SET", SqlTokenType::SET},{"DELETE", SqlTokenType::DELETE_KW},{"EXPLAIN", SqlTokenType::EXPLAIN_KW},{"AND", SqlTokenType::AND},
        {"OR", SqlTokenType::OR},{"NOT", SqlTokenType::NOT},{"TRUE", SqlTokenType::TRUE_KW},{"FALSE", SqlTokenType::FALSE_KW},{"NULL", SqlTokenType::NULL_KW},
        {"UNION", SqlTokenType::UNION_KW},{"INTERSECT", SqlTokenType::INTERSECT_KW},{"EXCEPT", SqlTokenType::EXCEPT_KW},{"ALL", SqlTokenType::ALL_KW},{"DISTINCT", SqlTokenType::DISTINCT_KW},
        {"COUNT", SqlTokenType::COUNT},{"SUM", SqlTokenType::SUM},{"MIN", SqlTokenType::MIN},{"MAX", SqlTokenType::MAX},
        {"BOOLEAN", SqlTokenType::BOOLEAN_T},{"INT", SqlTokenType::INT_T},{"INTEGER", SqlTokenType::INTEGER_T},{"BIGINT", SqlTokenType::BIGINT_T},{"DOUBLE", SqlTokenType::DOUBLE_T},{"VARCHAR", SqlTokenType::VARCHAR_T},
        {"ASC", SqlTokenType::ASC},{"DESC", SqlTokenType::DESC}
    };
    std::vector<SqlToken> tokens; std::size_t i=0;
    while(i<input_.size()) {
        char c=Peek(i);
        if(std::isspace(static_cast<unsigned char>(c))){i++;continue;}
        if(std::isalpha(static_cast<unsigned char>(c))||c=='_'){
            std::size_t start=i++; while(std::isalnum(static_cast<unsigned char>(Peek(i)))||Peek(i)=='_') i++;
            std::string text=input_.substr(start,i-start); auto up=ToUpper(text); auto it=keywords.find(up);
            tokens.push_back({it==keywords.end()?SqlTokenType::IDENTIFIER:it->second,text,start}); continue;
        }
        if(std::isdigit(static_cast<unsigned char>(c))){ std::size_t start=i; bool seen_dot=false; i++; while(true){ char p=Peek(i); if(std::isdigit(static_cast<unsigned char>(p))){i++; continue;} if(p=='.'&&!seen_dot){seen_dot=true;i++;continue;} break;} tokens.push_back({SqlTokenType::NUMBER,input_.substr(start,i-start),start}); continue; }
        if(c=='\''){ std::size_t start=i++; std::string text; while(i<input_.size()){ char p=Peek(i); if(p=='\''){ if(Peek(i+1)=='\''){ text.push_back('\''); i+=2; continue; } i++; break; } text.push_back(p); i++; } tokens.push_back({SqlTokenType::STRING,text,start}); continue; }
        if(c=='!'&&Peek(i+1)=='='){ tokens.push_back({SqlTokenType::NEQ,"!=",i}); i+=2; continue; }
        if(c=='<'&&Peek(i+1)=='='){ tokens.push_back({SqlTokenType::LTE,"<=",i}); i+=2; continue; }
        if(c=='>'&&Peek(i+1)=='='){ tokens.push_back({SqlTokenType::GTE,">=",i}); i+=2; continue; }
        switch(c){ case ',': tokens.push_back({SqlTokenType::COMMA,",",i}); break; case ';': tokens.push_back({SqlTokenType::SEMICOLON,";",i}); break; case '.': tokens.push_back({SqlTokenType::DOT,".",i}); break; case '(': tokens.push_back({SqlTokenType::LPAREN,"(",i}); break; case ')': tokens.push_back({SqlTokenType::RPAREN,")",i}); break; case '*': tokens.push_back({SqlTokenType::STAR,"*",i}); break; case '=': tokens.push_back({SqlTokenType::EQ,"=",i}); break; case '<': tokens.push_back({SqlTokenType::LT,"<",i}); break; case '>': tokens.push_back({SqlTokenType::GT,">",i}); break; default: throw std::runtime_error("Unexpected SQL character at position "+std::to_string(i)); } i++; }
    tokens.push_back({SqlTokenType::END,"",input_.size()}); return tokens;
}
} // namespace simpledb
