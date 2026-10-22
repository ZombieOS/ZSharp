"""Opt-in, real-model speech checks; never records or plays physical audio.

Install authorized local models first. This expensive suite is deliberately not
part of normal CTest. It uses only the operator-owned synthetic SAPI reference
at SPEECH_HOME/diagnostics/synthetic-reference.wav, never a user's voice sample.

Example:
  python tests/speech-model-tests.py --speech-home ABSOLUTE_DIRECTORY \
      --zvm build/Release/zsharp.exe --recognize

These checks establish conditioning/synthesis and VM plumbing, not subjective
speaker-identity fidelity, pronunciation quality, or microphone performance.
"""

from __future__ import annotations

import argparse
import array
import base64
import hashlib
import importlib.util
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
import time
import wave


PHRASE = "The local Z sharp voice test has completed successfully."
MAX_LOG = 8 * 1024 * 1024
ROOT = Path(__file__).resolve().parents[1]
VOICE_HELPER = ROOT / "native" / "python" / "zsharp_voice.py"
STT_HELPER = ROOT / "native" / "python" / "zsharp_speech_input.py"

# Exercise the real worker dispatcher/cache repeatedly using only a generated
# WAV. No capture device or physical playback is involved. The fake-model unit
# suite separately verifies exactly one Model and one recognizer per utterance.
REPEATED_RECOGNITION = r'''
import importlib.util, io, json, pathlib, struct, sys
spec = importlib.util.spec_from_file_location("input_model_fixture", sys.argv[1])
helper = importlib.util.module_from_spec(spec)
spec.loader.exec_module(helper)
recording, result = sys.argv[2:4]
requests = []
for index in range(12):
    for arguments in (["preflight"], ["transcribe", recording, result]):
        payload = json.dumps(arguments, ensure_ascii=False).encode("utf-8")
        requests.append(struct.pack("<I", len(payload)) + payload)
responses = io.BytesIO()
assert helper._worker_main(io.BytesIO(b"".join(requests)), responses) == 0
responses.seek(0)
count = 0
while header := responses.read(4):
    assert len(header) == 4
    length = struct.unpack("<I", header)[0]
    assert 0 < length <= 8192
    response = json.loads(responses.read(length).decode("utf-8"))
    assert response == {"ok": True}, response
    count += 1
assert count == 24
assert helper._model is not None
assert pathlib.Path(result).read_text(encoding="utf-8").strip()
print(json.dumps({"requests":count,"utterances":12,"physicalAudio":False}))
'''


