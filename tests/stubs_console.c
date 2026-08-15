// Os pedacos que so existem no console. O teste de nome de pasta nao encosta
// em nenhum deles — estao aqui so pra o linker parar de reclamar.
#include "savemount.h"
#include "titles.h"
#include "cloud_backend.h"

#include <stdio.h>

bool savemount_mount_typed(u64 a, AccountUid u, bool d, bool r) { (void)a;(void)u;(void)d;(void)r; return false; }
bool savemount_unmount(bool c) { (void)c; return true; }
bool savemount_wipe_contents(void) { return false; }
bool savemount_copy_tree(const char *s, const char *d) { (void)s;(void)d; return false; }
size_t titles_list_with_savedata(TitleEntry *o, size_t m) { (void)o;(void)m; return 0; }

// O apelido da conta em uso. O teste escreve aqui pra mandar no resultado: e
// justamente este valor que decide o nome da pasta quando o save nao diz de
// quem e. Vazio = o console nao soube dizer.
char stub_conta_atual[0x21] = "";

bool titles_current_account_name(char *out, size_t outsz)
{
    if (stub_conta_atual[0] == '\0') return false;
    snprintf(out, outsz, "%s", stub_conta_atual);
    return true;
}

static const char *n_name(void)  { return "fora do teste"; }
static const char *n_hint(void)  { return ""; }
static bool        n_ready(void) { return false; }
static void        n_logout(void){ }
static bool n_begin(char *a, size_t n) { (void)a;(void)n; return false; }
static bool n_root(const char *a, char *o, size_t n) { (void)a;(void)o;(void)n; return false; }
static bool n_find(const char *a, const char *p, const char *m, bool f, char *o, size_t n)
{ (void)a;(void)p;(void)m;(void)f;(void)o;(void)n; return false; }
static bool n_mk(const char *a, const char *p, const char *m, char *o, size_t n)
{ (void)a;(void)p;(void)m;(void)o;(void)n; return false; }
static bool n_put(const char *a, const char *p, const char *m, const char *l, const char *t)
{ (void)a;(void)p;(void)m;(void)l;(void)t; return false; }
static bool n_get(const char *a, const char *i, const char *l) { (void)a;(void)i;(void)l; return false; }
static bool n_rm(const char *a, const char *i) { (void)a;(void)i; return false; }
static bool n_list(const char *a, const char *f, cloud_list_cb cb, void *u)
{ (void)a;(void)f;(void)cb;(void)u; return false; }

const CloudBackend *drive_backend(void)
{
    static const CloudBackend b = {
        .key = "drive", .name = n_name, .is_ready = n_ready, .setup_hint = n_hint,
        .logout = n_logout, .begin = n_begin, .root = n_root, .find_child = n_find,
        .make_folder = n_mk, .put_file = n_put, .get_file = n_get, .remove = n_rm, .list = n_list,
    };
    return &b;
}
