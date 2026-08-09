#include "syncstate.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define LOG_MAX_BYTES (64 * 1024)

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
    // "não configurei nada ainda" virava "pode mexer sozinho nos meus saves".
    // Escrever em save é irreversível; ficar quieto não é. Quem liga é ele, no
    // overlay ou no app — nunca o padrão de fábrica.
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

size_t syncstate_list_excluded(u64 *out, size_t max)
{
    FILE *f = fopen(SYNC_EXCLUDE_PATH, "r");
    if (!f)
        return 0;

    size_t n = 0;
    char line[64];
    while (n < max && fgets(line, sizeof(line), f))
    {
        // aceita com ou sem "0x", e ignora linha vazia / comentário
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '#' || *p == '\0' || *p == '\n' || *p == '\r')
            continue;
        u64 id = strtoull(p, NULL, 16);
        if (id)
            out[n++] = id;
    }
    fclose(f);
    return n;
}

bool syncstate_is_excluded(u64 application_id)
{
    u64 ids[128];
    size_t n = syncstate_list_excluded(ids, 128);
    for (size_t i = 0; i < n; i++)
        if (ids[i] == application_id)
            return true;
    return false;
}

void syncstate_set_excluded(u64 application_id, bool excluded)
{
    u64 ids[128];
    size_t n = syncstate_list_excluded(ids, 128);

    bool present = false;
    for (size_t i = 0; i < n; i++)
        if (ids[i] == application_id)
            present = true;

    if (present == excluded)
        return; // já está do jeito pedido, não mexe no arquivo

    syncstate_ensure_dirs();
    FILE *f = fopen(SYNC_EXCLUDE_PATH, "w");
    if (!f)
        return;

    fprintf(f, "# Jogos que o SwitchSaveSync deve ignorar (um application_id em hex por linha).\n"
               "# Nao sobe save no autosync nem puxa save da nuvem.\n");
    for (size_t i = 0; i < n; i++)
        if (ids[i] != application_id)
            fprintf(f, "%016lX\n", ids[i]);
    if (excluded)
        fprintf(f, "%016lX\n", application_id);

    fclose(f);
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

static void truncate_log_if_huge(void)
{
    FILE *f = fopen(SYNC_LOG_PATH, "r");
    if (!f)
        return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fclose(f);
    if (size > LOG_MAX_BYTES)
        remove(SYNC_LOG_PATH);
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
    // espaço/ponto no fim atrapalha em FAT e no Windows quando ele copiar do SD
    while (n > 0 && (out[n - 1] == ' ' || out[n - 1] == '.'))
        n--;
    out[n] = '\0';

    if (out[0] == '\0')
        snprintf(out, outsz, "sem-nome");
}
