#!/bin/sh
# Starts FernSDR in its image. It begins as root to make the volume and any
# USB devices the container was given the receiver's, and to start
# `fernsdr --supervise`, which runs the receiver as `fernsdr` and carries out
# the admin panel's updates inside the container, as it does on a machine
# without systemd. The configuration of a first start is written as the
# receiver.
#
# FERNSDR_SETUP=internet leaves the admin panel to HTTPS through a web
# server in front, as install.sh does for a server on the internet; http
# serves a server without a domain, the admin panel through an SSH tunnel;
# the default, home, lets the home network in over plain HTTP.
#
# FERNSDR_USB_GID is the host's group for SDR devices, as install.sh sets it
# up with a udev rule. With /dev/bus/usb bound in from the host, a radio
# plugged in later appears with that group, and the receiver, a member of
# it here too, opens it without the container starting again.
set -eu

STATE=/var/lib/fernsdr
CONFIG=$STATE/fernsdr.conf
# The image's own program: it checks what the volume keeps with the release
# keys built into it, and it supervises.
PROGRAM=/opt/fernsdr/image/fernsdr
WEB=/opt/fernsdr/image/web

if [ "$(id -u)" != 0 ]; then
    # Started with --user: nothing here can change owners, so the volume has
    # to be that user's already, with a configuration in it.
    [ -f "$CONFIG" ] || { echo "fernsdr: no $CONFIG; start the container once without --user to make one" >&2; exit 1; }
    # Without root there is no updater either: the Updates page says to pull
    # the new image.
    exec "$PROGRAM" "$CONFIG" --root "$WEB" "$@"
fi

# A bind mount arrives owned by whoever made it on the host. The updater's
# directory stays root's: the receiver must not be able to change it.
if [ "$(stat -c %U "$STATE")" != fernsdr ]; then
    find "$STATE" -path "$STATE/update" -prune -o -exec chown -h fernsdr:fernsdr {} +
fi
# The top directory's group is the receiver's group, whatever the host left.
chown fernsdr:fernsdr "$STATE"
chmod 0700 "$STATE"

# Devices given with --device /dev/bus/usb/... keep the host's owner inside
# the container, usually root alone. These nodes are the container's own, so
# giving them to the receiver's group changes nothing on the host.
if [ -n "${FERNSDR_USB_GID:-}" ]; then
    case "$FERNSDR_USB_GID" in *[!0-9]* | '') echo "fernsdr: FERNSDR_USB_GID is a group number, not $FERNSDR_USB_GID" >&2; exit 1 ;; esac
    group=$(awk -F: -v gid="$FERNSDR_USB_GID" '$3 == gid { print $1 }' /etc/group)
    if [ -z "$group" ]; then
        addgroup -g "$FERNSDR_USB_GID" hostusb
        group=hostusb
    fi
    addgroup fernsdr "$group" 2> /dev/null || true
elif [ -d /dev/bus/usb ]; then
    find /dev/bus/usb -type c -exec chgrp fernsdr {} + -exec chmod 0660 {} +
fi

if [ ! -f "$CONFIG" ]; then
    case "${FERNSDR_SETUP:-home}" in
        home | internet | http) ;;
        *) echo "fernsdr: FERNSDR_SETUP is home, internet or http, not ${FERNSDR_SETUP}" >&2; exit 1 ;;
    esac
    password=$(head -c 32 /dev/urandom | base64 | tr -dc 'A-Za-z0-9' | head -c 20)
    hash=$(printf '%s\n' "$password" | "$PROGRAM" --hash-password 2> /dev/null | sed -n 's/^password_hash = //p')
    [ -n "$hash" ] || { echo "fernsdr: could not make the admin password" >&2; exit 1; }
    # Behind a web server on the host, which reaches the container through
    # a port published on the host's 127.0.0.1 only: Docker hands those
    # connections over from the bridge's gateway, not from loopback, so the
    # gateway is the proxy to believe. Nothing else arrives from it there.
    proxies=loopback
    if [ "${FERNSDR_SETUP:-home}" = internet ]; then
        gateway=$(ip route 2> /dev/null | awk '/^default/ { print $3; exit }')
        case "$gateway" in
            *[!0-9.]* | '') ;;
            *) proxies="loopback, $gateway" ;;
        esac
    fi
    config=$(
        awk -v proxies="$proxies" '
            /^[ \t]*\[/ { section = $0; gsub(/[][ \t]/, "", section) }
            section == "server" && /^[ \t]*trusted_proxies[ \t]*=/ { print "trusted_proxies = " proxies; next }
            section == "server" && /^[ \t]*bind[ \t]*=/ { print "bind = 0.0.0.0"; next }
            section == "server" && /^[ \t]*root[ \t]*=/ { print "root = /opt/fernsdr/web"; next }
            { print }' /opt/fernsdr/image/fernsdr.example.conf
        printf '\n[admin]\n'
        printf '# The password is in admin-password beside this file. Change it on the\n'
        printf "# admin panel's Station page, or with install.sh run again.\n"
        printf 'password_hash = %s\n' "$hash"
        if [ "${FERNSDR_SETUP:-home}" = home ]; then
            printf '# The admin panel over plain HTTP from the home network (docs/DEPLOYMENT.md).\n'
            printf 'home_network = yes\n'
        fi
        if [ -n "${FERNSDR_PLAIN_ADMIN:-}" ]; then
            printf '# The admin panel over plain HTTP from anywhere, at your own risk: anyone\n'
            printf '# on the way can read it and catch the password (docs/DEPLOYMENT.md).\n'
            printf 'plain_http_anywhere = yes\n'
        fi
    )
    # Written as the receiver, not as root: the receiver owns the volume and
    # could have left a link to /usr/local/bin/fernsdr-entrypoint where
    # either file goes, and root would write through it and hand it over.
    printf '%s\n' "$password" | su-exec fernsdr sh -c 'umask 077 && cat > "$1" && chmod 0600 "$1"' sh "$STATE/admin-password"
    printf '%s\n' "$config" | su-exec fernsdr sh -c 'umask 077 && cat > "$1.new" && chmod 0600 "$1.new" && mv -f "$1.new" "$1"' sh "$CONFIG"
    echo "FernSDR: a configuration was made in the volume, with a synthetic band to try it."
    echo "FernSDR: admin password: $password (also in the volume as admin-password)"
fi

# The release to run, the image's or a newer one the volume keeps, checked
# against its signature, in /opt/fernsdr; then the supervisor. Where that
# cannot be set up, as in a container started with --read-only, the
# receiver runs as before, without updates from the admin panel.
if "$PROGRAM" --prepare-container; then
    exec "$PROGRAM" --supervise -- "$@"
fi
echo "FernSDR: the receiver runs without updates from the admin panel; pull a new image to update it." >&2
exec su-exec fernsdr "$PROGRAM" "$CONFIG" --root "$WEB" "$@"
