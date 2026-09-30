#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Writes docs/BENCHMARKS.md from a round's summaries.

  python3 bench/analysis/report.py --round bench/runs/ROUND [--capacity bench/runs/CAPACITY]
      [--before bench/runs/ROUND_OF_AN_EARLIER_FERNSDR]

The round is a session round (harness/links.py) that analysis/session.sh
has summarised; every number here comes from one of its summary files, and
each summary sits next to the raw data it was computed from.
"""
import argparse
import json
import os

BENCH = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
REPO = os.path.dirname(BENCH)

NAMES = {
    "fernsdr": "FernSDR", "vertexsdr": "VertexSDR", "websdr": "PA3FWM WebSDR", "novasdr": "NovaSDR",
    "phantomsdr": "PhantomSDR", "phantomsdr-plus": "PhantomSDR-Plus", "phantomsdr-plus-sv1btl": "PhantomSDR-Plus (sv1btl)",
    "openwebrx": "OpenWebRX", "openwebrx-plus": "OpenWebRX+", "ka9q-web": "ka9q-radio + ka9q-web", "ubersdr": "UberSDR",
}
LINEAGE = {"vertexsdr", "novasdr", "phantomsdr-plus"}


def load(path, default=None):
    try:
        with open(path) as f:
            return json.load(f)
    except (OSError, ValueError):
        return default


def med(s, fmt="{:.0f}", none="n/a"):
    if not s:
        return none
    if isinstance(s, (int, float)):
        return fmt.format(s)
    if s.get("n", 1) == 1 or abs(s["max"] - s["min"]) < 1e-9:
        return fmt.format(s["median"])
    return f"{fmt.format(s['median'])} ({fmt.format(s['min'])} to {fmt.format(s['max'])})"


def order(root):
    """Receivers by listener latency, FernSDR's lineage marked in the names."""
    ov = load(os.path.join(root, "summary-overview.json"), {})
    key = lambda r: ov.get(r, {}).get("latency median ms") or 1e9  # noqa: E731
    return sorted(ov, key=key)


def name(rid):
    return NAMES.get(rid, rid) + (" *" if rid in LINEAGE else "")


def table(head, rows):
    return "\n".join(["| " + " | ".join(head) + " |", "|" + "---|" * len(head)] + ["| " + " | ".join(r) + " |" for r in rows])


def versions():
    rows = []
    for rid in NAMES:
        rx = load(os.path.join(BENCH, "receivers", rid, "receiver.json"), {})
        v = rx.get("version", {})
        ref = v.get("tag") or v.get("ref") if v.get("tag") else (v.get("commit", "")[:10] or rx.get("image", ""))
        inp = rx.get("input", {})
        kind = "20.48 Msps real" if inp.get("format") == "s16" else "2.048 Msps IQ"
        rows.append([name(rid), f"`{ref}`", f"{kind} ({inp.get('format')}, {inp.get('transport')})"])
    return table(["Receiver", "Version", "Input"], rows)


def overview(root, rids):
    ov = load(os.path.join(root, "summary-overview.json"), {})
    links = load(os.path.join(root, "summary-links.json"), {})
    rows = []
    for rid in rids:
        o = ov[rid]
        f = lambda k, p="{:.0f}": "n/a" if o.get(k) is None else p.format(o[k])  # noqa: E731
        r24 = (links.get(rid, {}).get("rate24") or {}).get("markers_share")
        rec = (links.get(rid, {}).get("out15") or {}).get("recovery_s")
        rows.append([name(rid), o.get("valid jobs") or "n/a", f("latency median ms"), f("audio kbit/s", "{:.1f}"),
                     f("waterfall kbit/s", "{:.1f}"), f("page kB"), f("CPU % 4 listeners", "{:.1f}"),
                     f("memory MB 4 listeners"), f("SNR dB", "{:.1f}"), f("SINAD dB", "{:.1f}"),
                     f("FT8 threshold dB"), med(r24, "{:.2f}"), "never" if not rec else med(rec, "{:.1f}")])
    return table(["Receiver", "Valid jobs", "Latency ms", "Audio kbit/s", "Waterfall kbit/s", "Page kB",
                  "CPU % (4 listeners)", "Memory MB", "SNR dB", "SINAD dB", "FT8 threshold dB",
                  "Markers heard at 24 kbit/s", "Back to normal latency after a 15 s outage, s"], rows)


