// Background backup: lifecycle polling, local capture, and network work never
// share a loop.
#include "backupqueue.h"
#include "cloud.h"
#include "http.h"
#include "lang.h"
#include "oauth.h"
#include "syncjob.h"
#include "syncstate.h"
#include <dirent.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <switch.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define CAPTURE_DIR SYNC_APP_DIR "/capture"
#define INNER_HEAP_SIZE (8 * 1024 * 1024)
u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 2;
u32 __nx_time_service_type = TimeServiceType_System;
extern void __libnx_init_time(void);
static bool g_disabled, g_ready, g_socket_ok;
static Mutex g_pm_mutex, g_log_mutex, g_capture_mutex;
static Thread g_capture_thread, g_upload_thread;
static atomic_bool g_stopping;
static char g_last_result[128] = "Waiting for a snapshot";
static u64 g_status_tick;
static bool g_have_status;
static unsigned g_active_backend;
static char g_checkpoint_path[256];

void __libnx_initheap(void) {
  static char heap[INNER_HEAP_SIZE];
  extern void *fake_heap_start, *fake_heap_end;
  fake_heap_start = heap;
  fake_heap_end = heap + sizeof(heap);
}

static void log_line(const char *message) {
  mutexLock(&g_log_mutex);
  syncstate_log("%s", message);
  mutexUnlock(&g_log_mutex);
}

static void startup_error(const char *message) {
  log_line(message);
  syncstate_set_status("%s", message);
}

static bool disabled(void) { return access(BACKUP_DISABLE_PATH, F_OK) == 0; }

void __appInit(void) {
  if (R_FAILED(smInitialize()))
    return;
  if (R_FAILED(fsInitialize()) || R_FAILED(fsdevMountSdmc()))
    return;
  syncstate_ensure_dirs();
  // The kill switch is checked before any optional IPC session is opened.
  g_disabled = disabled();
  if (g_disabled) {
    syncstate_log("Disabled by disabled.flag; no PM, metadata, or network "
                  "services opened");
    syncstate_set_status("Disabled by disabled.flag");
    return;
  }
  Result rc = setsysInitialize();
  if (R_FAILED(rc)) {
    startup_error("set:sys initialization failed; refusing unknown firmware");
    return;
  }
  SetSysFirmwareVersion fw;
  rc = setsysGetFirmwareVersion(&fw);
  if (R_SUCCEEDED(rc))
    hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
  setsysExit();
  if (R_FAILED(rc) || hosversionBefore(6, 0, 0))
    return;
  if (R_FAILED(pmdmntInitialize()) || R_FAILED(pminfoInitialize())) {
    startup_error("PM initialization failed; lifecycle detection disabled");
    return;
  }
  // ns:am2 is used only for read-only title metadata, never for lifecycle
  // hooks.
  if (R_FAILED(nsInitialize()) ||
      R_FAILED(accountInitialize(AccountServiceType_Application))) {
    startup_error("Title/profile metadata unavailable; refusing to invent "
                  "cloud folder names");
    return;
  }
  lang_load();
  if (R_SUCCEEDED(timeInitialize()))
    __libnx_init_time();
  SocketInitConfig cfg = *socketGetDefaultInitConfig();
  cfg.tcp_tx_buf_size = 0x4000;
  cfg.tcp_rx_buf_size = 0x8000;
  cfg.tcp_tx_buf_max_size = 0x20000;
  cfg.tcp_rx_buf_max_size = 0x20000;
  cfg.udp_tx_buf_size = 0x800;
  cfg.udp_rx_buf_size = 0x1000;
  cfg.sb_efficiency = 1;
  cfg.num_bsd_sessions = 2;
  cfg.bsd_service_type = BsdServiceType_System;
  g_socket_ok = R_SUCCEEDED(socketInitialize(&cfg));
  g_ready = true;
}

void __appExit(void) {
  if (g_socket_ok)
    socketExit();
  accountExit();
  nsExit();
  pminfoExit();
  pmdmntExit();
  timeExit();
  fsdevUnmountAll();
  fsExit();
  smExit();
}

