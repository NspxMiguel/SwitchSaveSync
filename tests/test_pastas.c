// Teste do caminho da pasta na nuvem: "<Jogo>/<Dono>".
//
// Este arquivo existe porque a regra que ele testa errou TRÊS vezes no mesmo
// dia em que foi escrita:
//
//   1. o ramo de primeira sync entregou o caminho de dois níveis pra uma função
//      que cria um nome só — no Drive nasceu uma pasta cujo NOME tinha barra;
//   2. o fallback do layout antigo passou a resolver pra pasta-container do
//      jogo, e restaurar aquilo escreveria as pastas das outras contas dentro
//      do savedata;
//   3. a contagem da barra de progresso criava pasta numa rota de leitura.
//
// Nenhuma das três precisava de console pra ser pega. Precisava disto aqui.
//
// O que dá pra testar no Mac é a REGRA (que caminho sai de um TitleEntry), não
// a conversa com o Drive. Que é justamente onde os três erros moravam.

#include "syncjob.h"
#include "syncstate.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>

// ----------------------------------------------------------------- o que falta

bool lang_is_pt(void) { return true; }

Result timeGetCurrentTime(TimeType type, u64 *out)
{
    (void)type;
    *out = 0;
    return 1; // falha de propósito: o syncstate cai no caminho sem data
}

// O stub da conta em uso mora no stubs_console.c; o teste escreve aqui.
extern char stub_conta_atual[0x21];

// ------------------------------------------------------------------ placar

static int falhas = 0, testes = 0;

static void eq(const char *got, const char *want, const char *msg)
{
    testes++;
    int bom = strcmp(got, want) == 0;
    printf(bom ? "  ok   %s\n" : "  FALHA %s\n", msg);
    if (!bom)
    {
        printf("         esperava: \"%s\"\n         veio:     \"%s\"\n", want, got);
        falhas++;
    }
}

static void ok(int cond, const char *msg)
{
    testes++;
    printf(cond ? "  ok   %s\n" : "  FALHA %s\n", msg);
    if (!cond) falhas++;
}

// Monta um TitleEntry como o titles.c entregaria.
static TitleEntry jogo(const char *nome, const char *apelido, bool compartilhado)
{
    TitleEntry t = {0};
    snprintf(t.name, sizeof(t.name), "%s", nome);
    if (apelido)
        snprintf(t.account, sizeof(t.account), "%s", apelido);
    t.shared_game    = compartilhado;
    t.application_id = 0x0100152000022000ULL;
    t.uid.uid[0]     = 0x1000000000563F8CULL;
    t.uid.uid[1]     = 0x4FEBE4CDBD951CB7ULL;
    return t;
}

// ------------------------------------------------------------- os dois níveis

static void teste_sempre_dois_niveis(void)
{
    printf("\n== sempre <Jogo>/<Dono>, nunca save solto ==\n");

    char out[0x220];

    // Vale ate pro save unico: sem a pasta de conta, o save fica solto na raiz
    // do jogo, misturado com as pastas das outras contas.
    {
        TitleEntry t = jogo("Super Mario 3D World", "Jogador", false);
        syncjob_cloud_folder_path(&t, out, sizeof(out));
        eq(out, "Super Mario 3D World/Jogador",
            "save unico TAMBEM ganha pasta de conta");
    }

    {
        TitleEntry t = jogo("Rayman Legends", "CONVIDADO", true);
        syncjob_cloud_folder_path(&t, out, sizeof(out));
        eq(out, "Rayman Legends/CONVIDADO", "jogo com varias contas: uma pasta por conta");
    }

    // O nome do jogo nunca leva apelido — era isso que fazia um jogo com save
    // de quatro contas virar quatro pastas lado a lado na raiz do Drive.
    {
        TitleEntry a = jogo("Rayman Legends", "Jogador", true);
        TitleEntry b = jogo("Rayman Legends", "Convidado", true);
        char pa[0x220], pb[0x220];
        syncjob_cloud_folder_path(&a, pa, sizeof(pa));
        syncjob_cloud_folder_path(&b, pb, sizeof(pb));

        // Mesmo prefixo até a barra = mesma pasta de jogo.
        const char *ba = strchr(pa, '/');
        const char *bb = strchr(pb, '/');
        ok(ba && bb && (ba - pa) == (bb - pb) && strncmp(pa, pb, (size_t)(ba - pa)) == 0,
            "duas contas do mesmo jogo caem na MESMA pasta de jogo");
        ok(strcmp(pa, pb) != 0, "e em subpastas diferentes");
    }
}

