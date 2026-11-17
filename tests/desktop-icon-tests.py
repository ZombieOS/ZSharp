"""Real Linux launcher icon/identity tests, including a Gio argument roundtrip."""
from __future__ import annotations

import argparse
import binascii
import ctypes
import ctypes.util
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import time
import zlib


def inspect_desktop(identity):
    """Resolve an installed desktop ID through GIO in a fresh process.

    GIO caches the XDG application directories, so this helper starts after the
    fixture environment is set rather than inspecting the user's application
    database from the parent test process.
    """
    gio = ctypes.CDLL(ctypes.util.find_library("gio-2.0") or "libgio-2.0.so.0")
    glib = ctypes.CDLL(ctypes.util.find_library("glib-2.0") or "libglib-2.0.so.0")
    gobject = ctypes.CDLL(ctypes.util.find_library("gobject-2.0") or "libgobject-2.0.so.0")
    gio.g_desktop_app_info_new.argtypes = [ctypes.c_char_p]
    gio.g_desktop_app_info_new.restype = ctypes.c_void_p
    for name in ("g_app_info_get_id", "g_app_info_get_name", "g_app_info_get_commandline",
                 "g_desktop_app_info_get_filename", "g_desktop_app_info_get_startup_wm_class"):
        function = getattr(gio, name)
        function.argtypes = [ctypes.c_void_p]
        function.restype = ctypes.c_char_p
    for name in ("g_app_info_should_show", "g_desktop_app_info_get_nodisplay"):
        function = getattr(gio, name)
        function.argtypes = [ctypes.c_void_p]
        function.restype = ctypes.c_int
    gio.g_app_info_get_icon.argtypes = [ctypes.c_void_p]
    gio.g_app_info_get_icon.restype = ctypes.c_void_p
    gio.g_file_icon_get_file.argtypes = [ctypes.c_void_p]
    gio.g_file_icon_get_file.restype = ctypes.c_void_p
    gio.g_file_get_path.argtypes = [ctypes.c_void_p]
    gio.g_file_get_path.restype = ctypes.c_void_p
    glib.g_free.argtypes = [ctypes.c_void_p]
    glib.g_shell_parse_argv.argtypes = [ctypes.c_char_p, ctypes.POINTER(ctypes.c_int),
        ctypes.POINTER(ctypes.POINTER(ctypes.c_char_p)), ctypes.c_void_p]
    glib.g_shell_parse_argv.restype = ctypes.c_int
    glib.g_strfreev.argtypes = [ctypes.POINTER(ctypes.c_char_p)]
    gobject.g_object_unref.argtypes = [ctypes.c_void_p]
    info = gio.g_desktop_app_info_new(identity.encode())
    assert info, f"GIO could not resolve installed desktop ID {identity}"
    try:
        icon = gio.g_app_info_get_icon(info)
        assert icon, f"GIO found no launcher icon for {identity}"
        file = gio.g_file_icon_get_file(icon)
        assert file, f"GIO did not resolve the absolute PNG as a file icon for {identity}"
        pointer = gio.g_file_get_path(file)
        assert pointer, f"GIO icon has no local path for {identity}"
        try:
            icon_path = ctypes.string_at(pointer).decode()
        finally:
            glib.g_free(pointer)
        def text(name):
            value = getattr(gio, name)(info)
            return None if value is None else value.decode()
        # get_executable() can truncate a quoted path at its first space.
        # Parse GIO's full command line with GLib; real launch roundtrips below
        # separately verify the executed arguments, including field-code escapes.
        commandline = text("g_app_info_get_commandline")
        count = ctypes.c_int()
        arguments = ctypes.POINTER(ctypes.c_char_p)()
        assert glib.g_shell_parse_argv(commandline.encode(), ctypes.byref(count),
                                      ctypes.byref(arguments), None), commandline
        try:
            assert count.value > 0, commandline
            executable = arguments[0].decode()
        finally:
            glib.g_strfreev(arguments)
        return {"id": text("g_app_info_get_id"), "name": text("g_app_info_get_name"),
                "executable": executable,
                "filename": text("g_desktop_app_info_get_filename"),
                "startupWMClass": text("g_desktop_app_info_get_startup_wm_class"),
                "icon": icon_path, "shouldShow": bool(gio.g_app_info_should_show(info)),
                "noDisplay": bool(gio.g_desktop_app_info_get_nodisplay(info))}
    finally:
        gobject.g_object_unref(info)


