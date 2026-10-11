# Z# syntax guide

## Development preview: input submission (1.2.2.0)

Native text inputs may declare `Submit[Chat:Chat:Send]:`, using the same callback
path rules as button clicks. On the Windows development backend, Enter invokes
this callback without inserting a newline; Shift+Enter inserts a newline when
`multiline: alive:` is enabled. Without Submit, existing Enter behavior is
unchanged. Holding Enter does not repeatedly submit. Linux/macOS submission
handling is not implemented yet.

`ZSHARP_C_FUNCTION_DURATION_OPTION` (kind 4) is a C-extension function option
accepting exactly one nonnegative duration literal, such as `Demo.Cooldown(5s):`
or `Demo.Cooldown(5ms):`. Like text options, it appears before executable
statements in a custom function declaration. Its binder receives the callback
target followed by a NUMBER measured in milliseconds, once during initialization.
Seconds are converted using the same decimal arithmetic as `wait`.

C extensions may also register `ZSHARP_C_FUNCTION_OPTION` (kind 3) through
`add_declaration`. Its name is a qualified identifier, for example `Demo.Option`.
Inside a custom function declaration, `Demo.Option("one", "two"):` must precede
executable statements and takes one or more literal texts. The option binder
runs after the function binder during initialization, receiving the function's
`File:Room:Function` target followed by the texts. Options are not executed as
part of the callback body and are preserved in bytecode. Extensions validate
their own option semantics; ordinary brains do not accept these declarations.

## C-defined declarations and background services (1.2.1.0)

C modules may export `zsharp_c_register_v2(ZSharpCSyntaxRegistryV2 *, char *, size_t)`.
Its `v1` member retains the existing statement, block and expression registry.
`add_declaration(context, name, kind, binder, error, error_size)` adds a new
function declaration type (`ZSHARP_C_FUNCTION_DECLARATION`) or a named room-level
configuration block (`ZSHARP_C_NAMED_BLOCK_DECLARATION`). Existing built-in types
cannot be replaced. Modules with only the v1 hook continue to work.

```javascript
noticed custom_event Handler[text Message, number Count] (
 Print(Message):
)

CustomConfig Settings (
 label: File.read("label.txt"):
 nested ( value: Math.pow(2, 3): )
):
```

The module must register `custom_event` and `CustomConfig`; they are not built-ins.
Function declarations retain normal Z# parameters and bodies, and do not run
automatically even if named `Start`. Before startup, their binder receives the
declaration name, `File:Room:Function` target, then alternating parameter name/type
texts. Named configuration binders receive their name followed by alternating
dotted field paths and evaluated scalar values. Nested blocks flatten to paths
such as `nested.value`. Hex colors and literal text/number lists are passed as text.
Lists use JSON text. Configuration expressions are evaluated at runtime, not while
the compiler checks source. They are preserved in bytecode.

An optional `zsharp_c_service_v1` hook supports queued callbacks and host lifetime.
See `native/include/zsharp_c.h` for ACTIVE/POLL/COMPLETE/SHUTDOWN operations.
POLL receives the owning source name in `event.target` and must only return events
for that owner. The returned target must belong to an owned custom function
declaration; parameter counts and types are checked by ZVM. Worker threads queue
events; ZVM task threads execute the bodies. Callbacks are serviced at instruction
boundaries and after `Start[]` returns. Active services keep console scripts alive;
closing a game/window cancels its tasks and shuts down their services. Pending
callbacks are not serviced while a Z# task is blocked inside a native call.

DiscordZ uses this extension for `discord_command_hybrid`, `discord_command_slash`,
`discord_command_msg`, named `Webhook` blocks and `Discord.Print`. These are
dependency-provided syntax, not built-in Z# features. Import `c:discordz.C.Main()`
and rebuild DiscordZ against the 1.2.1 development C header before using them.

This guide explains how to write Z# and compares its concepts with C#, Java, and C. The official source extension is `.zsharp`.

This edition is organized for Z# 1.2.0.0 around five areas: getting started, the Z# language, apps, games, and language interoperability. Features that were previously documented as version-specific additions are now placed with the part of Z# they belong to.

For the underlying design record, see [LANGUAGE.md](LANGUAGE.md). For bytecode details, see [BYTECODE.md](BYTECODE.md).

## Contents

