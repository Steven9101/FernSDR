#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""The latency analysis measures a delay it was built to know.

An ideal receiver demodulates the markers scene, its audio is "played"
with a known delay through a made-up audio clock, and the analysis must
find that delay for every marker: longer than a marker period (which only
the labels resolve), with a clock that runs slow, and through holes.
"""
import json
import os
import shutil
import sys
import tempfile
import unittest

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
BENCH = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(BENCH, "source"))
sys.path.insert(0, HERE)
import latency  # noqa: E402
import scene  # noqa: E402

RATE = 2048000
AUDIO = 32000
DIAL = 7_159_200


def ideal_usb(seconds, seed=3):
    """Audio-rate oracle with independently fixed chips and identity prefix.

    No wideband rendering or FFT is involved: these samples are the ideal
    demodulator output, before any receiver or codec can alter it.
    """
    marker = {"freq": 7160000, "chips": 31, "chip_s": .005, "period_s": .5,
              "start_s": .1, "edge_s": .001, "labels": 31, "label_shift": 2,
              "identity": "pairs-v1", "identity_sequence": scene.marker_identity(31), "framing": "double-pilot-v1"}
    sequence = [0, 0, 1, 0, 2, 0, 3, 0, 4, 0, 5, 0, 6, 0, 7, 0, 8, 0, 9, 0, 10, 0, 11, 0]
    bits = np.array([int(c) for c in "1111100011011101010000100101100"])
    env = np.zeros(int(seconds * AUDIO))
    for m, symbol in enumerate(sequence):
        offset = round((.1 + m * .5) * AUDIO)
        chips = np.concatenate(([2], np.roll(bits, -symbol * 2), [2]))
        pulse = np.repeat(chips, round(.005 * AUDIO))
        if offset >= len(env): break
        n = min(len(pulse), len(env) - offset)
        env[offset:offset+n] = pulse[:n]
    t = np.arange(len(env)) / AUDIO
    return .1 * env * np.cos(2 * np.pi * 800 * t), marker


class LatencyTests(unittest.TestCase):
    def test_output_clock_step_is_counted_and_followed(self):
        # A 20 ms jump at 30 s (the output underran): samples after it map
        # 20 ms later, samples before it do not, and the jump is counted.
        clocks = [{"contextTime": t, "performanceTime": (t + (.020 if t >= 30 else 0))*1000,
                   "timeOrigin": 1700000000000, "outputLatency": .032}
                  for t in np.arange(.25, 60, .25)]
        to_wall, info = latency.presentation_map(clocks)
        self.assertEqual(info["output_clock_jumps"], 1)
        self.assertAlmostEqual(to_wall(20.0) - 1700000000 - 20.0, 0, places=6)
        self.assertAlmostEqual(to_wall(40.0) - 1700000000 - 40.0, .020, places=6)

    def test_output_clock_interpolates_local_pairs(self):
        clocks = [{"contextTime": t, "performanceTime": (t + (.001 if t == 3 else 0))*1000,
                   "timeOrigin": 1700000000000, "outputLatency": .032}
                  for t in range(1, 8)]
        mapping, _ = latency.presentation_map(clocks)
        self.assertAlmostEqual(mapping(3), 1700000003.001, places=6)

    def setUp(self):
        self.dir = tempfile.mkdtemp(prefix="latency-test-")
        self.addCleanup(shutil.rmtree, self.dir, True)

    def record(self, audio, delay_s, ratio=1.0, holes=()):
        """Writes a listen.mjs-style recording of `audio` presented `delay_s`
        after the antenna, with an audio clock running at `ratio`."""
        t0 = 1_790_000_000.0
        first_frame = 12345
        a = audio.astype("<f4").copy()
        for lo, hi in holes:
            a[int(lo * AUDIO):int(hi * AUDIO)] = np.nan
        a.tofile(os.path.join(self.dir, "audio.f32"))
        with open(os.path.join(self.dir, "audio.json"), "w") as f:
            json.dump({"rate": AUDIO, "firstFrame": first_frame, "holes": int(np.isnan(a).sum())}, f)
        # Sample i (context frame first_frame + i) is presented at
        # t0 + delay + i / AUDIO / ratio on the wall clock.
        origin_ms = (t0 - 7.0) * 1000
        clocks = []
        for k in range(int(len(audio) / AUDIO * 4)):
            i = k * AUDIO // 4
            wall = t0 + delay_s + i / AUDIO / ratio
            clocks.append({"contextTime": (first_frame + i) / AUDIO, "performanceTime": wall * 1000 - origin_ms,
                           "timeOrigin": origin_ms, "outputLatency": 0.025})
        with open(os.path.join(self.dir, "clocks.json"), "w") as f:
            json.dump(clocks, f)
        with open(os.path.join(self.dir, "pace.jsonl"), "w") as f:
            f.write(json.dumps({"ev": "start", "t0_real_ns": int(t0 * 1e9), "clock": RATE}) + "\n")

    def run_analysis(self, marker):
        return latency.analyse(self.dir, os.path.join(self.dir, "pace.jsonl"), marker, RATE, DIAL, "usb")

    def test_finds_a_delay_longer_than_a_marker_period(self):
        audio, marker = ideal_usb(12)
        self.record(audio, 1.2345)
        res = self.run_analysis(marker)
        s = res["summary"]
        self.assertGreaterEqual(s["markers"], 22)
        self.assertEqual(s["markers"], s["expected"])
        for row in res["markers"]:
            self.assertAlmostEqual(row["to_output_ms"], 1234.5, delta=0.2, msg=row)
        self.assertAlmostEqual(s["to_destination_median_ms"], 1234.5 - 25, delta=0.2)
        self.assertAlmostEqual(s["tone_hz"], 800, delta=0.5)

    def test_a_slow_audio_clock_is_mapped_not_mistaken_for_drift(self):
        audio, marker = ideal_usb(12)
        self.record(audio, 0.3, ratio=0.99)
        res = self.run_analysis(marker)
        # A player that does not compensate plays each marker later than the
        # one before: marker m, on the air at T, sounds at D + T / 0.99.
        for row in res["markers"]:
            t = marker["start_s"] + row["marker"] * marker["period_s"]
            self.assertAlmostEqual(row["to_output_ms"], 300 + t * (1 / 0.99 - 1) * 1000, delta=0.3, msg=row)
        self.assertAlmostEqual(res["summary"]["clock"]["clock_ratio"], 0.99, delta=1e-6)
        # The reported slope is per minute of wall time, not source time.
        self.assertAlmostEqual(res["summary"]["drift_ms_per_min"], 60 * (1 - 0.99) * 1000, delta=1)

    def test_thirty_second_delay_does_not_wrap_to_a_recent_marker(self):
        audio, marker = ideal_usb(12)
        self.record(audio, 30.125)
        res = self.run_analysis(marker)
        self.assertGreaterEqual(res["summary"]["markers"], 20)
        for row in res["markers"]:
            self.assertAlmostEqual(row["to_output_ms"], 30125, delta=0.5, msg=row)

    def test_delay_outside_registered_range_is_unscored(self):
        audio, marker = ideal_usb(12)
        self.record(audio, 100)
        self.assertEqual(self.run_analysis(marker)["error"], "no marker found")

    def test_marker_pairs_are_unique_over_the_whole_cycle(self):
        sequence = scene.marker_identity(31)
        self.assertEqual(len(sequence), 961)
        pairs = {(sequence[i], sequence[(i+1) % len(sequence)]) for i in range(len(sequence))}
        self.assertEqual(pairs, {(a,b) for a in range(31) for b in range(31)})

    def test_source_uses_the_known_prefix_without_exceeding_peak(self):
        src = scene.IdentifiedMarker("marker", 7160000, .1, 31, .005, .5, .001, 2, .1)
        actual = src.next(4 * AUDIO)
        expected, _ = ideal_usb(4)
        # Compare the independently written binary envelope at chip centres.
        for m in range(7):
            symbol = [0,0,1,0,2,0,3][m]
            chips = [2] + list(np.roll([int(c) for c in "1111100011011101010000100101100"], -symbol*2)) + [2]
            for k, value in enumerate(chips):
                index = round((.1 + m*.5 + (k+.5)*.005)*AUDIO)
                self.assertAlmostEqual(actual[index], .05*value, delta=1e-10)
        self.assertLessEqual(float(actual.max()), .1 + 1e-12)

    def test_holes_lose_markers_but_not_the_timing(self):
        audio, marker = ideal_usb(12)
        self.record(audio, 0.7, holes=[(3.0, 4.2)])
        res = self.run_analysis(marker)
        s = res["summary"]
        self.assertLess(s["markers"], s["expected"])
        for row in res["markers"]:
            self.assertAlmostEqual(row["to_output_ms"], 700, delta=0.2)


if __name__ == "__main__":
    unittest.main(verbosity=2)