def png(red):
    def chunk(kind, data):
        value = kind + data
        return struct.pack(">I", len(data)) + value + struct.pack(">I", binascii.crc32(value))
    return (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", 1, 1, 8, 6, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(bytes([0, red, 70, 140, 255])))
            + chunk(b"IEND", b""))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("driver", type=Path)
    options = parser.parse_args()
    driver = options.driver.resolve(strict=True)
    glib_name = ctypes.util.find_library("glib-2.0")
    if glib_name is None:
        print("Linux desktop icon Gio regressions skipped: GLib is not installed.")
        return
    glib = ctypes.CDLL(glib_name)
    glib.g_key_file_new.restype = ctypes.c_void_p
    glib.g_key_file_load_from_file.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_int, ctypes.c_void_p]
    glib.g_key_file_load_from_file.restype = ctypes.c_int
    glib.g_key_file_get_string.argtypes = [ctypes.c_void_p, ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p]
    glib.g_key_file_get_string.restype = ctypes.c_void_p
    glib.g_key_file_free.argtypes = [ctypes.c_void_p]
    glib.g_free.argtypes = [ctypes.c_void_p]

    def value(path, name):
        keyfile = glib.g_key_file_new()
        try:
            if not glib.g_key_file_load_from_file(keyfile, os.fsencode(path), 0, None):
                raise AssertionError(f"GLib rejected desktop entry {path}")
            pointer = glib.g_key_file_get_string(keyfile, b"Desktop Entry", name.encode(), None)
            if not pointer:
                raise AssertionError(f"Missing {name} in {path}")
            try:
                return ctypes.string_at(pointer).decode()
            finally:
                glib.g_free(pointer)
        finally:
            glib.g_key_file_free(keyfile)

    with tempfile.TemporaryDirectory(prefix="zsharp-desktop-icons-") as temporary:
        root = Path(temporary)
        desktop, data = root / "Desktop", root / "data % #\nNotAField=true"
        desktop.mkdir()
        data.mkdir()
        project = root / 'project % # \\ "$\nNotAField=true'
        project.mkdir()
        custom, legacy = project / "icon.png", project / "legacy.png"
        custom.write_bytes(png(50)); legacy.write_bytes(png(220))
        settings = '''zsharp = type.settings
Project: "Icon fixture":
PID: "desktop_icon_fixture":
Version: [1.4.1.0]:
Authors: ["Tests"]:
Description: "Owned temporary desktop launcher fixture":
ZSharp: [1.4.1.0]:
{icon}
Dependencies (
):
'''
        (project / "project.zsettings").write_text(settings.format(icon='Icon: "icon.png":'))
        entry = project / 'source %f # \\ "$\nOtherField=false.zsharp'
        entry.write_text("owned launcher argument fixture")
        package = project / "Owned.zapp"
        package.write_bytes(b"owned path fixture; the test driver never runs a package")
        # The desktop-entry specification forbids '=' in the executable path.
        copied_driver = root / 'driver # with spaces'
        shutil.copy2(driver, copied_driver)
        copied_driver.chmod(0o755)
        capture = root / "arguments.bin"
        environment = os.environ.copy()
        environment.update({
            "XDG_DATA_HOME": str(data), "XDG_CONFIG_HOME": str(root / "config"),
            "XDG_CACHE_HOME": str(root / "cache"), "ZSHARP_DESKTOP_DIRECTORY": str(desktop),
            "ZSHARP_ICON_ARGUMENT_CAPTURE": str(capture), "PATH": "/intentionally-no-network-tools",
            "GSETTINGS_BACKEND": "memory", "GIO_USE_VFS": "local",
            "DBUS_SESSION_BUS_ADDRESS": "unix:path=" + str(root / "no-session-bus"),
        })
        environment.pop("ZSHARP_HUB_DEFAULT_ICON", None)

        def run(arguments):
            result = subprocess.run([str(copied_driver), *map(str, arguments)],
                                    env=environment, text=True, capture_output=True, timeout=20)
            if result.returncode != 0:
                raise AssertionError(result.stdout + result.stderr)
            return result.stdout.strip()

        project_id = run(["id", "desktop_icon_fixture"])
        hub_id = run(["id", "zsharp-hub"])
        applications = data / "applications"
        gio = Path("/usr/bin/gio")
        roundtrips = 0
        lookups = 0

        def lookup(identity, launcher, icon, name, *, should_show=True):
            nonlocal lookups
            result = subprocess.run([sys.executable, str(Path(__file__).resolve()),
                                     "--inspect-desktop", identity + ".desktop"],
                                    env=environment, text=True, capture_output=True, timeout=20)
            assert result.returncode == 0, result.stdout + result.stderr
            info = json.loads(result.stdout)
            assert info["id"] == identity + ".desktop", info
            assert info["filename"] == str(launcher), info
            assert info["name"] == name, info
            assert info["executable"] == str(copied_driver), info
            assert info["startupWMClass"] == (hub_id if identity == hub_id else project_id), info
            assert info["icon"] == str(icon), info
            assert info["shouldShow"] is should_show, (
                "GNOME's manual pinning path requires a visible application entry", info)
            assert info["noDisplay"] is not should_show, info
            lookups += 1

        def roundtrip(path, expected):
            nonlocal roundtrips
            if not gio.is_file():
                return
            capture.unlink(missing_ok=True)
            result = subprocess.run([str(gio), "launch", str(path)], env=environment,
                                    text=True, capture_output=True, timeout=20)
            if result.returncode != 0:
                raise AssertionError(result.stdout + result.stderr + "\n" + path.read_text())
            for _ in range(200):
                if capture.exists():
                    break
                time.sleep(0.01)
            content = capture.read_bytes()
            count = struct.unpack_from("<I", content)[0]
            offset, arguments = 4, []
            for _ in range(count):
                size = struct.unpack_from("<I", content, offset)[0]
                offset += 4
                arguments.append(content[offset:offset + size].decode())
                offset += size
            if arguments != expected:
                raise AssertionError(f"Launcher argument mismatch: {arguments!r} != {expected!r}")
            roundtrips += 1

        for kind, action in enumerate(["run", "run-bytecode", "open-desktop", "hub"]):
            run(["register", "Demo\nNotAField=false", "desktop_icon_fixture",
                 kind, entry, custom])
            launcher = applications / f"{hub_id if kind == 3 else project_id}.desktop"
            assert value(launcher, "Icon") == str(custom)
            assert value(launcher, "Name") == "Demo\nNotAField=false"
            assert value(launcher, "StartupWMClass") == (hub_id if kind == 3 else project_id)
            assert value(launcher, "NoDisplay") == "false"
            assert not list(desktop.iterdir()), "Registration must not create a Desktop shortcut"
            raw = launcher.read_text()
            assert "\nNotAField=false\n" not in raw, "Newline injected a desktop field"
            lookup(hub_id if kind == 3 else project_id, launcher, custom,
                   "Demo\nNotAField=false")
            roundtrip(launcher, [action] if kind == 3 else [action, str(entry)])

        run(["hub-shortcut"])
        hub_shortcut = desktop / "Z# Hub.desktop"
        default_icon = Path(value(hub_shortcut, "Icon"))
        assert default_icon.is_absolute() and default_icon.is_file()
        assert default_icon.read_bytes().startswith(b"\x89PNG\r\n\x1a\n")
        assert value(hub_shortcut, "StartupWMClass") == hub_id
        assert value(applications / f"{hub_id}.desktop", "Icon") == str(default_icon)
        lookup(hub_id, applications / f"{hub_id}.desktop", default_icon, "Z# Hub")
        assert hub_shortcut.stat().st_mode & 0o111
        roundtrip(hub_shortcut, ["hub"])
        run(["shortcut", "Owned App", package, project, "legacy.png"])
        shortcut = desktop / "Owned App.desktop"
        assert value(shortcut, "Icon") == str(custom), "Settings Icon must beat legacy design icon"
        assert value(shortcut, "StartupWMClass") == project_id
        roundtrip(shortcut, ["open", str(package)])
        (project / "project.zsettings").write_text(settings.format(icon=""))
        run(["shortcut", "Owned App", package, project, "legacy.png"])
        assert value(shortcut, "Icon") == str(legacy), "Legacy icon remains a fallback"
        run(["shortcut", "Owned App", package, project, "missing.png"])
        assert value(shortcut, "Icon") == str(default_icon), "Missing icon uses offline official fallback"
        (project / "project.zsettings").write_text(settings.format(icon='Icon: "icon.png":'))
        custom.write_text("not a PNG")
        run(["shortcut", "Owned App", package, project, "legacy.png"])
        assert value(shortcut, "Icon") == str(default_icon), "Invalid settings PNG uses official fallback"
        # Confirm the eligibility probe detects the old pinning blocker, rather
        # than merely succeeding for any desktop file that can be parsed.
        hidden = applications / "hidden-control.desktop"
        hidden.write_text((applications / f"{project_id}.desktop").read_text().replace(
            "NoDisplay=false", "NoDisplay=true"))
        lookup("hidden-control", hidden, custom, "Demo\nNotAField=false", should_show=False)
    print(f"Linux desktop icon/identity/metadata regressions passed "
          f"({roundtrips} real Gio argument roundtrips, {lookups} GIO identity/visibility lookups).")


if __name__ == "__main__":
    if len(sys.argv) == 3 and sys.argv[1] == "--inspect-desktop":
        print(json.dumps(inspect_desktop(sys.argv[2])))
    else:
        main()
