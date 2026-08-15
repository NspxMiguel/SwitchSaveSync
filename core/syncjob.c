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

// Todos os saves deste jogo, de todas as contas.
//
// Existe porque o application_id sozinho NÃO diz de quem é o save, e quem
// chama — o sysmodule — só sabe o application_id do jogo que fechou. Pegar só
// o primeiro da lista era pior do que parece: com duas contas jogando o mesmo
// jogo, a ordem entre as duas é a que o fsSaveDataInfoReader devolveu, não a
// de quem acabou de jogar. Com duas contas do mesmo console, quem fechava o
// jogo era uma e o save que subia era o da outra, sem mudança nenhuma, e a
// tela dizia "nuvem OK".
size_t syncjob_find_all_titles(u64 application_id, TitleEntry *out, size_t max)
{
    // Estático: são ~70 KB de TitleEntry, e no sysmodule a heap é apertada.
    // Não é reentrante, mas só existe uma thread chamando isso.
    static TitleEntry entries[MAX_TITLES];

    size_t n       = titles_list_with_savedata(entries, MAX_TITLES);
    size_t achados = 0;

    for (size_t i = 0; i < n && achados < max; i++)
        if (entries[i].application_id == application_id)
            out[achados++] = entries[i];

    return achados;
}

bool syncjob_find_title(u64 application_id, TitleEntry *out)
{
    return syncjob_find_all_titles(application_id, out, 1) == 1;
}

// O guarda de escrita. Ver o comentário no syncjob.h.
static syncjob_guard_cb g_write_guard = NULL;

