#!/usr/bin/env python3
"""Prove that a receiver reproduces what was transmitted.

Every other measurement in this project scores the receiver against a signal
this project also generated, which is a closed loop: if the same wrong
assumption is in both, the result looks perfect. This one closes the loop with
something outside it - an ordinary recorded WAV file - and asks whether the
audio that comes back out of the WebSocket is still that recording.

    tools/fidelity-check.py build voice.wav --out capture.cs16
    #   ... point a [band:...] at capture.cs16 and start the server ...
    tools/fidelity-check.py check voice.wav --port 8073 --freq 7101500

`build` writes a cs16 IQ capture - the format rtl_sdr and rx888_stream produce -
containing that audio as real single sideband, plus two neighbouring carriers
and a noise floor so the AGC and the codec see a band rather than one clean
tone. `check` connects like a browser, decodes the audio, and cross-correlates
it against the original.

The sideband has to be real single sideband, which means a real Hilbert
transform. Approximating it with a fixed delay gives 90 degrees at exactly one
frequency and nonsense everywhere else; a receiver demodulating USB then
correctly recovers something that does not match the source, and the test
blames the receiver for a broken test vector.

numpy is required for this tool and this tool only. It is not needed to build
or run FernSDR.
"""

import argparse
import importlib.util
import os
import sys
import wave

try:
    import numpy as np
except ImportError:
    sys.exit("this tool needs numpy: pip install numpy")

_spec = importlib.util.spec_from_file_location(
    "fernsdr_probe", os.path.join(os.path.dirname(os.path.abspath(__file__)), "fernsdr-probe.py")
)
_probe = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(_probe)


def read_wav(path):
    with wave.open(path, "rb") as handle:
        channels, width, rate = handle.getnchannels(), handle.getsampwidth(), handle.getframerate()
        raw = handle.readframes(handle.getnframes())
    if width != 2:
        sys.exit(f"{path}: needs to be 16-bit PCM")
    data = np.frombuffer(raw, dtype="<i2").astype(np.float64) / 32768.0
    if channels == 2:
        data = data[0::2]
    return data, rate


def resample(data, source_rate, target_rate):
    count = int(len(data) * target_rate / source_rate)
    index = np.arange(count) * source_rate / target_rate
    base = index.astype(np.int64)
    frac = index - base
    return data[base] * (1 - frac) + data[np.minimum(base + 1, len(data) - 1)] * frac


def band_limit(data, rate, low=300.0, high=2700.0):
    spectrum = np.fft.rfft(data)
    freqs = np.fft.rfftfreq(len(data), 1 / rate)
    spectrum[(freqs < low) | (freqs > high)] = 0
    return np.fft.irfft(spectrum, n=len(data)), spectrum


def build(arguments):
    audio, audio_rate = read_wav(arguments.wav)
    print(f"source: {len(audio)/audio_rate:.2f} s at {audio_rate} Hz", file=sys.stderr)

    total = int(arguments.rate * arguments.seconds)
    t = np.arange(total) / arguments.rate
    index = (t * audio_rate) % len(audio)
    base = index.astype(np.int64)
    frac = index - base
    speech = audio[base] * (1 - frac) + audio[(base + 1) % len(audio)] * frac

    _, spectrum = band_limit(speech, arguments.rate)

    # The analytic signal: positive frequencies only, doubled. Its real part is
    # the audio and its imaginary part the audio's Hilbert transform, which is
    # exactly what single sideband needs.
    analytic = np.fft.irfft(spectrum * 2, n=total).astype(np.complex128)
    analytic += 1j * np.fft.irfft(spectrum * -2j, n=total)
    peak = float(np.abs(analytic).max())
    analytic *= arguments.amplitude / max(peak, 1e-9)

    offset = arguments.freq - arguments.center
    if arguments.mode == "lsb":
        analytic = np.conj(analytic)
    signal = analytic * np.exp(2j * np.pi * offset * t)

    # Neighbours and a noise floor, so nothing is measured on an unrealistically
    # empty band.
    signal += 0.03 * np.exp(2j * np.pi * (offset + 6500.0) * t)
    signal += 0.008 * np.exp(2j * np.pi * (offset - 21500.0) * t)
    rng = np.random.default_rng(5)
    signal += rng.normal(0, arguments.noise, total) + 1j * rng.normal(0, arguments.noise, total)

    interleaved = np.empty(total * 2, dtype="<i2")
    interleaved[0::2] = np.clip(signal.real * 32767, -32768, 32767).astype("<i2")
    interleaved[1::2] = np.clip(signal.imag * 32767, -32768, 32767).astype("<i2")
    interleaved.tofile(arguments.out)

    print(f"wrote {arguments.out}: {total} IQ samples, {arguments.rate/1e6:.3f} Msps cs16")
    print()
    print("Add this to your config, then start the server:")
    print()
    print("[band:fidelity]")
    print("name        = Fidelity check")
    print("source      = file")
    print(f"path        = {os.path.abspath(arguments.out)}")
    print("format      = cs16")
    print("signal      = iq")
    print(f"sample_rate = {int(arguments.rate)}")
    print(f"center      = {int(arguments.center)}")
    print("loop        = true")
    print()
    print(f"Then: {sys.argv[0]} check {arguments.wav} --freq {int(arguments.freq)}")
    return 0


