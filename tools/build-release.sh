#!/bin/sh
# A release, built and signed the way the release workflow builds it: the
# program for each platform with its tests run there, the archives with the
# pages, their manifest, install.sh with the keys the programs carry, and
# the manifest's signature. Everything lands in OUT, as it is published.
#
#   tools/build-release.sh build OUT          all of it but the signature
#   tools/build-release.sh sign OUT SECRET    the signature of OUT's manifest
#   tools/build-release.sh rehearsal OUT      both, with a key made for the run
#
# SECRET is a file, or env:NAME; `fernsdr-release sign` refuses it unless
# src/update/release_keys.h carries its public half. The workflow runs the
# two steps one after the other in a job that runs nothing but this
# repository's code and the system's compilers: the key never meets npm, and
# neither does what it signs, since the program each receiver's updater
# later runs as root is built there. Only the pages come from npm, built in
# a job of their own and taken as files.
#
# A rehearsal makes a key and puts it in place of the release keys in its
# copy of the source, so that everything that checks a key checks that one:
# what it builds installs and updates like a release, and no receiver
# anywhere trusts it. Its notes are the changelog's newest section, and its
# date today's.
#
# Builds in a copy of the checkout under a temporary directory, never in the
# checkout itself. FERNSDR_WEB_DIST names the pages, built beforehand;
# without it they are built here with npm, in the copy, with the install
# scripts of the packages switched off. Needs g++, the aarch64-linux-gnu and
# arm-linux-gnueabihf cross compilers, qemu-user and, without
# FERNSDR_WEB_DIST, Node.js.
set -eu

REPO=$(cd "$(dirname "$0")/.." && pwd)
ARCHES="x86_64 aarch64 armhf"
MANIFEST=fernsdr-release-v1.txt

die() {
    printf 'build-release: %s\n' "$*" >&2
    exit 1
}

[ $# -ge 2 ] || die "usage: build-release.sh build|sign|rehearsal OUT [SECRET]"
STEP=$1
OUT=$2
SECRET=${3:-}
case "$STEP" in
    build | rehearsal) [ $# -eq 2 ] || die "$STEP takes OUT only" ;;
    sign) [ $# -eq 3 ] || die "sign takes OUT and SECRET" ;;
    *) die "no step $STEP: build, sign or rehearsal" ;;
esac
mkdir -p "$OUT"
OUT=$(cd "$OUT" && pwd)

WORK=$(mktemp -d)
trap 'rm -rf "${WORK:?}"' EXIT
SRC=$WORK/src
# The copy is no git checkout: the archives take the commit's time from here,
# so that the same commit always packs to the same bytes.
SOURCE_DATE_EPOCH=$(git -C "$REPO" log -1 --format=%ct) || die "$REPO is not a git checkout"
export SOURCE_DATE_EPOCH

# The checkout, without builds, and with the rehearsal's public key (hex) as
# the one key the release carries, if there is one.
copy_source() {
    mkdir -p "$SRC"
    rsync -a --exclude 'build' --exclude 'build-*' --exclude 'dist' "$REPO/server/" "$SRC/server/"
    rsync -a "$REPO/tools/" "$SRC/tools/"
    cp "$REPO/LICENSE" "$REPO/CHANGELOG.md" "$SRC/"
    if [ -n "${1:-}" ]; then
        key=$(printf '%s' "$1" | sed 's/\(..\)/0x\1, /g; s/, $//')
        python3 - "$SRC/server/src/update/release_keys.h" "$key" << 'EOF'
import re, sys
path, key = sys.argv[1], sys.argv[2]
text = open(path).read()
text, count = re.subn(r"static const std::vector<ReleaseKey> keys = \{.*?\};",
                      "static const std::vector<ReleaseKey> keys = {ReleaseKey{" + key + "}};", text, flags=re.S)
if count != 1:
    sys.exit("release_keys.h does not hold the key list where it used to")
open(path, "w").write(text)
EOF
    fi
    VERSION=$(sed -n 's/.*kVersion = "\(.*\)";/\1/p' "$SRC/server/src/version.h")
    [ -n "$VERSION" ] || die "no version in src/version.h"
}

