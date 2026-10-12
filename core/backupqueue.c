#include "backupqueue.h"
#include "syncjob.h"
#include "syncstate.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static uint64_t job_checksum(const BackupJob *job) {
    uint64_t hash = 1469598103934665603ULL;
    const unsigned char *bytes = (const unsigned char *)job;
    for (size_t i = 0; i < sizeof(*job); i++) {
        unsigned char value = (i >= offsetof(BackupJob, checksum) && i < offsetof(BackupJob, checksum) + sizeof(job->checksum)) ? 0 : bytes[i];
        hash ^= value; hash *= 1099511628211ULL;
    }
    return hash;
}

static bool write_job(const char *path, const BackupJob *source) {
    BackupJob checked = *source;
    checked.checksum = job_checksum(&checked);
    const BackupJob *job = &checked;
    char temp[256], dest[256];
    snprintf(temp, sizeof(temp), "%s/job.tmp", path);
    snprintf(dest, sizeof(dest), "%s/job.bin", path);
    FILE *file = fopen(temp, "wb");
    if (!file) return false;
    bool ok = fwrite(job, sizeof(*job), 1, file) == 1;
    if (fflush(file) || fsync(fileno(file))) ok = false;
    if (fclose(file)) ok = false;
    if (!ok || rename(temp, dest)) { unlink(temp); return false; }
    return true;
}

static bool read_job(const char *path, BackupJob *job) {
    char name[256];
    snprintf(name, sizeof(name), "%s/job.bin", path);
    FILE *file = fopen(name, "rb");
    if (!file) return false;
    bool ok = fread(job, sizeof(*job), 1, file) == 1 && fgetc(file) == EOF && !ferror(file);
    fclose(file);
    return ok && job->checksum == job_checksum(job) && job->version == 1 && job->backend < 2 && job->title.application_id &&
           memchr(job->title.name, 0, sizeof(job->title.name)) &&
           memchr(job->title.account, 0, sizeof(job->title.account));
}

static bool remove_tree(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) return errno == ENOENT;
    bool ok = true;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        char child[1024];
        if (snprintf(child, sizeof(child), "%s/%s", path, entry->d_name) >= (int)sizeof(child)) { ok = false; break; }
        struct stat st;
        if (lstat(child, &st)) { ok = false; break; }
        if (S_ISDIR(st.st_mode) ? !remove_tree(child) : unlink(child) != 0) ok = false;
    }
    closedir(dir);
    return ok && rmdir(path) == 0;
}

static uint64_t scan(char *first, size_t size, size_t *count) {
    DIR *dir = opendir(BACKUP_QUEUE_DIR);
    uint64_t largest = 0, smallest = UINT64_MAX;
    *count = 0;
    if (!dir) return 0;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        unsigned long long id; char suffix[16], extra;
        if (sscanf(entry->d_name, "%20llu.%15s%c", &id, suffix, &extra) != 2) continue;
        if (id > largest) largest = id;
        if (strcmp(suffix, "job")) continue;
        (*count)++;
        if (id < smallest) {
            smallest = id;
            if (first) snprintf(first, size, "%s/%s", BACKUP_QUEUE_DIR, entry->d_name);
        }
    }
    closedir(dir);
    return largest;
}

bool backupqueue_enqueue(const TitleEntry *title, unsigned backend) {
    syncstate_ensure_dirs();
    if (backend >= 2) return false;
    if (mkdir(BACKUP_QUEUE_DIR, 0777) && errno != EEXIST) return false;
    size_t count;
    uint64_t id = scan(NULL, 0, &count) + 1;
    char path[192], ready[192], data[224];
    snprintf(path, sizeof(path), BACKUP_QUEUE_DIR "/%020llu.building", (unsigned long long)id);
    snprintf(ready, sizeof(ready), BACKUP_QUEUE_DIR "/%020llu.job", (unsigned long long)id);
    if (mkdir(path, 0777)) return false;
    snprintf(data, sizeof(data), "%s/data", path);
    BackupJob job = {0};
    job.version = 1;
    job.backend = backend;
    job.title = *title;
    bool ok = syncjob_snapshot(title, data, &job.fingerprint) && write_job(path, &job);
    if (ok) ok = rename(path, ready) == 0;
    if (!ok) remove_tree(path);
    return ok;
}

size_t backupqueue_count(void) { size_t count; scan(NULL, 0, &count); return count; }

bool backupqueue_next(BackupJob *job, char *path, size_t size) {
    size_t count;
    scan(path, size, &count);
    return count && read_job(path, job);
}

bool backupqueue_retry(BackupJob *job, const char *path, uint64_t now) {
    if (job->attempts < 16) job->attempts++;
    unsigned shift = job->attempts > 7 ? 7 : job->attempts - 1;
    uint64_t delay = 30ULL << shift;
    if (delay > 3600) delay = 3600;
    job->due = now + delay;
    return write_job(path, job);
}

bool backupqueue_finish(const char *path) {
    char done[256];
    snprintf(done, sizeof(done), "%s.done", path);
    // Retirement is atomic. A crash during cleanup must never replay an old upload.
    if (rename(path, done)) return false;
    remove_tree(done); // Best-effort cleanup; startup retries retired directories.
    return true;
}

// Run before either worker. Only unpublished captures and atomically retired jobs
// are garbage; committed .job entries, including damaged manifests, stay intact.
void backupqueue_recover(void) {
    DIR *dir = opendir(BACKUP_QUEUE_DIR);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        unsigned long long id; char suffix[32];
        if (sscanf(entry->d_name, "%20llu.%31s", &id, suffix) != 2) continue;
        if (strcmp(suffix, "building") && strcmp(suffix, "job.done")) continue;
        char path[320];
        snprintf(path, sizeof(path), "%s/%s", BACKUP_QUEUE_DIR, entry->d_name);
        remove_tree(path);
    }
    closedir(dir);
}
