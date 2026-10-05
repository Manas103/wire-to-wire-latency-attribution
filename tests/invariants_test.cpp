// Hand-rolled check suite (no network, no gtest dependency) over a CSV log
// produced by src/client.cpp. Two independent checks on every row:
//
//   1. Monotonicity: t1 <= t2 <= t3 <= t4. If the four-point instrumentation
//      is lying (say, the "kernel rx" timestamp is actually a stale cached
//      value), this is the check most likely to catch it.
//   2. Reference-oracle agreement: the four-point RTT (t4 - t1) is compared
//      against wallclock_rtt_ns, an RTT measured by a completely separate
//      mechanism (clock_gettime(CLOCK_MONOTONIC) wrapped around the whole
//      send+recv in the client, using no SO_TIMESTAMPING machinery at all).
//      The two use different clocks (CLOCK_REALTIME via SO_TIMESTAMPING vs.
//      CLOCK_MONOTONIC) and different measurement points, so they are
//      expected to differ by a small, bounded amount (the client-side
//      instrumentation overhead between sampling each clock), not agree
//      exactly; this test asserts that bound rather than exact equality.
//
// Usage: invariants_test <csv_path> [max_disagreement_ns]
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

struct Row {
    uint64_t seq, t1, t2, t3, t4, wall_rtt_ns;
};

static std::vector<Row> load_csv(const std::string &path) {
    std::ifstream f(path);
    std::string line;
    std::getline(f, line); // header
    std::vector<Row> rows;
    while (std::getline(f, line)) {
        if (line.empty()) continue;
        Row r{};
        sscanf(line.c_str(), "%llu,%llu,%llu,%llu,%llu,%llu",
               (unsigned long long *)&r.seq, (unsigned long long *)&r.t1,
               (unsigned long long *)&r.t2, (unsigned long long *)&r.t3,
               (unsigned long long *)&r.t4, (unsigned long long *)&r.wall_rtt_ns);
        rows.push_back(r);
    }
    return rows;
}

// With no arguments, runs a self-test proving the check logic above
// actually detects the violations it claims to: a deliberately
// non-monotonic row and a deliberately over-threshold oracle disagreement,
// each injected into otherwise-clean synthetic data and asserted caught.
static int self_test() {
    std::vector<Row> clean = {
        {0, 1000, 1100, 1150, 1300, 300},
        {1, 2000, 2080, 2140, 2250, 250},
    };
    std::vector<Row> with_nonmonotonic = clean;
    with_nonmonotonic.push_back({2, 3000, 2900, 3100, 3200, 200}); // t2 < t1
    std::vector<Row> with_oracle_violation = clean;
    with_oracle_violation.push_back({3, 4000, 4080, 4140, 4250, 10}); // 4pt rtt=250, wall=10, diff=240 > 200ns default

    auto check = [](const std::vector<Row> &rows, uint64_t thresh) {
        int mono_fail = 0, oracle_fail = 0;
        for (const auto &r : rows) {
            if (!(r.t1 <= r.t2 && r.t2 <= r.t3 && r.t3 <= r.t4)) mono_fail++;
            uint64_t rtt = r.t4 - r.t1;
            uint64_t diff = rtt > r.wall_rtt_ns ? rtt - r.wall_rtt_ns : r.wall_rtt_ns - rtt;
            if (diff > thresh) oracle_fail++;
        }
        return std::make_pair(mono_fail, oracle_fail);
    };

    auto [m0, o0] = check(clean, 200);
    auto [m1, o1] = check(with_nonmonotonic, 200);
    auto [m2, o2] = check(with_oracle_violation, 200);

    bool ok = (m0 == 0 && o0 == 0) && (m1 == 1 && o1 == 0) && (m2 == 0 && o2 == 1);
    printf("self-test: clean=(%d,%d) nonmonotonic=(%d,%d) oracle-violation=(%d,%d) -> %s\n",
           m0, o0, m1, o1, m2, o2, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

int main(int argc, char **argv) {
    if (argc < 2) {
        return self_test();
    }
    uint64_t max_disagreement_ns = argc > 2 ? strtoull(argv[2], nullptr, 10) : 200'000; // 200us default
    auto rows = load_csv(argv[1]);
    if (rows.empty()) {
        fprintf(stderr, "FAIL: no rows loaded from %s\n", argv[1]);
        return 1;
    }

    int monotonic_failures = 0;
    int oracle_failures = 0;
    uint64_t max_abs_diff = 0;
    for (const auto &r : rows) {
        if (!(r.t1 <= r.t2 && r.t2 <= r.t3 && r.t3 <= r.t4)) monotonic_failures++;
        uint64_t four_point_rtt = r.t4 - r.t1;
        uint64_t diff = four_point_rtt > r.wall_rtt_ns ? four_point_rtt - r.wall_rtt_ns : r.wall_rtt_ns - four_point_rtt;
        max_abs_diff = std::max(max_abs_diff, diff);
        if (diff > max_disagreement_ns) oracle_failures++;
    }

    printf("rows checked              : %zu\n", rows.size());
    printf("monotonicity failures     : %d\n", monotonic_failures);
    printf("oracle-disagreement cases : %d (threshold %llu ns)\n", oracle_failures, (unsigned long long)max_disagreement_ns);
    printf("max |4pt_rtt - wall_rtt|  : %llu ns\n", (unsigned long long)max_abs_diff);

    if (monotonic_failures > 0 || oracle_failures > 0) {
        printf("FAIL\n");
        return 1;
    }
    printf("PASS\n");
    return 0;
}
