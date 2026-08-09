#include "drive.h"
#include "http.h"
#include "minijson.h"
#include "config.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <dirent.h>
#include <sys/stat.h>

static void url_encode(const char *in, char *out, size_t outsz) {
    size_t oi = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && oi + 4 < outsz; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~') {
            out[oi++] = (char)*p;
        } else {
            oi += snprintf(out + oi, outsz - oi, "%%%02X", *p);
        }
    }
    out[oi < outsz ? oi : outsz - 1] = '\0';
}

// Escapa " \ e \n pra poder embutir uma string arbitrária (ex: nome de
// jogo vindo do NACP) dentro de um corpo JSON sem quebrar o parsing do
// lado do Google.
static void json_escape(const char *in, char *out, size_t outsz) {
    size_t oi = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && oi + 2 < outsz; p++) {
        if (*p == '"' || *p == '\\') {
            out[oi++] = '\\';
            out[oi++] = (char)*p;
        } else if (*p == '\n') {
            out[oi++] = '\\';
            out[oi++] = 'n';
        } else {
            out[oi++] = (char)*p;
        }
    }
    out[oi < outsz ? oi : outsz - 1] = '\0';
}

// Procura um arquivo/pasta por nome. parent_id_or_null restringe a busca a
// dentro de uma pasta; mime_type_or_null restringe pelo mimeType exato
// (usado pra "só pastas"). Devolve o id do primeiro resultado.
static bool find_by_name(const char *access_token, const char *name,
                          const char *parent_id_or_null, const char *mime_type_or_null,
                          char *id_out, size_t outsz) {
    char name_escaped[256];
    size_t ni = 0;
    for (const char *p = name; *p && ni + 2 < sizeof(name_escaped); p++) {
        if (*p == '\'' || *p == '\\') name_escaped[ni++] = '\\';
        name_escaped[ni++] = *p;
    }
    name_escaped[ni] = '\0';

    char query[700];
    int qi = snprintf(query, sizeof(query), "name='%s' and trashed=false", name_escaped);
    if (parent_id_or_null && qi > 0 && (size_t)qi < sizeof(query)) {
        qi += snprintf(query + qi, sizeof(query) - qi, " and '%s' in parents", parent_id_or_null);
    }
    if (mime_type_or_null && qi > 0 && (size_t)qi < sizeof(query)) {
        qi += snprintf(query + qi, sizeof(query) - qi, " and mimeType='%s'", mime_type_or_null);
    }

    // url_encode troca boa parte dos caracteres por "%XX" (3x o tamanho).
    // query[] tem até 700 bytes, então no pior caso (quase tudo escapado)
    // precisa de ~2100 bytes pra não truncar a query no meio — foi um bug
    // real aqui (buffer de 1100 cortava o filtro de mimeType sem avisar,
    // não travava, só dava resultado errado).
    char query_enc[2200];
    url_encode(query, query_enc, sizeof(query_enc));

    char url[2600];
    snprintf(url, sizeof(url),
             "https://www.googleapis.com/drive/v3/files?q=%s&fields=files(id,name)&pageSize=1",
             query_enc);

    HttpResponse r = http_get(url, access_token);
    bool found = false;
    if (r.ok && r.status == 200) {
        char files_block[512];
        if (json_get_block(r.body, "files", files_block, sizeof(files_block))) {
            char first[300];
            if (json_first_element(files_block, first, sizeof(first))) {
                found = json_get_string(first, "id", id_out, outsz);
            }
        }
    }
    http_response_free(&r);
    return found;
}

static bool ensure_folder(const char *access_token, const char *parent_id_or_null,
                           const char *name, char *out, size_t outsz) {
    if (find_by_name(access_token, name, parent_id_or_null,
                      "application/vnd.google-apps.folder", out, outsz)) {
        return true;
    }

    char name_json[300];
    json_escape(name, name_json, sizeof(name_json));

    char body[500];
    if (parent_id_or_null) {
        snprintf(body, sizeof(body),
                 "{\"name\":\"%s\",\"mimeType\":\"application/vnd.google-apps.folder\",\"parents\":[\"%s\"]}",
                 name_json, parent_id_or_null);
    } else {
        snprintf(body, sizeof(body),
                 "{\"name\":\"%s\",\"mimeType\":\"application/vnd.google-apps.folder\"}",
                 name_json);
    }

    HttpResponse r = http_post_json("https://www.googleapis.com/drive/v3/files", access_token, body);
    bool ok = false;
    if (r.ok && (r.status == 200 || r.status == 201)) {
        ok = json_get_string(r.body, "id", out, outsz);
    }
    http_response_free(&r);
    return ok;
}

