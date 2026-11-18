#!/usr/bin/env bash
set -e

ENDPOINTS=(
  "/api/orders/paid"
  "/api/users/vip"
  "/api/analytics/summary"
  "/api/orders/top"
  "/api/orders/flagged"
)

REQS_PER_QUERY=${1:-1000}
CONCURRENCY=${2:-50}

echo "========================================================================"
echo " ChronosTap 5-Query Benchmark ($REQS_PER_QUERY reqs x 5 queries, c=$CONCURRENCY)"
echo "========================================================================"
printf "%-26s | %-12s | %-10s | %-10s\n" "Endpoint" "Throughput" "p50 (ms)" "p99 (ms)"
echo "------------------------------------------------------------------------"

for ep in "${ENDPOINTS[@]}"; do
  OUT=$(ab -n "$REQS_PER_QUERY" -c "$CONCURRENCY" -k "http://127.0.0.1:8080${ep}" 2>/dev/null)
  RPS=$(echo "$OUT" | awk '/Requests per second:/ {print $4 " req/s"}')
  P50=$(echo "$OUT" | awk '/50%/ {print $2 " ms"}')
  P99=$(echo "$OUT" | awk '/99%/ {print $2 " ms"}')
  printf "%-26s | %-12s | %-10s | %-10s\n" "$ep" "$RPS" "$P50" "$P99"
done
echo "========================================================================"
