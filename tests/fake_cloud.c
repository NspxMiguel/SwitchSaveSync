// Uma nuvem de mentira e um savedata de mentira, os dois em cima de pastas
// de verdade no disco do Mac.
//
// Por que isto existe: o syncjob_sync_title decide SOZINHO se sobe, se baixa,
// se não faz nada ou se recusa por conflito — e errar essa decisão apaga
// progresso de jogo. Até hoje essa decisão só era exercitada no console, com
// o save real, o que é o pior lugar possível pra descobrir um erro.
//
// Nada aqui simula rede, latência ou erro do Google. O que se testa é a
// DECISÃO, e ela não depende disso.
//
// Os "id" da nuvem falsa são o próprio caminho no disco. Fica trivial de
// inspecionar: o teste pode abrir a pasta e conferir o que subiu.

#include "cloud_backend.h"
#include "savemount.h"
#include "titles.h"

#include <dirent.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define RAIZ_NUVEM "nuvem-falsa"
#define RAIZ_SAVE  "save-falso"

// ---------------------------------------------------------------- utilidades

static void junta(char *out, size_t n, const char *a, const char *b)
{
    snprintf(out, n, "%s/%s", a, b);
}

static bool eh_dir(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool eh_arquivo(const char *p)
{
    struct stat st;
    return stat(p, &st) == 0 && S_ISREG(st.st_mode);
}

static bool copia_arquivo(const char *de, const char *para)
{
    FILE *a = fopen(de, "rb");
    if (!a) return false;

    FILE *b = fopen(para, "wb");
    if (!b) { fclose(a); return false; }

    char buf[4096];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), a)) > 0)
        if (fwrite(buf, 1, n, b) != n) { ok = false; break; }

    fclose(a);
    return fclose(b) == 0 && ok;
}

static void apaga_recursivo(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d) { unlink(dir); return; }

    struct dirent *e;
    while ((e = readdir(d)))
    {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char filho[1024];
        junta(filho, sizeof(filho), dir, e->d_name);
        if (eh_dir(filho)) apaga_recursivo(filho); else unlink(filho);
    }
    closedir(d);
    rmdir(dir);
}

// ------------------------------------------------------------ a nuvem falsa

// Contadores que o teste lê pra provar o que aconteceu — "subiu" e "baixou"
// não são a mesma coisa que "o resultado deu UPLOADED".
int fake_puts = 0, fake_gets = 0, fake_removes = 0;

// O teste liga isto pra simular o que acontece de verdade: o Drive recusando o
// DELETE por rate limit, o WebDAV devolvendo 423, ou o usuário apertando B no
// meio.
int fake_remove_falha = 0;

void fake_cloud_reset(void)
{
    apaga_recursivo(RAIZ_NUVEM);
    fake_puts = fake_gets = fake_removes = 0;
    fake_remove_falha = 0;
}

static const char *f_name(void)  { return "Nuvem de teste"; }
static const char *f_hint(void)  { return ""; }
static bool        f_ready(void) { return true; }
static void        f_logout(void){ }

static bool f_begin(char *a, size_t n) { snprintf(a, n, "token-de-mentira"); return true; }

static bool f_root(const char *a, char *o, size_t n)
{
    (void)a;
    mkdir(RAIZ_NUVEM, 0777);
    snprintf(o, n, "%s", RAIZ_NUVEM);
    return true;
}

static bool f_find(const char *a, const char *pai, const char *nome, bool quer_pasta,
                    char *o, size_t n)
{
    (void)a;
    char p[1024];
    junta(p, sizeof(p), pai, nome);

    // want_folder separa pasta de arquivo, igual ao contrato do cloud_backend.h
    // — sem isso o .nxsaves do jogo passaria por pasta de conta.
    if (quer_pasta ? !eh_dir(p) : !eh_arquivo(p)) return false;

    snprintf(o, n, "%s", p);
    return true;
}

static bool f_mk(const char *a, const char *pai, const char *nome, char *o, size_t n)
{
    (void)a;
    char p[1024];
    junta(p, sizeof(p), pai, nome);
    if (!eh_dir(p) && mkdir(p, 0777) != 0 && !eh_dir(p)) return false;
    snprintf(o, n, "%s", p);
    return true;
}

static bool f_put(const char *a, const char *pai, const char *nome,
                   const char *local, const char *mime)
{
    (void)a; (void)mime;
    char p[1024];
    junta(p, sizeof(p), pai, nome);
    fake_puts++;
    return copia_arquivo(local, p);
}

