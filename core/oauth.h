// oauth.h — login OAuth 2.0 "Device Flow" (Limited Input) do Google.
// O usuário loga no celular/PC visitando uma URL curta e digitando um
// código — nada de browser no Switch. Mesmo esquema que o JKSV usa pro
// Google Drive.
#pragma once
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Tenta carregar um refresh_token salvo no SD (sdmc:/switch/SwitchSaveSync/token.txt).
// Retorna true se achou algo (não valida se ainda é válido — isso só se sabe
// na hora de trocar por um access_token).
bool oauth_load_saved_login(void);

bool oauth_is_logged_in(void);

// Apaga o token salvo (logout).
void oauth_logout(void);

// Fluxo interativo completo: pede o device_code pro Google, imprime no
// console o código + a URL (ex: "vá em google.com/device e digite ABCD-EFGH"),
// e fica dando poll até o usuário confirmar no celular ou o código expirar.
// Chama print_status(msg) a cada atualização de tela pra quem chamou poder
// redesenhar (ex: reler input, dar consoleUpdate) sem essa função saber de
// UI. Salva o refresh_token no SD em caso de sucesso.
typedef void (*oauth_status_cb)(const char *msg);
// Chamada a cada iteração do poll; se retornar true, o login é abortado
// (ex: usuário segurou B pra cancelar). Pode ser NULL.
typedef bool (*oauth_cancel_cb)(void);
bool oauth_start_device_flow(oauth_status_cb on_status, oauth_cancel_cb should_cancel);

// Chamado uma vez, assim que o Google devolve o código, com a URL e o código
// separados — a UI usa isso pra desenhar o QR e mostrar o código em letra
// grande. 'url_with_code' já vem com o código embutido na query string, que é
// o que vale a pena virar QR: quem escaneia cai na página com o campo
// preenchido, sem digitar nada.
typedef void (*oauth_device_cb)(const char *verification_url,
                                const char *user_code,
                                const char *url_with_code);
void oauth_set_device_cb(oauth_device_cb cb);

// Por que o token não veio.
//
// Rede caindo e conta revogada davam os dois `false`, e isso já custou uma
// sessão de teste inteira: o `token.txt` no cartão tinha um refresh token
// morto (as chaves do projeto tinham sido resetadas), toda sincronização
// falhava, e a tela dizia só "nuvem FALHOU" — que é exatamente o que ela diz
// quando o Wi-Fi cai. Uma passa sozinha, a outra só um login novo resolve.
typedef enum {
    OAUTH_OK = 0,
    OAUTH_FAIL_NETWORK,    // deu ruim agora; tentar de novo depois faz sentido
    OAUTH_FAIL_LOGGED_OUT, // o Google recusou o refresh token: tem que entrar de novo
} OauthResult;

// Troca o refresh_token salvo por um access_token novo (o Google não deixa
// reusar um access_token por muito tempo, então é mais simples pedir um
// novo antes de cada chamada à Drive API do que ficar controlando expiração
// e relógio do console, que pode estar errado).
// 'out' recebe o access_token; outsz é o tamanho do buffer.
//
// Quando devolve OAUTH_FAIL_LOGGED_OUT, o token salvo JÁ FOI APAGADO — não
// adianta guardar um token que o Google não aceita mais.
OauthResult oauth_refresh_access_token(char *out, size_t outsz);

// Igual à de cima, sem o motivo. Continua aqui porque metade do projeto só
// quer saber se deu ou não deu.
bool oauth_get_fresh_access_token(char *out, size_t outsz);

#ifdef __cplusplus
}
#endif
