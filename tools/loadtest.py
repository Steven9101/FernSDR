#!/usr/bin/env python3
"""Put many concurrent listeners on a running receiver and measure what breaks.

The capacity figure in docs/PERFORMANCE.md comes from a benchmark loop: the DSP
for N listeners, timed in one process, with no sockets involved. That number is
honest about what it measures and silent about everything else - the epoll loop,
the per-connection buffers, the kernel's send queues, and what happens when two
hundred clients all want a waterfall line at the same instant.

This does the other half. It opens real TCP connections, completes real
WebSocket handshakes, tunes each client somewhere different, and then reads
frames for as long as you ask, watching for the three things that actually go
wrong at scale:

    - sequence gaps in the audio stream, which are dropped frames
    - the server's CPU, sampled from /proc, which is the capacity claim
    - the server's memory, which is where a per-connection leak would show

It decodes nothing. Two hundred NAC decoders in Python would measure Python.
Frames are counted and their headers parsed, which is enough to see loss.

    tools/loadtest.py --clients 200 --seconds 60
    tools/loadtest.py --clients 200 --seconds 60 --no-waterfall   # audio only
"""

import argparse
import base64
import hashlib
import json
import os
import random
import selectors
import socket
import struct
import sys
import time
import statistics

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"
STREAM_AUDIO = 0x01
STREAM_WATERFALL = 0x02


