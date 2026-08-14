// switch.h de mentira, so pra compilar core/ no Mac.
// Os headers do projeto incluem <switch.h> por causa dos tipos u8..u64. E so
// isso que eles usam; nada de servico do console entra nos testes.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <time.h>   // o <switch.h> de verdade puxa este; o syncstate.c conta com isso

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   s8;
typedef int16_t  s16;
typedef int32_t  s32;
typedef int64_t  s64;
typedef uint32_t Result;

// igual ao da libnx (services/acc.h): dois u64.
typedef struct {
    u64 uid[2];
} AccountUid;

// O syncstate carimba a data com o relogio do console. No Mac nao ha console,
// entao o teste linka a sua propria versao disto.
#define R_SUCCEEDED(res) ((res) == 0)
#define R_FAILED(res)    ((res) != 0)
typedef enum { TimeType_UserSystemClock = 0, TimeType_NetworkSystemClock = 1,
               TimeType_LocalSystemClock = 2 } TimeType;
Result timeGetCurrentTime(TimeType type, u64 *out);

// O oauth.c dorme entre uma pergunta e outra ao Google.
void svcSleepThread(u64 nanos);

// --- o pouco que o servidor de FTP precisa ---------------------------------
// Ver tests/stubs_libnx.c: thread e mutex viram pthread no Mac.
#include <pthread.h>
#include <stddef.h>

typedef struct {
    void (*entry)(void *);
    void *arg;
    pthread_t id;
} Thread;

typedef u32 Mutex;

Result threadCreate(Thread *t, void (*entry)(void *), void *arg, void *stack,
                    size_t stack_size, int prio, int cpuid);
Result threadStart(Thread *t);
Result threadWaitForExit(Thread *t);
Result threadClose(Thread *t);

void mutexLock(Mutex *m);
void mutexUnlock(Mutex *m);

// --- o pouco de fs/fsdev que o core/savemount.c precisa --------------------
// Ver tests/stubs_fsdev.c: o mount vira uma pasta chamada "save:" no
// diretorio do teste, e o commit vira um contador.

typedef enum { FsSaveDataType_System = 0, FsSaveDataType_Account = 1 } FsSaveDataType;
typedef enum { FsSaveDataSpaceId_System = 0, FsSaveDataSpaceId_User = 1 } FsSaveDataSpaceId;
typedef enum { FsSaveDataMetaType_None = 0, FsSaveDataMetaType_Thumbnail = 1 } FsSaveDataMetaType;

typedef struct {
    u64 application_id;
    AccountUid uid;
    u64 system_save_data_id;
    u8  save_data_type;
    u8  save_data_rank;
    u16 save_data_index;
    u32 pad_x24;
    u64 unk_x28;
    u64 unk_x30;
    u64 unk_x38;
} FsSaveDataAttribute;

typedef struct {
    s64 save_data_size;
    s64 journal_size;
    u64 available_size;
    u64 owner_id;
    u32 flags;
    u8  save_data_space_id;
    u8  unk;
    u8  padding[0x1A];
} FsSaveDataCreationInfo;

typedef struct {
    u32 size;
    u8  type;
    u8  padding[0x0B];
} FsSaveDataMetaInfo;

Result fsdevMountSaveData(const char *name, u64 application_id, AccountUid uid);
Result fsdevMountSaveDataReadOnly(const char *name, u64 application_id, AccountUid uid);
Result fsdevMountDeviceSaveData(const char *name, u64 application_id);
Result fsdevCommitDevice(const char *name);
Result fsdevUnmountDevice(const char *name);
Result fsCreateSaveDataFileSystem(const FsSaveDataAttribute *attr,
                                  const FsSaveDataCreationInfo *info,
                                  const FsSaveDataMetaInfo *meta);
