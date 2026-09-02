# Z#

Official website and downloads: <https://www.zsharp.zombieos.com>

Z# is a systems programming language implemented in C. Its official source-file
extension is **`.zsharp`**.

Version 1.0.2.0 is the first playable game-engine update. It adds executable
`.zgame` packages, declarative `.zobject` scenes and objects, the
`zsharpgame:1.0.0.0` dependency, Vulkan drawing, input, cameras, frame timing,
2D/3D transforms, physics, collisions, and audio. Windows and Linux are the
advertised game targets. A MoltenVK path is built for macOS, but game support
there remains experimental until it is tested on Mac hardware.

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
millisecond/second timing while preserving window redraws. It runs
`type.script:2D` and `type.script:3D` projects through SDL3 and Vulkan when
`zsharpgame:1.0.0.0` is declared. `.zobject` files provide scenes, primitive
and text objects, transforms, cameras, input-driven movement,
static/dynamic/kinematic bodies, gravity, collisions, generated tones, and WAV
playback. Native `.zss` files apply CSS-style rules to game objects and window
elements without a browser. Game properties use normal Z# reads and `.set:` writes. The runtime
owns the window, event/input/audio loop, frame timing, concurrent `Start[]`
tasks, Vulkan presentation, and resize recovery. The bundled
native `zsharpwindow` backends render designs,
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
zsharp game-info
zsharp project path/to/project.zsettings
zsharp run hello.zsharp
zsharp run-bytecode hello.zbc
zsharp package app path/to/project MyApp --unbytecode
zsharp package game path/to/project MyGame --unbytecode
zsharp run path/to/project/Packages/MyApp.zapp
zsharp run path/to/project/Packages/MyGame.zgame
zsharp uninstall path/to/project/Packages/MyApp.zapp
zsharp associate
zsharp hub
zsharp hub shortcut
zsharp update
zsharp publish
```

Command documentation uses `<value>` for a required argument and `[value]`
for an optional argument. For example, `zsharp publish [repository]` can be run
from the Z# language repository with no argument, or be given its path.

The `.zbc` bytecode extension shown here is provisional. `.zsharp`, `.zapp`,
and `.zgame` are official extensions.

End users can install the standalone ZVM with the small platform bootstrap in
`%USERPROFILE%\Downloads\ZSharp Publishing\1.0.2.0`. It downloads the current runtime from
`https://www.zsharp.zombieos.com/update.js?v=CURRENTVERSION-OS`, compares the
installed version with the static GitHub Pages manifest, verifies
`assets/download/ZVM-LATEST.zip` and the selected runtime, preserves the
previous runtime, and configures the command and package associations for the
current user. Every successful install creates or refreshes a `Z# Hub`
launcher on the user's desktop. On Windows, a single tray agent starts at sign-in, checks once
immediately and then hourly, and notifies the user before installing a newer
verified release. Linux and macOS repeat the quiet check when the ZVM launches.
See [INSTALLING.md](INSTALLING.md).

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
Add `--unbytecode` to also create either
`<filename>-unbytecoded.zapp` or `<filename>-unbytecoded.zgame`. The companion is a standard
ZIP-compatible source archive: renaming it from `.zapp` to `.zip` exposes a
copy of the original project files. The command adds extensions automatically,
validates the settings and every included `.zsharp` file, and rejects unsafe
paths when opened. `zsharp run App.zapp` verifies and extracts an application
into a private content-addressed cache before its configured `Window Startup`
script runs. For `.zgame`, Z# selects a 3D startup script when one exists,
otherwise a 2D startup script. Normal packages launch their embedded startup
bytecode; source companions launch the validated `.zsharp` startup.
On first installation, Z# offers to create a Desktop shortcut using the app's
design icon. `zsharp associate` registers `.zapp` and `.zgame` for the current
user so opening either launches the package without opening a terminal. Running
`zsharp open`, `zsharp run`, or another command in a terminal keeps using that
terminal normally.

Opening a package also adds it to the Z# Hub. `zsharp hub` opens the graphical
Hub, where remembered apps and games can be launched, new package paths can be
added, and entries can be forgotten without deleting the package. An optional
`Icon: "assets/icon.png":` entry in `project.zsettings` supplies the image at
the left of the project name and the app's Z#-created Desktop shortcut;
otherwise the official Z# logo is used. Clicking the icon or project name
shows its type, PID, project version, required Z# version, accumulated
playtime, and last-played time. Achievements are marked **Coming soon**. The
script-friendly forms are `zsharp hub list`, `zsharp hub add <package>`, and
`zsharp hub remove <PID>`. `zsharp hub shortcut` repairs or recreates the
desktop Hub launcher. Missing package files are omitted automatically.
`zsharp update` starts the installed verified updater without opening another
terminal and checks the official update manifest immediately. A desktop
notification reports either `Z# is up to date.` or a successful version
change.

The `.zgame` container uses the same integrity, cache, association, shortcut,
and uninstall foundation as `.zapp`. Its startup creates an SDL3 high-DPI game
window and forces the Vulkan renderer. Windows and Linux are supported game
targets. macOS builds retain the MoltenVK translation path, but it is currently
experimental and is not advertised as supported. If a package cannot launch,
the Hub displays `APPNAME failed to launch!`
followed by the preserved compiler, settings, package, or runtime reason.

## Native build

The native toolchain uses C17 and CMake:

```text
cmake -S . -B build
cmake --build build
```

Release game runtimes must be built natively so SDL3 can select each operating
system's video, input, gamepad, and audio backends. Run the repository's
**Build native game runtimes** GitHub workflow; it builds and checksums Windows
and Linux release candidates plus experimental macOS candidates on x64 and
ARM64. Its artifacts are already shaped for
`java/src/main/resources/META-INF/zsharp/runtime/<platform>`. The
`scripts/stage-native-runtime.ps1` helper stages a locally built runtime and,
on macOS, its `libMoltenVK.dylib` companion. The older portable Zig script now
requires an explicit opt-in because its output is compiler/server-only and does
not contain the SDL/Vulkan runtime.

## Java integration

The Java library requires Java 17 or newer. It provides the same API to Gradle
and Maven projects and communicates with the native toolchain as a child
process. Native runtimes are bundled for Windows x64/ARM64, Linux x64/ARM64,
and macOS Intel/Apple Silicon, so a separate Z# installation is not required
on those platforms. The macOS game path is still experimental; the macOS
runtime remains available for the compiler, VM, and app/window features.

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
    implementation("com.zombieos:zsharp:1.0.2.0")
}
```

or Maven:

```xml
<dependency>
    <groupId>com.zombieos</groupId>
    <artifactId>zsharp</artifactId>
    <version>1.0.2.0</version>
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
