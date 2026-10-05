#!/bin/sh
#
# FernSDR from source: build, test and optionally install as a service.
#
#   tools/source-install.sh                  build, configure and start a receiver
#   tools/source-install.sh --service        also install a systemd unit and enable it
#   tools/source-install.sh --prefix DIR     install the binary and client into DIR
#   tools/source-install.sh --port N         listen on N instead of 8073
#   tools/source-install.sh --no-client      skip npm; use a current prebuilt client if present
#   tools/source-install.sh --no-start       install without starting or restarting the receiver
#   tools/source-install.sh --tls HOST       also write a Caddy config that terminates
#                                            HTTPS for HOST with an automatic certificate
#   tools/source-install.sh --service --usb  also let modules open USB devices, and set up
#                                            RTL-SDR dongles for them
#
# POSIX sh on purpose: this has to run on a Debian box, an Alpine container and
# a Raspberry Pi without anyone thinking about it first. It builds from source,
# for a machine or a system the released builds do not cover, and for working
# on FernSDR itself; install.sh, published with each release, installs one.

set -eu

PREFIX=""
PORT=8073
SERVICE=0
USB=0
CLIENT=1
START=1
TLS_HOST=""

say() { printf '\033[36m==\033[0m %s\n' "$*"; }
warn() { printf '\033[33m!!\033[0m %s\n' "$*" >&2; }
die() { printf '\033[31m!!\033[0m %s\n' "$*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case "$1" in
        --service) SERVICE=1 ;;
        --prefix) [ $# -ge 2 ] && [ -n "$2" ] || die "--prefix needs a directory"; PREFIX="$2"; shift ;;
        --port) [ $# -ge 2 ] && [ -n "$2" ] || die "--port needs a number"; PORT="$2"; shift ;;
        --no-client) CLIENT=0 ;;
        --tls) [ $# -ge 2 ] && [ -n "$2" ] || die "--tls needs a hostname"; TLS_HOST="$2"; shift ;;
        --no-start) START=0 ;;
        --usb) USB=1 ;;
        -h|--help) sed -n '2,19p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        *) die "unknown option $1 (try --help)" ;;
    esac
    shift
done

case "$PORT" in ''|*[!0-9]*) die "port must be a number from 1 to 65535" ;; esac
[ "${#PORT}" -le 5 ] && [ "$PORT" -ge 1 ] && [ "$PORT" -le 65535 ] || die "port must be from 1 to 65535"
case "$TLS_HOST" in *[!a-zA-Z0-9.-]*) die "--tls expects a hostname, such as radio.example.org" ;; esac

if [ "$SERVICE" = 1 ]; then
    command -v systemctl >/dev/null 2>&1 || die "--service needs systemd"
    [ "$(id -u)" = 0 ] || die "--service needs root (try: sudo tools/source-install.sh --service)"
    # install.sh's service has the same name. This would replace it, and
    # leave the release's updater restarting a program it did not install.
    [ ! -L /opt/fernsdr/trusted ] ||
        die "install.sh installed a release here, whose service this would replace; run this build by hand instead"
    if [ -z "$PREFIX" ]; then
        PREFIX=/opt/fernsdr
    fi
fi
# USB access is granted to the service's own user; run by hand, the receiver
# has whatever access the person running it has.
[ "$USB" = 0 ] || [ "$SERVICE" = 1 ] || die "--usb goes with --service (sudo tools/source-install.sh --service --usb)"

