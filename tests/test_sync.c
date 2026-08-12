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
extern int fake_puts, fake_gets, fake_commits, fake_mount_falha, fake_remove_falha;

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

static void apaga_pasta(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)))
    {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char p[1024];
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        unlink(p);
    }
    closedir(d);
    rmdir(dir);
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

// ======================================================= mais de uma conta
//
// O caso do Miguel: quatro contas com save do mesmo Rayman Legends. Aqui mora
// o risco que o layout aninhado criou — a pasta do jogo virou CONTAINER, com
// as contas e o .nxsaves do jogo dentro. Se o prune ou o restore olharem pro
// nivel errado, sincronizar a conta de um apaga ou entrega o save do outro.

static TitleEntry o_jogo_de(const char *apelido, u64 uid0)
{
    TitleEntry t = o_jogo();
    snprintf(t.account, sizeof(t.account), "%s", apelido);
    t.uid.uid[0]  = uid0;
    t.shared_game = true; // duas contas com save do mesmo jogo
    return t;
}

static void caso_duas_contas(void)
{
    printf("\n== duas contas do mesmo jogo ==\n");
    do_zero();

    TitleEntry a = o_jogo_de("Miguel", 0x1111111111111111ULL);
    TitleEntry b = o_jogo_de("Convidado",  0x2222222222222222ULL);

    escreve("save-falso/progresso.dat", "save do Miguel");
    res_eq(syncjob_sync_title(&a, NULL), SYNCJOB_SYNC_UPLOADED, "conta A sobe");

    // O savedata falso e um so, entao trocar o conteudo simula trocar de conta.
    escreve("save-falso/progresso.dat", "save da Convidado");
    res_eq(syncjob_sync_title(&b, NULL), SYNCJOB_SYNC_UPLOADED, "conta B sobe");

    char buf[64];
    le("nuvem-falsa/Jogo de Teste/Miguel/progresso.dat", buf, sizeof(buf));
    ok(strcmp(buf, "save do Miguel") == 0,
        "o save da conta A continua la, intacto, depois de B subir");

    le("nuvem-falsa/Jogo de Teste/Convidado/progresso.dat", buf, sizeof(buf));
    ok(strcmp(buf, "save da Convidado") == 0, "e o de B esta no lugar dele");
}

// O prune existe pra tirar da nuvem o que o save nao tem mais. Ele roda na
// pasta do DONO. Se rodasse na pasta do jogo, veria as outras contas como
// "sobra" — porque elas nao existem no staging — e apagaria o backup delas.
static void caso_prune_nao_come_o_vizinho(void)
{
    printf("\n== o prune nao pode encostar na conta vizinha ==\n");
    do_zero();

    TitleEntry a = o_jogo_de("Miguel", 0x1111111111111111ULL);
    TitleEntry b = o_jogo_de("Convidado",  0x2222222222222222ULL);

    escreve("save-falso/progresso.dat", "A");
    syncjob_sync_title(&a, NULL);
    escreve("save-falso/progresso.dat", "B");
    syncjob_sync_title(&b, NULL);

    // A conta A perde um arquivo e sobe de novo: e aqui que o prune dispara.
    escreve("save-falso/progresso.dat", "A mudou");
    escreve("save-falso/extra.dat", "vai sumir depois");
    syncjob_sync_title(&a, NULL);
    unlink("save-falso/extra.dat");
    escreve("save-falso/progresso.dat", "A mudou de novo");
    res_eq(syncjob_sync_title(&a, NULL), SYNCJOB_SYNC_UPLOADED, "A sobe de novo");

    ok(!tem_arquivo("nuvem-falsa/Jogo de Teste/Miguel/extra.dat"),
        "o prune tirou da nuvem o arquivo que o save nao tem mais");
    ok(tem_arquivo("nuvem-falsa/Jogo de Teste/Convidado/progresso.dat"),
        "e NAO encostou na pasta da conta vizinha");
}

