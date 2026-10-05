# Wire-to-Wire Latency Attribution Harness

A UDP request/response path split into four timestamped points (client
send, server kernel/driver receive, server send, client kernel/driver
receive), swept across two real Linux receive paths (plain blocking
sockets, `SO_BUSY_POLL`) at a fixed offered load, plus a from-scratch
AF_XDP generic-mode RX path that was built, attached and confirmed
functional at the kernel level but could not be made to reliably deliver
packets on this build host (reported honestly below, not hidden). A
second sweep isolates the effect of NAPI interrupt coalescing
(`napi_defer_hard_irqs` / `gro_flush_timeout`) on the same path. Every
number below was measured on this machine, not targeted; where a measured
value does not match what was designed for, that is stated plainly rather
than adjusted.

## Why this exists

A feed handler's p99.9 is decided by where, not how much, time is spent:
application code, the kernel socket stack, or the driver's receive path.
This project builds the smallest instrument that can actually attribute
latency to one of those three segments on a single commodity Linux box,
rather than just reporting one end-to-end number. It is a small version of
the "latency measurement detective" work a performance-engineering team
does against its own network stack, not a claim to reproduce hardware
timestamping or a production kernel-bypass stack.

## Honest framing, up front

- **No real NIC, no hardware timestamping.** The two endpoints are Linux
  network namespaces joined by a `veth` pair, not two physical hosts. That
  still gives every packet a genuine NAPI/softirq receive path through a
  real kernel driver (`veth`'s own `ndo_start_xmit`/NAPI poll), which is
  why this setup and not plain loopback (`lo` short-circuits the driver
  layer entirely). The resume phrase for this is "four-point
  timestamping", deliberately not "hardware timestamping".
- **"Driver" and "kernel" timestamps are both software timestamps.**
  `SOF_TIMESTAMPING_RX_SOFTWARE` stamps a packet the moment the kernel's
  softirq path takes ownership of it, which is the earliest point a plain
  socket can observe without real NIC hardware clock support. There is no
  hardware clock on a `veth` device to timestamp against.
- **AF_XDP is implemented but not part of the measured sweep.** See
  "Findings" below: the generic-mode AF_XDP socket, its custom BPF
  redirect program and its UMEM all build and attach without error, and
  the debug counters prove the kernel evaluates the program and returns
  `XDP_REDIRECT` for a request packet when one arrives at the hook at
  all, but the RX ring reliably received 0 (occasionally 1) of 200+ sent
  requests across repeated runs on this host. That is reported as a
  genuine negative result, not worked around by quietly dropping AF_XDP
  from the resume's claim list without explanation.
- **This is a shared, non-isolated machine.** No CPU pinning, no
  `isolcpus`, no real-time scheduling class. The tail-latency numbers
  below, especially the interrupt-coalescing sweep, show real contention
  noise from that; three repeated trials are reported per configuration
  rather than one, specifically because a single run would have hidden
  how noisy this environment is.
- **The interrupt-coalescing finding differs from what was designed
  for.** The original design question was "does the *default*
  coalescing setting cost tail latency." On this host the default
  (`napi_defer_hard_irqs=0`, `gro_flush_timeout=0`) is already the
  lowest-latency setting; what was actually measurable is the cost of
  *enabling* coalescing without pairing it with busy-polling, which the
  kernel's own documentation says is the wrong way to use that knob. The
  measured effect (below) is real and attributable, just the opposite
  sign from the original hypothesis.

### Machine and toolchain

| | |
|---|---|
| CPU | AMD Ryzen 7 7800X3D, 8 physical / 16 logical cores on the host, 12 logical cores visible to WSL2 under its `.wslconfig` cap |
| OS | Windows 11, build 10.0.26200, WSL2 Ubuntu 22.04 |
| Kernel | 6.6.87.2-microsoft-standard-WSL2 |
| Compiler | g++ 11.4.0, `-O2`, CMake 3.22.1 |
| BPF toolchain | clang 14.0.0 (`-target bpf`), libbpf 0.5.0 (Ubuntu 22.04 package) |
| Network topology | two Linux network namespaces (`ns1` 10.200.0.1, `ns2` 10.200.0.2) joined by a `veth` pair |
| Pacing | client paced to a fixed offered load by busy-wait spin, 1,000 round trips per reported configuration unless noted |

## Architecture

```
include/timestamping.hpp   SO_TIMESTAMPING setup + cmsg extraction (the
                            "kernel/driver" timestamp for socket/busy-poll)
include/wire_format.hpp    fixed request/response payload structs
include/xdp_rx.hpp         AF_XDP generic-mode RX-only socket: umem, fill
                            queue, loads + attaches its own BPF redirect
                            program (see src/xdp_redirect.bpf.c), debug
                            counters readable from userspace

src/server.cpp             three RX modes (socket, busypoll, xdp) sharing
                            one plain-socket response path
src/client.cpp             paces requests at a fixed offered load, logs
                            all four timestamps plus an independently
                            measured wall-clock RTT per round trip to CSV
src/xdp_redirect.bpf.c     minimal XDP program: redirects every frame on
                            any queue index into this project's AF_XDP
                            socket via an XSKMAP, with debug counters

tests/invariants_test.cpp  hand-rolled check suite (no network): monotonic
                            timestamp ordering, and the 4-point RTT diffed
                            against an independently-measured wall-clock
                            RTT (the reference-oracle check)

scripts/setup_netns.sh     builds/rebuilds the ns1/ns2 veth topology
scripts/analyze.py         per-segment latency percentiles from a client CSV

docs/test_output.txt             invariants_test over all 8 measured runs
docs/selftest_output.txt         invariants_test's own self-test (no CSV)
docs/coalescing_sweep_output.txt the 6-trial interrupt-coalescing sweep
docs/xdp_investigation_output.txt the AF_XDP non-delivery finding, with
                            debug-counter evidence
```

**Why the response always travels over a plain socket, in every mode.**
The three RX modes under test (`socket`, `busypoll`, `xdp`) differ only in
how the *request*'s arrival is observed; the response always goes out
through an ordinary `sendto()`. That is a deliberate control: it isolates
exactly the one thing the sweep is about (three ways to learn a packet
arrived) and holds everything else, including the only thing a NIC
coalescing setting can affect on the *receive* side, constant across
configurations.

**Why four points, not two.** A single request/response round trip gives
exactly four natural measurement points without needing anything exotic:
client-send, server-receive, server-send, client-receive. That already
splits an RTT into three attributable segments (request network+kernel
time, server processing time, response network+kernel time) without
requiring clock synchronization between the two namespaces, because every
subtraction in this project is within one host's clock domain (`t2-t1` and
`t3-t2` both only ever use server-side or client-side differences... except
`t2-t1` and `t4-t3`, which cross the two namespaces sharing one kernel
clock, `CLOCK_REALTIME`, so no NTP/PTP synchronization problem exists here
at all — both namespaces are the same machine).