if [ -n "$PREFIX" ]; then
    case "$PREFIX" in /*) ;; *) PREFIX="$PWD/$PREFIX" ;; esac
fi
if [ "$SERVICE" = 1 ]; then
    case "$PREFIX" in /root|/root/*|/home|/home/*) die "the service cannot read home directories; use --prefix /opt/fernsdr" ;; esac
    case "$PREFIX" in *[!a-zA-Z0-9_./\ -]*) die "the service prefix contains unsupported characters" ;; esac
fi

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$ROOT"

# --- what is missing -------------------------------------------------------

missing=""
for tool in make g++; do
    command -v "$tool" >/dev/null 2>&1 || missing="$missing $tool"
done
if [ "$CLIENT" = 1 ] && ! command -v npm >/dev/null 2>&1; then
    warn "npm is not installed, so the browser client cannot be built here."
    warn "Building the server only. Install Node.js and re-run to add the client,"
    warn "or copy a prebuilt web/dist from another machine."
    CLIENT=0
fi

# The client's build tools need Node 20.19 or later in the 20 series, or 22.12
# or later: web/package.json says so under "engines". Several distributions
# still ship older ones, and the build would fail halfway with a less helpful
# message than this.
node_supported() {
    version=$(node --version 2>/dev/null | sed 's/^v//')
    major=${version%%.*}
    rest=${version#*.}
    minor=${rest%%.*}
    case "$major.$minor" in *[!0-9.]*|.*|*.) return 1 ;; esac
    [ "$major" -ge 23 ] && return 0
    [ "$major" -eq 22 ] && [ "$minor" -ge 12 ] && return 0
    [ "$major" -eq 20 ] && [ "$minor" -ge 19 ] && return 0
    return 1
}
if [ "$CLIENT" = 1 ] && ! node_supported; then
    warn "Node.js $(node --version 2>/dev/null || echo '(unknown)') is too old to build the browser client;"
    warn "it needs 20.19 or later, or 22.12 or later (https://nodejs.org/en/download)."
    warn "Building the server only. Re-run with a newer Node.js to add the client,"
    warn "or copy a prebuilt web/dist from another machine."
    CLIENT=0
fi

if [ -n "$missing" ]; then
    say "Missing:$missing"
    if command -v apt-get >/dev/null 2>&1; then
        echo "  sudo apt-get install -y build-essential"
    elif command -v dnf >/dev/null 2>&1; then
        echo "  sudo dnf install -y gcc-c++ make"
    elif command -v apk >/dev/null 2>&1; then
        echo "  sudo apk add build-base"
    elif command -v pacman >/dev/null 2>&1; then
        echo "  sudo pacman -S --needed base-devel"
    fi
    die "install the above, then run this again"
fi

# --- build -----------------------------------------------------------------

JOBS=$( (command -v nproc >/dev/null 2>&1 && nproc) || echo 2 )
[ "$JOBS" -le 8 ] || JOBS=8
if [ -r /proc/meminfo ]; then
    MEMORY_JOBS=$(awk '/MemAvailable:/ { n=int($2/524288); print (n>0 ? n : 1); exit }' /proc/meminfo)
    if [ -n "$MEMORY_JOBS" ] && [ "$MEMORY_JOBS" -lt "$JOBS" ]; then JOBS=$MEMORY_JOBS; fi
fi
JOBS=${FERNSDR_JOBS:-$JOBS}
case "$JOBS" in ''|*[!0-9]*|0) die "FERNSDR_JOBS must be a positive integer" ;; esac
say "Building the server with $JOBS jobs"
make -C server -j"$JOBS" >/dev/null

say "Running the test suite"
TEST_LOG=$(mktemp)
trap 'rm -f "$TEST_LOG"' EXIT HUP INT TERM
if ! make -C server test >"$TEST_LOG" 2>&1; then
    tail -20 "$TEST_LOG"
    die "the test suite failed; this build is not safe to run"
fi
grep -E "checks, .* failures" "$TEST_LOG" || tail -1 "$TEST_LOG"
rm -f "$TEST_LOG"
trap - EXIT HUP INT TERM

if [ "$CLIENT" = 1 ]; then
    say "Building the browser client"
    if [ "$(id -u)" = 0 ]; then
        # npm runs the packages' own install scripts and build tools, which
        # must not run as root: under sudo, as --service needs, the client is
        # built as the checkout's owner.
        OWNER=$(stat -c %U web)
        [ "$OWNER" != root ] ||
            die "the browser client's build runs code from npm packages, and here it would run as root. Build it as an ordinary user (cd web && npm ci && npm run build) and run this again with --no-client, or clone as an ordinary user and run this with sudo."
        # Left by an earlier run as root, which the owner could not replace.
        for built in web/node_modules web/dist; do
            if [ -e "$built" ] && [ "$(stat -c %U "$built")" != "$OWNER" ]; then rm -rf "$built"; fi
        done
        if command -v runuser >/dev/null 2>&1; then
            runuser -u "$OWNER" -- sh -c 'cd web && npm ci --no-audit --no-fund && npm run build'
        else
            su -s /bin/sh "$OWNER" -c 'cd web && npm ci --no-audit --no-fund && npm run build'
        fi
    else
        (cd web && npm ci --no-audit --no-fund && npm run build)
    fi
fi

# --- configure -------------------------------------------------------------

CONFIG="$ROOT/server/fernsdr.conf"
if [ -n "$PREFIX" ]; then
    mkdir -p "$PREFIX/etc"
    CONFIG="$PREFIX/etc/fernsdr.conf"
fi
NEW_CONFIG=0
if [ -f "$CONFIG" ]; then
    say "Keeping the existing $CONFIG"
else
    NEW_CONFIG=1
    say "Writing $CONFIG (synthetic band, so it works before any hardware does)"
    cp server/fernsdr.example.conf "$CONFIG"
    if [ "$PORT" != 8073 ]; then
        sed -i.bak "s/^port *=.*/port = $PORT/" "$CONFIG" && rm -f "$CONFIG.bak"
    fi
