"""Opt-in genuine cloned-voice streaming latency check; no audio devices used.

Uses only SPEECH_HOME/diagnostics/synthetic-reference.wav, an operator-owned
synthetic test recording. Never reads user voices, records, or plays sound.
Not registered with ordinary CTest because authorized local models are needed.
"""
from __future__ import annotations

import argparse
import base64
import importlib.util
import json
import os
from pathlib import Path
import queue
import struct
import subprocess
import sys
import tempfile
import threading
import time
import wave

HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("speech_model_fixtures", HERE / "speech-model-tests.py")
FIXTURE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FIXTURE)
REQUEST_LIMIT = 16384
RESPONSE_LIMIT = 131072


def read_exact(stream, count):
    data = bytearray()
    while len(data) < count:
        part = stream.read(count - len(data))
        if not part:
            break
        data.extend(part)
    return bytes(data)


class Worker:
    def __init__(self, python, fixture, environment):
        self.fixture = fixture
        self.log = fixture / "stream-worker.log"
        self.diagnostics = self.log.open("wb")
        self.process = subprocess.Popen(
            [str(python), "-I", str(FIXTURE.VOICE_HELPER), "worker"],
            cwd=fixture, env=environment, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
            stderr=self.diagnostics, close_fds=True, bufsize=0,
            start_new_session=os.name != "nt",
            creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        self.responses = queue.Queue()
        self.reader = threading.Thread(target=self._read, daemon=True)
        self.reader.start()

    def _read(self):
        try:
            while True:
                header = read_exact(self.process.stdout, 4)
                if not header:
                    raise RuntimeError("Speech worker closed its response pipe.")
                FIXTURE.require(len(header) == 4, "Truncated worker header")
                length = struct.unpack("<I", header)[0]
                FIXTURE.require(0 < length <= RESPONSE_LIMIT, "Invalid worker response size")
                data = read_exact(self.process.stdout, length)
                FIXTURE.require(len(data) == length, "Truncated worker response")
                value = json.loads(data.decode("utf-8"))
                self.responses.put((time.monotonic(), value))
        except BaseException as exception:
            self.responses.put((time.monotonic(), exception))

    def request(self, arguments, output=None, timeout=180):
        FIXTURE.require(self.process.poll() is None, "The cached speech worker unexpectedly exited")
        payload = json.dumps(arguments).encode("utf-8")
        FIXTURE.require(0 < len(payload) <= REQUEST_LIMIT, "Invalid fixture request length")
        started = time.monotonic()
        pending = memoryview(struct.pack("<I", len(payload)) + payload)
        while pending:
            count = self.process.stdin.write(pending)
            FIXTURE.require(count is not None and count > 0, "Cannot write fixture request")
            pending = pending[count:]
        self.process.stdin.flush()
        pcm, first, chunks, rate = bytearray(), None, 0, None
        while True:
            remaining = started + timeout - time.monotonic()
            FIXTURE.require(remaining > 0, "Speech worker request timed out")
            try:
                arrived, value = self.responses.get(timeout=min(remaining, 1))
            except queue.Empty:
                FIXTURE.require(self.log.stat().st_size <= FIXTURE.MAX_LOG, "Excessive worker diagnostics")
                continue
            if isinstance(value, BaseException):
                raise value
            FIXTURE.require(isinstance(value, dict), "Expected a framed worker object")
            if "pcm" in value:
                FIXTURE.require(output is not None, "Preparation must not generate audible PCM")
                FIXTURE.require(set(value) == {"pcm", "rate"} and type(value["rate"]) is int and value["rate"] == 24000,
                                "Expected mono PCM16 at 24 kHz")
                chunk = base64.b64decode(value["pcm"], validate=True)
                FIXTURE.require(0 < len(chunk) <= 65536 and len(chunk) % 2 == 0, "Invalid PCM chunk")
                FIXTURE.require(len(pcm) + len(chunk) <= 24000 * 300 * 2, "Excessive generated audio")
                first = arrived if first is None else first
                rate = value["rate"]
                pcm.extend(chunk)
                chunks += 1
            else:
                FIXTURE.require(value == {"ok": True}, "Speech worker returned a failure: " + str(value))
                elapsed = arrived - started
                result = {"elapsedSeconds": round(elapsed, 4), "workerPid": self.process.pid}
                if output is not None:
                    FIXTURE.require(first is not None and chunks >= 2 and elapsed > first - started,
                                    "Audio must stream in multiple chunks before synthesis finishes")
                    with wave.open(str(output), "wb") as recording:
                        recording.setnchannels(1)
                        recording.setsampwidth(2)
                        recording.setframerate(rate)
                        recording.writeframes(pcm)
                    result.update(FIXTURE.validate_wave(output))
                    result.update({"firstPCMSeconds": round(first - started, 4), "chunks": chunks,
                                   "streamingLeadSeconds": round(arrived - first, 4)})
                return result

    def close(self):
        try:
            if self.process.stdin:
                self.process.stdin.close()
            self.process.wait(timeout=15)
        except (OSError, subprocess.TimeoutExpired):
            if self.process.poll() is None:
                FIXTURE.kill_owned_process(self.process)
        finally:
            self.reader.join(timeout=2)
            if self.process.stdout:
                self.process.stdout.close()
            self.diagnostics.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--speech-home", type=Path, default=FIXTURE.default_home())
    parser.add_argument("--report", type=Path, help="Optional new JSON report path; never overwrites an existing report")
    arguments = parser.parse_args()
    home = arguments.speech_home.expanduser()
    FIXTURE.require(home.is_absolute(), "--speech-home must be absolute")
    home = home.resolve(strict=True)
    python = home / "venv" / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
    reference = home / "diagnostics" / "synthetic-reference.wav"
    configuration = home / "models" / "pocket-tts" / "model.yaml"
    FIXTURE.require(python.is_file(), "Install the optional managed speech interpreter first")
    FIXTURE.require(reference.is_file(), "The operator-owned synthetic reference is missing")
    FIXTURE.require(configuration.is_file(), "Install authorized local cloning weights/configuration first")
    environment = dict(os.environ)
    environment.update({"ZSHARP_SPEECH_HOME": str(home), "SDL_AUDIODRIVER": "dummy",
                        "ZSHARP_VOICE_MODEL_CONFIG": str(configuration),
                        "HF_HUB_OFFLINE": "1", "HF_DATASETS_OFFLINE": "1",
                        "TRANSFORMERS_OFFLINE": "1", "HF_HUB_DISABLE_TELEMETRY": "1"})
    report = {"physicalAudio": False, "reference": "diagnostics/synthetic-reference.wav",
              "description": "First PCM availability, not subjective audio quality or live microphone latency"}
    with tempfile.TemporaryDirectory(prefix="zsharp-real-stream-test-") as directory:
        fixture = Path(directory)
        voice = fixture / "synthetic.zvoice"
        report["conditioning"] = FIXTURE.invoke_helper(python, fixture, environment, "create", "create",
                                                       "--reference", str(reference), "--output", str(voice))
        report["voice"] = FIXTURE.validate_voice(voice, reference)
        workspace = fixture / "prepare-owned"
        workspace.mkdir()
        text = fixture / "utterance.txt"
        text.write_text("Understood. Could you clarify what you would like me to do?", encoding="utf-8")
        worker = Worker(python, fixture, environment)
        try:
            report["prepare"] = worker.request(["prepare", str(voice), str(workspace)])
            report["utterances"] = [worker.request(["synthesize-stream", str(voice), str(text)], fixture / f"stream-{index}.wav")
                                     for index in range(2)]
            FIXTURE.require(all(entry["workerPid"] == report["prepare"]["workerPid"] for entry in report["utterances"]),
                            "The same cached worker must serve both utterances")
        finally:
            worker.close()
        FIXTURE.require(worker.process.returncode == 0, "Worker did not exit gracefully on EOF")
        FIXTURE.require(worker.log.stat().st_size <= FIXTURE.MAX_LOG, "Excessive worker diagnostics")
        FIXTURE.require(not any(workspace.iterdir()), "Preparation left conditioning workspace files behind")
        FIXTURE.require(not any(fixture.glob("zsharp-voice-worker-*")), "Synthesis left conditioning workspace files behind")
    if arguments.report is not None:
        with arguments.report.open("x", encoding="utf-8", newline="\n") as destination:
            json.dump(report, destination, indent=2)
            destination.write("\n")
    print(json.dumps(report, indent=2))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
