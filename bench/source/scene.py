#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""The lab's antenna: renders a scene file into IQ samples.

A scene lists signals (carriers, markers, SSB, AM, FM, CW, noise bands)
with their frequencies and levels, over a noise floor. Every receiver in a
run gets exactly these samples, bit for bit, through pace.

How a band is built. Each signal is generated as complex baseband at 32 kHz
around its own zero, then placed into the band at the full rate. Placing
works in the frequency domain, block by block (overlap-save): a signal's
block is transformed, weighted by an interpolation filter, moved to its bin
and added into one spectrum for the whole band, and one inverse transform
per block makes the band. The cost hardly grows with the number of signals,
which matters for a crowded band at 2 Msps in real time. The interpolation
filter is a zero-phase Kaiser low-pass (flat to 12 kHz, 100 dB down from
16 kHz), so a signal comes out neither early nor late: a marker keyed at
0.5 s is on the air at sample 0.5 s * rate, which the latency measurements
rely on. A frequency between bins gets the rest of its offset as a phase
ramp at 32 kHz, continuous across blocks.

Levels are in dBFS of the complex signal: 0 dBFS is an amplitude of 1.0,
the full scale of every output format. A carrier's level is its amplitude;
a modulated signal's is its average power. `snr` instead of `level` sets a
signal's power against the noise in 2500 Hz, the convention of WSJT-X.
"""
import argparse
import configparser
import hashlib
import json
import math
import os
import re
import sys
import wave

import numpy as np

FS_LOW = 32000  # every signal is generated at this rate before placement
M = 1024  # low-rate samples per block, overlap included
M_OV = 64  # low-rate overlap: covers the interpolation filter at any band rate
M_NEW = M - M_OV
SNR_BANDWIDTH = 2500.0


# ---------------------------------------------------------------- units

_SUFFIX = {"": 1.0, "k": 1e3, "K": 1e3, "M": 1e6, "G": 1e9}


def parse_hz(text):
    """7.1M, 2048k, 1500 or 1.5k, in Hz."""
    m = re.fullmatch(r"\s*([-+]?[0-9.]+(?:e[-+]?[0-9]+)?)\s*([kKMG]?)(?:Hz)?\s*", text)
    if not m:
        raise ValueError(f"not a frequency: {text!r}")
    return float(m.group(1)) * _SUFFIX[m.group(2)]


def parse_seconds(text):
    """0.5s, 5ms or 2, in seconds."""
    m = re.fullmatch(r"\s*([-+]?[0-9.]+)\s*(ms|s)?\s*", text)
    if not m:
        raise ValueError(f"not a duration: {text!r}")
    return float(m.group(1)) * (1e-3 if m.group(2) == "ms" else 1.0)


def parse_db(text):
    m = re.fullmatch(r"\s*([-+]?[0-9.]+)\s*(?:dB(?:FS)?(?:/Hz)?)?\s*", text)
    if not m:
        raise ValueError(f"not a level: {text!r}")
    return float(m.group(1))


# ---------------------------------------------------------------- filters

def kaiser_lowpass(pass_hz, stop_hz, rate, atten_db=100.0):
    """Zero-phase windowed-sinc low-pass with unity gain at DC, odd length."""
    width = 2 * math.pi * (stop_hz - pass_hz) / rate
    n = int(math.ceil((atten_db - 8) / (2.285 * width))) | 1
    beta = 0.1102 * (atten_db - 8.7)
    t = np.arange(n) - (n - 1) / 2
    cutoff = (pass_hz + stop_hz) / 2 / rate
    h = 2 * cutoff * np.sinc(2 * cutoff * t) * np.kaiser(n, beta)
    return h / h.sum()


class FirStream:
    """Streams a signal through a FIR filter, keeping the history across
    calls, so that consecutive blocks give the same result as one long one.
    The filter's delay is removed: output sample i is centred on input i."""

    def __init__(self, taps, source):
        self.taps = np.asarray(taps)
        self.half = (len(self.taps) - 1) // 2
        self.source = source  # callable n -> next n input samples
        self.buf = None

    def next(self, n):
        if self.buf is None:
            # Pre-roll: the first half-filter of input only primes history.
            self.buf = np.concatenate([np.zeros(self.half, dtype=np.complex128), self.source(self.half)])
        new = self.source(n)
        data = np.concatenate([self.buf, new])
        t = len(self.taps)
        # Overlap-save through the FFT: the same sums as np.convolve's
        # 'valid' part, an order of magnitude faster for filters of a
        # thousand taps, which a crowded scene has dozens of.
        size = 1 << int(np.ceil(np.log2(len(data))))
        if getattr(self, "_size", None) != size:
            self._size = size
            self._spec = np.fft.fft(self.taps, size)
        y = np.fft.ifft(np.fft.fft(data, size) * self._spec)[t - 1:t - 1 + n]
        if np.isrealobj(data) and np.isrealobj(self.taps):
            y = y.real
        self.buf = data[-(t - 1):]
        return y


