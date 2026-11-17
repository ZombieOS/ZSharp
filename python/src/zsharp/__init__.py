"""Call public Z# functions from standalone Python using the local ZVM.

Each call starts a new VM. The in-VM Python callback module with the same name
is installed separately by ZVM and retains its existing synchronous behavior.
"""
from __future__ import annotations

from decimal import Decimal, InvalidOperation
import json
import math
import os
from pathlib import Path
import re
import shutil
import signal
import subprocess
import sys
import tempfile

__all__ = ["ZSharp", "ZSharpError", "call"]
__version__ = "1.4.1.2"

_LIMIT = 1024 * 1024
_RESPONSE_LIMIT = _LIMIT + 8192  # bounded room for the CLI's result envelope
_DEPTH = 32
_IDENTIFIER = re.compile(r"[A-Za-z_][A-Za-z_0-9]*\Z")
_DECIMAL = re.compile(r"[+-]?(?:[0-9]+(?:\.[0-9]*)?|\.[0-9]+)(?:[eE][+-]?[0-9]+)?\Z")
_DEFAULT = object()


class ZSharpError(RuntimeError):
    """A runtime, transport, or discovery failure, with captured native logs."""

    def __init__(self, message, *, stdout="", stderr="", returncode=None,
                 command=(), timed_out=False):
        super().__init__(message)
        self.stdout = stdout
        self.stderr = stderr
        self.returncode = returncode
        self.command = tuple(command)
        self.timed_out = timed_out


def _number_text(value):
    if isinstance(value, int):
        try:
            return str(value)
        except ValueError:
            # Respect CPython's process-wide integer/string limit: convert an
            # already-existing integer without changing that global setting.
            return format(Decimal(value), "f")
    if isinstance(value, Decimal):
        if not value.is_finite():
            raise ValueError("Z# numbers must be finite")
        parts = value.as_tuple()
        if len(parts.digits) + abs(parts.exponent) > _LIMIT:
            raise ValueError("Z# number exceeds the 1 MiB wire limit")
        return format(value, "f")
    if not math.isfinite(value):
        raise ValueError("Z# numbers must be finite")
    return repr(value)


def _encode(value, depth=0):
    if depth > _DEPTH:
        raise ValueError("Z# argument nesting limit exceeded")
    if value is None or isinstance(value, (str, bool)):
        return value
    if isinstance(value, (int, Decimal, float)):
        return {"$number": _number_text(value)}
    if isinstance(value, (list, tuple)):
        return [_encode(item, depth + 1) for item in value]
    if isinstance(value, dict):
        if any(not isinstance(key, str) for key in value):
            raise TypeError("Z# records require string keys")
        return {"$record": {key: _encode(item, depth + 1) for key, item in value.items()}}
    raise TypeError("Unsupported Z# argument type: " + type(value).__name__)


def _decode(value, depth=0):
    if depth > _DEPTH:
        raise ValueError("Z# result nesting limit exceeded")
    if value is None or isinstance(value, (str, bool)):
        return value
    if isinstance(value, list):
        return [_decode(item, depth + 1) for item in value]
    if not isinstance(value, dict):
        raise ValueError("Z# result numbers require tagged decimal text")
    if set(value) == {"$number"}:
        text = value["$number"]
        if not isinstance(text, str) or not _DECIMAL.fullmatch(text):
            raise ValueError("Invalid Z# decimal result")
        try:
            number = Decimal(text)
        except InvalidOperation as error:
            raise ValueError("Invalid Z# decimal result") from error
        parts = number.as_tuple()
        if not number.is_finite() or len(parts.digits) + abs(parts.exponent) > _LIMIT:
            raise ValueError("Z# result number exceeds the wire limit")
        if any(character in text for character in ".eE"):
            return number
        # Decimal -> int also supports exact integers beyond CPython's default
        # decimal string conversion limit, without modifying interpreter flags.
        return int(number)
    if set(value) == {"$record"} and isinstance(value["$record"], dict):
        return {key: _decode(item, depth + 1) for key, item in value["$record"].items()}
    if "$ref" in value or "$zref" in value:
        raise ValueError("Live Z# objects cannot outlive a standalone call; return ordinary values instead")
    raise ValueError("Unknown or ambiguous Z# result tag")


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError("Duplicate JSON key in Z# response")
        result[key] = value
    return result


