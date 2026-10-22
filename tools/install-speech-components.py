"""Install optional local ZVM speech components in a private per-user directory.

Run with a full Python 3.10..3.14 installation (the bundled ZVM Python works).
Never installs globally, changes the OS audio device, records, or plays audio.
Kyutai's gated model requires the user's own lawful consent/accepted model terms;
an explicitly supplied HF_TOKEN may authenticate a previously authorized account.
An authorized browser download can instead be imported with --voice-weights;
its size and SHA256 must match official metadata for the installed model revision.
This script never logs into an account or substitutes non-cloning model weights.
"""
from __future__ import annotations

import argparse
import datetime
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import shutil
import stat
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.parse
import urllib.request
import uuid
import venv
import zipfile


PACKAGES = {"pocket-tts": "3.3.0", "vosk": "0.3.44" if sys.platform == "darwin" else "0.3.45", "soundfile": "0.14.0",
            "PyYAML": "6.0.3", "librosa": "0.11.0" if sys.version_info < (3, 12) else "1.0.0",
            "torch": "2.14.1", "srt": "3.5.3"}
VOSK_URL = "https://alphacephei.com/vosk/models/vosk-model-small-en-us-0.15.zip"
MANIFEST = "components.json"
IDENTITY = "zsharp-local-speech-components"
MAX_DOWNLOAD = 1024 * 1024 * 1024


class ModelRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, request, response, code, message, headers, new_url):
        parsed = urllib.parse.urlsplit(new_url)
        host = (parsed.hostname or "").lower()
        if parsed.scheme != "https" or not (host in ("huggingface.co", "alphacephei.com") or
                host.endswith(".huggingface.co") or host.endswith(".hf.co")):
            raise RuntimeError("Model download redirected outside its official HTTPS distribution hosts.")
        redirected = super().redirect_request(request, response, code, message, headers, new_url)
        if redirected is not None and parsed.hostname != urllib.parse.urlsplit(request.full_url).hostname:
            # HF authorizes first, then uses a signed CDN URL; never forward the
            # operator's bearer token to a different server.
            redirected.remove_header("Authorization")
        return redirected


def default_home() -> Path:
    if "ZSHARP_SPEECH_HOME" in os.environ:
        override = os.environ["ZSHARP_SPEECH_HOME"].strip()
        root = Path(override).expanduser()
        if not override or not root.is_absolute():
            raise RuntimeError("ZSHARP_SPEECH_HOME must identify an absolute managed speech directory.")
        return root
    if sys.platform == "win32":
        return Path(os.environ.get("LOCALAPPDATA", Path.home() / "AppData/Local")) / "ZombieOS/ZSharp/speech"
    if sys.platform == "darwin":
        return Path.home() / "Library/Application Support/ZSharp/speech"
    base = Path(os.environ.get("XDG_DATA_HOME", Path.home() / ".local/share")).expanduser()
    if not base.is_absolute():
        base = Path.home() / ".local/share"
    return base / "zsharp/speech"


def digest(path: Path) -> str:
    result = hashlib.sha256()
    with path.open("rb") as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b""):
            result.update(chunk)
    return result.hexdigest()


def save_manifest(home: Path, document: dict) -> None:
    document["updatedUTC"] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    temporary = home / (".manifest-" + uuid.uuid4().hex + ".tmp")
    with temporary.open("x", encoding="utf-8") as stream:
        json.dump(document, stream, indent=2, sort_keys=True)
        stream.write("\n")
    temporary.replace(home / MANIFEST)


def prepare_home(home: Path) -> dict:
    if not home.is_absolute() or home.resolve() in (Path.home().resolve(), Path(home.anchor)):
        raise RuntimeError("Speech home must be a dedicated absolute directory, not a home directory or filesystem root.")
    if home.is_symlink():
        raise RuntimeError("Refusing a symlink as the managed speech directory.")
    home.mkdir(parents=True, exist_ok=True)
    manifest_path = home / MANIFEST
    if manifest_path.exists():
        document = json.loads(manifest_path.read_text(encoding="utf-8"))
        if document.get("identity") != IDENTITY or document.get("version") != 1:
            raise RuntimeError("Existing speech directory belongs to another installer/version; nothing was replaced.")
        return document
    if any(home.iterdir()):
        raise RuntimeError("Existing speech directory has unmanaged files. Choose a new --home; nothing was replaced.")
    document = {"identity": IDENTITY, "version": 1, "artifacts": {}, "models": {}, "packages": {}}
    save_manifest(home, document)
    return document


