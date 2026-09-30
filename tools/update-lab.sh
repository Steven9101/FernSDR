#!/bin/sh
# The updater end to end, in a container that boots systemd: 0.1.0 installed
# the way install.sh lays it out, releases on a local HTTPS server signed
# with a key made for the run, and updates asked for the way the panel asks.
#
#   sudo tools/update-lab.sh [WORKDIR]
#   LAB_WEB=web/dist LAB_KEEP=1 sudo tools/update-lab.sh [WORKDIR]
#   LAB_INIT=sysv sudo tools/update-lab.sh [WORKDIR]
#
# With LAB_INIT=sysv the machine is Devuan with SysV init, and `fernsdr
# --supervise`, started by the release's init script, does what the units do.
#
# With LAB_WEB, the releases carry those pages instead of a stub, and with
# LAB_KEEP the run stops once 0.1.0 is installed and 0.1.1 published, and
# leaves the container up for the admin panel's Updates page: its address and
# password are printed, and `docker rm -f fernsdr-update-lab` ends it.
#
# Builds the receiver twice from this checkout (0.1.0 and 0.1.1, both
# trusting the run's key) in a copy under WORKDIR, and three releases
# that fail on purpose: one whose check refuses the configuration, one that
# crashes as it starts, one that never says it works. Then, in order:
#   1. 0.1.1 is installed and kept once it says it works, a minute later;
#   2. the release whose check refuses the configuration is not switched to;
#   3. the crashing one is rolled back within seconds;
#   4. the silent one is rolled back when its trial runs out;
#   5. the silent one again, with the container restarted during its trial:
#      the boot check puts the trusted version back before the receiver runs.
# Needs docker; changes nothing on the host but a container, an image and
# WORKDIR. The container runs privileged: under cgroup v2, Docker gives
# systemd the writable cgroup file system it needs to start only then. It
# runs this checkout's code and nothing else, and is removed at the end.
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
WORK=${1:-$(mktemp -d /tmp/fernsdr-update-lab.XXXXXX)}
mkdir -p "$WORK"
WORK=$(cd "$WORK" && pwd)
NAME=fernsdr-update-lab
IMAGE=fernsdr-update-lab
FAILURES=0

say() { printf '%s\n' "$*"; }
pass() { say "PASS  $*"; }
fail() { say "FAIL  $*"; FAILURES=$((FAILURES + 1)); }

in_lab() { docker exec "$NAME" "$@"; }

# --- keys, certificates, builds ---------------------------------------------

say "work directory: $WORK"
# Built in a copy, as everything here is: the checkout stays as it was.
mkdir -p "$WORK/tool"
rsync -a --delete --exclude 'build' --exclude 'build-*' --exclude 'dist' "$REPO/server/" "$WORK/tool/server/"
make -C "$WORK/tool/server" -s release-tools
TOOL=$WORK/tool/server/build/fernsdr-release
rm -f "$WORK/release.key"
PUB=$("$TOOL" keygen "$WORK/release.key")
KEY_BYTES=$(printf '%s' "$PUB" | sed 's/\(..\)/0x\1, /g; s/, $//')

mkdir -p "$WORK/share"
openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj "/CN=FernSDR update lab CA" \
    -keyout "$WORK/ca.key" -out "$WORK/share/ca.pem" 2>/dev/null
openssl req -newkey rsa:2048 -nodes -subj "/CN=127.0.0.1" -keyout "$WORK/share/server.key" \
    -out "$WORK/server.csr" 2>/dev/null
printf 'subjectAltName=IP:127.0.0.1\n' > "$WORK/san.ext"
openssl x509 -req -in "$WORK/server.csr" -CA "$WORK/share/ca.pem" -CAkey "$WORK/ca.key" -CAcreateserial \
    -days 2 -extfile "$WORK/san.ext" -out "$WORK/share/server.pem" 2>/dev/null
chmod 644 "$WORK/share/server.key"

# The checkout as it stands, committed or not, without its builds.
SRC=$WORK/src
mkdir -p "$SRC"
rsync -a --delete --exclude 'build' --exclude 'build-*' --exclude 'dist' "$REPO/server/" "$SRC/server/"
# The run's key is the one key these builds trust.
python3 - "$SRC/server/src/update/release_keys.h" "$KEY_BYTES" <<'EOF'
import sys, re
path, key = sys.argv[1], sys.argv[2]
text = open(path).read()
text = re.sub(r"static const std::vector<ReleaseKey> keys = \{.*?\};",
              "static const std::vector<ReleaseKey> keys = {ReleaseKey{" + key + "}};", text, flags=re.S)