def listen(root, rids):
    t = {k.split("/")[0]: v for k, v in (load(os.path.join(root, "summary-metrics.json"), {}) or {}).items()}
    rows = []
    for rid in rids:
        r = t.get(rid)
        if not r:
            continue
        rows.append([name(rid), f"{r['valid']}/{r['runs']}", med(r["latency_median_ms"]), med(r["latency_p95_ms"]),
                     med(r["audio_kbit"], "{:.1f}"), med(r["waterfall_kbit"], "{:.1f}"), med(r["total_payload_kbit"], "{:.1f}"),
                     med(r["page_kbytes"]), med(r["page_load_ms"])])
    return table(["Receiver", "Runs", "Latency median ms", "Latency p95 ms", "Audio kbit/s", "Waterfall kbit/s",
                  "Total kbit/s", "Page kB", "Page load ms"], rows)


def resources(root, rids):
    t = load(os.path.join(root, "summary-resources.json"), {})
    rows = [[name(r), med(t[r]["idle_cpu"], "{:.1f}"), med(t[r]["busy_cpu"], "{:.1f}"), med(t[r]["idle_mem"]), med(t[r]["busy_mem"])]
            for r in rids if r in t]
    return table(["Receiver", "CPU % idle", "CPU % with 4 listeners", "Memory MB idle", "Memory MB with 4 listeners"], rows)


def compression(root, rids):
    t = load(os.path.join(root, "summary-compression.json"), {})
    codecs = load(os.path.join(BENCH, "desk", "codecs.json"), {})
    rows = []
    notes = []
    for rid in rids:
        r = t.get(rid)
        if not r:
            continue
        c = codecs.get(rid, {})
        yn = lambda v: "yes" if v else "no" if v is False else "?"  # noqa: E731
        rows.append([name(rid), (c.get("audio") or {}).get("codec", "?").split(",")[0], yn(r["audio_lossy"]), med(r["audio_kbit"], "{:.1f}"),
                     med(r["waterfall_kbit"], "{:.1f}"), med(r["waterfall_frames_per_s"], "{:.1f}"),
                     str(r["waterfall_bins_per_row"] or "n/a"), med(r["waterfall_bytes_per_frame"]),
                     "n/a" if "waterfall_bits_per_bin" not in r else f"{r['waterfall_bits_per_bin']:.2f}"])
        w = c.get("waterfall") or {}
        notes.append(f"- **{name(rid)}**: audio {c.get('audio', {}).get('codec', '?')}"
                     + (f" ({c['audio']['notes']})" if c.get("audio", {}).get("notes") else "")
                     + f"; waterfall {w.get('codec', '?')}"
                     + (f"; lossy part: {w['lossy_part']}" if w.get("lossy_part") else "")
                     + (f" ({w['notes']})" if w.get("notes") else "") + f". Source: {w.get('source', c.get('audio', {}).get('source', '?'))}.")
    return (table(["Receiver", "Audio codec", "Lossy", "Audio kbit/s", "Waterfall kbit/s", "Rows/s", "Bins/row", "Bytes/row", "Bits/bin"], rows)
            + "\n\n" + "\n".join(notes))


