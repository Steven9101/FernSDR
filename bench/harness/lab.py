# SPDX-License-Identifier: AGPL-3.0-or-later
"""Runs one receiver in the lab: isolated, fed by the scene, measured.

A run is: a network holder container with no network but the lab's veth,
the scene rendered and paced into the receiver (a FIFO it reads, or an
rtl_tcp server inside its namespace), the receiver's container on the
holder's network with a read-only root, capped memory, two cores at low
CPU weight and capped logs, and then clients in their own namespaces.
Everything the run starts it stops again, by container name or systemd
unit, never by pattern.

The containers are named fbh-<id>, not fb-<id>: the names people use by
hand while building a receiver stay theirs.
"""
import contextlib
import fcntl
import json
import os
import re
import shlex
import shutil
import subprocess
import tempfile
import threading
import time
import uuid

BENCH = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
RECEIVERS = os.path.join(BENCH, "receivers")
SCENES = os.path.join(BENCH, "scenes")
SOURCE = os.path.join(BENCH, "source")
LAB_NET = os.path.join(BENCH, "net", "lab-net.sh")
RX_ADDR = "198.18.0.2"
RX_CPUS = os.environ.get("FB_RX_CPUS", "4,5")
CLIENT_CPUS = os.environ.get("FB_CLIENT_CPUS", "0-3")
SLICE = "fernbench.slice"


def sh(*args, check=True, **kw):
    kw.setdefault("timeout", 30)
    return subprocess.run(list(args), check=check, text=True, capture_output=True, **kw)


