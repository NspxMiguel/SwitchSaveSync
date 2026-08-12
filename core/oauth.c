#include "oauth.h"
#include "http.h"
#include "minijson.h"
#include "config.h"
#include "credencial.h"
#include "lang.h"

#include <switch.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#define TOKEN_DIR  "sdmc:/switch/SwitchSaveSync"
#define TOKEN_PATH TOKEN_DIR "/token.txt"

// ---------------------------------------------------------------------------
// Falar com o Google, direto ou pelo nosso endpoint
//
// As duas rotas devolvem exatamente o mesmo JSON — o servidor repassa a
// resposta do Google sem tocar. Por isso só a montagem do pedido muda aqui, e
// todo o resto do arquivo (o parse, o loop de poll, os códigos de erro)
// continua sendo um só caminho.
// ---------------------------------------------------------------------------

static bool via_backend(void) { return credencial_origem() == CRED_BACKEND; }

static void junta_url(char *out, size_t sz, const char *rota) {
    const char *base = credencial_endpoint();
    size_t n = strlen(base);
    while (n > 0 && base[n - 1] == '/') n--; // barra sobrando é erro de digitação, não de configuração
    snprintf(out, sz, "%.*s%s", (int)n, base, rota);
}

static HttpResponse pede_device_code(void) {
    char fields[512];
    if (via_backend()) {
        char url[400];
        junta_url(url, sizeof(url), "/device");
        snprintf(fields, sizeof(fields), "scope=%s", GOOGLE_OAUTH_SCOPE);
        return http_post_form(url, fields);
    }
    snprintf(fields, sizeof(fields), "client_id=%s&scope=%s",
             credencial_client_id(), GOOGLE_OAUTH_SCOPE);
    return http_post_form("https://oauth2.googleapis.com/device/code", fields);
}

static HttpResponse troca_device_code(const char *device_code) {
    char fields[700];
    if (via_backend()) {
        char url[400];
        junta_url(url, sizeof(url), "/token");
        snprintf(fields, sizeof(fields), "device_code=%s", device_code);
        return http_post_form(url, fields);
    }
    snprintf(fields, sizeof(fields),
             "client_id=%s&client_secret=%s&device_code=%s"
             "&grant_type=urn:ietf:params:oauth:grant-type:device_code",
             credencial_client_id(), credencial_client_secret(), device_code);
    return http_post_form("https://oauth2.googleapis.com/token", fields);
}

static HttpResponse renova(const char *refresh_token) {
    char fields[900];
    if (via_backend()) {
        char url[400];
        junta_url(url, sizeof(url), "/refresh");
        snprintf(fields, sizeof(fields), "refresh_token=%s", refresh_token);
        return http_post_form(url, fields);
    }
    snprintf(fields, sizeof(fields),
             "client_id=%s&client_secret=%s&refresh_token=%s&grant_type=refresh_token",
             credencial_client_id(), credencial_client_secret(), refresh_token);
    return http_post_form("https://oauth2.googleapis.com/token", fields);
}

// 429 é o servidor cortando a chamada: ou este IP passou do limite, ou o
// endpoint inteiro está apanhando e entrou em modo de defesa. São situações
// diferentes pro usuário — uma é "calma", a outra é "não é você" — e por isso
// têm texto diferente.
static bool freou(const HttpResponse *r) {
    return r->ok && r->status == 429;
}

static bool freou_por_ataque(const HttpResponse *r) {
    return freou(r) && r->body && strstr(r->body, "under_attack") != NULL;
}

static const char *texto_do_freio(const HttpResponse *r) {
    // Mesma frase que a tela de sincronização mostra pelo código de erro; ter
    // duas redações pro mesmo acontecimento é como se descobre, meses depois,
    // que uma delas nunca foi traduzida.
    return oauth_motivo_texto(freou_por_ataque(r) ? OAUTH_FAIL_ATAQUE : OAUTH_FAIL_LIMITE);
}

static char g_refresh_token[512] = {0};

static void ensure_token_dir(void) {
    mkdir(TOKEN_DIR, 0777); // ignora erro se já existe
}

bool oauth_load_saved_login(void) {
    FILE *f = fopen(TOKEN_PATH, "rb");
    if (!f) return false;
    size_t n = fread(g_refresh_token, 1, sizeof(g_refresh_token) - 1, f);
    fclose(f);
    g_refresh_token[n] = '\0';
    // tira \r\n do final se tiver
    while (n > 0 && (g_refresh_token[n - 1] == '\n' || g_refresh_token[n - 1] == '\r')) {
        g_refresh_token[--n] = '\0';
    }
    return g_refresh_token[0] != '\0';
}

bool oauth_is_logged_in(void) {
    return g_refresh_token[0] != '\0';
}

void oauth_logout(void) {
    g_refresh_token[0] = '\0';
    remove(TOKEN_PATH);
}