def quality(root, rids):
    t = load(os.path.join(root, "summary-quality.json"), {})
    tones = ["200", "300", "2700", "2900", "3200"]
    rows = []
    for rid in ["ideal receiver"] + rids:
        v = t.get(rid)
        if not v:
            continue
        rows.append([name(rid) if rid in NAMES else rid, med(v["snr_db"], "{:.1f}"), med(v["sinad_db"], "{:.1f}"),
                     med(v["pitch_error_hz"], "{:+.1f}"), med(v["scale_error_ppm"], "{:+.0f}"), med(v["pitch_wander_hz"], "{:.2f}")]
                    + [med(v["passband_db"][x], "{:.1f}") for x in tones])
    return table(["Receiver", "SNR dB", "SINAD dB", "Pitch error Hz", "Scale error ppm", "Pitch wander Hz"]
                 + [f"{x} Hz dB" for x in tones], rows)


def digital(root, rids):
    t = load(os.path.join(root, "summary-digital.json"), {})
    rows = []
    o = t.get("oracle") or {}
    if o:
        rt, cw = o.get("rtty_score") or {}, o.get("cw_score") or {}
        f = lambda v: "n/a" if v is None else f"{v:.2f}"  # noqa: E731
        share = o["ft8_decodes"] / o["ft8_opportunities"] if o.get("ft8_opportunities") else None
        rows.append(["ideal receiver", str(o.get("ft8_threshold_db")), f(share), f(rt.get("-6")), f(rt.get("4")),
                     f(cw.get("10")), f(cw.get("20"))])
    for rid in rids:
        v = t.get(rid)
        if not v:
            continue
        rows.append([name(rid), med(v["ft8_threshold_db"]), med(v["ft8_decode_share"], "{:.2f}"), med(v["rtty_low"], "{:.2f}"),
                     med(v["rtty_high"], "{:.2f}"), med(v["cw_low"], "{:.2f}"), med(v["cw_high"], "{:.2f}")])
    return table(["Receiver", "FT8 threshold dB", "FT8 decoded", "RTTY -6 dB", "RTTY +4 dB", "CW +10 dB", "CW +20 dB"], rows)


def links(root, rids):
    t = load(os.path.join(root, "summary-links.json"), {})
    profiles = ["listen", "loss2", "wifi", "hold1s", "drop1s", "cell", "rtt600", "rate32", "rate24", "out15"]
    out = []
    for metric, label, fmt in (("markers_share", "Share of markers heard", "{:.2f}"),
                               ("latency_median_ms", "Latency median, ms", "{:.0f}"),
                               ("latency_p95_ms", "Latency p95, ms", "{:.0f}"),
                               ("dropouts", "Audio dropouts, seconds in the 30 s window", "{:.1f}"),
                               ("waterfall_fps", "Waterfall rows a second", "{:.1f}"),
                               ("kbit", "Payload kbit/s", "{:.0f}")):
        rows = [[name(r)] + [med((t.get(r, {}).get(p) or {}).get(metric), fmt) for p in profiles] for r in rids]
        out += [f"### {label}", "", table(["Receiver"] + profiles, rows), ""]
    return "\n".join(out)


def validity(root, rids):
    v = load(os.path.join(root, "summary-validity.json"), {})
    lines = []
    for rid in rids:
        x = v.get(rid) or {}
        parts = []
        if x.get("invalid"):
            parts.append("invalid: " + "; ".join(f"{p} ({', '.join(w[:2])})" for p, w in x["invalid"].items()))
        if x.get("no_audio_on_link"):
            parts.append("played nothing on: " + ", ".join(x["no_audio_on_link"]))
        lost = {k: round(s, 1) for k, s in (x.get("input_dropped_session_s") or {}).items() if s}
        if lost:
            parts.append("input not taken outside the windows, s: " + ", ".join(f"{k} {s}" for k, s in lost.items()))
        if parts:
            lines.append(f"- {name(rid)}: " + "; ".join(parts) + ".")
    return "\n".join(lines) or "Every job of every receiver was valid."


def why(last, m):
    if not last:
        return m.get("error", "n/a")
    if last.get("passed"):
        return "passed every step"
    return (f"{last['listeners']}: {last.get('closed_early')} disconnected, "
            f"{last.get('audio_ok_share', 0):.2f} got their audio" + (", receiver ended" if last.get("receiver_lost") else ""))


