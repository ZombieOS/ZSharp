"""Owned subprocess checks for the persistent ABI-2 Python bridge."""

import json
import os
import pathlib
import queue
import subprocess
import sys
import tempfile
import threading
import unittest


ROOT = pathlib.Path(__file__).resolve().parents[1]
WORKER = ROOT / "native" / "interop" / "python" / "zsharp_worker.py"
MODULE = ROOT / "tests" / "native-interop-v2" / "python_fixture.py"


class WorkerTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="zsharp-python-interop-")
        self.marker = pathlib.Path(self.temp.name) / "normal-exit.txt"
        environment = dict(os.environ, ZSHARP_PYTHON_EXIT_MARKER=str(self.marker))
        self.process = subprocess.Popen([sys.executable, "-u", str(WORKER)],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            cwd=ROOT, env=environment)
        self.frames = queue.Queue()
        def receive():
            for line in self.process.stdout:
                try:
                    self.frames.put(json.loads(line))
                except BaseException as error:
                    self.frames.put(error)
        self.reader = threading.Thread(target=receive, daemon=True)
        self.reader.start()
        self.identifier = 0

    def tearDown(self):
        self.process.stdin.close()
        try:
            self.process.wait(timeout=5)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait(timeout=5)
            self.fail("Python worker did not exit normally on EOF")
        self.reader.join(timeout=2)
        self.process.stdout.close()
        self.logs = self.process.stderr.read().decode("utf-8", "replace")
        self.process.stderr.close()
        self.assertEqual(self.process.returncode, 0)
        if getattr(self, "expect_logs", False):
            self.assertIn("Python user print is a log, not protocol", self.logs)
            self.assertIn("Python native stdout is a log, not protocol", self.logs)
        # Every test loads MODULE, whose atexit hook must run at normal EOF.
        self.assertEqual(self.marker.read_text(encoding="utf-8"), "clean exit")
        self.temp.cleanup()

    def send(self, request):
        self.process.stdin.write(json.dumps(request, ensure_ascii=False).encode("utf-8") + b"\n")
        self.process.stdin.flush()

    def receive(self):
        value = self.frames.get(timeout=5)
        if isinstance(value, BaseException):
            raise value
        return value

    def request(self, op="call", target="echo", args=None, **fields):
        self.identifier += 1
        request = {"id": self.identifier, "op": op, "module": str(MODULE),
                   "target": target, "args": [] if args is None else args}
        request.update(fields)
        self.send(request)
        response = self.receive()
        self.assertEqual(response.get("id"), self.identifier)
        self.assertEqual(response.get("op"), "result")
        return response

    def value(self, *args, **kwargs):
        result = self.request(*args, **kwargs)
        self.assertTrue(result["ok"], result.get("error"))
        return result["value"]

    def test_scalars_arrays_records_exact_numbers_and_persistence(self):
        values = [None, True, False, "hello ✨\nworld", {"$number": "1234567890123456789012345678901234567890"},
                  {"$number": "0.0000000000000000000000000000000000000000000000001"},
                  {"$record": {"name": "Ada", "items": [{"$number": "42"}]}}]
        for value in values:
            self.assertEqual(self.value(args=[value]), value)
        self.assertEqual(self.value(target="next_value"), {"$number": "1"})
        self.assertEqual(self.value(target="next_value"), {"$number": "2"})
        self.assertEqual(self.value(target="exact"), [
            {"$number": "1" + "0" * 100}, {"$number": "0." + "0" * 48 + "1"}])

    def test_objects_identity_public_members_and_release(self):
        person = self.value("new", "Person", ["Ada"])
        reference = person["$ref"]
        self.assertEqual(person["$type"], "Person")
        self.assertEqual(self.value("get", ref=reference, member="name"), "Ada")
        self.assertIsNone(self.value("set", args=["Grace"], ref=reference, member="name"))
        self.assertEqual(self.value("invoke", args=["!"], ref=reference, member="greet"), "Grace!")
        self.assertEqual(self.value("invoke", ref=reference, member="identity"), person)
        self.assertEqual(self.value(target="Person.static_greeting"), "static export")
        for member in ("_secret", "__class__", "name.missing"):
            self.assertFalse(self.request("get", ref=reference, member=member)["ok"])
        self.assertIsNone(self.value("release", ref=reference))
        self.assertFalse(self.request("get", ref=reference, member="name")["ok"])
        next_person = self.value("new", "Person", ["Second"])
        self.assertNotEqual(reference, next_person["$ref"])

    def test_callbacks_host_objects_and_reentrant_guard(self):
        self.send({"id": "outer", "op": "call", "module": str(MODULE), "target": "callback", "args": ["Ada"]})
        callback = self.receive()
        self.assertEqual(callback["op"], "host.call")
        self.assertEqual(callback["target"], "Main:Main:Callback")
        self.assertEqual(callback["args"], ["Ada"])
        self.send({"id": "nested", "op": "call", "module": str(MODULE), "target": "next_value", "args": []})
        denied = self.receive()
        self.assertFalse(denied["ok"])
        self.assertIn("Reentrant", denied["error"])
        self.send({"id": callback["id"], "op": "result", "ok": True, "value": "Hello Ada"})
        self.assertEqual(self.receive(), {"id": "outer", "op": "result", "ok": True, "value": "Hello Ada"})
        self.send({"id": "object", "op": "call", "module": str(MODULE), "target": "host_object",
                   "args": [{"$zref": "7", "$type": "Main.Room"}]})
        for op, value in (("host.get", {"$number": "10"}), ("host.set", None), ("host.invoke", "called")):
            callback = self.receive()
            self.assertEqual(callback["op"], op)
            self.assertEqual(callback["ref"], "7")
            if op == "host.set": self.assertEqual(callback["args"], [{"$number": "11"}])
            self.send({"id": callback["id"], "op": "result", "ok": True, "value": value})
        response = self.receive()
        self.assertTrue(response["ok"])
        self.assertEqual(response["value"], {"$record": {"before": {"$number": "10"}, "result": "called",
                         "proxy": {"$zref": "7", "$type": "Main.Room"}}})
        self.assertTrue(self.value(target="background_callback"))

    def test_errors_recover_without_losing_module_state(self):
        self.assertEqual(self.value(target="next_value"), {"$number": "1"})
        for request in ({"target": "hidden"}, {"target": "fail"}, {"args": [42]},
                        {"args": [{"$number": "NaN"}]}, {"args": [{"$number": "1", "$ref": "1"}]}):
            self.assertFalse(self.request(**request)["ok"])
        self.assertEqual(self.value(target="next_value"), {"$number": "2"})
        self.assertEqual(self.value(target="logs"), "logged")
        self.expect_logs = True
        # Module state and request handling remain usable after an oversized result.
        oversized = self.request(target="too_large")
        self.assertFalse(oversized["ok"])
        self.assertIn("1 MiB", oversized["error"])
        result = self.request(args=["x" * (1024 * 1024 - 200)])
        self.assertTrue(result["ok"])
        self.assertEqual(self.value(target="next_value"), {"$number": "3"})


if __name__ == "__main__":
    unittest.main()
