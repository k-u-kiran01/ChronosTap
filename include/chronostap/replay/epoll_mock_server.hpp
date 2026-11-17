#ifndef CHRONOSTAP_EPOLL_MOCK_SERVER_HPP
#define CHRONOSTAP_EPOLL_MOCK_SERVER_HPP

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace chronostap {

struct RecordedMock {
    std::string canonical_sql;
    uint64_t latency_ns;
    std::vector<uint8_t> raw_reply_bytes;
};

struct PendingTimerReply {
    int client_fd;
    std::vector<uint8_t> payload;
    std::string canonical_sql;
    double simulated_ms;
};

class EpollMockServer {
public:
    EpollMockServer(uint16_t port = 9999, const std::string& store_dir = ".ctap_store", double chaos_multiplier = 1.0);
    bool load_mocks();
    void run(volatile bool& keep_running);

private:
    uint16_t port_;
    std::string store_dir_;
    double chaos_multiplier_;

    // Map from Canonical AST SQL -> Recorded Mock (Blob + Latency)
    std::unordered_map<std::string, RecordedMock> mock_table_;

    // Map from active timer_fd -> Pending reply waiting to be flushed
    std::unordered_map<int, PendingTimerReply> active_timers_;

    void handle_client_bytes(int epoll_fd, int client_fd, const uint8_t* buf, size_t len);
    void schedule_delayed_reply(int epoll_fd, int client_fd, const RecordedMock& mock);
    void send_pg_error(int client_fd, const std::string& msg);
};

} // namespace chronostap
#endif