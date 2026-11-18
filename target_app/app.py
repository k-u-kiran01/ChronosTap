from http.server import BaseHTTPRequestHandler, HTTPServer
import subprocess, json

class Handler(BaseHTTPRequestHandler):
    def do_GET(self):
        # Executes a real PostgreSQL query over TCP port 5432
        out = subprocess.check_output(
            ["psql", "-h", "127.0.0.1", "-p", "5432", "-U", "postgres", "-t", "-A",
             "-c", "SELECT 10 + 20 AS sum;"]
        ).decode().strip()
        body = json.dumps({"endpoint": self.path, "db_result": int(out)}).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

HTTPServer(("127.0.0.1", 8080), Handler).serve_forever()