// ------------------------------------------------------- quem nomeia o dono

static void teste_nome_do_dono(void)
{
    printf("\n== de onde sai o nome da pasta da conta ==\n");

    char out[0x220];

    // 1. device save: o save e do aparelho, nao de uma pessoa.
    {
        TitleEntry t = jogo("Animal Crossing", NULL, false);
        t.device_save = true;
        syncjob_cloud_folder_path(&t, out, sizeof(out));
        eq(out, "Animal Crossing/console", "device save vai pra 'console'");
    }

    // 2. o caso normal.
    {
        TitleEntry t = jogo("Zelda", "Jogador", false);
        syncjob_cloud_folder_path(&t, out, sizeof(out));
        eq(out, "Zelda/Jogador", "com apelido, usa o apelido");
    }

    // 3. sem apelido e com UM save so: cai na conta que esta usando o app
    //    agora, que e o unico palpite que nao mistura save de gente diferente.
    {
        snprintf(stub_conta_atual, sizeof(stub_conta_atual), "%s", "Jogador");
        TitleEntry t = jogo("Stardew Valley", NULL, false);
        syncjob_cloud_folder_path(&t, out, sizeof(out));
        eq(out, "Stardew Valley/Jogador",
            "sem apelido e save unico: cai na conta em uso");
        stub_conta_atual[0] = '\0';
    }

    // 4. sem apelido mas com MAIS DE UM save: aqui chutar a conta em uso
    //    misturaria save de gente diferente. Tem que cair no uid.
    {
        snprintf(stub_conta_atual, sizeof(stub_conta_atual), "%s", "Jogador");
        TitleEntry t = jogo("Mario Kart 8 Deluxe", NULL, true);
        syncjob_cloud_folder_path(&t, out, sizeof(out));
        ok(strstr(out, "/conta-") != NULL,
            "sem apelido e varios saves: NAO chuta a conta em uso, usa o uid");
        ok(strstr(out, "Jogador") == NULL,
            "e o nome de quem esta no console nao aparece");
        stub_conta_atual[0] = '\0';
    }

    // 5. nem apelido, nem conta em uso: sobra o uid, que e estavel.
    {
        stub_conta_atual[0] = '\0';
        TitleEntry t = jogo("Pokemon", NULL, false);
        char a[0x220], b[0x220];
        syncjob_cloud_folder_path(&t, a, sizeof(a));
        syncjob_cloud_folder_path(&t, b, sizeof(b));
        eq(a, b, "o nome pelo uid e estavel entre duas chamadas");
        ok(strstr(a, "/conta-") != NULL, "e tem a cara de 'conta-<uid>'");
    }
}

// ----------------------------------------------------- barra so a nossa

static void teste_barra(void)
{
    printf("\n== a unica barra do caminho e a nossa ==\n");

    char out[0x220];

    // Se o nome do jogo passasse cru, um jogo chamado "A/B" criaria um nivel
    // fantasma e o save cairia numa pasta que ninguem procura.
    {
        TitleEntry t = jogo("Jogo/Com/Barra", "Jogador", false);
        syncjob_cloud_folder_path(&t, out, sizeof(out));

        int barras = 0;
        for (const char *p = out; *p; p++) if (*p == '/') barras++;
        ok(barras == 1, "nome de jogo com barra nao cria nivel fantasma");
        eq(out, "Jogo_Com_Barra/Jogador", "a barra do nome vira sublinhado");
    }

    // O mesmo pelo lado do apelido.
    {
        TitleEntry t = jogo("Zelda", "Jo/gador", true);
        syncjob_cloud_folder_path(&t, out, sizeof(out));

        int barras = 0;
        for (const char *p = out; *p; p++) if (*p == '/') barras++;
        ok(barras == 1, "apelido com barra tambem nao cria nivel");
    }
}

// ------------------------------------------------------------- truncagem

