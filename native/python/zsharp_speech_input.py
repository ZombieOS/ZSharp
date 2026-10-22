"""Offline speech recognition for ZVM; never opens a recording device itself.

The VM must call preflight before microphone capture. Models are supplied by the
managed local speech component or explicitly through ZSHARP_SPEECH_MODEL; this
helper never downloads models or sends audio to a network service. Recorded files
are owned/removed by the calling VM.
"""

from __future__ import annotations

import json
import os
import pathlib
import stat
import struct
import sys
import threading
import wave

try:
    from zsharp import export
except ImportError:
    # Private standalone worker mode does not require a zsharp pip package.
    def export(function):
        return function


_MODEL_LIMIT = 512 * 1024 * 1024
_WAV_LIMIT = 3 * 1024 * 1024
_REQUEST_LIMIT = 16384
_RESPONSE_LIMIT = 8192
_lock = threading.RLock()
_model = None
_model_path = None
_recognizer_type = None


def _speech_home() -> pathlib.Path:
    if "ZSHARP_SPEECH_HOME" in os.environ:
        configured = os.environ["ZSHARP_SPEECH_HOME"].strip()
        root = pathlib.Path(configured).expanduser()
        if not configured or not root.is_absolute():
            raise RuntimeError("ZSHARP_SPEECH_HOME must identify an absolute managed speech directory")
        return root
    if sys.platform == "win32":
        local = os.environ.get("LOCALAPPDATA")
        return ((pathlib.Path(local) if local else pathlib.Path.home() / "AppData" / "Local")
                / "ZombieOS" / "ZSharp" / "speech")
    if sys.platform == "darwin":
        return pathlib.Path.home() / "Library" / "Application Support" / "ZSharp" / "speech"
    local = os.environ.get("XDG_DATA_HOME", "")
    base = pathlib.Path(local).expanduser() if local else pathlib.Path.home() / ".local" / "share"
    if not base.is_absolute():
        base = pathlib.Path.home() / ".local" / "share"
    return base / "zsharp" / "speech"


def _model_directory() -> pathlib.Path:
    explicit = "ZSHARP_SPEECH_MODEL" in os.environ
    configured = os.environ["ZSHARP_SPEECH_MODEL"].strip() if explicit else str(_speech_home() / "models" / "vosk")
    if not configured:
        raise RuntimeError(
            "ZSHARP_SPEECH_MODEL must identify a local extracted Vosk model directory. "
            "No microphone recording has been requested by this check."
        )
    try:
        root = pathlib.Path(configured).expanduser().resolve(strict=True)
    except (OSError, ValueError) as exc:
        raise RuntimeError(
            "Offline speech recognition model is unavailable. Install the managed speech component "
            "or set ZSHARP_SPEECH_MODEL to an accessible extracted Vosk model directory. "
            "No microphone recording has been requested by this check."
        ) from exc
    if not root.is_dir() or not (root / "am" / "final.mdl").is_file():
        raise RuntimeError(
            "ZSHARP_SPEECH_MODEL must point to an extracted Vosk model directory "
            "containing am/final.mdl, not a ZIP archive or its parent directory"
        )
    return root


def _validate_model_size(root: pathlib.Path) -> None:
    total = 0
    visited = 0
    for directory, subdirectories, files in os.walk(root, followlinks=False):
        visited += len(subdirectories) + len(files)
        if visited > 32768:
            raise RuntimeError("The configured Vosk model contains too many files")
        for name in subdirectories:
            if pathlib.Path(directory, name).is_symlink():
                raise RuntimeError("Speech model directories must not contain symbolic links")
        for name in files:
            path = pathlib.Path(directory, name)
            if path.is_symlink():
                raise RuntimeError("Speech model files must be local regular files, not symbolic links")
            total += path.stat().st_size
            if total > _MODEL_LIMIT:
                raise RuntimeError("This speech prototype supports local Vosk models up to 512 MiB; choose a small model")