def _reject_constant(value):
    raise ValueError("Non-finite JSON number in Z# response: " + value)


def _as_text(value):
    return value.decode("utf-8", "replace") if isinstance(value, bytes) else value or ""


def _forward(stdout, stderr):
    if stdout:
        sys.stdout.write(stdout)
        sys.stdout.flush()
    if stderr:
        sys.stderr.write(stderr)
        sys.stderr.flush()


def _timeout(value):
    try:
        valid = (value is None or (not isinstance(value, bool) and isinstance(value, (int, float))
                                    and math.isfinite(value) and value > 0))
    except OverflowError:
        valid = False
    if not valid:
        raise ValueError("timeout must be a positive number of seconds or None")
    return value


def _run(command, *, cwd, timeout):
    # Foreign workers can inherit VM logging handles and use independent process
    # groups. Regular temporary files keep timeout cleanup independent of pipe
    # EOF. Append mode also keeps late worker writes from sharing our read offset.
    with tempfile.TemporaryFile(mode="a+b") as output, tempfile.TemporaryFile(mode="a+b") as errors:
        process = subprocess.Popen(command, cwd=cwd, stdout=output, stderr=errors,
                                   start_new_session=(os.name == "posix"),
                                   creationflags=(subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0))

        def snapshot(stream):
            size = os.fstat(stream.fileno()).st_size
            stream.seek(0)
            return stream.read(size).decode("utf-8", "replace")

        def stop():
            try:
                if os.name == "posix":
                    os.killpg(process.pid, signal.SIGKILL)
                else:
                    process.kill()
            except ProcessLookupError:
                pass
            try:
                process.wait(timeout=2)
            except subprocess.TimeoutExpired:
                pass

        try:
            process.wait(timeout=timeout)
        except subprocess.TimeoutExpired as error:
            stop()
            raise subprocess.TimeoutExpired(command, timeout, output=snapshot(output),
                                            stderr=snapshot(errors)) from error
        except BaseException:
            stop()
            raise
        return subprocess.CompletedProcess(command, process.returncode,
                                           snapshot(output), snapshot(errors))


def _executable(explicit):
    selected = explicit if explicit is not None else os.environ.get("ZSHARP_EXECUTABLE")
    if selected is not None:
        selected = os.fspath(selected)
        if not selected:
            raise ZSharpError("The Z# executable path is empty")
        found = shutil.which(selected)
        candidate = Path(found or selected).expanduser().resolve()
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate
        raise ZSharpError("Z# executable is not an executable file: " + str(candidate))
    found = shutil.which("zsharp")
    if found:
        return Path(found).resolve()
    candidates = [Path.home() / ".local" / "bin" / "zsharp"]
    local = os.environ.get("LOCALAPPDATA")
    if local:
        candidates.append(Path(local) / "ZombieOS" / "ZSharp" / "bin" / "zsharp.exe")
    for candidate in candidates:
        if candidate.is_file() and os.access(candidate, os.X_OK):
            return candidate.resolve()
    raise ZSharpError("ZVM was not found. Install Z#, set ZSHARP_EXECUTABLE, or pass executable='/absolute/path/to/zsharp'.")


def _target(project, target):
    if not isinstance(target, str) or "\0" in target:
        raise ValueError("target must be File:Room:Function text")
    parts = target.split(":")
    if len(parts) != 3 or not all(parts):
        raise ValueError("target must contain File:Room:Function")
    file, room, function = parts
    if not _IDENTIFIER.fullmatch(room) or not _IDENTIFIER.fullmatch(function):
        raise ValueError("Z# room and function must be identifiers")
    # Both separator forms receive the same traversal checks on every host.
    file = file.replace("\\", "/")
    path = Path(file)
    if path.is_absolute() or any(part in (".", "..") for part in file.split("/")):
        raise ValueError("Z# target file must be a safe path relative to the project")
    if not file.endswith(".zsharp"):
        file += ".zsharp"
    candidate = (project / file).resolve()
    try:
        candidate.relative_to(project)
    except ValueError as error:
        raise ValueError("Z# target file escapes the project directory") from error
    if not candidate.is_file():
        raise ZSharpError("Z# source file was not found: " + str(candidate))
    return candidate, room + ":" + function


