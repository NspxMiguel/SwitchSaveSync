// syncjob.h — "faz o backup desse jogo", do save montado até o Drive, num
// passo só.
//
// Existe porque agora tem dois programas querendo a mesma coisa: o app
// gráfico (quando o Miguel aperta o botão) e o sysmodule de autosync (quando
// o jogo fecha). A sequência é idêntica e não é curta — montar read-only,
// copiar pro staging, desmontar, achar/criar a pasta no Drive, subir a
// árvore — então mora aqui em vez de estar copiada nos dois.
#pragma once
#include <switch.h>
#include <stdbool.h>
#include "titles.h"

#ifdef __cplusplus
extern "C" {
#endif

// Recebe cada passo em texto, pra quem chamou mostrar na tela ou logar.
// Pode ser NULL.
typedef void (*syncjob_log_cb)(const char *msg);

// Acha, entre os jogos com save data, o que tem esse application_id.
// Precisa disso porque o sysmodule só descobre o application_id do jogo que
// fechou — e pra montar o save também é preciso a conta (AccountUid) e o
// nome, que só a enumeração dá.
bool syncjob_find_title(u64 application_id, TitleEntry *out);

// Backup completo de um jogo. O mount é sempre read-only: essa função não
// tem como corromper save, no pior caso ela falha.
bool syncjob_backup_title(const TitleEntry *title, syncjob_log_cb log);

// Backup local: copia o save pra SYNC_LOCAL_DIR/<jogo>, no proprio cartao.
// Nao encosta em rede nenhuma — funciona sem internet e sem conta Google.
// Mount read-only, igual ao backup pra nuvem: nao tem como estragar o save.
bool syncjob_backup_title_local(const TitleEntry *title, syncjob_log_cb log);

// Restaura do backup do cartao por cima do save do jogo. Mesma regra do
// restore da nuvem: o jogo NAO pode estar com o save aberto.
bool syncjob_restore_title_local(const TitleEntry *title, syncjob_log_cb log);

// Existe backup no cartao pra esse jogo?
bool syncjob_has_local_backup(const TitleEntry *title);

// Traz o save da nuvem e escreve por cima do save local. É a única função do
// projeto que escreve em save de jogo — quem chama tem que ter certeza de que
// o jogo NÃO está com o save aberto.
bool syncjob_restore_title(const TitleEntry *title, syncjob_log_cb log);

// "Impressão digital" do save: nome, tamanho e conteúdo de cada arquivo,
// misturados num u64. É hash de conteúdo de propósito — o mesmo save tem que
// dar o mesmo número dos dois lados, e arquivo baixado do Drive nasce com
// mtime de agora, então qualquer coisa com data na conta diria "diferente"
// mesmo quando os bytes são idênticos.
bool syncjob_fingerprint(const TitleEntry *title, u64 *out);

// Grava/lê a impressão digital do último upload bem-sucedido desse jogo.
void syncjob_mark_synced(u64 application_id, u64 fingerprint);
bool syncjob_last_synced(u64 application_id, u64 *out);

// O que a sincronização decidiu fazer.
typedef enum
{
    SYNCJOB_SYNC_FAILED = 0,  // erro no meio do caminho; nada foi decidido
    SYNCJOB_SYNC_NOTHING,     // não tem save nem aqui nem na nuvem
    SYNCJOB_SYNC_EQUAL,       // já estavam iguais; ninguém escreveu nada
    SYNCJOB_SYNC_UPLOADED,    // o save do console subiu pro Drive
    SYNCJOB_SYNC_DOWNLOADED,  // o save do Drive desceu pro console
    SYNCJOB_SYNC_CONFLICT,    // os dois mudaram: escolher sozinho apagaria progresso
} SyncjobSyncResult;

// Sincroniza o save desse jogo com o Drive num clique só, decidindo sozinho
// pra que lado vai. Quando não dá pra decidir com segurança (os dois lados
// mudaram desde a última sync), devolve CONFLICT e NÃO escreve em lugar
// nenhum — quem escolhe nessa hora é ele, pelo menu do Y.
SyncjobSyncResult syncjob_sync_title(const TitleEntry *title, syncjob_log_cb log);

#ifdef __cplusplus
}
#endif