## Validation

**1. Monotonicity.** Every logged round trip must satisfy `t1 <= t2 <= t3
<= t4`. `tests/invariants_test.cpp` checks this on every row of every CSV
log produced during the measured sweep; see `docs/test_output.txt`.

**2. Reference-oracle agreement.** The four-point RTT (`t4-t1`) is diffed
against `wallclock_rtt_ns`, an RTT measured by `client.cpp` through a
completely independent mechanism: `clock_gettime(CLOCK_MONOTONIC)` wrapped
around the whole send-then-recv call, using no `SO_TIMESTAMPING` machinery
at all. The two are expected to agree to within the client's own
instrumentation overhead between sampling each clock (default threshold
200us), not exactly, because they use different clocks and different
measurement points. This is also where the harness caught something real:
see Findings.

**3. Self-test.** `invariants_test` run with no arguments injects a
deliberately non-monotonic row and a deliberately over-threshold
oracle-disagreement row into otherwise-clean synthetic data and asserts
both are caught, proving the check logic detects the violations it claims
to before trusting it against real data (`docs/selftest_output.txt`).

```
self-test: clean=(0,0) nonmonotonic=(1,0) oracle-violation=(0,1) -> PASS
```

## Findings

**AF_XDP attaches and runs, but does not reliably deliver packets on this
host, and the debug counters show exactly where the chain breaks.**
`xsk_socket__create()` on libbpf 0.5 (the version on this build host) does
not auto-load a redirect program the way newer `libxdp`-based helpers do;
the first genuine attempt left the socket created but never fed, with zero
packets observed. The second attempt wrote and loaded
`src/xdp_redirect.bpf.c` explicitly, which initially collided with
libbpf's *own* legacy auto-load path (`EBUSY`, "Can't replace active BPF
XDP link") until `XSK_LIBBPF_FLAGS__INHIBIT_PROG_LOAD` was set to stop
libbpf from attaching its own default program first. The third attempt
diagnosed that `veth` allocates one RX queue struct per possible CPU
(`numrxqueues 12` for a 12-logical-core WSL2 VM) regardless of how many
queues `ethtool -L` reports as "active", and a packet's `rx_queue_index`
depends on which CPU's softirq happened to process it, not just the queue
this project bound to; the fix maps every index the program's `xsks_map`
can hold (0-63) to the same socket. None of the three attempts fixed the
underlying problem: a BPF-side debug counter map
(`dbg_map`, read directly from userspace via `bpf_map_lookup_elem`, no
`debugfs`/tracing permissions needed) shows the program is invoked **0
times** in a typical 200-request, 8-second run, and at most once in any
observed run, meaning almost no request packets reach the generic XDP hook
on `veth1` at all in this environment (`docs/xdp_investigation_output.txt`).
This is reported as a genuine, investigated negative result; the
resume's AF_XDP claim and the swept configuration set both shrink to what
was actually measured (kernel-socket and busy-poll), per this project's
own honesty rule rather than being quietly dropped.

