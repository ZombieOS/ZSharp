"""Minimal subprocess bridge used by the Z# VM's bundled CPython runtime."""

from __future__ import annotations

import importlib.util
import pathlib
import sys
import traceback
import types
import contextlib
import os

modules = {}


def export(function):
    function.__zsharp_export__ = True
    return function


api = types.ModuleType("zsharp")
api.export = export
sys.modules["zsharp"] = api


def decode_argument(line: str):
    kind, _, payload = line.rstrip("\n").partition("\t")
    if kind == "text":
        return bytes.fromhex(payload).decode("utf-8")
    if kind == "number":
        return float(payload) if "." in payload else int(payload)
    if kind == "status":
        return payload == "1"
    if kind == "null":
        return None
    raise TypeError(f"unsupported Z# argument type: {kind}")


def encode_result(value) -> tuple[str, str]:
    if value is None:
        return "null", ""
    if isinstance(value, bool):
        return "status", "1" if value else "0"
    if isinstance(value, (int, float)) and not isinstance(value, bool):
        return "number", str(value)
    if isinstance(value, str):
        return "text", value.encode("utf-8").hex()
    raise TypeError(
        "Python returned an unsupported value; expected str, int, float, bool, or None"
    )


def call(module_name, function_name, input_name, output_name) -> int:
    module_path = pathlib.Path(module_name).resolve()
    input_path = pathlib.Path(input_name)
    output_path = pathlib.Path(output_name)
    try:
        lines = input_path.read_text(encoding="utf-8").splitlines()
        arguments = [decode_argument(line) for line in lines]
        module = modules.get(module_path)
        if module is None:
            module_id = f"zsharp_user_{len(modules)}_{module_path.stem}"
            specification = importlib.util.spec_from_file_location(module_id, module_path)
            if specification is None or specification.loader is None:
                raise ImportError(f"could not load Python module {module_path}")
            module = importlib.util.module_from_spec(specification)
            sys.modules[module_id] = module
            try:
                specification.loader.exec_module(module)
            except BaseException:
                sys.modules.pop(module_id, None)
                raise
            modules[module_path] = module
        function = getattr(module, function_name, None)
        if not callable(function):
            raise AttributeError(f"Python function '{function_name}' was not found")
        if not getattr(function, "__zsharp_export__", False):
            raise PermissionError(
                f"Python function '{function_name}' is not exported; add @export"
            )
        kind, payload = encode_result(function(*arguments))
        output_path.write_text(f"OK\t{kind}\t{payload}\n", encoding="utf-8")
        return 0
    except BaseException:
        report = traceback.format_exc()
        output_path.write_text(
            "ERROR\t" + report.encode("utf-8").hex() + "\n", encoding="utf-8"
        )
        return 1


def main():
    if sys.argv[1:] != ["--worker"]:
        return call(*sys.argv[1:])
    # Keep protocol acknowledgements separate from user print() output.
    if sys.stderr is None:
        sys.stderr = open(os.devnull, "w")
    protocol = os.fdopen(os.dup(sys.stdout.fileno()), "w", encoding="ascii", buffering=1, newline="\n")
    os.dup2(sys.stderr.fileno(), sys.stdout.fileno())
    for request in sys.stdin:
        fields = [bytes.fromhex(value).decode("utf-8")
                  for value in request.rstrip("\n").split("\t")]
        with contextlib.redirect_stdout(sys.stderr):
            call(*fields)
        protocol.write("DONE\n")
        protocol.flush()
    # ZVM bounds shutdown even if a user-created non-daemon thread stays alive.
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
