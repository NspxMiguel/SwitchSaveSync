#include "syncjob.h"

#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "config.h"
#include "cloud.h"
#include "lang.h"
#include "savemount.h"
#include "nxsaves.h"
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

// O nome de quem está guardando: "Google Drive", "Servidor WebDAV...".
// Existe porque as mensagens diziam "Drive" na mão, e agora nem sempre é.
static const char *nuvem(void)
{
    return cloud_name(cloud_current());
}

// O nome da pasta desse save — na nuvem, no staging e no backup do cartão.
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

void syncjob_save_folder_name(const TitleEntry *title, char *out, size_t outsz)
{
    save_folder_name(title, out, outsz);
}

// O nome da pasta NA NUVEM — que nem sempre é o nome calculado acima.
//
// Ver o syncstate.h: o apelido da conta entra no nome, e o console deixa trocar
// o apelido quando quiser. Se já existe registro de qual pasta este save usou,
// é ela que vale. Sem isso, trocar o apelido faz o app procurar uma pasta que
// não existe, tratar como primeira sync, e o backup antigo fica órfão na nuvem
// — que é exatamente o "mudei d nome e foi nao" que ele relatou.
//
// O staging e o backup do cartão continuam com o nome calculado, de propósito:
// são pastas nossas e descartáveis, e renomear elas não perde nada.
// ---------------------------------------------------------------------------
// O layout na nuvem: <raiz>/<Jogo>[/<Dono>]
//
// conversa privada removida do historico
// Rayman Legends ("(Player 1)", "(Convidado)", "(Convidado)", "(Convidado)"): "Vai ter o
// jogo com o nome normal. ai dentro vai ter uma subpasta com os usuarios CASO,
// tenha mais usuarios, se nao taca direto nessa pasta."
//
// O apelido saiu do nome da pasta e virou subpasta. Quem tem save único não
// ganha nível nenhum a mais — a pasta do jogo continua sendo o destino, e o
// nome dela é o mesmo de antes, então backup antigo de jogo de save único
// continua sendo achado sem migrar nada.
// ---------------------------------------------------------------------------

// Só o nome do jogo. Nunca leva apelido: quem separa um dono do outro é a
// subpasta.
static void cloud_game_folder_name(const TitleEntry *title, char *out, size_t outsz)
{
    syncstate_sanitize_name(title->name, out, outsz);
}

// O nome da subpasta do dono, ou vazio quando não há o que separar. Mesma
// condição do save_folder_name: sem apelido vindo do console, não inventa nome
// (nome chutado hoje vira pasta órfã na próxima versão).
static void cloud_user_folder_name(const TitleEntry *title, char *out, size_t outsz)
{
    if (!title->shared_game || (!title->device_save && title->account[0] == '\0'))
    {
        out[0] = '\0';
        return;
    }

    if (title->device_save)
        syncstate_sanitize_name("console", out, outsz);
    else
        syncstate_sanitize_name(title->account, out, outsz);
}

// O caminho relativo à raiz do app, com '/' entre os níveis.
static void cloud_folder_path(const TitleEntry *title, char *out, size_t outsz)
{
    char dono[0x201];
    cloud_user_folder_name(title, dono, sizeof(dono));

    char rec[0x220];
    if (syncstate_recall_folder(title->application_id, title->uid, rec, sizeof(rec))
        && rec[0] != '\0')
    {
        // Registro com barra é do layout novo: vale. Registro sem barra pode
        // ser das duas eras — e só continua valendo pra save único, onde o nome
        // gravado é igualzinho ao nome da pasta do jogo de hoje.
        //
        // Pra jogo com mais de um dono, registro sem barra é do layout velho
        // ("Rayman Legends_ Definitive Edition (Player 1)"): obedecer ele
        // recriaria exatamente a bagunça que mandaram desfazer.
        if (strchr(rec, '/') != NULL || dono[0] == '\0')
        {
            snprintf(out, outsz, "%s", rec);
            return;
        }
    }

    char jogo[0x201];
    cloud_game_folder_name(title, jogo, sizeof(jogo));

    if (dono[0] != '\0')
        snprintf(out, outsz, "%s/%s", jogo, dono);
    else
        snprintf(out, outsz, "%s", jogo);
}

