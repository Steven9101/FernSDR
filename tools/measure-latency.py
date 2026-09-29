#!/usr/bin/env python3
"""Measure FernSDR's end-to-end latency against a running server.

Every other latency figure in this project is arithmetic: block sizes, codec
overlap, buffer targets.  This one is a stopwatch.  It feeds the server IQ
through a FIFO, marks a known instant in that IQ with a tone burst, listens on
the WebSocket like any browser would, and reports how long the burst took to
come back as decoded audio.

What that number covers:

    tone enters the server  ->  channelizer  ->  demodulator  ->  AGC
    ->  NAC encoder  ->  WebSocket  ->  network  ->  NAC decoder

What it does NOT cover, because no tool outside a browser can: the Web Audio
output buffer and the jitter buffer ahead of it.  Those are reported by the
client itself (the "Buffer" readout) and are configurable; add the buffer
target to this figure for the number a listener actually experiences.

Usage - two terminals.  First:

    tools/measure-latency.py --prepare /tmp/fernsdr-latency.fifo

which creates the FIFO and prints the config section to paste into
fernsdr.conf.  Start the server, then:

    tools/measure-latency.py --fifo /tmp/fernsdr-latency.fifo --host 127.0.0.1

Run it across a real network by pointing --host at a remote server that reads
the same FIFO; the difference between the two runs is the network's
contribution, measured rather than assumed.
"""

import argparse
import json
import math
import os
import socket
import struct
import importlib.util
import sys
import threading
import time

# fernsdr-probe.py has a hyphen in its name, so it has to be loaded by path.
# Sharing its decoder rather than copying one keeps the two tools honest: if
# the bitstream changes, both notice at once.
_spec = importlib.util.spec_from_file_location(
    "fernsdr_probe", os.path.join(os.path.dirname(os.path.abspath(__file__)), "fernsdr-probe.py")
)
_probe = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_probe)

WebSocket = _probe.WebSocket
NacDecoder = _probe.NacDecoder
STREAM_AUDIO = _probe.STREAM_AUDIO


CONFIG_TEMPLATE = """\
# Paste into fernsdr.conf, then start the server.
#
# noise_blanker is off deliberately: the burst this tool sends looks a little
# like impulse noise, and a blanker doing its job would attenuate the very
# thing being timed.

[band:latency]
name = Latency test
source = file
path = {fifo}
format = cf32
signal = iq
sample_rate = {rate}
center = {center}
realtime = false
noise_blanker = 0
"""


