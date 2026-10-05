#!/bin/sh
# The container image end to end: built the way `make -C server docker`
# builds it, started the way docs/DEPLOYMENT.md says, and looked at from
# outside.
#
#   tools/docker-lab.sh [IMAGE]          default: fernsdr:<version of this checkout>
#   LAB_PARTS=updates tools/docker-lab.sh
#
# First part, on IMAGE (the lab's own newest image when there is no such
# image): on a first start with an empty volume it writes a configuration
# and a password and serves; the receiver runs as `fernsdr` under the
# supervisor, which runs as root under tini, with FERNSDR_CONTAINER set; the
# admin panel takes plain HTTP from the machine it runs on in the default
# home setup and not with FERNSDR_SETUP=internet; a host directory owned by
# root works as the volume; a USB device given with --device reaches the
# receiver's group; a restart keeps configuration and password; `docker
# stop` ends it cleanly; and a first start writes nothing through links the
# receiver left in the volume.
#
# Second part, updates from the admin panel inside the container: images of
# 0.1.1, 0.1.2 and 0.1.4 are built from this checkout with `make docker`, in
# a copy whose builds trust a key made for the run, and releases are served
# over HTTPS from a container beside it. A container of 0.1.1 is updated to
# 0.1.2 through the admin API and serves it without the container
# restarting; a release that crashes is rolled back; a restart keeps 0.1.2,
# checked against its signature; a byte changed in the release kept in the
# volume, or a directory the receiver put in place of the updater's, sends
# the next start back to the image's own release; a container made again
# from a newer image runs the image's; and one started with --user says to
# pull a new image.
#
# Needs docker and python3. Changes nothing but containers, volumes, images
# and a network named fernsdr-docker-lab*, removed at the end (the images are
# kept with LAB_KEEP_IMAGES=1), and a work directory.
set -u
REPO=$(cd "$(dirname "$0")/.." && pwd)
VERSION=$(sed -n 's/.*kVersion = "\(.*\)";/\1/p' "$REPO/server/src/version.h")
IMAGE=${1:-fernsdr:$VERSION}
PARTS=${LAB_PARTS:-all}
PREFIX=fernsdr-docker-lab
LAB_IMAGE=$PREFIX
NETWORK=$PREFIX-net
RELEASES=$PREFIX-releases
PORT=${LAB_PORT:-18260}
WORK=$(mktemp -d)
FAILURES=0
pass() { printf 'PASS  %s\n' "$*"; }
fail() { printf 'FAIL  %s\n' "$*"; FAILURES=$((FAILURES + 1)); }
say() { printf '%s\n' "$*"; }
cleanup() {
    docker rm -f "$PREFIX-a" "$PREFIX-b" "$PREFIX-c" "$PREFIX-d" "$PREFIX-u" "$PREFIX-user" "$RELEASES" > /dev/null 2>&1
    docker volume rm "$PREFIX-state" "$PREFIX-linked" "$PREFIX-update" > /dev/null 2>&1
    docker network rm "$NETWORK" > /dev/null 2>&1
    if [ -z "${LAB_KEEP_IMAGES:-}" ]; then
        docker rmi "$LAB_IMAGE:0.1.1" "$LAB_IMAGE:0.1.2" "$LAB_IMAGE:0.1.4" > /dev/null 2>&1
    fi
    rm -rf "$WORK"
}
trap cleanup EXIT
trap 'exit 1' INT TERM
cleanup
WORK=$(mktemp -d)

up() {
    for i in $(seq 1 30); do
        [ "$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$1/api/status")" = 200 ] && return 0
        sleep 1
    done
    return 1
}

# --- the lab's images ----------------------------------------------------------