static bool f_get(const char *a, const char *id, const char *local)
{
    (void)a;
    fake_gets++;
    return copia_arquivo(id, local);
}

static bool f_rm(const char *a, const char *id)
{
    (void)a;
    fake_removes++;
    if (fake_remove_falha) return false;
    apaga_recursivo(id);
    return true;
}

static bool f_list(const char *a, const char *pasta, cloud_list_cb cb, void *ud)
{
    (void)a;
    DIR *d = opendir(pasta);
    if (!d) return false;

    struct dirent *e;
    while ((e = readdir(d)))
    {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char filho[1024];
        junta(filho, sizeof(filho), pasta, e->d_name);
        if (cb) cb(filho, e->d_name, eh_dir(filho), ud);
    }
    closedir(d);
    return true;
}

const CloudBackend *drive_backend(void)
{
    static const CloudBackend b = {
        .key = "drive", .name = f_name, .is_ready = f_ready, .setup_hint = f_hint,
        .logout = f_logout, .begin = f_begin, .root = f_root, .find_child = f_find,
        .make_folder = f_mk, .put_file = f_put, .get_file = f_get, .remove = f_rm,
        .list = f_list,
    };
    return &b;
}

// ------------------------------------------------------------ o save falso
//
// O savedata do jogo vira a pasta save-falso/. O "commit" é o detalhe que
// importa: montar sem commitar tem que deixar o save como estava, e é essa a
// regra que já custou save de jogo de verdade.

static bool g_montado    = false;
static bool g_escrita    = false;
int  fake_commits        = 0;
int  fake_mount_falha    = 0; // o teste liga isto pra simular jogo aberto

// "save:" existe SÓ enquanto está montado.
//
// Nem todo caminho passa pelo savemount_copy_tree: o arquivo único lê e
// escreve com o nxsaves direto em "save:/". Aqui isso vira um atalho pra
// save-falso — e ele só existe entre o mount e o unmount, de propósito. Assim
// mexer no savedata sem montar não "quase funciona": não acha pasta nenhuma,
// que é o que a regra do console diz (savemount.h).
#define ATALHO "save:"

void fake_save_reset(void)
{
    unlink(ATALHO);
    apaga_recursivo(RAIZ_SAVE);
    mkdir(RAIZ_SAVE, 0777);
    g_montado = g_escrita = false;
    fake_commits = 0;
    fake_mount_falha = 0;
}

bool savemount_mount_typed(u64 app, AccountUid uid, bool device, bool read_only)
{
    (void)app; (void)uid; (void)device;
    if (fake_mount_falha) return false;
    if (g_montado) return false; // montar duas vezes é erro, e o teste pega
    g_montado = true;
    g_escrita = !read_only;
    symlink(RAIZ_SAVE, ATALHO);
    return true;
}

bool savemount_unmount(bool commit)
{
    // Commitar um mount read-only é o erro que corrompe save. Se acontecer,
    // o teste tem que ver.
    if (commit && g_escrita) fake_commits++;
    if (commit && !g_escrita) fake_commits = -1000; // sabotagem visível
    g_montado = false;
    g_escrita = false;
    unlink(ATALHO);
    return true; // aqui o commit nunca falha; quem testa a falha e o test_savemount
}

bool savemount_wipe_contents(void)
{
    if (!g_montado || !g_escrita) return false;
    apaga_recursivo(RAIZ_SAVE);
    mkdir(RAIZ_SAVE, 0777);
    return true;
}

// "save:/" vira RAIZ_SAVE; qualquer outro caminho é usado como está.
static const char *traduz(const char *p)
{
    return strncmp(p, "save:/", 6) == 0 ? RAIZ_SAVE : p;
}

static bool copia_arvore(const char *de, const char *para)
{
    DIR *d = opendir(de);
    if (!d) return false;

    mkdir(para, 0777);

    struct dirent *e;
    bool ok = true;
    while ((e = readdir(d)))
    {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char a[1024], b[1024];
        junta(a, sizeof(a), de, e->d_name);
        junta(b, sizeof(b), para, e->d_name);
        ok = (eh_dir(a) ? copia_arvore(a, b) : copia_arquivo(a, b)) && ok;
    }
    closedir(d);
    return ok;
}

bool savemount_copy_tree(const char *src, const char *dst)
{
    if (!g_montado) return false;
    return copia_arvore(traduz(src), traduz(dst));
}

size_t titles_list_with_savedata(TitleEntry *o, size_t m) { (void)o; (void)m; return 0; }

bool titles_current_account_name(char *out, size_t outsz)
{
    snprintf(out, outsz, "Jogador");
    return true;
}
