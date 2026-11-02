"""Exercise scoped Java/Kotlin source imports through the real Java worker.

Requires a JDK 17+ and a local Kotlin compiler lib directory. --kotlin-lib can
point at a Kotlin installation or Gradle distribution's lib directory. No
project source is compiled by the worker; this test builds its own fixtures.
"""

from __future__ import annotations

import argparse
from contextlib import contextmanager
import json
import os
from pathlib import Path
import queue
import shutil
import subprocess
import tempfile
import threading
import zipfile


ROOT = Path(__file__).resolve().parents[2]


def require(condition, message):
    if not condition:
        raise AssertionError(message)


def command(arguments):
    result = subprocess.run(arguments, capture_output=True, text=True, timeout=120)
    require(result.returncode == 0, result.stdout + result.stderr)


def tool(name):
    home = os.environ.get("JAVA_HOME")
    candidate = Path(home, "bin", name + (".exe" if os.name == "nt" else "")) if home else None
    return str(candidate) if candidate and candidate.is_file() else shutil.which(name) or name


def number(value):
    return {"$number": str(value)}


class Client:
    def __init__(self, sdk, environment):
        self.process = subprocess.Popen(
            [tool("java"), "-cp", str(sdk), "ZSharp.Worker"], env=environment,
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
            text=True, encoding="utf-8",
        )
        self.lines = queue.Queue()
        self.stderr = []
        self.serial = 0
        self.callbacks = 0
        threading.Thread(target=self._read, daemon=True).start()
        threading.Thread(target=lambda: self.stderr.append(self.process.stderr.read()), daemon=True).start()

    def _read(self):
        try:
            for line in self.process.stdout:
                self.lines.put(json.loads(line))
        except BaseException as error:
            self.lines.put(error)
        finally:
            self.lines.put(None)

    def request(self, module, operation="call", **fields):
        self.serial += 1
        identifier = self.serial
        self.process.stdin.write(json.dumps({"id": identifier, "op": operation,
                                             "module": str(module), **fields}) + "\n")
        self.process.stdin.flush()
        while True:
            message = self.lines.get(timeout=15)
            require(message is not None and not isinstance(message, BaseException),
                    "Worker ended unexpectedly: " + "".join(self.stderr))
            if message.get("op") == "host.call":
                require(message["target"] == "Main:Main:Double", message)
                self.callbacks += 1
                response = {"id": message["id"], "op": "result", "ok": True,
                            "value": number(int(message["args"][0]["$number"]) * 2)}
                self.process.stdin.write(json.dumps(response) + "\n")
                self.process.stdin.flush()
                continue
            require(message.get("id") == identifier and message.get("op") == "result", message)
            return message

    def ok(self, module, operation="call", **fields):
        result = self.request(module, operation, **fields)
        require(result.get("ok") is True, result)
        return result.get("value")

    def failure(self, module, contains, operation="call", **fields):
        result = self.request(module, operation, **fields)
        require(result.get("ok") is False and contains in result.get("error", ""), result)

    def close(self):
        self.process.stdin.close()
        try:
            require(self.process.wait(timeout=10) == 0, "Worker exited unsuccessfully")
        finally:
            if self.process.poll() is None:
                self.process.kill()


@contextmanager
def worker(sdk, environment):
    client = Client(sdk, environment)
    try:
        yield client
    finally:
        client.close()


