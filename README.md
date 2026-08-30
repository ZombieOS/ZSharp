# Z#

Official website and downloads: <https://www.zsharp.zombieos.com>

Z# is a systems programming language implemented in C. Its official source-file
extension is **`.zsharp`**.

Version 1.0.1.1 adds multiline window inputs, live text/caret information, and
overflow-only scrollbars to the native Z# windows introduced in 1.0.1.0. The
release also includes project launching and cross-platform `.zapp`/`.zgame`
containers while retaining the Z1 language and toolchain. It is ready for
experiments and local projects; the public dependency registry, hardware APIs,
and game engine are follow-up work.

The evolving official syntax decisions are recorded in
[LANGUAGE.md](LANGUAGE.md). The user-facing language guide and comparisons are
in [SYNTAX.md](SYNTAX.md). Syntax is added only after it is supplied or
confirmed by the language designer. The implemented and planned safe removal
flow for `.zapp` and `.zgame` packages is recorded in
[UNINSTALL.md](UNINSTALL.md).
ZVM bootstrap downloads and the official update-site format are documented in
[INSTALLING.md](INSTALLING.md).

The toolchain compiles Z# source to portable Z# bytecode and runs that
bytecode in a fast C virtual machine. Native compilation can be added later for
programs that need the last bit of performance.

## Current status

This repository contains the first working compiler, bytecode VM, and Java
integration library. The compiler accepts only language rules that have already
been confirmed; new syntax is added incrementally as it is designed.

Window files now compile with `zsharp = type.script:window` and exactly one
top-level `Window`. The compiler validates official feature imports, design,
text, buttons, images, text/image input, `zu`/`px` measurements, colors, and
callback targets. Design backgrounds support multi-stop linear and radial
gradients. Imported callbacks can update live titles, backgrounds, colors,
content, icons, placeholders, size, and position with
`File.Element.property.set: value:`. Text variables can also hold reusable
property paths and use `PathAlias.set: value:`; `wait(...)` and `delay(...)` provide
millisecond/second timing while preserving window redraws. It also accepts the
`type.script:2D` and `type.script:3D`
headers for ordinary room-based code while their game-object syntax is still
being designed. The bundled native `zsharpwindow` backends render designs,
text, buttons, images, and text/image inputs; execute left/right callbacks;
automatically run every normal script's non-`DR` `Start[]`; support display
scaling; wrap text and scale controls when the window narrows; and provide
vertical scrolling when its height is reduced. Button clicks are the only
window-originated function calls. Windows uses
native Win32 controls and WIC, Linux loads GTK 3 when a window is launched, and
macOS uses AppKit. A Linux server can still use the compiler and VM without a
graphical display; only launching a window requires GTK 3 and a desktop session.

The command-line interface includes:

```text
zsharp compile hello.zsharp -o hello.zbc
zsharp project path/to/project.zsettings
zsharp run hello.zsharp
zsharp run-bytecode hello.zbc
zsharp package app path/to/project MyApp --unbytecode
zsharp run path/to/project/Packages/MyApp.zapp
zsharp uninstall path/to/project/Packages/MyApp.zapp
zsharp associate
zsharp hub
```

The `.zbc` bytecode extension shown here is provisional. `.zsharp`, `.zapp`,
and `.zgame` are official extensions.

End users can install the standalone ZVM with the small platform bootstrap in
`%USERPROFILE%\Downloads\ZSharp Publishing\1.0.1.1`. It downloads the current runtime from
`https://www.zsharp.zombieos.com/update.js?v=CURRENTVERSION-OS`, compares the
installed version with the static GitHub Pages manifest, verifies
`assets/download/ZVM-LATEST.zip` and the selected runtime, preserves the
previous runtime, and configures the command and package associations for the
current user. Standalone installations quietly repeat this version check on
every ZVM launch. See [INSTALLING.md](INSTALLING.md).

The first implementation supports `noticed`/`silent` rooms; `text`, `number`,
status, text/number/object arrays; `brain`, number-returning, and text-returning functions;
`feed`; local
numbers; assignment; `if`/`else`; comparisons; array indexing; `status`,
`alive`/`dead`, `and`, and `or`; unconditional `loop`; constructors, objects,
fields, and methods; `loop.end`/`continue`; number arithmetic and text
concatenation; `addition(...)`;
`Print`; value and value-less `feed`; early return from an unmatched `if`; and
`Function.call`. A `Start[]` brain runs automatically. `Start[DR]` does not.
Window applications create their startup window and then run the eligible
`Start[]` brains from normal project scripts as independent tasks. Internal
button callbacks also run as tasks, and closing a window cleanly cancels and
joins all of its active work.
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
another file. A final wildcard works for every project and namespace, including
`import Project.*():`, `import Project.Folder.*():`, `import ZSharp.*():`, and
`import ZSharp.Window.*():`. Dependencies and visibility are still enforced.
The compiler performs this check before producing bytecode.
Objects use the same qualification ladder for fields, writes, and calls, up to
`Project.File.Room.User.Move[...]`. Imported Z# modules stay loaded for the VM
run, so cross-file writes and object changes remain visible to later reads.

