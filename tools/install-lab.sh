#!/bin/sh
# install.sh end to end, on the distributions it is written for. Each boots
# its init in a container (systemd, or OpenRC, runit or SysV init running
# `fernsdr --supervise`) and fetches install.sh and the releases from a
# local HTTPS server, the way an operator's machine fetches them from
# GitHub. The releases are this checkout's, built as 0.1.0 and 0.1.1 and
# signed with a key made for the run, which the builds and the installer
# carry in place of the real ones.
#
#   sudo tools/install-lab.sh [WORKDIR]
#   LAB_DISTROS="debian:12 fedora:latest" LAB_JOBS=2 sudo tools/install-lab.sh [WORKDIR]
#
# On each distribution:
#   1. with no terminal and no FERNSDR_SETUP, install.sh stops before it
#      changes anything;
#   2. `curl ... | sh` with FERNSDR_SETUP=home installs 0.1.0: the receiver
#      runs as its own user, answers, has the password install.sh printed,
#      and lets the home network into the admin panel over plain HTTP;
#   3. run again with 0.1.0 the newest, it leaves the receiver running;
#   4. run again with 0.1.1 published, it updates through the updater.
# On debian:12, or the distribution LAB_EXTRAS names, besides:
#   5. a manifest changed after it was signed is refused, with nothing
#      installed;
#   6. an internet install with a domain listens on this machine only, and
#      what the receiver's user leaves in place of its configuration, a link
#      to a file of root's or a FIFO, gets it nothing from a second run;
#   7. a receiver that tools/source-install.sh --service set up moves over
#      with its files;
#   8. one whose configuration the release refuses is left as it was, and
#      one whose move fails halfway runs again as before;
#   9. install.sh as the source has it refuses to run.
# Needs docker and python3. Changes nothing on the host but containers,
# images and a network, all named fernsdr-install-lab, and WORKDIR. The
# containers run privileged, which systemd in them needs under cgroup v2,
# run this checkout's code and nothing else, and are removed at the end
# unless LAB_KEEP is set.
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
WORK=${1:-$(mktemp -d /tmp/fernsdr-install-lab.XXXXXX)}
mkdir -p "$WORK"
WORK=$(cd "$WORK" && pwd)
# alpine (OpenRC and BusyBox's mdev), void (runit) and devuan (SysV init)
# boot their own init and run the first four cases with `fernsdr
# --supervise`, and two of its own; the rest boot systemd.
DISTROS=${LAB_DISTROS:-"debian:12 debian:10 debian:11 debian:13 ubuntu:18.04 ubuntu:20.04 ubuntu:22.04 ubuntu:24.04 ubuntu:26.04 fedora:latest rockylinux/rockylinux:9 almalinux:8 amazonlinux:2023 opensuse/leap:15.6 opensuse/tumbleweed archlinux:latest alpine:3.22 ghcr.io/void-linux/void-glibc:latest devuan/devuan:daedalus"}
# Where the cases beyond the first four run.
EXTRAS=${LAB_EXTRAS:-debian:12}
JOBS=${LAB_JOBS:-3}
PREFIX=fernsdr-install-lab
NETWORK=$PREFIX
SERVER=$PREFIX-releases
BASE=https://releases:8443

say() { printf '%s\n' "$*"; }

# --- key, certificates, releases ----------------------------------------------

say "work directory: $WORK"
TOOL=$REPO/server/build/fernsdr-release
make -C "$REPO/server" -s release-tools
rm -f "$WORK/release.key"
PUB=$("$TOOL" keygen "$WORK/release.key")
KEY_BYTES=$(printf '%s' "$PUB" | sed 's/\(..\)/0x\1, /g; s/, $//')

mkdir -p "$WORK/tls" "$WORK/share"
openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj "/CN=FernSDR install lab CA" \
    -keyout "$WORK/tls/ca.key" -out "$WORK/share/ca.pem" 2>/dev/null
openssl req -newkey rsa:2048 -nodes -subj "/CN=releases" -keyout "$WORK/tls/server.key" \
    -out "$WORK/tls/server.csr" 2>/dev/null
printf 'subjectAltName=DNS:releases\n' > "$WORK/tls/san.ext"
openssl x509 -req -in "$WORK/tls/server.csr" -CA "$WORK/share/ca.pem" -CAkey "$WORK/tls/ca.key" \
    -CAcreateserial -days 2 -extfile "$WORK/tls/san.ext" -out "$WORK/tls/server.pem" 2>/dev/null
chmod 644 "$WORK/tls/server.key"

# The checkout as it stands, committed or not, without its builds; the run's
# key is the one key these builds trust.
SRC=$WORK/src
mkdir -p "$SRC"
rsync -a --delete --exclude 'build' --exclude 'build-*' --exclude 'dist' "$REPO/server/" "$SRC/server/"
rsync -a "$REPO/tools/" "$SRC/tools/"
cp "$REPO/LICENSE" "$SRC/LICENSE"
python3 - "$SRC/server/src/update/release_keys.h" "$KEY_BYTES" <<'EOF'
import sys, re
path, key = sys.argv[1], sys.argv[2]
text = open(path).read()
text = re.sub(r"static const std::vector<ReleaseKey> keys = \{.*?\};",
              "static const std::vector<ReleaseKey> keys = {ReleaseKey{" + key + "}};", text, flags=re.S)
