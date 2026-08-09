// cloud_backend.h — o contrato que cada nuvem preenche.
//
// Separado do cloud.h de propósito: cloud.h é o que o resto do programa usa
// ("sobe essa pasta"), isto aqui é o que só o drive.c, o webdav.c e o próprio
// cloud.c enxergam. Quem for escrever a próxima nuvem mexe só neste arquivo e
// no seu.
#pragma once
#include "cloud.h"

#ifdef __cplusplus
extern "C" {
#endif

// Um item dentro de uma pasta da nuvem. É o mesmo do cloud.h com outro
// nome: lá ele se chama cloud_item_cb porque quem inclui cloud.h não
// enxerga este arquivo.
typedef cloud_item_cb cloud_list_cb;

typedef struct
{
    const char *key;    // "drive" — o que vai gravado no nuvem.cfg

    // "Google Drive" — o que ele lê na tela. É função, e não string fixa,
    // porque o nome passa pelo TR(): o campo de um struct estático não aceita
    // uma escolha feita em tempo de execução.
    const char *(*name)(void);

    // Dá pra usar agora? Quando não, setup_hint diz o que falta, numa frase
    // curta que a tela mostra embaixo do nome.
    bool (*is_ready)(void);
    const char *(*setup_hint)(void);
    void (*logout)(void);

    // Abre a sessão. 'auth' é um buffer de CLOUD_AUTH_MAX de quem chamou, e o
    // que vai dentro só interessa pro próprio backend (access_token no Drive,
    // header Basic no WebDAV).
    bool (*begin)(char *auth, size_t authsz);

    // A pasta-raiz do app, criada se não existir.
    bool (*root)(const char *auth, char *id_out, size_t outsz);

    // Acha um filho de parent_id pelo nome. want_folder separa pasta de
    // arquivo: sem isso, um jogo chamado igual a um arquivo confundiria os
    // dois.
    bool (*find_child)(const char *auth, const char *parent_id, const char *name,
                        bool want_folder, char *id_out, size_t outsz);

    bool (*make_folder)(const char *auth, const char *parent_id, const char *name,
                         char *id_out, size_t outsz);

    // Sobe local_path pra dentro de parent_id com o nome 'name'. Se já existe
    // um arquivo com esse nome lá, substitui — não duplica.
    bool (*put_file)(const char *auth, const char *parent_id, const char *name,
                      const char *local_path, const char *mime_type);

    bool (*get_file)(const char *auth, const char *id, const char *local_path);

    // Tira da nuvem. Pra lixeira quando a nuvem tiver uma; ver o aviso no
    // cloud.h — isto aqui mexe em backup de save, e engano tem que ter volta.
    bool (*remove)(const char *auth, const char *id);

    bool (*list)(const char *auth, const char *folder_id,
                  cloud_list_cb cb, void *userdata);
} CloudBackend;

const CloudBackend *drive_backend(void);
const CloudBackend *webdav_backend(void);

#ifdef __cplusplus
}
#endif
