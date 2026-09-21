# Changelog

## 1.1.2.0 - The Lua Update - 2026-09-21

- Added embedded Lua 5.5.1 interoperability without requiring a separate Lua
  installation, with project-scoped `lua:` imports and Z# function calls.
- Added text, number, status, null, `text[]`, and `number[]` value conversion
  between Z# and Lua.
- Added guarded Lua memory and execution limits plus Lua tracebacks in native
  Z# failure reports.
- Changed native 3D cubes from hollow edge outlines to clipped, depth-ordered
  solid faces with simple face shading.
- Completed native 3D camera pitch/yaw/roll, runtime mouse delta and capture,
  XYZ object rotation, camera-space depth ordering, `COLLIDER3D` grounding,
  and kinematic moving-platform support while preserving 2D behavior.
- Added safe project-relative text file reading, writing, appending, and
  existence checks.
- Added regression coverage for Lua imports, calls, errors, version gating,
  and solid cube projection.
- Expanded the shared Z# Language Test app to test Python, JavaScript, and Lua
  from one window.

## 1.1.1.0 - The JavaScript Update - 2026-09-18

- Added embedded JavaScript interoperability without requiring Node.js,
  including synchronous functions, immediately-resolving async functions,
  Unicode values, text/number arrays, guarded memory/stack use, and detailed
  JS stack traces.
- Fixed persistent room-level text and number state.
- Fixed UTF-8 rendering and dynamic text wrapping, height, and scrolling.
- Added normal-text alignment plus programmatic input clearing and focus.
- Fixed runtime text relayout and alignment consistency across Windows, Linux,
  and macOS when chat-style content changes.
- Expanded native Z# failure reports with call-chain context.
- Added native dropdown/select controls with readable and mutable selections.
- Added simple horizontal and vertical UI anchors for responsive layouts.
- Fixed native 3D cubes disappearing into a black screen, including cubes
  crossing the camera near plane and the documented default cube size.
- Added 3D regression coverage for XYZ transforms, camera position/FOV,
  Z-axis properties, dynamic/static bodies, and `COLLIDER3D` projects.

## 1.1.0.1 - Window text setter fix - 2026-09-15

- Window text properties now accept text variables directly, such as
  `Startup.ChatOutput.content.set: Response:`.
- Bytecoded apps now keep the underlying Z# launch error in Hub messages and
  runtime failure logs instead of reporting only a generic launch failure.

## 1.1.0.0 - Python interoperability - 2026-09-14

- Added Python source files to Z# projects with project-scoped `py:` imports.
- Added calls from Z# into explicitly exported Python functions, including
  text, number, status, and null values.
- Added a lazily loaded bundled Python runtime, so users do not need to install
  Python separately and idle Z# apps do not carry Python's memory cost.
- Existing installations updated by a pre-1.1 updater automatically download
  and verify the missing Python component the first time Python is used.
- Added Python exceptions and tracebacks to Z# runtime errors.
- Added a shared language-interoperability test app, beginning with Python.

## 1.0.2.5 - Text input and ZSS patch - 2026-09-09

- Added direct `fontSize`, `maxLength`, `textAlign`, and `textTransform`
  properties for text inputs.
- Added ZSS `text-align`, `text-transform`, `max-length`, `width`, `height`,
  and `background-color` declarations.
- Text inputs can force uppercase or lowercase characters as the user types.
- Added case-insensitive `allowedCharacters` lists that reject unlisted typed
  or pasted characters.

## 1.0.2.4 - JSON patch - 2026-09-09

- Apps and games may target future Z# versions, while launch-time checks keep
  them from opening on an older installed runtime.
- Added typed custom JSON schema declarations with text, number, and status
  fields, including JSON keys containing hyphens.
- Added project-relative `JSON.load` values for local and room variables with
  schema validation and normal Z# visibility rules.
