// Teste do Google Drive de verdade, numa conta real, rodando do Mac.
//
// Era o buraco que sobrava: os outros testes cobrem o formato do arquivo, o
// leitor de JSON e o espelhamento, mas o caminho de rede do Drive só o console
// exercitava.
//
// COMO SE ENTRA NA CONTA
//
// O login do app é device flow: o programa mostra um código, você digita no
// seu navegador e aprova. Nenhuma senha passa por aqui. E o normal é nem
// precisar disso: se já houver um refresh token guardado (o do cartão serve),
// o teste usa ele e não pede nada.
//
// O QUE ESTE TESTE NÃO PODE FAZER
//
// Encostar nos backups de verdade. Tudo acontece dentro de UMA pasta criada
// pra isto, com nome que diz o que é, apagada no fim. O cloud_prune_extras,
// que é a única operação que apaga da nuvem, só roda dentro dessa pasta.
//
// Nada de token ou senha é impresso na tela em momento nenhum.

#include <switch.h>   // o de mentira, do stubs/

#include "cloud.h"
#include "oauth.h"
#include "http.h"
#include "config.h"
#include "cloud_backend.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <time.h>

// A pasta onde tudo acontece, dentro da raiz do app. O nome é feio de
// propósito: se alguma coisa der errado no meio e ela sobrar no Drive,
// tem que ser óbvio que é lixo de teste e que pode apagar.
#define PASTA_TESTE "_teste do Mac (pode apagar)"

// ----------------------------------------------------------------- o que falta

bool lang_is_pt(void) { return true; }

// No console isto e syscall; aqui e nanosleep. O oauth.c usa pra esperar
// entre uma pergunta e outra ao Google enquanto o codigo nao e aprovado.
void svcSleepThread(u64 nanos)
{
    struct timespec t = { (time_t)(nanos / 1000000000ULL), (long)(nanos % 1000000000ULL) };
    nanosleep(&t, NULL);
}

void syncstate_ensure_dirs(void)
{
    mkdir("sdmc:", 0777);
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/SwitchSaveSync", 0777);
}

// ------------------------------------------------------------------ placar

static int falhas = 0, testes = 0;

static void ok(int cond, const char *msg)
{
    testes++;
    printf(cond ? "  ok   %s\n" : "  FALHA %s\n", msg);
    fflush(stdout);
    if (!cond) falhas++;
}

// ----------------------------------------------------------------- utilidades

static void escrever(const char *path, const char *txt)
{
    FILE *f = fopen(path, "wb");
    if (!f) { printf("  !! nao criei %s\n", path); return; }
    fputs(txt, f);
    fclose(f);
}

static int conteudo_igual(const char *path, const char *esperado)
{
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char buf[4096] = {0};
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    return strcmp(buf, esperado) == 0;
}

static int existe(const char *path)
{
    struct stat st;
    return stat(path, &st) == 0;
}

static void rm_rf(const char *path)
{
    struct stat st;
    if (lstat(path, &st) != 0) return;
    if (S_ISDIR(st.st_mode)) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *e;
            while ((e = readdir(d))) {
                if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
                char filho[2048];
                snprintf(filho, sizeof(filho), "%s/%s", path, e->d_name);
                rm_rf(filho);
            }
            closedir(d);
        }
        rmdir(path);
    } else unlink(path);
}

typedef struct { int n; char nomes[64][256]; bool pasta[64]; char ids[64][CLOUD_ID_MAX]; } Lista;

static void lista_cb(const char *id, const char *name, bool is_folder, void *ud)
{
    Lista *l = (Lista *)ud;
    if (l->n < 64) {
        snprintf(l->nomes[l->n], sizeof(l->nomes[0]), "%s", name);
        snprintf(l->ids[l->n], sizeof(l->ids[0]), "%s", id);
        l->pasta[l->n] = is_folder;
    }
    l->n++;
}

