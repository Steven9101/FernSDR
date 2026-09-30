#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""What each receiver's listeners got over each emulated link.

Per receiver, repetition and profile (bench/harness/links.py), inside the
measurement window: the share of markers heard (a marker lost means the
audio broke while it was on the air), their latency (median, p95, max; a
player that never catches up after a stall shows here), waterfall frames
per second, payload kbit/s, audio dropouts, whether the page opened new
sockets (a reconnect), and after the 15 s outage how long until the first
marker was heard again.

Dropouts are read from the channel of known noise audio (1200 to 2600 Hz):
a dropout is at least 40 ms in which that band's level stays 20 dB under
its median. Concealment that fills a gap with something noise-like hides a
gap from this measure; the missing markers still show it.

  python3 bench/analysis/links_report.py bench/runs/ROUND
"""
import json
import os
import sys
from urllib.parse import urlparse

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
from listen_report import bins_kbit, load, summarize  # noqa: E402


def dropouts(x, rate):
    n = len(x)
    if n < rate:
        return None
    spec = np.fft.rfft(x)
    f = np.fft.rfftfreq(n, 1 / rate)
    spec[(f < 1300) | (f > 2500)] = 0
    y = np.fft.irfft(spec, n)
    w = int(rate * 0.02)
    e = np.array([np.mean(y[i:i + w] ** 2) for i in range(0, n - w, w)])
    if not len(e) or np.median(e) <= 0:
        return {"count": None, "seconds": None}
    low = e < np.median(e) * 0.01
    count, seconds, run = 0, 0.0, 0
    for v in list(low) + [False]:
        if v:
            run += 1
        else:
            if run >= 2:
                count += 1
                seconds += run * 0.02
            run = 0
    return {"count": count, "seconds": seconds}


def input_dropped(rep_dir, t0_ms=None, t1_ms=None):
    """Seconds of input pace could not hand the receiver, inside [t0_ms,
    t1_ms] (wall clock) when given: dropped because the receiver fell
    behind real time ("full"), because nothing had the FIFO open
    ("absent"), or queued for a receiver that went away ("discarded").
    Any of them inside a measurement window makes that window
    invalid; outside, it is a finding about the receiver (a stall when
    listeners connect, a source that only runs while someone listens)."""
    out = {"full": 0.0, "absent": 0.0, "discarded": 0.0}
    try:
        with open(os.path.join(rep_dir, "receiver", "pace.jsonl")) as f:
            events = [json.loads(line) for line in f]
    except (OSError, ValueError):
        return None
    start = next((e for e in events if e.get("ev") == "start"), None)
    if not start:
        return None
    t0 = start["t0_real_ns"] / 1e9
    rate = start["clock"]
    for e in events:
        if e.get("ev") != "drop" or e.get("why") not in out:
            continue
        a = t0 + e["first"] / rate
        b = a + e["samples"] / rate
        if t0_ms is not None:
            a, b = max(a, t0_ms / 1000), min(b, t1_ms / 1000)
        if b > a:
            out[e["why"]] += b - a
    return out


def outage_recovery(markers, events):
    """After an outage: seconds until the first marker was heard, and until
    the first one within 1 s of the latency before the outage. Heard again is
    not recovered: a player that queues what arrives after the outage plays
    it at the old delay plus the outage, as NovaSDR did 16 s late."""
    on = [e["t"] for e in events if e["what"] == "outage on"]
    off = [e["t"] for e in events if e["what"] == "outage off"]
    if not on or not off or not markers:
        return {}
    before = [m["to_destination_ms"] for m in markers if m["wall"] < on[0]]
    after = sorted((m for m in markers if m["wall"] > off[0]), key=lambda m: m["wall"])
    out = {"first_heard_s": (after[0]["wall"] - off[0]) if after else None}
    if before:
        base = float(np.median(before))
        ok = [m["wall"] for m in after if m["to_destination_ms"] <= base + 1000]
        out["recovery_s"] = (ok[0] - off[0]) if ok else None
    return out


def profile_metrics(d, marker, rate):
    meta = load(os.path.join(d, "meta.json"), {})
    link = load(os.path.join(d, "link.json"), {})
    t0, t1 = meta.get("measurementStartedWallMs"), meta.get("endedWallMs")
    out = {"exit": link.get("exit"), "gates": meta.get("gates"),
           "input_dropped_s": input_dropped(os.path.dirname(d), t0, t1) if t0 and t1 else None,
           "input_dropped_session_s": input_dropped(os.path.dirname(d))}
    if not t0 or not t1:
        out["error"] = meta.get("error") or "no window"
        return out
    socks = load(os.path.join(d, "sockets.json"), [])
    total = None
    audio = None
    wf_frames = 0
    for s in socks:
        for k, v in s["streams"].items():
            if k.endswith(":rx"):
                b = bins_kbit(v["times"], v.get("sizes", []), t0, t1)
                total = b if total is None else total + b
            if k == "audio:rx":
                b = bins_kbit(v["times"], v.get("sizes", []), t0, t1)
                audio = b if audio is None else audio + b
            if k == "waterfall:rx":
                wf_frames += sum(1 for t in v["times"] if t0 <= t <= t1)
    secs = (t1 - t0) / 1000
    out["kbit"] = float(total.mean()) if total is not None and len(total) else 0.0
    # Audio alone: a page can keep receiving a waterfall while its audio has
    # stopped, and markers can go unrecognised in audio that did arrive.
    out["audio_kbit"] = float(audio.mean()) if audio is not None and len(audio) else 0.0
    out["waterfall_fps"] = wf_frames / secs
    # Only the receiver's own sockets: sv1btl's page probes CAT bridges on
    # localhost every few seconds, which is not a reconnect.
    host = urlparse(meta.get("url") or "").hostname
    out["sockets_opened_in_window"] = sum(1 for s in socks if s["opened"] > t0 and urlparse(s["url"]).hostname == host)
    x = np.fromfile(os.path.join(d, "audio.f32"), "<f4").astype(float) if os.path.exists(os.path.join(d, "audio.f32")) else np.zeros(0)
    info = load(os.path.join(d, "audio.json"), {})
    if len(x) and info.get("rate"):
        x[np.isnan(x)] = 0
        out["dropouts"] = dropouts(x, info["rate"])
    try:
        pace = os.path.join(os.path.dirname(d), "receiver", "pace.jsonl")
        res = latency.analyse(d, pace, marker, rate, 7159200, "usb", loop_s=30.0 if session_loop(pace) else None)
        s = res.get("summary") or {}
        out["markers_share"] = (s["markers"] / (secs / marker["period_s"])) if s else 0.0
        out["latency_median_ms"] = s.get("median_ms")
        out["latency_p95_ms"] = s.get("p95_ms")
        out["latency_max_ms"] = s.get("max_ms")
        out.update(outage_recovery(res.get("markers") or [], link.get("events", [])))
    except Exception as e:
        out["latency_error"] = str(e)[:200]
        out["markers_share"] = 0.0
    return out


def problems(prof, r):
    """Why a job is invalid, empty when it is not, and whether it played
    nothing on an impaired link. A job is valid when every gate held and pace
    handed the receiver all of its input inside the job's window."""
    why = [k for k, v in (r.get("gates") or {}).items() if not v]
    silent = False
    # On an impaired link a page that plays nothing is the receiver's result
    # (ka9q-web drops a listener whose writes stall), not a broken
    # measurement; on the plain listeners it is.
    if why == ["audio"] and prof not in ("listen", "digital", "tones"):
        silent = True
        why = []
    lost = r.get("input_dropped_s") or {}
    why += [f"input {k} {v:.1f} s" for k, v in lost.items() if v > 0]
    if r.get("error"):
        why.append(r["error"][:80])
    return why, silent


