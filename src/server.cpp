// Server side of the wire-to-wire latency harness. Three RX modes share
// identical response logic (a plain UDP sendto) so the only thing that
// differs between runs is how the request's arrival is observed:
//   socket    plain blocking recvmsg(), default socket options
//   busypoll  SO_BUSY_POLL spin-polling recvmsg(), no blocking wakeup
//   xdp       AF_XDP generic-mode RX ring, busy-polled directly, bypassing
//             the kernel socket layer for the request leg entirely
//
// Usage: server <mode: socket|busypoll|xdp> <bind_ip> <bind_port> <iterations> [iface] [queue_id]
#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

#include "timestamping.hpp"
#include "wire_format.hpp"
#include "xdp_rx.hpp"

using namespace wwla;

namespace {

int make_request_socket(const char *bind_ip, int bind_port, bool busy_poll) {
    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); exit(1); }
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(bind_port);
    inet_pton(AF_INET, bind_ip, &addr.sin_addr);
    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        perror("bind"); exit(1);
    }
    enable_rx_software_timestamping(fd);
    if (busy_poll) {
        int budget_usec = 50;
        if (setsockopt(fd, SOL_SOCKET, SO_BUSY_POLL, &budget_usec, sizeof(budget_usec)) < 0) {
            perror("SO_BUSY_POLL (continuing without it)");
        }
#ifdef SO_PREFER_BUSY_POLL
        int prefer = 1;
        setsockopt(fd, SOL_SOCKET, SO_PREFER_BUSY_POLL, &prefer, sizeof(prefer));
#endif
    }
    return fd;
}

uint64_t recv_request_via_socket(int fd, RequestPayload *out_req, sockaddr_in *peer) {
    char cmsg_buf[1024];
    iovec iov{out_req, sizeof(*out_req)};
    msghdr msg{};
    msg.msg_name = peer;
    msg.msg_namelen = sizeof(*peer);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = cmsg_buf;
    msg.msg_controllen = sizeof(cmsg_buf);
    ssize_t n = recvmsg(fd, &msg, 0);
    if (n != sizeof(RequestPayload)) return 0;
    uint64_t t2 = extract_rx_timestamp_ns(&msg);
    if (t2 == 0) t2 = now_ns(); // fallback if the kernel did not attach a timestamp
    return t2;
}

} // namespace

int main(int argc, char **argv) {
    if (argc < 5) {
        fprintf(stderr, "usage: %s <socket|busypoll|xdp> <bind_ip> <bind_port> <iterations> [iface] [queue_id]\n", argv[0]);
        return 1;
    }
    std::string mode = argv[1];
    const char *bind_ip = argv[2];
    int bind_port = atoi(argv[3]);
    int iterations = atoi(argv[4]);
    std::string iface = argc > 5 ? argv[5] : "veth1";
    int queue_id = argc > 6 ? atoi(argv[6]) : 0;

    // Always-present response socket: unaffected by AF_XDP RX redirect,
    // which only steals ingress traffic on the bound queue, not egress.
    int reply_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (reply_fd < 0) { perror("reply socket"); return 1; }

    if (mode == "socket" || mode == "busypoll") {
        int fd = make_request_socket(bind_ip, bind_port, mode == "busypoll");
        fprintf(stderr, "server mode=%s listening on %s:%d, %d iterations\n", mode.c_str(), bind_ip, bind_port, iterations);
        for (int i = 0; i < iterations; i++) {
            sockaddr_in peer{};
            RequestPayload req{};
            uint64_t t2 = recv_request_via_socket(fd, &req, &peer);
            uint64_t t3 = now_ns();
            ResponsePayload resp{};
            resp.seq = req.seq;
            resp.t1_client_send_ns = req.t1_client_send_ns;
            resp.t2_server_rx_ns = t2;
            resp.t3_server_send_ns = t3;
            sendto(reply_fd, &resp, sizeof(resp), 0, reinterpret_cast<sockaddr *>(&peer), sizeof(peer));
        }
        close(fd);
    } else if (mode == "xdp") {
        XdpRxSocket xsk(iface, queue_id);
        fprintf(stderr, "server mode=xdp listening on %s queue %d, %d iterations\n", iface.c_str(), queue_id, iterations);
        sockaddr_in peer{};
        peer.sin_family = AF_INET;
        peer.sin_port = htons(bind_port + 1); // client's response-listening port, fixed convention
        // Peer IP is fixed to the known client address for this harness (see scripts/run_sweep.sh).
        inet_pton(AF_INET, argc > 7 ? argv[7] : "10.200.0.1", &peer.sin_addr);

        int received = 0;
        uint64_t spins = 0;
        const uint64_t max_spins_per_packet = 20'000'000ull; // generous deadline, not a correctness bound
        while (received < iterations) {
            const uint8_t *payload = nullptr;
            uint16_t len = 0;
            if (!xsk.try_recv(&payload, &len)) {
                spins++;
                if (spins > max_spins_per_packet) {
                    fprintf(stderr, "xdp server: timed out waiting for packet %d/%d\n", received, iterations);
                    xsk.print_debug_counters();
                    break;
                }
                continue;
            }
            spins = 0;
            if (getenv("WWLA_XDP_DEBUG")) {
                fprintf(stderr, "xdp frame: len=%u dst_port=%u\n", len, xsk.last_dst_port());
            }
            if (xsk.last_dst_port() != static_cast<uint16_t>(bind_port) || len < sizeof(RequestPayload)) {
                xsk.recycle_last();
                continue;
            }
            uint64_t t2 = now_ns(); // observed immediately on busy-polled ring peek
            RequestPayload req{};
            memcpy(&req, payload, sizeof(RequestPayload));
            xsk.recycle_last();
            uint64_t t3 = now_ns();
            ResponsePayload resp{};
            resp.seq = req.seq;
            resp.t1_client_send_ns = req.t1_client_send_ns;
            resp.t2_server_rx_ns = t2;
            resp.t3_server_send_ns = t3;
            sendto(reply_fd, &resp, sizeof(resp), 0, reinterpret_cast<sockaddr *>(&peer), sizeof(peer));
            received++;
        }
    } else {
        fprintf(stderr, "unknown mode: %s\n", mode.c_str());
        return 1;
    }
    close(reply_fd);
    return 0;
}
