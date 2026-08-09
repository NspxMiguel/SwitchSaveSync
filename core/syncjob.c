#include "syncjob.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "drive.h"
#include "lang.h"
#include "oauth.h"
#include "savemount.h"
#include "syncstate.h"

#define MAX_TITLES 128

static void clear_dir(const char *dir); // definida junto com dir_is_empty

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

// Atenção: com mais de uma conta tendo save do mesmo jogo, isso devolve a
// PRIMEIRA que aparecer — o application_id sozinho não diz de quem é o save.
// Quem chama é só o sysmodule de autosync, que hoje está fora de escopo e só
// sabe o application_id do jogo que fechou; quando ele voltar, vai precisar
// descobrir a conta que estava jogando antes de chamar aqui.
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

// O nome da pasta desse save — no Drive, no staging e no backup do cartão.
//
// O dono do save só entra no nome quando o console TEM mais de um save pra
// esse jogo (duas contas, ou uma conta e o console). Não é preguiça: incluir
// sempre renomearia a pasta de todo mundo, e save que já está no Drive numa
// pasta com o nome antigo viraria órfão — o app não acharia mais e trataria
// como "primeira sync", pronto pra subir o save de estreia por cima. Do jeito
// que está, jogo de save único mantém exatamente o nome de sempre.
static void save_folder_name(const TitleEntry *title, char *out, size_t outsz)
{
    // Sem sufixo quando não há com o que confundir; e sem sufixo também quando
    // é save de conta e o apelido não veio (nome chutado viraria pasta órfã na
    // próxima versão).
    if (!title->shared_game || (!title->device_save && title->account[0] == '\0'))
    {
        syncstate_sanitize_name(title->name, out, outsz);
        return;
    }

    // O sufixo é justamente o que separa um save do outro, então não pode ser
    // ele a sobrar da truncagem. Reservo o espaço dele primeiro; quem encurta,
    // se precisar, é o nome do jogo.
    char sufixo[0x28];
    if (title->device_save)
        snprintf(sufixo, sizeof(sufixo), " (console)");
    else
        snprintf(sufixo, sizeof(sufixo), " (%s)", title->account);

    char junto[0x201];
    size_t espaco = sizeof(junto) - strlen(sufixo) - 1;
    snprintf(junto, sizeof(junto), "%.*s%s", (int)espaco, title->name, sufixo);

    syncstate_sanitize_name(junto, out, outsz);
}

bool syncjob_backup_title(const TitleEntry *title, syncjob_log_cb log)
{
    char safe[0x201];
    save_folder_name(title, safe, sizeof(safe));

    char staging[0x280];
    snprintf(staging, sizeof(staging), "%s/%s", SYNC_STAGING_DIR, safe);

    syncstate_ensure_dirs();
    mkdir(SYNC_STAGING_DIR, 0777);
    mkdir(staging, 0777);

    // 1) save -> staging. Read-only: impossível estragar o save do jogo aqui.
    say(log, TR("Montando o save de %s...", "Mounting %s's save..."), title->name);
    if (!savemount_mount_typed(title->application_id, title->uid, title->device_save, true))
    {
        say(log, TR("Não consegui montar o save (jogo aberto? conta errada?)", "Couldn't mount the save (game running? wrong account?)"));
        return false;
    }

    // Esta pasta de staging é a MESMA que o restore usa pra baixar da nuvem.
    // Sem limpar, um restore anterior deixa arquivo dele aqui e o backup sobe
    // o save de agora misturado com o que veio da nuvem antes.
    clear_dir(staging);

    bool copied = savemount_copy_tree("save:/", staging);
    savemount_unmount(false);

    if (!copied)
    {
        say(log, TR("Falhou ao copiar o save pro cartão", "Failed to copy the save to the SD card"));
        return false;
    }

    // 2) staging -> Drive
    say(log, TR("Pegando token do Google...", "Getting a Google token..."));
    char token[2048];
    if (!oauth_get_fresh_access_token(token, sizeof(token)))
    {
        say(log, TR("Sem token válido — precisa entrar na conta pelo app", "No valid token — sign in from the app first"));
        return false;
    }

    char root_id[128];
    if (!drive_ensure_app_folder(token, root_id, sizeof(root_id)))
    {
        say(log, TR("Não achei/criei a pasta \"%s\" no Drive", "Couldn't find/create the \"%s\" folder on Drive"), DRIVE_APP_FOLDER_NAME);
        return false;
    }

    char game_id[128];
    if (!drive_ensure_subfolder(token, root_id, safe, game_id, sizeof(game_id)))
    {
        say(log, TR("Não criei a pasta do jogo no Drive", "Couldn't create the game's folder on Drive"));
        return false;
    }

    say(log, TR("Subindo pro Drive...", "Uploading to Drive..."));
    if (!drive_upload_tree(token, game_id, staging))
    {
        say(log, TR("Upload falhou", "Upload failed"));
        return false;
    }

    // Subir só escreve. O que o save não tem mais precisa sair da nuvem, senão
    // volta no próximo restore. Vai pra lixeira do Drive, não some.
    drive_prune_extras(token, game_id, staging);

    say(log, TR("Backup de %s concluído", "Backup of %s done"), title->name);
    return true;
}

