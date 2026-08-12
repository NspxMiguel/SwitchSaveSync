// A matriz de decisão do syncjob_sync_title, rodando inteira no Mac.
//
// Essa função decide sozinha se sobe, se baixa, se não mexe ou se recusa por
// conflito. Errar aqui apaga progresso de jogo — e até agora a única forma de
// exercitar isso era no console do Miguel, com o save de verdade dele, que é o
// pior lugar do mundo pra descobrir um erro.
//
// A nuvem e o savedata são falsos (fake_cloud.c), mas o syncjob.c é o de
// produção, sem uma linha alterada.

#include "syncjob.h"
#include "syncstate.h"
#include "cloud.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

bool lang_is_pt(void) { return true; }

Result timeGetCurrentTime(TimeType type, u64 *out)
{
    (void)type; *out = 0; return 1;
}

// do fake_cloud.c
void fake_cloud_reset(void);
void fake_save_reset(void);
extern int fake_puts, fake_gets, fake_commits, fake_mount_falha;

// ------------------------------------------------------------------ placar

static int falhas = 0, testes = 0;

static void ok(int cond, const char *msg)
{
    testes++;
    printf(cond ? "  ok   %s\n" : "  FALHA %s\n", msg);
    if (!cond) falhas++;
}

static const char *nome_res(SyncjobSyncResult r)
{
    switch (r)
    {
        case SYNCJOB_SYNC_FAILED:     return "FAILED";
        case SYNCJOB_SYNC_NOTHING:    return "NOTHING";
        case SYNCJOB_SYNC_EQUAL:      return "EQUAL";
        case SYNCJOB_SYNC_UPLOADED:   return "UPLOADED";
        case SYNCJOB_SYNC_DOWNLOADED: return "DOWNLOADED";
        case SYNCJOB_SYNC_CONFLICT:   return "CONFLICT";
    }
    return "?";
}

static void res_eq(SyncjobSyncResult got, SyncjobSyncResult want, const char *msg)
{
    testes++;
    int bom = got == want;
    printf(bom ? "  ok   %s\n" : "  FALHA %s\n", msg);
    if (!bom)
    {
        printf("         esperava: %s\n         veio:     %s\n",
            nome_res(want), nome_res(got));
        falhas++;
    }
}

// O caminho de um arquivo do save deste jogo dentro da nuvem falsa.
//
// Sem o nivel "Nintendo Switch Saves": a raiz da nuvem falsa JA e a pasta do
// app, porque quem cria esse nivel no Drive de verdade e o drive.c, e ele esta
// substituido aqui. O que interessa testar e o que vem DEPOIS dele — <Jogo>/
// <Dono>/ —, que e a regra do syncjob.
#define NA_NUVEM(resto) "nuvem-falsa/Jogo de Teste/Miguel/" resto

// -------------------------------------------------------------- utilidades

static void escreve(const char *caminho, const char *texto)
{
    FILE *f = fopen(caminho, "wb");
    if (!f) { printf("  !! nao criei %s\n", caminho); exit(2); }
    fputs(texto, f);
    fclose(f);
}

static bool tem_arquivo(const char *caminho)
{
    struct stat st;
    return stat(caminho, &st) == 0 && S_ISREG(st.st_mode);
}

static void le(const char *caminho, char *out, size_t n)
{
    out[0] = '\0';
    FILE *f = fopen(caminho, "rb");
    if (!f) return;
    size_t lidos = fread(out, 1, n - 1, f);
    out[lidos] = '\0';
    fclose(f);
}

static TitleEntry o_jogo(void)
{
    TitleEntry t = {0};
    snprintf(t.name, sizeof(t.name), "Jogo de Teste");
    snprintf(t.account, sizeof(t.account), "Miguel");
    t.application_id = 0x0100152000022000ULL;
    t.uid.uid[0]     = 0x1000000000563F8CULL;
    t.uid.uid[1]     = 0x4FEBE4CDBD951CB7ULL;
    return t;
}

// Zera tudo: nuvem, save, marcadores e registro de pasta.
static void do_zero(void)
{
    fake_cloud_reset();
    fake_save_reset();

    TitleEntry t = o_jogo();
    syncstate_forget_folder(t.application_id, t.uid);

    // Os marcadores de fingerprint moram em rev-*.txt na pasta do app.
    DIR *d = opendir("sdmc:/switch/SwitchSaveSync");
    if (d)
    {
        struct dirent *e;
        while ((e = readdir(d)))
        {
            if (strncmp(e->d_name, "rev-", 4) == 0)
            {
                char p[512];
                snprintf(p, sizeof(p), "sdmc:/switch/SwitchSaveSync/%s", e->d_name);
                unlink(p);
            }
        }
        closedir(d);
    }
}

