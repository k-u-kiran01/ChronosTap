// src/replay/epoll_mock_server.cpp
#include "chronostap/replay/epoll_mock_server.hpp"
#include "chronostap/protocol/pg_wire.hpp"
#include "chronostap/sql_ast/canonicalizer.hpp"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/timerfd.h>
#include <unistd.h>

#include <cstring>
#include <fstream>
#include <iostream>

namespace chronostap {

EpollMockServer::EpollMockServer(uint16_t port, const std::string& store_dir, double chaos_multiplier)
    : port_(port), store_dir_(store_dir), chaos_multiplier_(chaos_multiplier) {}

// Simple helper to extract JSON fields from .ctap_store/index.jsonl
static std::string extract_json_str(const std::string& line, const std::string& key) {
    std::string pattern = "\"" + key + "\":\"";
    auto start = line.find(pattern);
    if (start == std::string::npos) return "";
    start += pattern.size();
    auto end = line.find("\"", start);
    return line.substr(start, end - start);
}

static uint64_t extract_json_u64(const std::string& line, const std::string& key) {
    std::string pattern = "\"" + key + "\":";
    auto start = line.find(pattern);
    if (start == std::string::npos) return 1000000;
    start += pattern.size();
    return std::stoull(line.substr(start));
}

bool EpollMockServer::load_mocks() {
    std::ifstream idx(store_dir_ + "/index.jsonl");
    if (!idx.is_open()) {
        std::cerr << "[-] Could not open " << store_dir_ << "/index.jsonl\n";
        return false;
    }

    std::string line;
    while (std::getline(idx, line)) {
        if (line.empty()) continue;
        std::string canonical = extract_json_str(line, "canonical_sql");
        if (canonical.empty()) {
            // Backwards-compatible with Stage 2 recordings
            canonical = SqlCanonicalizer::canonicalize(extract_json_str(line, "sql"));
        }
        std::string sha256 = extract_json_str(line, "blob_sha256");
        uint64_t latency_ns = extract_json_u64(line, "latency_ns");

        std::string blob_path = store_dir_ + "/blobs/" + sha256 + ".bin";
        std::ifstream blob_in(blob_path, std::ios::binary);
        if (!blob_in.is_open()) continue;

        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(blob_in)),
                                    std::istreambuf_iterator<char>());

        mock_table_[canonical] = RecordedMock{canonical, latency_ns, std::move(bytes)};
    }

    std::cout << "[+] Loaded " << mock_table_.size() << " canonical SQL mocks from " << store_dir_ << "\n";
    return !mock_table_.empty();
}

void EpollMockServer::schedule_delayed_reply(int epoll_fd, int client_fd, const RecordedMock& mock) {
    int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (tfd < 0) return;

    uint64_t scaled_ns = static_cast<uint64_t>(mock.latency_ns * chaos_multiplier_);
    if (scaled_ns == 0) scaled_ns = 1000; // minimum 1 microsecond

    struct itimerspec ts = {};
    ts.it_value.tv_sec = scaled_ns / 1000000000ULL;
    ts.it_value.tv_nsec = scaled_ns % 1000000000ULL;
    timerfd_settime(tfd, 0, &ts, nullptr);

    struct epoll_event ev = {};
    ev.events = EPOLLIN;
    ev.data.fd = tfd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, tfd, &ev);

    active_timers_[tfd] = PendingTimerReply{
        client_fd,
        mock.raw_reply_bytes,
        mock.canonical_sql,
        scaled_ns / 1e6
    };
}

void EpollMockServer::send_pg_error(int client_fd, const std::string& msg) {
    // Construct a minimal PostgreSQL 'E' (ErrorResponse) + 'Z' (ReadyForQuery) packet
    std::string body = std::string("SERROR\0C42601\0M", 15) + msg + std::string("\0\0", 2);
    uint32_t len = htonl(static_cast<uint32_t>(body.size() + 4));
    std::vector<uint8_t> pkt;
    pkt.push_back('E');
    pkt.insert(pkt.end(), reinterpret_cast<uint8_t*>(&len), reinterpret_cast<uint8_t*>(&len) + 4);
    pkt.insert(pkt.end(), body.begin(), body.end());
    // Append ReadyForQuery ('Z', len 5, 'I')
    const uint8_t ready[] = {'Z', 0, 0, 0, 5, 'I'};
    pkt.insert(pkt.end(), ready, ready + 6);
    send(client_fd, pkt.data(), pkt.size(), 0);
}

