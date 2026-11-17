/* src/sql_ast/sql_lexer.l */
%option noyywrap
%option nounput
%option noinput
%option case-insensitive

%{
#include <string>
#include <algorithm>
#include <cctype>
#include "sql_parser.tab.hpp"

static std::string to_upper_str(const char* s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(),
                   [](unsigned char c) { return std::toupper(c); });
    return out;
}
%}

%%

[ \t\r\n]+              { /* Skip whitespace */ }
"--"[^\n]*              { /* Strip single-line SQL comments */ }
"/*"([^*]|\*+[^*/])*\*+"/" { /* Strip block comments */ }

"SELECT"                { return TOK_SELECT; }
"FROM"                  { return TOK_FROM; }
"WHERE"                 { return TOK_WHERE; }
"AS"                    { return TOK_AS; }
"AND"                   { return TOK_AND; }
"OR"                    { return TOK_OR; }

"="                     { return TOK_EQ; }
"!="|"<>"               { return TOK_NEQ; }
"<="                    { return TOK_LE; }
">="                    { return TOK_GE; }
"<"                     { return TOK_LT; }
">"                     { return TOK_GT; }
"+"                     { return TOK_PLUS; }
"-"                     { return TOK_MINUS; }
"*"                     { return TOK_STAR; }
"/"                     { return TOK_SLASH; }
"("                     { return TOK_LPAREN; }
")"                     { return TOK_RPAREN; }
","                     { return TOK_COMMA; }
";"                     { return TOK_SEMICOLON; }
"."                     { return TOK_DOT; }

\'[^\']*\'              { yylval.str = new std::string(yytext); return TOK_STRING; }
[0-9]+(\.[0-9]+)?       { yylval.str = new std::string(yytext); return TOK_NUMBER; }
[a-zA-Z_][a-zA-Z0-9_]*  { yylval.str = new std::string(to_upper_str(yytext)); return TOK_IDENT; }

.                       { return yytext[0]; }

%%