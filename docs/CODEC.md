# Bitstreams

In plain words: to fit through a slow connection, the sound and the waterfall
are packed small before they are sent, and the browser unpacks them. This is
exactly how, bit by bit, for anyone writing their own client.

Two formats: **NAC** for audio and a line codec for the waterfall. Both are
documented here in enough detail to write an independent decoder, and both are
implemented three times over - the C++ encoder/decoder, the TypeScript decoder
in the browser, and the Python decoder in `tools/fernsdr-probe.py`. Cross-check
vectors keep them honest (`make -C server vectors`).

## Shared conventions

Bits are packed **most significant first**. Multi-byte fields in the message
headers (not the bitstreams themselves) are little-endian.

### Rice coding

For an unsigned value `v` with parameter `k`:

1. `q = v >> k`
2. If `q < 24`: emit `q` one-bits, then a zero bit, then the low `k` bits of `v`.
3. Otherwise (the escape): emit 24 one-bits, a zero bit, then `v` as 32 raw bits.

The escape bounds the worst case. Without it a single outlier could occupy
thousands of bits.

### Exp-Golomb coding

Order 0. For an unsigned `v`, let `w = v + 1` and `n` be the number of bits in
`w` minus one. Emit `n` zero bits, then `w` in `n + 1` bits.

### Signed values

Zig-zag mapped before coding: `u = (v << 1) ^ (v >> 31)`, so small magnitudes
of either sign become small unsigned values.

---

## NAC - the audio codec

### What it is for

The constraint that shaped every decision: **people decode digital modes from
this audio**. FT8, RTTY, PSK31, WSPR and packet all travel through the audio
path and are demodulated in software at the far end.

That rules out most of what a modern audio codec does:

- **No psychoacoustic model.** Masking thresholds discard exactly the weak
  tone next to a strong one that a digital decoder needs.
- **No time warping, ever.** Opus and friends stretch or compress time to
  conceal loss and to resynchronise. A decoder riding on the audio loses
  symbol timing when that happens.
- **No pitch prediction or bandwidth extension.** Both invent signal that was
  never received.

Decoding is a pure linear operation: dequantise, inverse MDCT, overlap-add.

### Frame structure

| | |
|---|---|
| Hop | 128 samples |
| Window | 256 samples, sine |
| Delay | 21.3 ms at 12 kHz |
| Frame rate | sample rate / 128 (93.75/s at 12 kHz) |
| Coefficients per frame | 128 |

The sine window `w[n] = sin(pi/N * (n + 0.5))` satisfies the Princen-Bradley
condition `w[n]^2 + w[n+M]^2 = 1`, so window, transform, inverse transform,
window and overlap-add reconstruct the input exactly.

### The MDCT, and its factorisation

The transform is the standard one:

```
X[k] = sum_{n=0}^{2M-1} x[n] * cos(pi/M * (n + 0.5 + M/2) * (k + 0.5))
```

It is computed as a fold followed by a DCT-IV, and the DCT-IV through a
complex FFT of length `M/2`. Both stages are worth writing down because
getting either slightly wrong produces output that looks plausible and is
subtly wrong.

**The fold.** Split the 2M-sample window into quarters `a`, `b`, `c`, `d` of
length `M/2`. Then `MDCT(x) = DCT-IV(u)` where

```
u[i]       = -c[M/2-1-i] - d[i]        for i in [0, M/2)
u[M/2 + i] =  a[i] - b[M/2-1-i]        for i in [0, M/2)
```

This follows from two symmetries of the kernel: it is even about `t = -1/2`
and odd about `t = M - 1/2`.

**The DCT-IV.** With `P = M/2`, form `c[p] = u[2p] + i*u[M-1-2p]`, then

```
S[2m] = e^{-i*pi*(m + 1/8)/M} * FFT_P( c[p] * e^{-i*pi*(p + 1/8)/M} )[m]

X[2m]       =  Re(S[2m])
X[M-1-2m]   = -Im(S[2m])
```

The `1/8` is not a typo and is the detail most implementations get wrong.
Expanding `(2p + 1/2)(2m + 1/2) * pi/M` gives `2*pi*p*m/P` plus a residual
`pi*(p + m + 1/4)/M`; splitting that residual evenly between the input and
output twiddles leaves `1/8` on each side. A value of `1/4` on either side is
a common error that still round-trips, and still produces the wrong spectrum.

### Band layout

The 128 coefficients are grouped into 17 bands, narrow at the bottom where a
misplaced quantiser is most audible and wider at the top where scale-factor
side information would otherwise dominate:

