#include "http.h"
#include "ota.h"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

bool ota_fetch_release(OtaRelease* release)
{
    HttpResponse response = http_request_raw("GET",
        "https://api.github.com/repos/NspxMiguel/SwitchSaveSync/releases/latest", NULL, NULL, NULL,
        0, "Accept: application/vnd.github+json");
    bool ok = response.ok && response.status == 200 && response.body
        && ota_parse_release(response.body, release);
    http_response_free(&response);
    return ok;
}
typedef struct
{
    unsigned asset;
    OtaProgress callback;
    void* user;
} Transfer;
static int progress_callback(
    void* user, curl_off_t total, curl_off_t received, curl_off_t upload_total, curl_off_t uploaded)
{
    (void)upload_total;
    (void)uploaded;
    Transfer* transfer = user;
    return transfer->callback
        && !transfer->callback(
            transfer->asset, (uint64_t)received, (uint64_t)total, transfer->user);
}
static size_t write_file(void* data, size_t size, size_t count, void* user)
{
    return fwrite(data, 1, size * count, user);
}
/* Uses the HTTP stack's initialization and CA selection, with streaming progress. */
static bool download(const char* url, const char* path, Transfer* transfer)
{
    if (!http_is_ready() || !*http_ca_bundle())
        return false;
    CURL* curl = curl_easy_init();
    if (!curl)
        return false;
    FILE* file = fopen(path, "wb");
    if (!file)
    {
        curl_easy_cleanup(curl);
        return false;
    }
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_CAINFO, http_ca_bundle());
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);
    curl_easy_setopt(curl, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 5L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "SwitchSaveSync OTA");
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 600L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1024L);
    curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 30L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, file);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, transfer);
    CURLcode result = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    bool ok = result == CURLE_OK && status == 200;
    if (fflush(file) || fsync(fileno(file)))
        ok = false;
    if (fclose(file))
        ok = false;
    curl_easy_cleanup(curl);
    if (!ok)
        remove(path);
    return ok;
}
bool ota_stage(
    const OtaRelease* release, OtaFile files[OTA_ASSET_COUNT], OtaProgress progress, void* user)
{
    HttpResponse sums = http_get(release->sums_url, NULL);
    unsigned char hashes[4][32];
    bool ok = sums.ok && sums.status == 200 && sums.body;
    for (int i = 0; ok && i < 4; i++)
        ok = ota_checksum(sums.body, ota_asset_names[i], hashes[i]);
    http_response_free(&sums);
    if (!ok)
        return false;
    for (int i = 0; i < 4; i++)
    {
        char parent[1024];
        strcpy(parent, files[i].target);
        char* slash = strrchr(parent, '/');
        if (!slash)
        {
            ok = false;
            break;
        }
        *slash = 0;
        for (char* p = parent + 6; *p; p++)
            if (*p == '/')
            {
                *p = 0;
                mkdir(parent, 0777);
                *p = '/';
            }
        mkdir(parent, 0777);
        Transfer transfer = { (unsigned)i, progress, user };
        memcpy(files[i].digest, hashes[i], 32);
        files[i].verify_before_replace = true;
        if (!download(release->urls[i], files[i].staged, &transfer)
            || !ota_verify_file(files[i].staged, hashes[i]))
        {
            ok = false;
            break;
        }
    }
    if (!ok)
        ota_discard(files, 4);
    return ok;
}
