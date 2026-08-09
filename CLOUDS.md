# Adding a new cloud

This guide is for anyone who wants SwitchSaveSync to talk to a cloud it doesn't know yet —
OneDrive, Dropbox, pCloud, Backblaze B2, whatever — and is willing to write the code. You
don't need to ask me first: the app was built so that this is one new file, not surgery.

> If you just want to use a server of your own (Nextcloud, a Synology or QNAP NAS,
> ownCloud, Box), **you don't have to write anything**: WebDAV is already there, under
> Settings → Where to save. This guide is for clouds that speak their own API.

*Em português: [NUVENS.md](NUVENS.md).*

---

## How big the job actually is

One new file in `core/`, **twelve functions**, and three lines to register them. Nothing
else in the app changes — not the game list, not the syncing, not the conflict handling,
not `.nxsaves`.

That works because the hard part is already written and belongs to no cloud in particular.
Uploading a whole folder recursively, mirroring it, deleting what disappeared from the
console, pulling the tree back down — all of that lives in [`core/cloud.c`](core/cloud.c)
and is written **exactly once**, on top of seven primitives. It used to be half of
`drive.c` and there wasn't a line of Google in it.

You implement the seven primitives. The rest you get for free.

---

## Step 1 — copy the closest neighbour

Don't start from scratch. Pick by the shape of the **id**:

| Your cloud identifies a file by... | Copy | Why |
| --- | --- | --- |
| **path** (`/SwitchSaveSync/Zelda/save.dat`) | [`core/webdav.c`](core/webdav.c) | Already treats the id as a path, and it's the smaller of the two |
| **opaque id** (`1a2B3c4D...`) | [`core/drive.c`](core/drive.c) | Already handles opaque ids, paging and OAuth |

Dropbox and OneDrive accept both styles. I'd take the **opaque id** in both: renaming a
folder from the cloud's website then breaks nothing, because the id doesn't change.

Call the file `core/onedrive.c` and `core/onedrive.h`, in the same shape as its neighbours.

> **You don't need to touch a Makefile.** All three (`gui`, `app`, `sysmodule`) list
> `SOURCES` as a *directory*, not a file list. A new `.c` in `core/` joins the build on its
> own.

---

## Step 2 — the twelve functions

The table below is the contract. It's declared in
[`core/cloud_backend.h`](core/cloud_backend.h) — read that too, the comments say more.

### The five about identity and state

| Field | What to return |
| --- | --- |
| `key` | The short word written into `nuvem.cfg`. Lowercase, no spaces: `"onedrive"`. Never change it after release, or everyone who picked that cloud silently falls back to Drive. |
| `name()` | The name on screen: `"OneDrive"`. It's a **function**, not a fixed string, because it goes through `TR()` — and a static struct field can't hold a choice made at runtime. |
| `is_ready()` | `true` if it's usable right now (login saved, server configured). The screen shows ready clouds differently from ones still to be set up. |
| `setup_hint()` | One short sentence saying what's missing when `is_ready()` is false: `"Not signed in yet"`. It shows under the name. |
| `logout()` | Wipe the saved login from the SD card. It must leave `is_ready()` false. |

### The one about the session

| Field | What to return |
| --- | --- |
| `begin(auth, authsz)` | Open a session and write into `auth` whatever the other functions will need. On Drive that's the `access_token`; on WebDAV it's the ready-made `Basic` header. The buffer is `CLOUD_AUTH_MAX` (2048) — Google's token is over 1 KB, so don't shrink that. This is also where you refresh an expired token. |

### The seven primitives

All of them take the `auth` from `begin()` as their first argument.

| Field | Contract |
| --- | --- |
| `root(auth, id_out, outsz)` | The id of the app's root folder, **creating it if it doesn't exist**. It's the only one allowed to create without being asked. |
| `find_child(auth, parent_id, name, want_folder, id_out, outsz)` | Find a direct child of `parent_id` named `name`. `want_folder` separates folders from files — without it, a game named the same as a file would be confused for it. **Not finding it is not an error**: return `false` and move on. |
| `make_folder(auth, parent_id, name, id_out, outsz)` | Create the folder and return its id. |
| `put_file(auth, parent_id, name, local_path, mime)` | Upload the local file. **If one with that name is already there, replace it** — don't duplicate. This is mandatory: without it, every sync would leave another copy behind. |
| `get_file(auth, id, local_path)` | Download to the local path. |
| `remove(auth, id)` | Take it off the cloud. **Prefer a trash bin over a hard delete** if your cloud has one: this touches save backups, and a mistake has to be undoable. |
| `list(auth, folder_id, cb, userdata)` | Call `cb` once per direct child, with `(id, name, is_folder, userdata)`. **Handle paging** — nearly every API returns the first N plus a cursor for the rest. A folder of 300 saves listed halfway turns into deleted saves during `prune`. |

Put it all in a `CloudBackend` instance and return the pointer, like the bottom of
`webdav.c`:

