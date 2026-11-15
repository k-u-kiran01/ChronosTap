#ifndef CHRONOSTAP_CAS_STORE_HPP
#define CHRONOSTAP_CAS_STORE_HPP

#include "chronostap/protocol/pg_wire.hpp"
#include <string>

namespace chronostap {

class CasStore {
public:
    CasStore(const std::string& base_dir = ".ctap_store");
    std::string save_exchange(const PgQueryExchange& ex);

private:
    std::string base_dir_;
    std::string compute_sha256(const std::vector<uint8_t>& data);
};

} // namespace chronostap
#endif