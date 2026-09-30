#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Capacity round (bench/harness/capacity.py) as a table.

Per receiver: the knee (the most listeners at which 95% still got 90% of a
real browser's audio rate and no socket closed), the receiver's CPU at one
listener and at the knee (percent of one core; the receiver has two), CPU
per added listener (least squares over the passed steps), memory at the
knee, and whether the load clients could give each listener its own
frequency (where they could not, the load is easier for receivers that
share work between listeners on one frequency).

  python3 bench/analysis/capacity_report.py ROUND
"""
import json
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from listen_report import load, summarize  # noqa: E402


def main():
    root = sys.argv[1]
    table = {}
    for rid in sorted(os.listdir(root)):
        rdir = os.path.join(root, rid)
        if not os.path.isdir(rdir):
            continue
        reps = []
        for rep in sorted(os.listdir(rdir)):
            m = load(os.path.join(rdir, rep, "manifest.json"))
            if not m:
                continue
            steps = [s for s in m.get("steps", []) if "listeners" in s]
            passed = [s for s in steps if s.get("passed")]
            cpu1 = next((s["cpu_percent"] for s in steps if s["listeners"] == 1), None)
            knee = max(passed, key=lambda s: s["listeners"]) if passed else None
            per = None
            pts = [(s["listeners"], s["cpu_percent"]) for s in passed if s.get("cpu_percent") is not None]
            if len(pts) >= 2:
                per = float(np.polyfit([p[0] for p in pts], [p[1] for p in pts], 1)[0])
            failed = next((s for s in steps if not s.get("passed")), None)
            reps.append({"knee": m.get("knee"), "cpu_one": cpu1, "cpu_knee": knee and knee.get("cpu_percent"),
                         "cpu_per_listener": per, "mem_knee": knee and knee.get("mem_mb"),
                         "spread": min((s.get("spread_share", 0) for s in steps), default=None),
                         "limit": ("closed sockets" if failed and failed.get("closed_early") else
                                   "audio short" if failed else "last step passed") if steps else m.get("error"),
                         "state": m.get("state")})
        table[rid] = {"runs": len(reps), "knee": summarize([r["knee"] for r in reps]),
                      "cpu_one": summarize([r["cpu_one"] for r in reps]),
                      "cpu_knee": summarize([r["cpu_knee"] for r in reps]),
                      "cpu_per_listener": summarize([r["cpu_per_listener"] for r in reps]),
                      "mem_knee": summarize([r["mem_knee"] for r in reps]),
                      "spread": [r["spread"] for r in reps], "limit": [r["limit"] for r in reps]}
    with open(os.path.join(root, "summary-capacity.json"), "w") as f:
        json.dump(table, f, indent=1)
    f = lambda s, p="{:.0f}": "n/a" if not s else p.format(s["median"])  # noqa: E731
    lines = ["| receiver | listeners at the knee | what stopped it | CPU % at 1 | CPU % at the knee | CPU % per listener | memory MB at the knee | own frequency per listener |",
             "|---|---|---|---|---|---|---|---|"]
    for rid, v in table.items():
        spread = v["spread"][0] if v["spread"] else None
        lines.append(f"| {rid} | {f(v['knee'])} | {', '.join(str(x) for x in v['limit'])} | {f(v['cpu_one'], '{:.1f}')} | "
                     f"{f(v['cpu_knee'])} | {f(v['cpu_per_listener'], '{:.2f}')} | {f(v['mem_knee'])} | "
                     f"{'yes' if spread and spread > 0.9 else 'no' if spread is not None else 'n/a'} |")
    with open(os.path.join(root, "summary-capacity.md"), "w") as fh:
        fh.write("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
