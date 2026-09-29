#!/bin/sh
#
# FernSDR installer: a signed release of FernSDR, set up as a service: with
# systemd's units where systemd runs, and otherwise with a small script for
# OpenRC, runit or SysV init that starts `fernsdr --supervise`, which does
# what the units do.
#
#   curl -fsSL https://github.com/Steven9101/FernSDR/releases/latest/download/install.sh | sudo sh
#
# The first run asks one question, whether the receiver is at home or on a
# server on the internet, and sets it up for that: the program in
# /opt/fernsdr, its configuration and everything it writes in
# /var/lib/fernsdr, run by a user of its own, with a password for the admin
# panel made up on the spot. Until hardware is added it receives a synthetic
# band, so the whole chain can be tried in a browser first. A receiver that
# tools/source-install.sh --service set up in /opt/fernsdr moves over with
# its configuration.
#
# Run again, it updates the receiver to the newest release the way the admin
# panel does: the updater installs it and keeps it once it works, and
# otherwise puts the version before back.
#
# Without a terminal to ask on, FERNSDR_SETUP gives the answer: home,
# internet, or internet and the server's domain, as in
#   curl -fsSL .../install.sh | sudo FERNSDR_SETUP="internet radio.example.org" sh
#
# This is the installer as the source has it. A release build puts in the
# keys its releases are signed with and publishes the result beside them;
# as it stands here, it refuses to run. Everything happens in functions that
# the last line calls, so a download cut short runs nothing.

set -eu

RELEASE_URL=${FERNSDR_RELEASE_URL:-https://github.com/Steven9101/FernSDR/releases/latest/download}
# The keys releases are signed with, as OpenSSL reads a public key: the base64
# of each one's SubjectPublicKeyInfo. `fernsdr-release public-keys` prints
# them from src/update/release_keys.h, the list the receiver checks with.
RELEASE_KEYS="@RELEASE_KEYS@"

INSTALL=/opt/fernsdr
STATE=/var/lib/fernsdr
UPDATE=/var/lib/fernsdr-update
UNITS=/etc/systemd/system
SERVICE_USER=fernsdr
MANIFEST=fernsdr-release-v1.txt
IMAGE=${FERNSDR_IMAGE:-ghcr.io/steven9101/fernsdr:latest}
CONTAINER=fernsdr

WORK=""
PLATFORM=""
VERSION="" DATE="" ARCHIVE="" SIZE="" SHA256="" SIGNATURE=""
INSTALLED=""
WHERE="" DOMAIN="" PLAIN_ADMIN="" HTTPS=""
PASSWORD="" HASH=""
CONFIG_WRITTEN="" MIGRATING="" MIGRATION_OPEN="" OLD_RUNNING=""
UNITS_CHANGED="" SIGNED=""
INIT="" SVDIR="" SERVICE_CHANGED="" SERVICE_SCRIPT=""
ACTION=update

say() { printf '== %s\n' "$*"; }
note() { printf '   %s\n' "$*"; }
warn() { printf '!! %s\n' "$*" >&2; }
die() {
    printf '!! %s\n' "$*" >&2
    exit 1
}
have() { command -v "$1" >/dev/null 2>&1; }

cleanup() {
    # A move from the old layout that stopped halfway, by an error or ^C,
    # would leave no receiver running.
    if [ -n "$MIGRATION_OPEN" ]; then undo_migration; fi
    if [ -n "$WORK" ]; then rm -rf "${WORK:?}"; fi
}

size_of() { wc -c < "$1" | tr -d ' \t'; }

# Text from elsewhere, fit for the terminal: without control characters,
# including the C1 ones written as UTF-8, which some terminals act on.
printable() { LC_ALL=C awk '{ gsub(/\302[\200-\237]/, ""); gsub(/[\001-\010\013-\037\177]/, ""); print }'; }

# download URL FILE LIMIT: over HTTPS, and no more than LIMIT bytes.
download() {
    rm -f "$2"
    if have curl; then
        # -q first: curl reads no .curlrc, so this is all it is told.
        curl -q --silent --show-error --fail --location --max-redirs 5 --proto '=https' --proto-redir '=https' \
            --connect-timeout 30 --max-time 1800 --retry 3 --max-filesize "$3" --output "$2" "$1" || return 1
    else
        # wget cannot be held to HTTPS across redirects. The signature and the
        # hash vouch for what arrives, however it came.
        wget -q --max-redirect=5 --timeout=60 --tries=3 -O "$2" "$1" || return 1
    fi
    [ "$(size_of "$2")" -le "$3" ]
}

# probe URL FILE: the receiver on this machine, over plain HTTP and past any
# proxy the environment names.
probe() {
    if have curl; then
        curl -q --silent --fail --noproxy '*' --max-time 5 --output "$2" "$1" 2>/dev/null
    else
        wget -q --no-proxy --timeout=5 --tries=1 -O "$2" "$1" 2>/dev/null
    fi
}

sha256_of() {
    if have sha256sum; then
        sha256sum "$1"
    elif have shasum; then
        shasum -a 256 "$1"
    else
        openssl dgst -sha256 -r "$1"
    fi | awk '{ print $1 }'
}

# The release for this machine. The programs are static, so the processor is
# all that matters, not the distribution or the libraries it has.
platform() {
    case "$(uname -m)" in
        x86_64 | amd64) echo linux-x86_64 ;;
        aarch64 | arm64) echo linux-aarch64 ;;
        armv7* | armv8l)
            # ARMv7 with NEON: a Raspberry Pi 2 or later on a 32-bit system.
            if awk '/^Features/ { for (i = 3; i <= NF; i++) if ($i == "neon" || $i == "asimd") found = 1 }
                    END { exit !found }' /proc/cpuinfo; then
                echo linux-armhf
            else
                die "This ARM processor has no NEON, which the released builds need. tools/source-install.sh in the FernSDR source builds it here instead."
            fi
            ;;
        armv6*) die "The released builds need ARMv7 or later, and this is ARMv6, a Raspberry Pi 1 or Zero. tools/source-install.sh in the FernSDR source builds it here instead." ;;
        *) die "No release is built for $(uname -m) processors. tools/source-install.sh in the FernSDR source builds it here instead." ;;
    esac
}

# MAJOR.MINOR.PATCH, as the receiver reads a version.
valid_version() {
    case "$1" in '' | *[!0-9.]*) return 1 ;; esac
    printf '%s\n' "$1" | awk -F. 'NF != 3 { exit 1 }
        { for (i = 1; i <= 3; i++) if ($i == "" || length($i) > 9 || $i ~ /^0[0-9]/) exit 1 }'
}

# newer A B: whether version A comes after version B.
newer() {
    awk -v a="$1" -v b="$2" 'BEGIN {
        split(a, x, "."); split(b, y, ".")
        for (i = 1; i <= 3; i++) if (x[i] + 0 != y[i] + 0) exit !(x[i] + 0 > y[i] + 0)
        exit 1
    }'
}

# Nothing but lines of text: the receiver refuses a manifest with any other
# control character, and so does this, before printing a word of it.
plain_text() {
    [ "$(LC_ALL=C tr -d '\n\040-\176\200-\377' < "$1" | wc -c | tr -d ' \t')" = 0 ]
}

# The archive for this machine from the manifest, as "VERSION DATE FILE SIZE
# SHA256", read as strictly as the receiver reads a manifest in everything
# used here. Exits 2 for a testing release and 3 when none is for this
# machine.
manifest_asset() {
    awk -v platform="$PLATFORM" '
        function fail(code) { status = code; exit code }
        NR == 1 { if ($0 != "fernsdr-release 1") fail(1); next }
        NR == 2 { if ($1 != "version" || NF != 2) fail(1); version = $2; next }
        NR == 3 { if ($1 != "date" || NF != 2) fail(1); date = $2; next }
        NR == 4 { if ($1 != "channel" || NF != 2) fail(1); if ($2 != "stable") fail(2); next }
        $1 == "asset" && $2 == platform {
            if (found != "" || NF != 5 || $3 != "fernsdr-" version "-" platform ".tar" || $4 !~ /^[1-9][0-9]*$/ ||
                length($4) > 12 || $5 !~ /^[0-9a-f]+$/ || length($5) != 64) fail(1)
            found = $3 " " $4 " " $5
        }
        END {
            if (status) exit status
            if (NR < 4) exit 1
            if (found == "") exit 3
            print version, date, found
        }' "$1"
}

