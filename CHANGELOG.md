# Changelog

## 1.0.2.0 - Playable Vulkan game runtime - 2026-09-03

- Added the official `zsharpgame:1.0.0.0` dependency gate for
  `type.script:2D` and `type.script:3D` files.
- Added an SDL3 game runtime with keyboard, mouse, gamepad, audio, high-DPI
  sizing, timing, cancellation, and concurrent automatic `Start[]` tasks.
- Added Vulkan rendering for resizable 2D scenes, colored rectangles, circles,
  triangles, text, BMP sprites, and the first perspective 3D cube primitive.
- Added `.zobject` files with scene backgrounds, cameras, transforms, layers,
  visibility, static/dynamic/kinematic bodies, gravity, velocity, box/circle
  collision state, triggers, friction, restitution, controls, and jumping.
- Added WAV playback, looping/autoplay/collision triggers, runtime audio play,
  volume, and asset-free generated tones for small effects.
- Added runtime paths for `Input.*`, `Game.*`, and object property reads and
  `.set:` updates, including live scene switching while scripts and rendering
  run concurrently.
- Added native `.zss` style sheets with `.Object` and `.File Object` selectors,
  CSS declarations, comments, cascading file order, and kebab-case aliases for
  game-object fields.
- Added native window ZSS loading with `.Element` and `.Window Element`
  selectors, button `:hover`, text-input `:focus`, colors, backgrounds, fonts,
  borders, radius, padding, caret/selection declarations, and compile-time
  validation. Window styles work for source and bytecoded `.zapp` launches.
- Kept the integrity-pinned MoltenVK candidate in macOS builds, while marking
  macOS games experimental and advertising Windows/Linux game support until
  real Mac hardware testing is completed.
- Changed `.zgame` packaging from a placeholder into an executable game
  package. Games select a 3D startup script when present, otherwise a 2D
  startup script, and bytecoded and unbytecoded packages use the game runtime.
- Added `.zobject` validation to both bytecoded and unbytecoded game packaging.
- Added a playable `examples/test-game` project and dependency, packaging,
  source/bytecode launch, automatic `Start[]`, object parsing, and live Vulkan
  smoke coverage.
- Replaced the placeholder Hub alert with a graphical installed app/game list,
  package launching, path-based package addition, and non-destructive removal.
  Packages are remembered automatically when opened.
- Added optional `Icon` project metadata, official Z# logo fallback, clickable
  Hub project cards, project and required-Z# version details, accumulated
  playtime, last-played timestamps, and a clearly labeled **Coming soon**
  achievements section. Project icons now carry through to Z#-created Desktop
  shortcuts as well.
- Removed stray button-border corner marks, fixed Add Package's internal
  window path, and made Hub/update launches stay out of a second terminal.
- Added updater result notifications for both an already-current ZVM and a
  successfully installed newer version.
- Added a cross-platform `Z# Hub` desktop launcher that is created on install,
  refreshed on update, and repairable with `zsharp hub shortcut`.
- Added `zsharp update` for an immediate verified update check through the
  bundled platform installer.
- Added `zsharp publish [repository]` to build and test local release files in
  `Downloads/ZSharp Publishing/VERSION` without pushing or uploading anything.

## 1.0.1.2 - Windows update tray patch - 2026-08-29

- Added a single-instance Windows ZVM update agent that starts for the current
  user at sign-in, checks immediately, and checks again every hour.
- Added tray actions to check manually or exit the agent for the current
  sign-in session.
- Added a Windows notification before a newer verified ZVM release begins
  downloading and installing.
- Restarted the tray automatically after a successful ZVM replacement while
  keeping registered apps, games, package caches, and application data intact.
- Changed four-part update comparison to install only genuinely newer
  releases. Matching versions do nothing, and an older web manifest can no
  longer downgrade a newer runtime.
- Retained the quiet standalone-launch update check for Linux and macOS, where
  the Windows tray integration does not apply.
- Added installer regression coverage for downgrade prevention.
- Rebuilt the embedded Windows, Linux, and macOS runtimes for version 1.0.1.2.

## 1.0.1.1 - Window input and scrolling patch - 2026-08-29

- Added multiline text inputs with `multiline: alive:`.
- Added live full-content, character-count, line-count, current-line, and
  current-column reads for text inputs. Line and column positions are 1-based.
- Added optional line wrapping with `wrap: alive:` or horizontal scrolling with
  `wrap: dead:`.
- Made native window scrollbars remain hidden until the window content actually
  extends beyond the visible height.
- Added compiler validation that limits `multiline` and `wrap` to text inputs
  and requires multiline mode before `wrap` can be configured.
- Added composited, double-buffered Windows background painting so rapidly
  changing gradients no longer erase and flash text controls between frames.
