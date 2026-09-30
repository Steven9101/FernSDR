#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Capacity: how many listeners a receiver serves on two cores, and what
each one costs.

Per repetition: the receiver starts fresh (matched tier: listener caps and
per-address limits raised as the method says), one real browser listens
for 25 s so that its traffic can be replayed, then bench/load/replay.mjs
adds listeners in steps. At every step, over a 15 s window after the ramp:
the receiver's CPU (all of its containers) and memory, and for every
listener the audio payload it received against the browser's. A step
passes when at least 95% of listeners got 90% of the browser's audio messages a second
and none lost its sockets; the knee is the last step that passed. Steps
stop after the first failure. A listener limit that refuses connections
shows as a failure with closed sockets, and is reported as a cap.

  python3 bench/harness/capacity.py RECEIVER... --repeats 3 --out bench/runs/ID
"""
import argparse
import json
import os
from pathlib import Path
import random
import threading
import time
import traceback

# The real band's signals as a 30 s loop (scenes/all-real-signals.scene,
# rendered once), with pace's live noise at the scene's -97 dBFS/Hz: at
# 20.48 Msps real that is an RMS of 1481 of 32767.
REAL_LOOP = ("/dev/shm/fernbench/all-real-30s.s16", 1481)
from lab import BENCH, LabSession, Client, ReceiverRun, CgroupSampler, guard, sh

STEPS = [1, 10, 50, 100, 200, 400]


def stream_rate(sockets, t0, t1):
    """Frames per second per stream class, rx, in [t0, t1] ms, and bytes
    per second under "CLASS_bytes"."""
    out = {}
    for s in sockets:
        for k, v in s["streams"].items():
            if not k.endswith(":rx"):
                continue
            span = (t1 - t0) / 1000
            inside = [b for t, b in zip(v["times"], v.get("sizes", [])) if t0 <= t <= t1]
            out[k[:-3]] = out.get(k[:-3], 0) + len(inside) / span
            out[k[:-3] + "_bytes"] = out.get(k[:-3] + "_bytes", 0) + sum(inside) / span
    return out


def run_rep(rid, rep, out, steps, slot=0):
    base = out / rid / f"rep{rep}"
    base.mkdir(parents=True, exist_ok=False)
    record = {"receiver": rid, "repeat": rep, "environment": guard(), "steps": []}
    rx = client = None
    try:
        # 200 source addresses for up to 400 listeners (see replay.mjs).
        client = Client(100 + slot, extra=200)
        # Two receivers at once, each on its own pair of cores, the load
        # clients on the remaining two.
        rx = ReceiverRun(rid, "matched", str(base / "receiver"), "markers", rep, slot=slot,
                         cpus=["4,5", "2,3"][slot % 2])
        if rx.rx["input"]["format"] == "s16":
            rx.scene = os.path.join(BENCH, "scenes", "markers-real.scene")
            rx.loop = REAL_LOOP
        rx.start(client)
        rec = base / "browser"
        r = client.run(["bash", "-c", 'ulimit -f 65536; exec "$@"', "b", "node", os.path.join(BENCH, "browser", "listen.mjs"),
                        os.path.join(BENCH, "receivers", rid, "adapter.mjs"), rx.url, "--seconds", "20", "--warmup", "5",
                        "--freq", "7159200", "--out", str(rec)], timeout=240)
        meta = json.loads((rec / "meta.json").read_text())
        sockets = json.loads((rec / "sockets.json").read_text())
        ref = stream_rate(sockets, meta["measurementStartedWallMs"], meta["endedWallMs"])
        record["browser_rates"] = ref
        record["browser_exit"] = r.returncode
        if not ref.get("audio"):
            raise RuntimeError("the browser got no audio to compare with")
        sampler = CgroupSampler(rx.cgroup_paths())
        for n in steps:
            guard(min_mem_mb=1000)
            ramp = max(1.0, n / 20)
            window_from = ramp + 8
            seconds = window_from + 15
            result = base / f"step-{n}.json"
            rows = []
            lost = []
            stop = threading.Event()

            def sample():
                while not stop.is_set():
                    try:
                        sampler.sample()
                    except RuntimeError as e:
                        # A container's cgroup is gone: the receiver, or one
                        # of its programs, was ended (UberSDR at 400 listeners
                        # by the 1 GB memory limit). The step's result says so.
                        lost.append(str(e))
                        return
                    rows.append(sampler.rows[-1])
                    stop.wait(1)

            th = threading.Thread(target=sample)
            th.start()
            # Past 500 listeners the replay process holds over 1024 sockets, the
            # default limit on open files, and every socket above it failed
            # to connect: WebSDR at 800 and FernSDR at 1600 both stopped at
            # the same 64 %.
            rr = client.run(["prlimit", "--nofile=65536:65536", "--", "node", "--max-old-space-size=1400",
                             os.path.join(BENCH, "load", "replay.mjs"), str(rec),
                             os.path.join(BENCH, "receivers", rid, "adapter.mjs"), rx.url, "--clients", str(n),
                             "--seconds", str(seconds), "--window-from", str(window_from), "--seed", str(rep * 1000 + n),
                             "--sources", f"198.18.{100 + slot}.2+200",
                             "--out", str(result)], memory="1600M", timeout=seconds + 120)
            stop.set()
            th.join()
            if rr.returncode != 0 or not result.exists():
                record["steps"].append({"listeners": n, "error": rr.stderr[-500:]})
                break
            res = json.loads(result.read_text())
            w0, w1 = res["windowStartMs"] / 1000, res["windowEndMs"] / 1000
            win = [x for x in rows if w0 <= x["t"] <= w1]
            cpu = ((win[-1]["cpu_usec"] - win[0]["cpu_usec"]) / 1e6 / (win[-1]["t"] - win[0]["t"]) * 100) if len(win) >= 2 else None
            mem = max((x["mem_bytes"] for x in win), default=0) / 2**20
            span = res["windowSeconds"]
            # Messages, not bytes: each listener is tuned elsewhere in the
            # band, and a variable-rate codec (NAC3, Opus, FLAC) spends fewer
            # bytes on quieter frequencies while delivering every frame.
            ratios = [(c["streams"].get("audio", {}).get("frames", 0) / span) / ref["audio"] for c in res["results"]]
            wf = [(c["streams"].get("waterfall", {}).get("frames", 0) / span) / ref["waterfall"]
                  for c in res["results"]] if ref.get("waterfall") else []
            audio_kbit = [c["streams"].get("audio", {}).get("bytes", 0) * 8 / 1000 / span for c in res["results"]]
            closed = sum(1 for c in res["results"] if c["closedEarly"])
            ok_share = sum(1 for x in ratios if x >= 0.9) / max(1, len(ratios))
            step = {"listeners": n, "cpu_percent": cpu, "mem_mb": mem, "audio_ok_share": ok_share,
                    "audio_ratio_median": sorted(ratios)[len(ratios) // 2] if ratios else None,
                    "waterfall_ratio_median": sorted(wf)[len(wf) // 2] if wf else None,
                    "audio_kbit_median": sorted(audio_kbit)[len(audio_kbit) // 2] if audio_kbit else None,
                    "closed_early": closed, "spread_share": sum(1 for c in res["results"] if c["spread"]) / max(1, n),
                    "errors": sum(c["errors"] for c in res["results"]),
                    "first_errors": sorted({c.get("firstError") or c.get("firstClose") for c in res["results"]} - {None})[:3]}
            if lost:
                step["receiver_lost"] = lost[0]
            step["passed"] = ok_share >= 0.95 and closed == 0 and not lost
            record["steps"].append(step)
            print(f"  {rid} rep{rep} {n}: cpu {cpu and round(cpu)}% ok {ok_share:.2f} closed {closed} "
                  f"spread {step['spread_share']:.2f}", flush=True)
            if not step["passed"]:
                break
        passed = [s["listeners"] for s in record["steps"] if s.get("passed")]
        record["knee"] = max(passed) if passed else 0
        record["state"] = "done"
    except Exception as e:
        record["state"] = "failed"
        record["error"] = str(e)
        (base / "failure.txt").write_text(traceback.format_exc())
    finally:
        if rx:
            rx.stop()
        if client:
            client.close()
        (base / "manifest.json").write_text(json.dumps(record, indent=1))
    return record


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("receivers", nargs="+")
    ap.add_argument("--repeats", type=int, default=1)
    ap.add_argument("--slots", type=int, default=2)
    ap.add_argument("--steps", type=int, default=len(STEPS), help="how many of the standard steps")
    ap.add_argument("--step-list", help="listener counts instead, comma-separated (e.g. 400,800,1600)")
    ap.add_argument("--seed", type=int, default=20260927)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    cells = [(rid, rep) for rep in range(1, args.repeats + 1) for rid in args.receivers]
    random.Random(args.seed).shuffle(cells)
    steps = [int(x) for x in args.step_list.split(",")] if args.step_list else STEPS[:args.steps]
    (args.out / "round.json").write_text(json.dumps({"suite": "capacity", "order": cells, "steps": steps}, indent=1))
    with LabSession():
        todo = list(cells)
        lock = threading.Lock()

        def worker(slot):
            while True:
                with lock:
                    if not todo:
                        return
                    rid, rep = todo.pop(0)
                r = run_rep(rid, rep, args.out, steps, slot)
                print(f"{rid} rep{rep} (slot {slot}): {r['state']} knee {r.get('knee')} {r.get('error', '')}", flush=True)

        threads = [threading.Thread(target=worker, args=(slot,)) for slot in range(args.slots)]
        for t in threads:
            t.start()
            time.sleep(5)
        for t in threads:
            t.join()

if __name__ == "__main__":
    main()