# The manifest's signature, checked with OpenSSL 3 where it is installed: 0
# when a release key made it, 1 when none did, 2 when nothing here can check
# it. This is for the way in only: from then on the receiver's own updater
# checks every release before it takes it.
check_signature() {
    have openssl || return 2
    case "$(openssl version 2>/dev/null)" in "OpenSSL "[3-9].*) ;; *) return 2 ;; esac
    # RFC 8032's second test vector first. An OpenSSL that cannot check it,
    # one in FIPS mode for instance, has no Ed25519, and a refusal from it
    # would say nothing about the release.
    printf '%s\n' '-----BEGIN PUBLIC KEY-----' 'MCowBQYDK2VwAyEAPUAXw+hDiVqStwqnTRt+vJyYLM8uxJaMwM1V8Sr0Zgw=' \
        '-----END PUBLIC KEY-----' > "$WORK/test.pem"
    printf 'r' > "$WORK/test.txt"
    printf '%s' 'kqAJqfDUyrhyDoILX2QlQKKye1QWUD+Ps3YiI+vbadoIWsHkPhWZbkWPNhPQ8R2MOHsurrQwKu6wDSkWErsMAA==' |
        openssl base64 -d -A > "$WORK/test.sig" 2>/dev/null || return 2
    openssl pkeyutl -verify -pubin -inkey "$WORK/test.pem" -rawin -in "$WORK/test.txt" \
        -sigfile "$WORK/test.sig" > /dev/null 2>&1 || return 2
    {
        printf 'fernsdr-release-v1\n'
        cat "$1"
    } > "$WORK/signed"
    for key in $RELEASE_KEYS; do
        printf '%s\n' '-----BEGIN PUBLIC KEY-----' "$key" '-----END PUBLIC KEY-----' > "$WORK/key.pem"
        if openssl pkeyutl -verify -pubin -inkey "$WORK/key.pem" -rawin -in "$WORK/signed" \
            -sigfile "$2" > /dev/null 2>&1; then
            return 0
        fi
    done
    return 1
}

valid_domain() {
    case "$1" in
        *[!a-zA-Z0-9.-]* | .* | *. | *..* | -* | *-.* | *.-*) return 1 ;;
        *.*) return 0 ;;
        *) return 1 ;;
    esac
}

# An answer to the question: 1 or home; 2 or internet, which a domain may
# follow; 3 or http, which `admin` may follow for the admin panel over plain
# HTTP too.
parse_where() {
    set -f
    # shellcheck disable=SC2086 # split into words on purpose
    set -- $1
    set +f
    case "$(printf '%s' "${1:-}" | tr '[:upper:]' '[:lower:]')" in
        1 | home)
            [ $# -eq 1 ] || return 1
            WHERE=home
            ;;
        2 | internet | server)
            [ $# -le 2 ] || return 1
            if [ $# -eq 2 ]; then
                valid_domain "$2" || return 1
                DOMAIN=$(printf '%s' "$2" | tr '[:upper:]' '[:lower:]')
            fi
            WHERE=internet
            ;;
        3 | http)
            [ $# -le 2 ] || return 1
            if [ $# -eq 2 ]; then
                [ "$2" = admin ] || return 1
                PLAIN_ADMIN=1
            fi
            WHERE=http
            ;;
        *) return 1 ;;
    esac
}

# The one question: where the receiver runs. At home it serves the home
# network over plain HTTP, the admin panel included. On a server with a
# domain it listens on this machine only and Caddy serves it over HTTPS,
# set up here where the distribution has Caddy. On a server without one it
# serves plain HTTP to everyone, and the admin panel only through an SSH
# tunnel, unless the operator takes the risk of plain HTTP for it too.
ask_where() {
    if [ -n "${FERNSDR_SETUP:-}" ]; then
        parse_where "$FERNSDR_SETUP" ||
            die "FERNSDR_SETUP is \"$FERNSDR_SETUP\"; it takes home, internet and the server's domain, or http."
        return 0
    fi
    # Piped into sh, the script is standard input, so the answer comes from
    # the terminal itself.
    if ! (exec < /dev/tty) 2> /dev/null; then
        die "There is no terminal to ask where the receiver runs. FERNSDR_SETUP says it instead: home, internet and the server's domain, or http, as in FERNSDR_SETUP=\"internet radio.example.org\"."
    fi
    {
        printf '\nWhere does this receiver run?\n\n'
        printf '  1  At home. Listeners and the admin panel reach it on the home network.\n'
        printf '  2  On a server with a domain name, such as radio.example.org. It is\n'
        printf '     served over HTTPS, with a certificate this installer sets up.\n'
        printf '  3  On a server without a domain name. Listeners reach it over plain HTTP.\n\n'
        printf 'For 2, add the domain: 2 radio.example.org\n'
    } > /dev/tty
    while :; do
        printf 'Answer: ' > /dev/tty
        IFS= read -r answer < /dev/tty || die "No answer, so nothing was installed."
        case "$answer" in 2 | internet) answer="" ;; esac
        [ -n "$answer" ] && parse_where "$answer" && break
        printf 'Answer 1, 2 with the domain (2 radio.example.org), or 3.\n' > /dev/tty
    done
    if [ "$WHERE" = http ] && [ -z "$PLAIN_ADMIN" ]; then
        {
            printf '\nWithout a domain there is no HTTPS, so the admin panel is reached through\n'
            printf 'an SSH tunnel: ssh -L 8073:localhost:8073 you@this-server, then\n'
            printf 'http://localhost:8073/admin. It can also answer plain HTTP from anywhere,\n'
            printf 'at your own risk: anyone between you and the server, a public Wi-Fi or a\n'
            printf 'provider, can then read the panel and change it to catch the password.\n\n'
        } > /dev/tty
        printf 'The admin panel over plain HTTP from anywhere? [y/N] ' > /dev/tty
        IFS= read -r answer < /dev/tty || answer=""
        case "$answer" in y | Y | yes | Yes) PLAIN_ADMIN=1 ;; esac
    fi
    printf '\n' > /dev/tty
}

# What is already here: a release this installer put in (INSTALLED), a
# receiver from tools/source-install.sh --service to move over (MIGRATING),
# or nothing.
find_installed() {
    if [ -L "$INSTALL/trusted" ]; then
        target=$(readlink "$INSTALL/trusted")
        INSTALLED=${target#releases/}
        if [ "$target" = "$INSTALLED" ] || ! valid_version "$INSTALLED" ||
            [ ! -x "$INSTALL/releases/$INSTALLED/fernsdr" ]; then
            die "$INSTALL/trusted names $target, which is not an installed release. Look at $INSTALL before running this again."
        fi
        return 0
    fi
    if [ -f "$UNITS/fernsdr.service" ] && ! grep -q "$INSTALL/current/fernsdr" "$UNITS/fernsdr.service" &&
        ! grep -q "$INSTALL/bin/fernsdr" "$UNITS/fernsdr.service"; then
        die "$UNITS/fernsdr.service runs a receiver installed some other way. Stop it and move its unit away, and its configuration to $STATE/fernsdr.conf if it is to be kept, then run this again."
    fi
    if [ -f "$INSTALL/etc/fernsdr.conf" ]; then MIGRATING=1; fi
}

read_release() {
    say "Looking up the newest release"
    download "$RELEASE_URL/$MANIFEST" "$WORK/manifest" 65536 || die "Could not fetch $RELEASE_URL/$MANIFEST."
    download "$RELEASE_URL/$MANIFEST.sig" "$WORK/manifest.sig" 64 || die "Could not fetch $RELEASE_URL/$MANIFEST.sig."
    [ "$(size_of "$WORK/manifest.sig")" = 64 ] || die "The release's signature is not a signature."
    if check_signature "$WORK/manifest" "$WORK/manifest.sig"; then
        SIGNED=1
        SIGNATURE="signature checked with $(openssl version | awk '{ print $1, $2 }')"
    else
        case $? in
            1) die "The release is not signed by a FernSDR release key, so nothing was installed." ;;
            *) SIGNATURE="signature not checked, with no OpenSSL 3 here; fetched over HTTPS" ;;
        esac
    fi
    plain_text "$WORK/manifest" || die "The release's manifest is not one."
    if line=$(manifest_asset "$WORK/manifest"); then :; else
        case $? in
            2) die "The newest release is a testing one, and this installs stable releases only." ;;
            3) die "The newest release has no build for $PLATFORM." ;;
            *) die "The release's manifest is not one." ;;
        esac
    fi
    read -r VERSION DATE ARCHIVE SIZE SHA256 << EOF
$line
EOF
    valid_version "$VERSION" || die "The release's manifest is not one."
    case "$DATE" in
        [0-9][0-9][0-9][0-9]-[0-9][0-9]-[0-9][0-9]) ;;
        *) die "The release's manifest is not one." ;;
    esac
    say "FernSDR $VERSION of $DATE for $PLATFORM ($SIGNATURE)"
}

# Only from a manifest whose signature was checked here: the update itself
# is checked by the updater, but these lines are shown before it runs.
release_notes() {
    [ -n "$SIGNED" ] || return 0
    sed -n 's/^note$//p; s/^note //p' "$WORK/manifest" | printable | sed 's/^/   | /'
}

ensure_user() {
    if ! getent group "$SERVICE_USER" > /dev/null 2>&1; then
        if have groupadd; then groupadd --system "$SERVICE_USER"; else addgroup -S "$SERVICE_USER"; fi ||
            die "Could not add the group $SERVICE_USER."
    fi
    id "$SERVICE_USER" > /dev/null 2>&1 && return 0
    shell=/usr/sbin/nologin
    [ -x "$shell" ] || shell=/sbin/nologin
    [ -x "$shell" ] || shell=/bin/false
    if have useradd; then
        useradd --system --gid "$SERVICE_USER" --home-dir "$STATE" --no-create-home --shell "$shell" \
            --comment "FernSDR receiver" "$SERVICE_USER"
    else
        adduser -S -D -H -h "$STATE" -s "$shell" -G "$SERVICE_USER" -g "FernSDR receiver" "$SERVICE_USER"
    fi || die "Could not add the user $SERVICE_USER."
}

# Each argument in single quotes, for a command line that a shell reads.
quote_args() {
    for argument; do
        printf " '%s'" "$(printf '%s' "$argument" | sed "s/'/'\\\\''/g")"
    done
}

