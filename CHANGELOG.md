# Changelog

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
