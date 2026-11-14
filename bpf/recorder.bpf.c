
#include "vmlinux.h"
#include <bpf/bpf_helpers.h>
#include <bpf/bpf_tracing.h>
#include "common.h"

char LICENSE[] SEC("license") = "Dual BSD/GPL";

const volatile unsigned int target_pid = 0;
const volatile unsigned long long pidns_dev = 0;
const volatile unsigned long long pidns_ino = 0;

struct {
    __uint(type, BPF_MAP_TYPE_RINGBUF);
    __uint(max_entries, 16 * 1024 * 1024);
} rb SEC(".maps");

struct read_args {
    int fd;
    u32 pid;
    const char *buf;
};

struct {
    __uint(type, BPF_MAP_TYPE_HASH);
    __uint(max_entries, 10240);
    __type(key, u64);
    __type(value, struct read_args);
} active_reads SEC(".maps");

static __always_inline u32 get_resolved_pid(u64 id) {
    u32 pid = id >> 32;
    struct bpf_pidns_info ns = {};
    if (pidns_dev && pidns_ino &&
        bpf_get_ns_current_pid_tgid(pidns_dev, pidns_ino, &ns, sizeof(ns)) == 0) {
        pid = ns.tgid;
    }
    return pid;
}

// Helper 1: Save buffer pointer when entering read() or recvfrom()
static __always_inline int trace_read_entry(struct trace_event_raw_sys_enter *ctx) {
    u64 id = bpf_get_current_pid_tgid();
    u32 pid = get_resolved_pid(id);

    if (pid != target_pid)
        return 0;

    int fd = (int)ctx->args[0];
    if (fd <= 2) return 0; // Ignore stdin, stdout, stderr

    struct read_args args = {};
    args.fd = fd;
    args.pid = pid;
    args.buf = (const char *)ctx->args[1];

    bpf_map_update_elem(&active_reads, &id, &args, BPF_ANY);
    return 0;
}

// Helper 2: Copy received bytes when exiting read() or recvfrom()
static __always_inline int trace_read_exit(struct trace_event_raw_sys_exit *ctx) {
    u64 id = bpf_get_current_pid_tgid();
    struct read_args *args = bpf_map_lookup_elem(&active_reads, &id);
    if (!args)
        return 0;

    long bytes_read = ctx->ret;
    if (bytes_read <= 0) {
        bpf_map_delete_elem(&active_reads, &id);
        return 0;
    }

    struct socket_event *e = bpf_ringbuf_reserve(&rb, sizeof(*e), 0);
    if (!e) {
        bpf_map_delete_elem(&active_reads, &id);
        return 0;
    }

    e->pid = args->pid;
    e->fd = args->fd;
    e->timestamp_ns = bpf_ktime_get_ns();
    e->direction = DIR_INGRESS_READ;

    u32 copy_size = bytes_read < MAX_PAYLOAD_SIZE ? bytes_read : MAX_PAYLOAD_SIZE;
    e->payload_len = copy_size;
    bpf_probe_read_user(e->payload, copy_size, args->buf);

    bpf_ringbuf_submit(e, 0);
    bpf_map_delete_elem(&active_reads, &id);
    return 0;
}

// Helper 3: Copy outgoing bytes when entering write() or sendto()
static __always_inline int trace_write_entry(struct trace_event_raw_sys_enter *ctx) {
    u64 id = bpf_get_current_pid_tgid();
    u32 pid = get_resolved_pid(id);

    if (pid != target_pid)
        return 0;

    int fd = (int)ctx->args[0];
    if (fd <= 2) return 0; // Ignore stdin, stdout, stderr

    const char *buf = (const char *)ctx->args[1];
    long bytes_written = (long)ctx->args[2];
    if (bytes_written <= 0)
        return 0;

    struct socket_event *e = bpf_ringbuf_reserve(&rb, sizeof(*e), 0);
    if (!e)
        return 0;

    e->pid = pid;
    e->fd = fd;
    e->timestamp_ns = bpf_ktime_get_ns();
    e->direction = DIR_EGRESS_WRITE;

    u32 copy_size = bytes_written < MAX_PAYLOAD_SIZE ? bytes_written : MAX_PAYLOAD_SIZE;
    e->payload_len = copy_size;
    bpf_probe_read_user(e->payload, copy_size, buf);

    bpf_ringbuf_submit(e, 0);
    return 0;
}

// Attach to both socket recvfrom/sendto AND standard read/write (for Node.js / PostgreSQL)
SEC("tp/syscalls/sys_enter_recvfrom")
int handle_enter_recvfrom(struct trace_event_raw_sys_enter *ctx) { return trace_read_entry(ctx); }

SEC("tp/syscalls/sys_exit_recvfrom")
int handle_exit_recvfrom(struct trace_event_raw_sys_exit *ctx) { return trace_read_exit(ctx); }

SEC("tp/syscalls/sys_enter_read")
int handle_enter_read(struct trace_event_raw_sys_enter *ctx) { return trace_read_entry(ctx); }

SEC("tp/syscalls/sys_exit_read")
int handle_exit_read(struct trace_event_raw_sys_exit *ctx) { return trace_read_exit(ctx); }

SEC("tp/syscalls/sys_enter_sendto")
int handle_enter_sendto(struct trace_event_raw_sys_enter *ctx) { return trace_write_entry(ctx); }

SEC("tp/syscalls/sys_enter_write")
int handle_enter_write(struct trace_event_raw_sys_enter *ctx) { return trace_write_entry(ctx); }