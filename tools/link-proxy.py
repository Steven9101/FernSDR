#!/usr/bin/env python3
"""Local TCP relay for testing streaming backpressure, without root or tc.

Only the receiver-to-client direction is shaped. --jitter-ms adds a delivery
pause every two seconds; it is a repeatable stress case, not a cellular model.
--outage-every repeats the --outage-ms pause, which is how a link that is fast
enough but stalls now and then (Wi-Fi, a phone changing cells, a relay) looks
from the receiver.
"""
import argparse
import math
import socket
import threading
import time


def relay(client, options):
    upstream = socket.socket()
    upstream.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2048)
    upstream.connect(("127.0.0.1", options.upstream))
    upstream.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    client.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    def upload():
        try:
            while chunk := client.recv(4096):
                upstream.sendall(chunk)
        except OSError:
            pass
        finally:
            upstream.close()
            client.close()

    threading.Thread(target=upload, daemon=True).start()
    try:
        started = time.monotonic()
        next_pause = started + 2
        outage_pending = options.outage_ms > 0
        next_outage = started + options.outage_at
        while chunk := upstream.recv(128):
            if outage_pending and time.monotonic() >= next_outage:
                time.sleep(options.outage_ms / 1000)
                outage_pending = options.outage_every > 0
                next_outage += options.outage_every
            time.sleep(len(chunk) * 8 / (options.kbps * 1000))
            if time.monotonic() >= next_pause:
                time.sleep(options.jitter_ms / 1000)
                next_pause = time.monotonic() + 2
            client.sendall(chunk)
    except OSError:
        pass
    finally:
        upstream.close()
        client.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=18074)
    parser.add_argument("--upstream", type=int, default=8073)
    parser.add_argument("--kbps", type=float, default=64)
    parser.add_argument("--jitter-ms", type=float, default=80)
    parser.add_argument("--outage-at", type=float, default=10,
                        help="seconds after connecting to the first pause")
    parser.add_argument("--outage-ms", type=float, default=0)
    parser.add_argument("--outage-every", type=float, default=0,
                        help="seconds between repeated outages; 0 pauses once")
    options = parser.parse_args()
    if not all(math.isfinite(value) for value in
               (options.kbps, options.jitter_ms, options.outage_at, options.outage_ms, options.outage_every)):
        parser.error("rate and timing must be finite")
    if options.kbps <= 0 or min(options.jitter_ms, options.outage_at, options.outage_ms, options.outage_every) < 0:
        parser.error("rate must be positive and timing must be nonnegative")
    if 0 < options.outage_every <= options.outage_ms / 1000:
        parser.error("--outage-every must be longer than the outage itself")
    if not 1 <= options.port <= 65535 or not 1 <= options.upstream <= 65535:
        parser.error("ports must be between 1 and 65535")
    with socket.create_server(("127.0.0.1", options.port)) as listener:
        print(f"ready on 127.0.0.1:{options.port}", flush=True)
        while True:
            client, _ = listener.accept()
            threading.Thread(target=relay, args=(client, options), daemon=True).start()


if __name__ == "__main__":
    main()
