"""Persistent desktop ZVM Python interop worker (tagged JSON lines, ABI 2).

This is not a security sandbox. Only explicitly exported Python functions and
classes are reachable through the bridge. Host callbacks are synchronous and
thread-bound; user background threads cannot retain a live ZVM call context.
"""

from __future__ import annotations

import hashlib
import importlib.util
import inspect
import json
import math
import os
import pathlib
import re
import sys
import threading
import types
from decimal import Decimal


FRAME_LIMIT = 1024 * 1024
DEPTH_LIMIT = 128
IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z_0-9]*\Z")
DECIMAL = re.compile(r"[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?\Z")
_connection = None


class TransportClosed(Exception):
    pass


def export(value):
    """The existing identity decorator, now also usable on exported classes."""
    if not (inspect.isfunction(value) or inspect.isclass(value)):
        raise TypeError("@export requires a Python function or class")
    value.__zsharp_export__ = True
    return value


def _host():
    if _connection is None:
        raise RuntimeError("Z# callbacks are only available inside a ZVM worker call")
    return _connection


def call(target, *arguments):
    if not isinstance(target, str) or not target.strip():
        raise ValueError("Z# callback target must be a non-empty File:Room:Function")
    return _host().callback("host.call", target=target, arguments=arguments)


def _reference(value):
    if isinstance(value, ZSharpObject):
        return value.reference
    if not isinstance(value, str) or not value or "\0" in value:
        raise ValueError("Z# object reference must be a live proxy or non-empty handle")
    return value


def _member(value):
    if not isinstance(value, str) or not IDENTIFIER.fullmatch(value) or value.startswith("_"):
        raise ValueError("Object member must be a public identifier")
    return value


def get(reference, member):
    return _host().callback("host.get", reference=_reference(reference), member=_member(member))


def set(reference, member, value):
    return _host().callback("host.set", reference=_reference(reference), member=_member(member), arguments=[value])


def invoke(reference, member, *arguments):
    return _host().callback("host.invoke", reference=_reference(reference), member=_member(member), arguments=arguments)


class ZSharpObject:
    """A live Z# instance proxy: fields remain owned by the VM, not copied."""

    __slots__ = ("_reference", "_type_name")

    def __init__(self, reference, type_name):
        self._reference = _reference(reference)
        self._type_name = type_name

    @property
    def reference(self):
        return self._reference

    @property
    def type_name(self):
        return self._type_name

    def get(self, member):
        return get(self, member)

    def set(self, member, value):
        return set(self, member, value)

    def invoke(self, member, *arguments):
        return invoke(self, member, *arguments)


def _install_sdk():
    api = types.ModuleType("zsharp")
    api.export = export
    api.call = call
    api.get = get
    api.set = set
    api.invoke = invoke
    api.Object = api.ZSharpObject = ZSharpObject
    api.ZSharp = api
    sys.modules["zsharp"] = api


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Duplicate JSON object key")
        result[key] = value
    return result


def _reject_constant(value):
    raise ValueError(f"Non-finite JSON number {value} is not supported")


