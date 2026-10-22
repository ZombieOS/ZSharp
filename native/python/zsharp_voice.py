"""Private, offline-only ZVM voice-conditioning/synthesis helper.

No model or Python package is installed/downloaded here. The operator explicitly
sets ZSHARP_VOICE_MODEL_CONFIG to an existing Pocket TTS YAML whose weights and
tokenizer paths are local files. Only use reference recordings with permission.
.zvoice contains JSON plus non-executable safetensors conditioning, not pickle,
Python code, or a copy of the full shared inference model.
"""

from __future__ import annotations

import argparse
import base64
from collections import OrderedDict
from contextlib import redirect_stdout
import hashlib
import importlib.metadata
import json
import math
import os
from pathlib import Path
import struct
import sys
import tempfile
import threading
import wave


VOICE_LIMIT = 64 * 1024 * 1024
STATE_LIMIT = 32 * 1024 * 1024
REFERENCE_LIMIT = 50 * 1024 * 1024
TEXT_LIMIT = 16384
FORMAT_VERSION = 1
REQUEST_LIMIT = 16384
RESPONSE_LIMIT = 131072
PCM_CHUNK_LIMIT = 65536

# One heavyweight inference model per worker; bounded immutable voice containers
# are cached separately. No generated/mutable Pocket model state is reused.
_model_cache = None
_voice_cache = OrderedDict()
_network_guard_installed = False


class VoiceError(ValueError):
    pass


def _read(path: Path, maximum: int) -> bytes:
    with path.open("rb") as stream:
        data = stream.read(maximum + 1)
    if not data or len(data) > maximum:
        raise VoiceError("The voice file is empty or exceeds its size limit.")
    return data


