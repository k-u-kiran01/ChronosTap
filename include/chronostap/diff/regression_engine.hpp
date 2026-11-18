#ifndef CHRONOSTAP_REGRESSION_ENGINE_HPP
#define CHRONOSTAP_REGRESSION_ENGINE_HPP

#include <string>
#include <vector>
#include <cstdint>

namespace chronostap {

struct RecordedHttpTrace {
    std::string method;
    std::string path;
    int expected_status;
    uint64_t recorded_latency_ns;
    std::string expected_body;
};

class RegressionEngine {
public:
    static std::vector<RecordedHttpTrace> load_traces(const std::string& store_dir = ".ctap_store");
    static int run_suite(uint16_t app_port, const std::string& store_dir = ".ctap_store");
};

} // namespace chronostap
#endif