// Desce o caminho nível a nível e devolve o id da pasta que recebe o save.
//
// 'create' separa quem escreve de quem lê, e a diferença não é estilo: o
// restore NÃO pode criar. Criando, ele recebe uma pasta vazia recém-nascida em
// vez de "não tem backup", e segue em frente escrevendo o nada por cima de um
// save bom — está contado no cloud.h, já aconteceu.
//
// 'path_out' devolve o caminho usado, que é o que vai pro pastas.txt.
static bool cloud_title_folder(const char *token, const char *root_id,
                                const TitleEntry *title, bool create,
                                char *id_out, size_t outsz,
                                char *path_out, size_t path_sz)
{
    char caminho[0x220];
    cloud_folder_path(title, caminho, sizeof(caminho));

    if (path_out)
        snprintf(path_out, path_sz, "%s", caminho);

    char atual[CLOUD_ID_MAX];
    snprintf(atual, sizeof(atual), "%s", root_id);

    char resto[0x220];
    snprintf(resto, sizeof(resto), "%s", caminho);

    // strtok_r e não strtok: o app roda o job numa thread e o sysmodule na
    // dele, e o estado interno do strtok é global.
    char *ctx = NULL;
    for (char *seg = strtok_r(resto, "/", &ctx); seg; seg = strtok_r(NULL, "/", &ctx))
    {
        char filho[CLOUD_ID_MAX];
        bool ok = create
            ? cloud_ensure_subfolder(token, atual, seg, filho, sizeof(filho))
            : cloud_find_subfolder(token, atual, seg, filho, sizeof(filho));

        if (!ok)
            return false;

        snprintf(atual, sizeof(atual), "%s", filho);
    }

    snprintf(id_out, outsz, "%s", atual);
    return true;
}

