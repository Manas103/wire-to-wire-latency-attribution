// Fixed wire format for the request/response payloads this harness sends.
// Not a real exchange protocol; see README "Honest framing".
#pragma once
#include <cstdint>

namespace wwla {

#pragma pack(push, 1)
struct RequestPayload {
    uint64_t seq;
    uint64_t t1_client_send_ns;
};

struct ResponsePayload {
    uint64_t seq;
    uint64_t t1_client_send_ns;
    uint64_t t2_server_rx_ns;
    uint64_t t3_server_send_ns;
};
#pragma pack(pop)

} // namespace wwla