open(path, "w").write(text)
EOF
build_version() {
    sed -i "s/kVersion = \"[^\"]*\"/kVersion = \"$1\"/" "$SRC/server/src/version.h"
    make -C "$SRC/server" -s -j"$(nproc)" release-binary ARCH=x86_64 > /dev/null
    cp "$SRC/server/build-release-x86_64/fernsdr" "$WORK/fernsdr-$1"
}
build_version 0.1.0
build_version 0.1.1

# A release tree: a program and its pages, the real ones with LAB_WEB.
tree() {
    rm -rf "$WORK/tree-$1"
    mkdir -p "$WORK/tree-$1"
    if [ -n "${LAB_WEB:-}" ]; then
        cp -R "$LAB_WEB" "$WORK/tree-$1/web"
    else
        mkdir -p "$WORK/tree-$1/web"
        printf '<!doctype html><title>FernSDR %s</title>\n' "$1" > "$WORK/tree-$1/web/index.html"
    fi
}
tree 0.1.0; cp "$WORK/fernsdr-0.1.0" "$WORK/tree-0.1.0/fernsdr"
tree 0.1.1; cp "$WORK/fernsdr-0.1.1" "$WORK/tree-0.1.1/fernsdr"
tree 0.1.2; printf '#!/bin/sh\necho "the configuration names a setting this version does not know" >&2\nexit 1\n' \
    > "$WORK/tree-0.1.2/fernsdr"
tree 0.1.3; printf '#!/bin/sh\n[ "$1" = --check ] && exit 0\necho crashing >&2\nexit 1\n' > "$WORK/tree-0.1.3/fernsdr"
tree 0.1.4; printf '#!/bin/sh\n[ "$1" = --check ] && exit 0\nexec sleep 100000\n' > "$WORK/tree-0.1.4/fernsdr"
chmod 755 "$WORK"/tree-*/fernsdr

# Each version as it would be published: archive, manifest, signature.
for version in 0.1.0 0.1.1 0.1.2 0.1.3 0.1.4; do
    out=$WORK/share/www-$version
    rm -rf "$out"; mkdir -p "$out"
    SOURCE_DATE_EPOCH=0 "$TOOL" pack "$out/fernsdr-$version-linux-x86_64.tar" "$WORK/tree-$version"
    printf 'Update lab release %s.\n\nWhat changed:\n- one thing\n- another <b>thing</b>\n' "$version" > "$WORK/notes"
    "$TOOL" manifest "$out/fernsdr-release-v1.txt" "$version" 2026-10-01 stable "$WORK/notes" \
        "$out/fernsdr-$version-linux-x86_64.tar"
    "$TOOL" sign --test-key "$WORK/release.key" "$out/fernsdr-release-v1.txt"
done
mkdir -p "$WORK/share/systemd" "$WORK/share/sysv"
cp "$REPO"/server/systemd/* "$WORK/share/systemd/"
cp "$REPO"/server/init/sysv/fernsdr "$WORK/share/sysv/"
INIT=${LAB_INIT:-systemd}

# --- the machine --------------------------------------------------------------

mkdir -p "$WORK/image"
if [ "$INIT" = sysv ]; then
    cat > "$WORK/image/Dockerfile" <<'EOF'
FROM devuan/devuan:daedalus
RUN apt-get update && apt-get install -y --no-install-recommends sysvinit-core curl ca-certificates \
        openssl procps && apt-get clean && rm -rf /var/lib/apt/lists/* && sed -i '/getty/d' /etc/inittab
CMD ["/sbin/init"]
EOF
else
    cat > "$WORK/image/Dockerfile" <<'EOF'
FROM debian:12
RUN apt-get update && apt-get install -y --no-install-recommends systemd systemd-sysv curl ca-certificates \
        openssl procps && apt-get clean && rm -rf /var/lib/apt/lists/*
STOPSIGNAL SIGRTMIN+3
CMD ["/sbin/init"]
EOF
fi
docker build -q -t "$IMAGE" "$WORK/image" > /dev/null

docker rm -f "$NAME" > /dev/null 2>&1 || true
PASSWORD="update lab $(date +%s)"
HASH=$(printf '%s\n' "$PASSWORD" | "$WORK/fernsdr-0.1.0" --hash-password 2>/dev/null | sed -n 's/^password_hash *= *//p')
printf '%s\n' "$HASH" > "$WORK/share/admin-hash"
docker run -d --name "$NAME" --privileged --cgroupns=private -p 127.0.0.1:18199:8073 \
    --tmpfs /run --tmpfs /run/lock -v "$WORK/share:/lab:ro" "$IMAGE" > /dev/null
