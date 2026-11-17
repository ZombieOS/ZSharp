"""Public CLI source-package cache/integrity regressions, without a desktop.

Linux Landlock keeps legacy terminal/error-log writes out of the user's HOME.
Extraction must reach the existing forced-window-failure hook; these tests do
not need GTK, a display server, or changes to the user's desktop settings.
"""
from __future__ import annotations

import argparse
import ctypes
import hashlib
import os
from pathlib import Path
import platform
import shutil
import struct
import subprocess
import sys
import tempfile
import zipfile
import zlib


PID = "package_cache_regression"
VERSION = "1.0.0.0"
MARKER = ".zsharp-package-complete"
FORCED = "forced window launch failure"
COLLISION = "Assets/collision.bin"


def restrict_writes(root):
    class Ruleset(ctypes.Structure):
        _fields_ = [("handled_access_fs", ctypes.c_uint64)]

    class Beneath(ctypes.Structure):
        _layout_ = "ms"
        _pack_ = 1
        _fields_ = [("allowed_access", ctypes.c_uint64), ("parent_fd", ctypes.c_int32)]

    libc = ctypes.CDLL(None, use_errno=True)
    writes = sum(1 << bit for bit in (1, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14))
    attributes = Ruleset(writes)
    ruleset = libc.syscall(444, ctypes.byref(attributes), ctypes.sizeof(attributes), 0)
    if ruleset < 0:
        raise OSError(ctypes.get_errno(), "landlock_create_ruleset")
    directory = os.open(root, os.O_PATH | os.O_CLOEXEC)
    try:
        rule = Beneath(writes, directory)
        if libc.syscall(445, ruleset, 1, ctypes.byref(rule), 0):
            raise OSError(ctypes.get_errno(), "landlock_add_rule")
        if libc.prctl(38, 1, 0, 0, 0) or libc.syscall(446, ruleset, 0):
            raise OSError(ctypes.get_errno(), "landlock_restrict_self")
    finally:
        os.close(directory)
        os.close(ruleset)


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def snapshot(paths):
    result = {}
    for path in paths:
        assert path.is_file() and not path.is_symlink(), path
        status = path.stat()
        result[path] = (status.st_ino, status.st_size, status.st_mtime_ns)
    return result


def freeze(paths):
    # An old timestamp also makes rewrites visible on coarse timestamp clocks.
    for index, path in enumerate(paths):
        status = path.stat()
        os.utime(path, ns=(status.st_atime_ns, 1_650_000_000_123_456_789 + index))


def zip_payload_offset(data, entry):
    assert data[entry.header_offset:entry.header_offset + 4] == b"PK\x03\x04"
    name_size, extra_size = struct.unpack_from("<HH", data, entry.header_offset + 26)
    return entry.header_offset + 30 + name_size + extra_size


