// savemount.h — monta o save data de um jogo como dispositivo devoptab
// ("save:/"), pra ler/escrever nele com fopen/opendir normais, e copia
// árvores de arquivo entre ele e a pasta de staging local.
//
// O mount é por application_id + conta (AccountUid), usando os wrappers
// fsdevMountSaveData/fsdevMountSaveDataReadOnly da libnx — conferidos
// contra o header instalado.
#pragma once
#include <switch.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Monta o save do jogo application_id, da conta uid, no devoptab "save"
// (acessível como "save:/..."). Com read_only=true usa o mount somente
// leitura — use isso pro backup, é impossível corromper save assim.
bool savemount_mount(u64 application_id, AccountUid uid, bool read_only);

// Igual, mas escolhendo o TIPO de save data.
//
// Nem todo save do Switch pertence a uma conta. Existe o "device save", que é
// do console inteiro e não tem dono — é onde Animal Crossing guarda a ilha e
// onde Pokémon Sword/Shield guarda parte das coisas. Um app que só olha save
// de conta simplesmente não enxerga esses jogos (o JKSV tem o mesmo furo, com
// issue aberta). Pior: no Animal Crossing o save de conta é quase vazio, então
// dava pra "fazer backup" e não levar a ilha junto.
//
// Aviso que vale a pena saber: a libnx NÃO tem versão somente-leitura do mount
// de device save (só existe fsdevMountDeviceSaveData). Com device=true o
// read_only é ignorado e o mount sai read-write mesmo no backup — a proteção
// aqui passa a ser só o código não escrever nada, em vez de o sistema impedir.
bool savemount_mount_typed(u64 application_id, AccountUid uid, bool device, bool read_only);

// Desmonta o save. should_commit=true chama fsdevCommitDevice antes
// (obrigatório depois de ESCREVER, senão a escrita não persiste).
void savemount_unmount(bool should_commit);

// Apaga TODO o conteúdo do save montado (precisa ter sido montado com
// read_only=false, e precisa de savemount_unmount(true) depois pra valer).
//
// Apaga só os arquivos de dentro, não o container de save data em si — de
// propósito: o jogo continua tendo save data alocada, então dá pra montar e
// restaurar depois. Apagar o container (fsDeleteSaveDataFileSystem) deixaria
// o restore sem onde escrever até o jogo criar save nova.
bool savemount_wipe_contents(void);

// Copia recursivamente src_dir -> dst_dir, criando dst_dir e subpastas.
// Usado nos dois sentidos: save:/ -> staging e staging -> save:/.
bool savemount_copy_tree(const char *src_dir, const char *dst_dir);

#ifdef __cplusplus
}
#endif
