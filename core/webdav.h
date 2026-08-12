// webdav.h — guardar o save num servidor WebDAV.
//
// Entre as nuvens candidatas, o WebDAV é o que rende mais por linha escrita: é
// o mesmo protocolo do Nextcloud, do ownCloud, do Synology (Drive/WebDAV
// Server), do QNAP, do Box e de qualquer NAS com WebDAV ligado. Uma
// implementação, todos esses.
//
// E é o único que não depende de nada de fora: OneDrive e Dropbox exigem
// registrar um app no portal deles pra ter client_id. Aqui basta o endereço e
// a senha, que quem tem o servidor já tem.
//
// Só o que o webdav.c precisa de fora está aqui — a tela de Ajustes lê e
// grava os dados de acesso, e o resto do programa fala com ele pelo cloud.h,
// sem saber que é WebDAV.
#pragma once
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WEBDAV_URL_MAX  384
#define WEBDAV_USER_MAX 128
#define WEBDAV_PASS_MAX 192

typedef struct
{
    char url[WEBDAV_URL_MAX];    // ex: https://nas.local/remote.php/dav/files/usuario
    char user[WEBDAV_USER_MAX];
    char pass[WEBDAV_PASS_MAX];
} WebdavConfig;

// Lê/grava sdmc:/switch/SwitchSaveSync/webdav.cfg. O get devolve false quando
// não tem nada gravado ainda.
bool webdav_get_config(WebdavConfig *out);
void webdav_set_config(const WebdavConfig *cfg);
void webdav_clear_config(void);

// Bate na raiz do servidor com os dados salvos e diz se respondeu. Serve pro
// botão "Testar conexão" — sem isso, endereço digitado errado só aparece no
// meio de um backup.
//
// 'erro' recebe uma frase curta pra mostrar na tela quando der false.
bool webdav_test_connection(char *erro, size_t errosz);

#ifdef __cplusplus
}
#endif
