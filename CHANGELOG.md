# Changelog

## 1.0.0.1 - First Z1 release - 2026-08-23

- Added the C17 Z# compiler and bytecode virtual machine.
- Added `.zsharp` source parsing and `project.zsettings` validation.
- Added rooms, visibility, horde members and rooms, functions, constructors,
  objects, arrays, statuses, conditions, loops, and `Function.call`.
- Added arbitrary-size ordinary decimal arithmetic and Z# repeating-division
  behavior.
- Added named brain outcomes with dynamically typed `feed(...)` results.
- Added room-scoped imports, recursive project file discovery, and native
  provider integration for non-Z# projects.
- Added permanent PID-derived project identities and per-build SHA-256 bytecode
  integrity checks.
- Added the self-contained `com.zombieos:zsharp:1.0.0.1` Java integration
  library for Maven and Gradle workspaces. Runtimes for Windows x64/ARM64,
  Linux x64/ARM64, and macOS Intel/Apple Silicon are embedded,
  integrity-checked, and extracted automatically when no explicit toolchain is
  configured.