# CMD...: run as the receiver's user, root gone. runuser where util-linux
# has it; otherwise BusyBox's su, which takes options out of the command's
# own arguments too (the -c of `head -c`), so the command goes to it quoted
# in one -c.
as_receiver() {
    if have runuser; then
        runuser -u "$SERVICE_USER" -- "$@"
    else
        su -s /bin/sh "$SERVICE_USER" -c "exec$(quote_args "$@")"
    fi
}

# Standard input to NAME in the receiver's directory, written as the
# receiver's user: root never writes into a directory that user owns, where a
# link placed beforehand could send the write somewhere else.
write_as_receiver() {
    as_receiver sh -c 'umask 077 && cat > "$1/.$2.new" && mv -f "$1/.$2.new" "$1/$2"' \
        sh "$STATE" "$1"
}

# The receiver's configuration, copied to $WORK/config by the receiver's
# user, for everything here to read instead. Root never opens a file in the
# receiver's directory: a link the receiver put there could point at any
# file root can read, and a FIFO or /dev/zero would hang this script or fill
# its memory. A configuration over a megabyte is no configuration.
read_config() {
    : > "$WORK/config"
    [ -f "$STATE/fernsdr.conf" ] || return 0
    as_receiver timeout 10 head -c 1048577 "$STATE/fernsdr.conf" < /dev/null > "$WORK/config" ||
        die "$STATE/fernsdr.conf cannot be read by $SERVICE_USER, as the receiver has to read it."
    [ "$(size_of "$WORK/config")" -le 1048576 ] || die "$STATE/fernsdr.conf is larger than a configuration can be."
}

# SECTION KEY DEFAULT: a setting of the receiver's configuration.
config_value() {
    read_config
    value=$(awk -v want="$1" -v key="$2" '
        { line = $0; sub(/[#;].*/, "", line); gsub(/^[ \t]+|[ \t]+$/, "", line) }
        line ~ /^\[/ { section = line; gsub(/[][ \t]/, "", section); next }
        section == want && index(line, "=") {
            name = substr(line, 1, index(line, "=") - 1); gsub(/[ \t]+$/, "", name)
            if (name != key) next
            value = substr(line, index(line, "=") + 1); gsub(/^[ \t]+|"/, "", value)
        }
        END { print value }' "$WORK/config" 2> /dev/null || true)
    printf '%s\n' "${value:-$3}"
}

receiver_port() {
    port=$(config_value server port 8073)
    case "$port" in '' | *[!0-9]*) port=8073 ;; esac
    printf '%s\n' "$port"
}

# The address the receiver listens on, as the configuration names it, or
# the default for anything that is not an address.
receiver_bind() {
    bind=$(config_value server bind 0.0.0.0)
    case "$bind" in
        '' | *[!0-9A-Za-z.:%-]*) echo 0.0.0.0 ;;
        *) printf '%s\n' "$bind" ;;
    esac
}

# Where to reach the receiver from this machine.
receiver_host() {
    bind=$(receiver_bind)
    case "$bind" in
        0.0.0.0 | :: | '[::]' | localhost) echo 127.0.0.1 ;;
        *:*) echo "[$bind]" ;;
        *) echo "$bind" ;;
    esac
}

this_address() {
    address=$(ip -4 route get 192.0.2.1 2> /dev/null |
        awk '{ for (i = 1; i < NF; i++) if ($i == "src") { print $(i + 1); exit } }' || true)
    [ -n "$address" ] || address=$(hostname -I 2> /dev/null | awk '{ print $1 }' || true)
    printf '%s\n' "${address:-this-machine}"
}

# A password for the admin panel: 20 characters from 32 that cannot be
# mistaken for one another, 100 bits. The receiver keeps only its hash; the
# password itself goes in a file for root, which /opt/fernsdr is.
make_password() {
    PASSWORD=$(od -An -N20 -tu1 /dev/urandom | awk -v alphabet=abcdefghjkmnpqrstuvwxyz023456789 '
        { for (i = 1; i <= NF; i++) out = out substr(alphabet, $i % 32 + 1, 1) }
        END { print substr(out, 1, 4) "-" substr(out, 5, 4) "-" substr(out, 9, 4) "-" substr(out, 13, 4) "-" substr(out, 17, 4) }')
    [ "${#PASSWORD}" = 24 ] || die "Could not make up a password for the admin panel."
}

new_admin_password() {
    make_password
    HASH=$(printf '%s\n' "$PASSWORD" | "$1/fernsdr" --hash-password 2> /dev/null | sed -n 's/^password_hash = //p')
    [ -n "$HASH" ] || die "Could not hash the admin panel's password."
    (umask 077 && printf '%s\n' "$PASSWORD" > "$INSTALL/admin-password")
}

# The example configuration, with its synthetic band, set up for where the
# receiver runs.
write_config() {
    new_admin_password "$1"
    case "$WHERE" in home | http) bind=0.0.0.0 ;; *) bind=127.0.0.1 ;; esac
    {
        awk -v bind="$bind" -v root="$INSTALL/current/web" '
            /^[ \t]*\[/ { section = $0; gsub(/[][ \t]/, "", section) }
            section == "server" && /^[ \t]*bind[ \t]*=/ { print "bind = " bind; next }
            section == "server" && /^[ \t]*root[ \t]*=/ { print "root = " root; next }
            { print }' "$1/fernsdr.example.conf"
        printf '\n[admin]\n'
        printf '# The password is in %s, for root only. Change it on the\n' "$INSTALL/admin-password"
        printf "# admin panel's Station page, or with %s --set-password.\n" "$INSTALL/current/fernsdr"
        printf 'password_hash = %s\n' "$HASH"
        if [ "$WHERE" = home ]; then
            printf '# The admin panel over plain HTTP from the home network (docs/DEPLOYMENT.md).\n'
            printf 'home_network = yes\n'
        fi
        if [ -n "$PLAIN_ADMIN" ]; then
            printf '# The admin panel over plain HTTP from anywhere, at your own risk: anyone\n'
            printf '# on the way can read it and catch the password (docs/DEPLOYMENT.md).\n'
            printf 'plain_http_anywhere = yes\n'
        fi
    } > "$WORK/fernsdr.conf"
    write_as_receiver fernsdr.conf < "$WORK/fernsdr.conf"
    CONFIG_WRITTEN=1
}

# A configuration kept or moved over has an admin password once this is done:
# without one, the panel and the updates it offers stay shut.
ensure_password() {
    read_config
    if grep -q '^[[:space:]]*password_hash[[:space:]]*=[[:space:]]*[^[:space:]]' "$WORK/config"; then
        return 0
    fi
    new_admin_password "$1"
    # The receiver reads the first [admin] section, so the hash goes into
    # that one if there is one.
    awk -v hash="$HASH" '
        { print }
        /^[ \t]*\[[ \t]*admin[ \t]*\][ \t]*$/ && !done { print "password_hash = " hash; done = 1 }
        END { if (!done) { print ""; print "[admin]"; print "password_hash = " hash } }' \
        "$WORK/config" > "$WORK/fernsdr.conf"
    write_as_receiver fernsdr.conf < "$WORK/fernsdr.conf"
    say "The configuration had no password for the admin panel, and has one now"
}

# The release, downloaded, checked against the manifest and unpacked into
# releases/VERSION, once its program has shown that it runs here.
unpack_release() {
    say "Downloading $ARCHIVE, $(((SIZE + 524288) / 1048576)) MB"
    download "$RELEASE_URL/$ARCHIVE" "$WORK/$ARCHIVE" "$SIZE" || die "Could not download $RELEASE_URL/$ARCHIVE."
    [ "$(size_of "$WORK/$ARCHIVE")" = "$SIZE" ] && [ "$(sha256_of "$WORK/$ARCHIVE")" = "$SHA256" ] ||
        die "The download is not the archive the release names."
    free=$(df -Pk "$INSTALL/releases" | awk 'NR == 2 { print $4 }')
    [ "${free:-0}" -ge $((SIZE / 1024 * 2 + 20480)) ] ||
        die "$INSTALL has $((${free:-0} / 1024)) MB free, too little to unpack a release of $((SIZE / 1048576)) MB."
    staging="$INSTALL/releases/.staging-install-$$"
    mkdir -m 0755 "$staging"
    if ! tar -xf "$WORK/$ARCHIVE" -C "$staging" --no-same-owner; then
        rm -rf "${staging:?}"
        die "Could not unpack $ARCHIVE."
    fi
    ran=$("$staging/fernsdr" --version 2>&1 || true)
    if [ "$ran" != "FernSDR $VERSION" ] || [ ! -f "$staging/web/index.html" ] || [ ! -d "$staging/systemd" ] ||
        { [ "$INIT" != systemd ] && [ ! -d "$staging/init" ]; }; then
        rm -rf "${staging:?}"
        die "The release does not run on this machine, or is not whole: ${ran:-its program says nothing}"
    fi
    if [ -e "$INSTALL/releases/$VERSION" ]; then rm -rf "${INSTALL:?}/releases/${VERSION:?}"; fi
    mv "$staging" "$INSTALL/releases/$VERSION"
}

make_state_directory() {
    install -d -m 0700 "$STATE"
    chown "$SERVICE_USER:$SERVICE_USER" "$STATE"
    chmod 0700 "$STATE"
}

