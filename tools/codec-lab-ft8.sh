#!/bin/sh
#
# Counts FT8 messages that survive the audio codec, per codec setting.
#
#   tools/codec-lab-ft8.sh <variants> <rates> <wav>...
#   tools/codec-lab-ft8.sh nac2,nac3 24000,32000,48000 test/wav/20m_busy/*.wav
#
# LAB_ARGS is passed to codec-lab, for example LAB_ARGS="--passband 200,3000"
# to tell NAC3 where the receiver's audio passband was.
#
# Each WAV must be 12 kHz mono 16-bit, which is what WSJT-X's jt9 reads. The
# original is decoded first; a message counts as kept when the decoded codec
# output contains the same message text. Needs jt9 on PATH and a built
# server/build/codec-lab (make -C server lab).
set -eu

VARIANTS=${1:?usage: codec-lab-ft8.sh <variants> <rates> <wav>...}
RATES=${2:?usage: codec-lab-ft8.sh <variants> <rates> <wav>...}
shift 2

LAB=$(dirname "$0")/../server/build/codec-lab
[ -x "$LAB" ] || { echo "build it first: make -C server lab" >&2; exit 1; }
command -v jt9 >/dev/null || { echo "jt9 (WSJT-X) is not installed" >&2; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# jt9 writes its scratch files into the working and temp directories given,
# so each decode gets a fresh one.
decode() {
    dir=$(mktemp -d "$WORK/jt9.XXXXXX")
    jt9 -8 -p 15 -m 1 -w 0 -d 3 -a "$dir" -t "$dir" "$1" 2>/dev/null |
        grep -E '^[0-9*]{6} ' | sed -E 's/^[0-9*]{6} +-?[0-9]+ +-?[0-9.]+ +[0-9]+ +~ +//; s/ +\?$//; s/ +a[0-9]$//; s/ +$//' |
        sort -u
}

printf '%-16s %8s %9s %9s %9s\n' variant ceiling original kept "kept %"
for variant in $(echo "$VARIANTS" | tr ',' ' '); do
    for rate in $(echo "$RATES" | tr ',' ' '); do
        total=0
        kept=0
        for wav in "$@"; do
            name=$(basename "$wav" .wav)
            out="$WORK/$name"
            mkdir -p "$out"
            "$LAB" --audio "$wav" --variants "$variant" --rates "$rate" --write-dir "$out" ${LAB_ARGS:-} > /dev/null
            decode "$out/input.wav" > "$out/before.txt"
            decode "$out/$(echo "$variant" | tr '/' '_')-$rate.wav" > "$out/after.txt"
            before=$(wc -l < "$out/before.txt")
            survived=$(comm -12 "$out/before.txt" "$out/after.txt" | wc -l)
            total=$((total + before))
            kept=$((kept + survived))
        done
        printf '%-16s %8s %9d %9d %8s%%\n' "$variant" "$rate" "$total" "$kept" \
            "$(awk "BEGIN{printf \"%.1f\", $total ? $kept * 100 / $total : 0}")"
    done
done
