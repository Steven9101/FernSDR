#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""The receiver's CPU and memory in a session (harness/links.py).

Per session the harness samples the receiver's containers every second,
tagged idle (input running, no listener, the first seconds) or with the
batch of listeners connected. CPU is in percent of one core, all of the
receiver's processes; memory is the containers' charged memory. Batches
have four listeners, some of them on shaped links; the first batch is the
plain listener, the digital and tones listeners and one on 32 kbit/s.

  python3 bench/analysis/resources_report.py SESSION_ROUND
"""
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from listen_report import load, summarize  # noqa: E402


def phase_stats(rows, prefix):
    sel = [r for r in rows if str(r.get("phase", "")).startswith(prefix)]
    if len(sel) < 3:
        return None, None
    # Skip the first sample of a phase: listeners were still connecting.
    sel = sel[1:]
    dt = sel[-1]["t"] - sel[0]["t"]
    cpu = (sel[-1]["cpu_usec"] - sel[0]["cpu_usec"]) / 1e6 / dt * 100 if dt > 0 else None
    mem = max(r["mem_bytes"] for r in sel) / 2**20
    return cpu, mem


def main():
    root = sys.argv[1]
    table = {}
    for rid in sorted(os.listdir(root)):
        rdir = os.path.join(root, rid)
        if not os.path.isdir(rdir):
            continue
        runs = []
        for rep in sorted(os.listdir(rdir)):
            rows = load(os.path.join(rdir, rep, "resources.json"))
            if not rows:
                continue
            idle_cpu, idle_mem = phase_stats(rows, "idle")
            busy_cpu, busy_mem = phase_stats(rows, "batch0")
            runs.append({"idle_cpu": idle_cpu, "idle_mem": idle_mem, "busy_cpu": busy_cpu, "busy_mem": busy_mem})
        table[rid] = {k: summarize([r[k] for r in runs]) for k in ("idle_cpu", "idle_mem", "busy_cpu", "busy_mem")}
        table[rid]["runs"] = len(runs)
    with open(os.path.join(root, "summary-resources.json"), "w") as f:
        json.dump(table, f, indent=1)
    fmt = lambda s, p="{:.1f}": "n/a" if not s else p.format(s["median"])  # noqa: E731
    lines = ["| receiver | CPU % idle | memory MB idle | CPU % with 4 listeners | memory MB with 4 listeners |", "|---|---|---|---|---|"]
    for rid, v in table.items():
        lines.append(f"| {rid} | {fmt(v['idle_cpu'])} | {fmt(v['idle_mem'], '{:.0f}')} | {fmt(v['busy_cpu'])} | {fmt(v['busy_mem'], '{:.0f}')} |")
    with open(os.path.join(root, "summary-resources.md"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