// ============================================================ os casos

static void caso_nada_em_lugar_nenhum(void)
{
    printf("\n== sem save aqui e sem save la ==\n");
    do_zero();

    TitleEntry t = o_jogo();
    res_eq(syncjob_sync_title(&t, NULL), SYNCJOB_SYNC_NOTHING,
        "os dois lados vazios: nao faz nada");
    ok(fake_commits == 0, "e nao commita nada no savedata");
}

static void caso_primeira_subida(void)
{
    printf("\n== so tem save aqui: primeira sync ==\n");
    do_zero();
    escreve("save-falso/progresso.dat", "fase 3");

    TitleEntry t = o_jogo();
    res_eq(syncjob_sync_title(&t, NULL), SYNCJOB_SYNC_UPLOADED,
        "sobe o save do console");
    ok(fake_puts > 0, "e algo foi mesmo enviado");
    ok(tem_arquivo(NA_NUVEM("progresso.dat")),
        "o arquivo caiu em <Jogo>/<Dono>/ na nuvem");
}

// ESTE e o teste que teria pego o pior bug: subir e sincronizar
// de novo. Se o caminho que ESCREVE e o que LE nao forem o mesmo, a segunda
// chamada nao acha nada, cai em "primeira sync" outra vez e sobe de novo pra
// sempre — que foi exatamente o que a pasta com barra no nome causou.
static void caso_ida_e_volta(void)
{
    printf("\n== subir e sincronizar de novo (o caminho fecha?) ==\n");
    do_zero();
    escreve("save-falso/progresso.dat", "fase 3");

    TitleEntry t = o_jogo();
    res_eq(syncjob_sync_title(&t, NULL), SYNCJOB_SYNC_UPLOADED, "primeira: sobe");
    res_eq(syncjob_sync_title(&t, NULL), SYNCJOB_SYNC_EQUAL,
        "segunda: acha o que subiu e diz que ja esta igual");
    ok(fake_commits == 0, "e nenhuma das duas escreveu no savedata");
}

static void caso_so_na_nuvem(void)
{
    printf("\n== so tem save la: traz pra ca ==\n");
    do_zero();

    // Coloca um save na nuvem por outro console.
    escreve("save-falso/progresso.dat", "fase 9");
    TitleEntry t = o_jogo();
    syncjob_sync_title(&t, NULL);   // sobe

    // Agora o console perde o save (jogo reinstalado).
    unlink("save-falso/progresso.dat");

    res_eq(syncjob_sync_title(&t, NULL), SYNCJOB_SYNC_DOWNLOADED,
        "console vazio e nuvem cheia: baixa sem perguntar");
    ok(fake_commits == 1, "commitou uma vez (a escrita no savedata)");

    char buf[64];
    le("save-falso/progresso.dat", buf, sizeof(buf));
    ok(strcmp(buf, "fase 9") == 0, "e o conteudo que chegou e o da nuvem");
}

static void caso_conflito_sem_marcador(void)
{
    printf("\n== os dois lados tem save e nunca sincronizaram ==\n");
    do_zero();

    // Sobe um save e depois apaga o marcador, deixando os dois lados
    // diferentes e sem historia — e o console de outra pessoa.
    escreve("save-falso/progresso.dat", "da nuvem");
    TitleEntry t = o_jogo();
    syncjob_sync_title(&t, NULL);

    DIR *d = opendir("sdmc:/switch/SwitchSaveSync");
    if (d)
    {
        struct dirent *e;
        while ((e = readdir(d)))
            if (strncmp(e->d_name, "rev-", 4) == 0)
            {
                char p[512];
                snprintf(p, sizeof(p), "sdmc:/switch/SwitchSaveSync/%s", e->d_name);
                unlink(p);
            }
        closedir(d);
    }
    escreve("save-falso/progresso.dat", "do console, diferente");

    res_eq(syncjob_sync_title(&t, NULL), SYNCJOB_SYNC_CONFLICT,
        "sem marcador e com os dois diferentes: RECUSA e deixa escolher");
    ok(fake_commits == 0, "e nao escreve em savedata nenhum");

    char buf[64];
    le("save-falso/progresso.dat", buf, sizeof(buf));
    ok(strcmp(buf, "do console, diferente") == 0,
        "o save do console fica intacto");
}