class ZSharp:
    """A reusable project/runtime configuration; each call uses a fresh VM.

    ``timeout`` is measured in seconds; ``None`` disables the limit. Returned
    integers are ``int`` and fractional/exponent numbers are ``Decimal``.
    """

    def __init__(self, project=None, *, executable=None, timeout=30):
        self.project = Path(project if project is not None else Path.cwd()).expanduser().resolve()
        if not self.project.is_dir():
            raise ZSharpError("Z# project directory does not exist: " + str(self.project))
        self.executable = executable
        self.timeout = _timeout(timeout)

    def call(self, target, *arguments, timeout=_DEFAULT):
        """Invoke a public function named ``File:Room:Function``.

        The source is resolved under ``project``. Automatic Start/Shutdown
        handlers are not run. Native stdout/stderr remain ordinary log output.
        """
        limit = self.timeout if timeout is _DEFAULT else _timeout(timeout)
        source, function = _target(self.project, target)
        if len(arguments) > 1024:
            raise ValueError("Z# calls support at most 1024 arguments")
        payload = json.dumps([_encode(value) for value in arguments], ensure_ascii=False,
                             separators=(",", ":"), allow_nan=False).encode("utf-8")
        if len(payload) > _LIMIT:
            raise ValueError("Z# arguments exceed the 1 MiB wire limit")
        executable = _executable(self.executable)
        with tempfile.TemporaryDirectory(prefix="zsharp-call-") as temporary:
            request = Path(temporary) / "arguments.json"
            response = Path(temporary) / "result.json"
            request.write_bytes(payload)
            command = [str(executable), "call", str(source), function,
                       "--args-file", str(request), "--result-file", str(response)]
            try:
                process = _run(command, cwd=self.project, timeout=limit)
            except subprocess.TimeoutExpired as error:
                stdout, stderr = _as_text(error.stdout), _as_text(error.stderr)
                _forward(stdout, stderr)
                raise ZSharpError("Z# call timed out after " + str(limit) + " seconds",
                                  stdout=stdout, stderr=stderr, command=command, timed_out=True) from error
            except OSError as error:
                raise ZSharpError("Could not start ZVM: " + str(error), command=command) from error
            stdout, stderr = _as_text(process.stdout), _as_text(process.stderr)
            _forward(stdout, stderr)
            detail = None
            result = None
            try:
                with response.open("rb") as stream:
                    raw = stream.read(_RESPONSE_LIMIT + 1)
                if len(raw) > _RESPONSE_LIMIT:
                    raise ValueError("Z# response exceeds the 1 MiB wire limit")
                result = json.loads(raw.decode("utf-8"), object_pairs_hook=_unique_object,
                                    parse_constant=_reject_constant)
                if not isinstance(result, dict) or not isinstance(result.get("ok"), bool):
                    raise ValueError("Z# response requires an ok status")
                if result["ok"] is False:
                    if not isinstance(result.get("error"), str) or not result["error"]:
                        raise ValueError("Z# error response has no diagnostic")
                    detail = result["error"]
                elif "value" not in result:
                    raise ValueError("Z# response has no return value")
                elif len(json.dumps(result["value"], ensure_ascii=False, separators=(",", ":"),
                                    allow_nan=False).encode("utf-8")) > _LIMIT:
                    raise ValueError("Z# return value exceeds the 1 MiB wire limit")
            except (OSError, UnicodeError, ValueError, RecursionError) as error:
                detail = "Invalid or missing Z# response: " + str(error)
            if process.returncode or detail:
                if process.returncode and (not isinstance(result, dict) or result.get("ok") is not False):
                    logs = stderr.strip() or stdout.strip()
                    detail = ("ZVM exited with status " + str(process.returncode)
                              + (": " + logs if logs else "") + ("; " + detail if detail else ""))
                raise ZSharpError(detail, stdout=stdout, stderr=stderr,
                                  returncode=process.returncode, command=command)
            try:
                return _decode(result["value"])
            except (ValueError, RecursionError) as error:
                raise ZSharpError("Invalid Z# return value: " + str(error), stdout=stdout,
                                  stderr=stderr, returncode=process.returncode, command=command) from error


def call(target, *arguments, project=None, executable=None, timeout=30):
    """Call a public Z# function from an independent Python process."""
    return ZSharp(project, executable=executable, timeout=timeout).call(target, *arguments)
