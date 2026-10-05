// Client side of the wire-to-wire latency harness. Always uses a plain UDP
// socket with SO_TIMESTAMPING enabled for the response leg (t4), regardless
// of which RX mode the server is running, because the response always
// travels back over a normal socket (see server.cpp). Paces requests at a
// fixed offered load and logs all four timestamps plus an independently
// measured wall-clock RTT per round trip to a CSV file.
//
// Usage: client <server_ip> <server_port> <my_ip> <my_port> <iterations> <rate_per_sec> <out_csv>
#include <arpa/inet.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "timestamping.hpp"
#include "wire_format.hpp"

using namespace wwla;

int main(int argc, char **argv) {
    if (argc < 8) {
        fprintf(stderr, "usage: %s <server_ip> <server_port> <my_ip> <my_port> <iterations> <rate_per_sec> <out_csv>\n", argv[0]);
        return 1;
    }
    const char *server_ip = argv[1];
    int server_port = atoi(argv[2]);
    const char *my_ip = argv[3];
    int my_port = atoi(argv[4]);
    int iterations = atoi(argv[5]);
    double rate = atof(argv[6]);
    const char *out_csv = argv[7];

    int fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (fd < 0) { perror("socket"); return 1; }
    sockaddr_in me{};
    me.sin_family = AF_INET;
    me.sin_port = htons(my_port);
    inet_pton(AF_INET, my_ip, &me.sin_addr);
    if (bind(fd, reinterpret_cast<sockaddr *>(&me), sizeof(me)) < 0) { perror("bind"); return 1; }
    enable_rx_software_timestamping(fd);

    sockaddr_in server{};
    server.sin_family = AF_INET;
    server.sin_port = htons(server_port);
    inet_pton(AF_INET, server_ip, &server.sin_addr);

    FILE *csv = fopen(out_csv, "w");
    if (!csv) { perror("fopen"); return 1; }
    fprintf(csv, "seq,t1,t2,t3,t4,wallclock_rtt_ns\n");

    uint64_t interval_ns = static_cast<uint64_t>(1e9 / rate);
    uint64_t next_send = now_ns();

    for (int i = 0; i < iterations; i++) {
        while (now_ns() < next_send) { /* busy-wait pacing */ }
        next_send += interval_ns;

        RequestPayload req{};
        req.seq = static_cast<uint64_t>(i);
        timespec wall_start;
        clock_gettime(CLOCK_MONOTONIC, &wall_start);
        req.t1_client_send_ns = now_ns();
        sendto(fd, &req, sizeof(req), 0, reinterpret_cast<sockaddr *>(&server), sizeof(server));

        char cmsg_buf[1024];
        ResponsePayload resp{};
        iovec iov{&resp, sizeof(resp)};
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = cmsg_buf;
        msg.msg_controllen = sizeof(cmsg_buf);
        ssize_t n = recvmsg(fd, &msg, 0);
        timespec wall_end;
        clock_gettime(CLOCK_MONOTONIC, &wall_end);
        if (n != sizeof(ResponsePayload)) { fprintf(stderr, "short read at seq %d\n", i); continue; }

        uint64_t t4 = extract_rx_timestamp_ns(&msg);
        if (t4 == 0) t4 = now_ns();
        uint64_t wall_rtt_ns = (static_cast<uint64_t>(wall_end.tv_sec) * 1'000'000'000ull + wall_end.tv_nsec) -
                               (static_cast<uint64_t>(wall_start.tv_sec) * 1'000'000'000ull + wall_start.tv_nsec);

        fprintf(csv, "%llu,%llu,%llu,%llu,%llu,%llu\n",
                static_cast<unsigned long long>(resp.seq),
                static_cast<unsigned long long>(resp.t1_client_send_ns),
                static_cast<unsigned long long>(resp.t2_server_rx_ns),
                static_cast<unsigned long long>(resp.t3_server_send_ns),
                static_cast<unsigned long long>(t4),
                static_cast<unsigned long long>(wall_rtt_ns));
    }
    fclose(csv);
    close(fd);
    fprintf(stderr, "client done: %d iterations, log at %s\n", iterations, out_csv);
    return 0;
}
