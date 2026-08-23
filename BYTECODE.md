# Z# bytecode identity and integrity

Z# 1.0.0.1 automatically embeds two SHA-256 values in each compiled bytecode
file. Neither value is written in `project.zsettings`.

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

`zsharp run-bytecode` checks the project identity and build hash before parsing
or running the program. It also checks that the bytecode PID matches the
current project's `project.zsettings`.

These checks detect corrupt or changed bytecode. They do not prove who
published it, because someone who rewrites a file could also calculate a new
hash. Trusted publisher authentication will require the official registry to
sign project identities and released builds.
