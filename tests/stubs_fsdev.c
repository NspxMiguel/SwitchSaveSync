// O fsdev da libnx, de mentira, pra rodar o core/savemount.c no Mac.
//
// Metade do savemount.c é serviço do console (fsdevMountSaveData e amigos) e a
// outra metade é POSIX puro — opendir/readdir/unlink/rmdir/fopen. É essa
// segunda metade que APAGA O SAVE DO JOGO, e ela não tem nada de Switch: roda
// igualzinho aqui. Trocando só as chamadas de mount por estas, o MESMO arquivo
// que roda no console apanha de um teste de verdade.
//
// A montagem vira uma pasta chamada "save:" no diretório do teste, do mesmo
// jeito que o teste do FTP usa uma pasta "sdmc:". O caminho "save:/x" que o
// devoptab resolveria no console é, aqui, literalmente o arquivo "save:/x".
//
// O que este stub NÃO imita, e por isso não dá pra testar aqui:
//   - o mount somente-leitura. No console é o fs que recusa a escrita; aqui
//     unlink funciona igual, então um teste de "read_only protege" passaria
//     por engano. Ficou de fora de propósito.
//   - o commit. No console, sem fsdevCommitDevice a escrita não persiste;
//     aqui todo unlink já é definitivo. O que dá pra conferir — e o teste
//     confere — é se o savemount CHAMOU o commit ou não.

#include "switch.h"

#include <stdbool.h>
#include <string.h>
#include <sys/stat.h>

// O que aconteceu, pro teste perguntar depois.
int  fsdev_commits   = 0;
int  fsdev_unmounts  = 0;
int  fsdev_mounts    = 0;
bool fsdev_last_readonly = false;
bool fsdev_last_device   = false;

// O teste liga isto pra fingir "esse save não existe neste console".
bool fsdev_mount_falha = false;

static Result monta(bool read_only, bool device)
{
    fsdev_mounts++;
    fsdev_last_readonly = read_only;
    fsdev_last_device   = device;

    if (fsdev_mount_falha)
        return 1;

    mkdir("save:", 0777); // o container de save data existe antes do mount
    return 0;
}

Result fsdevMountSaveData(const char *name, u64 application_id, AccountUid uid)
{
    (void)name; (void)application_id; (void)uid;
    return monta(false, false);
}

Result fsdevMountSaveDataReadOnly(const char *name, u64 application_id, AccountUid uid)
{
    (void)name; (void)application_id; (void)uid;
    return monta(true, false);
}

Result fsdevMountDeviceSaveData(const char *name, u64 application_id)
{
    (void)name; (void)application_id;
    return monta(false, true);
}

Result fsdevCommitDevice(const char *name)
{
    (void)name;
    fsdev_commits++;
    return 0;
}

Result fsdevUnmountDevice(const char *name)
{
    (void)name;
    fsdev_unmounts++;
    return 0;
}

Result fsCreateSaveDataFileSystem(const FsSaveDataAttribute *attr,
                                  const FsSaveDataCreationInfo *info,
                                  const FsSaveDataMetaInfo *meta)
{
    (void)attr; (void)info; (void)meta;
    mkdir("save:", 0777);
    return 0;
}
