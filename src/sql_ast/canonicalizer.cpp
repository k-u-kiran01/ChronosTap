// src/sql_ast/canonicalizer.cpp
#include "chronostap/sql_ast/canonicalizer.hpp"
#include <algorithm>
#include <cctype>
#include <sstream>

extern int yyparse();
typedef struct yy_buffer_state* YY_BUFFER_STATE;
extern YY_BUFFER_STATE yy_scan_string(const char* str);
extern void yy_delete_buffer(YY_BUFFER_STATE buffer);
extern std::string g_canonical_output;

namespace chronostap {

std::vector<Token> SqlCanonicalizer::tokenize(const std::string& sql) {
    std::vector<Token> tokens;
    size_t i = 0, n = sql.size();

    while (i < n) {
        // 1. Skip whitespace
        if (std::isspace(static_cast<unsigned char>(sql[i]))) {
            ++i;
            continue;
        }

        // 2. Strip '--' single-line comments
        if (sql[i] == '-' && i + 1 < n && sql[i + 1] == '-') {
            while (i < n && sql[i] != '\n') ++i;
            continue;
        }

        // 3. Strip '/* ... */' block comments
        if (sql[i] == '/' && i + 1 < n && sql[i + 1] == '*') {
            i += 2;
            while (i + 1 < n && !(sql[i] == '*' && sql[i + 1] == '/')) ++i;
            i += 2;
            continue;
        }

        // 4. Preserve single-quoted string literals ('PAID') with exact case
        if (sql[i] == '\'') {
            std::string lit = "'";
            ++i;
            while (i < n && sql[i] != '\'') {
                lit += sql[i++];
            }
            if (i < n) { lit += '\''; ++i; }
            tokens.push_back({TokenType::STRING, lit});
            continue;
        }

        // 5. Numbers
        if (std::isdigit(static_cast<unsigned char>(sql[i]))) {
            std::string num;
            while (i < n && (std::isdigit(static_cast<unsigned char>(sql[i])) || sql[i] == '.')) {
                num += sql[i++];
            }
            tokens.push_back({TokenType::NUMBER, num});
            continue;
        }

        // 6. Words (Keywords & Identifiers -> convert to UPPERCASE)
        if (std::isalpha(static_cast<unsigned char>(sql[i])) || sql[i] == '_') {
            std::string word;
            while (i < n && (std::isalnum(static_cast<unsigned char>(sql[i])) || sql[i] == '_')) {
                word += static_cast<char>(std::toupper(static_cast<unsigned char>(sql[i++])));
            }
            tokens.push_back({TokenType::WORD, word});
            continue;
        }

        // 7. Trailing semicolon can be ignored for canonical form
        if (sql[i] == ';') {
            ++i;
            continue;
        }

        // 8. Multi-character operators (!=, <=, >=, <>) or single symbols (=, +, ,, (, ))
        std::string sym(1, sql[i++]);
        if (i < n && (sql[i] == '=' || sql[i] == '>') &&
            (sym[0] == '!' || sym[0] == '<' || sym[0] == '>')) {
            sym += sql[i++];
        }
        tokens.push_back({TokenType::SYMBOL, sym});
    }
    return tokens;
}

void SqlCanonicalizer::sort_where_and_clauses(std::vector<Token>& tokens) {
    // Locate WHERE keyword
    size_t where_idx = tokens.size();
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (tokens[i].type == TokenType::WORD && tokens[i].text == "WHERE") {
            where_idx = i;
            break;
        }
    }
    if (where_idx == tokens.size()) return;

    // Find end of WHERE clause (stops at GROUP, ORDER, LIMIT, or end of query)
    size_t end_idx = tokens.size();
    for (size_t i = where_idx + 1; i < tokens.size(); ++i) {
        if (tokens[i].type == TokenType::WORD &&
            (tokens[i].text == "GROUP" || tokens[i].text == "ORDER" || tokens[i].text == "LIMIT")) {
            end_idx = i;
            break;
        }
    }

    // Split WHERE body by top-level 'AND' into predicate AST groups
    std::vector<std::vector<Token>> predicates;
    std::vector<Token> current;
    int paren_depth = 0;

    for (size_t i = where_idx + 1; i < end_idx; ++i) {
        if (tokens[i].text == "(") paren_depth++;
        else if (tokens[i].text == ")") paren_depth--;

        if (paren_depth == 0 && tokens[i].type == TokenType::WORD && tokens[i].text == "AND") {
            if (!current.empty()) predicates.push_back(current);
            current.clear();
        } else {
            current.push_back(tokens[i]);
        }
    }
    if (!current.empty()) predicates.push_back(current);

    if (predicates.size() <= 1) return;

    // Sort predicates lexicographically by their serialized canonical string
    std::sort(predicates.begin(), predicates.end(),
              [](const std::vector<Token>& a, const std::vector<Token>& b) {
                  return serialize(a) < serialize(b);
              });

    // Reconstruct sorted WHERE tokens
    std::vector<Token> rebuilt;
    for (size_t p = 0; p < predicates.size(); ++p) {
        if (p > 0) rebuilt.push_back({TokenType::WORD, "AND"});
        rebuilt.insert(rebuilt.end(), predicates[p].begin(), predicates[p].end());
    }

    tokens.erase(tokens.begin() + where_idx + 1, tokens.begin() + end_idx);
    tokens.insert(tokens.begin() + where_idx + 1, rebuilt.begin(), rebuilt.end());
}

std::string SqlCanonicalizer::serialize(const std::vector<Token>& tokens) {
    std::ostringstream oss;
    for (size_t i = 0; i < tokens.size(); ++i) {
        if (i > 0) {
            // Omit space before ',' or ')' and after '('
            bool no_space = (tokens[i].text == "," || tokens[i].text == ")" || tokens[i - 1].text == "(");
            if (!no_space) oss << ' ';
        }
        oss << tokens[i].text;
    }
    return oss.str();
}

std::string SqlCanonicalizer::canonicalize(const std::string& raw_sql) {
    g_canonical_output.clear();

    // 1. Parse in-memory SQL string using Flex + Bison LALR(1) AST
    YY_BUFFER_STATE buf = yy_scan_string(raw_sql.c_str());
    int status = yyparse();
    yy_delete_buffer(buf);

    if (status == 0 && !g_canonical_output.empty()) {
        return g_canonical_output;
    }

    // 2. Fallback to lexer normalization for non-SELECT utility commands
    auto tokens = tokenize(raw_sql);
    sort_where_and_clauses(tokens);
    return serialize(tokens);
}

} // namespace chronostap