open(path, "w").write(text)
EOF
mkdir -p "$WORK/web"
printf '<!doctype html><title>FernSDR install lab</title>\n' > "$WORK/web/index.html"
for version in 0.1.0 0.1.1; do
    sed -i "s/kVersion = \"[^\"]*\"/kVersion = \"$version\"/" "$SRC/server/src/version.h"
    make -C "$SRC/server" -s -j"$(nproc)" release ARCH=x86_64 WEB_DIST="$WORK/web" > /dev/null
done
make -C "$SRC/server" -s installer

for version in 0.1.0 0.1.1; do
    out=$WORK/www/v$(printf '%s' "$version" | tr -d .)
    rm -rf "${out:?}"
    mkdir -p "$out"
    cp "$SRC/server/dist/fernsdr-$version-linux-x86_64.tar" "$SRC/server/dist/install.sh" "$out/"
    printf 'Install lab release %s.\n\nWhat changed:\n- one thing\n- another\n' "$version" > "$WORK/notes"
    "$TOOL" manifest "$out/fernsdr-release-v1.txt" "$version" 2026-10-01 stable "$WORK/notes" \
        "$out/fernsdr-$version-linux-x86_64.tar"
    # The run's tool knows only the real keys, which the run's key is not.
    "$TOOL" sign --test-key "$WORK/release.key" "$out/fernsdr-release-v1.txt"
done
# 0.1.0 with its notes changed after it was signed.
rm -rf "${WORK:?}/www/tampered"
cp -R "$WORK/www/v010" "$WORK/www/tampered"
sed -i 's/^note - one thing$/note - one thing, and a download from somewhere else/' "$WORK/www/tampered/fernsdr-release-v1.txt"
mkdir -p "$WORK/www/source"
cp "$REPO/tools/install.sh" "$WORK/www/source/install.sh"

# What tools/source-install.sh --service leaves behind: the program in
# bin, the pages in share and the receiver's directory in etc, with the unit
# it wrote.
OLD=$WORK/share/old-layout
rm -rf "${OLD:?}"
mkdir -p "$OLD/bin" "$OLD/share/fernsdr" "$OLD/etc/history"
tar -xf "$WORK/www/v010/fernsdr-0.1.0-linux-x86_64.tar" -C "$WORK/share" fernsdr fernsdr.example.conf
mv "$WORK/share/fernsdr" "$OLD/bin/fernsdr"
cp -R "$WORK/web" "$OLD/share/fernsdr/web"
{
    sed 's/^name *= Example WebSDR/name = Old layout/' "$WORK/share/fernsdr.example.conf"
    printf '\n[modules]\ndirectory = /opt/fernsdr/etc/fernsdr-modules\n'
} > "$OLD/etc/fernsdr.conf"
printf 'kept by the operator\n' > "$OLD/etc/notes.txt"
printf 'waterfall\n' > "$OLD/etc/history/20m.bin"
cat > "$OLD/fernsdr.service" <<'UNIT'
[Unit]
Description=FernSDR WebSDR
After=network-online.target
Wants=network-online.target

[Service]
Type=exec
User=fernsdr
Group=fernsdr
ExecStart="/opt/fernsdr/bin/fernsdr" "/opt/fernsdr/etc/fernsdr.conf" --root "/opt/fernsdr/share/fernsdr/web"
ExecStartPre="/opt/fernsdr/bin/fernsdr" "/opt/fernsdr/etc/fernsdr.conf" --root "/opt/fernsdr/share/fernsdr/web" --check
WorkingDirectory=/opt/fernsdr/etc
Restart=always
RestartSec=2
NoNewPrivileges=yes
PrivateTmp=yes
PrivateDevices=yes
ProtectSystem=strict
ProtectHome=yes
ReadWritePaths="/opt/fernsdr/etc"

[Install]
WantedBy=multi-user.target
UNIT

# --- images, network, release server --------------------------------------------

slug() { printf '%s' "${1##*/}" | tr -c 'a-z0-9' '-'; }

