#!/bin/sh
#
# Does the codec let digital modes through?
#
# This is the claim the whole project rests on: people decode FT8, RTTY and
# PSK31 out of a WebSDR's audio, so the codec must not distort time. It is easy
# to assert and easy to get wrong, so this measures it with somebody else's
# decoder on somebody else's off-air recordings.
#
#   tools/digital-modes-check.sh <decode_ft8> <wav-directory> [bitrates...]
#
# `decode_ft8` is the demo decoder from github.com/kgoba/ft8_lib, which knows
# nothing about FernSDR. The recordings in its test/wav/20m_busy are real
# captures of a crowded 20 m band - dozens of overlapping signals, real noise,
# real fading - which is exactly the case a synthetic test never covers.
#
# For each recording it decodes the original, puts the same audio through the
# NAC codec, decodes that, and counts how many messages survived.

set -eu

DECODER=${1:?usage: digital-modes-check.sh <decode_ft8> <wav-dir> [bitrates...]}
WAVDIR=${2:?usage: digital-modes-check.sh <decode_ft8> <wav-dir> [bitrates...]}
shift 2
BITRATES=${*:-48000}

ROUNDTRIP=$(dirname "$0")/../server/build/nac-roundtrip
[ -x "$ROUNDTRIP" ] || { echo "build it first: make -C server roundtrip" >&2; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# A decoder that failed decoded nothing, which is not a recording with
# nothing in it.
messages() {
    if ! "$DECODER" "$1" > "$WORK/decodes" 2> "$WORK/errors"; then
        echo "$DECODER failed on $1:" >&2
        cat "$WORK/errors" >&2
        exit 1
    fi
    grep '~' "$WORK/decodes" | sed 's/.*~ *//' | sort -u
}

printf '%-10s %10s %10s %10s %9s\n' "bitrate" "original" "survived" "lost" "kept"
printf '%-10s %10s %10s %10s %9s\n' "-------" "--------" "--------" "----" "----"

for RATE in $BITRATES; do
    total=0
    kept=0
    for wav in "$WAVDIR"/*.wav; do
        [ -f "$wav" ] || continue
        messages "$wav" > "$WORK/before.txt"
        before=$(wc -l < "$WORK/before.txt")
        [ "$before" -gt 0 ] || continue
        "$ROUNDTRIP" "$wav" "$WORK/coded.wav" --bitrate "$RATE" > /dev/null
        messages "$WORK/coded.wav" > "$WORK/after.txt"
        survived=$(comm -12 "$WORK/before.txt" "$WORK/after.txt" | wc -l)
        total=$((total + before))
        kept=$((kept + survived))
    done
    [ "$total" -gt 0 ] || continue
    printf '%-10s %10d %10d %10d %8s%%\n' "$RATE" "$total" "$kept" "$((total - kept))" \
        "$(awk "BEGIN{printf \"%.1f\", $kept * 100 / $total}")"
done
