#!/usr/bin/env python3
"""How many weak FT8 signals survive the audio codec when a strong one sits beside each.

The off-air FT8 fixtures hold 81 messages, which cannot tell a codec that
costs a quarter of a decibel from one that costs nothing. This builds its own
test instead, with known content and enough of it to count:

  - each 15-second file carries ten weak signals, from -21 to -17 dB in the
    usual 2500 Hz WSJT-X bandwidth, where jt9's decode probability is steep;
  - each weak signal has a strong neighbour 60 to 90 Hz away, +5 to +15 dB,
    which puts both in one NAC band: the case a band-relative quantiser loses;
  - white noise sets the SNR, with a new seed for every file.

The signals come from WSJT-X's own ft8sim at a high SNR and are mixed here, so
the noise level is exactly what this script says it is. Then every file goes
through server/build/codec-lab at each rate and variant and is decoded with
jt9. The table is the fraction of weak signals decoded, for the original audio
and for each codec setting.

    tools/codec-lab-ft8-weak.py --files 20 --variants nac2,nac3 --rates 24000,32000,48000

Needs ft8sim and jt9 (WSJT-X) and a built codec-lab (make -C server lab).
"""

import argparse
import math
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import wave

RATE = 12000
SECONDS = 15
HERE = os.path.dirname(os.path.abspath(__file__))
LAB = os.path.join(HERE, "..", "server", "build", "codec-lab")


