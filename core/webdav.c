// webdav.c — o backend de nuvem que fala WebDAV.
//
// Diferença de fundo pro Drive: lá um id é opaco e a única forma de achar
// algo é perguntar pra API; aqui o id É o caminho. Então "achar a pasta do
// jogo X" não é uma busca, é montar a string — o que deixa este arquivo mais
// curto que o drive.c mesmo fazendo o mesmo serviço.
//
// AVISO, e ele importa: no Drive o cloud_prune_extras manda pra lixeira, e a
// lixeira do Google guarda 30 dias. Aqui é DELETE. Nextcloud, ownCloud e
// Synology têm lixeira própria e pegam esse DELETE; um servidor WebDAV pelado
// (apache mod_dav, rclone serve webdav, caixinha de NAS simples) apaga na
// hora e não tem volta. Está dito na tela de Ajustes também.

#include "webdav.h"
#include "cloud_backend.h"
#include "http.h"
#include "syncstate.h"
#include "config.h"
#include "lang.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <strings.h>

#define WEBDAV_CFG_PATH SYNC_APP_DIR "/webdav.cfg"

// A mesma pasta que o Drive usa, pelo mesmo motivo de sempre: se ele trocar
// de nuvem, quer achar as coisas com o nome que já conhece.
#define WEBDAV_ROOT_NAME DRIVE_APP_FOLDER_NAME

// --- os dados de acesso -----------------------------------------------------

static bool         g_cfg_lida = false;
static bool         g_cfg_tem  = false;
static WebdavConfig g_cfg;

static void tira_fim_de_linha(char *s) {
    for (char *p = s; *p; p++) {
        if (*p == '\r' || *p == '\n') { *p = '\0'; return; }
    }
}

static void ler_cfg(void) {
    if (g_cfg_lida) return;
    g_cfg_lida = true;
    memset(&g_cfg, 0, sizeof(g_cfg));

    FILE *f = fopen(WEBDAV_CFG_PATH, "r");
    if (!f) return;

    char linha[WEBDAV_URL_MAX + 64];
    while (fgets(linha, sizeof(linha), f)) {
        tira_fim_de_linha(linha);
        char *eq = strchr(linha, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *chave = linha, *valor = eq + 1;

        if (strcmp(chave, "url") == 0)  snprintf(g_cfg.url,  sizeof(g_cfg.url),  "%s", valor);
        else if (strcmp(chave, "user") == 0) snprintf(g_cfg.user, sizeof(g_cfg.user), "%s", valor);
        else if (strcmp(chave, "pass") == 0) snprintf(g_cfg.pass, sizeof(g_cfg.pass), "%s", valor);
    }
    fclose(f);

    // Barra no fim atrapalha: tudo daqui pra frente monta "base + /coisa".
    size_t n = strlen(g_cfg.url);
    while (n > 0 && g_cfg.url[n - 1] == '/') g_cfg.url[--n] = '\0';

    g_cfg_tem = g_cfg.url[0] != '\0';
}

bool webdav_get_config(WebdavConfig *out) {
    ler_cfg();
    if (out) *out = g_cfg;
    return g_cfg_tem;
}

void webdav_set_config(const WebdavConfig *cfg) {
    if (!cfg) return;

    syncstate_ensure_dirs();
    FILE *f = fopen(WEBDAV_CFG_PATH, "w");
    if (f) {
        // A senha fica em texto puro no cartão, igual ao token do Google no
        // token.txt. Não dá pra fazer melhor sem um lugar seguro pra guardar
        // chave, que o Switch não oferece pra homebrew. O jeito certo de usar
        // isso é com uma "senha de app" do Nextcloud/Synology, que é
        // revogável e não é a senha da conta — está escrito na tela.
        fprintf(f, "url=%s\nuser=%s\npass=%s\n", cfg->url, cfg->user, cfg->pass);
        fclose(f);
    }

    g_cfg = *cfg;
    size_t n = strlen(g_cfg.url);
    while (n > 0 && g_cfg.url[n - 1] == '/') g_cfg.url[--n] = '\0';
    g_cfg_tem  = g_cfg.url[0] != '\0';
    g_cfg_lida = true;
}

void webdav_clear_config(void) {
    remove(WEBDAV_CFG_PATH);
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg_tem  = false;
    g_cfg_lida = true;
}

// --- base64, url encode/decode ----------------------------------------------

static void base64(const char *in, char *out, size_t outsz) {
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t len = strlen(in), oi = 0;
    for (size_t i = 0; i < len; i += 3) {
        unsigned v = (unsigned char)in[i] << 16;
        if (i + 1 < len) v |= (unsigned char)in[i + 1] << 8;
        if (i + 2 < len) v |= (unsigned char)in[i + 2];

        if (oi + 5 > outsz) break;
        out[oi++] = T[(v >> 18) & 63];
        out[oi++] = T[(v >> 12) & 63];
        out[oi++] = (i + 1 < len) ? T[(v >> 6) & 63] : '=';
        out[oi++] = (i + 2 < len) ? T[v & 63]        : '=';
    }
    out[oi < outsz ? oi : outsz - 1] = '\0';
}

// Escapa pra caber numa URL, mas deixa a '/' passar — o que entra aqui é um
// caminho inteiro, e escapar as barras faria o servidor procurar um arquivo
// com barra no nome.
static void encode_path(const char *in, char *out, size_t outsz) {
    size_t oi = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && oi + 4 < outsz; p++) {
        if (isalnum(*p) || *p == '-' || *p == '_' || *p == '.' || *p == '~' || *p == '/') {
            out[oi++] = (char)*p;
        } else {
            oi += snprintf(out + oi, outsz - oi, "%%%02X", *p);
        }
    }
    out[oi < outsz ? oi : outsz - 1] = '\0';
}