// ---------------------------------------------------------------------------
// Backup no proprio cartao (sem nuvem)
// ---------------------------------------------------------------------------

static void local_backup_path(const TitleEntry *title, char *out, size_t outsz)
{
    char safe[0x201];
    save_folder_name(title, safe, sizeof(safe));
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

// Conta arquivos e bytes de uma árvore.
static void dir_stats(const char *dir, int *files, u64 *bytes)
{
    DIR *d = opendir(dir);
    if (!d)
        return;

    struct dirent *ent;
    while ((ent = readdir(d)))
    {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
            continue;

        char path[0x300];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

        struct stat st;
        if (stat(path, &st) != 0)
            continue;

        if (S_ISDIR(st.st_mode))
            dir_stats(path, files, bytes);
        else
        {
            (*files)++;
            *bytes += (u64)st.st_size;
        }
    }
    closedir(d);
}

// O tamanho dos dois lados, lado a lado, na hora do conflito.
//
// Sem isso a tela dizia "os dois mudaram, escolha" e não dava NADA em cima do
// que escolher — e é justamente aí que uma escolha errada apaga progresso. O
// conversa privada removida do historico
// de estreia no console contra o save de verdade no Drive. Na tela os dois são
// "um save"; em número, um tem alguns KB e o outro não.
static void say_dois_lados(syncjob_log_cb log, const char *console_dir, const char *cloud_dir)
{
    int cf = 0, nf = 0;
    u64 cb = 0, nb = 0;
    dir_stats(console_dir, &cf, &cb);
    dir_stats(cloud_dir, &nf, &nb);

    say(log, TR("  no console: %d arquivo%s, %llu KB", "  on the console: %d file%s, %llu KB"), cf, cf == 1 ? "" : "s",
        (unsigned long long)((cb + 1023) / 1024));
    say(log, TR("  na nuvem:   %d arquivo%s, %llu KB", "  in the cloud:  %d file%s, %llu KB"), nf, nf == 1 ? "" : "s",
        (unsigned long long)((nb + 1023) / 1024));
}

// Esvazia a pasta de staging (mantém a pasta em si).
//
// O staging tem nome de jogo, então o lixo que sobra ali é do download
// ANTERIOR do mesmo jogo. Se a nuvem tiver perdido um arquivo desde então, sem
// isso o arquivo velho continuaria no staging e seria copiado pra dentro do
// save junto com o resto — restaurar o save de ontem misturado com o de hoje.
static void clear_dir(const char *dir)
{
    DIR *d = opendir(dir);
    if (!d)
        return;

    struct dirent *ent;
    while ((ent = readdir(d)))
    {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, ".."))
            continue;

        char path[0x300];
        snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);

        struct stat st;
        if (stat(path, &st) != 0)
            continue;

        if (S_ISDIR(st.st_mode))
        {
            clear_dir(path);
            rmdir(path);
        }
        else
        {
            unlink(path);
        }
    }
    closedir(d);
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

    say(log, TR("Copiando o save de %s pro cartão...", "Copying %s's save to the SD card..."), title->name);
    if (!savemount_mount_typed(title->application_id, title->uid, title->device_save, true))
    {
        say(log, TR("Não consegui montar o save (jogo aberto? conta errada?)", "Couldn't mount the save (game running? wrong account?)"));
        return false;
    }

    // Backup novo não herda sobra do antigo: sem isso, arquivo que o jogo
    // apagou continuaria aqui e voltaria pro save no restore do cartão.
    clear_dir(dir);

    bool copied = savemount_copy_tree("save:/", dir);
    savemount_unmount(false);

    if (!copied)
    {
        say(log, TR("Falhou ao copiar pro cartão", "Failed to copy to the SD card"));
        return false;
    }

    say(log, TR("Backup de %s guardado em %s", "Backup of %s stored in %s"), title->name, dir);
    return true;
}