def read_wav(path):
    with wave.open(path) as handle:
        assert handle.getframerate() == RATE and handle.getsampwidth() == 2 and handle.getnchannels() == 1
        raw = handle.readframes(handle.getnframes())
    return [value / 32768.0 for value in struct.unpack("<%dh" % (len(raw) // 2), raw)]


def write_wav(path, samples):
    peak = max(1e-9, max(abs(value) for value in samples))
    # Leave headroom, and scale the whole file identically so relative levels hold.
    scale = 0.5 / peak
    data = struct.pack("<%dh" % len(samples), *[int(round(value * scale * 32767)) for value in samples])
    with wave.open(path, "wb") as handle:
        handle.setnchannels(1)
        handle.setsampwidth(2)
        handle.setframerate(RATE)
        handle.writeframes(data)


def ft8_signal(work, message, frequency):
    """One clean FT8 transmission, unit RMS while keyed."""
    directory = tempfile.mkdtemp(dir=work)
    # ft8sim adds its own noise. At 40 dB it is negligible here, and unlike
    # 60 dB it does not clip the transmission into a square wave.
    subprocess.run(["ft8sim", message, "%.1f" % frequency, "0.0", "0.0", "0.0", "1", "40"],
                   cwd=directory, check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    files = [name for name in os.listdir(directory) if name.endswith(".wav")]
    samples = read_wav(os.path.join(directory, files[0]))
    shutil.rmtree(directory)
    # The transmission runs from 0.5 s to about 13.1 s; measure where it is
    # certainly keyed.
    keyed = samples[RATE * 1:RATE * 12]
    rms = math.sqrt(sum(value * value for value in keyed) / len(keyed))
    return [value / rms for value in samples]


def decode(work, path):
    directory = tempfile.mkdtemp(dir=work)
    # jt9 takes the UTC slot from the file name; give it a plain one.
    target = os.path.join(directory, "000000.wav")
    shutil.copy(path, target)
    output = subprocess.run(["jt9", "-8", "-p", "15", "-m", "1", "-w", "0", "-d", "3",
                             "-a", directory, "-t", directory, target],
                            capture_output=True, text=True).stdout
    shutil.rmtree(directory)
    messages = set()
    for line in output.splitlines():
        # jt9 marks a weak-confidence decode with a trailing "?" and an a-priori
        # assisted one with "a1".."a7"; the message itself is what counts.
        match = re.match(r"^[0-9*]{6}\s+-?\d+\s+-?[\d.]+\s+\d+\s+~\s+(.*?)\s*(a\d)?\s*\??\s*$", line)
        if match:
            messages.add(match.group(1).strip())
    return messages


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--files", type=int, default=12)
    parser.add_argument("--variants", default="nac2,nac3")
    parser.add_argument("--rates", default="24000,32000,48000")
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--passband", default="200,3000")
    args = parser.parse_args()

    for tool in ("ft8sim", "jt9"):
        if not shutil.which(tool):
            sys.exit(tool + " (WSJT-X) is not installed")
    if not os.access(LAB, os.X_OK):
        sys.exit("build it first: make -C server lab")

    random.seed(args.seed)
    work = tempfile.mkdtemp(prefix="ft8-weak-")
    variants = args.variants.split(",")
    rates = [int(value) for value in args.rates.split(",")]
    totals = {"original": [0, 0]}
    try:
        for index in range(args.files):
            weak = []
            mixed = [0.0] * (RATE * SECONDS)
            noise_rms = 1.0
            # White noise: an SNR in 2500 Hz means the noise power in that
            # bandwidth, and white noise at 12 kHz spreads over 6000 Hz.
            noise_in_band = noise_rms ** 2 * 2500.0 / 6000.0
            rng = random.Random(args.seed * 1000 + index)
            for slot in range(10):
                base = 400 + slot * 250 + rng.uniform(-20, 20)
                call = "K%d%s" % (slot, "".join(rng.choice("ABCDEFGHIJKLMNOPQRSTUVWXYZ") for _ in range(3)))
                weak_message = "CQ %s FN%02d" % (call, rng.randint(10, 99))
                strong_message = "CQ W%d%s EM%02d" % (slot, "".join(rng.choice("ABCDEFGHIJKLMNOPQRSTUVWXYZ")
                                                               for _ in range(3)), rng.randint(10, 99))
                weak_snr = rng.uniform(-21.0, -17.0)
                strong_snr = rng.uniform(5.0, 15.0)
                offset = rng.uniform(60.0, 90.0) * rng.choice((-1, 1))
                for message, frequency, snr in ((weak_message, base, weak_snr),
                                                (strong_message, base + offset, strong_snr)):
                    signal = ft8_signal(work, message, frequency)
                    amplitude = math.sqrt(noise_in_band * 10 ** (snr / 10.0))
                    for i, value in enumerate(signal[:len(mixed)]):
                        mixed[i] += amplitude * value
                weak.append(weak_message)
            gauss = random.Random(args.seed * 7919 + index)
            for i in range(len(mixed)):
                mixed[i] += gauss.gauss(0.0, noise_rms)
            source = os.path.join(work, "mix-%02d.wav" % index)
            write_wav(source, mixed)

            original = decode(work, source)
            totals["original"][0] += sum(message in original for message in weak)
            totals["original"][1] += len(weak)
            for variant in variants:
                for rate in rates:
                    out = os.path.join(work, "out-%02d" % index)
                    os.makedirs(out, exist_ok=True)
                    subprocess.run([LAB, "--audio", source, "--variants", variant, "--rates", str(rate),
                                    "--write-dir", out, "--passband", args.passband],
                                   check=True, stdout=subprocess.DEVNULL)
                    decoded = decode(work, os.path.join(out, "%s-%d.wav" % (variant.replace("/", "_"), rate)))
                    key = "%s@%d" % (variant, rate)
                    totals.setdefault(key, [0, 0])
                    totals[key][0] += sum(message in decoded for message in weak)
                    totals[key][1] += len(weak)
            print("file %d/%d done" % (index + 1, args.files), file=sys.stderr)
    finally:
        shutil.rmtree(work)

    print("%-24s %8s %8s %8s" % ("audio", "weak", "decoded", "rate"))
    for key, (decoded, total) in totals.items():
        print("%-24s %8d %8d %7.1f%%" % (key, total, decoded, 100.0 * decoded / max(1, total)))


if __name__ == "__main__":
    main()
