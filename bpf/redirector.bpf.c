// bpf/redirector.bpf.c
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_endian.h>

char LICENSE[] SEC("license") = "Dual BSD/GPL";

const volatile unsigned int target_pid = 0;

SEC("cgroup/connect4")
int redirect_pg_connect(struct bpf_sock_addr *ctx) {
    u32 pid = bpf_get_current_pid_tgid() >> 32;
    if (target_pid != 0 && pid != target_pid)
        return 1; // 1 = allow normal connection

    // If the app is trying to connect to PostgreSQL port 5432
    if (ctx->user_port == bpf_htons(5432)) {
        // Secretly redirect it to 127.0.0.1:9999 (our C++ Mock Server)
        ctx->user_ip4 = bpf_htonl(0x7f000001); // 127.0.0.1
        ctx->user_port = bpf_htons(9999);
    }
    return 1;
}