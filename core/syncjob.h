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
#include "nxsaves.h"   // só pelo NXSAVES_EXT, que é quem manda na extensão

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

// O nome da pasta desse save: o mesmo no Drive, no backup do cartao e dentro
// do arquivo unico. Exposto porque a interface precisa fazer o caminho
// contrario — dada uma pasta que veio de dentro de um arquivo, descobrir de
// qual save deste console ela e (ou que nao e de nenhum).
void syncjob_save_folder_name(const TitleEntry *title, char *out, size_t outsz);

// Quantos arquivos tem o backup deste save na nuvem. Serve pra barra de
// progresso saber a porcentagem em vez de só girar; 0 quando não tem backup.
//
// NUNCA cria nada. A versão que isto substituiu vivia no sysmodule e usava
// ensure em vez de find, então cada título sem backup ganhava uma pasta vazia
// no Drive de quem só queria ver uma barrinha — e, depois do layout aninhado,
// ela ainda contava a pasta do JOGO em vez da pasta da conta, com o .nxsaves e
// as outras contas entrando na conta do total.
int syncjob_cloud_file_count(const TitleEntry *title);

// O caminho deste save na nuvem, relativo a raiz do app: "<Jogo>/<Dono>".
//
// Publico pelo mesmo motivo que o syncjob_save_folder_name: da pra testar no
// Mac sem console nenhum, e esta e a regra que ja errou tres vezes num dia so.
void syncjob_cloud_folder_path(const TitleEntry *title, char *out, size_t outsz);

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
void syncjob_mark_synced(const TitleEntry *title, u64 fingerprint);
bool syncjob_last_synced(const TitleEntry *title, u64 *out);

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

// ---------------------------------------------------------------------------
// Tudo num arquivo só
//
// conversa privada removida do historico
// conversa privada removida do historico
// continua sendo uma pasta por jogo no Drive. Ver core/nxsaves.h.
// ---------------------------------------------------------------------------

// Nome do arquivo único no Drive e no cartão.
#define SYNC_ARCHIVE_NAME "SwitchSaveSync." NXSAVES_EXT

// Perguntado entre um jogo e outro: devolver true faz parar. Pode ser NULL.
// Parar aqui é seguro — nada foi escrito em save nenhum ainda.
typedef bool (*syncjob_stop_cb)(void);

// Onde o arquivo único fica no cartão.
void syncjob_archive_path(char *out, size_t outsz);

// Junta o save de todos esses jogos num arquivo só. Os saves são montados
// read-only e lidos DIRETO pro arquivo — nada é copiado pro cartão no meio do
// caminho, então isso não pede o dobro de espaço.
//
// Escreve num temporário e só troca pelo definitivo no fim: se falhar no meio,
// o arquivo de antes continua inteiro.
//
// Devolve quantos jogos entraram; 0 quer dizer que nada foi guardado.
size_t syncjob_archive_titles(const TitleEntry *titles, size_t count,
                               syncjob_log_cb log, syncjob_stop_cb stop);

// Sobe o arquivo único pro Drive (e o baixa de volta).
bool syncjob_archive_upload(syncjob_log_cb log);
bool syncjob_archive_download(syncjob_log_cb log);

// Existe arquivo único no cartão?
bool syncjob_has_archive(void);

// Passa por cada jogo que está dentro do arquivo do cartão.
typedef void (*syncjob_archive_cb)(const char *game_folder, u64 bytes, void *userdata);
bool syncjob_archive_list(syncjob_archive_cb cb, void *userdata);

// Tira o save desse jogo de dentro do arquivo único e escreve por cima do save
// do console. Mesma regra de sempre: o jogo NÃO pode estar aberto.
bool syncjob_archive_restore_title(const TitleEntry *title, syncjob_log_cb log);

// ---------------------------------------------------------------------------
// Um arquivo por jogo, com todas as contas dentro
//
// conversa privada removida do historico
// conversa privada removida do historico
// conversa privada removida do historico
// juntar as contas de um jogo não passa por cima do arquivo com tudo.
// ---------------------------------------------------------------------------

// Onde fica o arquivo desse jogo. O nome sai do nome do jogo, sem o sufixo de
// conta: o arquivo é do JOGO, e as contas são o que tem dentro dele.
void syncjob_game_archive_path(const TitleEntry *title, char *out, size_t outsz);

// As mesmas funções de cima, com o caminho do arquivo dito na mão. As de cima
// são estas aqui com o caminho do arquivo geral.
size_t syncjob_archive_titles_to(const char *path, const TitleEntry *titles, size_t count,
                                  syncjob_log_cb log, syncjob_stop_cb stop);
bool syncjob_archive_upload_path(const char *path, syncjob_log_cb log);

// O arquivo de UM jogo, pra dentro da pasta daquele jogo na nuvem — e não pra
// conversa privada removida do historico
// conversa privada removida do historico
bool syncjob_game_archive_upload(const TitleEntry *title, const char *path,
                                  syncjob_log_cb log);
bool syncjob_archive_list_path(const char *path, syncjob_archive_cb cb, void *userdata);

// Tira a pasta `folder` de dentro do arquivo e escreve por cima do save de
// `dest`.
//
// A diferença pro restore normal é que o dono do save que saiu do arquivo e o
// dono do save que vai receber podem ser CONTAS DIFERENTES. É o que faz um
// save vindo de outro console ter pra onde ir quando a conta original não
// existe aqui.
bool syncjob_archive_restore_folder(const char *path, const char *folder,
                                     const TitleEntry *dest, syncjob_log_cb log);

#ifdef __cplusplus
}
#endif
