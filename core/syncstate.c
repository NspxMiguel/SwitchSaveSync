#include "syncstate.h"

#include <stdbool.h>

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define LOG_MAX_BYTES  (64 * 1024)
#define LOG_KEEP_BYTES (32 * 1024)   // o que sobra depois do corte: a metade NOVA

void syncstate_ensure_dirs(void)
{
    mkdir("sdmc:/switch", 0777);
    mkdir(SYNC_APP_DIR, 0777);
}

// ---------------------------------------------------------------- config

static bool read_first_line(const char *path, char *out, size_t outsz)
{
    FILE *f = fopen(path, "r");
    if (!f)
        return false;
    bool ok = fgets(out, (int)outsz, f) != NULL;
    fclose(f);
    if (!ok)
        return false;
    out[strcspn(out, "\r\n")] = '\0';
    return true;
}

bool syncstate_autosync_enabled(void)
{
    // Sem arquivo = DESLIGADO. Era o contrário, e o contrário está errado:
    // "não configurei nada ainda" virava "pode mexer sozinho nos saves".
    // Escrever em save é irreversível; ficar quieto não é. Quem liga é quem
    // usa, no overlay ou no app — nunca o padrão de fábrica.
    char line[64];
    if (!read_first_line(SYNC_CONFIG_PATH, line, sizeof(line)))
        return false;
    // formato: "enabled=0" / "enabled=1"
    const char *eq = strchr(line, '=');
    if (!eq)
        return false;
    return eq[1] != '0';
}

void syncstate_set_autosync_enabled(bool enabled)
{
    syncstate_ensure_dirs();
    FILE *f = fopen(SYNC_CONFIG_PATH, "w");
    if (!f)
        return;
    fprintf(f, "enabled=%d\n", enabled ? 1 : 0);
    fclose(f);
}

// ------------------------------------------------------------- destino

// Formato: "local=1 cloud=1", numa linha so.
static void read_dest(bool *local, bool *cloud)
{
    *local = true;
    *cloud = true;

    char line[64];
    if (!read_first_line(SYNC_DEST_PATH, line, sizeof(line)))
        return; // sem arquivo = os dois ligados

    const char *l = strstr(line, "local=");
    const char *c = strstr(line, "cloud=");
    if (l)
        *local = l[6] != '0';
    if (c)
        *cloud = c[6] != '0';
}

bool syncstate_dest_local(void)
{
    bool l, c;
    read_dest(&l, &c);
    return l;
}

bool syncstate_dest_cloud(void)
{
    bool l, c;
    read_dest(&l, &c);
    return c;
}

void syncstate_set_dest(bool local, bool cloud)
{
    syncstate_ensure_dirs();
    FILE *f = fopen(SYNC_DEST_PATH, "w");
    if (!f)
        return;
    fprintf(f, "local=%d cloud=%d\n", local ? 1 : 0, cloud ? 1 : 0);
    fclose(f);
}

// ------------------------------------------------------------- exclusões

// Um id por linha, em hexa. Comentário começa com #.
//
// Streaming, sem array: a lista era lida pra um `u64 ids[128]` e o 129º jogo em
// diante simplesmente não existia — o overlay desenhava "Sync LIGADO" num jogo
// que o usuário acabou de desligar, e o autosync subia o save dele. Pior: o
// toggle seguinte reescrevia o arquivo a partir dessa lista já cortada e
// APAGAVA os registros do 129 em diante. Nada disso precisava de arquivo
// corrompido nem de falta de energia: era determinístico a partir do 129º jogo.
static u64 le_id(const char *line)
{
    const char *p = line;
    while (*p == ' ' || *p == '\t')
        p++;
    if (*p == '#' || *p == '\0' || *p == '\n' || *p == '\r')
        return 0;
    return strtoull(p, NULL, 16); // aceita com ou sem "0x"
}

size_t syncstate_list_excluded(u64 *out, size_t max)
{
    FILE *f = fopen(SYNC_EXCLUDE_PATH, "r");
    if (!f)
        return 0;

    size_t n = 0;
    char line[64];
    while (n < max && fgets(line, sizeof(line), f))
    {
        u64 id = le_id(line);
        if (id)
            out[n++] = id;
    }
    fclose(f);
    return n;
}

bool syncstate_is_excluded(u64 application_id)
{
    FILE *f = fopen(SYNC_EXCLUDE_PATH, "r");
    if (!f)
        return false;

    bool achou = false;
    char line[64];
    while (!achou && fgets(line, sizeof(line), f))
        achou = (le_id(line) == application_id);

    fclose(f);
    return achou;
}