# Builds this checkout as VERSION into $LAB_IMAGE:VERSION and its release
# archive, in a copy whose builds trust the run's key alone.
lab_images() {
    say "building the lab's images in $WORK"
    mkdir -p "$WORK/tool"
    rsync -a --delete --exclude 'build' --exclude 'build-*' --exclude 'dist' "$REPO/server/" "$WORK/tool/server/"
    make -C "$WORK/tool/server" -s release-tools > /dev/null || return 1
    TOOL=$WORK/tool/server/build/fernsdr-release
    PUB=$("$TOOL" keygen "$WORK/release.key")
    KEY_BYTES=$(printf '%s' "$PUB" | sed 's/\(..\)/0x\1, /g; s/, $//')
    SRC=$WORK/src
    mkdir -p "$SRC"
    rsync -a --delete --exclude 'build' --exclude 'build-*' --exclude 'dist' "$REPO/server/" "$SRC/server/"
    cp "$REPO/LICENSE" "$SRC/LICENSE"
    python3 - "$SRC/server/src/update/release_keys.h" "$KEY_BYTES" << 'EOF'
import sys, re
path, key = sys.argv[1], sys.argv[2]
text = open(path).read()
text = re.sub(r"static const std::vector<ReleaseKey> keys = \{.*?\};",
              "static const std::vector<ReleaseKey> keys = {ReleaseKey{" + key + "}};", text, flags=re.S)
open(path, "w").write(text)
EOF
    # The pages are a stub unless LAB_WEB names a build of them: the admin
    # API is what is tried here.
    if [ -n "${LAB_WEB:-}" ]; then
        WEB=$(cd "$LAB_WEB" && pwd)
    else
        WEB=$WORK/web
        mkdir -p "$WEB"
        printf '<!doctype html><title>FernSDR docker lab</title>\n' > "$WEB/index.html"
    fi
    mkdir -p "$WORK/share"
    for version in 0.1.1 0.1.2 0.1.4; do
        sed -i "s/kVersion = \"[^\"]*\"/kVersion = \"$version\"/" "$SRC/server/src/version.h"
        make -C "$SRC/server" -s -j"$(nproc)" docker ARCH=x86_64 WEB_DIST="$WEB" DOCKER_TAG="$LAB_IMAGE:$version" \
            > "$WORK/build-$version.log" 2>&1 || { tail -20 "$WORK/build-$version.log"; return 1; }
        publish "$version" "$SRC/server/dist/fernsdr-$version-linux-x86_64.tar"
    done
    # 0.1.3 passes the configuration check and crashes as it starts.
    mkdir -p "$WORK/tree-0.1.3/web"
    cp "$WEB/index.html" "$WORK/tree-0.1.3/web/"
    printf '#!/bin/sh\n[ "$1" = --check ] && exit 0\necho crashing >&2\nexit 1\n' > "$WORK/tree-0.1.3/fernsdr"
    chmod 755 "$WORK/tree-0.1.3/fernsdr"
    SOURCE_DATE_EPOCH=0 "$TOOL" pack "$WORK/fernsdr-0.1.3-linux-x86_64.tar" "$WORK/tree-0.1.3"
    publish 0.1.3 "$WORK/fernsdr-0.1.3-linux-x86_64.tar"
}

# VERSION ARCHIVE: the archive as published, with its manifest, signed.
publish() {
    out=$WORK/share/www-$1
    rm -rf "$out"
    mkdir -p "$out"
    cp "$2" "$out/"
    printf 'Docker lab release %s.\n' "$1" > "$WORK/notes"
    "$TOOL" manifest "$out/fernsdr-release-v1.txt" "$1" 2026-10-01 stable "$WORK/notes" "$out/${2##*/}" > /dev/null
    "$TOOL" sign --test-key "$WORK/release.key" "$out/fernsdr-release-v1.txt" > /dev/null
}

# --- part 1: the image as it starts ------------------------------------------------

