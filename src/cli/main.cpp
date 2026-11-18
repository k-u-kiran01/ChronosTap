#include <iostream>
#include <string>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <bpf/libbpf.h>
#include <thread>

#include "common.h"
#include "recorder.skel.h"
#include "redirector.skel.h"
#include "chronostap/protocol/pg_wire.hpp"
#include "chronostap/storage/cas_store.hpp"
#include "chronostap/sql_ast/canonicalizer.hpp"
#include "chronostap/replay/epoll_mock_server.hpp"
#include "chronostap/protocol/http_parser.hpp"
#include "chronostap/diff/regression_engine.hpp"

static volatile bool keep_running = true;
void sig_handler(int) { keep_running = false; }

static chronostap::PgWireTracker pg_tracker;
static chronostap::HttpTracker http_tracker;
static chronostap::CasStore cas_store(".ctap_store");

static int handle_event(void *ctx, void *data, size_t data_sz) {
    const auto *e = static_cast<const struct socket_event *>(data);

    if (e->direction == DIR_INGRESS_READ) {
        // 1. Check if app just received an incoming HTTP request
        if (http_tracker.on_app_read(e->fd, e->payload, e->payload_len, e->timestamp_ns)) {
            std::cout << "[HTTP REQUEST IN on FD " << e->fd << "]\n";
        }
        // 2. Check if app just received a PostgreSQL query response
        chronostap::PgQueryExchange completed;
        if (pg_tracker.on_app_read(e->fd, e->payload, e->payload_len, e->timestamp_ns, completed)) {
            std::string hash = cas_store.save_exchange(completed);
            double latency_ms = (completed.response_received_ns - completed.query_sent_ns) / 1e6;
            std::cout << "  -> Raw SQL:       " << completed.sql_query << "\n"
                      << "  -> Canonical AST: " << chronostap::SqlCanonicalizer::canonicalize(completed.sql_query) << "\n"
                      << "  -> DB Latency:    " << latency_ms << " ms (" << completed.raw_db_reply.size() << " bytes)\n"
                      << "  -> Saved CAS Blob: .ctap_store/blobs/" << hash.substr(0, 12) << "...bin\n"
                      << "----------------------------------------\n";
        }
    } else if (e->direction == DIR_EGRESS_WRITE) {
        // 1. Check if app is sending a PostgreSQL query
        if (pg_tracker.on_app_write(e->fd, e->payload, e->payload_len, e->timestamp_ns)) {
            std::cout << "[PG QUERY DETECTED on FD " << e->fd << "]\n";
        }
        // 2. Check if app is sending an HTTP response back to the client
        chronostap::HttpExchange http_done;
        if (http_tracker.on_app_write(e->fd, e->payload, e->payload_len, e->timestamp_ns, http_done)) {
            chronostap::HttpTracker::save_http_trace(http_done, ".ctap_store");
            std::cout << "[HTTP RESPONSE OUT on FD " << e->fd << "] "
                      << http_done.method << " " << http_done.path << " -> " << http_done.status_code
                      << " (" << (http_done.resp_end_ns - http_done.req_start_ns) / 1e6 << " ms)\n"
                      << "========================================\n";
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    signal(SIGINT, sig_handler);

    if (argc < 2) {
        std::cerr << "Usage:\n"
                  << "  Record Mode: sudo ./build/chronostap --record <PID>\n"
                  << "  Replay Mode: sudo ./build/chronostap --replay [chaos_latency_multiplier]\n";
        return 1;
    }

    std::string mode = argv[1];

    // =========================================================================
    // MODE 1: RECORD (Attaches ONLY recorder.bpf.c; leaves Port 5432 untouched)
    // =========================================================================
    if (mode == "--record") {
        if (argc < 3) {
            std::cerr << "Error: Provide target PID (e.g., sudo ./build/chronostap --record 1234)\n";
            return 1;
        }
        unsigned int target_pid = std::stoi(argv[2]);

        struct stat st;
        bool has_pidns = (stat("/proc/self/ns/pid", &st) == 0);

        struct recorder_bpf *rec_skel = recorder_bpf__open();
        if (!rec_skel) return 1;
        if (has_pidns) {
            rec_skel->rodata->pidns_dev = st.st_dev;
            rec_skel->rodata->pidns_ino = st.st_ino;
        }
        rec_skel->rodata->target_pid = target_pid;

        if (recorder_bpf__load(rec_skel) || recorder_bpf__attach(rec_skel)) {
            std::cerr << "[-] Failed to attach recorder\n";
            recorder_bpf__destroy(rec_skel);
            return 1;
        }

        struct ring_buffer *rb = ring_buffer__new(bpf_map__fd(rec_skel->maps.rb), handle_event, nullptr, nullptr);
        std::cout << "[+] ChronosTap RECORD Mode active on PID " << target_pid << " (Port 5432 untouched!)\n";

        while (keep_running) {
            ring_buffer__poll(rb, 100);
        }

        ring_buffer__free(rb);
        recorder_bpf__destroy(rec_skel);
    }
    // =========================================================================
    // MODE 2: REPLAY (Attaches redirector.bpf.c 5432 -> 9999 + EpollMockServer)
    // =========================================================================
    else if (mode == "--replay") {
        double chaos_mult = (argc >= 3) ? std::stod(argv[2]) : 1.0;

        chronostap::EpollMockServer mock_server(9999, ".ctap_store", chaos_mult);
        if (!mock_server.load_mocks()) {
            return 1;
        }

        struct redirector_bpf *red_skel = redirector_bpf__open();
        if (!red_skel) return 1;
        red_skel->rodata->target_pid = 0; // 0 = Redirect any client connecting to 5432
        if (redirector_bpf__load(red_skel)) {
            std::cerr << "[-] Failed to load cgroup redirector\n";
            return 1;
        }

        // Try /sys/fs/cgroup/unified first (WSL2 / hybrid cgroup layouts),
        // then fall back to /sys/fs/cgroup (native Ubuntu pure cgroup v2)
        int cgroup_fd = open("/sys/fs/cgroup/unified", O_RDONLY);
        struct bpf_link *cgroup_link = nullptr;
        if (cgroup_fd >= 0) {
            cgroup_link = bpf_program__attach_cgroup(red_skel->progs.redirect_pg_connect, cgroup_fd);
        }
        if (!cgroup_link) {
            if (cgroup_fd >= 0) close(cgroup_fd);
            cgroup_fd = open("/sys/fs/cgroup", O_RDONLY);
            if (cgroup_fd >= 0) {
                cgroup_link = bpf_program__attach_cgroup(red_skel->progs.redirect_pg_connect, cgroup_fd);
            }
        }
        if (!cgroup_link) {
            std::cerr << "[-] Failed to attach cgroup redirector\n";
            redirector_bpf__destroy(red_skel);
            return 1;
        }

        std::cout << "[+] ChronosTap REPLAY Mode active! Redirecting 5432 -> 9999...\n";
        uint16_t http_port = (argc >= 4) ? static_cast<uint16_t>(std::stoi(argv[3])) : 0;
        std::thread regression_thread;
        if (http_port > 0) {
            regression_thread = std::thread([http_port]() {
                sleep(1); // Wait 1s for epoll mock server to be ready
                chronostap::RegressionEngine::run_suite(http_port, ".ctap_store");
            });
        }

        mock_server.run(keep_running);
        if (regression_thread.joinable()) regression_thread.join();

        bpf_link__destroy(cgroup_link);
        close(cgroup_fd);
        redirector_bpf__destroy(red_skel);
    }

    return 0;
}