// Devolve false quando a lista NÃO ficou como pedido.
//
// Era void, e o silêncio custava caro: esta lista é o "não mexa nesses jogos".
// A gravação abria o arquivo com "w", o que TRUNCA na hora (truncar nunca falta
// espaço), despejava tudo e ignorava o retorno do fclose — que é onde o buffer
// vai pro cartão de verdade. Cartão cheio, e a lista inteira virava um arquivo
// de zero byte: os jogos protegidos voltavam a subir e a receber save da nuvem
// por cima, sozinhos, e o overlay ainda dizia "jogo marcado como excluido".
//
// Agora é o mesmo padrão do arquivo único: escreve num .parcial, confere cada
// escrita e o fclose, e só então troca por rename. Falhou em qualquer ponto, o
// arquivo de antes continua inteiro onde estava.
bool syncstate_set_excluded(u64 application_id, bool excluded)
{
    if (syncstate_is_excluded(application_id) == excluded)
        return true; // já está do jeito pedido, não mexe no arquivo

    syncstate_ensure_dirs();

    char temp[sizeof(SYNC_EXCLUDE_PATH) + 16];
    snprintf(temp, sizeof(temp), "%s.parcial", SYNC_EXCLUDE_PATH);

    FILE *out = fopen(temp, "w");
    if (!out)
        return false;

    bool ok = fprintf(out,
                  "# Jogos que o SwitchSaveSync deve ignorar (um application_id em hex por linha).\n"
                  "# Nao sobe save no autosync nem puxa save da nuvem.\n")
        > 0;

    // Copia linha a linha em vez de reescrever a partir de uma lista lida na
    // memória: assim não existe teto nenhum de quantos jogos cabem.
    FILE *in = fopen(SYNC_EXCLUDE_PATH, "r");
    if (in)
    {
        char line[64];
        while (ok && fgets(line, sizeof(line), in))
        {
            u64 id = le_id(line);
            if (id == 0 || id == application_id)
                continue; // comentário, linha vazia, ou justamente o que sai

            ok = fprintf(out, "%016lX\n", id) > 0;
        }
        fclose(in);
    }

    if (ok && excluded)
        ok = fprintf(out, "%016lX\n", application_id) > 0;

    // O fclose é a escrita de verdade: é nele que o buffer da stdio vai pro
    // cartão. Sem conferir ele, cartão cheio passava por sucesso.
    if (fclose(out) != 0)
        ok = false;

    if (!ok)
    {
        remove(temp); // o de antes continua inteiro
        return false;
    }

    return rename(temp, SYNC_EXCLUDE_PATH) == 0;
}

// ---------------------------------------------------------------- pedido

void syncstate_request_backup(u64 application_id)
{
    syncstate_ensure_dirs();
    FILE *f = fopen(SYNC_REQUEST_PATH, "w");
    if (!f)
        return;
    fprintf(f, "%016lX\n", application_id);
    fclose(f);
}

bool syncstate_take_request(u64 *application_id_out)
{
    char line[64];
    if (!read_first_line(SYNC_REQUEST_PATH, line, sizeof(line)))
        return false;

    remove(SYNC_REQUEST_PATH);

    u64 id = strtoull(line, NULL, 16);
    if (!id)
        return false;

    *application_id_out = id;
    return true;
}

// ---------------------------------------------------------------- status

