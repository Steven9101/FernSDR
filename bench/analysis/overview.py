#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""One line per receiver from a session round's summaries (analysis/session.sh
writes them first): the numbers a reader compares first, each with the
summary file it comes from, so that nothing here is measured twice.

  python3 bench/analysis/overview.py SESSION_ROUND
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from listen_report import load  # noqa: E402

COLUMNS = [
    # header, summary file, path in it, format
    ("latency median ms", "metrics", ("latency_median_ms",), "{:.0f}"),
    ("latency p95 ms", "metrics", ("latency_p95_ms",), "{:.0f}"),
    ("audio kbit/s", "compression", ("audio_kbit",), "{:.1f}"),
    ("waterfall kbit/s", "compression", ("waterfall_kbit",), "{:.1f}"),
    ("waterfall rows/s", "compression", ("waterfall_frames_per_s",), "{:.1f}"),
    ("waterfall bits/bin", "compression", ("waterfall_bits_per_bin",), "{:.2f}"),
    ("page kB", "metrics", ("page_kbytes",), "{:.0f}"),
    ("page load ms", "metrics", ("page_load_ms",), "{:.0f}"),
    ("CPU % idle", "resources", ("idle_cpu",), "{:.1f}"),
    ("CPU % 4 listeners", "resources", ("busy_cpu",), "{:.1f}"),
    ("memory MB idle", "resources", ("idle_mem",), "{:.0f}"),
    ("memory MB 4 listeners", "resources", ("busy_mem",), "{:.0f}"),
    ("SINAD dB", "quality", ("sinad_db",), "{:.1f}"),
    ("SNR dB", "quality", ("snr_db",), "{:.1f}"),
    ("200 Hz dB", "quality", ("passband_db", "200"), "{:.1f}"),
    ("2700 Hz dB", "quality", ("passband_db", "2700"), "{:.1f}"),
    ("2900 Hz dB", "quality", ("passband_db", "2900"), "{:.1f}"),
    ("pitch error Hz", "quality", ("pitch_error_hz",), "{:+.1f}"),
    ("scale error ppm", "quality", ("scale_error_ppm",), "{:+.0f}"),
    ("pitch wander Hz", "quality", ("pitch_wander_hz",), "{:.2f}"),
    ("FT8 threshold dB", "digital", ("ft8_threshold_db",), "{:.0f}"),
    ("RTTY -6 dB", "digital", ("rtty_low",), "{:.2f}"),
    ("CW +10 dB", "digital", ("cw_low",), "{:.2f}"),
    ("24 kbit/s markers", "links", ("rate24", "markers_share"), "{:.2f}"),
    ("24 kbit/s p95 ms", "links", ("rate24", "latency_p95_ms"), "{:.0f}"),
    ("outage recovery s", "links", ("out15", "recovery_s"), "{:.1f}"),
]


def main():
    root = sys.argv[1]
    sums = {k: load(os.path.join(root, f"summary-{k}.json"), {}) or {}
            for k in ("metrics", "compression", "resources", "quality", "digital", "links")}
    # listen_report keys its rows RID/TIER; a session round has one tier.
    sums["metrics"] = {k.split("/")[0]: v for k, v in sums["metrics"].items()}
    rids = sorted(d for d in os.listdir(root) if os.path.isdir(os.path.join(root, d)))
    validity = load(os.path.join(root, "summary-validity.json"), {}) or {}
    rows = {}
    for rid in rids:
        v = validity.get(rid)
        row = {"valid jobs": f"{v['jobs'] - len(v['invalid'])}/{v['jobs']}" if v else None}
        for head, src, path, fmt in COLUMNS:
            v = sums[src].get(rid)
            for p in path:
                v = v.get(p) if isinstance(v, dict) else None
            if isinstance(v, dict):
                v = v.get("median")
            row[head] = v
        rows[rid] = row
    with open(os.path.join(root, "summary-overview.json"), "w") as f:
        json.dump(rows, f, indent=1)
    lines = ["| receiver | valid jobs | " + " | ".join(c[0] for c in COLUMNS) + " |", "|---|---|" + "---|" * len(COLUMNS)]
    for rid, row in rows.items():
        lines.append(f"| {rid} | {row['valid jobs'] or 'n/a'} | " + " | ".join("n/a" if row[h] is None else fmt.format(row[h]) for h, _, _, fmt in COLUMNS) + " |")
    with open(os.path.join(root, "summary-overview.md"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
