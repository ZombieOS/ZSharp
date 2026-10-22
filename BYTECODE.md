# Z# bytecode identity and integrity

Format `0.26` adds `ZOP_SYSTEM` (73), carrying an operation-name string and
a u32 argument count. Arguments are evaluated and pushed in source order;
the opcode consumes them and, for property reads, pushes the result type
defined by that operation. Operations cover native Speech, Microphone, Speaker
and Desktop APIs, require the corresponding explicit room import and project
Z# 1.4.0.0, and are not supported by bZVM. Readers retain support for `0.25`
and previously supported formats.

Format `0.25` adds `ZOP_STORE_LOCAL_STATUS` (72), carrying the local variable's
name string. It consumes a status stack value and stores a typed local status;
later assignments must preserve that type. This supports initialized local
`status` declarations using the existing source syntax. Readers retain support
for `0.24` and previously supported formats.
Projects using this typed local instruction require Z# 1.4.0.0 or newer.

Format `0.24` adds `ZOP_APPLICATION_QUIT_MODE` (71), with no instruction
payload. It consumes one text stack value, exactly `regular` or `tray`.
New source requires `ZSharp.Application` in the using room and Z# 1.4.0.0.
The older operandless `ZOP_APPLICATION_QUIT` (60) keeps its regular-close
semantics when older bytecode is loaded directly. Readers retain support for
`0.23` and previously supported formats. This does not by itself preserve an
older package containing bare `Application.Quit:` source: packaged startup,
callback and shutdown discovery can also parse the bundled source. Such
packages currently need source migration and rebuilding for this change.

Format `0.23` adds `ZOP_NETWORK` (70), carrying an operation string, a u32
expression-result flag and a u32 argument count. Arguments are stack values;
HTTP request fields are alternating names/values, and callbacks are target text.
Readers retain support for `0.22`.

Format `0.22` adds `ZOP_TIME` (69), carrying a u32 method selector and u32
argument count. Arguments are pushed in source order. Selectors 1..12 are
since, until, convert, getYear, getMonth, getDay, getWeek, getHour, getMinute,
getSecond, getMS and getHour().formatted. Readers retain support for `0.21`
and previously supported formats. Time requires Z# 1.2.3.0.

Format `0.21` adds `ZOP_REGEX` (65), carrying a u32 operation selector
(1=test, 2=matches, 3=replace) and u32 argument count. Arguments are pushed
in source order; an optional final text argument supplies flags. The reader
retains support for `0.20` and previously supported older formats.

Format `0.20` adds `ZOP_FILE_SEARCH_EXTENSION` (63, no payload) and
`ZOP_STORE_LOCAL_TEXT_ARRAY` (64, string local name). Readers retain support
for format `0.19` and previously supported older formats.

Format `0.19` adds the `function` value type (9), `ZOP_PUSH_FUNCTION` (61),
and `ZOP_STORE_LOCAL_FUNCTION` (62). References store a validated target name,
not a process-specific pointer. Variable-based calls reuse qualified-call
opcodes with the `@callback` marker: arguments are pushed first, followed by
the callback value, then the call instruction resolves the target and checks
the caller's imports/visibility. The reader continues to accept format `0.18`
as well as its previously accepted legacy formats.

Z# 1.0.2.1 automatically embeds two SHA-256 values in each compiled bytecode
file. Neither value is written in `project.zsettings`.

Bytecode format `0.17` adds text-based aliases for live window-property paths.
For example, a text value containing `Main.Design.background` can be used with
`Background.set: #FFFFFF:`. Format `0.16` added direct live window-property and
wait/delay instructions.
Text-input reads use the existing `ZOP_LOAD_PATH` instruction. While a window
callback is active, paths ending in `contents`, `totalcharacters`,
`currentcolumn`, `totallines`, or `currentline` are resolved against the live
native text input; no bytecode-format bump is needed.
It also records the script kind and the complete validated
window model, including imports, UI elements, properties, units, and callback
references. The reader remains compatible with ordinary `0.14`, window
`0.15`, and live-window `0.16` bytecode from earlier builds. The Windows
`zsharpwindow` backend renders
the stored model; Linux and macOS render the same model through their GTK 3 and
AppKit backends.

Normal `.zapp` packages embed an integrity-checked bytecode for the configured
startup window and launch it through the ZVM. A package built with
`--unbytecode` also has a `-unbytecoded.zapp` source companion; that companion
is ZIP-compatible and intentionally runs its validated `.zsharp` startup.

## Project identity

The project identity is stable for a normalized PID. The compiler calculates:

```text
SHA-256("zsharp.project.identity.v1:" + PID)
```

Changing ordinary source code or the project version does not change this
identity. Changing the PID creates a different identity. A future official
registry will reserve each PID for exactly one project owner.

## Build integrity

The build hash covers the complete bytecode file except for the 32-byte field
that stores the hash itself. Recompiling identical input produces the same
hash; changing compiled content produces a different hash.

`zsharp check-bytecode` and `zsharp run-bytecode` check the project identity
and build hash before parsing
or running the program. It also checks that the bytecode PID matches the
current project's `project.zsettings`.

These checks detect corrupt or changed bytecode. They do not prove who
published it, because someone who rewrites a file could also calculate a new
hash. Trusted publisher authentication will require the official registry to
sign project identities and released builds.
