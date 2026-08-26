# Z# application and game uninstalling

This document separates the uninstall behavior implemented in Z# 1.0.1.0 from
the larger hub, app-data, and cloud-backup design planned for later releases.
It applies to application packages (`.zapp`) and game packages (`.zgame`).

## Commands

An application is uninstalled with:

```text
zsharp uninstall App.zapp
```

A game is uninstalled with:

```text
zsharp uninstall Game.zgame
```

The argument identifies the package and its PID. The runtime must resolve and
validate that identity before presenting or deleting anything. Quoted paths
are accepted when the package path contains spaces.

In 1.0.1.0, the command reads and validates package metadata, displays the
package name, PID, type, version, and exact package path, and requires the user
to type `yes`. It then permanently removes the package's content-addressed Z#
cache, the package file, and any Desktop shortcut Z# created for that display
name. Cancellation deletes nothing.

Applications cannot register saves or other writable data with the runtime
yet. Consequently, 1.0.1.0 does not claim to remove unrelated app-created
data. That requires the future managed-data ledger described below.

## Future Z# hub integration

Launching `zsharp` without a package opens the current informational Z# Hub.
The full future Hub will list Z# apps and games that have been run or manually
added. It will store each package's PID, display name, package type, canonical
package location, version, and managed data locations.

Uninstalling is transactional with the hub:

1. The runtime resolves the package and locks its hub record.
2. The app or game is stopped before its files are removed.
3. The runtime inventories only data registered to that PID.
4. Optional ZOS Cloud backup completes and is verified.
5. The hub record is marked as uninstalling.
6. The runtime permanently removes the confirmed package and managed data.
7. The hub entry is removed only after deletion succeeds.

If deletion is incomplete, the hub retains an `uninstall incomplete` record so
the user can retry. It must never report a successful uninstall while managed
files remain.

## Project settings

Window-enabled projects declare the official dependency and window paths:

```zsharp
Dependencies (
 zsharpwindow:1.0.0.0
):

Window (
 Startup: "window/location/Window.zsharp":
 Uninstall: "window/location/Uninstall.zsharp":
):
```

`Window (...)` is a settings section, not a callable function. It is valid
only when the project depends on `zsharpwindow`.

In 1.0.1.0, `Uninstall` is syntax-checked and packaged but the terminal
confirmation is used. A later runtime may open that window to explain what will
be removed, offer cloud backup, and collect the user's choice. The Z# runtime
will still perform and validate deletion; application-owned uninstall code
will not be allowed to delete arbitrary filesystem locations.

Both paths are relative to the project root, use `/` in package metadata, must
remain inside the project, and must identify a `.zsharp` file whose header is
`zsharp = type.script:window`.

## Confirmation and permanent deletion

Uninstalling permanently deletes confirmed data without placing it in the
Windows Recycle Bin, macOS Trash, or a Linux desktop trash directory. The
1.0.1.0 runtime shows:

- the app or game name and PID;
- the package version and type;
- the package path;
- and a clear statement that its verified cache and package file will be
  removed.

After the package and cache are removed, the runtime also removes a matching
Desktop shortcut created by Z#. If shortcut cleanup fails, uninstall remains
complete and reports a warning so the user can remove that shortcut manually.

Cancellation leaves the package and cache unchanged. Noninteractive
uninstalling is not allowed unless a future explicitly designed automation flag
provides equivalent confirmation and an exact deletion inventory.

## Future ZOS Cloud backup

Before deletion, users can choose to preserve eligible data in ZOS Cloud. The
runtime prompts them to sign in to their ZOS account when needed. Saves,
settings, and other user data may be selected; application binaries, temporary
files, and reproducible caches are not backed up by default.

Deletion begins only after the upload is complete and the cloud service has
verified the uploaded content. If backup fails, uninstalling stops unless the
user explicitly chooses to continue without that backup. Authentication tokens
and private signing material are never included in an app backup.

## Current and future safe deletion boundaries

The current runtime constructs the cache path itself from a validated PID,
four-part version, and full-package SHA-256. It rejects absolute/traversal paths
during extraction, never packages symlinks or reparse points, and only removes
that exact cache tree during uninstall.

Z# can safely remove all future app-owned data only when that data was recorded while
the app was installed or running. Each package therefore has a runtime-owned
installation ledger keyed by its PID and immutable project identity.

The uninstaller must:

- canonicalize and display every deletion target;
- delete only the package and PID-owned managed locations;
- refuse project paths containing unresolved `..` components;
- refuse roots, home directories, drive roots, and shared Z# runtime folders;
- never follow symbolic links, junctions, or mount points outside a managed
  location;
- preserve shared dependencies still used by another package; and
- treat user-created files outside managed locations as user files unless they
  were explicitly registered to the app.

An uninstall window cannot widen these boundaries. This prevents a corrupted
or malicious `.zapp` or `.zgame` from asking `zsharp` to erase unrelated data.

## Cross-platform behavior

The package, verification, cache, shortcut, and uninstall behavior is
implemented on Windows, Linux, and macOS. Window applications use Win32/WIC on
Windows, GTK 3 on Linux, and AppKit on macOS. Closing the final application
window stops that window script and exits the current application process.
