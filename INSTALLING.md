# Installing the Z# Virtual Machine

Z# 1.0.1.1 provides small bootstrap installers for Windows, Linux, and macOS.
The installer itself does not contain the ZVM. It asks the official update
endpoint for the newest runtime for the current operating system and CPU,
downloads `assets/download/ZVM-LATEST.zip` over HTTPS, verifies the archive,
extracts only the matching runtime, and verifies that runtime's exact byte size
and SHA-256 before installing it.

## Download to give users

Most Windows computers use:

```text
zsharp-installer-1.0.1.1-windows-x86_64.exe
```

Other available downloads are:

```text
zsharp-installer-1.0.1.1-windows-aarch64.exe
zsharp-installer-1.0.1.1-linux-x86_64
zsharp-installer-1.0.1.1-linux-aarch64
zsharp-installer-1.0.1.1-macos-x86_64
zsharp-installer-1.0.1.1-macos-aarch64
```

Windows users can double-click the matching `.exe`. Linux and macOS users run:

```text
chmod +x zsharp-installer-1.0.1.1-PLATFORM
./zsharp-installer-1.0.1.1-PLATFORM
```

Linux requires `curl` for HTTPS downloads. macOS includes the required command
line downloader. These first installers are not Authenticode-signed or Apple
notarized, so Windows SmartScreen or macOS Gatekeeper may show an unknown
publisher warning until official code-signing certificates are added.

## Installation behavior

The default locations are:

```text
Windows: %LOCALAPPDATA%\ZombieOS\ZSharp\bin\zsharp.exe
Linux:   ~/.local/bin/zsharp
macOS:   ~/.local/bin/zsharp
```

On Windows, the installer adds its bin folder to the current user's `PATH`.
The user must open a new terminal before the `zsharp` command becomes visible.
Linux and macOS print a reminder if `~/.local/bin` is not already in `PATH`.

After installation, the new ZVM runs `zsharp associate`. This registers
`.zapp` and `.zgame` for the current user. If an older ZVM exists at the same
location, it is preserved as `zsharp.previous.exe` on Windows or
`zsharp.previous` on Linux and macOS before replacement.

An interactive first-time installation then offers to open the official Z#
Test App download in the user's browser. The prompt is skipped for automatic,
quiet, and update-check runs. Declining it does not change the installation.

The bootstrap also installs itself beside the ZVM as `zsharp-installer`. Every
standalone ZVM launch quietly downloads
`update.js?v=INSTALLED_VERSION-OS` and compares its installed version with the
manifest's `latestVersion`. GitHub Pages serves the same static manifest for
every query value. Concurrent launches share a lock. When the version differs,
the updater verifies `ZVM-LATEST.zip`, waits for the running ZVM process to
finish, and replaces only the ZVM installation. Registered applications,
games, package caches, and their data are outside this replacement and remain
intact. After replacement, package associations are refreshed so opening a
`.zapp` or `.zgame` from the desktop does not open a terminal.

ZVM copies embedded in the Java/Maven artifact do not have this sibling
updater, so they stay pinned to the dependency version selected by the Java or
Minecraft project.

The installer also supports unattended and mirror-based use:

```text
zsharp-installer --yes
zsharp-installer --manifest https://mirror.example/update.js --yes
zsharp-installer --install-dir "custom/location"
```

## Official update endpoint

The bootstrap request is:

```text
https://www.zsharp.zombieos.com/update.js?v=0.0.0.0-windows
```

`0.0.0.0` means that no ZVM is installed yet. The runtime auto-updater should
send its installed four-part version instead, for example:

```text
https://www.zsharp.zombieos.com/update.js?v=1.0.1.1-windows
```

The query is retained for compatibility and cache separation, but GitHub Pages
ignores it when selecting the static `update.js` file. The installer performs
the version comparison locally. The manifest always contains the latest release:

```json
{
  "schema": 1,
  "latestVersion": "1.0.1.1",
  "download": {
    "url": "https://www.zsharp.zombieos.com/assets/download/ZVM-LATEST.zip",
    "sha256": "archive-sha256",
    "size": 1234567
  },
  "platforms": {
    "windows-x86_64": {
      "path": "runtimes/windows-x86_64/zsharp.exe",
      "sha256": "runtime-sha256",
      "size": 254464
    }
  }
}
```

When the versions match, the installer stops before downloading the archive.
When they differ, it selects the entry for its operating system and processor.

The archive URL must use HTTPS. Every archive and platform entry carries a
64-character SHA-256 and exact positive size. The installer rejects unsupported
schemas, missing or unsafe archive paths, non-HTTPS URLs, encrypted or
compressed runtime entries, oversized downloads, size mismatches, and checksum
mismatches. `ZVM-LATEST.zip` deliberately stores its six runtime entries
without compression so the small bootstrap can extract them without another
software library.

## Uploading the download site

Release maintainers build the installers and website payload with:

```text
powershell -File scripts/build-installers.ps1 -Zig path/to/zig.exe
```

If the test app is not in the default Downloads project location, pass its
package explicitly with `-TestAppPackage path/to/ZSharp-Test-App.zapp`.

By default, the command keeps generated publishing files outside the source
repository and produces:

```text
%USERPROFILE%\Downloads\ZSharp Publishing\1.0.1.1\zsharp-download-site-1.0.1.1.zip
%USERPROFILE%\Downloads\ZSharp Publishing\1.0.1.1\zsharp-installer-1.0.1.1-PLATFORM
%USERPROFILE%\Downloads\ZSharp Publishing\1.0.1.1\zsharp-installer-1.0.1.1-SHA256SUMS.txt
%USERPROFILE%\Downloads\ZSharp Publishing\1.0.1.1\zsharp-update-1.0.1.1.js
```

The download-site ZIP contains the static `update.js` manifest,
`assets/download/ZVM-LATEST.zip`, the six installer downloads, and
`assets/download/ZSharp-Test-App.zapp` when a test app package was supplied or
found in the default Downloads project location. Extract its contents into the
root of the GitHub Pages site. `zsharp-update-1.0.1.1.js` is a second copy of
the same manifest for release records.

After GitHub Pages publishes the files, test:

```text
https://www.zsharp.zombieos.com/update.js?v=1.0.1.1-windows
https://www.zsharp.zombieos.com/update.js?v=1.0.1.1-linux
https://www.zsharp.zombieos.com/update.js?v=1.0.1.1-macos
https://www.zsharp.zombieos.com/update.js?v=1.0.0.1-windows
https://www.zsharp.zombieos.com/assets/download/ZVM-LATEST.zip
```

All four update URLs should return the same valid JSON manifest, and the archive
URL should download `ZVM-LATEST.zip`. The installer itself decides whether the
installed version needs updating.

The website hostname must have a working DNS record and HTTPS certificate. The
installers report a download error until those are configured and this bundle
has been uploaded.