static int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

// Desfaz o encode_path e, de quebra, os quatro escapes de XML que aparecem
// dentro de <href> (o & vira &amp; mesmo já estando percent-encoded).
static void decode_path(const char *in, size_t inlen, char *out, size_t outsz) {
    size_t oi = 0;
    for (size_t i = 0; i < inlen && oi + 1 < outsz; i++) {
        if (in[i] == '%' && i + 2 < inlen) {
            int hi = hexval(in[i + 1]), lo = hexval(in[i + 2]);
            if (hi >= 0 && lo >= 0) { out[oi++] = (char)(hi * 16 + lo); i += 2; continue; }
        }
        if (in[i] == '&') {
            if (inlen - i >= 5 && strncmp(in + i, "&amp;", 5) == 0)  { out[oi++] = '&'; i += 4; continue; }
            if (inlen - i >= 4 && strncmp(in + i, "&lt;", 4) == 0)   { out[oi++] = '<'; i += 3; continue; }
            if (inlen - i >= 4 && strncmp(in + i, "&gt;", 4) == 0)   { out[oi++] = '>'; i += 3; continue; }
            if (inlen - i >= 6 && strncmp(in + i, "&quot;", 6) == 0) { out[oi++] = '"'; i += 5; continue; }
        }
        out[oi++] = in[i];
    }
    out[oi < outsz ? oi : outsz - 1] = '\0';
}

// --- endereços --------------------------------------------------------------
//
// A configuração é a URL inteira da pasta base ("https://nas/remote.php/dav/
// files/miguel"). Um id, aqui, é o CAMINHO absoluto no servidor
// ("/remote.php/dav/files/miguel/Nintendo Switch Saves/Zelda"), sem escapar —
// é o que o PROPFIND devolve depois de decodificado, e é legível no log.

// Onde acaba o "https://host:porta" da URL configurada.
static size_t tam_origem(const char *url) {
    const char *p = strstr(url, "://");
    if (!p) return 0;
    const char *barra = strchr(p + 3, '/');
    return barra ? (size_t)(barra - url) : strlen(url);
}

static void origem(char *out, size_t outsz) {
    size_t n = tam_origem(g_cfg.url);
    if (n == 0 || n >= outsz) { out[0] = '\0'; return; }
    memcpy(out, g_cfg.url, n);
    out[n] = '\0';
}

// O caminho da pasta base, sem o host.
static void caminho_base(char *out, size_t outsz) {
    size_t n = tam_origem(g_cfg.url);
    snprintf(out, outsz, "%s", g_cfg.url + n);
    if (out[0] == '\0') snprintf(out, outsz, "/");
}

// Caminho -> URL completa, já escapada.
static void url_de(const char *caminho, char *out, size_t outsz) {
    char host[WEBDAV_URL_MAX];
    origem(host, sizeof(host));

    char esc[CLOUD_ID_MAX * 3];
    encode_path(caminho, esc, sizeof(esc));

    snprintf(out, outsz, "%s%s", host, esc);
}