static int tem_na_lista(const Lista *l, const char *nome)
{
    for (int i = 0; i < l->n && i < 64; i++)
        if (strcmp(l->nomes[i], nome) == 0) return 1;
    return 0;
}

static int quantos_com_nome(const Lista *l, const char *nome)
{
    int c = 0;
    for (int i = 0; i < l->n && i < 64; i++)
        if (strcmp(l->nomes[i], nome) == 0) c++;
    return c;
}

// O login por código, pra quando não houver token guardado. Você digita, aqui
// só imprime. É a mesma função que roda no console.
static void status_cb(const char *msg) { printf("     %s\n", msg); fflush(stdout); }

static void device_cb(const char *url, const char *code, const char *url_com_code)
{
    printf("\n");
    printf("  ==========================================================\n");
    printf("   ABRE ISTO NO NAVEGADOR:  %s\n", url);
    printf("   E DIGITA ESTE CODIGO:    %s\n", code);
    printf("\n");
    printf("   (ou vai direto, ja com o codigo preenchido:)\n");
    printf("   %s\n", url_com_code);
    printf("  ==========================================================\n\n");
    fflush(stdout);
}

// --------------------------------------------------------------------- main

int main(void)
{
    printf("\n== entrar na conta ==\n");

    if (!http_init()) { printf("http_init falhou\n"); return 1; }

    if (oauth_load_saved_login()) {
        printf("  ..   achei um login guardado, nao vou pedir codigo\n");
    } else {
        printf("  ..   nao tem login guardado — vai precisar do codigo\n");
        oauth_set_device_cb(device_cb);
        if (!oauth_start_device_flow(status_cb, NULL)) {
            printf("\n  !! o login nao completou. Sem isso nao da pra testar o Drive.\n");
            return 1;
        }
    }

    ok(oauth_is_logged_in(), "tem login");

    // A prova real de que o token presta: trocar por um access_token novo.
    // O valor NAO e impresso.
    {
        char tok[CLOUD_AUTH_MAX];
        bool deu = oauth_get_fresh_access_token(tok, sizeof(tok));
        ok(deu, "o Google aceitou o login e devolveu acesso");
        ok(deu && strlen(tok) > 20, "e veio um acesso de verdade, nao string vazia");
        if (!deu) {
            printf("\n  !! o login guardado nao vale mais (expirou ou foi revogado).\n");
            printf("     Apaga o token e roda de novo pra fazer o login por codigo.\n");
            return 1;
        }
        memset(tok, 0, sizeof(tok));
    }

    cloud_set_current(CLOUD_DRIVE);
    ok(cloud_current() == CLOUD_DRIVE, "a nuvem escolhida e o Drive");
    ok(cloud_is_ready(CLOUD_DRIVE), "o Drive se declara pronto");

    char auth[CLOUD_AUTH_MAX];
    ok(cloud_begin(auth, sizeof(auth)), "abriu a sessao");

    printf("\n== a pasta do app ==\n");

    char raiz[CLOUD_ID_MAX];
    ok(cloud_ensure_app_folder(auth, raiz, sizeof(raiz)), "achou/criou a pasta \"" DRIVE_APP_FOLDER_NAME "\"");

    // Chamar de novo tem que devolver a MESMA pasta. Se criasse outra, o
    // console passaria a ver duas pastas com o mesmo nome e os backups
    // ficariam espalhados entre as duas.
    char raiz2[CLOUD_ID_MAX];
    ok(cloud_ensure_app_folder(auth, raiz2, sizeof(raiz2)), "chamar de novo funciona");
    ok(strcmp(raiz, raiz2) == 0, "e devolve a MESMA pasta, sem duplicar a raiz");

    printf("\n== o que ja esta no Drive (so olhando) ==\n");

    {
        Lista l = {0};
        ok(cloud_list_children(auth, raiz, lista_cb, &l), "listou a pasta do app");
        printf("     %d item(ns) la dentro:\n", l.n);
        for (int i = 0; i < l.n && i < 64; i++)
            printf("       %s %s\n", l.pasta[i] ? "[pasta]" : "       ", l.nomes[i]);
        ok(quantos_com_nome(&l, PASTA_TESTE) == 0,
            "nao tem sobra de teste anterior la dentro");
    }

    // ------------------------------------------------------------------
    // Daqui pra baixo, TUDO acontece dentro da pasta de teste. Nenhuma
    // operacao destrutiva ve a raiz.
    // ------------------------------------------------------------------

    printf("\n== a partir daqui, so dentro de \"%s\" ==\n", PASTA_TESTE);

    char teste[CLOUD_ID_MAX];
    ok(cloud_ensure_subfolder(auth, raiz, PASTA_TESTE, teste, sizeof(teste)),
        "criou a pasta de teste");
    ok(strcmp(teste, raiz) != 0, "e ela nao e a raiz (senao o prune pegaria tudo)");

    rm_rf("local");
    mkdir("local", 0777);

    printf("\n== subir e baixar um arquivo ==\n");

    {
        escrever("local/save.dat", "o save do jogador, versao 1");
        ok(cloud_upload(auth, teste, "save.dat", "local/save.dat", "application/octet-stream"),
            "subiu o arquivo");

        unlink("local/voltou.dat");
        ok(cloud_download(auth, teste, "save.dat", "local/voltou.dat"), "baixou de volta");
        ok(conteudo_igual("local/voltou.dat", "o save do jogador, versao 1"),
            "e o conteudo voltou igual");

        // Subir por cima nao pode criar um segundo arquivo com o mesmo nome —
        // no Drive isso e permitido, e seria o jeito mais facil de acabar com
        // dois saves e o app pegando o errado.
        escrever("local/save.dat", "o save do jogador, versao 2 (mais nova)");
        ok(cloud_upload(auth, teste, "save.dat", "local/save.dat", "application/octet-stream"),
            "subiu por cima do mesmo nome");

        unlink("local/voltou.dat");
        ok(cloud_download(auth, teste, "save.dat", "local/voltou.dat"), "baixou de novo");
        ok(conteudo_igual("local/voltou.dat", "o save do jogador, versao 2 (mais nova)"),
            "veio a versao nova, e nao a antiga");

        Lista l = {0};
        cloud_list_children(auth, teste, lista_cb, &l);
        ok(quantos_com_nome(&l, "save.dat") == 1,
            "e existe UM save.dat la, nao dois (o Drive deixa duplicar nome)");
    }

    printf("\n== arquivo que nao existe ==\n");

    {
        unlink("local/fantasma.dat");
        ok(!cloud_download(auth, teste, "nao-existe.dat", "local/fantasma.dat"),
            "baixar o que nao existe falha, em vez de criar arquivo vazio");
        ok(!existe("local/fantasma.dat"), "e nao deixa arquivo pela metade no cartao");

        char nada[CLOUD_ID_MAX];
        ok(!cloud_find_subfolder(auth, teste, "pasta que nao existe", nada, sizeof(nada)),
            "o find nao inventa pasta (e o que protege o restore)");
    }

    printf("\n== a arvore inteira ==\n");

    char arvore[CLOUD_ID_MAX];
    ok(cloud_ensure_subfolder(auth, teste, "Jogo de Teste (Jogador)", arvore, sizeof(arvore)),
        "criou pasta com parentese e espaco no nome");

    {
        rm_rf("local/save");
        mkdir("local/save", 0777);
        mkdir("local/save/meio", 0777);
        mkdir("local/save/meio/fundo", 0777);
        escrever("local/save/raiz.txt", "arquivo da raiz");
        escrever("local/save/meio/no-meio.txt", "arquivo do meio");
        escrever("local/save/meio/fundo/la-embaixo.txt", "arquivo do fundo");
        escrever("local/save/Coração.dat", "nome com acento");

        ok(cloud_upload_tree(auth, arvore, "local/save"), "subiu a arvore de 3 niveis");

        rm_rf("local/voltou");
        mkdir("local/voltou", 0777);
        ok(cloud_download_tree(auth, arvore, "local/voltou"), "baixou a arvore de volta");
        ok(conteudo_igual("local/voltou/raiz.txt", "arquivo da raiz"), "o arquivo da raiz voltou");
        ok(conteudo_igual("local/voltou/meio/no-meio.txt", "arquivo do meio"), "o do meio voltou");
        ok(conteudo_igual("local/voltou/meio/fundo/la-embaixo.txt", "arquivo do fundo"),
            "o do fundo voltou");
        ok(conteudo_igual("local/voltou/Coração.dat", "nome com acento"),
            "e o de nome com acento tambem (o Drive recebe UTF-8 direito)");
    }

    printf("\n== a limpeza (a unica que apaga da nuvem) ==\n");

    {
        // Apaga um arquivo e uma pasta do lado do console, roda a limpeza, e
        // confere que ela apagou EXATAMENTE isso — nem de menos, nem demais.
        unlink("local/save/meio/fundo/la-embaixo.txt");
        rmdir("local/save/meio/fundo");
        unlink("local/save/Coração.dat");

        ok(cloud_prune_extras(auth, arvore, "local/save"), "rodou a limpeza");

        rm_rf("local/depois");
        mkdir("local/depois", 0777);
        ok(cloud_download_tree(auth, arvore, "local/depois"), "baixou de novo depois da limpeza");

        ok(conteudo_igual("local/depois/raiz.txt", "arquivo da raiz"),
            "o arquivo que continua no console NAO foi apagado da nuvem");
        ok(conteudo_igual("local/depois/meio/no-meio.txt", "arquivo do meio"),
            "a pasta que ainda existe no console continua la");
        ok(!existe("local/depois/Coração.dat"),
            "o arquivo apagado no console sumiu da nuvem");
        ok(!existe("local/depois/meio/fundo"),
            "a pasta apagada no console sumiu da nuvem");
    }

    printf("\n== faxina: tirar a pasta de teste do Drive ==\n");

    {
        // Achar a pasta de teste pelo nome e apagar. No Drive isso vai pra
        // lixeira, entao da pra recuperar por 30 dias se algo der errado.
        char achou[CLOUD_ID_MAX];
        ok(cloud_find_subfolder(auth, raiz, PASTA_TESTE, achou, sizeof(achou)),
            "achou a pasta de teste pra apagar");

        Lista antes = {0};
        cloud_list_children(auth, raiz, lista_cb, &antes);
        int tinha = quantos_com_nome(&antes, PASTA_TESTE);

        // Apagar a pasta apaga tudo que esta dentro dela. O cloud.h nao expoe
        // "apagar isto aqui" de proposito — quem apaga e o prune —, entao aqui
        // a faxina chama a primitiva do backend direto.
        ok(drive_backend()->remove(auth, achou), "apagou a pasta de teste");

        Lista depois = {0};
        ok(cloud_list_children(auth, raiz, lista_cb, &depois), "listou a raiz de novo");
        ok(quantos_com_nome(&depois, PASTA_TESTE) == 0,
            "a pasta de teste sumiu do Drive");
        ok(tinha == 1, "e so tinha uma pra apagar");

        printf("     sobrou na pasta do app: %d item(ns)\n", depois.n);
        for (int i = 0; i < depois.n && i < 64; i++)
            printf("       %s %s\n", depois.pasta[i] ? "[pasta]" : "       ", depois.nomes[i]);

        ok(!tem_na_lista(&depois, PASTA_TESTE), "confirmado: nao sobrou lixo de teste");
    }

    rm_rf("local");

    printf("\n=========================================\n");
    printf("  %d testes, %d falha(s)\n", testes, falhas);
    printf("=========================================\n\n");
    return falhas != 0;
}
