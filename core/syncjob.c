#include "syncjob.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "config.h"
#include "drive.h"
#include "oauth.h"
#include "savemount.h"
#include "syncstate.h"

#define MAX_TITLES 128

static void say(syncjob_log_cb log, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void say(syncjob_log_cb log, const char *fmt, ...)
{
    char buf[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (log)
        log(buf);
}

bool syncjob_find_title(u64 application_id, TitleEntry *out)
{
    // Estático: são ~70 KB de TitleEntry, e no sysmodule a heap é apertada.
    // Não é reentrante, mas só existe uma thread chamando isso.
    static TitleEntry entries[MAX_TITLES];

    size_t n = titles_list_with_savedata(entries, MAX_TITLES);
    for (size_t i = 0; i < n; i++)
    {
        if (entries[i].application_id == application_id)
        {
            *out = entries[i];
            return true;
        }
    }
    return false;
}

bool syncjob_backup_title(const TitleEntry *title, syncjob_log_cb log)
{
    char safe[0x201];
    syncstate_sanitize_name(title->name, safe, sizeof(safe));

    char staging[0x280];
    snprintf(staging, sizeof(staging), "%s/%s", SYNC_STAGING_DIR, safe);

    syncstate_ensure_dirs();
    mkdir(SYNC_STAGING_DIR, 0777);
    mkdir(staging, 0777);

    // 1) save -> staging. Read-only: impossível estragar o save do jogo aqui.
    say(log, "Montando o save de %s...", title->name);
    if (!savemount_mount(title->application_id, title->uid, true))
    {
        say(log, "Nao consegui montar o save (jogo aberto? conta errada?)");
        return false;
    }

    bool copied = savemount_copy_tree("save:/", staging);
    savemount_unmount(false);

    if (!copied)
    {
        say(log, "Falhou ao copiar o save pro cartao");
        return false;
    }

    // 2) staging -> Drive
    say(log, "Pegando token do Google...");
    char token[2048];
    if (!oauth_get_fresh_access_token(token, sizeof(token)))
    {
        say(log, "Sem token valido — precisa entrar na conta pelo app");
        return false;
    }

    char root_id[128];
    if (!drive_ensure_app_folder(token, root_id, sizeof(root_id)))
    {
        say(log, "Nao achei/criei a pasta \"%s\" no Drive", DRIVE_APP_FOLDER_NAME);
        return false;
    }

    char game_id[128];
    if (!drive_ensure_subfolder(token, root_id, safe, game_id, sizeof(game_id)))
    {
        say(log, "Nao criei a pasta do jogo no Drive");
        return false;
    }

    say(log, "Subindo pro Drive...");
    if (!drive_upload_tree(token, game_id, staging))
    {
        say(log, "Upload falhou");
        return false;
    }

    say(log, "Backup de %s concluido", title->name);
    return true;
}

// ---------------------------------------------------------------------------
// Backup no proprio cartao (sem nuvem)
// ---------------------------------------------------------------------------

static void local_backup_path(const TitleEntry *title, char *out, size_t outsz)
{
    char safe[0x201];
    syncstate_sanitize_name(title->name, safe, sizeof(safe));
    snprintf(out, outsz, "%s/%s", SYNC_LOCAL_DIR, safe);
}

// "Tem pelo menos um arquivo de verdade aqui dentro?" — recursivo de propósito:
// uma árvore só de pastas vazias é tão inútil quanto pasta vazia, e é
// exatamente o que um download que não baixou nada deixa pra trás.
static bool dir_has_any_file(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return false;

    bool found = false;
    struct dirent *ent;
    while (!found && (ent = readdir(d)))
    {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
            continue;

        char path[0x300];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

        struct stat st;
        if (stat(path, &st) != 0)
            continue;

        if (S_ISDIR(st.st_mode))
            found = dir_has_any_file(path);
        else
            found = true;
    }
    closedir(d);
    return found;
}

static bool dir_is_empty(const char *dir)
{
    return !dir_has_any_file(dir);
}

bool syncjob_has_local_backup(const TitleEntry *title)
{
    char dir[0x280];
    local_backup_path(title, dir, sizeof(dir));

    DIR *d = opendir(dir);
    if (!d)
        return false;

    // Pasta vazia nao conta como backup.
    bool any = false;
    struct dirent *ent;
    while ((ent = readdir(d)))
    {
        if (strcmp(ent->d_name, ".") && strcmp(ent->d_name, ".."))
        {
            any = true;
            break;
        }
    }
    closedir(d);
    return any;
}

bool syncjob_backup_title_local(const TitleEntry *title, syncjob_log_cb log)
{
    char dir[0x280];
    local_backup_path(title, dir, sizeof(dir));

    syncstate_ensure_dirs();
    mkdir(SYNC_LOCAL_DIR, 0777);
    mkdir(dir, 0777);

    say(log, "Copiando o save de %s pro cartao...", title->name);
    if (!savemount_mount(title->application_id, title->uid, true))
    {
        say(log, "Nao consegui montar o save (jogo aberto? conta errada?)");
        return false;
    }

    bool copied = savemount_copy_tree("save:/", dir);
    savemount_unmount(false);

    if (!copied)
    {
        say(log, "Falhou ao copiar pro cartao");
        return false;
    }

    say(log, "Backup de %s guardado em %s", title->name, dir);
    return true;
}

bool syncjob_restore_title_local(const TitleEntry *title, syncjob_log_cb log)
{
    char dir[0x280];
    local_backup_path(title, dir, sizeof(dir));

    if (!syncjob_has_local_backup(title))
    {
        say(log, "Esse jogo nao tem backup no cartao");
        return false;
    }

    say(log, "Gravando no save do console...");
    if (!savemount_mount(title->application_id, title->uid, false))
    {
        say(log, "Nao consegui montar o save pra escrita");
        return false;
    }

    bool copied = savemount_copy_tree(dir, "save:/");
    savemount_unmount(copied); // commit so se a copia deu certo

    if (!copied)
    {
        say(log, "Falhou ao gravar o save");
        return false;
    }

    say(log, "Save de %s restaurado do cartao", title->name);
    return true;
}

// ---------------------------------------------------------------------------
// Impressão digital do save
// ---------------------------------------------------------------------------

static void fingerprint_dir(const char *dir, u64 *acc)
{
    DIR *d = opendir(dir);
    if (!d)
        return;

    struct dirent *ent;
    while ((ent = readdir(d)))
    {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
            continue;

        char path[0x310];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

        struct stat st;
        if (stat(path, &st) != 0)
            continue;

        if (S_ISDIR(st.st_mode))
        {
            fingerprint_dir(path, acc);
            continue;
        }

        // Mistura nome, tamanho e mtime. Não precisa ser criptográfico: só
        // precisa mudar quando o save muda.
        u64 h = 1469598103934665603ULL; // FNV-1a
        for (const char *p = ent->d_name; *p; p++)
        {
            h ^= (unsigned char)*p;
            h *= 1099511628211ULL;
        }
        h ^= (u64)st.st_size * 0x9E3779B97F4A7C15ULL;
        h ^= (u64)st.st_mtime * 0xC2B2AE3D27D4EB4FULL;

        *acc += h; // soma: não depende da ordem que o readdir devolveu
    }

    closedir(d);
}

bool syncjob_fingerprint(const TitleEntry *title, u64 *out)
{
    if (!savemount_mount(title->application_id, title->uid, true))
        return false;

    u64 acc = 0;
    fingerprint_dir("save:", &acc);
    savemount_unmount(false);

    *out = acc;
    return true;
}

static void marker_path(u64 application_id, char *out, size_t outsz)
{
    snprintf(out, outsz, "%s/rev-%016llX.txt", SYNC_APP_DIR,
        (unsigned long long)application_id);
}

void syncjob_mark_synced(u64 application_id, u64 fingerprint)
{
    syncstate_ensure_dirs();

    char path[0x120];
    marker_path(application_id, path, sizeof(path));

    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "%016llX\n", (unsigned long long)fingerprint);
    fclose(f);
}

bool syncjob_last_synced(u64 application_id, u64 *out)
{
    char path[0x120];
    marker_path(application_id, path, sizeof(path));

    FILE *f = fopen(path, "r");
    if (!f)
        return false;

    char line[64];
    bool ok = fgets(line, sizeof(line), f) != NULL;
    fclose(f);
    if (!ok)
        return false;

    *out = strtoull(line, NULL, 16);
    return true;
}

// ---------------------------------------------------------------------------
// Restore
// ---------------------------------------------------------------------------

bool syncjob_restore_title(const TitleEntry *title, syncjob_log_cb log)
{
    char safe[0x201];
    syncstate_sanitize_name(title->name, safe, sizeof(safe));

    char staging[0x280];
    snprintf(staging, sizeof(staging), "%s/%s", SYNC_STAGING_DIR, safe);

    syncstate_ensure_dirs();
    mkdir(SYNC_STAGING_DIR, 0777);
    mkdir(staging, 0777);

    say(log, "Pegando token do Google...");
    char token[2048];
    if (!oauth_get_fresh_access_token(token, sizeof(token)))
    {
        say(log, "Sem token valido — precisa entrar na conta pelo app");
        return false;
    }

    char root_id[128];
    if (!drive_ensure_app_folder(token, root_id, sizeof(root_id)))
    {
        say(log, "Nao achei a pasta \"%s\" no Drive", DRIVE_APP_FOLDER_NAME);
        return false;
    }

    // find, e não ensure: "ensure" CRIA a pasta quando não acha, e foi assim
    // que um jogo sem backup na nuvem virou "restore de pasta vazia" que ainda
    // assim montava o save e commitava. Quem lê da nuvem nunca cria nada.
    char game_id[128];
    if (!drive_find_subfolder(token, root_id, safe, game_id, sizeof(game_id)))
    {
        say(log, "Esse jogo nao tem backup no Drive");
        return false;
    }

    say(log, "Baixando da nuvem...");
    if (!drive_download_tree(token, game_id, staging))
    {
        say(log, "Download falhou");
        return false;
    }

    // Segunda trava, independente da primeira: se por qualquer motivo o staging
    // veio vazio, não existe restauração possível — e escrever "nada" por cima
    // de um save bom, commitando, é exatamente o caminho da corrupção.
    if (dir_is_empty(staging))
    {
        say(log, "O backup na nuvem esta vazio — nao vou gravar nada por cima");
        return false;
    }

    // Só aqui monta pra escrita, e só depois do download ter dado certo: se a
    // rede cair no meio, o save local não foi tocado.
    say(log, "Gravando no save do console...");
    if (!savemount_mount(title->application_id, title->uid, false))
    {
        say(log, "Nao consegui montar o save pra escrita");
        return false;
    }

    bool copied = savemount_copy_tree(staging, "save:/");
    savemount_unmount(copied); // commit só se a cópia deu certo

    if (!copied)
    {
        say(log, "Falhou ao gravar o save");
        return false;
    }

    say(log, "Save de %s veio da nuvem", title->name);
    return true;
}
