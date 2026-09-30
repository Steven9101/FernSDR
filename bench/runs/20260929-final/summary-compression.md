| receiver | audio codec | lossy | audio kbit/s | waterfall coding | lossy | waterfall kbit/s | rows/s | bins/row | bytes/row | bits/bin | vs raw 8-bit |
|---|---|---|---|---|---|---|---|---|---|---|---|
| fernsdr | NAC3, FernSDR's own MDCT transform codec | yes | 28.5 | WFC5: dB levels rounded to 1 dB (at most 0.5 dB error) or, as the page asks in Low and Balanced, 2 dB (at most 1 dB), range -200 to +100 dB; each residual against the bin to the left coded as binary decisions with adaptive probabilities, in contexts from the row above, through LZMA's range coder; a key row every 24 rows | yes | 21.0 | 12.0 | 1536 | 219 | 1.14 | 7.0x |
| ka9q-web | Opus (default) over RTP; 16-bit PCM at 12 kHz optional | yes | 19.5 | one byte a bin, uncompressed, after a float header | yes | 88.7 | 6.4 | 1620 | 1724 | 8.51 | 0.9x |
| novasdr | IMA ADPCM (default), Opus 40 kbit/s optional | yes | 51.2 | 8-bit dB rows in CBOR, zstd stream | yes | 26.8 | 7.8 | n/a | 428 | n/a | n/a |
| openwebrx | IMA ADPCM, 4 bits a sample at 12 kHz | yes | 44.5 | ADPCM-coded FFT rows | yes | 147.9 | 9.0 | 4096 | 2054 | 4.01 | 2.0x |
| openwebrx-plus | IMA ADPCM, 4 bits a sample at 12 kHz | yes | 44.7 | ADPCM-coded FFT rows | yes | 147.3 | 9.0 | 4096 | 2054 | 4.01 | 2.0x |
| phantomsdr | FLAC of 16-bit samples in CBOR | no | 81.9 | int8 dB rows, zstd stream | yes | 53.4 | 15.6 | n/a | 427 | n/a | n/a |
| phantomsdr-plus | FLAC at 8 bits a sample in CBOR | yes | 38.0 | int8 dB rows, zstd stream | yes | 116.7 | 15.6 | n/a | 933 | n/a | n/a |
| phantomsdr-plus-sv1btl | FLAC of 16-bit samples (default); Opus for C-QUAM | no | 65.8 | int8 dB rows, zstd stream | yes | 53.4 | 15.6 | n/a | 427 | n/a | n/a |
| ubersdr | Opus 48 kbit/s (default) | yes | 44.1 | binary8 spectrum frames ("SPEC") | yes | 13.6 | 10.0 | 1024 | 170 | 1.33 | 6.0x |
| vertexsdr | the PA3FWM WebSDR audio format (tag-byte frames) | yes | 27.1 | PA3FWM waterfall format 9, variable-length coded | yes | 7.6 | 5.2 | 1024 | 182 | 1.42 | 5.6x |
| websdr | PA3FWM's own 8 kHz compression | yes | 49.5 | PA3FWM waterfall format 9/10, 1.5 to 2.3 bits a pixel | yes | 22.8 | 15.6 | 1024 | 183 | 1.43 | 5.6x |