def require(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def default_home() -> Path:
    configured = os.environ.get("ZSHARP_SPEECH_HOME")
    if configured is not None:
        return Path(configured).expanduser()
    if sys.platform == "win32":
        return Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData" / "Local")) / "ZombieOS" / "ZSharp" / "speech"
    if sys.platform == "darwin":
        return Path.home() / "Library" / "Application Support" / "ZSharp" / "speech"
    return Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local" / "share")) / "zsharp" / "speech"


def kill_owned_process(process: subprocess.Popen) -> None:
    """Stop only the child/tree this fixture created, never another ZVM."""
    if os.name == "nt":
        # A VM may own a private Python helper. /T targets this known child PID;
        # no shell, executable-name matching, or unrelated process termination.
        try:
            subprocess.run(["taskkill.exe", "/PID", str(process.pid), "/T", "/F"],
                           stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                           stderr=subprocess.DEVNULL, timeout=10, check=False)
        except (OSError, subprocess.TimeoutExpired):
            pass
        if process.poll() is None:
            process.kill()
    else:
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    process.wait(timeout=10)


def run(command: list[str], fixture: Path, environment: dict[str, str],
        label: str, timeout: int = 180) -> dict:
    require(environment.get("SDL_AUDIODRIVER") == "dummy", "Physical playback must remain disabled")
    log = fixture / (label + ".log")
    started = time.monotonic()
    with log.open("wb") as stream:
        process = subprocess.Popen(command, cwd=fixture, env=environment,
                                   stdin=subprocess.DEVNULL, stdout=stream,
                                   stderr=subprocess.STDOUT, close_fds=True,
                                   start_new_session=os.name != "nt",
                                   creationflags=subprocess.CREATE_NO_WINDOW if os.name == "nt" else 0)
        try:
            deadline = started + timeout
            while process.poll() is None:
                if time.monotonic() >= deadline or log.stat().st_size > MAX_LOG:
                    kill_owned_process(process)
                    raise AssertionError(f"{label} exceeded its {timeout}s timeout or diagnostic limit")
                time.sleep(0.1)
        except BaseException:
            if process.poll() is None:
                kill_owned_process(process)
            raise
    require(log.stat().st_size <= MAX_LOG, f"{label} produced excessive diagnostics")
    output = log.read_text(encoding="utf-8", errors="replace")
    require(process.returncode == 0, f"{label} failed ({process.returncode}):\n{output[-6000:]}")
    return {"seconds": round(time.monotonic() - started, 3), "output": output}


def validate_voice(path: Path, reference: Path) -> dict:
    require(path.is_file() and 0 < path.stat().st_size <= 64 * 1024 * 1024,
            "Expected a bounded .zvoice file")
    specification = importlib.util.spec_from_file_location("zsharp_voice_validation", VOICE_HELPER)
    require(specification is not None and specification.loader is not None, "Cannot load local voice validator")
    helper = importlib.util.module_from_spec(specification)
    specification.loader.exec_module(helper)
    document, conditioning = helper._voice_document(path)
    require(document["type"] == "clone" and document["backend"] == "pocket-tts",
            "A native pitch/speed preset is not a genuine cloned voice")
    require(re.fullmatch(r"[0-9a-f]{64}", document.get("modelFingerprint", "")) is not None,
            "Missing model fingerprint")
    require(isinstance(document.get("backendVersion"), str) and document["backendVersion"],
            "Missing backend version")
    require(document.get("reference", {}).get("sha256") == hashlib.sha256(reference.read_bytes()).hexdigest(),
            "Voice must be conditioned from this fixture's synthetic reference")
    seconds = document.get("reference", {}).get("seconds")
    require(isinstance(seconds, (float, int)) and math.isfinite(seconds) and 1 <= seconds <= 30,
            "Invalid reference duration")
    require(base64.b64decode(document["conditioning"]["data"], validate=True) == conditioning,
            "Conditioning failed its safetensors/checksum validation")
    return {"bytes": path.stat().st_size, "conditioningBytes": len(conditioning),
            "backendVersion": document["backendVersion"],
            "modelFingerprint": document["modelFingerprint"]}


def validate_wave(path: Path) -> dict:
    require(path.is_file() and 44 < path.stat().st_size <= 24_000 * 300 * 2 + 4096,
            "Expected bounded nonempty WAV output")
    with wave.open(str(path), "rb") as recording:
        require(recording.getnchannels() == 1, "Cloned speech must be mono")
        require(recording.getsampwidth() == 2 and recording.getcomptype() == "NONE",
                "Cloned speech must be uncompressed PCM16")
        require(recording.getframerate() == 24_000, "Pocket TTS output must be 24 kHz")
        frames = recording.getnframes()
        require(0 < frames <= 24_000 * 300, "Invalid synthesized duration")
        samples = array.array("h", recording.readframes(frames))
        if sys.byteorder != "little":
            samples.byteswap()
        require(len(samples) == frames, "Truncated synthesized WAV")
    rms = math.sqrt(sum(float(sample) ** 2 for sample in samples) / len(samples)) / 32768
    peak = max(abs(sample) for sample in samples) / 32768
    require(math.isfinite(rms) and math.isfinite(peak) and rms > 0.0001 and peak > 0.001,
            "Cloned speech must contain finite, non-silent PCM samples")
    return {"sampleRate": 24_000, "channels": 1, "pcmBits": 16,
            "seconds": round(frames / 24_000, 3), "rms": round(rms, 6), "peak": round(peak, 6)}


def invoke_helper(python: Path, fixture: Path, environment: dict[str, str],
                  label: str, *arguments: str) -> dict:
    result = fixture / (label + "-result.txt")
    metrics = run([str(python), "-I", str(VOICE_HELPER), *arguments,
                   "--result", str(result)], fixture, environment, label)
    require(result.read_bytes() == b"OK\n", f"{label} returned an error rather than exact OK newline: "
            + result.read_text(encoding="utf-8", errors="replace")[:3000])
    return {"seconds": metrics["seconds"]}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--speech-home", type=Path, default=default_home())
    parser.add_argument("--zvm", type=Path, help="Also exercise source and compiled native VM with dummy audio")
    parser.add_argument("--recognize", action="store_true", help="Require installed offline Vosk to recognize generated speech")
    parser.add_argument("--adjustments", action="store_true", help="Also synthesize a cloned preset with nondefault pitch and speed")
    arguments = parser.parse_args()
    home = arguments.speech_home.expanduser()
    require(home.is_absolute(), "--speech-home must be absolute")
    home = home.resolve(strict=True)
    python = home / "venv" / ("Scripts/python.exe" if os.name == "nt" else "bin/python")
    reference = home / "diagnostics" / "synthetic-reference.wav"
    configuration = home / "models" / "pocket-tts" / "model.yaml"
    require(python.is_file(), "Install the managed local speech interpreter first")
    require(reference.is_file(), "Expected the operator-owned diagnostics/synthetic-reference.wav")
    require(configuration.is_file(), "Install authorized genuine Pocket TTS weights/config first")
    zvm = arguments.zvm.resolve(strict=True) if arguments.zvm else None
    if zvm:
        require(zvm.is_file(), "--zvm must identify an executable file")
    environment = dict(os.environ)
    for name in ("HF_TOKEN", "HUGGING_FACE_HUB_TOKEN", "HUGGINGFACE_HUB_TOKEN", "ZSHARP_DISABLE_PROJECT_STARTS"):
        environment.pop(name, None)
    environment.update({"SDL_AUDIODRIVER": "dummy", "ZSHARP_SPEECH_HOME": str(home),
                        "ZSHARP_SPEECH_PYTHON": str(python), "ZSHARP_PYTHON_RUNTIME": str(python),
                        "ZSHARP_PYTHON_BRIDGE": str(ROOT / "native/python/zsharp_bridge.py"),
                        "ZSHARP_VOICE_MODEL_CONFIG": str(configuration),
                        "ZSHARP_SPEECH_MODEL": str(home / "models/vosk"),
                        "ZSHARP_SKIP_UPDATE_CHECK": "1", "ZSHARP_SKIP_ASSOCIATION_INSTALL": "1",
                        "ZSHARP_SKIP_DESKTOP_INTEGRATION": "1", "ZSHARP_DESKTOP_DISABLE_ACTIONS": "1",
                        "HF_HUB_OFFLINE": "1", "HF_DATASETS_OFFLINE": "1",
                        "TRANSFORMERS_OFFLINE": "1", "HF_HUB_DISABLE_TELEMETRY": "1"})
    metrics = {"physicalPlayback": False, "microphoneRecording": False,
               "subjectiveIdentityQualityTested": False}
    with tempfile.TemporaryDirectory(prefix="zsharp-real-speech-") as directory:
        fixture = Path(directory).resolve()
        copied_reference = fixture / "SyntheticReference.wav"
        shutil.copyfile(reference, copied_reference)
        voice = fixture / "HelperVoice.zvoice"
        text = fixture / "Speech.txt"
        output = fixture / "Generated.wav"
        text.write_text(PHRASE, encoding="utf-8")
        metrics["helperCreateWav"] = invoke_helper(python, fixture, environment, "create-wav", "create",
                                                  "--reference", str(copied_reference), "--output", str(voice))
        metrics["voice"] = validate_voice(voice, copied_reference)
        metrics["helperSynthesize"] = invoke_helper(python, fixture, environment, "synthesize", "synthesize",
                                                   "--voice", str(voice), "--text", str(text), "--output", str(output))
        metrics["audio"] = validate_wave(output)

        if arguments.adjustments:
            adjusted_voice = fixture / "AdjustedVoice.zvoice"
            adjusted = json.loads(voice.read_text(encoding="utf-8"))
            adjusted.update({"pitch": 110, "speed": 120})
            adjusted_voice.write_text(json.dumps(adjusted), encoding="utf-8")
            adjusted_output = fixture / "Adjusted.wav"
            metrics["adjustedSynthesize"] = invoke_helper(python, fixture, environment, "synthesize-adjusted", "synthesize",
                                                         "--voice", str(adjusted_voice), "--text", str(text),
                                                         "--output", str(adjusted_output))
            metrics["adjustedAudio"] = {**validate_wave(adjusted_output), "pitch": 110, "speed": 120}

        # Encode an actual MP3 from our own synthetic WAV; never rename WAV bytes.
        mp3 = fixture / "SyntheticReference.mp3"
        encoding = run([str(python), "-I", "-c",
                        "import json,sys,soundfile as sf; "
                        "subtypes=sf.available_subtypes('MP3'); "
                        "audio,rate=sf.read(sys.argv[1]); "
                        "sf.write(sys.argv[2],audio,rate,format='MP3') if subtypes else None; "
                        "print(json.dumps({'encoded':bool(subtypes),'subtypes':list(subtypes)}))",
                        str(copied_reference), str(mp3)], fixture, environment, "encode-mp3", timeout=30)
        mp3_status = json.loads(encoding["output"].strip().splitlines()[-1])
        require(mp3_status["encoded"] == mp3.is_file(), "MP3 encoder result does not match output")
        metrics["mp3Reference"] = mp3_status
        native_reference = mp3 if mp3.is_file() else copied_reference
        if mp3.is_file():
            require(mp3.read_bytes()[:4] != b"RIFF", "MP3 fixture must contain genuine encoded MP3, not WAV")
            if not zvm:
                mp3_voice = fixture / "Mp3Voice.zvoice"
                metrics["helperCreateMp3"] = invoke_helper(python, fixture, environment, "create-mp3", "create",
                                                          "--reference", str(mp3), "--output", str(mp3_voice))
                validate_voice(mp3_voice, mp3)

        if arguments.recognize:
            transcript = fixture / "Transcript.txt"
            transcript.touch()
            run([str(python), "-I", str(STT_HELPER), "preflight"], fixture, environment, "recognition-preflight", 60)
            run([str(python), "-I", str(STT_HELPER), "transcribe", str(output), str(transcript)],
                fixture, environment, "recognize-generated-speech", 60)
            recognized = transcript.read_text(encoding="utf-8").strip()
            words = set(re.findall(r"[a-z]+", recognized.lower()))
            require(len(words & {"local", "voice", "test", "completed", "successfully"}) >= 2,
                    "Generated speech failed the optional offline recognition signal check")
            metrics["recognizerSignal"] = {"text": recognized, "subjectiveIdentityQualityTested": False}
            repeated = run([str(python), "-I", "-c", REPEATED_RECOGNITION,
                            str(STT_HELPER), str(output), str(transcript)],
                           fixture, environment, "recognize-repeated-worker", 120)
            metrics["repeatedRecognition"] = {
                **json.loads(repeated["output"].strip().splitlines()[-1]),
                "seconds": repeated["seconds"],
            }

        if zvm:
            (fixture / "project.zsettings").write_text('''zsharp = type.settings
Project: "Real Model Speech Fixtures":
PID: "real_model_speech_fixtures":
Version: [1.4.0.0]:
Authors: ["Z# Tests"]:
Description: "Synthetic reference and dummy audio only":
ZSharp: [1.4.0.0]:
Dependencies ():
''', encoding="utf-8")
            source = fixture / "Main.zsharp"
            for mode, name in (("source", "NativeSource.zvoice"), ("bytecode", "NativeBytecode.zvoice")):
                source.write_text(f'''zsharp = type.script
noticed room Main[] (
 import ZSharp.Speech():
 noticed brain Start[] (
  Speech.createVoice("{native_reference.name}", "{name}"):
  Speech.prepare("{name}"):
  Speech.speak("{PHRASE}", "{name}"):
  Speech.speak("The cached voice is ready.", "{name}"):
  Print("ZSHARP_CLONE_QUEUED"):
 )
)
''', encoding="utf-8")
                if mode == "source":
                    command = [str(zvm), "run", str(source)]
                else:
                    bytecode = fixture / "Main.zbc"
                    run([str(zvm), "compile", str(source), "-o", str(bytecode)],
                        fixture, environment, "native-compile", 60)
                    command = [str(zvm), "run-bytecode", str(bytecode)]
                result = run(command, fixture, environment, "native-" + mode, 300)
                require(result["output"].splitlines().count("ZSHARP_CLONE_QUEUED") == 1,
                        f"Native {mode} did not execute its queued speech marker exactly once")
                document = validate_voice(fixture / name, native_reference)
                require(document["modelFingerprint"] == metrics["voice"]["modelFingerprint"],
                        "Native and direct helper must use the same authorized local model")
                metrics["native" + mode.title()] = {"seconds": result["seconds"], "referenceFormat": native_reference.suffix,
                                                       "completedWithDummyAudio": True}
            if arguments.recognize:
                # SDL's dummy capture supplies silence, never a real device.
                # This covers native persistent-worker RPC and end-of-run
                # shutdown in both source and serialized-bytecode execution.
                source.write_text('''zsharp = type.script
noticed room Main[] (
 import ZSharp.Microphone():
 import ZSharp.Speech():
 noticed brain Start[] (
  Speech.prepare("NativeSource.zvoice"):
  Print(Microphone.getInput):
  Speech.speak("The cached recognizer remains available while speech is generated.", "NativeSource.zvoice"):
  Print(Microphone.getInput):
  Print(Microphone.getInput):
  Print("ZSHARP_REPEATED_INPUT_OK"):
 )
)
''', encoding="utf-8")
                for mode in ("source", "bytecode"):
                    if mode == "source":
                        command = [str(zvm), "run", str(source)]
                    else:
                        bytecode = fixture / "Recognition.zbc"
                        run([str(zvm), "compile", str(source), "-o", str(bytecode)],
                            fixture, environment, "recognition-compile", 60)
                        command = [str(zvm), "run-bytecode", str(bytecode)]
                    repeated = run(command, fixture, environment, "native-recognition-" + mode, 90)
                    require(repeated["output"].splitlines().count("ZSHARP_REPEATED_INPUT_OK") == 1,
                            "Repeated native recognition failed its completion marker")
                    metrics["nativeRecognition" + mode.title()] = {
                        "utterances": 3, "dummyCapture": True,
                        "overlappedSynthesis": True,
                        "seconds": repeated["seconds"],
                    }
    print(json.dumps({"passed": True, **metrics}, sort_keys=True))
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exception:
        print(json.dumps({"passed": False, "error": str(exception)}), file=sys.stderr)
        raise SystemExit(1)
