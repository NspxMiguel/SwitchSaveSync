# Install

*[Em português](INSTALACAO.md).*

Two ways. The first one is a single command and does everything; the second is
four files you copy by hand, and it is here because you should be able to check
what the first one did.

---

## The one command

**macOS or Linux** — plug the SD card into the computer and run:

```bash
curl -fsSL https://raw.githubusercontent.com/NspxMiguel/SwitchSaveSync/main/install.sh | bash
```

**Windows** — download **[install.bat](https://github.com/NspxMiguel/SwitchSaveSync/releases/latest/download/install.bat)** and **double-click it**. That's the whole
thing: one file, no admin, nothing to install. It fetches the rest by itself. Windows shows a
small "Open File - Security Warning" first — that's what any unsigned script gets.

If you'd rather type a command, PowerShell does the same:

```powershell
irm https://raw.githubusercontent.com/NspxMiguel/SwitchSaveSync/main/install.ps1 | iex
```

It lists the FAT32/exFAT cards it can see, marks the one that looks like a Switch, and
asks which. Then it downloads the newest release, copies four files, and checks that each
one arrived at the right size. **It deletes nothing** and it never formats — your games,
saves and configs are not touched.

Useful flags, when you'd rather not be asked:

| | |
| --- | --- |
| `--card /Volumes/SWITCH` (`-Card E:\`) | use this card, don't go looking |
| `--app-only` (`-AppOnly`) | just the app, skip autosync |
| `--zip file.zip` (`-Zip`) | install from a file you already downloaded, no internet |
| `--version v0.4.0` (`-Version`) | an older release instead of the newest |
| `--yes` (`-Yes`) | don't ask anything |
| `--no-hbl` (`-NoHbl`) | don't create `atmosphere/config/override_config.ini` |
| `--eject` (`-Eject`) | unmount the card at the end |

Both scripts are short and readable, and they are the same files the command above
downloads. Read them first if you'd rather — that's why they ship as source.

---

## Before you start

- **A console on CFW (Atmosphère) with the homebrew menu working.** If that isn't in
  place yet, sort it out first — that's someone else's tutorial, not this one.
- **Firmware:** built and tested against 18.1.0. The autosync sysmodule declares firmware
  6.0 as its floor.
- **Free space on the card**: at least as much as your largest save, twice over. A restore
  stages the download on the card and keeps a local copy of the old save before writing.
- **Ultrahand** ([nx-ovlloader + Ultrahand Overlay](https://github.com/ppkantorski/Ultrahand-Overlay)),
  **only if you want autosync.** The app on its own doesn't need it.
- **sysMMC or emuMMC — pick one and stay there.** The app's state lives on the SD card,
  which both share, while the saves themselves are separate. Syncing from sysMMC and then
  from emuMMC makes "I already synced this save" describe the wrong system's save.

---

## 1. The app

### Download

The [latest release](https://github.com/NspxMiguel/SwitchSaveSync/releases/latest) has five
files. You need either the zip **or** the loose files:

| File | Where it goes on the card |
| --- | --- |
| `SwitchSaveSync-x.y.z-sd.zip` | unzip it **at the root of the card** — everything below is already inside, in the right folders |
| `SwitchSaveSync.nro` | `/switch/SwitchSaveSync.nro` |
| `SwitchSaveSync.ovl` | `/switch/.overlays/SwitchSaveSync.ovl` |
| `exefs.nsp` | `/atmosphere/contents/00FF0000535953FF/exefs.nsp` |
| `toolbox.json` | `/atmosphere/contents/00FF0000535953FF/toolbox.json` |

Only the `.nro` is needed for the app. The other three are autosync, which is optional
and comes in part 2.

**Copy and paste that folder id** — `00FF0000535953FF`. Typing it is how you get a
directory the console reads as a mod for some game instead of a system module. And if you
built the sysmodule yourself, the file comes out as `SwitchSaveSync.nsp` and has to be
**renamed to `exefs.nsp`**; with the wrong name nothing loads and nothing complains.

### The line that makes R work

To open homebrew *as an application* you hold **R** while launching an installed game. That
only works if Atmosphère has `override_any_app` turned on, and **it does not turn itself
on**: the package ships this file under `atmosphere/config_templates/` and never writes
over what is in `atmosphere/config/`. Updating Atmosphère doesn't add it either.

So, on the card, `/atmosphere/config/override_config.ini` needs to contain:

```ini
[hbl_config]
override_any_app=true
override_any_app_key=R
override_any_app_address_space=39_bit
path=atmosphere/hbl.nsp
```

If the file already exists, **keep the lines that are already there** (`program_id_1`,
`override_key_0` and friends) and just add these. The installer does exactly this: it
creates the file if it's missing, and if it exists it asks before touching it — and keeps
a copy next to it.

### Launch it

From the homebrew menu, **holding R on an installed game — not from the Album**.

> Launched from the Album, homebrew runs in *applet mode*: about 448 MB of memory and a
> network stack that sometimes never comes up. Holding R while opening a game makes the
> homebrew menu take that game's place, with full memory and real networking. If you're
> unsure which mode you're in, the app tells you: **Settings → Diagnostics**.

### Sign in

**Google Drive:** the first screen shows an address, a short code, and a QR of the same
address. Open it on your phone or PC, type the code, approve. The console never sees your
password, and the app only ever gets access to the files it creates itself. The result is a
token at `sdmc:/switch/SwitchSaveSync/token.txt`; "sign out" deletes it.

**Or no Google at all:** **Settings → Cloud → WebDAV** points the app at your own server —
Nextcloud, ownCloud, Synology, QNAP, Box. Details in [CLOUDS.md](CLOUDS.md). Your user and
password are stored on the card, in the clear, at `sdmc:/switch/SwitchSaveSync/webdav.cfg`.

**Or no cloud at all:** the SD-card backup works with no account and no network.

### Your own credential (google.cfg)

Not required, and most people won't want it. It's here for anyone who'd rather
**not depend on the project's credential** — neither the one in the app nor the
server that holds it.

Create a Google Cloud project (instructions in
[core/config.h.example](core/config.h.example), about 10 minutes) and write
`sdmc:/switch/SwitchSaveSync/google.cfg`:

```ini
client_id=YOUR_ID.apps.googleusercontent.com
client_secret=YOUR_SECRET
```

From then on the app talks straight to Google with **your** key: no server of
ours in the path, and no rate limit of ours reaching you. This wins over
everything — a complete file here beats both the endpoint and the built-in key.

To leave our server without creating a key at all, write `endpoint=direto`; the
built-in credential takes over again.

> **Why this exists.** An installed app holds no secrets:
> [RFC 6749 §2.1](https://datatracker.ietf.org/doc/html/rfc6749#section-2.1)
> calls it a *public client* and assumes the key is extractable from the binary.
> What protects you is the `drive.file` scope (the app only ever sees what it
> created), the consent screen, and — if you want it — your own key. Settings →
> Diagnostics shows which of the three is in effect.

### Check that it works

**Settings → Diagnostics → Test connection** uploads and downloads a small file. It never
touches a save, and it's the fastest way to know whether the problem is the account, the
network, or the mode the app is running in.

### Optional: a shortcut on the home menu

With [Sphaira](https://github.com/ITotalJustice/sphaira) you can make the app show up as an
icon on the console's own menu, no R and no homebrew menu.

> **Don't move the `.nro` afterwards.** The shortcut stores the file's path, and Sphaira
> derives its title ID from a hash of that path. Move the file and the shortcut points at
> nothing — and rebuilding it creates a *second* icon instead of fixing the first. Leave it
> at `sdmc:/switch/SwitchSaveSync.nro`.

---

## 2. Autosync (optional, and still being tested)

A background process that backs a save up when you close the game, and — if you let it —
brings saves down from the cloud while the console sits on the menu.

**1.** Install [Ultrahand](https://github.com/ppkantorski/Ultrahand-Overlay) first. The
overlay is how you start and stop the sysmodule; there is no other switch.

**2.** The three files from the table above must be on the card.

**3.** Reboot the console. The sysmodule is read at boot; replacing `exefs.nsp` while the
console is on changes nothing until you reboot.

**4.** Open the Ultrahand menu → **SwitchSaveSync**. There are **two switches, and both
matter**:

- **Sysmodule** — starts the process. Off means nothing runs at all.
- **Back up when a game closes** — autosync itself. **It starts off**, on purpose: "I
  haven't configured anything yet" must not mean "go ahead and write to my saves".

Then **Where to save** — *SD card* and *Google Drive*, independent, both on by default.
Turn both off and autosync has nowhere to put anything. **Games** turns it off for one
game. **Back up the last game played** does it now, and refuses while a game is open.

**It does not start with the console.** There is no `boot2.flag` in the package and there
shouldn't be one: something that mounts savedata should not come up before you've said so.

### What it actually does

**Uploading:** three seconds after a game closes, that game's save goes up.

**Downloading — read this one.** With the cloud enabled and the console idle on the menu
for 30 seconds, it sweeps your library and pulls saves down, one game per pass, each game
at most once every 30 minutes. A download **replaces the whole savedata** — the contents
are wiped and rewritten. That is the point of a sync, and it's also the part that can cost
you progress, so it happens only when all three of these hold:

1. **This console uploaded that save before.** No record of a previous sync, no download.
2. **The local save hasn't changed since that upload.** If it has, the local one wins and
   the cloud is left alone.
3. **A copy of the local save was written to the card first.** If that copy fails, the
   download is aborted.

While it downloads, a screen with a progress bar sits over the menu with two buttons:
*"Nao puxar save da nuvem nesse jogo"* and OK. It stays up for at least five seconds.

### Is it working?

The overlay shows the last status line. The long version is
`sdmc:/switch/SwitchSaveSync/autosync.log`.

---

## The card, once everything is in place

```
/switch/SwitchSaveSync.nro                              the app
/switch/.overlays/SwitchSaveSync.ovl                    the Ultrahand overlay
/switch/SwitchSaveSync/                                 everything the app remembers
    token.txt          Google login          webdav.cfg     server, user and password
    nuvem.cfg          which cloud is on     idioma.txt     language
    google.cfg         your own credential (optional)
    autosync.cfg       autosync on/off       destino.cfg    card and/or cloud
    excluidos.txt      games you excluded    pastas.txt     cloud folder per save
    status.txt         last status           autosync.log   the long version
    rev-*.txt          "already synced" markers
    backups/           SD-card backups       staging/       scratch space
/atmosphere/contents/00FF0000535953FF/exefs.nsp         the sysmodule
/atmosphere/contents/00FF0000535953FF/toolbox.json      goes beside it
/atmosphere/config/override_config.ini                  what makes R work
```

Deleting `/switch/SwitchSaveSync/` throws away your login, your SD-card backups and the
sync markers. Nothing in the cloud is touched, but the next sync will look like the first
one ever.

---

## If it didn't work

**I hold R and the game just opens.** `override_any_app=true` is missing from
`/atmosphere/config/override_config.ini`. See above. Nine times out of ten this is it.

**"No connection", but the wi-fi is fine.** You're in applet mode — opened from the Album
instead of holding R on a game. **Settings → Diagnostics** says which mode you're in.

**The overlay isn't in the Ultrahand list.** The `.ovl` has to be at
`/switch/.overlays/SwitchSaveSync.ovl` exactly, and Ultrahand itself has to be installed.

**The overlay's Sysmodule switch turns itself back off.** The `exefs.nsp` isn't where it
should be, or the folder id is wrong, or you haven't rebooted since copying it. The
overlay shows the failure code.

**"No Google account saved — open the app and sign in".** The sysmodule has no login of
its own: it uses the app's token. Open the app once and sign in.

**It says I have to sign in again.** Either the token was revoked — the app deletes it and
says so — or the network is down, which it words differently. If you built the app with
your own Google credentials and left the consent screen in *Testing*, Google expires that
token every 7 days.

---

## Questions

**Do I need the sysmodule?** No. The `.nro` is complete on its own and depends on none of it.

**Will this delete my save?** Not by accident. Uploads never touch the console's save at
all. A restore, whether you asked for it or autosync did, replaces the savedata — and always
writes a copy to the card first, refuses to run while the game is open, and asks you when
the two copies disagree.

**Does it work with two accounts?** Yes. The game shows up once, with as many saves as
there are accounts, and each one gets its own folder in the cloud — named after the
profile's nickname, or `console` for saves that belong to the console rather than a person.
Renaming the profile later doesn't orphan the backup: the folder is remembered in
`pastas.txt`.

**Can I use it without a Google account?** Yes — WebDAV, or the SD-card backup with no
network at all.

**My game isn't in the list.** Only installed games are listed. A save left behind by a
game you deleted doesn't show up.

**Can it start with the console?** No, and that's deliberate. You start it from the overlay.

**How do I update it?** Run the installer again, or copy the files over the old ones. Reboot
if `exefs.nsp` changed. Nothing in `/switch/SwitchSaveSync/` is touched, so you stay logged
in and keep your settings.

**How do I remove it?** Delete `/switch/SwitchSaveSync.nro`,
`/switch/.overlays/SwitchSaveSync.ovl` and `/atmosphere/contents/00FF0000535953FF/`. Delete
`/switch/SwitchSaveSync/` too if you want the login and the local backups gone — what's in
the cloud stays in the cloud, and you can delete that from the cloud's own website.
