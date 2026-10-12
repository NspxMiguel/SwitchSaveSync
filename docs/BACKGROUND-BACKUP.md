# Background backup implementation and owner installation

Branch: `background-backup`, based on `9235c2f`. Target console: firmware
21.2.0, Atmosphere 1.11.2. No console installation or runtime test was performed.
The main checkout's uncommitted changes were read and left untouched.

## Behavior

The module is now an upload-only background service. It has no display, input,
application suspension, launch interception, or savedata restore path. The app's
existing sync UI remains unchanged; cloud downloads are done through the app.
The former idle sysmodule download screen is removed from the running module.

Three independent loops run on CPU 3:

- The main thread polls `pm:dmnt` once per second and maps the PID through
  `pm:info`. PID changes detect an exit even when another title starts immediately,
  or the same title restarts. Only PM's `ProcessNotFound` means no application;
  other IPC failures preserve the previous state.
- A capture worker consumes durable title intents in `capture/`, enumerating only
  that game's account and device saves. It tries immediately on its next pass,
  including while a different game is running. It checks the target before and
  after capture, mounts account saves read-only, copies the complete tree, closes
  the mount without commit, flushes the SD files, and publishes the queue manifest
  only after fingerprinting succeeds. Unavailable/busy saves, missing metadata,
  and SD errors retain the intent and retry after 30 seconds. Each completed account
  is checkpointed in the intent. The 16-save limit covers the eight console users
  plus device saves; overflow fails closed instead of silently omitting accounts.
- An upload worker runs at priority 59, below capture/main priority 50 (larger
  Horizon priority numbers mean lower scheduling priority). Transfers are limited
  to 128 KiB/s and use streaming, including small Drive files. It never mounts a
  game's savedata. A game can launch while the worker continues sending its earlier
  snapshot; there is no waiting screen or launch hook.

The heap is fixed at 8 MiB. Each worker has a 128 KiB stack. This is not a claim
that IPC, SD, CPU, or network contention is zero: real-console timing and memory
usage still need owner validation.

## Persistence, retries, and shared state

`/switch/SwitchSaveSync/queue/<sequence>.job/` contains `data/`, a versioned,
checksummed `job.bin` manifest, and an `uploaded.txt` checkpoint journal. There
are no credentials in queue manifests. Publication and retirement use directory
rename. On restart, incomplete `.building` captures and retired `.job.done`
entries are cleaned; committed snapshots remain. SD/FAT cannot guarantee recovery
from arbitrary physical corruption or removal while writing.

The worker processes a strict FIFO. Retry deadlines and attempt counts persist in
the manifest: 30, 60, 120 seconds, doubling to a one-hour maximum. Wi-Fi loss,
sleep-interrupted HTTP, missing credentials, HTTP errors, prune failures, and pause
keep the job. Completed files are checkpointed with their destination folder and
are skipped on retry only if they still exist there. The interrupted **file**
restarts from byte zero; Drive upload session URLs/byte offsets are not persisted.
Jobs resume across sleep and reboots when the owner manually starts the module.

No queue entry is automatically evicted to free disk space. Keep enough SD space
for every outstanding snapshot, plus the optional local backup. A full SD stops
publication and is logged; it does not delete previous snapshots.

Uploads reuse `syncjob_upload_snapshot`, the existing cloud folder resolver,
`pastas.txt`, tree upload and prune logic. The cloud remains exactly
`Nintendo Switch Saves/<Game>/<Account>/`, with existing legacy-folder handling.
NXbox sees the same raw bytes and directory layout. `rev-*.txt` uses the existing
fingerprint algorithm and is atomically flushed/replaced **only after upload and
prune succeed**, using the queued snapshot's fingerprint, never a newly reopened
save. Thus newer local gameplay remains detectable as unsynchronized.

Google Drive reads the same `token.txt` and `google.cfg` as the app; WebDAV reads
`webdav.cfg`. There is no second login. The provider is pinned when the module
starts, and every manifest records it: a different provider blocks the FIFO rather
than uploading the job to the wrong cloud. Restart after changing provider or
credential configuration. A blocked excluded game also retains its queue entry;
re-enable it to drain the FIFO.

The upload/prune sequence is the existing backup operation, not a transactional
cloud directory or an automatic cross-client merge. Let the queue finish before
syncing the same game in the app or NXbox. Pause cancels uploads and continues local
capture; it is not a safe substitute for draining older queued copies before an
explicit restore or independent upload of that game.

## IPC surface and firmware safety

The module uses these sessions, with checked startup failures:

| Service | Purpose and restricted operation |
| --- | --- |
| `fsp-srv` | SD files, save enumeration, account read-only mounting; device mounting has no read-only libnx wrapper, so code only reads and never commits |
| `pm:dmnt` | `GetApplicationProcessId`, command 4 on modern firmware; polling only, no hooks/debug attachment |
| `pm:info` | `GetProgramId`, command 0; read-only PID-to-title mapping |
| `ns:am2` | Title NACP metadata through `IReadOnlyApplicationControlDataInterface`; no application-manager lifecycle hooks, launch, or termination |
| `acc:u0` | Read profile nicknames for the unchanged cloud account-folder naming |
| `set:sys` | Read firmware once so libnx chooses the correct IPC ABI; close immediately |
| `set` | Read the configured display language at startup, if automatic |
| `time:s` | TLS certificate validity and persisted retry deadlines |
| `bsd:s`, `sfdnsres` | System sockets and DNS with bounded socket buffers |
| `csrng` | Software mbedTLS/libcurl entropy |

