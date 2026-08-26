# Changelog

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
