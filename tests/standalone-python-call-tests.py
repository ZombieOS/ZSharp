"""Exercise standalone calls through the real native source/bytecode CLI."""

from __future__ import annotations

import argparse
from decimal import Decimal
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


FIXTURE = Path(__file__).resolve().parent / "standalone_python_call"
FORBIDDEN_HOOKS = (
    "CALL_START_MUST_NOT_RUN", "CALL_SHUTDOWN_MUST_NOT_RUN",
    "IMPORT_START_MUST_NOT_RUN", "IMPORT_SHUTDOWN_MUST_NOT_RUN",
)
OMITTED = object()


def number(value):
    return {"$number": str(value)}


def record(**fields):
    return {"$record": fields}


def assert_wire_equal(actual, expected, location="result"):
    # Compare decimal text without ever converting exact Z# numbers to floats.
    if isinstance(expected, dict) and set(expected) == {"$number"}:
        assert isinstance(actual, dict) and set(actual) == {"$number"}, (location, actual)
        assert isinstance(actual["$number"], str), (location, actual)
        assert Decimal(actual["$number"]) == Decimal(expected["$number"]), (location, actual, expected)
    elif isinstance(expected, dict):
        assert set(expected) == {"$record"}, (location, expected)
        assert isinstance(actual, dict) and set(actual) == {"$record"}, (location, actual)
        actual_fields, expected_fields = actual["$record"], expected["$record"]
        assert isinstance(actual_fields, dict) and set(actual_fields) == set(expected_fields), (location, actual, expected)
        for key, value in expected_fields.items():
            assert_wire_equal(actual_fields[key], value, f"{location}.{key}")
    elif isinstance(expected, list):
        assert isinstance(actual, list) and len(actual) == len(expected), (location, actual, expected)
        for index, value in enumerate(expected):
            assert_wire_equal(actual[index], value, f"{location}[{index}]")
    else:
        assert type(actual) is type(expected) and actual == expected, (location, actual, expected)


