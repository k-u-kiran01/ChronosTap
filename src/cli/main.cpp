#include <iostream>
#include <csignal>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <bpf/libbpf.h>
#include "common.h"
#include "recorder.skel.h"
#include "redirector.skel.h"
#include "chronostap/protocol/pg_wire.hpp"
#include "chronostap/storage/cas_store.hpp"

static volatile bool keep_running = true;
void sig_handler(int) { keep_running = false; }

static chronostap::PgWireTracker pg_tracker;
static chronostap::CasStore cas_store(".ctap_store");

static int handle_event(void *ctx, void *data, size_t data_sz) {
    const auto *e = static_cast<const struct socket_event *>(data);

    if (e->direction == DIR_EGRESS_WRITE) {
        if (pg_tracker.on_app_write(e->fd, e->payload, e->payload_len, e->timestamp_ns)) {
            std::cout << "[PG QUERY DETECTED on FD " << e->fd << "]\n";
        }
    } else if (e->direction == DIR_INGRESS_READ) {
        chronostap::PgQueryExchange completed;
        if (pg_tracker.on_app_read(e->fd, e->payload, e->payload_len, e->timestamp_ns, completed)) {
            std::string hash = cas_store.save_exchange(completed);
            double latency_ms = (completed.response_received_ns - completed.query_sent_ns) / 1e6;

            std::cout << "  -> SQL: " << completed.sql_query << "\n"
                      << "  -> DB Latency: " << latency_ms << " ms (" << completed.raw_db_reply.size() << " bytes)\n"
                      << "  -> Saved CAS Blob: .ctap_store/blobs/" << hash.substr(0, 12) << "...bin\n"
                      << "----------------------------------------\n";
        }
    }
    return 0;
}

int main(int argc, char **argv) {
    signal(SIGINT, sig_handler);

    struct stat st;
    bool has_pidns = (stat("/proc/self/ns/pid", &st) == 0);
    unsigned int target_pid = (argc > 1) ? std::stoi(argv[1]) : 0;

    // 1. Load & Attach the Wiretap (recorder.bpf.c)
    struct recorder_bpf *rec_skel = recorder_bpf__open();
    if (!rec_skel) return 1;
    if (has_pidns) {
        rec_skel->rodata->pidns_dev = st.st_dev;
        rec_skel->rodata->pidns_ino = st.st_ino;
    }
    rec_skel->rodata->target_pid = target_pid;
    if (recorder_bpf__load(rec_skel) || recorder_bpf__attach(rec_skel)) {
        std::cerr << "Failed to attach recorder\n";
        return 1;
    }

    // 2. Load & Attach the Port Redirector (redirector.bpf.c) to cgroup v2
    struct redirector_bpf *red_skel = redirector_bpf__open();
    if (!red_skel) return 1;
    red_skel->rodata->target_pid = target_pid;
    if (redirector_bpf__load(red_skel)) {
        std::cerr << "Failed to load cgroup redirector\n";
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
        std::cerr << "Failed to attach cgroup redirector\n";
        return 1;
    }

    struct ring_buffer *rb = ring_buffer__new(bpf_map__fd(rec_skel->maps.rb), handle_event, nullptr, nullptr);
    std::cout << "[+] ChronosTap active! Wiretap ON + Port 5432 -> 9999 Redirector ON\n";

    while (keep_running) {
        ring_buffer__poll(rb, 100);
    }

    bpf_link__destroy(cgroup_link);
    close(cgroup_fd);
    redirector_bpf__destroy(red_skel);
    ring_buffer__free(rb);
    recorder_bpf__destroy(rec_skel);
    return 0;
}