pages() {
    if [ -n "${FERNSDR_WEB_DIST:-}" ]; then
        [ -f "$FERNSDR_WEB_DIST/index.html" ] || die "no pages in $FERNSDR_WEB_DIST"
        WEB=$(cd "$FERNSDR_WEB_DIST" && pwd)
        return 0
    fi
    rsync -a --exclude node_modules --exclude dist "$REPO/web/" "$WORK/web/"
    (cd "$WORK/web" && npm ci --ignore-scripts --no-audit --no-fund && npm run build) > "$WORK/pages.log" 2>&1 ||
        { tail -n 30 "$WORK/pages.log" >&2; die "the pages did not build"; }
    WEB=$WORK/web/dist
}

# The changelog's section for this version, without its heading: the release
# notes. A release needs the section to be dated, "## 0.1.1 (2026-10-01)", and
# takes its date from there; a rehearsal takes any heading, and today.
notes() {
    heading=$(grep -m 1 "^## $VERSION\( \|$\)" "$SRC/CHANGELOG.md" || true)
    [ -n "$heading" ] || die "CHANGELOG.md has no section for $VERSION"
    DATE=$(printf '%s\n' "$heading" | sed -n "s/^## $VERSION (\([0-9]\{4\}-[0-9]\{2\}-[0-9]\{2\}\))$/\1/p")
    if [ -z "$DATE" ]; then
        [ "$STEP" = rehearsal ] || die "CHANGELOG.md's heading for $VERSION has no date: \"## $VERSION (YYYY-MM-DD)\""
        DATE=$(date -u +%Y-%m-%d)
    fi
    # From its first line of text; the manifest drops the empty lines at the end.
    awk -v heading="$heading" '$0 == heading { inside = 1; next } inside && /^## / { exit } inside' \
        "$SRC/CHANGELOG.md" | sed -e '/./,$!d' > "$WORK/notes"
}

build() {
    notes
    pages
    rm -f "$OUT"/fernsdr-*.tar "$OUT/$MANIFEST" "$OUT/$MANIFEST.sig" "$OUT/install.sh"
    for arch in $ARCHES; do
        make -C "$SRC/server" -s -j"$(nproc)" release-test ARCH="$arch"
        make -C "$SRC/server" -s -j"$(nproc)" release ARCH="$arch" WEB_DIST="$WEB"
        cp "$SRC/server/dist/fernsdr-$VERSION-linux-$arch.tar" "$OUT/"
    done
    # One argument per archive, however many spaces OUT has.
    set --
    for arch in $ARCHES; do set -- "$@" "$OUT/fernsdr-$VERSION-linux-$arch.tar"; done
    "$SRC/server/build/fernsdr-release" manifest "$OUT/$MANIFEST" "$VERSION" "$DATE" stable "$WORK/notes" "$@"
    # The same key list as the programs, from the same copy of the source.
    make -C "$SRC/server" -s installer
    cp "$SRC/server/dist/install.sh" "$OUT/install.sh"
}

sign() {
    for arch in $ARCHES; do
        [ -f "$OUT/fernsdr-$VERSION-linux-$arch.tar" ] || die "no archive for $arch in $OUT"
    done
    [ -f "$OUT/$MANIFEST" ] || die "no manifest in $OUT"
    make -C "$SRC/server" -s release-tools
    "$SRC/server/build/fernsdr-release" sign "$1" "$OUT/$MANIFEST"
    "$SRC/server/build/fernsdr-release" verify "$OUT/$MANIFEST"
}

case "$STEP" in
    build)
        copy_source
        build
        ;;
    sign)
        copy_source
        sign "$SECRET"
        ;;
    rehearsal)
        make -C "$REPO/server" -s release-tools
        key=$("$REPO/server/build/fernsdr-release" keygen "$WORK/rehearsal.key") || die "no key for the rehearsal"
        copy_source "$key"
        build
        sign "$WORK/rehearsal.key"
        ;;
esac
ls -l "$OUT"
