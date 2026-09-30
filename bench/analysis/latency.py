# SPDX-License-Identifier: AGPL-3.0-or-later
"""Latency from the antenna to the listener's ear, marker by marker.

Inputs are a listen.mjs recording (audio.f32, audio.json, clocks.json), the
pace log of the same run (t0 and the clock) and the scene's marker
description. The marker is a carrier keyed with a 31-chip m-sequence every
period; with labels, marker m is the sequence rotated by (m mod labels)
times label_shift chips, so which marker a detection is, and therefore a
latency up to labels * period, is read without ambiguity.

Antenna time. Sample n of the scene leaves the lab's antenna (pace) at
t0 + n / clock, t0 on CLOCK_REALTIME. Marker m starts at scene time
start + m * period, sample round(that * rate).

Ear time. The tap records what reaches the audio context's destination,
by context frame. getOutputTimestamp pairs context time with the
performance time at which that sample is expected at the output device;
performance.timeOrigin puts performance time on CLOCK_REALTIME. A linear
fit over the pairs maps every recorded sample to its presentation time.
Latency "to output" is presentation minus antenna; "to destination"
subtracts the context's reported outputLatency, which in the lab is the
null sink's buffer and not a real device's.
"""
import json
import math
import os

import numpy as np


def load_recording(d):
    if os.path.getsize(os.path.join(d, "audio.f32")) > 100_000_000:
        raise ValueError("recording exceeds the registered capture bound")
    x = np.fromfile(os.path.join(d, "audio.f32"), dtype="<f4").astype(np.float64)
    with open(os.path.join(d, "audio.json")) as f:
        info = json.load(f)
    with open(os.path.join(d, "clocks.json")) as f:
        clocks = json.load(f)
    holes = np.isnan(x)
    x[holes] = 0.0
    return x, info, clocks, holes


def pace_start(path):
    with open(path) as f:
        for line in f:
            e = json.loads(line)
            if e.get("ev") == "start":
                return e
    raise ValueError(f"{path}: no start event")


def presentation_map(clocks):
    """Context seconds -> wall-clock seconds, and the output latency, from
    the getOutputTimestamp pairs (a least-squares line)."""
    pairs = [c for c in clocks if c.get("contextTime") and c.get("performanceTime")]
    if len(pairs) < 4:
        raise ValueError("too few clock pairs")
    ctx = np.array([p["contextTime"] for p in pairs])
    wall = np.array([(p["timeOrigin"] + p["performanceTime"]) / 1000.0 for p in pairs])
    # Pairs that go backwards (a clock reset in the browser) are dropped.
    keep = np.concatenate([[True], (np.diff(ctx) > 0) & (np.diff(wall) > 0)])
    ctx, wall = ctx[keep], wall[keep]
    if len(ctx) < 4:
        raise ValueError("too few increasing clock pairs")
    # Subtract the epoch before fitting to preserve sub-millisecond precision.
    slope, icpt = np.polyfit(ctx-ctx[0], wall-wall[0], 1)
    resid = wall-wall[0] - (slope * (ctx-ctx[0]) + icpt)
    # The browser's output clock jumps when the output underran (a busy
    # machine, or a receiver that stalled the player). Each sample is mapped
    # through the pairs around it, so a jump moves the markers after it by
    # what the listener really heard; the jumps are counted, not refused.
    jumps = int(np.sum(np.abs(np.diff(wall - ctx)) > 0.005))
    def to_wall(c):
        if c < ctx[0]:
            return wall[0] + slope * (c - ctx[0])
        if c > ctx[-1]:
            return wall[-1] + slope * (c - ctx[-1])
        return float(np.interp(c, ctx, wall))
    out_lat = float(np.median([p["outputLatency"] for p in pairs if p.get("outputLatency") is not None] or [0.0]))
    return to_wall, {"clock_ratio": 1.0 / slope, "fit_residual_ms": float(np.std(resid) * 1000),
                                          "max_residual_ms": float(np.max(np.abs(resid)) * 1000),
                                          "output_clock_jumps": jumps,
                                          "output_latency_ms": out_lat * 1000}


def mseq(degree=5, taps=(5, 3)):
    state = [1] * degree
    out = []
    for _ in range(2 ** degree - 1):
        out.append(state[-1])
        fb = 0
        for t in taps:
            fb ^= state[t - 1]
        state = [fb] + state[:-1]
    return np.array(out, dtype=np.float64)


