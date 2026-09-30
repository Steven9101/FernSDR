#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Digital modes through a receiver's whole chain.

One SSB channel at 7.074 MHz carries, at once, an SNR ladder of FT8
signals (500 to 1300 Hz of audio), a ladder of WSPR signals (1400 to
1600 Hz), two RTTY signals (1700 to 2250 Hz) and two CW signals (2400 and
2550 Hz). A listener tuned to 7.074 MHz USB hears all of them; one
recording serves every mode. The same decoders read the recording and an
ideal receiver's audio of the same samples, which is the ceiling.

  digital.py assets            write the reference audio and scenes/digital.scene
  digital.py decode REC LAT    decode a recording (listen.mjs output directory),
                               cutting periods LAT seconds after the antenna
  digital.py oracle SECONDS    render the scene, demodulate it ideally, decode

FT8 comes from WSJT-X's ft8sim (noiseless), WSPR from wsprsim's channel
symbols as 4-FSK, RTTY from minimodem, CW from the scene's own keyer.
Signals start on the scene's period boundaries: FT8 every 15 s, WSPR every
120 s, both at scene time 0, so a recording is cut by antenna time plus the
receiver's latency.
"""
import json
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
import wave

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH = os.path.dirname(HERE)
ASSETS = os.path.join(HERE, "assets")
SCENE = os.path.join(BENCH, "scenes", "digital.scene")
DIAL = 7_074_000
NOISE = -97  # dBFS/Hz

FT8 = [(f"K{i}AB{chr(65 + i)} W{i}XY EN{30 + i}", 500 + 80 * i, snr) for i, snr in enumerate(range(-24, -2, 2))]
WSPR = [(f"K{i}WSP FN{20 + i} 33", 1410 + 20 * i, snr) for i, snr in enumerate(range(-36, -18, 2))]
RTTY_TEXT = "RYRYRY THE QUICK BROWN FOX JUMPS OVER THE LAZY DOG 0123456789 "
RTTY = [(1700, 1870, -6), (2000, 2170, 4)]
CW_TEXT = "CQ CQ TEST DE K1FRN K1FRN K"
CW = [(2400, 10), (2550, 20)]  # multimon-ng needs about 12 dB in 150 Hz; the chain, not the decoder, is on test


def write_wav(path, x, rate=12000):
    x = np.clip(np.round(x * 32767), -32768, 32767).astype("<i2")
    with wave.open(path, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(x.tobytes())


def read_wav(path):
    with wave.open(path) as w:
        return np.frombuffer(w.readframes(w.getnframes()), "<i2").astype(float) / 32768, w.getframerate()


def assets():
    os.makedirs(ASSETS, exist_ok=True)
    manifest = {"ft8": [], "wspr": [], "rtty": [], "cw": []}
    lines = [f"; Written by bench/digital/digital.py assets. Listen at {DIAL} Hz USB.",
             "[scene]", "rate   = 2048000", "center = 7.1M", f"noise  = {NOISE} dB/Hz", ""]
    with tempfile.TemporaryDirectory() as tmp:
        for i, (msg, f0, snr) in enumerate(FT8):
            subprocess.run(["ft8sim", msg, str(f0), "0", "0", "0", "1", "99"], cwd=tmp, check=True, capture_output=True)
            x, _ = read_wav(os.path.join(tmp, "000000_000001.wav"))
            path = os.path.join(ASSETS, f"ft8-{i:02d}.wav")
            write_wav(path, x / np.max(np.abs(x)) * 0.9)
            manifest["ft8"].append({"msg": msg, "f0": f0, "snr": snr, "wav": path})
            lines += [f"[signal ft8-{i:02d}]", "kind = ssb", "sideband = usb", f"freq = {DIAL}", f"audio = wav:{path}",
                      "low = 200", "high = 3000", f"snr = {snr}", ""]
    for i, (msg, f0, snr) in enumerate(WSPR):
        # wsprsim exits 1 even when it prints the symbols.
        out = subprocess.run(["wsprsim", "-c", msg], capture_output=True, text=True).stdout
        sym = [int(s) for s in out.split("Channel symbols:")[1].split()]
        rate, ns = 12000, 8192
        spacing = rate / ns
        x = np.zeros(120 * rate)
        phase = 0.0
        t = np.arange(ns)
        for k, s in enumerate(sym):
            f = f0 + (s - 1.5) * spacing
            seg = np.cos(phase + 2 * np.pi * f * t / rate)
            phase += 2 * np.pi * f * ns / rate
            x[rate + k * ns: rate + (k + 1) * ns] = seg
        path = os.path.join(ASSETS, f"wspr-{i:02d}.wav")
        write_wav(path, x * 0.9)
        manifest["wspr"].append({"msg": msg, "f0": f0, "snr": snr, "wav": path})
        lines += [f"[signal wspr-{i:02d}]", "kind = ssb", "sideband = usb", f"freq = {DIAL}", f"audio = wav:{path}",
                  "low = 200", "high = 3000", f"snr = {snr}", ""]
    for i, (mark, space, snr) in enumerate(RTTY):
        path = os.path.join(ASSETS, f"rtty-{i}.wav")
        subprocess.run(["minimodem", "--tx", "rtty", "-M", str(mark), "-S", str(space), "-f", path, "-R", "12000"],
                       input=RTTY_TEXT * 4, text=True, check=True, capture_output=True)
        manifest["rtty"].append({"mark": mark, "space": space, "snr": snr, "wav": path, "text": RTTY_TEXT})
        lines += [f"[signal rtty-{i}]", "kind = ssb", "sideband = usb", f"freq = {DIAL}", f"audio = wav:{path}",
                  "low = 200", "high = 3000", f"snr = {snr}", ""]
    lines += ["[signal reference]", "kind = carrier", f"freq = {DIAL + 350}", "level = -60", ""]
    for i, (pitch, snr) in enumerate(CW):
        manifest["cw"].append({"pitch": pitch, "snr": snr, "text": CW_TEXT})
        lines += [f"[signal cw-{i}]", "kind = cw", f"freq = {DIAL + pitch}", f"text = {CW_TEXT}", "wpm = 20",
                  "edge = 5ms", "gap = 3s", f"snr = {snr}", ""]
    with open(SCENE, "w") as f:
        f.write("\n".join(lines))
    # The same signals as a real 20.48 Msps band, for receivers that take an
    # RX888-like front end (ka9q-web, UberSDR).
    with open(SCENE.replace(".scene", "-real.scene"), "w") as f:
        f.write("\n".join(lines).replace("rate   = 2048000", "rate   = 20.48M\nreal   = yes").replace("center = 7.1M\n", ""))
    with open(os.path.join(ASSETS, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=1)
    print(f"wrote {SCENE} and {len(FT8)} FT8, {len(WSPR)} WSPR, {len(RTTY)} RTTY, {len(CW)} CW signals")


def resample(x, rate, to):
    if rate == to:
        return x
    n = int(round(len(x) * to / rate))
    spec = np.fft.rfft(x)
    out = np.zeros(n // 2 + 1, dtype=complex)
    k = min(len(spec), len(out))
    out[:k] = spec[:k]
    return np.fft.irfft(out, n) * n / len(x)


def levenshtein(a, b):
    prev = list(range(len(b) + 1))
    for i, ca in enumerate(a, 1):
        cur = [i]
        for j, cb in enumerate(b, 1):
            cur.append(min(prev[j] + 1, cur[j - 1] + 1, prev[j - 1] + (ca != cb)))
        prev = cur
    return prev[-1]


def text_score(got, sent):
    """Share of the sent text recovered: 1 - edit distance / length, best
    alignment against the repeated text, whitespace ignored (multimon-ng
    drops word spaces)."""
    g = re.sub(r"\s+", "", got.upper())
    s = re.sub(r"\s+", "", sent.upper())
    if not g:
        return 0.0
    ref = (s * (len(g) // max(1, len(s)) + 2))
    best = 0.0
    for off in range(len(s)):
        r = ref[off:off + len(g)]
        best = max(best, 1 - levenshtein(g, r) / max(1, len(r)))
    return max(0.0, best)


def decode_audio(x, rate, first_antenna_s, manifest):
    """Decodes audio whose sample 0 left the antenna at scene time
    first_antenna_s (already corrected for latency). Returns per-signal
    results."""
    x12 = resample(x, rate, 12000)
    res = {"ft8": [], "wspr": [], "rtty": [], "cw": []}
    with tempfile.TemporaryDirectory() as tmp:
        # FT8: every complete 15 s period.
        start = math.ceil(first_antenna_s / 15) * 15
        periods = 0
        found = {}
        while (start + 15 - first_antenna_s) * 12000 <= len(x12):
            a = int(round((start - first_antenna_s) * 12000))
            seg = x12[a:a + 180000]
            path = os.path.join(tmp, "ft8.wav")
            write_wav(path, seg / max(1e-9, np.max(np.abs(seg))) * 0.5)
            out = subprocess.run(["jt9", "-8", "-d", "3", path], cwd=tmp, capture_output=True, text=True, timeout=120).stdout
            periods += 1
            for line in out.splitlines():
                for s in manifest["ft8"]:
                    if s["msg"] in line:
                        parts = line.split()
                        found.setdefault(s["msg"], []).append({"snr": int(parts[1]), "dt": float(parts[2])})
            start += 15
        for s in manifest["ft8"]:
            d = found.get(s["msg"], [])
            res["ft8"].append({"snr": s["snr"], "decoded": len(d), "periods": periods,
                               "reported_snr": float(np.median([e["snr"] for e in d])) if d else None,
                               "dt": float(np.median([e["dt"] for e in d])) if d else None})
        # WSPR: every complete 120 s period.
        start = math.ceil(first_antenna_s / 120) * 120
        wfound, wperiods = {}, 0
        while (start + 114 - first_antenna_s) * 12000 <= len(x12):
            a = int(round((start - first_antenna_s) * 12000))
            seg = x12[a:a + 120 * 12000]
            path = os.path.join(tmp, "wspr.wav")
            write_wav(path, seg / max(1e-9, np.max(np.abs(seg))) * 0.5)
            out = subprocess.run(["wsprd", "-w", path], cwd=tmp, capture_output=True, text=True, timeout=300).stdout
            wperiods += 1
            for line in out.splitlines():
                for s in manifest["wspr"]:
                    if " ".join(s["msg"].split()[:2]) in line:
                        wfound.setdefault(s["msg"], []).append(line)
            start += 120
        for s in manifest["wspr"]:
            res["wspr"].append({"snr": s["snr"], "decoded": len(wfound.get(s["msg"], [])), "periods": wperiods})
        # RTTY and CW over the whole recording.
        path = os.path.join(tmp, "all.wav")
        write_wav(path, x12 / max(1e-9, np.max(np.abs(x12))) * 0.5)
        for s in manifest["rtty"]:
            out = subprocess.run(["minimodem", "--rx", "rtty", "-M", str(s["mark"]), "-S", str(s["space"]), "-f", path, "-q"],
                                 capture_output=True, text=True, timeout=300).stdout
            res["rtty"].append({"snr": s["snr"], "score": text_score(out, s["text"]), "chars": len(out.strip())})
        for s in manifest["cw"]:
            # multimon-ng reads 22050 Hz raw; move the CW pitch to 800 Hz first.
            n = np.arange(len(x12))
            shifted = x12 * np.cos(2 * np.pi * (s["pitch"] - 800) * n / 12000)
            spec = np.fft.rfft(shifted)
            f = np.fft.rfftfreq(len(shifted), 1 / 12000)
            spec[(f < 725) | (f > 875)] = 0  # 150 Hz: the other CW signal, 150 Hz away, stays out
            y = resample(np.fft.irfft(spec, len(shifted)), 12000, 22050)
            raw = os.path.join(tmp, "cw.raw")
            np.clip(np.round(y / max(1e-9, np.max(np.abs(y))) * 16000), -32768, 32767).astype("<i2").tofile(raw)
            out = subprocess.run(["multimon-ng", "-q", "-a", "MORSE_CW", "-t", "raw", raw], capture_output=True, text=True,
                                 timeout=300).stdout
            text = " ".join(l.split(":", 1)[-1] for l in out.splitlines() if "MORSE" in l or l.strip())
            res["cw"].append({"snr": s["snr"], "score": text_score(text, CW_TEXT)})
    return res


def load_manifest():
    with open(os.path.join(ASSETS, "manifest.json")) as f:
        return json.load(f)


def oracle(seconds):
    sys.path.insert(0, os.path.join(BENCH, "source"))
    import scene
    # The same signals and noise density in a narrow band around the
    # channel: an ideal receiver needs nothing else, and 2 Msps for minutes
    # would not fit in memory.
    text = open(SCENE).read().replace("rate   = 2048000", "rate   = 128000").replace("center = 7.1M", f"center = {DIAL + 1500}")
    fd, path = tempfile.mkstemp(suffix=".scene", dir=HERE)
    os.write(fd, text.encode())
    os.close(fd)
    try:
        s = scene.Scene(path, 1)
    finally:
        os.remove(path)
    r = scene.Renderer(s)
    rate = s.rate
    total = int(seconds * rate)
    step = rate // 12000 if rate % 12000 == 0 else None
    parts, got = [], 0
    while got < total:
        b = r.next_block()[: total - got]
        parts.append(b)
        got += len(b)
    x = np.concatenate(parts)
    n = np.arange(len(x))
    base = x * np.exp(-2j * np.pi * (DIAL - s.center) * n / rate)
    spec = np.fft.fft(base)
    f = np.fft.fftfreq(len(base), 1 / rate)
    spec[(f < 100) | (f > 3000)] = 0
    audio = resample(np.fft.ifft(spec).real, rate, 12000)
    return decode_audio(audio, 12000, 0.0, load_manifest())


def decode_recording(d, latency_s, pace_log=None):
    x = np.fromfile(os.path.join(d, "audio.f32"), "<f4").astype(float)
    x[np.isnan(x)] = 0
    with open(os.path.join(d, "audio.json")) as f:
        info = json.load(f)
    with open(os.path.join(d, "meta.json")) as f:
        meta = json.load(f)
    with open(os.path.join(d, "clocks.json")) as f:
        clocks = json.load(f)
    sys.path.insert(0, os.path.join(BENCH, "analysis"))
    import latency
    to_wall, _ = latency.presentation_map(clocks)
    start = latency.pace_start(pace_log or os.path.join(d, "pace.jsonl"))
    wall0 = to_wall(info["firstFrame"] / info["rate"])
    first_antenna = wall0 - latency_s - start["t0_real_ns"] / 1e9
    return decode_audio(x, info["rate"], first_antenna, load_manifest())


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else ""
    if cmd == "assets":
        assets()
    elif cmd == "oracle":
        print(json.dumps(oracle(float(sys.argv[2])), indent=1))
    elif cmd == "decode":
        print(json.dumps(decode_recording(sys.argv[2], float(sys.argv[3])), indent=1))
    else:
        print(__doc__)
        sys.exit(2)