def summarise(table):
    """Medians over the valid jobs only: an invalid one says nothing about
    the receiver."""
    out = {}
    for rid, profs in table.items():
        out[rid] = {}
        for prof, runs in profs.items():
            ok = [r for r in runs if not problems(prof, r)[0]]
            out[rid][prof] = {k: summarize([r.get(k) for r in ok]) for k in
                              ("markers_share", "latency_median_ms", "latency_p95_ms", "latency_max_ms",
                               "waterfall_fps", "kbit", "audio_kbit", "recovery_s", "first_heard_s",
                               "sockets_opened_in_window")}
            out[rid][prof] |= {"dropouts": summarize([(r.get("dropouts") or {}).get("seconds") for r in ok]),
                               "runs": len(ok), "invalid_runs": len(runs) - len(ok)}
    return out


def validity_of(table):
    """Each invalid job by repetition and profile, so that three invalid
    repetitions of one profile count three times. The digital and tones
    jobs are judged here too; their own reports read the same files."""
    validity = {}
    for rid, profs in table.items():
        bad, silent = {}, []
        for prof, runs in profs.items():
            for r in runs:
                why, quiet = problems(prof, r)
                if quiet:
                    silent.append(prof)
                if why:
                    bad[f"{r.get('rep', '?')}/{prof}"] = why
        session = [r.get("input_dropped_session_s") for runs in profs.values() for r in runs]
        validity[rid] = {"jobs": sum(len(v) for v in profs.values()), "invalid": bad, "no_audio_on_link": silent,
                         "input_dropped_session_s": next((x for x in session if x), None)}
    return validity