bool drive_ensure_app_folder(const char *access_token, char *folder_id_out, size_t outsz) {
    return ensure_folder(access_token, NULL, DRIVE_APP_FOLDER_NAME, folder_id_out, outsz);
}

bool drive_ensure_subfolder(const char *access_token, const char *parent_id,
                             const char *name, char *out, size_t outsz) {
    return ensure_folder(access_token, parent_id, name, out, outsz);
}

// A versão que NÃO cria. Existe por causa de um estrago real: o restore usava
// "ensure", e "ensure" cria a pasta quando não acha. Jogo sem backup na nuvem
// virava pasta vazia recém-criada em vez de "não tem backup", e o restore
// seguia em frente e commitava por cima de um save bom. Quem lê, usa esta.
bool drive_find_subfolder(const char *access_token, const char *parent_id,
                           const char *name, char *out, size_t outsz) {
    return find_by_name(access_token, name, parent_id,
                        "application/vnd.google-apps.folder", out, outsz);
}

bool drive_upload(const char *access_token, const char *folder_id,
                   const char *remote_name, const char *local_path,
                   const char *mime_type) {
    char existing_id[128] = {0};
    bool updating = find_by_name(access_token, remote_name, folder_id, NULL,
                                  existing_id, sizeof(existing_id));

    char name_json[300];
    json_escape(remote_name, name_json, sizeof(name_json));

    char metadata[500];
    char url[300];
    if (updating) {
        // No PATCH de mídia não se reenvia "parents" (ignorado/rejeitado);
        // metadata pode ficar só com o nome, ou vazia — mandamos o nome por
        // clareza no log da API.
        snprintf(metadata, sizeof(metadata), "{\"name\":\"%s\"}", name_json);
        snprintf(url, sizeof(url),
                 "https://www.googleapis.com/upload/drive/v3/files/%s?uploadType=multipart",
                 existing_id);
    } else {
        snprintf(metadata, sizeof(metadata), "{\"name\":\"%s\",\"parents\":[\"%s\"]}",
                 name_json, folder_id);
        snprintf(url, sizeof(url),
                 "https://www.googleapis.com/upload/drive/v3/files?uploadType=multipart");
    }

    HttpResponse r = http_upload_multipart_related(url, access_token,
                                                     updating ? "PATCH" : "POST",
                                                     metadata, local_path, mime_type);
    bool ok = r.ok && (r.status == 200 || r.status == 201);
    http_response_free(&r);
    return ok;
}

bool drive_download(const char *access_token, const char *folder_id,
                     const char *remote_name, const char *local_path) {
    char file_id[128] = {0};
    if (!find_by_name(access_token, remote_name, folder_id, NULL, file_id, sizeof(file_id))) {
        return false;
    }
    return drive_download_by_id(access_token, file_id, local_path);
}

bool drive_download_by_id(const char *access_token, const char *file_id, const char *local_path) {
    char url[256];
    snprintf(url, sizeof(url), "https://www.googleapis.com/drive/v3/files/%s?alt=media", file_id);
    return http_download_to_file(url, access_token, local_path);
}

bool drive_list_children(const char *access_token, const char *folder_id,
                          drive_list_cb cb, void *userdata) {
    char query[300];
    snprintf(query, sizeof(query), "'%s' in parents and trashed=false", folder_id);
    char query_enc[600];
    url_encode(query, query_enc, sizeof(query_enc));

    char url[900];
    snprintf(url, sizeof(url),
             "https://www.googleapis.com/drive/v3/files?q=%s&fields=files(id,name,mimeType)&pageSize=1000",
             query_enc);
    // LIMITACAO CONHECIDA: sem paginação via nextPageToken. Se uma pasta
    // tiver mais itens do que cabem no buffer files_block abaixo, os
    // excedentes somem da listagem silenciosamente. Não deveria acontecer
    // com pasta de save de jogo (poucos arquivos), mas é bom saber que
    // existe esse teto.

    HttpResponse r = http_get(url, access_token);
    if (!r.ok || r.status != 200) {
        http_response_free(&r);
        return false;
    }

    char files_block[8192];
    bool got_block = json_get_block(r.body, "files", files_block, sizeof(files_block));
    http_response_free(&r);
    if (!got_block) return false;

    const char *cursor;
    if (!json_array_begin(files_block, &cursor)) return true; // array vazio = pasta vazia

    char elem[900];
    while (json_array_next(&cursor, elem, sizeof(elem))) {
        char id[128] = {0}, name[300] = {0}, mime[128] = {0};
        json_get_string(elem, "id", id, sizeof(id));
        json_get_string(elem, "name", name, sizeof(name));
        json_get_string(elem, "mimeType", mime, sizeof(mime));
        bool is_folder = strcmp(mime, "application/vnd.google-apps.folder") == 0;
        if (id[0] && cb) cb(id, name, is_folder, userdata);
    }
    return true;
}

