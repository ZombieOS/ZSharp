"""Offline speech-worker fixtures; no microphone, model weights, or audio playback.

Each worker subprocess imports the real helper with only a tiny in-memory Vosk
stand-in. Tests use the public binary worker protocol, not private implementation
functions, so framing, CLI dispatch, model lifetime, and result-file checks are
covered together.
"""

import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
import wave


sys.dont_write_bytecode = True
HELPER_PATH = Path(__file__).resolve().parents[1] / "native/python/zsharp_speech_input.py"
REQUEST_LIMIT = 16384
RESPONSE_LIMIT = 8192

# -I prevents user-site packages/PYTHONPATH from supplying a real Vosk module.
# Installing the fixture in sys.modules before runpy also avoids touching any
# optional speech installation on the machine running these tests.
WORKER_BOOTSTRAP = r'''
import json
import os
import pathlib
import runpy
import sys
import types

sys.dont_write_bytecode = True
helper, trace_filename, failure = sys.argv[1:4]
model_count = 0
recognizer_count = 0

def trace(event, **details):
    with open(trace_filename, "a", encoding="utf-8") as output:
        output.write(json.dumps({"event": event, **details}) + "\n")

def set_log_level(level):
    trace("log-level", level=level)

class Model:
    def __init__(self, directory):
        global model_count
        model_count += 1
        trace("model", number=model_count, directory=directory)
        if failure == "always" or (failure == "once" and model_count == 1):
            raise RuntimeError("fixture model load cause: permission denied")
        if failure == "huge":
            raise RuntimeError("fixture oversized model cause: " + "x" * 40000)
        self.number = model_count

class Recognizer:
    def __init__(self, model, rate):
        global recognizer_count
        recognizer_count += 1
        self.number = recognizer_count
        self.calls = 0
        trace("recognizer", number=self.number, model=model.number, rate=rate)

    def AcceptWaveform(self, data):
        if not isinstance(data, bytes) or len(data) % 2:
            raise RuntimeError("fixture received invalid PCM16 bytes")
        self.calls += 1
        return False

    def Result(self):
        return json.dumps({"text": ""})

    def FinalResult(self):
        return json.dumps({"text": "fixture recognition %d caf\u00e9" % self.number})

vosk = types.ModuleType("vosk")
vosk.Model = Model
vosk.KaldiRecognizer = Recognizer
vosk.SetLogLevel = set_log_level
sys.modules["vosk"] = vosk
sys.argv = [helper, "worker"]
runpy.run_path(helper, run_name="__main__")
'''