# Until the init has started what it starts at boot.
booted() {
    if [ "$INIT" = sysv ]; then
        sleep 5
        state=booted
        return
    fi
    for i in $(seq 1 60); do
        state=$(in_lab systemctl is-system-running 2>/dev/null || true)
        case "$state" in running|degraded) break ;; esac
        sleep 1
    done
}
booted
say "$INIT in the container: $state"

in_lab sh -eu -c '
useradd --system --home-dir /var/lib/fernsdr --shell /usr/sbin/nologin fernsdr
mkdir -p /opt/fernsdr/releases/0.1.0 /var/lib/fernsdr /var/lib/fernsdr-update /srv/releases
tar -xf /lab/www-0.1.0/fernsdr-0.1.0-linux-x86_64.tar -C /opt/fernsdr/releases/0.1.0
ln -s releases/0.1.0 /opt/fernsdr/current
ln -s releases/0.1.0 /opt/fernsdr/trusted
cat > /var/lib/fernsdr/fernsdr.conf <<CONF
[site]
name = Update lab
[server]
bind = 0.0.0.0
port = 8073
[admin]
password_hash = $(cat /lab/admin-hash)
home_network = yes
[band:demo]
source = test
sample_rate = 192k
center = 7.1M
realtime = true
CONF
chown -R fernsdr:fernsdr /var/lib/fernsdr
chmod 700 /var/lib/fernsdr
chmod 600 /var/lib/fernsdr/fernsdr.conf
if [ "'"$INIT"'" = sysv ]; then
    # The supervisor hands the updater its two variables and nothing else,
    # so the lab authority goes where every program on the machine looks.
    cp /lab/ca.pem /usr/local/share/ca-certificates/fernsdr-lab.crt && update-ca-certificates > /dev/null 2>&1
    printf "FERNSDR_UPDATE_URL=https://127.0.0.1:8443/\nFERNSDR_UPDATE_TRIAL_SECONDS=90\n" > /etc/default/fernsdr
    install -m 0755 /lab/sysv/fernsdr /etc/init.d/fernsdr
    update-rc.d fernsdr defaults > /dev/null 2>&1
    /etc/init.d/fernsdr start > /dev/null
else
    cp /lab/systemd/*.service /lab/systemd/*.path /etc/systemd/system/
    for unit in fernsdr.service fernsdr-update.service fernsdr-update-boot.service; do
        mkdir -p /etc/systemd/system/$unit.d
        printf "[Service]\nEnvironment=SSL_CERT_FILE=/lab/ca.pem FERNSDR_UPDATE_URL=https://127.0.0.1:8443/ FERNSDR_UPDATE_TRIAL_SECONDS=90\n" \
            > /etc/systemd/system/$unit.d/lab.conf
    done
    systemctl daemon-reload
    systemctl enable --now fernsdr-update-boot.service fernsdr.service fernsdr-update.path > /dev/null 2>&1
fi
'
release_server() {
    if [ "$INIT" = sysv ]; then
        docker exec -d -w /srv/releases "$NAME" openssl s_server -quiet -accept 8443 -cert /lab/server.pem \
            -key /lab/server.key -WWW
    else
        in_lab systemd-run --quiet --unit=release-server -p WorkingDirectory=/srv/releases \
            openssl s_server -quiet -accept 8443 -cert /lab/server.pem -key /lab/server.key -WWW
    fi
}
release_server

publish() {
    in_lab sh -c "rm -f /srv/releases/* && cp /lab/www-$1/* /srv/releases/"
}
request() {
    in_lab runuser -u fernsdr -- sh -c "printf 'demo\n' > /var/lib/fernsdr/update-bands && printf '$1\n' > /var/lib/fernsdr/update-request"
}
status() { in_lab cat /var/lib/fernsdr-update/status.json 2>/dev/null || true; }
state_of() { status | sed -n 's/.*"state":"\([^"]*\)".*/\1/p'; }
link() { in_lab readlink "/opt/fernsdr/$1"; }
# Waits for the updater to finish: a state that is not a step on the way.
wait_done() {
    for i in $(seq 1 "$1"); do
        case "$(state_of)" in updated|rolled-back|refused|failed) return 0 ;; esac
        sleep 1
    done
    return 1
}
running_version() { in_lab /opt/fernsdr/current/fernsdr --version 2>/dev/null | sed 's/FernSDR //'; }

for i in $(seq 1 30); do
    in_lab curl -s -o /dev/null http://127.0.0.1:8073/api/status && break
    sleep 1
done
[ "$(running_version)" = 0.1.0 ] && pass "0.1.0 installed and serving" || fail "0.1.0 is not serving"