// Escreve src_dir por cima do save do jogo, deixando o save IGUAL a src_dir:
// o que estava lá e não está em src_dir sai fora.
//
// Apagar antes de escrever parece pior do que é: nada disso persiste sem o
// fsdevCommitDevice, que só acontece no fim e só se tudo deu certo. Se falhar
// no meio — rede, bateria, crash — o save de antes continua inteiro.
//
// Sem apagar, restaurar um save que tem MENOS arquivos que o de agora deixa os
// velhos no meio dos novos: save metade de ontem, metade de hoje. É assim que
// um jogo abre e diz que os dados estão corrompidos.
static bool write_over_save(const TitleEntry *title, const char *src_dir, syncjob_log_cb log)
{
    if (!savemount_mount_typed(title->application_id, title->uid, title->device_save, false))
    {
        say(log, TR("Não consegui montar o save pra escrita", "Couldn't mount the save for writing"));
        return false;
    }

    if (!savemount_wipe_contents())
    {
        savemount_unmount(false); // sem commit: nada do que apaguei valeu
        say(log, TR("Não consegui limpar o save antes de gravar", "Couldn't clear the save before writing"));
        return false;
    }

    bool copied = savemount_copy_tree(src_dir, "save:/");
    savemount_unmount(copied); // commit só se a cópia deu certo

    if (!copied)
        say(log, TR("Falhou ao gravar o save", "Failed to write the save"));
    return copied;
}

bool syncjob_restore_title_local(const TitleEntry *title, syncjob_log_cb log)
{
    char dir[0x280];
    local_backup_path(title, dir, sizeof(dir));

    if (!syncjob_has_local_backup(title))
    {
        say(log, TR("Esse jogo não tem backup no cartão", "This game has no backup on the SD card"));
        return false;
    }

    say(log, TR("Gravando no save do console...", "Writing to the console's save..."));
    if (!write_over_save(title, dir, log))
        return false;

    say(log, TR("Save de %s restaurado do cartão", "%s's save restored from the SD card"), title->name);
    return true;
}

// ---------------------------------------------------------------------------
// Impressão digital do save
// ---------------------------------------------------------------------------

// Impressão digital de nome + tamanho + CONTEÚDO. O mtime ficou de fora de
// propósito: ele serve pra comparar dois momentos do mesmo cartão, mas aqui a
// comparação é entre o save do console e o save que veio do Drive — e arquivo
// baixado nasce com mtime de agora. Com mtime na conta, os dois lados NUNCA
// batiam, e "iguais" seria reportado como "diferentes" toda vez.
//
// Ler o conteúdo inteiro é barato aqui: save de Switch é pequeno (o do Mario 3D
// World tem 64 KB), e neste ponto o save da nuvem já foi baixado mesmo.
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

        u64 h = 1469598103934665603ULL; // FNV-1a
        for (const char *p = ent->d_name; *p; p++)
        {
            h ^= (unsigned char)*p;
            h *= 1099511628211ULL;
        }
        h ^= (u64)st.st_size * 0x9E3779B97F4A7C15ULL;

        FILE *f = fopen(path, "rb");
        if (f)
        {
            unsigned char buf[4096];
            size_t n;
            while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
                for (size_t i = 0; i < n; i++)
                {
                    h ^= buf[i];
                    h *= 1099511628211ULL;
                }
            fclose(f);
        }
        else
        {
            // Não deu pra ler: marca como "desconhecido" em vez de deixar o
            // hash igual ao de um arquivo vazio, que seria uma igualdade falsa.
            h ^= 0xDEADBEEFDEADBEEFULL;
        }

        *acc += h; // soma: não depende da ordem que o readdir devolveu
    }

    closedir(d);
}

bool syncjob_fingerprint(const TitleEntry *title, u64 *out)
{
    if (!savemount_mount_typed(title->application_id, title->uid, title->device_save, true))
        return false;

    u64 acc = 0;
    fingerprint_dir("save:", &acc);
    savemount_unmount(false);

    *out = acc;
    return true;
}

