#!/bin/sh
# The container image end to end: built by `make -C server docker`, started
# the way docs/DEPLOYMENT.md says, and looked at from outside.
#
#   tools/docker-lab.sh [IMAGE]          default: fernsdr:<version of this checkout>
#
# Checks that on a first start with an empty volume it writes a
# configuration and a password and serves; that the receiver runs as
# `fernsdr` under tini with FERNSDR_CONTAINER set; that the admin panel takes
# plain HTTP from the machine it runs on in the default home setup and not
# with FERNSDR_SETUP=internet; that a host directory owned by root works as
# the volume; that a USB device given with --device reaches the receiver's
# group; that a restart keeps configuration and password; that `docker
# stop` ends it cleanly; and that a first start writes nothing through
# links the receiver left in the volume. Changes nothing but containers and
# volumes named fernsdr-docker-lab-*, removed at the end.
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
VERSION=$(sed -n 's/.*kVersion = "\(.*\)";/\1/p' "$REPO/server/src/version.h")
IMAGE=${1:-fernsdr:$VERSION}
PREFIX=fernsdr-docker-lab
PORT=${LAB_PORT:-18260}
WORK=$(mktemp -d)
FAILURES=0
pass() { printf 'PASS  %s\n' "$*"; }
fail() { printf 'FAIL  %s\n' "$*"; FAILURES=$((FAILURES + 1)); }
cleanup() {
    docker rm -f "$PREFIX-a" "$PREFIX-b" "$PREFIX-c" "$PREFIX-d" > /dev/null 2>&1
    docker volume rm "$PREFIX-state" "$PREFIX-linked" > /dev/null 2>&1
    rm -rf "$WORK"
}
trap cleanup EXIT
cleanup
WORK=$(mktemp -d)
docker image inspect "$IMAGE" > /dev/null 2>&1 || { echo "no image $IMAGE: make -C server docker first"; exit 1; }

up() {
    for i in $(seq 1 30); do
        [ "$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$1/api/status")" = 200 ] && return 0
        sleep 1
    done
    return 1
}

# 1. A named volume, the default setup.
docker run -d --name "$PREFIX-a" -p "127.0.0.1:$PORT:8073" -v "$PREFIX-state:/var/lib/fernsdr" \
    --device /dev/null:/dev/bus/usb/001/002 "$IMAGE" > /dev/null
up "$PORT" && pass "a first start with an empty volume serves" || fail "it does not serve: $(docker logs "$PREFIX-a" 2>&1 | tail -3)"
password=$(docker logs "$PREFIX-a" 2>&1 | sed -n 's/^FernSDR: admin password: \([A-Za-z0-9]*\) .*/\1/p')
stored=$(docker exec "$PREFIX-a" cat /var/lib/fernsdr/admin-password 2> /dev/null)
[ -n "$password" ] && [ "$password" = "$stored" ] && pass "the password is in the log once and in the volume" ||
    fail "password: log '$password', volume '$stored'"
modes=$(docker exec "$PREFIX-a" stat -c '%U %a' /var/lib/fernsdr /var/lib/fernsdr/fernsdr.conf /var/lib/fernsdr/admin-password | tr '\n' ',')
[ "$modes" = "fernsdr 700,fernsdr 600,fernsdr 600," ] && pass "owners and modes in the volume" || fail "owners and modes: $modes"
tree=$(docker exec "$PREFIX-a" ps -o pid,user,comm | awk 'NR > 1 { print $1 ":" $2 ":" $3 }' | tr '\n' ' ')
case "$tree" in
    "1:root:tini "*fernsdr:fernsdr*) pass "tini is first, the receiver runs as fernsdr" ;;
    *) fail "processes: $tree" ;;
esac
# The receiver's own /proc/PID/environ is closed even to root in the
# container: it is not dumpable. The image's environment is what su-exec
# passes on.
docker inspect -f '{{range .Config.Env}}{{println .}}{{end}}' "$IMAGE" | grep -qx 'FERNSDR_CONTAINER=1' &&
    pass "the image tells the receiver it is in a container, for the Updates page" || fail "no FERNSDR_CONTAINER in the image"