# tools/source-install.sh --service ran the receiver from /opt/fernsdr/bin
# with everything it keeps in /opt/fernsdr/etc. That directory is copied to
# /var/lib/fernsdr, with the old receiver stopped so that nothing changes
# under the copy; the original stays until the new receiver runs.
migrate_old_layout() {
    say "Moving the receiver in $INSTALL/etc over to $STATE"
    [ ! -e "$STATE" ] || die "Both $INSTALL/etc and $STATE exist, so it is not clear which receiver to keep. Move one of them away, then run this again."
    # Whether the old receiver runs is known before anything could need
    # undoing, so that an undo starts it again only if it ran.
    if systemctl is-active --quiet fernsdr.service; then OLD_RUNNING=1; fi
    MIGRATION_OPEN=1
    install -d -m 0700 "$INSTALL/old-layout"
    if [ -f "$UNITS/fernsdr.service" ]; then cp "$UNITS/fernsdr.service" "$INSTALL/old-layout/fernsdr.service"; fi
    if [ -n "$OLD_RUNNING" ]; then systemctl stop fernsdr.service; fi
    cp -a "$INSTALL/etc" "$STATE"
    make_state_directory
    # Paths into the old directory now name the new one.
    read_config
    if grep -q "$INSTALL/etc" "$WORK/config"; then
        sed "s|$INSTALL/etc/|$STATE/|g; s|$INSTALL/etc\$|$STATE|" "$WORK/config" > "$WORK/fernsdr.conf"
        write_as_receiver fernsdr.conf < "$WORK/fernsdr.conf"
        note "Paths in fernsdr.conf that named $INSTALL/etc name $STATE now"
    fi
    as_receiver chmod 0600 "$STATE/fernsdr.conf" < /dev/null || true
}

# The new receiver did not take over: the old one comes back as it was, and
# the copy is kept aside.
# Every step may fail without stopping the rest: this also runs from the
# EXIT trap, after whatever went wrong.
undo_migration() {
    MIGRATION_OPEN=""
    warn "Putting the receiver in $INSTALL/etc back"
    systemctl disable --now fernsdr-update.path fernsdr-update-boot.service > /dev/null 2>&1 || true
    systemctl stop fernsdr.service > /dev/null 2>&1 || true
    rm -f "$INSTALL/current" "$INSTALL/trusted" "$UNITS/fernsdr-update.path" "$UNITS/fernsdr-update.service" \
        "$UNITS/fernsdr-update-boot.service" || true
    if [ -f "$INSTALL/old-layout/fernsdr.service" ]; then
        cp "$INSTALL/old-layout/fernsdr.service" "$UNITS/fernsdr.service" || true
    else
        rm -f "$UNITS/fernsdr.service" || true
    fi
    systemctl daemon-reload || true
    if [ -n "$PASSWORD" ]; then rm -f "$INSTALL/admin-password" || true; fi
    if [ -e "$STATE" ]; then
        aside="$STATE.not-moved-$(date +%Y%m%d-%H%M%S)"
        if mv "$STATE" "$aside"; then note "The copy in $STATE is in $aside now"; fi
    fi
    rm -f "$INSTALL/old-layout/fernsdr.service" || true
    rmdir "$INSTALL/old-layout" 2> /dev/null || true
    if [ -n "$OLD_RUNNING" ]; then systemctl start fernsdr.service || true; fi
}

finish_migration() {
    MIGRATION_OPEN=""
    for part in bin etc share; do
        if [ -e "$INSTALL/$part" ]; then mv "$INSTALL/$part" "$INSTALL/old-layout/$part"; fi
    done
    note "The receiver before is in $INSTALL/old-layout; remove it once this one does all it did."
}

check_config() {
    if ! out=$(cd "$STATE" && as_receiver "$1/fernsdr" "$STATE/fernsdr.conf" --root "$1/web" \
        --check < /dev/null 2>&1); then
        warn "FernSDR $VERSION refuses $STATE/fernsdr.conf:"
        printf '%s\n' "$out" | printable | sed 's/^/   /' >&2
        return 1
    fi
}

# The release's units, where they differ from those in place.
install_units() {
    UNITS_CHANGED=""
    for unit in "$1"/systemd/*.service "$1"/systemd/*.path; do
        [ -f "$unit" ] || continue
        name=${unit##*/}
        if [ ! -f "$UNITS/$name" ] || [ "$(cat "$unit")" != "$(cat "$UNITS/$name")" ]; then
            install -m 0644 "$unit" "$UNITS/$name"
            UNITS_CHANGED="$UNITS_CHANGED $name"
        fi
    done
    old_systemd_dropins
    if [ -n "$UNITS_CHANGED" ]; then systemctl daemon-reload; fi
}

# systemd before 239 (Ubuntu 18.04 has 237) does not know the syscall group
# @system-service. It skips the line with a warning but still turns the
# filter on, with nothing allowed, so every unit dies at its first system
# call. There the filter is dropped; the rest of the sandbox stays. The
# drop-ins go again once systemd is new enough, after a distribution
# upgrade.
old_systemd_dropins() {
    version=$(systemctl --version 2> /dev/null | awk 'NR == 1 { print $2 }')
    case "$version" in '' | *[!0-9]*) version=0 ;; esac
    for unit in fernsdr.service fernsdr-update.service fernsdr-update-boot.service; do
        dropin=$UNITS/$unit.d/old-systemd.conf
        if [ "$version" -gt 0 ] && [ "$version" -lt 239 ]; then
            text=$(printf '# Written by install.sh: systemd %s does not know @system-service.\n[Service]\nSystemCallFilter=' "$version")
            if [ ! -f "$dropin" ] || [ "$(cat "$dropin")" != "$text" ]; then
                mkdir -p "$UNITS/$unit.d"
                printf '%s\n' "$text" > "$dropin"
                UNITS_CHANGED="$UNITS_CHANGED $unit"
            fi
        elif [ -f "$dropin" ]; then
            rm -f "$dropin"
            rmdir "$UNITS/$unit.d" 2> /dev/null || true
            UNITS_CHANGED="$UNITS_CHANGED $unit"
        fi
    done
}

# RTL-SDR dongles and RX-888s for the receiver, whose unit lets it open USB
# devices and nothing else.
install_usb_rules() {
    mkdir -p /etc/modprobe.d
    # RTL2832U dongles, the RTL-SDR Blog V3 and V4 among them, for the
    # receiver's group and nobody else. Named to sort after the
    # distribution's 60-librtlsdr*.rules, and with := so that no later rule
    # takes the group back.
    # The RX-888's FX3 controller, before its firmware is loaded (00f3) and
    # after (00f1): Cypress's own ids, which other FX3 boards share, so the
    # module loads firmware only into the device a band selects.
    cat > "$WORK/usb.rules" << RULES
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
RULES
    if [ -d /etc/udev ] || have udevadm || [ ! -f /etc/mdev.conf ]; then
        mkdir -p /etc/udev/rules.d
        install -m 0644 "$WORK/usb.rules" /etc/udev/rules.d/61-fernsdr-usb.rules
    else
        install_mdev_rules
    fi
    # The kernel's DVB-T driver claims these dongles as TV tuners. The module
    # can detach it, but keeping it away is what works every time.
    printf 'blacklist dvb_usb_rtl28xxu\n' > "$WORK/blacklist.conf"
    install -m 0644 "$WORK/blacklist.conf" /etc/modprobe.d/fernsdr-rtlsdr.conf
    if have lsmod && lsmod 2> /dev/null | grep -q '^dvb_usb_rtl28xxu'; then
        modprobe -r dvb_usb_rtl28xxu 2> /dev/null ||
            warn "The DVB-T driver has a dongle; unplug it once, or restart the machine."
    fi
    # The same for SDRplay's first RSP, the RSP1: the kernel's msi2500
    # driver takes its Mirics chip as a TV tuner, and SDRplay's service then
    # lists the RSP but cannot start it.
    printf 'blacklist sdr_msi3101
blacklist msi001
blacklist msi2500
' > "$WORK/blacklist-sdrplay.conf"
    install -m 0644 "$WORK/blacklist-sdrplay.conf" /etc/modprobe.d/fernsdr-sdrplay.conf
    if have lsmod && lsmod 2> /dev/null | grep -q '^msi2500'; then
        modprobe -r msi2500 msi001 2> /dev/null ||
            warn "The kernel's msi2500 driver has an SDRplay RSP1; unplug it once, or restart the machine."
    fi
    if have udevadm; then
        { udevadm control --reload-rules && udevadm trigger --subsystem-match=usb; } > /dev/null 2>&1 || true
    fi
}

# The machine's init: systemd, whose units the release carries, or one that
# starts `fernsdr --supervise` from a small script of the release's.
detect_init() {
    pid1=$(cat /proc/1/comm 2> /dev/null || true)
    if [ -d /run/systemd/system ] && have systemctl; then
        INIT=systemd
    elif have rc-service && have rc-update && { have openrc-run || [ -x /sbin/openrc-run ]; }; then
        INIT=openrc
    elif [ "$pid1" = runit ] || [ "$pid1" = runit-init ]; then
        INIT=runit
        for dir in /var/service /run/runit/service /etc/service; do
            if [ -d "$dir" ]; then
                SVDIR=$dir
                break
            fi
        done
        [ -n "$SVDIR" ] || die "runit runs, but none of /var/service, /run/runit/service and /etc/service is there."
        have sv || die "runit runs, but its sv command is not installed."
    elif [ -f /etc/slackware-version ] && [ -d /etc/rc.d ]; then
        INIT=slackware
        SERVICE_SCRIPT=/etc/rc.d/rc.fernsdr
    elif [ -d /etc/init.d ] && { have update-rc.d || have chkconfig || have insserv; }; then
        INIT=sysv
        SERVICE_SCRIPT=/etc/init.d/fernsdr
    else
        # No init this knows, as in a container that runs a shell: the
        # SysV script works on its own, and the operator adds one line to
        # whatever starts things at boot.
        INIT=none
        SERVICE_SCRIPT=/usr/local/sbin/fernsdr-service
    fi
}

