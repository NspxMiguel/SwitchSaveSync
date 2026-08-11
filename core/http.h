// http.h — wrapper fino em cima do libcurl pras chamadas HTTP que o app
// precisa (form-urlencoded pro OAuth, JSON/multipart pro Drive API).
#pragma once
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    long status;   // código HTTP (0 se a requisição nem saiu, ver 'ok')
    char *body;    // corpo da resposta, sempre null-terminated; free com http_response_free
    size_t size;   // tamanho do corpo (sem contar o \0)
    bool ok;       // false = erro de transporte (sem rede, TLS, timeout...)
    char error[256]; // mensagem de erro do curl, se ok == false
} HttpResponse;

// Inicializa/finaliza o curl global. Chamar uma vez no início/fim do app.
bool http_init(void);
void http_shutdown(void);

// true só se o http_init passou. Enquanto for false, TODA função daqui
// devolve erro sem encostar em socket — de propósito: no Switch, chamar
// socket sem socketInitialize aborta o processo em vez de dar erro.
bool http_is_ready(void);

void http_response_free(HttpResponse *r);

// POST com Content-Type: application/x-www-form-urlencoded.
// 'fields' já deve vir pronto tipo "grant_type=...&client_id=...".
HttpResponse http_post_form(const char *url, const char *fields);

// GET simples, com header Authorization: Bearer <token> se bearer != NULL.
HttpResponse http_get(const char *url, const char *bearer);

// POST com corpo application/json (usado pra criar a pasta no Drive).
HttpResponse http_post_json(const char *url, const char *bearer, const char *json_body);

// PATCH com corpo application/json. Usado pra mandar arquivo pra lixeira do
// Drive ({"trashed":true}) — de propósito em vez do DELETE, que na API v3
// apaga na hora e sem volta.
HttpResponse http_patch_json(const char *url, const char *bearer, const char *json_body);

// POST/PATCH multipart/related, formato exigido pelo upload multipart da
// Drive API v3: duas partes, a primeira "application/json" com os metadados
// (nome do arquivo, pasta pai), a segunda com os bytes do arquivo local.
// method deve ser "POST" (criar) ou "PATCH" (atualizar mídia existente).
HttpResponse http_upload_multipart_related(const char *url, const char *bearer,
                                            const char *method,
                                            const char *metadata_json,
                                            const char *local_file_path,
                                            const char *file_mime_type);

// Baixa o corpo da resposta (bytes crus, não precisa ser texto) direto pra
// um arquivo local. Usado pra 'alt=media' do Drive.
bool http_download_to_file(const char *url, const char *bearer, const char *local_file_path);

// --- o que não é Google -------------------------------------------------
//
// As de cima assumem duas coisas do Drive: que o método é GET/POST/PATCH e
// que o Authorization é "Bearer <token>". O WebDAV (NAS, Nextcloud, Synology)
// não é nenhum dos dois — usa PROPFIND/MKCOL/PUT/DELETE e Authorization
// "Basic ...". Estas três recebem o valor CRU do header e o método na mão.

// 'auth_raw' é o valor inteiro do header Authorization ("Basic dXNlcjpwdw=="
// ou "Bearer ..."), ou NULL pra não mandar nenhum.
// 'extra_header' é uma linha extra ("Depth: 1"), ou NULL.
// 'body'/'body_len' podem ser NULL/0.
HttpResponse http_request_raw(const char *method, const char *url,
                               const char *auth_raw, const char *content_type,
                               const char *body, size_t body_len,
                               const char *extra_header);

// PUT com o conteúdo de um arquivo local. Vai em streaming (o curl lê do
// FILE* conforme manda), não carrega o arquivo inteiro na RAM — save de jogo
// pode ter dezenas de MB e o Switch não tem folga de memória.
// Passo 1 do upload resumivel: manda so o metadado em JSON e captura o
// cabecalho Location, que e a URI de sessao pra onde os bytes vao depois.
// method e "POST" (arquivo novo) ou "PATCH" (substituindo um que ja existe).
HttpResponse http_json_get_location(const char *method, const char *url,
                                     const char *bearer, const char *json_body,
                                     char *location_out, size_t location_sz);

HttpResponse http_put_file_raw(const char *url, const char *auth_raw,
                                const char *local_path, const char *content_type);

// GET direto pra arquivo, com Authorization cru.
bool http_download_to_file_raw(const char *url, const char *auth_raw,
                                const char *local_file_path);

#ifdef __cplusplus
}
#endif