static void junta(const char *pai, const char *nome, char *out, size_t outsz) {
    size_t n = strlen(pai);
    bool tem_barra = n > 0 && pai[n - 1] == '/';
    snprintf(out, outsz, "%s%s%s", pai, tem_barra ? "" : "/", nome);
}

// O último pedaço de um caminho.
static const char *nome_do_caminho(const char *caminho) {
    const char *barra = strrchr(caminho, '/');
    return barra ? barra + 1 : caminho;
}

// --- XML do PROPFIND --------------------------------------------------------

// Acha a próxima tag de abertura com esse nome local. "Local" porque cada
// servidor usa o prefixo de namespace que quer (d:, D:, lp1:, ou nenhum) —
// comparar "d:href" na mão só funcionaria com um deles.
static const char *proxima_tag(const char *p, const char *nome, const char **depois) {
    size_t nlen = strlen(nome);
    while (p && *p) {
        if (*p != '<') { p++; continue; }
        const char *q = p + 1;
        if (*q == '/' || *q == '?' || *q == '!') { p++; continue; }

        const char *ini = q;
        while (*q && *q != '>' && *q != ' ' && *q != '/' && *q != '\t' && *q != '\n') q++;

        const char *local = ini;
        for (const char *r = ini; r < q; r++) {
            if (*r == ':') local = r + 1;
        }

        if ((size_t)(q - local) == nlen && strncasecmp(local, nome, nlen) == 0) {
            while (*q && *q != '>') q++;
            if (*q == '>') q++;
            if (depois) *depois = q;
            return p;
        }
        p++;
    }
    return NULL;
}

typedef void (*webdav_item_cb)(const char *caminho, bool pasta, void *ud);

// Percorre a resposta de um PROPFIND chamando cb pra cada <href>.
//
// Pasta ou arquivo sai do <resourcetype><collection/>, procurado entre este
// href e o próximo: no XML de qualquer servidor WebDAV o resourcetype vem
// dentro do mesmo <response> e depois do href.
static void percorre_propfind(const char *xml, webdav_item_cb cb, void *ud) {
    const char *p = xml;
    while (p && *p) {
        const char *conteudo = NULL;
        const char *tag = proxima_tag(p, "href", &conteudo);
        if (!tag) return;

        const char *fim = strchr(conteudo, '<');
        if (!fim) return;

        // Até onde vale procurar o "collection" deste item.
        const char *prox = proxima_tag(fim, "href", NULL);
        const char *limite = prox ? prox : (conteudo + strlen(conteudo));

        bool pasta = false;
        const char *col = proxima_tag(fim, "collection", NULL);
        if (col && col < limite) pasta = true;

        char caminho[CLOUD_ID_MAX];
        decode_path(conteudo, (size_t)(fim - conteudo), caminho, sizeof(caminho));

        // Barra no fim: o servidor põe em pasta, e o resto do arquivo trata
        // caminho de pasta sem barra.
        size_t n = strlen(caminho);
        while (n > 1 && caminho[n - 1] == '/') caminho[--n] = '\0';

        // href pode vir como URL inteira em vez de caminho.
        const char *so_caminho = caminho;
        if (strncmp(caminho, "http", 4) == 0) {
            const char *pp = strstr(caminho, "://");
            if (pp) {
                const char *barra = strchr(pp + 3, '/');
                so_caminho = barra ? barra : "/";
            }
        }

        if (cb) cb(so_caminho, pasta, ud);
        p = fim;
    }
}

// --- as operações do backend ------------------------------------------------

#define PROPFIND_BODY \
    "<?xml version=\"1.0\" encoding=\"utf-8\"?>" \
    "<d:propfind xmlns:d=\"DAV:\"><d:prop><d:resourcetype/></d:prop></d:propfind>"

static HttpResponse propfind(const char *auth, const char *caminho, int profundidade) {
    char url[WEBDAV_URL_MAX * 4];
    url_de(caminho, url, sizeof(url));

    const char *depth = (profundidade == 0) ? "Depth: 0" : "Depth: 1";
    return http_request_raw("PROPFIND", url, auth, "application/xml; charset=utf-8",
                             PROPFIND_BODY, strlen(PROPFIND_BODY), depth);
}

