#include "ota.h"
#include "minijson.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

const char* const ota_asset_names[OTA_ASSET_COUNT]
    = { "SwitchSaveSync.nro", "exefs.nsp", "toolbox.json", "SwitchSaveSync.ovl" };

typedef struct
{
    unsigned long parts[3];
    char pre[128];
} Version;
static bool parse_version(const char* text, Version* out)
{
    memset(out, 0, sizeof(*out));
    if (*text == 'v')
        text++;
    for (int i = 0; i < 3; i++)
    {
        if (!isdigit((unsigned char)*text) || (*text == '0' && isdigit((unsigned char)text[1])))
            return false;
        errno = 0;
        char* end;
        out->parts[i] = strtoul(text, &end, 10);
        if (errno)
            return false;
        text = end;
        if (i < 2 && *text++ != '.')
            return false;
    }
    if (*text == '-')
    {
        text++;
        const char* start = text;
        while (*text && *text != '+')
            text++;
        size_t n = (size_t)(text - start);
        if (!n || n >= sizeof(out->pre))
            return false;
        memcpy(out->pre, start, n);
    }
    if (*text == '+')
    {
        text++;
        if (!*text)
            return false;
    }
    else if (*text)
        return false;
    /* Validate both prerelease and metadata identifiers. */
    const char* sections[2] = { out->pre, text };
    for (int s = 0; s < 2; s++)
    {
        const char* p = sections[s];
        if (!*p)
            continue;
        while (*p)
        {
            const char* start = p;
            bool numeric = true;
            while (*p && *p != '.')
            {
                if (!isalnum((unsigned char)*p) && *p != '-')
                    return false;
                if (!isdigit((unsigned char)*p))
                    numeric = false;
                p++;
            }
            if (p == start || (s == 0 && numeric && p - start > 1 && *start == '0'))
                return false;
            if (*p && !*++p)
                return false;
        }
    }
    return true;
}

bool ota_version_compare(const char* a, const char* b, int* order)
{
    Version x, y;
    if (!a || !b || !order || !parse_version(a, &x) || !parse_version(b, &y))
        return false;
    for (int i = 0; i < 3; i++)
        if (x.parts[i] != y.parts[i])
        {
            *order = x.parts[i] > y.parts[i] ? 1 : -1;
            return true;
        }
    if (!*x.pre || !*y.pre)
    {
        *order = !*x.pre - !*y.pre;
        return true;
    }
    char *p = x.pre, *q = y.pre;
    while (*p && *q)
    {
        char *pe = strchr(p, '.'), *qe = strchr(q, '.');
        size_t pn = pe ? (size_t)(pe - p) : strlen(p), qn = qe ? (size_t)(qe - q) : strlen(q);
        bool pd = strspn(p, "0123456789") >= pn, qd = strspn(q, "0123456789") >= qn;
        int c = 0;
        if (pd != qd)
            c = pd ? -1 : 1;
        else if (pd && pn != qn)
            c = pn > qn ? 1 : -1;
        else
        {
            c = memcmp(p, q, pn < qn ? pn : qn);
            if (!c && pn != qn)
                c = pn > qn ? 1 : -1;
        }
        if (c)
        {
            *order = c > 0 ? 1 : -1;
            return true;
        }
        p += pn;
        q += qn;
        if (*p)
            p++;
        if (*q)
            q++;
    }
    *order = !!*p - !!*q;
    return true;
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return -1;
}
bool ota_checksum(const char* manifest, const char* name, unsigned char digest[32])
{
    bool found = false;
    for (const char* p = manifest; *p;)
    {
        const char* end = strchr(p, '\n');
        if (!end)
            end = p + strlen(p);
        size_t n = (size_t)(end - p);
        if (n && p[n - 1] == '\r')
            n--;
        if (n)
        {
            if (n < 67 || p[64] != ' ' || (p[65] != ' ' && p[65] != '*'))
                return false;
            unsigned char value[32];
            for (int i = 0; i < 32; i++)
            {
                int h = hex_value(p[i * 2]), l = hex_value(p[i * 2 + 1]);
                if (h < 0 || l < 0)
                    return false;
                value[i] = (unsigned char)((h << 4) | l);
            }
            if (strlen(name) == n - 66 && !memcmp(p + 66, name, n - 66))
            {
                if (found)
                    return false;
                memcpy(digest, value, 32);
                found = true;
            }
        }
        p = *end ? end + 1 : end;
    }
    return found;
}