// Mesma regra do save_folder_name: a conta só entra no nome quando o jogo tem
// save de mais de uma. Marcador é por (jogo, conta) — com um marcador só pros
// dois, o save de uma conta seria comparado contra o que a OUTRA sincronizou, e
// a decisão de "quem mudou" sairia errada nos dois sentidos: conflito onde não
// tem, e pior, sobrescrita silenciosa onde tem.
static void marker_path(const TitleEntry *title, char *out, size_t outsz)
{
    if (title->shared_game)
        snprintf(out, outsz, "%s/rev-%016llX-%016llX%016llX.txt", SYNC_APP_DIR,
            (unsigned long long)title->application_id,
            (unsigned long long)title->uid.uid[0],
            (unsigned long long)title->uid.uid[1]);
    else
        snprintf(out, outsz, "%s/rev-%016llX.txt", SYNC_APP_DIR,
            (unsigned long long)title->application_id);
}

void syncjob_mark_synced(const TitleEntry *title, u64 fingerprint)
{
    syncstate_ensure_dirs();

    char path[0x120];
    marker_path(title, path, sizeof(path));

    FILE *f = fopen(path, "w");
    if (!f)
        return;
    fprintf(f, "%016llX\n", (unsigned long long)fingerprint);
    fclose(f);
}

bool syncjob_last_synced(const TitleEntry *title, u64 *out)
{
    char path[0x120];
    marker_path(title, path, sizeof(path));

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
    save_folder_name(title, safe, sizeof(safe));

    char staging[0x280];
    snprintf(staging, sizeof(staging), "%s/%s", SYNC_STAGING_DIR, safe);

    syncstate_ensure_dirs();
    mkdir(SYNC_STAGING_DIR, 0777);
    mkdir(staging, 0777);

    say(log, TR("Pegando token do Google...", "Getting a Google token..."));
    char token[2048];
    if (!oauth_get_fresh_access_token(token, sizeof(token)))
    {
        say(log, TR("Sem token válido — precisa entrar na conta pelo app", "No valid token — sign in from the app first"));
        return false;
    }

    char root_id[128];
    if (!drive_ensure_app_folder(token, root_id, sizeof(root_id)))
    {
        say(log, TR("Não achei a pasta \"%s\" no Drive", "Couldn't find the \"%s\" folder on Drive"), DRIVE_APP_FOLDER_NAME);
        return false;
    }

    // find, e não ensure: "ensure" CRIA a pasta quando não acha, e foi assim
    // que um jogo sem backup na nuvem virou "restore de pasta vazia" que ainda
    // assim montava o save e commitava. Quem lê da nuvem nunca cria nada.
    char game_id[128];
    if (!drive_find_subfolder(token, root_id, safe, game_id, sizeof(game_id)))
    {
        say(log, TR("Esse jogo não tem backup no Drive", "This game has no backup on Drive"));
        return false;
    }

    say(log, TR("Baixando da nuvem...", "Downloading from the cloud..."));
    clear_dir(staging); // ver clear_dir(): sobra de download antigo ia junto
    if (!drive_download_tree(token, game_id, staging))
    {
        say(log, TR("Download falhou", "Download failed"));
        return false;
    }

    // Segunda trava, independente da primeira: se por qualquer motivo o staging
    // veio vazio, não existe restauração possível — e escrever "nada" por cima
    // de um save bom, commitando, é exatamente o caminho da corrupção.
    if (dir_is_empty(staging))
    {
        say(log, TR("O backup na nuvem está vazio — não vou gravar nada por cima", "The cloud backup is empty — nothing will be written over your save"));
        return false;
    }

    // Só aqui monta pra escrita, e só depois do download ter dado certo: se a
    // rede cair no meio, o save local não foi tocado.
    say(log, TR("Gravando no save do console...", "Writing to the console's save..."));
    if (!write_over_save(title, staging, log))
        return false;

    say(log, TR("Save de %s veio da nuvem", "%s's save came from the cloud"), title->name);
    return true;
}