void syncjob_set_write_guard(syncjob_guard_cb guard)
{
    g_write_guard = guard;
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

// ---------------------------------------------------------------------------
// O layout na nuvem: <raiz>/<Jogo>/<Dono>  — sempre os dois níveis
//
// O layout achatado espalhava o MESMO jogo por várias pastas de topo, uma por
// apelido de conta: um jogo com save de quatro contas virava quatro pastas
// "Jogo (Fulano)" lado a lado na raiz, sem nada dizendo que eram o mesmo jogo.
// Aninhado, o jogo aparece uma vez só, com o nome normal, e as contas são
// subpastas dele.
//
// A primeira tentativa só criava a subpasta quando havia mais de um save. Não
// serve: um jogo com pasta de conta e outro com arquivo solto na raiz é a
// mesma bagunça, só disfarçada. Então é sempre dois níveis, sem exceção —
// inclusive pro device save, que fica em "console".
//
// O staging e o backup do cartão continuam com o nome achatado do
// save_folder_name, de propósito: são pastas nossas e descartáveis, e mexer
// nelas junto só orfanaria backup local sem ganhar nada.
// ---------------------------------------------------------------------------

// O maior nome de dono possível: apelido da conta (0x21), "console" (7) ou
// "conta-" + 16 hexa (23). 0x41 sobra pra todos, e o sanitize nunca aumenta uma
// string — só troca caractere por caractere ou encurta.
#define CLOUD_OWNER_MAX 0x41

// Só o nome do jogo. Nunca leva apelido: quem separa um dono do outro é a
// subpasta.
static void cloud_game_folder_name(const TitleEntry *title, char *out, size_t outsz)
{
    syncstate_sanitize_name(title->name, out, outsz);
}

// O nome da subpasta do dono. SEMPRE devolve alguma coisa.
//
// A primeira versão disto só criava a subpasta quando o jogo tinha mais de um
// save, e deixava o save único solto na pasta do jogo. Não presta — do lado de
// quem abre o Drive, um jogo com pasta de conta e outro com arquivos soltos é
// a mesma bagunça de antes, só que disfarçada.
//
// A ordem de quem dá nome à pasta:
//   1. save do console (device save) — "console". Não é chute: esse save é do
//      aparelho mesmo, não tem dono.
//   2. o apelido da conta dona, quando o console soube dizer. É o caso normal.
//   3. com UM save só e sem apelido: a conta que está usando o app agora. Só
//      vale com um save porque aí não há dúvida de quem é; com dois, chutar o
//      dono seria misturar save de gente diferente.
//   4. o uid, em hexa. Feio, mas é o único que sobra, e é estável: a mesma
//      conta cai sempre na mesma pasta, hoje e daqui a seis versões. Um nome
//      inventado não teria essa propriedade, e é por isso que não invento.
static void cloud_user_folder_name(const TitleEntry *title, char *out, size_t outsz)
{
    if (title->device_save)
    {
        syncstate_sanitize_name("console", out, outsz);
        return;
    }

    if (title->account[0] != '\0')
    {
        syncstate_sanitize_name(title->account, out, outsz);
        return;
    }

    if (!title->shared_game)
    {
        char atual[0x21];
        if (titles_current_account_name(atual, sizeof(atual)))
        {
            syncstate_sanitize_name(atual, out, outsz);
            return;
        }
    }

    char cru[0x40];
    snprintf(cru, sizeof(cru), "conta-%016llX",
        (unsigned long long)title->uid.uid[0]);
    syncstate_sanitize_name(cru, out, outsz);
}

// O caminho relativo à raiz do app, com '/' entre os níveis.
static void cloud_folder_path(const TitleEntry *title, char *out, size_t outsz)
{
    char dono[CLOUD_OWNER_MAX];
    cloud_user_folder_name(title, dono, sizeof(dono));

    char rec[0x220];
    if (syncstate_recall_folder(title->application_id, title->uid, rec, sizeof(rec))
        && rec[0] != '\0' && strchr(rec, '/') != NULL)
    {
        // Só registro COM barra vale, e a razão é que agora todo save mora em
        // <Jogo>/<Dono> — registro sem barra é necessariamente do layout velho,
        // achatado, e obedecer ele recriaria a bagunça que o aninhamento veio
        // desfazer.
        //
        // O que o registro salva continua sendo o mesmo de sempre: o apelido da
        // conta pode mudar, e sem ele o app procuraria uma pasta que não existe,
        // trataria como primeira sync e deixaria o backup antigo órfão — é o
        // que acontecia ao renomear a conta depois de um backup.
        snprintf(out, outsz, "%s", rec);
        return;
    }

    char jogo[0x201];
    cloud_game_folder_name(title, jogo, sizeof(jogo));

    // Sempre dois níveis: o cloud_user_folder_name garante que 'dono' nunca sai
    // vazio, justamente pra não existir save solto na pasta do jogo.
    //
    // O dono é o que separa um save do outro, então não pode ser ELE a sobrar da
    // truncagem — se sobrasse, dois donos do mesmo jogo cairiam no mesmo
    // caminho e um escreveria por cima do save do outro. Reservo o espaço dele
    // primeiro; quem encurta, se precisar, é o nome do jogo. Mesma disciplina
    // do save_folder_name, e pelo mesmo motivo.
    size_t reservado = strlen(dono) + 2; // a barra e o \0
    size_t espaco    = (outsz > reservado) ? outsz - reservado : 0;
    snprintf(out, outsz, "%.*s/%s", (int)espaco, jogo, dono);
}

// Desce o caminho nível a nível e devolve o id da pasta que recebe o save.
//
// 'create' separa quem escreve de quem lê, e a diferença não é estilo: o
// restore NÃO pode criar. Criando, ele recebe uma pasta vazia recém-nascida em
// vez de "não tem backup", e segue em frente escrevendo o nada por cima de um
// save bom — está contado no cloud.h, já aconteceu.
//
// 'path_out' devolve o caminho usado, que é o que vai pro pastas.txt.
// Quantas subpastas tem aqui dentro, e qual é a primeira. O .nxsaves do jogo
// também mora nesse nível, por isso o filtro por is_folder: arquivo não é conta.
typedef struct {
    char   id[CLOUD_ID_MAX];
    size_t quantas;
} UnicaSubpasta;

static void conta_subpasta(const char *id, const char *name, bool is_folder, void *ud)
{
    (void)name;
    if (!is_folder)
        return;

    UnicaSubpasta *u = (UnicaSubpasta *)ud;
    if (u->quantas == 0)
        snprintf(u->id, sizeof(u->id), "%s", id);
    u->quantas++;
}

void syncjob_cloud_folder_path(const TitleEntry *title, char *out, size_t outsz)
{
    cloud_folder_path(title, out, outsz);
}

// Tem arquivo solto no primeiro nível desta pasta — sem contar os nossos?
typedef struct { bool tem_arquivo; } TemArquivoCtx;

// O arquivo por jogo (<Jogo>.nxsaves) NÃO conta.
//
// Ele mora justamente aqui, no primeiro nível da pasta do jogo, posto pelo
// syncjob_game_archive_upload — ou seja, o próprio app cria o "arquivo solto"
// que o cloud_flat_backup usa como prova de backup antigo. Sem esta exceção,
// quem guardou um jogo no arquivo único e depois pediu restore recebia o ID do
// CONTAINER, e o write_over_save limpava o savedata e escrevia lá dentro um
// .nxsaves de 29 MB mais uma pasta com nome de conta. Save do console
// destruído, com a mensagem "Save de %s veio da nuvem" na tela.
//
// Savedata de jogo nenhum contém arquivo nosso, então ignorar a extensão não
// esconde backup de verdade nenhum.
static bool eh_arquivo_nosso(const char *name)
{
    size_t n = strlen(name), e = strlen("." NXSAVES_EXT);
    return n > e && strcmp(name + n - e, "." NXSAVES_EXT) == 0;
}

static void marca_se_arquivo(const char *id, const char *name, bool is_folder, void *ud)
{
    (void)id;
    if (!is_folder && !eh_arquivo_nosso(name))
        ((TemArquivoCtx *)ud)->tem_arquivo = true;
}

// O backup do layout achatado antigo ("Jogo (Dono)", ou só "Jogo" pra save
// único), se ainda existir.
//
// Precisa de cuidado desde que a pasta do jogo virou CONTAINER: pra save único
// o save_folder_name devolve exatamente o mesmo nome que a pasta do jogo de
// hoje. Procurar cegamente por ele devolve o container, cujos filhos são as
// pastas das contas e o .nxsaves do jogo — e restaurar ISSO por cima do
// savedata escreveria uma pasta com nome de conta e um arquivo de 29 MB dentro
// do save do jogo. É o pior tipo de bug que este projeto pode ter.
//
// A distinção está no conteúdo e é confiável: backup antigo tem ARQUIVO solto
// no primeiro nível (é uma cópia de save); container do layout novo só tem
// subpasta de conta. Sem arquivo solto ali, não é backup — é container, e a
// resposta certa é "não achei".
static bool cloud_flat_backup(const char *token, const char *root_id,
                               const TitleEntry *title,
                               char *id_out, size_t outsz)
{
    char safe[0x201];
    save_folder_name(title, safe, sizeof(safe));

    char id[CLOUD_ID_MAX];
    if (!cloud_find_subfolder(token, root_id, safe, id, sizeof(id)))
        return false;

    // Sem recursão de propósito: descendo, os arquivos que moram dentro das
    // pastas de conta fariam todo container passar por backup antigo.
    TemArquivoCtx ctx = { .tem_arquivo = false };
    if (!cloud_list_children(token, id, marca_se_arquivo, &ctx) || !ctx.tem_arquivo)
        return false;

    snprintf(id_out, outsz, "%s", id);
    return true;
}

static bool cloud_title_folder(const char *token, const char *root_id,
                                const TitleEntry *title, bool create,
                                char *id_out, size_t outsz,
                                char *path_out, size_t path_sz);

typedef struct {
    const char *token;
    int         n;
} ContaArquivos;

static void soma_arquivos(const char *id, const char *name, bool is_folder, void *ud)
{
    (void)name;
    ContaArquivos *c = (ContaArquivos *)ud;
    if (is_folder)
        cloud_list_children(c->token, id, soma_arquivos, c);
    else
        c->n++;
}

int syncjob_cloud_file_count(const TitleEntry *title)
{
    char token[CLOUD_AUTH_MAX];
    if (!cloud_begin(token, sizeof(token)))
        return 0;

    char root_id[CLOUD_ID_MAX];
    if (!cloud_ensure_app_folder(token, root_id, sizeof(root_id)))
        return 0;

    // find, nunca ensure: isto é uma contagem pra desenhar barra. Criar pasta
    // aqui enche o Drive de quem só passou os olhos numa tela.
    char game_id[CLOUD_ID_MAX];
    if (!cloud_title_folder(token, root_id, title, false,
            game_id, sizeof(game_id), NULL, 0)
        && !cloud_flat_backup(token, root_id, title, game_id, sizeof(game_id)))
        return 0;

    ContaArquivos ctx = { token, 0 };
    cloud_list_children(token, game_id, soma_arquivos, &ctx);
    return ctx.n;
}

static bool cloud_title_folder(const char *token, const char *root_id,
                                const TitleEntry *title, bool create,
                                char *id_out, size_t outsz,
                                char *path_out, size_t path_sz)
{
    char caminho[0x220];
    cloud_folder_path(title, caminho, sizeof(caminho));

    if (path_out)
        snprintf(path_out, path_sz, "%s", caminho);

    // Separa o último nível (o dono) do resto: só ele tem o resgate de baixo.
    char resto[0x220];
    snprintf(resto, sizeof(resto), "%s", caminho);

    char *dono = strrchr(resto, '/');
    if (dono)
        *dono++ = '\0';
    else
    {
        // Caminho de um nível só não devia acontecer (todo save mora em
        // <Jogo>/<Dono>), mas registro velho no pastas.txt pode ter um. Trata
        // como pasta única, sem resgate.
        dono  = resto;
        resto[0] = '\0';
    }

    // Desce até a pasta do jogo. strtok_r e não strtok: o app roda o job numa
    // thread e o sysmodule na dele, e o estado interno do strtok é global.
    char pai[CLOUD_ID_MAX];
    snprintf(pai, sizeof(pai), "%s", root_id);

    char *ctx = NULL;
    for (char *seg = strtok_r(resto, "/", &ctx); seg; seg = strtok_r(NULL, "/", &ctx))
    {
        char filho[CLOUD_ID_MAX];
        bool ok = create
            ? cloud_ensure_subfolder(token, pai, seg, filho, sizeof(filho))
            : cloud_find_subfolder(token, pai, seg, filho, sizeof(filho));

        if (!ok)
            return false;

        snprintf(pai, sizeof(pai), "%s", filho);
    }

    // O nível do dono.
    char alvo[CLOUD_ID_MAX];
    bool achou = create
        ? cloud_ensure_subfolder(token, pai, dono, alvo, sizeof(alvo))
        : cloud_find_subfolder(token, pai, dono, alvo, sizeof(alvo));

    if (achou)
    {
        snprintf(id_out, outsz, "%s", alvo);
        return true;
    }

    if (create)
        return false;

    // Resgate, só na leitura: a pasta do jogo existe, mas não tem conta com
    // esse nome. Se lá dentro houver EXATAMENTE UMA conta, é ela.
    //
    // É o caso de restaurar num console onde o perfil tem outro apelido, ou de
    // ter renomeado a conta depois do backup: o save está lá, com o nome de
    // antes, e recusar por causa do nome seria esconder do dono um backup bom.
    //
    // O shared_game é a trava, e ela não é opcional. Sem ela este resgate
    // ENTREGA O SAVE DE UMA PESSOA PRA OUTRA: num console com duas contas do
    // mesmo jogo, a segunda conta a sincronizar não acha a pasta dela, encontra
    // a única que existe — a da primeira — e recebe o save do vizinho. Foi o
    // teste com duas contas do mesmo console que pegou isso.
    //
    // A distinção é exata: shared_game falso quer dizer que este console só tem
    // UM save deste jogo, e aí "a única pasta lá" só pode ser dele, com o nome
    // de antes. Verdadeiro quer dizer que existe mais de um dono, e aí nome é
    // identidade — não tem palpite honesto a dar.
    //
    // Nada é criado aqui em nenhum caso: criar na leitura devolveria pasta
    // vazia recém-nascida em vez de "não tem backup", que é como save bom vira
    // save vazio (está contado no cloud.h).
    if (title->shared_game)
        return false;

    UnicaSubpasta u = { .quantas = 0 };
    if (!cloud_list_children(token, pai, conta_subpasta, &u) || u.quantas != 1)
        return false;

    snprintf(id_out, outsz, "%s", u.id);
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
    return syncjob_backup_title_ex(title, log, NULL);
}

bool syncjob_backup_title_ex(const TitleEntry *title, syncjob_log_cb log,
                              bool *nuvem_bate)
{
    if (nuvem_bate)
        *nuvem_bate = false;

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
    bool limpou = cloud_prune_extras(token, game_id, staging);

    if (!limpou)
        say(log, TR("Subiu, mas não consegui tirar da nuvem o que o save não tem mais",
                    "Uploaded, but couldn't remove from the cloud what the save no longer has"));

    // Quem chama é que grava o marcador (o sysmodule, quando o jogo fecha), e
    // ele precisa saber disto: marcar "sincronizado" com sobra na nuvem faz o
    // sync seguinte achar a nuvem mais nova e ressuscitar dentro do savedata o
    // arquivo que o jogo apagou.
    if (nuvem_bate)
        *nuvem_bate = limpou;

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
// caso típico: jogo reinstalado, aberto uma vez, save de estreia no console
// contra o save de verdade no Drive. Na tela os dois são "um save"; em número,
// um tem alguns KB e o outro não.
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

// O backup ANTIGO só sai depois que o novo está inteiro no cartão.
//
// Antes daqui era clear_dir(dir) e só então copiar — ou seja, o backup de
// semana passada era destruído antes de existir substituto. Isso importa
// porque esta função é a rede de segurança do "Esvaziar o save deste jogo":
// o app chama ela, e só apaga o save se ela devolver true. Copiando por cima,
// uma falha no meio (cartão cheio, erro de leitura) deixava o dono sem save
// nenhum guardado — ele não perdia o save do console, mas perdia o backup que
// já tinha.
//
// O preço é ocupar as duas cópias ao mesmo tempo enquanto copia. Save de jogo
// tem alguns MB; a segurança vale muito mais que isso.
bool syncjob_backup_title_local(const TitleEntry *title, syncjob_log_cb log)
{
    char dir[0x280];
    local_backup_path(title, dir, sizeof(dir));

    char novo[sizeof(dir) + 8], velho[sizeof(dir) + 8];
    snprintf(novo,  sizeof(novo),  "%s.novo",  dir);
    snprintf(velho, sizeof(velho), "%s.velho", dir);

    syncstate_ensure_dirs();
    mkdir(SYNC_LOCAL_DIR, 0777);

    // Sobra de uma tentativa que morreu no meio da vez passada.
    clear_dir(novo);
    rmdir(novo);
    clear_dir(velho);
    rmdir(velho);
    mkdir(novo, 0777);

    say(log, TR("Copiando o save de %s pro cartão...", "Copying %s's save to the SD card..."), title->name);
    if (!savemount_mount_typed(title->application_id, title->uid, title->device_save, true))
    {
        say(log, TR("Não consegui montar o save (jogo aberto? conta errada?)", "Couldn't mount the save (game running? wrong account?)"));
        rmdir(novo);
        return false;
    }

    bool copied = savemount_copy_tree("save:/", novo);
    savemount_unmount(false);

    if (!copied)
    {
        // O backup de antes continua onde estava, inteiro.
        clear_dir(novo);
        rmdir(novo);
        say(log, TR("Falhou ao copiar pro cartão — o backup anterior continua no lugar",
                    "Failed to copy to the SD card — the previous backup is still there"));
        return false;
    }

    // A troca: o antigo sai do caminho, o novo toma o nome, e só então o antigo
    // é apagado. Em nenhum instante existe zero cópia completa no cartão.
    bool tinha_antes = (rename(dir, velho) == 0);
    if (rename(novo, dir) != 0)
    {
        if (tinha_antes)
            rename(velho, dir); // desfaz: melhor o backup velho que nenhum
        clear_dir(novo);
        rmdir(novo);
        say(log, TR("Copiei, mas não consegui pôr no lugar — o backup anterior continua valendo",
                    "Copied it, but couldn't put it in place — the previous backup still stands"));
        return false;
    }

    if (tinha_antes)
    {
        clear_dir(velho);
        rmdir(velho);
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
    // A última pergunta antes de montar pra escrita, e o único lugar do projeto
    // onde ela cabe: TODA escrita em savedata passa por aqui. Quem responde é o
    // sysmodule ("nenhum jogo vivo agora?"), porque só ele escreve com o
    // console na mão de outra pessoa. Ver syncjob_set_write_guard.
    if (g_write_guard && !g_write_guard())
    {
        say(log, TR("O jogo abriu no meio — não vou escrever no save dele",
                    "The game started in the middle — I won't write to its save"));
        return false;
    }

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

    // O commit é o único momento em que o que foi escrito vira save de verdade,
    // e ele pode falhar sozinho (cartão cheio, journal do save data estourado).
    // Enquanto o retorno era jogado fora, isso saía como "restaurado com
    // sucesso" e o console continuava com o save antigo — quem fosse jogar
    // perdia o progresso outra vez, sem nada na tela dizendo por quê.
    bool commitou = savemount_unmount(copied); // commit só se a cópia deu certo

    if (!copied)
        say(log, TR("Falhou ao gravar o save", "Failed to write the save"));
    else if (!commitou)
        say(log, TR("Gravei, mas o console recusou salvar de vez — o save NAO mudou",
                    "Wrote it, but the console refused to commit — the save did NOT change"));
    return copied && commitou;
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
    // que é onde mora o backup de quem sincronizou antes do layout aninhado.
    // Nada é migrado: só continua sendo lido, o que não custa nada e evita que
    // backup bom vire inalcançável de uma versão pra outra.
    char game_id[CLOUD_ID_MAX];
    char pasta_nuvem[0x220];

    if (!cloud_title_folder(token, root_id, title, false,
            game_id, sizeof(game_id), pasta_nuvem, sizeof(pasta_nuvem))
        && !cloud_flat_backup(token, root_id, title, game_id, sizeof(game_id)))
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

    // Já é a mesma coisa? Então não escreve.
    //
    // Não é economia de tempo, é segurança: escrever significa montar pra
    // escrita, APAGAR o savedata inteiro e regravar. Fazer isso pra deixar o
    // save exatamente como já estava é abrir a janela de corrupção por nada — e
    // na varredura ociosa do sysmodule isso acontecia a cada meia hora, em cada
    // jogo, a noite toda, sem ninguém na frente do console.
    //
    // A comparação é a mesma do sync de um clique (nome + tamanho + conteúdo,
    // sem mtime): o que veio do Drive nasce com data de agora, então data aqui
    // só produziria "diferente" pra sempre.
    u64 fp_nuvem = 0, fp_local = 0;
    fingerprint_dir(staging, &fp_nuvem);

    if (syncjob_fingerprint(title, &fp_local) && fp_local == fp_nuvem)
    {
        syncjob_mark_synced(title, fp_local);
        say(log, TR("O save daqui já é igual ao da nuvem — não mexi em nada",
                    "This save is already the same as the cloud's — nothing was touched"));
        return true;
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
// A ideia do app é um clique só: entra como se fosse um jogo, manda sincronizar
// o save com a nuvem e pronto. Por isso ele não pergunta pra que lado vai —
// esta função decide.
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
// progresso de um dos lados, então quem escolhe é o usuário, pelo menu do Y.
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
    if (!tem_na_nuvem && cloud_flat_backup(token, root_id, title, game_id, sizeof(game_id)))
    {
        // A registrada sumiu e a calculada existe: o registro envelheceu (a
        // pasta foi apagada na nuvem pelo navegador, por exemplo). Vale a que
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
        // cloud_title_folder e NAO cloud_ensure_subfolder: o pasta_nuvem tem
        // dois niveis ("Jogo/Dono") e o ensure_subfolder cria UM nome so. No
        // Drive isso criava, na raiz, uma pasta cujo NOME continha uma barra —
        // o upload dizia que deu certo e nenhum leitor achava aquilo nunca
        // mais. No WebDAV era MKCOL com pai inexistente: 409, e a primeira
        // sync de todo jogo novo falhava.
        if (!cloud_title_folder(token, root_id, title, true,
                game_id, sizeof(game_id), pasta_nuvem, sizeof(pasta_nuvem)) ||
            !cloud_upload_tree(token, game_id, console_dir))
        {
            say(log, TR("Upload falhou", "Upload failed"));
            return SYNCJOB_SYNC_FAILED;
        }
        lembra_pasta(title, pasta_nuvem);
        bool limpou = cloud_prune_extras(token, game_id, console_dir);

        // O marcador só vale se o prune deu certo, e isso não é preciosismo.
        //
        // O marcador quer dizer "os dois lados estavam iguais neste ponto". Se
        // o prune falhou — o Drive recusou o DELETE, o WebDAV devolveu 423, ou
        // alguém apertou B no meio —, a nuvem ficou com um arquivo que o jogo
        // apagou, e os dois lados NÃO estão iguais. Gravar o marcador assim
        // mente pro sync seguinte: ele vê o console parado e a nuvem "mais
        // nova", desce por cima do savedata e RESSUSCITA o arquivo apagado
        // dentro do save. É o save metade de ontem, metade de hoje que o
        // write_over_save descreve.
        //
        // Sem marcador, o próximo sync não decide sozinho: pergunta. Um
        // diálogo a mais é barato; save remendado não.
        if (limpou)
            syncjob_mark_synced(title, local_fp);
        else
            say(log, TR("Subiu, mas não consegui tirar da nuvem o que o save não tem mais",
                        "Uploaded, but couldn't remove from the cloud what the save no longer has"));

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
    bool limpou = cloud_prune_extras(token, game_id, console_dir);

    // O marcador só vale se o prune deu certo, e isso não é preciosismo.
    //
    // O marcador quer dizer "os dois lados estavam iguais neste ponto". Se o
    // prune falhou — o Drive recusou o DELETE, o WebDAV devolveu 423, ou
    // alguém apertou B no meio —, a nuvem ficou com um arquivo que o jogo
    // apagou e os dois lados NÃO estão iguais. Gravar o marcador assim mente
    // pro sync seguinte: ele vê o console parado e a nuvem "mais nova", desce
    // por cima do savedata e RESSUSCITA o arquivo apagado dentro do save. É o
    // save metade de ontem, metade de hoje que o write_over_save descreve.
    //
    // Sem marcador, o próximo sync não decide sozinho: pergunta. Um diálogo a
    // mais é barato; save remendado não.
    if (limpou)
        syncjob_mark_synced(title, local_fp);
    else
        say(log, TR("Subiu, mas não consegui tirar da nuvem o que o save não tem mais",
                    "Uploaded, but couldn't remove from the cloud what the save no longer has"));

    say(log, TR("Save de %s subiu pra nuvem", "%s's save went up to the cloud"), title->name);
    return SYNCJOB_SYNC_UPLOADED;
}

// ---------------------------------------------------------------------------
// Tudo num arquivo só
//
// Todos os saves num arquivo proprietário, que só este app lê. O formato está
// em nxsaves.c. Aqui é só a costura: montar save, empurrar pro arquivo, e a
// volta.
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
    // renomear depois abre uma janela em que uma falha deixa o cartão sem o
    // arquivo novo E sem o velho — e o velho é o backup de tudo.
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
// Antes ele ia pra raiz, junto com o global: o arquivo existia e estava certo
// — só que largado num lugar onde não dá pra saber de que jogo é só de olhar.
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
