#ifndef CHRONOSTAP_SQL_CANONICALIZER_HPP
#define CHRONOSTAP_SQL_CANONICALIZER_HPP

#include <string>
#include <vector>

namespace chronostap {

enum class TokenType {
    WORD,       // Keywords & identifiers (normalized to UPPERCASE)
    STRING,     // Single-quoted literals ('PAID' - case preserved!)
    NUMBER,     // Numeric literals (42, 3.14)
    SYMBOL      // Operators & punctuation (=, +, ,, (, ), ;)
};

struct Token {
    TokenType type;
    std::string text;
};

class SqlCanonicalizer {
public:
    // Converts messy raw SQL into a deterministic canonical SQL string
    static std::string canonicalize(const std::string& raw_sql);

private:
    static std::vector<Token> tokenize(const std::string& sql);
    static void sort_where_and_clauses(std::vector<Token>& tokens);
    static std::string serialize(const std::vector<Token>& tokens);
};

} // namespace chronostap
#endif