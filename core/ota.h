#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C"
{
#endif
#define OTA_ASSET_COUNT 4
#define OTA_MODULE_ID UINT64_C(0x00FF0000535953FF)
    typedef struct
    {
        char version[128];
        char notes[1200];
        char urls[OTA_ASSET_COUNT][1024];
        char sums_url[1024];
    } OtaRelease;
    typedef struct
    {
        char target[1024];
        char staged[1040];
        char backup[1040];
        bool existed;
        bool installed;
        bool verify_before_replace;
        unsigned char digest[32];
    } OtaFile;
    typedef bool (*OtaProgress)(unsigned asset, uint64_t received, uint64_t total, void* user);
    extern const char* const ota_asset_names[OTA_ASSET_COUNT];
    /* Returns false for invalid SemVer. Build metadata does not affect ordering. */
    bool ota_version_compare(const char* a, const char* b, int* order);
    bool ota_checksum(const char* manifest, const char* name, unsigned char digest[32]);
    bool ota_verify_file(const char* path, const unsigned char expected[32]);
    bool ota_plan(OtaFile files[OTA_ASSET_COUNT], const char* nro_path);
    /* Roll back completed renames on failure. Existing .bak files are never consumed. */
    bool ota_apply(OtaFile* files, size_t count);
    void ota_discard(OtaFile* files, size_t count);
    bool ota_parse_release(const char* json, OtaRelease* release);
    /* Persist attempts before requests; manual requests also obey this cooldown. */
    bool ota_check_due(const char* state_path, int64_t now);
    bool ota_record_check(const char* state_path, int64_t now);
    bool ota_fetch_release(OtaRelease* release);
    bool ota_stage(const OtaRelease* release, OtaFile files[OTA_ASSET_COUNT], OtaProgress progress,
        void* user);
#ifdef __cplusplus
}
#endif