fi

# --- install ---------------------------------------------------------------

BINARY="$ROOT/server/build/fernsdr"
WEBROOT="$ROOT/web/dist"

# A new configuration gets an admin password of its own, as install.sh gives
# one: 20 characters from 32 that cannot be mistaken for one another. The
# receiver keeps only its hash; the password is shown once, at the end.
ADMIN_PASSWORD=""
if [ "$NEW_CONFIG" = 1 ]; then
    ADMIN_PASSWORD=$(od -An -N20 -tu1 /dev/urandom | awk -v alphabet=abcdefghjkmnpqrstuvwxyz023456789 '
        { for (i = 1; i <= NF; i++) out = out substr(alphabet, $i % 32 + 1, 1) }
        END { print substr(out, 1, 4) "-" substr(out, 5, 4) "-" substr(out, 9, 4) "-" substr(out, 13, 4) "-" substr(out, 17, 4) }')
    printf '%s\n' "$ADMIN_PASSWORD" | "$BINARY" --set-password "$CONFIG" 2>/dev/null ||
        { warn "could not set an admin password; set one with: $BINARY --set-password $CONFIG"; ADMIN_PASSWORD=""; }
fi

if [ -n "$PREFIX" ]; then
    say "Installing into $PREFIX"
    mkdir -p "$PREFIX/bin" "$PREFIX/share/fernsdr" "$PREFIX/etc"
    if [ -f "$WEBROOT/fernsdr-client-v3" ]; then
        mkdir -p "$PREFIX/share/fernsdr/web"
        cp -R "$WEBROOT/." "$PREFIX/share/fernsdr/web/"
    elif [ -f "$PREFIX/share/fernsdr/web/index.html" ] && [ ! -f "$PREFIX/share/fernsdr/web/fernsdr-client-v3" ]; then
        die "the installed client is older than this server; build web/dist here or copy a current build first"
    fi
    # Replace the inode so an already running receiver can finish using its
    # previous executable. Copying over that executable fails with ETXTBSY.
    cp "$BINARY" "$PREFIX/bin/.fernsdr-$$"
    mv -f "$PREFIX/bin/.fernsdr-$$" "$PREFIX/bin/fernsdr"
    BINARY="$PREFIX/bin/fernsdr"
    WEBROOT="$PREFIX/share/fernsdr/web"
fi

# --- service ---------------------------------------------------------------

if [ "$SERVICE" = 1 ]; then
    command -v systemctl >/dev/null 2>&1 || die "--service needs systemd"
    [ "$(id -u)" = 0 ] || die "--service needs root (try: sudo tools/source-install.sh --service)"

    # A dedicated unprivileged user, and a unit that gives the process nothing
    # it does not need. A receiver is on the public internet 24/7; it should
    # not be able to write to its own binary, see other users' processes, or
    # gain privileges even if something in it is one day found to be wrong.
    SERVICE_USER=fernsdr
    id "$SERVICE_USER" >/dev/null 2>&1 || useradd --system --no-create-home --shell /usr/sbin/nologin "$SERVICE_USER"
    # As the receiver's user once the configuration is its own: root reading a
    # file the receiver can rewrite would act on whatever the receiver put
    # there.
    if [ "$(stat -c %U "$CONFIG" 2>/dev/null)" = "$SERVICE_USER" ]; then
        runuser -u "$SERVICE_USER" -- "$BINARY" "$CONFIG" --root "$WEBROOT" --check >/dev/null
    else
        "$BINARY" "$CONFIG" --root "$WEBROOT" --check >/dev/null
    fi

    # Without --usb the receiver sees no devices at all. With it, it sees USB
    # devices and nothing else: the hardware modules it starts need them, and
    # libusb needs a netlink socket to notice a dongle being plugged in.
    DEVICES="PrivateDevices=yes"
    FAMILIES="AF_INET AF_INET6 AF_UNIX"
    if [ "$USB" = 1 ]; then
        DEVICES="PrivateDevices=no
DevicePolicy=closed
DeviceAllow=char-usb_device rw"
        FAMILIES="AF_INET AF_INET6 AF_UNIX AF_NETLINK"
        # RTL2832U dongles, including the RTL-SDR Blog V3 and V4, for the
        # service's own group and nobody else. Named to sort after the
        # distribution's own 60-librtlsdr*.rules, and with := so that no later
        # rule can take the group back: under the old name the librtlsdr
        # package's rule won and the service could not open the dongle.
        rm -f /etc/udev/rules.d/60-fernsdr-usb.rules
        # And the RX-888's FX3, before (00f3) and after (00f1) its firmware.
        cat > /etc/udev/rules.d/61-fernsdr-usb.rules <<RULES
SUBSYSTEM=="usb", ATTRS{idVendor}=="0bda", ATTRS{idProduct}=="2838", MODE:="0660", GROUP:="$SERVICE_USER"
SUBSYSTEM=="usb", ATTRS{idVendor}=="0bda", ATTRS{idProduct}=="2832", MODE:="0660", GROUP:="$SERVICE_USER"
SUBSYSTEM=="usb", ATTRS{idVendor}=="0ccd", ATTRS{idProduct}=="00a9", MODE:="0660", GROUP:="$SERVICE_USER"
SUBSYSTEM=="usb", ATTRS{idVendor}=="0ccd", ATTRS{idProduct}=="00b3", MODE:="0660", GROUP:="$SERVICE_USER"
SUBSYSTEM=="usb", ATTRS{idVendor}=="0ccd", ATTRS{idProduct}=="00d3", MODE:="0660", GROUP:="$SERVICE_USER"
SUBSYSTEM=="usb", ATTRS{idVendor}=="0ccd", ATTRS{idProduct}=="00e0", MODE:="0660", GROUP:="$SERVICE_USER"
SUBSYSTEM=="usb", ATTRS{idVendor}=="185b", ATTRS{idProduct}=="0620", MODE:="0660", GROUP:="$SERVICE_USER"
SUBSYSTEM=="usb", ATTRS{idVendor}=="185b", ATTRS{idProduct}=="0650", MODE:="0660", GROUP:="$SERVICE_USER"
SUBSYSTEM=="usb", ATTRS{idVendor}=="1b80", ATTRS{idProduct}=="d393", MODE:="0660", GROUP:="$SERVICE_USER"
SUBSYSTEM=="usb", ATTRS{idVendor}=="1b80", ATTRS{idProduct}=="d394", MODE:="0660", GROUP:="$SERVICE_USER"
SUBSYSTEM=="usb", ATTRS{idVendor}=="1b80", ATTRS{idProduct}=="d395", MODE:="0660", GROUP:="$SERVICE_USER"
SUBSYSTEM=="usb", ATTRS{idVendor}=="1b80", ATTRS{idProduct}=="d39d", MODE:="0660", GROUP:="$SERVICE_USER"
SUBSYSTEM=="usb", ATTRS{idVendor}=="1d19", ATTRS{idProduct}=="1101", MODE:="0660", GROUP:="$SERVICE_USER"
SUBSYSTEM=="usb", ATTRS{idVendor}=="1d19", ATTRS{idProduct}=="1102", MODE:="0660", GROUP:="$SERVICE_USER"
SUBSYSTEM=="usb", ATTRS{idVendor}=="1d19", ATTRS{idProduct}=="1103", MODE:="0660", GROUP:="$SERVICE_USER"
SUBSYSTEM=="usb", ATTRS{idVendor}=="1d19", ATTRS{idProduct}=="1104", MODE:="0660", GROUP:="$SERVICE_USER"
SUBSYSTEM=="usb", ATTRS{idVendor}=="1f4d", ATTRS{idProduct}=="b803", MODE:="0660", GROUP:="$SERVICE_USER"
SUBSYSTEM=="usb", ATTRS{idVendor}=="1f4d", ATTRS{idProduct}=="c803", MODE:="0660", GROUP:="$SERVICE_USER"
SUBSYSTEM=="usb", ATTRS{idVendor}=="1f4d", ATTRS{idProduct}=="d286", MODE:="0660", GROUP:="$SERVICE_USER"
SUBSYSTEM=="usb", ATTRS{idVendor}=="1f4d", ATTRS{idProduct}=="d803", MODE:="0660", GROUP:="$SERVICE_USER"
SUBSYSTEM=="usb", ATTRS{idVendor}=="04b4", ATTRS{idProduct}=="00f3", MODE:="0660", GROUP:="$SERVICE_USER"
SUBSYSTEM=="usb", ATTRS{idVendor}=="04b4", ATTRS{idProduct}=="00f1", MODE:="0660", GROUP:="$SERVICE_USER"
SUBSYSTEM=="usb", ATTRS{idVendor}=="1d50", ATTRS{idProduct}=="60a1", MODE:="0660", GROUP:="$SERVICE_USER"
SUBSYSTEM=="usb", ATTRS{idVendor}=="03eb", ATTRS{idProduct}=="800c", MODE:="0660", GROUP:="$SERVICE_USER"
RULES
        # The kernel's DVB-T driver claims these dongles as TV tuners. The
        # module can detach it, but keeping it away is what works every time.
        printf 'blacklist dvb_usb_rtl28xxu\n' > /etc/modprobe.d/fernsdr-rtlsdr.conf
        if lsmod 2>/dev/null | grep -q '^dvb_usb_rtl28xxu'; then
            modprobe -r dvb_usb_rtl28xxu 2>/dev/null ||
                warn "the DVB-T driver is in use; unplug the dongle once, or reboot"
        fi
        # And the kernel's msi2500 driver off SDRplay's RSP1, which it takes
        # as a TV tuner so that SDRplay's service cannot start it.
        printf 'blacklist sdr_msi3101\nblacklist msi001\nblacklist msi2500\n' > /etc/modprobe.d/fernsdr-sdrplay.conf
        if lsmod 2>/dev/null | grep -q '^msi2500'; then
            modprobe -r msi2500 msi001 2>/dev/null ||
                warn "the msi2500 driver has an SDRplay RSP1; unplug it once, or reboot"
        fi
        if command -v udevadm >/dev/null 2>&1; then
            udevadm control --reload-rules && udevadm trigger --subsystem-match=usb || true
        fi
        say "USB devices allowed; RTL-SDR dongles and RX-888s belong to the $SERVICE_USER group"
    fi
    # systemd before 239 does not know @system-service, and on such a line
    # turns the filter on with nothing allowed: the receiver would die at its
    # first system call.
    SYSCALLS="SystemCallFilter=@system-service"
    SYSTEMD_VERSION=$(systemctl --version 2>/dev/null | awk 'NR == 1 { print $2 }')
    case "$SYSTEMD_VERSION" in ''|*[!0-9]*) SYSTEMD_VERSION=0 ;; esac
    if [ "$SYSTEMD_VERSION" -gt 0 ] && [ "$SYSTEMD_VERSION" -lt 239 ]; then
        SYSCALLS="# SystemCallFilter=@system-service needs systemd 239; this is $SYSTEMD_VERSION"
    fi
    cat > /etc/systemd/system/fernsdr.service <<UNIT
