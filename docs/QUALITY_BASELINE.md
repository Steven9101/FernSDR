# Codec quality baseline

Taken with `audio-quality --sweep` for each mode; `make quality` runs
`audio-quality` for USB at 48 kbit/s only, without the codec row at each
bitrate that `--sweep` adds. These are the numbers a codec change has to be compared
against. Regenerate with:

```sh
for m in usb cw am nfm; do server/build/audio-quality --mode $m --sweep; done
```

Chain SNR is against the transmitted audio, so the fading counts as error and
it is pessimistic; it is there to compare the rows above the codec. Codec SNR
is against the audio handed to the encoder, and is the column that judges the
codec. It is the only one that moves with the bitrate, which is what makes it
usable.

## USB

```
chain                                         chain SNR  codec SNR       peak      bitrate
-----                                         ---------  ---------       ----      -------
channel only (no AGC, no blanker, no codec)     -5.5 dB          -      0.564            -
AGC only                                        -6.3 dB          -      0.616            -
AGC + blanker                                   -5.4 dB          -      0.452            -
AGC + blanker + codec at 16 kbit/s              -5.1 dB    10.0 dB      0.378  15.2 kbit/s
AGC + blanker + codec at 24 kbit/s              -5.3 dB    10.9 dB      0.440  23.2 kbit/s
AGC + blanker + codec at 32 kbit/s              -5.1 dB    16.2 dB      0.461  31.0 kbit/s
AGC + blanker + codec at 48 kbit/s              -5.1 dB    31.0 dB      0.451  47.4 kbit/s
AGC + blanker + codec at 64 kbit/s              -5.3 dB    45.2 dB      0.453  63.3 kbit/s
AGC + blanker + codec at 96 kbit/s              -5.3 dB    64.6 dB      0.452  95.5 kbit/s
```

## CW

```
chain                                         chain SNR  codec SNR       peak      bitrate
-----                                         ---------  ---------       ----      -------
channel only (no AGC, no blanker, no codec)     -0.4 dB          -      1.020            -
AGC only                                         1.0 dB          -      0.193            -
AGC + blanker                                    1.0 dB          -      0.196            -
AGC + blanker + codec at 16 kbit/s               0.8 dB    18.5 dB      0.239  15.4 kbit/s
AGC + blanker + codec at 24 kbit/s               1.1 dB    32.3 dB      0.198  23.8 kbit/s
AGC + blanker + codec at 32 kbit/s               1.0 dB    42.7 dB      0.194  31.3 kbit/s
AGC + blanker + codec at 48 kbit/s               1.0 dB    56.8 dB      0.196  46.7 kbit/s
AGC + blanker + codec at 64 kbit/s               1.0 dB    59.0 dB      0.196  57.7 kbit/s
AGC + blanker + codec at 96 kbit/s               1.0 dB    59.1 dB      0.196  68.2 kbit/s
```

## AM

```
chain                                         chain SNR  codec SNR       peak      bitrate
-----                                         ---------  ---------       ----      -------
channel only (no AGC, no blanker, no codec)    -11.3 dB          -      2.115            -
AGC only                                       -11.5 dB          -      0.597            -
AGC + blanker                                   -7.6 dB          -      0.451            -
AGC + blanker + codec at 16 kbit/s              -7.8 dB     8.3 dB      0.257  15.1 kbit/s
AGC + blanker + codec at 24 kbit/s              -7.7 dB    10.2 dB      0.358  23.1 kbit/s
AGC + blanker + codec at 32 kbit/s              -7.8 dB    11.5 dB      0.425  30.2 kbit/s
AGC + blanker + codec at 48 kbit/s              -7.5 dB    16.4 dB      0.456  46.9 kbit/s
AGC + blanker + codec at 64 kbit/s              -7.5 dB    24.2 dB      0.453  62.3 kbit/s
AGC + blanker + codec at 96 kbit/s              -7.5 dB    40.8 dB      0.450  94.8 kbit/s
```

## NFM

```
chain                                         chain SNR  codec SNR       peak      bitrate
-----                                         ---------  ---------       ----      -------
channel only (no AGC, no blanker, no codec)     -3.4 dB          -      0.225            -
AGC only                                        -3.4 dB          -      0.225            -
AGC + blanker                                   -2.6 dB          -      0.182            -
AGC + blanker + codec at 16 kbit/s              -2.2 dB     6.7 dB      0.137  14.9 kbit/s
AGC + blanker + codec at 24 kbit/s              -2.6 dB     8.7 dB      0.157  22.9 kbit/s
AGC + blanker + codec at 32 kbit/s              -2.8 dB    10.1 dB      0.174  30.5 kbit/s
AGC + blanker + codec at 48 kbit/s              -2.6 dB    15.5 dB      0.189  46.8 kbit/s
AGC + blanker + codec at 64 kbit/s              -2.5 dB    23.3 dB      0.184  62.3 kbit/s
AGC + blanker + codec at 96 kbit/s              -2.5 dB    39.7 dB      0.182  94.7 kbit/s
```

## A correction, kept on purpose

An earlier version of this file reported two findings from these tables: that
AM was not monotonic in bitrate, and that NFM clipped at a peak of 1.5. Both
were wrong, and the cause was in the harness rather than in the receiver.

The transmitter synthesised SSB whatever mode was asked for. It even said so
in its own output line. So the AM and NFM rows were measuring an AM or FM
demodulator pointed at an SSB signal, which is not a thing any listener would
hear and not a measurement of anything.

With the transmitter modulating the mode being received, AM reads 8.7, 14.7
and 30.1 dB across the sweep and NFM reads 5.1, 14.0 and 27.1. Both monotonic,
and NFM peaks below full scale.

The correction is left here rather than quietly removed, because the failure
is the interesting part: a metric was rebuilt specifically so it could tell
better from worse, it immediately produced two confident results, and both
came from a part of the harness nobody had thought to check. A number is only
as good as the signal it was taken from.

## One hypothesis, measured and disproven

NFM peaks at 0.92 at 3 kHz deviation, which is what the discriminator gain is
normalised to, and there is no gain control after it because the AGC runs
ahead of demodulation. So a station running wider should exceed full scale.

It does not. `--deviation` sweeps it: at 3, 5, 6, 8, 12 and 20 kHz the peak
reads 0.92, 0.96, 0.80, 0.78, 0.84 and 0.89. It does not grow at all, because
the reasoning left out the passband: the channel filter is +/-6 kHz, so a
station running wider is bandlimited before the discriminator sees it, and the
phase steps it produces are limited with it.

Two hypotheses from reading this code have now been checked against it, and
both were wrong. That is the argument for the harness rather than against the
reading.

The discriminator's gain has since been brought down to the other modes'
level, 3 kHz of deviation at 0.25, so the NFM rows above peak near 0.2.
