const http = require('http');
const { Client } = require('pg');

const server = http.createServer(async (req, res) => {
  if (req.url === '/api/sum' && req.method === 'GET') {
    const db = new Client({
      host: '127.0.0.1',
      port: 5432,
      user: 'postgres',
      password: 'uday',
      ssl: false
    });

    try {
      await db.connect();
      // Simple query protocol ('Q' packet)
      const result = await db.query('SELECT NOW(), 20 + 10 AS sum; --refactored in PR');
      await db.end();

      const body = JSON.stringify(result.rows[0]);
      res.writeHead(200, {
        'Content-Type': 'application/json',
        'Content-Length': Buffer.byteLength(body)
      });
      res.end(body);
    } catch (err) {
      res.writeHead(500);
      res.end(JSON.stringify({ error: err.message }));
    }
  }
});

server.listen(8080, '127.0.0.1', () => {
  console.log(`[Target App] Listening on http://127.0.0.1:8080 | PID: ${process.pid}`);
});