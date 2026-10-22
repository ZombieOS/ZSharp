"""Fixture-only cached/streamed voice tests: no models, microphone or speakers."""
import base64
from contextlib import nullcontext
import importlib.util
import io
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True
HERE = Path(__file__).resolve().parent
SPEC = importlib.util.spec_from_file_location("voice_helper_worker_tests", HERE / "speech-voice-helper-tests.py")
FIXTURE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(FIXTURE)
VOICE = FIXTURE.VOICE


def frame(value):
    payload = json.dumps(value).encode("utf-8")
    return struct.pack("<I", len(payload)) + payload


def read_frames(data):
    result = []
    while data:
        if len(data) < 4:
            raise AssertionError("truncated frame")
        size = struct.unpack("<I", data[:4])[0]
        if not 0 < size <= VOICE.RESPONSE_LIMIT or len(data) < size + 4:
            raise AssertionError("invalid frame length")
        result.append(json.loads(data[4:4 + size]))
        data = data[4 + size:]
    return result


class Array(list):
    def reshape(self, _):
        return self

    def __mul__(self, value):
        return Array(item * value for item in self)

    def astype(self, dtype):
        if dtype != "<i2":
            raise AssertionError(dtype)
        values = self
        return types.SimpleNamespace(tobytes=lambda: struct.pack("<" + "h" * len(values), *(int(value) for value in values)))


class Tensor:
    def __init__(self, values):
        self.values = Array(values)

    def detach(self):
        return self

    def cpu(self):
        return self

    def numpy(self):
        return self.values


class Model:
    sample_rate = 24000
    has_voice_cloning = True

    def __init__(self):
        self.states = []
        self.finished = False

    def get_state_for_audio_prompt(self, filename):
        if not Path(filename).is_file():
            raise AssertionError("missing immutable conditioning")
        state = {"fresh": True}
        self.states.append(state)
        return state

    def generate_audio_stream(self, state, text, stop=None):
        if not state["fresh"]:
            raise AssertionError("state reused")
        state["fresh"] = False
        self.finished = False
        yield Tensor([0.25, -0.25])
        self.finished = True
        if not stop.is_set():
            yield Tensor([0.5, -0.5])


class VoiceWorkerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="zsharp-voice-worker-tests-")
        self.root = Path(self.temporary.name)
        self.voice = self.root / "fixture.zvoice"
        self.voice.write_text(json.dumps(FIXTURE.document()), encoding="utf-8")
        self.text = self.root / "text.txt"
        self.text.write_text("A fixture utterance.", encoding="utf-8")
        self.model = Model()
        self.numpy = types.SimpleNamespace(
            asarray=lambda value: Array(value),
            isfinite=lambda value: types.SimpleNamespace(all=lambda: all(abs(item) != float("inf") and item == item for item in value)),
            clip=lambda value, low, high: Array(max(low, min(high, item)) for item in value))
        self.modules = patch.dict(sys.modules, {"numpy": self.numpy, "torch": types.SimpleNamespace(no_grad=nullcontext)})
        self.modules.start()
        self.loader = patch.object(VOICE, "_load_model", return_value=(self.model, "fixture", "fixture"))
        self.loader.start()
        VOICE._voice_cache.clear()
        VOICE._model_cache = None

    def tearDown(self):
        self.loader.stop()
        self.modules.stop()
        VOICE._voice_cache.clear()
        VOICE._model_cache = None
        self.temporary.cleanup()

    def run_worker(self, requests):
        output = io.BytesIO()
        code = VOICE._worker_main(io.BytesIO(b"".join(frame(request) for request in requests)), output)
        return code, read_frames(output.getvalue())

    def test_first_pcm_precedes_completion_and_each_utterance_has_fresh_state(self):
        first = []
        for _ in range(2):
            emitted = []
            def emit(value):
                if not emitted:
                    first.append(self.model.finished)
                emitted.append(value)
            VOICE._synthesize_stream(self.voice, self.text, self.root, emit)
            self.assertEqual(len(emitted), 2)
            self.assertEqual(emitted[0]["rate"], 24000)
            self.assertEqual(base64.b64decode(emitted[0]["pcm"]), struct.pack("<hh", 8191, -8191))
        self.assertEqual(first, [False, False])
        self.assertEqual(len(self.model.states), 2)
        self.assertIsNot(self.model.states[0], self.model.states[1])

    def test_prepare_and_repeated_streams_have_only_framed_responses(self):
        code, frames = self.run_worker([["prepare", str(self.voice)],
                                       ["prepare", str(self.voice), str(self.root)],
                                       ["synthesize-stream", str(self.voice), str(self.text)],
                                       ["synthesize-stream", str(self.voice), str(self.text)]])
        self.assertEqual(code, 0)
        self.assertEqual([value for value in frames if "ok" in value], [{"ok": True}] * 4)
        self.assertEqual(len([value for value in frames if "pcm" in value]), 4)
        self.assertEqual(len(self.model.states), 4)

    def test_owned_workspace_contains_conditioning_and_is_cleaned(self):
        seen = []
        original = VOICE._prepared_voice
        def prepare(voice, temporary):
            self.assertEqual(temporary.parent, self.root)
            self.assertTrue(temporary.is_dir())
            result = original(voice, temporary)
            self.assertTrue((temporary / "voice.safetensors").is_file())
            seen.append(temporary)
            return result
        with patch.object(VOICE, "_prepared_voice", side_effect=prepare):
            code, frames = self.run_worker([["prepare", str(self.voice), str(self.root)],
                                           ["synthesize-stream", str(self.voice), str(self.text)]])
        self.assertEqual(code, 0)
        self.assertEqual(len(seen), 2)
        self.assertTrue(all(not path.exists() for path in seen))
        code, frames = self.run_worker([["prepare", str(self.voice), str(self.text)],
                                       ["prepare", str(self.voice), str(self.root / "missing")]])
        self.assertTrue(all(value["ok"] is False and "workspace" in value["error"] for value in frames))

    def test_request_error_does_not_end_worker_and_diagnostics_are_bounded(self):
        with patch.object(VOICE, "_prepared_voice", side_effect=VOICE.VoiceError("x\n" * 20000)):
            code, values = self.run_worker([["prepare", str(self.voice)], ["prepare", str(self.voice)]])
        self.assertEqual(code, 0)
        self.assertEqual(len(values), 2)
        self.assertTrue(all(value["ok"] is False and len(value["error"].encode()) <= 3000 and "\n" not in value["error"] for value in values))
        unicode_error = VOICE._error_frame(VOICE.VoiceError("\U0001f30d" * 10000))
        self.assertLessEqual(len(unicode_error["error"].encode("utf-8")), 3000)

    def test_partial_pipe_writes_preserve_complete_frame(self):
        class ShortWrites(io.BytesIO):
            def write(self, data):
                return super().write(data[:7])
        output = ShortWrites()
        value = {"pcm": base64.b64encode(b"\0\0" * 20000).decode("ascii"), "rate": 24000}
        VOICE._write_frame(output, value)
        self.assertEqual(read_frames(output.getvalue()), [value])

    def test_adjusted_voice_preserves_full_signal_librosa_fallback(self):
        document = FIXTURE.document()
        document["pitch"], document["speed"] = 110, 120
        self.voice.write_text(json.dumps(document), encoding="utf-8")
        effects = types.SimpleNamespace(pitch_shift=lambda audio, **kwargs: audio,
                                        time_stretch=lambda audio, **kwargs: audio)
        with patch.object(self.model, "generate_audio", create=True, return_value=Tensor([0.2, 0.3])) as full, \
                patch.object(self.model, "generate_audio_stream") as streaming, \
                patch.dict(sys.modules, {"librosa": types.SimpleNamespace(effects=effects)}), \
                patch.object(effects, "pitch_shift", wraps=effects.pitch_shift) as pitch, \
                patch.object(effects, "time_stretch", wraps=effects.time_stretch) as speed:
            values = []
            VOICE._synthesize_stream(self.voice, self.text, self.root, values.append)
        full.assert_called_once()
        streaming.assert_not_called()
        pitch.assert_called_once()
        speed.assert_called_once()
        self.assertEqual(len(values), 1)

    def test_malformed_unknown_or_relative_request_is_clear(self):
        code, values = self.run_worker([["unknown", str(self.voice)], ["prepare", "relative.zvoice"],
                                       ["prepare", 5], ["prepare", "bad\0file"]])
        self.assertEqual(code, 0)
        self.assertEqual(len(values), 4)
        self.assertTrue(all(value["ok"] is False for value in values))

    def test_truncated_and_oversized_frames_fail_without_allocation(self):
        for request in (b"\x01", struct.pack("<I", 500) + b"[]", struct.pack("<I", VOICE.REQUEST_LIMIT + 1), struct.pack("<I", 0)):
            with self.subTest(request=request):
                output = io.BytesIO()
                self.assertEqual(VOICE._worker_main(io.BytesIO(request), output), 1)
                self.assertIs(read_frames(output.getvalue())[0]["ok"], False)

    def test_pcm_frames_are_bounded_and_empty_audio_fails(self):
        def long_stream(*args, **kwargs):
            yield Tensor([0.25] * 70000)
        values = []
        with patch.object(self.model, "generate_audio_stream", side_effect=long_stream):
            VOICE._synthesize_stream(self.voice, self.text, self.root, values.append)
        self.assertEqual(sum(len(base64.b64decode(value["pcm"])) for value in values), 140000)
        self.assertTrue(all(len(base64.b64decode(value["pcm"])) <= VOICE.PCM_CHUNK_LIMIT for value in values))
        with patch.object(self.model, "generate_audio_stream", return_value=iter(())):
            # Pocket returns a real generator; use one here for close().
            with patch.object(self.model, "generate_audio_stream", side_effect=lambda *args, **kwargs: (item for item in ())):
                with self.assertRaisesRegex(VOICE.VoiceError, "no speech"):
                    VOICE._synthesize_stream(self.voice, self.text, self.root, values.append)

    def test_stdout_diagnostics_cannot_pollute_worker_frames(self):
        def prepare(*args):
            print("fixture dependency diagnostic")
        with patch.object(VOICE, "_prepared_voice", side_effect=prepare), patch.object(sys, "stderr", io.StringIO()) as diagnostics:
            code, values = self.run_worker([["prepare", str(self.voice)]])
        self.assertEqual(values, [{"ok": True}])
        self.assertIn("fixture dependency diagnostic", diagnostics.getvalue())

    def test_worker_cli_redirects_native_and_python_stdout(self):
        wrapper = """import importlib.util, os, sys
spec = importlib.util.spec_from_file_location('fixture_voice_worker', sys.argv[1])
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
def prepare(*args):
    print('python dependency diagnostic')
    os.write(1, b'native dependency diagnostic\\n')
module._prepared_voice = prepare
sys.argv = [sys.argv[1], 'worker']
raise SystemExit(module.main())
"""
        result = subprocess.run([sys.executable, "-I", "-c", wrapper, str(FIXTURE.HELPER_PATH)],
                                input=frame(["prepare", str(self.voice)]), capture_output=True, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(read_frames(result.stdout), [{"ok": True}])
        self.assertIn(b"python dependency diagnostic", result.stderr)
        self.assertIn(b"native dependency diagnostic", result.stderr)

    def test_voice_container_cache_invalidates_after_save_and_is_bounded(self):
        with patch.object(VOICE, "_voice_document", wraps=VOICE._voice_document) as reader:
            VOICE._cached_voice_document(self.voice)
            VOICE._cached_voice_document(self.voice)
            self.assertEqual(reader.call_count, 1)
            info = self.voice.stat()
            os.utime(self.voice, ns=(info.st_atime_ns, info.st_mtime_ns + 10000000))
            VOICE._cached_voice_document(self.voice)
            self.assertEqual(reader.call_count, 2)
            for index in range(3):
                filename = self.root / f"voice-{index}.zvoice"
                filename.write_bytes(self.voice.read_bytes())
                VOICE._cached_voice_document(filename)
            self.assertEqual(len(VOICE._voice_cache), 2)

    def test_model_cache_reuses_identity_and_invalidates_changed_assets(self):
        # Exercise actual model localization/fingerprinting with tiny local files
        # and a fixture backend, not downloaded weights or executable checkpoints.
        self.loader.stop()
        weights = self.root / "weights.safetensors"
        tokenizer = self.root / "tokenizer.model"
        config = self.root / "model.yaml"
        weights.write_bytes(b"fixture weights")
        tokenizer.write_bytes(b"fixture tokenizer")
        configuration = {"weights_path": str(weights), "flow_lm": {"lookup_table": {"tokenizer_path": str(tokenizer)}}}
        config.write_text(json.dumps(configuration), encoding="utf-8")
        workspace = self.root / "localized"
        workspace.mkdir()
        backend = types.SimpleNamespace(TTSModel=types.SimpleNamespace(load_model=lambda **kwargs: self.model))
        yaml = types.SimpleNamespace(safe_load=json.loads, safe_dump=json.dumps)
        with patch.dict(os.environ, {"ZSHARP_VOICE_MODEL_CONFIG": str(config)}), \
                patch.dict(sys.modules, {"yaml": yaml, "pocket_tts": backend}), \
                patch.object(VOICE.importlib.metadata, "version", return_value="fixture"), \
                patch.object(sys, "addaudithook"), \
                patch.object(VOICE, "_hash_file", wraps=VOICE._hash_file) as hashes:
            first = VOICE._load_model(workspace)
            second = VOICE._load_model(workspace)
            self.assertEqual(first, second)
            self.assertEqual(hashes.call_count, 2)
            weights.write_bytes(b"modified fixture weights")
            third = VOICE._load_model(workspace)
            self.assertNotEqual(first[1], third[1])
            self.assertEqual(hashes.call_count, 4)
        self.loader.start()


if __name__ == "__main__":
    unittest.main()
