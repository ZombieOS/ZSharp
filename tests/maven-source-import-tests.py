"""Focused native validation tests for Maven package-relative imports."""

from __future__ import annotations

import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import zipfile


binary = Path(sys.argv[1]).resolve()
checks = 0


def write(root: Path, relative: str, text: str) -> Path:
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(text, encoding="utf-8")
    return path


def settings(root: Path, pid: str = "maven_fixture", dependencies: str = "") -> None:
    write(root, "project.zsettings", f'''zsharp = type.settings
Project: "Maven import validation":
PID: "{pid}":
Version: [1.0.0.0]:
Authors: ["Validation"]:
Description: "Package-relative import validation":
ZSharp: [1.4.0.0]:
Dependencies (
{dependencies}
):
''')


def check(path: Path, environment: dict[str, str], expected: str | None = None) -> None:
    global checks
    result = subprocess.run([str(binary), "check", str(path)], env=environment,
                            cwd=path.parent, text=True, capture_output=True, timeout=30)
    output = result.stdout + result.stderr
    if expected is None:
        assert result.returncode == 0, output
    else:
        assert result.returncode != 0 and expected in output, output
    checks += 1


entry = '''zsharp = type.script
noticed room Main[] (
 import java:maven_fixture.Java():
 import kt:maven_fixture.Kotlin():
 import maven_fixture.Shared():
 noticed brain Start[] (
  Function.call(java:Java:hello []):
  Function.call(kt:Kotlin:hello []):
  Function.call(Shared:Shared:Hello):
 )
)
'''
shared = '''zsharp = type.script
noticed room Shared[] (
 noticed brain Hello[] ( Print("Selected package"): )
)
'''

