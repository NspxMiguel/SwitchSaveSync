#include "http.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// CA bundle da Mozilla embutido no romfs (ver romfs/cacert.pem). Sem isso o
// handshake TLS falha, porque o Switch não expõe um trust store utilizável
// pra libcurl/mbedTLS.
//
// NOTA PRA TESTAR: isso assume que fopen("romfs:/cacert.pem", ...) funciona
// por baixo do capô do curl+mbedTLS (o devoptab do romfs registrado pela
// libnx deveria deixar isso transparente). Se o build reclamar de TLS/CAINFO
// na hora H, o plano B é copiar romfs:/cacert.pem pra sdmc:/switch/
// SwitchSaveSync/cacert.pem no primeiro boot e apontar o CAINFO pra lá.
#define CA_BUNDLE_PATH "romfs:/cacert.pem"

static bool g_http_ready = false;

bool http_init(void) {
    if (g_http_ready) return true;
    CURLcode rc = curl_global_init(CURL_GLOBAL_DEFAULT);
    g_http_ready = (rc == CURLE_OK);
    return g_http_ready;
}

void http_shutdown(void) {
    if (!g_http_ready) return;
    curl_global_cleanup();
    g_http_ready = false;
}

void http_response_free(HttpResponse *r) {
    if (!r) return;
    free(r->body);
    r->body = NULL;
    r->size = 0;
}

typedef struct {
    char *data;
    size_t size;
} MemBuf;

static size_t write_to_membuf(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t realsize = size * nmemb;
    MemBuf *mem = (MemBuf *)userp;
    char *ptr = (char *)realloc(mem->data, mem->size + realsize + 1);
    if (!ptr) return 0; // aborta a transferência se a memória acabar
    mem->data = ptr;
    memcpy(&(mem->data[mem->size]), contents, realsize);
    mem->size += realsize;
    mem->data[mem->size] = '\0';
    return realsize;
}

bool http_is_ready(void) { return g_http_ready; }

static CURL *make_easy_handle(void) {
    // Guarda que evita crash de verdade: no Switch, mexer em socket sem ter
    // chamado socketInitialize não devolve erro — aborta o processo. Se o
    // http_init não passou (socket não subiu, curl_global_init falhou), toda
    // chamada tem que morrer AQUI, antes de encostar na rede.
    if (!g_http_ready) return NULL;

    CURL *curl = curl_easy_init();
    if (!curl) return NULL;
    curl_easy_setopt(curl, CURLOPT_CAINFO, CA_BUNDLE_PATH);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "SwitchSaveSync/0.1 (+homebrew)");
    return curl;
}

static void fill_error(HttpResponse *resp, CURLcode rc) {
    resp->ok = false;
    resp->status = 0;
    snprintf(resp->error, sizeof(resp->error), "curl: %s", curl_easy_strerror(rc));
}

HttpResponse http_post_form(const char *url, const char *fields) {
    HttpResponse resp = {0};
    MemBuf buf = {0};

    CURL *curl = make_easy_handle();
    if (!curl) {
        resp.ok = false;
        snprintf(resp.error, sizeof(resp.error), http_is_ready() ? "curl_easy_init falhou" : "rede nao iniciou nesta sessao");
        return resp;
    }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/x-www-form-urlencoded");

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, fields);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_membuf);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        fill_error(&resp, rc);
    } else {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        resp.ok = true;
        resp.status = status;
        resp.body = buf.data ? buf.data : strdup("");
        resp.size = buf.size;
        buf.data = NULL; // evita duplo free (ownership passou pra resp.body)
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(buf.data);
    return resp;
}

static struct curl_slist *auth_header(const char *bearer, struct curl_slist *headers) {
    if (!bearer) return headers;
    // 2600 e nao 600: cabe o CLOUD_AUTH_MAX (2048) mais o "Authorization: Bearer ".
    // Com 600 um token grande era truncado em silencio pelo snprintf, e o que
    // chegava no servidor era um header pela metade.
    char line[2600];
    snprintf(line, sizeof(line), "Authorization: Bearer %s", bearer);
    return curl_slist_append(headers, line);
}

HttpResponse http_get(const char *url, const char *bearer) {
    HttpResponse resp = {0};
    MemBuf buf = {0};

    CURL *curl = make_easy_handle();
    if (!curl) {
        resp.ok = false;
        snprintf(resp.error, sizeof(resp.error), http_is_ready() ? "curl_easy_init falhou" : "rede nao iniciou nesta sessao");
        return resp;
    }

    struct curl_slist *headers = auth_header(bearer, NULL);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_membuf);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        fill_error(&resp, rc);
    } else {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        resp.ok = true;
        resp.status = status;
        resp.body = buf.data ? buf.data : strdup("");
        resp.size = buf.size;
        buf.data = NULL;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(buf.data);
    return resp;
}

