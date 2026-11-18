#ifndef CHRONOSTAP_PG_WIRE_HPP
#define CHRONOSTAP_PG_WIRE_HPP

#include <string>
#include <vector>
#include <unordered_map>
#include <cstdint>

namespace chronostap {

struct PgQueryExchange {
    int fd;
    std::string sql_query;
    uint64_t query_sent_ns = 0;
    uint64_t response_received_ns = 0;
    std::vector<uint8_t> raw_db_reply;
    std::string reply_sha256;
};

class PgWireTracker {
public:
    bool on_app_write(int fd, const char* data, size_t len, uint64_t ts_ns);
    bool on_app_read(int fd, const char* data, size_t len, uint64_t ts_ns, PgQueryExchange& completed_out);

    // Extracts SQL from either 'Q' (Simple Query) or 'P' (Extended Parse Query)
    static bool extract_sql_from_packet(const char* data, size_t len, std::string& sql_out);

private:
    std::unordered_map<int, PgQueryExchange> active_exchanges_;
};

} // namespace chronostap
#endif