def _load_model():
    global _model, _model_path, _recognizer_type
    root = _model_directory()
    with _lock:
        if _model is not None and root == _model_path:
            return _model, _recognizer_type
        _validate_model_size(root)
        try:
            from vosk import KaldiRecognizer, Model, SetLogLevel
        except (ImportError, OSError) as exc:
            raise RuntimeError(
                "The ZVM Python runtime does not have the optional Vosk speech "
                "recognizer installed. Install the offline speech component in "
                "that runtime before using Microphone.getInput."
            ) from exc
        try:
            # Keep native failures available to the VM's private, bounded stderr
            # collector. Suppressing all Vosk logging hid the reason model
            # initialization failed and misleadingly suggested damaged files.
            SetLogLevel(0)
            model = Model(str(root))
        except Exception as exc:
            cause = str(exc).encode("utf-8", "replace")[:3072].decode("utf-8", "ignore")
            raise RuntimeError(
                "The local Vosk speech model could not be initialized ("
                + type(exc).__name__ + ": " + cause
                + "). See the recognizer diagnostic for details."
            ) from exc
        _model, _model_path, _recognizer_type = model, root, KaldiRecognizer
        return _model, _recognizer_type


@export
def preflight() -> bool:
    """Check dependency/model availability and cache the model before recording."""
    _load_model()
    return True


def _result_text(payload: str) -> str:
    result = json.loads(payload)
    if not isinstance(result, dict) or not isinstance(result.get("text", ""), str):
        raise RuntimeError("The offline speech recognizer returned an invalid result")
    return result.get("text", "").strip()


@export
def transcribe(filename: str) -> str:
    """Recognize an existing bounded mono PCM16 WAV; no device/network access."""
    if not isinstance(filename, str) or not filename or "\0" in filename:
        raise TypeError("Speech transcription requires a recording WAV filename")
    path = pathlib.Path(filename)
    try:
        size = path.stat().st_size
    except OSError as exc:
        raise RuntimeError("The speech recording WAV file is missing or unreadable") from exc
    if size < 44 or size > _WAV_LIMIT:
        raise ValueError("Speech recording WAV is invalid or exceeds 3 MiB")
    with _lock:
        model, recognizer_type = _load_model()
        try:
            with wave.open(str(path), "rb") as recording:
                rate = recording.getframerate()
                if (recording.getnchannels() != 1 or recording.getsampwidth() != 2
                        or recording.getcomptype() != "NONE" or not 8000 <= rate <= 48000):
                    raise ValueError("Speech recognition requires uncompressed mono PCM16 WAV audio at 8..48 kHz")
                expected = recording.getnframes()
                if expected == 0 or expected > rate * 30:
                    raise ValueError("Speech recording must contain more than zero and at most 30 seconds of audio")
                recognizer = recognizer_type(model, rate)
                texts = []
                frames = 0
                while True:
                    chunk = recording.readframes(4000)
                    if not chunk:
                        break
                    if len(chunk) % 2:
                        raise ValueError("Speech recording contains incomplete PCM16 data")
                    frames += len(chunk) // 2
                    if frames > expected:
                        raise ValueError("Speech recording exceeds its declared WAV frame count")
                    if recognizer.AcceptWaveform(chunk):
                        text = _result_text(recognizer.Result())
                        if text:
                            texts.append(text)
                if frames != expected:
                    raise ValueError("Speech recording WAV data is truncated")
                final = _result_text(recognizer.FinalResult())
                if final:
                    texts.append(final)
                text = " ".join(texts)
                if len(text.encode("utf-8")) > 65536:
                    raise RuntimeError("Speech recognition result exceeds 64 KiB")
                return text
        except (wave.Error, EOFError) as exc:
            raise ValueError("Speech recording is not a readable PCM WAV file") from exc