static drive_progress_cb g_progress_cb = NULL;

void drive_set_progress_cb(drive_progress_cb cb) {
    g_progress_cb = cb;
}

static void report(const char *action, const char *name, bool ok) {
    if (g_progress_cb) g_progress_cb(action, name, ok);
}

static drive_abort_cb g_abort_cb = NULL;

void drive_set_abort_cb(drive_abort_cb cb) {
    g_abort_cb = cb;
}

static bool aborted(void) {
    return g_abort_cb && g_abort_cb();
}

bool drive_upload_tree(const char *access_token, const char *parent_folder_id,
                        const char *local_dir) {
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
            char sub_id[128];
            if (!drive_ensure_subfolder(access_token, parent_folder_id, entry->d_name,
                                         sub_id, sizeof(sub_id))) {
                report("dir", entry->d_name, false);
                all_ok = false;
                continue;
            }
            report("dir", entry->d_name, true);
            if (!drive_upload_tree(access_token, sub_id, full_path)) all_ok = false;
        } else {
            bool ok = drive_upload(access_token, parent_folder_id, entry->d_name, full_path,
                                    "application/octet-stream");
            report("up", entry->d_name, ok);
            if (!ok) all_ok = false;
        }
    }
    closedir(d);
    return all_ok;
}

bool drive_trash_by_id(const char *access_token, const char *file_id) {
    char url[300];
    snprintf(url, sizeof(url), "https://www.googleapis.com/drive/v3/files/%s", file_id);

    // PATCH {"trashed":true} e não DELETE: o DELETE da API v3 apaga na hora,
    // sem passar pela lixeira. Isso aqui mexe em save de jogo — se eu errar a
    // conta de quem está mais novo, o arquivo tem que ter volta.
    HttpResponse r = http_patch_json(url, access_token, "{\"trashed\":true}");
    // 404 conta como sucesso: o objetivo é "isso não está mais na pasta", e
    // não estar lá já satisfaz.
    bool ok = r.ok && (r.status == 200 || r.status == 204 || r.status == 404);
    http_response_free(&r);
    return ok;
}

typedef struct {
    const char *access_token;
    const char *local_dir;
    bool ok;
} PruneCtx;

static void prune_item_cb(const char *id, const char *name, bool is_folder, void *userdata) {
    PruneCtx *ctx = (PruneCtx *)userdata;
    if (aborted()) { ctx->ok = false; return; }

    char child_path[700];
    snprintf(child_path, sizeof(child_path), "%s/%s", ctx->local_dir, name);

    struct stat st;
    bool aqui  = stat(child_path, &st) == 0;
    bool pasta = aqui && S_ISDIR(st.st_mode);

    if (!aqui || pasta != is_folder) {
        bool ok = drive_trash_by_id(ctx->access_token, id);
        report("del", name, ok);
        if (!ok) ctx->ok = false;
        return;
    }

    if (is_folder && !drive_prune_extras(ctx->access_token, id, child_path)) ctx->ok = false;
}

bool drive_prune_extras(const char *access_token, const char *folder_id,
                         const char *local_dir) {
    PruneCtx ctx = { access_token, local_dir, true };
    if (!drive_list_children(access_token, folder_id, prune_item_cb, &ctx)) return false;
    return ctx.ok;
}

typedef struct {
    const char *access_token;
    const char *local_dir;
    bool ok;
} DownloadTreeCtx;

static void download_tree_item_cb(const char *id, const char *name, bool is_folder, void *userdata) {
    DownloadTreeCtx *ctx = (DownloadTreeCtx *)userdata;
    if (aborted()) { ctx->ok = false; return; }

    char child_path[700];
    snprintf(child_path, sizeof(child_path), "%s/%s", ctx->local_dir, name);

    if (is_folder) {
        mkdir(child_path, 0777);
        report("dir", name, true);
        if (!drive_download_tree(ctx->access_token, id, child_path)) ctx->ok = false;
    } else {
        bool ok = drive_download_by_id(ctx->access_token, id, child_path);
        report("down", name, ok);
        if (!ok) ctx->ok = false;
    }
}

bool drive_download_tree(const char *access_token, const char *folder_id,
                          const char *local_dir) {
    mkdir(local_dir, 0777); // ignora erro se já existe

    DownloadTreeCtx ctx = { access_token, local_dir, true };
    if (!drive_list_children(access_token, folder_id, download_tree_item_cb, &ctx)) return false;
    return ctx.ok;
}
