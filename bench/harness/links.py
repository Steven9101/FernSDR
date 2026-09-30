#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
"""Constrained and stalling links: several listeners at once, each on its
own emulated link, against one receiver.

Every profile shapes one client namespace's veth, both directions, from the
start of the warm-up. Timed impairments (stalls, holds, cell changes,
outages) start with the measurement window and repeat as the profile says;
every change is logged with its wall-clock time, so the analysis knows when
the link was bad. The profiles run in batches of `--parallel` browsers; the
receiver is started fresh for every repetition, one receiver at a time.

  python3 bench/harness/links.py RECEIVER... --repeats 3 --out bench/runs/ID
"""
import argparse
import json
import os
from pathlib import Path
import random
import subprocess
import threading
import time
import traceback

# The real band's signals as a 30 s loop (scenes/all-real-signals.scene,
# rendered once), with pace's live noise at the scene's -97 dBFS/Hz: at
# 20.48 Msps real that is an RMS of 1481 of 32767.
REAL_LOOP = ("/dev/shm/fernbench/all-real-30s.s16", 1481)
# A wider real band (a 0 to 32 MHz RX-888 at 64.8 Msps) names its own
# scene, loop and noise: FB_REAL_SCENE, FB_REAL_LOOP, FB_REAL_NOISE_RMS.
REAL_SCENE = os.environ.get("FB_REAL_SCENE", "all-real.scene")
if os.environ.get("FB_REAL_LOOP"):
    REAL_LOOP = (os.environ["FB_REAL_LOOP"], float(os.environ["FB_REAL_NOISE_RMS"]))
from lab import BENCH, LabSession, Client, ReceiverRun, CgroupSampler, guard, sh

# name: (static downlink/uplink setup, timeline)
PROFILES = {
    # Plain listeners on an unshaped link, each tuned to its part of the
    # combined scene: the latency marker, the digital-mode channel, the tones.
    "listen": {},
    "digital": {"freq": 7074000, "tone": 350},
    "tones": {"freq": 7080000, "tone": 1000},
    "lan": {},
    "rate64": {"down": "tbf rate 64kbit burst 1600 latency 150ms"},
    "rate32": {"down": "tbf rate 32kbit burst 1600 latency 150ms"},
    "rate24": {"down": "tbf rate 24kbit burst 1600 latency 150ms"},
    "bloat": {"down": "tbf rate 64kbit burst 1600 latency 2000ms", "up": "tbf rate 256kbit burst 1600 latency 2000ms"},
    "loss2": {"down": "netem delay 20ms loss 2%", "up": "netem delay 20ms loss 2%"},
    "rtt600": {"down": "netem delay 300ms 15ms", "up": "netem delay 300ms 15ms"},
    "drop1s": {"down": "netem delay 20ms", "up": "netem delay 20ms", "every": 10, "kind": "drop", "length": 1.0},
    "hold1s": {"down": "netem delay 20ms", "up": "netem delay 20ms", "plug": True, "every": 10, "kind": "hold", "length": 1.0},
    "wifi": {"down": "netem delay 5ms", "up": "netem delay 5ms", "plug": True, "kind": "wifi"},
    "cell": {"down": "netem delay 25ms", "up": "netem delay 25ms", "every": 20, "kind": "cell"},
    "out15": {"down": "netem delay 20ms", "up": "netem delay 20ms", "kind": "outage", "at": 5, "length": 15.0},
}


def tc_host(dev, spec):
    sh("tc", "qdisc", "replace", "dev", dev, "root", "handle", "1:", *spec.split())


def tc_ns(ns, spec):
    sh("ip", "netns", "exec", ns, "tc", "qdisc", "replace", "dev", "eth0", "root", "handle", "1:", *spec.split())


def shape(client, profile):
    p = PROFILES[profile]
    if "down" in p:
        tc_host(client.ns, p["down"])
    if "up" in p:
        tc_ns(client.ns, p["up"])
    if p.get("plug"):
        # Behind the delay, a plug that passes everything until told to hold.
        sh("tc", "qdisc", "add", "dev", client.ns, "parent", "1:1", "handle", "10:", "plug", "limit", "10000000")
        sh("tc", "qdisc", "change", "dev", client.ns, "parent", "1:1", "handle", "10:", "plug", "release_indefinite")


