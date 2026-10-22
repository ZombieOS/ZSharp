"""Stdlib-only local voice import tests; no network, models, or audio devices."""
import hashlib
import importlib.util
import io
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from unittest.mock import patch, MagicMock

sys.dont_write_bytecode = True

INSTALLER_PATH = Path(__file__).resolve().parents[1] / "tools/install-speech-components.py"
SPEC = importlib.util.spec_from_file_location("zsharp_speech_installer_test", INSTALLER_PATH)
INSTALLER = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(INSTALLER)

REVISION = "9" * 40
MODEL_PATH = "languages/english/model.safetensors"
REFERENCE = "hf://kyutai/pocket-tts/" + MODEL_PATH + "@" + REVISION
PAYLOAD = b"owned test bytes, not an inference model"
SHA256 = hashlib.sha256(PAYLOAD).hexdigest()
FILENAME = "pocket-tts-english.safetensors"
REPOSITORY = INSTALLER_PATH.parents[1]
PYTHON_HELPERS = (
    "native/python/zsharp_bridge.py",
    "native/python/zsharp_network.py",
    "native/python/zsharp_voice.py",
    "native/python/zsharp_speech_input.py",
    "tools/install-speech-components.py",
)


def metadata():
    return {"sha": REVISION, "siblings": [{"rfilename": MODEL_PATH, "size": len(PAYLOAD),
            "lfs": {"size": len(PAYLOAD), "sha256": SHA256}}]}


