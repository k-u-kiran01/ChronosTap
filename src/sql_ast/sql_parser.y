%{
#include <iostream>
#include <string>
#include <vector>
#include <algorithm>
#include <memory>

extern int yylex();
void yyerror(const char* s);

// Global output string populated when yyparse() succeeds
std::string g_canonical_output;

struct AstNode {
    std::string kind; // "AND", "OR", "COMM_OP", "BIN_OP", "Atom"
    std::string text;
    std::vector<AstNode*> children;

    ~AstNode() {
        for (auto* c : children) delete c;
    }

    // Recursively serialize AST into canonical SQL
    std::string serialize() const {
        if (kind == "AND" || kind == "OR") {
            std::vector<std::string> parts;
            for (const auto* c : children) {
                parts.push_back(c->serialize());
            }
            // Sort commutative AND/OR branches lexicographically
            std::sort(parts.begin(), parts.end());
            std::string res;
            for (size_t i = 0; i < parts.size(); ++i) {
                if (i > 0) res += " " + kind + " ";
                res += parts[i];
            }
            return res;
        }
        if (kind == "COMM_OP") { // Commutative operators: =, +, *
            std::string left = children[0]->serialize();
            std::string right = children[1]->serialize();
            if (right < left) std::swap(left, right);
            return left + " " + text + " " + right;
        }
        if (kind == "BIN_OP") { // Non-commutative operators: -, /, <, >
            return children[0]->serialize() + " " + text + " " + children[1]->serialize();
        }
        return text;
    }
};

// Helper to flatten nested AND/OR trees: (A AND B) AND C -> [A, B, C]
AstNode* make_logical_node(const std::string& op, AstNode* left, AstNode* right) {
    AstNode* node = new AstNode{op, op, {}};
    auto absorb = [&](AstNode* child) {
        if (child->kind == op) {
            for (auto* grand : child->children) node->children.push_back(grand);
            child->children.clear();
            delete child;
        } else {
            node->children.push_back(child);
        }
    };
    absorb(left);
    absorb(right);
    return node;
}
%}

%union {
    std::string* str;
    struct AstNode* node;
}

%token TOK_SELECT TOK_FROM TOK_WHERE TOK_AS TOK_AND TOK_OR
%token <str> TOK_IDENT TOK_STRING TOK_NUMBER
%token TOK_EQ TOK_NEQ TOK_LT TOK_GT TOK_LE TOK_GE
%token TOK_PLUS TOK_MINUS TOK_STAR TOK_SLASH
%token TOK_LPAREN TOK_RPAREN TOK_COMMA TOK_SEMICOLON TOK_DOT

%type <node> expr select_item
%type <str> select_list opt_from opt_where

/* Operator Precedence (lowest to highest) */
%left TOK_OR
%left TOK_AND
%nonassoc TOK_EQ TOK_NEQ TOK_LT TOK_GT TOK_LE TOK_GE
%left TOK_PLUS TOK_MINUS
%left TOK_STAR TOK_SLASH

%%

query:
    TOK_SELECT select_list opt_from opt_where opt_semicolon {
        g_canonical_output = "SELECT " + *$2 + *$3 + *$4;
        delete $2; delete $3; delete $4;
    }
    ;

opt_semicolon:
    /* empty */
    | TOK_SEMICOLON
    ;

select_list:
    select_item {
        $$ = new std::string($1->serialize());
        delete $1;
    }
    | select_list TOK_COMMA select_item {
        $$ = new std::string(*$1 + ", " + $3->serialize());
        delete $1; delete $3;
    }
    ;

select_item:
    expr { $$ = $1; }
    | expr TOK_AS TOK_IDENT {
        $$ = new AstNode{"Atom", $1->serialize() + " AS " + *$3, {}};
        delete $1; delete $3;
    }
    ;

opt_from:
    /* empty */ { $$ = new std::string(""); }
    | TOK_FROM TOK_IDENT {
        $$ = new std::string(" FROM " + *$2);
        delete $2;
    }
    ;

opt_where:
    /* empty */ { $$ = new std::string(""); }
    | TOK_WHERE expr {
        $$ = new std::string(" WHERE " + $2->serialize());
        delete $2;
    }
    ;

expr:
    TOK_IDENT   { $$ = new AstNode{"Atom", *$1, {}}; delete $1; }
    | TOK_NUMBER { $$ = new AstNode{"Atom", *$1, {}}; delete $1; }
    | TOK_STRING { $$ = new AstNode{"Atom", *$1, {}}; delete $1; }
    | TOK_STAR   { $$ = new AstNode{"Atom", "*", {}}; }
    | TOK_IDENT TOK_LPAREN TOK_RPAREN {
        $$ = new AstNode{"Atom", *$1 + "()", {}};
        delete $1;
    }
    | TOK_IDENT TOK_LPAREN select_list TOK_RPAREN {
        $$ = new AstNode{"Atom", *$1 + "(" + *$3 + ")", {}};
        delete $1; delete $3;
    }
    | TOK_LPAREN expr TOK_RPAREN {
        $$ = $2; /* Unwrap redundant parentheses so AST can flatten! */
    }
    | expr TOK_PLUS expr  { $$ = new AstNode{"COMM_OP", "+", {$1, $3}}; }
    | expr TOK_STAR expr  { $$ = new AstNode{"COMM_OP", "*", {$1, $3}}; }
    | expr TOK_EQ expr    { $$ = new AstNode{"COMM_OP", "=", {$1, $3}}; }
    | expr TOK_MINUS expr { $$ = new AstNode{"BIN_OP", "-", {$1, $3}}; }
    | expr TOK_SLASH expr { $$ = new AstNode{"BIN_OP", "/", {$1, $3}}; }
    | expr TOK_NEQ expr   { $$ = new AstNode{"BIN_OP", "!=", {$1, $3}}; }
    | expr TOK_LT expr    { $$ = new AstNode{"BIN_OP", "<", {$1, $3}}; }
    | expr TOK_GT expr    { $$ = new AstNode{"BIN_OP", ">", {$1, $3}}; }
    | expr TOK_LE expr    { $$ = new AstNode{"BIN_OP", "<=", {$1, $3}}; }
    | expr TOK_GE expr    { $$ = new AstNode{"BIN_OP", ">=", {$1, $3}}; }
    | expr TOK_AND expr   { $$ = make_logical_node("AND", $1, $3); }
    | expr TOK_OR expr    { $$ = make_logical_node("OR", $1, $3); }
    ;

%%

void yyerror(const char* /*s*/) {
    // If a non-SELECT statement arrives, canonicalizer.cpp falls back cleanly
}