// Only PM's documented ProcessNotFound is interpreted as an exit. Other errors
// preserve the previous state instead of mounting a potentially live save.
static bool running_title(u64 *title, u64 *process) {
  mutexLock(&g_pm_mutex);
  u64 pid = 0, tid = 0;
  Result rc = pmdmntGetApplicationProcessId(&pid);
  bool ok = R_SUCCEEDED(rc) || rc == MAKERESULT(15, 1);
  if (R_SUCCEEDED(rc) && pid)
    ok = R_SUCCEEDED(pminfoGetProgramId(&tid, pid));
  mutexUnlock(&g_pm_mutex);
  if (!ok)
    return false;
  *title = tid;
  *process = pid;
  return true;
}

static bool capture_allowed(u64 title) {
  u64 live, pid;
  return !disabled() && running_title(&live, &pid) && live != title;
}

static bool request_capture(u64 title) {
  char path[160];
  snprintf(path, sizeof(path), CAPTURE_DIR "/%016llX.pending",
           (unsigned long long)title);
  mutexLock(&g_capture_mutex);
  FILE *file = fopen(path, "w");
  bool ok = file != NULL;
  if (file) {
    if (fflush(file) || fsync(fileno(file)))
      ok = false;
    if (fclose(file))
      ok = false;
  }
  mutexUnlock(&g_capture_mutex);
  return ok;
}

static bool uploads_paused(void) {
  return atomic_load(&g_stopping) || disabled() || backupqueue_paused() ||
         !syncstate_dest_cloud();
}

static void queue_status(int percent, bool uploading) {
  u64 now = armGetSystemTick();
  if (g_have_status && armTicksToNs(now - g_status_tick) < 1000000000ULL &&
      percent != 100)
    return;
  g_have_status = true;
  g_status_tick = now;
  static char previous[256];
  char state[256];
  snprintf(state, sizeof(state), "%zu %d %d\n%s\n", backupqueue_count(),
           uploading, percent, g_last_result);
  if (!strcmp(previous, state))
    return;
  char temp[160];
  snprintf(temp, sizeof(temp), "%s.tmp", BACKUP_STATUS_PATH);
  FILE *file = fopen(temp, "w");
  if (!file)
    return;
  fputs(state, file);
  bool ok = !ferror(file);
  if (fclose(file))
    ok = false;
  if (ok && !rename(temp, BACKUP_STATUS_PATH))
    snprintf(previous, sizeof(previous), "%s", state);
}

static bool transfer_progress(uint64_t total, uint64_t sent) {
  if (uploads_paused())
    return false;
  int percent = total ? (int)((double)sent * 100.0 / (double)total) : 0;
  if (percent > 99)
    percent = 99;
  queue_status(percent, true);
  return true;
}

