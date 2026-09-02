# Z# bytecode identity and integrity

Z# 1.0.2.0 automatically embeds two SHA-256 values in each compiled bytecode
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