```
widths:  4 4 4 4 4 4 4 4  8 8 8 8 8 8  16 16 16
starts:  0 4 8 12 16 20 24 28  32 40 48 56 64 72  80 96 112
```

### Quantisation

Each band gets its own scale factor, taken from the band's RMS rather than its
peak: the Rice coder handles an occasional outlier more cheaply than raising
the whole band's step would.

```
exponent[b] = round(4 * log2(rms[b]))           clamped to [-200, 200]
step[b]     = 2^((exponent[b] - quality) / 4)
q[i]        = round(coefficient[i] / step[b])
```

Because the step tracks each band's own level, quantisation noise sits a fixed
number of dB below the signal **in that band**. This is the property that
keeps a weak tone alive next to a strong carrier - the one a psychoacoustic
codec deliberately gives up.

**Silent bands.** A band is coded as silent when its RMS falls below either an
absolute floor (1e-6) or 60 dB below the loudest band in the frame. Without
that dynamic-range limit, window leakage keeps every band nominally occupied
and the rate loop spends its entire budget holding 80 dB-down noise to the
same relative accuracy as the signal. 60 dB is far more than any demodulated
channel carries.

### Rate control

Quality is a 6-bit index in quarter-bit steps (`Q = index / 4`). The encoder
binary-searches for the highest quality whose frame fits the target size; cost
is monotone in quality, so this converges in six probes. If even quality 0
overflows, the quietest bands are dropped until it fits, so the encoder never
emits an oversized frame at the effective bitrate. The minimum is 8 kbit/s
or the three-byte frame header times the frame rate, whichever is larger
(9 kbit/s at 48 kHz). The encoder reports this effective ceiling.

The Rice parameter is derived from the quality alone -
`k = clamp(quality/4 - 1, 0, 20)` - so no side information is needed.

The encoder also tries adding 4 or 8 to both the transmitted quality index
and every active band's exponent. Their difference, and therefore the
reconstruction step and quantised coefficients, stays the same. The larger
Rice parameter can encode those coefficients in fewer bits. All three
representations are compared, including their exponent overhead, and the
cheapest one wins. Values remain within the existing format's bounds; old
decoders need no change. The saved bits let the rate search retain more
precision at the same frame budget.

### Frame layout

```
  6 bits   quality index
 17 bits   one "band is active" flag per band, in band order
           then, for each active band in order:
  varies     signed Exp-Golomb of exponent[b] - previous
             (previous starts at -40, and updates only on active bands)
           then, for each active band in order, for each coefficient:
  varies     signed Rice, parameter k, of the quantised value
           zero padding to a byte boundary
```

A decoder that reads past the end must discard the frame and conceal, not use
whatever it parsed.

### Compact side information (NAC2)

The client negotiates `nac2` in `hello`. Audio flag bit 1 selects this layout
for that frame. Frames without the flag retain the layout above, including
when they arrive between compact frames. Both layouts use the same quantised
coefficients, reconstruction steps, overlap and sample clock.

After the six-bit quality index, read a two-bit mask selector:

| Selector | Activity mask |
|---|---|
| 0 | Read all 17 activity bits |
| 1 | All 17 bands are active |
| 2 | Read a five-bit count, activate that many bands from band 0 |
| 3 | Read a five-bit first band and five-bit count, activate that interval |

Prefix count 0 means silence. Counts may not exceed 17. An interval must
start below 17, contain at least one band and end at or before band 17.

If any band is active, its first exponent is nine bits encoding `exponent +
200`, followed by a three-bit delta selector. Absolute codes above 400 are
invalid. Delta selector 0 uses signed Exp-Golomb; selectors 1 through 5 use
signed Rice with `k = selector - 1`. Selectors 6 and 7 are invalid. Read one
delta for each remaining active band, relative to the previous active band.
Every accumulated exponent must stay in `[-200,200]`. A one-band frame still
carries the selector but has no deltas. An empty mask carries neither field.

Coefficients and final zero padding follow the original layout. The encoder
keeps its original quantiser decision, then compares padded frame sizes and
uses NAC2 only when smaller. Feeding the saved metadata bits back into the
quality search would spend them again rather than reduce traffic. No previous
packet is needed to unpack a compact frame.

### Per-band packets (NAC3)

The client negotiates `nac3` in `hello`. Audio flag bit 3 marks a NAC3
packet. It uses the same MDCT, window, band layout and concealment as NAC,
and differs in two things: where the quantisation noise goes, and how side
information is carried.