class BurstWriter(threading.Thread):
    """Feeds the FIFO in real time, marking the start of every burst."""

    def __init__(self, path, rate, tone_hz, period, burst_seconds, amplitude):
        super().__init__(daemon=True)
        self.path = path
        self.rate = rate
        self.tone_hz = tone_hz
        self.period = period
        self.burst_samples = max(1, int(burst_seconds * rate))
        self.amplitude = amplitude
        self.bursts = []          # (sample_index, wall_clock_of_that_sample)
        self.lock = threading.Lock()
        self.stop_flag = threading.Event()
        self.error = None

    def run(self):
        try:
            self._run()
        except Exception as error:  # noqa: BLE001 - reported to the user
            self.error = error

    def _run(self):
        # Opening a FIFO for writing blocks until the server opens the read
        # end, which is exactly the synchronisation we want.
        handle = open(self.path, "wb", buffering=0)
        # Small blocks, written only once their last sample is due. A FIFO
        # holds tens of kilobytes, so writing a big block early would let the
        # server consume the burst before its scheduled instant and the
        # measurement would come out shorter than the truth.
        block = 1024
        index = 0
        next_burst = int(self.rate * 1.0)   # a second of settling first
        phase = 0.0
        step = 2.0 * math.pi * self.tone_hz / self.rate
        # A little noise so the AGC has something to sit on between bursts and
        # is not slamming its gain to maximum into every burst.
        floor = 0.002
        started = time.time()

        while not self.stop_flag.is_set():
            # Wait until the whole block is due before writing any of it. The
            # cost is up to one block of pessimism in the result; the benefit
            # is that the figure is never optimistic.
            delay = started + (index + block) / self.rate - time.time()
            if delay > 0:
                time.sleep(delay)

            samples = []
            for n in range(block):
                absolute = index + n
                in_burst = next_burst <= absolute < next_burst + self.burst_samples
                if absolute == next_burst:
                    # Wall clock at which the server will consume this sample.
                    emit = started + absolute / self.rate
                    with self.lock:
                        self.bursts.append((absolute, emit))
                if in_burst:
                    # Raised-cosine edges: a hard gate splatters across the
                    # whole band and would be hunted down by the blanker.
                    position = absolute - next_burst
                    edge = max(1, self.burst_samples // 8)
                    envelope = 1.0
                    if position < edge:
                        envelope = 0.5 - 0.5 * math.cos(math.pi * position / edge)
                    elif position > self.burst_samples - edge:
                        remaining = self.burst_samples - position
                        envelope = 0.5 - 0.5 * math.cos(math.pi * remaining / edge)
                    gain = self.amplitude * envelope
                else:
                    gain = 0.0
                samples.append(gain * math.cos(phase) + floor)
                samples.append(gain * math.sin(phase))
                phase += step
                if phase > 2.0 * math.pi:
                    phase -= 2.0 * math.pi
            handle.write(struct.pack("<%df" % len(samples), *samples))

            index += block
            if index >= next_burst + self.burst_samples:
                next_burst += int(self.period * self.rate)

        handle.close()


def percentile(values, fraction):
    if not values:
        return float("nan")
    ordered = sorted(values)
    position = fraction * (len(ordered) - 1)
    low = int(math.floor(position))
    high = min(len(ordered) - 1, low + 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def main():
    parser = argparse.ArgumentParser(description="FernSDR end-to-end latency")
    parser.add_argument("--prepare", metavar="FIFO",
                        help="create the FIFO, print a config section, and exit")
    parser.add_argument("--fifo", default="/tmp/fernsdr-latency.fifo")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8073)
    parser.add_argument("--path", default="/ws")
    parser.add_argument("--band", default="latency")
    parser.add_argument("--rate", type=float, default=192000.0, help="FIFO sample rate")
    parser.add_argument("--center", type=float, default=7100000.0)
    parser.add_argument("--offset", type=float, default=2000.0,
                        help="tone offset from the band centre, Hz")
    parser.add_argument("--mode", default="usb")
    parser.add_argument("--period", type=float, default=2.0, help="seconds between bursts")
    parser.add_argument("--burst", type=float, default=0.05, help="burst length, seconds")
    parser.add_argument("--count", type=int, default=10, help="bursts to time")
    arguments = parser.parse_args()

    if arguments.prepare:
        if not os.path.exists(arguments.prepare):
            os.mkfifo(arguments.prepare)
        print(CONFIG_TEMPLATE.format(fifo=arguments.prepare, rate=int(arguments.rate),
                                     center=int(arguments.center)))
        print(f"FIFO ready at {arguments.prepare}. Start the server, then run this "
              f"tool again without --prepare.")
        return 0

    if not os.path.exists(arguments.fifo):
        print(f"{arguments.fifo} does not exist; run with --prepare first.", file=sys.stderr)
        return 2

    writer = BurstWriter(arguments.fifo, arguments.rate, arguments.offset, arguments.period,
                         arguments.burst, amplitude=0.3)
    writer.start()

    client = WebSocket(arguments.host, arguments.port, arguments.path)
    decoder = NacDecoder()
    audio_rate = 12000
    tuned = False
    latencies = []
    # Onsets are found against a running noise floor rather than a fixed
    # threshold: the AGC lifts the quiet gaps between bursts, so what counts as
    # "quiet" is whatever the last second or so looked like.
    recent = []
    armed = False
    matched = 0

    deadline = time.time() + 5.0 + arguments.period * (arguments.count + 2)
    print(f"timing {arguments.count} bursts of {arguments.burst * 1000:.0f} ms, "
          f"one every {arguments.period:.1f} s ...")

    while time.time() < deadline and matched < arguments.count:
        client.sock.settimeout(max(0.1, deadline - time.time()))
        try:
            opcode, payload = client.recv()
        except (socket.timeout, TimeoutError):
            break
        except ConnectionError as error:
            print(f"connection lost: {error}", file=sys.stderr)
            break
        arrival = time.time()

        if opcode == 0x8:
            break
        if opcode == 0x1:
            message = json.loads(payload.decode())
            if message.get("type") == "audio-config":
                audio_rate = int(message["rate"])
            if message.get("type") == "welcome" and not tuned:
                tuned = True
                if "nac2" in message.get("capabilities", []):
                    client.send_text(json.dumps({"type": "hello", "capabilities": ["nac2"]}))
                bands = {b["id"]: b for b in message["bands"]}
                band = arguments.band if arguments.band in bands else message["bands"][0]["id"]
                client.send_text(json.dumps({
                    "type": "tune", "band": band,
                    "freq": bands[band]["center"] + arguments.offset,
                    "mode": arguments.mode,
                }))
                # No waterfall: it would share the link with the audio and add
                # queueing delay to the thing being measured.
                client.send_text(json.dumps({"type": "viewport", "enabled": False}))
            continue

        if opcode != 0x2 or not payload or payload[0] != STREAM_AUDIO:
            continue

        samples = decoder.decode(payload[4:], compact=bool(payload[1] & 2))
        if not samples:
            continue

        peak = max(abs(s) for s in samples)
        recent.append(peak)
        if len(recent) > 24:
            recent.pop(0)
        if len(recent) < 8:
            continue
        floor = sorted(recent)[len(recent) // 4]      # a quartile, not a mean:
        threshold = max(floor * 4.0, 1e-4)            # a burst must not raise it

        if peak < floor * 2.0:
            armed = True
            continue
        if not armed or peak < threshold:
            continue
        armed = False

        # Onset alone would flatter the result: the first sample of a rising
        # edge appears well before the chain's group delay has elapsed. Time
        # the burst's energy centroid instead, which is what "how far behind
        # is this audio" actually means. Collect the burst first.
        collecting = [(arrival, samples)]
        wanted_frames = int(arguments.burst * audio_rate / len(samples)) + 3
        while len(collecting) < wanted_frames and time.time() < deadline:
            client.sock.settimeout(max(0.1, deadline - time.time()))
            try:
                opcode, payload = client.recv()
            except (socket.timeout, TimeoutError, ConnectionError):
                break
            if opcode != 0x2 or not payload or payload[0] != STREAM_AUDIO:
                continue
            more = decoder.decode(payload[4:], compact=bool(payload[1] & 2))
            if more:
                collecting.append((time.time(), more))

        weight_sum = 0.0
        moment = 0.0
        for frame_arrival, frame in collecting:
            for index, value in enumerate(frame):
                energy = value * value
                if energy <= threshold * threshold:
                    continue
                # Each frame is sent as soon as its last sample exists.
                when = frame_arrival - (len(frame) - index) / audio_rate
                weight_sum += energy
                moment += energy * when
        if weight_sum <= 0.0:
            continue
        centroid = moment / weight_sum

        with writer.lock:
            candidates = list(writer.bursts)
        if not candidates:
            continue
        # The burst is symmetric, so its emitted centroid is its midpoint.
        emitted = candidates[-1][1] + arguments.burst * 0.5
        latency = centroid - emitted
        # A burst whose emission has not been recorded yet, or an onset far too
        # old to belong to the newest burst, is not a measurement.
        if not (0.0 < latency < arguments.period * 0.5):
            continue

        latencies.append(latency)
        matched += 1
        print(f"  burst {matched:2d}   {latency * 1000:7.1f} ms")
        recent.clear()

    writer.stop_flag.set()
    if writer.error:
        print(f"IQ writer failed: {writer.error}", file=sys.stderr)

    if not latencies:
        print("no bursts were timed; is the server reading the FIFO, and is the "
              "band named correctly?", file=sys.stderr)
        return 1

    print()
    print(f"bursts timed      {len(latencies)}")
    print(f"minimum           {min(latencies) * 1000:.1f} ms")
    print(f"median            {percentile(latencies, 0.5) * 1000:.1f} ms")
    print(f"95th percentile   {percentile(latencies, 0.95) * 1000:.1f} ms")
    print(f"maximum           {max(latencies) * 1000:.1f} ms")
    print()
    print("Input to decoded audio, excluding the browser's output buffer.")
    print(f"Sampling granularity is one {1024 / arguments.rate * 1000:.1f} ms write block,")
    print("and the bias from it is pessimistic: the true figure is no larger.")
    print("Add the client's buffer target (40-90 ms typical) for what a")
    print("listener hears.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
