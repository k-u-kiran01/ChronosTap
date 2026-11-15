#ifndef CHRONOSTAP_PG_WIRE_HPP
#define CHRONOSTAP_PG_WIRE_HPP

#include <string>
#include <vector>
#include <cstdint>

namespace chronostap {

struct PgQueryExchange {
    int fd;
    std::string sql_query;              // The extracted SQL string
    uint64_t query_sent_ns = 0;         // Timestamp when app sent 'Q' packet
    uint64_t response_received_ns = 0;  // Timestamp when DB finished replying ('Z' packet)
    std::vector<uint8_t> raw_db_reply;  // Exact raw bytes returned by PostgreSQL
    std::string reply_sha256;           // SHA-256 hash of raw_db_reply for deduplicated storage
};

class PgWireTracker {
public:
    // Inspects an outgoing write() from the app; returns true if it's a PostgreSQL query
    bool on_app_write(int fd, const char* data, size_t len, uint64_t ts_ns);

    // Inspects an incoming read() to the app; returns true when a full DB reply finishes
    bool on_app_read(int fd, const char* data, size_t len, uint64_t ts_ns, PgQueryExchange& completed_out);

private:
    PgQueryExchange current_exchange_;
    bool waiting_for_db_reply_ = false;
};

} // namespace chronostap
#endif