static const char *webdav_be_name(void) {
    // Curto de proposito: este nome entra no meio de frase ("Subindo pro
    // %s...") e no seletor da tela, onde nome comprido é cortado. A lista de
    // quem fala WebDAV está escrita na tela de Ajustes, que é onde cabe.
    return TR("Servidor WebDAV", "WebDAV server");
}

static bool webdav_be_is_ready(void) {
    ler_cfg();
    return g_cfg_tem;
}

static const char *webdav_be_setup_hint(void) {
    ler_cfg();
    if (g_cfg_tem) return "";
    return TR("falta o endereço do servidor, aqui embaixo",
              "the server address is still missing, below");
}

static void webdav_be_logout(void) {
    webdav_clear_config();
}

static bool webdav_be_begin(char *auth, size_t authsz) {
    ler_cfg();
    if (!g_cfg_tem) return false;

    char par[WEBDAV_USER_MAX + WEBDAV_PASS_MAX + 2];
    snprintf(par, sizeof(par), "%s:%s", g_cfg.user, g_cfg.pass);

    char b64[((WEBDAV_USER_MAX + WEBDAV_PASS_MAX + 2) * 4) / 3 + 8];
    base64(par, b64, sizeof(b64));

    snprintf(auth, authsz, "Basic %s", b64);
    return true;
}

// MKCOL numa pasta que já existe devolve 405, e isso conta como sucesso: o
// que se queria é que ela exista.
static bool mkcol(const char *auth, const char *caminho) {
    char url[WEBDAV_URL_MAX * 4];
    url_de(caminho, url, sizeof(url));

    HttpResponse r = http_request_raw("MKCOL", url, auth, NULL, NULL, 0, NULL);
    bool ok = r.ok && (r.status == 201 || r.status == 405 || r.status == 301);
    http_response_free(&r);
    return ok;
}

static bool webdav_be_root(const char *auth, char *id_out, size_t outsz) {
    ler_cfg();
    if (!g_cfg_tem) return false;

    char base[CLOUD_ID_MAX];
    caminho_base(base, sizeof(base));
    junta(base, WEBDAV_ROOT_NAME, id_out, outsz);

    return mkcol(auth, id_out);
}

static bool webdav_be_find_child(const char *auth, const char *parent_id, const char *name,
                                  bool want_folder, char *id_out, size_t outsz) {
    char caminho[CLOUD_ID_MAX];
    junta(parent_id, name, caminho, sizeof(caminho));

    HttpResponse r = propfind(auth, caminho, 0);
    bool existe = r.ok && r.status == 207;
    bool pasta = false;

    if (existe) {
        // Depth 0 devolve um <response> só, o do próprio item.
        const char *col = proxima_tag(r.body, "collection", NULL);
        pasta = col != NULL;
    }
    http_response_free(&r);

    if (!existe || pasta != want_folder) return false;

    snprintf(id_out, outsz, "%s", caminho);
    return true;
}

static bool webdav_be_make_folder(const char *auth, const char *parent_id, const char *name,
                                   char *id_out, size_t outsz) {
    char caminho[CLOUD_ID_MAX];
    junta(parent_id, name, caminho, sizeof(caminho));
    if (!mkcol(auth, caminho)) return false;

    snprintf(id_out, outsz, "%s", caminho);
    return true;
}

static bool webdav_be_put_file(const char *auth, const char *parent_id, const char *name,
                                const char *local_path, const char *mime_type) {
    char caminho[CLOUD_ID_MAX];
    junta(parent_id, name, caminho, sizeof(caminho));

    char url[WEBDAV_URL_MAX * 4];
    url_de(caminho, url, sizeof(url));

    // PUT por cima já substitui — WebDAV não tem "criar" e "atualizar"
    // separados como o Drive.
    HttpResponse r = http_put_file_raw(url, auth, local_path,
                                        mime_type ? mime_type : "application/octet-stream");
    bool ok = r.ok && (r.status == 200 || r.status == 201 || r.status == 204);
    http_response_free(&r);
    return ok;
}

static bool webdav_be_get_file(const char *auth, const char *id, const char *local_path) {
    char url[WEBDAV_URL_MAX * 4];
    url_de(id, url, sizeof(url));
    return http_download_to_file_raw(url, auth, local_path);
}

