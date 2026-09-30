#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Checks that pace delivers samples the way the lab's method says it does.

Every check reads what a receiver would read and compares it with pace's own
log, so a bug in either shows up as a disagreement. The input is a counter:
sample n carries the number n, which makes every lost or repeated sample
visible and tells which sample index follows a gap.
"""
import json
import os
import resource
import shutil
import socket
import struct
import subprocess
import tempfile
import threading
import time
import unittest

import numpy as np

HERE = os.path.dirname(os.path.abspath(__file__))
PACE = os.environ.get("PACE") or os.path.join(HERE, "build", "pace")


def read_log(path):
    with open(path) as f:
        return [json.loads(line) for line in f if line.strip()]


def feed_counter(pipe, dtype, stop, rate_limit=None):
    """Writes sample indices, as dtype, until stop is set or the pipe closes."""
    n = 0
    block = 1 << 16
    started = time.monotonic()
    try:
        while not stop.is_set():
            values = np.arange(n, n + block, dtype=np.uint64).astype(dtype)
            pipe.write(values.tobytes())
            n += block
            if rate_limit:
                ahead = n / rate_limit - (time.monotonic() - started)
                if ahead > 0:
                    time.sleep(ahead)
    except (BrokenPipeError, ValueError):
        pass


class Reader:
    """Reads a FIFO or socket, checks the counter, and times every read."""

    def __init__(self, dtype, frame):
        self.dtype = dtype
        self.frame = frame
        self.partial = b""
        self.expected = 0
        self.gaps = []  # (first missing index, count)
        self.times = []  # (monotonic ns after the read, samples so far)
        self.samples = 0
        self.wrap = None  # counters that wrap, such as 16-bit ones

    def feed(self, data):
        now = time.monotonic_ns()
        data = self.partial + data
        whole = len(data) - len(data) % self.frame
        self.partial = data[whole:]
        if whole == 0:
            return
        values = np.frombuffer(data[:whole], dtype=self.dtype).astype(np.int64)
        if self.wrap:
            # Each value is the index modulo wrap; unwrap against expectation.
            base = self.expected - self.expected % self.wrap
            values = values + base
            values[values < self.expected - self.wrap // 2] += self.wrap
        jumps = np.nonzero(np.diff(np.concatenate(([self.expected - 1], values))) != 1)[0]
        for j in jumps:
            before = self.expected - 1 if j == 0 else values[j - 1]
            self.gaps.append((int(before + 1), int(values[j] - before - 1)))
        self.expected = int(values[-1]) + 1
        self.samples += len(values)
        self.times.append((now, self.samples))


def rate_from(times, nominal, skip_s=1.0):
    """Samples per second, from the reads that came earliest.

    A read can only be late, never early: the reader, a Python thread on a
    shared machine, gets scheduled when it gets scheduled. A straight fit
    through every read carries those delays into the slope and misses 1 ppm
    in 20 s now and then. Each second's earliest read, relative to the
    nominal clock, is the one least delayed, and the line through those
    measures the clock to a few hundredths of a ppm."""
    t = np.array([x[0] for x in times], dtype=np.float64) / 1e9
    s = np.array([x[1] for x in times], dtype=np.float64)
    keep = t > t[0] + skip_s
    t, s = t[keep] - t[0], s[keep]
    residual = t - s / nominal
    second = np.floor(t).astype(np.int64)
    picks = [np.flatnonzero(second == k)[np.argmin(residual[second == k])] for k in np.unique(second)]
    slope, _ = np.polyfit(t[picks], s[picks], 1)
    return slope


def arrival_offsets_us(times, start):
    """For every read: how long after its transfer was due it arrived.

    Transfer p is due when its last sample has been taken, at t0 + (p + 1)
    * chunk / clock on CLOCK_MONOTONIC, which is also what
    time.monotonic_ns reads. A read that returns data up to sample s holds
    transfer ceil(s / chunk) - 1 as its newest. Returns (time since t0 in
    s, offset in us) per read."""
    chunk = start["chunk_samples"]
    clock = start["clock"]
    t0 = start["t0_mono_ns"]
    out = []
    for t_ns, samples in times:
        newest = -(-samples // chunk) - 1
        due = t0 + (newest + 1) * chunk / clock * 1e9
        out.append(((t_ns - t0) / 1e9, (t_ns - due) / 1e3))
    return out


def scratch_dir(test):
    """A temporary directory that goes away with the test, pass or fail."""
    d = tempfile.mkdtemp(prefix="pace-test-")
    test.addCleanup(shutil.rmtree, d, True)
    return d


def start_pace(args, stdin=subprocess.PIPE):
    return subprocess.Popen([PACE] + args, stdin=stdin)


class FifoTests(unittest.TestCase):
    def setUp(self):
        self.dir = scratch_dir(self)
        self.fifo = os.path.join(self.dir, "iq")
        self.log = os.path.join(self.dir, "log.jsonl")

    def run_fifo(self, rate, seconds, ppm=0.0, pause=None, buffer_ms=1000):
        """Runs pace with a counter on stdin into a FIFO and reads it.

        pause: (at, for) in seconds, a reader that stops reading meanwhile."""
        pace = start_pace(["--rate", str(rate), "--ppm", str(ppm), "--format", "cs16", "--out", "fifo:" + self.fifo,
                           "--log", self.log, "--buffer-ms", str(buffer_ms), "--duration", str(seconds)])
        stop = threading.Event()
        feeder = threading.Thread(target=feed_counter, args=(pace.stdin, "<u4", stop), daemon=True)
        feeder.start()
        while not os.path.exists(self.fifo):
            time.sleep(0.01)
        reader = Reader("<u4", 4)
        fd = os.open(self.fifo, os.O_RDONLY)
        opened = time.monotonic()
        paused = False
        while True:
            if pause and not paused and time.monotonic() - opened >= pause[0]:
                time.sleep(pause[1])
                paused = True
            data = os.read(fd, 1 << 20)
            if not data:
                break
            reader.feed(data)
        os.close(fd)
        stop.set()
        pace.stdin.close()
        pace.wait(timeout=10)
        feeder.join(timeout=5)
        return reader, read_log(self.log)

    def test_rate_counter_and_timing(self):
        # A minute: even the earliest reads scatter by some 20 us, which in
        # 20 s is a ppm of slope; in 60 s it is a sixth of one.
        rate = 2048000
        reader, log = self.run_fifo(rate, 60)
        start = log[0]
        self.assertEqual(start["ev"], "start")
        self.assertEqual(start["chunk_samples"], 8192)
        self.assertEqual(reader.gaps, [])
        end = log[-1]
        self.assertEqual(end["ev"], "end")
        self.assertEqual(end["dropped_full"] + end["dropped_absent"] + end["discarded"], 0)
        # Everything written arrived; what was still queued at the end did not.
        self.assertEqual(reader.samples, end["written"])
        measured = rate_from(reader.times, rate)
        self.assertLess(abs(measured / rate - 1) * 1e6, 1.0, f"rate {measured:.3f}")
        secs = [e for e in log if e["ev"] == "sec"]
        self.assertGreaterEqual(len(secs), 59)
        self.assertTrue(start["sched"].startswith("fifo"), start["sched"])
        self.assertLess(max(e["late_p99_us"] for e in secs), 1000)
        self.assertEqual(sum(e["input_wait_us"] for e in secs), 0)
        # The antenna time of sample n is t0 + n / clock only if no transfer
        # leaves before its last sample is due, and each leaves soon after.
        offsets = arrival_offsets_us(reader.times, start)
        self.assertGreater(min(o for _, o in offsets), -100, "a transfer arrived before it was due")
        by_second = {}
        for t, o in offsets:
            by_second.setdefault(int(t), []).append(o)
        earliest = [min(v) for k, v in sorted(by_second.items()) if k >= 1]
        self.assertLess(max(earliest), 1000, f"earliest arrival per second up to {max(earliest):.0f} us after due")

    def test_clock_offset_in_ppm(self):
        # The offset has to be there, and in the right direction; 20 s
        # measure it to within about 3 ppm, the minute-long test above
        # checks the clock itself.
        rate = 2048000
        reader, log = self.run_fifo(rate, 20, ppm=100)
        want = rate * (1 + 100e-6)
        self.assertAlmostEqual(log[0]["clock"], want, places=3)
        measured = rate_from(reader.times, want)
        self.assertLess(abs(measured / want - 1) * 1e6, 3.0, f"rate {measured:.3f} want {want:.3f}")

    def test_a_reader_that_stops_loses_what_does_not_fit(self):
        rate = 2048000
        reader, log = self.run_fifo(rate, 10, pause=(3, 2))
        drops = [e for e in log if e["ev"] == "drop"]
        self.assertTrue(drops)
        self.assertTrue(all(d["why"] == "full" for d in drops))
        # The gaps in the counter are exactly the logged drops: dropped
        # transfers consumed their samples, so what follows a gap is the
        # sample with the index after it.
        self.assertEqual([(d["first"], d["samples"]) for d in drops], reader.gaps)
        lost = sum(d["samples"] for d in drops)
        # Two seconds stopped: one second waits in the queue and a few
        # milliseconds in the pipe, the rest is lost, in whole transfers.
        self.assertEqual(lost % 8192, 0)
        self.assertGreater(lost, 0.95 * rate)
        self.assertLess(lost, 1.0 * rate)
        end = log[-1]
        self.assertEqual(end["dropped_full"], lost)


class ReconnectTests(unittest.TestCase):
    def test_every_sample_arrives_or_is_accounted_across_a_reconnect(self):
        d = scratch_dir(self)
        fifo, log_path = os.path.join(d, "iq"), os.path.join(d, "log.jsonl")
        pace = start_pace(["--rate", "2048000", "--format", "cs16", "--out", "fifo:" + fifo, "--log", log_path,
                           "--duration", "6"])
        stop = threading.Event()
        feeder = threading.Thread(target=feed_counter, args=(pace.stdin, "<u4", stop), daemon=True)
        feeder.start()
        while not os.path.exists(fifo):
            time.sleep(0.01)

        def session(read_s, stall_s):
            fd = os.open(fifo, os.O_RDONLY)
            reader = Reader("<u4", 4)
            first = None
            end = time.monotonic() + read_s
            while time.monotonic() < end:
                data = os.read(fd, 1 << 20)
                if not data:
                    break
                if first is None and len(data) >= 4:
                    first = struct.unpack("<I", data[:4])[0]
                    reader.expected = first
                reader.feed(data)
            # Stop reading with data waiting in the pipe, then go away.
            time.sleep(stall_s)
            os.close(fd)
            return reader, first

        one, first_one = session(2.0, 0.3)
        time.sleep(0.5)
        two, first_two = session(1.5, 0.0)
        fd = os.open(fifo, os.O_RDONLY)  # a last reader drains to the end
        three = Reader("<u4", 4)
        data = os.read(fd, 1 << 20)
        three.expected = struct.unpack("<I", data[:4])[0] if len(data) >= 4 else 0
        first_three = three.expected
        while data:
            three.feed(data)
            data = os.read(fd, 1 << 20)
        os.close(fd)
        stop.set()
        pace.stdin.close()
        pace.wait(timeout=10)
        log = read_log(log_path)
        end = log[-1]
        self.assertEqual(end["ev"], "end")
        self.assertEqual(first_one, 0)
        for r in (one, two, three):
            self.assertEqual(r.gaps, [])
        # Each new receiver starts on a transfer boundary.
        self.assertEqual(first_two % 8192, 0)
        self.assertEqual(first_three % 8192, 0)
        # What the first reader left behind is logged as discarded, from the
        # first sample it did not read.
        discarded = [e for e in log if e["ev"] == "drop" and e["why"] == "discarded"]
        self.assertTrue(discarded)
        self.assertEqual(discarded[0]["first"], one.expected)
        # Every sample arrived or was accounted for, exactly once.
        arrived = one.samples + two.samples + three.samples
        accounted = end["dropped_full"] + end["dropped_absent"] + end["discarded"] + end["queued_at_end"]
        self.assertEqual(arrived + accounted, end["chunks"] * 8192)
        ranges = sorted((e["first"], e["samples"]) for e in log if e["ev"] == "drop")
        covered = sum(n for _, n in ranges)
        self.assertEqual(covered, end["dropped_full"] + end["dropped_absent"] + end["discarded"])

    def test_a_regular_file_is_refused(self):
        d = scratch_dir(self)
        path = os.path.join(d, "not-a-fifo")
        open(path, "w").close()
        pace = subprocess.run([PACE, "--rate", "2048000", "--format", "cs16", "--out", "fifo:" + path, "--log",
                               os.path.join(d, "log.jsonl"), "--duration", "1"], input=b"\0" * (1 << 20),
                              capture_output=True, timeout=20)
        self.assertNotEqual(pace.returncode, 0)
        self.assertIn(b"not a FIFO", pace.stderr)
        self.assertEqual(os.path.getsize(path), 0)


class InputTests(unittest.TestCase):
    def test_a_slow_renderer_shows_as_input_wait(self):
        d = scratch_dir(self)
        fifo, log_path = os.path.join(d, "iq"), os.path.join(d, "log.jsonl")
        pace = start_pace(["--rate", "2048000", "--format", "cs16", "--out", "fifo:" + fifo, "--log", log_path,
                           "--duration", "4"])
        stop = threading.Event()
        # Half the rate: the renderer cannot keep up.
        feeder = threading.Thread(target=feed_counter, args=(pace.stdin, "<u4", stop, 1024000), daemon=True)
        feeder.start()
        while not os.path.exists(fifo):
            time.sleep(0.01)
        fd = os.open(fifo, os.O_RDONLY)
        reader = Reader("<u4", 4)
        while True:
            data = os.read(fd, 1 << 20)
            if not data:
                break
            reader.feed(data)
        os.close(fd)
        stop.set()
        pace.stdin.close()
        pace.wait(timeout=10)
        log = read_log(log_path)
        self.assertGreater(sum(e.get("input_wait_us", 0) for e in log if e["ev"] == "sec"), 500000)
        self.assertEqual(reader.gaps, [])


class RtlTcpTests(unittest.TestCase):
    def test_header_commands_stall_and_a_client_that_comes_back(self):
        # cs16 rather than rtl_tcp's usual cu8: pace does not care about the
        # format, and a 32-bit counter shows every gap exactly.
        d = scratch_dir(self)
        log_path = os.path.join(d, "log.jsonl")
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        pace = start_pace(["--rate", "2048000", "--format", "cs16", "--out", f"rtltcp:127.0.0.1:{port}", "--log",
                           log_path, "--duration", "11"])
        stop = threading.Event()
        feeder = threading.Thread(target=feed_counter, args=(pace.stdin, "<u4", stop), daemon=True)
        feeder.start()

        def connect():
            for _ in range(500):
                try:
                    return socket.create_connection(("127.0.0.1", port))
                except ConnectionRefusedError:
                    time.sleep(0.02)
            raise AssertionError("pace did not listen")

        def read_for(sock, seconds, stall=None):
            reader = Reader("<u4", 4)
            first = struct.unpack("<I", sock.recv(4, socket.MSG_WAITALL))[0]
            reader.expected = first + 1
            begun = time.monotonic()
            stalled = False
            while time.monotonic() < begun + seconds:
                if stall and not stalled and time.monotonic() >= begun + stall[0]:
                    time.sleep(stall[1])
                    stalled = True
                data = sock.recv(1 << 20)
                if not data:
                    break
                reader.feed(data)
            return reader, first

        s1 = connect()
        header = s1.recv(12, socket.MSG_WAITALL)
        self.assertEqual(header, b"RTL0" + struct.pack(">II", 5, 29))
        s1.sendall(struct.pack(">BI", 1, 7100000) + struct.pack(">BI", 2, 2048000) + struct.pack(">BI", 3, 1))
        # Three seconds after the stall drain what TCP and the queue held.
        first, first_one = read_for(s1, 6, stall=(1.0, 2.0))
        s1.close()
        time.sleep(1)
        s2 = connect()
        self.assertEqual(s2.recv(12, socket.MSG_WAITALL)[:4], b"RTL0")
        second, first_two = read_for(s2, 2)
        s2.close()
        stop.set()
        pace.stdin.close()
        pace.wait(timeout=15)
        log = read_log(log_path)
        cmds = [(e["cmd"], e["arg"]) for e in log if e["ev"] == "cmd"]
        self.assertEqual(cmds, [(1, 7100000), (2, 2048000), (3, 1)])
        self.assertEqual([e["state"] for e in log if e["ev"] == "client"], ["closed", "connected", "closed"])
        # The stall lost whole transfers, and exactly the logged ones, as far
        # as the first reader got before it hung up.
        full = [(e["first"], e["samples"]) for e in log if e["ev"] == "drop" and e["why"] == "full"
                and e["first"] < first.expected]
        self.assertTrue(full)
        self.assertEqual(first.gaps, full)
        self.assertEqual(second.gaps, [])
        self.assertEqual(first_one, 0)
        self.assertEqual(first_two % 8192, 0)
        absent = [e["samples"] for e in log if e["ev"] == "drop" and e["why"] == "absent"]
        self.assertGreater(absent[0], 0.8 * 2048000)
        self.assertLess(absent[0], 1.5 * 2048000)


class WidebandTests(unittest.TestCase):
    def test_looped_signal_with_fresh_noise_at_60_msps(self):
        rate = 60_000_000
        d = scratch_dir(self)
        fifo, log_path, loop = (os.path.join(d, x) for x in ("iq", "log.jsonl", "loop.s16"))
        # One second of a 1 MHz tone at 1000 of full scale: 60 samples a
        # period, repeated, which keeps the test's memory small.
        period = np.round(1000 * np.sin(2 * np.pi * np.arange(60) / 60)).astype("<i2")
        tone = np.tile(period, rate // 60)
        tone.tofile(loop)
        pace = start_pace(["--rate", str(rate), "--format", "s16", "--out", "fifo:" + fifo, "--log", log_path,
                           "--loop", loop, "--noise-rms", "100", "--seed", "7", "--duration", "5"], stdin=None)
        while not os.path.exists(fifo):
            time.sleep(0.01)
        fd = os.open(fifo, os.O_RDONLY)
        reader_times, got = [], 0  # got counts bytes: a read may end inside a sample
        # The same loop position, one loop apart.
        want = [(rate + 1000, 200000), (2 * rate + 1000, 200000)]
        collected = {first: bytearray() for first, _ in want}
        while True:
            data = os.read(fd, 1 << 20)
            if not data:
                break
            start, got = got, got + len(data)
            reader_times.append((time.monotonic_ns(), got // 2))
            for first, count in want:
                lo, hi = max(first * 2, start), min((first + count) * 2, got)
                if lo < hi:
                    collected[first] += data[lo - start:hi - start]
        os.close(fd)
        windows = {first: np.frombuffer(bytes(buf), dtype="<i2").astype(np.int64) for first, buf in collected.items()}
        pace.wait(timeout=10)
        log = read_log(log_path)
        end = log[-1]
        self.assertEqual(end["dropped_full"] + end["dropped_absent"], 0)
        secs = [e for e in log if e["ev"] == "sec"]
        self.assertLess(max(e["late_p99_us"] for e in secs), 1000)
        # Reads of a megabyte are too coarse to time a clock to 1 ppm in five
        # seconds; the narrowband tests check the clock, this one the
        # throughput.
        measured = rate_from(reader_times, rate)
        self.assertLess(abs(measured / rate - 1) * 1e6, 100, f"rate {measured:.1f}")
        a, b = windows[want[0][0]], windows[want[1][0]]
        signal = tone[1000:1000 + 200000].astype(np.int64)
        na, nb = a - signal, b - signal
        # Noise of the requested strength, centred, and new on every pass.
        self.assertLess(abs(na.mean()), 1.0)
        self.assertAlmostEqual(na.std() / 100, 1.0, delta=0.01)
        self.assertAlmostEqual(nb.std() / 100, 1.0, delta=0.01)
        self.assertLess(abs(np.corrcoef(na, nb)[0, 1]), 0.01)


if __name__ == "__main__":
    if not os.path.exists(PACE):
        raise SystemExit("build pace first: make -C bench/source")
    # No file this test or pace writes may pass 256 MiB: a broken pace that
    # logs in a loop must fail the test, not fill the machine's disk.
    resource.setrlimit(resource.RLIMIT_FSIZE, (256 << 20, 256 << 20))
    unittest.main(verbosity=2)