Commands find the nearest `project.zsettings` by starting at the supplied
source, bytecode, settings, or project path and walking upward. This
file defines the display name, lowercase PID, project version, authors,
description, Z# generation, and dependency PIDs. Imports use the PID, so this
project imports one of its own files with `import zsharp.File():`. The current
compiler implements generation Z1.

`zsharp project <path>` validates and registers a project for the current user;
it never launches the project. Re-registering the same PID or path updates its
single registry entry. On Windows the registry is stored below
`%LOCALAPPDATA%\ZombieOS\ZSharp`; Linux uses the XDG data directory (or
`~/.local/share/zsharp`), and macOS uses
`~/Library/Application Support/ZSharp`.

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

`.zapp` and `.zgame` are Z#'s cross-platform package containers.
`zsharp package app <project> <filename>` creates the normal bytecoded package
at `<project>/Packages/<filename>.zapp`; use `game` to create `.zgame` instead.
Add `--unbytecode` to also create
`<project>/Packages/<filename>-unbytecoded.zapp`. The companion is a standard
ZIP-compatible source archive: renaming it from `.zapp` to `.zip` exposes a
copy of the original project files. The command adds extensions automatically,
validates the settings and every included `.zsharp` file, and rejects unsafe
paths when opened. `zsharp run App.zapp` verifies and extracts an application
into a private content-addressed cache before its configured `Window Startup`
script runs. Normal packages launch their embedded startup bytecode; source
companions launch the validated `.zsharp` startup.
On first installation, Z# offers to create a Desktop shortcut using the app's
design icon. `zsharp associate` registers `.zapp` and `.zgame` for the current
user so opening either launches the package without opening a terminal. Running
`zsharp open`, `zsharp run`, or another command in a terminal keeps using that
terminal normally.

The `.zgame` container uses the same validated metadata and packaging
foundation, but 1.0.1.1 does not install or run games. Opening one displays the
Z# Hub message that games are currently unavailable. If an application cannot
launch, the Hub displays `APPNAME failed to launch!` followed by the preserved
compiler, settings, package, or runtime reason.

## Native build

The native toolchain uses C17 and CMake:

```text
cmake -S . -B build
cmake --build build
```

Release maintainers can rebuild every native runtime embedded by the Java
library with a portable Zig compiler:

```text
powershell -File scripts/build-embedded-runtimes.ps1 -Zig path/to/zig.exe
```

## Java integration

The Java library requires Java 17 or newer. It provides the same API to Gradle
and Maven projects and communicates with the native toolchain as a child
process. Native runtimes are bundled for Windows x64/ARM64, Linux x64/ARM64,
and macOS Intel/Apple Silicon, so a separate Z# installation is not required
on those platforms.

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
    implementation("com.zombieos:zsharp:1.0.1.1")
}
```

or Maven:

```xml
<dependency>
    <groupId>com.zombieos</groupId>
    <artifactId>zsharp</artifactId>
    <version>1.0.1.1</version>
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

The Java API also exposes `registerProject(...)`,
`packageProject(project, ZSharpPackageType.APP, "MyApp")`, and
`openPackage(...)` for project registration and `.zapp`/`.zgame` files.
Pass `true` as the fourth `packageProject` argument to create the unbytecoded
companion as well.
`launchProject(...)` remains as a deprecated registration alias and no longer
launches a window.

External projects can be registered from Java as well:

```java
var result = zsharp.runWithProviders(
    java.nio.file.Path.of("game.zsharp"),
    java.util.Map.of("playfab", java.nio.file.Path.of("playfab_provider.dll"))
);
```

The library first honors `ZSHARP_BIN` and `ZSHARP_HOME/bin` as explicit
overrides. It otherwise selects the bundled runtime for the current operating
system and CPU, checksum-verifies it, extracts it to the user's private cache,
and launches it automatically. Unsupported platforms fall back to the system
`PATH`. Bundled Linux runtimes target glibc 2.17 or newer; a musl-based system
such as Alpine can use a compatible external runtime through `ZSHARP_BIN`.

JarJar, Shadow, and similar dependency-bundling tools can carry Z# inside a
developer's application or mod. The bundler must retain
`META-INF/zsharp/runtime/**`; no separate installation is needed for users on
a platform whose runtime is present there.

## License

Z# is licensed under the [Apache License 2.0](LICENSE).

## Syntax decisions still open

The main remaining definitions include memory and pointer syntax, hardware I/O,
the dependency registry and publisher signing model, engine-specific result
types, and eventually the first Z1-to-Z2 migration mappings.
