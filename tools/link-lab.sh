#!/bin/bash
# Runs a FernSDR binary over five emulated links at once and reports what a
# real browser received on each: waterfall rows and audio messages a second,
# and the player's dropouts.
#
#   sudo tools/link-lab.sh BINARY [SECONDS] [OUTDIR]
#
# Unlike tools/link-proxy.py, which paces a relay in user space, the links here
# are the kernel's own TCP over netem, so loss, retransmission timers and slow
# start behave as they do on a real connection. Each link is a veth pair into a
# network namespace of its own, with a receiver bound to the host end and the
# browser inside the namespace; nothing else on the machine is shaped. Needs
# root, iproute2 with netem and plug (modprobe sch_netem sch_plug), Node and
# the Playwright browser the web checks use, and a built web/dist (or
# FERNSDR_WEB_ROOT).
#
# The two stalling links stop delivery for a second every twenty in the two
# ways a real link does: one loses everything sent meanwhile, the other holds
# it and then delivers it in order, as Wi-Fi retrying or a phone changing
# cells does. TCP sees them differently: after a hold, the acknowledgements
# come back with round trips as long as the hold.
#
# The first stall comes FERNSDR_LAB_FIRST_OUTAGE seconds after the links are
# shaped (5 by default, a few seconds into the session), because a budget that
# only copes with stalls once it has settled is what this lab once missed.
set -euo pipefail

BINARY=$(realpath "${1:?usage: link-lab.sh BINARY [SECONDS] [OUTDIR]}")
SECONDS_TO_RUN=${2:-240}
# Results go to a fresh temporary directory unless OUTDIR names one; run as
# root from the repository, a default beside it would leave a root-owned
# directory in the working tree.
OUT=$(realpath -m "${3:-$(mktemp -d /tmp/link-lab.XXXXXX)}")
REPO=$(cd "$(dirname "$0")/.." && pwd)
WEB_ROOT=${FERNSDR_WEB_ROOT:-$REPO/web/dist}
FIRST_OUTAGE=${FERNSDR_LAB_FIRST_OUTAGE:-5}

[ "$(id -u)" = 0 ] || { echo "link-lab.sh: needs root for namespaces and netem" >&2; exit 1; }
[ -x "$BINARY" ] || { echo "link-lab.sh: $BINARY is not an executable" >&2; exit 1; }
[ -f "$WEB_ROOT/index.html" ] || { echo "link-lab.sh: no index.html in $WEB_ROOT" >&2; exit 1; }
[[ "$SECONDS_TO_RUN" =~ ^[0-9]+$ ]] && [ "$SECONDS_TO_RUN" -gt 10 ] || { echo "link-lab.sh: SECONDS must be more than 10" >&2; exit 1; }
[[ "$FIRST_OUTAGE" =~ ^[0-9]+$ ]] || { echo "link-lab.sh: FERNSDR_LAB_FIRST_OUTAGE must be whole seconds" >&2; exit 1; }
LINKS="1 2 3 4 5"
for i in $LINKS; do
  if ip netns list | grep -qw "fernsdr-lab$i" || ip link show "fl${i}s" > /dev/null 2>&1; then
    echo "link-lab.sh: fernsdr-lab$i or fl${i}s exists; is another run going?" >&2; exit 1
  fi
done
# The links take 10.78.1.0/24 to 10.78.5.0/24. A connected route to a network
# the host already reaches some other way would take its traffic for the run.
for i in $LINKS; do
  if ip -4 route show match "10.78.$i.1" | grep -qv '^default'; then
    echo "link-lab.sh: the host already routes 10.78.$i.0/24; refusing to add a link there" >&2; exit 1
  fi
done

NAMES=("" "1% loss, 40 ms RTT" "2% loss, 120 ms RTT" "1 s outage every 20 s" "40 kbit/s" "1 s hold every 20 s")
mkdir -p "$OUT"
PIDS=()
CHECKS=()
cleanup() {
  for pid in "${PIDS[@]}" "${CHECKS[@]}"; do kill "$pid" 2>/dev/null || true; done
  # Deleting the host end takes the pair with it at once; a deleted namespace
  # releases its end only after the last process in it has gone, and a run
  # straight after this one would find the name still taken.
  for i in $LINKS; do
    ip link del "fl${i}s" 2>/dev/null || true
    ip netns del "fernsdr-lab$i" 2>/dev/null || true
  done
}
trap cleanup EXIT