HttpResponse http_patch_json(const char *url, const char *bearer, const char *json_body) {
    HttpResponse resp = {0};
    MemBuf buf = {0};

    CURL *curl = make_easy_handle();
    if (!curl) {
        resp.ok = false;
        snprintf(resp.error, sizeof(resp.error), http_is_ready() ? "curl_easy_init falhou" : "rede nao iniciou nesta sessao");
        return resp;
    }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json; charset=UTF-8");
    headers = auth_header(bearer, headers);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    // CUSTOMREQUEST depois do POSTFIELDS: o POSTFIELDS sozinho manda POST, e
    // o CUSTOMREQUEST só troca o verbo, mantendo o corpo.
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_membuf);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        fill_error(&resp, rc);
    } else {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        resp.ok = true;
        resp.status = status;
        resp.body = buf.data ? buf.data : strdup("");
        resp.size = buf.size;
        buf.data = NULL;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(buf.data);
    return resp;
}

HttpResponse http_post_json(const char *url, const char *bearer, const char *json_body) {
    HttpResponse resp = {0};
    MemBuf buf = {0};

    CURL *curl = make_easy_handle();
    if (!curl) {
        resp.ok = false;
        snprintf(resp.error, sizeof(resp.error), http_is_ready() ? "curl_easy_init falhou" : "rede nao iniciou nesta sessao");
        return resp;
    }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json; charset=UTF-8");
    headers = auth_header(bearer, headers);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_membuf);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        fill_error(&resp, rc);
    } else {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        resp.ok = true;
        resp.status = status;
        resp.body = buf.data ? buf.data : strdup("");
        resp.size = buf.size;
        buf.data = NULL;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(buf.data);
    return resp;
}

// Monta o corpo multipart/related exigido pela Drive API (duas partes, sem
// Content-Disposition — é por isso que construímos na mão em vez de usar a
// API curl_mime_*, que por padrão gera multipart/form-data e o Drive rejeita
// com 400).
static char *build_multipart_related_body(const char *boundary,
                                           const char *metadata_json,
                                           const unsigned char *file_data,
                                           size_t file_size,
                                           const char *file_mime_type,
                                           size_t *out_size) {
    size_t head1_cap = strlen(metadata_json) + 128;
    size_t head2_cap = strlen(file_mime_type) + 64;
    size_t tail_cap = strlen(boundary) + 16;
    size_t total_cap = head1_cap + head2_cap + tail_cap + file_size + strlen(boundary) * 3 + 64;

    char *buf = (char *)malloc(total_cap);
    if (!buf) return NULL;

    int n = snprintf(buf, total_cap,
        "--%s\r\n"
        "Content-Type: application/json; charset=UTF-8\r\n\r\n"
        "%s\r\n"
        "--%s\r\n"
        "Content-Type: %s\r\n\r\n",
        boundary, metadata_json, boundary, file_mime_type);
    if (n < 0 || (size_t)n >= total_cap) { free(buf); return NULL; }

    size_t pos = (size_t)n;
    memcpy(buf + pos, file_data, file_size);
    pos += file_size;

    int n2 = snprintf(buf + pos, total_cap - pos, "\r\n--%s--", boundary);
    if (n2 < 0 || pos + (size_t)n2 >= total_cap) { free(buf); return NULL; }
    pos += (size_t)n2;

    *out_size = pos;
    return buf;
}