// Gravar só depois que a pasta existe de verdade na nuvem. Guardar antes
// deixaria registro apontando pra pasta que nunca nasceu, e aí o app procuraria
// eternamente por ela em vez de criar a certa.
static void lembra_pasta(const TitleEntry *title, const char *pasta)
{
    syncstate_remember_folder(title->application_id, title->uid, pasta);
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
    char token[CLOUD_AUTH_MAX];
    if (!cloud_begin(token, sizeof(token)))
    {
        say(log, TR("Sem token válido — precisa entrar na conta pelo app", "No valid token — sign in from the app first"));
        return false;
    }

    char root_id[CLOUD_ID_MAX];
    if (!cloud_ensure_app_folder(token, root_id, sizeof(root_id)))
    {
        say(log, TR("Não achei/criei a pasta \"%s\" em %s", "Couldn't find/create the \"%s\" folder on %s"), DRIVE_APP_FOLDER_NAME, nuvem());
        return false;
    }

    char pasta_nuvem[0x220];
    char game_id[CLOUD_ID_MAX];
    if (!cloud_title_folder(token, root_id, title, true,
            game_id, sizeof(game_id), pasta_nuvem, sizeof(pasta_nuvem)))
    {
        say(log, TR("Não criei a pasta do jogo em %s", "Couldn't create the game's folder on %s"), nuvem());
        return false;
    }
    lembra_pasta(title, pasta_nuvem);

    say(log, TR("Subindo pro %s...", "Uploading to %s..."), nuvem());
    if (!cloud_upload_tree(token, game_id, staging))
    {
        say(log, TR("Upload falhou", "Upload failed"));
        return false;
    }

    // Subir só escreve. O que o save não tem mais precisa sair da nuvem, senão
    // volta no próximo restore. No Drive vai pra lixeira e não some; no
    // WebDAV depende do servidor ter lixeira — ver o aviso no cloud.h.
    cloud_prune_extras(token, game_id, staging);

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
    char token[CLOUD_AUTH_MAX];
    if (!cloud_begin(token, sizeof(token)))
    {
        say(log, TR("Sem token válido — precisa entrar na conta pelo app", "No valid token — sign in from the app first"));
        return false;
    }

    char root_id[CLOUD_ID_MAX];
    if (!cloud_ensure_app_folder(token, root_id, sizeof(root_id)))
    {
        say(log, TR("Não achei a pasta \"%s\" em %s", "Couldn't find the \"%s\" folder on %s"), DRIVE_APP_FOLDER_NAME, nuvem());
        return false;
    }

    // find, e não ensure: "ensure" CRIA a pasta quando não acha, e foi assim
    // que um jogo sem backup na nuvem virou "restore de pasta vazia" que ainda
    // assim montava o save e commitava. Quem lê da nuvem nunca cria nada.
    //
    // Duas tentativas: a pasta registrada e a calculada. Aqui pode ser
    // generoso porque ninguém cria nada — o pior caso é não achar. E cobre os
    // dois lados: quem renomeou a conta depois do backup (vale a registrada) e
    // quem apagou o pastas.txt ou trouxe o cartão de outro console (vale a
    // calculada, que é o que sempre valeu).
    // A terceira tentativa é o layout achatado antigo ("Jogo (Dono)" na raiz),
    // que é onde mora o backup de quem sincronizou antes. Nada é
    // migrado: só continua sendo lido, o que não custa nada e evita que backup
    // bom vire inalcançável de uma versão pra outra.
    char game_id[CLOUD_ID_MAX];
    char pasta_nuvem[0x220];

    if (!cloud_title_folder(token, root_id, title, false,
            game_id, sizeof(game_id), pasta_nuvem, sizeof(pasta_nuvem))
        && (strcmp(pasta_nuvem, safe) == 0
            || !cloud_find_subfolder(token, root_id, safe, game_id, sizeof(game_id))))
    {
        say(log, TR("Esse jogo não tem backup em %s", "This game has no backup on %s"), nuvem());
        return false;
    }

    say(log, TR("Baixando da nuvem...", "Downloading from the cloud..."));
    clear_dir(staging); // ver clear_dir(): sobra de download antigo ia junto
    if (!cloud_download_tree(token, game_id, staging))
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
    char token[CLOUD_AUTH_MAX];
    if (!cloud_begin(token, sizeof(token)))
    {
        say(log, TR("Sem token válido — precisa entrar na conta pelo app", "No valid token — sign in from the app first"));
        return SYNCJOB_SYNC_FAILED;
    }

    char root_id[CLOUD_ID_MAX];
    if (!cloud_ensure_app_folder(token, root_id, sizeof(root_id)))
    {
        say(log, TR("Não achei/criei a pasta \"%s\" em %s", "Couldn't find/create the \"%s\" folder on %s"), DRIVE_APP_FOLDER_NAME, nuvem());
        return SYNCJOB_SYNC_FAILED;
    }

    // find, não ensure: procurar não pode criar pasta. Ver syncjob_restore_title.
    //
    // Aqui é o lugar mais caro de errar o nome da pasta: não achar leva direto
    // pro ramo "primeira sync desse jogo", que sobe o save de agora numa pasta
    // nova e deixa o backup antigo órfão. Por isso as duas tentativas, igual ao
    // restore — a pasta registrada e a calculada.
    char game_id[CLOUD_ID_MAX];
    char pasta_nuvem[0x220];

    bool tem_na_nuvem = cloud_title_folder(token, root_id, title, false,
        game_id, sizeof(game_id), pasta_nuvem, sizeof(pasta_nuvem));
    if (!tem_na_nuvem && strcmp(pasta_nuvem, safe) != 0
        && cloud_find_subfolder(token, root_id, safe, game_id, sizeof(game_id)))
    {
        // A registrada sumiu e a calculada existe: o registro envelheceu (ele
        // apagou a pasta na nuvem pelo navegador, por exemplo). Vale a que
        // existe, e o registro se acerta logo abaixo.
        snprintf(pasta_nuvem, sizeof(pasta_nuvem), "%s", safe);
        tem_na_nuvem = true;
    }
    if (tem_na_nuvem)
        lembra_pasta(title, pasta_nuvem);

    u64 cloud_fp = 0;
    bool cloud_vazio = true;

    if (tem_na_nuvem)
    {
        say(log, TR("Baixando o save da nuvem pra comparar...", "Downloading the cloud save to compare..."));
        clear_dir(cloud_dir);
        if (!cloud_download_tree(token, game_id, cloud_dir))
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
        if (!cloud_ensure_subfolder(token, root_id, pasta_nuvem, game_id, sizeof(game_id)) ||
            !cloud_upload_tree(token, game_id, console_dir))
        {
            say(log, TR("Upload falhou", "Upload failed"));
            return SYNCJOB_SYNC_FAILED;
        }
        lembra_pasta(title, pasta_nuvem);
        cloud_prune_extras(token, game_id, console_dir);
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
    if (!cloud_upload_tree(token, game_id, console_dir))
    {
        say(log, TR("Upload falhou", "Upload failed"));
        return SYNCJOB_SYNC_FAILED;
    }
    cloud_prune_extras(token, game_id, console_dir);
    syncjob_mark_synced(title, local_fp);
    say(log, TR("Save de %s subiu pra nuvem", "%s's save went up to the cloud"), title->name);
    return SYNCJOB_SYNC_UPLOADED;
}

// ---------------------------------------------------------------------------
// Tudo num arquivo só
//
// conversa privada removida do historico
// O formato está em nxsaves.c. Aqui é só a costura: montar save, empurrar pro
// arquivo, e a volta.
// ---------------------------------------------------------------------------

void syncjob_archive_path(char *out, size_t outsz)
{
    snprintf(out, outsz, "%s/%s", SYNC_APP_DIR, SYNC_ARCHIVE_NAME);
}

bool syncjob_has_archive(void)
{
    char path[0x300];
    syncjob_archive_path(path, sizeof(path));
    return nxsaves_is_box(path);
}

void syncjob_game_archive_path(const TitleEntry *title, char *out, size_t outsz)
{
    // O nome do JOGO, sem sufixo de conta: o arquivo junta as contas todas, e
    // pôr o apelido de uma delas no nome seria mentir sobre o conteúdo.
    char limpo[0x201];
    syncstate_sanitize_name(title->name, limpo, sizeof(limpo));
    snprintf(out, outsz, "%s/%s." NXSAVES_EXT, SYNC_APP_DIR, limpo);
}

size_t syncjob_archive_titles(const TitleEntry *titles, size_t count,
                               syncjob_log_cb log, syncjob_stop_cb stop)
{
    char final[0x300];
    syncjob_archive_path(final, sizeof(final));
    return syncjob_archive_titles_to(final, titles, count, log, stop);
}

size_t syncjob_archive_titles_to(const char *path, const TitleEntry *titles, size_t count,
                                  syncjob_log_cb log, syncjob_stop_cb stop)
{
    syncstate_ensure_dirs();

    char final[0x300], temp[0x310];
    snprintf(final, sizeof(final), "%s", path);
    snprintf(temp, sizeof(temp), "%s.parcial", final);

    NxSavesWriter *box = nxsaves_open_write(temp);
    if (!box)
    {
        say(log, TR("Não consegui criar o arquivo no cartão", "Couldn't create the file on the SD card"));
        return 0;
    }

    size_t entraram = 0;

    for (size_t i = 0; i < count; i++)
    {
        // Só entre um jogo e outro: parar no meio de um save deixaria o
        // arquivo com meio jogo dentro, e é isso que o abort abaixo evita.
        if (stop && stop())
        {
            say(log, TR("Parado a pedido — o arquivo não foi gravado", "Stopped on request — the file wasn't written"));
            nxsaves_abort_write(box);
            return 0;
        }

        const TitleEntry *t = &titles[i];

        char pasta[0x201];
        save_folder_name(t, pasta, sizeof(pasta));

        // Dois saves com o MESMO nome de pasta é raro (mesmo jogo, duas contas,
        // e o apelido de uma delas não veio do console), mas aqui não pode
        // passar: o segundo entraria com o nome do primeiro, e na hora de tirar
        // de volta sairia o save da conta errada. Fora do arquivo isso já é uma
        // limitação conhecida do nome de pasta; dentro dele, pelo menos, não
        // vai existir nome repetido.
        bool repetido = false;
        for (size_t j = 0; j < i && !repetido; j++)
        {
            char outra[0x201];
            save_folder_name(&titles[j], outra, sizeof(outra));
            repetido = (strcmp(outra, pasta) == 0);
        }
        if (repetido)
        {
            say(log, TR("%s: já tem outro save com esse mesmo nome — pulado",
                        "%s: another save already uses this same name — skipped"), t->name);
            continue;
        }

        char prefixo[0x210];
        snprintf(prefixo, sizeof(prefixo), "%s/", pasta);

        say(log, TR("Montando o save de %s...", "Mounting %s's save..."), t->name);
        if (!savemount_mount_typed(t->application_id, t->uid, t->device_save, true))
        {
            // Jogo aberto é o caso comum aqui, e não é motivo pra jogar fora o
            // trabalho dos outros. Pula e segue.
            say(log, TR("%s: não deu pra montar (jogo aberto?) — pulado", "%s: couldn't mount (game running?) — skipped"), t->name);
            continue;
        }

        // Lê DIRETO do save montado pro arquivo. Sem staging: o container é o
        // único lugar onde esses bytes pousam.
        size_t antes = nxsaves_entry_count(box);
        bool ok      = nxsaves_add_dir(box, prefixo, "save:/");
        savemount_unmount(false);

        if (!ok)
        {
            say(log, TR("Falhou ao guardar o save de %s", "Failed to store %s's save"), t->name);
            nxsaves_abort_write(box);
            return 0;
        }

        // Save montado e vazio existe (jogo instalado que nunca foi aberto).
        // Contar isso como "guardado" faria o resumo prometer um save que não
        // está lá dentro.
        if (nxsaves_entry_count(box) == antes)
        {
            say(log, TR("%s: o save está vazio — não tem o que guardar",
                        "%s: the save is empty — there's nothing to store"), t->name);
            continue;
        }

        entraram++;
        say(log, TR("%s: guardado", "%s: stored"), t->name);
    }

    if (entraram == 0)
    {
        say(log, TR("Nenhum save entrou — nada foi gravado", "No save made it in — nothing was written"));
        nxsaves_abort_write(box);
        return 0;
    }

    size_t arquivos = nxsaves_entry_count(box);

    if (!nxsaves_close_write(box))
    {
        say(log, TR("Não consegui fechar o arquivo (cartão cheio?)", "Couldn't close the file (SD card full?)"));
        return 0;
    }

    // A troca só acontece agora: até aqui, o arquivo de antes continuava
    // inteiro no lugar dele.
    //
    // O de antes sai de cena por rename, não por remove. Apagar primeiro e
    // renomear depois abre uma janela em que uma falha deixa ele sem o arquivo
    // novo E sem o velho — e o velho é o backup de tudo.
    char guardado[0x320];
    snprintf(guardado, sizeof(guardado), "%s.anterior", final);
    remove(guardado);
    bool tinha_antes = (rename(final, guardado) == 0);

    if (rename(temp, final) != 0)
    {
        say(log, TR("Não consegui pôr o arquivo no lugar", "Couldn't move the file into place"));
        if (tinha_antes)
        {
            rename(guardado, final);
            say(log, TR("O arquivo que já existia continua onde estava",
                        "The file that was already there is still where it was"));
        }
        remove(temp);
        return 0;
    }

    remove(guardado);

    const char *so_o_nome = strrchr(final, '/');
    say(log, TR("%zu saves, %zu arquivos, tudo em %s", "%zu saves, %zu files, all in %s"),
        entraram, arquivos, so_o_nome ? so_o_nome + 1 : final);
    return entraram;
}

bool syncjob_archive_upload(syncjob_log_cb log)
{
    char path[0x300];
    syncjob_archive_path(path, sizeof(path));
    return syncjob_archive_upload_path(path, log);
}

// O arquivo de UM jogo vai pra dentro da pasta daquele jogo, não pra raiz.
//
// conversa privada removida do historico
// conversa privada removida do historico
// raiz junto com o global, onde não dá pra saber de quem é olhando.
//
// O global (SwitchSaveSync.nxsaves, todos os jogos) continua na raiz, que é o
// lugar dele: não é de jogo nenhum em particular.
bool syncjob_game_archive_upload(const TitleEntry *title, const char *path,
                                  syncjob_log_cb log)
{
    const char *nome_remoto = strrchr(path, '/');
    nome_remoto = nome_remoto ? nome_remoto + 1 : path;

    if (!nxsaves_is_box(path))
    {
        say(log, TR("Não tem arquivo único no cartão pra subir", "There's no single file on the SD card to upload"));
        return false;
    }

    char token[CLOUD_AUTH_MAX];
    if (!cloud_begin(token, sizeof(token)))
    {
        say(log, TR("Sem token válido — precisa entrar na conta pelo app", "No valid token — sign in from the app first"));
        return false;
    }

    char root_id[CLOUD_ID_MAX];
    if (!cloud_ensure_app_folder(token, root_id, sizeof(root_id)))
    {
        say(log, TR("Não achei/criei a pasta \"%s\" em %s", "Couldn't find/create the \"%s\" folder on %s"), DRIVE_APP_FOLDER_NAME, nuvem());
        return false;
    }

    // A pasta do JOGO, sem descer pro dono: o arquivo já tem todos os donos
    // dentro dele, então pendurar num deles seria mentira.
    char jogo[0x201];
    cloud_game_folder_name(title, jogo, sizeof(jogo));

    char game_id[CLOUD_ID_MAX];
    if (!cloud_ensure_subfolder(token, root_id, jogo, game_id, sizeof(game_id)))
    {
        say(log, TR("Não criei a pasta do jogo em %s", "Couldn't create the game's folder on %s"), nuvem());
        return false;
    }

    say(log, TR("Subindo o arquivo pro %s...", "Uploading the file to %s..."), nuvem());
    if (!cloud_upload(token, game_id, nome_remoto, path, "application/octet-stream"))
    {
        say(log, TR("Upload falhou", "Upload failed"));
        return false;
    }

    say(log, TR("%s está em %s/%s", "%s is in %s/%s"), nome_remoto, DRIVE_APP_FOLDER_NAME, jogo);
    return true;
}

bool syncjob_archive_upload_path(const char *path, syncjob_log_cb log)
{
    // No Drive ele fica com o mesmo nome que tem no cartão.
    const char *nome_remoto = strrchr(path, '/');
    nome_remoto = nome_remoto ? nome_remoto + 1 : path;

    if (!nxsaves_is_box(path))
    {
        say(log, TR("Não tem arquivo único no cartão pra subir", "There's no single file on the SD card to upload"));
        return false;
    }

    char token[CLOUD_AUTH_MAX];
    if (!cloud_begin(token, sizeof(token)))
    {
        say(log, TR("Sem token válido — precisa entrar na conta pelo app", "No valid token — sign in from the app first"));
        return false;
    }

    char root_id[CLOUD_ID_MAX];
    if (!cloud_ensure_app_folder(token, root_id, sizeof(root_id)))
    {
        say(log, TR("Não achei/criei a pasta \"%s\" em %s", "Couldn't find/create the \"%s\" folder on %s"), DRIVE_APP_FOLDER_NAME, nuvem());
        return false;
    }

    say(log, TR("Subindo o arquivo pro %s...", "Uploading the file to %s..."), nuvem());
    if (!cloud_upload(token, root_id, nome_remoto, path, "application/octet-stream"))
    {
        say(log, TR("Upload falhou", "Upload failed"));
        return false;
    }

    say(log, TR("%s está no seu %s", "%s is on your %s"), nome_remoto, nuvem());
    return true;
}

bool syncjob_archive_download(syncjob_log_cb log)
{
    syncstate_ensure_dirs();

    char final[0x300], temp[0x310];
    syncjob_archive_path(final, sizeof(final));
    snprintf(temp, sizeof(temp), "%s.baixando", final);

    char token[CLOUD_AUTH_MAX];
    if (!cloud_begin(token, sizeof(token)))
    {
        say(log, TR("Sem token válido — precisa entrar na conta pelo app", "No valid token — sign in from the app first"));
        return false;
    }

    char root_id[CLOUD_ID_MAX];
    if (!cloud_ensure_app_folder(token, root_id, sizeof(root_id)))
    {
        say(log, TR("Não achei a pasta \"%s\" em %s", "Couldn't find the \"%s\" folder on %s"), DRIVE_APP_FOLDER_NAME, nuvem());
        return false;
    }

    say(log, TR("Baixando o arquivo do %s...", "Downloading the file from %s..."), nuvem());
    if (!cloud_download(token, root_id, SYNC_ARCHIVE_NAME, temp))
    {
        say(log, TR("Não tem %s no seu %s", "There's no %s on your %s"), SYNC_ARCHIVE_NAME, nuvem());
        remove(temp);
        return false;
    }

    // Baixou não quer dizer prestável. Conferir antes de trocar evita que um
    // download pela metade apague o arquivo bom que estava no cartão.
    if (!nxsaves_is_box(temp))
    {
        say(log, TR("O que veio da nuvem não é um arquivo nosso", "What came from the cloud isn't one of our files"));
        remove(temp);
        return false;
    }

    // Mesma dança do archive_titles, pelo mesmo motivo: o arquivo de antes só
    // some depois que o novo estiver no lugar.
    char guardado[0x320];
    snprintf(guardado, sizeof(guardado), "%s.anterior", final);
    remove(guardado);
    bool tinha_antes = (rename(final, guardado) == 0);

    if (rename(temp, final) != 0)
    {
        say(log, TR("Não consegui pôr o arquivo no lugar", "Couldn't move the file into place"));
        if (tinha_antes)
            rename(guardado, final);
        remove(temp);
        return false;
    }

    remove(guardado);

    say(log, TR("%s está no cartão", "%s is on the SD card"), SYNC_ARCHIVE_NAME);
    return true;
}

typedef struct
{
    syncjob_archive_cb cb;
    void *userdata;
} ArquivoCtx;

static void um_jogo_do_arquivo(const char *nome, uint64_t bytes, void *userdata)
{
    ArquivoCtx *c = userdata;
    c->cb(nome, bytes, c->userdata);
}

bool syncjob_archive_list(syncjob_archive_cb cb, void *userdata)
{
    char path[0x300];
    syncjob_archive_path(path, sizeof(path));
    return syncjob_archive_list_path(path, cb, userdata);
}

bool syncjob_archive_list_path(const char *path, syncjob_archive_cb cb, void *userdata)
{
    ArquivoCtx c = { cb, userdata };
    return nxsaves_list_folders(path, um_jogo_do_arquivo, &c);
}

bool syncjob_archive_restore_title(const TitleEntry *title, syncjob_log_cb log)
{
    char path[0x300];
    syncjob_archive_path(path, sizeof(path));

    char pasta[0x201];
    save_folder_name(title, pasta, sizeof(pasta));

    return syncjob_archive_restore_folder(path, pasta, title, log);
}

bool syncjob_archive_restore_folder(const char *path, const char *folder,
                                     const TitleEntry *dest, syncjob_log_cb log)
{
    if (!nxsaves_is_box(path))
    {
        say(log, TR("Não tem arquivo único no cartão", "There's no single file on the SD card"));
        return false;
    }

    char pasta[0x201];
    snprintf(pasta, sizeof(pasta), "%s", folder);

    char prefixo[0x210];
    snprintf(prefixo, sizeof(prefixo), "%s/", pasta);

    // Sai do container pro staging, e só do staging pro save. Escrever direto
    // no save montado economizaria uma cópia, mas aí um arquivo corrompido no
    // meio do container deixaria o save metade novo, metade velho — e é assim
    // que um jogo abre dizendo que os dados estão corrompidos.
    char staging[0x280];
    snprintf(staging, sizeof(staging), "%s/%s", SYNC_STAGING_DIR, pasta);

    syncstate_ensure_dirs();
    mkdir(SYNC_STAGING_DIR, 0777);
    mkdir(staging, 0777);
    clear_dir(staging);

    say(log, TR("Tirando %s de dentro do arquivo...", "Pulling %s out of the file..."), pasta);
    if (!nxsaves_extract(path, prefixo, staging))
    {
        say(log, TR("Esse save não está dentro do arquivo (ou o arquivo está corrompido)", "That save isn't inside the file (or the file is corrupted)"));
        return false;
    }

    say(log, TR("Gravando no save do console...", "Writing to the console's save..."));
    if (!write_over_save(dest, staging, log))
        return false;

    say(log, TR("Save de %s restaurado do arquivo", "%s's save restored from the file"), dest->name);
    return true;
}