bool ota_plan(OtaFile files[OTA_ASSET_COUNT], const char* nro_path)
{
    if (!nro_path || strncmp(nro_path, "sdmc:/", 6) || strlen(nro_path) >= 1024
        || strlen(nro_path) < 10 || strcmp(nro_path + strlen(nro_path) - 4, ".nro"))
        return false;
    const char* targets[4] = { nro_path, "sdmc:/atmosphere/contents/00FF0000535953FF/exefs.nsp",
        "sdmc:/atmosphere/contents/00FF0000535953FF/toolbox.json",
        "sdmc:/switch/.overlays/SwitchSaveSync.ovl" };
    memset(files, 0, sizeof(OtaFile) * 4);
    for (int i = 0; i < 4; i++)
    {
        snprintf(files[i].target, sizeof(files[i].target), "%s", targets[i]);
        snprintf(files[i].staged, sizeof(files[i].staged), "%s.ota", targets[i]);
        snprintf(files[i].backup, sizeof(files[i].backup), "%s.bak", targets[i]);
    }
    return true;
}
void ota_discard(OtaFile* files, size_t count)
{
    for (size_t i = 0; i < count; i++)
        remove(files[i].staged);
}
bool ota_apply(OtaFile* files, size_t count)
{
    struct stat st;
    for (size_t i = 0; i < count; i++)
    {
        if (stat(files[i].staged, &st) || !S_ISREG(st.st_mode) || st.st_size == 0)
            return false;
        if (files[i].verify_before_replace && !ota_verify_file(files[i].staged, files[i].digest))
            return false;
        files[i].installed = false;
        files[i].existed = stat(files[i].target, &st) == 0;
        if (files[i].existed && !S_ISREG(st.st_mode))
            return false;
        if (!files[i].existed && errno != ENOENT)
            return false;
    }
    size_t i;
    for (i = 0; i < count; i++)
    {
        OtaFile* f = &files[i];
        if (f->existed)
        {
            if (remove(f->backup) && errno != ENOENT)
                break;
            if (rename(f->target, f->backup))
                break;
        }
        if (rename(f->staged, f->target))
        {
            if (f->existed)
                rename(f->backup, f->target);
            break;
        }
        f->installed = true;
    }
    if (i == count)
        return true;
    while (i > 0)
    {
        OtaFile* f = &files[--i];
        // Continue restoring other files even if one rollback fails.
        if (rename(f->target, f->staged))
            continue;
        if (f->existed && rename(f->backup, f->target))
            continue;
        f->installed = false;
    }
    return false;
}

bool ota_parse_release(const char* json, OtaRelease* release)
{
    memset(release, 0, sizeof(*release));
    Version version;
    if (!json_get_string(json, "tag_name", release->version, sizeof(release->version))
        || !parse_version(release->version, &version))
        return false;
    /* Extract notes in full before trimming at a UTF-8 boundary. */
    char* notes = malloc(strlen(json) + 1);
    if (!notes)
        return false;
    if (json_get_string(json, "body", notes, strlen(json) + 1))
    {
        size_t n = strlen(notes);
        if (n > sizeof(release->notes) - 1)
            n = sizeof(release->notes) - 1;
        while (n && ((unsigned char)notes[n] & 0xc0) == 0x80)
            n--;
        memcpy(release->notes, notes, n);
        /* Strip control characters from untrusted release text. */
        for (size_t i = 0; i < n; i++)
            if ((unsigned char)release->notes[i] < 32 && release->notes[i] != '\n')
                release->notes[i] = ' ';
    }
    free(notes);
    const char *assets = json_value_at(json, "assets"), *cursor;
    if (!assets || !json_array_begin(assets, &cursor))
        return false;
    char element[8192], name[128], url[1024];
    while (json_array_next(&cursor, element, sizeof(element)))
    {
        if (!json_get_string(element, "name", name, sizeof(name))
            || !json_get_string(element, "browser_download_url", url, sizeof(url)))
            continue;
        if (strncmp(url, "https://github.com/NspxMiguel/SwitchSaveSync/releases/download/",
                sizeof("https://github.com/NspxMiguel/SwitchSaveSync/releases/download/") - 1))
            continue;
        for (int i = 0; i < 5; i++)
            if (!strcmp(name, i == 4 ? "SHA256SUMS" : ota_asset_names[i]))
            {
                char* dest = i == 4 ? release->sums_url : release->urls[i];
                if (*dest)
                    return false;
                strcpy(dest, url);
            }
    }
    if (!*release->sums_url)
        return false;
    for (int i = 0; i < 4; i++)
        if (!*release->urls[i])
            return false;
    return true;
}
bool ota_check_due(const char* path, int64_t now)
{
    if (now < 1577836800)
        return false;
    FILE* f = fopen(path, "r");
    if (!f)
        return errno == ENOENT;
    long long last = 0;
    bool valid = fscanf(f, "%lld", &last) == 1;
    fclose(f);
    return valid && last >= 1577836800 && now >= last && now - last >= 86400;
}
bool ota_record_check(const char* path, int64_t now)
{
    char tmp[1100];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int)sizeof(tmp))
        return false;
    FILE* f = fopen(tmp, "w");
    if (!f)
        return false;
    bool ok = fprintf(f, "%lld\n", (long long)now) > 0;
    if (fflush(f) || fsync(fileno(f)))
        ok = false;
    if (fclose(f))
        ok = false;
    if (ok && !rename(tmp, path))
        return true;
    remove(tmp);
    return false;
}
