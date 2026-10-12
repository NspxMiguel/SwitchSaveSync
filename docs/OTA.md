# OTA releases and rollback

The graphical NRO checks `NspxMiguel/SwitchSaveSync/releases/latest` over the
existing authenticated TLS/HTTP stack, without a GitHub token. Startup and
**About → Check for updates** share a persisted 24-hour cooldown, including
failed requests and rate-limit responses. The timestamp is saved before the
request; an invalid console clock or unwritable SD card prevents the request.
The release tag must be SemVer (an optional `v` prefix is accepted). Only a
newer version is offered; build metadata does not change version precedence.
Release notes are trimmed to 1,199 bytes without splitting UTF-8 characters.

## Publish a release

1. Set the repository **Actions variable** `SSS_AUTH_ENDPOINT` to the project's
   public HTTPS credential endpoint. Do not put Google credentials or tokens
   in this variable. Release builds otherwise stop, rather than ship a Drive
   configuration that cannot log in.
2. Merge the OTA implementation, then push a version tag, for example `v0.5.0`.
   The release workflow builds with `devkitpro/devkita64`, sets the embedded app
   version from the tag, creates or reuses the GitHub release and uploads:
   `SwitchSaveSync.nro`, `exefs.nsp`, `toolbox.json`, `SwitchSaveSync.ovl`,
   `SHA256SUMS`, and `SwitchSaveSync-sd.zip` for the existing installers.
   A manual rerun must select a SemVer tag. Reruns replace the assets together
   with freshly generated checksums. Never reuse a version tag for different
   source; publish a new version instead.
3. Install this OTA-capable NRO manually once. Existing v0.4.0 apps cannot
   discover or install OTA updates. The first newer complete release must
   include `SHA256SUMS` to be installable from the app.

The manifest uses the standard `sha256sum` format: 64 hexadecimal characters,
a space, a text/binary marker (` ` or `*`), then the exact asset basename.
Missing, malformed or duplicate checksum entries, missing assets, failed HTTPS,
failed writes and mismatched SHA-256 all stop installation. SHA256SUMS is an
integrity check delivered over HTTPS, not a detached publisher signature.

## Installation

Stop SwitchSaveSync using Ultrahand before pressing **Update now** and leave
it stopped until the app exits. The app checks `pm:dmnt` before downloading
and again before replacement; it proceeds only on Atmosphere's explicit
`ProcessNotFound` result. An unavailable service or any other error blocks
replacement. No sysmodule or overlay source changes are needed, and the
updater never creates `boot2.flag`.

All four downloads stream to adjacent `.ota` files and are SHA-256 verified.
The progress bar reports the actual bytes received / transfer size for the
current asset, rather than a timer. If the server supplies no size, it shows
only the download status until verification finishes. No installed files
change during download. The running NRO path comes from libnx's parsed
hbloader argv (libnx obtains that through `envGetArgv`); non-SD or missing paths
are refused. A custom NRO location is preserved.

The hashes are checked again on exit, before any rename, so editing a staged
file after download also prevents installation. On exit, the app renames each installed target to `.bak`, then the verified
`.ota` file to its target on the same SD filesystem. It restores previously
replaced files if a later rename fails. Successful installation keeps the
previous files as `.bak`. The app offers **Restart now / Later**; restarting
uses `envSetNextLoad`, and loaders without that support return to hbmenu.
The legacy console spike in `app/` is not the distributed graphical app and
has no update menu.

## Manual rollback and interrupted installation

Exit the app and stop the module. On the SD card, rename the current target
out of the way and restore its `.bak` to its original name. Restore all four
files from the same release:

- The launched `.nro` location (normally `/switch/SwitchSaveSync.nro`).
- `/atmosphere/contents/00FF0000535953FF/exefs.nsp` and `toolbox.json`.
- `/switch/.overlays/SwitchSaveSync.ovl`.

The next successful update replaces older `.bak` files. Per-file renames are
atomic; a multi-file update is not a power-loss-atomic transaction. If power
fails between renames, inspect `.bak`/`.ota` files and restore the previous
set before starting the module. Keep the card inserted until the app exits.
A replacement or rollback error leaves recovery files available, cancels queued
restart and records an error to display on the next app launch.

## Validation

`./tests/run.sh ota` runs synthetic host tests under ASan/UBSan for SemVer,
SHA256SUMS, SHA-256 reference vectors, release parsing, daily cooldown, SD
layout, successful replacement and failed-replacement rollback. The ordinary
CI test workflow also runs this target. Console-only validation remains:
review the PT/EN dialogs, download progress, module stop detection, exit swap
and hbloader restart on a real Switch.