# ---------------------------------------------------------------- audio sources

class Tone:
    def __init__(self, freqs):
        self.freqs = freqs
        self.m = 0
        # Normalised to unit RMS whatever the number of tones.
        self.scale = math.sqrt(2.0 / len(freqs))

    def next(self, n):
        t = (self.m + np.arange(n)) / FS_LOW
        self.m += n
        out = np.zeros(n)
        for f in self.freqs:
            out += np.cos(2 * np.pi * f * t)
        return out * self.scale


class NoiseAudio:
    """Gaussian audio in a pass band, unit RMS, from its own seed."""

    def __init__(self, low, high, seed):
        self.rng = np.random.Generator(np.random.PCG64(seed))
        lp = kaiser_lowpass((high - low) / 2, (high - low) / 2 + 200, FS_LOW, 80)
        centre = (low + high) / 2
        n = np.arange(len(lp)) - (len(lp) - 1) / 2
        # A real band-pass: the low-pass shifted up and down.
        bp = 2 * lp * np.cos(2 * np.pi * centre * n / FS_LOW)
        self.fir = FirStream(bp, lambda k: self.rng.standard_normal(k))
        self.scale = 1.0 / math.sqrt(np.sum(bp ** 2))

    def next(self, n):
        return self.fir.next(n).real * self.scale


