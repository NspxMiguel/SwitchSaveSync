// savemount.c — usa os wrappers de alto nível da libnx
// (fsdevMountSaveData / fsdevMountSaveDataReadOnly), que montam o save por
// application_id + AccountUid direto, sem precisar remontar um
// FsSaveDataAttribute na mão. Assinaturas conferidas contra
// /opt/devkitpro/libnx/include/switch/runtime/devices/fs_dev.h.

#include "savemount.h"

#include <switch.h>
#include <stdio.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#define SAVE_DEVICE_NAME "save"

static bool g_mounted = false;

bool savemount_mount_typed(u64 application_id, AccountUid uid, bool device, bool read_only) {
    if (g_mounted) return false; // já tem um save montado, evita mount duplo

    Result rc;
    if (device) {
        // Save do console, sem dono — não passa uid. E não existe variante
        // somente-leitura na libnx: read_only é ignorado aqui de propósito, e
        // está avisado no savemount.h pra ninguém achar que está protegido.
        (void)read_only;
        rc = fsdevMountDeviceSaveData(SAVE_DEVICE_NAME, application_id);
    } else {
        // read_only no backup: o jogo continua dono do save e nada que a gente
        // faça consegue escrever nele por acidente.
        rc = read_only
            ? fsdevMountSaveDataReadOnly(SAVE_DEVICE_NAME, application_id, uid)
            : fsdevMountSaveData(SAVE_DEVICE_NAME, application_id, uid);
    }
    if (R_FAILED(rc)) return false;

    g_mounted = true;
    return true;
}

bool savemount_mount(u64 application_id, AccountUid uid, bool read_only) {
    return savemount_mount_typed(application_id, uid, false, read_only);
}

void savemount_unmount(bool should_commit) {
    if (!g_mounted) return;

    if (should_commit) {
        fsdevCommitDevice(SAVE_DEVICE_NAME);
    }
    fsdevUnmountDevice(SAVE_DEVICE_NAME);
    g_mounted = false;
}

bool savemount_save_exists(u64 application_id, AccountUid uid) {
    if (g_mounted) return false; // não dá pra responder com outro save montado

    if (R_FAILED(fsdevMountSaveDataReadOnly(SAVE_DEVICE_NAME, application_id, uid)))
        return false;

    fsdevUnmountDevice(SAVE_DEVICE_NAME);
    return true;
}

bool savemount_create_account_save(u64 application_id, AccountUid uid, u64 size, u64 journal) {
    // Campos conferidos contra
    // /opt/devkitpro/libnx/include/switch/services/fs.h — FsSaveDataAttribute,
    // FsSaveDataCreationInfo e FsSaveDataMetaInfo.
    FsSaveDataAttribute attr;
    memset(&attr, 0, sizeof(attr));
    attr.application_id  = application_id;
    attr.uid             = uid;
    attr.save_data_type  = FsSaveDataType_Account;

    FsSaveDataCreationInfo info;
    memset(&info, 0, sizeof(info));
    info.save_data_size     = (s64)size;
    info.journal_size       = (s64)journal;
    info.available_size     = 0x4000;
    info.owner_id           = application_id;
    info.flags              = 0;
    info.save_data_space_id = FsSaveDataSpaceId_User;

    // Save de conta leva meta de thumbnail — é o que o sistema espera desse
    // tipo. Sem isso o save nasce, mas o console não o trata como save de jogo
    // de verdade.
    FsSaveDataMetaInfo meta;
    memset(&meta, 0, sizeof(meta));
    meta.size = 0x40060;
    meta.type = FsSaveDataMetaType_Thumbnail;

    return R_SUCCEEDED(fsCreateSaveDataFileSystem(&attr, &info, &meta));
}

// Apaga o conteúdo de dir recursivamente. Se remove_self=true, apaga a
// própria pasta no fim (não fazemos isso na raiz do save: o container tem
// que continuar existindo).
static bool wipe_dir(const char *dir, bool remove_self) {
    DIR *d = opendir(dir);
    if (!d) return false;

    bool all_ok = true;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        char path[600];
        snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name);

        struct stat st;
        if (stat(path, &st) != 0) { all_ok = false; continue; }

        if (S_ISDIR(st.st_mode)) {
            if (!wipe_dir(path, true)) all_ok = false;
        } else if (unlink(path) != 0) {
            all_ok = false;
        }
    }
    closedir(d);

    if (remove_self && rmdir(dir) != 0) all_ok = false;
    return all_ok;
}

bool savemount_wipe_contents(void) {
    if (!g_mounted) return false;
    return wipe_dir(SAVE_DEVICE_NAME ":/", false);
}

bool savemount_copy_tree(const char *src_dir, const char *dst_dir) {
    mkdir(dst_dir, 0777); // ignora erro se já existe

    DIR *d = opendir(src_dir);
    if (!d) return false;

    bool all_ok = true;
    struct dirent *entry;
    while ((entry = readdir(d)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        char src_path[600], dst_path[600];
        snprintf(src_path, sizeof(src_path), "%s/%s", src_dir, entry->d_name);
        snprintf(dst_path, sizeof(dst_path), "%s/%s", dst_dir, entry->d_name);

        struct stat st;
        if (stat(src_path, &st) != 0) {
            all_ok = false;
            continue;
        }

        if (S_ISDIR(st.st_mode)) {
            if (!savemount_copy_tree(src_path, dst_path)) all_ok = false;
            continue;
        }

        FILE *in = fopen(src_path, "rb");
        if (!in) { all_ok = false; continue; }
        FILE *out = fopen(dst_path, "wb");
        if (!out) { fclose(in); all_ok = false; continue; }

        char buf[8192];
        size_t n;
        while ((n = fread(buf, 1, sizeof(buf), in)) > 0) {
            if (fwrite(buf, 1, n, out) != n) { all_ok = false; break; }
        }

        fclose(in);
        fclose(out);
    }
    closedir(d);
    return all_ok;
}
