# Android player port (not yet release-ready)

The Android Z# app is a player, not a replacement for the desktop editor. It
must launch `.zapp` and `.zgame` packages without requiring an Android SDK,
compiler, Python installation, or terminal on the user's device. The first
test distribution may be a sideloaded APK; a Play release uses an AAB. Neither
artifact is ready until the checks below pass on a physical Android device.

## Runtime boundary

The current `zsharp` target in `CMakeLists.txt` is a desktop executable. The
Android app needs a JNI/SDL Android entry point and a native library packaged
inside its APK. Do not add `android` to the desktop build matrix and rename an
executable artifact to `.apk`.

- Move the reusable compiler, bytecode VM, package reader, and game model into
  a library target shared by desktop and Android.
  The first portion, `zsharp_language_core`, now contains the bytecode,
  decimal, hash, lexer, and parser sources and is still used by the desktop
  executable. CI can cross-compile it for Android with `ZSHARP_CORE_ONLY=ON`.
  The Android port-test APK also bundles an `arm64-v8a` shared library built
  from the current VM, QuickJS, Lua, package reader, and project sources; a
  fixed Z# script is called through JNI as a smoke test. This is a temporary
  duplicated source list, not the final shared desktop/Android runtime target.
  The current test APK includes the SDL game backend and a three-second
  renderer smoke-test activity. It does not yet connect a selected package to
  `zsharp_game_run`; native app windows are also not supported.
- Keep desktop CLI, installer, updater, shell integration, and native window
  implementations out of the Android library. Android is `__linux__` at the
  compiler level, so Linux/GTK branches must not be selected accidentally.
- Give the Android app its own lifecycle, touch input, file picker, internal
  package storage, crash-report directory, and SDL rendering surface.
- The app's bundled native libraries and interpreter versions are updated by
  installing a new APK (or through Play after publication), not by downloading
  executable replacement libraries from a ZIP.

## Language parity

- Z# bytecode, JavaScript (QuickJS), and Lua are already in the C runtime, but
  still need Android builds and end-to-end launch tests.
- Python currently starts a separate interpreter process via `vm.c`. Android
  needs an embedded Python runtime and a bridge that does not depend on
  spawning the desktop Python executable.
- Desktop C++ and Rust modules load project-specific native libraries with
  `dlopen`/`LoadLibrary`. Android-downloadable game modules must instead use a
  VM-executed format, with a stable bridge for text, number, status, null,
  errors, and memory ownership. Keep existing desktop native modules working.
  Source portability is not guaranteed when a module uses desktop OS APIs.
- The packager must report unsupported Android modules before publication and
  mark Android compatibility explicitly. The player must refuse unsupported
  packages with a useful error rather than crash or silently omit features.

## Required release checks

1. CI builds an installable `arm64-v8a` APK and Android App Bundle with all
   native dependencies inside the app; the sideloaded APK is release-signed.
2. A physical Android device launches Z#-only, Python, JavaScript, Lua, C++,
   and Rust package fixtures, including one mixed-language project.
3. Both a 2D app window and a 3D game render, accept touch input, and close
   cleanly. Test background/foreground, rotation, and low-memory recovery.
4. Invalid, outdated, truncated, and maliciously formed packages fail safely.
   Crash reports go to app-private storage without collecting unrelated files.
5. The APK package name and signing key are fixed before sharing a build meant
   to update seamlessly from a later Play installation. Test-only debug APKs
   are not presented as public releases.

Until these checks pass, Android is a port in progress, not a supported target
in `SupportedDevices` or the public download list.

The current installable test is built with `android/port-test/build.ps1` and
placed at `E:\Android\build\zsharp-port-test\ZSharp-Android-Port-Test.apk`.
It verifies bytecoded package headers and all entry SHA-256 hashes, but does
not install or launch packages. Its ID/signing key are test-only. The native VM
compiles and links for Android; no Android device launch has been verified yet.

The last verified VM-only APK is preserved as
`E:\Android\build\zsharp-port-test\ZSharp-Android-VM-Test.apk`. The newer
`ZSharp-Android-Port-Test.apk` builds and verifies with the SDL display-test
activity and all required native libraries, but it has not been launched on
a physical Android device. It is not a playable build. During the Android
build, Windows expanded `C:\pagefile.sys`; check free space on C: before
running another heavy build. Do not delete the pagefile while Windows is
running.