# runit's service definitions: /etc/sv on Void, /etc/runit/sv on Artix.
runit_definition() {
    if [ -d /etc/sv ] || [ ! -d /etc/runit/sv ]; then printf '/etc/sv/fernsdr'; else printf '/etc/runit/sv/fernsdr'; fi
}

# SOURCE TARGET MODE, where TARGET differs.
install_file() {
    if [ ! -f "$2" ] || [ "$(cat "$1")" != "$(cat "$2")" ]; then
        install -m "$3" "$1" "$2"
        SERVICE_CHANGED=1
    fi
}

# FILE LINE: LINE at the end of a boot script of Slackware's, once.
add_boot_line() {
    if [ ! -f "$1" ]; then
        printf '#!/bin/sh\n' > "$1"
        chmod 0755 "$1"
    fi
    grep -qxF "$2" "$1" || printf '%s\n' "$2" >> "$1"
}

# The release's service files for this machine's init, where they differ
# from those in place. SERVICE_CHANGED says whether the receiver's did.
install_service() {
    SERVICE_CHANGED=""
    case "$INIT" in
        systemd)
            install_units "$1"
            case " $UNITS_CHANGED " in *" fernsdr.service "*) SERVICE_CHANGED=1 ;; esac
            ;;
        openrc) install_file "$1/init/openrc/fernsdr" /etc/init.d/fernsdr 0755 ;;
        runit)
            definition=$(runit_definition)
            mkdir -p "$definition"
            install_file "$1/init/runit/run" "$definition/run" 0755
            ;;
        sysv | none)
            mkdir -p "${SERVICE_SCRIPT%/*}"
            install_file "$1/init/sysv/fernsdr" "$SERVICE_SCRIPT" 0755
            ;;
        slackware)
            install_file "$1/init/sysv/fernsdr" "$SERVICE_SCRIPT" 0755
            add_boot_line /etc/rc.d/rc.local '[ -x /etc/rc.d/rc.fernsdr ] && /etc/rc.d/rc.fernsdr start'
            add_boot_line /etc/rc.d/rc.local_shutdown '[ -x /etc/rc.d/rc.fernsdr ] && /etc/rc.d/rc.fernsdr stop'
            ;;
    esac
}

# Started at boot from now on.
enable_service() {
    case "$INIT" in
        systemd)
            systemctl enable fernsdr-update-boot.service fernsdr.service fernsdr-update.path > /dev/null 2>&1 ||
                die "systemctl could not enable FernSDR's units."
            ;;
        openrc) rc-update add fernsdr default > /dev/null 2>&1 || die "rc-update could not add FernSDR." ;;
        runit)
            [ -L "$SVDIR/fernsdr" ] || ln -s "$(runit_definition)" "$SVDIR/fernsdr" ||
                die "Could not link $(runit_definition) into $SVDIR."
            # runsvdir looks every five seconds.
            i=0
            while [ "$i" -lt 15 ] && ! sv status fernsdr > /dev/null 2>&1; do
                sleep 1
                i=$((i + 1))
            done
            ;;
        sysv)
            if have update-rc.d; then
                update-rc.d fernsdr defaults > /dev/null 2>&1
            elif have chkconfig; then
                chkconfig --add fernsdr > /dev/null 2>&1
            else
                insserv fernsdr > /dev/null 2>&1
            fi || die "Could not set FernSDR to start at boot."
            ;;
        slackware | none) ;;
    esac
}

# The receiver, started, or restarted if it runs.
restart_service() {
    case "$INIT" in
        systemd)
            systemctl start fernsdr-update-boot.service fernsdr-update.path ||
                die "systemctl could not start the updater's units."
            systemctl restart fernsdr.service || true
            ;;
        openrc) rc-service fernsdr restart > /dev/null 2>&1 || true ;;
        runit) sv restart fernsdr > /dev/null 2>&1 || true ;;
        sysv | slackware | none) "$SERVICE_SCRIPT" restart > /dev/null 2>&1 || true ;;
    esac
}

# Whether the service runs: under the supervisor, the supervisor, whatever
# the receiver does.
service_active() {
    case "$INIT" in
        systemd) systemctl is-active --quiet fernsdr.service ;;
        openrc) rc-service fernsdr status > /dev/null 2>&1 ;;
        runit) sv status fernsdr 2> /dev/null | grep -q '^run:' ;;
        sysv | slackware | none) "$SERVICE_SCRIPT" status > /dev/null 2>&1 ;;
    esac
}

# How the operator restarts it, for the messages.
restart_command() {
    case "$INIT" in
        systemd) printf 'systemctl restart fernsdr' ;;
        openrc) printf 'rc-service fernsdr restart' ;;
        runit) printf 'sv restart fernsdr' ;;
        sysv | slackware | none) printf '%s restart' "$SERVICE_SCRIPT" ;;
    esac
}

service_reload_command() {
    case "$INIT" in
        systemd) printf 'systemctl reload %s' "$1" ;;
        openrc) printf 'rc-service %s reload' "$1" ;;
        runit) printf 'sv reload %s' "$1" ;;
        slackware) printf '/etc/rc.d/rc.%s reload' "$1" ;;
        *) printf '/etc/init.d/%s reload' "$1" ;;
    esac
}

log_command() {
    if [ "$INIT" = systemd ]; then printf 'journalctl -u fernsdr -f'; else printf 'tail -f /var/log/fernsdr/fernsdr.log'; fi
}

updater_log() {
    if [ "$INIT" = systemd ]; then printf 'journalctl -u fernsdr-update'; else printf '/var/log/fernsdr/fernsdr.log'; fi
}