```c
static const CloudBackend g_onedrive_backend = {
    .key         = "onedrive",
    .name        = onedrive_be_name,
    .is_ready    = onedrive_be_is_ready,
    .setup_hint  = onedrive_be_setup_hint,
    .logout      = onedrive_be_logout,
    .begin       = onedrive_be_begin,
    .root        = onedrive_be_root,
    .find_child  = onedrive_be_find_child,
    .make_folder = onedrive_be_make_folder,
    .put_file    = onedrive_be_put_file,
    .get_file    = onedrive_be_get_file,
    .remove      = onedrive_be_remove,
    .list        = onedrive_be_list,
};

const CloudBackend *onedrive_backend(void) { return &g_onedrive_backend; }
```

---

## Step 3 — register it, in three lines

**1.** In [`core/cloud.h`](core/cloud.h), in the `CloudKind` enum, **before** `CLOUD_COUNT`:

```c
typedef enum
{
    CLOUD_DRIVE = 0,
    CLOUD_WEBDAV,
    CLOUD_ONEDRIVE,   // <-- here
    CLOUD_COUNT,
} CloudKind;
```

> Order matters and can't be shuffled later. Anyone with a saved `nuvem.cfg` is found again
> by `key`, not by number — but **always append at the end**, before `CLOUD_COUNT`. Slipping
> one into the middle changes the value of everything after it.

**2.** In [`core/cloud_backend.h`](core/cloud_backend.h), the declaration:

```c
const CloudBackend *onedrive_backend(void);
```

**3.** In [`core/cloud.c`](core/cloud.c), inside `backend_of()`:

```c
case CLOUD_ONEDRIVE: return onedrive_backend();
```

That's it. `cloud_name()`, `cloud_is_ready()`, `cloud_set_current()` and the `nuvem.cfg`
handling already walk the whole enum — they learn about your cloud without you touching
them.

---

## Step 4 — the screen

In `gui/source/main.cpp`, in the **Settings** tab, under "Where to save", there's one row
per cloud. Copy the `g_webdav_item` pattern: a row that opens that cloud's setup screen.

If your cloud signs in with a device code, the Google login screen is a ready-made
template — it shows the code, the link and a QR code, then waits. If it's username and
password, the WebDAV screen is the template.

---

## Step 5 — testing without a console

This is the part that saves the most time, and it's easy to miss that it exists at all.

**The whole of `core/` runs on a Mac or on Linux.** The trick is two `#define`s:
`SYNC_APP_DIR`, in [`core/syncstate.h`](core/syncstate.h), is `"sdmc:/switch/SwitchSaveSync"`,
and `CA_BUNDLE_PATH`, in [`core/http.c`](core/http.c), is `"romfs:/cacert.pem"`. Neither
starts with `/` — so on a computer they are **relative paths**. Create directories with
those literal names (`sdmc:` and `romfs:`, colon and all) and the core code runs unmodified,
TLS certificate verification included.

Look at the `drive` block in [`tests/run.sh`](tests/run.sh): it builds those directories,
points `romfs:/cacert.pem` at the project's own certificate bundle, and runs the suite
against the real cloud. Copy that block, swapping `test_drive.c` for yours.

```sh
./tests/run.sh            # everything that needs no account and no internet
./tests/run.sh drive      # talks to the real Google Drive
```

It all compiles with **ASan and UBSan on**. A buffer overrun that on the console becomes an
unexplained crash becomes, here, a message with a line number.

> **One rule, and it isn't negotiable:** only test deletion inside an isolated test folder.
> `cloud_prune_extras` and the backend's `remove` really do delete. Never point a
> destructive test at somebody's save folder.

---

# OneDrive, concretely

OneDrive works, and it's the closest to what already exists: the **same device flow** as
Google, and on top of that, no client secret.

## Registering the app (free)