def capacity(root, extra=None):
    if not root:
        return "Not run yet."
    rows = []
    for rid in sorted(os.listdir(root)):
        m = load(os.path.join(root, rid, "rep1", "manifest.json"))
        if not m:
            continue
        steps = [s for s in m.get("steps", []) if "listeners" in s]
        first = steps[0] if steps else {}
        passed = [s for s in steps if s.get("passed")]
        top = passed[-1] if passed else {}
        last = steps[-1] if steps else {}
        knee = str(m.get("knee", "n/a"))
        # Further rounds for receivers that passed every step: the highest
        # step passed anywhere, and the lowest failed step above it.
        if last.get("passed"):
            ext = []
            for root_extra in extra or []:
                more = load(os.path.join(root_extra, rid, "rep1", "manifest.json"))
                ext += [s for s in (more or {}).get("steps", []) if "listeners" in s]
            ok = [s for s in ext if s.get("passed")]
            if ok:
                best = max(ok, key=lambda s: s["listeners"])
                if best["listeners"] > top.get("listeners", 0):
                    top = best
                    knee = str(best["listeners"])
            above = [s for s in ext if not s.get("passed") and s["listeners"] > top.get("listeners", 0)]
            if above:
                last = min(above, key=lambda s: s["listeners"])
            elif ext:
                last = top
        if last.get("passed"):
            knee += " or more"
        per = "n/a"
        if top.get("listeners", 1) > 1 and top.get("cpu_percent") is not None and first.get("cpu_percent") is not None:
            per = f"{(top['cpu_percent'] - first['cpu_percent']) / (top['listeners'] - 1):.2f}"
        caps = (load(os.path.join(BENCH, "receivers", rid, "receiver.json"), {}) or {}).get("caps") or {}
        cap = f"{caps['listeners']}, compiled in" if caps.get("compile_time") and caps.get("listeners") else "raised"
        rows.append([name(rid), knee, cap, med(top.get("cpu_percent"), "{:.0f}") if top else "n/a",
                     med(top.get("mem_mb")) if top else "n/a", per,
                     "yes" if (top or first).get("spread_share", 0) >= 0.9 else "no, one frequency", why(last, m)])
    return table(["Receiver", "Listeners served (last step passed)", "Listener cap", "CPU % there", "Memory MB there",
                  "CPU % per extra listener", "Own frequency per listener", "Next step"], rows)


def runs_said(root):
    """How often the round measured each receiver, from its metrics."""
    m = load(os.path.join(root, "summary-metrics.json"), {}) or {}
    runs = {k.split("/")[0]: v.get("runs") for k, v in m.items()}
    counts = sorted({n for n in runs.values() if n})
    if len(counts) == 1:
        n = counts[0]
        return f"Each receiver was measured {n} time{'s' if n != 1 else ''} in this round"
    return "Runs per receiver: " + ", ".join(f"{name(r)} {n}" for r, n in runs.items())