def check(arguments):
    reference, reference_rate = read_wav(arguments.wav)

    client = _probe.WebSocket(arguments.host, arguments.port, arguments.path)
    decoder = _probe.NacDecoder()
    audio_rate = 12000
    tuned = False
    captured = []

    import json, socket, struct, time

    deadline = time.time() + arguments.seconds + 4
    while time.time() < deadline:
        client.sock.settimeout(max(0.1, deadline - time.time()))
        try:
            opcode, payload = client.recv()
        except (socket.timeout, TimeoutError, ConnectionError):
            break
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
                    "type": "tune", "band": band, "freq": arguments.freq, "mode": arguments.mode,
                }))
                client.send_text(json.dumps({"type": "viewport", "enabled": False}))
            continue
        if opcode != 0x2 or not payload or payload[0] != _probe.STREAM_AUDIO:
            continue
        decoded = decoder.decode(payload[4:], compact=bool(payload[1] & 2))
        if decoded:
            captured.extend(decoded)

    client.close()
    if len(captured) < audio_rate:
        sys.exit("not enough audio came back; is the band tuned where the signal is?")

    got = np.asarray(captured, dtype=np.float64)
    got -= got.mean()

    want = resample(reference, reference_rate, audio_rate)
    want, _ = band_limit(want, audio_rate)
    want -= want.mean()

    # Full cross-correlation at single-sample resolution. A coarse lag search
    # misses the peak entirely: 25 samples at 12 kHz is most of a cycle at
    # speech frequencies.
    size = 1 << int(np.ceil(np.log2(len(want) + len(got))))
    correlation = np.fft.irfft(np.fft.rfft(got, size) * np.conj(np.fft.rfft(want, size)), size)
    correlation /= np.sqrt((want ** 2).sum() * (got ** 2).sum())
    peak = int(np.argmax(np.abs(correlation)))
    lag = peak if peak < size // 2 else peak - size

    # The capture loops, so only part of the received audio lines up with one
    # pass of the source. Scored over the whole file even a perfect receiver
    # cannot beat the overlap fraction; the figure that means something is the
    # one over the aligned window.
    start = max(lag, 0)
    window = min(len(want), len(got) - start)
    if window < audio_rate:
        sys.exit("could not align the recording with what came back")
    a, b = want[:window], got[start:start + window]
    value = float((a * b).sum() / np.sqrt((a ** 2).sum() * (b ** 2).sum()))

    print(f"received     {len(got)/audio_rate:.2f} s at {audio_rate} Hz")
    print(f"aligned      {window/audio_rate:.2f} s at lag {lag/audio_rate:+.3f} s")
    print(f"correlation  {value:+.3f}")
    print()
    if value >= arguments.threshold:
        print(f"PASS - the receiver reproduced the recording (threshold {arguments.threshold})")
        return 0
    print(f"FAIL - correlation is below {arguments.threshold}. The audio coming out is not")
    print("       the audio that went in: check the mode, the tuned frequency, and that")
    print("       the band is reading the capture you built.")
    return 1


def main():
    parser = argparse.ArgumentParser(description="FernSDR fidelity check")
    sub = parser.add_subparsers(dest="command", required=True)

    b = sub.add_parser("build", help="make an IQ capture from a WAV")
    b.add_argument("wav")
    b.add_argument("--out", default="capture.cs16")
    b.add_argument("--rate", type=float, default=384000.0)
    b.add_argument("--center", type=float, default=7100000.0)
    b.add_argument("--freq", type=float, default=7101500.0)
    b.add_argument("--mode", default="usb", choices=["usb", "lsb"])
    b.add_argument("--seconds", type=float, default=8.0)
    b.add_argument("--amplitude", type=float, default=0.06)
    b.add_argument("--noise", type=float, default=0.004)
    b.set_defaults(run=build)

    c = sub.add_parser("check", help="compare a receiver's audio with the WAV")
    c.add_argument("wav")
    c.add_argument("--host", default="127.0.0.1")
    c.add_argument("--port", type=int, default=8073)
    c.add_argument("--path", default="/ws")
    c.add_argument("--band", default="fidelity")
    c.add_argument("--freq", type=float, default=7101500.0)
    c.add_argument("--mode", default="usb")
    c.add_argument("--seconds", type=float, default=12.0)
    c.add_argument("--threshold", type=float, default=0.75)
    c.set_defaults(run=check)

    arguments = parser.parse_args()
    return arguments.run(arguments)


if __name__ == "__main__":
    sys.exit(main())
