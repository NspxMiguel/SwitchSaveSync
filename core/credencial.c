#include "credencial.h"
#include "config.h"
#include "lang.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>

// Quem já tinha um config.h antes desta mudança não fica sem compilar: sem
// endpoint definido, o app se comporta exatamente como se comportava.
#ifndef SSS_AUTH_ENDPOINT
#define SSS_AUTH_ENDPOINT ""
#endif

#define CFG_PATH "sdmc:/switch/SwitchSaveSync/google.cfg"

static bool g_carregado = false;
static CredOrigem g_origem = CRED_EMBUTIDA;
static char g_id[256];
static char g_segredo[128];
static char g_endpoint[256];

static void tira_espaco(char *s) {
    char *ini = s;
    while (*ini && isspace((unsigned char)*ini)) ini++;
    if (ini != s) memmove(s, ini, strlen(ini) + 1);
    size_t n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) s[--n] = '\0';
}

// Aspas em volta do valor são um erro fácil de cometer (a pessoa copia do
// config.h, que é C). Em vez de o login falhar com "credencial inválida" e
// ninguém achar o motivo, tira as aspas e segue.
static void tira_aspas(char *s) {
    size_t n = strlen(s);
    if (n >= 2 && ((s[0] == '"' && s[n - 1] == '"') || (s[0] == '\'' && s[n - 1] == '\''))) {
        memmove(s, s + 1, n - 2);
        s[n - 2] = '\0';
    }
}

bool credencial_parse(const char *texto,
                      char *id, size_t idsz,
                      char *segredo, size_t segredosz,
                      char *endpoint, size_t endpointsz) {
    if (idsz) id[0] = '\0';
    if (segredosz) segredo[0] = '\0';
    if (endpointsz) endpoint[0] = '\0';
    if (!texto) return false;

    const char *p = texto;
    while (*p) {
        char linha[512];
        size_t n = 0;
        while (*p && *p != '\n' && n < sizeof(linha) - 1) linha[n++] = *p++;
        linha[n] = '\0';
        while (*p == '\n') p++;

        tira_espaco(linha);
        if (linha[0] == '\0' || linha[0] == '#' || linha[0] == ';') continue;

        char *igual = strchr(linha, '=');
        if (!igual) continue;
        *igual = '\0';
        char *chave = linha;
        char *valor = igual + 1;
        tira_espaco(chave);
        tira_espaco(valor);
        tira_aspas(valor);
        if (valor[0] == '\0') continue;

        // Aceita os dois nomes: quem vem do config.h escreve GOOGLE_CLIENT_ID.
        if (strcasecmp(chave, "client_id") == 0 || strcasecmp(chave, "google_client_id") == 0) {
            snprintf(id, idsz, "%s", valor);
        } else if (strcasecmp(chave, "client_secret") == 0 || strcasecmp(chave, "google_client_secret") == 0) {
            snprintf(segredo, segredosz, "%s", valor);
        } else if (strcasecmp(chave, "endpoint") == 0 || strcasecmp(chave, "backend") == 0) {
            snprintf(endpoint, endpointsz, "%s", valor);
        }
    }

    return id[0] != '\0' || endpoint[0] != '\0';
}

void credencial_carregar_de(const char *caminho) {
    g_carregado = true;
    g_id[0] = g_segredo[0] = g_endpoint[0] = '\0';

    char texto[2048] = {0};
    FILE *f = caminho ? fopen(caminho, "rb") : NULL;
    if (f) {
        size_t n = fread(texto, 1, sizeof(texto) - 1, f);
        texto[n] = '\0';
        fclose(f);
        credencial_parse(texto, g_id, sizeof(g_id), g_segredo, sizeof(g_segredo),
                         g_endpoint, sizeof(g_endpoint));
    }

    // Chave própria só vale inteira. Com id e sem segredo o Google recusa a
    // troca do device_code com um "invalid_client" que não explica nada, então
    // é melhor ignorar o arquivo pela metade e seguir pelo caminho normal.
    if (g_id[0] && g_segredo[0]) {
        g_origem = CRED_PROPRIA;
        return;
    }

    g_id[0] = g_segredo[0] = '\0';

    // "endpoint=direto" é a saída pra quem não quer passar pelo nosso servidor
    // e também não quer criar chave: cai na embutida.
    if (g_endpoint[0] && strcasecmp(g_endpoint, "direto") != 0 &&
        strcasecmp(g_endpoint, "direct") != 0 && strcasecmp(g_endpoint, "none") != 0) {
        g_origem = CRED_BACKEND;
        return;
    }
    if (g_endpoint[0]) { // era "direto"
        g_endpoint[0] = '\0';
        g_origem = CRED_EMBUTIDA;
        snprintf(g_id, sizeof(g_id), "%s", GOOGLE_CLIENT_ID);
        snprintf(g_segredo, sizeof(g_segredo), "%s", GOOGLE_CLIENT_SECRET);
        return;
    }

    snprintf(g_endpoint, sizeof(g_endpoint), "%s", SSS_AUTH_ENDPOINT);
    if (g_endpoint[0]) {
        g_origem = CRED_BACKEND;
        return;
    }

    g_origem = CRED_EMBUTIDA;
    snprintf(g_id, sizeof(g_id), "%s", GOOGLE_CLIENT_ID);
    snprintf(g_segredo, sizeof(g_segredo), "%s", GOOGLE_CLIENT_SECRET);
}

void credencial_carregar(void) {
    if (!g_carregado) credencial_carregar_de(CFG_PATH);
}

CredOrigem credencial_origem(void) { credencial_carregar(); return g_origem; }
const char *credencial_client_id(void) { credencial_carregar(); return g_id; }
const char *credencial_client_secret(void) { credencial_carregar(); return g_segredo; }
const char *credencial_endpoint(void) { credencial_carregar(); return g_endpoint; }

const char *credencial_descricao(void) {
    switch (credencial_origem()) {
        case CRED_PROPRIA:
            return TR("Credencial sua (google.cfg) — fala direto com o Google",
                      "Your own credential (google.cfg) - talks straight to Google");
        case CRED_BACKEND:
            return TR("Credencial do projeto, guardada no servidor",
                      "Project credential, kept on the server");
        default:
            return TR("Credencial do projeto, embutida neste build",
                      "Project credential, built into this binary");
    }
}
