# Receiver benchmark laboratory

Compares FernSDR with ten other web SDR receivers on identical generated
input, pinned builds, and the audio their own pages play. How it works and
what it cannot say: [METHOD.md](METHOD.md). Results: the published round's
summaries in [runs/20260929-final](runs/20260929-final/summary.md) and the
capacity steps in `runs/20260929-capacity-R1` (with `-R1y`, the 1,600 step on
a quiet machine). Each run's own files, without the audio, are the archive
`fernsdr-bench-runs-20260929.tar.xz`, attached to the
[v0.1.0 release](https://github.com/Steven9101/FernSDR/releases/tag/v0.1.0).

| Directory | What |
|---|---|
| `receivers/` | one per receiver: Dockerfile, configs, page adapter ([contract](receivers/README.md)) |
| `source/` | `scene.py` renders scenes, `pace` delivers them in real time |
| `scenes/` | the scene files |
| `net/` | the isolated lab network and its checks |
| `browser/` | the instrumented browser: audio tap, recorder, smoke check |
| `harness/` | `links.py` (the session: listener, digital, tones, links), `capacity.py`; `run.py` for smoke checks |
| `load/` | the replay clients for capacity |
| `digital/` | digital-mode signals, decoders, ideal receiver |
| `analysis/` | turns runs into tables |
| `calibration/` | the audio-output timing checks |
| `inventory/` | features per receiver, with sources |
| `runs/` | the published round's summaries; runs themselves are on the release, audio captures stay local |

Run it as root on a machine you can spare: it builds containers, creates
network namespaces and a bridge, and puts load on two cores. It refuses to
start without 1.5 GB of free memory and 20 GB of free disk.

A fresh clone needs more than the commands below, which assume a machine
that has run the lab before:

- **Paths.** The scenes and `digital/assets/manifest.json` name their audio
  files by absolute path under `/root/develop/websdr`. In a checkout
  elsewhere, point them at it:
  `sed -i "s#/root/develop/websdr#$PWD#" bench/scenes/*.scene bench/digital/assets/manifest.json`.
- **Reference audio.** The WAV files are not in the repository.
  `python3 bench/digital/digital.py assets` writes them and
  `scenes/digital.scene`; it needs WSJT-X's `ft8sim` and `wsprsim` and
  `minimodem` on the PATH. The digital report decodes with `jt9`, `wsprd`
  and `minimodem`.
- **Receiver images.** The harness runs the image each
  `receivers/<id>/receiver.json` names and does not build it. Build each
  from its `Dockerfile`; FernSDR's Dockerfile says in its header how its
  build context is made.
- **Real-input loop.** ka9q-web and UberSDR read a 30 s loop of the real
  band's signals from `/dev/shm/fernbench/all-real-30s.s16`:
  `mkdir -p /dev/shm/fernbench && python3 bench/source/scene.py bench/scenes/all-real-signals.scene --format s16 --seconds 30 --out /dev/shm/fernbench/all-real-30s.s16`.
- **Browser.** `npm ci --prefix bench/browser` installs Playwright; its
  Chromium comes with `(cd bench/browser && npx playwright install chromium)`.

```sh
make -C bench/source && make -C bench/source test
npm ci --prefix bench/browser && npm ci --prefix bench/load
# One session per receiver: twelve browser listeners, four at a time.
FB_SESSION_RX_CPUS=4,5 FB_CLIENT_CPUS=0-3 python3 bench/harness/links.py \
    fernsdr novasdr --repeats 3 --slots 1 --parallel 4 --seconds 30 --warmup 10 --out bench/runs/ID
bench/analysis/session.sh bench/runs/ID bench/digital/oracle-130s.json
FB_CLIENT_CPUS=0-3 python3 bench/harness/capacity.py fernsdr novasdr --slots 1 --out bench/runs/CAP
python3 bench/analysis/report.py --round bench/runs/ID --capacity bench/runs/CAP
```

Run one receiver at a time (`--slots 1`): with several at once this
machine's clients and receivers competed for the cores, and the round that
tried it was thrown away. `runs/20260929-final` names the session each
receiver's published figures come from.

Security findings about other projects stay out of this repository until
their maintainers have had them.
