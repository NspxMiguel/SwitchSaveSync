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

    // Esta pasta de staging é a MESMA que o restore usa pra baixar da nuvem.
    // Sem limpar, um restore anterior deixa arquivo dele aqui e o backup sobe
    // o save de agora misturado com o que veio da nuvem antes.
    clear_dir(staging);

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

    // Subir só escreve. O que o save não tem mais precisa sair da nuvem, senão
    // volta no próximo restore. Vai pra lixeira do Drive, não some.
    drive_prune_extras(token, game_id, staging);

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

    say(log, "  no console: %d arquivo%s, %llu KB", cf, cf == 1 ? "" : "s",
        (unsigned long long)((cb + 1023) / 1024));
    say(log, "  na nuvem:   %d arquivo%s, %llu KB", nf, nf == 1 ? "" : "s",
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

    say(log, "Copiando o save de %s pro cartao...", title->name);
    if (!savemount_mount(title->application_id, title->uid, true))
    {
        say(log, "Nao consegui montar o save (jogo aberto? conta errada?)");
        return false;
    }

    // Backup novo não herda sobra do antigo: sem isso, arquivo que o jogo
    // apagou continuaria aqui e voltaria pro save no restore do cartão.
    clear_dir(dir);

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
    if (!savemount_mount(title->application_id, title->uid, false))
    {
        say(log, "Nao consegui montar o save pra escrita");
        return false;
    }

    if (!savemount_wipe_contents())
    {
        savemount_unmount(false); // sem commit: nada do que apaguei valeu
        say(log, "Nao consegui limpar o save antes de gravar");
        return false;
    }

    bool copied = savemount_copy_tree(src_dir, "save:/");
    savemount_unmount(copied); // commit só se a cópia deu certo

    if (!copied)
        say(log, "Falhou ao gravar o save");
    return copied;
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
    if (!write_over_save(title, dir, log))
        return false;

    say(log, "Save de %s restaurado do cartao", title->name);
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
    clear_dir(staging); // ver clear_dir(): sobra de download antigo ia junto
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
    if (!write_over_save(title, staging, log))
        return false;

    say(log, "Save de %s veio da nuvem", title->name);
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
    syncstate_sanitize_name(title->name, safe, sizeof(safe));

    char cloud_dir[0x2A0], console_dir[0x2A0];
    snprintf(cloud_dir, sizeof(cloud_dir), "%s/%s", SYNC_STAGING_DIR, safe);
    snprintf(console_dir, sizeof(console_dir), "%s/%s.console", SYNC_STAGING_DIR, safe);

    syncstate_ensure_dirs();
    mkdir(SYNC_STAGING_DIR, 0777);
    mkdir(cloud_dir, 0777);
    mkdir(console_dir, 0777);

    // 1) o que tem no console, agora
    say(log, "Lendo o save de %s...", title->name);
    if (!savemount_mount(title->application_id, title->uid, true))
    {
        // Duas causas bem diferentes caem aqui, e a mensagem antiga só citava
        // uma: o jogo aberto agora, e o jogo que nunca foi aberto (aí o save
        // sequer existe pra ser montado).
        say(log, "Nao consegui abrir o save. O jogo esta rodando agora, ou");
        say(log, "nunca foi aberto neste console — abra ele uma vez e volte.");
        return SYNCJOB_SYNC_FAILED;
    }
    clear_dir(console_dir);
    bool got_console = savemount_copy_tree("save:/", console_dir);
    savemount_unmount(false);

    if (!got_console)
    {
        say(log, "Falhou ao copiar o save pro cartao");
        return SYNCJOB_SYNC_FAILED;
    }

    u64 local_fp = 0;
    fingerprint_dir(console_dir, &local_fp);
    bool local_vazio = dir_is_empty(console_dir);

    // 2) o que tem na nuvem, agora
    say(log, "Pegando token do Google...");
    char token[2048];
    if (!oauth_get_fresh_access_token(token, sizeof(token)))
    {
        say(log, "Sem token valido — precisa entrar na conta pelo app");
        return SYNCJOB_SYNC_FAILED;
    }

    char root_id[128];
    if (!drive_ensure_app_folder(token, root_id, sizeof(root_id)))
    {
        say(log, "Nao achei/criei a pasta \"%s\" no Drive", DRIVE_APP_FOLDER_NAME);
        return SYNCJOB_SYNC_FAILED;
    }

    // find, não ensure: procurar não pode criar pasta. Ver syncjob_restore_title.
    char game_id[128];
    bool tem_na_nuvem = drive_find_subfolder(token, root_id, safe, game_id, sizeof(game_id));

    u64 cloud_fp = 0;
    bool cloud_vazio = true;

    if (tem_na_nuvem)
    {
        say(log, "Baixando o save da nuvem pra comparar...");
        clear_dir(cloud_dir);
        if (!drive_download_tree(token, game_id, cloud_dir))
        {
            say(log, "Download falhou");
            return SYNCJOB_SYNC_FAILED;
        }
        fingerprint_dir(cloud_dir, &cloud_fp);
        cloud_vazio = dir_is_empty(cloud_dir);
    }

    // 3) decidir
    if (local_vazio && cloud_vazio)
    {
        say(log, "Esse jogo nao tem save nem aqui nem na nuvem");
        return SYNCJOB_SYNC_NOTHING;
    }

    // Nuvem vazia (ou nem existe) e save aqui: primeira sync desse jogo.
    if (cloud_vazio)
    {
        say(log, "Primeira sync desse jogo — subindo o save do console");
        if (!drive_ensure_subfolder(token, root_id, safe, game_id, sizeof(game_id)) ||
            !drive_upload_tree(token, game_id, console_dir))
        {
            say(log, "Upload falhou");
            return SYNCJOB_SYNC_FAILED;
        }
        drive_prune_extras(token, game_id, console_dir);
        syncjob_mark_synced(title->application_id, local_fp);
        say(log, "Save de %s guardado na nuvem", title->name);
        return SYNCJOB_SYNC_UPLOADED;
    }

    // Espelho do caso de cima: não tem save aqui e tem na nuvem. Baixar é
    // seguro por definição — não existe progresso no console pra ser apagado.
    // Sem este ramo, o caso mais comum de todos (jogo reinstalado, save só no
    // Drive) caía em CONFLITO lá embaixo por falta de marcador, e o app pedia
    // pra escolher entre um save e o nada.
    if (local_vazio)
    {
        say(log, "Nao tem save aqui e tem na nuvem — trazendo pro console...");
        if (!write_over_save(title, cloud_dir, log))
            return SYNCJOB_SYNC_FAILED;

        syncjob_mark_synced(title->application_id, cloud_fp);
        say(log, "Save de %s veio da nuvem", title->name);
        return SYNCJOB_SYNC_DOWNLOADED;
    }

    if (local_fp == cloud_fp)
    {
        // Grava o marcador mesmo sem transferir nada: os dois lados estão
        // iguais AGORA, e é exatamente isso que o marcador significa. Sem
        // esta linha, a primeira sync depois de uma instalação nova cairia
        // em conflito na próxima vez que qualquer um dos lados mudasse.
        syncjob_mark_synced(title->application_id, local_fp);
        say(log, "Ja estava sincronizado — nao mexi em nada");
        return SYNCJOB_SYNC_EQUAL;
    }

    u64 ultima = 0;
    bool tem_marcador = syncjob_last_synced(title->application_id, &ultima);

    if (!tem_marcador)
    {
        say(log, "Primeira sync deste jogo por aqui, e ja tem save dos dois");
        say(log, "lados. Sem uma sync anterior nao da pra saber qual andou.");
        say_dois_lados(log, console_dir, cloud_dir);
        say(log, "Se voce reinstalou o jogo e abriu uma vez, o do console e o");
        say(log, "save de estreia — nesse caso e o da nuvem que voce quer.");
        return SYNCJOB_SYNC_CONFLICT;
    }

    bool console_mudou = (local_fp != ultima);
    bool nuvem_mudou   = (cloud_fp != ultima);

    if (console_mudou && nuvem_mudou)
    {
        say(log, "Mudou dos DOIS lados desde a ultima sync.");
        say_dois_lados(log, console_dir, cloud_dir);
        return SYNCJOB_SYNC_CONFLICT;
    }

    if (nuvem_mudou)
    {
        // O save daqui é o mesmo de quando sincronizou; quem andou foi a nuvem.
        // O console_dir continua no cartão: se der ruim, o save de antes de
        // escrever está ali inteiro.
        say(log, "A nuvem esta mais nova — trazendo pro console...");
        if (!write_over_save(title, cloud_dir, log))
            return SYNCJOB_SYNC_FAILED;

        syncjob_mark_synced(title->application_id, cloud_fp);
        say(log, "Save de %s veio da nuvem", title->name);
        return SYNCJOB_SYNC_DOWNLOADED;
    }

    // Sobrou: só o console mudou.
    say(log, "O save daqui esta mais novo — subindo...");
    if (!drive_upload_tree(token, game_id, console_dir))
    {
        say(log, "Upload falhou");
        return SYNCJOB_SYNC_FAILED;
    }
    drive_prune_extras(token, game_id, console_dir);
    syncjob_mark_synced(title->application_id, local_fp);
    say(log, "Save de %s subiu pra nuvem", title->name);
    return SYNCJOB_SYNC_UPLOADED;
}