def run(command: list[str], timeout=1200, capture=False) -> str:
    # Argument vector only, not a shell. Do not print credential environment values.
    print("Preparing " + ("Python packages" if "pip" in command else "local speech environment") + "...", flush=True)
    result = subprocess.run(command, timeout=timeout, check=True, text=True,
                            stdout=subprocess.PIPE if capture else None)
    return result.stdout if capture else ""


def install_packages(home: Path, document: dict) -> Path:
    environment = home / "venv"
    python = environment / ("Scripts/python.exe" if sys.platform == "win32" else "bin/python")
    if not python.exists():
        print("Creating a private Python environment at " + str(environment), flush=True)
        venv.EnvBuilder(with_pip=True, clear=False).create(environment)
    if not (environment / "pyvenv.cfg").is_file():
        raise RuntimeError("Managed Python directory is not a virtual environment.")
    pins = [name + "==" + version for name, version in PACKAGES.items()]
    # PyPI's Linux torch default includes NVIDIA runtimes; select the official
    # CPU build first. Windows/macOS default wheels are already CPU-only.
    if sys.platform.startswith("linux"):
        run([str(python), "-I", "-m", "pip", "install", "--disable-pip-version-check",
             "--index-url", "https://download.pytorch.org/whl/cpu", "torch==" + PACKAGES["torch"]])
    report = home / "pip-install-report.json"
    run([str(python), "-I", "-m", "pip", "install", "--disable-pip-version-check",
         "--only-binary=:all:", "--no-binary=srt", "--report", str(report)] + pins)
    versions = run([str(python), "-I", "-c",
        "import importlib.metadata as m,json; print(json.dumps({d.metadata['Name']:d.version for d in m.distributions()}))"], capture=True)
    document["packages"] = json.loads(versions)
    document["python"] = str(python)
    document["packageSources"] = {name: "https://pypi.org/project/" + name + "/" + version + "/"
                                  for name, version in PACKAGES.items()}
    save_manifest(home, document)
    return python


def download(home: Path, document: dict, url: str, filename: str, token: str | None = None) -> Path:
    parsed = urllib.parse.urlsplit(url)
    if parsed.scheme != "https" or parsed.hostname not in ("huggingface.co", "alphacephei.com"):
        raise RuntimeError("Unexpected speech model source.")
    cache = home / "downloads"
    cache.mkdir(exist_ok=True)
    destination = cache / filename
    record = document["artifacts"].get(filename)
    if destination.exists():
        if not record or record.get("url") != url or digest(destination) != record.get("sha256"):
            raise RuntimeError("An existing model download is unmanaged or changed; it was not overwritten: " + str(destination))
        print("Verified cached " + filename, flush=True)
        return destination
    part = cache / (".download-" + uuid.uuid4().hex + ".tmp")
    headers = {"User-Agent": "ZSharp-LocalSpeechSetup/1.4.0.0"}
    if token and parsed.hostname == "huggingface.co":
        headers["Authorization"] = "Bearer " + token
    request = urllib.request.Request(url, headers=headers)
    started = time.monotonic()
    count = 0
    try:
        print("Downloading official model asset " + filename + "...", flush=True)
        opener = urllib.request.build_opener(ModelRedirect())
        with opener.open(request, timeout=30) as response, part.open("xb") as output:
            expected = int(response.headers.get("Content-Length", "0"))
            if expected > MAX_DOWNLOAD:
                raise RuntimeError("Model download exceeds the 1GiB per-file limit.")
            while True:
                chunk = response.read(1024 * 1024)
                if not chunk:
                    break
                output.write(chunk)
                count += len(chunk)
                if count > MAX_DOWNLOAD or time.monotonic() - started > 600:
                    raise RuntimeError("Model download exceeded its size/time limit.")
                if count // (20 * 1024 * 1024) != (count - len(chunk)) // (20 * 1024 * 1024):
                    print(f"  {count // (1024 * 1024)} MiB received", flush=True)
            if expected and count != expected:
                raise RuntimeError("Model download is incomplete.")
        if count == 0:
            raise RuntimeError("Empty model download.")
        sha = digest(part)
        # Installer owns the new cache entry; never replace a preexisting file.
        with destination.open("xb") as output, part.open("rb") as source:
            shutil.copyfileobj(source, output)
        document["artifacts"][filename] = {"url": url, "bytes": count, "sha256": sha,
                                           "verifiedVia": "official HTTPS download; local SHA256 recorded"}
        save_manifest(home, document)
        return destination
    except urllib.error.HTTPError as exception:
        if exception.code in (401, 403) and parsed.hostname == "huggingface.co":
            raise RuntimeError("Kyutai voice-cloning model is gated. Accept its terms at https://huggingface.co/kyutai/pocket-tts with your own account, then rerun with an explicitly supplied authorized HF_TOKEN. No account login or fallback was performed.") from exception
        raise
    finally:
        part.unlink(missing_ok=True)