for i in $LINKS; do
  ip netns add "fernsdr-lab$i"
  ip link add "fl${i}s" type veth peer name "fl${i}c"
  ip link set "fl${i}c" netns "fernsdr-lab$i"
  ip addr add "10.78.$i.1/24" dev "fl${i}s"
  ip link set "fl${i}s" up
  ip netns exec "fernsdr-lab$i" ip addr add "10.78.$i.2/24" dev "fl${i}c"
  ip netns exec "fernsdr-lab$i" ip link set "fl${i}c" up
  ip netns exec "fernsdr-lab$i" ip link set lo up
  mkdir -p "$OUT/$i"
  cat > "$OUT/$i/fernsdr.ini" <<INI
[site]
name = Link lab
max_users = 10

[server]
bind = 10.78.$i.1
port = 18190
root = $WEB_ROOT
log_level = info

[band:test]
name = Test 2.4 Msps
source = test
sample_rate = 2400000
center = 7100000
realtime = true
history = off
INI
  (cd "$OUT/$i" && exec "$BINARY" --allow-root fernsdr.ini > server.log 2>&1) &
  PIDS+=($!)
done

# Receiver to browser on the host end, the browser's acknowledgements inside
# the namespace: half the round trip each way.
shape() {
  local i=$1 one_way=$2 extra=$3
  # shellcheck disable=SC2086
  tc qdisc add dev "fl${i}s" root handle 1: netem delay "$one_way" $extra
  ip netns exec "fernsdr-lab$i" tc qdisc add dev "fl${i}c" root netem delay "$one_way"
}
shape 1 20ms "loss 1%"
shape 2 60ms "loss 2%"
shape 3 20ms ""
shape 4 20ms "rate 40kbit limit 30"
shape 5 20ms ""
# Behind the delay, a plug that lets everything through until it is told to
# hold, and then keeps what arrives until it is released.
tc qdisc add dev fl5s parent 1:1 handle 10: plug limit 10000000
tc qdisc change dev fl5s parent 1:1 handle 10: plug release_indefinite

(
  sleep "$FIRST_OUTAGE"
  while true; do
    tc qdisc change dev fl3s root netem delay 20ms loss 100%
    tc qdisc change dev fl5s parent 1:1 handle 10: plug block
    sleep 1
    tc qdisc change dev fl3s root netem delay 20ms
    tc qdisc change dev fl5s parent 1:1 handle 10: plug release_indefinite
    sleep 19
  done
) &
PIDS+=($!)
sleep 2

for i in $LINKS; do
  ip netns exec "fernsdr-lab$i" node "$REPO/web/tools/stream-rate-check.mjs" \
    "http://10.78.$i.1:18190/#band=test&f=7100000&m=usb" "$SECONDS_TO_RUN" "$OUT/$i.json" > "$OUT/$i.summary" &
  CHECKS+=($!)
done
failed=0
for pid in "${CHECKS[@]}"; do wait "$pid" || failed=1; done

printf '%-24s %10s %10s %9s %9s\n' link rows/s audio/s kbit/s dropouts
for i in $LINKS; do
  python3 - "$OUT/$i.summary" "${NAMES[$i]}" <<'PY' || printf '%-24s %s\n' "${NAMES[$i]}" "no summary; see $OUT/$i.summary"
import json, sys
summary = json.loads(open(sys.argv[1]).read().strip().splitlines()[-1])
print(f"{sys.argv[2]:24s} {summary['rowsPerSecond']:10.2f} {summary['audioPerSecond']:10.2f} "
      f"{summary['kbitPerSecond']:9.1f} {summary['dropouts']:9d}")
PY
done
echo "per-second series in $OUT"
exit "$failed"
