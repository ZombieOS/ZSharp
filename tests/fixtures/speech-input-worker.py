"""Model/device-free native persistent-worker transport fixture."""
import json
import os
import struct
import sys
import time


def read_exact(count):
    chunks = bytearray()
    while len(chunks) < count:
        part = sys.stdin.buffer.read(count - len(chunks))
        if not part:
            return None
        chunks.extend(part)
    return chunks


def respond(value):
    payload = json.dumps(value, ensure_ascii=True).encode("utf-8")
    sys.stdout.buffer.write(struct.pack("<I", len(payload)) + payload)
    sys.stdout.buffer.flush()


def main():
    assert sys.argv[1:] == ["worker"]
    assert sys.flags.isolated == 1
    requests = 0
    while True:
        header = read_exact(4)
        if header is None:
            return
        length = struct.unpack("<I", header)[0]
        assert 0 < length <= 16384
        request = json.loads(read_exact(length).decode("utf-8"))
        requests += 1
        if request == ["preflight"]:
            respond({"ok": True})
            continue
        if len(request) in {2, 3} and request[0] == "prepare":
            respond({"ok": True})
            continue
        if len(request) == 3 and request[0] == "synthesize-stream":
            mode, destination = request[1:]
            with open(destination, "w", encoding="utf-8") as output:
                output.write(f"{os.getpid()}:{requests}:stream")
            if mode == "stream-error":
                respond({"ok": False, "error": "fixture voice unavailable"})
            elif mode == "stream-oversize":
                sys.stdout.buffer.write(struct.pack("<I", 262145))
                sys.stdout.buffer.flush()
            elif mode == "stream-bad-utf8":
                payload = b'{"pcm":"\xff"}'
                sys.stdout.buffer.write(struct.pack("<I", len(payload)) + payload)
                sys.stdout.buffer.flush()
            elif mode == "stream-malformed":
                sys.stdout.buffer.write(struct.pack("<I", 4) + b"oops")
                sys.stdout.buffer.flush()
            else:
                respond({"pcm": "AQACAAMA", "rate": 24000})
                if mode == "stream-die":
                    os._exit(27)
                time.sleep(10 if mode == "stream-timeout" else 0.35 if mode == "stream-delay" else 0.01)
                respond({"pcm": "BAAFAA==", "rate": 24000})
                respond({"ok": True})
            continue
        assert len(request) == 3 and request[0] == "transcribe"
        mode, destination = request[1:]
        if mode == "structured-error":
            respond({"ok": False, "error": "fixture model unavailable: \u00e9 \U0001f50a"})
        elif mode == "structured-stderr":
            sys.stderr.write("fixture native model loader diagnostic\n")
            sys.stderr.flush()
            respond({"ok": False, "error": "fixture model could not load"})
        elif mode == "die":
            sys.stderr.write("fixture child crashed\n")
            sys.stderr.flush()
            os._exit(23)
        elif mode == "malformed":
            payload = b'{"ok":true,"ok":false}'
            sys.stdout.buffer.write(struct.pack("<I", len(payload)) + payload)
            sys.stdout.buffer.flush()
        elif mode == "oversize":
            sys.stdout.buffer.write(struct.pack("<I", 8193))
            sys.stdout.buffer.flush()
        elif mode == "partial":
            sys.stdout.buffer.write(struct.pack("<I", 20) + b'{')
            sys.stdout.buffer.flush()
            os._exit(24)
        elif mode == "stderr":
            while True:
                sys.stderr.buffer.write(b"bounded diagnostic output\n" * 200)
                sys.stderr.buffer.flush()
        elif mode == "unsolicited":
            respond({"ok": True})
            respond({"ok": True})
        else:
            if mode == "sleep":
                with open(destination, "w", encoding="utf-8") as output:
                    output.write("began")
                time.sleep(10)
            elif mode == "delay":
                with open(destination, "w", encoding="utf-8") as output:
                    output.write("began")
                time.sleep(0.35)
            with open(destination, "w", encoding="utf-8") as output:
                output.write(f"{os.getpid()}:{requests}:" + json.dumps(request, ensure_ascii=True))
            respond({"ok": True})


if __name__ == "__main__":
    main()