def write(root, relative, content):
    path = root / relative
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    return path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=ROOT / "build/interop-source-imports")
    parser.add_argument("--kotlin-lib", type=Path)
    options = parser.parse_args()
    kotlin_lib = options.kotlin_lib
    if kotlin_lib is None:
        candidates = list((Path.home() / ".gradle/wrapper/dists").glob(
            "gradle-8.8-*/**/lib/kotlin-compiler-embeddable-*.jar"))
        require(candidates, "Pass --kotlin-lib with a local Kotlin compiler lib directory")
        kotlin_lib = candidates[0].parent
    stdlib = next((path for path in kotlin_lib.glob("kotlin-stdlib*.jar")
                   if path.name == "kotlin-stdlib.jar" or path.name[len("kotlin-stdlib-"):][0].isdigit()), None)
    require(stdlib is not None, "Kotlin stdlib was not found")
    options.build.mkdir(parents=True, exist_ok=True)
    build = Path(tempfile.mkdtemp(prefix="run-", dir=options.build)).resolve()
    require(build.is_relative_to(options.build.resolve()), "Invalid fixture directory")
    sdk = build / "sdk"
    sdk.mkdir()
    sources = list((ROOT / "native/interop/java/src/ZSharp").glob("*.java"))
    command([tool("javac"), "--release", "17", "-encoding", "UTF-8", "-d", str(sdk), *map(str, sources)])
    project = build / "project"
    java_root = project / "src/main/java/com/example"
    kotlin_root = project / "src/main/kotlin/com/example"
    java_source = write(project, "src/main/java/com/example/Java.java", """
package com.example;
import ZSharp.ZSharp;
import ZSharp.ZSharp_Export;
@ZSharp_Export public class Java {
    public int value;
    private static int count;
    public Java(int initial) { value = initial; }
    public int increase(int amount) { return value += amount; }
    public static String greeting(String value) { return "Java " + value; }
    public static int counted() { return ++count; }
    public static Object callback(int value) { return ZSharp.call("Main:Main:Double", value); }
    @ZSharp_Export public static class Nested {
        public static String nested() { return "nested"; }
    }
}
""")
    other_source = write(project, "src/main/java/com/example/Other.java", """
package com.example;
import ZSharp.ZSharp_Export;
@ZSharp_Export public class Other {
    public static String secret() { return "other source only"; }
    public static int counted() { return Java.counted(); }
}
""")
    outside = write(project, "src/main/java/com/outside/Outside.java", "package com.outside; public class Outside {}")
    missing = write(project, "src/main/java/com/example/Missing.java", "package com.example; public class Missing {}")
    classes = project / "target/classes"
    classes.mkdir(parents=True)
    command([tool("javac"), "--release", "17", "-encoding", "UTF-8", "-cp", str(sdk),
             "-d", str(classes), str(java_source), str(other_source)])
    legacy = build / "Legacy.zjava.jar"
    with zipfile.ZipFile(legacy, "w") as archive:
        for path in classes.rglob("*.class"):
            archive.write(path, path.relative_to(classes).as_posix())
    kotlin_source = write(project, "src/main/kotlin/com/example/Kotlin.kt", """
package com.example
import ZSharp.ZSharp
import ZSharp.ZSharp_Export
@ZSharp_Export fun greeting(value: String): String = "Kotlin " + value
@ZSharp_Export fun callback(value: Int): Any? = ZSharp.call("Main:Main:Double", value)
""")
    named = write(project, "src/main/kotlin/com/example/Named.kt", """
@file:JvmName("RenamedExports")
package com.example
import ZSharp.ZSharp_Export
@ZSharp_Export fun customName(): String = "named facade"
""")
    ambiguous = write(project, "src/main/kotlin/com/example/Ambiguous.kt", """
package com.example
import ZSharp.ZSharp_Export
class First { companion object { @JvmStatic @ZSharp_Export fun same() = "first" } }
class Second { companion object { @JvmStatic @ZSharp_Export fun same() = "second" } }
""")
    command([tool("java"), "-cp", str(kotlin_lib / "*"), "org.jetbrains.kotlin.cli.jvm.K2JVMCompiler",
             "-jvm-target", "17", "-no-stdlib", "-no-reflect", "-classpath",
             os.pathsep.join(map(str, (sdk, classes, stdlib))), "-d", str(classes),
             str(kotlin_source), str(named), str(ambiguous)])
    environment = os.environ.copy()
    environment.update(ZSHARP_MAVEN_PROJECT_ROOT=str(project), ZSHARP_MAVEN_JAVA_ROOT=str(java_root),
                       ZSHARP_MAVEN_KOTLIN_ROOT=str(kotlin_root),
                       ZSHARP_JVM_CLASSPATH=os.pathsep.join(map(str, (classes, stdlib))))
    with worker(sdk, environment) as client:
        require(client.ok(java_source, target="greeting", args=["source"]) == "Java source", "Java source call")
        require(client.ok(java_source, target="Java.greeting", args=["qualified"]) == "Java qualified", "Qualified Java call")
        require(client.ok(java_source, target="com.example.Java.greeting", args=["package"]) == "Java package", "FQ class call")
        client.failure(java_source, "not part", target="Other.secret", args=[])
        client.failure(java_source, "No exported static", target="secret", args=[])
        require(client.ok(other_source, target="secret", args=[]) == "other source only", "Separate source visibility")
        require(client.ok(java_source, target="counted", args=[]) == number(1), "Static counter first call")
        require(client.ok(other_source, target="counted", args=[]) == number(2), "Shared project classloader")
        require(client.ok(java_source, target="Nested.nested", args=[]) == "nested", "Nested source class")
        instance = client.ok(java_source, "new", target="Java", args=[number(4)])
        require(client.ok(java_source, "get", ref=instance["$ref"], member="value") == number(4), "Source object field read")
        client.ok(java_source, "set", ref=instance["$ref"], member="value", args=[number(10)])
        require(client.ok(java_source, "invoke", ref=instance["$ref"], member="increase", args=[number(5)]) == number(15), "Source object method")
        require(client.ok(java_source, target="callback", args=[number(21)]) == number(42), "Java callback")
        require(client.ok(kotlin_source, target="greeting", args=["source"]) == "Kotlin source", "Kotlin top-level call")
        require(client.ok(kotlin_source, target="callback", args=[number(22)]) == number(44), "Kotlin callback")
        require(client.callbacks == 2, "Both callbacks reached the host")
        require(client.ok(named, target="customName", args=[]) == "named facade", "Kotlin @file:JvmName")
        client.failure(kotlin_source, "No exported static", target="customName", args=[])
        client.failure(ambiguous, "ambiguous within source", target="same", args=[])
        client.failure(outside, "outside its selected", target="anything", args=[])
        client.failure(missing, "No compiled JVM classes", target="anything", args=[])
    compiled_jar = build / "project-classes.jar"
    with zipfile.ZipFile(compiled_jar, "w") as archive:
        for path in classes.rglob("*.class"):
            archive.write(path, path.relative_to(classes).as_posix())
    jar_environment = dict(environment, ZSHARP_JVM_CLASSPATH=os.pathsep.join(map(str, (compiled_jar, stdlib))))
    with worker(sdk, jar_environment) as client:
        require(client.ok(java_source, target="greeting", args=["jar"]) == "Java jar", "Classpath JAR source binding")
        require(client.ok(named, target="customName", args=[]) == "named facade", "Kotlin classpath JAR binding")
    duplicate_environment = dict(environment, ZSHARP_JVM_CLASSPATH=os.pathsep.join(map(str, (classes, compiled_jar, stdlib))))
    with worker(sdk, duplicate_environment) as client:
        client.failure(java_source, "ambiguous across JVM classpath", target="greeting", args=["duplicate"])
    disabled = environment.copy()
    for name in ("ZSHARP_MAVEN_PROJECT_ROOT", "ZSHARP_MAVEN_JAVA_ROOT", "ZSHARP_MAVEN_KOTLIN_ROOT", "ZSHARP_JVM_CLASSPATH"):
        disabled.pop(name, None)
    with worker(sdk, disabled) as client:
        client.failure(java_source, "scoped Maven project roots", target="greeting", args=["disabled"])
        require(client.ok(legacy, target="Java.greeting", args=["legacy"]) == "Java legacy", "Legacy JAR import")
    print("Scoped Java/Kotlin source worker tests passed (real Kotlin compiler, objects, callbacks, JARs and ambiguity checks).")
    print("Fixture:", build)


if __name__ == "__main__":
    main()
