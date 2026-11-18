#include "chronostap/protocol/pg_wire.hpp"
#include <arpa/inet.h>
#include <cstring>

namespace chronostap {

bool PgWireTracker::extract_sql_from_packet(const char* data, size_t len, std::string& sql_out) {
    if (len < 6) return false;
    char tag = data[0];
    if (tag != 'Q' && tag != 'P') return false;

    uint32_t net_len = 0;
    std::memcpy(&net_len, data + 1, sizeof(uint32_t));
    uint32_t msg_len = ntohl(net_len);
    if (msg_len < 5 || msg_len + 1 > len) return false;

    if (tag == 'Q') {
        // Simple Query: ['Q'][len(4)][sql_string\0]
        sql_out.assign(data + 5, msg_len - 5);
        return true;
    } else if (tag == 'P') {
        // Extended Parse: ['P'][len(4)][stmt_name\0][sql_string\0][num_params(2)...]
        size_t pos = 5;
        while (pos < len && data[pos] != '\0') pos++; // skip stmt_name
        pos++; // skip '\0'
        if (pos >= len) return false;
        size_t sql_start = pos;
        while (pos < len && data[pos] != '\0') pos++;
        if (pos <= sql_start) return false;
        sql_out.assign(data + sql_start, pos - sql_start);
        return true;
    }
    return false;
}

bool PgWireTracker::on_app_write(int fd, const char* data, size_t len, uint64_t ts_ns) {
    std::string sql;
    if (!extract_sql_from_packet(data, len, sql)) return false;

    PgQueryExchange ex;
    ex.fd = fd;
    ex.sql_query = std::move(sql);
    ex.query_sent_ns = ts_ns;
    active_exchanges_[fd] = std::move(ex);
    return true;
}

bool PgWireTracker::on_app_read(int fd, const char* data, size_t len, uint64_t ts_ns, PgQueryExchange& completed_out) {
    auto it = active_exchanges_.find(fd);
    if (it == active_exchanges_.end() || len == 0) return false;

    it->second.raw_db_reply.insert(
        it->second.raw_db_reply.end(),
        reinterpret_cast<const uint8_t*>(data),
        reinterpret_cast<const uint8_t*>(data) + len
    );

    // Every query response finishes with 6-byte 'Z' (ReadyForQuery)
    if (len >= 6 && data[len - 6] == 'Z') {
        it->second.response_received_ns = ts_ns;
        completed_out = std::move(it->second);
        active_exchanges_.erase(it);
        return true;
    }
    return false;
}

} // namespace chronostap