[Unit]
Description=FernSDR WebSDR
After=network-online.target
Wants=network-online.target

[Service]
Type=exec
User=$SERVICE_USER
Group=$SERVICE_USER
ExecStart="$BINARY" "$CONFIG" --root "$WEBROOT"
ExecStartPre="$BINARY" "$CONFIG" --root "$WEBROOT" --check
WorkingDirectory=$(dirname "$CONFIG")
Restart=always
RestartSec=2
# Hardening. None of this is optional for something facing the internet.
NoNewPrivileges=yes
PrivateTmp=yes
$DEVICES
ProtectSystem=strict
ProtectHome=yes
ProtectKernelTunables=yes
ProtectKernelModules=yes
ProtectControlGroups=yes
RestrictAddressFamilies=$FAMILIES
RestrictNamespaces=yes
LockPersonality=yes
MemoryDenyWriteExecute=yes
SystemCallArchitectures=native
$SYSCALLS
ReadWritePaths="$(dirname "$CONFIG")"

[Install]
WantedBy=multi-user.target
UNIT
    chown -R "$SERVICE_USER:$SERVICE_USER" "$(dirname "$CONFIG")"
    systemctl daemon-reload
    if [ "$START" = 1 ]; then
        STARTED=1
        systemctl enable fernsdr && systemctl restart fernsdr || STARTED=0
        START_PID=$(systemctl show --property=MainPID --value fernsdr) || STARTED=0
        case "$START_PID" in ''|0|*[!0-9]*) STARTED=0 ;; esac
        # Type=exec catches execution failures. A stable process also catches
        # configuration, source-open and bind failures followed by automatic
        # restart, which a successful `systemctl start` alone would miss.
        for attempt in 1 2 3 4 5; do
            [ "$STARTED" = 1 ] || break
            sleep 1
            CURRENT_PID=$(systemctl show --property=MainPID --value fernsdr) || STARTED=0
            if ! systemctl is-active --quiet fernsdr || [ "$CURRENT_PID" != "$START_PID" ]; then
                STARTED=0
            fi
        done
        if [ "$STARTED" != 1 ]; then
            systemctl disable --now fernsdr || true
            die "fernsdr.service could not start; inspect journalctl -u fernsdr"
        fi
        say "Installed and started fernsdr.service"
    else
        systemctl enable fernsdr
        say "Installed fernsdr.service; start it with: systemctl start fernsdr"
    fi
    say "  systemctl status fernsdr      what it is doing"
    say "  journalctl -u fernsdr -f      follow the log"
    START=0
