// cloud.c — a parte que não muda de nuvem pra nuvem.
//
// Percorrer árvore, espelhar, avisar a UI, parar no meio: nada disso é
// Google. Estava tudo no drive.c e veio pra cá inteiro, trocando as chamadas
// diretas pelos ponteiros do CloudBackend. O drive.c continua fazendo o que
// só o Drive sabe (montar a query da API v3, o multipart, a lixeira).

#include "cloud.h"
#include "cloud_backend.h"
#include "syncstate.h"

#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#define CLOUD_CFG_PATH SYNC_APP_DIR "/nuvem.cfg"

static const CloudBackend *backend_of(CloudKind kind)
{
    switch (kind) {
        case CLOUD_WEBDAV: return webdav_backend();
        case CLOUD_DRIVE:
        default:           return drive_backend();
    }
}

// --- qual nuvem está valendo ------------------------------------------------

static bool     g_kind_lida = false;
static CloudKind g_kind      = CLOUD_DRIVE;

static void ler_kind(void)
{
    if (g_kind_lida) return;
    g_kind_lida = true;

    FILE *f = fopen(CLOUD_CFG_PATH, "r");
    if (!f) return; // sem arquivo = Google Drive, que é como sempre foi

    char linha[64] = {0};
    if (fgets(linha, sizeof(linha), f)) {
        for (char *p = linha; *p; p++) {
            if (*p == '\r' || *p == '\n') { *p = '\0'; break; }
        }
        for (int k = 0; k < CLOUD_COUNT; k++) {
            if (strcmp(linha, backend_of((CloudKind)k)->key) == 0) {
                g_kind = (CloudKind)k;
                break;
            }
        }
    }
    fclose(f);
}

CloudKind cloud_current(void)
{
    ler_kind();
    return g_kind;
}

void cloud_set_current(CloudKind kind)
{
    if ((int)kind < 0 || (int)kind >= CLOUD_COUNT) return;

    syncstate_ensure_dirs();
    FILE *f = fopen(CLOUD_CFG_PATH, "w");
    if (f) {
        fprintf(f, "%s\n", backend_of(kind)->key);
        fclose(f);
    }

    g_kind = kind;
    g_kind_lida = true;
}

const char *cloud_name(CloudKind kind)
{
    if ((int)kind < 0 || (int)kind >= CLOUD_COUNT) return "?";
    return backend_of(kind)->name();
}

const char *cloud_key(CloudKind kind)
{
    if ((int)kind < 0 || (int)kind >= CLOUD_COUNT) return "?";
    return backend_of(kind)->key;
}

bool cloud_is_ready(CloudKind kind)
{
    if ((int)kind < 0 || (int)kind >= CLOUD_COUNT) return false;
    return backend_of(kind)->is_ready();
}

const char *cloud_setup_hint(CloudKind kind)
{
    if ((int)kind < 0 || (int)kind >= CLOUD_COUNT) return "";
    return backend_of(kind)->setup_hint();
}

void cloud_logout(CloudKind kind)
{
    if ((int)kind < 0 || (int)kind >= CLOUD_COUNT) return;
    backend_of(kind)->logout();
}

static const CloudBackend *atual(void)
{
    return backend_of(cloud_current());
}

// --- avisos pra UI ----------------------------------------------------------

static cloud_progress_cb g_progress_cb = NULL;
static cloud_abort_cb    g_abort_cb    = NULL;

void cloud_set_progress_cb(cloud_progress_cb cb) { g_progress_cb = cb; }
void cloud_set_abort_cb(cloud_abort_cb cb)       { g_abort_cb = cb; }

static void report(const char *action, const char *name, bool ok)
{
    if (g_progress_cb) g_progress_cb(action, name, ok);
}

static bool aborted(void)
{
    return g_abort_cb && g_abort_cb();
}

// --- as operações -----------------------------------------------------------

bool cloud_begin(char *auth, size_t authsz)
{
    if (!auth || authsz == 0) return false;
    auth[0] = '\0';
    return atual()->begin(auth, authsz);
}

bool cloud_ensure_app_folder(const char *auth, char *id_out, size_t outsz)
{
    return atual()->root(auth, id_out, outsz);
}

bool cloud_find_subfolder(const char *auth, const char *parent_id,
                           const char *name, char *id_out, size_t outsz)
{
    return atual()->find_child(auth, parent_id, name, true, id_out, outsz);
}

