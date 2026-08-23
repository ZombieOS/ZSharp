# Z#

Z# is a systems programming language implemented in C. Its official source-file
extension is **`.zsharp`**.

Version 1.0.0.0 is the first release of the Z1 language and toolchain. It is
ready for experiments and local projects; the public dependency registry,
hardware APIs, and complete external-function ABI are planned follow-up work.

The evolving official syntax is recorded in [LANGUAGE.md](LANGUAGE.md). Syntax
is added there only after it is supplied or confirmed by the language designer.

The planned toolchain compiles Z# source to portable Z# bytecode and runs that
bytecode in a fast C virtual machine. Native compilation can be added later for
programs that need the last bit of performance.

## Current status

This repository contains the first working compiler, bytecode VM, and Java
integration library. The compiler accepts only language rules that have already
been confirmed; new syntax is added incrementally as it is designed.

The command-line interface is already reserved:

```text
zsharp compile hello.zsharp -o hello.zbc
zsharp run hello.zsharp
zsharp run-bytecode hello.zbc
```

The `.zbc` bytecode extension shown here is provisional; only `.zsharp` is
official at this point.

The first implementation supports `noticed`/`silent` rooms; `text`, `number`,
status, text/number/object arrays; `brain`, number-returning, and text-returning functions;
`feed`; local
numbers; assignment; `if`/`else`; comparisons; array indexing; `status`,
`alive`/`dead`, `and`, and `or`; unconditional `loop`; constructors, objects,
fields, and methods; `loop.end`/`continue`; number arithmetic and text
concatenation; `addition(...)`;
`Print`; value and value-less `feed`; early return from an unmatched `if`; and
`Function.call`. A `Start[]` brain runs automatically. `Start[DR]` does not.
Rooms can be project-public (`noticed`), file-only (bare), or private
(`silent`). `Function.call` searches the current project directory and all of
its subfolders for the named `.zsharp` file. Calls fail clearly when no matching
file exists, more than one match exists, or the target is not visible.

Qualified values use `Room.Var`, `File.Room.Var`, or
`Project.File.Room.Var`. Four-part project paths are handled by native provider
libraries, which lets Z# reach non-Z# code and SDKs without baking those SDKs
into the compiler. See [PROVIDERS.md](PROVIDERS.md).

Imports are room-scoped. A room must declare `import Project.File():` (or a
folder-qualified form such as `import Project.Folder.File():`) before using
another file. The compiler performs this check before producing bytecode.
Objects use the same qualification ladder for fields, writes, and calls, up to
`Project.File.Room.User.Move[...]`. Imported Z# modules stay loaded for the VM
run, so cross-file writes and object changes remain visible to later reads.

Every command reads `project.zsettings` from the current project root. This
file defines the display name, lowercase PID, project version, authors,
description, Z# generation, and dependency PIDs. Imports use the PID, so this
project imports one of its own files with `import zsharp.File():`. The current
compiler implements generation Z1.

`horde` is the static/shared modifier and follows visibility, for example
`noticed horde number Score = 10:`. A horde field is shared across every
instance of its room.

`Function.call` accepts arguments and can be used as an expression, for example
`number Result = Function.call(Math:Calculator:Add [4, 6]):`. The current VM
also supports operator precedence for `+`, `-`, `*`, `/`, `%`, comparisons,
`not`, `and`, and `or`, plus `loop.end:` for leaving a loop.

Numbers use exact arbitrary-size ordinary decimals without scientific notation.
Terminating division keeps every required digit; repeating division truncates
after its first fractional digit, so `1 / 3` produces `0.3`.

Text and number arrays support indexed reads, `.set[index]`, and `.Length`.
Object arrays support constructor values and
`Players.add(new Player["Name"]):`.

Brains can return a dynamically typed named outcome. Declare one with
`if(Result)[condition] (...)`, supply its value with `feed(value):`, and select
it with `Function.call(File:Room:Brain [arguments])[Result]`. A selected path
that reaches no `feed(value):` produces `null`.

Every compiled bytecode file contains two automatic hashes. The project
identity is stable for its registry-unique PID, while the build SHA-256 changes
with compiled content. The VM verifies both before running the file and rejects
changed bytecode. See [BYTECODE.md](BYTECODE.md).

## Native build

The native toolchain uses C17 and CMake:

```text
cmake -S . -B build
cmake --build build
```

## Java integration

The Java library requires Java 17 or newer. It provides the same API to Gradle
and Maven projects and communicates with the native toolchain as a child
process. This keeps the first integration portable and avoids platform-specific
JNI packages.

Build and publish it to your local dependency cache with either:

```text
cd java
gradle publishToMavenLocal
```

or:

```text
cd java
mvn install
```

Then import it from Gradle:

```kotlin
repositories {
    mavenLocal()
}

dependencies {
    implementation("com.zombieos:zsharp:1.0.0.0")
}
```

or Maven:

```xml
<dependency>
    <groupId>com.zombieos</groupId>
    <artifactId>zsharp</artifactId>
    <version>1.0.0.0</version>
</dependency>
```

Java programs can then compile or run Z# files:

```java
import com.zombieos.zsharp.ZSharp;

var zsharp = ZSharp.toolchain();
var result = zsharp.run(java.nio.file.Path.of("hello.zsharp"));
System.out.print(result.standardOutput());
System.err.print(result.standardError());
```

External projects can be registered from Java as well:

```java
var result = zsharp.runWithProviders(
    java.nio.file.Path.of("game.zsharp"),
    java.util.Map.of("playfab", java.nio.file.Path.of("playfab_provider.dll"))
);
```

The library searches for the toolchain using `ZSHARP_BIN`, then
`ZSHARP_HOME/bin`, and finally the system `PATH`.

Pushing the repository to GitHub does not by itself publish the Java
coordinate. The included GitHub Actions workflow publishes it to GitHub
Packages when a GitHub Release is published. Maven Central requires its own
namespace, license, signing, and publication setup. See
[PUBLISHING.md](PUBLISHING.md).

## Syntax decisions still open

The main remaining definitions include memory and pointer syntax, hardware I/O,
the dependency registry and publisher signing model, engine-specific result
types, and eventually the first Z1-to-Z2 migration mappings.
