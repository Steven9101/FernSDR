#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Turns a listen round into per-run metrics and a per-receiver table.

For every run it measures, inside the exact window listen.mjs recorded
(after the warm-up, 60 s): WebSocket payload per stream and direction as
kbit/s (mean and p95 of one-second bins), bytes on the client's veth
(everything on the wire: headers, acknowledgements, retransmissions), the
receiver's CPU (its containers' cgroups, all processes) and memory, the
page load, and the latency of every marker. Runs that failed a gate are
listed, never dropped silently; summaries use valid runs only.

  python3 bench/analysis/listen_report.py bench/runs/ROUND
"""
import json
import math
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "source"))
import latency  # noqa: E402


def session_loop(pace_log):
    """True when pace repeated a pre-rendered loop (harness REAL_LOOP)."""
    try:
        with open(pace_log) as f:
            return json.loads(f.readline()).get("loop") is True
    except (OSError, ValueError, TypeError):
        return False


def load(path, default=None):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


def bins_kbit(times, sizes, t0, t1):
    """kbit/s in complete one-second bins of [t0, t1) (ms)."""
    n = int((t1 - t0) // 1000)
    if n <= 0:
        return np.zeros(0)
    b = np.zeros(n)
    for t, s in zip(times, sizes):
        k = int((t - t0) // 1000)
        if 0 <= k < n:
            b[k] += s
    return b * 8 / 1000


def run_metrics(d, marker_real, marker_iq, pace_log=None, real=None):
    meta = load(os.path.join(d, "meta.json"), {})
    man = load(os.path.join(d, "manifest.json"), {})
    state = man.get("state")
    if state is None and meta.get("gates") is not None:
        # A session job has no manifest of its own: its gates decide.
        state = "validated" if all(meta["gates"].values()) else "setup_failed"
    out = {"state": state, "gates": meta.get("gates"), "error": man.get("error") or meta.get("error")}
    t0, t1 = meta.get("measurementStartedWallMs"), meta.get("endedWallMs")
    if not t0 or not t1:
        return out
    secs = (t1 - t0) / 1000
    streams = {}
    for s in load(os.path.join(d, "sockets.json"), []):
        for k, v in s["streams"].items():
            b = bins_kbit(v["times"], v.get("sizes", []), t0, t1)
            if k in streams:
                streams[k] = streams[k] + b
            else:
                streams[k] = b
    out["kbit"] = {k: {"mean": float(b.mean()), "p95": float(np.percentile(b, 95))} for k, b in streams.items() if len(b)}
    rx_total = sum(b for k, b in streams.items() if k.endswith(":rx"))
    if not isinstance(rx_total, int):
        out["kbit"]["total:rx"] = {"mean": float(rx_total.mean()), "p95": float(np.percentile(rx_total, 95))}
    rows = [r for r in load(os.path.join(d, "resources.json"), []) if t0 / 1000 <= r["t"] <= t1 / 1000]
    if len(rows) >= 2:
        dt = rows[-1]["t"] - rows[0]["t"]
        out["cpu_percent"] = (rows[-1]["cpu_usec"] - rows[0]["cpu_usec"]) / 1e6 / dt * 100
        mem = [r["mem_bytes"] for r in rows]
        out["mem_mb"] = {"mean": float(np.mean(mem)) / 2**20, "max": float(np.max(mem)) / 2**20}
        if "veth" in rows[0] and "veth" in rows[-1]:
            # The host end's tx is what the client received.
            out["wire_kbit"] = {"down": (rows[-1]["veth"]["tx_bytes"] - rows[0]["veth"]["tx_bytes"]) * 8 / 1000 / dt,
                                "up": (rows[-1]["veth"]["rx_bytes"] - rows[0]["veth"]["rx_bytes"]) * 8 / 1000 / dt}
        # Foreign load on the receiver's cores: busy time the receiver did not use.
        cores = [c for c in ("cpu4", "cpu5") if c in rows[0]["cores"]]
        busy = sum(rows[-1]["cores"][c]["busy"] - rows[0]["cores"][c]["busy"] for c in cores) / os.sysconf("SC_CLK_TCK")
        out["foreign_cpu_percent_rx_cores"] = max(0.0, (busy / dt * 100) - out["cpu_percent"])
    nav = (meta.get("navigation") or [{}])[0]
    reqs = load(os.path.join(d, "requests.json"), [])
    out["page"] = {
        "ttfb_ms": nav.get("responseStart"), "dcl_ms": nav.get("domContentLoadedEventEnd"), "load_ms": nav.get("loadEventEnd"),
        "requests": len(reqs),
        "kbytes": sum(((r.get("sizes") or {}).get("responseBodySize", 0) + (r.get("sizes") or {}).get("responseHeadersSize", 0))
                      for r in reqs) / 1000,
        "failed": sum(1 for r in reqs if r.get("failure") or r.get("error")),
        "third_party": sorted({r["url"].split("/")[2] for r in reqs if "198.18." not in r["url"] and "://" in r["url"]}),
    }
    out["audio"] = {"tone_hz": meta.get("toneHz"), "clock_ratio": meta.get("clockRatio"), "fps": meta.get("animationFps")}
    rec = load(os.path.join(d, "audio.json"), {})
    if real is None:
        real = man.get("receiver_manifest", {}).get("input", {}).get("format") == "s16"
    marker = marker_real if real else marker_iq
    try:
        rate = 20480000 if marker is marker_real else 2048000
        lat = latency.analyse(d, pace_log or os.path.join(d, "pace.jsonl"), marker, rate, 7159200, "usb",
                              loop_s=30.0 if real and session_loop(pace_log) else None)
        out["latency"] = lat.get("summary") or {"error": lat.get("error")}
    except Exception as e:  # a run without markers still has its other figures
        out["latency"] = {"error": str(e)[:200]}
    out["audio"]["seconds"] = rec.get("holes") is not None and secs
    return out


def summarize(values):
    v = [x for x in values if x is not None and not (isinstance(x, float) and math.isnan(x))]
    if not v:
        return None
    return {"median": float(np.median(v)), "min": float(np.min(v)), "max": float(np.max(v)), "n": len(v)}


def main():
    root = sys.argv[1]
    bench = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    import scene  # noqa: E402
    marker_iq = scene.Scene(os.path.join(bench, "scenes", "markers.scene"), 1).describe()["markers"][0]
    marker_real = scene.Scene(os.path.join(bench, "scenes", "markers-real.scene"), 1).describe()["markers"][0]
    per = {}
    session = os.path.exists(os.path.join(root, "round.json")) and json.load(open(os.path.join(root, "round.json"))).get("suite") == "session"
    if session:
        # Combined sessions (harness/links.py): the plain listener of each.
        for rid in sorted(os.listdir(root)):
            rdir = os.path.join(root, rid)
            if not os.path.isdir(rdir):
                continue
            rx = load(os.path.join(bench, "receivers", rid, "receiver.json"), {}) or {}
            real = rx.get("input", {}).get("format") == "s16"
            for rep in sorted(os.listdir(rdir)):
                d = os.path.join(rdir, rep, "listen")
                if not os.path.exists(os.path.join(d, "meta.json")):
                    continue
                m = run_metrics(d, marker_real, marker_iq, os.path.join(rdir, rep, "receiver", "pace.jsonl"), real)
                with open(os.path.join(d, "metrics.json"), "w") as f:
                    json.dump(m, f, indent=1)
                per.setdefault((rid, "documented"), []).append(m)
    for rid in (sorted(os.listdir(root)) if not session else []):
        rdir = os.path.join(root, rid)
        if not os.path.isdir(rdir):
            continue
        for tier in sorted(os.listdir(rdir)):
            for rep in sorted(os.listdir(os.path.join(rdir, tier)), key=lambda x: int(x) if x.isdigit() else 0):
                d = os.path.join(rdir, tier, rep)
                m = run_metrics(d, marker_real, marker_iq)
                with open(os.path.join(d, "metrics.json"), "w") as f:
                    json.dump(m, f, indent=1)
                per.setdefault((rid, tier), []).append(m)
    table = {}
    for (rid, tier), runs in per.items():
        ok = [r for r in runs if r.get("state") == "validated"]
        k = lambda name, path: summarize([_get(r, path) for r in ok])  # noqa: E731
        table[f"{rid}/{tier}"] = {
            "runs": len(runs), "valid": len(ok),
            "invalid": [{"state": r.get("state"), "gates": r.get("gates"), "error": (r.get("error") or "")[:200]}
                        for r in runs if r.get("state") != "validated"],
            "audio_kbit": k("", ["kbit", "audio:rx", "mean"]),
            "waterfall_kbit": k("", ["kbit", "waterfall:rx", "mean"]),
            "total_payload_kbit": k("", ["kbit", "total:rx", "mean"]),
            "total_payload_kbit_p95": k("", ["kbit", "total:rx", "p95"]),
            "wire_down_kbit": k("", ["wire_kbit", "down"]),
            "wire_up_kbit": k("", ["wire_kbit", "up"]),
            "cpu_percent_one_listener": k("", ["cpu_percent"]),
            "mem_mb": k("", ["mem_mb", "mean"]),
            "page_kbytes": k("", ["page", "kbytes"]),
            "page_requests": k("", ["page", "requests"]),
            "page_load_ms": k("", ["page", "load_ms"]),
            "latency_median_ms": k("", ["latency", "median_ms"]),
            "latency_p95_ms": k("", ["latency", "p95_ms"]),
            "latency_drift_ms_per_min": k("", ["latency", "drift_ms_per_min"]),
            "markers_found_share": summarize([_get(r, ["latency", "markers"]) / _get(r, ["latency", "expected"])
                                              for r in ok if _get(r, ["latency", "expected"])]),
            "foreign_cpu_percent": k("", ["foreign_cpu_percent_rx_cores"]),
        }
    with open(os.path.join(root, "summary-metrics.json"), "w") as f:
        json.dump(table, f, indent=1)
    cols = [("audio_kbit", "audio kbit/s"), ("waterfall_kbit", "waterfall kbit/s"), ("total_payload_kbit", "total kbit/s"),
            ("wire_down_kbit", "wire down kbit/s"), ("cpu_percent_one_listener", "CPU % (1 listener)"),
            ("mem_mb", "memory MB"), ("page_kbytes", "page kB"), ("page_load_ms", "page load ms"),
            ("latency_median_ms", "latency median ms"), ("latency_p95_ms", "latency p95 ms"),
            ("latency_drift_ms_per_min", "drift ms/min")]
    lines = ["| receiver | valid | " + " | ".join(c[1] for c in cols) + " |", "|---|---|" + "---|" * len(cols)]
    for name, row in table.items():
        cells = []
        for key, _ in cols:
            s = row[key]
            cells.append("n/a" if not s else f"{s['median']:.1f} ({s['min']:.1f}–{s['max']:.1f})")
        lines.append(f"| {name} | {row['valid']}/{row['runs']} | " + " | ".join(cells) + " |")
    with open(os.path.join(root, "summary.md"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))


def _get(d, path):
    for p in path:
        if not isinstance(d, dict) or p not in d:
            return None
        d = d[p]
    return d


if __name__ == "__main__":
    main()