basic() {
    say "--- $IMAGE as it starts"
    # 1. A named volume, the default setup.
    docker run -d --name "$PREFIX-a" -p "127.0.0.1:$PORT:8073" -v "$PREFIX-state:/var/lib/fernsdr" \
        --device /dev/null:/dev/bus/usb/001/002 "$IMAGE" > /dev/null
    up "$PORT" && pass "a first start with an empty volume serves" || fail "it does not serve: $(docker logs "$PREFIX-a" 2>&1 | tail -3)"
    password=$(docker logs "$PREFIX-a" 2>&1 | sed -n 's/^FernSDR: admin password: \([A-Za-z0-9]*\) .*/\1/p')
    stored=$(docker exec "$PREFIX-a" cat /var/lib/fernsdr/admin-password 2> /dev/null)
    [ -n "$password" ] && [ "$password" = "$stored" ] && pass "the password is in the log once and in the volume" ||
        fail "password: log '$password', volume '$stored'"
    modes=$(docker exec "$PREFIX-a" stat -c '%U %a' /var/lib/fernsdr /var/lib/fernsdr/fernsdr.conf /var/lib/fernsdr/admin-password /var/lib/fernsdr/update | tr '\n' ',')
    [ "$modes" = "fernsdr 700,fernsdr 600,fernsdr 600,root 755," ] && pass "owners and modes in the volume" || fail "owners and modes: $modes"
    tree=$(docker exec "$PREFIX-a" ps -o pid,user,comm | awk 'NR > 1 && $3 != "ps" { print $1 ":" $2 ":" $3 }' | tr '\n' ' ')
    case "$tree" in
        "1:root:tini "*":root:fernsdr "*":fernsdr:fernsdr "*) pass "tini is first, the supervisor runs as root, the receiver as fernsdr" ;;
        *) fail "processes: $tree" ;;
    esac
    # The receiver's own /proc/PID/environ is closed even to root in the
    # container: it is not dumpable. The image's environment is what the
    # supervisor passes on.
    docker inspect -f '{{range .Config.Env}}{{println .}}{{end}}' "$IMAGE" | grep -qx 'FERNSDR_CONTAINER=1' &&
        pass "the image tells the receiver it is in a container" || fail "no FERNSDR_CONTAINER in the image"
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
    # Given to the receiver when the container starts, all but the
    # updater's directory: made the receiver's, it would no longer be
    # believed, and what it keeps would be lost.
    docker stop "$PREFIX-b" > /dev/null
    chown -R 0:0 "$WORK/state"
    touch "$WORK/state/update/kept"
    docker start "$PREFIX-b" > /dev/null
    up "$((PORT + 1))" && [ -e "$WORK/state/update/kept" ] && [ "$(stat -c '%u' "$WORK/state/update")" = 0 ] &&
        [ "$(stat -c '%u' "$WORK/state/fernsdr.conf")" != 0 ] &&
        pass "a volume given back to root is the receiver's again, but for the updater's directory" ||
        fail "after chown: update $(stat -c '%u' "$WORK/state/update"), fernsdr.conf $(stat -c '%u' "$WORK/state/fernsdr.conf")"
    docker rm -f "$PREFIX-b" > /dev/null
    rm -rf "$WORK/state"

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
}

# --- part 2: updates from the admin panel ---------------------------------------------

U=$PREFIX-u
UPORT=$((PORT + 2))
# The admin API as the panel uses it.
admin() { python3 "$REPO/tools/admin-api.py" "http://127.0.0.1:$UPORT" "$UPASSWORD" "$@"; }
field() { python3 -c 'import json, sys
try:
    value = json.load(sys.stdin)
except ValueError:
    value = None
for key in sys.argv[1].split("."):
    value = value.get(key) if isinstance(value, dict) else None
print("" if value is None else value)' "$1"; }
update_state() { docker exec "$U" cat /var/lib/fernsdr/update/status.json 2> /dev/null | field state; }
running() { admin GET /api/admin/update 2> /dev/null | field running; }
serves() {
    for i in $(seq 1 60); do
        [ "$(running)" = "$1" ] && return 0
        sleep 1
    done
    return 1
}
# The container, from IMAGE, on the lab's network with the lab's release
# server and its authority.
start_u() {
    docker run -d --name "$U" --network "$NETWORK" -p "127.0.0.1:$UPORT:8073" -v "$PREFIX-update:/var/lib/fernsdr" \
        -v "$WORK/share:/lab:ro" -e FERNSDR_UPDATE_URL="https://$RELEASES:8443/" -e FERNSDR_UPDATE_TRIAL_SECONDS=90 \
        "$@" > /dev/null
    docker exec "$U" sh -c 'cat /lab/ca.pem >> /etc/ssl/certs/ca-certificates.crt'
    up "$UPORT"
}
# Looks, asks for VERSION, and waits for the updater to finish.
update_to() {
    admin POST /api/admin/update/check '{}' > /dev/null
    for i in $(seq 1 30); do
        [ "$(admin GET /api/admin/update | field check.state)" = done ] && break
        sleep 1
    done
    found=$(admin GET /api/admin/update | field check.version)
    [ "$found" = "$1" ] || { say "  the look found '$found', not $1"; return 1; }
    # The last update's account goes, so that only this one's end counts.
    docker exec "$U" rm -f /var/lib/fernsdr/update/status.json
    admin POST /api/admin/update/start "{\"version\":\"$1\"}" > /dev/null || return 1
    for i in $(seq 1 200); do
        case "$(update_state)" in updated | rolled-back | refused | failed) return 0 ;; esac
        sleep 1
    done
    return 1
}
publish_u() { docker exec "$RELEASES" sh -c "rm -f /srv/* && cp /lab/www-$1/* /srv/"; }