HttpResponse http_upload_multipart_related(const char *url, const char *bearer,
                                            const char *method,
                                            const char *metadata_json,
                                            const char *local_file_path,
                                            const char *file_mime_type) {
    HttpResponse resp = {0};

    FILE *f = fopen(local_file_path, "rb");
    if (!f) {
        resp.ok = false;
        snprintf(resp.error, sizeof(resp.error), "não abriu %s pra leitura", local_file_path);
        return resp;
    }
    fseek(f, 0, SEEK_END);
    long fsize = ftell(f);
    rewind(f);
    if (fsize < 0) {
        fclose(f);
        resp.ok = false;
        snprintf(resp.error, sizeof(resp.error), "ftell falhou em %s", local_file_path);
        return resp;
    }
    unsigned char *file_data = (unsigned char *)malloc((size_t)fsize > 0 ? (size_t)fsize : 1);
    if (!file_data) {
        fclose(f);
        resp.ok = false;
        snprintf(resp.error, sizeof(resp.error), "sem memória pra ler %s (%ld bytes)", local_file_path, fsize);
        return resp;
    }
    size_t read_bytes = fread(file_data, 1, (size_t)fsize, f);
    fclose(f);
    if (read_bytes != (size_t)fsize) {
        free(file_data);
        resp.ok = false;
        snprintf(resp.error, sizeof(resp.error), "leitura incompleta de %s", local_file_path);
        return resp;
    }

    const char *boundary = "SwitchSaveSyncBoundary7f3a91";
    size_t body_size = 0;
    char *body = build_multipart_related_body(boundary, metadata_json, file_data, read_bytes,
                                               file_mime_type, &body_size);
    free(file_data);
    if (!body) {
        resp.ok = false;
        snprintf(resp.error, sizeof(resp.error), "sem memória pra montar corpo multipart");
        return resp;
    }

    MemBuf buf = {0};
    CURL *curl = make_easy_handle();
    if (!curl) {
        free(body);
        resp.ok = false;
        snprintf(resp.error, sizeof(resp.error), http_is_ready() ? "curl_easy_init falhou" : "rede nao iniciou nesta sessao");
        return resp;
    }

    char content_type_header[128];
    snprintf(content_type_header, sizeof(content_type_header),
             "Content-Type: multipart/related; boundary=%s", boundary);

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, content_type_header);
    headers = auth_header(bearer, headers);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    if (method && strcmp(method, "PATCH") == 0) {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PATCH");
    }
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_size);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_membuf);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        fill_error(&resp, rc);
    } else {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        resp.ok = true;
        resp.status = status;
        resp.body = buf.data ? buf.data : strdup("");
        resp.size = buf.size;
        buf.data = NULL;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(buf.data);
    free(body);
    return resp;
}

// --- o que não é Google -----------------------------------------------------

// Igual ao auth_header de cima, mas com o valor do header vindo pronto de
// fora ("Basic ..." em vez de "Bearer ...").
static struct curl_slist *auth_header_raw(const char *auth_raw, struct curl_slist *headers) {
    if (!auth_raw) return headers;
    char line[2600]; // mesmo motivo do auth_header()

    snprintf(line, sizeof(line), "Authorization: %s", auth_raw);
    return curl_slist_append(headers, line);
}

static void no_handle(HttpResponse *resp) {
    resp->ok = false;
    snprintf(resp->error, sizeof(resp->error),
             http_is_ready() ? "curl_easy_init falhou" : "rede nao iniciou nesta sessao");
}

HttpResponse http_request_raw(const char *method, const char *url,
                               const char *auth_raw, const char *content_type,
                               const char *body, size_t body_len,
                               const char *extra_header) {
    HttpResponse resp = {0};
    MemBuf buf = {0};

    CURL *curl = make_easy_handle();
    if (!curl) { no_handle(&resp); return resp; }

    struct curl_slist *headers = NULL;
    if (content_type) {
        char line[160];
        snprintf(line, sizeof(line), "Content-Type: %s", content_type);
        headers = curl_slist_append(headers, line);
    }
    if (extra_header) headers = curl_slist_append(headers, extra_header);
    headers = auth_header_raw(auth_raw, headers);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    if (body) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, (long)body_len);
    }
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_membuf);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        fill_error(&resp, rc);
    } else {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        resp.ok = true;
        resp.status = status;
        resp.body = buf.data ? buf.data : strdup("");
        resp.size = buf.size;
        buf.data = NULL;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(buf.data);
    return resp;
}

// Cabecalho "Location:" da resposta — e por onde o Google devolve a URI de
// sessao do upload resumivel. curl entrega cabecalho por cabecalho aqui.
typedef struct { char *out; size_t sz; } LocationCap;

static size_t capture_location(char *buffer, size_t size, size_t nitems, void *userdata) {
    LocationCap *cap = (LocationCap *)userdata;
    size_t total = size * nitems;

    // Comparacao sem diferenciar maiuscula: o HTTP nao garante a grafia.
    const char *pref = "location:";
    if (total > 9 && cap->out) {
        size_t i = 0;
        while (i < 9 && (buffer[i] | 0x20) == pref[i]) i++;
        if (i == 9) {
            const char *v = buffer + 9;
            size_t n = total - 9;
            while (n > 0 && (*v == ' ' || *v == '\t')) { v++; n--; }
            while (n > 0 && (v[n - 1] == '\r' || v[n - 1] == '\n')) n--;
            if (n >= cap->sz) n = cap->sz - 1;
            memcpy(cap->out, v, n);
            cap->out[n] = '\0';
        }
    }
    return total;
}