**Where the noise goes.** NAC and NAC2 give every band the same SNR relative
to its own energy. Receiver audio is mostly the channel's own noise, so that
codes a band of noise 26 dB below itself, which nobody hears or decodes, and
it codes a band holding one strong and one weak station relative to the strong
one, where the weak one can drown. The NAC3 encoder instead places each band's
quantisation noise `noise_margin_db` below the channel noise it measures in
the listener's passband. With a 12 dB margin the codec adds 0.27 dB to the
noise already there, everywhere in the passband, whatever else shares a band.
A band far above the noise needs no more than `max_snr_db` relative to itself,
and gets no less than `min_snr_db`. Content 24 dB under the passband noise
outside the passband, or `margin + 24` dB under it inside, is left silent.

The channel noise is estimated from the codec's own coefficients, in the
units before the receiver's AGC. The passband's coefficients are pooled into a
histogram of their energies that forgets over about a second; the MDCT
coefficients of Gaussian noise have chi-square energies with one degree of
freedom, whose 20th percentile is 0.0642 of their mean. A sixteen-second
memory of that estimate keeps its minimum: crowded digital segments fill every
coefficient while they transmit, and the transform's leakage lifts every
percentile with them, but FT8 stations pause together for 2.4 s of every 15.
A band's own minimum over about a second may lower its estimate, never raise
it. Digital silence, such as a closed squelch, is left out. None of this is in
the bitstream: the decoder only reads step sizes.

When a frame's cost exceeds the ceiling set by the bitrate, every band's step
coarsens by the same number of quarter octaves until it fits, so the noise rise
stays even across the passband. Frames that fit use only what their targets
need, so the rate follows the signal instead of sitting at the ceiling.

**Packet layout.** A NAC3 payload holds one to four consecutive frames:

```
  2 bits   frame count - 1
  frame 1, independent
  frames 2..count, predicted
  zero padding to a byte boundary, once, at the end
```

An independent frame:

```
  2 bits   mask selector, with the fields of NAC2's compact mask
  if any band is active:
   9 bits  step index of the first active band, + 200
   3 bits  step residual selector: 0 signed Exp-Golomb, 1-5 signed Rice k = selector - 1
   varies  step index of each further active band, as a residual against the previous active band
   4 bits  Rice parameter of the first active band
   varies  Rice parameter of each further active band, signed Rice k = 0 against the previous
  for each active band, for each coefficient:
   varies  signed Rice of the quantised value, with that band's parameter
```

A predicted frame:

```
  1 bit    1: the same active bands as the previous frame; 0: a mask follows as above
  if any band is active:
   3 bits  step residual selector, as above
   varies  each active band's step index against its reference
   varies  each active band's Rice parameter against its reference, signed Rice k = 0
  coefficients as above
```

A band's reference is its step index and Rice parameter in the previous frame
of the packet. After the first frame, a band it left silent takes the values
of the nearest active band to its left, or of the first active band if none
is to its left, or zero if no band was active. A later frame updates only the
bands it carries. Nothing is predicted across packets, so a packet decodes on
its own.

The step index `s` is in quarter octaves: a coefficient is `value * 2^(s/4)`.
Steps must stay within `[-200, 200]` after every residual and Rice parameters
within `[0, 15]`. A coefficient whose magnitude exceeds `1e7` is not receiver
audio; a decoder rejects the frame. A malformed frame and every later frame of
its packet are concealed, so the output always holds whole frames.

The audio sequence counts frames. A packet carries the sequence of its first
frame, and the next packet's is that plus the count. Each extra frame in a
packet adds one frame of delay, 10.7 ms at 12 kHz, and saves a packet header
and most of that frame's side information. The client's profiles ask for one
frame (High), two (Balanced) or four (Low), with margins of 18, 12 and 10 dB.

### Concealment

A lost frame is concealed by repeating the previous spectrum at half
amplitude, halving again for each consecutive loss. It produces **exactly one
hop** of output, like any other frame. The output sample clock is therefore
never disturbed, which is what lets a digital decoder keep symbol timing
across a dropout.

The player uses a separate decoder state while waiting for delayed packets.
Concealed output does not fill the receive queue or overwrite the overlap
needed by returning real frames. After rebuffering, a 5 ms fade brings the
real signal back. Persistent excess latency still requires a discontinuous
trim; concealment cannot recover information that did not arrive.

### Measured behaviour

`server/build/codec-lab` (`make -C server lab`) runs off-air IQ or audio
through the receive chain and every codec setting, and reports payload and framed rates, SNR against the codec's input,
and the noise rise per band and frame against the recording's own noise floor.
`tools/codec-lab-ft8.sh` and `tools/codec-lab-ft8-weak.py` count FT8 messages
that survive, the second with synthetic weak signals beside strong ones. The following fixtures describe the earlier
encoder and are not a comparison against other codecs.

