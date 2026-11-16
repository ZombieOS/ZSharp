"""Opt-in tests against the actual local ZVM, with an owned fixture copy.

Set ZSHARP_TEST_EXECUTABLE to the native binary before unittest discovery.
"""
from contextlib import redirect_stderr, redirect_stdout
from decimal import Decimal
import io
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
from zsharp import ZSharp, ZSharpError, call

_EXECUTABLE = os.environ.get("ZSHARP_TEST_EXECUTABLE")
_FIXTURE = Path(__file__).resolve().parents[2] / "tests" / "standalone_python_call"


@unittest.skipUnless(_EXECUTABLE and _FIXTURE.is_dir(), "set ZSHARP_TEST_EXECUTABLE for native integration")
class NativeCalls(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix="zsharp-python-native-")
        cls.addClassCleanup(cls.temporary.cleanup)
        root = Path(cls.temporary.name)
        cls.project = root / "project with 'quotes$`literal`"
        shutil.copytree(_FIXTURE, cls.project)
        environment = patch.dict(os.environ, {"ZSHARP_SKIP_UPDATE_CHECK": "1",
                                             "XDG_CONFIG_HOME": str(root / "config"),
                                             "XDG_DATA_HOME": str(root / "data")})
        environment.start()
        cls.addClassCleanup(environment.stop)
        cls.api = ZSharp(cls.project, executable=_EXECUTABLE)

    def setUp(self):
        self.stdout, self.stderr = io.StringIO(), io.StringIO()
        out = redirect_stdout(self.stdout); out.__enter__(); self.addCleanup(out.__exit__, None, None, None)
        err = redirect_stderr(self.stderr); err.__enter__(); self.addCleanup(err.__exit__, None, None, None)

    def test_public_function_and_module_convenience(self):
        self.assertEqual(call("Api:Api:Add", 20, 22, project=self.project, executable=_EXECUTABLE), 42)
        self.assertEqual(self.api.call("Api:Api:EchoText", "quote' $ ` literal\nsecond line"),
                         "quote' $ ` literal\nsecond line")

    def test_exact_numbers_status_and_null(self):
        precise = Decimal("0.12345678901234567890123456789")
        self.assertEqual(self.api.call("Api:Api:EchoNumber", precise), precise)
        self.assertEqual(self.api.call("Api:Api:EchoNumber", 10 ** 100), 10 ** 100)
        self.assertIs(self.api.call("Api:Api:EchoStatus", True), True)
        self.assertIsNone(self.api.call("Api:Api:NullValue"))

    def test_plain_records_and_supported_arrays(self):
        record = {"nested": {"values": [1, 2]}, "$number": "ordinary key", "text": "literal"}
        self.assertEqual(self.api.call("Api:Api:EchoObject", record), record)
        self.assertEqual(self.api.call("Api:Api:NumberArray"), [1, 9007199254740993, Decimal("0.25")])
        self.assertEqual(self.api.call("Api:Api:TextArray"), ["alpha", "beta"])
        self.assertEqual(self.api.call("Api:Api:EmptyArray"), [])

    def test_initialization_fresh_state_and_automatic_handlers(self):
        self.assertEqual(self.api.call("Api:Api:Increment"), 42)
        self.assertEqual(self.api.call("Api:Api:Increment"), 42)
        self.assertIn("CALL_CONSTRUCTOR_RAN", self.stdout.getvalue())
        self.assertNotIn("CALL_START_MUST_NOT_RUN", self.stdout.getvalue())
        self.assertNotIn("CALL_SHUTDOWN_MUST_NOT_RUN", self.stdout.getvalue())

    def test_existing_imports_execute(self):
        self.assertEqual(self.api.call("Api:Api:Imported", 7), 21)

    def test_visibility_and_expired_handles_fail_clearly(self):
        for target in ("Api:Api:Hidden", "Api:Secret:VisibleMember", "Api:FileOnly:VisibleMember"):
            with self.subTest(target=target):
                with self.assertRaises(ZSharpError) as captured:
                    self.api.call(target)
                self.assertNotEqual(captured.exception.returncode, 0)
        with self.assertRaises(ZSharpError) as captured:
            self.api.call("Api:Api:LiveObject")
        self.assertIn("object", str(captured.exception).lower())


if __name__ == "__main__":
    unittest.main()