fi

PORT_IN_USE=$(grep -E '^ *port *=' "$CONFIG" | head -1 | sed 's/.*= *//' | tr -d ' ')
[ -n "$PORT_IN_USE" ] || PORT_IN_USE=$PORT

# --- HTTPS -----------------------------------------------------------------
#
# FernSDR speaks HTTP and only HTTP, deliberately: a TLS stack is the one part
# of this that genuinely should not be written here, and linking OpenSSL would
# cost the zero-dependency build for something a reverse proxy does better.
# Caddy gets a certificate on its own, renews it on its own, and is one binary.

if [ -n "$TLS_HOST" ]; then
    CADDYFILE="$ROOT/Caddyfile"
    say "Writing $CADDYFILE for $TLS_HOST"
    cat > "$CADDYFILE" <<CADDY
# FernSDR behind Caddy. Certificates are obtained and renewed automatically.
#
#   caddy run --config $CADDYFILE
#
# The receiver itself should then listen on loopback only - set
# bind = 127.0.0.1 in fernsdr.conf - so nothing reaches it except through here.
$TLS_HOST {
	encode zstd gzip

	reverse_proxy 127.0.0.1:$PORT_IN_USE {
		# The WebSocket carries audio: buffering it would add exactly the
		# latency this whole project exists to avoid.
		flush_interval -1

		# FernSDR believes these only from a trusted peer, which loopback is
		# by default. Without them every listener appears to come from the
		# proxy and per-address limits stop meaning anything.
		header_up X-Real-IP {remote_host}
		header_up X-Forwarded-Proto {scheme}
	}
}
CADDY
    if ! command -v caddy >/dev/null 2>&1; then
        warn "Caddy is not installed. https://caddyserver.com/docs/install"
    fi
    echo
    echo "Then, in fernsdr.conf:  bind = 127.0.0.1"
    echo "and run:                caddy run --config $CADDYFILE"
    echo
