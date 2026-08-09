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