with tempfile.TemporaryDirectory(prefix="maven-native-imports-") as temporary:
    base = Path(temporary).resolve()
    root = base / "project with spaces"
    settings(root)
    java = root / "src/main/java/com/example/project"
    kotlin = root / "src/main/kotlin/com/example/project"
    zsharp = root / "src/main/zsharp/com/example/project"
    java_source = write(java, "Java.java", "package com.example.project; public class Java {}\n")
    kotlin_source = write(kotlin, "Kotlin.kt", "package com.example.project\nclass Kotlin\n")
    main = write(zsharp, "Main.zsharp", entry)
    write(zsharp, "Shared.zsharp", shared)
    # A generated/resource copy and a different package must not be candidates.
    write(root, "target/classes/Shared.zsharp", "not Z# source\n")
    write(root, "src/main/zsharp/com/other/Shared.zsharp", "not Z# source\n")

    environment = os.environ.copy()
    for name in ("ZSHARP_MAVEN_PROJECT_ROOT", "ZSHARP_MAVEN_JAVA_ROOT",
                 "ZSHARP_MAVEN_KOTLIN_ROOT", "ZSHARP_MAVEN_ZSHARP_ROOT"):
        environment.pop(name, None)
    environment.update({
        "ZSHARP_MAVEN_PROJECT_ROOT": str(root),
        "ZSHARP_MAVEN_JAVA_ROOT": str(java),
        "ZSHARP_MAVEN_KOTLIN_ROOT": str(kotlin),
        "ZSHARP_MAVEN_ZSHARP_ROOT": str(zsharp),
    })
    check(main, environment)

    java_source.unlink()
    check(main, environment, "Maven java module 'Java' was not found")
    java_source.write_text("package com.example.project; public class Java {}\n", encoding="utf-8")
    kotlin_source.unlink()
    check(main, environment, "Maven kt module 'Kotlin' was not found")
    kotlin_source.write_text("package com.example.project\nclass Kotlin\n", encoding="utf-8")

    duplicate = write(zsharp, "nested/Shared.zsharp", shared)
    check(main, environment, "ambiguous")
    duplicate.unlink()

    outside = base / "outside"
    outside.mkdir()
    invalid = environment | {"ZSHARP_MAVEN_JAVA_ROOT": str(outside)}
    check(main, invalid, "must stay inside")
    invalid = environment | {"ZSHARP_MAVEN_KOTLIN_ROOT": "src/main/kotlin"}
    check(main, invalid, "must be an absolute package directory")
    invalid = environment | {"ZSHARP_MAVEN_ZSHARP_ROOT": str(outside)}
    check(main, invalid, "must stay inside")

    # Explicit subpackage paths retain their ordinary dot-separated meaning.
    write(java, "tools/Nested.java", "package com.example.project.tools; public class Nested {}\n")
    nested = write(zsharp, "NestedMain.zsharp", '''zsharp = type.script
noticed room Main[] (
 import java:maven_fixture.tools.Nested():
 noticed brain Start[] ( Function.call(java:tools.Nested:hello []): )
)
''')
    check(nested, environment)

    # Mappings belonging to the caller must not redirect a dependency's imports.
    dependency = root / "Dependencies/dep/1.0.0.0"
    settings(dependency, "dep")
    write(dependency, "Mod.java", "public class Mod {}\n")
    settings(root, dependencies="dep:1.0.0.0:")
    dependent = write(zsharp, "Dependent.zsharp", '''zsharp = type.script
noticed room Main[] (
 import java:dep.Mod():
 noticed brain Start[] ( Function.call(java:dep.Mod:hello []): )
)
''')
    check(dependent, environment)

    # An ordinary project keeps the original root-relative Java/Kotlin mapping.
    legacy = base / "legacy"
    settings(legacy)
    legacy_main = write(legacy, "Main.zsharp", entry)
    write(legacy, "Shared.zsharp", shared)
    write(legacy, "Java.java", "public class Java {}\n")
    write(legacy, "Kotlin.kt", "class Kotlin\n")
    clean = {key: value for key, value in environment.items()
             if not key.startswith("ZSHARP_MAVEN_")}
    check(legacy_main, clean)
    # Even invalid package-root values are ignored for a different project.
    check(legacy_main, environment | {"ZSHARP_MAVEN_JAVA_ROOT": str(outside)})

    # A compiled companion must win over source, and still retain Java's normal
    # export/worker behavior. No source compilation or ambient classpath is used.
    javac = shutil.which("javac")
    sdk = binary.parent / "interop/java/zsharp-java-worker.jar"
    if javac and sdk.is_file():
        settings(root)
        java_source.write_text('''package com.example.project;
import ZSharp.ZSharp_Export;
@ZSharp_Export
public class Java {
    public static String hello() { return "compiled companion selected"; }
}
''', encoding="utf-8")
        classes = base / "compiled classes"
        classes.mkdir()
        compiled = subprocess.run([javac, "--release", "17", "-cp", str(sdk),
                                   "-d", str(classes), str(java_source)],
                                  capture_output=True, text=True, timeout=30)
        assert compiled.returncode == 0, compiled.stdout + compiled.stderr
        with zipfile.ZipFile(java / "Java.zjava.jar", "w") as archive:
            archive.write(classes / "com/example/project/Java.class",
                          "com/example/project/Java.class")
        java_source.write_text("this source must not be parsed\n", encoding="utf-8")
        companion = write(zsharp, "Companion.zsharp", '''zsharp = type.script
noticed room Main[] (
 import java:maven_fixture.Java():
 noticed brain Start[] ( Print(Function.call(java:Java:hello [])): )
)
''')
        jvm_environment = environment | {
            "ZSHARP_JAVA": str(Path(javac).with_name("java.exe" if os.name == "nt" else "java"))
        }
        result = subprocess.run([str(binary), "run", str(companion)],
                                env=jvm_environment, cwd=companion.parent,
                                capture_output=True, text=True, timeout=30)
        assert result.returncode == 0, result.stdout + result.stderr
        assert result.stdout.strip() == "compiled companion selected", result.stdout
        checks += 1

    # Physical containment also rejects package directories linked out of src/main.
    link = root / "src/main/java/linked"
    try:
        link.symlink_to(outside, target_is_directory=True)
    except (OSError, NotImplementedError):
        pass  # Creating symlinks is not enabled on every Windows test host.
    else:
        check(main, environment | {"ZSHARP_MAVEN_JAVA_ROOT": str(link)},
              "must stay inside")

print(f"PASS: {checks} Maven source-root, short import and legacy compatibility checks")
