#!/usr/bin/env bash
set -e

if command -v bpftool >/dev/null 2>&1 && bpftool version >/dev/null 2>&1; then
    BPFTOOL="bpftool"
else
    # Fallback for WSL2 / custom kernels where Ubuntu's wrapper looks for $(uname -r)
    BPFTOOL=$(find -L /usr/lib/linux-tools* -name bpftool -type f -executable 2>/dev/null | sort -V | tail -n 1 || true)
fi

if [ -z "$BPFTOOL" ]; then
    echo "[-] Error: bpftool not found. Install it with: sudo apt-get update && sudo apt-get install -y linux-tools-common linux-tools-generic" >&2
    exit 1
fi

"$BPFTOOL" btf dump file /sys/kernel/btf/vmlinux format c > bpf/vmlinux.h
echo "[+] Generated bpf/vmlinux.h using $BPFTOOL"
