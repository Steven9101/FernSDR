#!/usr/bin/env python3
"""Connect to a FernSDR server, decode its streams, and report what arrived.

This is the tool to reach for when a deployment misbehaves and you want to
know whether the problem is the server, the network, or the browser.  It
speaks the protocol with nothing but the Python standard library: no
websockets package, no numpy, no browser.

It is also a deliberately independent implementation of both codecs.  If this
agrees with the C++ encoder, the bitstream in docs/CODEC.md is right; if it
does not, one of the three is wrong and that is worth knowing.

    tools/fernsdr-probe.py --host 127.0.0.1 --port 8073 --freq 7099200 --mode usb
    tools/fernsdr-probe.py --wav out.wav --seconds 10
"""

import argparse
import base64
import hashlib
import json
import math
import os
import socket
import struct
import sys
import time
import wave

# --------------------------------------------------------------------------
# Minimal WebSocket client
# --------------------------------------------------------------------------

WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11"


class WebSocket:
    def __init__(self, host, port, path="/ws", timeout=10.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        self.buffer = b""
        self._handshake(host, port, path)

    def _handshake(self, host, port, path):
        key = base64.b64encode(os.urandom(16)).decode()
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

        while b"\r\n\r\n" not in self.buffer:
            chunk = self.sock.recv(4096)
            if not chunk:
                raise ConnectionError("server closed during the handshake")
            self.buffer += chunk

        head, self.buffer = self.buffer.split(b"\r\n\r\n", 1)
        lines = head.decode("latin-1").split("\r\n")
        if "101" not in lines[0]:
            raise ConnectionError(f"handshake refused: {lines[0]}")

        expected = base64.b64encode(hashlib.sha1((key + WS_GUID).encode()).digest()).decode()
        for line in lines[1:]:
            if line.lower().startswith("sec-websocket-accept:"):
                if line.split(":", 1)[1].strip() != expected:
                    raise ConnectionError("Sec-WebSocket-Accept did not match")
                break
        else:
            raise ConnectionError("no Sec-WebSocket-Accept header")

    def send_text(self, text):
        self._send(0x1, text.encode())

    def _send(self, opcode, payload):
        header = bytearray([0x80 | opcode])
        length = len(payload)
        if length < 126:
            header.append(0x80 | length)
        elif length <= 0xFFFF:
            header.append(0x80 | 126)
            header += struct.pack(">H", length)
        else:
            header.append(0x80 | 127)
            header += struct.pack(">Q", length)
        mask = os.urandom(4)
        header += mask
        masked = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
        self.sock.sendall(bytes(header) + masked)

    def recv(self):
        """Returns (opcode, payload), reassembling fragments."""
        fragments = b""
        fragment_opcode = None
        while True:
            frame_opcode, fin, payload = self._recv_frame()
            if frame_opcode >= 0x8:
                if frame_opcode == 0x9:  # ping
                    self._send(0xA, payload)
                    continue
                if frame_opcode == 0x8:  # close
                    return 0x8, payload
                continue
            if frame_opcode != 0x0:
                fragment_opcode = frame_opcode
                fragments = payload
            else:
                fragments += payload
            if fin:
                return fragment_opcode, fragments

    def _recv_frame(self):
        head = self._read(2)
        fin = bool(head[0] & 0x80)
        opcode = head[0] & 0x0F
        masked = bool(head[1] & 0x80)
        length = head[1] & 0x7F
        if length == 126:
            length = struct.unpack(">H", self._read(2))[0]
        elif length == 127:
            length = struct.unpack(">Q", self._read(8))[0]
        mask = self._read(4) if masked else None
        payload = self._read(length)
        if mask:
            payload = bytes(b ^ mask[i & 3] for i, b in enumerate(payload))
        return opcode, fin, payload

    def _read(self, count):
        while len(self.buffer) < count:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("server closed the connection")
            self.buffer += chunk
        out, self.buffer = self.buffer[:count], self.buffer[count:]
        return out

    def close(self):
        try:
            self._send(0x8, struct.pack(">H", 1000))
        except OSError:
            pass
        self.sock.close()


# --------------------------------------------------------------------------
# Bit reader, matching server/src/util/bitio.h
# --------------------------------------------------------------------------

RICE_ESCAPE = 24


class BitReader:
    def __init__(self, data):
        self.data = data
        self.pos = 0
        self.overrun = False

    def bit(self):
        if self.pos >= len(self.data) * 8:
            self.overrun = True
            return 0
        value = (self.data[self.pos >> 3] >> (7 - (self.pos & 7))) & 1
        self.pos += 1
        return value

    def bits(self, count):
        value = 0
        for _ in range(count):
            value = (value << 1) | self.bit()
        return value

    def rice(self, k):
        q = 0
        while self.bit():
            q += 1
            if q >= RICE_ESCAPE:
                self.bit()  # terminating zero
                return self.bits(32)
            if self.overrun:
                return 0
        return (q << k) | (self.bits(k) if k else 0)

    def signed_rice(self, k):
        return zigzag_decode(self.rice(k))

    def exp_golomb(self):
        leading = 0
        while not self.bit():
            leading += 1
            if leading > 32 or self.overrun:
                self.overrun = True
                return 0
        value = 1
        for _ in range(leading):
            value = (value << 1) | self.bit()
        return value - 1

    def signed_exp_golomb(self):
        return zigzag_decode(self.exp_golomb())


def zigzag_decode(u):
    return (u >> 1) ^ -(u & 1)


# --------------------------------------------------------------------------
# NAC audio decoder, matching server/src/codec/
# --------------------------------------------------------------------------

FRAME_HOP = 128
BAND_WIDTHS = [4, 4, 4, 4, 4, 4, 4, 4, 8, 8, 8, 8, 8, 8, 16, 16, 16]
BAND_STARTS = []
_total = 0
for _width in BAND_WIDTHS:
    BAND_STARTS.append(_total)
    _total += _width
assert _total == FRAME_HOP, "band table must cover every coefficient"

QUALITY_BITS = 6
EXPONENT_REFERENCE = -40


def rice_k_for_quality(quality_index):
    return max(0, min(20, quality_index // 4 - 1))


def _sine_window(length):
    return [math.sin(math.pi / length * (n + 0.5)) for n in range(length)]


class NacDecoder:
    """Straightforward O(N^2) IMDCT: clarity over speed, this is a probe."""

    def __init__(self):
        self.window = _sine_window(2 * FRAME_HOP)
        self.overlap = [0.0] * FRAME_HOP
        m = FRAME_HOP
        self.cosine = [
            [math.cos(math.pi / m * (n + 0.5 + m / 2.0) * (k + 0.5)) for k in range(m)]
            for n in range(2 * m)
        ]

    def decode(self, payload, compact=False):
        reader = BitReader(payload)
        quality = reader.bits(QUALITY_BITS)
        mask = reader.bits(2) if compact else 0
        if mask == 0:
            active = [reader.bit() for _ in BAND_WIDTHS]
        elif mask == 1:
            active = [1] * len(BAND_WIDTHS)
        else:
            first = reader.bits(5) if mask == 3 else 0
            count = reader.bits(5)
            if first >= len(BAND_WIDTHS) or count > len(BAND_WIDTHS) - first or (mask == 3 and count == 0):
                return None
            active = [int(first <= b < first + count) for b in range(len(BAND_WIDTHS))]

        exponents = [0] * len(BAND_WIDTHS)
        previous = EXPONENT_REFERENCE
        first = True
        scale_mode = 0
        for b in range(len(BAND_WIDTHS)):
            if not active[b]:
                continue
            if compact and first:
                previous = reader.bits(9) - 200
                scale_mode = reader.bits(3)
            else:
                previous += reader.signed_rice(scale_mode - 1) if scale_mode else reader.signed_exp_golomb()
            if scale_mode > 5 or previous < -200 or previous > 200 or reader.overrun:
                return None
            first = False
            exponents[b] = previous

        k = rice_k_for_quality(quality)
        coefficients = [0.0] * FRAME_HOP
        for b in range(len(BAND_WIDTHS)):
            if not active[b]:
                continue
            step = 2.0 ** ((exponents[b] - quality) * 0.25)
            start = BAND_STARTS[b]
            for i in range(BAND_WIDTHS[b]):
                coefficients[start + i] = reader.signed_rice(k) * step

        if reader.overrun:
            return None
        return self._synthesise(coefficients)

    def decode_packet(self, payload):
        """A NAC3 packet: one to four frames, each a list of FRAME_HOP samples.

        Returns None when the frame count cannot be read or any frame is
        malformed; a probe reports that rather than concealing it.
        """
        if not payload:
            return None
        reader = BitReader(payload)
        frames = reader.bits(2) + 1
        references = {"step": [0] * len(BAND_WIDTHS), "rice": [0] * len(BAND_WIDTHS),
                      "active": [0] * len(BAND_WIDTHS)}
        out = []
        for index in range(frames):
            coefficients = self._per_band_frame(reader, index > 0, references)
            if coefficients is None:
                return None
            out.append(self._synthesise(coefficients))
        return out

    def _per_band_frame(self, reader, predicted, references):
        bands = len(BAND_WIDTHS)
        if predicted and reader.bit() == 1:
            active = list(references["active"])
        else:
            mask = reader.bits(2)
            if mask == 0:
                active = [reader.bit() for _ in range(bands)]
            elif mask == 1:
                active = [1] * bands
            else:
                first = reader.bits(5) if mask == 3 else 0
                count = reader.bits(5)
                if first >= bands or count > bands - first or (mask == 3 and count == 0):
                    return None
                active = [int(first <= b < first + count) for b in range(bands)]
        steps = [0] * bands
        rice = [0] * bands
        if any(active):
            selector = reader.bits(3) if predicted else 0
            first = True
            step = 0
            for b in range(bands):
                if not active[b]:
                    continue
                if predicted:
                    if selector > 5:
                        return None
                    residual = reader.signed_rice(selector - 1) if selector else reader.signed_exp_golomb()
                    step = references["step"][b] + residual
                elif first:
                    step = reader.bits(9) - 200
                    selector = reader.bits(3)
                    if selector > 5:
                        return None
                else:
                    step += reader.signed_rice(selector - 1) if selector else reader.signed_exp_golomb()
                first = False
                if step < -200 or step > 200 or reader.overrun:
                    return None
                steps[b] = step
            first = True
            parameter = 0
            for b in range(bands):
                if not active[b]:
                    continue
                if predicted:
                    parameter = references["rice"][b] + reader.signed_rice(0)
                else:
                    parameter = reader.bits(4) if first else parameter + reader.signed_rice(0)
                first = False
                if parameter < 0 or parameter > 15 or reader.overrun:
                    return None
                rice[b] = parameter
        coefficients = [0.0] * FRAME_HOP
        for b in range(bands):
            if not active[b]:
                continue
            size = 2.0 ** (0.25 * steps[b])
            start = BAND_STARTS[b]
            for i in range(BAND_WIDTHS[b]):
                value = reader.signed_rice(rice[b]) * size
                if abs(value) > 1e7:
                    return None
                coefficients[start + i] = value
        if reader.overrun:
            return None
        for b in range(bands):
            if active[b]:
                references["step"][b] = steps[b]
                references["rice"][b] = rice[b]
            references["active"][b] = active[b]
        if not predicted:
            # Bands the first frame left silent predict from the nearest
            # active band: the last to their left, else the first active one.
            active_bands = [b for b in range(bands) if active[b]]
            for b in range(bands):
                if active[b]:
                    continue
                left = [a for a in active_bands if a < b]
                source = left[-1] if left else (active_bands[0] if active_bands else None)
                references["step"][b] = references["step"][source] if source is not None else 0
                references["rice"][b] = references["rice"][source] if source is not None else 0
        return coefficients

    def _synthesise(self, coefficients):
        scale = 2.0 / FRAME_HOP
        out = [0.0] * FRAME_HOP
        for n in range(2 * FRAME_HOP):
            row = self.cosine[n]
            acc = 0.0
            for k_index in range(FRAME_HOP):
                c = coefficients[k_index]
                if c:
                    acc += c * row[k_index]
            value = acc * scale * self.window[n]
            if n < FRAME_HOP:
                out[n] = self.overlap[n] + value
            else:
                self.overlap[n - FRAME_HOP] = value
        return out


# --------------------------------------------------------------------------
# Waterfall line decoder
# --------------------------------------------------------------------------

DB_STEP = 1.0
INTRA_SEED_Q = -100


class WaterfallDecoder:
    def __init__(self):
        self.previous = None
        self.step_db = 1

    def decode(self, payload, width, zero_runs=False, adaptive=False, step_db=1):
        if not payload or not 1 <= width <= 4096 or step_db not in (1, 2):
            self.previous = None
            return None
        reader = BitReader(payload)
        mode = reader.bits(2 if adaptive else 1)
        k = reader.bits(4)

        if mode in (0, 2) and (self.previous is None or len(self.previous) != width or self.step_db != step_db):
            self.previous = None
            return None

        low, high, seed = -200 // step_db, 100 // step_db, INTRA_SEED_Q // step_db
        line = [0] * width
        i = 0
        while i < width:
            run = zero_runs and reader.bit() == 0
            count = reader.exp_golomb() + 1 if run else 1
            if count > width - i or reader.overrun:
                self.previous = None
                return None
            residual = 0 if run else reader.signed_rice(k)
            for _ in range(count):
                left = line[i - 1] if i else seed
                if mode == 1:
                    predictor = left
                elif mode == 3:
                    predictor = max(low, min(high, 2 * left - line[i - 2])) if i > 1 else left
                elif mode == 0 or i == 0:
                    predictor = self.previous[i]
                else:
                    above, diagonal = self.previous[i], self.previous[i - 1]
                    predictor = (min(left, above) if diagonal >= max(left, above) else
                                 max(left, above) if diagonal <= min(left, above) else left + above - diagonal)
                value = predictor + residual
                if not low <= value <= high or reader.overrun:
                    self.previous = None
                    return None
                line[i] = value
                i += 1

        if reader.overrun:
            return None
        self.previous = line
        self.step_db = step_db
        return [v * step_db for v in line]


# --------------------------------------------------------------------------
# Probe
# --------------------------------------------------------------------------

STREAM_AUDIO = 0x01
STREAM_WATERFALL = 0x02


def dominant_tone(samples, rate):
    """Coarse peak-picking correlation, good enough to name the pitch."""
    if len(samples) < 512:
        return 0.0, 0.0
    best_amplitude, best_frequency = 0.0, 0.0
    count = min(len(samples), 8192)
    window = samples[:count]
    frequency = 100.0
    while frequency < rate / 2:
        re = im = 0.0
        for i, value in enumerate(window):
            angle = 2.0 * math.pi * frequency * i / rate
            re += value * math.cos(angle)
            im += value * math.sin(angle)
        amplitude = 2.0 * math.hypot(re, im) / count
        if amplitude > best_amplitude:
            best_amplitude, best_frequency = amplitude, frequency
        frequency += 25.0
    return best_frequency, best_amplitude


def main():
    parser = argparse.ArgumentParser(description="FernSDR protocol probe")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8073)
    parser.add_argument("--path", default="/ws")
    parser.add_argument("--band", default=None)
    parser.add_argument("--freq", type=float, default=None, help="tuning frequency in Hz")
    parser.add_argument("--mode", default="usb")
    parser.add_argument("--seconds", type=float, default=5.0)
    parser.add_argument("--width", type=int, default=256, help="waterfall width to request")
    parser.add_argument("--fps", type=float, default=10.0)
    parser.add_argument("--wav", default=None, help="write decoded audio to this WAV file")
    parser.add_argument("--quiet", action="store_true")
    parser.add_argument("--frames", type=int, choices=[1, 2, 3, 4], default=2,
                        help="NAC3 frames per packet to ask for")
    arguments = parser.parse_args()

    socket_client = WebSocket(arguments.host, arguments.port, arguments.path)
    audio_decoder = NacDecoder()
    waterfall_decoder = WaterfallDecoder()

    audio = []
    audio_rate = 12000
    audio_bytes = waterfall_bytes = 0
    audio_frames = waterfall_lines = 0
    lost_frames = 0
    last_sequence = None
    last_frames = 1
    welcome = None
    tuned = False

    deadline = time.time() + arguments.seconds
    while time.time() < deadline:
        socket_client.sock.settimeout(max(0.1, deadline - time.time()))
        try:
            opcode, payload = socket_client.recv()
        except (socket.timeout, TimeoutError):
            break
        except ConnectionError as error:
            print(f"connection lost: {error}", file=sys.stderr)
            break

        if opcode == 0x8:
            print("server closed the connection", file=sys.stderr)
            break

        if opcode == 0x1:
            message = json.loads(payload.decode())
            kind = message.get("type")
            if kind == "welcome":
                welcome = message
                if not arguments.quiet:
                    site = message["site"]
                    print(f"connected to {site['name']}"
                          + (f" ({site['location']})" if site.get("location") else ""))
                    for band in message["bands"]:
                        print(f"  band {band['id']:<10} {band['low']/1e6:9.4f} - "
                              f"{band['high']/1e6:9.4f} MHz  {band['listeners']} listening")
            elif kind == "audio-config":
                audio_rate = int(message["rate"])
                if not arguments.quiet:
                    print(f"audio: {audio_rate} Hz, {message['frame_samples']} samples/frame, "
                          f"target {message['bitrate']} bit/s")
            elif kind == "state" and not arguments.quiet and message.get("note"):
                print(f"note: {message['note']}")
            elif kind == "error":
                print(f"server error: {message['message']}", file=sys.stderr)

            if welcome and not tuned:
                tuned = True
                offered = [c for c in ("nac2", "nac3") if c in welcome.get("capabilities", [])]
                if offered:
                    socket_client.send_text(json.dumps({"type": "hello", "capabilities": offered}))
                if "nac3" in offered:
                    socket_client.send_text(json.dumps({"type": "audio", "enabled": True,
                                                        "frames": arguments.frames}))
                band = arguments.band or welcome["bands"][0]["id"]
                band_info = next(b for b in welcome["bands"] if b["id"] == band)
                frequency = arguments.freq
                if frequency is None:
                    frequency = band_info["center"]
                socket_client.send_text(json.dumps({
                    "type": "tune", "band": band, "freq": frequency, "mode": arguments.mode,
                }))
                socket_client.send_text(json.dumps({
                    "type": "viewport", "enabled": True, "low": band_info["low"],
                    "codec": next((c for c in ("wfc4", "wfc3") if c in welcome.get("capabilities", [])), "wfc2"),
                    "step_db": 2,
                    "high": band_info["high"], "width": arguments.width, "fps": arguments.fps,
                }))
            continue

        if opcode != 0x2 or not payload:
            continue

        if payload[0] == STREAM_AUDIO:
            audio_bytes += len(payload)
            sequence = struct.unpack_from("<H", payload, 2)[0]
            if last_sequence is not None:
                gap = (sequence - last_sequence - last_frames) & 0xFFFF
                if 0 < gap < 1000:
                    lost_frames += gap
            last_sequence = sequence
            if payload[1] & 8:
                frames = audio_decoder.decode_packet(payload[4:])
                last_frames = (payload[4] >> 6) + 1 if len(payload) > 4 else 1
                if frames:
                    for frame in frames:
                        audio.extend(frame)
                    audio_frames += len(frames)
                continue
            last_frames = 1
            decoded = audio_decoder.decode(payload[4:], compact=bool(payload[1] & 2))
            if decoded:
                audio.extend(decoded)
                audio_frames += 1
        elif payload[0] == STREAM_WATERFALL:
            waterfall_bytes += len(payload)
            low, high = struct.unpack_from("<dd", payload, 4)
            width = struct.unpack_from("<H", payload, 20)[0]
            line = waterfall_decoder.decode(payload[22:], width, bool(payload[1] & 1), bool(payload[1] & 2), 2 if payload[1] & 8 else 1)
            if line:
                waterfall_lines += 1
                if waterfall_lines == 1 and not arguments.quiet:
                    peak = max(range(len(line)), key=lambda i: line[i])
                    peak_hz = low + (high - low) * (peak + 0.5) / len(line)
                    print(f"waterfall: {width} bins over {low/1e6:.4f}-{high/1e6:.4f} MHz, "
                          f"strongest signal near {peak_hz/1e6:.4f} MHz at {line[peak]:.0f} dBFS")

    socket_client.close()

    elapsed = arguments.seconds
    print()
    print(f"audio      {audio_frames:5d} frames  {audio_bytes*8/elapsed/1000:7.1f} kbit/s"
          + (f"  ({lost_frames} lost)" if lost_frames else ""))
    print(f"waterfall  {waterfall_lines:5d} lines   {waterfall_bytes*8/elapsed/1000:7.1f} kbit/s")
    print(f"total                      {(audio_bytes+waterfall_bytes)*8/elapsed/1000:7.1f} kbit/s")

    if audio:
        seconds_of_audio = len(audio) / audio_rate
        peak = max(abs(v) for v in audio)
        frequency, amplitude = dominant_tone(audio[audio_rate // 2:], audio_rate)
        print(f"decoded    {seconds_of_audio:.2f} s at {audio_rate} Hz, peak {peak:.3f}")
        print(f"strongest audio tone near {frequency:.0f} Hz, amplitude {amplitude:.3f}")

    if arguments.wav and audio:
        with wave.open(arguments.wav, "wb") as handle:
            handle.setnchannels(1)
            handle.setsampwidth(2)
            handle.setframerate(audio_rate)
            handle.writeframes(b"".join(
                struct.pack("<h", max(-32768, min(32767, int(v * 32767)))) for v in audio))
        print(f"wrote {arguments.wav}")

    return 0 if audio_frames else 1


if __name__ == "__main__":
    sys.exit(main())
