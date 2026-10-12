#include "backupqueue.h"
#include "cloud.h"
#include "syncjob.h"
#include "syncstate.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

void fake_cloud_reset(void);
void fake_save_reset(void);
extern int fake_puts, fake_commits, fake_mount_falha, fake_remove_falha;
Result timeGetCurrentTime(TimeType type, u64 *out) {
  (void)type;
  *out = 0;
  return 1;
}
bool lang_is_pt(void) { return false; }
static unsigned checks;
#define CHECK(condition)                                                       \
  do {                                                                         \
    checks++;                                                                  \
    assert(condition);                                                         \
  } while (0)
static void write_text(const char *path, const char *text) {
  FILE *file = fopen(path, "w");
  assert(file);
  assert(fputs(text, file) >= 0);
  assert(!fclose(file));
}
static void expect_text(const char *path, const char *text) {
  FILE *file = fopen(path, "r");
  assert(file);
  char buffer[64] = {0};
  fread(buffer, 1, sizeof(buffer) - 1, file);
  fclose(file);
  CHECK(!strcmp(buffer, text));
}
static bool deny_capture(u64 title) {
  (void)title;
  return false;
}
static char completed[700];
static bool checkpoint(const char *path, const char *folder, bool done) {
  (void)folder;
  if (done) {
    snprintf(completed, sizeof(completed), "%s", path);
    return true;
  }
  return !strcmp(path, completed);
}
static bool interrupt_after_one(void) { return completed[0] != 0; }