class Client:
    """One listener: a socket, a frame reader, and its own counters."""

    def __init__(self, index, host, port, path):
        self.index = index
        self.host = host
        self.port = port
        self.path = path
        self.sock = socket.create_connection((host, port), timeout=15.0)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buffer = b""
        self.handshaken = False
        self.tuned = False
        self.closed = False

        self.audio_frames = 0
        self.audio_bytes = 0
        self.audio_seconds = 0.0
        self.audio_rate = 0
        self.received_bytes = 0
        self.waterfall_lines = 0
        self.waterfall_bytes = 0
        self.lost = 0
        self.discontinuities = 0
        self.last_sequence = None
        self.last_frames = 1
        self.first_frame_at = None

        key = base64.b64encode(os.urandom(16)).decode()
        self.expected_accept = base64.b64encode(
            hashlib.sha1((key + WS_GUID).encode()).digest()
        ).decode()
        request = (
            f"GET {path} HTTP/1.1\r\n"
            f"Host: {host}:{port}\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            f"Sec-WebSocket-Key: {key}\r\n"
            "Sec-WebSocket-Version: 13\r\n"
            "\r\n"
        )
        self.sock.sendall(request.encode())
        self.sock.setblocking(False)

    def fileno(self):
        return self.sock.fileno()

    def send_text(self, text):
        payload = text.encode()
        mask = os.urandom(4)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        header = bytearray([0x81])
        if len(payload) < 126:
            header.append(0x80 | len(payload))
        elif len(payload) < 65536:
            header.append(0x80 | 126)
            header += struct.pack(">H", len(payload))
        else:
            header.append(0x80 | 127)
            header += struct.pack(">Q", len(payload))
        try:
            self.sock.sendall(bytes(header) + mask + masked)
        except OSError:
            self.closed = True

    def read(self, options, bands):
        """Drains the socket and handles whatever came out of it."""
        try:
            chunk = self.sock.recv(65536)
        except BlockingIOError:
            return
        except OSError:
            self.closed = True
            return
        if not chunk:
            self.closed = True
            return
        self.buffer += chunk
        self.received_bytes += len(chunk)

        if not self.handshaken:
            if b"\r\n\r\n" not in self.buffer:
                return
            head, self.buffer = self.buffer.split(b"\r\n\r\n", 1)
            lines = head.decode("latin-1").split("\r\n")
            headers = {}
            for line in lines[1:]:
                if ":" in line:
                    name, value = line.split(":", 1)
                    headers[name.lower()] = value.strip()
            if (len(lines[0].split()) < 2 or lines[0].split()[1] != "101" or
                    headers.get("sec-websocket-accept", "") != self.expected_accept):
                self.closed = True
                return
            self.handshaken = True

        while True:
            frame = self._take_frame()
            if frame is None:
                return
            opcode, payload = frame
            if opcode == 0x8:
                self.closed = True
                return
            if opcode == 0x9:  # ping; the server drops silent connections
                self._pong(payload)
            elif opcode == 0x1:
                self._on_text(payload, options, bands)
            elif opcode == 0x2:
                self._on_binary(payload)

    def _pong(self, payload):
        header = bytearray([0x8A])
        mask = os.urandom(4)
        masked = bytes(b ^ mask[i % 4] for i, b in enumerate(payload))
        header.append(0x80 | len(payload))
        try:
            self.sock.sendall(bytes(header) + mask + masked)
        except OSError:
            self.closed = True

    def _take_frame(self):
        data = self.buffer
        if len(data) < 2:
            return None
        first, second = data[0], data[1]
        length = second & 0x7F
        offset = 2
        if length == 126:
            if len(data) < 4:
                return None
            length = struct.unpack_from(">H", data, 2)[0]
            offset = 4
        elif length == 127:
            if len(data) < 10:
                return None
            length = struct.unpack_from(">Q", data, 2)[0]
            offset = 10
        if second & 0x80:  # server frames are never masked, but be safe
            offset += 4
        if len(data) < offset + length:
            return None
        payload = data[offset:offset + length]
        self.buffer = data[offset + length:]
        return first & 0x0F, payload

    def _on_text(self, payload, options, bands):
        try:
            message = json.loads(payload.decode())
        except ValueError:
            return
        if message.get("type") == "audio-config":
            self.audio_rate = message.get("rate", 0)
            return
        if message.get("type") != "welcome" or self.tuned:
            return
        self.tuned = True
        capabilities = []
        if options.meter_format == "binary" and "meter-v1" in message.get("capabilities", []):
            capabilities.append("meter-v1")
        if options.audio_codec in ("nac2", "nac3") and "nac2" in message.get("capabilities", []):
            capabilities.append("nac2")
        if options.audio_codec == "nac3" and "nac3" in message.get("capabilities", []):
            capabilities.append("nac3")
        if "audio-discontinuity" in message.get("capabilities", []):
            capabilities.append("audio-discontinuity")
        if capabilities:
            self.send_text(json.dumps({"type": "hello", "capabilities": capabilities}))
        if options.audio_codec == "nac3":
            self.send_text(json.dumps({"type": "audio", "enabled": True, "bitrate": options.audio_bitrate,
                                       "frames": options.audio_frames, "noise_margin": options.noise_margin}))
        bands.extend(b for b in message["bands"] if b not in bands)
        # Spread the clients out: every listener on the same frequency would
        # share a channel's worth of cache and flatter the result.
        #
        # --same does the opposite on purpose. Everyone arriving from one
        # shared link is byte-identical, which is the pile-up a busy receiver
        # actually hits, and it is the case any work on sharing the per
        # listener chain has to be measured against.
        band = message["bands"][0 if options.same else self.index % len(message["bands"])]
        span = band["high"] - band["low"]
        frequency = (band["low"] + span * 0.5 if options.same
                     else band["low"] + span * (0.15 + 0.7 * random.random()))
        self.send_text(json.dumps({
            "type": "tune", "band": band["id"], "freq": frequency,
            "mode": options.modes[0 if options.same else self.index % len(options.modes)],
        }))
        if options.waterfall:
            self.send_text(json.dumps({
                "type": "viewport", "enabled": True, "low": band["low"], "high": band["high"],
                "width": options.width, "fps": options.fps, "codec": options.codec, "step_db": options.waterfall_step,
            }))
        else:
            self.send_text(json.dumps({"type": "viewport", "enabled": False}))

    def _on_binary(self, payload):
        if not payload:
            return
        if self.first_frame_at is None:
            self.first_frame_at = time.time()
        if payload[0] == STREAM_AUDIO:
            # A NAC3 packet (flag bit 3) carries one to four frames, counted in
            # the top two bits of its first payload byte; the sequence counts
            # frames, so the next packet follows by that many.
            frames = (payload[4] >> 6) + 1 if payload[1] & 8 and len(payload) > 4 else 1
            self.audio_frames += frames
            self.discontinuities += bool(payload[1] & 4)
            self.audio_bytes += len(payload)
            if self.audio_rate > 0:
                self.audio_seconds += 128 * frames / self.audio_rate
            sequence = struct.unpack_from("<H", payload, 2)[0]
            if self.last_sequence is not None:
                gap = (sequence - self.last_sequence - self.last_frames) & 0xFFFF
                if 0 < gap < 1000:
                    self.lost += gap
            self.last_sequence = sequence
            self.last_frames = frames
        elif payload[0] == STREAM_WATERFALL:
            self.waterfall_lines += 1
            self.waterfall_bytes += len(payload)