def main():
    root = sys.argv[1]
    bench = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    import scene
    m_iq = scene.Scene(os.path.join(bench, "scenes", "markers.scene"), 1).describe()["markers"][0]
    m_real = scene.Scene(os.path.join(bench, "scenes", "markers-real.scene"), 1).describe()["markers"][0]
    table = {}
    for rid in sorted(os.listdir(root)):
        rdir = os.path.join(root, rid)
        if not os.path.isdir(rdir):
            continue
        for rep in sorted(os.listdir(rdir)):
            man = load(os.path.join(rdir, rep, "receiver", "manifest.json"), {}) or {}
            rx = load(os.path.join(bench, "receivers", rid, "receiver.json"), {})
            real = rx.get("input", {}).get("format") == "s16"
            for prof in sorted(os.listdir(os.path.join(rdir, rep))):
                d = os.path.join(rdir, rep, prof)
                if not os.path.exists(os.path.join(d, "link.json")):
                    continue
                m = profile_metrics(d, m_real if real else m_iq, 20480000 if real else 2048000)
                m["rep"] = rep
                # Kept with the job's metrics, where the digital and
                # compression reports read it.
                m["invalid"], m["no_audio_on_link"] = problems(prof, m)
                with open(os.path.join(d, "metrics.json"), "w") as f:
                    json.dump(m, f, indent=1)
                table.setdefault(rid, {}).setdefault(prof, []).append(m)
    summary = summarise(table)
    validity = validity_of(table)
    with open(os.path.join(root, "summary-validity.json"), "w") as f:
        json.dump(validity, f, indent=1)
    with open(os.path.join(root, "summary-links.json"), "w") as f:
        json.dump(summary, f, indent=1)
    # The digital and tones listeners are tuned away from the marker; they
    # have their own reports.
    order = ["listen", "rate32", "rate24", "loss2", "rtt600", "hold1s", "drop1s", "wifi", "cell", "out15", "lan", "rate64", "bloat"]
    present = {p for v in summary.values() for p in v}
    profiles = [p for p in order if p in present]
    lines = []
    for metric, label, fmt in (("markers_share", "Share of markers heard", "{:.2f}"),
                               ("latency_p95_ms", "Latency p95, ms", "{:.0f}"),
                               ("waterfall_fps", "Waterfall frames/s", "{:.1f}"),
                               ("dropouts", "Audio dropout seconds in the window", "{:.1f}"),
                               ("kbit", "Payload kbit/s", "{:.0f}")):
        lines += [f"### {label}", "", "| receiver | " + " | ".join(profiles) + " |", "|---|" + "---|" * len(profiles)]
        for rid, v in summary.items():
            cells = []
            for p in profiles:
                s = (v.get(p) or {}).get(metric)
                cells.append("n/a" if not s else fmt.format(s["median"]))
            lines.append(f"| {rid} | " + " | ".join(cells) + " |")
        lines.append("")
    rec = ["### After a 15 s outage, s (median)", "",
           "First heard: the first marker after the link came back. Recovered: the first one within 1 s of the latency before the outage.", "",
           "| receiver | first heard | recovered |", "|---|---|---|"]
    for rid, v in summary.items():
        o = v.get("out15") or {}
        cells = ["never" if not o.get(k) else format(o[k]["median"], ".1f") for k in ("first_heard_s", "recovery_s")]
        rec.append(f"| {rid} | " + " | ".join(cells) + " |")
    lines += rec
    with open(os.path.join(root, "summary-links.md"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
