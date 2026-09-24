# Z# Android port test

This is an installable **development test**, not a playable Z# runtime or a
public release. It contains the Android-built portable language core and native
VM, runs a fixed Z# VM smoke test, offers a short SDL display test, and checks
the header, paths, and SHA-256 digest
of every file in a bytecoded `.zapp` or `.zgame` selected with Android's
document picker. It does not extract or run packages, render games, or execute
Python, JavaScript, Lua, C++, or Rust. Source ZIP packages are not supported.

Run `build.ps1` on Windows with the Android SDK installed at `E:\Android\Sdk`.
The output is `E:\Android\build\zsharp-port-test\ZSharp-Android-Port-Test.apk`.
It uses a test-only app ID and debug signing key; do not upload it to Google Play
or present it as a playable build. The test key is retained under the build
directory so reinstalling a later test APK can update the same installation.