# DISTRO: the init it boots.
init_of() {
    case "$1" in
        alpine:*) printf openrc ;;
        */void-linux/*) printf runit ;;
        devuan/*) printf sysv ;;
        *) printf systemd ;;
    esac
}

# DISTRO: an image of it that boots systemd, with the tools install.sh
# expects of such a system and the updater's curl or wget. Debian 11 gets
# wget and no curl, and Debian 10 and 11 and Ubuntu 20.04 have an OpenSSL
# older than 3: those paths run too.
image() {
    dir=$WORK/image-$(slug "$1")
    mkdir -p "$dir"
    init=/sbin/init
    case "$1" in
        debian:10)
            # Buster lives in the archive now.
            install="sed -i 's|deb.debian.org|archive.debian.org|g; s|security.debian.org|archive.debian.org|g; /-updates/d' /etc/apt/sources.list \\
    && apt-get -o Acquire::Check-Valid-Until=false update \\
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends systemd systemd-sysv curl ca-certificates openssl procps iproute2 && apt-get clean && rm -rf /var/lib/apt/lists/*"
            ;;
        debian:11)
            # Wget and no curl. Bullseye's security updates are on their way to
            # the archive, with the index still naming files already gone, so
            # its packages come from the snapshot the image names.
            install="sed -i 's|^deb http://deb.debian.org|# deb http://deb.debian.org|; s|^# deb http://snapshot|deb http://snapshot|' /etc/apt/sources.list \\
    && apt-get -o Acquire::Check-Valid-Until=false update \\
    && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends systemd systemd-sysv wget ca-certificates openssl procps iproute2 && apt-get clean && rm -rf /var/lib/apt/lists/*"
            ;;
        debian:* | ubuntu:*)
            install="apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends systemd systemd-sysv curl ca-certificates openssl procps iproute2 && apt-get clean && rm -rf /var/lib/apt/lists/*"
            ;;
        fedora:* | */rockylinux:* | almalinux:* | amazonlinux:*)
            install="dnf install -y --setopt=install_weak_deps=False systemd openssl procps-ng iproute util-linux shadow-utils tar && { command -v curl || dnf install -y curl; } && dnf clean all"
            ;;
        alpine:*)
            # OpenRC in a container, and mdev's rules rather than udev's.
            install="apk add --no-cache openrc curl ca-certificates openssl busybox-mdev-openrc mdev-conf \\
    && sed -i 's/^#rc_sys=\"\"/rc_sys=\"docker\"/' /etc/rc.conf && sed -i '/getty/d' /etc/inittab"
            ;;
        */void-linux/*)
            install="xbps-install -Syu xbps && xbps-install -Syu && xbps-install -y runit-void curl ca-certificates openssl tar util-linux shadow procps-ng iproute2 \\
    && rm -f /etc/runit/runsvdir/default/agetty-*"
            init=/sbin/runit-init
            ;;
        devuan/*)
            install="apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends sysvinit-core curl ca-certificates openssl procps iproute2 && apt-get clean && rm -rf /var/lib/apt/lists/* \\
    && sed -i '/getty/d' /etc/inittab"
            ;;
        opensuse/*)
            install="zypper --non-interactive install --no-recommends systemd gawk curl openssl ca-certificates procps iproute2 util-linux shadow tar gzip && zypper clean --all"
            init=/usr/lib/systemd/systemd
            ;;
        archlinux:*)
            # systemd-resolved, which Arch starts, answers before the DNS
            # Docker gives the container and knows no other container's name;
            # networkd, which it starts too, never calls Docker's network
            # online, so everything after network-online.target waits two
            # minutes for it.
            install="pacman -Syu --noconfirm --needed systemd curl openssl procps-ng iproute2 util-linux shadow tar && pacman -Scc --noconfirm \\
    && systemctl mask systemd-resolved.service systemd-networkd-wait-online.service systemd-firstboot.service"
            init=/usr/lib/systemd/systemd
            ;;
        *) return 1 ;;
    esac
    cat > "$dir/Dockerfile" <<EOF
FROM $1
ENV container=docker
RUN $install
$([ "$(init_of "$1")" = systemd ] && printf 'STOPSIGNAL SIGRTMIN+3')
CMD ["$init"]
EOF
    docker build -q -t "$PREFIX-$(slug "$1")" "$dir" > "$dir/build.log" 2>&1
}

say "building images"
READY=""
for distro in $DISTROS; do
    if image "$distro"; then
        READY="$READY $distro"
    else
        say "FAIL  $distro: no image; $WORK/image-$(slug "$distro")/build.log says why"
        printf 'FAIL  %s: no image\n' "$distro" > "$WORK/result-$(slug "$distro").txt"
    fi
done
image debian:12 || { say "no debian:12 image for the release server"; exit 1; }

docker network rm "$NETWORK" > /dev/null 2>&1 || true
docker network create "$NETWORK" > /dev/null
docker rm -f "$SERVER" > /dev/null 2>&1 || true
docker run -d --name "$SERVER" --network "$NETWORK" --network-alias releases -w /srv/releases \
    -v "$WORK/www:/srv/releases:ro" -v "$WORK/tls:/tls:ro" "$PREFIX-debian-12" \
    openssl s_server -quiet -accept 8443 -cert /tls/server.pem -key /tls/server.key -WWW > /dev/null

# --- one machine ------------------------------------------------------------

# NAME DIRECTORY ENVIRONMENT...: the README's one-liner on NAME, with the
# lab's address, curl or wget, whichever the machine has. The script is
# fetched first, so that a download that fails cannot pass for an installer
# that ran and did nothing, and then piped into sh all the same. A run that
# hangs fails after a quarter of an hour.
one_liner() {
    target=$1
    directory=$2
    shift 2
    timeout 900 docker exec "$target" sh -c "
        if command -v curl > /dev/null; then curl -fsSL $BASE/$directory/install.sh; else wget -qO- $BASE/$directory/install.sh; fi \
            > /tmp/install.sh || { echo 'the download of install.sh failed'; exit 97; }
        cat /tmp/install.sh | env FERNSDR_RELEASE_URL=$BASE/$directory $* sh" 2>&1
}

# NAME DISTRO: a container of DISTRO booted, trusting the lab's certificate
# authority, with the updater pointed at the lab's 0.1.1.
boot() {
    docker rm -f "$1" > /dev/null 2>&1 || true
    docker run -d --name "$1" --hostname "$1" --network "$NETWORK" --privileged --cgroupns=private \
        --tmpfs /run --tmpfs /run/lock -v "$WORK/share:/lab:ro" "$PREFIX-$(slug "$2")" > /dev/null
    if [ "$(init_of "$2")" = systemd ]; then
        for i in $(seq 1 90); do
            state=$(docker exec "$1" systemctl is-system-running 2>/dev/null || true)
            case "$state" in running | degraded) break ;; esac
            sleep 1
        done
    else
        sleep 5
    fi
    docker exec "$1" sh -eu -c '
        if [ -d /etc/pki/trust/anchors ]; then
            cp /lab/ca.pem /etc/pki/trust/anchors/fernsdr-lab.pem && update-ca-certificates
        elif command -v update-ca-certificates > /dev/null; then
            mkdir -p /usr/local/share/ca-certificates
            cp /lab/ca.pem /usr/local/share/ca-certificates/fernsdr-lab.crt && update-ca-certificates
        elif [ -d /etc/pki/ca-trust/source/anchors ]; then
            cp /lab/ca.pem /etc/pki/ca-trust/source/anchors/fernsdr-lab.pem && update-ca-trust extract
        else
            cp /lab/ca.pem /etc/ca-certificates/trust-source/anchors/fernsdr-lab.pem && update-ca-trust extract
        fi
        # Void'"'"'s update-ca-certificates leaves local certificates out of the
        # bundle OpenSSL reads by default.
        bundle=/etc/ssl/certs/ca-certificates.crt
        if [ -f "$bundle" ] && ! grep -qF "$(sed -n 2p /lab/ca.pem)" "$bundle"; then cat /lab/ca.pem >> "$bundle"; fi
        mkdir -p /etc/systemd/system/fernsdr-update.service.d /etc/conf.d /etc/default
        printf "[Service]\nEnvironment=FERNSDR_UPDATE_URL=https://releases:8443/v011/\n" \
            > /etc/systemd/system/fernsdr-update.service.d/lab.conf
        printf "export FERNSDR_UPDATE_URL=https://releases:8443/v011/\n" > /etc/conf.d/fernsdr
        printf "FERNSDR_UPDATE_URL=https://releases:8443/v011/\n" > /etc/default/fernsdr
    ' > /dev/null 2>&1
}

# Runs in a subshell per machine, writing PASS and FAIL lines to its result
# file and everything else to its log.
machine() {
    name=$1
    distro=$2
    results=$3
    pass() { printf 'PASS  %s: %s\n' "$distro" "$*" >> "$results"; }
    fail() { printf 'FAIL  %s: %s\n' "$distro" "$*" >> "$results"; }
    lab() { docker exec "$name" "$@"; }
    # A URL's body, from inside the machine.
    get() { lab sh -c "curl -fsS --max-time 5 '$1' 2>/dev/null || wget -qO- -T 5 '$1' 2>/dev/null"; }
    # A URL's status code, from another machine on the network.
    code() { docker exec "$SERVER" curl -s -o /dev/null -w '%{http_code}' --max-time 5 "$@" || true; }
    status() { lab cat /var/lib/fernsdr-update/status.json 2>/dev/null || true; }

    # 1. No terminal and no FERNSDR_SETUP.
    if out=$(one_liner "$name" v010); then
        fail "installed with no answer to its question"
    else
        case "$out" in
            *"no terminal"*) ;;
            *) fail "without an answer: $out" ;;
        esac
        if lab test -e /opt/fernsdr || lab id fernsdr > /dev/null 2>&1; then
            fail "without an answer, something was installed"
        else
            pass "no terminal, no FERNSDR_SETUP: stopped, nothing installed"
        fi
    fi

    # 2. The first install, at home.
    if out=$(one_liner "$name" v010 FERNSDR_SETUP=home); then
        printf '%s\n' "$out" > "$WORK/out-$(slug "$distro")-install.txt"
        signature=$(printf '%s\n' "$out" | sed -n 's/.*(\(signature[^)]*\)).*/\1/p' | head -n 1)
        pass "installed 0.1.0 at home; $signature"
    else
        printf '%s\n' "$out" >> "$WORK/log-$(slug "$distro").txt"
        fail "the first install failed: $(printf '%s' "$out" | tail -n 3 | tr '\n' ' ')"
        return 0
    fi
    init=$(init_of "$distro")
    # The receiver's process: the unit's main one, or the one the supervisor runs.
    receiver_pid() {
        if [ "$init" = systemd ]; then
            lab systemctl show -p MainPID --value fernsdr.service
        else
            lab sh -c 'for p in /proc/[0-9]*; do [ "$(readlink $p/exe 2>/dev/null)" = /opt/fernsdr/releases/$(readlink /opt/fernsdr/current | sed s,releases/,,)/fernsdr ] && [ "$(stat -c %U $p)" = fernsdr ] && echo ${p#/proc/}; done' | head -n 1
        fi
    }
    case "$init" in
        systemd)
            active=$(lab systemctl is-active fernsdr.service fernsdr-update.path fernsdr-update-boot.service | tr '\n' ' ')
            enabled=$(lab systemctl is-enabled fernsdr.service fernsdr-update.path fernsdr-update-boot.service | tr '\n' ' ')
            [ "$active" = "active active active " ] && [ "$enabled" = "enabled enabled enabled " ] &&
                pass "the receiver, the update path and the boot check are enabled and active" ||
                fail "units: active $active, enabled $enabled"
            ;;
        openrc)
            lab rc-service fernsdr status > /dev/null 2>&1 && lab sh -c 'rc-update show default | grep -q fernsdr' &&
                pass "OpenRC runs the supervisor and starts it at boot" || fail "OpenRC: $(lab rc-service fernsdr status 2>&1)"
            ;;
        runit)
            lab sh -c 'sv status fernsdr | grep -q ^run: && [ -L /var/service/fernsdr ]' &&
                pass "runit runs the supervisor and starts it at boot" || fail "runit: $(lab sv status fernsdr 2>&1)"
            ;;
        sysv)
            lab sh -c '/etc/init.d/fernsdr status > /dev/null && ls /etc/rc2.d | grep -q "^S.*fernsdr"' &&
                pass "SysV init runs the supervisor and starts it at boot" || fail "SysV: $(lab /etc/init.d/fernsdr status 2>&1)"
            ;;
    esac
    [ "$(lab readlink /opt/fernsdr/current)" = releases/0.1.0 ] && [ "$(lab readlink /opt/fernsdr/trusted)" = releases/0.1.0 ] &&
        pass "current and trusted are 0.1.0" || fail "links: $(lab readlink /opt/fernsdr/current) $(lab readlink /opt/fernsdr/trusted)"
    modes=$(lab stat -c '%U:%G %a' /var/lib/fernsdr /var/lib/fernsdr/fernsdr.conf /opt/fernsdr/admin-password \
        /var/lib/fernsdr-update /opt/fernsdr/releases/0.1.0/fernsdr | tr '\n' ',')
    [ "$modes" = "fernsdr:fernsdr 700,fernsdr:fernsdr 600,root:root 600,root:root 755,root:root 755," ] &&
        pass "owners and modes" || fail "owners and modes: $modes"
    pid=$(receiver_pid)
    [ -n "$pid" ] && [ "$(lab stat -c %U "/proc/$pid")" = fernsdr ] && pass "the receiver runs as fernsdr" ||
        fail "the receiver runs as $(lab stat -c %U "/proc/$pid" 2>&1)"
    get http://127.0.0.1:8073/api/status | grep -q '"bands"' && [ "$(code "http://$name:8073/api/status")" = 200 ] &&
        pass "it answers on this machine and on the network" || fail "it does not answer"
    [ "$(code "http://$name:8073/api/admin/challenge")" = 405 ] &&
        pass "the admin panel lets the home network in over plain HTTP" ||
        fail "the admin panel from the home network: $(code "http://$name:8073/api/admin/challenge")"
    # Without a terminal the password stays in its file, out of the output.
    password=$(lab cat /opt/fernsdr/admin-password)
    hash=$(lab sed -n 's/^password_hash = //p' /var/lib/fernsdr/fernsdr.conf)
    if [ -n "$password" ] && ! printf '%s' "$out" | grep -qF "$password" &&
        printf '%s' "$out" | grep -q "Password: *in /opt/fernsdr/admin-password" &&
        python3 -c 'import hashlib, sys
kind, iterations, salt, derived = sys.argv[2].split("$")
sys.exit(hashlib.pbkdf2_hmac("sha256", sys.argv[1].encode(), salt.encode(), int(iterations)).hex() != derived)' \
            "$password" "$hash"; then
        pass "the password is in its file, not in the output, and is the one the receiver checks"
    else
        fail "the password ($password) is in the output or does not match the hash ($hash)"
    fi
    if [ "$init" = openrc ]; then
        lab grep -q '^# FernSDR begin' /etc/mdev.conf && ! lab test -e /etc/udev && lab test -f /etc/modprobe.d/fernsdr-rtlsdr.conf &&
            pass "the dongle rules are in place, for mdev" || fail "no dongle rules for mdev"
    else
        lab test -f /etc/udev/rules.d/61-fernsdr-usb.rules && lab test -f /etc/modprobe.d/fernsdr-rtlsdr.conf &&
            pass "the dongle rules are in place" || fail "no dongle rules"
    fi
    if [ "$init" != systemd ]; then
        # The supervisor's part: its log, and a receiver that dies is back.
        lab grep -q 'supervise: starting' /var/log/fernsdr/fernsdr.log &&
            pass "the supervisor logs to /var/log/fernsdr/fernsdr.log" || fail "no supervisor log"
        lab kill -9 "$pid"
        back=""
        for i in $(seq 1 15); do
            sleep 1
            new=$(receiver_pid)
            if [ -n "$new" ] && [ "$new" != "$pid" ] && get http://127.0.0.1:8073/api/status | grep -q '"bands"'; then
                back=$i
                break
            fi
        done
        [ -n "$back" ] && pass "a receiver killed is back within $back s" || fail "a receiver killed did not come back"
        pid=$(receiver_pid)
    fi

    # 3. Run again, nothing newer.
    if out=$(one_liner "$name" v010) && printf '%s' "$out" | grep -q "the newest release" &&
        [ "$(receiver_pid)" = "$pid" ]; then
        pass "run again with nothing newer: the receiver kept running"
    else
        fail "run again with nothing newer: $(printf '%s' "$out" | tail -n 3 | tr '\n' ' ')"
    fi

    # 4. Run again with 0.1.1 published.
    started=$(date +%s)
    if out=$(one_liner "$name" v011); then
        printf '%s\n' "$out" > "$WORK/out-$(slug "$distro")-update.txt"
        if [ "$(lab readlink /opt/fernsdr/current)" = releases/0.1.1 ] && [ "$(lab readlink /opt/fernsdr/trusted)" = releases/0.1.1 ] &&
            [ "$(lab /opt/fernsdr/current/fernsdr --version)" = "FernSDR 0.1.1" ] && status | grep -q '"state":"updated"' &&
            lab test -d /opt/fernsdr/releases/0.1.0 && get http://127.0.0.1:8073/api/status | grep -q '"bands"'; then
            pass "updated to 0.1.1 through the updater in $(($(date +%s) - started)) s"
        else
            fail "after the update: current $(lab readlink /opt/fernsdr/current), $(status)"
        fi
    else
        printf '%s\n' "$out" >> "$WORK/log-$(slug "$distro").txt"
        fail "the update failed: $(printf '%s' "$out" | tail -n 3 | tr '\n' ' ') $(status)"
    fi
    if [ "$init" = systemd ]; then
        failed_units=$(lab systemctl --failed --no-legend --plain 2>/dev/null | grep -o 'fernsdr[^ ]*' || true)
        [ -z "$failed_units" ] && pass "no FernSDR unit is left failed" || fail "failed units: $failed_units"
    else
        lab grep -q 'the receiver was restarted for the updater' /var/log/fernsdr/fernsdr.log &&
            lab grep -q 'the updater ended (exit status 0)' /var/log/fernsdr/fernsdr.log &&
            pass "the supervisor restarted the receiver for the updater, which ended well" ||
            fail "the supervisor's log: $(lab tail -n 5 /var/log/fernsdr/fernsdr.log | tr '\n' ' ')"
    fi
}