def hf_reference(reference: str) -> tuple[str, str, str]:
    if not reference.startswith("hf://") or "@" not in reference:
        raise RuntimeError("Pocket TTS package did not supply an immutable HF model revision.")
    repository_and_path, revision = reference[5:].rsplit("@", 1)
    parts = repository_and_path.split("/")
    allowed = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789._-"
    if (len(parts) < 3 or any(not part or part in (".", "..") or
            any(character not in allowed for character in part) for part in parts) or
            len(revision) != 40 or any(character not in "0123456789abcdef" for character in revision)):
        raise RuntimeError("Unexpected Pocket TTS model reference.")
    return "/".join(parts[:2]), "/".join(parts[2:]), revision


def hf_url(reference: str) -> str:
    repository, filename, revision = hf_reference(reference)
    return f"https://huggingface.co/{repository}/resolve/{revision}/{filename}"


def official_voice_identity(reference: str) -> tuple[int, str, str]:
    """Read public metadata, not gated weights or an operator's credentials."""
    repository, filename, revision = hf_reference(reference)
    if repository != "kyutai/pocket-tts" or not filename.endswith(".safetensors"):
        raise RuntimeError("Local voice weights must match the genuine kyutai/pocket-tts model reference.")
    metadata_url = f"https://huggingface.co/api/models/{repository}/revision/{revision}?blobs=true"
    request = urllib.request.Request(metadata_url, headers={
        "User-Agent": "ZSharp-LocalSpeechSetup/1.4.0.0", "Accept": "application/json"})
    opener = urllib.request.build_opener(ModelRedirect())
    with opener.open(request, timeout=30) as response:
        payload = response.read(4 * 1024 * 1024 + 1)
    if len(payload) > 4 * 1024 * 1024:
        raise RuntimeError("Official voice model metadata exceeds its size limit.")
    try:
        metadata = json.loads(payload)
    except (ValueError, UnicodeError) as exception:
        raise RuntimeError("Official voice model metadata is not valid JSON.") from exception
    if not isinstance(metadata, dict) or metadata.get("sha") != revision or not isinstance(metadata.get("siblings"), list):
        raise RuntimeError("Official voice model metadata does not match the installed model revision.")
    matches = [entry for entry in metadata["siblings"] if isinstance(entry, dict) and entry.get("rfilename") == filename]
    if len(matches) != 1:
        raise RuntimeError("Official metadata does not identify exactly one matching voice model asset.")
    entry = matches[0]
    size, lfs = entry.get("size"), entry.get("lfs")
    sha = lfs.get("sha256") if isinstance(lfs, dict) else None
    if (type(size) is not int or not 0 < size <= MAX_DOWNLOAD or not isinstance(lfs, dict) or
            type(lfs.get("size")) is not int or lfs["size"] != size or not isinstance(sha, str) or
            len(sha) != 64 or any(character not in "0123456789abcdef" for character in sha)):
        raise RuntimeError("Official voice model metadata has no valid bounded size and SHA256.")
    return size, sha, metadata_url