// Devolve false se o token NÃO chegou no cartão. A cópia em memória é feita
// primeiro de propósito: com o cartão cheio ou protegido, a sessão de agora
// continua inteira e o que se perde é só a parte de não precisar logar de novo
// no próximo boot. Antes, um fopen que falhasse saía daqui sem nem guardar o
// token na memória, e quem chamava ignorava o retorno.
static bool save_refresh_token(const char *token) {
    strncpy(g_refresh_token, token, sizeof(g_refresh_token) - 1);
    g_refresh_token[sizeof(g_refresh_token) - 1] = '\0';

    ensure_token_dir();
    FILE *f = fopen(TOKEN_PATH, "wb");
    if (!f) return false;

    size_t querido = strlen(token);
    size_t escrito = fwrite(token, 1, querido, f);
    // fclose também pode falhar: é nele que o buffer vai pro cartão de verdade.
    bool fechou_ok = (fclose(f) == 0);
    return fechou_ok && escrito == querido;
}

static oauth_device_cb g_device_cb = NULL;

void oauth_set_device_cb(oauth_device_cb cb) { g_device_cb = cb; }

bool oauth_start_device_flow(oauth_status_cb on_status, oauth_cancel_cb should_cancel) {
    HttpResponse r = pede_device_code();
    if (freou(&r)) {
        if (on_status) on_status(texto_do_freio(&r));
        http_response_free(&r);
        return false;
    }
    if (!r.ok || r.status != 200) {
        char msg[300];
        snprintf(msg, sizeof(msg), "Falha ao pedir device code (status %ld): %s",
                 r.status, r.ok ? r.body : r.error);
        if (on_status) on_status(msg);
        http_response_free(&r);
        return false;
    }

    char device_code[256] = {0};
    char user_code[64] = {0};
    char verification_url[256] = {0};
    double expires_in = 1800;
    double interval = 5;

    json_get_string(r.body, "device_code", device_code, sizeof(device_code));
    json_get_string(r.body, "user_code", user_code, sizeof(user_code));
    // o campo pode vir como "verification_url" ou "verification_uri"
    // dependendo da versão da API — tenta os dois.
    if (!json_get_string(r.body, "verification_url", verification_url, sizeof(verification_url))) {
        json_get_string(r.body, "verification_uri", verification_url, sizeof(verification_url));
    }
    json_get_number(r.body, "expires_in", &expires_in);
    json_get_number(r.body, "interval", &interval);
    http_response_free(&r);

    if (device_code[0] == '\0' || user_code[0] == '\0') {
        if (on_status) on_status("Resposta do Google veio sem device_code/user_code. Confira o CLIENT_ID em config.h.");
        return false;
    }

    // Avisa a UI com a URL e o código separados, pra ela poder desenhar o QR
    // e o código em letra grande em vez de só um blocão de texto.
    if (g_device_cb) {
        const char *base = verification_url[0] ? verification_url : "https://www.google.com/device";
        char url_with_code[400];
        // O Google aceita o código pré-preenchido na query string, então quem
        // escaneia o QR não digita nada.
        snprintf(url_with_code, sizeof(url_with_code), "%s?user_code=%s", base, user_code);
        g_device_cb(base, user_code, url_with_code);
    }

    char status_msg[400];
    if (g_device_cb) {
        // Quem tem UI gráfica já vai desenhar o QR, a URL e o código em letra
        // grande. Repetir tudo aqui em cima empurrava o QR pra fora da tela —
        // ele aparecia cortado na tela de login.
        snprintf(status_msg, sizeof(status_msg),
                 "Aguardando voce confirmar no celular ou no PC. (B cancela)");
    } else {
        snprintf(status_msg, sizeof(status_msg),
                 "No celular ou PC, abra:\n  %s\ne digite o codigo:\n  %s\n\nAguardando confirmacao... (B cancela)",
                 verification_url[0] ? verification_url : "google.com/device", user_code);
    }
    if (on_status) on_status(status_msg);

    // O loop abaixo era inteiramente mudo: erro de rede caía num continue sem
    // escrever nada, então "esperando você confirmar" e "a rede morreu" ficavam
    // exatamente iguais na tela — parada, no mesmo texto, pra sempre. Agora
    // todo caminho escreve alguma coisa, e o contador de segundos serve de
    // sinal de vida.
    double elapsed = 0;
    int falhas_rede = 0;
    while (elapsed < expires_in) {
        if (should_cancel && should_cancel()) {
            if (on_status) on_status("Login cancelado.");
            return false;
        }

        svcSleepThread((u64)(interval * 1000000000.0));
        elapsed += interval;

        HttpResponse poll = troca_device_code(device_code);

        // Freada no meio do poll não é motivo pra jogar o login fora: o código
        // na tela do usuário vale meia hora e o limite passa em minutos. Diz o
        // que houve, espaça as tentativas e continua — quem não quiser esperar
        // aperta B.
        if (freou(&poll)) {
            if (on_status) on_status(texto_do_freio(&poll));
            if (interval < 30) interval += 10;
            http_response_free(&poll);
            continue;
        }

        if (!poll.ok) {
            // erro de rede pontual (ex: wifi soneca) — tenta de novo no próximo tick
            falhas_rede++;
            if (on_status) {
                char m[200];
                snprintf(m, sizeof(m),
                         "Sem resposta do Google (%d falha%s de rede). Tentando de novo... (B cancela)",
                         falhas_rede, falhas_rede == 1 ? "" : "s");
                on_status(m);
            }
            http_response_free(&poll);
            continue;
        }

        if (poll.status == 200) {
            char refresh_token[512] = {0};
            if (json_get_string(poll.body, "refresh_token", refresh_token, sizeof(refresh_token))) {
                http_response_free(&poll);
                if (save_refresh_token(refresh_token)) {
                    if (on_status) on_status("Login concluido! Token salvo no cartao.");
                } else {
                    // Entrou de verdade — só não deu pra guardar. Dizer "falhou"
                    // aqui seria mentira ao contrário: a sessão funciona.
                    if (on_status) on_status("Login concluido, mas nao consegui gravar o token no cartao. Vai funcionar agora e pedir login de novo no proximo boot.");
                }
                return true;
            }
            // 200 sem refresh_token normalmente é o app já autorizado antes
            // sem "prompt=consent" — nesse caso o Google não manda refresh
            // token de novo. Avisa em vez de fingir sucesso.
            http_response_free(&poll);
            if (on_status) on_status("Google confirmou mas nao mandou refresh_token (talvez ja autorizado antes). Revogue o acesso em myaccount.google.com/permissions e tente de novo.");
            return false;
        }

        char err[64] = {0};
        json_get_string(poll.body, "error", err, sizeof(err));
        http_response_free(&poll);

        if (strcmp(err, "authorization_pending") == 0) {
            // Normal: a confirmação ainda não veio. O relógio na tela separa
            // "estou esperando você" de "travei".
            if (on_status) {
                char m[200];
                snprintf(m, sizeof(m),
                         "Aguardando voce confirmar no celular ou no PC — %ds. (B cancela)",
                         (int)elapsed);
                on_status(m);
            }
            continue;
        } else if (strcmp(err, "slow_down") == 0) {
            interval += 5; // Google pediu pra espaçar mais o poll
            continue;
        } else if (strcmp(err, "expired_token") == 0) {
            if (on_status) on_status("Codigo expirou. Tente de novo.");
            return false;
        } else if (strcmp(err, "access_denied") == 0) {
            if (on_status) on_status("Login negado no celular/PC.");
            return false;
        } else {
            char msg[128];
            snprintf(msg, sizeof(msg), "Erro inesperado do Google: %s", err[0] ? err : "desconhecido");
            if (on_status) on_status(msg);
            // continua tentando — pode ser transiente
        }
    }

    if (on_status) on_status("Tempo esgotado esperando confirmacao.");
    return false;
}

