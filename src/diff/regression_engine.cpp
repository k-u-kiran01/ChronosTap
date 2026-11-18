#include "chronostap/diff/regression_engine.hpp"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <iostream>

namespace chronostap {

static std::string extract_str(const std::string& line, const std::string& key) {
    std::string pat = "\"" + key + "\":\"";
    auto start = line.find(pat);
    if (start == std::string::npos) return "";
    start += pat.size();
    std::string out;
    for (size_t i = start; i < line.size(); ++i) {
        if (line[i] == '\\' && i + 1 < line.size()) {
            char next = line[++i];
            if (next == 'n') out += '\n';
            else if (next == 'r') out += '\r';
            else out += next;
        } else if (line[i] == '"') {
            break;
        } else {
            out += line[i];
        }
    }
    return out;
}

static uint64_t extract_num(const std::string& line, const std::string& key) {
    std::string pat = "\"" + key + "\":";
    auto start = line.find(pat);
    if (start == std::string::npos) return 0;
    return std::stoull(line.substr(start + pat.size()));
}

std::vector<RecordedHttpTrace> RegressionEngine::load_traces(const std::string& store_dir) {
    std::vector<RecordedHttpTrace> traces;
    std::ifstream in(store_dir + "/http_traces.jsonl");
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        traces.push_back({
            extract_str(line, "method"),
            extract_str(line, "path"),
            static_cast<int>(extract_num(line, "status")),
            extract_num(line, "latency_ns"),
            extract_str(line, "body")
        });
    }
    return traces;
}

int RegressionEngine::run_suite(uint16_t app_port, const std::string& store_dir) {
    auto traces = load_traces(store_dir);
    if (traces.empty()) {
        std::cout << "[!] No HTTP traces found in " << store_dir << "/http_traces.jsonl\n";
        return 0;
    }

    std::cout << "\n================ CHRONOSTAP CI REGRESSION SUITE ================\n";
    int failures = 0;

    for (size_t i = 0; i < traces.size(); ++i) {
        const auto& t = traces[i];

        int sock = socket(AF_INET, SOCK_STREAM, 0);
        struct sockaddr_in addr = {};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(app_port);
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

        auto t0 = std::chrono::steady_clock::now();
        if (connect(sock, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            std::cerr << "[-] Could not connect to target app on port " << app_port << "\n";
            close(sock);
            return 1;
        }

        std::string req = t.method + " " + t.path + " HTTP/1.1\r\nHost: localhost\r\nConnection: close\r\n\r\n";
        send(sock, req.data(), req.size(), 0);

        std::string raw_resp;
        char buf[4096];
        ssize_t n;
        while ((n = recv(sock, buf, sizeof(buf), 0)) > 0) {
            raw_resp.append(buf, n);
        }
        auto t1 = std::chrono::steady_clock::now();
        close(sock);

        double actual_ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
        double expected_ms = t.recorded_latency_ns / 1e6;

        int actual_status = 0;
        size_t sp = raw_resp.find(' ');
        if (sp != std::string::npos) actual_status = std::atoi(raw_resp.c_str() + sp + 1);

        std::string actual_body;
        size_t body_pos = raw_resp.find("\r\n\r\n");
        if (body_pos != std::string::npos) actual_body = raw_resp.substr(body_pos + 4);

        bool status_ok = (actual_status == t.expected_status);
        bool body_ok = (actual_body == t.expected_body);
        bool passed = status_ok && body_ok;
        if (!passed) failures++;

        std::cout << (passed ? "[PASS] " : "[FAIL] ")
                  << t.method << " " << t.path
                  << " | Status: " << actual_status << " (exp " << t.expected_status << ")"
                  << " | Latency: " << std::fixed << std::setprecision(2) << actual_ms
                  << " ms (rec " << expected_ms << " ms)\n";

        if (!body_ok) {
            std::cout << "  -> Expected Body: " << t.expected_body << "\n"
                      << "  -> Actual Body:   " << actual_body << "\n";
        }
    }

    std::cout << "================================================================\n";
    std::cout << "Summary: " << (traces.size() - failures) << "/" << traces.size() << " passed.\n\n";
    return failures == 0 ? 0 : 1;
}

} // namespace chronostap