def timeline(client, profile, start, seconds, log, stop, rng):
    """Applies the profile's timed impairments from `start` (wall seconds)."""
    p = PROFILES[profile]
    kind = p.get("kind")
    dev = client.ns

    def note(what):
        log.append({"t": time.time(), "what": what})

    def wait_until(t):
        while not stop.is_set() and time.time() < t:
            time.sleep(min(0.01, max(0, t - time.time())))
        return not stop.is_set()

    def drop(on):
        spec = p["down"] + (" loss 100%" if on else "")
        tc_host(dev, spec)
        tc_ns(client.ns, p["up"] + (" loss 100%" if on else ""))

    def plug(block):
        sh("tc", "qdisc", "change", "dev", dev, "parent", "1:1", "handle", "10:", "plug",
           "block" if block else "release_indefinite")

    end = start + seconds
    if kind in ("drop", "hold"):
        t = start + 2
        while t < end - 1 and wait_until(t):
            (drop if kind == "drop" else plug)(True)
            note(f"{kind} on")
            if not wait_until(t + p["length"]):
                break
            (drop if kind == "drop" else plug)(False)
            note(f"{kind} off")
            t += p["every"]
    elif kind == "wifi":
        t = start + rng.expovariate(1 / 3)
        while t < end - 0.5 and wait_until(t):
            hold = rng.uniform(0.05, 0.3)
            plug(True)
            note(f"hold on {hold:.3f}")
            wait_until(t + hold)
            plug(False)
            note("hold off")
            t += hold + rng.expovariate(1 / 3)
    elif kind == "cell":
        t = start + 3
        while t < end - 10 and wait_until(t):
            drop(True)
            note("cell loss on")
            wait_until(t + 1.5)
            tc_host(dev, "netem delay 125ms rate 500kbit")
            tc_ns(client.ns, "netem delay 125ms rate 500kbit")
            note("cell new: rtt 250 ms, 500 kbit/s")
            wait_until(t + 9.5)
            tc_host(dev, p["down"])
            tc_ns(client.ns, p["up"])
            note("cell back")
            t += p["every"]
    elif kind == "outage":
        if wait_until(start + p["at"]):
            drop(True)
            note("outage on")
            wait_until(start + p["at"] + p["length"])
            drop(False)
            note("outage off")


