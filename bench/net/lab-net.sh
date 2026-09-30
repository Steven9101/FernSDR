#!/bin/bash
# SPDX-License-Identifier: AGPL-3.0-or-later
#
# The lab's network: one bridge, the receiver's namespace and one namespace
# per client on it, and no way from any of them to anything else.
#
#   bench/net/lab-net.sh up                  bridge and forwarding guard
#   bench/net/lab-net.sh receiver PID        put PID's network namespace on
#                                            the bridge as 198.18.0.2
#   bench/net/lab-net.sh client N [EXTRA]    namespace fbcN as 198.18.N.1,
#                                            plus EXTRA more addresses
#   bench/net/lab-net.sh check               prove the isolation
#   bench/net/lab-net.sh down                remove everything
#
# This machine is on the internet and forwards between its interfaces
# (docker, tailscale), so the lab is built to be unreachable in both
# directions:
#   - the bridge has no address on the host: nothing in the lab can reach a
#     host service, and the host has no route into the lab;
#   - IPv6 is off on the bridge and in every lab namespace, since a
#     link-local address alone would reach every host service;
#   - no namespace has a default route: a page asking for a CDN fails at
#     once instead of timing out;
#   - an nft table drops anything routed into or out of the bridge, in
#     case some other rule ever gives the host a route there.
# The lab uses 198.18.0.0/16, part of the range RFC 2544 reserves for
# benchmarks, so that receivers see ordinary internet listeners.
set -euo pipefail

BR=br-fb
RX_ADDR=198.18.0.2
PREFIX=16

bridge_up() {
  if ! ip link show "$BR" >/dev/null 2>&1; then
    ip link add "$BR" type bridge
  fi
  sysctl -qw "net.ipv6.conf.$BR.disable_ipv6=1"
  ip link set "$BR" up
  # Bridged frames between lab ports pass (with br_netfilter they appear
  # here with the bridge on both sides); anything routed across drops.
  nft -f - <<EOF
table inet fernbench
delete table inet fernbench
table inet fernbench {
  chain input {
    type filter hook input priority -10; policy accept;
    iifname "$BR" counter drop
  }
  chain output {
    type filter hook output priority -10; policy accept;
    oifname "$BR" counter drop
  }
  chain forward {
    type filter hook forward priority -10; policy accept;
    iifname "$BR" oifname != "$BR" counter drop
    oifname "$BR" iifname != "$BR" counter drop
  }
}
EOF
}

ns_quiet() {
  # IPv6 off and loopback up inside the namespace entered by "$@".
  "$@" sysctl -qw net.ipv6.conf.all.disable_ipv6=1
  "$@" sysctl -qw net.ipv6.conf.default.disable_ipv6=1
  "$@" ip link set lo up
  "$@" ip link set lo multicast on
  "$@" nft -f - <<'EOF'
table inet fernbench_namespace {
  chain output {
    type filter hook output priority -10; policy accept;
    ip daddr != { 127.0.0.0/8, 198.18.0.0/16, 224.0.0.0/4 } drop
    meta nfproto ipv6 drop
  }
}
EOF
}

receiver() {
  # Slot N (default 0) puts several receivers on the bridge at once, as
  # 198.18.0.(2+N) on veth fbrN.
  local pid=${1:?usage: lab-net.sh receiver PID [SLOT]} slot=${2:-0}
  local dev=fbr$slot addr=198.18.0.$((2 + slot))
  bridge_up
  ip link del "$dev" 2>/dev/null || true
  ip link add "$dev" type veth peer name "${dev}c"
  sysctl -qw "net.ipv6.conf.$dev.disable_ipv6=1"
  ip link set "${dev}c" netns "$pid"
  ip link set "$dev" master "$BR" up
  ns_quiet nsenter -t "$pid" -n
  nsenter -t "$pid" -n ip link set "${dev}c" name eth-lab
  nsenter -t "$pid" -n ip addr add "$addr/$PREFIX" dev eth-lab
  nsenter -t "$pid" -n ip link set eth-lab up
  # Receiver sidecars exchange multicast locally. Reply sockets that do
  # not set IP_MULTICAST_IF need an explicit route in this offline netns.
  nsenter -t "$pid" -n ip route add 239.0.0.0/8 dev lo
}

