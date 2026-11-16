"""Exercise updater handoffs and installer defaults without network or installs."""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import time


def capture_helper(kind):
    arguments = sys.argv[2:]
    if kind == "--capture-installer":
        # An already installed 1.4.0.0 bootstrap keeps its old compiled default.
        manifest = "https://www.zsharp.zombieos.com/update.js"
        if "--manifest" in arguments:
            manifest = arguments[arguments.index("--manifest") + 1]
        result = {"arguments": arguments, "manifest": manifest,
                  "installer": os.environ["ZSHARP_TEST_INSTALLER"]}
    else:
        output = Path(arguments[arguments.index("--output") + 1])
        output.write_text(json.dumps({"schema": 1, "latestVersion":
                                     os.environ["ZSHARP_TEST_VERSION"]}), encoding="utf-8")
        result = {"arguments": arguments, "url": arguments[-1]}
    destination = Path(os.environ["ZSHARP_TEST_CAPTURE"])
    temporary = destination.with_suffix(".tmp")
    temporary.write_text(json.dumps(result), encoding="utf-8")
    temporary.replace(destination)


def write_helper(path, kind, marker=""):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        '#!/bin/sh\n'
        f'export ZSHARP_TEST_INSTALLER="{marker}"\n'
        f'exec "$ZSHARP_TEST_PYTHON" "$ZSHARP_TEST_SCRIPT" {kind} "$@"\n',
        encoding="utf-8")
    path.chmod(0o755)


def isolated_environment(directory):
    environment = {key: value for key, value in os.environ.items()
                   if not key.startswith("ZSHARP_")}
    config = directory / "config"
    config.mkdir()
    environment.update({
        "XDG_CONFIG_HOME": str(config),
        "TMPDIR": str(directory),
        "ZSHARP_TEST_PYTHON": sys.executable,
        "ZSHARP_TEST_SCRIPT": str(Path(__file__).resolve()),
        "ZSHARP_TEST_CAPTURE": str(directory / "capture.json"),
    })
    return environment


def wait_for_capture(directory):
    capture = directory / "capture.json"
    deadline = time.monotonic() + 5
    while not capture.exists():
        if time.monotonic() >= deadline:
            raise AssertionError(f"updater never launched the isolated installer: {directory.name}")
        time.sleep(0.02)
    return json.loads(capture.read_text(encoding="utf-8"))


def option(arguments, name):
    assert arguments.count(name) == 1, (name, arguments)
    return arguments[arguments.index(name) + 1]


def main():
    if os.name == "nt":
        print("Updater manifest handoffs: skipped (POSIX harness)")
        return
    if len(sys.argv) != 3:
        raise SystemExit("usage: updater-manifest-tests.py DRIVER INSTALLER")
    driver, installer = (Path(argument).resolve() for argument in sys.argv[1:])
    version = subprocess.check_output([str(driver), "--version"], text=True).strip()
    assert re.fullmatch(r"\d+\.\d+\.\d+\.\d+", version), version
    operating_system = "macos" if sys.platform == "darwin" else "linux"
    mirror = "https://mirror.example/zsharp/update.js?channel=owned-test"
    checks = 0
    with tempfile.TemporaryDirectory(prefix="zsharp-updater-manifest-") as temporary:
        root = Path(temporary)
        for mode in ("manual", "background"):
            for channel in ("stable", "beta"):
                for custom in (False, True):
                    directory = root / f"{mode}-{channel}-{'mirror' if custom else 'official'}"
                    directory.mkdir()
                    runtime = directory / "updater-driver"
                    shutil.copy2(driver, runtime)
                    write_helper(directory / "zsharp-installer", "--capture-installer", "sibling")
                    override = directory / "override" / "old-installer"
                    write_helper(override, "--capture-installer", "override")
                    environment = isolated_environment(directory)
                    # Manual override is supported; background updates use the sibling.
                    environment["ZSHARP_UPDATE_INSTALLER"] = str(override)
                    if custom:
                        environment["ZSHARP_INSTALLER_MANIFEST_URL"] = mirror
                    result = subprocess.run([str(runtime), mode, channel], env=environment,
                                            capture_output=True, text=True, timeout=10)
                    assert result.returncode == 0, result.stdout + result.stderr
                    captured = wait_for_capture(directory)
                    expected = (mirror if custom and channel == "stable" else
                                f"https://zsharp.zombieos.com/{'beta' if channel == 'beta' else 'update'}.js?v={version}-{operating_system}")
                    arguments = captured["arguments"]
                    assert option(arguments, "--manifest") == expected, captured
                    assert captured["manifest"] == expected, captured
                    assert captured["installer"] == ("override" if mode == "manual" else "sibling"), captured
                    assert option(arguments, "--current-version") == version, captured
                    assert option(arguments, "--wait-pid").isdigit(), captured
                    assert "--check" in arguments and "--quiet" in arguments, captured
                    assert ("--notify-result" in arguments) == (mode == "manual"), captured
                    checks += 1

        directory = root / "installer-default"
        directory.mkdir()
        environment = isolated_environment(directory)
        write_helper(directory / "bin" / "curl", "--capture-curl")
        environment["PATH"] = str(directory / "bin") + os.pathsep + os.defpath
        environment["ZSHARP_TEST_VERSION"] = version
        environment["ZSHARP_INSTALLER_SKIP_INTEGRATION"] = "1"
        environment["ZSHARP_INSTALLER_SKIP_HUB_SHORTCUT"] = "1"
        result = subprocess.run([str(installer), "--check", "--current-version", version,
                                 "--install-dir", str(directory / "install")],
                                env=environment, capture_output=True, text=True, timeout=10)
        assert result.returncode == 0, result.stdout + result.stderr
        assert "already current" in result.stdout, result.stdout
        captured = wait_for_capture(directory)
        assert captured["url"] == f"https://zsharp.zombieos.com/update.js?v={version}-{operating_system}", captured
        assert not (directory / "install" / "zsharp").exists()
        checks += 1
    print(f"Updater manifest migration: {checks} launch/default checks passed")


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] in ("--capture-installer", "--capture-curl"):
        capture_helper(sys.argv[1])
    else:
        main()
