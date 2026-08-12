<div align="center">

<img src="assets/icon.png" alt="SwitchSaveSync" width="128">

# SwitchSaveSync

**Switch saves on Google Drive, in one button press.**
Open the app, press A on a game, done — it works out for itself whether to upload or download.

[![Platform](https://img.shields.io/badge/platform-Nintendo%20Switch-e60012)](https://switchbrew.org/)
[![CFW](https://img.shields.io/badge/CFW-Atmosph%C3%A8re-5865f2)](https://github.com/Atmosphere-NX/Atmosphere)
[![Build](https://img.shields.io/badge/build-devkitPro-1f9c4b)](https://devkitpro.org/)
[![Languages](https://img.shields.io/badge/languages-PT%20%7C%20EN-blue)](#also-in-here)
[![Tested on](https://img.shields.io/badge/tested%20on-firmware%2018.1.0-f59f00)](#where-it-was-tested)
[![License](https://img.shields.io/badge/license-GPLv3-663366)](LICENSE)

[Português](README.pt-BR.md)

</div>

---

## What it is

Homebrew that keeps your game saves in **your** Google Drive and brings them back when you
need them — the way Steam Cloud does, but with no server, no subscription and no cost at
all. There is no backend: the console talks straight to the Google Drive API using an
account that belongs to you.

It **never interprets save contents**. It copies the file tree byte for byte, both ways.
That's why it works on games nobody ever tested it against — including games that aren't
out yet — with no supported-games list to maintain.

## Before anything else

Look... this was made by **one person**. One. No test team, no QA, nobody looking over my
shoulder going *"you sure about that?"*. It was tested on my console, with my games, my way.

So yes, it can have bugs. It probably does, and I have no idea which ones yet — I'll only
find out the worst way possible, which is someone telling me it broke.

**If you find one, will you tell me?** Please. It doesn't have to be pretty or technical: a
*"it froze when I did this"* helps more than you'd think. It's right there in
[Issues](https://github.com/NspxMiguel/SwitchSaveSync/issues).

I'll fix it. I'll cry a little first, but I'll fix it.

And before anything goes wrong: **keep a copy of the saves you can't afford to lose.** Not
because I think it'll break — but a save is a save, and I'd sleep a lot better knowing you
have one.

## The single press

**A** does the right thing on its own, comparing a fingerprint of both sides against the
one from the last sync:

| Situation | What it does |
| --- | --- |
| Only the console changed | Uploads to Drive |
| Only Drive changed | Downloads to the console |
| Neither changed | Touches nothing |
| The console has no save yet | Downloads from Drive |
| **Both changed** | **Stops and asks** |

That last row is the one that matters: picking a side there would erase real progress. The
app shows both sides (how many files, how many bytes) and leaves the decision to you.
Nothing is written until you press.

**Y** opens the menu with the individual actions, for when you'd rather decide yourself:
upload, download, back up to the SD card, restore from the SD card.

## What it does that others don't

**Saves kept apart per account.** The same game played by two console profiles has two
different saves, and they never get mixed. The game shows up once in the list, with
`2 saves` on the right, and whose save it is gets picked at click time.

**Device saves, not just account saves.** The Switch has two kinds: account saves
(`FsSaveDataType_Account`) and device saves (`FsSaveDataType_Device`), which belong to the
console rather than a profile. Animal Crossing is the ugly case: **the island is a device
save**, while the account save exists and is nearly empty. A tool that only reads account
saves will happily "back up Animal Crossing" and leave the island behind. Pokémon
Sword/Shield uses both kinds. Here both are read.

**A real mirror.** A file the game deleted from the save also leaves the cloud — otherwise
it would come back to life on the next restore. It goes to the Drive trash, not away
forever: you get 30 days to undo.

**Backup on the SD card itself.** With no internet and no Google account, you can still
keep a copy in `sdmc:/switch/SwitchSaveSync/backups` and restore from there. It's the
safety net for when the cloud isn't an option.

## Also in here

- **Sync everything at once** — one row at the top of the list walks every save, deciding
  which way each one goes. When both sides changed it does **not** choose: that game is
  left alone and shows up in a list at the end, for you to sort out one by one.
- **Everything in one file** — optional: packs every save into a `.nxsaves`, a format of ours
  that isn't a zip and that only this app reads. Handy for taking everything at once. *It's
  a disguise, not a lock* — the code is open, so anyone determined enough can read it.
  That's why the normal mode (one folder per game on Drive) is still the recommended one: a
  save locked in a format only one program reads is a save that dies with that program.
- **QR code login** — point your phone at it, type the code, done. No on-screen keyboard.
- **Parental lock** — a 4-to-8 digit password at startup, stored as a hash. Changing or
  removing it requires the current one.
- **Sorted by last played**, with each game's real name and icon read from the console.
- **Portuguese and English**, following the console's language or picked by hand.

## Where it was tested

**A Switch V2 on firmware 18.1.0, running Atmosphère.** That's the console it was written
on and the one it runs on every day — everything else here is honesty about what nobody has
tried yet.

Nothing in here is version-dependent: the app has no offset tables, patches nothing, and
never reads a save's internal structure. It uses libnx's save-mounting calls, which have
been the same since firmware 1.0, and the Google Drive API, which is HTTPS. So the odds of
it breaking on a different version are low — but *low* isn't *tested*, and that's what this
paragraph is saying.

If it works (or doesn't) on another version, [open an
issue](https://github.com/NspxMiguel/SwitchSaveSync/issues) saying which one — this can
become a real list.

## Install

You need a console on CFW (**Atmosphère**) with the homebrew menu working. If that isn't in
place yet, sort it out first — that's someone else's tutorial, not this one.

### 1. Download

Grab `SwitchSaveSync.nro` from the **[latest release](https://github.com/NspxMiguel/SwitchSaveSync/releases/latest)**.

One file. No installer, no dependencies, no account to create here.

### 2. Copy it to the SD card

Put `SwitchSaveSync.nro` in `sdmc:/switch/`. Either pull the card and use a PC, or send it
over FTP if you already run one.

### 3. Launch it

From the homebrew menu — but **holding R on a game, not from the Album**.

> Launched from the Album, homebrew runs in *applet mode*: it gets only ~448 MB of memory
> and the network stack sometimes fails to come up at all. Holding **R** while opening an
> installed game makes the homebrew menu take the game's place and run as an application,
> with full memory and networking. If you're unsure which mode you're in, the app itself
> tells you: **Settings** tab, under Diagnostics.
>
> This stops being necessary after step 5.

### 4. Sign in

The first time, the app shows a **code** and an address (and a QR code, if you'd rather use
your phone's camera). You open that address on your phone or PC, type the code and approve.

**The console never asks for your password.** You sign in yourself, on Google's own page.

The scope requested is **`drive.file`**: the app only ever sees files it created itself. The
rest of your Drive is invisible to it — that's not a promise from me, it's Google refusing.

The login is stored on the SD card only, in `/switch/SwitchSaveSync/token.txt`, and Sign out
removes it for good.

> **Whose credentials are these?** Mine — the app ships with them built in, so you don't
> have to create a Google Cloud project just to use a save-sync homebrew. The **account is
> yours**, the **Drive is yours** and the **files are yours**: I have no access to any of
> it, and `drive.file` stops even the app from looking at the rest of your Drive. If you'd
> still rather use your own, just [build it](#building-with-your-own-credentials) — that
> road stays open.

### 5. Done — and optionally, like a game

You can use it already: open it, press **A** on a game, and it works the rest out.

### Putting it on the home screen

You can have an icon for the app on the console's home screen, next to your games, and open
it from there. [Sphaira](https://github.com/ITotalJustice/sphaira) does it by itself, **on
the console** — no PC, no `hacbrewpack`, no `prod.keys` of yours: it derives the key
straight from the console.

1. Open Sphaira and find `SwitchSaveSync` in the homebrew list.
2. Open the options and pick **Install Forwarder**.
3. Installing ships disabled in Sphaira; it asks whether it may turn it on — say yes.

The shortcut is born with the name and icon that live inside the `.nro`, so it shows up as
**SwitchSaveSync**, by *Miguel*, with the same icon at the top of this page.

It also settles the paragraph above: the shortcut is installed as an *application*, so
opening it that way already gives you full memory and networking. The **R** trick stops
being necessary.

> **Don't move the `.nro` afterwards.** The shortcut stores the file's path, and Sphaira
> derives the title ID from a hash of that path. Move the file and the shortcut points at
> nothing — and rebuilding it from the new path creates a second icon instead of fixing the
> first. Leave it at `sdmc:/switch/SwitchSaveSync.nro` and be done.

### Autosync — optional, and new

The app above is the whole product; this part is extra. It's a **sysmodule**: it runs in the
background and, when you close a game, backs that game's save up on its own. You never open
it — you check on it from an **[Ultrahand](https://github.com/ppkantorski/Ultrahand-Overlay)
overlay**, which is where it says what it did.

Three files, and none of them replace the `.nro`:

| From the release | Where it goes on the SD card |
| --- | --- |
| `exefs.nsp` | `/atmosphere/contents/00FF0000535953FF/exefs.nsp` |
| `toolbox.json` | `/atmosphere/contents/00FF0000535953FF/toolbox.json` |
| `SwitchSaveSync.ovl` | `/switch/.overlays/SwitchSaveSync.ovl` |

Then reboot, open the Ultrahand overlay and start it from there.

> **It does not start with the console, on purpose.** There is no `boot2.flag` in that
> folder and there shouldn't be one: something that mounts savedata should not come up
> before you've said so. You start it by hand, from the overlay, and it stops when you say.

It uses the **same login as the app** — sign in there first, or the cloud half has nothing
to talk to and the overlay will say so.

**Where this actually is:** the SD-card half runs on the console today. The cloud half is
still being tested, so treat this as the part that's being built rather than the part you
rely on. The `.nro` doesn't depend on any of it.

## Where the saves end up

In a `Nintendo Switch Saves/` folder at the root of your Drive (or your WebDAV), a folder
per **game** and, inside it, a folder per **account**:

```
Nintendo Switch Saves/
  Rayman Legends_ Definitive Edition/
    Miguel/          ← the files, loose
    Convidado/
  The Legend of Zelda_ Breath of the Wild/
    Miguel/
```

The account folder is always there, even when the game has a single save: a save sitting
loose in the game's folder reads like a save with no owner, and that only stays true until
someone else on the console opens the same game.

No closed format: you can open the cloud's website and pull a single file out by hand
whenever you want. The one exception is optional — a `<Game>.nxsaves` sitting next to the
account folders, which is that game's saves, every account, packed into a single file for
carrying around.

Names are just names. What identifies a backup is the game + account pair, recorded in
`/switch/SwitchSaveSync/pastas.txt`, which is why you can rename the console account freely
without the app losing sight of it.

## Building with your own credentials

None of this is needed to use the app — it's for people who'd rather not go through my
credentials, or who are going to touch the code.

Needs [devkitPro](https://devkitpro.org/wiki/Getting_Started) with the `switch-dev` group,
plus `switch-curl`, `switch-mbedtls` and `switch-zlib`.

```bash
cp core/config.h.example core/config.h
```

`config.h.example` walks through it screen by screen: create a project at
[console.cloud.google.com](https://console.cloud.google.com), enable the **Google Drive
API**, fill in the consent screen, and generate an OAuth client of the **"TVs and Limited
Input devices"** type — that type specifically: it's the one that allows the code-based
login, with no keyboard.

One detail that saves you a headache: on the consent screen, set the publishing status to
**"In production"**. Under *"Testing"*, Google expires the login every **7 days**. Since
`drive.file` is a non-sensitive scope, publishing is instant — no verification process at
all.

Paste the ID and the secret into `config.h`; it's in `.gitignore` and never reaches a commit.

```bash
export DEVKITPRO=/opt/devkitpro
export DEVKITA64=$DEVKITPRO/devkitA64
export PATH=$DEVKITPRO/tools/bin:$DEVKITA64/bin:$PATH
make -C gui
```

Out comes `gui/SwitchSaveSync.nro` — from there it's install step 2 onwards.

The other folders build the same way (`make -C app`, `make -C sysmodule`), but check the
[project status](#project-status) first: they're parked on purpose.

## What it doesn't do

- **It won't touch a running game's save.** The game has to be closed; the app tells you
  when it can't mount.
- **It doesn't interpret saves.** No editing, no converting, no "fixing".
- **It never syncs behind your back.** Syncing is always a press of yours. Automatic mode is
  planned, but doesn't exist yet.
- **It doesn't install itself at boot.** No `boot2.flag` — a deliberate call: homebrew that
  comes up with the console is homebrew that can stop the console coming up.

## Project status

The **graphical app** (`gui/`) is what's finished and in use. The other folders are roads
that were opened and are parked on purpose:

| Folder | What it is | Status |
| --- | --- | --- |
| `gui/` | The app, built on [borealis](https://github.com/natinusala/borealis) | **In use** |
| `core/` | The engine: Drive, OAuth, save mounting, sync | **In use** |
| `app/` | The first version, text mode | Historical |
| `sysmodule/` | Background autosync | **New, under test** |
| `overlay/` | The Ultrahand overlay that drives it | **New, under test** |

Autosync backs a save up when you **close** the game. On the console it has done the
SD-card half; the cloud half is still being tested. [How to install
it](#autosync--optional-and-new).

What's still missing there is the other end: a screen at game **startup** — "syncing,
please wait", with a percentage bar and a **Skip** button for people who won't wait.

## Further reading

- [`ANALISE.md`](ANALISE.md) *(Portuguese)* — the feasibility study that started the
  project: what already existed, what had to be built, and where the risk lived.
- [`SAVES.md`](SAVES.md) *(Portuguese)* — how Switch saves actually work, and what that
  forces the app to do.
- [`CLOUDS.md`](CLOUDS.md) — **how to add a new cloud yourself** (OneDrive, Dropbox,
  whatever): the twelve functions to write, the three lines that register them, how to test
  it without a console, and the OneDrive Graph endpoints already chewed through. You don't
  need to ask me — it's one new file, not surgery.

## Credits

[borealis](https://github.com/natinusala/borealis) for the interface,
[libnx](https://github.com/switchbrew/libnx) and [devkitPro](https://devkitpro.org/) for
everything else, [Atmosphère](https://github.com/Atmosphere-NX/Atmosphere) for existing,
[qrcodegen](https://github.com/nayuki/QR-Code-generator) for the login QR.

The device-save path came from looking at where [JKSV](https://github.com/J-D-K/JKSV) and
[Checkpoint](https://github.com/FlagBrew/Checkpoint) trip up — both have an open issue
about it.

## License

[GPLv3](LICENSE) — the same one Atmosphère, JKSV and Checkpoint use. Use it, study it,
change it, pass it on; anyone distributing a modified version has to open their source too.