fi

# --- go --------------------------------------------------------------------

echo
say "Ready."
echo "    config    $CONFIG"
echo "    binary    $BINARY"
[ "$CLIENT" = 1 ] && echo "    client    $WEBROOT"
echo "    listen    http://localhost:$PORT_IN_USE"
if [ -n "$ADMIN_PASSWORD" ]; then
    echo "    admin     http://localhost:$PORT_IN_USE/admin, password $ADMIN_PASSWORD"
    echo "              (change it on the Station page; this is the only time it is shown)"
fi
echo
echo "The default configuration generates a synthetic 40 m band, so you can"
echo "check the whole chain in a browser before connecting an antenna. Edit"
echo "$CONFIG to point at real hardware; every option is documented in it."
echo

# The receiver refuses to serve as root; say how to run it instead of
# starting it only to watch it stop.
if [ "$START" = 1 ] && [ "$(id -u)" = 0 ]; then
    warn "Not starting it: the receiver will not serve as root."
    warn "Run tools/source-install.sh as a normal user, or install the service with its own"
    warn "user: sudo tools/source-install.sh --service"
    START=0
fi

if [ "$START" = 1 ]; then
    say "Starting. Ctrl-C to stop."
    exec "$BINARY" "$CONFIG" --root "$WEBROOT"
fi