class SpeechPackagingTests(unittest.TestCase):
    def copy_helpers(self, root):
        for relative in PYTHON_HELPERS:
            destination = root / relative
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(REPOSITORY / relative, destination)

    def assert_helpers(self, directory):
        for relative in PYTHON_HELPERS:
            self.assertEqual((directory / Path(relative).name).read_bytes(),
                             (REPOSITORY / relative).read_bytes())
        result = subprocess.run([sys.executable, "-I", str(directory / INSTALLER_PATH.name), "--help"],
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("--models-only", result.stdout)

    @unittest.skipUnless(shutil.which("pwsh") and shutil.which("tar"), "PowerShell and tar required")
    def test_release_python_archive_contains_runnable_speech_setup_and_helpers(self):
        workflow = (REPOSITORY / ".github/workflows/build-native-game-runtimes.yml").read_text(encoding="utf-8")
        bundle = workflow.split("      - name: Bundle the Python runtime\n", 1)[1].split("\n      - name:", 1)[0]
        # Execute the production packaging commands after the network/checksum
        # step against a tiny local Python archive, without downloading anything.
        commands = bundle[bundle.index("          New-Item -ItemType Directory -Path build/python-runtime"):]
        commands = "\n".join(line[10:] if line.startswith("          ") else line for line in commands.splitlines())
        with tempfile.TemporaryDirectory(prefix="zsharp-speech-package-") as temporary:
            root = Path(temporary)
            self.copy_helpers(root)
            (root / "stage/bin").mkdir(parents=True)
            marker = b"local Python runtime fixture"
            with tarfile.open(root / "python-download.tar.gz", "w:gz") as archive:
                member = tarfile.TarInfo("python/runtime-fixture.txt")
                member.size = len(marker)
                archive.addfile(member, io.BytesIO(marker))
            result = subprocess.run([shutil.which("pwsh"), "-NoProfile", "-NonInteractive", "-Command",
                                     "$ErrorActionPreference = 'Stop'\n" + commands],
                                    cwd=root, capture_output=True, text=True, timeout=60)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            with tarfile.open(root / "stage/bin/python-runtime.tar.gz", "r:gz") as archive:
                for relative in PYTHON_HELPERS:
                    member = archive.extractfile("python/" + Path(relative).name)
                    self.assertIsNotNone(member, relative)
                    self.assertEqual(member.read(), (REPOSITORY / relative).read_bytes())
            self.assert_helpers(root / "build/python-runtime/python")

    @unittest.skipUnless(shutil.which("cmake"), "CMake required")
    def test_cmake_install_contains_runnable_speech_setup_and_helpers(self):
        configuration = (REPOSITORY / "CMakeLists.txt").read_text(encoding="utf-8")
        commands = re.findall(r"install\(FILES\s+[^()]*?DESTINATION\s+bin/python\s*\)", configuration)
        self.assertTrue(commands, "No Python helper installation rules found")
        with tempfile.TemporaryDirectory(prefix="zsharp-speech-install-") as temporary:
            root = Path(temporary)
            self.copy_helpers(root)
            (root / "CMakeLists.txt").write_text(
                "cmake_minimum_required(VERSION 3.20)\nproject(speech_package NONE)\n" + "\n".join(commands),
                encoding="utf-8")
            for arguments in (("-S", str(root), "-B", str(root / "build")),
                              ("--install", str(root / "build"), "--prefix", str(root / "stage"))):
                result = subprocess.run([shutil.which("cmake"), *arguments], capture_output=True, text=True, timeout=60)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assert_helpers(root / "stage/bin/python")


class VoiceImportTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="zsharp-speech-installer-tests-")
        self.root = Path(self.temporary.name)
        self.home = self.root / "speech"
        self.document = INSTALLER.prepare_home(self.home)
        self.source = self.root / "browser-model.safetensors"
        self.source.write_bytes(PAYLOAD)
        self.destination = self.home / "downloads" / FILENAME

    def tearDown(self):
        self.temporary.cleanup()

    def opener(self, value=None, payload=None):
        opener = MagicMock()
        opener.open.return_value = io.BytesIO(payload if payload is not None else json.dumps(
            metadata() if value is None else value).encode())
        return patch.object(INSTALLER.urllib.request, "build_opener", return_value=opener), opener

    def import_model(self, value=None):
        context, _ = self.opener(value)
        with context:
            return INSTALLER.import_voice_weights(self.home, self.document, REFERENCE, self.source)

    def assert_not_imported(self):
        self.assertFalse(self.destination.exists())
        self.assertNotIn(FILENAME, self.document["artifacts"])
        self.assertFalse(any(self.home.rglob(".voice-import-*.tmp")))

    def test_matching_browser_file_is_verified_and_provenance_recorded(self):
        context, opener = self.opener()
        with context:
            output = INSTALLER.import_voice_weights(self.home, self.document, REFERENCE, self.source)
        self.assertEqual(output, self.destination)
        self.assertEqual(output.read_bytes(), PAYLOAD)
        self.assertEqual(self.source.read_bytes(), PAYLOAD)
        request = opener.open.call_args.args[0]
        expected_metadata = f"https://huggingface.co/api/models/kyutai/pocket-tts/revision/{REVISION}?blobs=true"
        self.assertEqual(request.full_url, expected_metadata)
        self.assertFalse(request.has_header("Authorization"))
        record = self.document["artifacts"][FILENAME]
        self.assertEqual(record["url"], f"https://huggingface.co/kyutai/pocket-tts/resolve/{REVISION}/{MODEL_PATH}")
        self.assertEqual(record["metadataUrl"], expected_metadata)
        self.assertEqual(record["bytes"], len(PAYLOAD))
        self.assertEqual(record["sha256"], SHA256)
        self.assertEqual(record["importedFrom"], str(self.source.resolve()))
        saved = json.loads((self.home / "components.json").read_text(encoding="utf-8"))
        self.assertEqual(saved["artifacts"][FILENAME], record)
        self.assertFalse(any(self.home.rglob(".voice-import-*.tmp")))

    def test_wrong_sha_preserves_source_and_does_not_publish(self):
        corrupted = b"X" + PAYLOAD[1:]
        self.source.write_bytes(corrupted)
        with self.assertRaisesRegex(RuntimeError, "SHA256"):
            self.import_model()
        self.assertEqual(self.source.read_bytes(), corrupted)
        self.assert_not_imported()

    def test_incomplete_file_is_rejected_before_publication(self):
        self.source.write_bytes(PAYLOAD[:-1])
        with self.assertRaisesRegex(RuntimeError, "wrong size|incomplete"):
            self.import_model()
        self.assert_not_imported()

    def test_non_safetensors_extension_is_rejected(self):
        self.source = self.root / "model.safetensors.partial"
        self.source.write_bytes(PAYLOAD)
        with self.assertRaisesRegex(RuntimeError, "regular .safetensors"):
            self.import_model()
        self.assert_not_imported()

    def test_directory_is_rejected(self):
        self.source = self.root / "directory.safetensors"
        self.source.mkdir()
        with self.assertRaisesRegex(RuntimeError, "regular .safetensors"):
            self.import_model()
        self.assert_not_imported()

    def test_unknown_existing_cache_is_not_adopted_or_overwritten(self):
        self.destination.parent.mkdir()
        self.destination.write_bytes(PAYLOAD)
        with self.assertRaisesRegex(RuntimeError, "unmanaged or changed"):
            self.import_model()
        self.assertEqual(self.destination.read_bytes(), PAYLOAD)
        self.assertNotIn(FILENAME, self.document["artifacts"])

    def test_managed_valid_cache_is_verified_idempotently(self):
        self.import_model()
        original = self.document["artifacts"][FILENAME].copy()
        self.source.unlink()
        self.assertEqual(self.import_model(), self.destination)
        self.assertEqual(self.document["artifacts"][FILENAME], original)

    def test_changed_managed_cache_is_not_overwritten(self):
        self.import_model()
        altered = b"X" + PAYLOAD[1:]
        self.destination.write_bytes(altered)
        with self.assertRaisesRegex(RuntimeError, "unmanaged or changed"):
            self.import_model()
        self.assertEqual(self.destination.read_bytes(), altered)

    def test_cache_manifest_cannot_override_official_hash(self):
        self.import_model()
        altered = b"X" + PAYLOAD[1:]
        self.destination.write_bytes(altered)
        self.document["artifacts"][FILENAME]["sha256"] = hashlib.sha256(altered).hexdigest()
        with self.assertRaisesRegex(RuntimeError, "unmanaged or changed"):
            self.import_model()
        self.assertEqual(self.destination.read_bytes(), altered)

    def test_interrupted_staging_is_cleaned_without_publishing(self):
        with patch.object(INSTALLER.os, "fsync", side_effect=OSError("fixture interrupted write")):
            with self.assertRaisesRegex(OSError, "interrupted"):
                self.import_model()
        self.assert_not_imported()

    def test_atomic_publication_does_not_replace_competing_cache_file(self):
        def competitor(staging, destination):
            self.assertEqual(Path(staging).read_bytes(), PAYLOAD)
            self.assertFalse(Path(destination).exists())
            Path(destination).write_bytes(b"competing file")
            raise FileExistsError("fixture concurrent cache creation")
        with patch.object(INSTALLER.os, "link", side_effect=competitor):
            with self.assertRaises(FileExistsError):
                self.import_model()
        self.assertEqual(self.destination.read_bytes(), b"competing file")
        self.assertNotIn(FILENAME, self.document["artifacts"])
        self.assertFalse(any(self.home.rglob(".voice-import-*.tmp")))

    def test_metadata_wrong_revision_is_rejected(self):
        value = metadata(); value["sha"] = "1" * 40
        with self.assertRaisesRegex(RuntimeError, "revision"):
            self.import_model(value)
        self.assert_not_imported()

    def test_metadata_missing_or_duplicate_asset_is_rejected(self):
        for siblings in ([], metadata()["siblings"] * 2):
            with self.subTest(siblings=siblings):
                value = metadata(); value["siblings"] = siblings
                with self.assertRaisesRegex(RuntimeError, "exactly one"):
                    self.import_model(value)
        self.assert_not_imported()

    def test_metadata_invalid_sizes_and_hashes_are_rejected(self):
        for size, lfs_size, sha in ((True, True, SHA256), (0, 0, SHA256),
                (INSTALLER.MAX_DOWNLOAD + 1, INSTALLER.MAX_DOWNLOAD + 1, SHA256),
                (len(PAYLOAD), 1, SHA256), (len(PAYLOAD), len(PAYLOAD), "invalid"),
                (len(PAYLOAD), len(PAYLOAD), "G" * 64)):
            with self.subTest(size=size, lfs_size=lfs_size, sha=sha):
                value = metadata(); value["siblings"][0].update(size=size, lfs={"size": lfs_size, "sha256": sha})
                with self.assertRaisesRegex(RuntimeError, "size and SHA256"):
                    self.import_model(value)
        self.assert_not_imported()

    def test_oversized_and_invalid_json_metadata_are_rejected(self):
        for payload, expected in ((b" " * (4 * 1024 * 1024 + 1), "size limit"), (b"not JSON", "valid JSON")):
            with self.subTest(expected=expected):
                context, _ = self.opener(payload=payload)
                with context, self.assertRaisesRegex(RuntimeError, expected):
                    INSTALLER.import_voice_weights(self.home, self.document, REFERENCE, self.source)
        self.assert_not_imported()

    def test_wrong_repository_or_unsafe_reference_never_requests_metadata(self):
        for reference in (REFERENCE.replace("kyutai/pocket-tts/", "other/model/"),
                REFERENCE.replace("languages/english", "../english"),
                REFERENCE.replace("languages/english", "%2e%2e/english"),
                REFERENCE.replace("languages/english", "languages\\english"),
                REFERENCE.replace("languages/english", "languages//english"),
                REFERENCE.replace("model.safetensors", "model.safetensors?token=x"),
                REFERENCE.replace("model.safetensors", "model.json"),
                REFERENCE.replace(REVISION, "main")):
            with self.subTest(reference=reference), patch.object(INSTALLER.urllib.request, "build_opener") as network:
                with self.assertRaises(RuntimeError):
                    INSTALLER.import_voice_weights(self.home, self.document, reference, self.source)
                network.assert_not_called()
        self.assert_not_imported()

    def test_source_symlink_is_not_read(self):
        link = self.root / "linked.safetensors"
        try:
            link.symlink_to(self.source)
        except (OSError, NotImplementedError):
            self.skipTest("Platform does not allow unprivileged symlinks")
        self.source = link
        with self.assertRaisesRegex(RuntimeError, "symlink"):
            self.import_model()
        self.assert_not_imported()

    def test_redirected_cache_is_rejected(self):
        outside = self.root / "outside"; outside.mkdir()
        try:
            (self.home / "downloads").symlink_to(outside, target_is_directory=True)
        except (OSError, NotImplementedError):
            self.skipTest("Platform does not allow unprivileged symlinks")
        with self.assertRaisesRegex(RuntimeError, "redirected path"):
            self.import_model()
        self.assertFalse(any(outside.iterdir()))


if __name__ == "__main__":
    unittest.main()