def templates(marker, rate):
    """Each label's keying envelope at the audio rate, with the scene's
    raised-cosine edges, zero-mean for correlation."""
    chips = marker["chips"]
    chip = marker["chip_s"] * rate
    edge = max(1, int(round(marker["edge_s"] * rate)))
    seq = mseq(int(round(math.log2(chips + 1))))
    framed = marker.get("framing") == "double-pilot-v1"
    length = int(math.ceil((chips + (2 if framed else 0)) * chip)) + 2 * edge
    k = np.hanning(edge + 2)[1:-1]
    k /= k.sum()
    out = []
    for label in range(marker.get("labels", 1)):
        s = np.roll(seq, -label * marker.get("label_shift", 2))
        if framed:
            s = np.concatenate(([2], s, [2]))
        on = np.zeros(length)
        for i, c in enumerate(s):
            if c:
                on[edge + int(round(i * chip)):edge + int(round((i + 1) * chip))] = c
        env = np.convolve(on, k, mode="same")
        out.append(env - env.mean())
    return out, edge


def envelope(x, rate, tone):
    n = np.arange(len(x))
    base = x * np.exp(-2j * np.pi * tone * n / rate)
    win = max(1, int(round(rate * 0.001)))
    kernel = np.ones(win) / win
    return np.abs(np.convolve(base, kernel, mode="same"))


