// Teste do formato .ssaves rodando no Mac, sem console no meio.
//
// É o pedaço do app onde bug significa save perdido: se o pacote sair torto, o
// erro só aparece na hora de restaurar, que é a pior hora possível. Aqui a
// gente escreve, lê de volta e compara byte a byte.

#include "sssbox.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int falhas = 0;
static int testes  = 0;

static void ok(int cond, const char *msg)
{
    testes++;
    if (cond) {
        printf("  ok   %s\n", msg);
    } else {
        printf("  FALHA %s\n", msg);
        falhas++;
    }
}

// ---------------------------------------------------------------- utilidades

static void escrever(const char *path, const void *dados, size_t n)
{
    FILE *f = fopen(path, "wb");
    if (!f) { printf("  !! nao consegui criar %s\n", path); exit(1); }
    if (n) fwrite(dados, 1, n, f);
    fclose(f);
}

static long tamanho(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) return -1;
    return (long)st.st_size;
}

static int mesmo_arquivo(const char *a, const char *b)
{
    FILE *fa = fopen(a, "rb"), *fb = fopen(b, "rb");
    if (!fa || !fb) { if (fa) fclose(fa); if (fb) fclose(fb); return 0; }

    int igual = 1;
    for (;;) {
        char ba[4096], bb[4096];
        size_t na = fread(ba, 1, sizeof(ba), fa);
        size_t nb = fread(bb, 1, sizeof(bb), fb);
        if (na != nb || memcmp(ba, bb, na) != 0) { igual = 0; break; }
        if (na == 0) break;
    }
    fclose(fa); fclose(fb);
    return igual;
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
    } else {
        unlink(path);
    }
}

// ------------------------------------------------------------ arvore de teste

// Casos escolhidos por serem os que quebram na prática: arquivo vazio (o save
// de jogo tem vários), arquivo grande (passa do buffer de compressão),
// incompressível (deflate cresce em vez de encolher), nome com acento e com
// espaço, e pasta dentro de pasta.
static void montar_arvore(const char *raiz)
{
    char p[2048];
    mkdir(raiz, 0777);

    snprintf(p, sizeof(p), "%s/vazio.bin", raiz);
    escrever(p, "", 0);

    snprintf(p, sizeof(p), "%s/um-byte.bin", raiz);
    escrever(p, "A", 1);

    // Compressível: 300 KB de zeros.
    {
        size_t n = 300 * 1024;
        char *z = calloc(1, n);
        snprintf(p, sizeof(p), "%s/zeros.bin", raiz);
        escrever(p, z, n);
        free(z);
    }

    // Incompressível: 2 MB pseudoaleatórios, mas com semente fixa pra o teste
    // dar sempre o mesmo resultado.
    {
        size_t n = 2 * 1024 * 1024;
        unsigned char *r = malloc(n);
        unsigned int s = 12345;
        for (size_t i = 0; i < n; i++) { s = s * 1103515245u + 12345u; r[i] = (unsigned char)(s >> 16); }
        snprintf(p, sizeof(p), "%s/ruido.bin", raiz);
        escrever(p, r, n);
        free(r);
    }

    // Todos os 256 valores de byte, inclusive o 0 no meio.
    {
        unsigned char todos[256];
        for (int i = 0; i < 256; i++) todos[i] = (unsigned char)i;
        snprintf(p, sizeof(p), "%s/Coração do save.dat", raiz);
        escrever(p, todos, sizeof(todos));
    }

    snprintf(p, sizeof(p), "%s/sub", raiz);
    mkdir(p, 0777);
    snprintf(p, sizeof(p), "%s/sub/fundo", raiz);
    mkdir(p, 0777);
    snprintf(p, sizeof(p), "%s/sub/fundo/la-embaixo.txt", raiz);
    escrever(p, "tres niveis", 11);
}

// Compara duas árvores inteiras. Devolve o número de arquivos conferidos, ou -1.
static int comparar_arvore(const char *a, const char *b)
{
    DIR *d = opendir(a);
    if (!d) return -1;

    int n = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        if (!strcmp(e->d_name, ".DS_Store")) continue;

        char pa[2048], pb[2048];
        snprintf(pa, sizeof(pa), "%s/%s", a, e->d_name);
        snprintf(pb, sizeof(pb), "%s/%s", b, e->d_name);

        struct stat st;
        if (stat(pa, &st) != 0) { closedir(d); return -1; }

        if (S_ISDIR(st.st_mode)) {
            int sub = comparar_arvore(pa, pb);
            if (sub < 0) { printf("  !! pasta diferente: %s\n", pa); closedir(d); return -1; }
            n += sub;
        } else {
            if (!mesmo_arquivo(pa, pb)) {
                printf("  !! conteudo diferente: %s (%ld) vs %s (%ld)\n",
                    pa, tamanho(pa), pb, tamanho(pb));
                closedir(d);
                return -1;
            }
            n++;
        }
    }
    closedir(d);
    return n;
}

