// drive.h — o que só o Google Drive sabe fazer: montar a query da API v3,
// o upload multipart, a lixeira.
//
// Percorrer árvore e espelhar pasta NÃO está mais aqui — não tinha nada de
// Google e virou o cloud.c, que faz isso por cima de qualquer nuvem. Quem
// quer "sobe essa pasta" chama cloud.h; isto aqui é o motor de baixo.
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

// Manda um arquivo/pasta do Drive pra lixeira pelo id (pasta leva o conteúdo
// junto). Não é o DELETE da API — ver drive.c pro porquê.
bool drive_trash_by_id(const char *access_token, const char *file_id);

#ifdef __cplusplus
}
#endif
