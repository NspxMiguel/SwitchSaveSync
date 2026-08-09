// drive.h — operações na Google Drive API v3: achar/criar pastas (inclusive
// aninhadas), subir/baixar um arquivo, e subir/baixar uma árvore de
// diretório inteira (usado pra espelhar a estrutura de um save, que pode
// ter mais de um arquivo/subpasta).
#pragma once
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Garante que a pasta-raiz do app (DRIVE_APP_FOLDER_NAME, ver config.h)
// existe no Drive (cria se não existir) e devolve o id dela em out.
bool drive_ensure_app_folder(const char *access_token, char *folder_id_out, size_t outsz);

// Garante que existe uma subpasta 'name' dentro de parent_id (cria se não
// existir) e devolve o id dela em out. Usada pra criar
// "Nintendo Switch Saves/<Nome do Jogo>".
// Acha sem criar. Use esta em tudo que LÊ da nuvem — drive_ensure_subfolder
// cria a pasta quando não acha, o que num restore vira "backup vazio".
bool drive_find_subfolder(const char *access_token, const char *parent_id,
                           const char *name, char *out, size_t outsz);

bool drive_ensure_subfolder(const char *access_token, const char *parent_id,
                             const char *name, char *out, size_t outsz);

// Sobe local_path pro Drive dentro da pasta folder_id, com o nome
// remote_name. Se já existir um arquivo com esse nome na pasta, atualiza o
// conteúdo (PATCH) em vez de criar duplicata.
bool drive_upload(const char *access_token, const char *folder_id,
                   const char *remote_name, const char *local_path,
                   const char *mime_type);

// Baixa o arquivo remote_name (dentro de folder_id) pra local_path.
// Retorna false se o arquivo não existir na pasta.
bool drive_download(const char *access_token, const char *folder_id,
                     const char *remote_name, const char *local_path);

// Baixa um arquivo já identificado pelo id direto (usado internamente pela
// listagem em árvore, onde já se tem o id sem precisar buscar por nome).
bool drive_download_by_id(const char *access_token, const char *file_id, const char *local_path);

// Lista os filhos diretos (arquivos e subpastas) de folder_id, chamando cb
// pra cada um. Sem paginação — limitação conhecida, ver comentário no .c;
// não deveria importar pra pastas de save de jogo (poucos arquivos).
typedef void (*drive_list_cb)(const char *id, const char *name, bool is_folder, void *userdata);
bool drive_list_children(const char *access_token, const char *folder_id,
                          drive_list_cb cb, void *userdata);

// Callback opcional de progresso, chamado pelas funções _tree a cada
// arquivo/pasta processado, pra UI conseguir mostrar o que está acontecendo
// em vez de congelar numa tela só. action = "up"/"down"/"dir".
// Passe NULL pra desligar.
typedef void (*drive_progress_cb)(const char *action, const char *name, bool ok);
void drive_set_progress_cb(drive_progress_cb cb);

// Callback opcional de cancelamento: as funções _tree perguntam antes de cada
// arquivo, e param se ele devolver true (a função retorna false, como
// qualquer outra falha). Existe pro botão "Nao puxar save da nuvem nesse
// jogo" fazer efeito no meio de um download em vez de só na próxima vez.
// Parar no meio é seguro porque o download vai pro staging no cartão; quem
// escreve no save do jogo é o syncjob, e só depois do download inteiro.
typedef bool (*drive_abort_cb)(void);
void drive_set_abort_cb(drive_abort_cb cb);

// Sobe recursivamente todo o conteúdo de local_dir pra dentro de
// parent_folder_id, recriando a mesma estrutura de subpastas no Drive.
bool drive_upload_tree(const char *access_token, const char *parent_folder_id,
                        const char *local_dir);

// Baixa recursivamente todo o conteúdo de folder_id pra local_dir,
// recriando a mesma estrutura de subpastas localmente.
bool drive_download_tree(const char *access_token, const char *folder_id,
                          const char *local_dir);

#ifdef __cplusplus
}
#endif