def import_voice_weights(home: Path, document: dict, reference: str, source: Path) -> Path:
    """Import only exact, publisher-verified bytes; never adopt an unknown cache."""
    expected_size, expected_sha, metadata_url = official_voice_identity(reference)
    original_url = hf_url(reference)
    filename = "pocket-tts-english.safetensors"
    cache = home / "downloads"
    if cache.is_symlink() or cache.resolve() != home.resolve() / "downloads":
        raise RuntimeError("Managed model cache must be a local directory, not a redirected path.")
    cache.mkdir(exist_ok=True)
    destination = cache / filename
    record = document["artifacts"].get(filename)
    if destination.exists() or destination.is_symlink():
        if (destination.is_symlink() or not destination.is_file() or not record or
                record.get("url") != original_url or record.get("bytes") != expected_size or
                record.get("sha256") != expected_sha or destination.stat().st_size != expected_size or
                digest(destination) != expected_sha):
            raise RuntimeError("Existing voice model cache is unmanaged or changed; it was not overwritten.")
        print("Verified cached voice weights against official metadata.", flush=True)
        return destination
    source = source.expanduser()
    details = source.stat()
    if source.is_symlink() or not stat.S_ISREG(details.st_mode) or source.suffix.lower() != ".safetensors":
        raise RuntimeError("--voice-weights must name a regular .safetensors browser download, not a symlink.")
    if details.st_size != expected_size:
        raise RuntimeError("Browser-downloaded voice weights have the wrong size or are incomplete; nothing was imported.")
    staging = cache / (".voice-import-" + uuid.uuid4().hex + ".tmp")
    count = 0
    imported_sha = hashlib.sha256()
    try:
        with source.open("rb") as input_stream, staging.open("xb") as output:
            for chunk in iter(lambda: input_stream.read(1024 * 1024), b""):
                count += len(chunk)
                if count > expected_size:
                    raise RuntimeError("Browser-downloaded voice weights changed size during import.")
                imported_sha.update(chunk)
                output.write(chunk)
            output.flush()
            os.fsync(output.fileno())
        if count != expected_size or imported_sha.hexdigest() != expected_sha:
            raise RuntimeError("Browser-downloaded voice weights do not match the official SHA256 and size; nothing was imported.")
        # Hard-link publication is atomic and never replaces a competing file
        # on Windows or POSIX. The verified staging inode is on the same volume.
        os.link(staging, destination)
        document["artifacts"][filename] = {"url": original_url, "bytes": count, "sha256": expected_sha,
            "metadataUrl": metadata_url, "importedFrom": str(source.resolve()),
            "verifiedVia": "official HTTPS revision metadata SHA256 and size; authorized browser download imported"}
        save_manifest(home, document)
        print("Imported browser-downloaded voice weights verified against official metadata.", flush=True)
        return destination
    finally:
        staging.unlink(missing_ok=True)


def install_voice(home: Path, document: dict, python: Path, voice_weights: Path | None = None) -> None:
    model_directory = home / "models/pocket-tts"
    configuration_path = model_directory / "model.yaml"
    old = document["models"].get("pocket-tts")
    if configuration_path.exists():
        if not old or digest(configuration_path) != old.get("configSha256"):
            raise RuntimeError("Existing Pocket TTS config is unmanaged or changed; it was not replaced.")
        for name, expected in old["files"].items():
            if digest(model_directory / name) != expected:
                raise RuntimeError("Existing Pocket TTS asset changed; it was not overwritten.")
        print("Verified installed Pocket TTS model.", flush=True)
        return
    query = ("import importlib.metadata as m,json,yaml; "
             "d=m.distribution('pocket-tts'); p=d.locate_file('pocket_tts/config/english.yaml'); "
             "print(json.dumps(yaml.safe_load(p.read_text(encoding='utf-8'))))")
    configuration = json.loads(run([str(python), "-I", "-c", query], capture=True))
    weights = configuration.get("weights_path")
    tokenizer = configuration["flow_lm"]["lookup_table"]["tokenizer_path"]
    if not isinstance(weights, str):
        raise RuntimeError("Pocket TTS package has no genuine model weights reference.")
    # Authenticate only when a token was explicitly supplied by the operator.
    token = os.environ.get("HF_TOKEN")
    downloaded_weights = (import_voice_weights(home, document, weights, voice_weights) if voice_weights is not None else
                          download(home, document, hf_url(weights), "pocket-tts-english.safetensors", token))
    downloaded_tokenizer = download(home, document, hf_url(tokenizer), "pocket-tts-english-tokenizer.json", token)
    model_directory.mkdir(parents=True, exist_ok=True)
    files = {}
    for source, name in ((downloaded_weights, "model.safetensors"), (downloaded_tokenizer, "tokenizer.json")):
        destination = model_directory / name
        if destination.exists():
            if digest(destination) != digest(source):
                raise RuntimeError("Refusing to overwrite an existing Pocket model asset: " + str(destination))
        else:
            with destination.open("xb") as output, source.open("rb") as input_stream:
                shutil.copyfileobj(input_stream, output)
        files[name] = digest(destination)
    configuration["weights_path"] = str((model_directory / "model.safetensors").resolve())
    configuration.pop("weights_path_without_voice_cloning", None)
    configuration["flow_lm"]["lookup_table"]["tokenizer_path"] = str((model_directory / "tokenizer.json").resolve())
    # Use venv PyYAML, not an unrequested global dependency in the installer.
    temporary = home / (".model-config-" + uuid.uuid4().hex + ".json")
    temporary.write_text(json.dumps(configuration), encoding="utf-8")
    try:
        run([str(python), "-I", "-c", "import json,pathlib,sys,yaml; p=pathlib.Path(sys.argv[2]); "
             "s=p.open('x',encoding='utf-8'); yaml.safe_dump(json.loads(pathlib.Path(sys.argv[1]).read_text(encoding='utf-8')),s); s.close()",
             str(temporary), str(configuration_path)])
    finally:
        temporary.unlink(missing_ok=True)
    document["models"]["pocket-tts"] = {"configSha256": digest(configuration_path), "files": files,
        "source": "https://huggingface.co/kyutai/pocket-tts", "license": "CC-BY-4.0 and publisher use terms",
        "purpose": "genuine speaker conditioning / voice cloning; no non-cloning fallback"}
    save_manifest(home, document)