- Changed file associations and Z#-created Desktop shortcuts to launch apps
  without opening a terminal. Explicit terminal commands continue using the
  current terminal normally, and existing shortcuts migrate on their next app
  launch.
- Rebuilt the embedded Windows, Linux, and macOS runtimes for version 1.0.1.1.
- Confirmed backward runtime compatibility: older Z1 applications receive
  1.0.1.1 client/runtime fixes without repackaging, while new source-facing
  features still have to be added to the project code.

## 1.0.1.0 - Specialized script syntax - 2026-08-25

- Added `type.script:window`, `type.script:2D`, and `type.script:3D` headers.
- Added compiler and bytecode support for one-window-per-file UI scripts.
- Added design, text, button, image, and text-input elements with strict field
  validation.
- Added `zu` and `px` measurements, centered coordinates, `#RRGGBB` colors,
  text variants, and left/right button callback references.
- Added the `Window` project settings section with safe project-relative
  Startup and Uninstall paths and a required `zsharpwindow` dependency.
- Added native Windows rendering for design, text, buttons, PNG/JPG/BMP/GIF
  images, icons, text input, image selection, left/right callbacks, display
  scaling, and centered resize layout.
- Added native Linux GTK 3 and macOS AppKit renderers for the same window model,
  including inputs, images, callbacks, scaling, and centered resize layout.
- Added responsive window content on all three desktop platforms: width changes
  scale images, buttons, and inputs while text wraps, and height reductions use
  vertical scrolling instead of shrinking the layout.
- Window applications now automatically run every normal project script's
  non-`DR` `Start[]` after the startup window is created. Window-originated
  function calls remain limited to configured button click events.
- Added multitasking for window application startup brains and internal button
  callbacks. Independent endless loops can run together without blocking the
  native event loop, and closing the window cancels and joins all tasks.
- Window callbacks now preserve primitive room state between button presses and
  serialize work from the same room while unrelated rooms remain concurrent.
- Windows text elements now paint transparently so solid and gradient window
  backgrounds remain visible behind their text.
- Added live callback mutation through
  `File.Element.property.set: value:` for window design fields and visual
  element content, colors, files, placeholders, sizes, and positions.
- Added text-based window-property path aliases such as
  `noticed text Background = "Main.Design.background":` followed by
  `Background.set: value:`.
- Added multi-stop `linear-gradient(...)` and `radial-gradient(...)` design
  backgrounds with native Windows, GTK 3, and AppKit rendering.
- Added `wait(NUMBERms/s):` and its `delay(...)` alias, including event-pumping
  waits so UI changes redraw during callback animations.
- Added final-segment wildcard imports for every project and namespace,
  including `import Project.*():`, `import ZSharp.*():`, and
  `import ZSharp.Window.*():`.
- Added automatic upward `project.zsettings` discovery and persistent,
  per-user `zsharp project <path>` registration without launching the app.
- Changed package creation to
  `zsharp package <app|game> <project> <filename>`. Z# now adds the extension,
  writes the result to the project's `Packages` folder, and prints its full
  path. `zsharp run` now accepts `.zapp` and `.zgame` packages.
- Added the optional `--unbytecode` package output. It creates both the normal
  bytecoded package and a `-unbytecoded` ZIP-compatible source package that can
  be renamed to `.zip` to inspect the original project tree. The ZVM verifies
  and runs either form.
- Added validated, SHA-256-protected `.zapp` and `.zgame` package creation,
  extraction, direct opening, and confirmed uninstall commands on Windows,
  Linux, and macOS.
- Added current-user `.zapp`/`.zgame` file associations that launch through a
  terminal, first-install Desktop shortcut prompts with app icons, and shortcut
  cleanup during uninstall.
- Added Z# Hub routing for unavailable `.zgame` packages and failed `.zapp`
  launches, including the exact preserved launch-failure reason. The `.zgame`
  container is ready; game execution and APIs are deferred.
- Added checksum-verifying ZVM bootstrap installers for Windows, Linux, and
  macOS x64/ARM64, plus a generated GitHub Pages-compatible static `update.js`
  download-site manifest. Existing
  runtimes are backed up before replacement, Windows receives a user `PATH`
  entry, and package associations are registered after installation. Standalone
  ZVM launches now perform a locked background version check and update from
  the verified cross-platform `assets/download/ZVM-LATEST.zip` archive without
  changing registered apps or games.
- Interactive installations now offer the official Z# Test App after success.
  The release website payload includes that `.zapp` beside the verified ZVM
  archives and platform installers.
- Added the official three-page Z# website with an interactive home page,
  versioned syntax guide, and automatic Windows/Linux/macOS installer selection.
- Rebuilt the embedded Windows, Linux, and macOS runtimes for version 1.0.1.0.

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