void syncstate_set_status(const char *fmt, ...)
{
    syncstate_ensure_dirs();
    FILE *f = fopen(SYNC_STATUS_PATH, "w");
    if (!f)
        return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

bool syncstate_get_status(char *out, size_t outsz)
{
    return read_first_line(SYNC_STATUS_PATH, out, outsz);
}

// ------------------------------------------------------------------- log

// Cortado o log, fica a METADE NOVA — não o arquivo vazio.
//
// Antes daqui, passar de 64 KB apagava o log inteiro. E isso mordia justamente
// na hora errada: o log só chega perto de 64 KB numa sessão comprida, que é
// exatamente a sessão em que alguma coisa deu errado; o corte levava junto as
// linhas que explicavam o quê. Quem lê o arquivo depois (eu, pra achar defeito;
// ele, pra saber se o save subiu) recebia um arquivo começando do nada.
//
// A cópia é em pedaços de 4 KB de propósito: isto roda dentro do sysmodule, com
// 128 KB de pilha e uma heap que ainda precisa caber o curl e o mbedTLS.
static void truncate_log_if_huge(void)
{
    FILE *f = fopen(SYNC_LOG_PATH, "r");
    if (!f)
        return;

    if (fseek(f, 0, SEEK_END) != 0)
    {
        fclose(f);
        return;
    }
    long size = ftell(f);
    if (size <= LOG_MAX_BYTES)
    {
        fclose(f);
        return;
    }

    // Começa no meio e anda até a próxima quebra de linha: sem isso o arquivo
    // abriria com metade de uma frase, que é pior que não ter a frase.
    if (fseek(f, size - LOG_KEEP_BYTES, SEEK_SET) != 0)
    {
        fclose(f);
        remove(SYNC_LOG_PATH); // como era antes: melhor perder do que crescer sem fim
        return;
    }
    int c;
    while ((c = fgetc(f)) != EOF && c != '\n')
        ;

    FILE *t = fopen(SYNC_LOG_TEMP_PATH, "w");
    if (!t)
    {
        fclose(f);
        remove(SYNC_LOG_PATH);
        return;
    }

    fprintf(t, "[--- o comeco do log foi cortado: passou de %d KB ---]\n",
        LOG_MAX_BYTES / 1024);

    char buf[4096];
    size_t n;
    bool ok = true;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
    {
        if (fwrite(buf, 1, n, t) != n)
        {
            ok = false;
            break;
        }
    }

    fclose(f);
    if (fclose(t) != 0)
        ok = false;

    // O rename só entra com o arquivo novo inteiro no cartão. Se falhou no
    // meio, joga fora a metade escrita e cai no comportamento antigo — o que
    // não pode acontecer é ficar com um log pela metade achando que é o log.
    if (!ok || rename(SYNC_LOG_TEMP_PATH, SYNC_LOG_PATH) != 0)
    {
        remove(SYNC_LOG_TEMP_PATH);
        remove(SYNC_LOG_PATH);
    }
}

void syncstate_log(const char *fmt, ...)
{
    syncstate_ensure_dirs();
    truncate_log_if_huge();

    FILE *f = fopen(SYNC_LOG_PATH, "a");
    if (!f)
        return;

    // Hora do console. Se o serviço de tempo não estiver disponível (pode
    // acontecer cedo no boot), grava sem timestamp em vez de não gravar nada.
    u64 now = 0;
    if (R_SUCCEEDED(timeGetCurrentTime(TimeType_LocalSystemClock, &now)))
    {
        time_t t = (time_t)now;
        struct tm *tm = localtime(&t);
        if (tm)
            fprintf(f, "[%02d/%02d %02d:%02d:%02d] ", tm->tm_mday, tm->tm_mon + 1,
                tm->tm_hour, tm->tm_min, tm->tm_sec);
    }

    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

// ----------------------------------------------------------------- nomes

void syncstate_sanitize_name(const char *in, char *out, size_t outsz)
{
    if (outsz == 0)
        return;

    size_t n = 0;
    for (; in[n] && n + 1 < outsz; n++)
    {
        unsigned char c = (unsigned char)in[n];
        bool bad = c < 0x20 || strchr("/\\:*?\"<>|", c) != NULL;
        out[n] = bad ? '_' : (char)c;
    }
    // espaço/ponto no fim atrapalha em FAT e no Windows na hora de copiar do SD
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '.'))
        n--;
    out[n] = '\0';

    if (out[0] == '\0')
        snprintf(out, outsz, "sem-nome");
}

// ------------------------------------------------- de-para das pastas
//
// Uma linha por save:
//
//   <application_id> <uid alto> <uid baixo> <nome da pasta>
//
// Os três primeiros em hex de 16 dígitos, o nome é o resto da linha — pode ter
// espaço e parêntese dentro, que é justamente o caso ("Jogo (apelido da
// conta)").
//
// Arquivo de texto, e não binário, pelo mesmo motivo do resto do syncstate: dá
// pra abrir com o cartão no PC e entender o que está escrito quando algo der
// errado.

#define FOLDERS_MAX 256

typedef struct
{
    u64 app;
    u64 uid0, uid1;
    char folder[0x201];
} Folder;