| Signal | Rate | Result |
|---|---|---|
| 1 kHz tone, 12 kHz | 46 kbit/s | 44 dB SNR |
| Speech-like, 8 kHz | under 48 kbit/s | over 15 dB SNR |
| Tone 45 dB below a carrier | 48 kbit/s | recovered within 3 dB |
| Digital silence | - | under 3 kbit/s |
| Full-band white noise | 24 / 48 / 96 kbit/s | 5.6 / 14.0 / 30.4 dB SNR |

The white-noise row is the worst case a codec can be given: all 128
coefficients incompressible, so 24 kbit/s really does buy only about two bits
each. Real channels are filtered and do far better.

---

## The waterfall line codec

A spectrum line is a row of dB values. Successive lines are strongly
correlated - the noise floor barely moves - so the temporal delta is small.

### Quantisation

Values are clamped to [-200, +100] dB and rounded to the nearest whole
decibel. WFC4 adds optional 2 dB steps. This reduces level detail, with at
most 1 dB error inside the codec range instead of 0.5 dB. It retains the same
frequency bins, peak selection and requested line rate. The saving depends
on the signal and spectrum averaging; it is not lossless compression.

Quantise directly from the source level: `round(db / step_db)`. Rounding to
whole decibels first would increase the error bound. Prediction and residual
coding operate on these integer units; multiply by `step_db` after decoding.
The 1 dB range is [-200,100], with an intra seed of -100. At 2 dB these become
[-100,50] and -50. Linear predictor clamping uses the corresponding range.

### Prediction

Two modes, chosen by the encoder:

- **Temporal** (mode 0): residual against the same bin of the previous line.
- **Intra** (mode 1): residual against the previous bin of the same line, with
  the first bin predicted from a fixed -100 dB seed.

The encoder emits an intra line for the first line, whenever the width
changes, and whenever the viewport moves - so a client that joins or retunes
decodes the very next line without waiting for a keyframe. It also emits one
after a line is dropped for backpressure, since that would otherwise
desynchronise the decoder's prediction chain.

The encoder compares spatial and temporal costs on other lines, and emits an
intra line at least every two seconds. A client detecting a sequence gap or a
failed decode discards its predictor until the next intra line.

### Line layout

```
  1 bit    mode: 0 temporal, 1 intra
  4 bits   Rice parameter k
  varies   signed Rice, parameter k, of each residual in bin order
           zero padding to a byte boundary
```

`k` is chosen per line by evaluating the coded length at the estimate from the
mean residual magnitude and at its two neighbours.

A temporally-predicted line with no matching history must be rejected, not
guessed at.

Clients can negotiate `wfc2` in their viewport command. If bit 0 of the
waterfall packet's flags is set, the mode and Rice header are unchanged, but
each residual token is one of:

- `0`, followed by unsigned Exp-Golomb of `run length - 1`: that many zero residuals.
- `1`, followed by a signed Rice residual using the header's `k`.

Runs must fit in the remaining bins. Reconstruction must stay within the
quantised level range. The encoder compares this representation with ordinary
Rice coding and sets the flag only when it is smaller. Clients without the
negotiation receive the original representation. Both preserve the same 1 dB
levels. Quiet spans benefit most; noisy spectra do not have the same savings.

### Additional predictors (WFC3)

Clients requesting `"codec":"wfc3"` also accept a two-bit mode header when
packet flag bit 1 is set. The four-bit Rice parameter and residual tokens
are unchanged. With `L` the decoded bin to the left, `T` the previous row's
bin and `TL` the previous row's left bin:

| Mode | Predictor |
|---|---|
| 0 | `T`, requires a previous row |
| 1 | `L`, with a first-bin seed of -100 |
| 2 | `clamp(L + T - TL, min(L,T), max(L,T))`; first bin uses `T` |
| 3 | `clamp(2*L - LL, -200,100)`; first two bins use mode 1 |

Modes 1 and 3 are independent rows. Modes 0 and 2 require valid history.
Every zero residual in a run must recompute the predictor: mode 3 can encode
a ramp as a zero run. Commit the previous row only after the entire payload
validates. Flag bit 0 continues to select zero-run tokens independently.

The encoder compares complete padded byte counts, including headers. Modes
0 and 1 keep the one-bit header with flag bit 1 clear. A new predictor is
used only when it saves a whole byte. All formats share the same previous
quantised row, so changing the payload representation does not reset history.