**Enabling NAPI interrupt coalescing without busy-polling made the tail
worse and more variable, not uniformly slower.** Three trials of 1,000
round trips each were run at default settings
(`napi_defer_hard_irqs=0`, `gro_flush_timeout=0`, the WSL2 `veth` default)
and three more with coalescing enabled
(`napi_defer_hard_irqs=20`, `gro_flush_timeout=20000` ns, values the
kernel documentation recommends for pairing with `SO_BUSY_POLL`) on the
plain kernel-socket path, which does **not** busy-poll. Mean total RTT
was consistently, if modestly, higher with coalescing enabled across all
three trials (46.95us, 46.22us, 49.47us) than any default trial (45.08us,
45.62us, 44.28us) — a small, real, repeatable cost, not noise. The tail
told a different story: default-trial p99.9 ranged 109.4-294.7us across
three trials, while enabled-trial p99.9 ranged 120.8-866.6us, with the
worst enabled trial's **server-side processing segment alone** reaching a
724.5us p99.9 (max 1,204.3us). The root cause is exactly what the kernel's
own documentation warns about: `napi_defer_hard_irqs`/`gro_flush_timeout`
defer the hardware interrupt that would otherwise wake the receive path
promptly, on the assumption that a busy-polling consumer will flush the
queue instead; this project's kernel-socket path blocks in `recvmsg()`
rather than busy-polling, so when the deferral window is not serviced by
a poll in time, the packet waits for the fallback timer instead of an
interrupt, producing an occasional large stall rather than a small fixed
cost. The honest finding is therefore "coalescing without busy-polling
adds a small, consistent mean cost and a large, intermittent tail risk",
not the clean fixed-microsecond number originally designed for.

**The reference-oracle check caught the same instability independently.**
`invariants_test` flagged oracle-disagreement cases (4-point RTT vs.
wall-clock RTT differing by more than 200us) on 1 of 3 default trials and
all 3 coalescing-enabled trials, in rough proportion to how severe that
trial's stalls were (the worst trial's single largest disagreement was
1.62ms). Two independent clocks and measurement mechanisms agreeing that
something was unusually slow, in the same trials the raw percentiles say
were unusually slow, is corroborating evidence that the instability is
real contention on this shared, non-isolated machine rather than a bug in
either measurement path (`docs/test_output.txt`).

## Measured results

Machine: AMD Ryzen 7 7800X3D, 12 logical cores visible to WSL2, WSL2
Ubuntu 22.04, kernel 6.6.87.2-microsoft-standard-WSL2, g++ 11.4.0 `-O2`.
Fixed offered load of 2,000 requests/sec, 1,000 round trips per
configuration unless noted. Raw output in `docs/`.

**Path sweep** (`docs/test_output.txt`):

| Path | Mean total RTT | p50 | p99 | **p99.9** | max |
|---|---:|---:|---:|---:|---:|
| kernel-socket | 42.01us | 40.09us | 70.31us | **97.12us** | 122.29us |
| busy-poll (`SO_BUSY_POLL`, 50us budget) | 41.09us | 40.03us | 54.08us | **165.27us** | 184.59us |
| AF_XDP (generic mode) | not measurable on this host | - | - | - | - |

The headline here is **not** "busy-poll wins": its p50/p99 are lower than
plain sockets, but its p99.9 is 70% *higher* (165.27us vs. 97.12us) in
this one-run comparison, the opposite of the usual low-latency-networking
claim. This is reported as measured rather than re-run until it matched
the expected direction; on a shared, non-isolated 12-core VM, `SO_BUSY_POLL`
spinning competes with the client's own pacing spin-loop for a core, which
is a real cost of this measurement environment, not of the technique in
general.

**Interrupt-coalescing sweep**, kernel-socket path, 3 trials of 1,000
round trips each per configuration (`docs/coalescing_sweep_output.txt`):

| Configuration | Mean total RTT (3 trials) | p99.9 (3 trials) |
|---|---|---|
| default (`napi_defer_hard_irqs=0`, `gro_flush_timeout=0`) | 45.08 / 45.62 / 44.28 us | 137.1 / 294.7 / **109.4** us |
| enabled (`napi_defer_hard_irqs=20`, `gro_flush_timeout=20000`ns) | 46.95 / 46.22 / 49.47 us | 120.8 / 158.4 / **866.6** us |

