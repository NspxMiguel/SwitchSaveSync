#pragma once
#include "titles.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define BACKUP_QUEUE_DIR "sdmc:/switch/SwitchSaveSync/queue"
#define BACKUP_PAUSE_PATH "sdmc:/switch/SwitchSaveSync/uploads.paused"
#define BACKUP_DISABLE_PATH "sdmc:/switch/SwitchSaveSync/disabled.flag"
#define BACKUP_STATUS_PATH "sdmc:/switch/SwitchSaveSync/queue-status.txt"
typedef struct {
  uint32_t version;
  uint32_t backend;
  uint32_t attempts;
  uint32_t reserved;
  uint64_t due;
  uint64_t fingerprint;
  uint64_t checksum;
  TitleEntry title;
} BackupJob;
// Single producer, single consumer. Published jobs are immutable except retry
// metadata.
void backupqueue_recover(void);
bool backupqueue_enqueue(const TitleEntry *title, unsigned backend);
size_t backupqueue_count(void);
bool backupqueue_next(BackupJob *job, char *path, size_t size);
bool backupqueue_retry(BackupJob *job, const char *path, uint64_t now);
bool backupqueue_finish(const char *path);
bool backupqueue_paused(void);
bool backupqueue_set_paused(bool paused);
#ifdef __cplusplus
}
#endif
