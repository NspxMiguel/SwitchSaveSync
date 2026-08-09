#include "oauth.h"
#include "http.h"
#include "minijson.h"
#include "config.h"

#include <switch.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

#define TOKEN_DIR  "sdmc:/switch/SwitchSaveSync"
#define TOKEN_PATH TOKEN_DIR "/token.txt"

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

static bool save_refresh_token(const char *token) {
    ensure_token_dir();
    FILE *f = fopen(TOKEN_PATH, "wb");
    if (!f) return false;
    fwrite(token, 1, strlen(token), f);
    fclose(f);
    strncpy(g_refresh_token, token, sizeof(g_refresh_token) - 1);
    g_refresh_token[sizeof(g_refresh_token) - 1] = '\0';
    return true;
}

static oauth_device_cb g_device_cb = NULL;

void oauth_set_device_cb(oauth_device_cb cb) { g_device_cb = cb; }

bool oauth_start_device_flow(oauth_status_cb on_status, oauth_cancel_cb should_cancel) {
    char fields[512];
    snprintf(fields, sizeof(fields),
             "client_id=%s&scope=%s",
             GOOGLE_CLIENT_ID, GOOGLE_OAUTH_SCOPE);

    HttpResponse r = http_post_form("https://oauth2.googleapis.com/device/code", fields);
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
        // grande. Repetir tudo aqui em cima empurrava o QR pra fora da tela
        // conversa privada removida do historico
        snprintf(status_msg, sizeof(status_msg),
                 "Aguardando voce confirmar no celular ou no PC. (B cancela)");
    } else {
        snprintf(status_msg, sizeof(status_msg),
                 "No celular ou PC, abra:\n  %s\ne digite o codigo:\n  %s\n\nAguardando confirmacao... (B cancela)",
                 verification_url[0] ? verification_url : "google.com/device", user_code);
    }
    if (on_status) on_status(status_msg);

    double elapsed = 0;
    while (elapsed < expires_in) {
        if (should_cancel && should_cancel()) {
            if (on_status) on_status("Login cancelado.");
            return false;
        }

        svcSleepThread((u64)(interval * 1000000000.0));
        elapsed += interval;

        char poll_fields[700];
        snprintf(poll_fields, sizeof(poll_fields),
                 "client_id=%s&client_secret=%s&device_code=%s"
                 "&grant_type=urn:ietf:params:oauth:grant-type:device_code",
                 GOOGLE_CLIENT_ID, GOOGLE_CLIENT_SECRET, device_code);

        HttpResponse poll = http_post_form("https://oauth2.googleapis.com/token", poll_fields);
        if (!poll.ok) {
            // erro de rede pontual (ex: wifi soneca) — tenta de novo no próximo tick
            http_response_free(&poll);
            continue;
        }

        if (poll.status == 200) {
            char refresh_token[512] = {0};
            if (json_get_string(poll.body, "refresh_token", refresh_token, sizeof(refresh_token))) {
                http_response_free(&poll);
                save_refresh_token(refresh_token);
                if (on_status) on_status("Login concluido! Token salvo no SD.");
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
            continue; // normal, usuário ainda não confirmou
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

bool oauth_get_fresh_access_token(char *out, size_t outsz) {
    if (!oauth_is_logged_in()) return false;

    char fields[900];
    snprintf(fields, sizeof(fields),
             "client_id=%s&client_secret=%s&refresh_token=%s&grant_type=refresh_token",
             GOOGLE_CLIENT_ID, GOOGLE_CLIENT_SECRET, g_refresh_token);

    HttpResponse r = http_post_form("https://oauth2.googleapis.com/token", fields);
    if (!r.ok || r.status != 200) {
        http_response_free(&r);
        return false;
    }

    bool got = json_get_string(r.body, "access_token", out, outsz);
    http_response_free(&r);
    return got;
}