def spectrum_peak(x, rate, lo, hi):
    n = 8192
    w = np.hanning(n)
    segs = [x[i:i + n] * w for i in range(0, len(x) - n, n // 2)]
    if not segs:
        return None
    psd = np.mean(np.abs(np.fft.rfft(segs, axis=1)) ** 2, axis=0)
    f = np.fft.rfftfreq(n, 1 / rate)
    sel = np.flatnonzero((f >= lo) & (f <= hi))
    k = sel[np.argmax(psd[sel])]
    a, b, c = (10 * np.log10(psd[k + d] + 1e-30) for d in (-1, 0, 1))
    delta = 0.5 * (a - c) / (a - 2 * b + c) if a - 2 * b + c else 0.0
    return (k + delta) * rate / n


def normalized_xcorr(env, tpl):
    """Correlation of the envelope with a zero-mean template at every lag,
    normalised by the envelope's local deviation: 1 is a perfect match."""
    n = len(tpl)
    m = len(env) - n + 1
    if m <= 0:
        return np.zeros(0)
    size = 1 << int(math.ceil(math.log2(len(env) + n)))
    num = np.fft.irfft(np.fft.rfft(env, size) * np.conj(np.fft.rfft(tpl, size)), size)[:m]
    c1 = np.concatenate([[0.0], np.cumsum(env)])
    c2 = np.concatenate([[0.0], np.cumsum(env * env)])
    s1 = c1[n:n + m] - c1[:m]
    s2 = c2[n:n + m] - c2[:m]
    var = np.maximum(s2 - s1 * s1 / n, 1e-30)
    return num / np.sqrt(var * np.sum(tpl * tpl))


def analyse(rec_dir, pace_log, marker, scene_rate, dial, mode="usb", min_corr=0.6, loop_s=None):
    x, info, clocks, holes = load_recording(rec_dir)
    rate = info["rate"]
    if not rate or len(x) < rate:
        return {"error": "no audio"}
    to_wall, clock = presentation_map(clocks)
    start = pace_start(pace_log)
    expect = (marker["freq"] - dial) if mode == "usb" else (dial - marker["freq"])
    tone = spectrum_peak(x[:int(4 * rate)], rate, expect - 60, expect + 60)
    if tone is None:
        return {"error": "no pitch estimate"}
    # Decimate only the demodulated envelope, in bounded blocks. All labels
    # share the FFT input; retain just the best scores rather than a
    # labels-by-recording matrix.
    stride = max(1, round(rate / 4000))
    env_parts = []
    block = stride * int(4 * rate / stride)
    halo = max(1, round(rate * .002))
    for begin in range(0, len(x), block):
        end = min(len(x), begin + block)
        left, right = max(0, begin - halo), min(len(x), end + halo)
        part = envelope(x[left:right], rate, tone)
        env_parts.append(part[begin-left:end-left:stride])
    env = np.concatenate(env_parts)
    detection_rate = rate / stride
    tpls, edge = templates(marker, detection_rate)
    best = np.full(max(0, len(env) - len(tpls[0]) + 1), -1.0)
    runner_up = best.copy()
    best_label = np.zeros(len(best), dtype=np.int16)
    for label, tpl in enumerate(tpls):
        scores = normalized_xcorr(env, tpl)
        higher = scores > best
        runner_up = np.where(higher, best, np.maximum(runner_up, scores))
        best_label[higher] = label
        best = np.maximum(best, scores)
    period = marker["period_s"]
    labels = marker.get("labels", 1)
    half = int(period * detection_rate / 2)
    found = []
    i = 0
    # One detection per half period window around each local maximum.
    while i < len(best):
        j = i + int(np.argmax(best[i:i + half]))
        if best[j] >= min_corr and best[j] == best[max(0, j - half):j + half].max():
            y0, y1, y2 = (best[j + d] if 0 <= j + d < len(best) else best[j] for d in (-1, 0, 1))
            frac = 0.5 * (y0 - y2) / (y0 - 2 * y1 + y2) if (y0 - 2 * y1 + y2) else 0.0
            onset = (j + frac + edge) * stride
            margin = float(best[j] - runner_up[j])
            if marker.get("identity") != "pairs-v1" or margin >= 0.03:
                found.append((onset, int(best_label[j]), float(best[j])))
            i = j + half
        else:
            i += half
    t0 = start["t0_real_ns"] / 1e9
    clock_hz = start["clock"]
    rows = []
    identities = {}
    conflicts = set()
    paired = marker.get("identity") == "pairs-v1"
    cycle = marker.get("identity_sequence", [])
    if paired:
        lookup = {(cycle[k], cycle[(k + 1) % len(cycle)]): k for k in range(len(cycle))}
        if len(lookup) != len(cycle):
            raise ValueError("marker identity pairs are not unique")
        for k in range(len(found) - 1):
            onset, label, _ = found[k]
            after, next_label, _ = found[k + 1]
            if abs((after - onset) / rate - period) > period * .05:
                continue
            symbol = lookup[(label, next_label)]
            wall = to_wall((info["firstFrame"] + onset) / rate)
            elapsed = (wall - t0) * clock_hz / scene_rate
            latest = math.floor((elapsed - marker["start_s"]) / period)
            m = latest - ((latest - symbol) % len(cycle))
            latency = wall - (t0 + round((marker["start_s"] + m * period) * scene_rate) / clock_hz)
            if m < 0 or not 0 <= latency <= 90:
                continue
            for index, identity in ((k, m), (k + 1, m + 1)):
                if index in identities and identities[index] != identity:
                    conflicts.add(index)
                identities[index] = identity
    ambiguous = 0
    for index, (onset, label, c) in enumerate(found):
        ctx_s = (info["firstFrame"] + onset) / rate
        wall = to_wall(ctx_s)
        if paired:
            if index not in identities or index in conflicts:
                ambiguous += 1
                continue
            m = identities[index]
        else:
            # Legacy recordings have only a short modulo identity; do not
            # use them to make long-outage recovery claims.
            m_est = math.floor(((wall - t0) * clock_hz / scene_rate - marker["start_s"]) / period)
            m = m_est - ((m_est - label) % labels)
        antenna = t0 + round((marker["start_s"] + m * period) * scene_rate) / clock_hz
        if loop_s:
            # A looped input repeats its markers every loop_s: the marker was
            # on the air in the latest repetition before it was heard.
            antenna += math.floor((wall - antenna) / loop_s) * loop_s
        if m < 0:
            continue
        rows.append({"marker": m, "label": label, "corr": round(c, 4), "wall": wall, "antenna": antenna,
                     "to_output_ms": (wall - antenna) * 1000,
                     "to_destination_ms": (wall - antenna) * 1000 - clock["output_latency_ms"]})
    if not rows:
        return {"error": "no marker found", "tone_hz": tone, "clock": clock}
    lat = np.array([r["to_output_ms"] for r in rows])
    t = np.array([r["wall"] for r in rows])
    slope = float(np.polyfit(t - t[0], lat, 1)[0]) if len(rows) >= 3 else None
    first_m = min(r["marker"] for r in rows)
    last_m = max(r["marker"] for r in rows)
    summary = {
        "markers": len(rows), "expected": last_m - first_m + 1, "ambiguous": ambiguous, "tone_hz": tone,
        "median_ms": float(np.median(lat)), "p95_ms": float(np.percentile(lat, 95)),
        "p99_ms": float(np.percentile(lat, 99)), "min_ms": float(lat.min()), "max_ms": float(lat.max()),
        "drift_ms_per_min": slope * 60 if slope is not None else None,
        "to_destination_median_ms": float(np.median([r["to_destination_ms"] for r in rows])),
        "clock": clock, "holes_s": float(holes.sum() / rate),
    }
    return {"summary": summary, "markers": rows}
