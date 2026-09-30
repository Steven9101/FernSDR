#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Checks that scene.py puts on the air exactly what a scene file says.

Every receiver is compared on these samples, so a level, a frequency, a
sideband or a marker time that is off here is off for every result. Most
checks render without noise, where the only error left is the renderer's
own.
"""
import hashlib
import math
import os
import resource
import subprocess
import sys
import tempfile
import time
import unittest

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import scene  # noqa: E402


def write_scene(test, text):
    fd, path = tempfile.mkstemp(suffix=".scene", prefix="scene-test-")
    with os.fdopen(fd, "w") as f:
        f.write(text)
    test.addCleanup(os.remove, path)
    return path


def render(path, seconds, seed=1):
    """Complex samples (or real, for a real scene) as float64."""
    s = scene.Scene(path, seed)
    r = scene.Renderer(s)
    total = int(round(seconds * s.rate))
    parts, got = [], 0
    while got < total:
        band = r.next_block()[: total - got]
        parts.append(band)
        got += len(band)
    x = np.concatenate(parts)
    return (x.real if s.real else x), s


def tone_at(x, rate, freq):
    """Amplitude of a complex exponential at `freq` Hz over the whole of x."""
    n = np.arange(len(x))
    return abs(np.mean(x * np.exp(-2j * np.pi * freq * n / rate)))


class SceneTests(unittest.TestCase):
    RATE = 2048000
    CENTER = 7_100_000

    def band(self, signals, noise=None):
        head = f"[scene]\nrate = {self.RATE}\ncenter = {self.CENTER}\n"
        if noise is not None:
            head += f"noise = {noise}\n"
        return write_scene(self, head + signals)

    def test_same_seed_same_samples_other_seed_other_noise(self):
        path = self.band("[signal c]\nkind = carrier\nfreq = 7.150M\nlevel = -40\n", noise=-97)

        def digest(seed):
            out = subprocess.run([sys.executable, os.path.join(HERE, "scene.py"), path, "--seed", str(seed),
                                  "--seconds", "1", "--format", "cu8"], capture_output=True, check=True).stdout
            self.assertEqual(len(out), 2 * self.RATE)
            return hashlib.sha256(out).hexdigest()

        self.assertEqual(digest(1), digest(1))
        self.assertNotEqual(digest(1), digest(2))

    def test_a_carrier_between_bins_has_its_level_frequency_and_no_spurs(self):
        f = 7_123_456.789
        path = self.band(f"[signal c]\nkind = carrier\nfreq = {f}\nlevel = -30\n")
        x, _ = render(path, 2.0)
        # Level to 0.05 dB, and the frequency: the amplitude at f is the full
        # level only if the phase does not wander, block to block.
        a = tone_at(x, self.RATE, f - self.CENTER)
        self.assertAlmostEqual(20 * math.log10(a), -30, delta=0.05)
        off = tone_at(x, self.RATE, f - self.CENTER + 0.5)
        self.assertLess(off, a * 0.1)
        # Nothing else in the band: block edges or the filter would show as
        # lines; 100 dB down is the interpolation filter's design.
        w = np.hanning(len(x))
        spec = np.abs(np.fft.fft(x * w)) / np.sum(w)
        freqs = np.fft.fftfreq(len(x), 1 / self.RATE)
        away = np.abs(freqs - (f - self.CENTER)) > 500
        self.assertLess(20 * math.log10(spec[away].max() / a), -100)

    def test_usb_puts_the_audio_above_the_dial_and_nothing_below(self):
        dial = 7_074_000
        path = self.band(f"[signal t]\nkind = ssb\nsideband = usb\nfreq = {dial}\naudio = tone:1000\nlevel = -35\n")
        x, _ = render(path, 2.0)
        x = x[self.RATE // 4:]  # past the filter's start
        wanted = tone_at(x, self.RATE, dial + 1000 - self.CENTER)
        image = tone_at(x, self.RATE, dial - 1000 - self.CENTER)
        self.assertAlmostEqual(20 * math.log10(wanted), -35, delta=0.1)
        self.assertLess(20 * math.log10(image / wanted), -90)
        path = self.band(f"[signal t]\nkind = ssb\nsideband = lsb\nfreq = {dial}\naudio = tone:1000\nlevel = -35\n")
        x, _ = render(path, 2.0)
        x = x[self.RATE // 4:]
        self.assertLess(20 * math.log10(tone_at(x, self.RATE, dial + 1000 - self.CENTER)
                                        / tone_at(x, self.RATE, dial - 1000 - self.CENTER)), -90)

    def test_markers_are_on_the_air_exactly_when_the_scene_says(self):
        f = 7_160_000
        path = self.band(f"[signal m]\nkind = marker\nfreq = {f}\nlevel = -30\nperiod = 0.5s\nstart = 0.1s\n"
                         "chip = 5ms\nedge = 1ms\n")
        x, _ = render(path, 2.2)
        n = np.arange(len(x))
        env = np.abs(x * np.exp(-2j * np.pi * (f - self.CENTER) * n / self.RATE))
        full = 10 ** (-30 / 20)
        # The first chip of each marker is on; its rising edge passes half
        # amplitude at the scheduled time, to within two samples (1 us).
        for j in range(4):
            due = int(round((0.1 + 0.5 * j) * self.RATE))
            window = env[due - 2000:due + 2000]
            crossing = np.flatnonzero(window >= full / 2)[0] + due - 2000
            self.assertLessEqual(abs(crossing - due), 2, f"marker {j} at sample {crossing}, due {due}")

    def test_the_noise_floor_has_its_density(self):
        path = self.band("", noise=-97)
        x, _ = render(path, 2.0)
        seg = x[: (len(x) // 8192) * 8192].reshape(-1, 8192)
        w = np.hanning(8192)
        psd = np.mean(np.abs(np.fft.fft(seg * w, axis=1)) ** 2, axis=0) / np.sum(w ** 2) / self.RATE
        self.assertAlmostEqual(10 * math.log10(np.mean(psd)), -97, delta=0.1)

    def test_snr_sets_power_against_the_noise_in_2500_hz(self):
        path = self.band("[signal t]\nkind = carrier\nfreq = 7.130M\nsnr = -10\n", noise=-97)
        s = scene.Scene(path, 1)
        power = s.signals[0].amplitude ** 2
        self.assertAlmostEqual(10 * math.log10(power), -97 + 10 * math.log10(2500) - 10, places=6)

    def test_renders_faster_than_real_time(self):
        path = os.path.join(HERE, "..", "scenes", "smoke.scene")
        started = time.monotonic()
        render(path, 5.0)
        self.assertLess(time.monotonic() - started, 2.5)

    def test_a_real_wideband_scene_puts_a_carrier_where_it_belongs(self):
        rate = 60_000_000
        path = write_scene(self, f"[scene]\nrate = {rate}\nreal = yes\n\n[signal c]\nkind = carrier\n"
                                 "freq = 10.000M\nlevel = -20\n")
        x, _ = render(path, 0.05)
        # A real cosine of the complex level's amplitude.
        self.assertAlmostEqual(20 * math.log10(2 * tone_at(x, rate, 10e6)), -20, delta=0.05)


if __name__ == "__main__":
    resource.setrlimit(resource.RLIMIT_FSIZE, (256 << 20, 256 << 20))
    unittest.main(verbosity=2)
