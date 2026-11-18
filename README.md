# ChronosTap

**Zero-Instrumentation eBPF Shadow-Traffic Recorder, SQL AST Canonicalizer & PostgreSQL Wire-Protocol Virtualization Engine**

![Language](https://img.shields.io/badge/Language-C%2B%2B20%20%7C%20C-blue.svg)
![Kernel](https://img.shields.io/badge/Linux-eBPF%20(libbpf%20%2F%20CO--RE)-orange.svg)
![Compiler](https://img.shields.io/badge/Parser-Flex%20%26%20Bison%20LALR(1)-purple.svg)
![Runtime](https://img.shields.io/badge/Async%20I%2FO-epoll%20%2B%20timerfd-success.svg)
![Build](https://img.shields.io/badge/Build-CMake%20%2B%20CTest-brightgreen.svg)

**ChronosTap** is a high-performance Linux systems engine written in C and C++20 that records live HTTP and **PostgreSQL v3 binary wire-protocol** traffic directly from the Linux kernel using **eBPF**, deduplicates binary database responses into a **SHA-256 Content-Addressable Store (CAS)**, and replays them during CI regression tests without a running database.

Unlike traditional traffic replay tools that break whenever a developer refactors a SQL query (e.g., reordering `WHERE` predicates, changing `amount + tax` to `tax + amount`, or adding comments), ChronosTap embeds a **two-tier Flex/Bison LALR(1) SQL AST Canonicalizer**. It normalizes SQL queries into a deterministic canonical form before hashing and lookup—eliminating brittle mock misses while sustaining **12,022 queries/sec at 4 ms p50 latency** on a single thread.

---

## Why ChronosTap?

| Capability | Application ORM Mocks (Prisma/Jest) | GoReplay | Keploy | **ChronosTap** |
| :--- | :--- | :--- | :--- | :--- |
| **Instrumentation Required** | Heavy code changes | None (pcap) | None (eBPF) | **Zero (Kernel eBPF)** |
| **Database Wire-Protocol Replay** | No (in-memory stubs) | No (HTTP only) | Yes | **Yes (PostgreSQL v3 Binary)** |
| **Survives SQL Query Refactors** | Manual mock updates | N/A | Brittle string match | **Yes (Flex/Bison AST Canonicalizer)** |
| **Payload Storage Deduplication** | N/A | Raw log files | YAML per test | **SHA-256 CAS (99.9% reduction)** |
| **Historical Latency & Chaos Simulation** | No | No | Static | **Nanosecond-accurate `timerfd` replay** |

---

## System Architecture

```text
========================================================================================
 MODE 1: PASSIVE RECORDING (sudo ./build/chronostap --record <PID> [--quiet])
========================================================================================

  [ Client (curl / ab) ] ──── HTTP :8080 ────► [ Target Backend (Node.js / pg.Pool) ]
                                                         │               ▲
                                       sys_enter_write   │               │ sys_exit_read
                                       sys_enter_writev  ▼               │ (Port 5432)
                                                  [ Live PostgreSQL DB ]
                                                         │
        ┌────────────────────────────────────────────────┴───────────────────────┐
        │ LINUX KERNEL SPACE (bpf/recorder.bpf.c)                                │
        │  • Hooks tracepoints: read, write, recvfrom, sendto, writev            │
        │  • WSL2 / Container PID-namespace translation (bpf_get_ns_current_pid) │
        │  • Streams socket_event payloads via 16 MB lockless BPF_MAP_TYPE_RINGBUF│
        └────────────────────────────────────────────────┬───────────────────────┘
                                                         │ Zero-copy poll
                                                         ▼
        ┌────────────────────────────────────────────────────────────────────────┐
        │ USERSPACE ENGINE (C++20)                                               │
        │  1. Per-FD Protocol State Machines (HttpTracker & PgWireTracker)       │
        │     • Reassembles scatter-gather HTTP chunks & PG 'Q'/'P' -> 'Z' frames│
        │  2. Two-Tier SQL Canonicalizer (Flex + Bison LALR(1) AST + Fallback)   │
        │     • Flattens nested AND/OR trees & sorts commutative operands        │
        │  3. SHA-256 Content-Addressable Store (.ctap_store/blobs/<sha256>.bin) │
        └────────────────────────────────────────────────────────────────────────┘

========================================================================================
 MODE 2: ZERO-DB REPLAY & CI DIFFING (sudo ./build/chronostap --replay 1.0 8080)
========================================================================================

  [ Target Backend (PostgreSQL STOPPED) ] ── connect(127.0.0.1:5432) ──┐
                                                                       ▼
        ┌────────────────────────────────────────────────────────────────────────┐
        │ LINUX KERNEL SPACE (bpf/redirector.bpf.c)                              │
        │  • Hooks cgroup/connect4: Transparently rewrites 127.0.0.1:5432 -> 9999│
        └────────────────────────────────────────────────────────────────────────┘
                                                                       │
                                                                       ▼
        ┌────────────────────────────────────────────────────────────────────────┐
        │ NON-BLOCKING EPOLL + TIMERFD MOCK SERVER (:9999)                       │
        │  1. Intercepts PG SSLRequest (80877103) -> sends 1-byte 'N' downgrade  │
        │  2. Completes StartupMessage handshake (AuthOk 'R' + ReadyForQuery 'Z')│
        │  3. Canonicalizes incoming SQL AST -> O(1) lookup in CAS Mock Table    │
        │  4. Arms kernel timerfd for (recorded_latency_ns * chaos_multiplier)   │
        │  5. Dispatches exact recorded binary DataRow ('T','D','C','Z') bytes   │
        └────────────────────────────────────────────────────────────────────────┘
```

---

## Key Engineering Highlights

1. **Zero-Instrumentation Kernel Wiretap (`bpf/recorder.bpf.c`):**
   * Attaches to `tp/syscalls/sys_enter_write`, `sys_enter_writev`, `sys_enter_sendto`, `sys_exit_read`, and `sys_exit_recvfrom`.
   * Handles scatter-gather Vector I/O (`struct iovec` in `writev`) used by Node.js/libuv when flushing HTTP headers and JSON bodies simultaneously.
   * Implements container/WSL2-aware PID filtering via `bpf_get_ns_current_pid_tgid(pidns_dev, pidns_ino, ...)` and tracks backpressure via an atomic `ringbuf_drops` counter.

2. **Transparent Socket Hijacking (`bpf/redirector.bpf.c`):**
   * Uses a `cgroup/connect4` eBPF program to intercept outbound TCP `connect()` syscalls targeting port `5432` and transparently reroute them to the local mock server on port `9999`—requiring zero changes to application connection strings or environment variables.

3. **PostgreSQL v3 Binary Wire-Protocol Engine (`src/protocol/pg_wire.cpp`):**
   * Multiplexes concurrent connection-pooled sockets via per-FD state machines (`std::unordered_map<int, PgQueryExchange>`).
   * Extracts SQL strings from both Simple Query (`'Q'`) and Extended/ORM Parse (`'P'`) packets and captures complete backend responses (`RowDescription` `'T'`, `DataRow` `'D'`, `CommandComplete` `'C'`, through the 6-byte `ReadyForQuery` `'Z'` trailer).

4. **Two-Tier SQL AST Canonicalizer (`src/sql_ast/`):**
   * **Tier 1 (Flex + Bison LALR(1) AST):** Parses `SELECT ... FROM ... WHERE` queries into an expression AST, recursively flattens nested `AND`/`OR` trees, lexicographically sorts boolean predicates, and normalizes commutative binary operations (`+`, `*`, `=`).
   * **Tier 2 (Fault-Tolerant Token-Stream Normalizer):** Gracefully handles complex vendor-specific SQL (`ORDER BY`, `LIMIT`, `JOIN`, CTEs) by stripping comments, preserving string literal casing, and sorting top-level `WHERE ... AND` clauses.

5. **Non-Blocking `epoll` + `timerfd` Chaos Server (`src/replay/epoll_mock_server.cpp`):**
   * Handles PostgreSQL SSL negotiation by replying `'N'` to `SSLRequest` (`80877103`) so `libpq`/`pg` clients automatically fall back to unencrypted wire framing.
   * Uses per-request Linux `timerfd` descriptors registered on the `epoll` instance to simulate historical database latency (or inject `10x` tail-latency spikes) without blocking the event loop thread.

---

## Live End-to-End Demo (Refactored SQL Survival)

ChronosTap ships with a 5-endpoint Node.js + `pg.Pool` target application (`target_app/server.js`). Setting `REFACTORED=1` switches the backend to execute **refactored SQL queries** (swapped arithmetic `tax + amount`, reordered `WHERE` clauses, flipped equality `1 = active`, and added SQL comments).

### 1. Record Live Traffic (PostgreSQL Running)
```text
$ sudo ./build/chronostap --record 5835
[+] ChronosTap RECORD Mode active on PID 5835 (Port 5432 untouched!)
[HTTP REQUEST IN on FD 27]
[PG QUERY DETECTED on FD 28]
  -> Raw SQL:       SELECT id, user_id, amount + tax AS total_cost
       FROM orders
       WHERE status = 'PAID' AND region = 'APAC' AND amount >= 100;
  -> Canonical AST: SELECT ID, USER_ID, AMOUNT + TAX AS TOTAL_COST FROM ORDERS WHERE AMOUNT >= 100 AND REGION = 'APAC' AND STATUS = 'PAID'
  -> DB Latency:    17.5787 ms (175 bytes)
  -> Saved CAS Blob: .ctap_store/blobs/67182ce0ef24...bin
----------------------------------------
[HTTP RESPONSE OUT on FD 27] GET /api/orders/paid -> 200 (49.4228 ms)
========================================
^C
[+] Recording stopped | Captured 4 HTTP traces & 4 PG queries | Kernel ringbuf drops: 0
```

### 2. Stop PostgreSQL, Enable `REFACTORED=1` SQL, and Run CI Replay
```text
$ sudo systemctl stop postgresql
$ REFACTORED=1 node target_app/server.js &
$ sudo ./build/chronostap --replay 1.0 8080
[+] Loaded 4 canonical SQL mocks from .ctap_store
[+] ChronosTap REPLAY Mode active! Redirecting 5432 -> 9999...
[+] ChronosTap Mock PostgreSQL Server listening on 127.0.0.1:9999 (Chaos Latency Multiplier: 1x)

================ CHRONOSTAP CI REGRESSION SUITE ================
[REPLAYED MOCK] AST: SELECT ID, USER_ID, AMOUNT + TAX AS TOTAL_COST FROM ORDERS WHERE AMOUNT >= 100 AND REGION = 'APAC' AND STATUS = 'PAID' | Simulated Delay: 17.5787 ms (175 bytes)
[PASS] GET /api/orders/paid | Status: 200 (exp 200) | Latency: 27.26 ms (rec 49.42 ms)
[REPLAYED MOCK] AST: SELECT ID, USERNAME, BALANCE FROM USERS WHERE 1 = ACTIVE AND BALANCE >= 500 AND TIER = 'GOLD' | Simulated Delay: 2.36 ms (162 bytes)
[PASS] GET /api/users/vip | Status: 200 (exp 200) | Latency: 4.33 ms (rec 3.39 ms)
[REPLAYED MOCK] AST: SELECT COUNT(*) AS ORDER_COUNT, SUM(AMOUNT) AS GROSS_REVENUE FROM ORDERS WHERE AMOUNT > 0 AND STATUS = 'PAID' | Simulated Delay: 5.81 ms (109 bytes)
[PASS] GET /api/analytics/summary | Status: 200 (exp 200) | Latency: 7.81 ms (rec 6.72 ms)
[REPLAYED MOCK] AST: SELECT ID, USER_ID, AMOUNT, STATUS FROM ORDERS WHERE REGION = 'APAC' AND STATUS = 'PAID' ORDER BY ID DESC LIMIT 2 | Simulated Delay: 1.66 ms (187 bytes)
[PASS] GET /api/orders/top | Status: 200 (exp 200) | Latency: 3.31 ms (rec 2.38 ms)
================================================================
Summary: 4/4 passed.
```

---

## Performance & Benchmarks

Benchmarked using ApacheBench (`scripts/benchmark_5queries.sh`) across **5 distinct SQL query patterns** (`5,000` requests per run, `50` concurrent connections, `pg.Pool` size `20`, HTTP Keep-Alive enabled):

| Endpoint | SQL / Compiler Feature Exercised | Baseline (Live PG) | Passive eBPF Record (`--quiet`) | Zero-DB Mock Replay (`--replay 0.0`) |
| :--- | :--- | :--- | :--- | :--- |
| `GET /api/orders/paid` | Commutative `+` & 3-way `WHERE AND` *(cold pool warmup)* | `5,105 req/s` (5 ms p50) | `6,160 req/s` (6 ms p50) | `6,153 req/s` (5 ms p50) |
| `GET /api/users/vip` | Nested `(A AND B) AND C` flattening & flipped `=` | `11,148 req/s` (4 ms p50) | `8,563 req/s` (6 ms p50) | `10,806 req/s` (4 ms p50) |
| `GET /api/analytics/summary` | SQL Aggregations (`COUNT(*)`, `SUM()`) | `12,490 req/s` (3 ms p50) | `8,849 req/s` (5 ms p50) | `10,701 req/s` (4 ms p50) |
| `GET /api/orders/top` | Tier-2 Token-Stream Fallback (`ORDER BY ... LIMIT`) | `12,020 req/s` (4 ms p50) | `9,047 req/s` (5 ms p50) | `10,798 req/s` (4 ms p50) |
| `GET /api/orders/flagged` | Commutative `OR` tree sorting & `*` multiplication | `12,375 req/s` (4 ms p50) | `8,889 req/s` (5 ms p50) | `12,022 req/s` (4 ms p50) |

### Key Benchmark Metrics
* **Peak Mock Server Throughput:** **12,022.70 req/s** at **4 ms p50 (11–22 ms p99) latency** on a single thread while parsing and canonicalizing SQL ASTs on every query.
* **Passive eBPF Recording Overhead:** Sustains **9,047.07 req/s (~36,180 kernel syscall events/sec)** with only **~1 ms median latency overhead** and **0% kernel ring-buffer packet drops**.
* **SHA-256 CAS Deduplication Ratio:** Across `5,000` recorded SQL executions in `.ctap_store/index.jsonl`, `.ctap_store/blobs/` stores **only 5 unique binary `.bin` files** (**99.9% disk footprint reduction**).

---

## Quickstart & Build Instructions

### 1. Prerequisites (Ubuntu 22.04+ / WSL2 Linux Kernel 5.15+)
```bash
sudo apt-get update
sudo apt-get install -y clang llvm libbpf-dev linux-tools-common linux-tools-generic \
    cmake flex bison libssl-dev pkg-config apache2-utils postgresql nodejs npm
```

### 2. Build Binary & Run Compiler AST Unit Tests
```bash
cmake -B build
cmake --build build -j$(nproc)
ctest --test-dir build --output-on-failure -V
```

### 3. Seed PostgreSQL & Start Target Service
```bash
sudo systemctl start postgresql
psql -h 127.0.0.1 -U postgres -d postgres -f target_app/seed.sql
cd target_app && npm install pg && cd ..
node target_app/server.js
```

### 4. Run the 5-Query Benchmark Suite
```bash
chmod +x scripts/benchmark_5queries.sh
./scripts/benchmark_5queries.sh 1000 50
```

---

## Repository Structure

```text
chronostap/
├── bpf/
│   ├── common.h                 # Shared kernel/userspace ring-buffer event layout
│   ├── recorder.bpf.c           # Tracepoint hooks (read/write/writev/recvfrom/sendto)
│   └── redirector.bpf.c         # cgroup/connect4 socket hijacker (5432 -> 9999)
├── include/chronostap/
│   ├── diff/regression_engine.hpp
│   ├── protocol/http_parser.hpp
│   ├── protocol/pg_wire.hpp
│   ├── replay/epoll_mock_server.hpp
│   ├── sql_ast/canonicalizer.hpp
│   └── storage/cas_store.hpp
├── src/
│   ├── cli/main.cpp             # CLI entrypoint (--record / --replay / --quiet)
│   ├── diff/regression_engine.cpp
│   ├── protocol/http_parser.cpp # HTTP/1.1 parser & trace serializer
│   ├── protocol/pg_wire.cpp     # Per-FD PostgreSQL v3 binary frame state machine
│   ├── replay/epoll_mock_server.cpp # Non-blocking epoll + timerfd mock DB server
│   ├── sql_ast/
│   │   ├── sql_lexer.l          # Flex tokenizer
│   │   ├── sql_parser.y         # Bison LALR(1) SQL AST grammar
│   │   └── canonicalizer.cpp    # Two-tier AST + Token-Stream normalizer
│   └── storage/cas_store.cpp    # OpenSSL EVP SHA-256 content-addressable store
├── target_app/
│   ├── seed.sql                 # Multi-table schema (users, orders)
│   └── server.js                # Pooled Node.js API with REFACTORED=1 SQL toggle
├── tests/unit/
│   └── test_ast.cpp             # CTest unit suite for SQL AST canonicalization
├── scripts/
│   └── benchmark_5queries.sh    # Automated 5-endpoint ApacheBench harness
└── CMakeLists.txt               # Unified BTF/eBPF skeleton, Flex/Bison & C++20 build
```