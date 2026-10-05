// SO_TIMESTAMPING helpers: enable kernel software RX timestamps on a UDP
// socket and extract them from a recvmsg() control-message buffer.
//
// SOF_TIMESTAMPING_RX_SOFTWARE stamps the packet at the point the kernel's
// network stack (softirq, right after the driver's NAPI poll hands the skb
// up) takes ownership of it -- the earliest timestamp a plain socket can
// see without real NIC hardware timestamping support. That is the "kernel"
// point in this project's four-point scheme; see README "Honest framing".
#pragma once

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <sys/socket.h>
#include <linux/net_tstamp.h>
#include <linux/errqueue.h>
#include <time.h>

namespace wwla {

inline void enable_rx_software_timestamping(int fd) {
    int flags = SOF_TIMESTAMPING_RX_SOFTWARE | SOF_TIMESTAMPING_SOFTWARE;
    if (setsockopt(fd, SOL_SOCKET, SO_TIMESTAMPING, &flags, sizeof(flags)) < 0) {
        throw std::runtime_error(std::string("SO_TIMESTAMPING: ") + strerror(errno));
    }
}

inline uint64_t ns_from_timespec(const timespec &ts) {
    return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ull + static_cast<uint64_t>(ts.tv_nsec);
}

// Extracts the SOF_TIMESTAMPING_RX_SOFTWARE timestamp from a cmsg buffer
// filled by recvmsg(). Returns 0 if no software timestamp control message
// was present (caller should treat that as "fall back to ktime_now()").
inline uint64_t extract_rx_timestamp_ns(msghdr *msg) {
    for (cmsghdr *cmsg = CMSG_FIRSTHDR(msg); cmsg != nullptr; cmsg = CMSG_NXTHDR(msg, cmsg)) {
        if (cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SO_TIMESTAMPING) {
            auto *ts = reinterpret_cast<timespec *>(CMSG_DATA(cmsg));
            // ts[0] = software timestamp, ts[1] = deprecated, ts[2] = hardware (unused, no HW clock here)
            if (ts[0].tv_sec != 0 || ts[0].tv_nsec != 0) {
                return ns_from_timespec(ts[0]);
            }
        }
    }
    return 0;
}

inline uint64_t now_ns() {
    timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ns_from_timespec(ts);
}

} // namespace wwla
