// Minimal XDP program redirecting every frame on a queue into this
// project's AF_XDP socket for that queue. libbpf 0.5 (the version on this
// build host) does not auto-attach a redirect program the way newer
// libxdp-based helpers do, so this program is written, compiled to BPF
// bytecode and loaded explicitly (see XdpRxSocket::XdpRxSocket in
// include/xdp_rx.hpp). XDP_PASS as the fallback (no matching map entry)
// means an unexpected queue index degrades to "let the kernel stack handle
// it" rather than dropping traffic silently.
#include <linux/bpf.h>
#include <bpf/bpf_helpers.h>

struct {
    __uint(type, BPF_MAP_TYPE_XSKMAP);
    __uint(max_entries, 64);
    __type(key, __u32);
    __type(value, __u32);
} xsks_map SEC(".maps");

// Debug counters, readable from userspace without debugfs/tracing
// permissions: [0] = program invocations, [1] = last observed
// rx_queue_index, [2] = last bpf_redirect_map return value (cast to u32).
struct {
    __uint(type, BPF_MAP_TYPE_ARRAY);
    __uint(max_entries, 3);
    __type(key, __u32);
    __type(value, __u32);
} dbg_map SEC(".maps");

SEC("xdp")
int xdp_redirect_prog(struct xdp_md *ctx) {
    __u32 k0 = 0, k1 = 1, k2 = 2;
    __u32 *count = bpf_map_lookup_elem(&dbg_map, &k0);
    if (count) (*count)++;
    __u32 index = ctx->rx_queue_index;
    bpf_map_update_elem(&dbg_map, &k1, &index, 0);
    long ret = bpf_redirect_map(&xsks_map, index, XDP_PASS);
    __u32 ret32 = (__u32)ret;
    bpf_map_update_elem(&dbg_map, &k2, &ret32, 0);
    return ret;
}

char _license[] SEC("license") = "GPL";
