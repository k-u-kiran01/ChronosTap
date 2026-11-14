// bpf/recorder.bpf.c
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include "common.h"

char LICENSE[] SEC("license") = "Dual BSD/GPL";

// 1. Target PID we want to spy on (set from C++ before loading)
const volatile unsigned int target_pid = 0;
const volatile unsigned long long pidns_dev = 0;
const volatile unsigned long long pidns_ino = 0;

// 2. Ring Buffer to send captured packets from Kernel -> C++
struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 16 * 1024 * 1024); // 16 MB buffer
} rb SEC(".maps");

// 3. Temporary map to remember buffer pointers between syscall enter and exit
struct read_args {
    int fd;
    u32 pid;
    const char *buf;
};

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 10240);
    __type(key, u64);               // Key: Thread ID (pid_tgid)
    __type(value, struct read_args);
} active_reads SEC(".maps");

// --- HOOK 1: When target app enters recvfrom() ---
SEC("tp/syscalls/sys_enter_recvfrom")
int handle_enter_recvfrom(struct trace_event_raw_sys_enter *ctx) {
    u64 id = bpf_get_current_pid_tgid();
    u32 pid = id >> 32;

    struct bpf_pidns_info ns = {};
    if (pidns_dev && pidns_ino &&
        bpf_get_ns_current_pid_tgid(pidns_dev, pidns_ino, &ns, sizeof(ns)) == 0) {
        pid = ns.tgid;
    }

    if (target_pid != 0 && pid != target_pid)
        return 0; // Ignore every other process on the machine

    struct read_args args = {};
    args.fd = (int)ctx->args[0];
    args.pid = pid;
    args.buf = (const char *)ctx->args[1];

    bpf_map_update_elem(&active_reads, &id, &args, BPF_ANY);
    return 0;
}

// --- HOOK 2: When target app exits recvfrom() with actual data ---
SEC("tp/syscalls/sys_exit_recvfrom")
int handle_exit_recvfrom(struct trace_event_raw_sys_exit *ctx) {
    u64 id = bpf_get_current_pid_tgid();
    struct read_args *args = bpf_map_lookup_elem(&active_reads, &id);
    if (!args)
        return 0;

    long bytes_read = ctx->ret;
    if (bytes_read <= 0) {
        bpf_map_delete_elem(&active_reads, &id);
        return 0;
    }

    // Reserve space in the Ring Buffer
    struct socket_event *e = bpf_ringbuf_reserve(&rb, sizeof(*e), 0);
    if (!e) {
        bpf_map_delete_elem(&active_reads, &id);
        return 0;
    }

    e->pid = args->pid;
    e->fd = args->fd;
    e->timestamp_ns = bpf_ktime_get_ns();
    e->direction = DIR_INGRESS_READ;

    // Clamp size to MAX_PAYLOAD_SIZE (4096) so the eBPF verifier is happy
    u32 copy_size = bytes_read < MAX_PAYLOAD_SIZE ? bytes_read : MAX_PAYLOAD_SIZE;
    e->payload_len = copy_size;

    // Safely copy bytes from User Space buffer into Kernel Ring Buffer
    bpf_probe_read_user(e->payload, copy_size, args->buf);

    bpf_ringbuf_submit(e, 0);
    bpf_map_delete_elem(&active_reads, &id);
    return 0;
}