static size_t folders_read(Folder *out, size_t max)
{
    FILE *f = fopen(SYNC_FOLDERS_PATH, "r");
    if (!f)
        return 0;

    size_t n = 0;
    char linha[0x280];
    while (n < max && fgets(linha, sizeof(linha), f))
    {
        linha[strcspn(linha, "\r\n")] = '\0';
        if (linha[0] == '#' || linha[0] == '\0')
            continue;

        // Os temporários existem porque u64 é "unsigned long" no Switch e
        // "unsigned long long" no Mac; passar &e.app direto pro %llX daria
        // ponteiro de tipo errado num dos dois.
        unsigned long long app = 0, u0 = 0, u1 = 0;
        int fim = 0;
        // O %n devolve onde a leitura dos três números parou; o nome é dali
        // em diante, tirando o espaço separador. Não dá pra usar %s no nome:
        // ele pararia no primeiro espaço.
        if (sscanf(linha, "%16llX %16llX %16llX%n", &app, &u0, &u1, &fim) != 3)
            continue;
        if (linha[fim] != ' ' || linha[fim + 1] == '\0')
            continue;

        Folder e;
        e.app  = (u64)app;
        e.uid0 = (u64)u0;
        e.uid1 = (u64)u1;
        snprintf(e.folder, sizeof(e.folder), "%s", linha + fim + 1);
        out[n++] = e;
    }

    fclose(f);
    return n;
}

static void folders_write(const Folder *list, size_t n)
{
    syncstate_ensure_dirs();
    FILE *f = fopen(SYNC_FOLDERS_PATH, "w");
    if (!f)
        return;

    fprintf(f, "# Em que pasta da nuvem mora o save de cada jogo.\n"
               "# <application_id> <uid alto> <uid baixo> <nome da pasta>\n"
               "# Existe porque o apelido da conta entra no nome da pasta e o\n"
               "# console deixa trocar o apelido; o uid nao muda, o nome muda.\n"
               "# Apagar este arquivo nao perde save nenhum: o app volta a usar\n"
               "# o nome calculado, e backup com nome antigo fica orfao na nuvem.\n");
    for (size_t i = 0; i < n; i++)
        fprintf(f, "%016llX %016llX %016llX %s\n",
                (unsigned long long)list[i].app,
                (unsigned long long)list[i].uid0,
                (unsigned long long)list[i].uid1,
                list[i].folder);

    fclose(f);
}

static bool mesma_chave(const Folder *e, u64 app, AccountUid uid)
{
    return e->app == app && e->uid0 == uid.uid[0] && e->uid1 == uid.uid[1];
}

void syncstate_remember_folder(u64 application_id, AccountUid uid, const char *folder)
{
    if (!folder || folder[0] == '\0')
        return;

    static Folder list[FOLDERS_MAX];
    size_t n = folders_read(list, FOLDERS_MAX);

    for (size_t i = 0; i < n; i++)
        if (mesma_chave(&list[i], application_id, uid))
        {
            if (strcmp(list[i].folder, folder) == 0)
                return; // já está gravado assim, não mexe no cartão à toa
            snprintf(list[i].folder, sizeof(list[i].folder), "%s", folder);
            folders_write(list, n);
            return;
        }

    if (n >= FOLDERS_MAX)
    {
        // Teto batido. Some com o mais antigo em vez de ignorar o pedido: o
        // registro novo é o que está em uso agora.
        memmove(&list[0], &list[1], (FOLDERS_MAX - 1) * sizeof(Folder));
        n = FOLDERS_MAX - 1;
    }

    list[n].app  = application_id;
    list[n].uid0 = uid.uid[0];
    list[n].uid1 = uid.uid[1];
    snprintf(list[n].folder, sizeof(list[n].folder), "%s", folder);
    folders_write(list, n + 1);
}

bool syncstate_recall_folder(u64 application_id, AccountUid uid, char *out, size_t outsz)
{
    if (!out || outsz == 0)
        return false;

    static Folder list[FOLDERS_MAX];
    size_t n = folders_read(list, FOLDERS_MAX);

    for (size_t i = 0; i < n; i++)
        if (mesma_chave(&list[i], application_id, uid))
        {
            snprintf(out, outsz, "%s", list[i].folder);
            return true;
        }

    return false;
}

void syncstate_forget_folder(u64 application_id, AccountUid uid)
{
    static Folder list[FOLDERS_MAX];
    size_t n = folders_read(list, FOLDERS_MAX);

    size_t saiu = 0;
    for (size_t i = 0; i < n; i++)
        if (mesma_chave(&list[i], application_id, uid))
            saiu++;
        else if (saiu)
            list[i - saiu] = list[i];

    if (saiu)
        folders_write(list, n - saiu);
}
