#include "drive.h"
#include "cloud_backend.h"
#include "oauth.h"
#include "cloud.h"   // so pelo CLOUD_AUTH_MAX, no upload resumivel
#include "http.h"
#include "minijson.h"
#include "config.h"
#include "lang.h"

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

#define DRIVE_FOLDER_MIME "application/vnd.google-apps.folder"

// Procura um arquivo/pasta por nome. parent_id_or_null restringe a busca a
// dentro de uma pasta; mime_type_or_null restringe pelo mimeType (igual a
// ele, ou diferente dele quando mime_negate — é assim que se pede "qualquer
// coisa MENOS pasta", que não tem como escrever com '='). Devolve o id do
// primeiro resultado.
// Procura por nome. Devolve true se ACHOU.
//
// `erro_out` (pode ser NULL) separa as duas respostas que antes eram a mesma:
// "não existe" e "não deu pra perguntar". Um 500 do Google ou o Wi-Fi do
// console engasgando devolvia false igual a "não existe" — e quem chama usava
// isso pra decidir entre atualizar o arquivo que já está lá e CRIAR outro. O
// Drive aceita nome repetido na mesma pasta, então uma engasgada no meio de um
// backup deixava dois "save.dat" lá dentro; no restore seguinte, os dois caem
// no mesmo caminho do staging e sobrescreve quem chegou por último — ordem da
// API, não data. Se o antigo ganhar, o savedata é apagado e regravado com o
// save velho por cima do bom.
static bool find_by_name(const char *access_token, const char *name,
                          const char *parent_id_or_null, const char *mime_type_or_null,
                          bool mime_negate,
                          char *id_out, size_t outsz, bool *erro_out) {
    if (erro_out) *erro_out = false;
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
        qi += snprintf(query + qi, sizeof(query) - qi, " and mimeType%s'%s'",
                       mime_negate ? "!=" : "=", mime_type_or_null);
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
    } else if (erro_out) {
        *erro_out = true; // a resposta não chegou: "não existe" seria chute
    }
    http_response_free(&r);
    return found;
}

static bool ensure_folder(const char *access_token, const char *parent_id_or_null,
                           const char *name, char *out, size_t outsz) {
    bool erro = false;
    if (find_by_name(access_token, name, parent_id_or_null,
                      DRIVE_FOLDER_MIME, false, out, outsz, &erro)) {
        return true;
    }

    // A busca falhou (não é "não achei", é "não deu pra perguntar"). Criar aqui
    // faria uma SEGUNDA pasta com o mesmo nome, e o save do jogo passaria a
    // viver dividido entre as duas.
    if (erro)
        return false;

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
                        DRIVE_FOLDER_MIME, false, out, outsz, NULL);
}

// Acima disto, o multipart nao serve.
//
// O Google recusa uploadType=multipart passando de 5 MB, e o nosso multipart
// ainda por cima monta o corpo inteiro na RAM (malloc do tamanho do arquivo) —
// o que no sysmodule, que tem 8 MB de heap TOTAL dividida com curl e mbedtls,
// e fatal bem antes dos 5 MB. Save de jogo grande batia nos dois limites.
//
// 4 MB, e nao 5, pra ter folga: o corpo multipart carrega o metadado e as
// fronteiras junto com os bytes do arquivo.
#define DRIVE_MULTIPART_MAX (4 * 1024 * 1024)

