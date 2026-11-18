#include "chronostap/protocol/http_parser.hpp"
#include <cstring>
#include <fstream>
#include <sstream>

namespace chronostap {

static std::string escape_json(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\r') out += "\\r";
        else if (c == '\n') out += "\\n";
        else if (c >= 32 && c <= 126) out += c;
    }
    return out;
}

bool HttpTracker::on_app_read(int fd, const char* data, size_t len, uint64_t ts_ns) {
    if (len < 14) return false;

    std::string_view sv(data, len);
    if (sv.starts_with("GET ") || sv.starts_with("POST ") ||
        sv.starts_with("PUT ") || sv.starts_with("DELETE ")) {
        
        size_t sp1 = sv.find(' ');
        size_t sp2 = sv.find(' ', sp1 + 1);
        if (sp1 == std::string_view::npos || sp2 == std::string_view::npos) return false;

        HttpExchange ex;
        ex.fd = fd;
        ex.method = std::string(sv.substr(0, sp1));
        ex.path = std::string(sv.substr(sp1 + 1, sp2 - sp1 - 1));
        ex.raw_request = std::string(data, len);
        ex.req_start_ns = ts_ns;

        active_http_[fd] = std::move(ex);
        return true;
    }
    return false;
}

bool HttpTracker::on_app_write(int fd, const char* data, size_t len, uint64_t ts_ns, HttpExchange& completed_out) {
    auto it = active_http_.find(fd);
    if (it == active_http_.end() || len == 0) return false;

    std::string_view sv(data, len);

    // Case 1: First outgoing chunk starting with "HTTP/1.1 200 OK..."
    if (sv.starts_with("HTTP/1.")) {
        size_t sp = sv.find(' ');
        if (sp != std::string_view::npos && sp + 4 <= len) {
            it->second.status_code = std::atoi(data + sp + 1);
        }

        size_t body_pos = sv.find("\r\n\r\n");
        if (body_pos != std::string_view::npos && body_pos + 4 < len) {
            // Headers and body arrived in a single write()
            it->second.response_body = std::string(sv.substr(body_pos + 4));
            it->second.resp_end_ns = ts_ns;
            completed_out = it->second;
            active_http_.erase(it);
            return true;
        }
        // Only headers arrived in iov[0]; wait for iov[1] to deliver the body!
        return false;
    }

    // Case 2: Second outgoing chunk (iov[1]) containing the HTTP response body
    if (it->second.status_code > 0) {
        it->second.response_body = std::string(sv);
        it->second.resp_end_ns = ts_ns;
        completed_out = it->second;
        active_http_.erase(it);
        return true;
    }

    return false;
}

void HttpTracker::save_http_trace(const HttpExchange& ex, const std::string& store_dir) {
    uint64_t latency_ns = ex.resp_end_ns - ex.req_start_ns;
    std::ofstream out(store_dir + "/http_traces.jsonl", std::ios::app);
    out << "{\"method\":\"" << ex.method << "\","
        << "\"path\":\"" << ex.path << "\","
        << "\"status\":" << ex.status_code << ","
        << "\"latency_ns\":" << latency_ns << ","
        << "\"body\":\"" << escape_json(ex.response_body) << "\"}\n";
}

} // namespace chronostap