def find_server(port):
    """The server's pid, so its CPU and memory can be sampled.

    Match the listening socket as well as the executable. Multiple receivers
    can run on this host, and a rebuild leaves running executables marked
    "(deleted)". Picking the first process measured an idle demo instead.
    """
    inodes = set()
    for table in ("tcp", "tcp6"):
        try:
            with open(f"/proc/net/{table}") as handle:
                for line in list(handle)[1:]:
                    fields = line.split()
                    if fields[3] == "0A" and int(fields[1].rsplit(":", 1)[1], 16) == port:
                        inodes.add(f"socket:[{fields[9]}]")
        except OSError:
            continue
    matches = []
    for entry in os.listdir("/proc"):
        if not entry.isdigit() or int(entry) == os.getpid():
            continue
        try:
            if os.path.basename(os.readlink(f"/proc/{entry}/exe")).removesuffix(" (deleted)") != "fernsdr":
                continue
            if any(os.readlink(f"/proc/{entry}/fd/{fd}") in inodes for fd in os.listdir(f"/proc/{entry}/fd")):
                matches.append(int(entry))
        except OSError:
            continue
    return matches[0] if len(matches) == 1 else None


def cpu_seconds(pid):
    try:
        with open(f"/proc/{pid}/stat") as handle:
            fields = handle.read().split()
        ticks = int(fields[13]) + int(fields[14])
        return ticks / os.sysconf("SC_CLK_TCK")
    except (OSError, IndexError, ValueError):
        return None


def rss_kb(pid):
    try:
        with open(f"/proc/{pid}/status") as handle:
            for line in handle:
                if line.startswith("VmRSS:"):
                    return int(line.split()[1])
    except OSError:
        pass
    return None


