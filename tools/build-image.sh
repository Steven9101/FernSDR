#!/bin/sh
# The container image of a release, for x86_64, aarch64 and armhf, made from
# the release's own archives, so that the program in the image is the one
# the release signed. The Dockerfile's RUN steps for the ARM images run
# under QEMU, through the host's binfmt handlers.
#
#   tools/build-image.sh OUT IMAGE            build each image into docker
#   tools/build-image.sh OUT IMAGE push       push them, and IMAGE:VERSION and
#                                             IMAGE:latest naming all three
#
# OUT is what tools/build-release.sh left: the archives and the manifest.
set -eu

[ $# -ge 2 ] || { echo "usage: $0 OUT IMAGE [push]" >&2; exit 2; }
OUT=$1
IMAGE=$2
PUSH=${3:-}
SRC=$(cd "$(dirname "$0")/.." && pwd)
VERSION=$(sed -n 's/^version //p' "$OUT/fernsdr-release-v1.txt" | head -n 1)
[ -n "$VERSION" ] || { echo "no version in $OUT/fernsdr-release-v1.txt" >&2; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
names=""
for arch in x86_64 aarch64 armhf; do
    archive=$OUT/fernsdr-$VERSION-linux-$arch.tar
    [ -f "$archive" ] || { echo "no $archive" >&2; exit 1; }
    case "$arch" in
        x86_64) platform=linux/amd64 ;;
        aarch64) platform=linux/arm64 ;;
        armhf) platform=linux/arm/v7 ;;
    esac
    context=$WORK/$arch
    mkdir -p "$context/tree"
    tar -xf "$archive" -C "$context/tree"
    cp "$SRC/server/docker/Dockerfile" "$SRC/server/docker/entrypoint.sh" "$context/"
    name=$IMAGE:$VERSION-$arch
    # --load for a look here; --push leaves no copy but the registry's.
    docker buildx build --platform "$platform" --provenance=false \
        --label "org.opencontainers.image.version=$VERSION" \
        --label "org.opencontainers.image.source=https://github.com/Steven9101/FernSDR" \
        --tag "$name" $([ -n "$PUSH" ] && echo --push || echo --load) "$context"
    names="$names $name"
done
if [ -n "$PUSH" ]; then
    # shellcheck disable=SC2086 # the names are words
    docker buildx imagetools create --tag "$IMAGE:$VERSION" --tag "$IMAGE:latest" $names
fi
echo "built:$names"