// Upload resumivel: dois passos.
//
//   1. POST (ou PATCH) so com o metadado -> o Google devolve uma URI de sessao
//      no cabecalho Location;
//   2. PUT dos bytes nessa URI, lendo do disco em pedacos.
//
// O passo 2 e o http_put_file_raw, que ja transmite direto do FILE* via
// CURLOPT_READDATA — memoria constante, nao importa se o arquivo tem 6 MB ou
// 6 GB. Era o que o caminho do WebDAV sempre fez e o do Drive nao fazia.
static bool drive_upload_resumable(const char *access_token, const char *folder_id,
                                    const char *remote_name, const char *local_path,
                                    const char *mime_type, bool updating,
                                    const char *existing_id) {
    char name_json[300];
    json_escape(remote_name, name_json, sizeof(name_json));

    char metadata[500];
    char url[300];
    if (updating) {
        snprintf(metadata, sizeof(metadata), "{\"name\":\"%s\"}", name_json);
        snprintf(url, sizeof(url),
                 "https://www.googleapis.com/upload/drive/v3/files/%s?uploadType=resumable",
                 existing_id);
    } else {
        snprintf(metadata, sizeof(metadata), "{\"name\":\"%s\",\"parents\":[\"%s\"]}",
                 name_json, folder_id);
        snprintf(url, sizeof(url),
                 "https://www.googleapis.com/upload/drive/v3/files?uploadType=resumable");
    }

    char session[1200] = {0};
    HttpResponse r = http_json_get_location(updating ? "PATCH" : "POST", url,
                                             access_token, metadata,
                                             session, sizeof(session));
    bool abriu = r.ok && r.status == 200 && session[0] != '\0';
    http_response_free(&r);
    if (!abriu) return false;

    // O bearer vai como header cru porque o http_put_file_raw monta
    // "Authorization: <o que vier>" — entao o "Bearer " entra aqui.
    char bearer[CLOUD_AUTH_MAX + 8];
    snprintf(bearer, sizeof(bearer), "Bearer %s", access_token);

    HttpResponse p = http_put_file_raw(session, bearer, local_path, mime_type);
    bool ok = p.ok && (p.status == 200 || p.status == 201);
    http_response_free(&p);
    return ok;
}

static long tamanho_do_arquivo(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return n;
}

