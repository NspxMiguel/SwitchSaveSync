// Teste do backend WebDAV contra um servidor WebDAV de verdade (Apache com
// mod_dav rodando em 127.0.0.1:8088).
//
// É o código mais novo do projeto e o primeiro a ser testado. Aqui a
// gente exercita as sete primitivas do CloudBackend e, principalmente, o
// espelhamento inteiro do cloud.c: subir árvore, baixar árvore e apagar da
// nuvem o que não existe mais no console — que é a operação que pode destruir
// backup se estiver errada.

#include "webdav.h"
#include "cloud.h"
#include "cloud_backend.h"
#include "http.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// ----------------------------------------------------------------- o que falta

// O core chama isto; no console cria a pasta no cartão. Aqui o caminho
// "sdmc:/switch/..." não tem barra na frente, então vira pasta relativa e o
// teste roda dentro da própria pasta de trabalho.
void syncstate_ensure_dirs(void)
{
    mkdir("sdmc:", 0777);
    mkdir("sdmc:/switch", 0777);
    mkdir("sdmc:/switch/SwitchSaveSync", 0777);
}

bool lang_is_pt(void) { return true; }

// O cloud.c conhece as duas nuvens. Aqui só o WebDAV interessa, mas o linker
// quer as duas.
static const char *nada_name(void)       { return "Google Drive (fora do teste)"; }
static const char *nada_hint(void)       { return ""; }
static bool        nada_ready(void)      { return false; }
static void        nada_logout(void)     { }
static bool        nada_begin(char *a, size_t n) { (void)a; (void)n; return false; }
static bool nada_root(const char *a, char *o, size_t n) { (void)a; (void)o; (void)n; return false; }
static bool nada_find(const char *a, const char *p, const char *nm, bool f, char *o, size_t n)
{ (void)a; (void)p; (void)nm; (void)f; (void)o; (void)n; return false; }
static bool nada_mk(const char *a, const char *p, const char *nm, char *o, size_t n)
{ (void)a; (void)p; (void)nm; (void)o; (void)n; return false; }
static bool nada_put(const char *a, const char *p, const char *nm, const char *l, const char *m)
{ (void)a; (void)p; (void)nm; (void)l; (void)m; return false; }
static bool nada_get(const char *a, const char *i, const char *l) { (void)a; (void)i; (void)l; return false; }
static bool nada_rm(const char *a, const char *i) { (void)a; (void)i; return false; }
static bool nada_list(const char *a, const char *f, cloud_list_cb cb, void *u)
{ (void)a; (void)f; (void)cb; (void)u; return false; }

const CloudBackend *drive_backend(void)
{
    static const CloudBackend b = {
        .key = "drive", .name = nada_name, .is_ready = nada_ready,
        .setup_hint = nada_hint, .logout = nada_logout, .begin = nada_begin,
        .root = nada_root, .find_child = nada_find, .make_folder = nada_mk,
        .put_file = nada_put, .get_file = nada_get, .remove = nada_rm, .list = nada_list,
    };
    return &b;
}

// ------------------------------------------------------------------ placar

static int falhas = 0, testes = 0;

static void ok(int cond, const char *msg)
{
    testes++;
    printf(cond ? "  ok   %s\n" : "  FALHA %s\n", msg);
    if (!cond) falhas++;
}

// ----------------------------------------------------------------- utilidades