static void caso_nuvem_andou(void)
{
    printf("\n== so a nuvem mudou desde a ultima sync ==\n");
    do_zero();
    escreve("save-falso/progresso.dat", "fase 3");

    TitleEntry t = o_jogo();
    syncjob_sync_title(&t, NULL); // sobe e grava marcador

    // Outro console avanca o save e sobe. Aqui: mexe direto na nuvem falsa.
    escreve(NA_NUVEM("progresso.dat"),
        "fase 7");

    res_eq(syncjob_sync_title(&t, NULL), SYNCJOB_SYNC_DOWNLOADED,
        "console parado e nuvem nova: baixa");

    char buf[64];
    le("save-falso/progresso.dat", buf, sizeof(buf));
    ok(strcmp(buf, "fase 7") == 0, "o save do console virou o da nuvem");
}

static void caso_console_andou(void)
{
    printf("\n== so o console mudou desde a ultima sync ==\n");
    do_zero();
    escreve("save-falso/progresso.dat", "fase 3");

    TitleEntry t = o_jogo();
    syncjob_sync_title(&t, NULL);

    escreve("save-falso/progresso.dat", "fase 12");

    res_eq(syncjob_sync_title(&t, NULL), SYNCJOB_SYNC_UPLOADED,
        "nuvem parada e console novo: sobe");

    char buf[64];
    le(NA_NUVEM("progresso.dat"),
        buf, sizeof(buf));
    ok(strcmp(buf, "fase 12") == 0, "a nuvem recebeu o save novo");
    ok(fake_commits == 0, "e o savedata nao foi tocado");
}

static void caso_os_dois_andaram(void)
{
    printf("\n== os DOIS mudaram desde a ultima sync ==\n");
    do_zero();
    escreve("save-falso/progresso.dat", "fase 3");

    TitleEntry t = o_jogo();
    syncjob_sync_title(&t, NULL);

    escreve("save-falso/progresso.dat", "console foi pra fase 12");
    escreve(NA_NUVEM("progresso.dat"),
        "nuvem foi pra fase 7");

    res_eq(syncjob_sync_title(&t, NULL), SYNCJOB_SYNC_CONFLICT,
        "mudou dos dois lados: RECUSA");
    ok(fake_commits == 0, "nao escreve no savedata");

    char buf[64];
    le("save-falso/progresso.dat", buf, sizeof(buf));
    ok(strcmp(buf, "console foi pra fase 12") == 0, "o save do console fica intacto");

    le(NA_NUVEM("progresso.dat"),
        buf, sizeof(buf));
    ok(strcmp(buf, "nuvem foi pra fase 7") == 0, "e o da nuvem tambem");
}

static void caso_jogo_aberto(void)
{
    printf("\n== o jogo esta rodando agora ==\n");
    do_zero();
    escreve("save-falso/progresso.dat", "fase 3");

    fake_mount_falha = 1; // e o que acontece com o jogo vivo segurando o save

    TitleEntry t = o_jogo();
    res_eq(syncjob_sync_title(&t, NULL), SYNCJOB_SYNC_FAILED,
        "nao consegue montar: falha em vez de inventar");
    ok(fake_puts == 0, "e nao sobe save meio lido pra nuvem");

    fake_mount_falha = 0;
}

static void caso_subpasta_no_save(void)
{
    printf("\n== save com subpasta (arvore, nao arquivo solto) ==\n");
    do_zero();
    mkdir("save-falso/perfil", 0777);
    escreve("save-falso/perfil/dados.bin", "abc");
    escreve("save-falso/topo.dat", "xyz");

    TitleEntry t = o_jogo();
    res_eq(syncjob_sync_title(&t, NULL), SYNCJOB_SYNC_UPLOADED, "sobe a arvore");
    ok(tem_arquivo(NA_NUVEM("perfil/dados.bin")),
        "a subpasta foi junto");

    res_eq(syncjob_sync_title(&t, NULL), SYNCJOB_SYNC_EQUAL,
        "e o fingerprint da arvore fecha na volta");
}

int main(void)
{
    mkdir("sdmc:", 0777);

    printf("=========================================\n");
    printf("  a decisao do sync, ponta a ponta\n");
    printf("=========================================\n");

    caso_nada_em_lugar_nenhum();
    caso_primeira_subida();
    caso_ida_e_volta();
    caso_so_na_nuvem();
    caso_conflito_sem_marcador();
    caso_nuvem_andou();
    caso_console_andou();
    caso_os_dois_andaram();
    caso_jogo_aberto();
    caso_subpasta_no_save();

    printf("\n=========================================\n");
    printf("  %d testes, %d falha(s)\n", testes, falhas);
    printf("=========================================\n");
    return falhas ? 1 : 0;
}
