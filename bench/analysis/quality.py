#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Audio quality from the tones scene: SINAD, passband and pitch.

The scene puts ten steady carriers into the band that sound, at 7.080 MHz
USB, as tones at 200, 300, 500, 1000, 1500, 2000, 2500, 2700, 2900 and
3200 Hz, all at the same RF level, over the noise floor. From the audio
the page played (Welch spectrum, 1 Hz bins over the window):

- passband: each tone's level against the 1000 Hz tone, in dB;
- SINAD: the tones inside 300 to 2700 Hz against everything else there
  (the scene's own noise, codec noise, distortion, anything the chain adds);
- SNR: the same with the noise read only 25 Hz or more away from any tone,
  so that a tone smeared by a wandering playback rate (wow and flutter)
  lowers SINAD but not SNR;
- pitch error of the 1000 Hz tone, which is tuning and resampling error;
- scale error, the 500 to 2500 Hz spacing against 2000 Hz in ppm, which is
  a playback or resampling rate that is off, not tuning;
- pitch wander, how far the 1000 Hz tone moves between 1 s windows (p5 to
  p95), which a player that steers its buffer by resampling shows.

The first row is an ideal receiver: the scene's tones and noise density
rendered narrowband and filtered to 100 to 3100 Hz, measured the same way.

An earlier version measured the coherence of a noise-like test signal. It
was dropped: a codec may code noise-like content coarsely on purpose, as
FernSDR's NAC3 does with what it takes for channel noise, and a noise test
signal then measures that design choice rather than the chain's quality.

  python3 bench/analysis/quality.py ROUND
"""
import json
import os
import sys

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import latency  # noqa: E402
from listen_report import load, summarize  # noqa: E402

TONES = [200, 300, 500, 1000, 1500, 2000, 2500, 2700, 2900, 3200]


def run_quality(d):
    x, info, clocks, holes = latency.load_recording(d)
    q = tone_quality(x, info["rate"])
    q["holes"] = int(holes.sum())
    return q


def ideal(seconds=20):
    """The same measures on what an ideal USB receiver would play: the
    scene's tones and noise density, rendered in a narrow band (only the
    tone signals, the header's noise), shifted to audio with a brick-wall
    100 to 3100 Hz filter. What a receiver loses against this is its own."""
    sys.path.insert(0, os.path.join(os.path.dirname(HERE), "source"))
    import scene
    import tempfile
    dial = 7_080_000
    with open(os.path.join(os.path.dirname(HERE), "scenes", "all.scene")) as f:
        src = f.read()
    src = src.replace("rate   = 2048000", "rate   = 128000").replace("center = 7.1M", f"center = {dial + 1500}")
    head, *sections = src.split("\n[")
    text = head + "".join("\n[" + sec for sec in sections if sec.startswith(("scene]", "signal tone-")))
    fd, path = tempfile.mkstemp(suffix=".scene", dir=HERE)
    os.write(fd, text.encode())
    os.close(fd)
    try:
        s = scene.Scene(path, 1)
    finally:
        os.remove(path)
    r = scene.Renderer(s)
    total = int(seconds * s.rate)
    parts, got = [], 0
    while got < total:
        b = r.next_block()[: total - got]
        parts.append(b)
        got += len(b)
    x = np.concatenate(parts)
    k = np.arange(len(x))
    spec = np.fft.fft(x * np.exp(-2j * np.pi * (dial - s.center) * k / s.rate))
    f = np.fft.fftfreq(len(x), 1 / s.rate)
    spec[(f < 100) | (f > 3100)] = 0
    audio = np.fft.ifft(spec).real[:: s.rate // 16000]
    return tone_quality(audio, 16000)


def tone_quality(x, rate):
    # Per 1 s window (1 Hz bins, half overlap), each tone is found near its
    # nominal frequency and masked around where it actually is. A spectrum
    # averaged over the whole recording would smear every tone over several
    # bins wherever the playback clock wanders by a fraction of a per mille
    # (the clock gate allows 0.1 %), and SINAD would then count the tones'
    # own energy as noise: 10 dB of it on a recording whose noise floor sat
    # exactly where the ideal receiver's does.
    n = rate
    w = np.hanning(n)
    f = np.fft.rfftfreq(n, 1 / rate)
    band = (f >= 300) & (f <= 2700)
    level = {t: 0.0 for t in TONES}
    peaks = {t: [] for t in TONES}
    tones_in = rest = far = 0.0
    far_bins = rest_bins = 0
    k = 0
    for i in range(0, len(x) - n, n // 2):
        psd = np.abs(np.fft.rfft(x[i:i + n] * w)) ** 2
        mask = np.zeros(len(f), bool)
        near = np.zeros(len(f), bool)
        for t in TONES:
            sel = np.flatnonzero(np.abs(f - t) <= 15)
            j = sel[np.argmax(psd[sel])]
            e = float(psd[max(0, j - 3):j + 4].sum())
            level[t] += e
            if 300 <= t <= 2700:
                tones_in += e
            mask[max(0, j - 3):j + 4] = True
            near[max(0, j - 25):j + 26] = True
            a, b, c = (np.log(psd[j + d] + 1e-30) for d in (-1, 0, 1))
            peaks[t].append(float(f[j] + 0.5 * (a - c) / (a - 2 * b + c)) if (a - 2 * b + c) else float(f[j]))
        rest += float(psd[band & ~mask].sum())
        far += float(psd[band & ~near].sum())
        far_bins += int((band & ~near).sum())
        rest_bins += int((band & ~mask).sum())
        k += 1
    if not k:
        return {"error": "too short"}
    ref = level[1000]
    pitch = {t: float(np.median(peaks[t])) for t in TONES}
    return {
        "sinad_db": float(10 * np.log10(tones_in / rest)) if rest > 0 else None,
        # The same with the noise taken only 25 Hz or more from any tone and
        # spread over the band: what the chain adds as noise, without the
        # tones' own smearing (wow and flutter) that SINAD counts.
        "snr_db": float(10 * np.log10(tones_in / (far / far_bins * rest_bins))) if far > 0 else None,
        "passband_db": {str(t): float(10 * np.log10(level[t] / ref)) if level[t] > 0 else None for t in TONES},
        "pitch_error_hz": pitch[1000] - 1000,
        # The 2500 Hz tone against 500 Hz: a frequency scale error (playback
        # or resampling rate) rather than a tuning offset, in parts per million.
        "scale_error_ppm": float(((pitch[2500] - pitch[500]) / 2000 - 1) * 1e6),
        # How far the 1000 Hz tone wandered between windows (p95 - p5).
        "pitch_wander_hz": float(np.percentile(peaks[1000], 95) - np.percentile(peaks[1000], 5)),
    }


def main():
    root = sys.argv[1]
    table = {}
    for rid in sorted(os.listdir(root)):
        rdir = os.path.join(root, rid)
        if not os.path.isdir(rdir):
            continue
        runs = []
        # A session round (harness/links.py) has RID/repN/tones, a run.py
        # round RID/TIER/repN with a manifest that says whether it validated.
        session = load(os.path.join(root, "round.json"), {}).get("suite") == "session"
        if session:
            dirs = [os.path.join(rdir, rep, "tones") for rep in sorted(os.listdir(rdir))]
        else:
            dirs = [os.path.join(rdir, tier, rep) for tier in os.listdir(rdir)
                    for rep in sorted(os.listdir(os.path.join(rdir, tier)))]
        for d in dirs:
            if session:
                gates = load(os.path.join(d, "meta.json"), {}).get("gates") or {}
                if not os.path.exists(os.path.join(d, "audio.json")) or not all(gates.values()):
                    continue
            elif load(os.path.join(d, "manifest.json"), {}).get("state") != "validated":
                continue
            try:
                q = run_quality(d)
            except Exception as e:
                q = {"error": str(e)[:200]}
            with open(os.path.join(d, "quality.json"), "w") as f:
                json.dump(q, f, indent=1)
            runs.append(q)
        table[rid] = {"runs": len(runs), "sinad_db": summarize([r.get("sinad_db") for r in runs]),
                      "snr_db": summarize([r.get("snr_db") for r in runs]),
                      "pitch_error_hz": summarize([r.get("pitch_error_hz") for r in runs]),
                      "scale_error_ppm": summarize([r.get("scale_error_ppm") for r in runs]),
                      "pitch_wander_hz": summarize([r.get("pitch_wander_hz") for r in runs]),
                      "passband_db": {str(t): summarize([(r.get("passband_db") or {}).get(str(t)) for r in runs]) for t in TONES}}
    q = ideal()
    one = lambda v: {"median": v, "min": v, "max": v, "n": 1}  # noqa: E731
    table = {"ideal receiver": {"runs": 0, **{k: one(q[k]) for k in ("sinad_db", "snr_db", "pitch_error_hz", "scale_error_ppm", "pitch_wander_hz")},
                                "passband_db": {t: one(v) for t, v in q["passband_db"].items()}}} | table
    with open(os.path.join(root, "summary-quality.json"), "w") as f:
        json.dump(table, f, indent=1)
    lines = ["| receiver | runs | SINAD dB | SNR dB | pitch error Hz | scale error ppm | pitch wander Hz | " + " | ".join(f"{t} Hz" for t in TONES) + " |",
             "|---|---|---|---|---|---|---|" + "---|" * len(TONES)]
    fmt = lambda s, p="{:.1f}": "n/a" if not s else p.format(s["median"])  # noqa: E731
    for rid, v in table.items():
        lines.append(f"| {rid} | {v['runs']} | {fmt(v['sinad_db'])} | {fmt(v['snr_db'])} | {fmt(v['pitch_error_hz'], '{:+.2f}')} | "
                     f"{fmt(v['scale_error_ppm'], '{:+.0f}')} | {fmt(v['pitch_wander_hz'], '{:.2f}')} | " +
                     " | ".join(fmt(v["passband_db"][str(t)]) for t in TONES) + " |")
    with open(os.path.join(root, "summary-quality.md"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))


if __name__ == "__main__":
    main()
