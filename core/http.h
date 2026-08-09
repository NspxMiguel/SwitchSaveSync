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

#ifdef __cplusplus
}
#endif
