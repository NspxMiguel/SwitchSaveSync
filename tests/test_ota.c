#include "ota.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void compare(const char* a, const char* b, int expected)
{
    int order = 99;
    assert(ota_version_compare(a, b, &order) && order == expected);
    assert(ota_version_compare(b, a, &order) && order == -expected);
}
static void write_text(const char* path, const char* text)
{
    FILE* f = fopen(path, "wb");
    assert(f);
    assert(fwrite(text, 1, strlen(text), f) == strlen(text));
    assert(!fclose(f));
}
static void expect_text(const char* path, const char* expected)
{
    char text[100] = { 0 };
    FILE* f = fopen(path, "rb");
    assert(f);
    assert(fread(text, 1, sizeof(text) - 1, f) == strlen(expected));
    fclose(f);
    assert(!strcmp(text, expected));
}
static void setup_file(OtaFile* f, const char* path, bool exists)
{
    memset(f, 0, sizeof(*f));
    snprintf(f->target, sizeof(f->target), "%s", path);
    snprintf(f->staged, sizeof(f->staged), "%s.ota", path);
    snprintf(f->backup, sizeof(f->backup), "%s.bak", path);
    if (exists)
        write_text(f->target, "old");
    write_text(f->staged, "new");
}
int main(void)
{
    compare("v1.2.3", "1.2.3", 0);
    compare("1.10.0", "1.9.99", 1);
    compare("2.0.0", "1.999.999", 1);
    compare("1.2.4", "1.2.3", 1);
    compare("1.0.0+build.9", "1.0.0+build.10", 0);
    const char* sequence[] = { "1.0.0-alpha", "1.0.0-alpha.1", "1.0.0-alpha.beta", "1.0.0-beta",
        "1.0.0-beta.2", "1.0.0-beta.11", "1.0.0-rc.1", "1.0.0" };
    for (size_t i = 1; i < sizeof(sequence) / sizeof(*sequence); i++)
        compare(sequence[i - 1], sequence[i], -1);
    compare("1.0.0-99999999999999999999999999", "1.0.0-100000000000000000000000000", -1);
    const char* invalid[] = { "", "1", "1.2", "1.2.3.4", "01.2.3", "1.02.3", "1.2.3-01", "1.2.3-",
        "1.2.3+", "1.2.3-x..y", "1.2.3+foo.", "1.2.3-@", "1.2.3 extra" };
    int order;
    for (size_t i = 0; i < sizeof(invalid) / sizeof(*invalid); i++)
        assert(!ota_version_compare(invalid[i], "1.0.0", &order));

    const char* abc = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    char manifest[500];
    unsigned char hash[32];
    snprintf(manifest, sizeof(manifest), "%s  SwitchSaveSync.nro\r\n", abc);
    assert(ota_checksum(manifest, "SwitchSaveSync.nro", hash));
    write_text("hash.bin", "abc");
    assert(ota_verify_file("hash.bin", hash));
    write_text("hash.bin", "abd");
    assert(!ota_verify_file("hash.bin", hash));
    snprintf(manifest, sizeof(manifest), "%s *file\n", abc);
    assert(ota_checksum(manifest, "file", hash));
    assert(!ota_checksum(manifest, "fil", hash));
    assert(!ota_checksum(manifest, "missing", hash));
    snprintf(manifest, sizeof(manifest), "%s  file\n%s  file\n", abc, abc);
    assert(!ota_checksum(manifest, "file", hash));
    manifest[0] = 'z';
    assert(!ota_checksum(manifest, "file", hash));
    assert(!ota_checksum("short\n", "file", hash));
    assert(!ota_checksum("", "file", hash));
    assert(ota_checksum(
        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855  file", "file", hash));
    write_text("hash.bin", "");
    assert(ota_verify_file("hash.bin", hash));
    assert(ota_checksum(
        "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0  file\n", "file", hash));
    FILE* million = fopen("hash.bin", "wb");
    assert(million);
    for (int i = 0; i < 1000000; i++)
        fputc('a', million);
    fclose(million);
    assert(ota_verify_file("hash.bin", hash));

    OtaFile files[4];
    assert(ota_plan(files, "sdmc:/switch/custom.nro"));
    assert(!strcmp(files[0].target, "sdmc:/switch/custom.nro"));
    assert(!strcmp(files[1].target, "sdmc:/atmosphere/contents/00FF0000535953FF/exefs.nsp"));
    assert(!ota_plan(files, "/switch/app.nro"));
    assert(!ota_plan(files, "sdmc:/app.nsp"));
    setup_file(&files[0], "first", true);
    setup_file(&files[1], "second", false);
    assert(ota_apply(files, 2));
    expect_text("first", "new");
    expect_text("first.bak", "old");
    expect_text("second", "new");
    assert(access("second.bak", F_OK));
    setup_file(&files[0], "rollback-first", true);
    setup_file(&files[1], "rollback-second", true);
    assert(!mkdir(files[1].backup, 0777));
    write_text("rollback-second.bak/block", "block");
    assert(!ota_apply(files, 2));
    expect_text(files[0].target, "old");
    expect_text(files[1].target, "old");
    expect_text(files[0].staged, "new");
    expect_text(files[1].staged, "new");
    ota_discard(files, 2);
    assert(access(files[0].staged, F_OK));
    setup_file(&files[0], "missing-first", true);
    setup_file(&files[1], "missing-second", true);
    remove(files[1].staged);
    assert(!ota_apply(files, 2));
    expect_text(files[0].target, "old");
    setup_file(&files[0], "empty", true);
    write_text(files[0].staged, "");
    assert(!ota_apply(files, 1));
    expect_text(files[0].target, "old");
    assert(ota_check_due("clock", 1800000000));
    assert(ota_record_check("clock", 1800000000));
    assert(!ota_check_due("clock", 1800000001));
    assert(!ota_check_due("clock", 1799999999));
    assert(ota_check_due("clock", 1800086400));
    assert(!ota_check_due("clock", 0));
    write_text("clock", "-9223372036854775808");
    assert(!ota_check_due("clock", 1800086400));
    write_text("clock", "garbage");
    assert(!ota_check_due("clock", 1800086400));

    char json[8000];
    strcpy(json, "{\"tag_name\":\"v1.2.3\",\"body\":\"Notes\\nMore\",\"assets\":[");
    for (int i = 0; i < 5; i++)
    {
        char asset[1000];
        const char* name = i == 4 ? "SHA256SUMS" : ota_asset_names[i];
        snprintf(asset, sizeof(asset),
            "%s{\"name\":\"%s\",\"browser_download_url\":\"https://github.com/NspxMiguel/"
            "SwitchSaveSync/releases/download/v1.2.3/%s\"}",
            i ? "," : "", name, name);
        strcat(json, asset);
    }
    strcat(json, "]}");
    OtaRelease release;
    assert(ota_parse_release(json, &release));
    assert(!strcmp(release.notes, "Notes\nMore"));
    char* sum = strstr(json, "SHA256SUMS");
    *sum = 'X';
    assert(!ota_parse_release(json, &release));
    *sum = 'S';
    char* host = strstr(json, "https://github.com");
    host[0] = 'x';
    assert(!ota_parse_release(json, &release));
    setup_file(&files[0], "tampered", true);
    files[0].verify_before_replace = true;
    assert(ota_checksum("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad  file",
        "file", files[0].digest));
    write_text(files[0].staged, "abc");
    assert(ota_apply(files, 1));
    expect_text(files[0].target, "abc");
    setup_file(&files[0], "tampered-after-verification", true);
    files[0].verify_before_replace = true;
    assert(ota_checksum("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad  file",
        "file", files[0].digest));
    write_text(files[0].staged, "tampered");
    assert(!ota_apply(files, 1));
    expect_text(files[0].target, "old");
    puts("OTA: SemVer, checksums, SHA-256 vectors, release assets, cooldown, replacement and "
         "rollback passed");
}
