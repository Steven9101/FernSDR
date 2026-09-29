#!/bin/sh
# Checks that the receiver keeps up with a direct-sampling module such as
# Fern-RX888: 16-bit real samples at a high rate through the module pipe, a
# band that starts at 0 Hz.
#
# It installs server/tools/wideband_module.cpp as a module in a scratch
# directory, runs a receiver with one real band at each rate given, and for
# each one reports whether the module had to drop samples because the
# receiver did not read them in time, how much CPU the receiver used, and
# whether a listener tuned to 7.099 MHz USB hears the module's 7.1 MHz
# carrier as a 1 kHz tone, which proves the frequency axis from 0 Hz.
#
#   tools/wideband-check.sh [RATE ...]        default: 64800000 129600000
#
# Needs python3 and a built web/ is not required. Exits non-zero when a rate
# drops samples or the tone is not where it belongs.
set -u
here=$(cd "$(dirname "$0")/.." && pwd)
rates=${*:-"64800000 129600000"}
seconds=${WIDEBAND_SECONDS:-30}
[ "$seconds" -ge 10 ] || seconds=10
port=${WIDEBAND_PORT:-18231}
tmp=$(mktemp -d)
pid=
cleanup() {
    [ -n "$pid" ] && kill "$pid" 2>/dev/null && wait "$pid" 2>/dev/null
    rm -rf "$tmp"
}
trap cleanup EXIT INT TERM

make -s -C "$here/server" all wideband-module || exit 1
fernsdr=$here/server/build/fernsdr
module=$here/server/build/wideband-module
arch=$(uname -m | sed -e 's/^armv7.*/armhf/' -e 's/^arm64$/aarch64/')

# The package FernSDR installs: a manifest and the executable (docs/MODULES.md).
python3 - "$module" "linux-$arch" "$tmp/wideband-0.1.0-linux-$arch.fernmod" <<'EOF' || exit 1
import hashlib, json, subprocess, sys
exe_path, platform, out = sys.argv[1:]
exe = open(exe_path, "rb").read()
describe = json.loads(subprocess.run([exe_path, "--describe"], stdout=subprocess.PIPE, check=True).stdout)
manifest = {"schema": 1, "id": "wideband", "name": "Wideband test", "version": "0.1.0", "kind": "input", "api": 1,
            "platform": platform, "size": len(exe), "sha256": hashlib.sha256(exe).hexdigest(),
            "license": "AGPL-3.0-or-later", "source": "https://github.com/Steven9101/FernSDR",
            "description": "test module", "settings": describe["settings"]}
text = json.dumps(manifest).encode()
open(out, "wb").write(b"FERNMOD1\n" + str(len(text)).encode() + b"\n" + text + exe)
EOF

failures=0
for rate in $rates; do
    conf=$tmp/fernsdr-$rate.conf
    mkdir -p "$tmp/www"
    cat >"$conf" <<EOF
[site]
name = Wideband check
[server]
bind = 127.0.0.1
port = $port
root = $tmp/www
log_level = info
[modules]
directory = $tmp/modules
[band:hf]
name = HF
source = module
module = wideband
sample_rate = $rate
signal = real
center = 0
module.tone = 7100000
EOF
    "$fernsdr" --install-module "$tmp/wideband-0.1.0-linux-$arch.fernmod" "$conf" >/dev/null 2>&1 ||
        "$fernsdr" --allow-root --install-module "$tmp/wideband-0.1.0-linux-$arch.fernmod" "$conf" >/dev/null ||
        { echo "installing the test module failed" >&2; exit 1; }
    "$fernsdr" "$conf" --allow-root >"$tmp/log-$rate" 2>&1 &
    pid=$!
    sleep 8
    ticks() { awk '{print $14 + $15}' "/proc/$pid/stat"; }
    # A listener stays tuned the whole time, so that the CPU is a band with
    # someone on it, not an idle one.
    python3 "$here/tools/fernsdr-probe.py" --host 127.0.0.1 --port "$port" --freq 7099000 --mode usb \
        --seconds "$seconds" --quiet >"$tmp/probe-$rate" 2>&1 &
    listener=$!
    sleep 2
    t0=$(ticks)
    sleep $((seconds - 4))
    t1=$(ticks)
    wait "$listener"
    probe=$(cat "$tmp/probe-$rate")
    hz=$(getconf CLK_TCK)
    cpu=$(awk -v a="$t0" -v b="$t1" -v hz="$hz" -v s="$((seconds - 4))" 'BEGIN { printf "%.0f", (b - a) / hz / s * 100 }')
    kill "$pid"
    wait "$pid" 2>/dev/null
    pid=
    tone=$(printf '%s\n' "$probe" | sed -n 's/^strongest audio tone near \([0-9]*\) Hz.*/\1/p')
    dropped=$(grep -o 'dropped [0-9]* samples' "$tmp/log-$rate" | tail -1 | awk '{print $2}')
    status=ok
    if [ -n "$dropped" ]; then status=FAIL; fi
    if [ -z "$tone" ] || [ "$tone" -lt 990 ] || [ "$tone" -gt 1010 ]; then status=FAIL; fi
    printf '%s  %s samples/s: CPU %s %% of a core, dropped %s, tone at 7.099 MHz USB %s Hz\n' \
        "$status" "$rate" "$cpu" "${dropped:-0}" "${tone:-none}"
    if [ "$status" != ok ]; then
        failures=$((failures + 1))
        grep -i "module\|error\|band" "$tmp/log-$rate" | tail -15 >&2
    fi
done
exit "$failures"
