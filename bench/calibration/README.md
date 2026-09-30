# Measurement calibration

`browser/calibrate.mjs` sends scheduled rectangular pulses through a browser's
normal output and the lab tap. HTTP pages use lossless native MediaRecorder
PCM with a separate sample-counter channel; secure pages use AudioWorklet.
The counter channel is never connected to audible output. The HTTP stream is
decoded by the installed FFmpeg 6.1.1. Tests cover counter rollover and lost
frames, which must remain timestamp gaps.

The earlier ScriptProcessor fallback is retired. Chromium can skip an input
quantum when its shared-buffer lock is busy, without advancing the buffer
index. Treating each callback as contiguous silently moved later samples.

`monitor.c` records `fb.monitor` with libpulse 16.1 and saves its raw timing
snapshots. These are client timing estimates; the measured discrepancies do
not permit treating them as a sub-millisecond reference.

`sink_trace.c` instead observes the isolated null sink at its render boundary.
It interposes only on the lab process through LD_PRELOAD, never on the system
or an existing PulseAudio service. It requires the exact PulseAudio 16.1 ABI,
a single 48 kHz stereo float32 null sink and zero port-latency offset.
The null-sink latency callback returns its scheduled render timestamp minus
its monotonic clock. A bracket around that callback recovers the timestamp;
the trace records the bracket width and realtime/monotonic conversion.
Rewinds invalidate later rendered samples. A bounded ring moves PCM copies
to a separate writer thread, and any overflow invalidates the trace.

Files open after the first render, because PulseAudio closes inherited file
descriptors during startup. Opening them from the preload constructor could
leave stale descriptors subsequently reused by the daemon.

The implementations were checked against PulseAudio's
[16.1 null sink](https://github.com/pulseaudio/pulseaudio/blob/v16.1/src/modules/module-null-sink.c),
[sink latency callback](https://github.com/pulseaudio/pulseaudio/blob/v16.1/src/pulsecore/sink.c),
[record timing calculation](https://github.com/pulseaudio/pulseaudio/blob/v16.1/src/pulse/stream.c)
and [timing API](https://github.com/pulseaudio/pulseaudio/blob/v16.1/src/pulse/def.h).
The local package is `1:16.1+dfsg1-2ubuntu10.1`.

Build the trace with `make -C bench/calibration`. Building the optional monitor
also needs matching libpulse headers and libraries. Development headers were
extracted into `/tmp/fernbench-pulse-dev/root` from the matching Ubuntu package;
no system audio installation was changed.

Run evidence in `runs/20260927-output-calibration-*` is calibration development
only. Native PCM passed all 24 pulse frame checks at 12, 32 and 48 kHz without
holes. The first successful sink trace retained every event, with a maximum
clock bracket of 37.17 microseconds, but found browser timestamp errors up to
9.28 ms. Absolute browser-API calibration therefore remains failed. Physical
sound-card and speaker latency is not measured on this host.