# The same for BusyBox's mdev (Alpine without eudev), whose first matching
# line wins: after the line that loads drivers for USB interfaces, which a
# device's own event does not carry, and in place of the lines an earlier
# run wrote.
install_mdev_rules() {
    rules="# FernSDR begin: RTL-SDR dongles and RX-888s for the receiver's group (install.sh)
\$PRODUCT=bda/283[82]/[0-9a-f]+ root:$SERVICE_USER 0660
\$PRODUCT=ccd/a9/[0-9a-f]+ root:$SERVICE_USER 0660
\$PRODUCT=ccd/b3/[0-9a-f]+ root:$SERVICE_USER 0660
\$PRODUCT=ccd/d3/[0-9a-f]+ root:$SERVICE_USER 0660
\$PRODUCT=ccd/e0/[0-9a-f]+ root:$SERVICE_USER 0660
\$PRODUCT=185b/620/[0-9a-f]+ root:$SERVICE_USER 0660
\$PRODUCT=185b/650/[0-9a-f]+ root:$SERVICE_USER 0660
\$PRODUCT=1b80/d393/[0-9a-f]+ root:$SERVICE_USER 0660
\$PRODUCT=1b80/d394/[0-9a-f]+ root:$SERVICE_USER 0660
\$PRODUCT=1b80/d395/[0-9a-f]+ root:$SERVICE_USER 0660
\$PRODUCT=1b80/d39d/[0-9a-f]+ root:$SERVICE_USER 0660
\$PRODUCT=1d19/1101/[0-9a-f]+ root:$SERVICE_USER 0660
\$PRODUCT=1d19/1102/[0-9a-f]+ root:$SERVICE_USER 0660
\$PRODUCT=1d19/1103/[0-9a-f]+ root:$SERVICE_USER 0660
\$PRODUCT=1d19/1104/[0-9a-f]+ root:$SERVICE_USER 0660
\$PRODUCT=1f4d/b803/[0-9a-f]+ root:$SERVICE_USER 0660
\$PRODUCT=1f4d/c803/[0-9a-f]+ root:$SERVICE_USER 0660
\$PRODUCT=1f4d/d286/[0-9a-f]+ root:$SERVICE_USER 0660
\$PRODUCT=1f4d/d803/[0-9a-f]+ root:$SERVICE_USER 0660
\$PRODUCT=4b4/f[13]/[0-9a-f]+ root:$SERVICE_USER 0660
# FernSDR end"
    awk -v rules="$rules" '
        /^# FernSDR begin/ { skip = 1 }
        skip { if (/^# FernSDR end/) skip = 0; next }
        { print }
        !done && /^\$MODALIAS=/ { print rules; done = 1 }
        END { if (!done) print rules }
    ' /etc/mdev.conf > "$WORK/mdev.conf"
    if [ "$(cat "$WORK/mdev.conf")" != "$(cat /etc/mdev.conf)" ]; then
        cat "$WORK/mdev.conf" > /etc/mdev.conf
        note "USB devices: lines for mdev are in /etc/mdev.conf; unplug a dongle or RX-888 plugged in before once"
    fi
}

# LINK VERSION: the link pointed at releases/VERSION, replaced in one step.
point_link() {
    ln -sfn "releases/$2" "$INSTALL/.$1.new"
    mv -Tf "$INSTALL/.$1.new" "$INSTALL/$1"
}

wait_for_receiver() {
    port=$(receiver_port)
    host=$(receiver_host)
    i=0
    while [ "$i" -lt 60 ]; do
        sleep 1
        if probe "http://$host:$port/api/status" "$WORK/status.json"; then return 0; fi
        if [ "$INIT" = systemd ] && [ "$(systemctl is-active fernsdr.service 2> /dev/null || true)" = failed ]; then
            break
        fi
        i=$((i + 1))
    done
    warn "The receiver did not come up. Its log:"
    if [ "$INIT" = systemd ]; then
        journalctl -u fernsdr.service -n 25 --no-pager 2> /dev/null
    else
        tail -n 25 /var/log/fernsdr/fernsdr.log 2> /dev/null
    fi | printable | sed 's/^/   /' >&2 || true
    return 1
}

start_receiver() {
    if [ "$INIT" = systemd ]; then systemctl daemon-reload; fi
    enable_service
    restart_service
    wait_for_receiver
}

first_install() {
    ensure_user
    install -d -m 0755 "$INSTALL" "$INSTALL/releases" "$UPDATE"
    chown root:root "$INSTALL" "$INSTALL/releases" "$UPDATE"
    chmod 0755 "$INSTALL" "$INSTALL/releases" "$UPDATE"
    unpack_release
    release="$INSTALL/releases/$VERSION"
    if [ -n "$MIGRATING" ]; then
        migrate_old_layout
    else
        make_state_directory
        if [ -f "$STATE/fernsdr.conf" ]; then
            say "Keeping $STATE/fernsdr.conf"
        else
            write_config "$release"
        fi
    fi
    ensure_password "$release"
    if ! check_config "$release"; then
        if [ -n "$MIGRATING" ]; then undo_migration; fi
        die "Nothing was switched over. Once fernsdr.conf is right, run this again."
    fi
    say "Setting up the service ($INIT)"
    install_service "$release"
    install_usb_rules
    point_link current "$VERSION"
    point_link trusted "$VERSION"
    if have restorecon; then restorecon -R "$INSTALL" "$STATE" "$UPDATE" > /dev/null 2>&1 || true; fi
    if ! start_receiver; then
        if [ -n "$MIGRATING" ]; then
            undo_migration
            die "FernSDR $VERSION did not start with the configuration moved over; the receiver before runs again."
        fi
        die "FernSDR $VERSION is installed but does not run. Once the cause is fixed: $(restart_command)"
    fi
    if [ -n "$MIGRATING" ]; then finish_migration; fi
    INSTALLED=$VERSION
    setup_https
    summary
}

# A field of the updater's status.json: the text of a string, or a number.
status_field() {
    [ -f "$UPDATE/status.json" ] || return 0
    awk -v key="\"$1\":" '{
        at = index($0, key); if (!at) next
        rest = substr($0, at + length(key)); out = ""
        if (substr(rest, 1, 1) != "\"") { match(rest, /^[0-9]+/); print substr(rest, 1, RLENGTH); exit }
        rest = substr(rest, 2)
        while (rest != "") {
            c = substr(rest, 1, 1); rest = substr(rest, 2)
            if (c == "\"") break
            if (c == "\\") {
                c = substr(rest, 1, 1); rest = substr(rest, 2)
                if (c == "n" || c == "r" || c == "t") c = " "
                else if (c == "u") { rest = substr(rest, 5); c = "?" }
            }
            out = out c
        }
        print out; exit
    }' "$UPDATE/status.json"
}

# The update, asked for the way the admin panel asks: the bands running now,
# which the new version has to bring back before it is kept, then the
# version, which wakes the updater. It runs as root, with the program of the
# version running now: as a unit of its own under systemd, and otherwise
# started by the supervisor, which looks for the request every second.
# JOB BEFORE STARTED: whether the update still runs. Under systemd, while
# the unit's job does; under the supervisor, until status.json has an
# outcome newer than BEFORE, for at most the updater's 40 minutes, and for
# a minute when the request is not taken at all.
updating() {
    if [ -n "$1" ]; then
        kill -0 "$1" 2> /dev/null
        return
    fi
    now=$(date +%s)
    if [ "$(status_field time)" != "$2" ]; then
        case "$(status_field state)" in updated | rolled-back | refused | failed) return 1 ;; esac
    elif [ -e "$STATE/update-request" ] && [ $((now - $3)) -gt 60 ]; then
        return 1
    fi
    [ $((now - $3)) -lt 2700 ]
}

run_update() {
    say "Updating FernSDR $INSTALLED to $VERSION"
    release_notes
    bands=""
    if probe "http://$(receiver_host):$(receiver_port)/metrics" "$WORK/metrics"; then
        bands=$(awk -F'"' '$1 == "fernsdr_band_running{band=" && $3 == "} 1" { print $2 }' "$WORK/metrics")
    fi
    before=$(status_field time)
    if [ "$INIT" = systemd ]; then
        systemctl start fernsdr-update.path
    elif ! service_active; then
        restart_service
    fi
    if [ -n "$bands" ]; then printf '%s\n' "$bands"; fi | write_as_receiver update-bands
    printf '%s\n' "$VERSION" | write_as_receiver update-request
    job=""
    if [ "$INIT" = systemd ]; then
        systemctl start fernsdr-update.service &
        job=$!
    fi
    shown=""
    started=$(date +%s)
    while updating "$job" "$before" "$started"; do
        if [ "$(status_field time)" != "$before" ]; then
            message=$(status_field message | printable)
            if [ -n "$message" ] && [ "$message" != "$shown" ]; then
                note "$message"
                shown=$message
            fi
        fi
        sleep 2
    done
    if [ -n "$job" ]; then wait "$job" || true; fi
    [ "$(status_field time)" != "$before" ] ||
        die "The updater did not take the request; $(updater_log) says why."
    state=$(status_field state)
    message=$(status_field message | printable)
    if [ "$state" != updated ]; then
        warn "$message"
        die "FernSDR $INSTALLED runs as before; $(updater_log) has the details."
    fi
    [ "$message" = "$shown" ] || note "$message"
    INSTALLED=$VERSION
}

update_install() {
    current=$(readlink "$INSTALL/current" 2> /dev/null || true)
    if [ "$current" != "releases/$INSTALLED" ] || [ -e "$UPDATE/trial" ] ||
        { [ "$INIT" = systemd ] && systemctl is-active --quiet fernsdr-update.service; }; then
        die "An update is under way. The admin panel's Updates page shows how it goes; run this again once it is done."
    fi
    make_state_directory
    if [ ! -f "$STATE/fernsdr.conf" ]; then write_config "$INSTALL/trusted"; fi
    ensure_password "$INSTALL/trusted"
    if newer "$VERSION" "$INSTALLED"; then
        run_update
    elif [ "$VERSION" = "$INSTALLED" ]; then
        say "FernSDR $INSTALLED is installed, the newest release"
    else
        say "FernSDR $INSTALLED is installed, newer than the release published, $VERSION"
    fi
    install_service "$INSTALL/trusted"
    install_usb_rules
    if [ -n "$SERVICE_CHANGED" ] || [ -n "$CONFIG_WRITTEN" ] || ! service_active; then
        start_receiver || die "FernSDR $INSTALLED does not run. Once the cause is fixed: $(restart_command)"
    else
        enable_service
        if [ "$INIT" = systemd ]; then systemctl start fernsdr-update.path || true; fi
    fi
    summary
}

# A package from the distribution's own repositories.
install_package() {
    if have apt-get; then
        DEBIAN_FRONTEND=noninteractive apt-get install -y -q "$1" ||
            { apt-get update -q && DEBIAN_FRONTEND=noninteractive apt-get install -y -q "$1"; }
    elif have dnf; then
        dnf install -y -q "$1"
    elif have zypper; then
        zypper --non-interactive --quiet install "$1"
    elif have pacman; then
        pacman -S --noconfirm --needed "$1"
    else
        return 1
    fi
}

# Caddy's configuration with this receiver's site in it, between two lines
# that a later run finds and replaces; whatever else the file serves stays.
# The file as it was is kept beside it the first time. Caddy checks the new
# one before it is put in place.
write_caddyfile() {
    file=/etc/caddy/Caddyfile
    {
        # The package's example serves a welcome page on port 80 for every
        # name, in the way of the certificate's challenge: it goes.
        if [ -f "$file" ] && ! grep -q 'root \* /usr/share/caddy' "$file"; then
            awk '/^# FernSDR begin/ { skip = 1 } !skip { print } /^# FernSDR end/ { skip = 0 }' "$file"
            printf '\n'
        fi
        printf '# FernSDR begin: written by its installer, which rewrites what is between\n'
        printf '# these lines when it runs again.\n'
        printf '%s {\n\tencode zstd gzip\n\treverse_proxy 127.0.0.1:%s {\n' "$DOMAIN" "$(receiver_port)"
        printf '\t\t# The audio is live: nothing in between may hold it back.\n'
        printf '\t\tflush_interval -1\n\t\theader_up X-Real-IP {remote_host}\n\t}\n}\n'
        printf '# FernSDR end\n'
    } > "$WORK/Caddyfile"
    if ! caddy validate --config "$WORK/Caddyfile" --adapter caddyfile > "$WORK/caddy.log" 2>&1; then
        warn "Caddy refused the configuration for $DOMAIN:"
        tail -n 3 "$WORK/caddy.log" | printable >&2
        return 1
    fi
    if [ -f "$file" ] && [ ! -f "$file.before-fernsdr" ]; then cp -p "$file" "$file.before-fernsdr"; fi
    install -D -m 0644 "$WORK/Caddyfile" "$file"
}

