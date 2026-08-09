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

// "Impressão digital" do save local: soma dos tamanhos e o mtime mais novo,
// misturados num u64. Não é hash de conteúdo (ler o save inteiro a cada boot
// de jogo seria caro demais) — serve só pra responder "mudou desde o último
// upload?", que é o suficiente pra decidir se pode puxar da nuvem sem
// atropelar progresso que ainda não subiu.
bool syncjob_fingerprint(const TitleEntry *title, u64 *out);

// Grava/lê a impressão digital do último upload bem-sucedido desse jogo.
void syncjob_mark_synced(u64 application_id, u64 fingerprint);
bool syncjob_last_synced(u64 application_id, u64 *out);

#ifdef __cplusplus
}
#endif