client() {
  local n=${1:?usage: lab-net.sh client N [EXTRA]} extra=${2:-0}
  [[ "$n" =~ ^[0-9]+$ ]] && [ "$n" -ge 1 ] && [ "$n" -le 250 ] || { echo "lab-net.sh: N is 1 to 250" >&2; exit 1; }
  bridge_up
  local ns=fbc$n
  ip netns del "$ns" 2>/dev/null || true
  ip link del "$ns" 2>/dev/null || true
  ip netns add "$ns"
  ip link add "$ns" type veth peer name "${ns}c"
  sysctl -qw "net.ipv6.conf.$ns.disable_ipv6=1"
  ip link set "${ns}c" netns "$ns"
  ip link set "$ns" master "$BR" up
  ns_quiet ip netns exec "$ns"
  ip netns exec "$ns" ip link set "${ns}c" name eth0
  ip netns exec "$ns" ip addr add "198.18.$n.1/$PREFIX" dev eth0
  local i
  for ((i = 2; i <= extra + 1; i++)); do
    ip netns exec "$ns" ip addr add "198.18.$n.$i/$PREFIX" dev eth0
  done
  ip netns exec "$ns" ip link set eth0 up
}

check() {
  local failed=0 ns target
  for ns in $(ip netns list | awk '/^fbc[0-9]+/ {print $1}'); do
    # Host addresses the lab must not reach, and one on the internet.
    for target in $(ip -4 -o addr show | awk '$2 != "lo" {split($4, a, "/"); print a[1]}') 1.1.1.1; do
      if ip netns exec "$ns" timeout 2 bash -c "exec 3<>/dev/tcp/$target/22" 2>/dev/null ||
         ip netns exec "$ns" timeout 2 bash -c "exec 3<>/dev/tcp/$target/443" 2>/dev/null; then
        echo "FAIL: $ns reaches $target"
        failed=1
      fi
    done
    if [ -n "$(ip netns exec "$ns" ip -6 addr show scope global scope link 2>/dev/null)" ]; then
      echo "FAIL: $ns has IPv6 addresses"
      failed=1
    fi
    if ip netns exec "$ns" ip route show default | grep -q .; then
      echo "FAIL: $ns has a default route"
      failed=1
    fi
    if ! ip netns exec "$ns" timeout 3 getent hosts example.com >/dev/null 2>&1; then :; else
      echo "FAIL: $ns resolves names"
      failed=1
    fi
  done
  if ip -4 addr show dev "$BR" 2>/dev/null | grep -q inet; then
    echo "FAIL: $BR has a host address"
    failed=1
  fi
  if ip -4 route get "$RX_ADDR" 2>/dev/null | grep -q "dev $BR"; then
    echo "FAIL: the host routes into the lab"
    failed=1
  fi
  [ "$failed" -eq 0 ] && echo "isolation ok"
  return "$failed"
}

down() {
  local ns
  for ns in $(ip netns list | awk '/^fbc[0-9]+/ {print $1}'); do
    ip link del "$ns" 2>/dev/null || true
    ip netns del "$ns" 2>/dev/null || true
  done
  for dev in $(ip -o link show | awk -F': ' '{print $2}' | grep -E '^fbr[0-9]+@' | cut -d@ -f1); do
    ip link del "$dev" 2>/dev/null || true
  done
  ip link del "$BR" 2>/dev/null || true
  nft delete table inet fernbench 2>/dev/null || true
}

case "${1:-}" in
up) bridge_up ;;
receiver) receiver "${2:-}" "${3:-0}" ;;
client) client "${2:-}" "${3:-0}" ;;
check) check ;;
down) down ;;
*) echo "usage: lab-net.sh up|receiver PID|client N [EXTRA]|check|down" >&2; exit 2 ;;
esac
