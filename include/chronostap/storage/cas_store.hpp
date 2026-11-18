#ifndef CHRONOSTAP_CAS_STORE_HPP
#define CHRONOSTAP_CAS_STORE_HPP

#include "chronostap/protocol/pg_wire.hpp"
#include <fstream>
#include <string>
#include <unordered_set>

namespace chronostap {

class CasStore {
public:
    CasStore(const std::string& base_dir = ".ctap_store");
    ~CasStore();
    std::string save_exchange(const PgQueryExchange& ex);
    void flush();

private:
    std::string base_dir_;
    std::ofstream index_out_;
    std::unordered_set<std::string> known_blobs_;
    std::string compute_sha256(const std::vector<uint8_t>& data);
};

} // namespace chronostap
#endif