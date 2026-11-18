#include "chronostap/storage/cas_store.hpp"
#include "chronostap/sql_ast/canonicalizer.hpp"
#include <openssl/evp.h>
#include <filesystem>
#include <iomanip>
#include <sstream>

namespace fs = std::filesystem;

namespace chronostap {

static std::string escape_json_line(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else out += c;
    }
    return out;
}

CasStore::CasStore(const std::string& base_dir) : base_dir_(base_dir) {
    fs::create_directories(base_dir_ + "/blobs");
    index_out_.open(base_dir_ + "/index.jsonl", std::ios::app);
}

CasStore::~CasStore() {
    flush();
}

void CasStore::flush() {
    if (index_out_.is_open()) {
        index_out_.flush();
    }
}

std::string CasStore::compute_sha256(const std::vector<uint8_t>& data) {
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len = 0;

    EVP_MD_CTX* ctx = EVP_MD_CTX_new();
    EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr);
    EVP_DigestUpdate(ctx, data.data(), data.size());
    EVP_DigestFinal_ex(ctx, hash, &hash_len);
    EVP_MD_CTX_free(ctx);

    std::ostringstream oss;
    for (unsigned int i = 0; i < hash_len; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    return oss.str();
}

std::string CasStore::save_exchange(const PgQueryExchange& ex) {
    std::string sha256 = compute_sha256(ex.raw_db_reply);

    // Fast in-memory deduplication check (avoids stat() syscall on every query)
    if (known_blobs_.insert(sha256).second) {
        std::string blob_path = base_dir_ + "/blobs/" + sha256 + ".bin";
        if (!fs::exists(blob_path)) {
            std::ofstream blob_out(blob_path, std::ios::binary);
            blob_out.write(reinterpret_cast<const char*>(ex.raw_db_reply.data()), ex.raw_db_reply.size());
        }
    }

    uint64_t latency_ns = ex.response_received_ns - ex.query_sent_ns;
    std::string canonical = SqlCanonicalizer::canonicalize(ex.sql_query);

    if (!index_out_.is_open()) {
        index_out_.open(base_dir_ + "/index.jsonl", std::ios::app);
    }

    index_out_ << "{\"canonical_sql\":\"" << escape_json_line(canonical) << "\","
               << "\"raw_sql\":\"" << escape_json_line(ex.sql_query) << "\","
               << "\"latency_ns\":" << latency_ns << ","
               << "\"reply_bytes\":" << ex.raw_db_reply.size() << ","
               << "\"blob_sha256\":\"" << sha256 << "\"}\n";

    return sha256;
}

} // namespace chronostap