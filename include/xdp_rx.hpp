// Minimal AF_XDP RX-only wrapper, generic (SKB) mode. See README "Honest
// framing": this is generic-mode AF_XDP (the kernel's software XDP hook,
// right after the driver's NAPI poll hands the frame up), not native/
// zero-copy driver-mode XDP, because the NICs available on this build host
// (WSL2 hv_netvsc uplink, Linux veth pair) do not advertise native XDP
// support for a raw AF_XDP RX ring. The socket and umem setup below is
// still exercising the real AF_XDP kernel path (xsk_socket__create,
// XDP_COPY umem, a real redirect program the kernel auto-attaches), not a
// simulation of one.
#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <sys/mman.h>
#include <net/if.h>
#include <bpf/bpf.h>
#include <bpf/libbpf.h>
#include <bpf/xsk.h>
#include <linux/if_link.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/udp.h>

#ifndef XDP_BPF_OBJ_PATH
#define XDP_BPF_OBJ_PATH "xdp_redirect.bpf.o"
#endif

namespace wwla {

constexpr int kUmemFrames = 4096;
constexpr int kFrameSize = XSK_UMEM__DEFAULT_FRAME_SIZE;

class XdpRxSocket {
public:
    XdpRxSocket(const std::string &ifname, int queue_id) : ifname_(ifname) {
        ifindex_ = if_nametoindex(ifname.c_str());
        if (!ifindex_) throw std::runtime_error("if_nametoindex failed for " + ifname);

        size_t umem_sz = static_cast<size_t>(kUmemFrames) * kFrameSize;
        umem_area_ = mmap(nullptr, umem_sz, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (umem_area_ == MAP_FAILED) throw std::runtime_error("mmap umem failed");

        xsk_umem_config umem_cfg{};
        umem_cfg.fill_size = XSK_RING_PROD__DEFAULT_NUM_DESCS;
        umem_cfg.comp_size = XSK_RING_CONS__DEFAULT_NUM_DESCS;
        umem_cfg.frame_size = kFrameSize;
        umem_cfg.frame_headroom = XSK_UMEM__DEFAULT_FRAME_HEADROOM;
        int ret = xsk_umem__create(&umem_, umem_area_, umem_sz, &fq_, &cq_, &umem_cfg);
        if (ret) throw std::runtime_error("xsk_umem__create: " + std::string(strerror(-ret)));

        xsk_socket_config xsk_cfg{};
        xsk_cfg.rx_size = XSK_RING_CONS__DEFAULT_NUM_DESCS;
        xsk_cfg.tx_size = XSK_RING_PROD__DEFAULT_NUM_DESCS;
        // libbpf <1.0 auto-loads its own default redirect program unless told
        // not to; this project loads and attaches its own (see below), so the
        // library's default would otherwise collide with it (EBUSY, "Can't
        // replace active BPF XDP link").
        xsk_cfg.libbpf_flags = XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD;
        xsk_cfg.xdp_flags = XDP_FLAGS_SKB_MODE;
        xsk_cfg.bind_flags = XDP_COPY;
        ret = xsk_socket__create(&xsk_, ifname.c_str(), queue_id, umem_, &rx_, nullptr, &xsk_cfg);
        if (ret) throw std::runtime_error("xsk_socket__create: " + std::string(strerror(-ret)));

        // Seed the fill queue with every frame so the kernel has somewhere to land packets.
        uint32_t idx;
        uint32_t n = xsk_ring_prod__reserve(&fq_, kUmemFrames, &idx);
        for (uint32_t i = 0; i < n; i++) {
            *xsk_ring_prod__fill_addr(&fq_, idx + i) = static_cast<uint64_t>(i) * kFrameSize;
        }
        xsk_ring_prod__submit(&fq_, n);

        // libbpf 0.5 (this build host's version) does not auto-attach a
        // redirect program the way newer libxdp helpers do; load and wire
        // one up explicitly, or no frame will ever reach this socket.
        bpf_obj_ = bpf_object__open_file(XDP_BPF_OBJ_PATH, nullptr);
        if (!bpf_obj_ || libbpf_get_error(bpf_obj_)) {
            throw std::runtime_error(std::string("bpf_object__open_file failed for ") + XDP_BPF_OBJ_PATH);
        }
        if (bpf_object__load(bpf_obj_) != 0) {
            throw std::runtime_error("bpf_object__load failed");
        }
        struct bpf_program *prog = bpf_object__find_program_by_name(bpf_obj_, "xdp_redirect_prog");
        if (!prog) throw std::runtime_error("xdp program not found in object");
        int prog_fd = bpf_program__fd(prog);
        int map_fd = bpf_object__find_map_fd_by_name(bpf_obj_, "xsks_map");
        if (map_fd < 0) throw std::runtime_error("xsks_map not found in object");
        dbg_map_fd_ = bpf_object__find_map_fd_by_name(bpf_obj_, "dbg_map");
        int xsk_fd = xsk_socket__fd(xsk_);
        // veth allocates one RX queue struct per possible CPU regardless of
        // ethtool's "active queue count", and a packet's xdp_md rx_queue_index
        // depends on which CPU's softirq processed it, so the single queue_id
        // this project binds to is not the only index real traffic can arrive
        // on. Map every index this program's xsks_map can hold to the same
        // socket rather than just queue_id, or most packets fall through to
        // XDP_PASS and this socket never sees them.
        for (uint32_t key = 0; key < 64; key++) {
            if (bpf_map_update_elem(map_fd, &key, &xsk_fd, 0) != 0) {
                throw std::runtime_error("bpf_map_update_elem(xsks_map) failed");
            }
        }
        if (bpf_set_link_xdp_fd(static_cast<int>(ifindex_), prog_fd, XDP_FLAGS_SKB_MODE) != 0) {
            throw std::runtime_error("bpf_set_link_xdp_fd failed (attaching redirect program)");
        }
        attached_ = true;
    }

    ~XdpRxSocket() {
        if (attached_) bpf_set_link_xdp_fd(static_cast<int>(ifindex_), -1, XDP_FLAGS_SKB_MODE);
        if (bpf_obj_) bpf_object__close(bpf_obj_);
        if (xsk_) xsk_socket__delete(xsk_);
        if (umem_) xsk_umem__delete(umem_);
    }

    int fd() const { return xsk_socket__fd(xsk_); }

    void print_debug_counters() const {
        if (dbg_map_fd_ < 0) return;
        uint32_t invocations = 0, last_queue = 0, last_ret = 0;
        uint32_t k0 = 0, k1 = 1, k2 = 2;
        bpf_map_lookup_elem(dbg_map_fd_, &k0, &invocations);
        bpf_map_lookup_elem(dbg_map_fd_, &k1, &last_queue);
        bpf_map_lookup_elem(dbg_map_fd_, &k2, &last_ret);
        fprintf(stderr, "xdp debug: program invocations=%u last_rx_queue_index=%u last_redirect_ret=%u (XDP_REDIRECT=4, XDP_PASS=2)\n",
                invocations, last_queue, last_ret);
    }

    // Busy-polls the RX ring for up to spin_budget iterations; on success
    // fills udp_payload (pointer into umem, valid until recycle_last() is
    // called) and its length, plus the raw frame's rx timestamp (now_ns()
    // at the moment the descriptor was observed -- see header comment).
    bool try_recv(const uint8_t **udp_payload, uint16_t *payload_len) {
        uint32_t idx;
        if (xsk_ring_cons__peek(&rx_, 1, &idx) != 1) return false;
        const xdp_desc *desc = xsk_ring_cons__rx_desc(&rx_, idx);
        uint8_t *frame = static_cast<uint8_t *>(umem_area_) + desc->addr;
        last_addr_ = desc->addr;
        xsk_ring_cons__release(&rx_, 1);

        if (getenv("WWLA_XDP_DEBUG")) {
            fprintf(stderr, "xdp raw desc: addr=%llu len=%u\n",
                    (unsigned long long)desc->addr, desc->len);
        }
        if (desc->len < sizeof(ethhdr) + sizeof(iphdr) + sizeof(udphdr)) return false;
        auto *eth = reinterpret_cast<ethhdr *>(frame);
        if (ntohs(eth->h_proto) != ETH_P_IP) {
            if (getenv("WWLA_XDP_DEBUG")) fprintf(stderr, "  not IP, h_proto=0x%04x\n", ntohs(eth->h_proto));
            return false;
        }
        auto *ip = reinterpret_cast<iphdr *>(frame + sizeof(ethhdr));
        if (ip->protocol != IPPROTO_UDP) {
            if (getenv("WWLA_XDP_DEBUG")) fprintf(stderr, "  not UDP, protocol=%u\n", ip->protocol);
            return false;
        }
        size_t ip_hdr_len = static_cast<size_t>(ip->ihl) * 4;
        auto *udp = reinterpret_cast<udphdr *>(frame + sizeof(ethhdr) + ip_hdr_len);
        size_t udp_hdr_total = sizeof(ethhdr) + ip_hdr_len + sizeof(udphdr);
        if (desc->len < udp_hdr_total) return false;
        *udp_payload = frame + udp_hdr_total;
        *payload_len = static_cast<uint16_t>(desc->len - udp_hdr_total);
        last_dst_port_ = ntohs(udp->dest);
        return true;
    }

    uint16_t last_dst_port() const { return last_dst_port_; }

    // Returns the frame to the fill queue so the kernel can reuse it.
    void recycle_last() {
        uint32_t idx;
        if (xsk_ring_prod__reserve(&fq_, 1, &idx) == 1) {
            *xsk_ring_prod__fill_addr(&fq_, idx) = last_addr_;
            xsk_ring_prod__submit(&fq_, 1);
        }
    }

private:
    void *umem_area_ = nullptr;
    xsk_umem *umem_ = nullptr;
    xsk_socket *xsk_ = nullptr;
    xsk_ring_prod fq_{};
    xsk_ring_cons cq_{};
    xsk_ring_cons rx_{};
    uint64_t last_addr_ = 0;
    uint16_t last_dst_port_ = 0;
    std::string ifname_;
    unsigned int ifindex_ = 0;
    struct bpf_object *bpf_obj_ = nullptr;
    bool attached_ = false;
    int dbg_map_fd_ = -1;
};

} // namespace wwla