if [ -n "${LAB_KEEP:-}" ]; then
    publish 0.1.1
    say "0.1.1 is published. The admin panel: http://127.0.0.1:18199/admin#/updates"
    say "password: $PASSWORD"
    exit 0
fi

# 1. An update that works.
publish 0.1.1
started=$(date +%s)
request 0.1.1
if wait_done 150 && [ "$(state_of)" = updated ]; then
    pass "0.1.1 kept after $(( $(date +%s) - started )) s: $(status)"
else
    fail "0.1.1 was not kept: $(status)"
fi
[ "$(link current)" = releases/0.1.1 ] && [ "$(link trusted)" = releases/0.1.1 ] &&
    in_lab curl -s -o /dev/null http://127.0.0.1:8073/api/status &&
    pass "current and trusted are 0.1.1, and it serves" || fail "after 1: current $(link current), trusted $(link trusted)"

# 2. A release whose check refuses the configuration.
in_lab sh -c 'rm -f /var/lib/fernsdr-update/status.json'
publish 0.1.2
request 0.1.2
if wait_done 60 && [ "$(state_of)" = refused ] && [ "$(link current)" = releases/0.1.1 ] &&
    ! in_lab test -e /opt/fernsdr/releases/0.1.2; then
    pass "0.1.2 refused before switching: $(status)"
else
    fail "0.1.2: $(status), current $(link current)"
fi

# 3. A release that crashes as it starts.
in_lab sh -c 'rm -f /var/lib/fernsdr-update/status.json'
publish 0.1.3
started=$(date +%s)
request 0.1.3
if wait_done 60 && [ "$(state_of)" = rolled-back ] && [ "$(link current)" = releases/0.1.1 ]; then
    pass "0.1.3 rolled back after $(( $(date +%s) - started )) s: $(status)"
else
    fail "0.1.3: $(status), current $(link current)"
fi

# 4. A release that never says it works.
in_lab sh -c 'rm -f /var/lib/fernsdr-update/status.json'
publish 0.1.4
started=$(date +%s)
request 0.1.4
if wait_done 150 && [ "$(state_of)" = rolled-back ] && [ "$(link current)" = releases/0.1.1 ]; then
    pass "0.1.4 rolled back after $(( $(date +%s) - started )) s: $(status)"
else
    fail "0.1.4: $(status), current $(link current)"
fi
in_lab curl -s -o /dev/null http://127.0.0.1:8073/api/status && pass "0.1.1 serves again" || fail "0.1.1 does not serve"

# 5. The machine restarts during a trial.
in_lab sh -c 'rm -f /var/lib/fernsdr-update/status.json'
request 0.1.4
for i in $(seq 1 60); do [ "$(state_of)" = trial ] && break; sleep 1; done
[ "$(link current)" = releases/0.1.4 ] && pass "0.1.4 on trial when the machine goes down" ||
    fail "0.1.4 never went on trial: $(status)"
docker restart "$NAME" > /dev/null
booted
if [ "$INIT" = sysv ]; then release_server; else in_lab systemctl start release-server.service > /dev/null 2>&1 || true; fi
for i in $(seq 1 30); do in_lab curl -s -o /dev/null http://127.0.0.1:8073/api/status && break; sleep 1; done
if [ "$(link current)" = releases/0.1.1 ] && [ "$(state_of)" = rolled-back ] &&
    [ "$(running_version)" = 0.1.1 ] && ! in_lab test -e /var/lib/fernsdr-update/trial; then
    pass "after the restart 0.1.1 is back: $(status)"
else
    fail "after the restart: current $(link current), $(status)"
fi

# Refusals and rollbacks are the updater doing its job: no unit is left failed.
if [ "$INIT" = sysv ]; then
    in_lab /etc/init.d/fernsdr status > /dev/null && pass "the supervisor runs" || fail "the supervisor does not run"
else
    failed_units=$(in_lab systemctl --failed --no-legend --plain 2>/dev/null | grep -o 'fernsdr[^ ]*' || true)
    [ -z "$failed_units" ] && pass "no FernSDR unit is left failed" || fail "failed units: $failed_units"
fi

say ""
if [ "$INIT" = sysv ]; then
    in_lab cat /var/log/fernsdr/fernsdr.log
else
    in_lab journalctl -u fernsdr-update.service -u fernsdr-update-boot.service --no-pager -o cat 2>/dev/null
fi | grep -E "Updated|Went back|refused|interrupted|refuses" | sed 's/^/  updater: /' || true
docker rm -f "$NAME" > /dev/null
if [ "$FAILURES" -eq 0 ]; then say "all passed"; else say "$FAILURES failed"; exit 1; fi
