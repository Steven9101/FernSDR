# Recordings

Real IQ, for measuring against something that was actually on the air rather
than only against a model of it.

`capture-20m.s16` is two seconds of the 20 m band, taken from an RTL dongle at
2.048 Msps, mixed down 25 kHz and decimated to 204800 Hz, as interleaved
16-bit signed IQ. It holds ordinary SSB traffic and the noise that goes with
it. 1.6 MB.

    server/build/audio-quality --iq server/testdata/capture-20m.s16 \
        --rate 204800 --center 14175000 --tune 14175000 --mode usb --sweep

## What these numbers mean

A recording has no ground truth, so only the codec column means anything: it
is measured against the audio the encoder was handed.

Measured on this file at three tunings a kilohertz apart, per bitrate:

| Tune | 16 | 24 | 32 | 48 | 64 | 96 kbit/s |
|---|---|---|---|---|---|---|
| 14.174 MHz | 7.1 | 10.8 | 13.7 | 19.7 | 30.8 | 55.4 |
| 14.175 MHz | 7.2 | 10.9 | 13.4 | 19.4 | 30.0 | 54.6 |
| 14.176 MHz | 15.5 | 16.2 | 23.0 | 29.2 | 40.3 | 59.8 |

The first two agree closely, as they should: a kilohertz apart in the same
passband is nearly the same audio. The third is a stronger signal and scores
better throughout, which is also as it should be.

Getting there needed the comparison hardened. The fractional-delay correction
estimates a phase slope, and on a quiet passband carrying a real fading signal
that estimate can be wrong: the same three runs previously read -18.3 dB at
24 kbit/s and 9.1 dB at 48, which a codec cannot do against its own input. The
correction now has to earn its place, and is kept only when the residual it
leaves is smaller than the residual without it.