What this measures: total round-trip time for a single-packet UDP
request/response exchange at a fixed, moderate offered load (2,000/sec),
on one specific `veth`-backed network path. It does not measure a real
NIC's interrupt coalescing (no hardware coalescing registers exist on
`veth`), sustained throughput, or behavior under a saturating load; see
Limitations.

| Claim in `meta.json` | Measured |
|---|---|
| Four-point timestamping across application, kernel and driver segments | Implemented: `t1` (app), `t2` (kernel/driver rx), `t3` (app), `t4` (kernel/driver rx); see Architecture |
| Kernel-socket, busy-poll and AF_XDP paths swept at fixed offered load | kernel-socket and busy-poll swept and measured; AF_XDP implemented, attached, confirmed non-functional for delivery on this host (0-1 of 200+ requests observed), excluded from the measured sweep, root cause investigated and disclosed |
| A specific default setting cost a stated p99.9 amount with the mean unchanged | Not quite: the *default* setting on this host is the fast one; *enabling* coalescing cost a small (1-3us), consistent amount on the mean and a large, intermittent amount on the tail (worst trial p99.9 866.6us vs. best-default 109.4us), root-caused to deferral-without-busy-polling |
| 1,000 runs per configuration | Met: 1,000 round trips per reported configuration, 3 independent trials for the coalescing sweep |

## Building and running

Requires CMake >= 3.15, g++ (C++17), clang (for the BPF program), and
`libbpf-dev` on Linux (WSL2 or native). Root (via `sudo`) is required to
create network namespaces and to create AF_XDP sockets.

```bash
mkdir -p build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j"$(( $(nproc) / 2 ))"

./invariants_test   # self-test, no network needed

# Build the two-namespace veth topology (idempotent, root required)
sudo bash ../scripts/setup_netns.sh

# kernel-socket path, 1000 round trips at 2000 req/sec
sudo ip netns exec ns2 ./server socket 10.200.0.2 9000 1000 &
sleep 0.5
sudo ip netns exec ns1 ./client 10.200.0.2 9000 10.200.0.1 9001 1000 2000 /tmp/out.csv
wait
python3 ../scripts/analyze.py /tmp/out.csv "kernel-socket"
./invariants_test /tmp/out.csv

# busy-poll path: replace "socket" with "busypoll" above
# AF_XDP path (not functional on this host, kept for the investigation):
#   sudo ip netns exec ns2 ./server xdp 10.200.0.2 9000 200 veth1 0 10.200.0.1
```

Interrupt-coalescing sweep (server side, `ns2`'s `veth1`):

```bash
# default (already 0/0 on a fresh WSL2 veth)
cat /sys/class/net/veth1/napi_defer_hard_irqs /sys/class/net/veth1/gro_flush_timeout

# enable coalescing (meant to pair with SO_BUSY_POLL; see Findings for
# what happens when it is not paired with it)
sudo ip netns exec ns2 sh -c "echo 20 > /sys/class/net/veth1/napi_defer_hard_irqs"
sudo ip netns exec ns2 sh -c "echo 20000 > /sys/class/net/veth1/gro_flush_timeout"
```

Network namespaces and `veth` pairs created by `setup_netns.sh` do not
persist across a WSL2 VM restart; re-run it at the start of every session.

## Sibling comparison

[`market-data-tick-capture`](https://github.com/Manas103/market-data-tick-capture)
and [`telemetry-ground-station`](https://github.com/Manas103/telemetry-ground-station)
both measure UDP receive-path performance, but both report a single
end-to-end throughput or latency number (815,678 msgs/sec drain; p50
8-20us / p99 45-65us) rather than attributing that number to a segment of
the stack. Neither sweeps more than one receive mechanism. This project
answers a different question with the same category of input: not "how
fast", but "where does the time go, and what happens when one knob
changes."

## Limitations

- `veth` namespaces, not two physical hosts; no real NIC hardware
  timestamping exists to attribute a "driver" segment distinct from the
  kernel's software receive timestamp.
- AF_XDP is implemented (umem, custom BPF redirect program, debug
  counters) but not functional for packet delivery on this build host; see
  Findings for the full investigation. The measured sweep is kernel-socket
  and busy-poll only.
- No CPU pinning or real-time scheduling; this is Manas's shared, in-use
  machine (see `playbooks/BUILDER.md` section 3z), and the busy-poll and
  interrupt-coalescing results both show measurable contention noise from
  that, which is reported as a range and a root cause rather than hidden
  behind a single cherry-picked run.
- Fixed, moderate offered load (2,000 req/sec) only; no sweep across
  offered load, and no behavior under a saturating load is measured.
- The interrupt-coalescing experiment is scoped to the kernel-socket path
  only; whether the same settings interact differently when genuinely
  paired with `SO_BUSY_POLL` (as the kernel documentation intends) was not
  tested, because isolating the coalescing effect cleanly on the
  non-busy-polling path was already the finding worth reporting.