1. [portal.azure.com](https://portal.azure.com) → **Microsoft Entra ID** → **App
   registrations** → **New registration**.
2. Account types: **"Accounts in any organizational directory and personal Microsoft
   accounts"** — without this, a personal account (which is the one with ordinary OneDrive)
   can't sign in.
3. Under **Authentication**, turn on **"Allow public client flows"**. That switch is what
   enables the device flow.
4. Under **API permissions** → Microsoft Graph → **Delegated permissions**, add
   `Files.ReadWrite.AppFolder` and `offline_access`.
5. Note the **Application (client) ID**. **No secret needed** — public clients don't use
   one, and that's a real advantage over Google here.

> Use `Files.ReadWrite.AppFolder`, not `Files.ReadWrite`. It's the equivalent of Google's
> `drive.file`: the app sees only its own folder and nothing else in the person's OneDrive.
> Asking for more than you need is how a save-sync app becomes an app that reads everything.

## Signing in (device flow)

Ask for the code — `{tenant}` is `consumers` for a personal account, `common` if you also
want to accept work accounts:

```
POST https://login.microsoftonline.com/consumers/oauth2/v2.0/devicecode
Content-Type: application/x-www-form-urlencoded

client_id=YOUR_ID&scope=Files.ReadWrite.AppFolder%20offline_access
```

Back come `user_code` (what the person types), `verification_uri` (where they type it),
`device_code` (what you keep), `expires_in` and `interval`.

Then you poll, waiting `interval` seconds between attempts:

```
POST https://login.microsoftonline.com/consumers/oauth2/v2.0/token
Content-Type: application/x-www-form-urlencoded

grant_type=urn:ietf:params:oauth:grant-type:device_code&client_id=YOUR_ID&device_code=...
```

Until the person finishes, you get errors — and each one wants a different reaction:

| Error | What to do |
| --- | --- |
| `authorization_pending` | Normal. Wait and ask again. |
| `authorization_declined` | They refused. Stop polling. |
| `expired_token` | Past `expires_in`. Stop, and offer to start over. |
| `bad_verification_code` | You sent the wrong `device_code`. That's your bug. |

On success you get an `access_token` (good for about an hour) and a `refresh_token` (the one
you write to the SD card, because it's the one that survives). The `refresh_token` only
comes if you asked for `offline_access` in the scope.

[`core/oauth.c`](core/oauth.c) already does exactly this dance for Google, including the
waiting, cancelling with B, and refreshing. That's the file to copy.

## The seven primitives in Microsoft Graph

Base: `https://graph.microsoft.com/v1.0`, with `Authorization: Bearer <access_token>`.

| Primitive | Call |
| --- | --- |
| `root` | `GET /me/drive/special/approot` → the `id` field. The folder is created the first time you call it. |
| `find_child` | `GET /me/drive/items/{parent}:/{name}` — a 404 means "not there", which is not an error. To tell a folder from a file, check whether the object carries the `folder` facet. |
| `make_folder` | `POST /me/drive/items/{parent}/children` with `{"name":"X","folder":{},"@microsoft.graph.conflictBehavior":"fail"}` |
| `put_file` | `PUT /me/drive/items/{parent}:/{name}:/content` — **see the size warning below** |
| `get_file` | `GET /me/drive/items/{id}/content` — answers **302** with a temporary download URL, so curl has to follow redirects (`CURLOPT_FOLLOWLOCATION`). |
| `remove` | `DELETE /me/drive/items/{id}` — goes to the OneDrive recycle bin, which is the behaviour we want. |
| `list` | `GET /me/drive/items/{id}/children` — **it pages**. While the response carries `@odata.nextLink`, fetch the next page. |

### Two traps that will catch you

**1. A plain PUT only goes up to 4 MB.** Above that Graph refuses, and Switch saves pass
that without trying — Zelda, Animal Crossing, any big game. For anything larger you need an
upload session: `POST /me/drive/items/{parent}:/{name}:/createUploadSession`, then send
chunks with `PUT` to the `uploadUrl` you got back, each carrying a `Content-Range`. Use
multiples of 320 KiB for the chunks — that's what Microsoft asks for.

Don't fall for testing with a small save only and concluding it works.

**2. `Content-Type` is mandatory on the PUT.** Without it Graph answers 400 — or worse,
accepts it and stores a corrupted file. Use `application/octet-stream`. `put_file` already
receives a `mime_type` parameter; pass it through.

---

# Dropbox, concretely

Also doable, and the simplest of the three: the `/2/files` API works by **path**, not by id,
so `find_child` becomes string concatenation and `root` is a constant.

1. Create an app at [dropbox.com/developers](https://www.dropbox.com/developers) → **Scoped
   access** → **App folder** (not "Full Dropbox" — same idea as
   `Files.ReadWrite.AppFolder`: the app only ever sees its own folder).
2. Permissions: `files.content.write` and `files.content.read`.
3. Dropbox's device flow is more limited than Microsoft's; the normal route is PKCE. If you
   don't want a browser screen on the console, you can generate a long-lived token in the
   dashboard and have the person paste it — ugly, but honest, and the typing screen already
   exists in the WebDAV setup.

Endpoints: `https://api.dropboxapi.com/2/files/*` for metadata operations (create folder,
list, delete) and `https://content.dropboxapi.com/2/files/*` for upload and download. There's
paging here too: `list_folder` returns a `cursor`, and you call `list_folder/continue` while
`has_more` is true.

---

# iCloud Drive: it can't be done

And it isn't for lack of trying.

Apple has no public third-party API for iCloud Drive. What exists is **CloudKit**, which only
answers to an app carrying an Apple identity, and **CloudKit Web Services**, which requires a
paid **Apple Developer Program** membership. That would run straight into the one
non-negotiable rule of this project: **zero cost for whoever uses it**.

If that ever changes, the place to work is the same as for the others: a `core/icloud.c` and
three lines of registration.

---

## Before you send the PR

- `./tests/run.sh` passing (that's the floor, and it needs no account to run).
- A test of your own in the shape of `tests/test_drive.c`, pointed **only** at a test folder.
- `make -C gui` with no new warnings.
- If your cloud needs a credential, it goes in `core/config.h` — which is gitignored. Put
  the how-to-get-yours steps in `core/config.h.example`, the way it's done there for Google.
- **Never commit a credential**, not even "just to test". If one slips out, treat it as burnt
  and issue another: bots sweeping GitHub commits for keys work in minutes, not days.

Open an [issue](https://github.com/NspxMiguel/SwitchSaveSync/issues) if you get stuck —
preferably before writing the whole thing, since course corrections are cheaper early.