static void escrever(const char *path, const char *txt)
{
    FILE *f = fopen(path, "wb");
    if (!f) { printf("  !! nao criei %s\n", path); exit(1); }
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

typedef struct { int n; char nomes[32][256]; bool pasta[32]; } Lista;

static void lista_cb(const char *id, const char *name, bool is_folder, void *ud)
{
    (void)id;
    Lista *l = (Lista *)ud;
    if (l->n < 32) {
        snprintf(l->nomes[l->n], sizeof(l->nomes[0]), "%s", name);
        l->pasta[l->n] = is_folder;
    }
    l->n++;
}

static bool tem_na_lista(const Lista *l, const char *nome)
{
    for (int i = 0; i < l->n && i < 32; i++)
        if (strcmp(l->nomes[i], nome) == 0) return true;
    return false;
}

// --------------------------------------------------------------------- main

int main(void)
{
    // Pasta de trabalho descartavel, ao lado do binario. O run.sh apaga depois.
    const char *base = "tmpdav";
    rm_rf(base);
    mkdir(base, 0777);
    if (chdir(base) != 0) { printf("chdir falhou\n"); return 1; }

    if (!http_init()) { printf("http_init falhou\n"); return 1; }

    printf("\n== os dados de acesso ==\n");

    {
        WebdavConfig c;
        webdav_clear_config();
        ok(!webdav_get_config(&c), "sem nada gravado, o get diz que nao tem");
        ok(c.url[0] == '\0' && c.user[0] == '\0' && c.pass[0] == '\0',
            "e devolve o struct zerado, e nao lixo da pilha");
    }

    {
        // A barra no fim tem que sumir: o codigo monta url + "/" + nome, e com
        // as duas sairia "//", que servidor nenhum trata igual.
        WebdavConfig c = {0};
        snprintf(c.url,  sizeof(c.url),  "http://127.0.0.1:8088/");
        snprintf(c.user, sizeof(c.user), "jogador");
        snprintf(c.pass, sizeof(c.pass), "senha de teste 123");
        webdav_set_config(&c);

        WebdavConfig d;
        ok(webdav_get_config(&d), "depois de gravar, o get diz que tem");
        ok(strcmp(d.url, "http://127.0.0.1:8088") == 0, "tirou a barra do fim do endereco");
        ok(strcmp(d.user, "jogador") == 0, "guardou o usuario");
        ok(strcmp(d.pass, "senha de teste 123") == 0, "guardou a senha com espaco dentro");
    }

    printf("\n== conectar ==\n");

    {
        char erro[256] = {0};
        ok(webdav_test_connection(erro, sizeof(erro)), "o teste de conexao passou");
        if (erro[0]) printf("     erro: %s\n", erro);
    }

    {
        // Senha errada tem que dar 401 e uma frase legivel, nao um numero solto.
        WebdavConfig c;
        webdav_get_config(&c);
        char boa[WEBDAV_PASS_MAX];
        snprintf(boa, sizeof(boa), "%s", c.pass);

        snprintf(c.pass, sizeof(c.pass), "essa nao e a senha");
        webdav_set_config(&c);

        char erro[256] = {0};
        ok(!webdav_test_connection(erro, sizeof(erro)), "com a senha errada, o teste falha");
        ok(erro[0] != '\0', "e diz o motivo em vez de calar");
        printf("     mensagem: %s\n", erro);

        snprintf(c.pass, sizeof(c.pass), "%s", boa);
        webdav_set_config(&c);
    }

    {
        WebdavConfig c;
        webdav_get_config(&c);
        char bom[WEBDAV_URL_MAX];
        snprintf(bom, sizeof(bom), "%s", c.url);

        snprintf(c.url, sizeof(c.url), "http://127.0.0.1:9999");
        webdav_set_config(&c);

        char erro[256] = {0};
        ok(!webdav_test_connection(erro, sizeof(erro)), "com o servidor fora do ar, o teste falha");
        printf("     mensagem: %s\n", erro);

        snprintf(c.url, sizeof(c.url), "%s", bom);
        webdav_set_config(&c);
    }

    printf("\n== a nuvem escolhida ==\n");

    cloud_set_current(CLOUD_WEBDAV);
    ok(cloud_current() == CLOUD_WEBDAV, "gravou a escolha da nuvem");
    ok(cloud_is_ready(CLOUD_WEBDAV), "o WebDAV se declara pronto");
    ok(strcmp(cloud_setup_hint(CLOUD_WEBDAV), "") == 0, "e nao pede mais nada");
    printf("     nome na tela: \"%s\"\n", cloud_name(CLOUD_WEBDAV));

    char auth[CLOUD_AUTH_MAX];
    ok(cloud_begin(auth, sizeof(auth)), "abriu a sessao");

    printf("\n== as pastas ==\n");

    char raiz[CLOUD_ID_MAX];
    ok(cloud_ensure_app_folder(auth, raiz, sizeof(raiz)), "criou/achou a pasta do app");
    printf("     raiz: %s\n", raiz);

    char jogo[CLOUD_ID_MAX];
    ok(cloud_ensure_subfolder(auth, raiz, "Zelda (Jogador)", jogo, sizeof(jogo)),
        "criou a pasta do jogo com parentese e espaco no nome");

    {
        char denovo[CLOUD_ID_MAX];
        ok(cloud_ensure_subfolder(auth, raiz, "Zelda (Jogador)", denovo, sizeof(denovo)),
            "chamar de novo nao quebra");
        ok(strcmp(jogo, denovo) == 0, "e devolve a mesma pasta, sem duplicar");
    }

    {
        char achada[CLOUD_ID_MAX];
        ok(cloud_find_subfolder(auth, raiz, "Zelda (Jogador)", achada, sizeof(achada)),
            "acha a pasta pelo nome");
        ok(!cloud_find_subfolder(auth, raiz, "Jogo Que Nao Existe", achada, sizeof(achada)),
            "e nao inventa pasta que nao existe");
    }

    printf("\n== subir e baixar um arquivo ==\n");

    escrever("save.dat", "o save do zelda");
    ok(cloud_upload(auth, jogo, "save.dat", "save.dat", "application/octet-stream"),
        "subiu o arquivo");

    ok(cloud_download(auth, jogo, "save.dat", "devolta.dat"), "baixou o arquivo");
    ok(conteudo_igual("devolta.dat", "o save do zelda"), "e o conteudo voltou igual");

    escrever("save.dat", "o save do zelda, agora com mais horas");
    ok(cloud_upload(auth, jogo, "save.dat", "save.dat", "application/octet-stream"),
        "subiu por cima do mesmo nome");
    unlink("devolta.dat");
    ok(cloud_download(auth, jogo, "save.dat", "devolta.dat"), "baixou de novo");
    ok(conteudo_igual("devolta.dat", "o save do zelda, agora com mais horas"),
        "veio a versao nova, e nao a antiga");

    {
        Lista l = {0};
        ok(cloud_list_children(auth, jogo, lista_cb, &l), "listou a pasta do jogo");
        ok(l.n == 1, "com exatamente 1 item (nao duplicou no upload por cima)");
        ok(tem_na_lista(&l, "save.dat"), "e o item e o save.dat");
    }

    ok(!cloud_download(auth, jogo, "nao-existe.dat", "nada.dat"),
        "baixar arquivo inexistente falha em vez de criar arquivo vazio");
    ok(!existe("nada.dat"), "e nao deixa arquivo pela metade no cartao");

    printf("\n== subir a arvore inteira ==\n");

    mkdir("origem", 0777);
    escrever("origem/principal.sav", "arquivo raiz");
    mkdir("origem/sub", 0777);
    escrever("origem/sub/dentro.sav", "arquivo de dentro");
    mkdir("origem/sub/maisfundo", 0777);
    escrever("origem/sub/maisfundo/fundo.sav", "tres niveis");

    char arv[CLOUD_ID_MAX];
    ok(cloud_ensure_subfolder(auth, raiz, "Arvore", arv, sizeof(arv)), "criou a pasta da arvore");
    ok(cloud_upload_tree(auth, arv, "origem"), "subiu a arvore de 3 niveis");

    mkdir("baixada", 0777);
    ok(cloud_download_tree(auth, arv, "baixada"), "baixou a arvore de volta");
    ok(conteudo_igual("baixada/principal.sav", "arquivo raiz"), "o arquivo da raiz voltou");
    ok(conteudo_igual("baixada/sub/dentro.sav", "arquivo de dentro"), "o do meio voltou");
    ok(conteudo_igual("baixada/sub/maisfundo/fundo.sav", "tres niveis"), "o do fundo voltou");

    printf("\n== apagar o que sobrou na nuvem ==\n");

    // Esta e a operacao perigosa: ela APAGA da nuvem. Tem que apagar exatamente
    // o que nao existe mais no console, e nada alem disso.
    unlink("origem/sub/dentro.sav");
    rm_rf("origem/sub/maisfundo");

    ok(cloud_prune_extras(auth, arv, "origem"), "rodou a limpeza");

    rm_rf("baixada");
    mkdir("baixada", 0777);
    ok(cloud_download_tree(auth, arv, "baixada"), "baixou de novo depois da limpeza");
    ok(conteudo_igual("baixada/principal.sav", "arquivo raiz"),
        "o arquivo que continua no console NAO foi apagado da nuvem");
    ok(!existe("baixada/sub/dentro.sav"), "o arquivo apagado no console sumiu da nuvem");
    ok(!existe("baixada/sub/maisfundo"), "a pasta apagada no console sumiu da nuvem");
    ok(existe("baixada/sub"), "a pasta que ainda existe no console continua la");

    printf("\n== apagar de proposito ==\n");

    {
        char id[CLOUD_ID_MAX];
        const CloudBackend *b = webdav_backend();
        ok(b->find_child(auth, jogo, "save.dat", false, id, sizeof(id)), "achou o arquivo pra apagar");
        ok(b->remove(auth, id), "apagou");
        ok(!b->find_child(auth, jogo, "save.dat", false, id, sizeof(id)), "e ele sumiu mesmo");
    }

    printf("\n== sair ==\n");

    cloud_logout(CLOUD_WEBDAV);
    ok(!cloud_is_ready(CLOUD_WEBDAV), "depois do logout, nao esta mais pronto");
    {
        WebdavConfig c;
        ok(!webdav_get_config(&c), "e os dados de acesso sumiram do cartao");
    }

    printf("\n=========================================\n");
    printf("  %d testes, %d falha(s)\n", testes, falhas);
    printf("=========================================\n\n");

    return falhas ? 1 : 0;
}