# The cases on one distribution only, which has curl.
extras() {
    distro=$1
    results=$2
    pass() { printf 'PASS  %s: %s\n' "$distro" "$*" >> "$results"; }
    fail() { printf 'FAIL  %s: %s\n' "$distro" "$*" >> "$results"; }
    code() { docker exec "$SERVER" curl -s -o /dev/null -w '%{http_code}' --max-time 5 "$@" || true; }

    name=$PREFIX-internet
    boot "$name" "$distro"
    lab() { docker exec "$name" "$@"; }
    # 5. A manifest changed after it was signed.
    if out=$(one_liner "$name" tampered FERNSDR_SETUP=home); then
        fail "a changed manifest was installed"
    elif printf '%s' "$out" | grep -q "not signed by a FernSDR release key" && ! lab test -e /opt/fernsdr &&
        ! lab id fernsdr > /dev/null 2>&1; then
        pass "a manifest changed after it was signed is refused, nothing installed"
    else
        fail "a changed manifest: $out"
    fi
    # 6. On a server on the internet, with a domain.
    if out=$(one_liner "$name" v010 "FERNSDR_SETUP='internet radio.example.org'"); then
        printf '%s\n' "$out" > "$WORK/out-internet.txt"
        conf=$(lab cat /var/lib/fernsdr/fernsdr.conf)
        if printf '%s\n' "$conf" | grep -q '^bind = 127.0.0.1$' && ! printf '%s\n' "$conf" | grep -q home_network &&
            printf '%s\n' "$out" | grep -q '^radio.example.org {$' &&
            [ "$(code "http://$name:8073/api/status")" = 000 ] &&
            [ "$(lab curl -s -o /dev/null -w '%{http_code}' -X POST http://127.0.0.1:8073/api/admin/challenge)" = 200 ]; then
            pass "an internet install listens on this machine only, and says how to put Caddy in front"
        else
            fail "the internet install: $(printf '%s' "$out" | tail -n 12 | tr '\n' ' ')"
        fi
    else
        fail "the internet install failed: $(printf '%s' "$out" | tail -n 3 | tr '\n' ' ')"
    fi

    # The receiver's user owns its directory, and a receiver someone took over
    # may leave anything there for the installer's next run as root: a link
    # to a file of root's in place of its configuration gets it nothing, and
    # a FIFO does not hold the installer up.
    lab sh -c 'printf "lab secret %s\n" "$(od -An -N8 -tx1 /dev/urandom | tr -d " ")" > /root/lab-secret
        chmod 600 /root/lab-secret'
    secret=$(lab cat /root/lab-secret)
    lab runuser -u fernsdr -- sh -c 'cd /var/lib/fernsdr && mv fernsdr.conf fernsdr.conf.real && ln -s /root/lab-secret fernsdr.conf'
    if out=$(one_liner "$name" v010); then
        fail "a configuration linked to a file of root's: the installer went on"
    elif printf '%s' "$out" | grep -q "cannot be read by fernsdr" &&
        ! lab grep -rqF "$secret" /var/lib/fernsdr /var/lib/fernsdr-update /tmp; then
        pass "a configuration linked to a file of root's: refused, and nothing of the file reached the receiver"
    else
        fail "a configuration linked to a file of root's: $(printf '%s' "$out" | tail -n 3 | tr '\n' ' ')"
    fi
    lab runuser -u fernsdr -- sh -c 'cd /var/lib/fernsdr && rm fernsdr.conf && mkfifo fernsdr.conf'
    started=$(date +%s)
    if out=$(one_liner "$name" v010 "FERNSDR_SETUP='internet radio.example.org'") &&
        [ "$(($(date +%s) - started))" -lt 120 ] && lab test -f /var/lib/fernsdr/fernsdr.conf &&
        [ "$(lab stat -c '%U %a' /var/lib/fernsdr/fernsdr.conf)" = "fernsdr 600" ]; then
        pass "a FIFO in place of the configuration: a new one in $(($(date +%s) - started)) s"
    else
        fail "a FIFO in place of the configuration: $(printf '%s' "$out" | tail -n 3 | tr '\n' ' ')"
    fi
    [ -n "${LAB_KEEP:-}" ] || docker rm -f "$name" > /dev/null

    # 7. A receiver that tools/source-install.sh --service set up.
    name=$PREFIX-migrate
    boot "$name" "$distro"
    old_layout() {
        lab sh -eu -c '
            useradd --system --no-create-home --shell /usr/sbin/nologin fernsdr
            mkdir -p /opt/fernsdr
            cp -R /lab/old-layout/bin /lab/old-layout/etc /lab/old-layout/share /opt/fernsdr/
            chown -R fernsdr:fernsdr /opt/fernsdr/etc
            cp /lab/old-layout/fernsdr.service /etc/systemd/system/fernsdr.service
            systemctl daemon-reload
            systemctl enable --now fernsdr.service > /dev/null 2>&1 || true'
    }
    old_layout
    sleep 3
    lab curl -fsS -o /dev/null http://127.0.0.1:8073/api/status && pass "the old layout serves before the move" ||
        fail "the old layout does not serve"
    if out=$(one_liner "$name" v010); then
        printf '%s\n' "$out" > "$WORK/out-migrate.txt"
        pid=$(lab systemctl show -p MainPID --value fernsdr.service)
        if [ "$(lab cat /var/lib/fernsdr/notes.txt)" = "kept by the operator" ] && lab test -f /var/lib/fernsdr/history/20m.bin &&
            lab grep -q '^directory = /var/lib/fernsdr/fernsdr-modules$' /var/lib/fernsdr/fernsdr.conf &&
            lab grep -q '^password_hash = pbkdf2' /var/lib/fernsdr/fernsdr.conf &&
            [ "$(lab readlink "/proc/$pid/exe")" = /opt/fernsdr/releases/0.1.0/fernsdr ] &&
            lab test -f /opt/fernsdr/old-layout/fernsdr.service && lab test -d /opt/fernsdr/old-layout/etc &&
            ! lab test -e /opt/fernsdr/etc && ! lab test -e /opt/fernsdr/bin &&
            lab curl -fsS -o /dev/null http://127.0.0.1:8073/api/status; then
            pass "the old layout moved over, its files and paths with it, and runs from the release"
        else
            fail "after the move: $(lab ls -la /var/lib/fernsdr /opt/fernsdr | tr '\n' ' ')"
        fi
    else
        fail "the move failed: $(printf '%s' "$out" | tail -n 5 | tr '\n' ' ')"
    fi
    [ -n "${LAB_KEEP:-}" ] || docker rm -f "$name" > /dev/null

    # 8. One whose configuration the release refuses.
    name=$PREFIX-refused
    boot "$name" "$distro"
    old_layout
    lab sh -c 'printf "\n[band:broken]\nsource = nothing-like-it\n" >> /opt/fernsdr/etc/fernsdr.conf'
    before=$(lab sha256sum /opt/fernsdr/etc/fernsdr.conf /etc/systemd/system/fernsdr.service)
    if out=$(one_liner "$name" v010); then
        fail "a configuration the release refuses was moved over"
    elif [ "$(lab sha256sum /opt/fernsdr/etc/fernsdr.conf /etc/systemd/system/fernsdr.service)" = "$before" ] &&
        ! lab test -e /var/lib/fernsdr && ! lab test -e /opt/fernsdr/current && ! lab test -e /opt/fernsdr/trusted &&
        ! lab test -e /etc/systemd/system/fernsdr-update.service &&
        [ "$(lab sh -c 'ls -d /var/lib/fernsdr.not-moved-* | wc -l')" = 1 ] &&
        printf '%s' "$out" | grep -q "refuses /var/lib/fernsdr/fernsdr.conf"; then
        pass "a configuration the release refuses: the receiver before is left as it was"
    else
        fail "a refused move: $(printf '%s' "$out" | tail -n 6 | tr '\n' ' ')"
    fi
    [ -n "${LAB_KEEP:-}" ] || docker rm -f "$name" > /dev/null

    # A move that fails halfway, here where the new configuration is
    # written, after the old receiver was stopped: it runs again as before.
    name=$PREFIX-halfway
    boot "$name" "$distro"
    old_layout
    sleep 3
    lab sh -c 'mkdir /opt/fernsdr/etc/.fernsdr.conf.new && chown fernsdr:fernsdr /opt/fernsdr/etc/.fernsdr.conf.new'
    before=$(lab sha256sum /opt/fernsdr/etc/fernsdr.conf /etc/systemd/system/fernsdr.service)
    if out=$(one_liner "$name" v010); then
        fail "a move that failed halfway went through"
    else
        pid=$(lab systemctl show -p MainPID --value fernsdr.service)
        if [ "$(lab sha256sum /opt/fernsdr/etc/fernsdr.conf /etc/systemd/system/fernsdr.service)" = "$before" ] &&
            [ "$(lab readlink "/proc/$pid/exe")" = /opt/fernsdr/bin/fernsdr ] &&
            lab curl -fsS -o /dev/null http://127.0.0.1:8073/api/status &&
            ! lab test -e /var/lib/fernsdr && ! lab test -e /opt/fernsdr/current &&
            printf '%s' "$out" | grep -q "Putting the receiver in /opt/fernsdr/etc back"; then
            pass "a move that fails halfway: the receiver before runs again as it was"
        else
            fail "a move that failed halfway: $(printf '%s' "$out" | tail -n 6 | tr '\n' ' ')"
        fi
    fi

    # 9. install.sh as the source has it.
    if out=$(one_liner "$name" source FERNSDR_SETUP=home); then
        fail "the source's install.sh ran"
    elif printf '%s' "$out" | grep -q "as the source has it"; then
        pass "install.sh as the source has it refuses to run"
    else
        fail "the source's install.sh: $out"
    fi
    [ -n "${LAB_KEEP:-}" ] || docker rm -f "$name" > /dev/null
}

run() {
    distro=$1
    name=$PREFIX-$(slug "$distro")
    results=$WORK/result-$(slug "$distro").txt
    : > "$results"
    boot "$name" "$distro"
    machine "$name" "$distro" "$results" >> "$WORK/log-$(slug "$distro").txt" 2>&1 || true
    [ -n "${LAB_KEEP:-}" ] || docker rm -f "$name" > /dev/null
    if [ "$distro" = "$EXTRAS" ]; then
        extras "$distro" "$results" >> "$WORK/log-$(slug "$distro").txt" 2>&1 || true
    fi
    cat "$results"
}

say "running on:$READY"
running=0
for distro in $READY; do
    run "$distro" &
    running=$((running + 1))
    if [ "$running" -ge "$JOBS" ]; then
        wait
        running=0
    fi
done
wait

[ -n "${LAB_KEEP:-}" ] || { docker rm -f "$SERVER" > /dev/null; docker network rm "$NETWORK" > /dev/null; }
say ""
FAILURES=$(cat "$WORK"/result-*.txt | grep -c '^FAIL' || true)
PASSES=$(cat "$WORK"/result-*.txt | grep -c '^PASS' || true)
if [ "$FAILURES" -eq 0 ]; then
    say "all $PASSES checks passed"
else
    say "$FAILURES failed, $PASSES passed"
    exit 1
fi