def compiled_payload_offset(data, relative):
    assert data[:8] == b"ZSPKG1\r\n"
    offset = 8 + 4 + 4 + 4 * 4
    for _ in range(2):
        size = struct.unpack_from("<I", data, offset)[0]
        offset += 4 + size
    count = struct.unpack_from("<I", data, offset)[0]
    offset += 4
    for _ in range(count):
        name_size = struct.unpack_from("<I", data, offset)[0]
        offset += 4
        name = data[offset:offset + name_size].decode()
        offset += name_size
        size = struct.unpack_from("<Q", data, offset)[0]
        offset += 8 + 32
        if name == relative:
            assert size > 0
            return offset
        offset += size
    raise AssertionError(f"Compiled package omitted {relative}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    options = parser.parse_args()
    if sys.platform != "linux" or platform.machine() not in ("x86_64", "aarch64"):
        print("Package cache regressions require Linux x86_64/aarch64 write isolation.")
        return 77
    libc = ctypes.CDLL(None, use_errno=True)
    if libc.syscall(444, 0, 0, 1) < 3:
        print("Package cache regressions require Landlock ABI 3+; HOME remains unchanged.")
        return 77
    binary = options.binary.resolve(strict=True)
    checks = 0
    with tempfile.TemporaryDirectory(prefix="zsharp-package-cache-") as temporary:
        root = Path(temporary).resolve()
        project = root / "project with spaces #"
        (project / "Assets").mkdir(parents=True)
        (project / "project.zsettings").write_text(f'''zsharp = type.settings
Project: "Package Cache Regression":
PID: "{PID}":
Version: [{VERSION}]:
Authors: ["Z# Tests"]:
Description: "Owned cache and CRC fixtures":
ZSharp: [1.4.1.0]:
Dependencies (
 zsharpwindow:1.0.0.0
):
Window (
 Startup: "Main.zsharp":
):
''')
        (project / "Main.zsharp").write_text('''zsharp = type.script:window
noticed Window Main[] (
 import ZSharp.Window.Design():
 noticed design Design[] (
  title: "Owned package cache fixture":
  width: 200px:
  height: 100px:
 )
)
''')
        assets = {"Assets/empty.bin": b"", "Assets/known.bin": b"123456789",
                  COLLISION: b"plumless"}
        for size in (65535, 65536, 65537, 131073):
            assets[f"Assets/boundary-{size}.bin"] = bytes((index * 73 + 19) % 256 for index in range(size))
        for relative, data in assets.items():
            (project / relative).write_bytes(data)
        assert zlib.crc32(assets["Assets/known.bin"]) == 0xcbf43926
        assert zlib.crc32(b"plumless") == zlib.crc32(b"buckeroo") == 0x4ddb0c25
        environment = {key: value for key, value in os.environ.items() if not key.startswith("ZSHARP_")}
        for name in ("config", "data", "cache", "state", "runtime", "tmp", "Desktop", "package-cache"):
            (root / name).mkdir(mode=0o700)
        for key, name in (("XDG_CONFIG_HOME", "config"), ("XDG_DATA_HOME", "data"),
                          ("XDG_CACHE_HOME", "cache"), ("XDG_STATE_HOME", "state"),
                          ("XDG_RUNTIME_DIR", "runtime"), ("TMPDIR", "tmp")):
            environment[key] = str(root / name)
        environment.update({"ZSHARP_PACKAGE_CACHE": str(root / "package-cache"),
            "ZSHARP_PROJECT_REGISTRY": str(root / "projects.json"),
            "ZSHARP_PACKAGE_REGISTRY": str(root / "packages.json"),
            "ZSHARP_PLAY_STATS_REGISTRY": str(root / "playtime.json"),
            "ZSHARP_DESKTOP_DIRECTORY": str(root / "Desktop"),
            "ZSHARP_SKIP_DESKTOP_INTEGRATION": "1", "ZSHARP_SKIP_ASSOCIATION_INSTALL": "1",
            "ZSHARP_SKIP_UPDATE_CHECK": "1", "ZSHARP_HUB_CONSOLE_ONLY": "1",
            "ZSHARP_WINDOW_FORCE_FAILURE": "1", "GSETTINGS_BACKEND": "memory",
            "GIO_USE_VFS": "local", "DBUS_SESSION_BUS_ADDRESS": "unix:path=" + str(root / "no-bus"),
            "DISPLAY": "", "WAYLAND_DISPLAY": "no-test-display", "GDK_BACKEND": "wayland"})
        assert environment.get("HOME") == os.environ.get("HOME")
        outside = root.parent / (root.name + "-outside-proof")
        proof = subprocess.run([sys.executable, "-c", '''
from pathlib import Path
import sys
Path(sys.argv[1]).write_text("allowed")
try: Path(sys.argv[2]).write_text("must be denied")
except PermissionError: pass
else: raise RuntimeError("Landlock did not restrict outside writes")
''', str(root / "write-proof"), str(outside)], env=environment,
            capture_output=True, text=True, preexec_fn=lambda: restrict_writes(root))
        assert proof.returncode == 0 and not outside.exists(), proof.stdout + proof.stderr

        def native(arguments, env=None):
            result = subprocess.run([str(binary), *map(str, arguments)], env=env or environment,
                cwd=root, capture_output=True, text=True, timeout=20,
                preexec_fn=lambda: restrict_writes(root))
            assert result.returncode >= 0, ("native process crashed", arguments, result.stdout, result.stderr)
            return result, result.stdout + result.stderr

        def open_valid(package, env=None):
            nonlocal checks
            result, output = native(["open", package], env)
            assert result.returncode != 0 and FORCED in output, (package, output)
            checks += 1

        def open_invalid(package, message, env=None):
            nonlocal checks
            result, output = native(["open", package], env)
            assert result.returncode != 0 and FORCED not in output and message.lower() in output.lower(), output
            checks += 1

        result, output = native(["package", "app", project, "Cache", "--unbytecode"])
        assert result.returncode == 0, output
        checks += 1
        source = project / "Packages/Cache-unbytecoded.zapp"
        compiled = project / "Packages/Cache.zapp"
        with zipfile.ZipFile(source) as archive:
            assert archive.testzip() is None
            entries = {item.filename: archive.read(item) for item in archive.infolist()}
            for relative, data in entries.items():
                item = archive.getinfo(relative)
                assert item.compress_type == zipfile.ZIP_STORED
                assert item.CRC == zlib.crc32(data) and item.file_size == len(data)
                checks += 1
            collision_entry = archive.getinfo(COLLISION)
            archive_comment = archive.comment
        source_bytes = source.read_bytes()
        collision_offset = zip_payload_offset(source_bytes, collision_entry)
        cache_base = root / "package-cache"

        def cache_for(package):
            return cache_base / PID / VERSION / digest(package)

        def assert_entries(cache, expected=entries):
            for relative, data in expected.items():
                path = cache / relative
                assert path.is_file() and not path.is_symlink() and path.read_bytes() == data, relative

        open_valid(source)
        cache = cache_for(source)
        marker = cache / MARKER
        marker_bytes = marker.read_bytes()
        assert marker_bytes
        assert_entries(cache)
        paths = [cache / relative for relative in entries] + [marker]
        freeze(paths)
        first = snapshot(paths)
        open_valid(source)
        open_valid(source)
        assert snapshot(paths) == first, "Valid warm cache entries or marker were rewritten"

        target = cache / COLLISION
        for data in (b"x", b"BUCKAROO", b"buckeroo"):
            siblings = [path for path in paths if path not in (target, marker)]
            freeze(siblings)
            before = snapshot(siblings)
            previous = target.stat()
            target.write_bytes(data)
            os.utime(target, ns=(previous.st_atime_ns, previous.st_mtime_ns))
            open_valid(source)
            assert_entries(cache)
            assert snapshot(siblings) == before, "Repair rewrote an unchanged sibling"
        target.unlink()
        before = snapshot(path for path in paths if path not in (target, marker))
        open_valid(source)
        assert_entries(cache)
        assert snapshot(before) == before, "Missing-file repair rewrote an unchanged sibling"

        for malformed in (b"forged complete marker\n", None):
            before = snapshot(path for path in paths if path != marker)
            if malformed is None:
                marker.unlink()
            else:
                marker.write_bytes(malformed)
            open_valid(source)
            assert marker.read_bytes() == marker_bytes
            assert snapshot(before) == before, "Marker repair rewrote verified payloads"
        shutil.rmtree(cache)
        cache.mkdir()
        (cache / MARKER).write_bytes(marker_bytes)
        open_valid(source)
        assert_entries(cache)

        # Repair through a cache hardlink must not overwrite its outside target.
        external = root / "outside-target.bin"
        external.write_bytes(b"EXTERNAL")
        target.unlink()
        os.link(external, target)
        external_before = snapshot([external])
        open_valid(source)
        assert target.read_bytes() == b"plumless" and external.read_bytes() == b"EXTERNAL"
        assert snapshot([external]) == external_before

        target.unlink()
        target.symlink_to(external)
        open_invalid(source, "cache path")
        assert external.read_bytes() == b"EXTERNAL" and target.is_symlink()
        target.unlink()
        open_valid(source)
        marker.unlink()
        marker.symlink_to(external)
        open_invalid(source, "cache path")
        assert external.read_bytes() == b"EXTERNAL"
        marker.unlink()
        open_valid(source)

        outside_directory = root / "outside-directory"
        outside_directory.mkdir()
        sentinel = outside_directory / "sentinel"
        sentinel.write_bytes(b"untouched")
        for original in (cache / "Assets", cache):
            saved = root / ("saved-" + original.name)
            original.rename(saved)
            original.symlink_to(outside_directory, target_is_directory=True)
            try:
                open_invalid(source, "cache path")
                assert sorted(path.name for path in outside_directory.iterdir()) == ["sentinel"]
                assert sentinel.read_bytes() == b"untouched"
            finally:
                original.unlink()
                saved.rename(original)
        alias = root / "cache-base-link"
        alias.symlink_to(cache_base, target_is_directory=True)
        alias_environment = {**environment, "ZSHARP_PACKAGE_CACHE": str(alias)}
        freeze(paths)
        before = snapshot(paths)
        open_valid(source, alias_environment)
        assert snapshot(paths) == before, "Trusted cache-base symlink broke warm reuse"

        # Package identity uses full bytes, even if path, size, timestamp and CRC
        # remain unchanged. These two strings are an actual CRC-32 collision.
        changed = bytearray(source_bytes)
        changed[collision_offset:collision_offset + 8] = b"buckeroo"
        previous = source.stat()
        source.write_bytes(changed)
        os.utime(source, ns=(previous.st_atime_ns, previous.st_mtime_ns))
        assert source.stat().st_size == len(source_bytes)
        with zipfile.ZipFile(source) as archive:
            assert archive.testzip() is None
        changed_cache = cache_for(source)
        assert changed_cache != cache
        open_valid(source)
        assert_entries(changed_cache, {**entries, COLLISION: b"buckeroo"})
        assert snapshot(paths) == before, "Changed package modified the previous digest's cache"
        source.write_bytes(source_bytes)
        os.utime(source, ns=(previous.st_atime_ns, previous.st_mtime_ns))
        open_valid(source)
        assert snapshot(paths) == before, "Returning to original package rewrote its valid cache"

        corrupt = project / "Packages/Corrupt-unbytecoded.zapp"
        damaged = bytearray(source_bytes)
        damaged[collision_offset] ^= 0x20
        corrupt.write_bytes(damaged)
        open_invalid(corrupt, "CRC")
        corrupt_cache = cache_for(corrupt)
        assert not list(corrupt_cache.rglob("*.zsharp-tmp-*")), "Failed CRC extraction leaked temporary files"
        if corrupt_cache.exists():
            shutil.rmtree(corrupt_cache)
        shutil.copytree(cache, corrupt_cache)
        open_invalid(corrupt, "CRC")
        assert not list(corrupt_cache.rglob("*.zsharp-tmp-*"))
        assert_entries(corrupt_cache)
        for index, name in enumerate((MARKER, MARKER.upper(), MARKER + "/child")):
            reserved = project / f"Packages/Reserved-{index}-unbytecoded.zapp"
            with zipfile.ZipFile(reserved, "w", compression=zipfile.ZIP_STORED) as archive:
                archive.comment = archive_comment
                for relative, data in entries.items():
                    archive.writestr(relative, data)
                archive.writestr(name, marker_bytes)
            open_invalid(reserved, "reserved")

        # The untouched compiled-package path still validates every entry SHA.
        open_valid(compiled)
        compiled_bad = project / "Packages/Corrupt-compiled.zapp"
        damaged = bytearray(compiled.read_bytes())
        offset = compiled_payload_offset(damaged, COLLISION)
        damaged[offset] ^= 0x20
        compiled_bad.write_bytes(damaged)
        open_invalid(compiled_bad, "SHA-256")
    print(f"Package cache/integrity regressions passed: {checks} public CLI/ZIP checks")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