bool cloud_ensure_subfolder(const char *auth, const char *parent_id,
                             const char *name, char *id_out, size_t outsz)
{
    const CloudBackend *b = atual();
    if (b->find_child(auth, parent_id, name, true, id_out, outsz)) return true;
    return b->make_folder(auth, parent_id, name, id_out, outsz);
}

bool cloud_upload(const char *auth, const char *folder_id, const char *remote_name,
                   const char *local_path, const char *mime_type)
{
    return atual()->put_file(auth, folder_id, remote_name, local_path, mime_type);
}

bool cloud_download(const char *auth, const char *folder_id, const char *remote_name,
                     const char *local_path)
{
    const CloudBackend *b = atual();

    char id[CLOUD_ID_MAX];
    if (!b->find_child(auth, folder_id, remote_name, false, id, sizeof(id))) return false;
    return b->get_file(auth, id, local_path);
}

bool cloud_list_children(const char *auth, const char *folder_id,
                          cloud_item_cb cb, void *userdata)
{
    return atual()->list(auth, folder_id, cb, userdata);
}

bool cloud_upload_tree(const char *auth, const char *folder_id, const char *local_dir)
{
    DIR *d = opendir(local_dir);
    if (!d) return false;

    bool all_ok = true;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (aborted()) { all_ok = false; break; }

        char full_path[700];
        snprintf(full_path, sizeof(full_path), "%s/%s", local_dir, entry->d_name);

        struct stat st;
        if (stat(full_path, &st) != 0) {
            all_ok = false;
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            char sub_id[CLOUD_ID_MAX];
            if (!cloud_ensure_subfolder(auth, folder_id, entry->d_name,
                                         sub_id, sizeof(sub_id))) {
                report("dir", entry->d_name, false);
                all_ok = false;
                continue;
            }
            report("dir", entry->d_name, true);
            if (!cloud_upload_tree(auth, sub_id, full_path)) all_ok = false;
        } else {
            bool ok = cloud_upload(auth, folder_id, entry->d_name, full_path,
                                    "application/octet-stream");
            report("up", entry->d_name, ok);
            if (!ok) all_ok = false;
        }
    }
    closedir(d);
    return all_ok;
}

typedef struct
{
    const char *auth;
    const char *local_dir;
    bool ok;
} TreeCtx;

static void prune_item_cb(const char *id, const char *name, bool is_folder, void *userdata)
{
    TreeCtx *ctx = (TreeCtx *)userdata;
    if (aborted()) { ctx->ok = false; return; }

    char child_path[700];
    snprintf(child_path, sizeof(child_path), "%s/%s", ctx->local_dir, name);

    struct stat st;
    bool aqui  = stat(child_path, &st) == 0;
    bool pasta = aqui && S_ISDIR(st.st_mode);

    if (!aqui || pasta != is_folder) {
        bool ok = atual()->remove(ctx->auth, id);
        report("del", name, ok);
        if (!ok) ctx->ok = false;
        return;
    }

    if (is_folder && !cloud_prune_extras(ctx->auth, id, child_path)) ctx->ok = false;
}

bool cloud_prune_extras(const char *auth, const char *folder_id, const char *local_dir)
{
    TreeCtx ctx = { auth, local_dir, true };
    if (!atual()->list(auth, folder_id, prune_item_cb, &ctx)) return false;
    return ctx.ok;
}

static void download_tree_item_cb(const char *id, const char *name, bool is_folder, void *userdata)
{
    TreeCtx *ctx = (TreeCtx *)userdata;
    if (aborted()) { ctx->ok = false; return; }

    char child_path[700];
    snprintf(child_path, sizeof(child_path), "%s/%s", ctx->local_dir, name);

    if (is_folder) {
        mkdir(child_path, 0777);
        report("dir", name, true);
        if (!cloud_download_tree(ctx->auth, id, child_path)) ctx->ok = false;
    } else {
        bool ok = atual()->get_file(ctx->auth, id, child_path);
        report("down", name, ok);
        if (!ok) ctx->ok = false;
    }
}

bool cloud_download_tree(const char *auth, const char *folder_id, const char *local_dir)
{
    mkdir(local_dir, 0777); // ignora erro se já existe

    TreeCtx ctx = { auth, local_dir, true };
    if (!atual()->list(auth, folder_id, download_tree_item_cb, &ctx)) return false;
    return ctx.ok;
}
