"""Deterministic cloned-voice protocol fixture: no models or audio devices."""
import base64
import json
import os
from pathlib import Path
import struct
import sys
import time


def read_exact(count):
    data = bytearray()
    while len(data) < count:
        chunk = sys.stdin.buffer.read(count - len(data))
        if not chunk:
            return None
        data.extend(chunk)
    return data


def send(value):
    payload = json.dumps(value, ensure_ascii=True).encode("utf-8")
    sys.stdout.buffer.write(struct.pack("<I", len(payload)) + payload)
    sys.stdout.buffer.flush()


def pcm(samples, rate=24000, **extra):
    value = {"pcm": base64.b64encode(struct.pack("<" + "h" * len(samples), *samples)).decode("ascii"), "rate": rate}
    value.update(extra)
    send(value)


def event(voice, name, mode, request):
    with (Path(voice).parent / "events.jsonl").open("a", encoding="utf-8") as output:
        output.write(json.dumps({"event": name, "mode": mode, "pid": os.getpid(), "request": request}) + "\n")


def workspace(voice, path):
    directory = Path(path)
    (directory / "fixture-owned-artifact.tmp").write_text("private fixture artifact", encoding="utf-8")
    with (Path(voice).parent / "workspaces.txt").open("a", encoding="utf-8") as output:
        output.write(str(directory) + "\n")


def main():
    assert sys.argv[1:] == ["worker"] and sys.flags.isolated
    requests = 0
    while True:
        header = read_exact(4)
        if header is None:
            return
        length = struct.unpack("<I", header)[0]
        assert 0 < length <= 16384
        arguments = json.loads(read_exact(length))
        requests += 1
        voice = arguments[1]
        if arguments[0] == "prepare":
            if len(arguments) == 3:
                workspace(voice, arguments[2])
            mode = json.loads(Path(voice).read_text(encoding="utf-8")).get("fixtureMode", "valid")
            event(voice, "prepare", mode, requests)
            if mode == "prepare-sleep":
                time.sleep(10)
            if mode == "prepare-error":
                send({"ok": False, "error": "fixture prepare failed"})
            elif mode == "prepare-pcm":
                pcm([1, 2, 3])
                send({"ok": True})
            else:
                send({"ok": True})
            continue
        assert arguments[0] == "synthesize-stream" and len(arguments) == 3
        workspace(voice, Path(arguments[2]).parent)
        mode = Path(arguments[2]).read_text(encoding="utf-8")
        event(voice, "start", mode, requests)
        if mode in {"valid", "delayed", "open-error", "append-error", "finish-error"}:
            pcm([1, 2, 3])
            time.sleep(0.35 if mode == "delayed" else 0.01)
            event(voice, "generated", mode, requests)
            pcm([-1, 1000, -32768])
            send({"ok": True})
        elif mode == "invalid-base64":
            send({"pcm": "!!!!", "rate": 24000})
        elif mode == "noncanonical-base64":
            send({"pcm": "AAF=", "rate": 24000})
        elif mode == "odd-pcm":
            send({"pcm": "AA==", "rate": 24000})
        elif mode == "empty-pcm":
            send({"pcm": "", "rate": 24000})
        elif mode == "oversized-pcm":
            pcm([0] * 32769)
        elif mode == "string-pcm":
            send({"pcm": 123, "rate": 24000})
        elif mode == "missing-rate":
            send({"pcm": "AQACAAMA"})
        elif mode == "unknown-field":
            pcm([1, 2, 3], ignored=True)
        elif mode.startswith("rate-") and mode != "rate-change":
            values = {"rate-low": 7999, "rate-high": 192001, "rate-fraction": 24000.5,
                      "rate-bool": True, "rate-text": "24000", "rate-null": None}
            pcm([1, 2, 3], values[mode])
        elif mode == "rate-change":
            pcm([1, 2, 3])
            pcm([1, 2, 3], 48000)
        elif mode == "too-long":
            for _ in range(74):
                pcm([0] * 32768, 8000)
            send({"ok": True})
        elif mode == "empty-success":
            send({"ok": True})
        elif mode == "error-before":
            send({"ok": False, "error": "fixture model failed"})
        elif mode in {"error-after", "die-after", "hang-after"}:
            pcm([1, 2, 3])
            if mode == "die-after":
                os._exit(29)
            if mode == "hang-after":
                time.sleep(10)
            send({"ok": False, "error": "fixture generation failed after PCM"})
        else:
            raise AssertionError("unknown fixture mode")


if __name__ == "__main__":
    main()