class LabSession:
    """Owns the shared bridge and aggregate budget under an exclusive lock.

    A leftover bridge is evidence of another or interrupted session; never
    delete it just because its name matches. The caller must investigate it.
    """

    def __init__(self, trace_dir=None):
        self.trace_dir = os.path.abspath(trace_dir) if trace_dir else None

    def __enter__(self):
        self.lock = open("/tmp/fernbench-run.lock", "a+")
        try:
            fcntl.flock(self.lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            guard()
            if sh("ip", "link", "show", "br-fb", check=False).returncode == 0:
                raise RuntimeError("br-fb already exists; refusing to take ownership")
            if any(re.match(r"fbc[0-9]+\b", line) for line in sh("ip", "netns", "list").stdout.splitlines()):
                raise RuntimeError("lab client namespaces already exist")
            if sh("nft", "list", "table", "inet", "fernbench", check=False).returncode == 0:
                raise RuntimeError("lab firewall table already exists")
            if sh("systemctl", "show", SLICE, "-p", "ActiveState", "--value").stdout.strip() == "active":
                raise RuntimeError("fernbench.slice is already active")
            sh("systemctl", "set-property", "--runtime", SLICE,
               "MemoryMax=7000M", "MemorySwapMax=0", "CPUWeight=25")
            sh("systemctl", "start", SLICE)
            self.slice_owned = True
            self.network_owned = True
            sh(LAB_NET, "up")
            self.pulse_dir = tempfile.mkdtemp(prefix="fernbench-pulse-", dir="/run")
            self.pulse_unit = "fbh-pulse-" + uuid.uuid4().hex[:8]
            self.old_pulse = os.environ.get("FB_PULSE_SERVER")
            self.pulse_log = open(os.path.join(self.pulse_dir, "pulse.log"), "w")
            pulse = ["pulseaudio", "-n", "--daemonize=no", "--use-pid-file=no", "--exit-idle-time=-1",
                     "--disallow-exit=yes", "--log-level=error", "--log-target=stderr",
                     "-L", "module-null-sink sink_name=fb rate=48000 channels=2 format=float32le",
                     "-L", f"module-native-protocol-unix socket={self.pulse_dir}/native auth-anonymous=1"]
            if self.trace_dir:
                os.makedirs(self.trace_dir, exist_ok=True)
                pulse = ["env", "LD_PRELOAD=" + os.path.join(BENCH, "calibration/build/sink_trace.so"),
                         "FB_PULSE_TRACE_PREFIX=" + os.path.join(self.trace_dir, "sink"), *pulse]
            self.pulse = subprocess.Popen(
                ["systemd-run", "--scope", "--quiet", "--collect", f"--unit={self.pulse_unit}", f"--slice={SLICE}",
                 "-p", "MemoryMax=64M", "-p", "TimeoutStopSec=5s", "-p", f"AllowedCPUs={CLIENT_CPUS}",
                 "bash", "-c", 'ulimit -f 65536; exec "$@"', "pulse", *pulse],
                env={**os.environ, "XDG_RUNTIME_DIR": self.pulse_dir, "PULSE_RUNTIME_PATH": self.pulse_dir,
                     "DBUS_SESSION_BUS_ADDRESS": "disabled:"}, stdout=self.pulse_log, stderr=subprocess.STDOUT)
            deadline = time.monotonic() + 5
            while not os.path.exists(os.path.join(self.pulse_dir, "native")):
                if self.pulse.poll() is not None or time.monotonic() > deadline:
                    self.pulse_log.flush()
                    detail = open(os.path.join(self.pulse_dir, "pulse.log")).read()[-2000:]
                    raise RuntimeError("owned PulseAudio failed to start: " + detail)
                time.sleep(.1)
            os.environ["FB_PULSE_SERVER"] = f"unix:{self.pulse_dir}/native"
            return self
        except BaseException as error:
            self.__exit__(type(error), error, error.__traceback__)
            raise

    def __exit__(self, exc_type, error, traceback):
        # Startup can fail between allocating a resource and creating the next
        # one. Every cleanup must run even if an earlier stop or log copy fails.
        failures = []

        def attempt(label, action):
            try:
                action()
            except Exception as cleanup_error:
                failures.append(f"{label}: {cleanup_error}")

        if getattr(self, "pulse_unit", None):
            attempt("pulse scope", lambda: sh("systemctl", "stop", self.pulse_unit + ".scope", check=False))
        if getattr(self, "network_owned", False):
            attempt("network", lambda: sh(LAB_NET, "down"))
        if getattr(self, "slice_owned", False):
            attempt("slice", lambda: sh("systemctl", "stop", SLICE))
        if getattr(self, "pulse", None) is not None:
            attempt("pulse process", lambda: self.pulse.wait(timeout=10))
        if getattr(self, "pulse_log", None) is not None:
            attempt("pulse log", self.pulse_log.close)
            if self.trace_dir:
                attempt("preserve pulse log", lambda: shutil.copyfile(
                    os.path.join(self.pulse_dir, "pulse.log"), os.path.join(self.trace_dir, "pulse.log")))
        if hasattr(self, "old_pulse"):
            if self.old_pulse is None:
                os.environ.pop("FB_PULSE_SERVER", None)
            else:
                os.environ["FB_PULSE_SERVER"] = self.old_pulse
        if getattr(self, "pulse_dir", None):
            attempt("pulse directory", lambda: shutil.rmtree(self.pulse_dir))
        if getattr(self, "lock", None) is not None:
            attempt("lab lock", self.lock.close)
        if failures:
            detail = "lab cleanup failed: " + "; ".join(failures)
            if error is not None:
                error.add_note(detail)
            else:
                raise RuntimeError(detail)


def process_cgroup(pid):
    with open(f"/proc/{pid}/cgroup") as f:
        for line in f:
            if line.startswith("0::"):
                return "/sys/fs/cgroup" + line.strip().split("::", 1)[1]
    raise RuntimeError(f"PID {pid} has no cgroup v2 membership")


def guard(min_mem_mb=1500, min_disk_gb=20):
    """Refuses to start a run the machine cannot afford: it has no swap and
    other people's services."""
    with open("/proc/meminfo") as f:
        mem = {line.split(":")[0]: int(line.split()[1]) for line in f}
    avail = mem["MemAvailable"] // 1024
    disk = shutil.disk_usage("/").free / 1e9
    if avail < min_mem_mb:
        raise RuntimeError(f"only {avail} MB of memory available")
    if disk < min_disk_gb:
        raise RuntimeError(f"only {disk:.1f} GB of disk free")
    return {"mem_available_mb": avail, "disk_free_gb": round(disk, 1)}


def load_receiver(rid):
    if not re.fullmatch(r"[a-z0-9][a-z0-9-]*", rid):
        raise ValueError("invalid receiver id")
    with open(os.path.join(RECEIVERS, rid, "receiver.json")) as f:
        rx = json.load(f)
    if rx.get("id") != rid or rx.get("input", {}).get("transport") not in ("fifo", "rtltcp"):
        raise ValueError(f"invalid receiver manifest: {rid}")
    return rx


class Client:
    """A client namespace, fbcN, as 198.18.N.1 (plus EXTRA addresses)."""

    def __init__(self, n, extra=0):
        self.n = n
        self.ns = f"fbc{n}"
        self.owned = False
        if sh("ip", "netns", "exec", self.ns, "true", check=False).returncode == 0:
            raise RuntimeError(f"namespace {self.ns} already exists")
        self.owned = True
        try:
            sh(LAB_NET, "client", str(n), str(extra))
        except BaseException:
            self.close()
            raise

    def run(self, argv, unit_prefix="fbh-client", memory="900M", timeout=300, cwd=None):
        """Runs argv inside the namespace, in a scope of its own (so that its
        CPU and memory are counted and capped), and waits for it."""
        unit = f"{unit_prefix}-{self.n}-{uuid.uuid4().hex[:8]}"
        cmd = ["systemd-run", "--scope", "--quiet", "--collect", f"--unit={unit}", f"--slice={SLICE}",
               "-p", f"AllowedCPUs={CLIENT_CPUS}", "-p", "CPUWeight=25", "-p", f"MemoryMax={memory}", "-p", "TimeoutStopSec=5s",
               "ip", "netns", "exec", self.ns] + argv
        try:
            return subprocess.run(cmd, text=True, capture_output=True, timeout=timeout, cwd=cwd)
        finally:
            sh("systemctl", "stop", f"{unit}.scope", check=False)

    def curl(self, url, timeout=3):
        r = subprocess.run(["ip", "netns", "exec", self.ns, "curl", "-s", "-o", "/dev/null", "-w", "%{http_code}",
                            "--max-time", str(timeout), url], text=True, capture_output=True)
        return r.stdout.strip()

    def close(self):
        if self.owned:
            sh("ip", "link", "del", self.ns, check=False)
            sh("ip", "netns", "del", self.ns, check=False)
            self.owned = False


class ReceiverRun:
    """One receiver, started fresh for one run, stopped at the end."""

    def __init__(self, rid, tier, run_dir, scene, seed, ppm=0.0, memory=None, slot=0, cpus=None):
        self.rx = load_receiver(rid)
        # Several receivers can run at once, each in its own slot: its own
        # address on the lab bridge and, for resource measurements, cores.
        self.slot = slot
        self.addr = f"198.18.0.{2 + slot}"
        self.cpus = cpus or RX_CPUS
        self.rid = rid
        self.tier = tier
        self.run_dir = os.path.abspath(run_dir)
        self.scene = scene if os.path.isabs(scene) else os.path.join(SCENES, scene + ".scene")
        self.seed = seed
        self.ppm = ppm
        self.memory = memory or self.rx.get("memory", "1g")
        self.name = f"fbh-{rid}"
        self.net = f"fbh-{rid}-net"
        self.feed_unit = f"fbh-feed-{rid}-{uuid.uuid4().hex[:6]}"
        self.feed = None
        self.started = False
        self.owned_containers = []
        self.owner = uuid.uuid4().hex
        self.feed_log = None

    @property
    def url(self):
        return f"http://{self.addr}:{self.rx['http_port']}/"

    def _scene_rate(self):
        out = sh("python3", os.path.join(SOURCE, "scene.py"), self.scene, "--describe").stdout
        return json.loads(out)

    def start(self, ready_client, ready_timeout=180):
        guard()
        if self.tier not in ("documented", "matched"):
            raise ValueError("tier must be documented or matched")
        os.makedirs(self.run_dir, exist_ok=True)
        for name in (self.name, self.net):
            if sh("docker", "inspect", name, check=False).returncode == 0:
                raise RuntimeError(f"container {name} already exists")
        # The namespace first, so that an rtl_tcp server can be in it before
        # the receiver looks for one, and so that a receiver restarted in a
        # test keeps its address.
        self.owned_containers.append(self.net)
        sh("docker", "run", "-d", "--name", self.net, "--label", f"fernbench.owner={self.owner}",
           "--cgroup-parent", SLICE, "--network", "none", "--memory", "32m", "--memory-swap", "32m",
           "--log-driver", "local", "--log-opt", "max-size=1m", "--log-opt", "max-file=2",
           "ubuntu:24.04", "sleep", "infinity")
        pid = sh("docker", "inspect", "-f", "{{.State.Pid}}", self.net).stdout.strip()
        sh(LAB_NET, "receiver", pid, str(self.slot))

        config = os.path.join(self.run_dir, "config")
        shutil.rmtree(config, ignore_errors=True)
        shutil.copytree(os.path.join(RECEIVERS, self.rid, "config", self.tier), config)

        desc = self._scene_rate()
        self.scene_desc = desc
        fmt = self.rx["input"]["format"]
        pace_log = os.path.join(self.run_dir, "pace.jsonl")
        scene_cmd = shlex.join(["python3", os.path.join(SOURCE, "scene.py"), self.scene,
                              "--seed", str(self.seed), "--format", fmt,
                              "--evidence", os.path.join(self.run_dir, "source.json")])
        pace_args = [os.path.join(SOURCE, "build/pace"), "--rate", str(desc['rate']),
                     "--ppm", str(self.ppm), "--format", fmt, "--log", pace_log]
        if getattr(self, "loop", None):
            # A pre-rendered loop of the signals, repeated by pace with fresh
            # noise added live: for real bands rendered too slowly to keep up.
            loop_file, noise_rms = self.loop
            scene_cmd = None
            pace_args += ["--loop", loop_file, "--noise-rms", str(noise_rms), "--seed", str(self.seed)]
        fifo = os.path.join(self.run_dir, "iq")
        if self.rx["input"]["transport"] == "fifo":
            if os.path.exists(fifo):
                os.remove(fifo)
            os.mkfifo(fifo, 0o600)
            uid = self.rx["input"].get("fifo_reader_uid")
            if uid is not None:
                os.chown(fifo, uid, -1)
            pace_cmd = shlex.join(pace_args + ['--out', 'fifo:' + fifo])
            pipeline = f"{scene_cmd} | {pace_cmd}" if scene_cmd else pace_cmd
        else:
            pace_cmd = shlex.join(['nsenter', '-t', pid, '-n'] + pace_args + ['--out', 'rtltcp:127.0.0.1:1234'])
            pipeline = f"{scene_cmd} | {pace_cmd}" if scene_cmd else pace_cmd
        # Files the renderer or pace might write are capped; the log caps
        # itself. The scope makes the feeder one unit to stop.
        self.feed_log = open(os.path.join(self.run_dir, "feed.log"), "w")
        self.feed = subprocess.Popen(
            ["systemd-run", "--scope", "--quiet", "--collect", f"--unit={self.feed_unit}", f"--slice={SLICE}",
             "-p", "CPUWeight=100", "-p", "MemoryMax=3072M", "-p", f"AllowedCPUs={CLIENT_CPUS}", "-p", "TimeoutStopSec=5s",
             "bash", "-o", "pipefail", "-c", f"ulimit -f 65536; {pipeline}"],
            stdout=self.feed_log, stderr=subprocess.STDOUT)

        specifications = [dict(item, name=self.name + '-' + item['id']) for item in self.rx.get('sidecars', [])]
        specifications.append({'name': self.name, 'image': self.rx['image'], 'run': self.rx['run'], 'memory': self.memory})
        for specification in specifications:
            run = specification['run']
            name = specification['name']
            memory = specification.get('memory', '512m')
            if sh('docker', 'inspect', name, check=False).returncode == 0:
                raise RuntimeError(f'container {name} already exists')
            args = ["docker", "run", "-d", "--name", name, "--network", f"container:{self.net}", "--read-only",
                    "--label", f"fernbench.owner={self.owner}", "--cgroup-parent", SLICE, "--oom-score-adj", "1000",
                    "--memory", memory, "--memory-swap", memory, "--cpuset-cpus", self.cpus, "--cpu-shares", "256",
                    "--log-driver", "local", "--log-opt", "max-size=10m", "--log-opt", "max-file=2"]
            for path in run.get("writable", []):
                if path in run.get("writable_exec", []):
                    continue
                args += ["--tmpfs", f"{path}:rw,size=256m"]
            for path in run.get("writable_exec", []):
                args += ["--tmpfs", f"{path}:rw,exec,size=64m"]
            for vol in run.get("volumes", []):
                args += ["-v", vol.replace("{FIFO}", fifo).replace("{CONFIG}", config).replace("{DATA}", self.run_dir)]
            for k, v in run.get("env", {}).items():
                args += ["-e", f"{k}={v}"]
            if run.get("entrypoint"):
                args += ["--entrypoint", run["entrypoint"]]
            args.append(specification["image"])
            args += run.get("command", [])
            self.owned_containers.append(name)
            sh(*args)
        self.started = True

        deadline = time.monotonic() + ready_timeout
        path = self.rx.get("ready", {}).get("http", "/")
        while time.monotonic() < deadline:
            guard(min_mem_mb=1000)
            if self.feed.poll() is not None:
                raise RuntimeError(f"{self.rid} source exited: {self.feed.returncode}")
            if ready_client.curl(f"http://{self.addr}:{self.rx['http_port']}{path}") == "200":
                return
            for name in self.owned_containers:
                state = sh("docker", "inspect", "-f", "{{.State.Status}}", name, check=False).stdout.strip()
                if state not in ("running", "created"):
                    raise RuntimeError(f"{name} stopped while starting: {state}")
            time.sleep(1)
        raise RuntimeError(f"{self.rid} not ready within {ready_timeout} s")

    def cgroup_paths(self):
        paths = []
        for name in self.owned_containers:
            if name == self.net:
                continue
            pid = sh("docker", "inspect", "-f", "{{.State.Pid}}", name).stdout.strip()
            paths.append(process_cgroup(int(pid)))
        return paths

    def stop(self):
        failures = []
        def attempt(action):
            try:
                action()
            except Exception as error:
                failures.append(str(error))
        def save_logs():
            for name in self.owned_containers:
                if name == self.net:
                    continue
                owner = sh("docker", "inspect", "-f", '{{index .Config.Labels "fernbench.owner"}}', name, check=False)
                if owner.returncode or owner.stdout.strip() != self.owner:
                    continue
                logs = sh("docker", "logs", "--tail", "400", name, check=False)
                filename = "receiver.log" if name == self.name else name + ".log"
                with open(os.path.join(self.run_dir, filename), "w") as f:
                    f.write(logs.stdout + logs.stderr)
                state = sh("docker", "inspect", "-f", "{{json .State}}", name, check=False)
                with open(os.path.join(self.run_dir, filename + ".state.json"), "w") as f:
                    f.write(state.stdout)
        attempt(save_logs)
        attempt(lambda: sh("systemctl", "stop", f"{self.feed_unit}.scope", check=False))
        if self.feed:
            with contextlib.suppress(Exception):
                self.feed.wait(timeout=10)
        if self.feed_log:
            self.feed_log.close()
        for name in reversed(self.owned_containers):
            def remove_owned(name=name):
                owner = sh("docker", "inspect", "-f", '{{index .Config.Labels "fernbench.owner"}}', name, check=False)
                if owner.returncode == 0 and owner.stdout.strip() == self.owner:
                    sh("docker", "rm", "-f", name)
            attempt(remove_owned)
        self.owned_containers.clear()
        fifo = os.path.join(self.run_dir, "iq")
        if os.path.exists(fifo):
            attempt(lambda: os.remove(fifo))
        if failures:
            raise RuntimeError("cleanup errors: " + "; ".join(failures))


class CgroupSampler:
    """CPU seconds and memory of the receiver's containers, once a second,
    and each core's busy time from /proc/stat, so that foreign load on the
    receiver's cores can be told apart from the receiver's own."""

    def __init__(self, paths):
        self.paths = paths
        self.rows = []
        self._stop = threading.Event()

    @staticmethod
    def _read(path, name):
        try:
            with open(os.path.join(path, name)) as f:
                return f.read()
        except OSError as error:
            raise RuntimeError(f"cannot sample {path}/{name}") from error

    def sample(self):
        usage = 0
        mem = 0
        for p in self.paths:
            for line in self._read(p, "cpu.stat").splitlines():
                if line.startswith("usage_usec"):
                    usage += int(line.split()[1])
            cur = self._read(p, "memory.current").strip()
            mem += int(cur) if cur else 0
        cores = {}
        with open("/proc/stat") as f:
            for line in f:
                if line.startswith("cpu") and line[3].isdigit():
                    parts = line.split()
                    vals = list(map(int, parts[1:9]))
                    cores[parts[0]] = {"busy": sum(vals) - vals[3] - vals[4], "total": sum(vals), "steal": vals[7]}
        self.rows.append({"t": time.time(), "cpu_usec": usage, "mem_bytes": mem, "cores": cores})

    def run(self, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end and not self._stop.is_set():
            self.sample()
            self._stop.wait(1)