- [Getting Started](#getting-started)
- [Z# Language](#z-language)
- [Apps](#apps)
- [Games](#games)
- [Languages](#languages)

---
# Getting Started

## Files and project layout

Every Z# project has one `project.zsettings` file in its root. Normal source
files end in `.zsharp`.

A small project can look like this:

```text
MyProject/
 project.zsettings
 Main.zsharp
 Math.zsharp
 assets/
```

Normal scripts begin with:

```zsharp
zsharp = type.script
```

The settings file begins with:

```zsharp
zsharp = type.settings
```

Project metadata may include an optional Hub icon:

```zsharp
Project: "My App":
PID: "my_app":
Version: [1.0.0.0]:
Authors: ["Author"]:
Description: "A Z# app":
Icon: "assets/icon.png":
ZSharp: [1.0.2.1]:
```

`Icon` accepts a project-relative PNG. Paths use `/`
and cannot leave the project. If `Icon` is omitted, the Hub downloads and
caches the official Z# logo from `https://www.zsharp.zombieos.com/zsharp.png`.
The same project icon is used for Desktop shortcuts created by Z#; a window's
design icon remains the fallback for older projects without `Icon`.

## Project settings

A complete basic settings file is:

```zsharp
zsharp = type.settings

Project: "Project Display Name":
PID: "project_id":
Version: [1.0.0.0]:
Authors: ["Author1", "Author2"]:
Description: "This is a Z# Project!":
ZSharp: [1.0.2.1]:

Dependencies (
 playfab:1.0.0.0
):
```

The fields mean:

- `Project` is the human-readable display name.
- `PID` is the unique project identifier.
- `Version` is the project's four-part version.
- `Authors` lists any number of authors.
- `Description` describes the project and uses `\n` for new lines.
- `ZSharp` selects the language version and generation.
- `Dependencies` lists `projectId:projectVersion` pairs.

Normal PIDs are lowercase and may contain numbers and underscores. Spaces in a
PID are normalized to underscores during compilation.

Reserved official projects are the exception to lowercase import names. Their
names use official capitalization, such as:

```zsharp
import ZSharp.Window.Button():
import ZOS.Cloud.Saves():
```

Third-party projects cannot claim reserved official names such as `ZSharp` or
`ZOS`.

## Application startup

A native Z# window application declares its startup window in settings:

```zsharp
Window (
 Startup: "Windows/Startup.zsharp":
 Uninstall: "Windows/Uninstall.zsharp":
):
```

`Startup` is required when a `.zapp` uses native Z# windows. `Uninstall` is
optional; omit it when the application does not need a custom uninstall UI.

An application backed by an existing native executable may use platform
targets instead of a Z# startup window:

```zsharp
Native[JSON] (
 [
  {
   "platform": "windows-x86_64",
   "start": "Engine/ZSharpEngine.exe"
  },
  {
   "platform": "linux-x86_64",
   "start": "Engine/zsharp-engine"
  }
 ]
):
```

Supported target IDs are `windows-x86_64`, `windows-aarch64`,
`linux-x86_64`, `linux-aarch64`, `macos-x86_64`, and `macos-aarch64`. Paths
are project-relative and must name existing files. ZVM selects the current
platform, starts the executable with its own directory as the working
directory, forwards launch arguments, waits for it to exit, and records its
playtime. A package with no matching target reports the missing platform
instead of attempting to run a different build.

The supported file headers are:

```zsharp
zsharp = type.script:window
zsharp = type.script:audio
zsharp = type.scene
zsharp = type.object
zsharp = type.settings
```

Normal game logic remains `type.script`. Each `.zscene` file uses `type.scene`,
each `.zobject` file uses `type.object`, and each `.zaudio` source uses
`type.script:audio`. The old `type.script:2D`,
`type.script:3D`, `type.object:2D`, and `type.object:3D` headers are no longer
valid source syntax.

## Application and game packages

### Package exclusions: `.zignore`

From Z# 1.2.1.0, place an optional UTF-8 `.zignore` in the project root.
It filters files before hashing and source validation for `.zapp`, `.zgame`,
their source companions, and `.zpackage` dependencies. It never deletes source
files or affects `zsharp run`/imports in your working project. `.gitignore` does
not control packaging. `.zignore` itself is always omitted from archives.

```text
# Secrets and source-only examples
.env
.env.*
/Z# Examples/
*.obj
__pycache__/
Assets/**/*.cache
!Assets/important.cache
```

Rules are ordered; the last matching rule wins. Blank lines and lines beginning
with `#` are ignored. `!pattern` re-includes a matching path; `\#` and `\!` match
literal leading characters. `/` anchors to the root, and patterns containing
`/` are root-relative. Names without `/` match at any depth. A trailing `/`
matches directories only. `*` and `?` do not cross `/`; `**` can cross directories
and `**/` also matches zero directory levels. Bracket classes/ranges such as
`[a-z]` and `[!0-9]` are supported. Backslashes escape literal characters;
use forward slashes for path separators. Unescaped trailing spaces are ignored.
Matching is case-sensitive on all systems; wildcard matching is byte-oriented.

This supports common Git-style patterns, not every Git ignore feature: only
the root `.zignore` is read, and existing built-in exclusions cannot be undone.
Excluded directories are not traversed, so re-include a parent before trying
to re-include its children. Settings and configured startup/native targets may
not be excluded; packaging fails with a clear error. Do not exclude runtime
imports, objects or assets your application needs. Rules are limited to 1 MiB,
10000 entries and 4096 bytes per pattern; oversized, NUL-containing or unreadable ignore files fail
packaging rather than silently including their contents.

### Source dependency packages

From Z# 1.2.1.0, reusable project source can be distributed as a `.zpackage`:

```text
zsharp package pack path/to/library DiscordZ
```

This creates `path/to/library/Packages/DiscordZ.zpackage`. Packs are always
source-only: no compiled Z# bytecode and no `--unbytecode`/`--unbytecoded`
option. A library needs `project.zsettings`, but does not need an application
startup window or a game scene. Native C/C++/Rust/Kotlin modules still need
their compiled platform libraries alongside their source; a pack does not
automatically compile those languages.

Declare a Store project ID and four-part release version in the consuming
project's settings:

```zsharp
Dependencies (
 discordz:1.0.0.0:
):
```

The trailing `:` after each release is optional for compatibility with older
settings. Both `zsharp run path/to/Bot.zsharp` and app/game packaging prepare
dependencies **before parsing source**, including imported custom C syntax:

- First look for a matching `.zpackage` directly inside `Dependencies/`.
  Its internal PID/version must match; the filename can be anything.
- If absent, download
  `https://zos-store-api.zos-store-api.workers.dev/?action=download&project=PROJECT_ID&release=RELEASE_ID`
  to `Dependencies/PROJECT_ID-RELEASE_ID.zpackage`.
- Validate the source archive and install it under
  `Dependencies/PROJECT_ID/RELEASE_ID/`. Existing matching installations are
  reused. Concurrent runs coordinate installation rather than sharing a
  partially downloaded file.

Corrupt local archives and multiple archives matching the same PID/release
are errors, not reasons to silently download a replacement. Releases are
immutable: replacing an already-installed release with different archive
contents reports an error; move its installed folder aside first. Downloads
are limited to 1 GiB and use HTTPS. Transitive dependencies are prepared too;
cycles and excessively deep/large dependency graphs are rejected.

Use normal project-qualified imports and calls, for example:

```zsharp
import discordz.Scripts.Client():
import c:discordz.C.Main():

Function.call(discordz:Client:Client:Initialize):
Function.call(c:discordz.C.Main:initialize []):
```

These are examples of library API names, not built-in DiscordZ functions.
Within the library, its own project-relative paths and imports stay relative
to that library. Dependency files are isolated from the consuming project's
source lookup, so both projects may have a `Main.zsharp`. Dependency `Start[]`
functions do not automatically run as application/game startup tasks; call
initialization explicitly.

App/game packages include the installed dependency source/assets, without
duplicating the `.zpackage` archives. Packaged applications therefore do not
need a Store download for those included dependencies. Built-in runtime
dependencies (`zsharp`, `zsharpwindow`, `zsharpgame`, `zos`) and explicitly
supplied native `--provider` projects keep their existing behavior.

Dependencies are executable code, including any native libraries or custom
syntax modules they contain. Archive validation checks identity, paths and
integrity; it is not a safety review or a publisher signature. Only use trusted
dependencies. A `.zpackage` is not a runnable `.zapp` or `.zgame`.

Z# applications use `.zapp` and games use `.zgame`.

These are cross-platform Z# container formats. The normal 1.0.2.1 container
stores the validated project plus its compiled startup, with a SHA-256 hash for
each entry. The unbytecoded companion uses the standard ZIP container and ZIP
CRC checks. The runtime rejects corrupt data, absolute paths, `..` traversal,
and symbolic-link/reparse-point content.

A package can contain:

```text
project.zsettings
assets/
source files
```

Build packages with:

From 1.2.1.0, game packaging shares one freshly loaded scene/object validation
model across script checks and startup compilation for each output. All scene,
object, script, and package integrity checks still run. This is build-local
reuse, not a persistent cache: changes are checked again on the next build.
Set the environment variable `ZSHARP_PACKAGE_PROFILE=1` to print the validation
model-load count and placement count while diagnosing packaging performance.

```text
zsharp package app path/to/project Application
zsharp package game path/to/project Game
zsharp package app path/to/project Application --unbytecode
zsharp package game path/to/project Game --unbytecode
```

Z# adds the extension and puts the result in the project's `Packages` folder:

```text
path/to/project/Packages/Application.zapp
path/to/project/Packages/Game.zgame
path/to/project/Packages/Application-unbytecoded.zapp
path/to/project/Packages/Game-unbytecoded.zgame
```

The last argument is a filename, not a path, and must not include `.zapp` or
`.zgame`. Without `--unbytecode`, one normal bytecoded package is created. With
the option, Z# creates both the normal package and the
`-unbytecoded` companion. The companion is a standard ZIP-compatible source
archive. Rename `Application-unbytecoded.zapp` to
`Application-unbytecoded.zip` to browse the original project files. Renaming
does not change their contents.

The packager checks `project.zsettings` and validates every included `.zsharp`,
`.zscene`, `.zobject`, and `.zaudio` file. An app requires a configured `Window Startup`;
a game requires `zsharpgame:1.0.0.1`, at least one normal `type.script` file,
and at least one `type.scene` `.zscene` file. The first source and scene in
sorted path order are used as the startup script and initial scene. Renaming an
ordinary ZIP file is not enough; Z# source packages carry a format marker and
must be produced by `zsharp package --unbytecode`.

The normal package launches an embedded, integrity-checked startup bytecode.
The unbytecoded companion launches its validated `.zsharp` startup. Both forms
use the same project metadata, package cache, application window behavior, and
uninstall flow.

Open or uninstall one with:

```text
zsharp run path/to/project/Packages/Application.zapp
zsharp run path/to/project/Packages/Game.zgame
zsharp uninstall path/to/project/Packages/Application.zapp
zsharp uninstall path/to/project/Packages/Game.zgame
```

File associations can pass a package directly to the runtime, so
`zsharp Application.zapp` and the older `zsharp open Application.zapp` alias
are equivalent to `zsharp run Application.zapp`.
Install or refresh the current user's associations with:

```text
zsharp associate
```

On Windows, association also registers and starts the single-instance Z# tray
updater for the current user. It checks once after sign-in and then hourly,
notifies before a newer verified ZVM is installed, and provides **Check for
updates** and **Exit** tray-menu actions. Version comparison is numeric across
all four parts, so an older manifest never downgrades a newer installation.
Linux and macOS keep the quiet launch-time update check.

Opening an associated package or a Z#-created Desktop shortcut launches the app
without opening a terminal. On Windows, Explorer launches are detached from the
console; Linux desktop entries use `Terminal=false`; and macOS launcher apps run
Z# in the background. Explicit `zsharp open` and `zsharp run` commands continue
using the terminal in which they were entered. Existing Z#-created shortcuts
are upgraded to the silent launch form the next time their package runs.

Opening an application verifies every entry and extracts it to a private,
content-addressed cache before running the configured startup window. On its
first successful installation, the runtime asks whether to create a Desktop
shortcut. The shortcut launches that package through Z# and uses the startup
window's design icon when the platform can convert or use it.

If the startup window cannot launch, the Z# Hub opens with:

```text
APPNAME failed to launch!
EXACT FAILURE REASON
```

Uninstall asks the user to type `yes`, then permanently removes that verified
package cache, the selected package file, and any Z#-created Desktop shortcut
without using Recycle Bin or Trash.
The future app-data ledger and optional ZOS Cloud backup are not part of
1.0.2.1. See [UNINSTALL.md](UNINSTALL.md) for current behavior and the planned
full safety model.

`.zgame` uses the same secure package, cache, association, shortcut, and
uninstall foundation as `.zapp`. Opening one launches its normal game startup
script, loads and validates the project's separate `.zscene`, `.zobject`, and
`.zaudio` files, starts every
eligible non-`DR` `Start[]` task, and enters the SDL3/Vulkan game loop. Windows
and Linux are the supported game targets for 1.0.2.1. The macOS/MoltenVK game
path is experimental and is not advertised as supported until hardware tests
are completed.


---

# Z# Language

## Comments and terminators

`//` begins a comment that continues to the end of its line:

```zsharp
noticed text Message = "Hello": // This is a comment.
```

Statements end with `:` rather than `;`:

From Z# 1.2.0.1, `Print(value).update:` replaces the current unfinished console
line without adding a newline. Use it for progress or live input displays:

```zsharp
Print("Loading: 10%").update:
Print("Loading: 100%").update:
Print("Loaded!"):
```

Shorter replacements erase leftover text. Ordinary `Print(value):` finishes
the current line with a newline; completed earlier lines are not editable.
Updates flush immediately and are replayed by `zsharp terminal` connections.
Use single-line values that fit the console width; this is not a multiline
terminal UI. Output from concurrent tasks shares one current line, with each
print operation synchronized to avoid interleaving its bytes.

```zsharp
Print("Hello"):
```

Comparison:

```csharp
Console.WriteLine("Hello"); // C#
```

```java
System.out.println("Hello"); // Java
```

```c
printf("Hello\n"); /* C */
```

```zsharp
Print("Hello"): // Z#
```

Parenthesized declaration bodies use `(` and `)`:

```zsharp
noticed room Main[] (
 // Members go here.
)
```

## Rooms

A `room` is Z#'s class-like container:

```zsharp
noticed room Player[] (
 noticed text Name = "Zombie":
)
```

The rough equivalents are:

| Z# | C# | Java | C |
|---|---|---|---|
| `room` | `class` | `class` | No direct equivalent; often a `struct` plus functions |
| `noticed` | `public` | `public` | External linkage/API declaration |
| `silent` | `private` | `private` | File-local or hidden implementation |
| `horde` | `static` | `static` | `static` has related but context-dependent meanings |

A room can have one of three visibility levels:

```zsharp
noticed room PublicRoom[] (
)

room FileOnlyRoom[] (
)

silent room PrivateRoom[] (
)
```

- `noticed room` is accessible throughout the project when imported.
- A bare `room` is accessible only to rooms in the same file.
- A nested `silent room` is accessible only to itself and its parent.
- A top-level `silent room` cannot be called by another room.

Rooms can be nested:

```zsharp
noticed room Parent[] (
 silent room Child[] (
  noticed text Message = "Only Parent can reach this room":
 )
)
```

## Values and types

#### Text

`text` stores text values:

```zsharp
noticed text Name = "Zombie":
text LocalMessage = "Hello":
```

It is closest to C# `string` and Java `String`. C normally represents text
with a character array or pointer.

Text concatenation uses `+`. Numbers and statuses are converted to their
printable text when either side is text:

```zsharp
text Message = "Hello, " + Name:
text Position = "L" + CurrentLine + ":C" + CurrentColumn:
```

#### Numbers

`number` stores an arbitrarily sized ordinary decimal:

```zsharp
noticed number Visits = 10:
noticed number Price = 99.25:
```

Unlike C# `int`, Java `int`, or C `int`, a Z# number is not limited to a fixed
32-bit range and may contain a decimal point. It is conceptually closer to an
arbitrary-precision decimal type.

Scientific notation is not valid:

```zsharp
number Valid = 10000000000:
number Invalid = 1e+10: // Compile error.
```

Arithmetic operators are:

```zsharp
number Add = 4 + 6:
number Subtract = 10 - 3:
number Multiply = 4 * 5:
number Divide = 8 / 2:
number Remainder = 10 % 4:
number Negative = -5:
```

Terminating division preserves its needed digits. Repeating division keeps
only the first fractional digit:

```zsharp
Print(3 / 1): // 3
Print(1 / 3): // 0.3
```

`addition(...)` is the confirmed built-in addition form:

```zsharp
number Result = addition(4 + 6):
```

#### Status

`status` is the two-state type:

```zsharp
noticed status Alive = alive:
Alive = dead:
```

| Z# | C# | Java | C |
|---|---|---|---|
| `status` | `bool` | `boolean` | `_Bool`/`bool` |
| `alive` | `true` | `true` | `true`/nonzero |
| `dead` | `false` | `false` | `false`/zero |

#### Null

The missing-value literal is `null`:

```zsharp
noticed text Missing = null:
```

#### Default field values

A room field may omit its initial value:

```zsharp
noticed room Player[] (
 noticed text Name:
 noticed number X:
)
```

## Visibility

`noticed` means code outside the current scope may access the declaration:

```zsharp
noticed text PublicMessage = "Visible":
```

`silent` restricts access:

```zsharp
silent text Secret = "Hidden":
```

A declaration with no visibility word follows its enclosing file or room
visibility rules.

## Functions

#### Brain functions

`brain` is Z#'s no-declared-return-type function form, similar to `void`:

```zsharp
noticed brain SayHello[] (
 Print("Hello"):
)
```

Comparison:

```csharp
public void SayHello() { }
```

```java
public void sayHello() { }
```

```c
void say_hello(void) { }
```

```zsharp
noticed brain SayHello[] (
)
```

Parameters go inside `[]`:

```zsharp
noticed brain Move[number x, number y] (
 Print(x):
 Print(y):
)
```

#### Number-returning functions

Use `number` in place of `brain`:

```zsharp
noticed number Add[number Left, number Right] (
 feed(number.Left + number.Right):
)
```

`feed(value):` supplies the result:

```zsharp
feed(number.Left + number.Right):
```

#### Text-returning functions

Use `text` in place of `brain`:

```zsharp
noticed text Greeting[text Name] (
 feed("Hello, " + Name):
)
```

#### Returning without a value

`feed:` ends the current function without a value:

```zsharp
noticed brain StopEarly[] (
 feed:
 Print("This does not run"):
)
```

#### Start functions

A `brain` named `Start` runs automatically:

```zsharp
noticed brain Start[] (
 Print("Started"):
)
```

`Start[DR]` disables automatic execution:

```zsharp
noticed brain Start[DR] (
 Print("Runs only when explicitly called"):
)
```

For a window application, the ZVM creates the startup window and then runs the
non-`DR` `Start` brain in every normal project script. This is application
startup behavior, not a call made by the window file. A window invokes an
event function only for a configured button click.

Each automatic start runs as an independent ZVM task. An endless `loop` in one
startup brain therefore does not prevent the other scripts from starting.
Internal button callbacks are tasks too, so a long-running callback does not
freeze the native window event loop. Closing the window cancels and joins its
tasks before the ZVM releases their memory.

Primitive room fields keep their current values between button presses. Calls
from the same source room run in order, so a counter used by a click handler is
not reset or updated by two overlapping presses. Different rooms can still run
at the same time.

#### Timing statements

`wait` pauses the current brain. Durations use `ms` for milliseconds or `s`
for seconds and may be ordinary Z# decimals:

```zsharp
wait(5ms):
wait(1.5s):
```

`delay` is an exact alias:

```zsharp
delay(0.25s):
```

Durations cannot be negative. The current runtime accepts up to seven days in
one statement. During a window callback, native window events and redraws keep
running while the callback waits.

#### Function calls

Ordinary declared functions are called with `Function.call(...)`:

```zsharp
Function.call(Home:Main:Start_Outcome):
```

Arguments appear in `[]` after the target:

```zsharp
number Result = Function.call(Math:Calculator:Add [4, 6]):
```

Call paths expand by location:

```zsharp
Function.call(File:Room:Function):
Function.call(Project.File.Room.Function):
```

The file search is recursive and considers only `.zsharp` files. Ambiguous
duplicate filenames produce an error.

Instance methods use the object method form:

```zsharp
User.Move[10, 20]:
Room.User.Move[10, 20]:
File.Room.User.Move[10, 20]:
Project.File.Room.User.Move[10, 20]:
```

#### Callback variables and function references (1.2.1.0)

`Function.call` also accepts a variable name. Room fields, local values, and
function parameters may supply either `text` or the dedicated `function` type:

```zsharp
noticed text TextCallback = "Actions:Actions:Move":
noticed function MoveCallback = Actions:Actions:Move:

noticed brain Forward[text CallBack] (
 Function.call(CallBack [10, "hi"]):
)

noticed brain ForwardTyped[function CallBack] (
 Function.call(CallBack [10, "hi"]):
)

noticed brain Start[] (
 text LocalText = "Actions:Actions:Move":
 function LocalFunction = Actions:Actions:Move:
 function Copy = LocalFunction:
 Function.call(LocalText [10, "hi"]):
 Function.call(Copy [10, "hi"]):
)
```

These declarations belong inside a room; import the target file normally in
every room that binds or invokes its callbacks. Defining a reference does not
execute it. Dedicated references bind qualified targets and are checked at
build time for existence, imports, and visibility; their parameter count/types
are checked when invoked. Local `function` declarations can copy an existing
function reference. Text targets are resolved at runtime and accept
`File:Room:Function`, `File.Room.Function`, a four-part project-qualified target,
or an existing foreign target such as `lua:Lua.Utilities:add`. Unquoted dedicated
references use `File:Room:Function`, `Project.File.Room.Function`, or the normal
`LANGUAGE:Path.To.File:function` spelling.

Omit the argument brackets for zero arguments: `Function.call(CallBack):`.
Returning functions also work in expressions, for example
`number Total = Function.call(CallBack [4, 6]):`, and existing named brain
outcome selectors still work. Invalid/empty targets, incorrect argument types
or counts, missing imports, and inaccessible targets report errors; callback
variables do not bypass visibility rules. A `function` parameter requires an
actual function reference, not ordinary text. This is a named-function
reference, not a closure capturing local variables or an object method.

`noticed brain Start[text CallBack]` is valid, but requires an explicit call
with its argument. Only zero-parameter `Start[]` brains automatically run.
The canonical spelling remains `Function.call`, with lowercase `call`.

## Local values and assignment

Local values omit visibility and exist only during that brain call. Room
fields include a visibility word such as `noticed` and keep their value for
later calls:

```zsharp
noticed brain Start[] (
 number Score = 10:
 text Name = "Zombie":
 status Alive = alive:
)

noticed number SharedScore = 10:
```

Direct assignment works for a local value:

```zsharp
Score = 15:
Name = "Updated":
```

Typed assignment works for both locals and room fields. If a local shadows a
room field with the same name, the local is changed:

```zsharp
noticed number Score = 10:
number.set:Score = Score + 5:
```

A text field on the current object is changed with `text.set`:

```zsharp
text.set.Name = Name + "!":
```

#### Random values

Z# provides random integers, decimals, and percentage chances without an
import:

```zsharp
number Die = random.number(1, 6):
number Offset = random.decimal(-1, 1):
status Critical = random.chance(15):
```

`random.number` includes both endpoints. `random.decimal` uses the supplied
bounds, and `random.chance` accepts a percentage from `0` through `100`.

A named object's field uses `.set`:

```zsharp
User.set.X = User.X + x:
User.set.Y = User.Y + y:
```

Qualified writes are allowed when visibility, imports, and provider permissions
allow them:

```zsharp
File.Room.User.set.Name = "Zombie":
Project.File.Room.User.set.X = 25:
```

### Persistent room fields

Room-level `text` and `number` values persist between repeated brain calls.
Use `text.set.Name = ...:` and `number.set:Name = ...:` to update them. Local
values remain local to the brain call.

## Conditions

### Shutdown cleanup (1.2.1.0)

An optional `noticed brain Shutdown[] (...)` runs during graceful shutdown.
For apps and games, ZVM stops and joins ordinary tasks first, then calls every
eligible `Shutdown[]` in project script files, including files without a
`Start[]`. Hooks run sequentially once, retaining the room's saved values.
`Shutdown[DR]` disables automatic cleanup. Console scripts run their shutdown
brains when execution finishes. No hooks are required.

```zsharp
noticed brain Shutdown[] (
 Function.call(cpp:Cpp.Discord.Discord:shutdown []):
)
```

Cleanup may print, update ordinary variables, and call native modules. It must
finish promptly; do not run endless loops or access closing window/game
properties. Cleanup does not receive the cancelled window runtime. A failed
hook is reported, but other hooks are still attempted. Force-ending the
process, power loss, or a native crash cannot guarantee cleanup. A native call
that never returns can prevent tasks from joining and reaching cleanup.

Conditions use square brackets:

```zsharp
if[Visits >= 10] (
 Print("Returning visitor"):
) else (
 Print("New visitor"):
)
```

Chain additional conditions with `else if`. Conditions are checked in order;
only the first matching branch runs. Spaces before `[` are optional.

```zsharp
if [Visits >= 10] (
 Print("Returning visitor"):
) else if [Visits >= 1] (
 Print("Welcome back"):
) else (
 Print("New visitor"):
)
```

Any number of `else if` branches can precede the optional final `else`.
As with a plain `if`, if no condition matches and the chain has no final
`else`, the current function returns early. Use `else ()` to continue instead.

Comparison operators include:

```text
==  !=  >  >=  <  <=
```

Logical operators are words:

```zsharp
if[Alive and Health > 0] (
)

if[Health == 0 or Health <= 20] (
)

if[not dead] (
)
```

An unusual Z# rule is that an unmatched `if` with no `else` ends the current
function early:

```zsharp
noticed brain OnlyWhenAlive[] (
 if[Alive] (
  Print("Alive"):
 )
 Print("This runs only when Alive was met"):
)
```

Add an `else`, even an empty one, when normal execution should continue:

```zsharp
if[Alive] (
 Print("Alive"):
) else (
)
Print("Continues either way"):
```

## Named brain outcomes

A brain can expose named conditional outcomes:

```zsharp
noticed brain Choose[number Value] (
 if(Score)[Value > 0] (
  feed(Value + 1):
 ) else (
  feed("not positive"):
 )
)
```

The caller selects the outcome after the call:

```zsharp
number Score =
 Function.call(Decisions:Choice:Choose [4])[Score]:
```

Only brains accept named-outcome selectors. The selected condition is checked
when the brain is called. If its path reaches no `feed(value):`, the result is
`null`.

Named outcomes are dynamically typed. Assigning a number to text converts it
to text. Assigning text to a number prints a recoverable runtime warning and
skips the assignment.

## Loops

Z# loops are unconditional:

```zsharp
loop (
 // Repeated code.
)
```

Loops do not contain a condition in their declaration. Use `if` and
`loop.end:` to stop:

```zsharp
noticed number Count = 0:

loop (
 number.set:Count = Count + 1:
 if[Count >= 10] (
  loop.end:
 ) else (
 )
)
```

`continue:` starts the next iteration:

```zsharp
loop (
 number.set:Count = Count + 1:
 if[Count == 1] (
  continue:
 ) else (
 )
 Print(Count):
)
```

Comparison:

| Z# | C#/Java/C |
|---|---|
| `loop (...)` | `while (true) { ... }` |
| `loop.end:` | `break;` |
| `continue:` | `continue;` |

## Arrays

An array type adds `()` after the element type:

```zsharp
noticed text() Names = ["Alex", "Sam", "Robin"]:
noticed number() Scores = [10, 20.5, 30]:
noticed Player() Players = [new Player["Zombie"]]:
```

Read an element with its zero-based index:

```zsharp
Print(Names[1]): // Sam
```

Change an element with `.set[index]`:

```zsharp
Names.set[1] = "Zombie":
Scores.set[0] = 99.25:
```

Read the length with `.Length`:

```zsharp
Print(Names.Length):
```

Add an object with `.add(...)`:

```zsharp
Players.add(new Player["Alex"]):
```

Comparison:

| Operation | Z# | Java/C# style |
|---|---|---|
| Declare text array | `text() Names = [...]` | `String[] names = {...}` / `string[] names = {...}` |
| Read | `Names[1]` | `names[1]` |
| Replace | `Names.set[1] = value:` | `names[1] = value;` |
| Length | `Names.Length` | `names.length` / `names.Length` |

## Objects and constructors

A constructor has the same name as its room:

```zsharp
room Player[] (
 noticed text Name:
 noticed number X = 0:
 noticed number Y = 0:

 noticed Player[text name] (
  text.set.Name = name:
 )

 noticed brain Move[number x, number y] (
  number.set:X = X + x:
  number.set:Y = Y + y:
 )
)
```

Create an object with `new` and constructor arguments in `[]`:

```zsharp
noticed Player User = new Player["Zombie"]:
```

Read fields with `.`:

```zsharp
Print(User.Name):
```

Call an instance method with `[]`:

```zsharp
User.Move[10, 20]:
```

## Horde members

`horde` is Z#'s static/shared modifier:

```zsharp
room Counter[] (
 noticed horde number Shared = 0:

 noticed horde brain Increase[] (
  number.set:Shared = Shared + 1:
 )
)
```

A horde field is shared by all objects of its room. A horde function cannot
directly access instance fields and cannot be invoked through an object.

Call it through its room path:

```zsharp
Function.call(Counters:Counter:Increase):
Print(Counter.Shared):
```

A horde room cannot be constructed, and every member must also be horde:

```zsharp
noticed horde room Tools[] (
 noticed horde number Uses = 0:
 noticed horde brain Use[] (
  number.set:Uses = Uses + 1:
 )
)
```

## Imports

Imports are placed inside a room and apply only to that room:

```zsharp
noticed room Launcher[] (
 import zsharp.Home():

 noticed brain Start[] (
  Function.call(Home:Main:Start_Outcome):
 )
)
```

Import an ordinary file with:

```zsharp
import Project.File():
```

Import a file inside folders with:

```zsharp
import Project.Folder.File():
```

Import every eligible endpoint from a project or namespace with a final
wildcard:

```zsharp
import Project.*():
import Project.Folder.*():
import ZSharp.*():
import ZSharp.Window.*():
```

`*` must be the last name in the import. Wildcards work for ordinary projects,
dependencies, and official namespaces, but do not bypass dependency or
visibility rules.

Importing a file exposes its eligible rooms and members. It does not bypass
visibility rules.

Using another file or project without importing it is a compile error.

## Qualified names

Value paths grow according to location:

```zsharp
Var
Room.Var
File.Room.Var
Project.File.Room.Var
```

Object paths use the same ladder:

```zsharp
User.Name
Room.User.Name
File.Room.User.Name
Project.File.Room.User.Name
```

Object method paths are:

```zsharp
User.Move[10, 20]:
Room.User.Move[10, 20]:
File.Room.User.Move[10, 20]:
Project.File.Room.User.Move[10, 20]:
```

Cross-project values may be read and written when their provider supports the
operation.

## External projects and native providers

Another project does not need to be written in Z#. A native provider can expose
C, C++, Java, C#, SDK, hardware, or service behavior as Z# project paths.

Example imports:

```zsharp
import playfab.Player():
import playfab.Entities():
```

Example access:

```zsharp
Print(playfab.Player.Data.PlayerId):
text.set.playfab.Player.Data.PlayerId = "updated-player-id":
playfab.Entities.Players.User.Move[3, 4]:
```

Provider ABI v1 currently supports the confirmed number, text, and status
operations described in [PROVIDERS.md](PROVIDERS.md).

## Typed JSON

Z# 1.0.2.4 can define a schema for flat JSON objects. Import the built-in JSON
feature and declare the expected keys and their Z# types:

```zsharp
noticed room JSONCustom[] (
 import ZSharp.JSON():

 noticed JSON PersonSchema[] (
  name: text:
  age: number:
  phone-number: text:
  enabled: status:
 )
)
```

Load a project-relative JSON file into a local value:

```zsharp
JSON Person = JSON.load(PersonSchema, "Data/Person.json"):
Print(Person.name):
Print(Person.age):
Print(Person["phone-number"]):
Print(Person.enabled):
```

Hyphenated keys use bracket access because `-` is also the subtraction
operator. A room-level loaded value may be `noticed`, bare/file-only, or
`silent`:

```zsharp
noticed JSON CurrentUser = JSON.load(PersonSchema, "Data/Person.json"):
JSON CachedUser = JSON.load(PersonSchema, "Data/Cached.json"):
silent JSON PrivateUser = JSON.load(PersonSchema, "Data/Private.json"):
```

Loading fails with a runtime error if the file is missing, leaves the project,
contains undeclared keys, omits required keys, or uses a value with the wrong
type. Schemas currently support `text`, `number`, and `status`; JSON booleans
become Z# `alive` and `dead` values.

## Text file I/O

Z# 1.1.2.0 provides project-relative text file operations:

```zsharp
File.write("Data/Save.txt", "Level=1"):
File.append("Data/Save.txt", "\nScore=250"):

status HasSave = File.exists("Data/Save.txt"):
text Save = File.read("Data/Save.txt"):
```

`File.write` creates or replaces a text file, while `File.append` creates it
or adds text to its end. `File.read` returns the full file as text and
`File.exists` returns `alive` or `dead`. Paths are relative to the project;
absolute paths and paths containing `..` are rejected. A single read is
limited to 16 MiB. Parent folders must already exist. These operations do not
open native file dialogs or grant access outside the project.

From Z# 1.2.1.0, recursively find files by extension:

```zsharp
text() Files = File.searchExtension(".zscene"):
Print(Files.Length):
text First = File.searchExtension(".zscene")[0]:
```

The extension must be nonempty and begin with `.`. Matching is ASCII
case-insensitive. Results are text paths relative to the project root with
`/` separators, sorted in case-sensitive path order. The search includes
all regular files under the project, including build/output/hidden folders;
symbolic links and junctions are not followed. No import is required.
No matches returns an empty array. Search-result arrays use ordinary zero-based
Z# indexing: `[0]` is the first file. Invalid indexes produce the usual
array bounds errors, so check `Files.Length` before reading `Files[0]`.
Results are a snapshot; another call searches again. Searches exceeding 256
nested directory levels fail instead of recursing indefinitely.

## Regular expressions

From Z# 1.2.1.0, no import is required for these expressions:

```javascript
status Valid = Regex.test("ABC123", "^[A-Z]+[0-9]+$"):
status Found = Regex.test("Hello", "hello", "i"):
text() Matches = Regex.matches("item12 item34", "\\d+"):
text First = Matches[0]:
text Updated = Regex.replace("a12 b34", "[0-9]+", "#"):
```

All arguments must be text. The optional final flags argument accepts `g`
(global), `i` (ignore case), `m` (multiline anchors), `s` (dot matches newlines),
and `u` (Unicode mode). Patterns follow the bundled QuickJS JavaScript RegExp
engine, not PCRE. Pattern and replacement strings are passed as data, never
executed as JavaScript source. Double backslashes in Z# strings: `"\\d+"`
passes `\d+` to the regex engine.

`Regex.test` returns alive/dead for any match; use `^` and `$` anchors when you
need a whole-string match. `Regex.matches` returns all full matches, not capture
groups, as a zero-based text array; no matches returns an empty array. You may
also index directly: `Regex.matches("a12", "\\d+")[0]`. Zero-length matches
advance safely instead of looping forever.

`Regex.replace` replaces all matches and returns the original text if none match.
Replacement templates support `$&` (the whole match), `$1`, `$2`, etc. (capture
groups), and `$$` (a literal dollar sign). `matches` and `replace` add `g`
automatically when it is absent. Invalid patterns, flags, or argument types
produce runtime errors. Each call uses an isolated engine context with a
16 MiB engine allocation limit, an approximately two-second execution deadline,
and at most 100000 returned matches. Returned text cannot contain NUL characters.

## Native math

Z# provides native scalar math expressions without requiring an imported
language:

```javascript
number Radians = Math.radians(InputDegrees):
number Sine = Math.sin(Radians):
number Cosine = Math.cos(Radians):
number Tangent = Math.tan(Radians):
number Root = Math.sqrt(Value):
number Positive = Math.abs(Value):
number Smaller = Math.min(First, Second):
number Larger = Math.max(First, Second):
number ConvertedDegrees = Math.degrees(Radians):
number Power = Math.pow(10, 5): // 100000; available from 1.2.1.0
```

`Math.pow(base, exponent)` accepts nested numeric expressions, for example
`number Power = Math.pow(2, Math.pow(3, 2)):` produces 512 (2 raised to 9).
It supports integer, fractional, zero, and negative exponents within the finite
double-precision range. Negative bases require integer exponents; zero cannot
have a negative exponent; `Math.pow(0, 0)` returns 1. Overflow produces a clear
runtime error, not infinity or invalid number text. Underflow may round to zero.
Thus `Math.pow(10, Math.pow(10, 123))` parses, but cannot be represented and
reports overflow. Use the result in an assignment, condition, or `Print(...)`;
math calls are expressions, not standalone colon-ended statements.

Trigonometric functions accept radians. `Math.radians` converts degrees to
radians, and `Math.degrees` performs the reverse conversion. `Math.sqrt`
reports a runtime error for a negative value. All Math arguments must be
numbers, and non-finite results are rejected. Native Math requires
`ZSharp: [1.1.2.2]:` or newer.

As of Z# 1.1.2.3, native Math results are always returned using ordinary
decimal notation. Tiny floating-point residue produced by trigonometry near
zero is normalized to `0`, so results never introduce unsupported scientific
notation into later Z# expressions.

Camera-relative X/Z movement can be calculated from the scene yaw:

```javascript
number Yaw = Math.radians(Start.cameraRotationY):
number ForwardX = Math.sin(Yaw):
number ForwardZ = 0 - Math.cos(Yaw):
number RightX = Math.cos(Yaw):
number RightZ = Math.sin(Yaw):

number MoveX = (ForwardX * ForwardInput) + (RightX * StrafeInput):
number MoveZ = (ForwardZ * ForwardInput) + (RightZ * StrafeInput):
number Length = Math.sqrt((MoveX * MoveX) + (MoveZ * MoveZ)):

if[Length > 0] (
 MoveX = MoveX / Length:
 MoveZ = MoveZ / Length:
) else (
)
```

Normalizing the X/Z movement vector prevents diagonal movement from being
faster than movement along one axis. Because repeating Z# division retains
one fractional digit, the result is intentionally approximate under the
current `number` rules.

## Errors and warnings

Compile errors include:

- misspelled or unknown language words;
- missing `:` terminators;
- unmatched room or function delimiters;
- invalid scientific number notation;
- missing imports;
- invisible rooms or members;
- unknown function outcome names;
- invalid object construction; and
- unresolved or ambiguous file names.

Recoverable runtime mistakes print a warning, skip the failed operation, and
continue. For example, assigning text output to a number does not crash the
program.

Failures that prevent meaningful execution stop the program. Examples include
division by zero, a missing required dependency, and corrupt bytecode.
When an installed app or game fails, its window closes, the Hub displays the
source error, and Z# writes a timestamped log under the installation data
folder's `logs` directory (`%LOCALAPPDATA%\\ZombieOS\\ZSharp\\logs` on
Windows).

#### Attaching a terminal

An installed app or game exposes its `Print(...)` output while it is running.
Connect by using the package filename recorded by the Hub:

```text
zsharp terminal "Z# IDE.zapp"
zsharp terminal ZSharpGameTest.zgame
```

The connection is output-only and does not start another copy of the project.
Type `zsharp terminal exit` inside the connected session to detach without
closing the app or game.

## Compilation and bytecode

Compile source to Z# bytecode:

```text
zsharp compile Main.zsharp -o Main.zbc
```

Compile and immediately run source:

```text
zsharp run Main.zsharp
```

Run compiled bytecode:

```text
zsharp run-bytecode Main.zbc
```

`.zbc` is currently provisional. `.zsharp` is the official source extension.

Compiled bytecode contains:

- a stable PID-derived project identity; and
- a SHA-256 hash of the compiled content.

The ZVM verifies these values before executing the bytecode.

## Java and Z# virtual machines

The current Java integration uses two execution systems:

```text
Java/Kotlin code -> JVM (`java`)
Z# bytecode      -> ZVM (`zsharp`)
```

A Java application starts on the JVM. When it uses
`com.zombieos:zsharp:1.0.2.1`, the library locates or extracts the bundled
native Z# runtime and starts it as a child process. The ZVM then compiles or
runs the requested Z# file.

JarJar, Shadow, or another dependency bundler can place the Z# runtime inside
the Java application's JAR. That makes distribution self-contained, but it
does not make Z# execute as JVM bytecode.

Therefore, a mixed Java and Z# application currently uses both:

- the JVM executes Java and Kotlin classes;
- the ZVM executes Z# bytecode; and
- the Java integration communicates with the Z# child process.

A future same-process integration could embed the ZVM as a native library
through JNI or Java's native-function facilities. Z# would still run on the
ZVM. A separate future compiler backend would be required for Z# itself to run
as JVM bytecode.

## Complete current console example

```zsharp
zsharp = type.script

noticed room Player[] (
 noticed text Name:
 noticed number X = 0:
 noticed number Y = 0:

 noticed Player[text name] (
  text.set.Name = name:
 )

 noticed brain Move[number x, number y] (
  number.set:X = X + x:
  number.set:Y = Y + y:
 )
)

noticed room Main[] (
 noticed Player User = new Player["Zombie"]:
 noticed text() Names = ["Alex", "Sam", "Robin"]:
 noticed status Alive = alive:

 noticed brain Start[] (
  User.Move[10, 20]:
  Names.set[1] = User.Name:

  if[Alive and User.X > 0] (
   Print(Names[1]):
   Print(User.X):
   Print(User.Y):
  ) else (
   Print("Player is unavailable"):
  )
 )
)
```

## Keyword comparison summary

| Purpose | Z# | C# | Java | C |
|---|---|---|---|---|
| Public visibility | `noticed` | `public` | `public` | Exported declaration |
| Private visibility | `silent` | `private` | `private` | `static`/hidden implementation |
| Class-like container | `room` | `class` | `class` | `struct` plus functions |
| No-value function | `brain` | `void` | `void` | `void` |
| Text | `text` | `string` | `String` | `char *`/`char[]` |
| Boolean | `status` | `bool` | `boolean` | `bool` |
| True | `alive` | `true` | `true` | `true`/nonzero |
| False | `dead` | `false` | `false` | `false`/zero |
| Shared/static | `horde` | `static` | `static` | `static` depending on context |
| Return a value | `feed(value):` | `return value;` | `return value;` | `return value;` |
| Early return | `feed:` | `return;` | `return;` | `return;` |
| Infinite loop | `loop (...)` | `while (true)` | `while (true)` | `while (1)` |
| Leave loop | `loop.end:` | `break;` | `break;` | `break;` |
| Next iteration | `continue:` | `continue;` | `continue;` | `continue;` |
| Missing value | `null` | `null` | `null` | `NULL` |

Z# deliberately does not copy the exact grammar of these languages. The table
compares intent, not necessarily implementation or memory behavior.

## Versioning and generations

Z# versions have four parts:

```text
generation.majorFeature.patch.revision
```

The first part selects the language generation:

```text
1.x.x.x -> Z1
2.x.x.x -> Z2
3.x.x.x -> Z3
```

The installed ZVM supplies runtime and client behavior. A 1.0.1.0 application
therefore continues to run on ZVM 1.0.2.1 and automatically receives runtime
fixes such as smoother window painting and silent Desktop launches; its package
does not need to be rebuilt. The `ZSharp` version in `project.zsettings`
describes the source version the project targets. New source fields and syntax
must be added to the project's code before the application can use them, while
runtime-only fixes apply automatically. Source may target a future Z# version
so development can begin early, but an installed runtime refuses to open an app
or game whose required version is newer than that runtime.

For the current release plan:

- `1.0.0.1` is the first Z1 release.
- `1.0.1.0` adds specialized script headers, native Windows/Linux/macOS window
  renderers, `.zapp`/`.zgame` packaging, file associations, Desktop shortcuts,
  and Hub launch-failure messages. Game objects remain a later runtime layer.
- A revision such as `1.0.0.2` would be a smaller patch without that feature
  scope.

When later generations replace older syntax, the compiler may emit migration
warnings that name both the old form and its recommended replacement. Z1 does
not invent warnings for generations that do not exist yet.


---

# Apps

## Window scripts

Window files begin with:

```zsharp
zsharp = type.script:window
```

There is exactly one window per `.zsharp` file. A window file does not contain
normal brain, number, or text functions. Event handlers live in an imported
normal script file.

The top-level window form is:

```zsharp
noticed Window Start[] (
 // Window imports and elements.
)
```

`Start` is only an example window name. The actual startup window is selected
by `project.zsettings`.

Window projects require:

```zsharp
Dependencies (
 zsharpwindow:1.0.0.0
):
```

Official UI features are imported individually:

```zsharp
import ZSharp.Window.Design():
import ZSharp.Window.Text():
import ZSharp.Window.Button():
import ZSharp.Window.Image():
import ZSharp.Window.TextInput():
```

Or import every Window feature at once:

```zsharp
import ZSharp.Window.*():
```

#### Window design

```zsharp
noticed design Design[] (
 title: "Window Title":
 icon: "assets/images/icon.png":
 width: 320zu:
 height: 180zu:
 scalable: alive:
 background: #FFFFFF:
)
```

A design background may also be a gradient:

```zsharp
background: linear-gradient(45:#FFFFFF:#336699):
background: radial-gradient(0:#101820:#336699:#FFFFFF):
```

Both gradient forms require a degree value and at least two `#RRGGBB` colors.
They accept any number of colors that fits in available memory, and the stops
are spaced evenly. Zero degrees points upward. For a radial gradient, degrees
rotate its focal point around the center.

If width and height are omitted, the window defaults to 25% of the current
screen. If its screen position is omitted, it starts in the screen center.

#### Responsive layout and scrolling

The full display is the window layout's reference canvas. At full width,
elements use their normal sizes and positions. As the user narrows the window,
images, buttons, and text inputs scale down horizontally with the available
width. Text keeps a readable font size and wraps onto additional lines instead
of disappearing outside the window.

Reducing only the window height does not shrink the layout. Content below the
visible area remains at its normal size and the window gains vertical
scrolling. Horizontal scrolling is not used.

#### Coordinates

Window and UI element coordinates use their center as `(0, 0)`:

```text
positive X -> right
negative X -> left
positive Y -> up
negative Y -> down
```

#### UI measurements

One `zu` is four pixels multiplied by the operating system display scale:

```text
pixels = zu * 4 * displayScale
```

An unsuffixed value defaults to `zu`:

```zsharp
width: 15:   // 15zu
width: 15zu: // 15zu
width: 15px: // exactly 15 pixels
```

#### Window text

Confirmed variants are:

```zsharp
text[title]
text[subtitle]
text[header]
text[subheader]
text[paragraph]
```

Plain `text` defaults to paragraph text.

```zsharp
noticed text[header] Header1[] (
 content: "This is a header!":
 color: #000000:
 locationX: 100:
 locationY: 100:
)
```

On Windows, text may begin with a project-relative inline PNG marker:

```zsharp
content: "<img:Assets/icon.png> Icon":
```

The image is drawn before the remaining text. The path cannot be absolute or
leave the project directory. Other desktop renderers will gain the same visual
behavior after their native implementations are tested.

#### Buttons

```zsharp
noticed button Button1[] (
 text: "Click Me!":
 textColor: #FFFFFF:
 buttonColor: #000000:
 width: 15:
 height: 15:
 locationX: 14:
 locationY: -12:
 Click[
  left: [File:Room:Function]:
  right: []:
 ]:
)
```

An empty click target means that mouse button has no handler. A target stores
a function reference; it does not call the function while the window is being
created. When a `Click` block is present, both `left` and `right` labels are
written; use `[]` for either unused target. The target script must be imported
and must satisfy visibility rules.

All UI `width` and `height` values must be greater than zero. Coordinates may
be positive, negative, or zero.

#### Images

```zsharp
noticed image Logo[] (
 file: "assets/images/ZSharp.png":
 width: 30:
 height: 35:
)
```

PNG, JPG/JPEG, BMP, and GIF files are scaled to the element's declared width
and height by the platform's native image facilities. The same applies to
`design.icon`. Windows uses WIC, Linux uses GdkPixbuf through GTK 3, and macOS
uses AppKit.

#### Text and image input

```zsharp
noticed textInput Input1[] (
 display: "This is an input box.":
 type: text:
 multiline: alive:
 wrap: alive:
 fontSize: 18px:
 maxLength: 120:
 textAlign: center:
 textTransform: uppercase:
 allowedCharacters: ["a", "b", "c", "1", "2", "3", "-", " "]:
 locationX: 10:
 locationY: 15:
 width: 15:
 height: 10:
 contents: []:
)
```

For image selection:

```zsharp
noticed textInput ImageInput[] (
 display: "Choose an image":
 type: image:
 supportedTypes: [png, jpg]:
 contents: []:
)
```

`contents` starts empty and is exposed to imported script functions as the full
live text or selected image path:

```zsharp
text EnteredText = Startup.Input1.contents:
text SelectedImage = Startup.ImageInput.contents:
```

The runtime changes UI state rather than rewriting packaged source code.
Functions in imported script files can read these live text-input values:

```zsharp
text FullText = Startup.Input1.contents:
number Characters = Startup.Input1.totalcharacters:
number Column = Startup.Input1.currentcolumn:
number Lines = Startup.Input1.totallines:
number Line = Startup.Input1.currentline:
```

The shorter `Input1.property` form also works. `currentline` and
`currentcolumn` are 1-based. An empty text input has zero characters, one line,
and a cursor at line 1, column 1. Line breaks count as characters, with a
Windows CRLF pair counted as one line-break character. The four numeric fields
are read-only and apply to text inputs; image inputs expose `contents` only.

#### Dropdown/select input

Use a native dropdown when one value must be selected from a fixed list:

```zsharp
noticed dropdown ModelSelector[] (
 options: ["ZOSAI-1 - Astroid", "Test Model"]:
 selected: "ZOSAI-1 - Astroid":
 textColor: #FFFFFF:
 dropdownColor: #181822:
 fontSize: 16px:
 width: 220px:
 height: 32px:
 locationX: 16px:
 locationY: 16px:
 anchorX: right:
 anchorY: top:
 Change[Handlers:Models:Changed]:
)
```

`dropdown` and `select` name the same element. `options` contains quoted
display values, and `selected` must equal one of them. Read or change the live
value with `Startup.ModelSelector.selected` and
`Startup.ModelSelector.selected.set: "Test Model":`. `Change` is optional and
uses the same imported `File:Room:Function` callback form as button clicks.

Every non-design element may use `anchorX: left:`, `center`, or `right` and
`anchorY: top:`, `center`, or `bottom`. Center is the compatibility default.
With an edge anchor, `locationX`/`locationY` becomes the inward distance from
that edge, allowing a top header, growing middle area, and bottom input bar to
stay placed when a scalable window is resized.

Text inputs are single-line by default. Add `multiline: alive:` to allow line
breaks. A multiline input wraps long lines by default; use `wrap: dead:` when
long lines should remain on one line and scroll horizontally instead. `wrap`
cannot be set unless multiline mode is alive, and neither field is valid for
an image input.

Text inputs can also set `fontSize`, limit input with a positive whole-number
`maxLength`, and align text with `textAlign: left:`, `center`, or `right`.
`textTransform` accepts `none`, `uppercase`, or `lowercase`. An optional
`allowedCharacters` array accepts quoted single characters; letters match
case-insensitively, so allowing `"b"` permits both `b` and `B`. Characters not
listed are ignored when typed or pasted. These fields require Z# 1.0.2.5.

Native window scrollbars remain hidden while all elements fit within the
visible window. A vertical scrollbar appears automatically when content
extends below the viewport. Multiline input scrollbars are handled inside the
input itself.

#### ZSS window styling

Window projects load every `.zss` file under the project directory. Use
`.Window Element` to target one element in a named window. `.Element` is the
short form when the element name is enough:

```css
.Startup CodeEditor {
 background: #0B0B10;
 color: #E8E8F0;
 border: 1px solid #272734;
 border-radius: 6px;
 font-family: Consolas;
 font-size: 15px;
 font-weight: normal;
 padding: 12px;
 caret-color: #FF2A42;
 outline: none;
 selection-background: #3A153D;
 selection-color: #FFFFFF;
}

.Startup Save:hover {
 background: #242431;
 border-color: #505064;
}

.Startup CodeEditor:focus {
 border-color: #606078;
}
```

The native window style pass supports `background`/`background-color`, `color`, solid
`border`, `border-color`, `border-radius`, `font-family`, `font-size`,
`font-weight`, `padding` and its four directional forms, `caret-color`,
`outline: none`, `selection-background`, `selection-color`, `width`, `height`,
`text-align`, `text-transform`, and `max-length`. `:hover`
targets buttons and `:focus` targets text inputs. ZSS rules are applied in
sorted file order and override matching values written in the window script.
Selectors and declarations are checked while the project compiles. ZSS is
CSS-shaped, but it styles native controls rather than a browser DOM, so
unsupported web-only properties produce a compile error.

#### Changing live window attributes

An imported normal-script callback can change the active window immediately:

```zsharp
noticed brain ToggleTheme[] (
 Startup.Design.title.set: "Dark mode":
 Startup.Design.background.set: linear-gradient(1:#101820:#000000):
 wait(1ms):
 Startup.Design.background.set: linear-gradient(2:#101820:#000000):
)
```

`Startup` is the window filename without `.zsharp`, `Design` is the declared
element name, and `background` is its field. The shorter form also works when
the active window is already unambiguous:

```zsharp
Design.background.set: #FFFFFF:
```

Wrap an expression in parentheses on the following line when a live property
needs a calculated value:

```zsharp
Startup.CursorPosition.content.set:
 ("L" + CurrentLine + ":C" + CurrentColumn):
```

From 1.1.0.1, text properties also accept a text variable directly:

```zsharp
text Response = "Hello":
Startup.ChatOutput.content.set: Response:
```

A text variable can serve as a reusable property-path alias. Its value is
resolved when the setter runs, so changing the text can retarget later writes:

```zsharp
noticed text Background = "Startup.Design.background":

noticed brain Animate[] (
 Background.set: linear-gradient(1:#FFFFFF:#000000):
 wait(1ms):
 Background.set: linear-gradient(2:#FFFFFF:#000000):
)
```

The alias must contain `Element.property` or `File.Element.property`. It must
refer to the active window when the callback runs.

The setter supports these live fields in 1.0.2.1:

- design: `title`, `icon`, `scalable`, `background`, `width`, `height`,
  `locationX`, and `locationY`;
- text: `content`, `color`, size, and position fields;
- button: `text`, `textColor`, `buttonColor`, size, and position fields;
- image: `file`, size, and position fields; and
- text input: `display`, size, and position fields.

Solid colors work in every color field. Gradients currently target the design
`background`. Input `contents` is owned by the person using the app at runtime;
input `type`, `supportedTypes`, and `Click` targets are not live-mutable.
Changing a property redraws it before the next statement, so a sequence of
gradient updates and short waits can animate a background.

### Dynamic window text and inputs

Normal text accepts `textAlign: left:`, `center`, or `right`. Wrapped text
recalculates its rendered height after `.content.set`, and window scroll bounds
are recalculated when that content grows or shrinks.

Text input contents can be replaced or cleared and keyboard focus can be
returned to an input:

```javascript
Startup.Prompt.contents.set: "":
Startup.Prompt.focus.set: alive:
```

Set `focus` to `dead` to release focus from that input. Multiline inputs treat
Enter as a newline. Z# 1.1.1.0 does not yet expose a portable Shift+Enter
modifier callback, so applications should use a Send button rather than
overriding Enter with application-specific behavior.

Window text and input use UTF-8 end-to-end, including non-Latin scripts and
emoji. Native runtime failures now include the source, room, brain, and Z# call
chain where available.

## Window project settings

```zsharp
zsharp = type.settings

Project: "My Application":
PID: "my_application":
Version: [1.0.0.0]:
Authors: ["Author"]:
Description: "A Z# application":
ZSharp: [1.0.2.1]:

Dependencies (
 zsharpwindow:1.0.0.0
):

Window (
 Startup: "window/Main.zsharp":
 Uninstall: "window/Uninstall.zsharp":
):
```

`Window (...)` is a settings section, not a function.

- `Startup` identifies the first window opened at launch.
- `Uninstall` identifies the window opened during `zsharp uninstall`.
- Both paths are relative to the project root.
- Both paths use `/` on every operating system.
- Neither path may escape the project with `../`.
- Both files must exist and use the `zsharp = type.script:window` header; the
  compiler validates their complete window syntax and imports.

Validate and register the project for the current user with:

```text
zsharp project path/to/project.zsettings
zsharp project path/to/project-folder
```

The tool walks upward from supplied files and folders to find the nearest
`project.zsettings` automatically. Registration does not launch the startup
window. Repeating the command updates the existing entry for that PID or path
instead of adding a duplicate.

The registry is stored in the current user's application-data location:

```text
Windows: %LOCALAPPDATA%\ZombieOS\ZSharp\projects.registry
Linux:   $XDG_DATA_HOME/zsharp/projects.registry
         (or ~/.local/share/zsharp/projects.registry)
macOS:   ~/Library/Application Support/ZSharp/projects.registry
```


---

# Games

## Game projects

Every game project must enable the official game
runtime in `project.zsettings`:

```zsharp
Dependencies (
 zsharpgame:1.0.0.1
):

Window (
 StartScene: "Game/Scenes/Main.zscene":
)

Splash[JSON] (
 [
  {
   "path": "assets/images/Studio.png",
   "duration": 2
  },
  {
   "path": "assets/images/Game.png",
   "duration": 2
  }
 ]
)
```

`Window.StartScene` selects the scene shown when the game starts; it replaces
the old file-order behavior. It requires `zsharpgame:1.0.0.1` but does not
require `zsharpwindow`. If a game also depends on `zsharpwindow`, its existing
`Startup` and `Uninstall` entries may share the same `Window` block, and
`Startup` remains optional whenever `StartScene` is present.

`Splash[JSON]` is optional. Each project-relative image is displayed in array
order for its positive `duration` in seconds before game scripts start. PNG and
BMP images are supported. Closing the game during a splash cancels launch.

The room and function syntax inside a game script is the normal Z# syntax. A
non-`DR` `Start[]` runs automatically after the SDL3 window and Vulkan renderer
are ready. Eligible starts across the project run as independent tasks, so an
endless game loop does not block window events or rendering. Closing the window
or pressing Escape cancels and joins those tasks.

Input paths such as `input.key.a` are global Z# runtime values and do not need
an import. A script must import another script-owned API before using it. For
example, a project with PID `zsharp_adhd_game` imports scene control with:

```zsharp
import zsharp_adhd_game.game.scene():
```

After that import, the room may read or write `Game.scene`. The wildcard form
`import zsharp_adhd_game.game.*():` is also accepted.

The 1.0.2.1 runtime initializes SDL video, audio, keyboard, mouse, and gamepad
support, creates a high-DPI resizable game window, and forces Vulkan for game
drawing. Windows and Linux are the advertised game targets. A bundled MoltenVK
path exists for macOS, but it is experimental until tested on Mac hardware.

Every scene lives in a separate `.zscene` file:

```zsharp
zsharp = type.scene

noticed scene Main[] (
 scene Main (
  title: "Main Scene":
  icon: "assets/icon.png":
  background: #08080B:
  gravityX: 0:
  gravityY: -900:
 )

 objects[JSON] (
  [
   {
    "id": "Player",
    "name": "Player One",
    "location": {
     "x": "-250",
     "y": "-200"
    }
   }
  ]
 )
)
```

Scene `icon` paths are project-relative PNG files. The active scene's icon is
used for the native game window. If a scene omits it, ZVM uses the project
`Icon`; if neither is supplied, it keeps the default Z# application icon.

Every object lives in a separate `.zobject` file and starts with
`zsharp = type.object`. ZVM treats the model as 3D when it uses a 3D-only
field such as a scene placement's `z`, `length`, `scaleZ`, `velocityZ`,
`gravityZ`, or
`cameraZ`, or the `cube` shape. Otherwise it renders as 2D. Coordinates start
at the center of the game view: positive X moves right, negative X moves left,
positive Y moves up, and negative Y moves down.

For example, a complete 3D scene file can contain:

```zsharp
noticed scene Main[] (
 scene Main (
  title: "3D Scene":
  background: #08080B:
  gravityX: 0:
  gravityY: -900:
  gravityZ: 0:
  cameraX: 0:
  cameraY: 0:
  cameraZ: 8:
  cameraRotationX: 0:
  cameraRotationY: 0:
  cameraRotationZ: 0:
  cameraFov: 70:
 )
 objects[JSON] (
  [
   { "id": "Cube", "name": "Main Cube", "location": { "x": "0", "y": "0", "z": "0" } }
  ]
 )
)
```

The configured `Window.StartScene` is active at launch. Without that setting,
the first discovered scene is used for compatibility. `cameraZ` and
`cameraFov` mainly affect 3D games. `cameraRotationX`, `cameraRotationY`, and
`cameraRotationZ` control pitch, yaw, and roll in degrees and can be changed
while the game is running. A game project must contain at least one
`.zscene` file.

Define a 2D object in its own `.zobject` file like this:

```zsharp
zsharp = type.object

noticed object Player[] (
 shape: rectangle:
 width: 48:
 height: 72:
 rotation: 0:
 scaleX: 1:
 scaleY: 1:
 color: #FF2A72:
 visible: alive:
 layer: 10:

 body: dynamic:
 collider: box:
 mass: 1:
 gravityScale: 1:
 restitution: 0:
 friction: 0.15:
 velocityX: 0:
 velocityY: 0:

 attributes[JSON] (
  [
   {
    "id": "COLLIDER2D",
    "active": true
   }
  ]
 )
)
```

The object declaration's ID is referenced by `id` in a scene's `objects[JSON]`
array. `name` is that placed instance's display name, while `location` owns its
X/Y position and optional 3D Z position. This lets an object definition remain
independent from the scene that places it. Starting in 1.1.4.0, a scene
placement may add optional `width`, `height`, `length` (or `depth`), `color`,
and `texture` fields after `location`. They override only that instance; omitted
fields inherit the `.zobject` defaults. Size values must be positive, colors
use quoted `"#RRGGBB"` text in the JSON, and texture paths are quoted safe
project-relative paths. For example, both placements below reuse `Cube.zobject`:

```json
[
  {
    "id": "Cube",
    "name": "Blue Cube",
    "location": { "x": "-10", "y": "0", "z": "0" },
    "width": "12",
    "height": "8",
    "length": "6",
    "color": "#3366FF"
  },
  {
    "id": "Cube",
    "name": "Black Cube",
    "location": { "x": "10", "y": "0", "z": "0" },
    "color": "#000000"
  }
]
```

The scene-level `texture` override uses the existing sprite/image asset path.
In 1.1.4.0, cubes also map a PNG or BMP texture onto each face; `color`
multiplies the texture, so use `#FFFFFF` for its unmodified colors.
`attributes[JSON]` accepts official
IDs such as `COLLIDER2D` and `COLLIDER3D`, plus project-defined IDs prefixed
with `CUSTOM:`. Each attribute has an `active` boolean. Object textures use
`texture: "path/to/texture.png":`; a 3D object's third size is `length:`.

Supported shapes are `rectangle`, `circle`, `triangle`, `sprite`, `cube`,
and `text`. `text` objects use a quoted `text:` field. The first sprite loader
accepts BMP assets through `asset:`; primitive shapes need no external asset.
`cube` is the initial 3D primitive and uses `positionZ`, `depth`, `scaleZ`, and
the active scene camera. Cubes support `rotationX`, `rotationY`, and
`rotationZ`; the older `rotation` property remains a Y-axis rotation for cube
compatibility and keeps its existing 2D behavior. A cube whose width, height,
or depth is omitted uses
`1` world unit for that dimension; 2D shapes retain their existing 64-pixel
defaults. The camera looks toward negative Z. Cubes crossing the camera's near
plane are clipped rather than discarded.

Bodies can be `static`, `dynamic`, or `kinematic`. Dynamic bodies receive
gravity and collision response. Kinematic bodies use `velocityX`, `velocityY`,
and `velocityZ` but do not receive gravity, making them suitable for moving
platforms. A grounded dynamic body is carried along X and Z by a kinematic
platform. Static bodies do not move. Colliders can be `none`, `box`, or `circle`;
the collision solver exposes grounded/colliding state, supports XYZ collision
for `COLLIDER3D`, and supports `trigger: alive:` for overlap-only objects. In
3D, box colliders follow the object's X/Y/Z rotation (including legacy
`rotation` as yaw), and spheres/capsules contact rotated boxes. Fast-moving
bodies are still resolved at discrete frame positions rather than by swept
collision. `restitution` controls
bounce and `friction` slows horizontal movement on a surface.

Z# does not provide a pre-made player movement controller. Game scripts read
input and change object position or velocity explicitly. A reusable movement
system can instead be distributed as a project dependency.

Game key reads use `input.key.KEY`. Supported names include `a` through `z`,
`0` through `9`, `larrow`, `rarrow`, `uarrow`, `darrow`, `fn1` through
`fn24`, `space`, `enter`, `escape`, `tab`, `backspace`, modifier keys, and
mouse properties:

```zsharp
input.key.a
input.key.rarrow
input.key.space
input.key.fn1
input.mouse.left
input.mouse.right
input.mouse.leftPressed
input.mouse.leftReleased
input.mouse.rightPressed
input.mouse.rightReleased
input.mouse.x
input.mouse.y
input.mouse.deltaX
input.mouse.deltaY
input.mouse.captured

Game.scene
Game.delta
Game.elapsed
Game.fps

Player.positionX
Player.velocityY
Player.grounded
Player.colliding
```

`Game.fps` measures the reciprocal of the most recent rendered-frame loop
interval (including presentation and pacing), not the fixed physics rate.
It can fluctuate; `Game.delta` still reports the physics timestep.
For performance investigation, set `ZSHARP_GAME_PROFILE=1` before launching
the development runtime. It reports frame intervals and physics/input,
snapshot, and render/present times to stderr. Profiling output itself adds
overhead; disable it for normal play.

The development 1.2.1.0 runtime uses SDL_GPU/Vulkan for 3D triangle filling,
texture sampling, blending, and depth testing where available. It retains
software depth rendering when GPU initialization is unavailable. Scripts,
physics, mesh transformation, and current lighting/shadow calculations remain
CPU work. No source changes are needed to select the GPU pass. For comparison
or troubleshooting, set `ZSHARP_GAME_SOFTWARE=1` before launch to force the
software renderer. Profiling prints the selected renderer at startup.
The new GPU path has been tested locally on Windows; Linux/macOS hardware
verification is still required before claiming support there.

### Game buttons (Z# 1.2.0.1)

```zsharp
zsharp = type.object
noticed object NewGame[] (
 shape: button:
 text: "New Game":
 width: 160:
 height: 32:
 color: #FFFFFF:
 hoverColor: #990000:
 event(
  left: "Menu:Menu:NewGame":
  right: "Menu:Menu:Options":
 )
)
```

Place the button in the scene's `objects[JSON]` like any other object.
`event(...)` is required, and must contain at least one nonempty `left` or
`right` callback. Both are optional individually. Targets use the existing
`File:Room:Function` window callback format (or `Project:File:Room:Function`).
Callbacks must take no arguments and execute as Z# tasks on button-down, not
by polling held-state. Hidden buttons do not receive clicks. Overlapping
buttons use the highest `layer`, with later placements winning ties.
`color` is the background; `hoverColor` is optional. The label is centered
with automatically contrasting text. Buttons are axis-aligned, respect size
and scale, and overlay the scene. In 2D they follow the scene camera; in 3D
their X/Y placement is screen-space (center = 0,0). Release mouse capture
before using menu buttons. Their press events remain available to scripts.

`input.mouse.left` and `input.mouse.right` are held-state statuses. A complete
click between script reads can leave both held-state reads `dead`.
Use `leftPressed`, `leftReleased`, `rightPressed`, and `rightReleased` for
reliable transitions. Each read consumes one pending transition of that kind
for the current script task and returns `alive`; when none remain it returns
`dead`. These are read-only statuses, not one-frame pulses: pending events
survive waits and render frames. Tasks consume independently, so one task
cannot steal another's clicks. Tracking starts when each task thread begins;
new tasks do not replay earlier clicks. Multiple queued clicks produce
multiple `alive` reads. Press/release queues are independent, not a combined
chronological event stream. Read once and save the result if multiple actions
need to use the same transition. Losing focus releases held mouse buttons.

```zsharp
loop (
 if[input.mouse.leftPressed == alive] (
  Print("Left button pressed"):
 ) else ()
 wait(16ms):
)
```

`input.mouse.deltaX` and `input.mouse.deltaY` contain the accumulated mouse
movement for the current frame. Capture the mouse for a normal 3D camera with
`input.mouse.captured.set: alive:` and release it with
`input.mouse.captured.set: dead:`. Capture is never enabled automatically.

Escape is only an input value (`input.key.escape`); it does not close a game
automatically. A game may assign it to pause, a side menu, or its own quit
logic.

Input and collision values are statuses (`alive` or `dead`). Positions,
velocities, dimensions, timing, and FPS are numbers. `Game.scene` and object
text are text values.

Use the normal property setter for literal updates:

```zsharp
Player.color.set: #00E5FF:
Player.velocityY.set: 500:
Player.visible.set: alive:
Game.scene.set: Gallery:
Main.background.set: #101820:
Player.texture.set: "Assets/Player2.png":
```

Calculated number and text changes use the normal typed assignment forms:

```zsharp
number.set:Player.positionX = Player.positionX + 5:
number.set:Player.rotation = Player.rotation + Game.delta * 90:
text.set.Player.text = "Score: " + Score:
number.set:Main.cameraX = Player.positionX:
number.set:Main.cameraRotationY = Main.cameraRotationY + input.mouse.deltaX:
number.set:Player.rotationZ = Player.rotationZ + Game.delta * 90:
```

For 3D objects, `positionX/Y/Z`, `velocityX/Y/Z`, `rotationX/Y/Z`, and
`scaleX/Y/Z` are all readable and writable. `Player.grounded` becomes alive
when a dynamic `COLLIDER3D` is supported from below; `Player.colliding`
indicates any current collider contact. Python, JavaScript, and Lua may help
with vector or camera calculations, but none is required for native 3D input,
rendering, or physics.

An object can be qualified with its scene as
`Main.Player.positionX`. Input and timing properties are read-only, as are
`grounded` and `colliding`. Scene changes, rendering, physics, and script loops
run concurrently, so `wait(...)` does not freeze the game window.

### Controlling a specific scene placement

In 1.2.1.0, `Scene.Instance.property` addresses one live placement, not its
shared `.zobject` definition. The scene prefix is the declared scene name,
not its folder path. Instance identifiers are case-sensitive.

```zsharp
objects[JSON] (
 [
  {"id": "Cube", "name": "ElevatorLeftDoor",
   "location": {"x": -1, "y": 0, "z": 0}},
  {"id": "Cube", "name": "Elevator Right Door",
   "location": {"x": 1, "y": 0, "z": 0},
   "instanceId": "ElevatorRightDoor"}
 ]
)
```

An identifier-style `name` is the default instance identifier. Use optional
`instanceId` after `location` when a hierarchy label contains spaces, or when
several placements share a display label. Projects using `instanceId` must
target `ZSharp: [1.2.1.0]:` or newer. Explicit IDs use at most 127 ASCII
letters, digits, or underscores, starting with a letter or underscore, and
must be unique within that scene. Names and IDs may repeat in different scenes.
An explicit ID replaces the display-name alias. Duplicate explicit IDs and
invalid IDs are rejected during packaging/loading. Repeated legacy display
labels are still allowed, but referencing an ambiguous label is an error.

```zsharp
number.set:Chapter1Floor1.ElevatorLeftDoor.positionX =
 Chapter1Floor1.ElevatorLeftDoor.positionX - (Game.delta * 2):
number.set:Chapter1Floor1.ElevatorRightDoor.positionX =
 Chapter1Floor1.ElevatorRightDoor.positionX + (Game.delta * 2):
Chapter1Floor1.ElevatorLeftDoor.visible.set: dead:
```

Readable/writable transforms are `positionX/Y/Z`, `rotationX/Y/Z`,
`scaleX/Y/Z`, and `velocityX/Y/Z`. Existing writable properties such as
`visible`, `width`, `height`, `depth`, `color`, `trigger`, and the supported
surface/physics/light fields use the same instance path. Collision flags
`grounded` and `colliding` remain read-only; this does not add a writable `body`
or `collider` field. Moving one Cube placement does not move the other Cubes.

Unqualified `Instance.property` addresses only the active scene. The old
object-definition name (`Player.positionX`, for example) remains an alias
when it uniquely identifies a placement; it is never an arbitrary first match.
A unique placement name/ID takes precedence over a definition-name alias.
Qualified paths address the named scene's own instance, including inactive
scene state, and do not follow an identically named instance into another scene.
Scene instances are indexed at load time; property access does not scan all
placements. Live reads and writes are synchronized with physics snapshots.

Position changes immediately affect the rendered transform and collider.
Kinematic velocity moves platforms normally; script-positioned static or
kinematic platforms also record movement for rider carrying. Grounded dynamic
bodies are carried horizontally and vertically by their supporting platform
where contact is maintained; jumping releases support. This is translation
carrying, not rotation/scale-driven rider attachment or swept collision for
large teleports. No scene rebuild/reload is necessary.

`Game.scene.set: "Chapter1Floor2":` keeps the existing scene-entry behavior:
positions return to their configured spawn locations and velocities reset.
Set arrival positions after switching scenes. Explicit transforms on an
inactive scene are not a substitute for configuring its spawn placements.
Scene switching itself still requires `import project_id.game.scene():`.

## Achievements and audio

Game achievements use ZSON in a `type.script:achievement` file:

```zsharp
zsharp = type.script:achievement

achievement CLIMBER[JSON] (
 [
  {
   "display": "Climber",
   "description": "Reach level 5!",
   "rarity": "HARD"
  }
 ]
)
```

Rarity must be `EASY`, `MEDIUM`, `HARD`, or `IMPOSSIBLE`. A normal game script
imports the official achievement API and awards the ID once its condition is
met:

```zsharp
import ZSharp.Achievements():
ZSharp.Achievement.Award.CLIMBER:
```

Awards are idempotent: awarding an earned ID again does nothing. New awards
are saved locally by project PID, retained across reinstall, counted in the
Z# Hub, and displayed in-game for five seconds with a short sound.

Reusable scene audio is defined in a `.zaudio` source file. The source points
to the real WAV asset; `.zaudio` does not contain encoded sound data:

```zsharp
zsharp = type.script:audio

noticed audio Bounce[] (
 source: "Assets/Audio/Bounce.wav":
 volume: 35:
 pitch: 100:
 loop: false:
)
```

Place it in a scene's existing `objects[JSON]` list by its audio ID:

```json
{
 "id": "Bounce",
 "name": "Ball Bounce",
 "location": {
  "x": "0",
  "y": "0"
 }
}
```

`volume` and `pitch` use 100 as their normal value. `loop` accepts `true` or
`false` (and the equivalent Z# statuses `alive` or `dead`). Audio remains idle
until a script in its active scene starts it with
`Bounce.audioPlay.set: alive:`. ZVM currently accepts WAV assets for `.zaudio`
sources.

Object-owned WAV audio and generated tones remain supported for compatibility.
For small effects that need no asset, use `tone:` (frequency in hertz) and
`toneDuration:` (seconds).

The complete playable example is in `examples/test-game`. Package both the
bytecoded and source forms with:

```text
zsharp package game "path/to/examples/test-game" ZSharpGameTest --unbytecode
```

## ZSS game styling

### Button states (1.2.1.0)

Game text and buttons accept a base `font-size` in pixels:

```css
.NewGame { font-size: 24px; }
```

Bare numbers are also pixels. Values must be greater than zero and at most
4096. The default is the existing 8-pixel bitmap font; increasing font size
scales that bitmap, rather than selecting a smooth/vector font. Button text
remains centered and clipped to the button area. Font-size state overrides
and font-size transitions are not implemented.

A quit button's callback can close the app/game with:

```zsharp
noticed brain Quit[] (
 Application.Quit:
)
```

No import is needed. This requires project ZSharp 1.2.1.0 or newer and a
running native app/game. It requests normal window closure, returns from the
current callback, cancels other tasks, and uses the usual `Shutdown[]` cleanup
path. It is not a force-kill operation.

Game buttons support `:hover` (pointer over the button) and `:focus`
(selected for keyboard activation):

```css
.NewGame:hover {
 color: #990000;
 opacity: 0.8;
 scale-x: 1.1;
 scale-y: 1.1;
}
.NewGame:focus {
 color: #3366FF;
 opacity: 1;
}
.NewGame {
 transition: all 150ms ease-in-out;
}
```

State declarations support `color`, `opacity`, `scale-x`, and `scale-y`.
The base button rule also accepts `background: transparent;` to hide only
the background while retaining the label and clickable area. A `#RRGGBB`
background restores a solid fill. Background transparency remains in effect
during hover/focus; use a solid background if those states should show a fill.
ZSS opacity is between 0 and 1; scene/runtime opacity uses its existing
0–100 range. Hover takes precedence when both states supply the same property.
These visual scale changes do not resize the physics collider or click area.
Tab/Shift+Tab cycles visible buttons; Enter/Space invokes the focused button's
left action. Clicking a button focuses it. Losing window focus clears selection;
mouse capture suppresses button focus/hover interaction.

Button transitions accept `all <duration> [timing]` or `none`. Durations use
`ms` or `s`; timing is `linear`, `ease`, `ease-in`, `ease-out`, or
`ease-in-out`. Transitions interpolate color, opacity, and visual scale.
Property lists, delays, and CSS keyframe animations are not implemented.


Z# Style Sheets use the `.zss` extension and CSS declaration syntax. They are
parsed directly by ZVM and do not require a browser. Use `.Object` when an
object name is enough, or `.File Object` to name the `.zobject` file too:

```css
.Player {
 color: #FF2A72;
 width: 48;
 height: 72;
}

.World Bouncer {
 color: #FFE66D;
 scale-x: 1.15;
 scale-y: 1.15;
 rotation: 8;
}
```

Kebab-case ZSS names such as `position-x`, `gravity-scale`, and
`audio-volume` map to the matching Z# fields (`positionX`, `gravityScale`, and
`audioVolume`). Rules are applied in sorted file order after `.zobject` files
load, so a later ZSS rule overrides an earlier object value. In 1.0.2.1 ZSS
styles the game fields supported by `.zobject`; web-only layout and browser DOM
properties do not apply to native game objects.

## Animation clips

`.zanimation` files are JSON containing named clips. A clip has `models`
(project-relative `.zobject`, `.zmodel`, or `.zai` references),
positive `fps`, `type` (`tween` or `static`), `loop`, and `parts` with ordered
keyframes. Each keyframe has `frame` and optional `posX/posY/posZ` and
`rotX/rotY/rotZ` values. These are offsets from the placed object's transform.
The optional keyframe `models` map uses one-based indexes into the clip's
`models` array, for example `"models": {"1": true, "2": false}`.

```zsharp
import my_project.GemBounce():
GemBounce.playClip.Pulse:
GemBounce.pauseClip.Pulse:
GemBounce.playClip.Pulse[2]: // resume or target the second instance
GemBounce.stopClip.Pulse: // restore the starting transform
```

Tracks named `root` or `part_1` animate the placed object's transform.
Other `parts` names can match FBX mesh-node names and animate those mesh
parts around their imported pivots. A track matching an FBX bone name instead
poses that bone and deforms its weighted vertices, including child-bone
hierarchy. This requires a skinned FBX and is tested with a multi-bone asset.
Clips are exclusive per placed instance: starting a different clip stops the
previous clip on that instance. Animation blending and direct playback of an
FBX's embedded animation stacks are not yet supported.

## Imported models and AI

An imported model uses a project-relative FBX source. Scene placements can
override its size, color, texture, rotation, and surface properties just as
they can for `.zobject` definitions. The packager validates and bundles the
source FBX; the renderer draws its triangulated meshes and UV textures.
Scene `width`, `height`, and `length` set the imported mesh's bounding size
on each axis, while the FBX origin remains its transform pivot. Runtime
`scaleX/Y/Z` multiply those dimensions.

```zsharp
zsharp = type.script:model
noticed model Suzanne[] (
 parent: "Models/Suzanne.fbx":
 collider: mesh:
 body: static:
 textures[JSON] (
  "BodyMaterial": "Textures/body.png":
 )
)
```

An AI definition uses an imported model and aliases for scene navigation
points:

```zsharp
zsharp = type.ai
noticed ai Patrol[] (
 model: "Models/Suzanne.zmodel":
 visible: alive:
 body: dynamic:
 collider: box:
 mass: 1:
 gravityScale: 0:
 Navigation[] (
  first: "patrolA":
  second: "patrolB":
 )
)
```

Place it by `"id": "Patrol"` in the scene. A built-in, invisible point is
`{"id": "nav:patrolA", "name": "A", "location": {"x": 0, "y": 0, "z": 0}}`.
Then a game script can use:

```zsharp
Patrol.toPoint.glide(first, 6):
Patrol.toPoint.teleport(second):
if[Patrol.navStatus == "notReachable"] (
 Print("No route"):
) else (
)
```

`navStatus` is read-only: `moving`, `reachable`, or `notReachable`. Glide
uses authored X/Z navigation points when available and falls back to a
bounded automatic grid route around static geometry. An AI can query line of sight using
`Patrol.canSee.Player`, which returns `alive` or `dead` based on blockers'
3D bounds. Model definitions may use `collider: sphere:` or
`collider: capsule:` for radial 3D contacts; the existing `box` and 2D
`circle` collider behavior is unchanged. A **static** `.zmodel` may use
`collider: mesh:` for triangle contacts with dynamic spheres or capsules;
moving mesh colliders and exact box-to-mesh contact are not supported.
Navigation does not yet use polygon navmeshes or rotated obstacle hulls. A
`.zmodel` or `.zai` may be referenced by a `.zanimation` clip, including
root, FBX mesh-node, and FBX bone tracks.

## Solid 3D cubes

`shape: cube` now renders solid faces instead of only an edge outline. Faces
are clipped against the camera near plane, ordered by depth, and shaded so
their orientation remains visible while preserving the object's chosen color.



## Scene appearance, surfaces, and lighting

Scene object placements may set per-instance XYZ rotation without changing
their `.zobject` definition:

```zsharp
"rotations": {"x": 10, "y": 20, "z": 30}
```

This uses the existing `rotationX`, `rotationY`, and `rotationZ` runtime
properties. Scene placements can also specify `surface` values from 0 to
100: `opacity`, `roughness`, `emissive`, and `metallic`. Color and texture
remain independent optional appearance overrides. A built-in `light` entry
can provide a point, spot, or directional light:

```zsharp
{"id": "light", "name": "Lamp", "location": {"x": 0, "y": 20, "z": 0},
 "rotations": {"x": -45, "y": 0, "z": 0},
 "attributes": {"light_type": "spot", "intensity": 100,
   "color": "#FFFFFF", "castShadows": true, "range": 80, "angle": 45}}
```

Use a unique identifier-style scene `name` such as `Flashlight` to address
one placed light from a script (the built-in `id` remains `light`):

```zsharp
number.set:Flashlight.positionX = Player.positionX:
number.set:Flashlight.positionY = Player.positionY + 8:
number.set:Flashlight.positionZ = Player.positionZ:
Flashlight.visible.set: alive:
number.set:Flashlight.lightIntensity = 240:
Flashlight.castShadows.set: alive:
```

With a placement `instanceId` of `Flashlight`, scene-qualified access is:

```zsharp
number Brightness = Chapter1Floor1.Flashlight.lightIntensity:
number.set:Chapter1Floor1.Flashlight.lightIntensity = 100:
number.set:Chapter1Floor1.Flashlight.rotationY = 45:
```

The script must import its project's game scene feature, for example
`import the_pallet.game.scene():`. JSON placement attributes use `intensity`,
`range`, and `angle`; runtime paths use `lightIntensity`, `lightRange`, and
`lightAngle`, not `intensity`/`range`/`angle`.

Lights also expose `rotationX/Y/Z`, `color`, `lightType` (`point`, `spot`, or
`directional`), `lightRange`, and `lightAngle` at runtime. Scene objects expose
`opacity`, `roughness`, `emissive`, and `metallic` as 0–100 number properties.

The current renderer applies per-face lighting with an ambient floor and
hard shadows from opaque cubes, including rotated cubes, and coarse bounding-volume shadows
from models. Opaque 3D triangles use a per-pixel depth buffer and
perspective-correct texture coordinates. Transparency ordering, exact
triangle-cast model shadows and transparency sorting remain approximate.
FBX models, skinned bone tracks, root/mesh-node tracks, and grid-backed AI
are available in this development build, subject to the limitations above.

---

# Languages

Z# can interoperate with several other languages. C is also the extension path for trusted modules that register new Z# statement, block, and expression syntax.

In Z# 1.2.1.0, C modules can register value-producing expressions:

```c
registry->add_expression(registry->context, "Discord.Client",
                         "client", error, error_size);
```

The module's `zsharp_c_call_v1` handler receives function `client` with no
arguments and returns a `ZSharpCValue`. Return `ZSHARP_C_STATUS` with
`number = 1` for `alive` or `number = 0` for `dead` to use it directly:

```javascript
import c:my_project.C.Console():
// Inside a brain, after importing the module that registers the expression:
if[Discord.Client] (
 Print("Client available"):
)
```

Expressions may also return numbers or text for assignments, arithmetic,
comparisons, and function arguments. For example, register
`NativeSum({a}, {b})` and use `number Total = NativeSum(10, 20) + 1:`.
Placeholders accept expressions, including nested calls, following the custom
statement rules. Expression patterns omit the statement-ending colon.
Registrations cannot replace built-in syntax or overlap another registered
pattern. Distinct patterns may share a namespace, including `Discord.Init(...)`,
`Discord.client`, and `Discord.client({name})`. Handlers execute at runtime on each evaluation;
registration itself must remain side-effect-free. Existing C modules remain
compatible; modules using the appended `add_expression` API require a
1.2.1.0-or-newer compiler and should declare that project version requirement.

## Python

Python imports always name the language and project ID. Import one module or
all Python modules in a project with:

```zsharp
import py:my_project.Python.Utilities():
import py:my_project.*():
```

Inside the same project, a call omits the project ID because the import has
already identified it:

```zsharp
text Greeting = Function.call(py:Python.Utilities:greet ["Z#"]):
number Total = Function.call(py:Python.Utilities:add [20, 22]):
```

The corresponding `Python/Utilities.py` file explicitly exports callable
functions:

```python
from zsharp import export

@export
def greet(name: str) -> str:
    return f"Hello, {name}!"

@export
def add(left: int, right: int) -> int:
    return left + right
```

Initial Python interoperability supports Z# text, number, status, and null
values, mapped to Python `str`, `int`/`float`, `bool`, and `None`. A Python
exception fails the Z# call and includes its Python traceback in the runtime
error. The official ZVM bundles Python; developers may set
`ZSHARP_PYTHON_RUNTIME` while testing a custom interpreter.

Python calls share one persistent interpreter process for the lifetime of the
ZVM process. Modules are cached by their resolved file path: globals and imported
modules survive subsequent calls, including calls from other Z# tasks. Calls
are serialized across tasks. Python remains available during `Shutdown[]`;
ZVM closes the worker at process exit, allowing up to two seconds for normal
Python cleanup before terminating a worker that remains alive. On Windows,
the worker is also owned by a kill-on-close job to prevent orphaned workers.
Restart the ZVM to reload edited Python modules. User Python output is forwarded
to standard error so it cannot corrupt the worker's result protocol.

## JavaScript

JavaScript is embedded in the ZVM; users do not need Node.js. Import a local
JavaScript module with its project PID and call exported functions through the
same value bridge used by other languages:

```javascript
import js:my_project.JavaScript.Utilities():

text Greeting = Function.call(js:JavaScript.Utilities:greeting ["Z#"]):
```

```javascript
export function greeting(name) {
  return `Hello, ${name}!`;
}
```

Arguments and return values support text, number, status/boolean, and null.
Homogeneous `text[]` and `number[]` values cross the bridge as JavaScript
arrays, and JavaScript may return arrays containing only strings or only
numbers. Empty JavaScript arrays become empty `text[]` values. Mixed arrays,
objects, and arrays of booleans are rejected because Z# does not have matching
general-purpose value types.
Named default exports and immediately-resolving `async` functions are also
supported. JavaScript exceptions retain their message and stack in Z# runtime
reports. Each call has a 64 MiB heap limit, a 1 MiB stack limit, and a five
second execution guard; promises that require an external browser/Node event
loop fail with a clear diagnostic because ZVM intentionally embeds no DOM or
Node APIs.

## Lua

Lua 5.5.1 is embedded in the ZVM, so users do not need a separate Lua
installation. Import a Lua module with the project PID, then call it with its
project-relative path:

```javascript
import lua:my_project.Lua.Utilities():

text Greeting = Function.call(lua:Lua.Utilities:greeting["Z#"]):
```

A Lua file can return a module table:

```lua
local utilities = {}

function utilities.greeting(name)
  return "Hello, " .. name .. "!"
end

return utilities
```

Global Lua functions are also supported when a file does not return a module
table. Arguments and results may be text, number, status/boolean, null/nil,
or homogeneous text and number arrays. Z# arrays use zero-based indexes while
Lua tables use their usual one-based indexes. Empty Lua arrays become empty
`text[]` values. Mixed tables and general Lua objects cannot cross the bridge.

Lua errors retain a Lua traceback in the Z# runtime failure report. Each call
has a 64 MiB memory limit and a five-second execution guard. Lua support
requires `ZSharp: [1.1.2.0]:` or newer.

## C++

C++ modules use the same project-qualified import and short same-project call
syntax as the other embedded-language bridges:

```zsharp
import cpp:my_project.Cpp.Utilities():

text Greeting = Function.call(cpp:Cpp.Utilities:greeting["Z#"]):
number Total = Function.call(cpp:Cpp.Utilities:add[20, 22]):
```

The import corresponds to `Cpp/Utilities.cpp`. Its compiled bridge must sit
beside it as `Cpp/Utilities.zcpp.dll` on Windows,
`Cpp/Utilities.zcpp.so` on Linux, or `Cpp/Utilities.zcpp.dylib` on macOS.
Applications distribute that compiled module inside their package, so users
do not need a C++ compiler or development tools.

Include the installed `zsharp_cpp.h` header and export one bridge entry:

```cpp
#include <cstring>
#include "zsharp_cpp.h"

extern "C" ZSHARP_CPP_EXPORT int zsharp_cpp_call_v1(
    uint32_t abi, const char *function,
    const ZSharpCppValue *arguments, size_t argument_count,
    ZSharpCppValue *result, char *error, size_t error_size) {
    if (abi != ZSHARP_CPP_ABI_VERSION) return 0;
    if (std::strcmp(function, "answer") == 0 && argument_count == 0) {
        result->type = ZSHARP_CPP_NUMBER;
        result->number = 42;
        return 1;
    }
    return 0;
}
```

Bridge arguments and results support text, number, status, and null. Returned
text is copied by ZVM immediately after the call. C++ modules are cached by
full library path and remain loaded until process exit, preserving global
state and SDK callbacks between calls. The cache is shared across script
tasks; loading is synchronized, but module functions must synchronize their
own shared state if called concurrently. Call an SDK's explicit shutdown
function when appropriate; ZVM does not invent or automatically invoke one.
Restart the app/game after rebuilding a module to load the changed library.
C++ calls require
`ZSharp: [1.1.3.0]:` or newer.

## Rust

Rust modules use project-qualified imports and short same-project calls:

```zsharp
import rust:my_project.Rust.Utilities():

text Greeting = Function.call(rust:Rust.Utilities:greeting["Z#"]):
number Total = Function.call(rust:Rust.Utilities:add[20, 22]):
```

The import corresponds to `Rust/Utilities.rs`. Build it as a Rust `cdylib`
with the bridge entry `zsharp_rust_call_v1`, then place the compiled library
beside the source as `Rust/Utilities.zrust.dll` on Windows,
`Rust/Utilities.zrust.so` on Linux, or `Rust/Utilities.zrust.dylib` on macOS.
Package the module for each supported platform; the user's device does not
need Rust or Cargo installed. Rust source alone cannot be called by the ZVM.
For example, from the project root on Windows (with `zsharp_ffi.rs` copied
beside the Rust source):

```powershell
rustc --edition 2021 --crate-type cdylib Rust/Utilities.rs -o Rust/Utilities.zrust.dll
```

The installed `share/zsharp/rust/zsharp_ffi.rs` file defines the bridge's
`#[repr(C)]` value layout and constants. A module exports this function:

```rust
#[no_mangle]
pub unsafe extern "C" fn zsharp_rust_call_v1(
    abi_version: u32,
    function: *const std::ffi::c_char,
    arguments: *const ZSharpRustValue,
    argument_count: usize,
    result: *mut ZSharpRustValue,
    error: *mut std::ffi::c_char,
    error_size: usize,
) -> i32 {
    // Return 1 for success, 0 after writing an error message for failure.
    0
}
```

Include the value definitions from `zsharp_ffi.rs` in the Rust module. The
module must check the ABI version before reading arguments. Arguments and
results support text, number, status, and null. Returned text must remain
valid until the ZVM copies it, immediately after the call. The full example
is in `tests/rust_project/Rust/Utilities.rs`. Rust calls require
`ZSharp: [1.1.4.0]:` or newer. As with C++, the native desktop bridge is not
the Android VM-executed module format planned for a later release.

## Kotlin/Native

Kotlin modules use `kt:` imports and same-project calls:

```zsharp
import kt:my_project.Kotlin.Rules():
number Speed = Function.call(kt:Kotlin.Rules:platformSpeed[8]):
```

The corresponding native library is `Kotlin/Rules.zkt.dll` on Windows
(`.zkt.so` on Linux or `.zkt.dylib` on macOS). It exports
`zsharp_kotlin_call_v1` with the `zsharp_c.h` value ABI. The ZVM retains the
library for the process lifetime because unloading a Kotlin/Native runtime
after a call can crash later. This is a native bridge, not a JVM requirement.
The current package workflow requires compiling each Kotlin library ahead of
time for its target platform; it does **not** compile `.kt` source automatically.

## C

C modules use project-qualified imports and same-project calls:

```zsharp
import c:my_project.C.Utilities():

number Total = Function.call(c:C.Utilities:add[20, 22]):
```

The import corresponds to `C/Utilities.c`. Compile it as a shared library
beside the source: `C/Utilities.zc.dll` on Windows,
`C/Utilities.zc.so` on Linux, or `C/Utilities.zc.dylib` on macOS. The
installed `zsharp_c.h` header defines the ABI and value types. A module
exports `zsharp_c_call_v1` and receives the function name, arguments,
result, and error buffer. Text, number, status, and null can cross the bridge.
This bridge is version-gated to `ZSharp: [1.2.0.0]:` or newer. C source
alone is not executable by the ZVM; distribute compiled modules for each
supported platform.

### Extending Z# with C

An imported C module may also export `zsharp_c_register_v1` to add statement
patterns. The compiler loads that compiled module while checking and
packaging source, so only use trusted C modules. For example:

```c
ZSHARP_C_EXPORT int zsharp_c_register_v1(
    ZSharpCSyntaxRegistry *registry, char *error, size_t error_size) {
    return registry->add_statement(registry->context,
        "move {who} to {where}", "move", error, error_size) &&
        registry->add_statement(registry->context,
        "Movement.move({who}, {where})", "move", error, error_size) &&
        registry->add_block(registry->context,
        "Detections", "detections", error, error_size);
}
```

Both statements call the module's `zsharp_c_call_v1` with two arguments:

```zsharp
move "Player" to "Spawn":
Movement.move("Player", "Spawn"):
Detections(
 Windows: true:
 Linux: false:
 MacOS: true:
)
```

Patterns use literal Z# tokens and `{name}` placeholders. Each placeholder
accepts an expression, including variables, qualified properties, arithmetic,
and nested calls such as `Discord.Bot.Login(File.read(".env")):`. Arguments
are evaluated at runtime, not during registration. Commas and closing
parentheses inside nested calls do not split the outer arguments. A literal
operator in a registered pattern remains a separator (use parentheses around
a complex operand in that uncommon form). Adjacent placeholders without a
separator keep their original single-atom interpretation. The first word of a
custom pattern cannot be a built-in Z# keyword. Distinct patterns may share
their first word; overlapping token patterns are rejected. Assignment and bracket syntax are
also reserved. A pattern is available only in a script that imports its C
module. Registration must have no side effects because the compiler may call
it repeatedly.
Registered blocks pass alternating field-name text and evaluated values to
the C handler. `true` and `false` work inside these blocks as status values;
normal Z# code still uses `alive` and `dead`.

## Browser prototype (1.2.2.0, development)

The local bZVM prototype executes Z# bytecode in WebAssembly. HTML selects one
entry `.zsharp` script, whose explicitly imported project files are loaded before
startup. The loader automatically finds `project.zsettings` in the HTML document's
directory or nearest parent directory, up to the server root (32 levels maximum).
That settings file's directory is the project root; `data-project` is not needed.
Its old explicit URL override remains supported for compatibility. If automatic
discovery receives only HTTP 404 responses, standalone scripts without imports
still work. Other HTTP errors or invalid settings stop startup.
Settings are parsed inside WebAssembly by the same settings
parser as the native ZVM, not by a separate JavaScript grammar. Use the normal
`Project`, `PID`, `Version`, `Authors`, `Description`, `ZSharp`, and `Dependencies`
fields. The prototype currently requires `Dependencies ():` and rejects native
window/game configuration and unsupported newer runtime versions. Malformed or
missing explicitly linked settings stop startup with a clear error. The
`bzvm:ready` event's `detail.project` contains the project name, ID and root URL
when settings are enabled.
Load `bzvm.js` with `defer` and include a `type="text/zsharp"` script tag.
Place `bzvm.wasm` beside the loader and serve the page over HTTP, not `file:`.

```html
<script src="bzvm/bzvm.js" defer></script>
<script type="text/zsharp" src="Main.zsharp"></script>
<input id="input">
<button id="button">Greet</button>
<div id="greeting"></div>
```

```zsharp
zsharp = type.script
noticed room Main[] (
 import ZSharp.Browser():
 noticed brain Start[] (
  Browser.button.clicked: (
   text Name = Browser.input.value:
   if[Name != ""] (
    Browser.greeting.content.set: "Hello, " + Name + "! Count: " + 10:
    wait(5s):
    Browser.greeting.content.set: "Five seconds later, " + Name:
   ) else (
    Browser.greeting.content.set: "Enter your name first.":
   )
  ):
 )
)
```

`if`, `else if`, `else` and text concatenation work in this subset. Joining text
with numbers/status values converts those fields to text. `.value` reads an
input's text; `.content` maps to `textContent`; `.html` maps to `innerHTML` and
preserves the outer element. Paths require a unique HTML ID that is also a Z#
identifier: use `bar_title`, not `bar-title`. Input `.contents` and `.length`
are not implemented. `.style.PROPERTY.set:` accepts CSS strings.

Click handlers may be inline blocks ending in `):`, or use
`Browser.button.clicked: Function.call(Main:Main:Greet):`. Registration does not
execute the body. Each click starts an independent task. Inline handlers have
fresh local variables, not captured locals from the registering function.

`wait(5s):`, `wait(16ms):` and `wait(0ms):` yield to browser timers and resume
the same task, preserving its locals and nested call stack. Other clicks remain
responsive. Wait durations are literal nonnegative values, limited to 2147483647
milliseconds; fractional milliseconds round up in the loader. Browser throttling
can delay timers, especially in background tabs. Loops should include a wait;
each uninterrupted task slice has a 100000-instruction safety limit. Leaving
the page cancels pending tasks.

The browser runtime supports typed function parameters, `feed` return values
(including calls suspended by waits), function-reference parameters, persistent
room variables, visibility checks, indexed text/number arrays, `Math` and
`Regex`. Arithmetic uses the desktop VM's decimal routines. Explicit project
imports and cross-file calls are supported as described below.

This is not yet full desktop language parity: object/class instance execution,
JSON loading, file operations, and additional browser events still need browser
runtime implementations. Native apps/games and foreign-language integrations
are outside the browser target. Unsupported instructions report an error rather
than silently executing with different semantics.

### Browser project imports and cross-file calls

Use the settings PID and explicit project-relative paths. Given PID `my_site`,
`import my_site.Scripts.Messages():` loads `Scripts/Messages.zsharp` from the
project root. URL/file capitalization must match the actual files. The browser
does not recursively search directories or support wildcard imports yet.

```zsharp
// Main.zsharp
zsharp = type.script
noticed room Main[] (
 import ZSharp.Browser():
 import my_site.Scripts.Messages():
 noticed brain Start[] (
  Function.call(Messages:Messages:Show):
  Browser.button.clicked: Function.call(Messages:Messages:Show):
 )
)
```

```zsharp
// Scripts/Messages.zsharp
zsharp = type.script
noticed room Messages[] (
 import ZSharp.Browser():
 noticed brain Show[] (
  Browser.greeting.content.set: "Hello from another file":
  wait(1s):
  Browser.greeting.content.set: "The same task resumed":
 )
)
```

The call target uses `File:Room:Brain`; a same-project PID prefix is also accepted
by normal `Function.call`. The calling room must import a foreign file, and the
target room/brain must be `noticed`. Script filenames must be unique across the
loaded project. Each file is loaded once; circular imports are supported.
Only the HTML entry script's automatic `Start[]` brains run, not imported files'
startup brains. Calls and click targets can cross files and preserve waits.
The first subset supports zero-argument, void brains only (no arguments or return
values yet), at most 64 files and 8 MiB total source. Missing/duplicate files,
missing imports, hidden targets and external project dependencies produce errors.

Raw multiline strings use `'''...'''`. Embedded browser-only blocks support
`JavaScript[( ... )]:` and `CSS[ClassName, ( ... )]:`. CSS bodies contain
declarations applied to `.ClassName`; they do not create elements. Embedded
JavaScript executes with normal page privileges and may require CSP permission.
