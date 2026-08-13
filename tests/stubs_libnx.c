// O pouco de libnx que o ftpd.c usa, traduzido pro Mac.
//
// O servidor de FTP é socket BSD do começo ao fim — a única coisa de console
// nele são a thread e o mutex da libnx. Trocando os dois por pthread, o MESMO
// arquivo que roda no Switch roda aqui e pode apanhar de um cliente de FTP de
// verdade.
//
// Isto não é um simulador de Switch: é o mínimo pra ligar o ftpd.c. Nada aqui
// deve virar muleta pra código novo.

#include "switch.h"

#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

// --- thread ---------------------------------------------------------------

Result threadCreate(Thread *t, void (*entry)(void *), void *arg, void *stack,
                    size_t stack_size, int prio, int cpuid)
{
    (void)stack; (void)stack_size; (void)prio; (void)cpuid;
    t->entry = entry;
    t->arg   = arg;
    return 0;
}

static void *ponte(void *p)
{
    Thread *t = (Thread *)p;
    t->entry(t->arg);
    return NULL;
}

Result threadStart(Thread *t)
{
    return pthread_create(&t->id, NULL, ponte, t) == 0 ? 0 : 1;
}

Result threadWaitForExit(Thread *t)
{
    pthread_join(t->id, NULL);
    return 0;
}

Result threadClose(Thread *t) { (void)t; return 0; }

// --- mutex ----------------------------------------------------------------
//
// O Mutex da libnx é um u32 que começa zerado e não precisa de init. Pra imitar
// isso sem exigir inicialização, um mutex de processo inteiro basta: o teste
// tem uma thread de servidor e a principal, e a disputa é por dois contadores.

static pthread_mutex_t g_um = PTHREAD_MUTEX_INITIALIZER;

void mutexLock(Mutex *m)   { (void)m; pthread_mutex_lock(&g_um); }
void mutexUnlock(Mutex *m) { (void)m; pthread_mutex_unlock(&g_um); }

// --- svc ------------------------------------------------------------------

void svcSleepThread(u64 nanos)
{
    struct timespec t = { (time_t)(nanos / 1000000000ULL), (long)(nanos % 1000000000ULL) };
    nanosleep(&t, NULL);
}

// --- rede -----------------------------------------------------------------
//
// gethostid() no Mac devolve algo que não é o IP da máquina. O ftpd usa esse
// valor só pra montar a resposta do PASV, e no teste o cliente é o próprio
// computador — então 127.0.0.1 é a resposta certa aqui.

long gethostid(void)
{
    return (long)htonl(0x7F000001); // 127.0.0.1 em ordem de rede, como no console
}
