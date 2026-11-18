#ifndef CHRONOSTAP_HTTP_PARSER_HPP
#define CHRONOSTAP_HTTP_PARSER_HPP

#include <string>
#include <unordered_map>
#include <cstdint>

namespace chronostap {

struct HttpExchange {
    int fd;
    std::string method;
    std::string path;
    std::string raw_request;
    int status_code = 0;
    std::string response_body;
    uint64_t req_start_ns = 0;
    uint64_t resp_end_ns = 0;
};

class HttpTracker {
public:
    // Called on DIR_INGRESS_READ: detects incoming HTTP request (GET/POST...)
    bool on_app_read(int fd, const char* data, size_t len, uint64_t ts_ns);

    // Called on DIR_EGRESS_WRITE: detects outgoing HTTP/1.x response and saves trace
    bool on_app_write(int fd, const char* data, size_t len, uint64_t ts_ns, HttpExchange& completed_out);

    static void save_http_trace(const HttpExchange& ex, const std::string& store_dir = ".ctap_store");

private:
    std::unordered_map<int, HttpExchange> active_http_;
};

} // namespace chronostap
#endif