Residuals lie in `[-300,300]`. The encoder counts their zigzag values and
prices each Rice parameter exactly, including escape codes. Combining
adjacent histogram entries divides each quotient by two and gives the next
parameter's distribution. This avoids repeatedly scanning a wide row to
calculate those costs. It does not alter the quantiser.

### Two-decibel levels (WFC4)

WFC4 accepts the WFC3 layouts. Packet flag bit 3 selects 2 dB units instead
of 1 dB, independently of the predictor and zero-run flags. An encoder must
send an independent row when precision changes. A temporal or gradient row
whose precision differs from the stored previous row must be rejected. Only
commit a new precision and previous row after validating the entire payload.
An independent intra or linear row recovers after a dropped transition row.

### Range-coded rows (WFC5)

WFC4 Rice-codes every residual, which costs at least one bit a bin however
predictable a row is. On rows from the benchmark lab (1536 bins, 12 rows a
second) most residuals are zero, and whether one is, and which way it goes,
depends on how the row above moved: the residuals carry about 1.0 bit a bin
while WFC4 spent 1.51 at 2 dB steps (1.23 against 1.67 at 1 dB). WFC5 codes
each residual as a few binary decisions with adaptive probabilities, through
LZMA's binary range coder, and spends 1.03 and 1.24 bits a bin on the same
rows, 32 and 26 % less. Levels, steps, native grids and the peak selection
are those of WFC4; only the payload differs. Packet flag bit 4 marks it.

The payload is one header byte, bit 0 set on a key row and the other bits
zero, then the range coder's output without its first byte (LZMA's encoder
always emits a zero first; the decoder starts by reading four bytes). A
decoder reading past the end takes zero bytes; more than four such reads,
or a level outside the quantiser's range, fails the row.

Each bin `i`, in order, with levels in quantiser units:

- `L` is the decoded bin to the left; for the first bin, the previous row's
  first bin, or on a key row the seed (-100 at 1 dB, -50 at 2 dB).
- On a row that is not a key row, `T` is the previous row's bin `i`,
  `a = bucket(T - L)` and `d` is 1 when `T > L`, 2 when `T < L`, 0 when equal.
  On a key row, `a = 4` and `d = 3`. `bucket(v)` of `|v|` is 0 for 0, 1 for 1,
  2 for 2 to 3, and 3 above.
- `r = level - L`; `p = r` of the bin before (0 for the first), `b = bucket(p)`
  and `s` is 1 when `p > 0`, 2 when `p < 0`, 0 otherwise.
- Code `r != 0` with `zero[a][b][d]`. If it is not zero, code `r < 0` with
  `sign[a][d][s]`, then `m = |r| - 1` in unary: for `j` from 0 up to 11, a one
  with `magnitude[min(j,4)][a][g]` for every step below `m` and a zero to
  stop, where `g` is 0 on a key row or when `T == L`, 1 when `T > L` and
  `r > 0` agree in direction, and 2 when they do not. After twelve ones,
  `m - 12 + 1 = n` follows as plain bits: four bits of `w = floor(log2 n)`
  (at most 9), then the `w` bits of `n` below its leading one.

Probabilities are 11-bit chances of a zero, 1024 at the start, and move by a
32nd of the distance to 0 or 2048 after each bit, as in LZMA. A key row resets
all of them. The encoder sends a key row every 24 rows, on the first row, and
whenever WFC4 would send an intra row (a view or width change, a precision
change, a dropped row, two seconds). A row that is not a key row needs the
previous row at the same width and precision and the probabilities as they
were after it; without them it must be rejected, and the client waits for the
next key row.

### Native bins when zoomed in

WFC3 clients accept packet flag bit 2 for a native FFT grid. A narrow view
can request more pixels than there are FFT bins behind it. The server sends
the native bins plus the neighbours needed for interpolation, with their
exact bin-cell frequency bounds. Width can be as small as two. The flag is
independent of the payload predictor, including its one-bit fallback.

The client interpolates decoded dB levels before mapping them to colours or
clipping the trace to its display range. Guard bins outside the visible
viewport do not enter automatic level statistics. The GL texture stores
decoded levels as R16F, preserving every integral codec level over the full
range before interpolation.

This changes the order of quantisation and interpolation. With in-range
native levels, reconstruction differs from the unquantised interpolated
spectrum by at most 0.5 dB with 1 dB steps, or 1 dB with 2 dB steps. If a source level would be clipped by the codec,
the server uses ordinary viewport rendering first. Wide views retain the
existing peak decimation so narrow carriers are not averaged away.

### Measured

Recorded-signal bitrate is measured with the same `codec-lab`. Spectrum
averaging, zoom, bin count and signal content all change the rate; one
bits-per-bin figure cannot describe every receiver.
