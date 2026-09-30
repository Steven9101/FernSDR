#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""How each receiver compresses, stated and measured.

The stated part comes from bench/desk/codecs.json (codec, lossy or not and
where the loss is, with the source at the pinned version). The measured
part comes from the plain listener of each session (harness/links.py),
inside its window: audio kbit/s, frames a second and bytes a frame;
waterfall kbit/s, rows a second (one frame a row), bytes a row and, where
the row's width is known, bits a bin and the factor against raw 8-bit rows
(one byte a bin, uncompressed).

  python3 bench/analysis/compression.py SESSION_ROUND
"""
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH = os.path.dirname(HERE)
sys.path.insert(0, HERE)
from listen_report import load, summarize  # noqa: E402


def stream_stats(sockets, t0, t1, cls):
    frames = 0
    size = 0
    for s in sockets:
        v = s["streams"].get(f"{cls}:rx")
        if not v:
            continue
        for t, b in zip(v["times"], v.get("sizes", [])):
            if t0 <= t <= t1:
                frames += 1
                size += b
    secs = (t1 - t0) / 1000
    return {"kbit": size * 8 / 1000 / secs, "frames_per_s": frames / secs,
            "bytes_per_frame": size / frames if frames else None}


def main():
    root = sys.argv[1]
    codecs = load(os.path.join(BENCH, "desk", "codecs.json"), {})
    table = {}
    for rid in sorted(os.listdir(root)):
        rdir = os.path.join(root, rid)
        if not os.path.isdir(rdir):
            continue
        runs = []
        for rep in sorted(os.listdir(rdir)):
            d = os.path.join(rdir, rep, "listen")
            meta = load(os.path.join(d, "meta.json"))
            if not meta or not meta.get("measurementStartedWallMs"):
                continue
            # A job links_report found invalid says nothing about the stream.
            if (load(os.path.join(d, "metrics.json"), {}) or {}).get("invalid"):
                continue
            socks = load(os.path.join(d, "sockets.json"), [])
            t0, t1 = meta["measurementStartedWallMs"], meta["endedWallMs"]
            runs.append({"audio": stream_stats(socks, t0, t1, "audio"), "waterfall": stream_stats(socks, t0, t1, "waterfall")})
        c = codecs.get(rid, {})
        bins = (c.get("waterfall") or {}).get("bins_per_row")
        row = {"audio_codec": (c.get("audio") or {}).get("codec"), "audio_lossy": (c.get("audio") or {}).get("lossy"),
               "waterfall_codec": (c.get("waterfall") or {}).get("codec"), "waterfall_lossy": (c.get("waterfall") or {}).get("lossy"),
               "waterfall_bins_per_row": bins, "runs": len(runs)}
        for part in ("audio", "waterfall"):
            for k in ("kbit", "frames_per_s", "bytes_per_frame"):
                row[f"{part}_{k}"] = summarize([r[part][k] for r in runs])
        bpf = row["waterfall_bytes_per_frame"]
        if bins and bpf:
            row["waterfall_bits_per_bin"] = bpf["median"] * 8 / bins
            row["waterfall_factor_vs_8bit"] = bins / bpf["median"]
        table[rid] = row
    with open(os.path.join(root, "summary-compression.json"), "w") as f:
        json.dump(table, f, indent=1)
    fmt = lambda s, p="{:.1f}": "n/a" if not s else p.format(s["median"])  # noqa: E731
    lines = ["| receiver | audio codec | lossy | audio kbit/s | waterfall coding | lossy | waterfall kbit/s | rows/s | bins/row | bytes/row | bits/bin | vs raw 8-bit |",
             "|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for rid, r in table.items():
        lines.append(f"| {rid} | {r['audio_codec'] or 'n/a'} | {'yes' if r['audio_lossy'] else 'no' if r['audio_lossy'] is False else '?'} | "
                     f"{fmt(r['audio_kbit'])} | {r['waterfall_codec'] or 'n/a'} | "
                     f"{'yes' if r['waterfall_lossy'] else 'no' if r['waterfall_lossy'] is False else '?'} | "
                     f"{fmt(r['waterfall_kbit'])} | {fmt(r['waterfall_frames_per_s'])} | {r['waterfall_bins_per_row'] or 'n/a'} | "
                     f"{fmt(r['waterfall_bytes_per_frame'], '{:.0f}')} | "
                     f"{'n/a' if 'waterfall_bits_per_bin' not in r else format(r['waterfall_bits_per_bin'], '.2f')} | "
                     f"{'n/a' if 'waterfall_factor_vs_8bit' not in r else format(r['waterfall_factor_vs_8bit'], '.1f') + 'x'} |")
    with open(os.path.join(root, "summary-compression.md"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
