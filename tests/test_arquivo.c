// As duas rotas que não passam pela decisão do sync: o arquivo único
// (.nxsaves) e o backup no próprio cartão.
//
// O teste do `sync` cobre subir e baixar. Estas aqui são as outras duas formas
// de o save do Miguel sair e voltar, e até agora nenhuma delas tinha sido
// exercitada fora do console.
//
// Mesma nuvem e mesmo savedata falsos do test_sync.c (fake_cloud.c); o
// syncjob.c é o de produção.

#include "syncjob.h"
#include "syncstate.h"
#include "nxsaves.h"

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

void fake_cloud_reset(void);
void fake_save_reset(void);
extern int fake_puts, fake_commits, fake_mount_falha;

// ------------------------------------------------------------------ placar

static int falhas = 0, testes = 0;

static void ok(int cond, const char *msg)
{
    testes++;
    printf(cond ? "  ok   %s\n" : "  FALHA %s\n", msg);
    if (!cond) falhas++;
}

static void escreve(const char *caminho, const char *texto)
{
    FILE *f = fopen(caminho, "wb");
    if (!f) { printf("  !! nao criei %s\n", caminho); exit(2); }
    fputs(texto, f);
    fclose(f);
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

static bool tem_arquivo(const char *caminho)
{
    struct stat st;
    return stat(caminho, &st) == 0 && S_ISREG(st.st_mode);
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

static void do_zero(void)
{
    fake_cloud_reset();
    fake_save_reset();

    TitleEntry t = o_jogo();
    syncstate_forget_folder(t.application_id, t.uid);
}

// ==================================================== o arquivo único

static void caso_arquivo_ida_e_volta(void)
{
    printf("\n== .nxsaves: guardar e tirar de volta ==\n");
    do_zero();

    escreve("save-falso/progresso.dat", "fase 21");
    mkdir("save-falso/perfil", 0777);
    escreve("save-falso/perfil/dados.bin", "detalhes");

    TitleEntry t = o_jogo();

    char caminho[0x300];
    syncjob_archive_path(caminho, sizeof(caminho));

    size_t quantos = syncjob_archive_titles(&t, 1, NULL, NULL);
    ok(quantos == 1, "guardou o save no arquivo");
    ok(nxsaves_is_box(caminho), "e o que saiu tem cara de .nxsaves de verdade");
    ok(fake_commits == 0, "guardar nao commita no savedata (montagem read-only)");

    // Agora o console perde o save e ele tira de volta do arquivo.
    unlink("save-falso/progresso.dat");
    unlink("save-falso/perfil/dados.bin");

    ok(syncjob_archive_restore_title(&t, NULL), "tirou o save de volta do arquivo");

    char buf[64];
    le("save-falso/progresso.dat", buf, sizeof(buf));
    ok(strcmp(buf, "fase 21") == 0, "o conteudo voltou igual");

    le("save-falso/perfil/dados.bin", buf, sizeof(buf));
    ok(strcmp(buf, "detalhes") == 0, "e a subpasta voltou junto");

    ok(fake_commits == 1, "e a volta commitou uma vez, so");
}

// O que mudou hoje: o arquivo de UM jogo vai pra DENTRO da pasta do jogo, e
// nao pra raiz da nuvem, que era onde o "Mario Kart 8 Deluxe.ssaves" dele
// estava largado.
static void caso_arquivo_do_jogo_vai_pra_pasta(void)
{
    printf("\n== o .nxsaves de um jogo mora na pasta daquele jogo ==\n");
    do_zero();
    escreve("save-falso/progresso.dat", "fase 4");

    TitleEntry t = o_jogo();

    char caminho[0x300];
    syncjob_game_archive_path(&t, caminho, sizeof(caminho));

    ok(syncjob_archive_titles_to(caminho, &t, 1, NULL, NULL) == 1,
        "montou o arquivo do jogo no cartao");
    ok(syncjob_game_archive_upload(&t, caminho, NULL), "e subiu");

    ok(tem_arquivo("nuvem-falsa/Jogo de Teste/Jogo de Teste.nxsaves"),
        "caiu DENTRO da pasta do jogo");
    ok(!tem_arquivo("nuvem-falsa/Jogo de Teste.nxsaves"),
        "e nao na raiz, que era o defeito antigo");
}

// Fora da função de propósito: função dentro de função é extensão do GCC, e
// aqui o compilador é o clang.
static void conta_pasta(const char *nome, uint64_t bytes, void *ud)
{
    (void)nome; (void)bytes;
    (*(int *)ud)++;
}

// O arquivo do jogo tem que carregar TODAS as contas — foi isso que ele
// pediu: "isso inclui tudo, todos os usuarios saves e etc".
static void caso_arquivo_leva_todas_as_contas(void)
{
    printf("\n== o arquivo do jogo leva todas as contas ==\n");
    do_zero();

    TitleEntry a = o_jogo();
    snprintf(a.account, sizeof(a.account), "Miguel");
    a.uid.uid[0]  = 0x1111111111111111ULL;
    a.shared_game = true;

    TitleEntry b = o_jogo();
    snprintf(b.account, sizeof(b.account), "Convidado");
    b.uid.uid[0]  = 0x2222222222222222ULL;
    b.shared_game = true;

    escreve("save-falso/progresso.dat", "conteudo qualquer");

    TitleEntry dois[2] = { a, b };
    char caminho[0x300];
    syncjob_game_archive_path(&a, caminho, sizeof(caminho));

    ok(syncjob_archive_titles_to(caminho, dois, 2, NULL, NULL) == 2,
        "as DUAS contas entraram no arquivo do jogo");

    // Confere pelo indice do proprio formato, nao por adivinhacao.
    int pastas = 0;
    nxsaves_list_folders(caminho, conta_pasta, &pastas);
    ok(pastas == 2, "e o indice do arquivo lista duas pastas");
}

// ==================================================== o backup no cartão

static void caso_backup_local(void)
{
    printf("\n== backup no proprio cartao ==\n");
    do_zero();
    escreve("save-falso/progresso.dat", "fase 30");

    TitleEntry t = o_jogo();

    ok(!syncjob_has_local_backup(&t), "comeca sem backup no cartao");
    ok(syncjob_backup_title_local(&t, NULL), "fez o backup");
    ok(syncjob_has_local_backup(&t), "e agora tem");
    ok(fake_commits == 0, "backup nao commita no savedata");

    unlink("save-falso/progresso.dat");
    ok(syncjob_restore_title_local(&t, NULL), "restaurou do cartao");

    char buf[64];
    le("save-falso/progresso.dat", buf, sizeof(buf));
    ok(strcmp(buf, "fase 30") == 0, "e o save voltou igual");
    ok(fake_commits == 1, "a volta commitou uma vez");
}

// Sem rede, sem conta Google: o backup no cartao tem que funcionar assim
// mesmo. E a promessa que o syncjob.h faz sobre ele.
static void caso_local_nao_precisa_de_nuvem(void)
{
    printf("\n== o backup do cartao nao depende de nuvem nenhuma ==\n");
    do_zero();
    escreve("save-falso/progresso.dat", "offline");

    TitleEntry t = o_jogo();
    int antes = fake_puts;

    ok(syncjob_backup_title_local(&t, NULL), "backup local funciona");
    ok(fake_puts == antes, "e nao mandou UM byte pra nuvem");
}

static void caso_jogo_aberto_nao_estraga(void)
{
    printf("\n== com o jogo aberto, nenhuma das rotas escreve ==\n");
    do_zero();
    escreve("save-falso/progresso.dat", "intacto");

    TitleEntry t = o_jogo();
    syncjob_backup_title_local(&t, NULL); // tem backup pra tentar restaurar

    fake_mount_falha = 1; // o jogo prendeu o save

    ok(!syncjob_restore_title_local(&t, NULL), "restore do cartao falha, nao insiste");
    ok(!syncjob_archive_restore_title(&t, NULL), "restore do arquivo tambem");
    ok(fake_commits == 0, "e nada foi commitado no savedata");

    fake_mount_falha = 0;

    char buf[64];
    le("save-falso/progresso.dat", buf, sizeof(buf));
    ok(strcmp(buf, "intacto") == 0, "o save continua do jeito que estava");
}

int main(void)
{
    mkdir("sdmc:", 0777);

    printf("=========================================\n");
    printf("  arquivo unico e backup no cartao\n");
    printf("=========================================\n");

    caso_arquivo_ida_e_volta();
    caso_arquivo_do_jogo_vai_pra_pasta();
    caso_arquivo_leva_todas_as_contas();
    caso_backup_local();
    caso_local_nao_precisa_de_nuvem();
    caso_jogo_aberto_nao_estraga();

    printf("\n=========================================\n");
    printf("  %d testes, %d falha(s)\n", testes, falhas);
    printf("=========================================\n");
    return falhas ? 1 : 0;
}