// O .nxsaves do jogo mora na pasta do JOGO, um nivel acima das contas. Um
// sync de conta nao pode leva-lo embora.
static void caso_arquivo_do_jogo_sobrevive(void)
{
    printf("\n== o .nxsaves do jogo sobrevive a um sync de conta ==\n");
    do_zero();

    TitleEntry a = o_jogo_de("Miguel", 0x1111111111111111ULL);
    escreve("save-falso/progresso.dat", "A");
    syncjob_sync_title(&a, NULL);

    // Simula o arquivo unico do jogo, que o jobGameArchive poe aqui.
    escreve("nuvem-falsa/Jogo de Teste/Jogo de Teste.nxsaves", "conteudo do arquivao");

    escreve("save-falso/progresso.dat", "A mudou");
    res_eq(syncjob_sync_title(&a, NULL), SYNCJOB_SYNC_UPLOADED, "a conta sobe de novo");

    ok(tem_arquivo("nuvem-falsa/Jogo de Teste/Jogo de Teste.nxsaves"),
        "o arquivo unico do jogo continua la");
}

// Restaurar a conta A nao pode trazer arquivo da conta B junto.
static void caso_restore_nao_mistura(void)
{
    printf("\n== restaurar uma conta nao traz a outra junto ==\n");
    do_zero();

    TitleEntry a = o_jogo_de("Miguel", 0x1111111111111111ULL);
    TitleEntry b = o_jogo_de("Convidado",  0x2222222222222222ULL);

    escreve("save-falso/progresso.dat", "so do Miguel");
    syncjob_sync_title(&a, NULL);
    escreve("save-falso/Convidado-only.dat", "so da Convidado");
    unlink("save-falso/progresso.dat");
    syncjob_sync_title(&b, NULL);

    // Console zerado; restaura so a conta A.
    unlink("save-falso/Convidado-only.dat");
    ok(syncjob_restore_title(&a, NULL), "o restore da conta A funciona");

    ok(tem_arquivo("save-falso/progresso.dat"), "trouxe o arquivo da conta A");
    ok(!tem_arquivo("save-falso/Convidado-only.dat"),
        "e NAO trouxe o arquivo da conta B");
}

// ============================== o prune que falha
//
// O prune tira da nuvem o que o save nao tem mais. Quando ele falha, a nuvem
// fica com um arquivo que o jogo apagou — e marcar "sincronizado" nessa hora
// mente: o sync seguinte ve a nuvem mais nova, desce por cima do savedata e
// RESSUSCITA o arquivo apagado dentro do save.
static void caso_prune_falhou_nao_marca(void)
{
    printf("\n== prune falhou: nao pode dizer que esta sincronizado ==\n");
    do_zero();

    TitleEntry t = o_jogo();
    escreve("save-falso/a.dat", "fica");
    escreve("save-falso/b.dat", "vai ser apagado pelo jogo");
    res_eq(syncjob_sync_title(&t, NULL), SYNCJOB_SYNC_UPLOADED, "sobe os dois arquivos");

    // O jogo apaga o b.dat. Na hora de subir, o DELETE na nuvem falha.
    unlink("save-falso/b.dat");
    fake_remove_falha = 1;
    res_eq(syncjob_sync_title(&t, NULL), SYNCJOB_SYNC_UPLOADED, "sobe mesmo assim");
    fake_remove_falha = 0;

    ok(tem_arquivo("nuvem-falsa/Jogo de Teste/Miguel/b.dat"),
        "o arquivo sobrou na nuvem, como esperado");

    // A prova: o proximo sync NAO pode trazer o b.dat de volta pro savedata.
    SyncjobSyncResult r = syncjob_sync_title(&t, NULL);
    ok(r != SYNCJOB_SYNC_DOWNLOADED,
        "o sync seguinte NAO baixa a sobra por cima do save");
    ok(!tem_arquivo("save-falso/b.dat"),
        "e o arquivo que o jogo apagou NAO ressuscita dentro do save");
}