OauthResult oauth_refresh_access_token(char *out, size_t outsz) {
    if (!oauth_is_logged_in()) return OAUTH_FAIL_LOGGED_OUT;

    HttpResponse r = renova(g_refresh_token);

    // Freio do servidor não é token morto nem rede caída: some sozinho. Tem
    // código próprio pra tela não mandar ninguém logar de novo à toa.
    if (freou(&r)) {
        bool ataque = freou_por_ataque(&r);
        http_response_free(&r);
        return ataque ? OAUTH_FAIL_ATAQUE : OAUTH_FAIL_LIMITE;
    }

    // "invalid_grant" é o Google dizendo que esse refresh token morreu e não
    // vai ressuscitar: conta desconectada em myaccount.google.com, senha
    // trocada, ou as chaves do projeto resetadas. Guardar ele no cartão faz
    // toda sincronização daqui pra frente falhar igualzinho a uma queda de
    // rede — e ninguém descobre que era só refazer o login. Some com ele.
    if (r.ok && r.status == 400 && r.body && strstr(r.body, "invalid_grant")) {
        http_response_free(&r);
        oauth_logout();
        return OAUTH_FAIL_LOGGED_OUT;
    }

    if (!r.ok || r.status != 200) {
        http_response_free(&r);
        return OAUTH_FAIL_NETWORK;
    }

    bool got = json_get_string(r.body, "access_token", out, outsz);
    http_response_free(&r);
    return got ? OAUTH_OK : OAUTH_FAIL_NETWORK;
}

bool oauth_get_fresh_access_token(char *out, size_t outsz) {
    return oauth_refresh_access_token(out, outsz) == OAUTH_OK;
}

const char *oauth_motivo_texto(OauthResult r) {
    switch (r) {
        case OAUTH_FAIL_ATAQUE:
            return TR("Foi detectado um ataque nos servidores do SwitchSaveSync. "
                      "Favor, tente novamente mais tarde.",
                      "An attack on the SwitchSaveSync servers was detected. "
                      "Please try again later.");
        case OAUTH_FAIL_LIMITE:
            return TR("Muitas tentativas deste endereco em pouco tempo. "
                      "Espere alguns minutos e tente de novo.",
                      "Too many tries from this address in a short time. "
                      "Wait a few minutes and try again.");
        default:
            return NULL;
    }
}