def _json(data: bytes) -> dict:
    def unique(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise VoiceError("Duplicate JSON fields are not allowed in a .zvoice file.")
            result[key] = value
        return result

    value = json.loads(data.decode("utf-8"), object_pairs_hook=unique,
                       parse_constant=lambda _: (_ for _ in ()).throw(VoiceError("Non-finite JSON value.")))
    if not isinstance(value, dict):
        raise VoiceError("Expected a .zvoice JSON object.")
    return value


def _percent(document: dict, name: str, low: float, high: float) -> float:
    value = document.get(name, 100)
    if isinstance(value, bool) or not isinstance(value, (float, int)) or not math.isfinite(value) or not low <= value <= high:
        raise VoiceError(f"Voice {name} must be a number from {low:g} to {high:g}.")
    return float(value)


def _voice_document(path: Path) -> tuple[dict, bytes]:
    document = _json(_read(path, VOICE_LIMIT))
    if document.get("format") != "ZVOICE" or type(document.get("version")) is not int or document["version"] != FORMAT_VERSION:
        raise VoiceError("Unsupported .zvoice format or version.")
    if document.get("type") != "clone" or document.get("backend") != "pocket-tts":
        raise VoiceError("This helper only loads genuine Pocket TTS cloned voices.")
    _percent(document, "pitch", 50, 200)
    _percent(document, "speed", 25, 400)
    _percent(document, "volume", 0, 100)
    state = document.get("conditioning")
    if not isinstance(state, dict) or state.get("encoding") != "safetensors-base64":
        raise VoiceError("The .zvoice file has no supported speaker-conditioning data.")
    encoded = state.get("data")
    if not isinstance(encoded, str) or len(encoded) > (STATE_LIMIT + 2) // 3 * 4:
        raise VoiceError("Invalid speaker-conditioning size.")
    try:
        data = base64.b64decode(encoded, validate=True)
    except (ValueError, TypeError) as exception:
        raise VoiceError("Invalid speaker-conditioning encoding.") from exception
    if len(data) > STATE_LIMIT or len(data) < 10 or hashlib.sha256(data).hexdigest() != state.get("sha256"):
        raise VoiceError("The .zvoice speaker-conditioning checksum is invalid.")
    _validate_safetensors(data)
    return document, data


def _validate_safetensors(data: bytes) -> None:
    """Bound dimensions/offsets before native tensor libraries see untrusted data."""
    header_length = struct.unpack("<Q", data[:8])[0]
    if not 2 <= header_length <= 1024 * 1024 or header_length + 8 > len(data):
        raise VoiceError("Invalid safetensors header.")
    header = _json(data[8:8 + header_length])
    widths = {"F32": 4, "F16": 2, "BF16": 2, "I64": 8, "I32": 4, "BOOL": 1}
    if not 1 <= len(header) <= 512:
        raise VoiceError("Invalid number of speaker-conditioning tensors.")
    intervals = []
    for name, tensor in header.items():
        if name == "__metadata__":
            continue
        if not isinstance(name, str) or name.count("/") != 1 or not isinstance(tensor, dict):
            raise VoiceError("Invalid speaker-conditioning tensor name.")
        module, field = name.split("/")
        if not module or len(module) > 256 or field not in ("cache", "offset", "pad", "current_end"):
            raise VoiceError("Unsupported speaker-conditioning tensor kind.")
        shape, offsets, dtype = tensor.get("shape"), tensor.get("data_offsets"), tensor.get("dtype")
        if not isinstance(shape, list) or not 1 <= len(shape) <= 5 or dtype not in widths:
            raise VoiceError("Invalid speaker-conditioning tensor shape or dtype.")
        elements = 1
        for dimension in shape:
            if type(dimension) is not int or not 0 <= dimension <= 4096:
                raise VoiceError("Speaker-conditioning dimensions are outside supported limits.")
            elements *= dimension
        if not isinstance(offsets, list) or len(offsets) != 2 or any(type(value) is not int for value in offsets):
            raise VoiceError("Invalid speaker-conditioning data offsets.")
        first, last = offsets
        if first < 0 or last < first or last > len(data) - header_length - 8 or last - first != elements * widths[dtype]:
            raise VoiceError("Speaker-conditioning data does not match its tensor shape.")
        if field == "cache" and (len(shape) != 5 or shape[0] != 2 or shape[1] != 1 or dtype not in ("F32", "F16", "BF16")):
            raise VoiceError("Unsupported speaker-conditioning cache layout.")
        if field in ("offset", "pad"):
            if shape != [1] or dtype != "I64":
                raise VoiceError("Unsupported speaker-conditioning offset layout.")
            value = struct.unpack_from("<q", data, 8 + header_length + first)[0]
            if not 0 <= value <= 4096:
                raise VoiceError("Speaker-conditioning offset is outside supported limits.")
            if field == "pad" and value != 0:
                raise VoiceError("Only unpadded single-speaker conditioning is supported.")
        intervals.append((first, last))
    intervals.sort()
    if not intervals or intervals[0][0] != 0 or intervals[-1][1] != len(data) - header_length - 8:
        raise VoiceError("Speaker-conditioning tensor data is incomplete.")
    if any(previous[1] != current[0] for previous, current in zip(intervals, intervals[1:])):
        raise VoiceError("Speaker-conditioning tensor data overlaps or contains gaps.")


def _hash_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def _deny_network(event, arguments):
    if event in ("socket.connect", "socket.connect_ex", "socket.getaddrinfo", "urllib.Request"):
        raise VoiceError("The ZVM voice helper is offline-only; download/configure model files explicitly first.")


def _model_config_path() -> Path:
    configured = os.environ.get("ZSHARP_VOICE_MODEL_CONFIG")
    if configured is None:
        speech_home = os.environ.get("ZSHARP_SPEECH_HOME")
        if speech_home is not None:
            if not speech_home.strip():
                raise VoiceError("ZSHARP_SPEECH_HOME must be an absolute local directory.")
            root = Path(speech_home).expanduser()
        elif sys.platform == "win32":
            root = Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local")) / "ZombieOS" / "ZSharp" / "speech"
        elif sys.platform == "darwin":
            root = Path.home() / "Library" / "Application Support" / "ZSharp" / "speech"
        else:
            root = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local" / "share")) / "zsharp" / "speech"
        if not root.is_absolute():
            raise VoiceError("ZSHARP_SPEECH_HOME must be an absolute local directory.")
        configured = str(root / "models" / "pocket-tts" / "model.yaml")
        if not Path(configured).is_file():
            raise VoiceError("Genuine voice cloning requires a local Pocket TTS model. Install the optional speech components and obtain authorized model access first, or set ZSHARP_VOICE_MODEL_CONFIG to an existing local YAML config. No model was downloaded by this call.")
    if not configured:
        raise VoiceError("Genuine voice cloning requires a local Pocket TTS model. Set ZSHARP_VOICE_MODEL_CONFIG to its local YAML config; no model has been downloaded.")
    config_path = Path(configured).expanduser()
    if not config_path.is_absolute() or config_path.suffix.lower() not in (".yaml", ".yml") or not config_path.is_file():
        raise VoiceError("ZSHARP_VOICE_MODEL_CONFIG must name an existing absolute local YAML file.")
    return config_path.resolve(strict=True)


def _file_stamp(path: Path):
    info = path.stat()
    return (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns)


def _load_model(temporary: Path):
    global _model_cache, _network_guard_installed
    config_path = _model_config_path()
    os.environ["HF_HUB_OFFLINE"] = "1"
    os.environ["HF_DATASETS_OFFLINE"] = "1"
    os.environ["TRANSFORMERS_OFFLINE"] = "1"
    os.environ["HF_HUB_DISABLE_TELEMETRY"] = "1"
    os.environ["POCKET_TTS_SAVE_WEIGHTS"] = "0"
    os.environ["DEBUG_MIMI"] = "0"
    if not _network_guard_installed:
        sys.addaudithook(_deny_network)
        _network_guard_installed = True
    try:
        import yaml
        from pocket_tts import TTSModel
    except ImportError as exception:
        raise VoiceError("The ZVM Python runtime needs the optional pocket-tts package and dependencies (including soundfile for MP3). Nothing was installed automatically.") from exception
    try:
        version = importlib.metadata.version("pocket-tts")
        config_stamp = _file_stamp(config_path)
        if _model_cache is not None:
            cached_config, cached_stamp, cached_assets, model, fingerprint, cached_version = _model_cache
            if (cached_config == config_path and cached_stamp == config_stamp and cached_version == version
                    and all(_file_stamp(path) == stamp for path, stamp in cached_assets)):
                return model, fingerprint, version
            # Do not retain a second 200+ MB model when assets/configuration changed.
            _model_cache = None
            del model
        configuration = yaml.safe_load(_read(config_path, 256 * 1024).decode("utf-8"))
    except (ValueError, OSError) as exception:
        raise VoiceError("The local Pocket TTS model configuration is unreadable.") from exception
    if not isinstance(configuration, dict):
        raise VoiceError("The local Pocket TTS model config must be a YAML mapping.")
    # No fallback to non-cloning model weights, and no implicit HTTP/HF downloads.
    configuration.pop("weights_path_without_voice_cloning", None)
    identity = {}
    assets = []
    def localize(value, location=""):
        if isinstance(value, dict):
            result = {}
            for key, entry in value.items():
                item = f"{location}.{key}"
                if isinstance(entry, str) and key.endswith("_path"):
                    if "://" in entry:
                        raise VoiceError(f"Model setting {item} must be a local file, not a download URL.")
                    path = Path(entry).expanduser()
                    if not path.is_absolute():
                        path = config_path.parent / path
                    source_path = path
                    path = path.resolve(strict=True)
                    if not path.is_file() or path.stat().st_size > 4 * 1024 * 1024 * 1024:
                        raise VoiceError("A local model asset is missing or exceeds 4GiB.")
                    if "weights" in key and path.suffix != ".safetensors":
                        raise VoiceError("Only non-executable .safetensors model weights are supported; pickle checkpoints are not loaded.")
                    result[key] = str(path)
                    before = _file_stamp(source_path)
                    identity[item] = _hash_file(path)
                    if _file_stamp(source_path) != before or _file_stamp(path) != before:
                        raise VoiceError("A local model asset changed while it was being verified; retry after the update finishes.")
                    # Retain the configured spelling so an atomically retargeted
                    # asset symlink invalidates the model cache as well.
                    assets.append((source_path, before))
                else:
                    result[key] = localize(entry, item)
            return result
        if isinstance(value, list):
            return [localize(item, location) for item in value]
        return value
    configuration = localize(configuration)
    flow = configuration.get("flow_lm", {})
    mimi = configuration.get("mimi", {})
    if not configuration.get("weights_path") and not (flow.get("weights_path") and mimi.get("weights_path")):
        raise VoiceError("The config needs genuine voice-cloning model weights, not an uninitialized model.")
    if not isinstance(flow.get("lookup_table"), dict) or not flow["lookup_table"].get("tokenizer_path"):
        raise VoiceError("The config needs a local tokenizer_path.")
    normalized = json.loads(json.dumps(configuration))
    def fingerprint_paths(value, location=""):
        if isinstance(value, dict):
            return {key: identity.get(f"{location}.{key}", fingerprint_paths(item, f"{location}.{key}"))
                    for key, item in value.items()}
        if isinstance(value, list):
            return [fingerprint_paths(item, location) for item in value]
        return value
    fingerprint = hashlib.sha256(json.dumps(fingerprint_paths(normalized), sort_keys=True,
                                             separators=(",", ":")).encode()).hexdigest()
    localized = temporary / "model.yaml"
    localized.write_text(yaml.safe_dump(configuration), encoding="utf-8")
    model = TTSModel.load_model(config=localized)
    if not getattr(model, "has_voice_cloning", False):
        raise VoiceError("This local Pocket TTS model does not provide genuine voice cloning.")
    if _file_stamp(config_path) != config_stamp or any(_file_stamp(path) != stamp for path, stamp in assets):
        raise VoiceError("Local model files changed while the model was loading; retry after the update finishes.")
    _model_cache = config_path, config_stamp, assets, model, fingerprint, version
    return model, fingerprint, version


def _create(reference: Path, output: Path, temporary: Path) -> None:
    if reference.suffix.lower() not in (".mp3", ".wav") or output.suffix.lower() != ".zvoice":
        raise VoiceError("Expected reference.mp3/reference.wav and output.zvoice.")
    if output.exists():
        raise VoiceError("Refusing to overwrite an existing voice file.")
    _read(reference, REFERENCE_LIMIT)
    model, fingerprint, version = _load_model(temporary)
    try:
        import numpy as np
        import soundfile as sf
        import torch
        from pocket_tts.models.model_state import export_model_state
    except ImportError as exception:
        raise VoiceError("Voice cloning needs pocket-tts and soundfile in the selected ZVM Python runtime.") from exception
    # Decode only a bounded reference excerpt, not an entire malicious/huge MP3.
    with sf.SoundFile(str(reference)) as stream:
        sample_rate = stream.samplerate
        if not 8000 <= sample_rate <= 192000 or not 1 <= stream.channels <= 8:
            raise VoiceError("Unsupported voice-reference sample rate or channel count.")
        audio = stream.read(int(sample_rate * 30), dtype="float32", always_2d=True)
    if len(audio) < sample_rate or not np.isfinite(audio).all():
        raise VoiceError("A voice reference needs at least one second of finite audio.")
    mono = audio.mean(axis=1)
    if float(np.sqrt(np.mean(mono * mono))) < 0.0001:
        raise VoiceError("The voice reference is silent or too quiet.")
    # A bounded PCM WAV avoids Pocket's full-file MP3 read and preserves rate.
    reference_wave = temporary / "reference.wav"
    with wave.open(str(reference_wave), "wb") as stream:
        stream.setnchannels(1); stream.setsampwidth(2); stream.setframerate(sample_rate)
        stream.writeframes((np.clip(mono, -1, 1) * 32767).astype("<i2").tobytes())
    # Pocket TTS mutates conditioning from worker threads. no_grad preserves
    # ordinary tensors; inference_mode tensors cannot be updated by those threads.
    with torch.no_grad():
        state = model.get_state_for_audio_prompt(reference_wave)
        state_path = temporary / "state.safetensors"
        export_model_state(state, state_path)
    state_data = _read(state_path, STATE_LIMIT)
    _validate_safetensors(state_data)
    document = {"format": "ZVOICE", "version": FORMAT_VERSION, "type": "clone",
                "backend": "pocket-tts", "backendVersion": version,
                "modelFingerprint": fingerprint,
                "pitch": 100, "speed": 100, "volume": 100,
                "reference": {"sha256": _hash_file(reference), "seconds": len(audio) / sample_rate},
                "conditioning": {"encoding": "safetensors-base64",
                                 "sha256": hashlib.sha256(state_data).hexdigest(),
                                 "data": base64.b64encode(state_data).decode("ascii")}}
    # Caller gives an owned staging path, then publishes atomically in-project.
    with output.open("x", encoding="utf-8", newline="\n") as stream:
        json.dump(document, stream, ensure_ascii=False, separators=(",", ":"))
        stream.write("\n")


def _synthesize(voice: Path, text_path: Path, output: Path, temporary: Path) -> None:
    document, conditioning = _voice_document(voice)
    text = _read(text_path, TEXT_LIMIT).decode("utf-8")
    model, fingerprint, version = _load_model(temporary)
    if document.get("modelFingerprint") != fingerprint or document.get("backendVersion") != version:
        raise VoiceError("The .zvoice file needs the same Pocket TTS model/assets and backend version used to create it.")
    state_path = temporary / "voice.safetensors"
    state_path.write_bytes(conditioning)
    import numpy as np
    import torch
    with torch.no_grad():
        state = model.get_state_for_audio_prompt(state_path)
        audio = model.generate_audio(state, text).detach().cpu().numpy().reshape(-1)
    sample_rate = int(model.sample_rate)
    if not 8000 <= sample_rate <= 192000 or len(audio) > sample_rate * 300 or not np.isfinite(audio).all():
        raise VoiceError("The model produced invalid or excessively long audio.")
    pitch = _percent(document, "pitch", 50, 200)
    speed = _percent(document, "speed", 25, 400)
    if pitch != 100 or speed != 100:
        try:
            import librosa
        except ImportError as exception:
            raise VoiceError("Non-default cloned-voice pitch/speed requires optional librosa in the ZVM Python runtime; the speaker identity itself is still genuine conditioning.") from exception
        if pitch != 100:
            audio = librosa.effects.pitch_shift(audio, sr=sample_rate, n_steps=12 * math.log2(pitch / 100))
        if speed != 100:
            audio = librosa.effects.time_stretch(audio, rate=speed / 100)
    if len(audio) > sample_rate * 300 or not np.isfinite(audio).all():
        raise VoiceError("The adjusted speech exceeds the maximum duration or contains invalid samples.")
    with wave.open(str(output), "wb") as stream:
        stream.setnchannels(1); stream.setsampwidth(2); stream.setframerate(sample_rate)
        stream.writeframes((np.clip(audio, -1, 1) * 32767).astype("<i2").tobytes())


def _cached_voice_document(path: Path) -> tuple[dict, bytes]:
    """Reuse only validated immutable bytes, never Pocket's mutable model state."""
    resolved = path.resolve(strict=True)
    stamp = _file_stamp(resolved)
    cached = _voice_cache.get(resolved)
    if cached is not None and cached[0] == stamp:
        _voice_cache.move_to_end(resolved)
        return cached[1], cached[2]
    document, conditioning = _voice_document(resolved)
    if _file_stamp(resolved) != stamp:
        raise VoiceError("The .zvoice file changed while it was being read; retry after saving it.")
    _voice_cache[resolved] = stamp, document, conditioning
    _voice_cache.move_to_end(resolved)
    while len(_voice_cache) > 2:
        _voice_cache.popitem(last=False)
    return document, conditioning


def _prepared_voice(voice: Path, temporary: Path):
    document, conditioning = _cached_voice_document(voice)
    model, fingerprint, version = _load_model(temporary)
    if document.get("modelFingerprint") != fingerprint or document.get("backendVersion") != version:
        raise VoiceError("The .zvoice file needs the same Pocket TTS model/assets and backend version used to create it.")
    state_path = temporary / "voice.safetensors"
    state_path.write_bytes(conditioning)
    # Loading the immutable safetensors again is inexpensive and gives each
    # utterance an independent state even if a backend mutates its argument.
    state = model.get_state_for_audio_prompt(state_path)
    return document, model, state


def _synthesize_stream(voice: Path, text_path: Path, temporary: Path, emit) -> None:
    text = _read(text_path, TEXT_LIMIT).decode("utf-8")
    document, model, state = _prepared_voice(voice, temporary)
    import numpy as np
    import torch
    sample_rate = int(model.sample_rate)
    if not 8000 <= sample_rate <= 192000:
        raise VoiceError("The model produced an unsupported sample rate.")
    pitch = _percent(document, "pitch", 50, 200)
    speed = _percent(document, "speed", 25, 400)
    total_samples = 0

    def send(audio):
        nonlocal total_samples
        audio = audio.detach().cpu().numpy().reshape(-1) if hasattr(audio, "detach") else np.asarray(audio).reshape(-1)
        total_samples += len(audio)
        if total_samples > sample_rate * 300 or not np.isfinite(audio).all():
            raise VoiceError("The model produced invalid or excessively long audio.")
        pcm = (np.clip(audio, -1, 1) * 32767).astype("<i2").tobytes()
        for offset in range(0, len(pcm), PCM_CHUNK_LIMIT):
            emit({"pcm": base64.b64encode(pcm[offset:offset + PCM_CHUNK_LIMIT]).decode("ascii"),
                  "rate": sample_rate})

    with torch.no_grad():
        if pitch == 100 and speed == 100:
            # Pocket's public stream yields decoder frames while autoregressive
            # generation continues on its producer thread. No full-WAV barrier.
            stop = threading.Event()
            generator = model.generate_audio_stream(state, text, stop=stop)
            try:
                for chunk in generator:
                    send(chunk)
            finally:
                # If validation/output fails after an early frame, stop and
                # drain the public generator before accepting another request;
                # Pocket's model is explicitly not concurrently reentrant.
                stop.set()
                try:
                    for _ in generator:
                        pass
                finally:
                    generator.close()
        else:
            # librosa's existing pitch/time algorithms need the complete signal.
            # Keep their exact behavior rather than alter cloned-voice quality.
            audio = model.generate_audio(state, text).detach().cpu().numpy().reshape(-1)
            if len(audio) > sample_rate * 300 or not np.isfinite(audio).all():
                raise VoiceError("The model produced invalid or excessively long audio.")
            try:
                import librosa
            except ImportError as exception:
                raise VoiceError("Non-default cloned-voice pitch/speed requires optional librosa in the ZVM Python runtime; the speaker identity itself is still genuine conditioning.") from exception
            if pitch != 100:
                audio = librosa.effects.pitch_shift(audio, sr=sample_rate, n_steps=12 * math.log2(pitch / 100))
            if speed != 100:
                audio = librosa.effects.time_stretch(audio, rate=speed / 100)
            send(audio)
    if not total_samples:
        raise VoiceError("The model produced no speech audio.")


def _write_frame(stream, value) -> None:
    payload = json.dumps(value, ensure_ascii=False, separators=(",", ":"), allow_nan=False).encode("utf-8")
    if not payload or len(payload) > RESPONSE_LIMIT:
        raise VoiceError("The speech worker response exceeds its size limit.")
    for data in (struct.pack("<I", len(payload)), payload):
        remaining = memoryview(data)
        while remaining:
            written = stream.write(remaining)
            if written is None or written <= 0:
                raise BrokenPipeError("The parent speech pipe was closed.")
            remaining = remaining[written:]
    stream.flush()


def _error_frame(exception: Exception) -> dict:
    # Byte-bounded, single-line diagnostics; never include a request/transcript.
    message = str(exception).replace("\r", " ").replace("\n", " ")
    return {"ok": False, "error": message.encode("utf-8")[:3000].decode("utf-8", "ignore") or "Local speech generation failed."}


def _read_exact(stream, count: int) -> bytes:
    output = bytearray()
    while len(output) < count:
        part = stream.read(count - len(output))
        if not part:
            break
        output.extend(part)
    return bytes(output)


def _worker_main(input_stream, output_stream) -> int:
    """Private sequential framed pipe RPC; no network or audio devices."""
    while True:
        header = _read_exact(input_stream, 4)
        if not header:
            return 0
        if len(header) != 4:
            _write_frame(output_stream, {"ok": False, "error": "Truncated speech worker frame header."})
            return 1
        size = struct.unpack("<I", header)[0]
        if not 1 <= size <= REQUEST_LIMIT:
            _write_frame(output_stream, {"ok": False, "error": "Invalid or oversized speech worker request."})
            return 1
        payload = _read_exact(input_stream, size)
        if len(payload) != size:
            _write_frame(output_stream, {"ok": False, "error": "Truncated speech worker frame body."})
            return 1
        try:
            request = json.loads(payload.decode("utf-8"))
            if (not isinstance(request, list) or len(request) not in (2, 3)
                    or any(not isinstance(value, str) or "\0" in value for value in request)):
                raise VoiceError("Expected a bounded speech worker operation and local paths.")
            operation = request[0]
            if not ((operation == "prepare" and len(request) in (2, 3))
                    or (operation == "synthesize-stream" and len(request) == 3)):
                raise VoiceError("Unknown speech worker operation.")
            paths = [Path(value) for value in request[1:]]
            if any(not path.is_absolute() for path in paths):
                raise VoiceError("Speech worker paths must be absolute local files.")
            # Production calls supply a parent-owned private directory so the
            # native side can erase conditioning even if it must kill us. A
            # synthesis job already owns the directory containing its text.
            workspace = paths[1] if operation == "prepare" and len(paths) == 2 else (
                paths[1].parent if operation == "synthesize-stream" else None)
            if workspace is not None and (workspace.is_symlink() or not workspace.is_dir()):
                raise VoiceError("Speech worker workspace must be an existing absolute local directory, not a symlink.")
            # Never let dependency print() calls pollute the binary protocol.
            with redirect_stdout(sys.stderr), tempfile.TemporaryDirectory(prefix="zsharp-voice-worker-", dir=workspace) as directory:
                temporary = Path(directory)
                if operation == "prepare":
                    _prepared_voice(paths[0], temporary)
                else:
                    _synthesize_stream(paths[0], paths[1], temporary,
                                       lambda frame: _write_frame(output_stream, frame))
            _write_frame(output_stream, {"ok": True})
        except (BrokenPipeError, OSError) as exception:
            # A disconnected parent cannot receive another framed reply.
            if isinstance(exception, BrokenPipeError):
                return 1
            _write_frame(output_stream, _error_frame(exception))
        except Exception as exception:
            _write_frame(output_stream, _error_frame(exception))


def _worker_stdio() -> int:
    # Preserve a dedicated binary protocol descriptor and send native-library as
    # well as Python stdout diagnostics to stderr. The parent owns both pipes.
    sys.stdout.flush()
    protocol_fd = os.dup(sys.stdout.fileno())
    try:
        if sys.platform == "win32":
            import msvcrt
            msvcrt.setmode(protocol_fd, os.O_BINARY)
            msvcrt.setmode(sys.stdin.fileno(), os.O_BINARY)
        os.dup2(sys.stderr.fileno(), sys.stdout.fileno())
        with os.fdopen(protocol_fd, "wb", buffering=0) as protocol:
            protocol_fd = -1
            return _worker_main(sys.stdin.buffer, protocol)
    finally:
        if protocol_fd >= 0:
            os.close(protocol_fd)


def main() -> int:
    if sys.argv[1:] == ["worker"]:
        return _worker_stdio()
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    create = commands.add_parser("create")
    create.add_argument("--reference", type=Path, required=True)
    create.add_argument("--output", type=Path, required=True)
    create.add_argument("--result", type=Path, required=True)
    synthesize = commands.add_parser("synthesize")
    synthesize.add_argument("--voice", type=Path, required=True)
    synthesize.add_argument("--text", type=Path, required=True)
    synthesize.add_argument("--output", type=Path, required=True)
    synthesize.add_argument("--result", type=Path, required=True)
    arguments = parser.parse_args()
    try:
        # Parent owns this private workspace and can clean it even after killing
        # a timed-out process; no orphan reference recordings in the project.
        with tempfile.TemporaryDirectory(prefix="voice-model-", dir=arguments.result.parent) as directory:
            temporary = Path(directory)
            if arguments.command == "create":
                _create(arguments.reference, arguments.output, temporary)
            else:
                _synthesize(arguments.voice, arguments.text, arguments.output, temporary)
        # The native protocol is byte-exact, including on Windows (no CRLF).
        arguments.result.write_bytes(b"OK\n")
    except Exception as exception:
        # Result contains no transcript/reference audio, and never token values.
        message = str(exception).replace("\r", " ").replace("\n", " ")[:3000]
        try:
            arguments.result.write_bytes(("ERROR\n" + message).encode("utf-8"))
        except OSError:
            print("Could not write the local speech helper result.", file=sys.stderr)
            return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