def install_vosk(home: Path, document: dict) -> None:
    destination = home / "models/vosk"
    old = document["models"].get("vosk")
    if destination.exists():
        if not old:
            raise RuntimeError("Existing Vosk model directory is unmanaged; it was not replaced.")
        for name, expected in old["files"].items():
            if digest(destination / name) != expected:
                raise RuntimeError("Existing Vosk model file changed; it was not overwritten.")
        print("Verified installed Vosk model.", flush=True)
        return
    archive = download(home, document, VOSK_URL, "vosk-model-small-en-us-0.15.zip")
    destination.parent.mkdir(parents=True, exist_ok=True)
    staging = Path(tempfile.mkdtemp(prefix=".vosk-stage-", dir=destination.parent))
    try:
        with zipfile.ZipFile(archive) as zipped:
            total = 0
            entries = zipped.infolist()
            if len(entries) > 10000:
                raise RuntimeError("Vosk archive contains too many files.")
            for entry in entries:
                parts = PurePosixPath(entry.filename).parts
                total += entry.file_size
                permissions = entry.external_attr >> 16
                if (not parts or parts[0] != "vosk-model-small-en-us-0.15" or
                    any(part in (".", "..") or ":" in part or "\\" in part for part in parts) or
                    stat.S_ISLNK(permissions) or total > MAX_DOWNLOAD):
                    raise RuntimeError("Unsafe or oversized Vosk model archive.")
                target = staging.joinpath(*parts[1:])
                if entry.is_dir():
                    target.mkdir(parents=True, exist_ok=True)
                else:
                    target.parent.mkdir(parents=True, exist_ok=True)
                    with zipped.open(entry) as source, target.open("xb") as output:
                        shutil.copyfileobj(source, output)
        if not (staging / "am/final.mdl").is_file() or not (staging / "conf/model.conf").is_file():
            raise RuntimeError("Vosk archive lacks its required model files.")
        files = {path.relative_to(staging).as_posix(): digest(path) for path in staging.rglob("*") if path.is_file()}
        staging.rename(destination)
        document["models"]["vosk"] = {"name": "vosk-model-small-en-us-0.15", "files": files,
            "source": VOSK_URL, "license": "Apache-2.0 (publisher model listing)"}
        save_manifest(home, document)
    finally:
        # This freshly generated staging directory is strictly inside the
        # installer-owned models directory; never recurse into an external path.
        if staging.exists() and staging.parent.resolve() == destination.parent.resolve() and staging.name.startswith(".vosk-stage-"):
            shutil.rmtree(staging)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--home", type=Path, default=default_home())
    parser.add_argument("--models-only", action="store_true", help="retry model provisioning without reinstalling Python packages")
    parser.add_argument("--voice-weights", type=Path,
                        help="import an authorized .safetensors browser download after official revision/hash verification")
    arguments = parser.parse_args()
    if not (3, 10) <= sys.version_info[:2] < (3, 15):
        raise RuntimeError("Use a full Python 3.10..3.14 interpreter (normally the ZVM bundled Python).")
    home = arguments.home.expanduser().absolute()
    document = prepare_home(home)
    python = home / "venv" / ("Scripts/python.exe" if sys.platform == "win32" else "bin/python")
    if not arguments.models_only:
        python = install_packages(home, document)
    elif not python.is_file():
        raise RuntimeError("No managed speech environment exists; omit --models-only for initial setup.")
    failures = []
    for name, action in (("Vosk", lambda: install_vosk(home, document)),
                         ("Pocket TTS", lambda: install_voice(home, document, python, arguments.voice_weights))):
        try:
            action()
        except Exception as exception:
            message = name + ": " + str(exception)
            print(message, file=sys.stderr, flush=True)
            failures.append(message)
    document["lastSetupFailures"] = failures
    save_manifest(home, document)
    print("Managed speech components: " + str(home), flush=True)
    print("No microphone was recorded and no audio was played.", flush=True)
    return 2 if failures else 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except (RuntimeError, OSError, subprocess.SubprocessError) as exception:
        print("Speech component setup stopped: " + str(exception), file=sys.stderr)
        raise SystemExit(1)