# HTTPS for a receiver on a server with a domain: Caddy from the
# distribution's packages serves the domain, with a certificate it gets and
# renews by itself, and passes everything to the receiver on this machine.
# Only under systemd, which runs Caddy as its package expects; elsewhere, or
# without a Caddy package, the summary says how to do it by hand.
setup_https() {
    [ "$WHERE" = internet ] && [ -n "$DOMAIN" ] && [ "$INIT" = systemd ] || return 0
    if ! have caddy; then
        say "Installing Caddy, for HTTPS"
        if ! install_package caddy > "$WORK/caddy.log" 2>&1 || ! have caddy; then
            warn "This distribution's packages have no Caddy; the summary says how to set up HTTPS by hand."
            return 0
        fi
    fi
    say "Setting up HTTPS for $DOMAIN"
    write_caddyfile || return 0
    systemctl enable caddy > /dev/null 2>&1 || true
    if systemctl reload-or-restart caddy > "$WORK/caddy.log" 2>&1; then
        HTTPS=1
    else
        warn "Caddy did not start; journalctl -u caddy says why."
    fi
}

# Run again on a receiver already here, with someone at the terminal: to
# update it, or for a new password when the old one is lost.
ask_rerun() {
    ACTION=update
    if [ -n "${FERNSDR_NEW_PASSWORD:-}" ]; then
        ACTION=password
        return 0
    fi
    [ -z "${FERNSDR_SETUP:-}" ] && (exec < /dev/tty) 2> /dev/null || return 0
    {
        printf '\nFernSDR %s is installed. What now?\n\n' "$INSTALLED"
        printf '  1  Update it to the newest release\n'
        printf '  2  A new password for the admin panel, when the old one is lost\n\n'
        printf 'Answer [1]: '
    } > /dev/tty
    IFS= read -r answer < /dev/tty || answer=1
    case "$answer" in 2) ACTION=password ;; esac
}

# A new admin password, in place of the one in the configuration; the
# receiver restarts to take it.
new_password() {
    read_config
    new_admin_password "$INSTALL/trusted"
    awk -v hash="$HASH" '
        /^[ \t]*\[/ { section = $0; gsub(/[][ \t]/, "", section) }
        section == "admin" && /^[ \t]*password_hash[ \t]*=/ && !done { print "password_hash = " hash; done = 1; next }
        { print }' "$WORK/config" > "$WORK/fernsdr.conf"
    write_as_receiver fernsdr.conf < "$WORK/fernsdr.conf"
    ensure_password "$INSTALL/trusted"
    restart_service
    printf '\n'
    say "The admin panel has a new password"
    if [ -t 1 ]; then note "Password:      $PASSWORD"; fi
    note "It is also in $INSTALL/admin-password, for root only."
    printf '\n'
}

# --- Docker -------------------------------------------------------------------
#
# The receiver in a container instead of as a service: the image FernSDR
# publishes, its configuration and modules in a volume, restarted by Docker,
# and the host's USB bus bound in, so that a radio plugged in later appears
# without the container starting again. The device rules and the kernel
# drivers kept away are the host's, as for a service; the container takes the
# host's group for the radios. SDRplay's API needs glibc and a service of
# its own, which the image does not have.

# A receiver this installer runs in Docker: the container it made, by label.
docker_managed() {
    have docker || return 1
    [ -n "$(docker inspect -f '{{index .Config.Labels "org.fernsdr.setup"}}' "$CONTAINER" 2> /dev/null)" ]
}

# Docker when the operator asks for it: FERNSDR_DOCKER=1, or the answer to
# a question asked only where Docker runs and nothing is installed yet.
want_docker() {
    [ ! -e "$INSTALL/trusted" ] && [ ! -f "$STATE/fernsdr.conf" ] && [ -z "$MIGRATING" ] || return 1
    if ! have docker || ! docker info > /dev/null 2>&1; then
        [ -z "${FERNSDR_DOCKER:-}" ] || die "FERNSDR_DOCKER asks for Docker, which does not run here."
        return 1
    fi
    [ -z "${FERNSDR_DOCKER:-}" ] || return 0
    [ -z "${FERNSDR_SETUP:-}" ] && (exec < /dev/tty) 2> /dev/null || return 1
    {
        printf '\nDocker runs on this machine. How should FernSDR run?\n\n'
        printf '  1  As a service of this machine: updates from the admin panel\n'
        printf '  2  In a Docker container: updates by running this installer again;\n'
        printf '     not for SDRplay radios\n\n'
        printf 'Answer [1]: '
    } > /dev/tty
    IFS= read -r answer < /dev/tty || answer=1
    [ "$answer" = 2 ]
}

docker_label() { docker inspect -f "{{index .Config.Labels \"org.fernsdr.$1\"}}" "$CONTAINER" 2> /dev/null; }

# SETUP PLAIN_ADMIN: the container, from $IMAGE.
docker_run() {
    setup=$1 plain=$2
    case "$setup" in internet) ports=127.0.0.1:8073:8073 ;; *) ports=8073:8073 ;; esac
    gid=$(getent group "$SERVICE_USER" | cut -d: -f3)
    set -- -d --name "$CONTAINER" --restart unless-stopped \
        --label "org.fernsdr.setup=$setup" --label "org.fernsdr.plain_admin=$plain" \
        -p "$ports" -v fernsdr:/var/lib/fernsdr \
        -e "FERNSDR_SETUP=$setup" -e "FERNSDR_PLAIN_ADMIN=$plain" -e "FERNSDR_USB_GID=$gid" -e FERNSDR_SUPERVISED=1
    # USB character devices, major 189, may be opened as they come and go.
    if [ -d /dev/bus/usb ]; then set -- "$@" -v /dev/bus/usb:/dev/bus/usb --device-cgroup-rule 'c 189:* rmw'; fi
    docker run "$@" "$IMAGE" > /dev/null
}

# The image, fetched; one already here does when fetching fails, as on a
# machine without internet access.
docker_pull() {
    docker pull -q "$IMAGE" > "$WORK/pull.log" 2>&1 && return 0
    docker image inspect "$IMAGE" > /dev/null 2>&1 || return 1
    warn "Could not fetch $IMAGE; going on with the one already here."
}

# Whether the container's receiver answers, within a minute.
docker_wait() {
    for i in $(seq 1 60); do
        if have curl; then
            curl -fsS -o /dev/null --max-time 2 http://127.0.0.1:8073/api/status 2> /dev/null && return 0
        else
            wget -q -O /dev/null -T 2 http://127.0.0.1:8073/api/status 2> /dev/null && return 0
        fi
        sleep 1
    done
    return 1
}

docker_first() {
    ask_where
    ensure_user
    install_usb_rules
    say "Fetching $IMAGE"
    docker_pull || die "Could not fetch $IMAGE."
    docker volume create fernsdr > /dev/null
    docker_run "$WHERE" "$PLAIN_ADMIN" || die "Docker could not start the container."
    docker_wait || die "The container does not answer; docker logs $CONTAINER says why."
    PASSWORD=$(docker exec "$CONTAINER" cat /var/lib/fernsdr/admin-password 2> /dev/null || true)
    setup_https
    docker_summary
}

# A newer image, if there is one: the container made again from it, with the
# volume as it was. One that does not answer gives way to the image before.
docker_update() {
    WHERE=$(docker_label setup)
    PLAIN_ADMIN=$(docker_label plain_admin)
    before=$(docker inspect -f '{{.Image}}' "$CONTAINER")
    # The image the container runs keeps a name of its own, given before the
    # new one takes its tag: an image without a name is dropped once its
    # container is gone, and cannot be named again by its id.
    previous=${IMAGE%:*}:previous
    if [ "$(docker image inspect -f '{{.Id}}' "$IMAGE" 2> /dev/null)" = "$before" ]; then
        docker tag "$IMAGE" "$previous"
    elif ! docker tag "$before" "$previous" 2> /dev/null; then
        previous=""
    fi
    say "Looking for a newer image"
    docker_pull || die "Could not fetch $IMAGE."
    if [ "$(docker image inspect -f '{{.Id}}' "$IMAGE")" = "$before" ]; then
        say "The container runs the newest image"
        docker start "$CONTAINER" > /dev/null 2>&1 || true
    else
        say "Starting the container again from the new image"
        docker rm -f "$CONTAINER" > /dev/null
        if ! docker_run "$WHERE" "$PLAIN_ADMIN" || ! docker_wait; then
            docker rm -f "$CONTAINER" > /dev/null 2>&1 || true
            [ -n "$previous" ] ||
                die "The new image does not start, and the one before has no name to go back to; docker logs $CONTAINER says why."
            IMAGE=$previous
            docker_run "$WHERE" "$PLAIN_ADMIN" && docker_wait ||
                die "Neither the new image nor the one before starts; docker logs $CONTAINER says why."
            die "The new image did not start, so the container runs the one before again."
        fi
    fi
    docker_summary
}

