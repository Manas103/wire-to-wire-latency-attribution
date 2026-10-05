#!/bin/bash
# Sets up a veth pair spanning two network namespaces for the latency
# harness: ns1 (client, 10.200.0.1) <-> veth0/veth1 <-> ns2 (server,
# 10.200.0.2). Idempotent: tears down any previous instance first.
set -e
sudo ip netns del ns1 2>/dev/null || true
sudo ip netns del ns2 2>/dev/null || true
sudo ip link del veth0 2>/dev/null || true
sudo ip netns add ns1
sudo ip netns add ns2
sudo ip link add veth0 type veth peer name veth1
sudo ip link set veth0 netns ns1
sudo ip link set veth1 netns ns2
sudo ip netns exec ns1 ip addr add 10.200.0.1/24 dev veth0
sudo ip netns exec ns2 ip addr add 10.200.0.2/24 dev veth1
sudo ip netns exec ns1 ip link set veth0 up
sudo ip netns exec ns2 ip link set veth1 up
sudo ip netns exec ns1 ip link set lo up
sudo ip netns exec ns2 ip link set lo up
sudo ip netns exec ns1 ping -c1 -W1 10.200.0.2 >/dev/null
echo "netns ready: ns1 10.200.0.1 (client) <-> ns2 10.200.0.2 (server, veth1)"