The background target excludes installation queries and play-history sorting, so
`pdm:qry` is not opened or permitted. The sysmodule does not request `am` applet
services (`appletAE`, `appletOE`), HID, VI, nvdrv, font services, or SSL IPC (TLS is
mbedTLS in-process). `gfx.c` is excluded from this target. Debug-attach syscalls
remain absent from the NPDM.

These choices avoid the known fragile AM/applet interaction rather than assuming
it is fixed on 21.x. PM calls and the modern NS read-only interface are documented
in [libnx PM source](https://github.com/switchbrew/libnx/blob/master/nx/source/services/pm.c)
and [libnx NS documentation](https://switchbrew.github.io/libnx/ns_8h.html).
The no-process error is defined in
[Atmosphere PM results](https://github.com/Atmosphere-NX/Atmosphere/blob/master/libraries/libvapours/include/vapours/results/pm_results.hpp).
They establish the intended IPC surface; they do **not** prove runtime compatibility
of this build on firmware 21.2.0/AMS 1.11.2. That remains untested on the console.

## Exact owner installation

The devkitPro builds are local at `sysmodule/SwitchSaveSync.nsp` and
`overlay/SwitchSaveSync.ovl`. They were built with the owner's existing private
`core/config.h`, copied read-only from the main checkout; it remains Git-ignored.
No OAuth credentials, tokens, real saves, or deployable binaries are committed.

1. Power the Switch off and mount its SD card on the Mac. Run the following yourself,
   substituting the actual SD mount point. These commands were **not** executed on
   a console/card by this change:

   ```sh
   cd /Users/miguel/Documents/Claude/Projetos/SwitchSaveSync-bg
   card=/Volumes/SWITCH
   mkdir -p "$card/atmosphere/contents/00FF0000535953FF" "$card/switch/.overlays"
   cp sysmodule/SwitchSaveSync.nsp "$card/atmosphere/contents/00FF0000535953FF/exefs.nsp"
   cp sysmodule/toolbox.json "$card/atmosphere/contents/00FF0000535953FF/toolbox.json"
   cp overlay/SwitchSaveSync.ovl "$card/switch/.overlays/SwitchSaveSync.ovl"
   rm -f "$card/atmosphere/contents/00FF0000535953FF/flags/boot2.flag"
   ```

2. Keep the installed app `.nro`. Open it once, sign in if needed, and confirm its
   cloud connection. This also supplies `/switch/SwitchSaveSync/cacert.pem`.
   No replacement app is needed for the background upload controls.
3. Reboot into Atmosphere. In Ultrahand → SwitchSaveSync, enable **Sysmodule**,
   **Back up on close**, and the desired destinations. Autosync still defaults off.
   Clear **Pause uploads** to resume. Close a game; no app/home-screen wait is required.
4. The overlay shows pending account snapshots, uploading state, current file byte
   percentage, and the last result. Pause/resume is persisted via `uploads.paused`;
   pause interrupts the current HTTP transfer and retains all snapshots. The retry
   deadline still applies after resume. Stopping/rebooting preserves the queue but
   requires manual module start again.
5. Check `/switch/SwitchSaveSync/autosync.log`, `status.txt`, and `queue-status.txt`.
   The bounded log records initialization, local capture, upload, retry, and shutdown
   results without credential values. Verify a completed game's files in Drive/WebDAV
   before relying on background backup, then test offline close → reboot → manual
   start → resume, and pause while a larger upload is running.

**Kill switch:** create `/switch/SwitchSaveSync/disabled.flag` (an empty file is enough).
At startup the module mounts SD, logs the disable, and exits before PM/NS/account/network
initialization. Creating it while running also cancels the uploader and stops the workers.
Remove it to allow a later manual start. The build/install path creates no `boot2.flag`.

**Rollback:** stop the module or reboot, restore the previous `exefs.nsp` and `.ovl`,
and keep `disabled.flag` in place. Preserve `queue/` and `capture/` when diagnosing.
A damaged manifest blocks uploads and is reported; copy that queue directory off
SD before repairing anything. Do not discard queue data until its backup is verified.

## Validation and changed files

- devkitPro: sysmodule `.nsp`, Ultrahand `.ovl`, and legacy text app `.nro` compiled successfully; no console used.
- `./tests/run.sh`: all default targets, ASan/UBSan, fake-cloud integration, local Apache
  WebDAV, FTP, authentication server tests, and NPDM service checks.
- `test_backupqueue.c`: 70 checks covering capture without network/commit, busy-save/guard rejection,
  immutable snapshots, persistent retry/backoff, pause, interrupted tree resume,
  prune safety, snapshot markers, multiple accounts/providers, corruption detection,
  incomplete capture and retirement recovery.
- Local WebDAV: 52 checks, including byte-progress callback cancels an in-flight streamed PUT and a retry
  succeeds from the same local file. Live Google Drive/OAuth integration was not run.
- No simulator, dev server, console install, certificate, or Apple-account operation.

Implementation files: `core/backupqueue.{c,h}`, `core/syncjob.{c,h}`,
`core/cloud.{c,h}`, `core/http.{c,h}`, `core/drive.c`, `core/titles.{c,h}`,
`core/syncstate.c`, `sysmodule/source/main.c`, sysmodule Makefile/NPDM,
`overlay/source/main.cpp`, queue/WebDAV tests, test runner/service audit/CI,
and README/INSTALL documentation. Formatting is recorded in a separate commit.