static void capture_worker(void *unused) {
  (void)unused;
  static TitleEntry
      titles[17]; // 16 saves + overflow detection, including device saves.
  u64 last_failure = 0;
  while (!atomic_load(&g_stopping) && !disabled()) {
    if (last_failure &&
        armTicksToNs(armGetSystemTick() - last_failure) < 30000000000ULL) {
      svcSleepThread(1000000000ULL);
      continue;
    }
    last_failure = 0;
    DIR *dir = opendir(CAPTURE_DIR);
    struct dirent *entry;
    while (dir && (entry = readdir(dir))) {
      unsigned long long id;
      char suffix[16];
      if (sscanf(entry->d_name, "%16llX.%15s", &id, suffix) != 2 ||
          (strcmp(suffix, "pending") && strcmp(suffix, "capturing")))
        continue;
      u64 live, pid;
      if (!running_title(&live, &pid) || live == id)
        continue;
      char path[160], active[160];
      snprintf(path, sizeof(path), CAPTURE_DIR "/%016llX.%s", id, suffix);
      snprintf(active, sizeof(active), CAPTURE_DIR "/%016llX.capturing", id);
      mutexLock(&g_capture_mutex);
      bool claimed = !strcmp(suffix, "capturing") ||
                     (access(active, F_OK) != 0 && rename(path, active) == 0);
      mutexUnlock(&g_capture_mutex);
      if (!claimed)
        continue;
      size_t count = titles_list_for_application(id, titles, 17);
      bool ok = count > 0 && count < 17;
      for (size_t i = 0; ok && i < count; i++) {
        if (!running_title(&live, &pid) || live == id || disabled()) {
          ok = false;
          break;
        }
        // A failed metadata lookup must not create a new fallback cloud layout.
        if (!strncmp(titles[i].name, "Titulo desconhecido", 19) ||
            (!titles[i].device_save && !titles[i].account[0])) {
          ok = false;
          break;
        }
        char account_key[80], captured[2048] = {0};
        snprintf(account_key, sizeof(account_key), "%016llX%016llX:%d\n",
                 (unsigned long long)titles[i].uid.uid[0],
                 (unsigned long long)titles[i].uid.uid[1],
                 titles[i].device_save);
        FILE *intent = fopen(active, "r");
        if (intent) {
          fread(captured, 1, sizeof(captured) - 1, intent);
          fclose(intent);
        }
        if (strstr(captured, account_key))
          continue;
        if (syncstate_dest_cloud())
          ok = backupqueue_enqueue(&titles[i], g_active_backend);
        if (ok && syncstate_dest_local())
          ok = syncjob_backup_title_local(&titles[i], NULL);
        if (ok) {
          intent = fopen(active, "a");
          ok = intent != NULL;
          if (intent) {
            if (fputs(account_key, intent) == EOF || fflush(intent) ||
                fsync(fileno(intent)))
              ok = false;
            if (fclose(intent))
              ok = false;
          }
        }
      }
      if (ok) {
        unlink(active);
        log_line("Closed-title snapshot captured for all accounts");
      } else {
        last_failure = armGetSystemTick();
        log_line("Snapshot incomplete; capture intent retained (save busy, "
                 "metadata, or SD error)");
      }
      if (atomic_load(&g_stopping))
        break;
    }
    if (dir)
      closedir(dir);
    svcSleepThread(1000000000ULL);
  }
}

static bool checkpoint(const char *path, const char *folder, bool completed) {
  char key[1400];
  if (snprintf(key, sizeof(key), "%s\t%s", folder, path) >= (int)sizeof(key))
    return false;
  FILE *file = fopen(g_checkpoint_path, completed ? "a" : "r");
  if (!file)
    return false;
  bool ok = false;
  if (completed) {
    ok =
        fprintf(file, "%s\n", key) > 0 && !fflush(file) && !fsync(fileno(file));
  } else {
    char line[1400];
    while (fgets(line, sizeof(line), file)) {
      size_t n = strlen(line);
      if (n && line[n - 1] == '\n')
        line[n - 1] = 0;
      if (!strcmp(line, key)) {
        ok = true;
        break;
      }
    }
  }
  if (fclose(file))
    ok = false;
  return ok;
}