[ "$(docker exec "$PREFIX-a" stat -c '%G %a' /dev/bus/usb/001/002)" = "fernsdr 660" ] &&
    pass "a device given with --device is the receiver's group's" ||
    fail "the device: $(docker exec "$PREFIX-a" stat -c '%G %a' /dev/bus/usb/001/002)"
code=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/api/admin/challenge")
[ "$code" = 405 ] && pass "the admin panel takes plain HTTP from this machine (home)" || fail "admin from this machine: $code"
docker restart "$PREFIX-a" > /dev/null
up "$PORT" && [ "$(docker exec "$PREFIX-a" cat /var/lib/fernsdr/admin-password)" = "$password" ] &&
    [ "$(docker logs "$PREFIX-a" 2>&1 | grep -c 'admin password')" = 1 ] &&
    pass "a restart keeps the configuration and the password" || fail "after a restart"
started=$(date +%s)
docker stop "$PREFIX-a" > /dev/null
status=$(docker inspect -f '{{.State.ExitCode}}' "$PREFIX-a")
[ "$status" = 0 ] && [ $(($(date +%s) - started)) -lt 10 ] && pass "docker stop ends it cleanly in $(($(date +%s) - started)) s" ||
    fail "docker stop: exit $status after $(($(date +%s) - started)) s"

# 2. A host directory of root's, on a server on the internet.
mkdir -p "$WORK/state"
chmod 0755 "$WORK/state"
docker run -d --name "$PREFIX-b" -p "127.0.0.1:$((PORT + 1)):8073" -e FERNSDR_SETUP=internet \
    -v "$WORK/state:/var/lib/fernsdr" "$IMAGE" > /dev/null
up "$((PORT + 1))" && pass "a host directory owned by root works as the volume" || fail "bind mount: $(docker logs "$PREFIX-b" 2>&1 | tail -3)"
! docker exec "$PREFIX-b" grep -q '^home_network' /var/lib/fernsdr/fernsdr.conf &&
    [ "$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$((PORT + 1))/api/admin/challenge")" = 403 ] &&
    pass "FERNSDR_SETUP=internet leaves the admin panel to HTTPS" || fail "internet setup lets plain HTTP in"

# 3. A setup that is neither.
docker run -d --name "$PREFIX-c" -e FERNSDR_SETUP=office "$IMAGE" > /dev/null
sleep 2
docker logs "$PREFIX-c" 2>&1 | grep -q 'FERNSDR_SETUP is home, internet or http, not office' &&
    [ "$(docker inspect -f '{{.State.Running}}' "$PREFIX-c")" = false ] &&
    pass "an unknown FERNSDR_SETUP stops with the reason" || fail "FERNSDR_SETUP=office: $(docker logs "$PREFIX-c" 2>&1 | tail -2)"

# 4. A volume where the receiver, which owns it, left links to a file of
# root's in place of the files a first start writes. Root must not write
# through them.
docker run --rm -u fernsdr --entrypoint /bin/sh -v "$PREFIX-linked:/var/lib/fernsdr" "$IMAGE" -c \
    'ln -s /usr/local/bin/fernsdr-entrypoint /var/lib/fernsdr/fernsdr.conf.new &&
     ln -s /usr/local/bin/fernsdr-entrypoint /var/lib/fernsdr/admin-password'
docker run -d --name "$PREFIX-d" -v "$PREFIX-linked:/var/lib/fernsdr" "$IMAGE" > /dev/null
sleep 2
! docker diff "$PREFIX-d" | grep -q ' /usr/local/bin/fernsdr-entrypoint$' &&
    pass "a first start does not write through links the receiver left in the volume" ||
    fail "a first start wrote through a link in the volume: $(docker diff "$PREFIX-d" | grep /usr/local/bin)"

if [ "$FAILURES" -eq 0 ]; then echo "all passed"; else echo "$FAILURES failed"; exit 1; fi