updates() {
    say "--- updates from the admin panel inside the container"
    lab_images || { fail "the lab's images could not be built"; return; }
    openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj "/CN=FernSDR docker lab CA" \
        -keyout "$WORK/ca.key" -out "$WORK/share/ca.pem" 2> /dev/null
    openssl req -newkey rsa:2048 -nodes -subj "/CN=$RELEASES" -keyout "$WORK/share/server.key" \
        -out "$WORK/server.csr" 2> /dev/null
    printf 'subjectAltName=DNS:%s\n' "$RELEASES" > "$WORK/san.ext"
    openssl x509 -req -in "$WORK/server.csr" -CA "$WORK/share/ca.pem" -CAkey "$WORK/ca.key" -CAcreateserial \
        -days 2 -extfile "$WORK/san.ext" -out "$WORK/share/server.pem" 2> /dev/null
    chmod -R a+rX "$WORK/share"
    docker network create "$NETWORK" > /dev/null
    docker run -d --name "$RELEASES" --network "$NETWORK" -v "$WORK/share:/lab:ro" alpine:3.22 sh -c \
        'apk add --no-cache openssl > /dev/null && mkdir -p /srv && cd /srv &&
         exec openssl s_server -quiet -accept 8443 -cert /lab/server.pem -key /lab/server.key -WWW' > /dev/null
    for i in $(seq 1 60); do
        docker exec "$RELEASES" pgrep openssl > /dev/null 2>&1 && break
        sleep 1
    done

    # 1. 0.1.1's image, updated to 0.1.2 from the admin panel.
    start_u "$LAB_IMAGE:0.1.1" || fail "0.1.1 does not serve: $(docker logs "$U" 2>&1 | tail -3)"
    UPASSWORD=$(docker exec "$U" cat /var/lib/fernsdr/admin-password)
    [ "$(admin GET /api/admin/update | field available)" = True ] && [ "$(running)" = 0.1.1 ] &&
        pass "the Updates page offers updates in the container" ||
        fail "updates: $(admin GET /api/admin/update | field unavailable)"
    [ "$(admin GET /api/admin/update | field autostart.init)" = container ] &&
        pass "starting with the computer is Docker's to decide" || fail "autostart: $(admin GET /api/admin/update | field autostart)"
    publish_u 0.1.2
    began=$(docker inspect -f '{{.State.StartedAt}}' "$U")
    started=$(date +%s)
    if update_to 0.1.2 && [ "$(update_state)" = updated ] && serves 0.1.2; then
        pass "updated to 0.1.2 in $(($(date +%s) - started)) s, and it serves"
    else
        fail "0.1.2: $(docker exec "$U" cat /var/lib/fernsdr/update/status.json)"
    fi
    [ "$(docker inspect -f '{{.State.StartedAt}}' "$U")" = "$began" ] &&
        pass "only the receiver restarted, not the container" || fail "the container restarted"
    [ "$(docker exec "$U" readlink /var/lib/fernsdr/update/release/trusted)" = 0.1.2 ] &&
        [ "$(docker exec "$U" stat -c '%U %a' /var/lib/fernsdr/update/release/0.1.2)" = "root 755" ] &&
        pass "the volume keeps 0.1.2 as published, in a directory of root's" ||
        fail "kept: $(docker exec "$U" ls -la /var/lib/fernsdr/update/release)"

    # 2. A release that crashes as it starts.
    publish_u 0.1.3
    started=$(date +%s)
    if update_to 0.1.3 && [ "$(update_state)" = rolled-back ] && serves 0.1.2; then
        pass "0.1.3 rolled back after $(($(date +%s) - started)) s, 0.1.2 serves"
    else
        fail "0.1.3: $(docker exec "$U" cat /var/lib/fernsdr/update/status.json)"
    fi
    ! docker exec "$U" test -e /var/lib/fernsdr/update/release/0.1.3 &&
        pass "the rolled-back release is not kept" || fail "0.1.3 is still kept in the volume"

    # 3. Restarted, it runs 0.1.2 again, checked.
    docker restart "$U" > /dev/null
    up "$UPORT" && serves 0.1.2 && docker logs --since "$began" "$U" 2>&1 | grep -q 'Running 0.1.2, kept in the volume and checked' &&
        pass "a restart runs 0.1.2 again, checked against its signature" || fail "after a restart: $(docker logs "$U" 2>&1 | grep 'FernSDR:' | tail -3)"

    # 4. A byte of the kept archive changed: the image's own release runs.
    docker exec "$U" sh -c 'f=$(ls /var/lib/fernsdr/update/release/0.1.2/*.tar) &&
        printf x | dd of="$f" bs=1 seek=4096 conv=notrunc 2> /dev/null'
    mark=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    docker restart "$U" > /dev/null
    up "$UPORT" && serves 0.1.1 && docker logs --since "$mark" "$U" 2>&1 | grep -q 'The release 0.1.2 kept in the volume was refused' &&
        pass "a changed release in the volume is refused at the next start, and 0.1.1 runs" ||
        fail "tampered: $(docker logs --since "$mark" "$U" 2>&1 | grep 'FernSDR:')"

    # 5. The receiver, which owns the volume, puts a directory of its own in
    # place of the updater's, with a genuine signed release in it.
    docker exec -u fernsdr "$U" sh -c 'cd /var/lib/fernsdr && mv update update.away &&
        mkdir -p update/release/0.1.2 && cp /lab/www-0.1.2/* update/release/0.1.2/ && ln -s 0.1.2 update/release/trusted'
    mark=$(date -u +%Y-%m-%dT%H:%M:%SZ)
    docker restart "$U" > /dev/null
    up "$UPORT" && serves 0.1.1 && docker logs --since "$mark" "$U" 2>&1 | grep -q 'was not root.s alone' &&
        [ "$(docker exec "$U" stat -c %U /var/lib/fernsdr/update)" = root ] &&
        pass "a directory of the receiver's in the updater's place is not believed" ||
        fail "planted: $(docker logs --since "$mark" "$U" 2>&1 | grep 'FernSDR:')"

    # 6. Updated to 0.1.2 again, then made again from a newer image.
    publish_u 0.1.2
    if update_to 0.1.2 && serves 0.1.2; then
        docker rm -f "$U" > /dev/null
        start_u "$LAB_IMAGE:0.1.4"
        serves 0.1.4 && docker logs "$U" 2>&1 | grep -q "The image's 0.1.4 is not older than 0.1.2" &&
            ! docker exec "$U" test -e /var/lib/fernsdr/update/release/trusted &&
            pass "a newer image wins over the release the volume keeps" ||
            fail "newer image: $(docker logs "$U" 2>&1 | grep 'FernSDR:')"
    else
        fail "0.1.2 again: $(docker exec "$U" cat /var/lib/fernsdr/update/status.json)"
    fi

    # 7. A file system the container cannot write: the receiver runs as
    # before, without the updater.
    docker rm -f "$U" > /dev/null
    docker run -d --name "$U" --read-only -p "127.0.0.1:$UPORT:8073" -v "$PREFIX-update:/var/lib/fernsdr" \
        "$LAB_IMAGE:0.1.4" > /dev/null
    up "$UPORT" && docker logs "$U" 2>&1 | grep -q 'runs without updates from the admin panel' &&
        admin GET /api/admin/update | field unavailable | grep -q 'pulling the new image' &&
        pass "with --read-only it serves without the updater, and says so" ||
        fail "--read-only: $(docker logs "$U" 2>&1 | tail -3)"

    # 8. Started as the receiver's user: no updater, and the page says so.
    docker rm -f "$U" > /dev/null
    docker run -d --name "$U" -u fernsdr -p "127.0.0.1:$UPORT:8073" -v "$PREFIX-update:/var/lib/fernsdr" \
        "$LAB_IMAGE:0.1.4" > /dev/null
    up "$UPORT" && admin GET /api/admin/update | field unavailable | grep -q 'pulling the new image' &&
        pass "started with --user, the Updates page says to pull a new image" ||
        fail "--user: $(admin GET /api/admin/update)"
}

if [ "$PARTS" != updates ]; then
    if ! docker image inspect "$IMAGE" > /dev/null 2>&1; then
        [ "$PARTS" = all ] && [ -z "${1:-}" ] || { echo "no image $IMAGE: make -C server docker first"; exit 1; }
    fi
fi
case "$PARTS" in
    basic) basic ;;
    updates) updates ;;
    all)
        updates
        # Without an image of the checkout, the first part runs on the lab's.
        docker image inspect "$IMAGE" > /dev/null 2>&1 || IMAGE=$LAB_IMAGE:0.1.4
        basic
        ;;
esac

if [ "$FAILURES" -eq 0 ]; then echo "all passed"; else echo "$FAILURES failed"; exit 1; fi
