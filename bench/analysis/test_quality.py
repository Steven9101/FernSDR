#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
import os
import sys
import unittest

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import quality  # noqa: E402

RATE = 16000


def tones(seconds=20, noise_db_hz=-97.0, level_db=-50.0, scale=1.0, wobble_hz=0.0, wobble_rate=0.2, seed=1):
    """The tones scene as audio: every tone at level_db (power), white noise
    at noise_db_hz, frequencies times scale, and a slow sine wobble of the
    playback rate, wobble_rate times a second, that moves the 1000 Hz tone
    by +-wobble_hz."""
    t = np.arange(int(seconds * RATE)) / RATE
    a = np.sqrt(2 * 10 ** (level_db / 10))
    # Rate factor 1 + d sin(2 pi wobble_rate t); the phase is its integral.
    d = wobble_hz / 1000
    warp = t + d * (1 - np.cos(2 * np.pi * wobble_rate * t)) / (2 * np.pi * wobble_rate)
    x = sum(a * np.cos(2 * np.pi * f * scale * warp) for f in quality.TONES)
    rng = np.random.default_rng(seed)
    return x + rng.normal(0, np.sqrt(10 ** (noise_db_hz / 10) * RATE / 2), len(t))


def expected_sinad(noise_db_hz=-97.0, level_db=-50.0):
    inside = [t for t in quality.TONES if 300 <= t <= 2700]
    # Real noise at N dB/Hz over [0, rate/2] is N + 10 log10(bandwidth).
    return level_db + 10 * np.log10(len(inside)) - (noise_db_hz + 10 * np.log10(2400 - 7 * len(inside)))


class ToneQuality(unittest.TestCase):
    def test_sinad_matches_the_noise_density(self):
        q = quality.tone_quality(tones(), RATE)
        self.assertAlmostEqual(q["sinad_db"], expected_sinad(), delta=0.5)
        self.assertAlmostEqual(q["snr_db"], expected_sinad(), delta=0.5)
        self.assertLess(abs(q["pitch_error_hz"]), 0.05)
        self.assertLess(abs(q["scale_error_ppm"]), 50)

    def test_flat_passband(self):
        q = quality.tone_quality(tones(), RATE)
        for t in quality.TONES:
            self.assertAlmostEqual(q["passband_db"][str(t)], 0.0, delta=0.3)

    def test_wander_is_reported_not_counted_as_noise(self):
        # The regression that motivated per-window tracking: a spectrum over
        # the whole recording lost about 8 dB here.
        q = quality.tone_quality(tones(wobble_hz=1.5), RATE)
        self.assertAlmostEqual(q["sinad_db"], expected_sinad(), delta=2.0)
        self.assertGreater(q["pitch_wander_hz"], 1.5)

    def test_flutter_lowers_sinad_not_snr(self):
        # FernSDR's player steering its buffer: the pitch moves about once a
        # second, inside one analysis window.
        q = quality.tone_quality(tones(wobble_hz=2.0, wobble_rate=1.5), RATE)
        self.assertLess(q["sinad_db"], expected_sinad() - 3)
        self.assertAlmostEqual(q["snr_db"], expected_sinad(), delta=1.0)

    def test_scale_error(self):
        q = quality.tone_quality(tones(scale=1 - 660e-6), RATE)
        self.assertAlmostEqual(q["scale_error_ppm"], -660, delta=60)

    def test_more_noise_lowers_sinad(self):
        q = quality.tone_quality(tones(noise_db_hz=-87.0), RATE)
        self.assertAlmostEqual(q["sinad_db"], expected_sinad(-87.0), delta=0.5)
        self.assertAlmostEqual(q["snr_db"], expected_sinad(-87.0), delta=0.5)

    def test_ideal_receiver_matches_the_scene(self):
        q = quality.ideal(seconds=10)
        self.assertAlmostEqual(q["sinad_db"], expected_sinad(), delta=0.5)
        self.assertLess(abs(q["pitch_error_hz"]), 0.05)


if __name__ == "__main__":
    unittest.main()