int main(void) {
  mkdir("sdmc:", 0777);
  mkdir("sdmc:/switch", 0777);
  syncstate_ensure_dirs();
  fake_cloud_reset();
  fake_save_reset();
  TitleEntry title = {0};
  title.application_id = 0x0100000000000020ULL;
  title.uid.uid[0] = 1;
  snprintf(title.name, sizeof(title.name), "Queue Test");
  snprintf(title.account, sizeof(title.account), "Player");
  write_text("save-falso/one.dat", "before launch");
  write_text("save-falso/two.dat", "second file");
  CHECK(backupqueue_count() == 0);
  fake_mount_falha = 1;
  CHECK(!backupqueue_enqueue(&title, CLOUD_DRIVE));
  CHECK(backupqueue_count() == 0);
  fake_mount_falha = 0;
  CHECK(!backupqueue_enqueue(&title, 99));
  syncjob_set_snapshot_guard(deny_capture);
  CHECK(!backupqueue_enqueue(&title, CLOUD_DRIVE) && backupqueue_count() == 0);
  syncjob_set_snapshot_guard(NULL);
  CHECK(backupqueue_enqueue(&title, CLOUD_DRIVE));
  CHECK(fake_puts == 0 &&
        fake_commits == 0); // Capture never uses cloud or write mounts.
  CHECK(backupqueue_count() == 1);
  BackupJob job;
  char path[192], data[224], file[256];
  CHECK(backupqueue_next(&job, path, sizeof(path)));
  CHECK(job.version == 1 && job.backend == CLOUD_DRIVE &&
        job.title.uid.uid[0] == 1);
  snprintf(data, sizeof(data), "%s/data", path);
  snprintf(file, sizeof(file), "%s/one.dat", data);
  expect_text(file, "before launch");
  write_text("save-falso/one.dat", "next session");
  u64 live, snapshot;
  CHECK(syncjob_fingerprint(&title, &live));
  CHECK(syncjob_snapshot_fingerprint(data, &snapshot));
  CHECK(snapshot == job.fingerprint && snapshot != live);
  // A fresh read is the reboot recovery path; deadlines persist on SD.
  CHECK(backupqueue_retry(&job, path, 1000));
  BackupJob recovered;
  CHECK(backupqueue_next(&recovered, path, sizeof(path)));
  CHECK(recovered.attempts == 1 && recovered.due == 1030);
  for (unsigned i = 0; i < 20; i++)
    CHECK(backupqueue_retry(&recovered, path, 1000));
  CHECK(recovered.attempts == 16 && recovered.due == 4600);
  CHECK(backupqueue_set_paused(true));
  CHECK(backupqueue_paused());
  CHECK(backupqueue_set_paused(false));
  CHECK(!backupqueue_paused());
  bool exact = true;
  cloud_set_checkpoint_cb(checkpoint);
  cloud_set_abort_cb(interrupt_after_one);
  CHECK(!syncjob_upload_snapshot(&job.title, data, NULL, &exact) && !exact);
  CHECK(fake_puts == 1);
  CHECK(backupqueue_count() == 1);
  // Resume the tree without uploading the completed file a second time.
  cloud_set_abort_cb(NULL);
  CHECK(syncjob_upload_snapshot(&job.title, data, NULL, &exact) && exact);
  CHECK(fake_puts == 2);
  expect_text("nuvem-falsa/Queue Test/Player/one.dat", "before launch");
  CHECK(fake_commits == 0);
  syncjob_mark_synced(&job.title, job.fingerprint);
  u64 marker;
  CHECK(syncjob_last_synced(&job.title, &marker) && marker == snapshot &&
        marker != live);
  // Failed prune must never report an exact mirror.
  cloud_set_checkpoint_cb(NULL);
  write_text("nuvem-falsa/Queue Test/Player/deleted.dat", "stale");
  fake_remove_falha = 1;
  CHECK(syncjob_upload_snapshot(&job.title, data, NULL, &exact) && !exact);
  fake_remove_falha = 0;
  CHECK(syncjob_upload_snapshot(&job.title, data, NULL, &exact) && exact);
  CHECK(access("nuvem-falsa/Queue Test/Player/deleted.dat", F_OK) != 0);
  // Another account remains a separate queued save and folder.
  title.shared_game = true;
  title.uid.uid[0] = 2;
  snprintf(title.account, sizeof(title.account), "Other Player");
  CHECK(backupqueue_enqueue(&title, CLOUD_WEBDAV));
  CHECK(backupqueue_count() == 2);
  CHECK(backupqueue_next(&recovered, file, sizeof(file)) &&
        recovered.title.uid.uid[0] == 1);
  CHECK(backupqueue_finish(path));
  CHECK(backupqueue_count() == 1);
  CHECK(backupqueue_next(&recovered, path, sizeof(path)) &&
        recovered.backend == CLOUD_WEBDAV && recovered.title.uid.uid[0] == 2);
  // Snapshot corruption changes its fingerprint; missing data fails closed.
  snprintf(file, sizeof(file), "%s/data/one.dat", path);
  write_text(file, "corrupt");
  snprintf(data, sizeof(data), "%s/data", path);
  CHECK(syncjob_snapshot_fingerprint(data, &snapshot) &&
        snapshot != recovered.fingerprint);
  CHECK(!syncjob_snapshot_fingerprint("missing-directory", &snapshot));
  CHECK(backupqueue_finish(path));
  CHECK(backupqueue_count() == 0);
  // Incomplete captures are never made visible to the uploader.
  mkdir(BACKUP_QUEUE_DIR "/00000000000000000001.building", 0777);
  CHECK(backupqueue_count() == 0 &&
        !backupqueue_next(&job, path, sizeof(path)));
  backupqueue_recover();
  CHECK(access(BACKUP_QUEUE_DIR "/00000000000000000001.building", F_OK) != 0);
  // A damaged manifest cannot redirect an upload to another title.
  CHECK(backupqueue_enqueue(&title, CLOUD_DRIVE));
  CHECK(backupqueue_next(&job, path, sizeof(path)));
  snprintf(file, sizeof(file), "%s/job.bin", path);
  FILE *manifest = fopen(file, "r+b");
  assert(manifest);
  job.title.application_id++;
  assert(fwrite(&job, sizeof(job), 1, manifest) == 1);
  fclose(manifest);
  CHECK(!backupqueue_next(&job, path, sizeof(path)) &&
        backupqueue_count() == 1);
  write_text(file, "truncated");
  CHECK(!backupqueue_next(&job, path, sizeof(path)) &&
        backupqueue_count() == 1);
  CHECK(backupqueue_finish(path));
  printf("%u queue checks passed\n", checks);
  return 0;
}
