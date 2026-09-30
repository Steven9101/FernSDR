#!/usr/bin/env python3
"""Run isolated receiver setup gates and retain the evidence, including failures."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import random
import signal
import subprocess
import threading
import time
import traceback

from lab import BENCH, LabSession, Client, ReceiverRun, CgroupSampler, guard, sh


def write_json(path, value):
    path = Path(path)
    temporary = path.with_suffix(path.suffix + ".tmp")
    temporary.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n")
    temporary.replace(path)


def hashes():
    root = Path(BENCH)
    return {str(p.relative_to(root)): hashlib.sha256(p.read_bytes()).hexdigest()
            for p in sorted(root.rglob("*")) if p.is_file()
            and not set(p.relative_to(root).parts) & {"node_modules", "build", "runs", "captures", "__pycache__"}}


def run_cell(rid, tier, repeat, out, seconds, suite="smoke"):
    directory = out / rid / tier / str(repeat)
    directory.mkdir(parents=True, exist_ok=False)
    evidence = {"receiver": rid, "tier": tier, "repeat": repeat, "suite": suite,
                "state": "running", "started": time.time(), "environment": guard()}
    write_json(directory / "manifest.json", evidence)
    client, rx, sampler = None, None, None
    done = threading.Event()
    thread = None
    errors = []
    try:
        client = Client(1)
        scene = {"smoke": "smoke", "digital": "digital", "tones": "tones"}.get(suite, "markers")
        rx = ReceiverRun(rid, tier, str(directory), scene, repeat)
        if suite != "smoke" and rx.rx["input"]["format"] == "s16":
            # Real-input receivers (ka9q-web, UberSDR) get the same signals as a
            # 20.48 Msps real band: a separate input cohort, said so in the report.
            rx.scene = os.path.join(BENCH, "scenes", f"{scene}-real.scene")
        if suite == "smoke" and rx.rx.get("smoke_scene"):
            rx.scene = os.path.join(BENCH, "scenes", rx.rx["smoke_scene"] + ".scene")
        evidence["receiver_manifest"] = rx.rx
        evidence["image"] = json.loads(sh("docker", "image", "inspect", rx.rx["image"]).stdout)[0]["Id"]
        rx.start(client)
        evidence["isolation"] = sh(os.path.join(BENCH, "net/lab-net.sh"), "check").stdout
        sampler = CgroupSampler(rx.cgroup_paths())

        def monitor():
            while not done.is_set():
                try:
                    guard(min_mem_mb=1000)
                    sampler.sample()
                    sampler.rows[-1]["veth"] = {k: int(open(f"/sys/class/net/{client.ns}/statistics/{k}").read())
                                                for k in ("rx_bytes", "tx_bytes")}
                    if rx.feed.poll() is not None:
                        raise RuntimeError("source exited while measuring")
                except Exception as error:
                    errors.append(str(error))
                    # Ending the common lab slice terminates only this locked
                    # session's receivers and clients, including hung children.
                    sh("systemctl", "stop", "fernbench.slice", check=False)
                    break
                done.wait(1)

        thread = threading.Thread(target=monitor, daemon=True)
        thread.start()
        result = client.run(["flock", "-w", "10", "/tmp/fernbench-browser.lock", "bash", "-c",
                             'ulimit -f 65536; exec "$@"', "bench-browser", "node",
                             os.path.join(BENCH, "browser", "smoke.mjs" if suite == "smoke" else "listen.mjs"),
                             os.path.join(BENCH, "receivers", rid, "adapter.mjs"), rx.url,
                             "--seconds", str(seconds),
                             "--freq", {"smoke": "7159000", "digital": "7074000", "tones": "7080000"}.get(suite, "7159200"),
                             "--out", str(directory)] + (["--warmup", "15"] if suite in ("listen", "digital", "tones") else []) +
                            (["--tone", "350"] if suite == "digital" else ["--tone", "1000"] if suite == "tones" else []),
                            timeout=seconds + 150)
        (directory / "browser.stderr.txt").write_text(result.stderr)
        (directory / "browser.stdout.json").write_text(result.stdout)
        evidence["exit_code"] = result.returncode
        if errors:
            evidence["lab_failed"] = True
            raise RuntimeError("; ".join(errors))
        report = json.loads(result.stdout) if suite == "smoke" else json.loads((directory / "meta.json").read_text())
        valid = report.get("ok") if suite == "smoke" else all(report.get("gates", {}).values())
        evidence["state"] = "validated" if result.returncode == 0 and valid else "setup_failed"
    except KeyboardInterrupt:
        evidence["state"] = "interrupted"
        raise
    except Exception as error:
        if errors:
            evidence["lab_failed"] = True
        evidence["state"] = "setup_failed"
        evidence["error"] = str(error)
        if isinstance(error, subprocess.CalledProcessError):
            evidence["command_stderr"] = error.stderr
        (directory / "failure.txt").write_text(traceback.format_exc())
    finally:
        done.set()
        if thread:
            thread.join(timeout=35)
            if thread.is_alive():
                errors.append("monitor did not stop")
        cleanup_errors = []
        if errors:
            evidence["lab_failed"] = True
            evidence["state"] = "setup_failed"
            evidence["monitor_errors"] = list(errors)
        for cleanup in (
            lambda: write_json(directory / "resources.json", sampler.rows) if sampler else None,
            lambda: rx.stop() if rx else None,
            lambda: client.close() if client else None,
        ):
            try:
                cleanup()
            except Exception as error:
                cleanup_errors.append(str(error))
        if cleanup_errors:
            evidence["cleanup_errors"] = cleanup_errors
            evidence["lab_failed"] = True
            evidence["state"] = "setup_failed"
        evidence["ended"] = time.time()
        write_json(directory / "manifest.json", evidence)
    return evidence


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("receivers", nargs="+")
    parser.add_argument("--tiers", nargs="+", choices=["documented", "matched"], default=["documented", "matched"])
    parser.add_argument("--repeats", type=int, default=1)
    parser.add_argument("--seconds", type=int, default=15)
    parser.add_argument("--seed", type=int, default=20260927)
    parser.add_argument("--suite", choices=["smoke", "capture-check", "listen", "digital", "tones"], default="smoke")
    parser.add_argument("--out", type=Path, required=True)
    parser.add_argument("--resume", action="store_true", help="continue a round in --out")
    args = parser.parse_args()
    if not 1 <= args.repeats <= 10 or not 10 <= args.seconds <= 200:
        parser.error("repeats must be 1..10 and seconds 10..180")
    args.out = args.out.resolve()
    if args.resume:
        # Same round, same order and files: valid cells stay, any other cell
        # is kept beside as rejected and run again.
        cells = [tuple(c) for c in json.loads((args.out / "round.json").read_text())["order"]]
        todo = []
        for rid, tier, repeat in cells:
            d = args.out / rid / tier / str(repeat)
            if d.exists():
                state = json.loads((d / "manifest.json").read_text()).get("state") if (d / "manifest.json").exists() else None
                if state == "validated":
                    continue
                k = 1
                while d.with_name(f"{repeat}.rejected-{k}").exists():
                    k += 1
                d.rename(d.with_name(f"{repeat}.rejected-{k}"))
            todo.append((rid, tier, repeat))
        cells = todo
    else:
        args.out.mkdir(parents=True, exist_ok=False)
        cells = [(rid, tier, i) for i in range(1, args.repeats + 1) for tier in args.tiers for rid in args.receivers]
        random.Random(args.seed).shuffle(cells)
        write_json(args.out / "round.json", {"suite": args.suite, "comparative": args.suite in ("listen", "digital", "tones"),
                   "host": platform.uname()._asdict(), "created": time.time(), "order": cells,
                   "seed": args.seed, "files": hashes()})
    def interrupted(signum, frame):
        raise KeyboardInterrupt(f"signal {signum}")
    signal.signal(signal.SIGTERM, interrupted)
    with LabSession():
        results = []
        for rid, tier, repeat in cells:
            result = run_cell(rid, tier, repeat, args.out, args.seconds, args.suite)
            results.append(result)
            write_json(args.out / "summary.json", results)
            print(f"{rid} {tier} {repeat}: {result['state']}", flush=True)
            if result.get("lab_failed"):
                print("Lab guard stopped the round; remaining cells were not attempted.", flush=True)
                break
    return 0 if all(r["state"] == "validated" for r in results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