static bool webdav_be_remove(const char *auth, const char *id) {
    char url[WEBDAV_URL_MAX * 4];
    url_de(id, url, sizeof(url));

    HttpResponse r = http_request_raw("DELETE", url, auth, NULL, NULL, 0, NULL);

    // 404 conta como sucesso: o que se queria é que não esteja mais lá.
    //
    // 207 NÃO conta, e contava. Num DELETE de coleção o 207 é exatamente a
    // resposta de "apaguei o resto, mas ESTES aqui não deu" (RFC 4918 §9.6.1) —
    // um arquivo travado ou sem permissão sai de lá dentro embrulhado num 207, e
    // não como 423 no status. Aceitando isso como sucesso, o prune dizia que
    // limpou, o marcador de "sincronizado" era gravado, e o sync seguinte via a
    // nuvem mais nova e ressuscitava dentro do savedata o arquivo que o jogo
    // tinha apagado. O caminho de falhar já existe e é bom: o syncjob avisa que
    // sobrou coisa na nuvem e não marca nada.
    bool ok = r.ok && (r.status == 200 || r.status == 204 || r.status == 404);
    http_response_free(&r);
    return ok;
}

typedef struct
{
    const char   *pasta;      // a pasta que foi pedida, pra pular ela mesma
    cloud_list_cb cb;
    void         *ud;
} ListCtx;

static void list_item(const char *caminho, bool pasta, void *ud) {
    ListCtx *ctx = (ListCtx *)ud;

    // O primeiro <response> de um PROPFIND Depth 1 é a própria pasta.
    if (strcmp(caminho, ctx->pasta) == 0) return;

    // O id é o caminho: sem busca, sem id opaco.
    if (ctx->cb) ctx->cb(caminho, nome_do_caminho(caminho), pasta, ctx->ud);
}

static bool webdav_be_list(const char *auth, const char *folder_id,
                            cloud_list_cb cb, void *userdata) {
    HttpResponse r = propfind(auth, folder_id, 1);
    if (!r.ok || r.status != 207) {
        http_response_free(&r);
        return false;
    }

    ListCtx ctx = { folder_id, cb, userdata };
    percorre_propfind(r.body, list_item, &ctx);
    http_response_free(&r);
    return true;
}

// --- teste de conexão -------------------------------------------------------

bool webdav_test_connection(char *erro, size_t errosz) {
    ler_cfg();
    if (!g_cfg_tem) {
        snprintf(erro, errosz, "falta preencher o endereço do servidor");
        return false;
    }
    if (!http_is_ready()) {
        snprintf(erro, errosz, "a rede não subiu nesta sessão");
        return false;
    }

    char auth[CLOUD_AUTH_MAX];
    if (!webdav_be_begin(auth, sizeof(auth))) {
        snprintf(erro, errosz, "não montei o login");
        return false;
    }

    char base[CLOUD_ID_MAX];
    caminho_base(base, sizeof(base));

    HttpResponse r = propfind(auth, base, 0);
    bool ok = false;

    if (!r.ok) {
        snprintf(erro, errosz, "%s", r.error[0] ? r.error : "não cheguei no servidor");
    } else if (r.status == 207) {
        ok = true;
    } else if (r.status == 401) {
        snprintf(erro, errosz, "usuário ou senha recusados (401)");
    } else if (r.status == 404) {
        snprintf(erro, errosz, "o endereço respondeu, mas essa pasta não existe (404)");
    } else if (r.status == 405 || r.status == 501) {
        snprintf(erro, errosz, "o servidor respondeu, mas não fala WebDAV (%ld)", r.status);
    } else {
        snprintf(erro, errosz, "o servidor respondeu %ld", r.status);
    }
    http_response_free(&r);
    return ok;
}

// --- a tabela ---------------------------------------------------------------

static const CloudBackend g_webdav_backend = {
    .key         = "webdav",
    .name        = webdav_be_name,
    .is_ready    = webdav_be_is_ready,
    .setup_hint  = webdav_be_setup_hint,
    .logout      = webdav_be_logout,
    .begin       = webdav_be_begin,
    .root        = webdav_be_root,
    .find_child  = webdav_be_find_child,
    .make_folder = webdav_be_make_folder,
    .put_file    = webdav_be_put_file,
    .get_file    = webdav_be_get_file,
    .remove      = webdav_be_remove,
    .list        = webdav_be_list,
};

const CloudBackend *webdav_backend(void) {
    return &g_webdav_backend;
}