def request_frame(value):
    payload = json.dumps(value, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
    return struct.pack("<I", len(payload)) + payload


class SpeechWorkerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="zsharp-speech-input-tests-")
        self.root = Path(self.temporary.name)
        self.model = self.root / "fixture model"
        (self.model / "am").mkdir(parents=True)
        (self.model / "am/final.mdl").write_bytes(b"not real model weights")
        self.trace = self.root / "worker-trace.jsonl"
        self.wav = self.root / "recording fixture.wav"
        self.make_wav(self.wav)

    def tearDown(self):
        self.temporary.cleanup()

    def make_wav(self, filename, *, channels=1, width=2, rate=16000, frames=320):
        with wave.open(str(filename), "wb") as recording:
            recording.setnchannels(channels)
            recording.setsampwidth(width)
            recording.setframerate(rate)
            recording.writeframes(b"\0" * frames * channels * width)

    def result_file(self, name="existing result.txt"):
        filename = self.root / name
        filename.write_text("previous owned result", encoding="utf-8")
        return filename

    def run_worker(self, payload, *, failure="", model=None):
        environment = os.environ.copy()
        environment["ZSHARP_SPEECH_MODEL"] = str(model or self.model)
        completed = subprocess.run(
            [sys.executable, "-I", "-c", WORKER_BOOTSTRAP,
             str(HELPER_PATH), str(self.trace), failure],
            input=payload, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            env=environment, cwd=str(self.root), timeout=10,
        )
        # Diagnostic failures must not turn into unlimited error output either.
        self.assertLessEqual(len(completed.stderr), 32768)
        return completed

    def responses(self, stdout):
        result = []
        offset = 0
        while offset < len(stdout):
            self.assertGreaterEqual(len(stdout) - offset, 4, "truncated response length")
            length = struct.unpack_from("<I", stdout, offset)[0]
            offset += 4
            self.assertGreater(length, 0)
            self.assertLessEqual(length, RESPONSE_LIMIT)
            self.assertGreaterEqual(len(stdout) - offset, length, "truncated response payload")
            value = json.loads(stdout[offset:offset + length].decode("utf-8"))
            self.assertIsInstance(value, dict)
            self.assertIn(value.get("ok"), (True, False))
            self.assertIsInstance(value.get("ok"), bool)
            if not value["ok"]:
                self.assertIsInstance(value.get("error"), str)
                self.assertTrue(value["error"])
            result.append(value)
            offset += length
        self.assertEqual(offset, len(stdout))
        return result

    def events(self, name):
        if not self.trace.exists():
            return []
        return [event for line in self.trace.read_text(encoding="utf-8").splitlines()
                if (event := json.loads(line))["event"] == name]

    def assert_recoverable(self, request):
        completed = self.run_worker(request_frame(request) + request_frame(["preflight"]))
        self.assertEqual(completed.returncode, 0, completed.stderr.decode("utf-8", "replace"))
        response = self.responses(completed.stdout)
        self.assertEqual(len(response), 2)
        self.assertFalse(response[0]["ok"])
        self.assertEqual(response[1], {"ok": True})
        return response[0]["error"]

    def test_eof_before_request_is_clean_and_does_not_load_model(self):
        completed = self.run_worker(b"")
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(completed.stdout, b"")
        self.assertEqual(self.events("model"), [])

    def test_preflight_then_transcriptions_share_model_but_not_recognizer(self):
        first = self.result_file("first result.txt")
        second = self.result_file("second result.txt")
        requests = [
            ["preflight"], ["preflight"],
            ["transcribe", str(self.wav), str(first)],
            ["transcribe", str(self.wav), str(second)],
        ]
        completed = self.run_worker(b"".join(map(request_frame, requests)))
        self.assertEqual(completed.returncode, 0, completed.stderr)
        self.assertEqual(self.responses(completed.stdout), [{"ok": True}] * 4)
        self.assertEqual(len(self.events("model")), 1)
        recognizers = self.events("recognizer")
        self.assertEqual([event["number"] for event in recognizers], [1, 2])
        self.assertEqual([event["model"] for event in recognizers], [1, 1])
        self.assertEqual([event["rate"] for event in recognizers], [16000, 16000])
        self.assertEqual(first.read_text(encoding="utf-8"), "fixture recognition 1 caf\u00e9")
        self.assertEqual(second.read_text(encoding="utf-8"), "fixture recognition 2 caf\u00e9")
        self.assertTrue(self.events("log-level"))
        self.assertTrue(all(event["level"] == 0 for event in self.events("log-level")))

    def test_unknown_operation_is_framed_error_and_worker_continues(self):
        self.assert_recoverable(["unknown-operation"])

    def test_wrong_json_request_types_are_recoverable(self):
        for request in (None, {}, "preflight", 7, True, [], [1], ["preflight", "extra"]):
            with self.subTest(request=request):
                self.assert_recoverable(request)

    def test_transcribe_arity_and_path_types_are_recoverable(self):
        for request in (["transcribe"], ["transcribe", str(self.wav)],
                        ["transcribe", 1, "output"], ["transcribe", str(self.wav), None]):
            with self.subTest(request=request):
                self.assert_recoverable(request)

    def test_malformed_json_is_bounded_error_and_worker_continues(self):
        payload = b'{"not valid json"'
        completed = self.run_worker(struct.pack("<I", len(payload)) + payload
                                    + request_frame(["preflight"]))
        self.assertEqual(completed.returncode, 0, completed.stderr)
        responses = self.responses(completed.stdout)
        self.assertEqual(len(responses), 2)
        self.assertFalse(responses[0]["ok"])
        self.assertEqual(responses[1], {"ok": True})

    def test_invalid_utf8_is_bounded_error_and_worker_continues(self):
        payload = b'["\xff"]'
        completed = self.run_worker(struct.pack("<I", len(payload)) + payload
                                    + request_frame(["preflight"]))
        self.assertEqual(completed.returncode, 0, completed.stderr)
        responses = self.responses(completed.stdout)
        self.assertEqual(len(responses), 2)
        self.assertFalse(responses[0]["ok"])
        self.assertEqual(responses[1], {"ok": True})

    def test_oversized_request_is_rejected_without_model_loading(self):
        completed = self.run_worker(struct.pack("<I", REQUEST_LIMIT + 1))
        self.assertNotEqual(completed.returncode, 0)
        self.assertLessEqual(len(completed.stdout), RESPONSE_LIMIT + 4)
        self.assertEqual(self.events("model"), [])

    def test_zero_length_frame_is_rejected(self):
        completed = self.run_worker(struct.pack("<I", 0))
        self.assertNotEqual(completed.returncode, 0)
        self.assertLessEqual(len(completed.stdout), RESPONSE_LIMIT + 4)

    def test_truncated_frame_header_is_rejected(self):
        for payload in (b"\x01", b"\x01\0", b"\x01\0\0"):
            with self.subTest(length=len(payload)):
                completed = self.run_worker(payload)
                self.assertNotEqual(completed.returncode, 0)
                self.assertLessEqual(len(completed.stdout), RESPONSE_LIMIT + 4)

    def test_truncated_frame_body_is_rejected(self):
        completed = self.run_worker(struct.pack("<I", 20) + b'["preflight"]')
        self.assertNotEqual(completed.returncode, 0)
        self.assertLessEqual(len(completed.stdout), RESPONSE_LIMIT + 4)
        self.assertEqual(self.events("model"), [])

    def test_invalid_wav_does_not_kill_cached_worker(self):
        invalid = self.root / "invalid.wav"
        invalid.write_bytes(b"not a wave recording" * 4)
        result = self.result_file()
        completed = self.run_worker(
            request_frame(["preflight"])
            + request_frame(["transcribe", str(invalid), str(result)])
            + request_frame(["transcribe", str(self.wav), str(result)]))
        self.assertEqual(completed.returncode, 0, completed.stderr)
        responses = self.responses(completed.stdout)
        self.assertEqual(len(responses), 3)
        self.assertTrue(responses[0]["ok"])
        self.assertFalse(responses[1]["ok"])
        self.assertTrue(responses[2]["ok"])
        self.assertEqual(len(self.events("model")), 1)
        self.assertEqual(result.read_text(encoding="utf-8"), "fixture recognition 1 caf\u00e9")

    def test_stereo_recording_is_recoverable(self):
        stereo = self.root / "stereo.wav"
        self.make_wav(stereo, channels=2)
        result = self.result_file()
        error = self.assert_recoverable(["transcribe", str(stereo), str(result)])
        self.assertIn("mono", error.lower())
        self.assertEqual(result.read_text(encoding="utf-8"), "previous owned result")

    def test_missing_result_file_is_not_created(self):
        missing = self.root / "not owned.txt"
        self.assert_recoverable(["transcribe", str(self.wav), str(missing)])
        self.assertFalse(missing.exists())

    def test_result_directory_is_rejected_without_modification(self):
        directory = self.root / "not a regular file"
        directory.mkdir()
        self.assert_recoverable(["transcribe", str(self.wav), str(directory)])
        self.assertEqual(list(directory.iterdir()), [])

    def test_result_symlink_cannot_overwrite_target(self):
        target = self.result_file("outside target.txt")
        link = self.root / "result symlink.txt"
        try:
            link.symlink_to(target)
        except (OSError, NotImplementedError):
            self.skipTest("symbolic link creation is unavailable to this test process")
        self.assert_recoverable(["transcribe", str(self.wav), str(link)])
        self.assertEqual(target.read_text(encoding="utf-8"), "previous owned result")
        self.assertTrue(link.is_symlink())

    def test_original_model_failure_cause_is_exposed_and_retry_can_succeed(self):
        completed = self.run_worker(request_frame(["preflight"]) * 2, failure="once")
        self.assertEqual(completed.returncode, 0, completed.stderr)
        responses = self.responses(completed.stdout)
        self.assertEqual(len(responses), 2)
        self.assertFalse(responses[0]["ok"])
        self.assertIn("fixture model load cause", responses[0]["error"])
        self.assertIn("permission denied", responses[0]["error"])
        self.assertEqual(responses[1], {"ok": True})
        self.assertEqual(len(self.events("model")), 2)

    def test_large_underlying_model_error_stays_within_wire_limit(self):
        completed = self.run_worker(request_frame(["preflight"]), failure="huge")
        self.assertEqual(completed.returncode, 0, completed.stderr)
        responses = self.responses(completed.stdout)
        self.assertEqual(len(responses), 1)
        self.assertFalse(responses[0]["ok"])
        self.assertIn("fixture oversized model cause", responses[0]["error"])
        self.assertLessEqual(len(completed.stdout), RESPONSE_LIMIT + 4)

    def test_missing_model_is_reported_before_any_recognizer_is_created(self):
        completed = self.run_worker(request_frame(["preflight"]), model=self.root / "missing model")
        self.assertEqual(completed.returncode, 0, completed.stderr)
        responses = self.responses(completed.stdout)
        self.assertEqual(len(responses), 1)
        self.assertFalse(responses[0]["ok"])
        self.assertIn("model", responses[0]["error"].lower())
        self.assertEqual(self.events("model"), [])
        self.assertEqual(self.events("recognizer"), [])


if __name__ == "__main__":
    unittest.main()
