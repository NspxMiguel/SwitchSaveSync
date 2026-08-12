// credencial.h — de onde vem a credencial do Google.
//
// Em app instalado não existe segredo: o RFC 6749 §2.1 chama isso de "public
// client" e assume que a chave é extraível. O que protege o usuário é o escopo
// (drive.file), a tela de consentimento e a possibilidade de trocar a chave —
// não o esconderijo. Então aqui não se tenta esconder nada; se decide de qual
// das três fontes a chave vem, em ordem de confiança:
//
//   1. CRED_PROPRIA  — google.cfg no cartão. A chave é do dono do console, o
//                      app fala direto com o Google, e ninguém depende de
//                      servidor de terceiro (nem do nosso).
//   2. CRED_BACKEND  — nosso endpoint. A chave não está no .nro nem no cartão:
//                      quem a guarda é o servidor, que também aplica limite por
//                      IP e corta rajada. É o padrão da release.
//   3. CRED_EMBUTIDA — a chave está no binário, do jeito clássico. É o que
//                      sobra pra quem compila em casa sem endpoint nenhum.
#pragma once
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CRED_PROPRIA = 0,
    CRED_BACKEND = 1,
    CRED_EMBUTIDA = 2,
} CredOrigem;

// Lê o google.cfg do cartão e decide a origem. Pode chamar quantas vezes
// quiser: só o primeiro carrega de verdade.
void credencial_carregar(void);

// Igual, apontando pra outro arquivo, e sempre relendo. É o que os testes usam.
void credencial_carregar_de(const char *caminho);

CredOrigem credencial_origem(void);

// Vazios quando a origem é CRED_BACKEND — nesse caso a chave nunca chega aqui.
const char *credencial_client_id(void);
const char *credencial_client_secret(void);

// "" quando o app fala direto com o Google.
const char *credencial_endpoint(void);

// Uma linha pra tela de diagnóstico, no idioma do app.
const char *credencial_descricao(void);

// Exposto pro teste: lê o conteúdo do google.cfg. Devolve false quando não
// achou um par utilizável. Aceita comentário com # ou ;, espaço em volta do
// '=', CRLF, e ignora chave que não conhece.
bool credencial_parse(const char *texto,
                      char *id, size_t idsz,
                      char *segredo, size_t segredosz,
                      char *endpoint, size_t endpointsz);

#ifdef __cplusplus
}
#endif
