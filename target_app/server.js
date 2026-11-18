const http = require('http');
const { Pool } = require('pg');

const pool = new Pool({
  host: '127.0.0.1',
  port: 5432,
  user: 'postgres',
  password: 'uday',
  database: 'postgres',
  ssl: false,
  max: 20
});

// Toggle between original recorded SQL and refactored PR SQL
const isRefactored = process.env.REFACTORED === '1';

const QUERIES = {
  // Endpoint 1: Multi-row SELECT with commutative arithmetic (amount + tax) & 3-way WHERE AND
  paidOrders: isRefactored
    ? `select id, user_id, tax + amount as total_cost
       from orders
       where amount >= 100 and region = 'APAC' and 'PAID' = status; -- refactored order & math`
    : `SELECT id, user_id, amount + tax AS total_cost
       FROM orders
       WHERE status = 'PAID' AND region = 'APAC' AND amount >= 100;`,

  // Endpoint 2: Nested parentheses & flipped equality operands
  vipUsers: isRefactored
    ? `select id, username, balance from users
       where balance >= 500 and ('GOLD' = tier and 1 = active); /* nested AST test */`
    : `SELECT id, username, balance FROM users
       WHERE (active = 1 AND tier = 'GOLD') AND balance >= 500;`,

  // Endpoint 3: SQL Aggregations (COUNT & SUM)
  revenueSummary: isRefactored
    ? `select count(*) as order_count, sum(amount) as gross_revenue
       from orders where amount > 0 and status = 'PAID';`
    : `SELECT COUNT(*) AS order_count, SUM(amount) AS gross_revenue
       FROM orders WHERE status = 'PAID' AND amount > 0;`,

  // Endpoint 4: Complex query with ORDER BY and LIMIT (exercises Tier-2 Lexer Fallback)
  topOrders: isRefactored
    ? `select id, user_id, amount, status from orders
       where region = 'APAC' and status = 'PAID'
       order by id desc limit 2; -- tier 2 fallback with ORDER BY + LIMIT`
    : `SELECT id, user_id, amount, status FROM orders
       WHERE status = 'PAID' AND region = 'APAC'
       ORDER BY id DESC LIMIT 2;`,
    
  // Endpoint 5: Commutative OR tree sorting & commutative multiplication (amount * 100)
  flaggedOrders: isRefactored
    ? `select id, user_id, 100 * amount as amount_cents, status
       from orders
       where 'PENDING' = status or status = 'CANCELLED'; -- refactored OR & multiplication`
    : `SELECT id, user_id, amount * 100 AS amount_cents, status
       FROM orders
       WHERE status = 'CANCELLED' OR status = 'PENDING';`
};

const server = http.createServer(async (req, res) => {
  try {
    let sql = null;
    if (req.url === '/api/orders/paid') sql = QUERIES.paidOrders;
    else if (req.url === '/api/users/vip') sql = QUERIES.vipUsers;
    else if (req.url === '/api/analytics/summary') sql = QUERIES.revenueSummary;
    else if (req.url === '/api/orders/top') sql = QUERIES.topOrders;
    else if (req.url === '/api/orders/flagged') sql = QUERIES.flaggedOrders;
    if (!sql || req.method !== 'GET') {
      res.writeHead(404);
      return res.end(JSON.stringify({ error: 'Not found' }));
    }

    const result = await pool.query(sql);
    const body = JSON.stringify({ count: result.rows.length, data: result.rows });

    res.writeHead(200, {
      'Content-Type': 'application/json',
      'Content-Length': Buffer.byteLength(body)
    });
    res.end(body);
  } catch (err) {
    res.writeHead(500);
    res.end(JSON.stringify({ error: err.message }));
  }
});

server.listen(8080, '127.0.0.1', () => {
  console.log(`[Target App] Mode: ${isRefactored ? 'REFACTORED_SQL' : 'ORIGINAL_SQL'} | Port: 8080 | PID: ${process.pid}`);
});