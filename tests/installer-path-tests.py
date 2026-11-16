"""Exercise generated PATH profiles through real fresh shells, in private homes.

The driver takes an explicit test directory; HOME is inherited and never changed.
Every profile sourced below is selected explicitly from the private fixture.
"""
from __future__ import annotations

import argparse
import os
from pathlib import Path
import shutil
import stat
import subprocess
import tempfile


BEGIN = "# >>> Z# installer PATH >>>"
END = "# <<< Z# installer PATH <<<"
BASH = shutil.which("bash")
SH = shutil.which("sh")
BASE_PATH = "/usr/bin:/bin"
PROBE = '''
printf 'FOUND=%s\\n' "$(command -v zsharp || true)"
zsharp
printf 'PATH_RESULT=%s\\n' "$PATH"
printf 'PROFILE_SENTINEL=%s\\n' "${ZSHARP_PATH_TEST_SENTINEL-}"
'''


def environment(test_root):
    result = os.environ.copy()
    for key in ("ZDOTDIR", "BASH_ENV", "ENV"):
        result.pop(key, None)
    result.update({"PATH": BASE_PATH, "SHELL": BASH or "/bin/bash",
                   "XDG_CONFIG_HOME": str(test_root / "config"),
                   "HISTFILE": os.devnull})
    return result


def install_stub(directory, label="owned-zsharp"):
    directory.mkdir(parents=True, exist_ok=True)
    executable = directory / "zsharp"
    executable.write_text("#!/bin/sh\nprintf '%s\\n' '" + label + "'\n", encoding="utf-8")
    executable.chmod(0o755)
    return executable


def configure(driver, test_home, directory, env, success=True):
    result = subprocess.run([str(driver), str(test_home), str(directory)],
                            env=env, text=True, capture_output=True, timeout=10)
    assert (result.returncode == 0) == success, result.stdout + result.stderr
    if not success:
        assert "PATH setup failed:" in result.stderr and len(result.stderr.strip()) > 24, result.stderr
    return result


def shell_probe(test_home, directory, env, *, shell="bash", profile=None, initial_path=BASE_PATH):
    child_environment = env.copy()
    child_environment["PATH"] = initial_path
    if shell == "bash" and profile is None:
        command = [BASH, "--noprofile", "--rcfile", str(test_home / ".bashrc"), "-i", "-c", PROBE]
    else:
        binary = BASH if shell == "bash" else SH
        options = ["--noprofile", "--norc", "-i"] if shell == "bash" else []
        command = [binary, *options, "-c", '. "$1"\n' + PROBE,
                   "zsharp-path-test", str(profile or test_home / ".profile")]
    result = subprocess.run(command, env=child_environment, text=True,
                            capture_output=True, timeout=10, input="")
    assert result.returncode == 0, result.stdout + result.stderr
    fields = dict(line.split("=", 1) for line in result.stdout.splitlines()
                  if line.startswith(("FOUND=", "PATH_RESULT=", "PROFILE_SENTINEL=")))
    assert fields["FOUND"] == str(directory / "zsharp"), (fields, result.stderr)
    assert "owned-zsharp" in result.stdout, result.stdout
    members = fields["PATH_RESULT"].split(":")
    assert members.count(str(directory)) == 1, members
    return fields


