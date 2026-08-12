// Teste de onde vem a credencial do Google.
//
// A regra tem três fontes e uma ordem de precedência, e errar a ordem é um
// erro silencioso do pior tipo: o app continua logando, só que pela chave
// errada. Quem põe um google.cfg no cartão está justamente dizendo "não quero
// depender da chave de vocês" — se o endpoint ganhar do arquivo dele, o
// pedido foi ignorado sem ninguém perceber.
//
// O parser também é testado com o arquivo torto que a pessoa real escreve:
// aspas copiadas do config.h, espaço em volta do '=', CRLF do Windows,
// comentário, e o arquivo pela metade (id sem segredo), que precisa ser
// recusado inteiro em vez de virar uma chave quebrada.

#include "credencial.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

// ----------------------------------------------------------------- o que falta

bool lang_is_pt(void) { return true; }

// ------------------------------------------------------------------ placar

static int falhas = 0, testes = 0;

static void eq(const char *got, const char *want, const char *msg)
{
    testes++;
    if (strcmp(got ? got : "(null)", want) != 0) {
        printf("  FALHOU  %s\n          esperado: '%s'\n          veio:     '%s'\n", msg, want, got ? got : "(null)");
        falhas++;
    } else {
        printf("  ok      %s\n", msg);
    }
}

static void eq_int(int got, int want, const char *msg)
{
    testes++;
    if (got != want) {
        printf("  FALHOU  %s (esperado %d, veio %d)\n", msg, want, got);
        falhas++;
    } else {
        printf("  ok      %s\n", msg);
    }
}

// ------------------------------------------------------------------ ajudantes

static const char *CFG = "google.cfg";

static void escreve(const char *conteudo)
{
    FILE *f = fopen(CFG, "wb");
    if (!f) { printf("nao consegui escrever o %s\n", CFG); exit(1); }
    fwrite(conteudo, 1, strlen(conteudo), f);
    fclose(f);
    credencial_carregar_de(CFG);
}

int main(void)
{
    char id[256], seg[128], end[256];

    printf("\n-- o parser --\n");

    eq_int(credencial_parse("client_id=abc\nclient_secret=xyz\n",
                            id, sizeof(id), seg, sizeof(seg), end, sizeof(end)), 1,
           "o par simples e aceito");
    eq(id, "abc", "client_id lido");
    eq(seg, "xyz", "client_secret lido");

    credencial_parse("  client_id  =  com-espaco  \r\nclient_secret=\"entre-aspas\"\r\n",
                     id, sizeof(id), seg, sizeof(seg), end, sizeof(end));
    eq(id, "com-espaco", "espaco em volta do = nao entra no valor");
    eq(seg, "entre-aspas", "aspas copiadas do config.h sao tiradas");

    credencial_parse("# comentario\n; outro\nclient_id=so-esse\n",
                     id, sizeof(id), seg, sizeof(seg), end, sizeof(end));
    eq(id, "so-esse", "comentario com # e com ; e ignorado");

    credencial_parse("GOOGLE_CLIENT_ID=maiusculo\nGOOGLE_CLIENT_SECRET=tambem\n",
                     id, sizeof(id), seg, sizeof(seg), end, sizeof(end));
    eq(id, "maiusculo", "aceita o nome do config.h (GOOGLE_CLIENT_ID)");
    eq(seg, "tambem", "aceita GOOGLE_CLIENT_SECRET");

    eq_int(credencial_parse("linha solta sem igual\nlixo\n",
                            id, sizeof(id), seg, sizeof(seg), end, sizeof(end)), 0,
           "arquivo sem nada aproveitavel e recusado");

    printf("\n-- a ordem das fontes --\n");

    escreve("client_id=meu-id\nclient_secret=meu-segredo\n");
    eq_int(credencial_origem(), CRED_PROPRIA, "com id e segredo, a chave e do usuario");
    eq(credencial_client_id(), "meu-id", "usa o id do arquivo");
    eq(credencial_client_secret(), "meu-segredo", "usa o segredo do arquivo");
    eq(credencial_endpoint(), "", "chave propria fala direto com o Google, sem endpoint");

    escreve("client_id=meu-id\nclient_secret=meu-segredo\nendpoint=https://x.vercel.app/api\n");
    eq_int(credencial_origem(), CRED_PROPRIA,
           "chave propria ganha do endpoint no mesmo arquivo");

    escreve("endpoint=https://meu.vercel.app/api\n");
    eq_int(credencial_origem(), CRED_BACKEND, "so endpoint = passa pelo servidor");
    eq(credencial_endpoint(), "https://meu.vercel.app/api", "guarda o endpoint escrito");
    eq(credencial_client_id(), "", "no backend a chave nao chega no console");
    eq(credencial_client_secret(), "", "nem o segredo");

    escreve("client_id=so-metade\n");
    eq_int(credencial_origem(), CRED_EMBUTIDA,
           "id sem segredo nao vira chave propria pela metade");
    eq_int(credencial_client_id()[0] != '\0', 1, "cai na embutida, que existe");

    escreve("endpoint=direto\n");
    eq_int(credencial_origem(), CRED_EMBUTIDA, "'endpoint=direto' recusa o servidor");
    eq(credencial_endpoint(), "", "e nao sobra endpoint nenhum");

    remove(CFG);
    credencial_carregar_de(CFG);
    eq_int(credencial_origem(), CRED_EMBUTIDA,
           "sem google.cfg e sem SSS_AUTH_ENDPOINT, e a chave do build");

    credencial_carregar_de(NULL);
    eq_int(credencial_origem(), CRED_EMBUTIDA, "caminho nulo nao derruba nada");

    printf("\n%d/%d passaram\n", testes - falhas, testes);
    return falhas ? 1 : 0;
}