// POST/PATCH com corpo JSON que devolve o Location. E o passo 1 do upload
// resumivel: manda so o metadado e recebe a URI pra onde os bytes vao.
HttpResponse http_json_get_location(const char *method, const char *url,
                                     const char *bearer, const char *json_body,
                                     char *location_out, size_t location_sz) {
    HttpResponse resp = {0};
    MemBuf buf = {0};

    if (location_out && location_sz) location_out[0] = '\0';

    CURL *curl = make_easy_handle();
    if (!curl) { no_handle(&resp); return resp; }

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json; charset=UTF-8");
    headers = auth_header(bearer, headers);

    LocationCap cap = { location_out, location_sz };

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json_body);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, capture_location);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &cap);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_membuf);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        fill_error(&resp, rc);
    } else {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        resp.ok = true;
        resp.status = status;
        resp.body = buf.data ? buf.data : strdup("");
        resp.size = buf.size;
        buf.data = NULL;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    free(buf.data);
    return resp;
}

HttpResponse http_put_file_raw(const char *url, const char *auth_raw,
                                const char *local_path, const char *content_type) {
    HttpResponse resp = {0};
    MemBuf buf = {0};

    FILE *in = fopen(local_path, "rb");
    if (!in) {
        resp.ok = false;
        snprintf(resp.error, sizeof(resp.error), "não abriu %s pra leitura", local_path);
        return resp;
    }
    fseek(in, 0, SEEK_END);
    long fsize = ftell(in);
    rewind(in);
    if (fsize < 0) {
        fclose(in);
        resp.ok = false;
        snprintf(resp.error, sizeof(resp.error), "ftell falhou em %s", local_path);
        return resp;
    }

    CURL *curl = make_easy_handle();
    if (!curl) { fclose(in); no_handle(&resp); return resp; }

    struct curl_slist *headers = NULL;
    if (content_type) {
        char line[160];
        snprintf(line, sizeof(line), "Content-Type: %s", content_type);
        headers = curl_slist_append(headers, line);
    }
    // Alguns servidores WebDAV respondem 417 se o curl mandar "Expect:
    // 100-continue" — que ele manda sozinho em upload grande. Desliga.
    headers = curl_slist_append(headers, "Expect:");
    headers = auth_header_raw(auth_raw, headers);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);          // manda PUT e lê do READDATA
    curl_easy_setopt(curl, CURLOPT_READDATA, in);
    curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)fsize);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_membuf);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &buf);
    // Upload não tem timeout fixo: save grande em rede lenta estoura os 30s
    // do handle. O LOW_SPEED cobre o caso que importa (a transferência
    // travou de vez) sem punir quem tem link ruim.
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);

    CURLcode rc = curl_easy_perform(curl);
    if (rc != CURLE_OK) {
        fill_error(&resp, rc);
    } else {
        long status = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
        resp.ok = true;
        resp.status = status;
        resp.body = buf.data ? buf.data : strdup("");
        resp.size = buf.size;
        buf.data = NULL;
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    fclose(in);
    free(buf.data);
    return resp;
}

bool http_download_to_file_raw(const char *url, const char *auth_raw,
                                const char *local_file_path) {
    FILE *out = fopen(local_file_path, "wb");
    if (!out) return false;

    CURL *curl = make_easy_handle();
    if (!curl) { fclose(out); return false; }

    struct curl_slist *headers = auth_header_raw(auth_raw, NULL);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fwrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 0L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L);

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    fclose(out);

    return rc == CURLE_OK && status >= 200 && status < 300;
}

bool http_download_to_file(const char *url, const char *bearer, const char *local_file_path) {
    FILE *out = fopen(local_file_path, "wb");
    if (!out) return false;

    CURL *curl = make_easy_handle();
    if (!curl) {
        fclose(out);
        return false;
    }

    struct curl_slist *headers = auth_header(bearer, NULL);

    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, fwrite);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, out);

    CURLcode rc = curl_easy_perform(curl);
    long status = 0;
    if (rc == CURLE_OK) curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    fclose(out);

    return rc == CURLE_OK && status >= 200 && status < 300;
}
