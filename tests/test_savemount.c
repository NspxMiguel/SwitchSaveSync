// Teste do core/savemount.c — o único caminho do projeto que APAGA save.
//
// Por que este teste existe: até agora o savemount.c era o único arquivo do
// core que nenhum teste tocava. Os testes que precisavam dele linkavam um
// stub (tests/stubs_console.c) que devolve false pra tudo. Ou seja: a função
// que apaga o save do jogo do dono do console nunca tinha rodado fora do
// console. E o app acabou de ganhar um botão "Esvaziar o save deste jogo".
//
// Metade do savemount.c é serviço do console; a outra metade — a que apaga e a
// que copia — é POSIX puro. Com os mounts trocados (ver stubs_fsdev.c), o
// MESMO arquivo que roda no Switch roda aqui.
//
// O que este teste NÃO cobre, e nenhum teste no Mac cobre:
//   - o mount somente-leitura de verdade (aqui unlink funciona igual);
//   - o commit de verdade (aqui todo unlink já é definitivo — o que dá pra
//     conferir é se o savemount chamou fsdevCommitDevice ou não);
//   - o journal do save data acabando no meio de uma escrita.

#include "savemount.h"

#include <dirent.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// Os contadores do stub.
extern int  fsdev_commits;
extern int  fsdev_unmounts;
extern int  fsdev_mounts;
extern bool fsdev_last_readonly;
extern bool fsdev_last_device;
extern bool fsdev_mount_falha;

// ------------------------------------------------------------------ placar

static int falhas = 0, testes = 0;

static void ok(int cond, const char *msg)
{
    testes++;
    if (cond) { printf("  ok      %s\n", msg); }
    else      { printf("  FALHOU  %s\n", msg); falhas++; }
}

// --------------------------------------------------------------- ajudantes

static void escreve(const char *caminho, const char *conteudo)
{
    FILE *f = fopen(caminho, "wb");
    if (!f) { printf("nao consegui escrever %s\n", caminho); exit(1); }
    fwrite(conteudo, 1, strlen(conteudo), f);
    fclose(f);
}

static bool existe(const char *caminho)
{
    struct stat st;
    return stat(caminho, &st) == 0;
}

static bool eh_pasta(const char *caminho)
{
    struct stat st;
    return stat(caminho, &st) == 0 && S_ISDIR(st.st_mode);
}

static char *le(const char *caminho)
{
    static char buf[4096];
    buf[0] = '\0';
    FILE *f = fopen(caminho, "rb");
    if (!f) return buf;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

// Quantas entradas tem dentro da pasta (sem contar . e ..).
static int conta(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) return -1;
    int n = 0;
    struct dirent *e;
    while ((e = readdir(d)) != NULL)
        if (strcmp(e->d_name, ".") && strcmp(e->d_name, ".."))
            n++;
    closedir(d);
    return n;
}

// Um save com coisa em vários níveis — é assim que save de jogo de verdade é.
// O Animal Crossing, por exemplo, guarda a ilha numa subpasta.
static void monta_save_de_exemplo(void)
{
    system("rm -rf 'save:'");
    mkdir("save:", 0777);
    escreve("save:/game.dat", "progresso");
    escreve("save:/config.bin", "opcoes");
    mkdir("save:/slot1", 0777);
    escreve("save:/slot1/a.sav", "slot 1");
    mkdir("save:/slot1/fundo", 0777);
    escreve("save:/slot1/fundo/b.sav", "bem la no fundo");
    mkdir("save:/vazia", 0777);
}

static AccountUid UID = { { 0x1122334455667788ULL, 0x99AABBCCDDEEFF00ULL } };

// -----------------------------------------------------------------------

