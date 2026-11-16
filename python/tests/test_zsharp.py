"""Public standalone API tests. No network, user preferences, or installed ZVM."""
from contextlib import redirect_stderr, redirect_stdout
from decimal import Decimal
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "src"))
import zsharp


class StandaloneCalls(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="zsharp-python-test-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.project = self.root / "project 'with spaces$`literal`"
        self.project.mkdir()
        self.source = self.project / "Main.zsharp"
        self.source.write_text("owned test fixture\n")
        self.executable = self.root / "runtime 'with spaces$`literal`"
        self.executable.write_text("owned executable fixture\n")
        self.executable.chmod(0o755)
        self.client = zsharp.ZSharp(self.project, executable=self.executable)
        self.commands = []
        self.payloads = []
        self.logs = io.StringIO(), io.StringIO()

    def reply(self, value=None, *, raw=None, ok=True, diagnostic="native failure",
              status=0, stdout="", stderr=""):
        def complete(command, **options):
            self.commands.append((command, options))
            request = Path(command[command.index("--args-file") + 1])
            self.payloads.append(json.loads(request.read_text()))
            response = Path(command[command.index("--result-file") + 1])
            if raw is not None:
                response.write_bytes(raw)
            else:
                response.write_text(json.dumps({"ok": True, "value": value} if ok
                                               else {"ok": False, "error": diagnostic}))
            return subprocess.CompletedProcess(command, status, stdout, stderr)
        return complete

    def invoke(self, value=None, *arguments, target="Main:Tools:Echo", **reply):
        with patch("zsharp._run", side_effect=self.reply(value, **reply)) as run:
            with redirect_stdout(self.logs[0]), redirect_stderr(self.logs[1]):
                result = self.client.call(target, *arguments)
            return result, run

    def test_scalar_array_record_roundtrip_and_exact_numbers(self):
        huge = 10 ** 100
        precise = Decimal("0.12345678901234567890123456789")
        wire = {"$record": {"answer": {"$number": str(huge)}, "fraction": {"$number": str(precise)},
                            "items": ["text", True, None], "$number": "literal record key"}}
        value, _ = self.invoke(wire, huge, precise, 1.5, "hello", True, None,
                               {"items": (1, 2), "$number": "literal key"})
        self.assertEqual(value, {"answer": huge, "fraction": precise, "items": ["text", True, None],
                                 "$number": "literal record key"})
        self.assertIsInstance(value["fraction"], Decimal)
        self.assertEqual(self.payloads[0], [{"$number": str(huge)}, {"$number": str(precise)},
                                          {"$number": "1.5"}, "hello", True, None,
                                          {"$record": {"items": [{"$number": "1"}, {"$number": "2"}],
                                                       "$number": "literal key"}}])

    def test_large_integers_do_not_change_interpreter_digit_limit(self):
        limit = sys.get_int_max_str_digits() if hasattr(sys, "get_int_max_str_digits") else None
        value = 10 ** 5000
        returned, _ = self.invoke({"$number": "1" + "0" * 5000}, value)
        self.assertEqual(returned, value)
        self.assertEqual(self.payloads[0], [{"$number": "1" + "0" * 5000}])
        if limit is not None:
            self.assertEqual(sys.get_int_max_str_digits(), limit)

    def test_command_is_separate_arguments_with_private_json_and_project_cwd(self):
        result, run = self.invoke("returned", "'literal$`text`", stdout="normal Print\n", stderr="native log\n")
        self.assertEqual(result, "returned")
        command, options = self.commands[0]
        self.assertEqual(command[:4], [str(self.executable), "call", str(self.source), "Tools:Echo"])
        self.assertEqual(options["cwd"], self.project)
        self.assertEqual(options["timeout"], 30)
        self.assertNotIn("shell", options)
        self.assertEqual(self.logs[0].getvalue(), "normal Print\n")
        self.assertEqual(self.logs[1].getvalue(), "native log\n")
        self.assertFalse(Path(command[5]).exists())
        self.assertFalse(Path(command[7]).exists())
        run.assert_called_once()

    def test_relative_nested_and_explicit_extension_targets(self):
        (self.project / "tools").mkdir()
        nested = self.project / "tools" / "Other.zsharp"
        nested.write_text("owned nested fixture")
        self.invoke(None, target="tools/Other.zsharp:Tools:Echo")
        self.assertEqual(self.commands[0][0][2], str(nested))
        self.invoke(None, target="tools\\Other:Tools:Echo")
        self.assertEqual(self.commands[1][0][2], str(nested))

    def test_convenience_defaults_to_cwd_and_configured_calls_start_new_processes(self):
        with patch("zsharp.Path.cwd", return_value=self.project):
            with patch("zsharp._run", side_effect=self.reply({"$number": "42"})) as run:
                self.assertEqual(zsharp.call("Main:Tools:Echo", executable=self.executable), 42)
                self.assertEqual(self.client.call("Main:Tools:Echo"), 42)
                self.assertEqual(run.call_count, 2)
                self.assertNotEqual(self.commands[0][0][5], self.commands[1][0][5])

    def test_configured_and_per_call_timeout(self):
        client = zsharp.ZSharp(self.project, executable=self.executable, timeout=7)
        with patch("zsharp._run", side_effect=self.reply(None)):
            client.call("Main:Tools:Echo")
            client.call("Main:Tools:Echo", timeout=2)
            client.call("Main:Tools:Echo", timeout=None)
        self.assertEqual([options["timeout"] for _, options in self.commands], [7, 2, None])

    def test_timeout_is_diagnostic_and_forwards_partial_logs(self):
        failure = subprocess.TimeoutExpired([str(self.executable)], 3, output=b"partial\n", stderr=b"stalled\n")
        with patch("zsharp._run", side_effect=failure):
            with redirect_stdout(self.logs[0]), redirect_stderr(self.logs[1]):
                with self.assertRaises(zsharp.ZSharpError) as captured:
                    self.client.call("Main:Tools:Echo", timeout=3)
        self.assertTrue(captured.exception.timed_out)
        self.assertEqual(captured.exception.stdout, "partial\n")
        self.assertEqual(captured.exception.stderr, "stalled\n")
        self.assertEqual(self.logs[0].getvalue(), "partial\n")
        self.assertIn("3 seconds", str(captured.exception))

    def test_native_error_retains_diagnostic_logs_and_status(self):
        with self.assertRaises(zsharp.ZSharpError) as captured:
            self.invoke(ok=False, diagnostic="Main:7: unknown function", status=1,
                        stdout="Print before failure\n", stderr="native diagnostic\n")
        error = captured.exception
        self.assertIn("Main:7: unknown function", str(error))
        self.assertEqual(error.returncode, 1)
        self.assertEqual(error.stdout, "Print before failure\n")
        self.assertEqual(error.stderr, "native diagnostic\n")
        self.assertEqual(error.command[1], "call")

    def test_nonzero_exit_cannot_be_hidden_by_success_response(self):
        with self.assertRaisesRegex(zsharp.ZSharpError, "status 3"):
            self.invoke("untrusted success", status=3)

    def test_invalid_response_shapes_and_json_are_clear_errors(self):
        for raw in (b"", b"not json", b"[]", b"null", b"42", b'{"ok":true}',
                    b'{"ok":false}', b'{"ok":"true","value":null}',
                    b'{"ok":true,"ok":false,"value":null}',
                    b'{"ok":true,"value":NaN}', b"\xff"):
            with self.subTest(raw=raw):
                with self.assertRaises(zsharp.ZSharpError):
                    self.invoke(raw=raw, status=1)

    def test_unknown_ambiguous_or_live_result_tags_are_rejected(self):
        for value in (1, {"$number": "NaN"}, {"$number": "1", "extra": 2},
                      {"$ref": "dead", "$type": "Object"}, {"$zref": "expired"},
                      {"$record": []}, {"unknown": "tag"}):
            with self.subTest(value=value):
                with self.assertRaises(zsharp.ZSharpError):
                    self.invoke(value)

    def test_missing_result_is_a_transport_error(self):
        with patch("zsharp._run", return_value=subprocess.CompletedProcess([], 0, "", "")):
            with self.assertRaisesRegex(zsharp.ZSharpError, "missing Z# response"):
                self.client.call("Main:Tools:Echo")

    def test_launch_failure_is_custom_error(self):
        with patch("zsharp._run", side_effect=OSError("owned launch failure")):
            with self.assertRaisesRegex(zsharp.ZSharpError, "Could not start ZVM"):
                self.client.call("Main:Tools:Echo")

    def test_payload_and_result_size_limits(self):
        with patch("zsharp._run") as run:
            with self.assertRaisesRegex(ValueError, "1 MiB"):
                self.client.call("Main:Tools:Echo", "x" * (1024 * 1024))
            run.assert_not_called()
        with self.assertRaisesRegex(zsharp.ZSharpError, "1 MiB"):
            self.invoke(raw=b" " * (1024 * 1024 + 8193))
        # The CLI envelope adds bytes around an otherwise valid maximum value.
        value = "x" * (1024 * 1024 - 2)
        self.assertEqual(self.invoke(value)[0], value)
        with self.assertRaisesRegex(zsharp.ZSharpError, "1 MiB"):
            self.invoke(value + "x")

    def test_invalid_types_nonfinite_and_excessive_arguments_fail_before_launch(self):
        for value in (float("inf"), float("nan"), Decimal("NaN"), Decimal("1e10000000"),
                      {1: "non-text key"}, object()):
            with self.subTest(value=type(value).__name__):
                with patch("zsharp._run") as run:
                    with self.assertRaises((TypeError, ValueError)):
                        self.client.call("Main:Tools:Echo", value)
                    run.assert_not_called()
        with self.assertRaisesRegex(ValueError, "1024"):
            self.client.call("Main:Tools:Echo", *([None] * 1025))

    def test_recursive_and_deep_values_fail_with_bounded_diagnostics(self):
        cycle = []
        cycle.append(cycle)
        with self.assertRaisesRegex(ValueError, "nesting"):
            self.client.call("Main:Tools:Echo", cycle)
        value = None
        for _ in range(130):
            value = [value]
        with self.assertRaisesRegex(zsharp.ZSharpError, "nesting"):
            self.invoke(value)

    def test_invalid_targets_and_project_escape_fail_before_launch(self):
        for target in ("Main", "Main:Tools", "PID:Main:Tools:Echo", "Main::Echo",
                       "Main:Bad Room:Echo", "../Main:Tools:Echo", "/Main:Tools:Echo",
                       "tools/../Main:Tools:Echo", "Main\0:Tools:Echo"):
            with self.subTest(target=target):
                with patch("zsharp._run") as run:
                    with self.assertRaises(ValueError):
                        self.client.call(target)
                    run.assert_not_called()
        outside = self.root / "outside.zsharp"
        outside.write_text("never execute outside project")
        try:
            (self.project / "Escape.zsharp").symlink_to(outside)
        except (OSError, NotImplementedError):
            return
        with self.assertRaisesRegex(ValueError, "escapes"):
            self.client.call("Escape:Tools:Echo")

    def test_missing_source_and_project_have_clear_errors(self):
        with self.assertRaisesRegex(zsharp.ZSharpError, "source file was not found"):
            self.client.call("Absent:Tools:Echo")
        with self.assertRaisesRegex(zsharp.ZSharpError, "directory does not exist"):
            zsharp.ZSharp(self.root / "absent")

    def test_invalid_timeouts(self):
        for timeout in (0, -1, True, "5", float("nan"), float("inf")):
            with self.subTest(timeout=timeout):
                with self.assertRaises(ValueError):
                    zsharp.ZSharp(self.project, timeout=timeout)

    def test_explicit_and_environment_runtime_paths(self):
        with patch.dict(os.environ, {"ZSHARP_EXECUTABLE": str(self.executable)}):
            self.assertEqual(zsharp._executable(None), self.executable)
            with self.assertRaises(zsharp.ZSharpError):
                zsharp._executable(self.root / "missing explicit runtime")

    def test_path_runtime_and_default_install_locations(self):
        real_get = os.environ.get
        def environment(key, default=None):
            if key == "ZSHARP_EXECUTABLE":
                return None
            if key == "LOCALAPPDATA":
                return str(self.root / "local app data")
            return real_get(key, default)
        with patch("zsharp.os.environ.get", side_effect=environment):
            with patch("zsharp.shutil.which", return_value=str(self.executable)):
                self.assertEqual(zsharp._executable(None), self.executable)
            home = self.root / "private home"
            unix = home / ".local/bin/zsharp"
            unix.parent.mkdir(parents=True)
            unix.write_text("owned runtime fixture"); unix.chmod(0o755)
            windows = self.root / "local app data/ZombieOS/ZSharp/bin/zsharp.exe"
            windows.parent.mkdir(parents=True)
            windows.write_text("owned runtime fixture"); windows.chmod(0o755)
            with patch("zsharp.shutil.which", return_value=None), patch("zsharp.Path.home", return_value=home):
                self.assertEqual(zsharp._executable(None), unix)
                unix.unlink()
                self.assertEqual(zsharp._executable(None), windows)
                windows.unlink()
                with self.assertRaisesRegex(zsharp.ZSharpError, "ZVM was not found"):
                    zsharp._executable(None)

    def test_real_process_logging_uses_no_shell(self):
        result = zsharp._run([sys.executable, "-c",
                              "import sys; print(sys.argv[1]); print('error log', file=sys.stderr)",
                              "'literal$`not executed`"], cwd=self.project, timeout=5)
        self.assertEqual(result.returncode, 0)
        self.assertEqual(result.stdout, "'literal$`not executed`\n")
        self.assertEqual(result.stderr, "error log\n")

    @unittest.skipUnless(os.name == "posix", "POSIX independent worker groups")
    def test_timeout_remains_bounded_when_worker_keeps_logging_handles_open(self):
        code = ("import subprocess,sys,time; "
                "child=subprocess.Popen([sys.executable,'-c','import time; time.sleep(30)'],start_new_session=True); "
                "print(child.pid,flush=True); print('before timeout',file=sys.stderr,flush=True); time.sleep(30)")
        start = time.monotonic()
        worker = None
        try:
            with self.assertRaises(subprocess.TimeoutExpired) as captured:
                zsharp._run([sys.executable, "-c", code], cwd=self.project, timeout=0.5)
            worker = int(captured.exception.stdout.strip())
            self.assertLess(time.monotonic() - start, 3)
            self.assertEqual(captured.exception.stderr, "before timeout\n")
            # This independently grouped child deliberately models the current
            # native foreign-worker lifetime. The test owns and removes it.
        finally:
            if worker is not None:
                try:
                    os.kill(worker, 9)
                except ProcessLookupError:
                    pass


if __name__ == "__main__":
    unittest.main()