def main():
    arguments = argparse.ArgumentParser(description=__doc__)
    arguments.add_argument("binary", type=Path)
    options = arguments.parse_args()
    binary = options.binary.resolve(strict=True)
    checks = 0
    with tempfile.TemporaryDirectory(prefix="zsharp-standalone-call-") as temporary:
        directory = Path(temporary)
        project = directory / "project with spaces # unicode Ω"
        shutil.copytree(FIXTURE, project, ignore=shutil.ignore_patterns("__pycache__"))
        outside = directory / "outside project"
        outside.mkdir()
        environment = os.environ.copy()
        for key in tuple(environment):
            if key.startswith("ZSHARP_MAVEN_") or key == "ZSHARP_JVM_CLASSPATH":
                environment.pop(key)
        for key, folder in (("XDG_CONFIG_HOME", "config"), ("XDG_CACHE_HOME", "cache"),
                            ("XDG_DATA_HOME", "data")):
            environment[key] = str(directory / folder)
        for key in ("ZSHARP_PROJECT_REGISTRY", "ZSHARP_PACKAGE_REGISTRY", "ZSHARP_PLAY_STATS_REGISTRY"):
            environment[key] = str(directory / (key.lower() + ".json"))
        environment["ZSHARP_SKIP_UPDATE_CHECK"] = "1"
        environment["ZSHARP_SKIP_DESKTOP_INTEGRATION"] = "1"
        environment["PYTHONDONTWRITEBYTECODE"] = "1"
        request = directory / "request with spaces.json"
        response = directory / "response with spaces.json"

        def invoke(command):
            result = subprocess.run([str(binary), *map(str, command)], cwd=outside,
                                    env=environment, capture_output=True, text=True,
                                    encoding="utf-8", timeout=15)
            assert result.returncode >= 0, ("native process crashed", command,
                                            result.returncode, result.stdout, result.stderr)
            for marker in FORBIDDEN_HOOKS:
                assert marker not in result.stdout + result.stderr, (marker, command, result.stdout)
            return result

        def call(name, target, values=OMITTED, expected=OMITTED, *, source="Api.zsharp",
                 raw=None, failure=None, no_initializers=False, extra=()):
            nonlocal checks
            response.unlink(missing_ok=True)
            command = ["call", project / source, target, "--result-file", response]
            if raw is not None or values is not OMITTED:
                request.write_bytes(raw if isinstance(raw, bytes) else
                                    (raw if raw is not None else json.dumps(
                                        values, ensure_ascii=True, separators=(",", ":"))).encode("utf-8"))
                command += ["--args-file", request]
            result = invoke([*command, *extra])
            output = result.stdout + result.stderr
            assert response.exists(), (name, "missing response envelope", result.returncode, output)
            envelope = json.loads(response.read_text(encoding="utf-8"))
            if failure is None:
                assert result.returncode == 0, (name, output, envelope)
                assert isinstance(envelope, dict) and set(envelope) == {"ok", "value"}, (name, envelope)
                assert envelope["ok"] is True, (name, envelope)
                assert expected is not OMITTED, name
                assert_wire_equal(envelope["value"], expected, name)
            else:
                assert result.returncode != 0, (name, output, envelope)
                assert isinstance(envelope, dict) and set(envelope) == {"ok", "error"}, (name, envelope)
                assert envelope["ok"] is False and isinstance(envelope["error"], str), (name, envelope)
                assert envelope["error"], (name, envelope)
                assert not failure or failure.lower() in envelope["error"].lower(), (name, failure, envelope)
            if no_initializers:
                assert "CALL_CONSTRUCTOR_RAN" not in output, (name, "invalid request ran initializers", output)
            checks += 1
            return result, envelope

        nested = record(name="Måne 🌙", exact=number("1234567890123456789012345678901234567890"),
                        enabled=True, absent=None,
                        child=record(values=[number(1), number("-0.00001")], labels=["x", "y"],
                                     empty=[], literal=record(**{"$number": "literal field"})),
                        items=[record(index=number(1)), record(index=number(2))])
        # Both source and bytecode must use the same public visibility, codec,
        # object initialization, imports and one-shot lifecycle behavior.
        for source in ("Api.zsharp", "Api.zbc"):
            if source.endswith(".zbc"):
                compiled = invoke(["compile", project / "Api.zsharp", "-o", project / source])
                assert compiled.returncode == 0, compiled.stdout + compiled.stderr
                checks += 1
            label = source.rsplit(".", 1)[-1]
            call(f"{label}: exact integer addition", "Api:Add", [number("9007199254740993"), number(7)],
                 number("9007199254741000"), source=source)
            for value in ("-1234567890123456789012345678901234567890", "1e-30", "0.12345678901234567890123456789"):
                call(f"{label}: exact numeric {value}", "Api:EchoNumber", [number(value)], number(value), source=source)
            text_value = 'Måne 🌙\n\tquote " slash ' + chr(92)
            call(f"{label}: text", "Api:EchoText", [text_value], text_value, source=source)
            call(f"{label}: status alive", "Api:EchoStatus", [True], True, source=source)
            call(f"{label}: status dead", "Api:EchoStatus", [False], False, source=source)
            call(f"{label}: nested record", "Api:EchoObject", [nested], nested, source=source)
            call(f"{label}: empty record", "Api:EchoObject", [record()], record(), source=source)
            call(f"{label}: explicit null result", "Api:NullValue", expected=None, source=source)
            no_feed, _ = call(f"{label}: void brain result", "Api:NoFeed", expected=None, source=source)
            assert "CALL_TARGET_RAN" in no_feed.stdout
            call(f"{label}: number array result", "Api:NumberArray", expected=[number(1), number("9007199254740993"), number("0.25")], source=source)
            call(f"{label}: text array result", "Api:TextArray", expected=["alpha", "beta"], source=source)
            call(f"{label}: empty array result", "Api:EmptyArray", expected=[], source=source)
            initialized, _ = call(f"{label}: room constructor", "Api:Initialized", expected=number(41), source=source)
            assert "CALL_CONSTRUCTOR_RAN" in initialized.stdout
            call(f"{label}: fresh state first", "Api:Increment", expected=number(42), source=source)
            call(f"{label}: fresh state second", "Api:Increment", expected=number(42), source=source)
            call(f"{label}: imported function", "Api:Imported", [number(14)], number(42), source=source)
            call(f"{label}: import shares module within call", "Api:ImportedTwice", expected=number(12), source=source)
            for target in ("Api:Hidden", "Secret:VisibleMember", "FileOnly:VisibleMember"):
                call(f"{label}: nonpublic {target}", target, failure="noticed", source=source, no_initializers=True)
            call(f"{label}: unknown room", "Missing:Add", failure="resolve", source=source, no_initializers=True)
            call(f"{label}: unknown function", "Api:Missing", failure="resolve", source=source, no_initializers=True)
            call(f"{label}: typed object cannot be spoofed", "Api:RequiresCounter", [record(Value=number(3))],
                 failure="argument", source=source)
            call(f"{label}: live object result", "Api:LiveObject", failure="live", source=source)
            call(f"{label}: nested live array result", "Api:LiveObjects", failure="live", source=source)

        # Arity/codec errors must be rejected before executing room constructors.
        call("arity missing", "Api:Add", [], failure="expects 2", no_initializers=True)
        call("arity excess", "Api:NullValue", [None], failure="expects 0", no_initializers=True)
        call("argument count bound", "Api:Add", [None] * 1025, failure="1024", no_initializers=True)
        call("wrong scalar type", "Api:EchoNumber", ["3"], failure="argument")
        call("wrong status type", "Api:EchoStatus", [number(1)], failure="argument")
        call("array parameters remain unsupported", "Api:EchoObject", [[number(1)]], failure="argument")
        call("raw JSON number", "Api:EchoNumber", [3], failure="wire", no_initializers=True)
        for label, raw in (("object root", "{}"), ("null root", "null"), ("malformed", "["),
                           ("trailing input", "[] false"), ("NaN", "[NaN]")):
            call(label, "Api:EchoObject", raw=raw, failure="JSON array", no_initializers=True)
        for label, value in (
            ("untagged record", {"x": "value"}),
            ("unknown tag", {"$unknown": "value"}),
            ("conflicting tags", {"$number": "1", "$record": {}}),
            ("extra field on number", {"$number": "1", "extra": "value"}),
            ("extra field on record", {"$record": {}, "extra": "value"}),
            ("record is array", {"$record": []}),
            ("record is null", {"$record": None}),
            ("number tag is numeric", {"$number": 3}),
            ("number tag is null", {"$number": None}),
            ("live foreign handle", {"$ref": "1", "$type": "Foreign"}),
            ("live Z# handle", {"$zref": "1", "$type": "Counter"}),
            ("nested live foreign handle", record(child={"$ref": "1"})),
            ("nested live Z# handle", record(child=[record(ref={"$zref": "1"})])),
            ("NUL record field", record(**{"bad\0name": "value"})),
            ("NUL text field", record(value="bad\0text")),
            ("mixed array", record(values=["x", number(1)])),
            ("status array", record(values=[True, False])),
            ("null array", record(values=[None])),
            ("nested array", record(values=[["x"]])),
        ):
            call(label, "Api:EchoObject", [value], failure="", no_initializers=True)
        for value in ("", "NaN", "Infinity", "-Infinity", "1 2", "0x10", "1.2.3", "1\0garbage", "1e99999", "1" * 8191):
            call(f"invalid numeric text {value[:18]!r}", "Api:EchoNumber", [number(value)], failure="", no_initializers=True)
        deep = "leaf"
        for _ in range(34):
            deep = record(child=deep)
        call("nested depth bound", "Api:EchoObject", [deep], failure="depth", no_initializers=True)
        call("array length bound", "Api:EchoObject", [record(values=["x"] * 65537)], failure="", no_initializers=True)
        call("request byte bound", "Api:EchoText", raw=b'["' + b'x' * (1024 * 1024) + b'"]',
             failure="1 MiB", no_initializers=True)
        call("request NUL bytes", "Api:EchoText", raw=b'["x"]\0', failure="NUL", no_initializers=True)

        for function, error in (("WrongReturn", "feed a number"), ("MissingReturn", "feed"), ("DivideByZero", "zero")):
            call(f"typed runtime failure {function}", f"Failures:{function}", source="Failures.zsharp", failure=error)
        call("missing import", "MissingImport:Run", source="MissingImport.zsharp", failure="import")
        for target in ("Api", "Api:Add:Extra", ":Add", "Api:", "9Api:Add", "Api:bad-name"):
            call(f"malformed target {target}", target, failure="target", no_initializers=True)
        call("unknown CLI option", "Api:NullValue", extra=("--unknown", "x"), failure="option", no_initializers=True)
        call("repeated CLI option", "Api:NullValue", extra=("--result-file", "other.json"), failure="repeated", no_initializers=True)

        call("missing source file", "Api:NullValue", source="DoesNotExist.zsharp", failure="")
        (project / "Broken.zbc").write_bytes(b"invalid bytecode")
        call("invalid bytecode", "Api:NullValue", source="Broken.zbc", failure="")
        settings_file = project / "project.zsettings"
        settings = settings_file.read_text(encoding="utf-8")
        try:
            settings_file.write_text(settings.replace('PID: "standalone_python_call_regression":',
                                                      'PID: "different_project":'), encoding="utf-8")
            call("bytecode PID mismatch", "Api:NullValue", source="Api.zbc", failure="PID")
        finally:
            settings_file.write_text(settings, encoding="utf-8")

        # Even failing calls cannot replace their project, request, or settings.
        request.write_text("[]", encoding="utf-8")
        for label, protected in (("source", project / "Api.zsharp"), ("settings", project / "project.zsettings"), ("request", request)):
            original = protected.read_bytes()
            result = invoke(["call", project / "Api.zsharp", "Api:NullValue", "--args-file", request, "--result-file", protected])
            assert result.returncode != 0 and "overwrite" in result.stderr, (label, result.stdout, result.stderr)
            assert protected.read_bytes() == original, (label, "protected input changed")
            checks += 1
        no_result = invoke(["call", project / "Api.zsharp", "Api:NullValue"])
        assert no_result.returncode != 0 and "result-file" in no_result.stderr, no_result.stderr
        checks += 1

    print(f"Standalone source/bytecode call regressions passed: {checks} checks")


if __name__ == "__main__":
    main()