static void upload_worker(void *unused) {
  (void)unused;
  bool network = g_socket_ok && http_init();
  if (!network)
    log_line("Network initialization failed; snapshots remain queued until "
             "module restart");
  else if (!http_ca_bundle()[0])
    log_line("Missing cacert.pem; open the app once to copy the TLS CA bundle");
  http_set_transfer_cb(transfer_progress);
  http_set_upload_limit(128 *
                        1024); // Bytes/s: preserve bandwidth for gameplay.
  cloud_set_abort_cb(uploads_paused);
  while (!atomic_load(&g_stopping) && !disabled()) {
    BackupJob job;
    char path[192];
    if (!uploads_paused() && network &&
        backupqueue_next(&job, path, sizeof(path))) {
      uint64_t now = (uint64_t)time(NULL);
      if (job.backend != g_active_backend) {
        snprintf(g_last_result, sizeof(g_last_result),
                 "Provider mismatch; restart with the queued provider");
      } else if (syncstate_is_excluded(job.title.application_id)) {
        snprintf(g_last_result, sizeof(g_last_result),
                 "Excluded title; queue retained");
      } else if (job.due <= now) {
        char data[224];
        snprintf(data, sizeof(data), "%s/data", path);
        u64 fingerprint;
        bool exact = false;
        snprintf(g_checkpoint_path, sizeof(g_checkpoint_path),
                 "%s/uploaded.txt", path);
        cloud_set_checkpoint_cb(checkpoint);
        snprintf(g_last_result, sizeof(g_last_result), "Uploading %016llX",
                 (unsigned long long)job.title.application_id);
        queue_status(0, true);
        bool login = job.backend != CLOUD_DRIVE || oauth_load_saved_login();
        bool ok = login && syncjob_snapshot_fingerprint(data, &fingerprint) &&
                  fingerprint == job.fingerprint &&
                  syncjob_upload_snapshot(&job.title, data, log_line, &exact) &&
                  exact;
        if (ok) {
          // Record the snapshot's hash, never the live save after a later game
          // session.
          ok = syncjob_mark_synced_checked(&job.title, job.fingerprint);
          if (ok)
            ok = backupqueue_finish(path);
        }
        if (ok) {
          snprintf(g_last_result, sizeof(g_last_result), "Upload complete");
          log_line("Snapshot upload and prune complete; synchronized "
                   "fingerprint recorded");
        } else {
          if (!backupqueue_retry(&job, path, now))
            log_line("Could not persist retry deadline; snapshot retained");
          snprintf(g_last_result, sizeof(g_last_result),
                   "Interrupted/failed; retry in %llu s",
                   (unsigned long long)(job.due - now));
          log_line(g_last_result);
        }
        queue_status(ok ? 100 : 0, false);
      }
    } else if (!uploads_paused() && network && backupqueue_count()) {
      if (strcmp(g_last_result,
                 "Queue needs repair; damaged manifest retained"))
        log_line("Queue needs repair; damaged manifest retained");
      snprintf(g_last_result, sizeof(g_last_result),
               "Queue needs repair; damaged manifest retained");
    }
    queue_status(-1, false);
    svcSleepThread(1000000000ULL);
  }
  http_set_transfer_cb(NULL);
  cloud_set_abort_cb(NULL);
  cloud_set_checkpoint_cb(NULL);
  http_shutdown();
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;
  if (g_disabled || !g_ready)
    return 0;
  mkdir(CAPTURE_DIR, 0777);
  syncjob_set_snapshot_guard(capture_allowed);
  backupqueue_recover();
  g_active_backend = cloud_current(); // Pin provider until restart; queued jobs
                                      // retain their provider.
  syncstate_set_status(
      TR("Ativo — backup em segundo plano", "Active — background backup"));
  log_line("Background backup started: 8 MiB heap, 1 Hz PM polling, no applet "
           "or display hooks");
  Result rc = threadCreate(&g_capture_thread, capture_worker, NULL, NULL,
                           0x20000, 50, 3);
  if (R_FAILED(rc)) {
    log_line("Capture thread creation failed");
    return 0;
  }
  rc = threadStart(&g_capture_thread);
  if (R_FAILED(rc)) {
    threadClose(&g_capture_thread);
    return 0;
  }
  rc =
      threadCreate(&g_upload_thread, upload_worker, NULL, NULL, 0x20000, 59, 3);
  bool uploader = R_SUCCEEDED(rc);
  if (uploader && R_FAILED(threadStart(&g_upload_thread))) {
    threadClose(&g_upload_thread);
    uploader = false;
  }
  if (!uploader)
    log_line("Upload thread unavailable; local capture continues");
  u64 previous_title = 0, previous_pid = 0;
  while (!disabled()) {
    u64 title, pid;
    if (running_title(&title, &pid)) {
      if (previous_title && previous_pid != pid &&
          syncstate_autosync_enabled() &&
          !syncstate_is_excluded(previous_title) &&
          (syncstate_dest_cloud() || syncstate_dest_local())) {
        if (!request_capture(previous_title)) {
          log_line("Could not persist capture intent; retrying next poll");
          svcSleepThread(1000000000ULL);
          continue;
        }
        syncstate_set_status(
            TR("Backup em segundo plano", "Backing up in the background"));
      }
      previous_title = title;
      previous_pid = pid;
      u64 asked;
      if (syncstate_take_request(&asked)) {
        if (!pid)
          request_capture(asked);
        else
          log_line("Manual capture refused while an application is running");
      }
    }
    svcSleepThread(1000000000ULL);
  }
  atomic_store(&g_stopping, true);
  threadWaitForExit(&g_capture_thread);
  threadClose(&g_capture_thread);
  if (uploader) {
    threadWaitForExit(&g_upload_thread);
    threadClose(&g_upload_thread);
  }
  log_line("Stopped by disabled.flag; unfinished snapshots remain on SD");
  return 0;
}
