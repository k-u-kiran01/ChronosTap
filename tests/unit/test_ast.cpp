#include "chronostap/sql_ast/canonicalizer.hpp"
#include <cassert>
#include <iostream>

using chronostap::SqlCanonicalizer;

int main() {
    // 1. Multi-clause e-commerce query: commutative arithmetic (amount + tax vs tax + amount)
    //    and 3-way WHERE predicate reordering
    assert(
        SqlCanonicalizer::canonicalize(
            "SELECT id, user_id, amount + tax AS total_cost FROM orders "
            "WHERE status = 'PAID' AND region = 'APAC' AND amount >= 100;") ==
        SqlCanonicalizer::canonicalize(
            "select id, user_id, tax + amount as total_cost from orders "
            "where amount >= 100 and region = 'APAC' and 'PAID' = status; -- PR refactor")
    );

    // 2. Nested boolean tree flattening ((A AND B) AND C == C AND (B AND A))
    assert(
        SqlCanonicalizer::canonicalize(
            "SELECT id, username, balance FROM users WHERE (active = 1 AND tier = 'GOLD') AND balance >= 500;") ==
        SqlCanonicalizer::canonicalize(
            "select id, username, balance from users where balance >= 500 and ('GOLD' = tier and 1 = active);")
    );

    // 3. SQL Aggregations (COUNT, SUM) with comments stripped
    assert(
        SqlCanonicalizer::canonicalize(
            "SELECT COUNT(*) AS order_count, SUM(amount) AS gross_revenue FROM orders WHERE status = 'PAID' AND amount > 0;") ==
        SqlCanonicalizer::canonicalize(
            "/* analytics */ select count( * ) as order_count, sum( amount ) as gross_revenue from orders where amount > 0 and status = 'PAID'")
    );

    // 4. Commutative multiplication & addition in projections
    assert(
        SqlCanonicalizer::canonicalize("SELECT price * quantity AS subtotal FROM items;") ==
        SqlCanonicalizer::canonicalize("SELECT quantity * price AS subtotal FROM items;")
    );

    // 5. Non-commutative operators (<, >, -) preserve exact operand order
    assert(
        SqlCanonicalizer::canonicalize("SELECT amount - discount FROM orders WHERE amount > 100;") !=
        SqlCanonicalizer::canonicalize("SELECT discount - amount FROM orders WHERE 100 > amount;")
    );

    // 6. Case preservation inside single-quoted string literals
    assert(
        SqlCanonicalizer::canonicalize("select * from orders where status = 'PendingReview'") ==
        "SELECT * FROM ORDERS WHERE STATUS = 'PendingReview'"
    );

    // 7. Tier-2 Fallback: Complex query with ORDER BY and LIMIT still normalizes WHERE clauses
    assert(
        SqlCanonicalizer::canonicalize(
            "SELECT id, user_id, amount, status FROM orders WHERE status = 'PAID' AND region = 'APAC' ORDER BY id DESC LIMIT 2;") ==
        SqlCanonicalizer::canonicalize(
            "select id, user_id, amount, status from orders where region = 'APAC' and status = 'PAID' order by id desc limit 2; -- comment")
    );

    std::cout << "[PASS] All 7 production SQL canonicalization test suites passed!\n";
    return 0;
}