// ---------------------------------------------------------------------------
// Sync de um clique
// ---------------------------------------------------------------------------
//
// conversa privada removida do historico
// conversa privada removida do historico
// vai — esta função decide.
//
// Pra decidir sem chutar são precisos TRÊS números, não dois: o save de agora
// no console, o save de agora na nuvem, e o save de quando os dois estavam
// iguais pela última vez (o marcador rev-<id>.txt, gravado só depois de uma
// sync que deu certo). Com "console" e "nuvem" só dá pra saber que estão
// diferentes; é o terceiro que diz QUEM mudou.
//
//   nuvem vazia/inexistente          -> sobe
//   console == nuvem                 -> nada a fazer
//   só a nuvem mudou desde a última  -> desce
//   só o console mudou desde a última-> sobe
//   os dois mudaram (ou sem marcador)-> CONFLITO, não escreve nada
//
// O caso "os dois mudaram" é o único que importa de verdade: é jogar no Switch
// e jogar em outro lugar sem sincronizar no meio. Escolher sozinho aí apaga
// progresso de um dos lados, então ele escolhe, pelo menu do Y.
//
// Tudo que envolve o save do console é feito em cópia no cartão: o save é
// montado read-only uma vez, copiado, e desmontado. Daí em diante é arquivo
// comum. A única escrita em save é o passo final do ramo "desce".
SyncjobSyncResult syncjob_sync_title(const TitleEntry *title, syncjob_log_cb log)
{
    char safe[0x201];
    save_folder_name(title, safe, sizeof(safe));

    char cloud_dir[0x2A0], console_dir[0x2A0];
    snprintf(cloud_dir, sizeof(cloud_dir), "%s/%s", SYNC_STAGING_DIR, safe);
    snprintf(console_dir, sizeof(console_dir), "%s/%s.console", SYNC_STAGING_DIR, safe);

    syncstate_ensure_dirs();
    mkdir(SYNC_STAGING_DIR, 0777);
    mkdir(cloud_dir, 0777);
    mkdir(console_dir, 0777);

    // 1) o que tem no console, agora
    say(log, TR("Lendo o save de %s...", "Reading %s's save..."), title->name);
    if (!savemount_mount_typed(title->application_id, title->uid, title->device_save, true))
    {
        // Duas causas bem diferentes caem aqui, e a mensagem antiga só citava
        // uma: o jogo aberto agora, e o jogo que nunca foi aberto (aí o save
        // sequer existe pra ser montado).
        say(log, TR("Não consegui abrir o save. O jogo está rodando agora, ou", "Couldn't open the save. The game is running right now, or"));
        say(log, TR("nunca foi aberto neste console — abra ele uma vez e volte.", "it was never opened on this console — open it once and come back."));
        return SYNCJOB_SYNC_FAILED;
    }
    clear_dir(console_dir);
    bool got_console = savemount_copy_tree("save:/", console_dir);
    savemount_unmount(false);

    if (!got_console)
    {
        say(log, TR("Falhou ao copiar o save pro cartão", "Failed to copy the save to the SD card"));
        return SYNCJOB_SYNC_FAILED;
    }

    u64 local_fp = 0;
    fingerprint_dir(console_dir, &local_fp);
    bool local_vazio = dir_is_empty(console_dir);

    // 2) o que tem na nuvem, agora
    say(log, TR("Pegando token do Google...", "Getting a Google token..."));
    char token[2048];
    if (!oauth_get_fresh_access_token(token, sizeof(token)))
    {
        say(log, TR("Sem token válido — precisa entrar na conta pelo app", "No valid token — sign in from the app first"));
        return SYNCJOB_SYNC_FAILED;
    }

    char root_id[128];
    if (!drive_ensure_app_folder(token, root_id, sizeof(root_id)))
    {
        say(log, TR("Não achei/criei a pasta \"%s\" no Drive", "Couldn't find/create the \"%s\" folder on Drive"), DRIVE_APP_FOLDER_NAME);
        return SYNCJOB_SYNC_FAILED;
    }

    // find, não ensure: procurar não pode criar pasta. Ver syncjob_restore_title.
    char game_id[128];
    bool tem_na_nuvem = drive_find_subfolder(token, root_id, safe, game_id, sizeof(game_id));

    u64 cloud_fp = 0;
    bool cloud_vazio = true;

    if (tem_na_nuvem)
    {
        say(log, TR("Baixando o save da nuvem pra comparar...", "Downloading the cloud save to compare..."));
        clear_dir(cloud_dir);
        if (!drive_download_tree(token, game_id, cloud_dir))
        {
            say(log, TR("Download falhou", "Download failed"));
            return SYNCJOB_SYNC_FAILED;
        }
        fingerprint_dir(cloud_dir, &cloud_fp);
        cloud_vazio = dir_is_empty(cloud_dir);
    }

    // 3) decidir
    if (local_vazio && cloud_vazio)
    {
        say(log, TR("Esse jogo não tem save nem aqui nem na nuvem", "This game has no save here and none in the cloud"));
        return SYNCJOB_SYNC_NOTHING;
    }

    // Nuvem vazia (ou nem existe) e save aqui: primeira sync desse jogo.
    if (cloud_vazio)
    {
        say(log, TR("Primeira sync desse jogo — subindo o save do console", "First sync for this game — uploading the console's save"));
        if (!drive_ensure_subfolder(token, root_id, safe, game_id, sizeof(game_id)) ||
            !drive_upload_tree(token, game_id, console_dir))
        {
            say(log, TR("Upload falhou", "Upload failed"));
            return SYNCJOB_SYNC_FAILED;
        }
        drive_prune_extras(token, game_id, console_dir);
        syncjob_mark_synced(title, local_fp);
        say(log, TR("Save de %s guardado na nuvem", "%s's save stored in the cloud"), title->name);
        return SYNCJOB_SYNC_UPLOADED;
    }

    // Espelho do caso de cima: não tem save aqui e tem na nuvem. Baixar é
    // seguro por definição — não existe progresso no console pra ser apagado.
    // Sem este ramo, o caso mais comum de todos (jogo reinstalado, save só no
    // Drive) caía em CONFLITO lá embaixo por falta de marcador, e o app pedia
    // pra escolher entre um save e o nada.
    if (local_vazio)
    {
        say(log, TR("Não tem save aqui e tem na nuvem — trazendo pro console...", "No save here but there is one in the cloud — bringing it over..."));
        if (!write_over_save(title, cloud_dir, log))
            return SYNCJOB_SYNC_FAILED;

        syncjob_mark_synced(title, cloud_fp);
        say(log, TR("Save de %s veio da nuvem", "%s's save came from the cloud"), title->name);
        return SYNCJOB_SYNC_DOWNLOADED;
    }

    if (local_fp == cloud_fp)
    {
        // Grava o marcador mesmo sem transferir nada: os dois lados estão
        // iguais AGORA, e é exatamente isso que o marcador significa. Sem
        // esta linha, a primeira sync depois de uma instalação nova cairia
        // em conflito na próxima vez que qualquer um dos lados mudasse.
        syncjob_mark_synced(title, local_fp);
        say(log, TR("Já estava sincronizado — não mexi em nada", "Already in sync — nothing was touched"));
        return SYNCJOB_SYNC_EQUAL;
    }

    u64 ultima = 0;
    bool tem_marcador = syncjob_last_synced(title, &ultima);

    if (!tem_marcador)
    {
        say(log, TR("Primeira sync deste jogo por aqui, e já tem save dos dois", "First sync for this game here, and there is already a save on both"));
        say(log, TR("lados. Sem uma sync anterior não dá pra saber qual andou.", "sides. With no earlier sync there is no way to tell which one moved."));
        say_dois_lados(log, console_dir, cloud_dir);
        say(log, TR("Se você reinstalou o jogo e abriu uma vez, o do console é o", "If you reinstalled the game and opened it once, the console's is the"));
        say(log, TR("save de estreia — nesse caso é o da nuvem que você quer.", "brand-new save — in that case the cloud one is what you want."));
        return SYNCJOB_SYNC_CONFLICT;
    }

    bool console_mudou = (local_fp != ultima);
    bool nuvem_mudou   = (cloud_fp != ultima);

    if (console_mudou && nuvem_mudou)
    {
        say(log, TR("Mudou dos DOIS lados desde a última sync.", "BOTH sides changed since the last sync."));
        say_dois_lados(log, console_dir, cloud_dir);
        return SYNCJOB_SYNC_CONFLICT;
    }

    if (nuvem_mudou)
    {
        // O save daqui é o mesmo de quando sincronizou; quem andou foi a nuvem.
        // O console_dir continua no cartão: se der ruim, o save de antes de
        // escrever está ali inteiro.
        say(log, TR("A nuvem está mais nova — trazendo pro console...", "The cloud one is newer — bringing it to the console..."));
        if (!write_over_save(title, cloud_dir, log))
            return SYNCJOB_SYNC_FAILED;

        syncjob_mark_synced(title, cloud_fp);
        say(log, TR("Save de %s veio da nuvem", "%s's save came from the cloud"), title->name);
        return SYNCJOB_SYNC_DOWNLOADED;
    }

    // Sobrou: só o console mudou.
    say(log, TR("O save daqui está mais novo — subindo...", "This console's save is newer — uploading..."));
    if (!drive_upload_tree(token, game_id, console_dir))
    {
        say(log, TR("Upload falhou", "Upload failed"));
        return SYNCJOB_SYNC_FAILED;
    }
    drive_prune_extras(token, game_id, console_dir);
    syncjob_mark_synced(title, local_fp);
    say(log, TR("Save de %s subiu pra nuvem", "%s's save went up to the cloud"), title->name);
    return SYNCJOB_SYNC_UPLOADED;
}