static void teste_truncagem(void)
{
    printf("\n== quem sobra na truncagem e o nome do jogo, nunca o dono ==\n");

    // Dois donos do MESMO jogo de nome enorme. Se a truncagem comesse o dono,
    // os dois cairiam no mesmo caminho e um escreveria por cima do save do
    // outro — o pior desfecho possivel aqui.
    //
    // Os numeros importam, e a primeira versao deste teste errou neles: com um
    // apelido de 6 letras a soma 512 + 1 + 6 ainda cabia nos 544 do
    // buffer, entao o teste passava mesmo com a reserva de espaco removida —
    // provado por mutacao. O pior caso de verdade e o apelido no tamanho
    // maximo que o console entrega: TitleEntry.account tem 0x21 bytes, ou seja
    // 32 caracteres. Ai 512 + 1 + 32 = 545 estoura por um byte, e sem a reserva
    // quem perde o ultimo caractere e o DONO.
    //
    // Por isso os dois apelidos abaixo tem 32 caracteres e diferem so no
    // ultimo: e exatamente o par que colide quando a truncagem come o fim.
    char nome[0x201];
    memset(nome, 'A', sizeof(nome) - 1);
    nome[sizeof(nome) - 1] = '\0';

    char dono_a[0x21], dono_b[0x21];
    memset(dono_a, 'B', sizeof(dono_a) - 1);
    memset(dono_b, 'B', sizeof(dono_b) - 1);
    dono_a[sizeof(dono_a) - 2] = '1';
    dono_b[sizeof(dono_b) - 2] = '2';
    dono_a[sizeof(dono_a) - 1] = '\0';
    dono_b[sizeof(dono_b) - 1] = '\0';

    TitleEntry a = jogo(nome, dono_a, true);
    TitleEntry b = jogo(nome, dono_b, true);
    b.uid.uid[0] = 0x2000000000563F8CULL;

    char pa[0x220], pb[0x220];
    syncjob_cloud_folder_path(&a, pa, sizeof(pa));
    syncjob_cloud_folder_path(&b, pb, sizeof(pb));

    ok(strcmp(pa, pb) != 0, "nome de jogo gigante: os dois donos NAO colidem");

    const char *fa = strrchr(pa, '/');
    const char *fb = strrchr(pb, '/');
    ok(fa && strcmp(fa + 1, dono_a) == 0, "o dono chega inteiro (nao perde o fim)");
    ok(fb && strcmp(fb + 1, dono_b) == 0, "o outro dono tambem");
}

// --------------------------------------------------- o registro do pastas.txt

static void teste_registro(void)
{
    printf("\n== pastas.txt: registro novo vale, registro velho nao ==\n");

    char out[0x220];
    TitleEntry t = jogo("Rayman Legends", "Jogador", true);

    // Registro do layout NOVO (com barra): manda, mesmo com o apelido trocado.
    // E pra isso que o pastas.txt existe: renomear a conta nao pode orfanar o
    // backup que ja esta na nuvem.
    syncstate_remember_folder(t.application_id, t.uid, "Rayman Legends/NomeAntigo");
    syncjob_cloud_folder_path(&t, out, sizeof(out));
    eq(out, "Rayman Legends/NomeAntigo",
        "registro com barra manda (apelido trocado nao orfana o backup)");

    // Registro do layout VELHO (achatado, sem barra): tem que ser ignorado,
    // senao o app recria a bagunca do layout antigo.
    syncstate_remember_folder(t.application_id, t.uid, "Rayman Legends (Jogador)");
    syncjob_cloud_folder_path(&t, out, sizeof(out));
    eq(out, "Rayman Legends/Jogador",
        "registro achatado e ignorado (nao recria pasta de topo com apelido)");

    syncstate_forget_folder(t.application_id, t.uid);
}

int main(void)
{
    // O core acha que "sdmc:" e um prefixo de dispositivo; no Mac vira pasta
    // relativa, e por isso o core roda aqui sem alteracao nenhuma.
    mkdir("sdmc:", 0777);

    printf("=========================================\n");
    printf("  caminho da pasta na nuvem\n");
    printf("=========================================\n");

    teste_sempre_dois_niveis();
    teste_nome_do_dono();
    teste_barra();
    teste_truncagem();
    teste_registro();

    printf("\n=========================================\n");
    printf("  %d testes, %d falha(s)\n", testes, falhas);
    printf("=========================================\n");
    return falhas ? 1 : 0;
}