def main():
    parser = argparse.ArgumentParser(description="Concurrent-listener load test")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8073)
    parser.add_argument("--path", default="/ws")
    parser.add_argument("--pid", type=int, help="explicit local server PID for CPU/RSS measurement")
    parser.add_argument("--clients", type=int, default=200)
    parser.add_argument("--seconds", type=float, default=60.0)
    parser.add_argument("--width", type=int, default=1024)
    parser.add_argument("--fps", type=float, default=10.0)
    parser.add_argument("--codec", choices=["wfc1", "wfc2", "wfc3", "wfc4"], default="wfc4")
    parser.add_argument("--waterfall-step", type=int, choices=[1, 2], default=2)
    parser.add_argument("--audio-codec", choices=["nac", "nac2", "nac3"], default="nac3")
    parser.add_argument("--audio-bitrate", type=int, default=48000, help="NAC3 ceiling, bit/s")
    parser.add_argument("--audio-frames", type=int, choices=[1, 2, 3, 4], default=2, help="NAC3 frames per packet")
    parser.add_argument("--noise-margin", type=float, default=12.0, help="NAC3 codec noise below the channel noise, dB")
    parser.add_argument("--meter-format", choices=["json", "binary"], default="binary")
    parser.add_argument("--modes", default="usb", help="comma-separated receive modes, distributed across clients")
    parser.add_argument("--no-waterfall", dest="waterfall", action="store_false")
    parser.add_argument("--same", action="store_true",
                        help="put every client on the same band, frequency and mode: the "
                             "pile-up case, and the one that sharing work between listeners "
                             "would have to be measured against")
    parser.add_argument("--ramp", type=float, default=5.0,
                        help="seconds over which to connect, so the server is not "
                             "measured handling a thundering herd")
    options = parser.parse_args()
    options.modes = options.modes.split(",")
    if any(mode not in {"usb", "lsb", "cw", "cwl", "am", "sam", "nfm", "dsb"} for mode in options.modes):
        parser.error("--modes must contain usb, lsb, cw, cwl, am, sam, nfm or dsb")

    random.seed(1)
    pid = options.pid or (find_server(options.port) if options.host in ("127.0.0.1", "localhost", "::1") else None)
    if pid is None:
        print("warning: no fernsdr process found; CPU will not be reported", file=sys.stderr)
    else:
        print(f"measuring server PID {pid}")

    selector = selectors.DefaultSelector()
    clients = []
    bands = []

    print(f"connecting {options.clients} clients over {options.ramp:.0f}s ...")
    started = time.time()
    for index in range(options.clients):
        try:
            client = Client(index, options.host, options.port, options.path)
        except OSError as error:
            print(f"client {index} could not connect: {error}", file=sys.stderr)
            break
        clients.append(client)
        selector.register(client.sock, selectors.EVENT_READ, client)
        # Spread the connections out, and keep reading while doing it, or the
        # first clients' receive buffers fill before the last one is up.
        deadline = started + options.ramp * (index + 1) / options.clients
        while time.time() < deadline:
            for key, _ in selector.select(timeout=0.02):
                key.data.read(options, bands)

    ready_deadline = time.monotonic() + 5
    while time.monotonic() < ready_deadline and any(not c.closed and (not c.tuned or c.audio_frames < 4) for c in clients):
        for key, _ in selector.select(timeout=0.02):
            key.data.read(options, bands)
    connected = sum(1 for c in clients if c.handshaken and c.tuned and not c.closed)
    print(f"{connected} connected; measuring for {options.seconds:.0f}s")

    # Everything before this point is setup. The measurement window starts with
    # the counters and the CPU sample zeroed together.
    for client in clients:
        client.audio_frames = client.waterfall_lines = 0
        client.audio_bytes = client.waterfall_bytes = 0
        client.audio_seconds = 0.0
        client.received_bytes = 0
        client.lost = 0
        client.discontinuities = 0

    cpu_before = cpu_seconds(pid) if pid else None
    rss_before = rss_kb(pid) if pid else None
    window_started = time.time()
    deadline = window_started + options.seconds

    while time.time() < deadline:
        for key, _ in selector.select(timeout=0.2):
            key.data.read(options, bands)

    elapsed = time.time() - window_started
    cpu_after = cpu_seconds(pid) if pid else None
    rss_after = rss_kb(pid) if pid else None

    alive = [c for c in clients if not c.closed]
    audio_frames = sum(c.audio_frames for c in alive)
    audio_bytes = sum(c.audio_bytes for c in alive)
    waterfall_lines = sum(c.waterfall_lines for c in alive)
    waterfall_bytes = sum(c.waterfall_bytes for c in alive)
    lost = sum(c.lost for c in alive)
    starved = sum(1 for c in alive if c.audio_frames == 0)
    realtime = [c.audio_seconds / elapsed for c in alive]
    behind = sum(ratio < 0.97 for ratio in realtime)

    total_bits = (audio_bytes + waterfall_bytes) * 8
    print()
    print(f"clients connected   {connected}")
    print(f"still connected     {len(alive)}")
    if starved:
        print(f"receiving nothing   {starved}   <-- these are the failures")
    print(f"window              {elapsed:.1f} s")
    print()
    print(f"audio frames        {audio_frames}")
    print(f"lost frames         {lost}"
          + (f"  ({100.0 * lost / max(audio_frames + lost, 1):.3f}%)" if audio_frames else ""))
    print(f"discontinuities     {sum(c.discontinuities for c in alive)}")
    print(f"waterfall lines     {waterfall_lines}")
    if realtime:
        print(f"audio realtime      min {min(realtime):.3f}x, median {statistics.median(realtime):.3f}x")
        if behind:
            print(f"below realtime      {behind} clients below 97% of the announced sample rate")
    print(f"media payload       {total_bits / elapsed / 1e6:.2f} Mbit/s")
    print(f"TCP payload         {sum(c.received_bytes for c in clients) * 8 / elapsed / 1e6:.2f} Mbit/s (including control and WebSocket framing)")
    if alive:
        print(f"per listener        {total_bits / elapsed / len(alive) / 1000:.1f} kbit/s")
        print(f"audio per listener  {audio_bytes * 8 / elapsed / len(alive) / 1000:.1f} kbit/s")
    if cpu_before is not None and cpu_after is not None:
        used = cpu_after - cpu_before
        print()
        if used <= 0:
            print("server CPU          below the sampling resolution; run for longer")
        else:
            print(f"server CPU          {100.0 * used / elapsed:.1f}% of one core")
            if alive:
                print(f"per listener        {100.0 * used / elapsed / len(alive):.4f}% of one core")
    if rss_before is not None and rss_after is not None:
        print(f"server memory       {rss_after / 1024:.1f} MB "
              f"({(rss_after - rss_before) / 1024:+.1f} MB over the window)")
        if alive:
            print(f"per listener        {rss_after / len(alive):.0f} kB")

    return 0 if len(alive) == options.clients and starved == 0 and lost == 0 and behind == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