def run_rep(rid, rep, profiles, parallel, out, seconds, warmup, slot=0):
    base = out / rid / f"rep{rep}"
    base.mkdir(parents=True, exist_ok=False)
    rx = None
    clients = []
    record = {"receiver": rid, "repeat": rep, "environment": guard(), "batches": []}
    try:
        ready = Client(slot * 20 + 1)
        clients.append(ready)
        # Sessions measure what listeners get, not the receiver's CPU, so
        # receivers in parallel slots share the cores.
        rx = ReceiverRun(rid, "documented", str(base / "receiver"), "all", rep, slot=slot,
                         cpus=os.environ.get("FB_SESSION_RX_CPUS", os.environ.get("FB_CLIENT_CPUS", "0-5")))
        if rx.rx["input"]["format"] == "s16":
            rx.scene = os.path.join(BENCH, "scenes", REAL_SCENE)
            rx.loop = REAL_LOOP
        rx.start(ready)
        ready.close()
        clients.clear()
        sampler = CgroupSampler(rx.cgroup_paths())
        # Idle is measured with input flowing: radiod (ka9q-web) opens its
        # FIFO some seconds after the container starts, and CPU before that
        # would read as an idle receiver.
        pace_log = base / "receiver" / "pace.jsonl"
        for _ in range(120):
            try:
                if any('"ev":"sec"' in l and '"written":0,' not in l for l in pace_log.read_text().splitlines()):
                    break
            except OSError:
                pass
            time.sleep(0.5)
        phase = {"name": "idle"}
        sampling = threading.Event()

        def sample_all():
            # The receiver's CPU and memory every second, tagged with what
            # was going on: idle (input running, no listener) or a batch.
            while not sampling.is_set():
                try:
                    sampler.sample()
                except RuntimeError as e:
                    # A container's cgroup is gone: a program in the receiver
                    # exited (ka9q-web segfaulted this way). That is a result,
                    # recorded in the manifest, not a harness failure.
                    record["receiver_lost"] = {"t": time.time(), "phase": phase["name"], "error": str(e)}
                    return
                sampler.rows[-1]["phase"] = phase["name"]
                sampling.wait(1)

        sample_thread = threading.Thread(target=sample_all, daemon=True)
        sample_thread.start()
        time.sleep(12)
        for b in range(0, len(profiles), parallel):
            phase["name"] = f"batch{b // parallel}:{'+'.join(profiles[b:b + parallel])}"
            batch = profiles[b:b + parallel]
            guard(min_mem_mb=1000)
            cs = [Client(slot * 20 + i + 2) for i in range(len(batch))]
            clients[:] = cs
            # Links are shaped just before the window opens, once the page has loaded
            # and tuned: a page of a few hundred kilobytes takes minutes at
            # 24 kbit/s, and what a listener on such a link then gets is what
            # this measures, not how long the page took.
            results = {}
            logs = {prof: [] for prof in batch}
            stop = threading.Event()
            start_at = time.time() + 20 + warmup  # page open and tuning take a while

            # The other listeners use a small window to spare the client
            # cores; a page that turns into another layout there (UberSDR's
            # compact one, OpenWebRX+'s panel under the scale) states the
            # smallest window its adapter drives in receiver.json.
            small = tuple(rx.rx.get("small_viewport", [800, 600]))

            def listen(c, prof):
                d = base / prof
                r = c.run(["bash", "-c", 'ulimit -f 65536; exec "$@"', "bench-browser", "node",
                           os.path.join(BENCH, "browser", "listen.mjs"),
                           os.path.join(BENCH, "receivers", rid, "adapter.mjs"), rx.url, "--seconds", str(seconds),
                           "--warmup", str(warmup), "--freq", str(PROFILES[prof].get("freq", 7159200)),
                           "--tone", str(PROFILES[prof].get("tone", 800)), "--out", str(d),
                           "--barrier", str(d / "go")] +
                          ([] if prof == "listen" else ["--width", str(small[0]), "--height", str(small[1])]),
                          timeout=seconds + warmup + 240)
                (d / "browser.stderr.txt").write_text(r.stderr[-20000:])
                results[prof] = r.returncode

            threads = [threading.Thread(target=listen, args=(c, prof)) for c, prof in zip(cs, batch)]
            for t in threads:
                t.start()
            # Each browser writes ready.json once warmed up and then waits for
            # its go file: every link is shaped before any window opens, so
            # no window has an unshaped stretch. The timelines start at each
            # page's own window, which it writes to window.json.
            deadline = time.time() + 120
            ready = set()
            while time.time() < deadline and len(ready) < len(batch):
                ready |= {prof for prof in batch if (base / prof / "ready.json").exists()}
                time.sleep(0.2)
            rng = random.Random(rep * 1000 + b)
            for c, prof in zip(cs, batch):
                shape(c, prof)
                logs[prof].append({"t": time.time(), "what": "shaped"})
            for prof in batch:
                (base / prof).mkdir(parents=True, exist_ok=True)
                (base / prof / "go").write_text("")
            deadline = time.time() + 30
            starts = {}
            while time.time() < deadline and len(starts) < len(batch):
                for prof in batch:
                    m = base / prof / "window.json"
                    if m.exists():
                        starts[prof] = json.loads(m.read_text())["startedWallMs"] / 1000
                time.sleep(0.1)
            tl = [threading.Thread(target=timeline, args=(c, prof, starts.get(prof, start_at), seconds, logs[prof], stop, rng))
                  for c, prof in zip(cs, batch)]
            for t in tl:
                t.start()
            for t in threads:
                t.join()
            stop.set()
            for t in tl:
                t.join()
            for prof in batch:
                (base / prof).mkdir(parents=True, exist_ok=True)
                (base / prof / "link.json").write_text(json.dumps({"profile": prof, "spec": PROFILES[prof],
                                                                  "events": logs[prof], "exit": results.get(prof)}))
            for c in cs:
                c.close()
            clients.clear()
            pass  # the sampling thread records every second
            record["batches"].append({"profiles": batch, "exit": results})
        sampling.set()
        sample_thread.join(timeout=5)
        (base / "resources.json").write_text(json.dumps(sampler.rows))
        record["state"] = "done"
    except Exception as e:
        record["state"] = "failed"
        record["error"] = str(e)
        (base / "failure.txt").write_text(traceback.format_exc())
    finally:
        for c in clients:
            c.close()
        if rx:
            rx.stop()
        (base / "manifest.json").write_text(json.dumps(record, indent=1))
    return record


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("receivers", nargs="+")
    ap.add_argument("--repeats", type=int, default=3)
    ap.add_argument("--parallel", type=int, default=4)
    ap.add_argument("--slots", type=int, default=1, help="receivers measured at once")
    ap.add_argument("--seconds", type=int, default=60)
    ap.add_argument("--warmup", type=int, default=15)
    ap.add_argument("--profiles", nargs="+", default=["listen", "digital", "tones", "rate32", "rate24", "loss2", "rtt600", "hold1s", "drop1s", "cell", "out15", "wifi"])
    ap.add_argument("--seed", type=int, default=20260927)
    ap.add_argument("--out", type=Path, required=True)
    args = ap.parse_args()
    args.out = args.out.resolve()
    args.out.mkdir(parents=True, exist_ok=False)
    cells = [(rid, rep) for rep in range(1, args.repeats + 1) for rid in args.receivers]
    random.Random(args.seed).shuffle(cells)
    (args.out / "round.json").write_text(json.dumps({"suite": "session", "order": cells, "profiles": args.profiles,
                                                     "parallel": args.parallel, "seconds": args.seconds}, indent=1))
    with LabSession():
        # Receivers run in parallel slots; each takes the next cell.
        todo = list(cells)
        lock = threading.Lock()

        def worker(slot):
            while True:
                with lock:
                    if not todo:
                        return
                    rid, rep = todo.pop(0)
                r = run_rep(rid, rep, args.profiles, args.parallel, args.out, args.seconds, args.warmup, slot)
                print(f"{rid} rep{rep} (slot {slot}): {r['state']} {r.get('error', '')}", flush=True)

        threads = [threading.Thread(target=worker, args=(slot,)) for slot in range(args.slots)]
        for t in threads:
            t.start()
            time.sleep(5)
        for t in threads:
            t.join()

if __name__ == "__main__":
    main()