// O .nxsaves do jogo mora solto na pasta do jogo — que e exatamente a marca
// que o cloud_flat_backup usa pra reconhecer backup do layout antigo. Se ele
// se confundir, devolve o CONTAINER, e o restore escreve o proprio .nxsaves
// (dezenas de MB) dentro do savedata.
static void caso_arquivo_do_jogo_nao_vira_backup(void)
{
    printf("\n== o .nxsaves do jogo nao pode passar por backup antigo ==\n");
    do_zero();

    TitleEntry t = o_jogo();
    escreve("save-falso/progresso.dat", "o save bom");
    syncjob_sync_title(&t, NULL);

    // O app poe o arquivo do jogo aqui, no primeiro nivel da pasta do jogo.
    escreve("nuvem-falsa/Jogo de Teste/Jogo de Teste.nxsaves", "blob gigante");

    // Agora some a pasta da conta: e o estado em que o resgate nao acha nada e
    // o fallback achatado e consultado.
    apaga_pasta("nuvem-falsa/Jogo de Teste/Miguel");

    unlink("save-falso/progresso.dat");
    ok(!syncjob_restore_title(&t, NULL),
        "restore diz que NAO tem backup, em vez de entregar o container");
    ok(!tem_arquivo("save-falso/Jogo de Teste.nxsaves"),
        "e o .nxsaves NAO foi escrito dentro do savedata");
    ok(fake_commits == 0, "nada commitado");
}

// A porta que o AUTOSYNC usa, e que nenhum teste tocava.
//
// Quando o jogo fecha, o sysmodule chama o syncjob_backup_title_ex e decide,
// pelo `nuvem_bate`, se pode marcar aquele save como sincronizado. Marcar
// errado nao e um detalhe: o marcador e o que faz o proximo sync decidir que a
// nuvem esta na frente — e ai o arquivo que o jogo apagou VOLTA pra dentro do
// savedata. Foi um bug de verdade, e ninguem estava olhando essa saida.
static void caso_backup_ex_conta_a_verdade(void)
{
    printf("\n== o backup do autosync diz quando a nuvem NAO bate ==\n");
    do_zero();

    TitleEntry t = o_jogo();
    escreve("save-falso/a.dat", "fica");
    escreve("save-falso/b.dat", "o jogo vai apagar");

    bool bate = false;
    ok(syncjob_backup_title_ex(&t, NULL, &bate), "subiu os dois arquivos");
    ok(bate, "e a nuvem bate com o save: pode marcar como sincronizado");

    // O jogo apaga o b.dat e, na hora de subir, o DELETE da nuvem falha.
    unlink("save-falso/b.dat");
    fake_remove_falha = 1;
    bate = true; // se ninguem escrever, o teste tem que pegar
    ok(syncjob_backup_title_ex(&t, NULL, &bate), "sobe mesmo com a limpeza falhando");
    ok(!bate, "e AVISA que a nuvem nao bate — o autosync nao pode marcar");
    fake_remove_falha = 0;

    ok(tem_arquivo("nuvem-falsa/Jogo de Teste/Miguel/b.dat"),
        "a sobra continua la, que e o motivo do aviso");

    // Com a limpeza funcionando, volta a bater.
    ok(syncjob_backup_title_ex(&t, NULL, &bate), "sobe de novo, agora sem falha");
    ok(bate, "e agora bate");
    ok(!tem_arquivo("nuvem-falsa/Jogo de Teste/Miguel/b.dat"), "a sobra saiu da nuvem");

    // E o de sempre: nada disso encosta no savedata pra escrever.
    ok(fake_commits == 0, "backup nao commita no savedata em momento nenhum");
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

    caso_duas_contas();
    caso_prune_nao_come_o_vizinho();
    caso_arquivo_do_jogo_sobrevive();
    caso_restore_nao_mistura();

    caso_prune_falhou_nao_marca();
    caso_arquivo_do_jogo_nao_vira_backup();
    caso_backup_ex_conta_a_verdade();

    printf("\n=========================================\n");
    printf("  %d testes, %d falha(s)\n", testes, falhas);
    printf("=========================================\n");
    return falhas ? 1 : 0;
}