class WavAudio:
    """A mono WAV file resampled to 32 kHz, played once or in a loop,
    normalised so that its active part has unit RMS."""

    def __init__(self, path, loop, start_s=0.0):
        with wave.open(path, "rb") as w:
            if w.getnchannels() != 1 or w.getsampwidth() != 2:
                raise ValueError(f"{path}: need mono 16-bit")
            rate = w.getframerate()
            x = np.frombuffer(w.readframes(w.getnframes()), dtype="<i2").astype(np.float64)
        if rate != FS_LOW:
            # Band-limited resampling of the whole file through its spectrum.
            g = math.gcd(rate, FS_LOW)
            up, down = FS_LOW // g, rate // g
            pad = (-len(x)) % down
            x = np.concatenate([x, np.zeros(pad)])
            n_out = len(x) * up // down
            spec = np.fft.rfft(x)
            keep = min(len(spec), n_out // 2 + 1)
            out_spec = np.zeros(n_out // 2 + 1, dtype=np.complex128)
            out_spec[:keep] = spec[:keep]
            x = np.fft.irfft(out_spec, n_out) * (n_out / len(x))
        active = np.abs(x) > 0.01 * np.max(np.abs(x)) if len(x) else x
        rms = math.sqrt(np.mean(x[active] ** 2)) if np.any(active) else 1.0
        # float32: a scene with minutes of WSPR audio would otherwise need
        # hundreds of megabytes before it renders a sample.
        self.x = (x / rms).astype(np.float32)
        self.loop = loop
        self.pos = -int(round(start_s * FS_LOW))

    def next(self, n):
        out = np.zeros(n)
        filled = 0
        while filled < n:
            if self.pos < 0:
                take = min(n - filled, -self.pos)
                self.pos += take
                filled += take
                continue
            if self.pos >= len(self.x):
                if not self.loop:
                    break
                self.pos = 0
            take = min(n - filled, len(self.x) - self.pos)
            out[filled:filled + take] = self.x[self.pos:self.pos + take]
            self.pos += take
            filled += take
        return out


def audio_source(spec, seed, base_dir, loop=True, start_s=0.0):
    kind, _, arg = spec.partition(":")
    if kind == "tone":
        return Tone([parse_hz(arg)])
    if kind == "tones":
        return Tone([parse_hz(f) for f in arg.split(",")])
    if kind == "noise":
        low, high = (parse_hz(f) for f in arg.split("-"))
        return NoiseAudio(low, high, seed)
    if kind == "wav":
        path = arg if os.path.isabs(arg) else os.path.join(base_dir, arg)
        return WavAudio(path, loop, start_s)
    if kind == "silence":
        return Tone([0.0]) if False else _Silence()
    raise ValueError(f"unknown audio source {spec!r}")


class _Silence:
    def next(self, n):
        return np.zeros(n)


# ---------------------------------------------------------------- keying

def mseq(degree=5, taps=(5, 3)):
    """A maximal-length sequence of 0 and 1, 2**degree - 1 long."""
    state = [1] * degree
    out = []
    for _ in range(2 ** degree - 1):
        out.append(state[-1])
        fb = 0
        for t in taps:
            fb ^= state[t - 1]
        state = [fb] + state[:-1]
    return np.array(out, dtype=np.float64)


def keyed(intervals, total):
    """One period of keying as samples: each sample is the share of its cell,
    [i - 0.5, i + 0.5), that the on-intervals (in samples, fractions
    allowed) cover. A chip that starts exactly at sample i thus gives sample
    i half, and the smoothed edge passes half amplitude at i, not half a
    sample before it."""
    on = np.zeros(total)
    for a, b in intervals:
        lo = max(0, int(math.floor(a + 0.5)))
        hi = min(total, int(math.ceil(b + 0.5)))
        i = np.arange(lo, hi)
        on[lo:hi] += np.clip(np.minimum(b, i + 0.5) - np.maximum(a, i - 0.5), 0, 1)
    return on


def smooth_keying(on, edge_samples):
    """Turns one period of a repeating 0/1 keying into an envelope with
    raised-cosine edges of the given length, centred on each transition, so
    that a chip that begins at time T is at half amplitude at T.

    The smoothing wraps around: the pattern repeats, so the edge of a chip
    at the start of the period begins at the end of the previous one. Cut
    off there, it jumped from 0 to half amplitude at once."""
    on = on.astype(np.float64)
    if edge_samples < 2:
        return on
    # Odd length, so that the kernel has a middle sample and 'same' does not
    # shift by half a sample.
    k = np.hanning(edge_samples + 3)[1:-1] if edge_samples % 2 == 0 else np.hanning(edge_samples + 2)[1:-1]
    k /= k.sum()
    h = len(k) // 2
    wrapped = np.concatenate([on[-h:], on, on[:h]])
    return np.convolve(wrapped, k, mode="valid")


# ---------------------------------------------------------------- signals

class Signal:
    """A signal: complex baseband at 32 kHz around its own zero, placed at
    `freq` in the band."""

    def __init__(self, name, freq, amplitude):
        self.name = name
        self.freq = freq
        self.amplitude = amplitude

    def next(self, n):
        raise NotImplementedError


class Carrier(Signal):
    def next(self, n):
        return np.full(n, self.amplitude, dtype=np.complex128)


class KeyedPattern(Signal):
    """A carrier keyed by a pattern that repeats every period: markers and CW."""

    def __init__(self, name, freq, amplitude, envelope, period_samples, start_samples):
        super().__init__(name, freq, amplitude)
        self.env = envelope
        self.period = period_samples
        self.m = -start_samples

    # How far before the first chip the pattern already sounds: the first
    # half of that chip's smoothed edge, which belongs to the time before.
    LEAD = 64

    def next(self, n):
        idx = self.m + np.arange(n)
        self.m += n
        out = np.zeros(n)
        live = idx >= -self.LEAD
        out[live] = self.env[idx[live] % self.period]
        return out * self.amplitude


def marker_sequence(chips, label, label_shift):
    """The chips of marker number `label`: the m-sequence rotated by
    label * label_shift chips. Rotations of an m-sequence are nearly
    orthogonal, so a receiver's output tells which marker it is."""
    seq = mseq(int(round(math.log2(chips + 1))))
    return np.roll(seq, -label * label_shift)


def marker_identity(alphabet=31):
    """An Euler cycle of the complete directed graph: every ordered pair
    occurs once, so two consecutive rotations identify a marker for
    alphabet**2 periods rather than just alphabet periods."""
    remaining = [list(range(alphabet - 1, -1, -1)) for _ in range(alphabet)]
    stack, circuit = [0], []
    while stack:
        vertex = stack[-1]
        if remaining[vertex]:
            stack.append(remaining[vertex].pop())
        else:
            circuit.append(stack.pop())
    return list(reversed(circuit))[:-1]


class IdentifiedMarker(Signal):
    """Stores one short envelope per rotation, not a 480-second waveform."""

    def __init__(self, name, freq, amplitude, chips, chip_s, period_s, edge_s, label_shift, start_s):
        # Pilots keep the scene's stated peak. The data chips are 6 dB lower,
        # so identity framing does not introduce a stronger AGC stimulus.
        super().__init__(name, freq, amplitude / 2)
        self.identity = np.asarray(marker_identity(chips), dtype=np.int32)
        env, self.cycle = marker_envelope(chips, chip_s, period_s, edge_s, chips, label_shift, framed=True)
        self.period = int(round(period_s * FS_LOW))
        self.env = env.reshape(chips, self.period)
        self.m = -int(round(start_s * FS_LOW))

    def next(self, n):
        idx = self.m + np.arange(n)
        self.m += n
        live = idx >= -KeyedPattern.LEAD
        out = np.zeros(n)
        marker = idx[live] // self.period
        out[live] = self.env[self.identity[marker % len(self.identity)], idx[live] % self.period]
        return out * self.amplitude


def marker_envelope(chips, chip_s, period_s, edge_s, labels=1, label_shift=2, framed=False):
    """The repeating marker pattern: every period, the m-sequence keyed on
    chip by chip, then silence to the end of the period. With labels > 1,
    marker k is rotated by (k mod labels) * label_shift chips, and the
    pattern repeats after `labels` periods: a latency up to that long is
    then read without ambiguity. Sample 0 is the first marker's start."""
    period = int(round(period_s * FS_LOW))
    chip = chip_s * FS_LOW
    intervals = []
    for k in range(labels):
        seq = marker_sequence(chips, k, label_shift)
        if framed:
            seq = np.concatenate(([2], seq, [2]))
        intervals += [(k * period + i * chip, k * period + (i + 1) * chip)
                      for i, c in enumerate(seq) for _ in range(int(c))]
    on = keyed(intervals, labels * period)
    return smooth_keying(on, int(round(edge_s * FS_LOW))), labels * period


MORSE = {
    "A": ".-", "B": "-...", "C": "-.-.", "D": "-..", "E": ".", "F": "..-.", "G": "--.", "H": "....",
    "I": "..", "J": ".---", "K": "-.-", "L": ".-..", "M": "--", "N": "-.", "O": "---", "P": ".--.",
    "Q": "--.-", "R": ".-.", "S": "...", "T": "-", "U": "..-", "V": "...-", "W": ".--", "X": "-..-",
    "Y": "-.--", "Z": "--..", "0": "-----", "1": ".----", "2": "..---", "3": "...--", "4": "....-",
    "5": ".....", "6": "-....", "7": "--...", "8": "---..", "9": "----.", "/": "-..-.", "?": "..--..",
    "=": "-...-", ".": ".-.-.-", ",": "--..--",
}


def morse_envelope(text, wpm, edge_s, gap_s):
    """Keying of `text` in Morse at `wpm` (PARIS timing), then `gap_s` of
    silence; one period of the repeating pattern."""
    unit = 1.2 / wpm
    bits = []
    for w, word in enumerate(text.upper().split()):
        if w:
            bits += [0] * 4  # word gap of 7 units: 3 after the letter plus 4
        for li, letter in enumerate(word):
            if li:
                bits += [0] * 2  # letter gap of 3 units: 1 after the element plus 2
            for e in MORSE[letter]:
                bits += [1] * (1 if e == "." else 3) + [0]
    unit_samples = unit * FS_LOW
    total = int(round(len(bits) * unit_samples + gap_s * FS_LOW))
    on = keyed([(i * unit_samples, (i + 1) * unit_samples) for i, b in enumerate(bits) if b], total)
    return smooth_keying(on, int(round(edge_s * FS_LOW))), total


class Ssb(Signal):
    """Single sideband: the audio's positive (USB) or negative (LSB)
    frequencies within low..high, through a complex band-pass whose other
    side is 100 dB down."""

    def __init__(self, name, freq, amplitude, audio, sideband, low, high):
        super().__init__(name, freq, amplitude)
        lp = kaiser_lowpass((high - low) / 2, (high - low) / 2 + 150, FS_LOW, 100)
        centre = (low + high) / 2 * (1 if sideband == "usb" else -1)
        n = np.arange(len(lp)) - (len(lp) - 1) / 2
        # Twice the gain: one side of a real signal carries half its power.
        bp = 2 * lp * np.exp(2j * np.pi * centre * n / FS_LOW)
        self.fir = FirStream(bp, audio.next)
        # Unit-RMS audio in, unit average power out.
        self.scale = 1.0 / math.sqrt(2.0)

    def next(self, n):
        return self.fir.next(n) * self.scale * self.amplitude


class Am(Signal):
    def __init__(self, name, freq, amplitude, audio, depth):
        super().__init__(name, freq, amplitude)
        self.audio = audio
        self.depth = depth
        # Unit-RMS audio: average power of (1 + d a) is 1 + d^2.
        self.scale = 1.0 / math.sqrt(1 + depth ** 2)

    def next(self, n):
        return (1 + self.depth * self.audio.next(n)) * self.scale * self.amplitude + 0j


class Fm(Signal):
    def __init__(self, name, freq, amplitude, audio, deviation):
        super().__init__(name, freq, amplitude)
        self.audio = audio
        # Unit-RMS audio: its peaks near 1.4 reach 1.4 times the deviation.
        self.k = 2 * np.pi * deviation / FS_LOW
        self.phase = 0.0

    def next(self, n):
        ph = self.phase + np.cumsum(self.k * self.audio.next(n))
        self.phase = float(ph[-1] % (2 * np.pi))
        return np.exp(1j * ph) * self.amplitude


class NoiseBand(Signal):
    """Complex Gaussian noise, flat across `width`, centred on `freq`."""

    def __init__(self, name, freq, amplitude, width, seed):
        super().__init__(name, freq, amplitude)
        rng = np.random.Generator(np.random.PCG64(seed))
        lp = kaiser_lowpass(width / 2, width / 2 + 200, FS_LOW, 100)
        self.fir = FirStream(lp, lambda k: (rng.standard_normal(k) + 1j * rng.standard_normal(k)) / math.sqrt(2))
        self.scale = 1.0 / math.sqrt(np.sum(lp ** 2))

    def next(self, n):
        return self.fir.next(n) * self.scale * self.amplitude


# ---------------------------------------------------------------- the scene

class Scene:
    def __init__(self, path, seed):
        cp = configparser.ConfigParser(inline_comment_prefixes=(";", "#"))
        with open(path) as f:
            cp.read_file(f)
        self.path = path
        base_dir = os.path.dirname(os.path.abspath(path))
        s = cp["scene"]
        self.rate = int(parse_hz(s["rate"]))
        self.center = parse_hz(s.get("center", "0"))
        self.real = s.get("real", "no").lower() in ("yes", "true", "1")
        self.noise_dbfs_hz = parse_db(s["noise"]) if "noise" in s else None
        self.seed = seed
        self.noise_n0 = 10 ** (self.noise_dbfs_hz / 10) if self.noise_dbfs_hz is not None else 0.0
        # render_noise = no keeps the floor for levels given as snr but leaves
        # it out of the samples, for a loop that pace fills with fresh noise.
        self.render_noise = s.get("render_noise", "yes").lower() not in ("no", "false", "0")
        if self.rate % FS_LOW:
            raise ValueError(f"rate must be a multiple of {FS_LOW}")
        if self.real and self.center != 0:
            raise ValueError("a real scene is centred on 0 Hz")
        self.signals = []
        self.markers = []
        for i, section in enumerate(cp.sections()):
            if not section.startswith("signal "):
                continue
            name = section[len("signal "):].strip()
            self.signals.append(self._signal(name, cp[section], self._sub_seed(name), base_dir))

    def _sub_seed(self, name):
        digest = hashlib.sha256(f"{self.seed}/{name}".encode()).digest()
        return int.from_bytes(digest[:8], "little")

    def _amplitude(self, sec):
        if "snr" in sec:
            if not self.noise_n0:
                raise ValueError("snr needs a noise floor")
            power = self.noise_n0 * SNR_BANDWIDTH * 10 ** (parse_db(sec["snr"]) / 10)
            return math.sqrt(power)
        return 10 ** (parse_db(sec["level"]) / 20)

    def _signal(self, name, sec, seed, base_dir):
        kind = sec["kind"]
        freq = parse_hz(sec["freq"])
        amp = self._amplitude(sec)
        if kind == "carrier":
            return Carrier(name, freq, amp)
        if kind == "marker":
            chips = int(sec.get("chips", "31"))
            chip_s = parse_seconds(sec.get("chip", "5ms"))
            period_s = parse_seconds(sec.get("period", "0.5s"))
            start_s = parse_seconds(sec.get("start", "0.1s"))
            edge_s = parse_seconds(sec.get("edge", "1ms"))
            labels = int(sec.get("labels", "1"))
            label_shift = int(sec.get("label_shift", "2"))
            identity = sec.get("identity", "rotation-v1")
            if identity not in ("rotation-v1", "pairs-v1"):
                raise ValueError(f"marker {name}: unknown identity {identity}")
            if identity == "pairs-v1":
                labels = chips
            # Rotations by k * label_shift are all different while k stays
            # under chips / gcd(label_shift, chips).
            if labels < 1 or labels > chips // math.gcd(label_shift, chips):
                raise ValueError(f"marker {name}: at most {chips // math.gcd(label_shift, chips)} labels")
            self.markers.append({"name": name, "freq": freq, "chips": chips, "chip_s": chip_s,
                                 "period_s": period_s, "start_s": start_s, "edge_s": edge_s,
                                 "labels": labels, "label_shift": label_shift, "identity": identity})
            if identity == "pairs-v1":
                self.markers[-1]["identity_sequence"] = marker_identity(chips)
                self.markers[-1]["framing"] = "double-pilot-v1"
                return IdentifiedMarker(name, freq, amp, chips, chip_s, period_s, edge_s, label_shift, start_s)
            env, period = marker_envelope(chips, chip_s, period_s, edge_s, labels, label_shift)
            return KeyedPattern(name, freq, amp, env, period, int(round(start_s * FS_LOW)))
        if kind == "cw":
            env, period = morse_envelope(sec["text"], float(sec.get("wpm", "20")),
                                         parse_seconds(sec.get("edge", "5ms")), parse_seconds(sec.get("gap", "2s")))
            return KeyedPattern(name, freq, amp, env, period, int(round(parse_seconds(sec.get("start", "0s")) * FS_LOW)))
        if kind == "noiseband":
            return NoiseBand(name, freq, amp, parse_hz(sec.get("width", "10k")), seed)
        loop = sec.get("loop", "yes").lower() in ("yes", "true", "1")
        audio = audio_source(sec["audio"], seed, base_dir, loop, parse_seconds(sec.get("start", "0s")))
        if kind == "ssb":
            return Ssb(name, freq, amp, audio, sec.get("sideband", "usb").lower(),
                       parse_hz(sec.get("low", "200")), parse_hz(sec.get("high", "3000")))
        if kind == "am":
            return Am(name, freq, amp, audio, float(sec.get("depth", "0.8")))
        if kind == "fm":
            return Fm(name, freq, amp, audio, parse_hz(sec.get("deviation", "2.5k")))
        raise ValueError(f"signal {name}: unknown kind {kind!r}")

    def describe(self):
        return {"rate": self.rate, "center": self.center, "real": self.real, "noise_dbfs_hz": self.noise_dbfs_hz,
                "seed": self.seed, "markers": self.markers,
                "signals": [{"name": s.name, "kind": type(s).__name__, "freq": s.freq,
                             "level_dbfs": 20 * math.log10(s.amplitude) if s.amplitude > 0 else None}
                            for s in self.signals]}


class Renderer:
    """Renders a scene block by block; each block is L_NEW samples of the band."""

    def __init__(self, scene):
        self.scene = scene
        self.U = scene.rate // FS_LOW
        self.L = self.U * M
        self.L_ov = self.U * M_OV
        self.L_new = self.U * M_NEW
        h = kaiser_lowpass(12000, 16000, scene.rate)
        c = (len(h) - 1) // 2
        if c > self.L_ov // 2:
            raise AssertionError("interpolation filter longer than the overlap")
        hz = np.zeros(self.L)
        hz[:c + 1] = h[c:]
        hz[-c:] = h[:c]
        H = np.fft.fft(hz).real
        # The M bins around each signal's zero, in FFT order, and their weights.
        self.base_bins = np.concatenate([np.arange(0, M // 2), np.arange(-M // 2, 0)])
        self.H = H[self.base_bins % self.L] * self.U
        self.bin_hz = scene.rate / self.L
        self.placed = []
        for sig in scene.signals:
            offset = sig.freq - scene.center
            if scene.real and not 0 < offset < scene.rate / 2:
                raise ValueError(f"{sig.name}: a real scene needs 0 < freq < rate/2")
            if not scene.real and abs(offset) >= scene.rate / 2 - 16000:
                raise ValueError(f"{sig.name}: outside the band")
            k = int(round(offset / self.bin_hz))
            delta = offset - k * self.bin_hz
            # Overlap kept from the previous block; the first block starts
            # half an overlap before time zero.
            history = sig.next(M_OV // 2) if False else None
            self.placed.append({"sig": sig, "k": k, "delta": delta, "history": None})
        self.block = 0
        self.noise_rng = np.random.Generator(np.random.PCG64(scene._sub_seed("noise floor")))
        self.noise_sigma = math.sqrt(scene.noise_n0 * scene.rate) if scene.render_noise else 0.0

    def next_block(self):
        b = self.block
        m0 = b * M_NEW - M_OV // 2
        spectrum = np.zeros(self.L, dtype=np.complex128)
        for p in self.placed:
            sig = p["sig"]
            if p["history"] is None:
                # Time before zero is silence: every signal starts at t = 0.
                x = np.concatenate([np.zeros(M_OV // 2, dtype=np.complex128), sig.next(M - M_OV // 2)])
            else:
                x = np.concatenate([p["history"], sig.next(M_NEW)])
            p["history"] = x[-M_OV:]
            if p["delta"]:
                ph = 2 * np.pi * p["delta"] * ((m0 + np.arange(M)) / FS_LOW)
                x = x * np.exp(1j * np.mod(ph, 2 * np.pi))
            X = np.fft.fft(x) * self.H
            # Continuity of the shift by k bins: the phase of k at this
            # block's first sample, from integers so that it never drifts.
            turn = ((p["k"] * m0) % M) / M
            X *= np.exp(2j * np.pi * turn)
            spectrum[(self.base_bins + p["k"]) % self.L] += X
        band = np.fft.ifft(spectrum)[self.L_ov // 2:self.L_ov // 2 + self.L_new]
        if self.noise_sigma:
            noise = self.noise_rng.standard_normal(2 * self.L_new).view(np.complex128)
            band = band + noise * (self.noise_sigma / math.sqrt(2))
        self.block += 1
        return band


def quantise(band, fmt, real):
    """Samples in an output format; returns bytes and the number clipped."""
    if real:
        if fmt != "s16":
            raise ValueError("a real scene is written as s16")
        v = np.round(band.real * 32767)
        clipped = int(np.count_nonzero((v > 32767) | (v < -32768)))
        return np.clip(v, -32768, 32767).astype("<i2").tobytes(), clipped
    iq = np.empty(2 * len(band))
    iq[0::2] = band.real
    iq[1::2] = band.imag
    if fmt == "cu8":
        v = np.round(iq * 127.5 + 127.5)
        clipped = int(np.count_nonzero((v > 255) | (v < 0)))
        return np.clip(v, 0, 255).astype(np.uint8).tobytes(), clipped
    if fmt == "cs16":
        v = np.round(iq * 32767)
        clipped = int(np.count_nonzero((v > 32767) | (v < -32768)))
        return np.clip(v, -32768, 32767).astype("<i2").tobytes(), clipped
    if fmt == "cf32":
        return iq.astype("<f4").tobytes(), 0
    raise ValueError(f"unknown format {fmt}")


def main():
    ap = argparse.ArgumentParser(description="Render a scene into IQ samples on stdout or into a file.")
    ap.add_argument("scene")
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--format", default="cu8", choices=["cu8", "cs16", "cf32", "s16"])
    ap.add_argument("--seconds", type=float, help="stop after this much; without it, render until killed")
    ap.add_argument("--out", help="file instead of stdout")
    ap.add_argument("--evidence", help="write first-64-MiB sample hash without retaining IQ")
    ap.add_argument("--describe", action="store_true", help="print the scene as JSON and exit")
    args = ap.parse_args()

    scene = Scene(args.scene, args.seed)
    if args.describe:
        json.dump(scene.describe(), sys.stdout, indent=1)
        print()
        return
    r = Renderer(scene)
    total = int(round(args.seconds * scene.rate)) if args.seconds else None
    out = open(args.out, "wb") if args.out else sys.stdout.buffer
    written = clipped = 0
    hashed = 0
    digest = hashlib.sha256()
    try:
        while total is None or written < total:
            band = r.next_block()
            if total is not None:
                band = band[:total - written]
            data, c = quantise(band, args.format, scene.real)
            if args.evidence and hashed < 64 * 1024 * 1024:
                prefix = data[:64 * 1024 * 1024 - hashed]
                digest.update(prefix)
                hashed += len(prefix)
                if hashed == 64 * 1024 * 1024:
                    with open(args.evidence, "w") as evidence:
                        json.dump({"seed": args.seed, "format": args.format,
                                   "bytes": hashed, "sha256": digest.hexdigest(),
                                   "scene": scene.describe()}, evidence)
            out.write(data)
            written += len(band)
            clipped += c
    except BrokenPipeError:
        pass
    finally:
        try:
            out.flush()
        except BrokenPipeError:
            pass
        if clipped:
            print(f"scene: {clipped} values clipped", file=sys.stderr)


if __name__ == "__main__":
    main()