int main(void)
{
    printf("\n-- montar e desmontar --\n");
    fsdev_mount_falha = false;
    ok(savemount_mount(0x0100000000010000ULL, UID, true), "monta somente leitura");
    ok(fsdev_last_readonly, "e pediu o mount somente leitura pra libnx");
    ok(!savemount_mount(0x0100000000010000ULL, UID, true),
       "recusa montar de novo com um save ja montado");
    savemount_unmount(false);
    ok(fsdev_commits == 0, "desmontar sem commit NAO chama o commit");
    ok(fsdev_unmounts == 1, "mas desmonta");

    ok(savemount_mount(0x0100000000010000ULL, UID, false), "monta pra escrita");
    ok(!fsdev_last_readonly, "e desta vez sem o somente leitura");
    savemount_unmount(true);
    ok(fsdev_commits == 1, "desmontar com commit chama o commit");

    printf("\n-- device save (o do console, sem dono) --\n");
    AccountUid zero = { { 0, 0 } };
    ok(savemount_mount_typed(0x0100000000010000ULL, zero, true, false),
       "monta device save");
    ok(fsdev_last_device, "pelo caminho de device save, e nao pelo de conta");
    savemount_unmount(false);

    printf("\n-- mount que falha --\n");
    fsdev_mount_falha = true;
    ok(!savemount_mount(0x0100000000010000ULL, UID, false),
       "mount recusado devolve false");
    savemount_unmount(false); // nao pode estourar nem chamar commit
    int commits_antes = fsdev_commits;
    savemount_unmount(true);
    ok(fsdev_commits == commits_antes,
       "desmontar sem nada montado nao chama commit (nem estoura)");
    fsdev_mount_falha = false;

    printf("\n-- esvaziar sem save montado --\n");
    ok(!savemount_wipe_contents(), "recusa apagar sem nada montado");

    printf("\n-- esvaziar de verdade --\n");
    monta_save_de_exemplo();
    ok(savemount_mount(0x0100000000010000ULL, UID, false), "montou");
    ok(savemount_wipe_contents(), "a limpeza disse que deu certo");

    ok(eh_pasta("save:"), "o container do save CONTINUA existindo");
    ok(conta("save:") == 0, "e ficou vazio por dentro");
    ok(!existe("save:/game.dat"), "arquivo do primeiro nivel foi embora");
    ok(!existe("save:/slot1"), "a subpasta foi embora inteira");
    ok(!existe("save:/slot1/fundo/b.sav"), "inclusive o que estava dois niveis abaixo");
    ok(!existe("save:/vazia"), "e a subpasta vazia tambem");
    savemount_unmount(true);

    printf("\n-- esvaziar o que ja esta vazio --\n");
    ok(savemount_mount(0x0100000000010000ULL, UID, false), "montou");
    ok(savemount_wipe_contents(), "apagar nada da certo (e nao 'falhou')");
    savemount_unmount(true);

    // O caso que o botao "Esvaziar o save deste jogo" precisa acertar: se a
    // limpeza parar no meio, o savedata fica com METADE do save. O app so
    // pode fazer commit se a limpeza inteira deu certo — por isso a funcao
    // tem que saber DIZER que falhou.
    printf("\n-- limpeza que para no meio tem que devolver false --\n");
    monta_save_de_exemplo();
    // Pasta sem permissao de escrita: o unlink de dentro dela falha, do mesmo
    // jeito que o fs do console recusaria.
    chmod("save:/slot1", 0500);
    ok(savemount_mount(0x0100000000010000ULL, UID, false), "montou");
    bool limpou = savemount_wipe_contents();
    ok(!limpou, "a limpeza avisa que NAO deu certo");
    ok(existe("save:/slot1/a.sav"), "e o que ela nao conseguiu apagar continua la");
    // E o chamador, vendo false, desmonta SEM commit — que e o que o
    // jobWipeSave do app faz (savemount_unmount(ok)).
    commits_antes = fsdev_commits;
    savemount_unmount(limpou);
    ok(fsdev_commits == commits_antes, "e ninguem faz commit de meia limpeza");
    chmod("save:/slot1", 0700);

    printf("\n-- copiar arvore (save -> cartao) --\n");
    monta_save_de_exemplo();
    system("rm -rf staging");
    // O copy_tree nao precisa de mount (ele so anda em caminho), mas o
    // wipe_contents lá embaixo precisa — e o bloco de cima desmontou.
    ok(savemount_mount(0x0100000000010000ULL, UID, false), "montou");
    ok(savemount_copy_tree("save:/", "staging"), "copiou");
    ok(existe("staging/game.dat"), "trouxe o arquivo do primeiro nivel");
    ok(strcmp(le("staging/game.dat"), "progresso") == 0, "com o conteudo certo");
    ok(existe("staging/slot1/fundo/b.sav"), "e o de dois niveis abaixo");
    ok(strcmp(le("staging/slot1/fundo/b.sav"), "bem la no fundo") == 0,
       "tambem com o conteudo certo");
    ok(eh_pasta("staging/vazia"), "a pasta vazia veio junto (o jogo pode contar com ela)");

    printf("\n-- copiar de volta (cartao -> save) --\n");
    ok(savemount_wipe_contents(), "esvaziou o save antes");
    ok(savemount_copy_tree("staging", "save:/"), "copiou de volta");
    ok(strcmp(le("save:/slot1/fundo/b.sav"), "bem la no fundo") == 0,
       "o save voltou byte a byte");
    ok(conta("save:") == conta("staging"), "com a mesma quantidade de entradas na raiz");
    savemount_unmount(true);

    printf("\n-- copiar de uma pasta que nao existe --\n");
    ok(!savemount_copy_tree("nao-existe", "staging2"),
       "avisa que falhou em vez de dizer que copiou nada com sucesso");

    printf("\n-- arquivo de zero byte --\n");
    system("rm -rf 'save:' staging3");
    mkdir("save:", 0777);
    escreve("save:/vazio.dat", "");
    ok(savemount_copy_tree("save:/", "staging3"), "copiou");
    ok(existe("staging3/vazio.dat"), "e o arquivo de zero byte chegou do outro lado");

    system("rm -rf 'save:' staging staging2 staging3");

    printf("\n%d/%d passaram\n", testes - falhas, testes);
    return falhas ? 1 : 0;
}
