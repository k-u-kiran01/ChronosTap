#include "chronostap/protocol/pg_wire.hpp"
#include <arpa/inet.h>
#include <cstring>
#include <iostream>

namespace chronostap {

bool PgWireTracker::on_app_write(int fd, const char* data, size_t len, uint64_t ts_ns) {
    // A valid PostgreSQL 'Q' packet has at least 1 byte tag + 4 byte length + 1 byte null-terminator
    if (len < 6) return false;

    char tag = data[0];
    if (tag != 'Q') return false; // Start with Simple Query ('Q')

    // Read Big-Endian 32-bit length from bytes 1..4
    uint32_t net_len = 0;
    std::memcpy(&net_len, data + 1, sizeof(uint32_t));
    uint32_t msg_len = ntohl(net_len);

    // Sanity check: Does the declared PostgreSQL length match the packet size?
    if (msg_len < 4 || msg_len + 1 > len) return false;

    // Extract SQL string (starts at byte 5, length is msg_len - 4, minus trailing '\0')
    std::string sql(data + 5, msg_len - 5);

    current_exchange_ = PgQueryExchange{};
    current_exchange_.fd = fd;
    current_exchange_.sql_query = sql;
    current_exchange_.query_sent_ns = ts_ns;
    waiting_for_db_reply_ = true;

    return true;
}

bool PgWireTracker::on_app_read(int fd, const char* data, size_t len, uint64_t ts_ns, PgQueryExchange& completed_out) {
    if (!waiting_for_db_reply_ || fd != current_exchange_.fd || len == 0) {
        return false;
    }

    // Append raw database bytes so we can replay them byte-for-byte later!
    current_exchange_.raw_db_reply.insert(
        current_exchange_.raw_db_reply.end(),
        reinterpret_cast<const uint8_t*>(data),
        reinterpret_cast<const uint8_t*>(data) + len
    );

    // Every PostgreSQL query response ends with a 6-byte 'Z' (ReadyForQuery) message:
    // ['Z'][0x00, 0x00, 0x00, 0x05]['I' or 'T' or 'E']
    if (len >= 6 && data[len - 6] == 'Z') {
        current_exchange_.response_received_ns = ts_ns;
        completed_out = current_exchange_;
        waiting_for_db_reply_ = false;
        return true;
    }

    return false;
}

} // namespace chronostap