class Worker:
    def __init__(self, source, output):
        self.input = source
        self.output = output
        self.modules = {}
        self.objects = {}
        self.identities = {}
        self.next_object = 0
        self.next_callback = 0
        self.owner = threading.get_ident()
        self.active = False
        self.project_directory = pathlib.Path.cwd().resolve()

    def read(self):
        try:
            line = self.input.readline(FRAME_LIMIT + 2)
        except OSError as error:
            raise TransportClosed() from error
        if not line:
            return None
        if len(line) > FRAME_LIMIT + 1 or not line.endswith(b"\n"):
            raise ValueError("Python bridge frame exceeds 1 MiB or is incomplete")
        frame = line[:-1]
        if len(frame) > FRAME_LIMIT or b"\r" in frame or b"\0" in frame:
            raise ValueError("Invalid Python bridge frame")
        value = json.loads(frame.decode("utf-8"), object_pairs_hook=_unique_object,
                           parse_constant=_reject_constant)
        if not isinstance(value, dict) or not isinstance(value.get("op"), str):
            raise ValueError("Python bridge request must be an object with an operation")
        return value

    def send(self, message):
        frame = json.dumps(message, ensure_ascii=False, separators=(",", ":"), allow_nan=False).encode("utf-8")
        if len(frame) > FRAME_LIMIT:
            raise ValueError("Python bridge result exceeds 1 MiB")
        try:
            self.output.write(frame + b"\n")
            self.output.flush()
        except OSError as error:
            raise TransportClosed() from error

    @staticmethod
    def error_message(error):
        try:
            message = (type(error).__name__ + ": " + str(error))[:4096]
            return message.encode("utf-8", "backslashreplace").decode("utf-8")
        except BaseException:
            return "Python bridge operation failed"

    def respond(self, identifier, *, value=None, error=None):
        response = {"id": identifier, "op": "result", "ok": error is None}
        response["value" if error is None else "error"] = value if error is None else error
        self.send(response)

    def serve(self):
        global _connection
        _install_sdk()
        _connection = self
        try:
            while True:
                identifier = None
                try:
                    request = self.read()
                    if request is None:
                        break
                    identifier = request.get("id")
                    if not (identifier is None or isinstance(identifier, (str, int))):
                        identifier = None
                        raise ValueError("Invalid Python bridge request ID")
                    self.active = True
                    self.respond(identifier, value=self.encode(self.dispatch(request)))
                except TransportClosed:
                    break
                except BaseException as error:
                    try:
                        self.respond(identifier, error=self.error_message(error))
                    except TransportClosed:
                        break
                finally:
                    self.active = False
        finally:
            _connection = None
            self.objects.clear()
            self.identities.clear()
            self.modules.clear()

    def callback(self, operation, *, target=None, reference=None, member=None, arguments=()):
        if threading.get_ident() != self.owner or not self.active:
            raise RuntimeError("Z# callbacks must run on the thread handling the current foreign call")
        self.next_callback += 1
        identifier = f"python-host-{self.next_callback}"
        request = {"id": identifier, "op": operation, "args": [self.encode(value) for value in arguments]}
        if target is not None:
            request["target"] = target
        if reference is not None:
            request["ref"], request["member"] = reference, member
        self.send(request)
        while True:
            response = self.read()
            if response is None:
                raise RuntimeError("ZVM connection closed during Python callback")
            if response["op"] != "result":
                self.respond(response.get("id"), error="Reentrant call to the active Python worker is not supported")
                continue
            if response.get("id") != identifier:
                raise ValueError("Unexpected ZVM Python callback response ID")
            if response.get("ok") is not True:
                raise RuntimeError("Z# callback failed: " + str(response.get("error", "unknown error")))
            return self.decode(response.get("value"))

    def load_module(self, filename):
        if not isinstance(filename, str) or not filename:
            raise ValueError("Python module path is required")
        path = pathlib.Path(filename).resolve(strict=True)
        if path.suffix.lower() != ".py":
            raise ValueError("Python modules must use the .py extension")
        if path in self.modules:
            return self.modules[path]
        # Source-local imports and imports rooted at the project both work.
        for directory in (self.project_directory, path.parent):
            if str(directory) not in sys.path:
                sys.path.insert(0, str(directory))
        name = "_zsharp_user_" + hashlib.sha256(str(path).encode("utf-8")).hexdigest()
        specification = importlib.util.spec_from_file_location(name, path)
        if specification is None or specification.loader is None:
            raise ImportError("Could not load Python module " + str(path))
        module = importlib.util.module_from_spec(specification)
        sys.modules[name] = module
        try:
            specification.loader.exec_module(module)
        except BaseException:
            sys.modules.pop(name, None)
            raise
        self.modules[path] = module
        return module

    @staticmethod
    def exported(value):
        return bool(getattr(value, "__dict__", {}).get("__zsharp_export__", False))

    def resolve_export(self, module, target):
        if not isinstance(target, str) or not target:
            raise ValueError("Python export name is required")
        parts = target.split(".")
        if any(not IDENTIFIER.fullmatch(part) for part in parts):
            raise ValueError("Python export name is invalid")
        value = vars(module).get(parts[0])
        if not self.exported(value):
            raise PermissionError(f"Python export {parts[0]!r} is not exported; add @export")
        for part in parts[1:]:
            _member(part)
            if not inspect.isclass(value):
                raise PermissionError("Qualified Python exports must belong to an exported class")
            value = getattr(value, part)
        return value

    def dispatch(self, request):
        operation = request["op"]
        raw = request.get("args", [])
        if not isinstance(raw, list):
            raise ValueError("Python arguments must be an array")
        arguments = [self.decode(value) for value in raw]
        if operation in ("call", "new"):
            module = self.load_module(request.get("module"))
            target = self.resolve_export(module, request.get("target"))
            if operation == "new" and not inspect.isclass(target):
                raise TypeError("Python constructor target must be an exported class")
            if not callable(target):
                raise TypeError("Python export is not callable")
            return target(*arguments)
        reference = request.get("ref")
        if not isinstance(reference, str) or reference not in self.objects:
            raise ValueError("Python object reference is invalid or has been released")
        value = self.objects[reference]
        if operation == "release":
            if arguments:
                raise ValueError("Python object release does not accept arguments")
            del self.objects[reference]
            del self.identities[id(value)]
            return None
        member = _member(request.get("member"))
        if operation == "get":
            if arguments:
                raise ValueError("Python field get does not accept arguments")
            result = getattr(value, member)
            if callable(result):
                raise TypeError("Use invoke to call a Python method")
            return result
        if operation == "set":
            if len(arguments) != 1:
                raise ValueError("Python field set requires exactly one value")
            if not hasattr(value, member):
                raise AttributeError("Python object has no public field " + member)
            setattr(value, member, arguments[0])
            return None
        if operation == "invoke":
            method = getattr(value, member)
            if not callable(method):
                raise TypeError("Python object member is not callable")
            return method(*arguments)
        raise ValueError("Unsupported Python bridge operation " + repr(operation))

    def encode(self, value, depth=0):
        if depth > DEPTH_LIMIT:
            raise ValueError("Python bridge value nesting limit exceeded")
        if value is None or isinstance(value, (bool, str)):
            return value
        if isinstance(value, int):
            return {"$number": str(value)}
        if isinstance(value, Decimal):
            if not value.is_finite():
                raise ValueError("Non-finite Python number cannot cross the bridge")
            digits = value.as_tuple()
            if len(digits.digits) + abs(digits.exponent) > FRAME_LIMIT:
                raise ValueError("Python bridge number exceeds 1 MiB")
            return {"$number": format(value, "f")}
        if isinstance(value, float):
            if not math.isfinite(value):
                raise ValueError("Non-finite Python number cannot cross the bridge")
            return {"$number": repr(value)}
        if isinstance(value, ZSharpObject):
            return {"$zref": value.reference, "$type": value.type_name}
        if isinstance(value, (list, tuple)):
            return [self.encode(item, depth + 1) for item in value]
        if isinstance(value, dict):
            if any(not isinstance(key, str) for key in value):
                raise TypeError("Python records require string keys")
            return {"$record": {key: self.encode(item, depth + 1) for key, item in value.items()}}
        if not self.exported(type(value)):
            raise TypeError("Python returned an unexported object type; add @export to its class")
        reference = self.identities.get(id(value))
        if reference is None:
            self.next_object += 1
            reference = str(self.next_object)
            self.objects[reference] = value
            self.identities[id(value)] = reference
        return {"$ref": reference, "$type": type(value).__qualname__}

    def decode(self, value, depth=0):
        if depth > DEPTH_LIMIT:
            raise ValueError("Python bridge value nesting limit exceeded")
        if value is None or isinstance(value, (bool, str)):
            return value
        if isinstance(value, list):
            return [self.decode(item, depth + 1) for item in value]
        if not isinstance(value, dict):
            raise TypeError("Python bridge numbers require exact $number text")
        if set_builtin(value) == {"$number"}:
            text = value["$number"]
            if not isinstance(text, str) or not DECIMAL.fullmatch(text):
                raise ValueError("$number must contain finite decimal text")
            return Decimal(text) if any(char in text for char in ".eE") else int(text)
        if "$ref" in value and set_builtin(value) <= {"$ref", "$type"}:
            reference = value["$ref"]
            if not isinstance(reference, str) or reference not in self.objects:
                raise ValueError("Python object reference is invalid or has been released")
            return self.objects[reference]
        if "$zref" in value and set_builtin(value) <= {"$zref", "$type"}:
            type_name = value.get("$type", "")
            if not isinstance(type_name, str):
                raise ValueError("Z# object type must be text")
            return ZSharpObject(value["$zref"], type_name)
        if set_builtin(value) == {"$record"} and isinstance(value["$record"], dict):
            return {key: self.decode(item, depth + 1) for key, item in value["$record"].items()}
        raise ValueError("Unknown or ambiguous Python bridge value tag")


# The public API intentionally includes set(...); avoid shadowing the builtin
# inside the wire validator.
from builtins import set as set_builtin


def main():
    # Reserve duplicate descriptors for protocol traffic, then redirect both
    # Python print() and native extension writes to fd 1 into the log stream.
    if hasattr(sys, "set_int_max_str_digits"):
        # Payloads are already bounded by FRAME_LIMIT; don't truncate or reject
        # valid exact Z# integers merely because they exceed CPython's default.
        sys.set_int_max_str_digits(0)
    protocol = os.fdopen(os.dup(sys.stdout.fileno()), "wb", buffering=0)
    source = os.fdopen(os.dup(sys.stdin.fileno()), "rb", buffering=0)
    if sys.stderr is None:
        sys.stderr = open(os.devnull, "w", encoding="utf-8")
    os.dup2(sys.stderr.fileno(), sys.stdout.fileno())
    sys.stdout = sys.stderr
    # input() in a user module must not consume a pending bridge frame.
    with open(os.devnull, "rb") as empty:
        os.dup2(empty.fileno(), sys.stdin.fileno())
    try:
        Worker(source, protocol).serve()
    finally:
        source.close()
        protocol.close()
    # Return normally so Python runs user finally blocks and atexit handlers.
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