def managed_once(test_home, names=(".bashrc", ".profile")):
    for name in names:
        contents = (test_home / name).read_text(encoding="utf-8")
        assert contents.count(BEGIN) == 1 and contents.count(END) == 1, (name, contents)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("driver", type=Path)
    parser.add_argument("--work-root", type=Path)
    options = parser.parse_args()
    if os.name == "nt":
        print("Installer PATH profiles: skipped (POSIX shells)")
        return
    if not BASH or not SH:
        raise SystemExit("Real bash and sh are required for installer PATH tests")
    driver = options.driver.resolve()
    if options.work_root:
        options.work_root.mkdir(parents=True, exist_ok=True)
    original_home = os.environ.get("HOME")
    checks = 0
    with tempfile.TemporaryDirectory(prefix="zsharp-installer-path-", dir=options.work_root) as temporary:
        root = Path(temporary)
        # Exercise literal paths with each shell metacharacter independently.
        for index, name in enumerate(("bin", "bin with spaces", "bin'quoted", "bin$dollar",
                                      "bin`literal`", "bin\\backslash", "bin[1]*?")):
            test_root = root / f"literal-{index}"
            test_home = test_root / "private home"
            test_home.mkdir(parents=True)
            directory = test_root / name
            install_stub(directory)
            env = environment(test_root)
            assert env.get("HOME") == original_home
            configure(driver, test_home, directory, env)
            managed_once(test_home)
            assert not (test_home / ".zshrc").exists() and not (test_home / ".zprofile").exists()
            assert not (Path(env["XDG_CONFIG_HOME"]) / "fish").exists()
            shell_probe(test_home, directory, env)
            shell_probe(test_home, directory, env, shell="sh")
            # A substring entry must not suppress adding the real directory.
            shell_probe(test_home, directory, env, initial_path=str(directory) + "-extra:" + BASE_PATH)
            # An existing exact member must not be duplicated.
            shell_probe(test_home, directory, env, shell="sh", initial_path=str(directory) + ":" + BASE_PATH)
            checks += 6

        test_root = root / "existing"
        test_home = test_root / "private home"
        test_home.mkdir(parents=True)
        original = {".bashrc": b"# user bash preferences\nexport ZSHARP_PATH_TEST_SENTINEL='kept bash'",
                    ".profile": b"# user login settings\nexport ZSHARP_PATH_TEST_SENTINEL='kept sh'\n",
                    ".zshrc": b"# existing zsh options\n", ".zprofile": b"# existing zsh login\n"}
        for name, contents in original.items():
            (test_home / name).write_bytes(contents)
            (test_home / name).chmod(0o640)
        first, second = test_root / "old bin", test_root / "new 'bin$`literal`"
        install_stub(first); install_stub(second)
        env = environment(test_root)
        configure(driver, test_home, first, env)
        for name, contents in original.items():
            assert (test_home / name).read_bytes().startswith(contents), name
            assert stat.S_IMODE((test_home / name).stat().st_mode) == 0o640
        assert shell_probe(test_home, first, env)["PROFILE_SENTINEL"] == "kept bash"
        assert shell_probe(test_home, first, env, shell="sh")["PROFILE_SENTINEL"] == "kept sh"
        snapshots = {name: (test_home / name).read_bytes() for name in original}
        configure(driver, test_home, first, env)
        assert snapshots == {name: (test_home / name).read_bytes() for name in original}
        configure(driver, test_home, second, env)
        managed_once(test_home, tuple(original))
        shell_probe(test_home, second, env)
        shell_probe(test_home, second, env, shell="sh")
        for name, contents in original.items():
            assert (test_home / name).read_bytes().startswith(contents)
            assert str(first) not in (test_home / name).read_text(), name
        checks += 7

        # Updating a previous install preserves both sides of its owned block.
        test_root = root / "middle-block"
        test_home = test_root / "private home"
        test_home.mkdir(parents=True)
        directory = test_root / "replacement bin"
        install_stub(directory)
        before = b"# first user section\n"
        after = b"# final user section\nexport ZSHARP_PATH_TEST_SENTINEL='after block'\n"
        (test_home / ".bashrc").write_bytes(before + (BEGIN + "\n# previous installer block\n" + END + "\n").encode() + after)
        env = environment(test_root)
        configure(driver, test_home, directory, env)
        contents = (test_home / ".bashrc").read_bytes()
        assert contents.startswith(before) and contents.endswith(after)
        assert b"previous installer block" not in contents
        managed_once(test_home)
        assert shell_probe(test_home, directory, env)["PROFILE_SENTINEL"] == "after block"
        checks += 3

        # Bash only reads the first available personal login profile.
        for index, present in enumerate(((".bash_profile",), (".bash_login",),
                                         (".bash_profile", ".bash_login"))):
            test_root = root / f"login-{index}"
            test_home = test_root / "private home"
            test_home.mkdir(parents=True)
            directory = test_root / "bin"
            install_stub(directory)
            env = environment(test_root)
            for name in present:
                (test_home / name).write_text(f"# preserved {name}\nexport ZSHARP_PATH_TEST_SENTINEL='login preserved'\n")
            ignored = (test_home / ".bash_login").read_bytes() if len(present) == 2 else None
            configure(driver, test_home, directory, env)
            selected = next(test_home / name for name in (".bash_profile", ".bash_login", ".profile")
                            if (test_home / name).exists())
            assert shell_probe(test_home, directory, env, profile=selected)["PROFILE_SENTINEL"] == "login preserved"
            if ignored is not None:
                assert (test_home / ".bash_login").read_bytes() == ignored
            checks += 2

        test_root = root / "symlink"
        test_home = test_root / "private home"
        test_home.mkdir(parents=True)
        target = test_root / "real bashrc"
        target.write_text("# symlink-owned user preferences\n")
        target.chmod(0o640)
        (test_home / ".bashrc").symlink_to(target)
        directory = test_root / "bin"
        install_stub(directory)
        env = environment(test_root)
        configure(driver, test_home, directory, env)
        assert (test_home / ".bashrc").is_symlink()
        assert target.read_text().startswith("# symlink-owned user preferences\n")
        assert stat.S_IMODE(target.stat().st_mode) == 0o640
        shell_probe(test_home, directory, env)
        checks += 2

        # Refuse ambiguous paths and incomplete managed sections with diagnostics.
        for index, name in enumerate(("bin\nline", "bin\rline", "bin:colon")):
            test_root = root / f"invalid-{index}"
            test_home = test_root / "private home"
            test_home.mkdir(parents=True)
            before = b"# user content stays untouched\n"
            (test_home / ".bashrc").write_bytes(before)
            configure(driver, test_home, test_root / name, environment(test_root), False)
            assert (test_home / ".bashrc").read_bytes() == before
            checks += 2
        test_root = root / "malformed"
        test_home = test_root / "private home"
        test_home.mkdir(parents=True)
        before = (BEGIN + "\n# incomplete existing block\n").encode()
        (test_home / ".bashrc").write_bytes(before)
        configure(driver, test_home, test_root / "bin", environment(test_root), False)
        assert (test_home / ".bashrc").read_bytes() == before
        checks += 2

        # A profile may be an unsupported special file. Refuse it promptly and
        # continue other independent profiles, without replacing the user's file.
        for kind in ("directory", "fifo", "dangling-symlink", "oversize"):
            test_root = root / ("unsafe-profile-" + kind)
            test_home = test_root / "private home"
            test_home.mkdir(parents=True)
            profile = test_home / ".bashrc"
            if kind == "directory":
                profile.mkdir()
            elif kind == "fifo":
                os.mkfifo(profile)
            elif kind == "dangling-symlink":
                profile.symlink_to(test_root / "absent target")
            else:
                profile.write_bytes(b"#" + b"x" * (1024 * 1024))
            before_stat = profile.lstat()
            configure(driver, test_home, test_root / "bin", environment(test_root), False)
            after_stat = profile.lstat()
            assert (before_stat.st_ino, before_stat.st_mode, before_stat.st_size) == (after_stat.st_ino, after_stat.st_mode, after_stat.st_size)
            managed_once(test_home, (".profile",))
            checks += 2

        # Only selected shell profiles are created. An existing Zsh profile or
        # current Zsh shell opts in; ZDOTDIR redirects both files literally.
        for index, selection in enumerate(("existing", "shell", "zdotdir")):
            test_root = root / f"zsh-selection-{index}"
            test_home = test_root / "private home"
            test_home.mkdir(parents=True)
            directory = test_root / "bin"
            install_stub(directory)
            env = environment(test_root)
            selected_home = test_home
            if selection == "existing":
                (test_home / ".zprofile").write_text("# preserve Zsh login settings\n")
            else:
                env["SHELL"] = "/not-installed/zsh"
            if selection == "zdotdir":
                selected_home = test_root / "custom Zsh configuration"
                selected_home.mkdir()
                env["ZDOTDIR"] = str(selected_home)
            configure(driver, test_home, directory, env)
            managed_once(selected_home, (".zshrc", ".zprofile"))
            if selected_home != test_home:
                assert not (test_home / ".zshrc").exists() and not (test_home / ".zprofile").exists()
            # The generated POSIX content must work even without Zsh installed.
            shell_probe(test_home, directory, env, shell="sh", profile=selected_home / ".zshrc")
            checks += 3
        test_root = root / "relative-zdotdir"
        test_home = test_root / "private home"
        test_home.mkdir(parents=True)
        env = environment(test_root)
        env.update({"SHELL": "/not-installed/zsh", "ZDOTDIR": "relative-config"})
        result = configure(driver, test_home, test_root / "bin", env, False)
        assert "ZDOTDIR" in result.stderr and "absolute" in result.stderr
        assert not (test_home / ".zshrc").exists()
        checks += 2

        # Zsh falls back to HOME only when ZDOTDIR is unset, not when it is
        # explicitly empty. Avoid writing to a different startup directory.
        test_root = root / "empty-zdotdir"
        test_home = test_root / "private home"
        test_home.mkdir(parents=True)
        env = environment(test_root)
        env.update({"SHELL": "/not-installed/zsh", "ZDOTDIR": ""})
        result = configure(driver, test_home, test_root / "bin", env, False)
        assert "ZDOTDIR" in result.stderr
        assert not (test_home / ".zshrc").exists()
        checks += 2

        # A current Fish shell opts in even before its config directory exists;
        # relative XDG_CONFIG_HOME follows XDG's explicit-home fallback.
        test_root = root / "fish-selection"
        test_home = test_root / "private home"
        test_home.mkdir(parents=True)
        env = environment(test_root)
        env.update({"SHELL": "/not-installed/fish", "XDG_CONFIG_HOME": "relative-config"})
        configure(driver, test_home, test_root / "bin", env)
        fish_profile = test_home / ".config/fish/conf.d/zsharp-path.fish"
        assert fish_profile.is_file() and fish_profile.read_text().count(BEGIN) == 1
        checks += 1

        # Optional shell execution uses explicit fixture configs, never their HOME.
        test_root = root / "optional-shells"
        test_home = test_root / "private home"
        test_home.mkdir(parents=True)
        directory = test_root / "bin 'quoted$`literal`"
        install_stub(directory)
        env = environment(test_root)
        (test_home / ".zshrc").write_text("# optional Zsh fixture\n")
        fish_conf = Path(env["XDG_CONFIG_HOME"]) / "fish/conf.d"
        fish_conf.mkdir(parents=True)
        configure(driver, test_home, directory, env)
        assert (fish_conf / "zsharp-path.fish").is_file()
        checks += 1
        zsh = shutil.which("zsh")
        if zsh:
            result = subprocess.run([zsh, "-dfc", 'source "$1"; command -v zsharp; zsharp',
                                     "zsharp-path-test", str(test_home / ".zshrc")],
                                    env=env, text=True, capture_output=True, timeout=10)
            assert result.returncode == 0 and str(directory / "zsharp") in result.stdout, result.stdout + result.stderr
            checks += 1
        fish = shutil.which("fish")
        if fish:
            result = subprocess.run([fish, "--no-config", "-c",
                                    'source $argv[1]; command -v zsharp; zsharp',
                                    str(fish_conf / "zsharp-path.fish")],
                                    env=env, text=True, capture_output=True, timeout=10)
            assert result.returncode == 0 and str(directory / "zsharp") in result.stdout, result.stdout + result.stderr
            checks += 1
    assert os.environ.get("HOME") == original_home
    print(f"Installer PATH profiles: {checks} real-shell/filesystem checks passed"
          f" (zsh {'included' if zsh else 'unavailable'}, fish {'included' if fish else 'unavailable'})")


if __name__ == "__main__":
    main()