def _write_existing_result(filename: str, text: str) -> None:
    if not isinstance(filename, str) or not filename or "\0" in filename:
        raise ValueError("Speech recognition needs an owned temporary result filename")
    content = text.encode("utf-8")
    if len(content) > 65536:
        raise ValueError("Speech recognition result exceeds 64 KiB")
    details = os.lstat(filename)
    if not stat.S_ISREG(details.st_mode):
        raise ValueError("Speech result must be an existing regular temporary file")
    flags = os.O_WRONLY | getattr(os, "O_NOFOLLOW", 0) | getattr(os, "O_BINARY", 0)
    descriptor = os.open(filename, flags)  # Never O_CREAT: caller owns the file.
    try:
        opened = os.fstat(descriptor)
        if (not stat.S_ISREG(opened.st_mode)
                or (details.st_dev, details.st_ino) != (opened.st_dev, opened.st_ino)):
            raise ValueError("The speech result file changed before it could be written")
        os.ftruncate(descriptor, 0)
        with os.fdopen(descriptor, "wb", closefd=False) as result:
            result.write(content)
            result.flush()
    finally:
        os.close(descriptor)


def _execute(arguments: list[str]) -> None:
    if (not isinstance(arguments, list) or not arguments
            or not all(isinstance(value, str) and value and "\0" not in value
                       for value in arguments)):
        raise ValueError("Speech worker requires a nonempty list of text arguments")
    if arguments == ["preflight"]:
        preflight()
    elif len(arguments) == 3 and arguments[0] == "transcribe":
        _write_existing_result(arguments[2], transcribe(arguments[1]))
    else:
        raise ValueError("expected preflight or transcribe <recording.wav> <existing-result-file>")


def _read_exact(stream, count: int) -> bytes:
    chunks = []
    remaining = count
    while remaining:
        chunk = stream.read(remaining)
        if not chunk:
            break
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def _write_response(stream, error: Exception | None = None) -> None:
    if error is None:
        payload = b'{"ok":true}'
    else:
        # Never echo a request/transcript. Exceptions may contain long messages
        # from native libraries, so bound bytes before JSON escaping as well as
        # the final wire payload (including control-character escapes).
        message = str(error).encode("utf-8", "replace")[:4096].decode("utf-8", "ignore")
        message = message or type(error).__name__
        while True:
            payload = json.dumps({"ok": False, "error": message},
                                 ensure_ascii=False, separators=(",", ":")).encode("utf-8")
            if len(payload) <= _RESPONSE_LIMIT:
                break
            message = message[:max(1, len(message) // 2)]
    stream.write(struct.pack("<I", len(payload)))
    stream.write(payload)
    stream.flush()


def _worker_main(input_stream, output_stream) -> int:
    """Serve one VM's framed requests, retaining only the Vosk model cache.

    EOF is normal shutdown. Invalid framing is fatal since it cannot safely be
    resynchronized; a valid frame with invalid arguments is a recoverable error.
    The VM owns cancellation/timeout, private diagnostics, and temporary files.
    """
    while True:
        header = _read_exact(input_stream, 4)
        if not header:
            return 0
        if len(header) != 4:
            print("speech worker: truncated request header", file=sys.stderr)
            return 1
        length = struct.unpack("<I", header)[0]
        if not 0 < length <= _REQUEST_LIMIT:
            print("speech worker: invalid request length", file=sys.stderr)
            return 1
        payload = _read_exact(input_stream, length)
        if len(payload) != length:
            print("speech worker: truncated request payload", file=sys.stderr)
            return 1
        try:
            try:
                arguments = json.loads(payload.decode("utf-8"))
            except (UnicodeError, ValueError, RecursionError) as exc:
                raise ValueError("Speech worker request must contain valid UTF-8 JSON") from exc
            _execute(arguments)
        except Exception as exc:
            _write_response(output_stream, exc)
        else:
            _write_response(output_stream)


def main(arguments: list[str]) -> int:
    try:
        if arguments == ["worker"]:
            return _worker_main(sys.stdin.buffer, sys.stdout.buffer)
        # Retain the file-only, one-shot CLI for diagnostics and integrations.
        _execute(arguments)
        return 0
    except Exception as exc:
        detail = str(exc).encode("utf-8", "replace")[:4096].decode("utf-8", "ignore")
        print("speech recognition error: " + detail, file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