void EpollMockServer::handle_client_bytes(int epoll_fd, int client_fd, const uint8_t* buf, size_t len) {
    if (len == 0) return;

    // 1. Terminate PostgreSQL Session ('X')
    if (buf[0] == 'X') {
        close(client_fd);
        return;
    }

    // 2. Handle Simple Query ('Q') or Extended Parse ('P')
    std::string raw_sql;
    if (PgWireTracker::extract_sql_from_packet(reinterpret_cast<const char*>(buf), len, raw_sql)) {
        std::string canonical = SqlCanonicalizer::canonicalize(raw_sql);

        auto it = mock_table_.find(canonical);
        if (it != mock_table_.end()) {
            schedule_delayed_reply(epoll_fd, client_fd, it->second);
        } else {
            std::cout << "[-] MOCK MISS for Canonical AST: " << canonical << "\n";
            send_pg_error(client_fd, "ChronosTap Mock Not Found for AST: " + canonical);
        }
        return;
    }

    // 3. Handle Initial PostgreSQL Handshake (SSLRequest or StartupMessage)
    if (len >= 8 && buf[0] == 0x00) {
        uint32_t code = 0;
        std::memcpy(&code, buf + 4, 4);
        code = ntohl(code);

        if (code == 80877103) {
            // Client sent SSLRequest (80877103) -> Reply 'N' (downgrade to plaintext!)
            send(client_fd, "N", 1, 0);
            return;
        }
        if (code == 196608) {
            // Client sent StartupMessage (Protocol v3.0 = 196608) -> Reply AuthOK + ReadyForQuery
            const uint8_t greeting[] = {
                'R', 0, 0, 0, 8, 0, 0, 0, 0, // AuthenticationOk
                'Z', 0, 0, 0, 5, 'I'         // ReadyForQuery (Idle)
            };
            send(client_fd, greeting, sizeof(greeting), 0);
            return;
        }
    }
}

void EpollMockServer::run(volatile bool& keep_running) {
    int listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK, 0);
    int opt = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port_);

    if (bind(listen_fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0 ||
        listen(listen_fd, 128) < 0) {
        std::cerr << "[-] Failed to bind Mock Server on port " << port_ << "\n";
        return;
    }

    int epoll_fd = epoll_create1(0);
    struct epoll_event ev = {};
    ev.events = EPOLLIN;
    ev.data.fd = listen_fd;
    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, listen_fd, &ev);

    std::cout << "[+] ChronosTap Mock PostgreSQL Server listening on 127.0.0.1:" << port_
              << " (Chaos Latency Multiplier: " << chaos_multiplier_ << "x)\n";

    struct epoll_event events[64];
    uint8_t buf[4096];

    while (keep_running) {
        int nfds = epoll_wait(epoll_fd, events, 64, 100);
        for (int i = 0; i < nfds; ++i) {
            int fd = events[i].data.fd;

            if (fd == listen_fd) {
                // 1. Accept new redirected PostgreSQL connection
                int client_fd = accept4(listen_fd, nullptr, nullptr, SOCK_NONBLOCK);
                if (client_fd >= 0) {
                    struct epoll_event cev = {};
                    cev.events = EPOLLIN;
                    cev.data.fd = client_fd;
                    epoll_ctl(epoll_fd, EPOLL_CTL_ADD, client_fd, &cev);
                }
            } else if (active_timers_.count(fd)) {
                // 2. A timerfd expired! Flush the recorded PostgreSQL reply now
                uint64_t expirations = 0;
                read(fd, &expirations, sizeof(expirations));

                const auto& pending = active_timers_[fd];
                send(pending.client_fd, pending.payload.data(), pending.payload.size(), 0);

                std::cout << "[REPLAYED MOCK] AST: " << pending.canonical_sql
                          << " | Simulated Delay: " << pending.simulated_ms << " ms ("
                          << pending.payload.size() << " bytes)\n";

                epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                close(fd);
                active_timers_.erase(fd);
            } else {
                // 3. Incoming data from a connected PostgreSQL client
                ssize_t n = recv(fd, buf, sizeof(buf), 0);
                if (n <= 0) {
                    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, nullptr);
                    close(fd);
                } else {
                    handle_client_bytes(epoll_fd, fd, buf, static_cast<size_t>(n));
                }
            }
        }
    }

    close(listen_fd);
    close(epoll_fd);
}

} // namespace chronostap