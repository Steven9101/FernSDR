#!/usr/bin/env python3
"""Check format ordering, acknowledgements and frame limits on a test receiver."""
import argparse
import importlib.util
import json
import os
from pathlib import Path
import struct
import time

spec = importlib.util.spec_from_file_location("probe", Path(__file__).with_name("fernsdr-probe.py"))
probe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(probe)


def send_fragment(ws, opcode, payload, final):
    mask = os.urandom(4)
    header = bytes([(0x80 if final else 0) | opcode])
    if len(payload) < 126:
        header += bytes([0x80 | len(payload)])
    else:
        header += b"\xfe" + struct.pack(">H", len(payload))
    ws.sock.sendall(header + mask + bytes(value ^ mask[index % 4]
                                         for index, value in enumerate(payload)))


def receive_json(ws, kind):
    deadline = time.monotonic() + 5
    while time.monotonic() < deadline:
        opcode, payload = ws.recv()
        assert opcode != 8, "observer disconnected during Unicode checks"
        if opcode == 1:
            message = json.loads(payload.decode("utf-8", errors="strict"))
            if message["type"] == kind:
                return message
    raise AssertionError(f"no {kind} response")


def check_unicode(port):
    observers = [probe.WebSocket("127.0.0.1", port) for _ in range(2)]
    sender = probe.WebSocket("127.0.0.1", port)
    try:
        for observer in observers:
            receive_json(observer, "welcome")
        # Both byte limits end inside a valid two-byte character. Every
        # broadcast and the retained history must remain valid WS text.
        sender.send_text(json.dumps({"type": "chat", "name": "n" * 23 + "é",
                                     "text": "t" * 399 + "é"}, ensure_ascii=False))
        for observer in observers:
            message = receive_json(observer, "chat")
            assert message["name"] == "n" * 23
            assert message["text"] == "t" * 399

        payload = json.dumps({"type": "chat", "text": "fragmented é"},
                             ensure_ascii=False).encode()
        split = payload.index("é".encode()) + 1
        send_fragment(sender, 1, payload[:split], False)
        send_fragment(sender, 0, payload[split:], True)
        for observer in observers:
            assert receive_json(observer, "chat")["text"] == "fragmented é"

        sender._send(1, b'{"type":"chat","text":"\xc0\xaf"}')
        while True:
            opcode, payload = sender.recv()
            if opcode == 8:
                assert struct.unpack_from(">H", payload)[0] == 1007
                break
        for observer in observers:
            observer.send_text('{"type":"chat","history":true}')
            history = receive_json(observer, "chat-history")["messages"]
            assert history[-1]["text"] == "fragmented é"
        print("Unicode truncation, fragmented text and sender-only rejection passed")
    finally:
        sender.sock.close()
        for observer in observers:
            observer.sock.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", type=int, default=8073)
    args = parser.parse_args()
    ws = probe.WebSocket("127.0.0.1", args.port)
    bands = []
    generation = None
    audio_frames = 0
    acknowledged = 0
    try:
        def receive():
            nonlocal generation, bands, acknowledged, audio_frames
            opcode, payload = ws.recv()
            assert opcode != 8, "receiver closed during format changes"
            if opcode == 1:
                message = json.loads(payload)
                if message["type"] == "welcome":
                    bands = message["bands"]
                elif message["type"] == "audio-config":
                    generation = message["generation"]
                elif message["type"] == "state":
                    acknowledged = max(acknowledged, message.get("ack", {}).get("tune", 0))
            elif opcode == 2 and payload[0] == 1:
                assert generation is not None, "audio arrived before its format"
                assert payload[1] >> 4 == generation, "audio format and generation disagree"
                audio_frames += 1

        while not bands:
            receive()
        for request in range(1, 17):
            band = bands[request % len(bands)]
            ws.send_text(json.dumps({"type": "tune", "band": band["id"], "freq": band["center"],
                                     "mode": ["am", "usb", "nfm", "cw"][request % 4], "request_id": request}))
            ws.send_text(json.dumps({"type": "viewport", "codec": "wfc2", "width": 512,
                                     "low": band["low"], "high": band["high"], "fps": 10}))
            deadline = time.monotonic() + 0.15
            while time.monotonic() < deadline or acknowledged < request:
                receive()
        assert audio_frames > 30
        print(f"format ordering and acknowledgements passed: {audio_frames} audio frames, 16 reconfigurations")
    finally:
        ws.sock.close()

    check_unicode(args.port)

    ws = probe.WebSocket("127.0.0.1", args.port)
    try:
        # A masked 64-bit length declaration, deliberately without its body.
        # The server must reject the header instead of waiting for 4 GB.
        ws.sock.sendall(bytes([0x81, 0xFF]) + struct.pack(">Q", 1 << 32) + b"test")
        while True:
            opcode, payload = ws.recv()
            if opcode == 8:
                assert struct.unpack_from(">H", payload)[0] in (1002, 1009)
                break
        print("oversized frame rejected before its body")
    finally:
        ws.sock.close()


if __name__ == "__main__":
    main()