def before_after(before, after):
    if not before:
        return ""
    b = load(os.path.join(before, "summary-overview.json"), {}).get("fernsdr")
    a = load(os.path.join(after, "summary-overview.json"), {}).get("fernsdr")
    if not b or not a:
        return ""
    keys = [("latency median ms", "{:.0f}"), ("latency p95 ms", "{:.0f}"), ("page load ms", "{:.0f}"),
            ("CPU % idle", "{:.1f}"), ("CPU % 4 listeners", "{:.1f}"), ("waterfall kbit/s", "{:.1f}"),
            ("waterfall bits/bin", "{:.2f}"), ("SINAD dB", "{:.1f}"), ("SNR dB", "{:.1f}"),
            ("pitch wander Hz", "{:.2f}"), ("scale error ppm", "{:+.0f}"), ("FT8 threshold dB", "{:.0f}"), ("RTTY -6 dB", "{:.2f}"),
            ("CW +10 dB", "{:.2f}"), ("24 kbit/s markers", "{:.2f}")]
    rows = [[k, "n/a" if b.get(k) is None else f.format(b[k]), "n/a" if a.get(k) is None else f.format(a[k])] for k, f in keys]
    return table(["FernSDR", "Before", "After"], rows)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--round", required=True)
    ap.add_argument("--capacity")
    ap.add_argument("--capacity-extra", action="append", help="rounds with further steps for receivers that passed them all")
    ap.add_argument("--before")
    a = ap.parse_args()
    rids = order(a.round)
    rel = os.path.relpath(a.round, REPO)
    note = (load(os.path.join(a.round, "round.json"), {}) or {}).get("note")
    parts = [
        "# Benchmarks", "",
        f"FernSDR against ten other web SDR receivers, measured in the round `{rel}` in the lab in `bench/`: identical "
        "generated input, one receiver at a time on two pinned cores, real browsers as listeners. How it was "
        f"done is in [bench/METHOD.md](../bench/METHOD.md); every number below comes from a summary file in `{rel}/`, "
        "next to the raw data it was computed from.", "",
        "Receivers marked * (VertexSDR, NovaSDR, PhantomSDR-Plus) are earlier projects of FernSDR's author, and "
        "every adapter and configuration in the lab was written by that author; they are in `bench/receivers/` "
        f"for anyone to check. {runs_said(a.round)}: a difference smaller than the spread a receiver's own runs "
        "show is not a difference. Where another receiver does better, the tables say so."
        + (f" The round's own note: {note}" if note else ""), "",
        "## Versions", "", versions(), "",
        "## Overview", "",
        "Sorted by latency. Four listeners at a time on one receiver: a plain one (latency, bitrate), one tuned to "
        "a digital-mode channel, one to ten test tones, and one on an emulated link; the tables below have the rest.", "",
        overview(a.round, rids), "",
        "## Latency, bitrate and page", "",
        "The plain listener, 30 s after a 10 s warm-up. Latency runs from the moment a sample leaves the lab's "
        "sample source to the moment the browser expects it at its audio output (uncertainty about 10 ms).", "",
        listen(a.round, rids), "",
        "## Compression", "",
        "Measured on the plain listener's sockets; codec facts from each project's code at the pinned version "
        "(`bench/desk/codecs.json`). Bits a bin is the waterfall payload per row over the bins a row carries.", "",
        compression(a.round, rids), "",
        "## Audio quality", "",
        "Ten carriers of equal level heard as tones from 200 to 3200 Hz in USB, over the scene's noise. SNR is the "
        "tones between 300 and 2700 Hz against the noise 25 Hz or more away from any tone; SINAD against everything "
        "that is not a tone, so a tone smeared by a wandering playback speed lowers SINAD only. Pitch wander is how "
        "far the 1000 Hz tone moves between 1 s windows. The ideal receiver is the same measure on the scene itself.", "",
        quality(a.round, rids), "",
        "## Digital modes", "",
        "FT8 at -24 to -4 dB, RTTY at -6 and +4 dB and CW at +10 and +20 dB (SNR in 2.5 kHz) in one USB channel, "
        "decoded by jt9, minimodem and multimon-ng from what each page played. RTTY and CW are character "
        "accuracy. The ideal receiver decodes the scene's own audio.", "",
        digital(a.round, rids), "",
        "## Server CPU and memory", "",
        "All of the receiver's processes, in percent of one core; idle is with the input running and nobody "
        "listening, the second column with the first four listeners.", "",
        resources(a.round, rids), "",
        "## Slow, lossy and stalling links", "",
        "Profiles: loss2 2 % loss each way; wifi micro-stalls; hold1s and drop1s a 1 s hold or loss every 10 s; "
        "cell a cell change every 20 s; rtt600 600 ms round trip; rate32 and rate24 a 32 or 24 kbit/s bottleneck "
        "with a 150 ms queue; out15 a 15 s outage (half the window, so 0.5 of the markers is everything).", "",
        links(a.round, rids), "",
        "## Capacity", "",
        "One browser listens; then light clients repeat what that page sent, each on its own frequency where the "
        "protocol allows (where it does not, all share one frequency, an easier load for a receiver that shares work "
        "between them), in steps of 1, 10, 50, 100, 200 and 400 listeners on the receiver's two cores, and for "
        "those that passed them all the further steps of the rounds given with --capacity-extra. A step "
        "passes when 95 % of the listeners get at least 90 % of the browser's audio messages and none is "
        "disconnected; caps an operator would raise are raised.", "",
        capacity(a.capacity, a.capacity_extra), "",
        "## Excluded jobs and findings", "",
        validity(a.round, rids), "",
        "Notes on single receivers, from the rounds that led to this one; the tables above are what this round "
        "measured:", "",
        "- Share of markers heard, for ka9q-web on loss2, wifi, drop1s and cell and for UberSDR on wifi, drop1s, cell and "
        "rate32: the audio arrived at close to its plain rate (summary-links.json, audio_kbit) but the markers' code "
        "was not found in it, so the audio was changed in time or shape, not lost. Both use Opus.",
        "- ka9q-web and UberSDR take a 20.48 Msps real band, the others 2.048 Msps IQ: the same signals and noise "
        "density, but a separate input, so their SNR and decode figures compare with each other more than with the rest.",
        "- PhantomSDR-Plus (sv1btl): retuning through the page's CAT API sends the band plan's mode and then the "
        "previous one within milliseconds, and the server drops a mode command that follows another within 100 ms, "
        "so it stays in LSB while the page shows USB. The adapter sets the mode apart from the retune.",
        "- UberSDR and OpenWebRX+ change their layout in a small window; their side listeners use 1280x800.",
        "- OpenWebRX and OpenWebRX+ stop reading the input while nobody listens; that is the input they did not take.",
        "- Capacity: the lab gives each receiver 1 GB of memory. OpenWebRX and OpenWebRX+ at 100 listeners (about "
        "900 MB of shared memory) and UberSDR at 400 were ended by that limit, not by their two cores (the kernel's "
        "out-of-memory log); with more memory they would serve more. ka9q-web serves five clients at most, a limit "
        "compiled into it.",
        "- Before 3415639 FernSDR, like PA3FWM's WebSDR, stopped accepting at about 1017 sockets, the soft limit on "
        "open files the containers (and a systemd service by default) start with; FernSDR now raises it. In the "
        "earlier capacity rounds B3 and B4w, not kept, WebSDR's 800-listener step failed at that limit, so its own "
        "is not known.",
        "- The tones of PhantomSDR, PhantomSDR-Plus, sv1btl and NovaSDR sit 15.6 Hz low: one FFT bin of their "
        "tuning grid.", "",
    ]
    ba = before_after(a.before, a.round)
    if ba:
        parts += ["## What these measurements changed in FernSDR", "",
                  "The first round showed FernSDR's player steering its speed on every packet (a pitch warble that "
                  "cost SINAD and weak-signal decodes), a 4096-frame audio block on plain-HTTP pages, a limit on open "
                  "files that capped it at about a thousand connections, and a waterfall coded at 1.63 bits a bin. "
                  "All four were changed (the last by WFC5, a range-coded waterfall format, docs/CODEC.md) and FernSDR "
                  "measured again. A second pass took on what it still lost: an idle band now skips the listeners' "
                  "transform and makes four waterfall lines a second, the player gives back the buffer a one-off "
                  "break took once audio arrives evenly again, listener sockets keep their retransmission timeouts "
                  "linear, and the page no longer forces a layout while it mounts. The other receivers were not "
                  "changed.", "", ba, ""]
    parts += ["## What was not measured", "",
              "Real radio hardware, ARM boards, phones and real cellular paths; HTTPS pages; stability over days. "
              "See METHOD.md.", ""]
    text = "\n".join(parts)
    with open(os.path.join(REPO, "docs", "BENCHMARKS.md"), "w") as f:
        f.write(text)
    print(f"wrote docs/BENCHMARKS.md ({len(text)} bytes)")


if __name__ == "__main__":
    main()