- Added inline PNG images to Windows text controls with the
  `<img:path/to/image.png>` marker.
- Reduced native-window flicker caused by frequently updated controls.

## 1.0.2.3 - Developer experience patch - 2026-09-09

- Added calculated live-window property setters and mixed text, number, and
  status concatenation.
- Added built-in random integers, decimals, and percentage chances for games.
- Added direct runtime texture swapping for game objects.
- Added output-only terminal attachment for running installed apps and games.
- Made typed assignments update local variables when a matching local exists.
- Runtime task failures now close the affected window or game and report the
  actual error instead of appearing to freeze silently.

## 1.0.2.2 - Audio and Windows ARM64 updater patch - 2026-09-08

- Fixed Windows ARM64 installers being treated as administrator-only programs,
  which prevented the Z# Hub from starting its updater.
- Hub updater launch failures now include the underlying Windows error code.
- Added scene-owned `.zaudio` sources with WAV playback, volume, pitch, looping,
  and script-controlled playback.
- Packages requiring a newer Z# runtime now show the minimum required version
  instead of failing later on unfamiliar syntax.

## 1.0.2.1 - Game structure and achievements - 2026-09-07

- Reworked games around separate `.zscene`, `.zobject`, and `.zsharp` files,
  with automatic 2D/3D detection and named `input.key.*` controls.
- Added per-scene startup selection, splash screens, project/scene icons, and
  explicit script-owned movement and Escape-key behavior.
- Added persistent achievements with Hub totals and in-game notifications.
- Added Hub cleanup for missing packages, a package-only file picker, and
  settings for automatic, beta, and automatic-beta updates.
- Expanded the test game to a menu and five levels, including moving platforms.
- Known issue: unsigned Windows builds may still show publisher or Defender
  warnings while the Microsoft review and signing work is completed.

## 1.0.2.0 - Gaming and styling update - 2026-09-03

- Added playable 2D and experimental 3D games using SDL3 and Vulkan, with
  physics, collision, input, audio, scenes, objects, and `.zgame` packaging.
- Added Z# Style Sheets (`.zss`) for styling apps and games.
- Introduced the graphical Z# Hub with installed-project management, project
  details, icons, playtime, and last-played information.
- Added the automatic updater, update tray, `zsharp update`, Hub shortcut, and
  `zsharp publish` release-preparation command.
- Added Windows and Linux game support; the MoltenVK macOS path remains
  experimental until tested on Mac hardware.

## 1.0.1.2 - Windows update tray patch - 2026-08-29

- Added the Windows startup/tray update agent with hourly checks and update
  notifications.
- Prevented the updater from downgrading an installed ZVM.
- Preserved registered projects and application data during updates.

## 1.0.1.1 - Window input and scrolling patch - 2026-08-29

- Added multiline text inputs, wrapping controls, cursor/line/character reads,
  and scrollbars that appear only when needed.
- Fixed text flickering over animated backgrounds.
- Made apps and Desktop shortcuts launch without opening a terminal.
- Added backward-compatible runtime fixes for older Z1 applications.

## 1.0.1.0 - Window applications - 2026-08-25

- Added cross-platform window scripts with designs, text, buttons, images,
  inputs, callbacks, responsive scaling, gradients, and live property changes.
- Added wildcard imports, concurrent script tasks, and `wait`/`delay`.
- Added project registration plus `.zapp`/`.zgame` packaging, source packages,
  checksums, file associations, installation, launching, and uninstallation.
- Added the cross-platform bootstrap installer, update manifest, test app, and
  initial Z# website.

## 1.0.0.1 - First Z1 release - 2026-08-23

- Added the C17 Z# compiler, bytecode format, and ZVM.
- Added the core language syntax, project settings, imports, decimal behavior,
  bytecode integrity checks, and native-provider integration.
- Added the self-contained `com.zombieos:zsharp:1.0.0.1` Java integration
  library with embedded desktop runtimes.