// ----------------------------------------------------------------- callbacks

typedef struct { int n; unsigned long long total; char primeiro[256]; } Conta;

static void conta_cb(const char *name, uint64_t size, void *ud)
{
    Conta *c = (Conta *)ud;
    if (c->n == 0) snprintf(c->primeiro, sizeof(c->primeiro), "%s", name);
    c->n++;
    c->total += size;
}

// ---------------------------------------------------------------------- main

int main(void)
{
    // Pasta de trabalho descartavel, ao lado do binario. O run.sh apaga depois.
    const char *tmp = "tmp";
    rm_rf(tmp);
    mkdir(tmp, 0777);

    char origem[2048], caixa[2048], destino[2048];
    snprintf(origem,  sizeof(origem),  "%s/origem", tmp);
    snprintf(caixa,   sizeof(caixa),   "%s/tudo.ssaves", tmp);
    snprintf(destino, sizeof(destino), "%s/destino", tmp);

    montar_arvore(origem);

    printf("\n== escrever ==\n");

    SssBoxWriter *w = sssbox_open_write(caixa);
    ok(w != NULL, "abriu o arquivo pra escrita");
    if (!w) return 1;

    ok(sssbox_add_dir(w, "Jogo A/", origem), "guardou a arvore como \"Jogo A/\"");
    ok(sssbox_add_dir(w, "Jogo B (Player 1)/", origem), "guardou de novo como \"Jogo B (Player 1)/\"");

    size_t entradas = sssbox_entry_count(w);
    unsigned long long escritos = sssbox_bytes_written(w);
    ok(entradas == 12, "contou 12 entradas (6 arquivos x 2 pastas)");
    ok(escritos > 0, "contabilizou bytes escritos");

    ok(sssbox_close_write(w), "fechou o arquivo");
    ok(tamanho(caixa) > 0, "o arquivo existe e nao esta vazio");

    printf("     %ld bytes no arquivo, %llu bytes de save la dentro\n",
        tamanho(caixa), escritos);

    printf("\n== e nosso, e nao e zip ==\n");

    ok(sssbox_is_box(caixa), "sssbox_is_box reconhece o proprio arquivo");

    {
        char naoehcaixa[2048];
        snprintf(naoehcaixa, sizeof(naoehcaixa), "%s/qualquer.bin", tmp);
        escrever(naoehcaixa, "PK\x03\x04 isto aqui e um zip de mentira", 34);
        ok(!sssbox_is_box(naoehcaixa), "sssbox_is_box recusa arquivo de fora");
    }

    {
        FILE *f = fopen(caixa, "rb");
        unsigned char cab[8] = {0};
        fread(cab, 1, sizeof(cab), f);
        fclose(f);
        ok(!(cab[0] == 'P' && cab[1] == 'K'), "nao comeca com PK (o Mac nao vai achar que e zip)");
        printf("     cabecalho: %02X %02X %02X %02X %02X %02X %02X %02X\n",
            cab[0], cab[1], cab[2], cab[3], cab[4], cab[5], cab[6], cab[7]);
    }

    // conversa privada removida do historico
    // Procuro no arquivo inteiro a sequencia de 256 bytes que esta dentro do
    // save. Se aparecer em claro, o embaralhamento nao esta valendo.
    {
        long n = tamanho(caixa);
        FILE *f = fopen(caixa, "rb");
        unsigned char *todo = malloc((size_t)n);
        fread(todo, 1, (size_t)n, f);
        fclose(f);

        unsigned char agulha[256];
        for (int i = 0; i < 256; i++) agulha[i] = (unsigned char)i;

        int achou = 0;
        for (long i = 0; i + 256 <= n && !achou; i++)
            if (memcmp(todo + i, agulha, 256) == 0) achou = 1;

        ok(!achou, "o conteudo do save nao aparece em claro dentro do arquivo");

        // E o nome do jogo tambem nao, senao quem abrisse no bloco de notas ja
        // sabia o que tem ali dentro.
        int nome_em_claro = 0;
        for (long i = 0; i + 6 <= n && !nome_em_claro; i++)
            if (memcmp(todo + i, "Jogo A", 6) == 0) nome_em_claro = 1;
        ok(!nome_em_claro, "o nome da pasta tambem nao aparece em claro");

        free(todo);
    }

    printf("\n== ler o indice ==\n");

    {
        Conta c = {0};
        ok(sssbox_list(caixa, conta_cb, &c), "sssbox_list leu o indice");
        ok(c.n == 12, "listou as 12 entradas");
        printf("     primeiro nome: %s\n", c.primeiro);

        // 0 + 1 + 300K + 2M + 256 + 11, vezes duas pastas
        unsigned long long esperado = (0ULL + 1 + 300 * 1024 + 2 * 1024 * 1024 + 256 + 11) * 2;
        ok(c.total == esperado, "a soma dos tamanhos bate com o que foi guardado");
        if (c.total != esperado)
            printf("     esperado %llu, veio %llu\n", esperado, c.total);
    }

    {
        Conta c = {0};
        ok(sssbox_list_folders(caixa, conta_cb, &c), "sssbox_list_folders leu o indice");
        ok(c.n == 2, "achou as 2 pastas de primeiro nivel");
    }

    printf("\n== tirar de volta ==\n");

    mkdir(destino, 0777);
    ok(sssbox_extract(caixa, "Jogo A/", destino), "extraiu so o \"Jogo A/\"");

    {
        int n = comparar_arvore(origem, destino);
        ok(n == 6, "os 6 arquivos voltaram identicos, byte a byte");
        if (n != 6) printf("     conferidos: %d\n", n);
    }

    {
        char tudo[2048];
        snprintf(tudo, sizeof(tudo), "%s/tudo", tmp);
        mkdir(tudo, 0777);
        ok(sssbox_extract(caixa, "", tudo), "extraiu tudo com prefixo vazio");

        char a[2048], b[2048];
        snprintf(a, sizeof(a), "%s/Jogo A", tudo);
        snprintf(b, sizeof(b), "%s/Jogo B (Player 1)", tudo);
        ok(comparar_arvore(origem, a) == 6, "a pasta \"Jogo A\" saiu inteira");
        ok(comparar_arvore(origem, b) == 6, "a pasta \"Jogo B (Player 1)\" saiu inteira");
    }

    {
        char vazio[2048];
        snprintf(vazio, sizeof(vazio), "%s/naoexiste", tmp);
        mkdir(vazio, 0777);
        // Prefixo que nao casa com nada: nao pode explodir nem inventar arquivo.
        sssbox_extract(caixa, "Jogo Z/", vazio);
        DIR *d = opendir(vazio);
        int n = 0;
        struct dirent *e;
        while (d && (e = readdir(d))) if (e->d_name[0] != '.') n++;
        if (d) closedir(d);
        ok(n == 0, "prefixo que nao existe nao cria nada");
    }

    printf("\n== desistir no meio ==\n");

    {
        char meio[2048];
        snprintf(meio, sizeof(meio), "%s/abortado.ssaves", tmp);

        SssBoxWriter *w2 = sssbox_open_write(meio);
        ok(w2 != NULL, "abriu um segundo arquivo");
        sssbox_add_dir(w2, "Jogo C/", origem);
        ok(tamanho(meio) > 0, "tinha bytes gravados antes de desistir");
        sssbox_abort_write(w2);
        ok(tamanho(meio) < 0, "o abort apagou o arquivo (meio backup nao fica no cartao)");
    }

    {
        // Fechar sem ter posto nada dentro. O codigo recusa de proposito: um
        // .ssaves com zero save dentro parece backup e nao e. Entao o close
        // tem que dizer que nao deu E apagar o arquivo.
        char zerado[2048];
        snprintf(zerado, sizeof(zerado), "%s/vazio.ssaves", tmp);

        SssBoxWriter *w3 = sssbox_open_write(zerado);
        ok(w3 != NULL, "abriu um arquivo pra deixar vazio");
        ok(!sssbox_close_write(w3), "recusa fechar sem nenhuma entrada dentro");
        ok(tamanho(zerado) < 0, "e apaga o arquivo vazio em vez de deixar no cartao");
    }

    printf("\n== arquivo estragado ==\n");

    {
        // Um .ssaves truncado no meio: tem que responder que nao deu, e nao
        // sair escrevendo lixo em cima de save.
        char torto[2048];
        snprintf(torto, sizeof(torto), "%s/torto.ssaves", tmp);

        long n = tamanho(caixa);
        FILE *fi = fopen(caixa, "rb");
        FILE *fo = fopen(torto, "wb");
        char buf[4096];
        long faltam = n / 2;
        while (faltam > 0) {
            size_t pedaco = fread(buf, 1, (size_t)(faltam < 4096 ? faltam : 4096), fi);
            if (!pedaco) break;
            fwrite(buf, 1, pedaco, fo);
            faltam -= (long)pedaco;
        }
        fclose(fi); fclose(fo);

        char dest2[2048];
        snprintf(dest2, sizeof(dest2), "%s/dotorto", tmp);
        mkdir(dest2, 0777);

        ok(!sssbox_list(torto, conta_cb, &(Conta){0}), "recusa listar arquivo cortado pela metade");
        ok(!sssbox_extract(torto, "", dest2), "recusa extrair arquivo cortado pela metade");
    }

    printf("\n=========================================\n");
    printf("  %d testes, %d falha(s)\n", testes, falhas);
    printf("=========================================\n\n");

    return falhas ? 1 : 0;
}
