#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import json  # noqa: E402
import tempfile  # noqa: E402

from digital_report import aggregate  # noqa: E402
from links_report import input_dropped, outage_recovery, summarise, validity_of  # noqa: E402

EVENTS = [{"t": 100.0, "what": "shaped"}, {"t": 105.0, "what": "outage on"}, {"t": 120.0, "what": "outage off"}]


def markers(latency_after_ms):
    before = [{"wall": 100.0 + 0.5 * i, "to_destination_ms": 300.0} for i in range(10)]
    after = [{"wall": 121.0 + 0.5 * i, "to_destination_ms": lat} for i, lat in enumerate(latency_after_ms)]
    return before + after


class OutageRecovery(unittest.TestCase):
    def test_back_at_the_old_latency(self):
        r = outage_recovery(markers([350, 300, 300]), EVENTS)
        self.assertAlmostEqual(r["first_heard_s"], 1.0)
        self.assertAlmostEqual(r["recovery_s"], 1.0)

    def test_heard_but_playing_the_backlog(self):
        r = outage_recovery(markers([16000, 15500, 15000]), EVENTS)
        self.assertAlmostEqual(r["first_heard_s"], 1.0)
        self.assertIsNone(r["recovery_s"])

    def test_catches_up_later(self):
        r = outage_recovery(markers([9000, 5000, 1400, 400]), EVENTS)
        self.assertAlmostEqual(r["recovery_s"], 2.5)

    def test_nothing_after_the_outage(self):
        r = outage_recovery(markers([]), EVENTS)
        self.assertIsNone(r["first_heard_s"])
        self.assertIsNone(r["recovery_s"])

    def test_no_outage_in_the_profile(self):
        self.assertEqual(outage_recovery(markers([300]), EVENTS[:1]), {})


class InputDropped(unittest.TestCase):
    def pace(self, events):
        d = tempfile.mkdtemp()
        os.mkdir(os.path.join(d, "receiver"))
        with open(os.path.join(d, "receiver", "pace.jsonl"), "w") as f:
            f.write(json.dumps({"ev": "start", "t0_real_ns": 1000 * 10**9, "clock": 1000.0}) + "\n")
            for e in events:
                f.write(json.dumps(e) + "\n")
        return d

    def test_counts_only_inside_the_window(self):
        # 2 s behind at t = 1010 s, 3 s without a reader at t = 1050 s.
        d = self.pace([{"ev": "drop", "first": 10000, "samples": 2000, "why": "full"},
                       {"ev": "drop", "first": 50000, "samples": 3000, "why": "absent"}])
        self.assertEqual(input_dropped(d), {"full": 2.0, "absent": 3.0, "discarded": 0.0})
        self.assertEqual(input_dropped(d, 1020_000, 1045_000), {"full": 0.0, "absent": 0.0, "discarded": 0.0})
        self.assertEqual(input_dropped(d, 1011_000, 1051_000), {"full": 1.0, "absent": 1.0, "discarded": 0.0})

    def test_queued_for_a_receiver_that_went_away(self):
        d = self.pace([{"ev": "drop", "first": 0, "samples": 500, "why": "discarded"}])
        self.assertEqual(input_dropped(d)["discarded"], 0.5)

    def test_no_log(self):
        self.assertIsNone(input_dropped(tempfile.mkdtemp()))


def job(rep, markers_share, gates=None, lost=None):
    return {"rep": rep, "markers_share": markers_share, "gates": gates or {"audio": True, "clock": True},
            "input_dropped_s": lost or {}}


class InvalidJobs(unittest.TestCase):
    table = {"rx": {"rate24": [job("1", 0.9), job("2", 0.1, gates={"audio": True, "clock": False}),
                               job("3", 0.2, lost={"full": 1.5})],
                    "listen": [job(str(i), 1.0, gates={"audio": True, "clock": False}) for i in (1, 2, 3)]}}

    def test_summaries_leave_invalid_jobs_out(self):
        s = summarise(self.table)["rx"]["rate24"]
        self.assertEqual(s["runs"], 1)
        self.assertEqual(s["invalid_runs"], 2)
        self.assertAlmostEqual(s["markers_share"]["median"], 0.9)

    def test_every_invalid_repetition_counts(self):
        v = validity_of(self.table)["rx"]
        self.assertEqual(v["jobs"], 6)
        self.assertEqual(len(v["invalid"]), 5)

    def test_digital_scores_of_an_invalid_run_are_left_out(self):
        good = {"ft8_threshold_db": -18, "ft8_decodes": 5, "ft8_opportunities": 10, "invalid": []}
        bad = {"ft8_threshold_db": -20, "ft8_decodes": 10, "ft8_opportunities": 10, "invalid": ["clock"]}
        a = aggregate([good, bad], 250)
        self.assertEqual(a["runs"], 1)
        self.assertEqual(a["invalid_runs"], 1)
        self.assertEqual(a["ft8_threshold_db"]["median"], -18)
        self.assertAlmostEqual(a["ft8_decode_share"]["median"], 0.5)


if __name__ == "__main__":
    unittest.main()
