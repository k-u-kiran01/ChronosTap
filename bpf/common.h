// bpf/common.h
#ifndef CHRONOSTAP_COMMON_H
#define CHRONOSTAP_COMMON_H

#define MAX_PAYLOAD_SIZE 4096

enum traffic_dir {
    DIR_INGRESS_READ = 0,  // App reading from socket (e.g., HTTP request in, or DB response in)
    DIR_EGRESS_WRITE = 1   // App writing to socket (e.g., DB query out, or HTTP response out)
};

struct socket_event {
    unsigned int pid;
    int fd;
    unsigned long long timestamp_ns;
    enum traffic_dir direction;
    unsigned int payload_len;
    char payload[MAX_PAYLOAD_SIZE];
};

#endif