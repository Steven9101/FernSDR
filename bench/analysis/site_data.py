#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""The release page's numbers, from a round's summaries (analysis/session.sh
has written them) and the capacity rounds.

  python3 bench/analysis/site_data.py --round bench/runs/ROUND --capacity bench/runs/CAP \
      [--capacity-extra bench/runs/MORE ...] --out site/src/data/benchmarks.json

Nothing is typed in by hand: every value the page shows is a field of this
file, and every field comes from a summary next to its raw data.
"""
import argparse
import json
import os
import subprocess

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH = os.path.dirname(HERE)
REPO = os.path.dirname(BENCH)

NAMES = {
    "fernsdr": "FernSDR", "vertexsdr": "VertexSDR", "websdr": "PA3FWM WebSDR", "novasdr": "NovaSDR",
    "phantomsdr": "PhantomSDR", "phantomsdr-plus": "PhantomSDR-Plus", "phantomsdr-plus-sv1btl": "PhantomSDR-Plus (sv1btl)",
    "openwebrx": "OpenWebRX", "openwebrx-plus": "OpenWebRX+", "ka9q-web": "ka9q-radio + ka9q-web", "ubersdr": "UberSDR",
}
# Earlier projects of FernSDR's author.
LINEAGE = {"vertexsdr", "novasdr", "phantomsdr-plus"}
# What stopped a capacity step where the step's numbers alone would mislead,
# each with the evidence it rests on.
CAPACITY_NOTES = {
    # 800 listeners open 1600 sockets; in capacity-B3 and -B4w exactly 1017 of
    # them connected, as FernSDR's did at 1600 before it raised its own
    # limit (3415639): the containers' soft limit of 1024 open files.
    "websdr": "the lab's limit of 1024 open files at 800",
    # The kernel's out-of-memory log (dmesg) during capacity-B at 400 and 100.
    "ubersdr": "ended by the 1 GB memory limit at 400",
    "openwebrx": "ended by the 1 GB memory limit at 100",
    "openwebrx-plus": "ended by the 1 GB memory limit at 100",
}
PROFILES = ["listen", "loss2", "wifi", "hold1s", "drop1s", "cell", "rtt600", "rate32", "rate24", "out15"]


def load(path, default=None):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


def med(s):
    """The median of a summary, or a plain number, or None."""
    if s is None:
        return None
    if isinstance(s, (int, float)):
        return float(s)
    return s.get("median")


def spread(s):
    if not isinstance(s, dict) or s.get("n", 1) < 2:
        return None
    return [s["min"], s["max"]]


def unrecognised(links, profile):
    """Audio arrived at over half the plain listener's rate, yet under a fifth
    of the markers were found in it: the audio was altered in time or shape,
    not lost, and "share heard" would misstate it."""
    x, base = links.get(profile) or {}, links.get("listen") or {}
    markers, audio, plain = med(x.get("markers_share")), med(x.get("audio_kbit")), med(base.get("audio_kbit"))
    return bool(markers is not None and markers < 0.2 and audio and plain and audio >= 0.5 * plain)


def capacity(rid, root, extras):
    m = load(os.path.join(root, rid, "rep1", "manifest.json"))
    if not m:
        return None
    steps = [s for s in m.get("steps", []) if "listeners" in s]
    passed = [s for s in steps if s.get("passed")]
    top = passed[-1] if passed else None
    last = steps[-1] if steps else None
    if last and last.get("passed"):
        more = []
        for root_extra in extras:
            e = load(os.path.join(root_extra, rid, "rep1", "manifest.json"))
            more += [s for s in (e or {}).get("steps", []) if "listeners" in s]
        ok = [s for s in more if s.get("passed")]
        if ok and (not top or max(ok, key=lambda s: s["listeners"])["listeners"] > top["listeners"]):
            top = max(ok, key=lambda s: s["listeners"])
        above = [s for s in more if not s.get("passed") and s["listeners"] > (top or {}).get("listeners", 0)]
        last = min(above, key=lambda s: s["listeners"]) if above else top
    caps = (load(os.path.join(BENCH, "receivers", rid, "receiver.json"), {}) or {}).get("caps") or {}
    first = steps[0] if steps else {}
    per = None
    if top and top["listeners"] > 1 and top.get("cpu_percent") is not None and first.get("cpu_percent") is not None:
        per = (top["cpu_percent"] - first["cpu_percent"]) / (top["listeners"] - 1)
    return {
        "listeners": top["listeners"] if top else 0,
        "or_more": bool(last and last.get("passed")),
        "cpu_percent": top.get("cpu_percent") if top else None,
        "memory_mb": top.get("mem_mb") if top else None,
        "cpu_per_listener": per,
        "own_frequency": bool(top and top.get("spread_share", 0) >= 0.9),
        "compiled_cap": caps.get("listeners") if caps.get("compile_time") else None,
        "limit_note": CAPACITY_NOTES.get(rid),
        "next_step": None if not last or last.get("passed") else {
            "listeners": last["listeners"], "disconnected": last.get("closed_early"),
            "got_audio": last.get("audio_ok_share"), "receiver_ended": bool(last.get("receiver_lost"))},
    }


def features(rid):
    """What the receiver can do, from its inventory: each answer there cites
    the line of source or documentation it rests on, at a pinned commit."""
    inv = load(os.path.join(BENCH, "inventory", f"{rid}.json"), {}) or {}
    return {
        "checked": inv.get("checked"),
        "rows": {k: {"value": v.get("value"), "cell": v.get("cell", "")}
                 for k, v in (inv.get("features") or {}).items() if v.get("value") in ("yes", "partial", "no", "unknown")},
    }


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--round", required=True)
    ap.add_argument("--capacity", required=True)
    ap.add_argument("--capacity-extra", action="append", default=[])
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    s = {k: load(os.path.join(a.round, f"summary-{k}.json"), {}) for k in
         ("overview", "links", "quality", "digital", "compression", "resources", "validity")}
    metrics = {k.split("/")[0]: v for k, v in (load(os.path.join(a.round, "summary-metrics.json"), {}) or {}).items()}
    codecs = load(os.path.join(BENCH, "desk", "codecs.json"), {})
    receivers = []
    for rid in NAMES:
        o = s["overview"].get(rid)
        if not o:
            continue
        rx = load(os.path.join(BENCH, "receivers", rid, "receiver.json"), {}) or {}
        ver = rx.get("version", {})
        m, q, d, c, r, l = (metrics.get(rid, {}), s["quality"].get(rid, {}), s["digital"].get(rid, {}),
                            s["compression"].get(rid, {}), s["resources"].get(rid, {}), s["links"].get(rid, {}))
        pb = q.get("passband_db") or {}
        receivers.append({
            "id": rid, "name": NAMES[rid], "earlier_project": rid in LINEAGE,
            "version": ver.get("tag") or (ver.get("commit") or "")[:7],
            "upstream": rx.get("upstream"),
            "input": "20.48 Msps real" if rx.get("input", {}).get("format") == "s16" else "2.048 Msps IQ",
            "runs": m.get("runs"), "valid_jobs": o.get("valid jobs"),
            "latency_ms": med(m.get("latency_median_ms")), "latency_ms_range": spread(m.get("latency_median_ms")),
            "latency_p95_ms": med(m.get("latency_p95_ms")),
            "audio_kbit": med(c.get("audio_kbit")), "waterfall_kbit": med(c.get("waterfall_kbit")),
            "total_kbit": med(m.get("total_payload_kbit")),
            "waterfall_rows_per_s": med(c.get("waterfall_frames_per_s")), "waterfall_bins": c.get("waterfall_bins_per_row"),
            "waterfall_bits_per_bin": c.get("waterfall_bits_per_bin"),
            "audio_codec": (codecs.get(rid, {}).get("audio") or {}).get("codec", "").split(",")[0].split(" (")[0],
            "audio_lossy": c.get("audio_lossy"),
            "waterfall_codec": (codecs.get(rid, {}).get("waterfall") or {}).get("codec", "").split(":")[0].split(",")[0],
            "page_kb": med(m.get("page_kbytes")), "page_load_ms": med(m.get("page_load_ms")),
            "cpu_idle": med(r.get("idle_cpu")), "cpu_four": med(r.get("busy_cpu")),
            "memory_mb": med(r.get("busy_mem")),
            "snr_db": med(q.get("snr_db")), "sinad_db": med(q.get("sinad_db")),
            "pitch_error_hz": med(q.get("pitch_error_hz")), "pitch_wander_hz": med(q.get("pitch_wander_hz")),
            "passband_db": {k: med(pb.get(k)) for k in ("200", "300", "2700", "2900")},
            "ft8_threshold_db": med(d.get("ft8_threshold_db")), "ft8_decoded": med(d.get("ft8_decode_share")),
            "rtty_minus6": med(d.get("rtty_low")), "cw_plus10": med(d.get("cw_low")),
            "links": {p: {"markers": med((l.get(p) or {}).get("markers_share")),
                          "latency_ms": med((l.get(p) or {}).get("latency_median_ms")),
                          "dropout_s": med((l.get(p) or {}).get("dropouts")),
                          "unrecognised": unrecognised(l, p)} for p in PROFILES},
            "outage_recovery_s": med((l.get("out15") or {}).get("recovery_s")),
            "capacity": capacity(rid, a.capacity, a.capacity_extra),
            "features": features(rid),
        })
    ideal_q = s["quality"].get("ideal receiver", {})
    oracle = s["digital"].get("oracle", {})
    commit = subprocess.run(["git", "-C", REPO, "log", "-1", "--format=%h %cs"], capture_output=True, text=True).stdout.split()
    out = {
        "generated_from": {"round": os.path.relpath(a.round, REPO), "capacity": os.path.relpath(a.capacity, REPO),
                           "capacity_extra": [os.path.relpath(x, REPO) for x in a.capacity_extra],
                           "commit": commit[0] if commit else None, "date": commit[1] if len(commit) > 1 else None},
        "ideal": {"snr_db": med(ideal_q.get("snr_db")), "sinad_db": med(ideal_q.get("sinad_db")),
                  "ft8_threshold_db": oracle.get("ft8_threshold_db"),
                  "ft8_decoded": (oracle["ft8_decodes"] / oracle["ft8_opportunities"]) if oracle.get("ft8_opportunities") else None,
                  "rtty_minus6": (oracle.get("rtty_score") or {}).get("-6"), "cw_plus10": (oracle.get("cw_score") or {}).get("10")},
        "receivers": receivers,
    }
    os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
    with open(a.out, "w") as f:
        json.dump(out, f, indent=1)
        f.write("\n")
    print(f"wrote {a.out}: {len(receivers)} receivers")


if __name__ == "__main__":
    main()
