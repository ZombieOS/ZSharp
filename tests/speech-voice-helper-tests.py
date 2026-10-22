"""Fixture-only voice-container tests; no model, microphone, or speakers used."""
import base64
import hashlib
import importlib.util
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.dont_write_bytecode = True

HELPER_PATH = Path(__file__).resolve().parents[1] / "native/python/zsharp_voice.py"
SPEC = importlib.util.spec_from_file_location("zsharp_voice_test", HELPER_PATH)
VOICE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(VOICE)


def tensors(header=None, data=None):
    header = header or {"layer/cache": {"dtype": "F32", "shape": [2, 1, 1, 1, 1], "data_offsets": [0, 8]},
                        "layer/offset": {"dtype": "I64", "shape": [1], "data_offsets": [8, 16]}}
    encoded = json.dumps(header, separators=(",", ":")).encode()
    encoded += b" " * (-len(encoded) % 8)
    return struct.pack("<Q", len(encoded)) + encoded + (data if data is not None else struct.pack("<ffq", 0.5, 0.25, 1))


def document(state=None):
    state = state or tensors()
    return {"format": "ZVOICE", "version": 1, "type": "clone", "backend": "pocket-tts",
            "backendVersion": "fixture", "modelFingerprint": "fixture",
            "pitch": 100, "speed": 100, "volume": 100,
            "conditioning": {"encoding": "safetensors-base64", "sha256": hashlib.sha256(state).hexdigest(),
                             "data": base64.b64encode(state).decode("ascii")}}


class VoiceContainerTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="zsharp-voice-tests-")
        self.root = Path(self.temporary.name)

    def tearDown(self):
        self.temporary.cleanup()

    def write(self, value):
        path = self.root / "fixture.zvoice"
        path.write_text(json.dumps(value), encoding="utf-8")
        return path

    def test_nonexecutable_conditioning_round_trip(self):
        value, state = VOICE._voice_document(self.write(document()))
        self.assertEqual(state, tensors())
        self.assertEqual(value["backend"], "pocket-tts")

    def test_checksum_rejects_changed_state(self):
        value = document(); value["conditioning"]["sha256"] = "0" * 64
        with self.assertRaisesRegex(VOICE.VoiceError, "checksum"):
            VOICE._voice_document(self.write(value))

    def test_invalid_base64(self):
        value = document(); value["conditioning"]["data"] = "not!!base64"
        with self.assertRaises(VOICE.VoiceError):
            VOICE._voice_document(self.write(value))

    def test_no_pickle_or_python_backend(self):
        value = document(); value["backend"] = "python-pickle"
        with self.assertRaises(VOICE.VoiceError):
            VOICE._voice_document(self.write(value))

    def test_version_is_integer_not_bool_or_fraction(self):
        for version in (True, 1.5, "1", 2):
            with self.subTest(version=version):
                value = document(); value["version"] = version
                with self.assertRaises(VOICE.VoiceError):
                    VOICE._voice_document(self.write(value))

    def test_percent_types_and_bounds(self):
        for field, value in (("pitch", 0), ("speed", 401), ("volume", -1), ("pitch", True), ("speed", "100")):
            with self.subTest(field=field, value=value):
                data = document(); data[field] = value
                with self.assertRaises(VOICE.VoiceError):
                    VOICE._voice_document(self.write(data))

    def test_duplicate_keys_rejected(self):
        with self.assertRaisesRegex(VOICE.VoiceError, "Duplicate"):
            VOICE._json(b'{"version":1,"version":2}')

    def test_non_finite_json_rejected(self):
        for value in (b'{"pitch":NaN}', b'{"pitch":Infinity}'):
            with self.assertRaises(VOICE.VoiceError):
                VOICE._json(value)

    def test_header_length_cannot_escape_file(self):
        with self.assertRaises(VOICE.VoiceError):
            VOICE._validate_safetensors(struct.pack("<Q", 1024 * 1024) + b"{}")

    def test_unbounded_dimensions_rejected(self):
        state = tensors({"layer/cache": {"dtype": "F32", "shape": [2, 1, 9999999, 1, 1], "data_offsets": [0, 8]}})
        with self.assertRaises(VOICE.VoiceError):
            VOICE._validate_safetensors(state)

    def test_offset_cannot_expand_unbounded_state(self):
        with self.assertRaisesRegex(VOICE.VoiceError, "offset"):
            VOICE._validate_safetensors(tensors(data=struct.pack("<ffq", 0.5, 0.25, 9999999)))

    def test_pocket_330_zero_pad_state_supported(self):
        header = {"layer/cache": {"dtype": "F32", "shape": [2, 1, 1, 1, 1], "data_offsets": [0, 8]},
                  "layer/offset": {"dtype": "I64", "shape": [1], "data_offsets": [8, 16]},
                  "layer/pad": {"dtype": "I64", "shape": [1], "data_offsets": [16, 24]}}
        VOICE._validate_safetensors(tensors(header, struct.pack("<ffqq", 0.5, 0.25, 1, 0)))
        with self.assertRaisesRegex(VOICE.VoiceError, "unpadded"):
            VOICE._validate_safetensors(tensors(header, struct.pack("<ffqq", 0.5, 0.25, 1, 1)))

    def test_tensor_bytes_match_shape(self):
        state = tensors({"layer/cache": {"dtype": "F32", "shape": [2, 1, 1, 1, 1], "data_offsets": [0, 16]}})
        with self.assertRaises(VOICE.VoiceError):
            VOICE._validate_safetensors(state)

    def test_unknown_tensor_kind_rejected(self):
        with self.assertRaises(VOICE.VoiceError):
            VOICE._validate_safetensors(tensors({"layer/python": {"dtype": "F32", "shape": [4], "data_offsets": [0, 16]}}))

    def test_explicit_model_configuration_required(self):
        with patch.dict(os.environ, {"ZSHARP_SPEECH_HOME": str(self.root)}, clear=True):
            with self.assertRaisesRegex(VOICE.VoiceError, "[Nn]o model (has been|was) downloaded"):
                VOICE._load_model(self.root)

    def test_remote_config_cannot_download(self):
        with patch.dict(os.environ, {"ZSHARP_VOICE_MODEL_CONFIG": "https://example.invalid/model.yaml"}, clear=True):
            with self.assertRaisesRegex(VOICE.VoiceError, "absolute local YAML"):
                VOICE._load_model(self.root)

    def test_network_audit_denies_connections(self):
        for event in ("socket.connect", "socket.getaddrinfo", "urllib.Request"):
            with self.assertRaisesRegex(VOICE.VoiceError, "offline-only"):
                VOICE._deny_network(event, ())

    def test_cli_missing_model_returns_explicit_failure_and_no_voice(self):
        reference, result, output = self.root / "reference.wav", self.root / "result.txt", self.root / "created.zvoice"
        reference.write_bytes(b"fixture only - no model invocation")
        environment = dict(os.environ); environment.pop("ZSHARP_VOICE_MODEL_CONFIG", None)
        # Never discover a developer's real installed model in this negative test.
        environment["ZSHARP_SPEECH_HOME"] = str(self.root)
        process = subprocess.run([sys.executable, "-I", str(HELPER_PATH), "create", "--reference", str(reference),
                                  "--output", str(output), "--result", str(result)],
                                 env=environment, timeout=5, capture_output=True)
        self.assertEqual(process.returncode, 0)
        self.assertIn("ERROR\nGenuine voice cloning requires a local Pocket TTS model", result.read_text())
        self.assertFalse(output.exists())
        self.assertFalse(any(path.is_dir() for path in self.root.iterdir()))

    def test_result_protocol_uses_exact_lf_bytes_on_every_platform(self):
        result = self.root / "protocol.txt"
        arguments = [str(HELPER_PATH), "create", "--reference", str(self.root / "reference.wav"),
                     "--output", str(self.root / "voice.zvoice"), "--result", str(result)]
        with patch.object(sys, "argv", arguments), patch.object(VOICE, "_create"):
            self.assertEqual(VOICE.main(), 0)
        self.assertEqual(result.read_bytes(), b"OK\n")
        with patch.object(sys, "argv", arguments), patch.object(VOICE, "_create", side_effect=VOICE.VoiceError("fixture failure")):
            self.assertEqual(VOICE.main(), 0)
        self.assertEqual(result.read_bytes(), b"ERROR\nfixture failure")


if __name__ == "__main__":
    unittest.main()
