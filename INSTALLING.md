# Installing the Z# Virtual Machine

Z# 1.0.2.0 provides small bootstrap installers for Windows, Linux, and macOS.
The installer itself does not contain the ZVM. It asks the official update
endpoint for the newest runtime for the current operating system and CPU,
downloads `assets/download/ZVM-LATEST.zip` over HTTPS, verifies the archive,
extracts only the matching runtime, and verifies that runtime's exact byte size
and SHA-256 before installing it.

## Download to give users

Most Windows computers use:

```text
zsharp-installer-1.0.2.0-windows-x86_64.exe
```

Other available downloads are:

```text
zsharp-installer-1.0.2.0-windows-aarch64.exe
zsharp-installer-1.0.2.0-linux-x86_64
zsharp-installer-1.0.2.0-linux-aarch64
zsharp-installer-1.0.2.0-macos-x86_64
zsharp-installer-1.0.2.0-macos-aarch64
```

Windows users can double-click the matching `.exe`. Linux and macOS users run:

```text
chmod +x zsharp-installer-1.0.2.0-PLATFORM
./zsharp-installer-1.0.2.0-PLATFORM
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
`.zapp` and `.zgame` for the current user. On Windows it also registers and
starts the single-instance Z# update tray agent for the current user. If an
older ZVM exists at the same location, it is preserved as
`zsharp.previous.exe` on Windows or `zsharp.previous` on Linux and macOS before
replacement.

Every successful installation or version update also creates or refreshes a
`Z# Hub` launcher on the current user's desktop. It opens the graphical Hub
without a terminal window. Project icons come from the optional `Icon` entry
in `project.zsettings`, with the official Z# logo as the fallback. Selecting
an icon or project name shows its version, playtime, and last-played time;
achievements are labeled **Coming soon**. If desktop integration is temporarily unavailable,
installation still succeeds and prints a repair command:

```text
zsharp hub shortcut
```

On a headless machine, set `ZSHARP_INSTALLER_SKIP_HUB_SHORTCUT=1` before
running the installer to intentionally omit the desktop launcher.

An interactive first-time installation then offers to open the official Z#
Test App download in the user's browser. The prompt is skipped for automatic,
quiet, and update-check runs. Declining it does not change the installation.

The bootstrap also installs itself beside the ZVM as `zsharp-installer`. On
Windows, the tray agent checks `update.js?v=INSTALLED_VERSION-windows` once
after it starts at sign-in and then once per hour. Its menu can also check
manually or exit the agent for the current sign-in session. When a genuinely
newer release exists, the tray notifies the user that installation is starting,
then the updater verifies `ZVM-LATEST.zip`, waits for the old ZVM process to
finish, and replaces only the ZVM installation. The new runtime refreshes the
associations and restarts the tray agent. Linux and macOS perform the same quiet
check when a standalone ZVM launches instead of running a tray process.

The version comparison uses all four numeric parts. A matching version does
nothing, and an older website manifest is refused rather than being treated as
an update. Concurrent checks share a lock. Registered applications, games,
package caches, and their data are outside runtime replacement and remain
intact. Opening an associated `.zapp` or `.zgame` from the desktop does not open
a terminal.

The `1.0.1.1` runtime cannot create a tray process that did not exist in its
code. Existing `1.0.1.1` users therefore need to run any Z# command once, or
run the `1.0.1.2` installer once, to receive this patch. From `1.0.1.2` onward,
the startup tray handles future Windows update checks automatically.

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
https://www.zsharp.zombieos.com/update.js?v=1.0.2.0-windows
```

The query is retained for compatibility and cache separation, but GitHub Pages
ignores it when selecting the static `update.js` file. The installer performs
the version comparison locally. The manifest always contains the latest release:

```json
{
  "schema": 1,
  "latestVersion": "1.0.2.0",
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
When the available version is newer, it selects the entry for its operating
system and processor. When the available version is older, it refuses the
downgrade and leaves the installed ZVM unchanged.

The archive URL must use HTTPS. Every archive and platform entry carries a
64-character SHA-256 and exact positive size. The installer rejects unsupported
schemas, missing or unsafe archive paths, non-HTTPS URLs, encrypted or
compressed runtime entries, oversized downloads, size mismatches, and checksum
mismatches. `ZVM-LATEST.zip` deliberately stores its six runtime entries
without compression so the small bootstrap can extract them without another
software library.

## Uploading the download site

Release maintainers can prepare every local release artifact from the language
repository with:

```text
zsharp publish
```

This builds and tests the native runtime, packages the test game, creates the
native Windows archive, builds and signs the Java/Maven files, and generates
the installers, checksums, update manifest, and website upload ZIP. It never
pushes Git commits, creates tags, uploads a GitHub Release, changes the website,
or sends anything to Maven Central. The optional
`zsharp publish [repository]` form can be used outside the repository folder.

Game runtimes are built natively by the **Build native game runtimes** GitHub
workflow. Before the final local publishing pass, extract each downloaded
artifact into:

```text
%USERPROFILE%\Downloads\ZSharp Publishing\VERSION\Native Runtimes\PLATFORM
```

The command stages any runtime found there. If a current runtime is missing,
it leaves a readable `PUBLISHING-STATUS.txt` in the version folder and stops
before creating an incomplete cross-platform installer or Maven bundle. Run
the same command again after adding the missing artifacts.

The lower-level installer and website builder remains available as:

```text
powershell -File scripts/build-installers.ps1 -Zig path/to/zig.exe
```

If the test app is not in the default Downloads project location, pass its
package explicitly with `-TestAppPackage path/to/ZSharp-Test-App.zapp`.

By default, the command keeps generated publishing files outside the source
repository and produces:

```text
%USERPROFILE%\Downloads\ZSharp Publishing\1.0.2.0\zsharp-download-site-1.0.2.0.zip
%USERPROFILE%\Downloads\ZSharp Publishing\1.0.2.0\zsharp-installer-1.0.2.0-PLATFORM
%USERPROFILE%\Downloads\ZSharp Publishing\1.0.2.0\zsharp-installer-1.0.2.0-SHA256SUMS.txt
%USERPROFILE%\Downloads\ZSharp Publishing\1.0.2.0\zsharp-update-1.0.2.0.js
```

The download-site ZIP contains the static `update.js` manifest,
`assets/download/ZVM-LATEST.zip`, the six installer downloads, and
`assets/download/ZSharp-Test-App.zapp` when a test app package was supplied or
found in the default Downloads project location. Extract its contents into the
root of the GitHub Pages site. `zsharp-update-1.0.2.0.js` is a second copy of
the same manifest for release records.

After GitHub Pages publishes the files, test:

```text
https://www.zsharp.zombieos.com/update.js?v=1.0.2.0-windows
https://www.zsharp.zombieos.com/update.js?v=1.0.2.0-linux
https://www.zsharp.zombieos.com/update.js?v=1.0.2.0-macos
https://www.zsharp.zombieos.com/update.js?v=1.0.0.1-windows
https://www.zsharp.zombieos.com/assets/download/ZVM-LATEST.zip
```

All four update URLs should return the same valid JSON manifest, and the archive
URL should download `ZVM-LATEST.zip`. The installer itself decides whether the
installed version needs updating.

The website hostname must have a working DNS record and HTTPS certificate. The
installers report a download error until those are configured and this bundle
has been uploaded.