# A new admin password in the container's configuration; the receiver
# restarts to take it.
docker_password() {
    make_password
    printf '%s\n' "$PASSWORD" | docker exec -i "$CONTAINER" /opt/fernsdr/fernsdr --set-password /var/lib/fernsdr/fernsdr.conf \
        > /dev/null 2>&1 || die "The container did not take the new password."
    printf '%s\n' "$PASSWORD" | docker exec -i "$CONTAINER" sh -c \
        'umask 077 && cat > /var/lib/fernsdr/admin-password && chown fernsdr:fernsdr /var/lib/fernsdr/admin-password'
    docker restart "$CONTAINER" > /dev/null
    docker_wait || warn "The container does not answer yet; docker logs $CONTAINER says why."
    printf '\n'
    say "The admin panel has a new password"
    if [ -t 1 ]; then note "Password:      $PASSWORD"; fi
    note "It is also in the volume, as admin-password."
    printf '\n'
}

docker_main() {
    if docker_managed; then
        INSTALLED=$(docker inspect -f '{{index .Config.Labels "org.opencontainers.image.version"}}' "$CONTAINER" 2> /dev/null || true)
        [ -n "$INSTALLED" ] || INSTALLED="in Docker"
        ask_rerun
        if [ "$ACTION" = password ]; then docker_password; else docker_update; fi
    else
        docker_first
    fi
}

docker_summary() {
    address=$(this_address)
    printf '\nFernSDR runs in Docker, in the container %s.\n\n' "$CONTAINER"
    if [ "$WHERE" = internet ]; then
        site=${DOMAIN:-radio.example.org}
        note "Listeners:     https://$site"
        note "Admin panel:   https://$site/admin"
        [ -n "$HTTPS" ] || note "               once a web server in front serves it over HTTPS, as the guide shows"
    else
        note "Listeners:     http://$address:8073"
        if [ -n "$PLAIN_ADMIN" ]; then
            note "Admin panel:   http://$address:8073/admin, over plain HTTP, at your own risk"
        elif [ "$WHERE" = http ]; then
            note "Admin panel:   through an SSH tunnel: ssh -L 8073:localhost:8073 you@$address,"
            note "               then http://localhost:8073/admin"
        else
            note "Admin panel:   http://$address:8073/admin, from the home network"
        fi
    fi
    if [ -n "$PASSWORD" ] && [ -t 1 ]; then
        note "Password:      $PASSWORD"
        note "               also in the volume, as admin-password"
    elif [ -n "$PASSWORD" ]; then
        note "Password:      in the volume, as admin-password"
    fi
    printf '\n'
    note "Settings:      the volume fernsdr, kept when the container is made again"
    note "Log:           docker logs $CONTAINER"
    note "Updates:       this installer again, which fetches the newest image"
    note "Lost password: this installer again, answer 2"
    printf '\n'
}

firewall_hint() {
    if have firewall-cmd && firewall-cmd --state > /dev/null 2>&1; then
        note "firewalld is on. For other machines to reach the receiver:"
        note "  firewall-cmd --permanent --add-port=$1/tcp && firewall-cmd --reload"
    elif have ufw && ufw status 2> /dev/null | grep -q '^Status: active'; then
        note "ufw is on. For other machines to reach the receiver: ufw allow $1/tcp"
    fi
}

summary() {
    port=$(receiver_port)
    bind=$(receiver_bind)
    case "$bind" in
        127.* | localhost | ::1 | '[::1]')
            site=${DOMAIN:-radio.example.org}
            if [ -n "$HTTPS" ]; then
                printf '\nFernSDR %s runs, served over HTTPS by Caddy. Caddy gets the certificate\n' "$INSTALLED"
                printf 'once %s points at this machine and ports 80 and 443 are open.\n\n' "$DOMAIN"
                note "Listeners:     https://$site"
                note "Admin panel:   https://$site/admin"
            else
            printf '\nFernSDR %s runs, on this machine only, for a web server in front of it\n' "$INSTALLED"
            printf 'to serve over HTTPS. With Caddy (https://caddyserver.com/docs/install),\n'
            if [ -n "$DOMAIN" ]; then
                printf 'these lines in /etc/caddy/Caddyfile do that, certificate included, once\n'
                printf 'the domain points at this machine; then: %s\n' "$(service_reload_command caddy)"
            else
                printf 'these lines in /etc/caddy/Caddyfile do that, certificate included, with\n'
                printf 'the receiver'"'"'s domain for radio.example.org; then: %s\n' "$(service_reload_command caddy)"
            fi
            printf '\n%s {\n\treverse_proxy 127.0.0.1:%s\n}\n\n' "$site" "$port"
            note "Listeners:     https://$site"
            note "Admin panel:   https://$site/admin"
            fi
            ;;
        *)
            address=$(this_address)
            case "$bind" in 0.0.0.0 | :: | '[::]') ;; *) address=$bind ;; esac
            printf '\nFernSDR %s runs.\n\n' "$INSTALLED"
            note "Listeners:     http://$address:$port"
            if [ "$(config_value admin plain_http_anywhere no)" = yes ]; then
                note "Admin panel:   http://$address:$port/admin, over plain HTTP, at your own risk"
            elif [ "$(config_value admin home_network no)" = yes ]; then
                note "Admin panel:   http://$address:$port/admin, from the home network"
            elif [ "$WHERE" = http ]; then
                note "Admin panel:   through an SSH tunnel: ssh -L $port:localhost:$port you@$address,"
                note "               then http://localhost:$port/admin"
            else
                note "Admin panel:   http://127.0.0.1:$port/admin on this machine, or over HTTPS"
                note "               through a web server in front of it; from the home network"
                note "               over plain HTTP once [admin] says home_network = yes"
            fi
            ;;
    esac
    # Not into a log: without a terminal, as from a provisioning script, the
    # password stays in its file.
    if [ -n "$PASSWORD" ] && [ -t 1 ]; then
        note "Password:      $PASSWORD"
        note "               also in $INSTALL/admin-password, for root only"
    elif [ -n "$PASSWORD" ]; then
        note "Password:      in $INSTALL/admin-password, for root only"
    fi
    printf '\n'
    note "Configuration: $STATE/fernsdr.conf"
    note "Log:           $(log_command)"
    note "Updates:       the admin panel's Updates page, or this installer again"
    note "Lost password: run this installer again and answer 2"
    if [ "$INIT" = none ]; then
        printf '\n'
        warn "Nothing starts FernSDR when this machine starts: its init is none of systemd,"
        warn "OpenRC, runit and SysV init. Add this line to what the machine runs at boot:"
        note "$SERVICE_SCRIPT start"
    fi
    case "$bind" in 127.* | localhost | ::1 | '[::1]') ;; *) firewall_hint "$port" ;; esac
    if [ -n "$CONFIG_WRITTEN" ]; then
        printf '\n'
        note "It receives a synthetic band for now, so the whole chain can be tried in"
        note "a browser before an antenna is connected. With an RTL-SDR dongle plugged"
        note "in, the admin panel's Modules page sets it up."
    fi
    printf '\n'
}

main() {
    [ $# -eq 0 ] || die "install.sh takes no arguments. FERNSDR_SETUP answers its question where there is no terminal."
    umask 022
    cd /
    [ "$(id -u)" = 0 ] || die "It installs a system service, so it runs as root: curl -fsSL .../install.sh | sudo sh"
    case "$RELEASE_KEYS" in
        *@*) die "This is install.sh as the source has it, without the keys a release build puts in. Use the one published with a release, or build from source with tools/source-install.sh." ;;
    esac
    case "$RELEASE_URL" in https://*) ;; *) die "FERNSDR_RELEASE_URL has to be an https:// address." ;; esac
    RELEASE_URL=${RELEASE_URL%/}
    detect_init
    have curl || have wget || die "It downloads with curl or wget, and neither is installed."
    have sha256sum || have shasum || have openssl || die "It needs sha256sum to check the download."
    for tool in awk sed tar od df install timeout getent; do
        have "$tool" || die "It needs $tool, which is not installed."
    done
    have runuser || have su || die "It needs runuser or su, and neither is installed."
    have useradd || have adduser || die "It needs useradd or adduser, and neither is installed."
    have groupadd || have addgroup || die "It needs groupadd or addgroup, and neither is installed."
    PLATFORM=$(platform) || exit 1

    WORK=$(mktemp -d) || die "Could not make a temporary directory."
    trap cleanup EXIT
    trap 'exit 130' INT
    trap 'exit 143' HUP TERM

    find_installed
    if docker_managed || want_docker; then
        docker_main
        exit 0
    fi
    if [ -n "$MIGRATING" ] && [ "$INIT" != systemd ]; then
        die "$INSTALL/etc holds a receiver that tools/source-install.sh set up, which only systemd runs. Move $INSTALL/etc/fernsdr.conf to $STATE/fernsdr.conf by hand if it is to be kept, move $INSTALL away, then run this again."
    fi
    if [ -z "$MIGRATING" ] && [ ! -f "$STATE/fernsdr.conf" ]; then ask_where; fi
    if [ -n "$INSTALLED" ]; then
        ask_rerun
        if [ "$ACTION" = password ]; then
            new_password
            exit 0
        fi
    fi
    read_release
    if [ -n "$INSTALLED" ]; then
        update_install
    else
        first_install
    fi
}

main "$@"