bool drive_upload(const char *access_token, const char *folder_id,
                   const char *remote_name, const char *local_path,
                   const char *mime_type) {
    char existing_id[128] = {0};
    bool erro     = false;
    bool updating = find_by_name(access_token, remote_name, folder_id, NULL, false,
                                  existing_id, sizeof(existing_id), &erro);

    // Sem saber se o arquivo já está lá, subir é apostar: acertando, sobrescreve;
    // errando, cria um duplicado com o mesmo nome. Falhar agora custa um item
    // do backup, que a próxima passada refaz — o duplicado, não.
    if (erro)
        return false;

    long fsize = tamanho_do_arquivo(local_path);
    if (fsize < 0) return false;
    if (fsize > DRIVE_MULTIPART_MAX)
        return drive_upload_resumable(access_token, folder_id, remote_name, local_path,
                                       mime_type, updating, existing_id);

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
    if (!find_by_name(access_token, remote_name, folder_id, NULL, false, file_id, sizeof(file_id), NULL)) {
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
    // LIMITACAO CONHECIDA: sem paginação via nextPageToken. Acima de 1000
    // itens numa pasta, o resto fica de fora. Save de jogo não chega perto
    // disso, mas está dito.
    //
    // O outro teto, esse sim mordia: até a lista era copiada
    // inteira pra um buffer de 8 KB na pilha, e o copiador NÃO trunca — ele
    // devolve false quando não cabe. Ou seja, pasta com mais de ~60 arquivos
    // não listava PARCIALMENTE: não listava NADA, e download e prune falhavam
    // junto, sem dizer por quê. Agora o array é percorrido direto no corpo da
    // resposta, sem cópia e sem teto.

    HttpResponse r = http_get(url, access_token);
    if (!r.ok || r.status != 200) {
        http_response_free(&r);
        return false;
    }

    const char *files = json_value_at(r.body, "files");
    if (!files || *files != '[') {
        // Sem a chave "files", ou ela não é um array: o Drive respondeu 200 com
        // outra coisa. Pasta vazia devolve "files": [], que passa reto daqui.
        http_response_free(&r);
        return false;
    }

    const char *cursor;
    if (!json_array_begin(files, &cursor)) {
        http_response_free(&r);
        return true; // array vazio = pasta vazia
    }

    // O 'cursor' aponta pra DENTRO do r.body, então a resposta tem que
    // continuar viva até o laço acabar — só depois é que ela é liberada.
    char elem[1024];
    while (json_array_next(&cursor, elem, sizeof(elem))) {
        char id[128] = {0}, name[300] = {0}, mime[128] = {0};
        json_get_string(elem, "id", id, sizeof(id));
        json_get_string(elem, "name", name, sizeof(name));
        json_get_string(elem, "mimeType", mime, sizeof(mime));
        bool is_folder = strcmp(mime, DRIVE_FOLDER_MIME) == 0;
        if (id[0] && cb) cb(id, name, is_folder, userdata);
    }

    http_response_free(&r);
    return true;
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

// --- o adaptador pro cloud.c ------------------------------------------------
//
// As sete funcoes que qualquer nuvem precisa saber fazer. Percorrer arvore,
// espelhar e avisar a UI nao estao mais aqui: nao tinham nada de Google e
// foram pro cloud.c, que faz isso por cima destas.

static const char *drive_be_name(void) {
    return "Google Drive"; // nome proprio, igual nas duas linguas
}

static bool drive_be_is_ready(void) {
    return oauth_is_logged_in();
}

static const char *drive_be_setup_hint(void) {
    if (oauth_is_logged_in()) return "";
    return TR("falta entrar na conta Google, aqui embaixo",
              "you still have to sign in to the Google account, below");
}

static void drive_be_logout(void) {
    oauth_logout();
}

static bool drive_be_begin(char *auth, size_t authsz) {
    return oauth_get_fresh_access_token(auth, authsz);
}

static bool drive_be_root(const char *auth, char *id_out, size_t outsz) {
    return ensure_folder(auth, NULL, DRIVE_APP_FOLDER_NAME, id_out, outsz);
}

static bool drive_be_find_child(const char *auth, const char *parent_id, const char *name,
                                 bool want_folder, char *id_out, size_t outsz) {
    // "qualquer coisa menos pasta" em vez de "sem filtro": um jogo e o
    // arquivo .nxsaves dele podem ter o mesmo nome dentro da mesma pasta, e
    // sem o filtro a busca por arquivo podia devolver a pasta.
    return find_by_name(auth, name, parent_id, DRIVE_FOLDER_MIME, !want_folder,
                        id_out, outsz, NULL);
}

static bool drive_be_make_folder(const char *auth, const char *parent_id, const char *name,
                                  char *id_out, size_t outsz) {
    return ensure_folder(auth, parent_id, name, id_out, outsz);
}

static bool drive_be_put_file(const char *auth, const char *parent_id, const char *name,
                               const char *local_path, const char *mime_type) {
    return drive_upload(auth, parent_id, name, local_path, mime_type);
}

static bool drive_be_get_file(const char *auth, const char *id, const char *local_path) {
    return drive_download_by_id(auth, id, local_path);
}

static bool drive_be_remove(const char *auth, const char *id) {
    return drive_trash_by_id(auth, id);
}

static bool drive_be_list(const char *auth, const char *folder_id,
                           cloud_list_cb cb, void *userdata) {
    return drive_list_children(auth, folder_id, (drive_list_cb)cb, userdata);
}

static const CloudBackend g_drive_backend = {
    .key         = "drive",
    .name        = drive_be_name,
    .is_ready    = drive_be_is_ready,
    .setup_hint  = drive_be_setup_hint,
    .logout      = drive_be_logout,
    .begin       = drive_be_begin,
    .root        = drive_be_root,
    .find_child  = drive_be_find_child,
    .make_folder = drive_be_make_folder,
    .put_file    = drive_be_put_file,
    .get_file    = drive_be_get_file,
    .remove      = drive_be_remove,
    .list        = drive_be_list,
};

const CloudBackend *drive_backend(void) {
    return &g_drive_backend;
}
