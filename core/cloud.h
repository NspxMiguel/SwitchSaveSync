// cloud.h — "a nuvem", sem dizer qual.
//
// conversa privada removida do historico
// conversa privada removida do historico
// conversa privada removida do historico
//
// Até aqui o syncjob.c chamava drive_* direto, então o Google Drive não era
// uma opção — era a única forma do programa saber guardar arquivo. Isto aqui
// é a mesma lista de operações que o drive.h já tinha, com o "quem faz" saindo
// de uma tabela em vez de estar escrito no código.
//
// O que cada nuvem precisa implementar são SETE coisas pequenas (achar filho,
// criar pasta, subir arquivo, baixar arquivo, listar, apagar, abrir sessão).
// Subir/baixar/espelhar uma árvore inteira é escrito uma vez só, aqui, em
// cima dessas sete — era metade do drive.c e não tem nada de Google nele.
//
// Sobre o iCloud, que ele perguntou: não dá, e não é falta de vontade. A Apple
// não tem API pública de terceiro pro iCloud Drive. O que existe é o CloudKit,
// que só responde pra um app com identidade da Apple, e o CloudKit Web
// Services exige uma conta paga do Apple Developer Program — o que também
// bateria de frente com "custo zero", que é regra do projeto. Está escrito no
// CLOUD_ICLOUD abaixo pra não se perder.
#pragma once
#include <stddef.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// Um id de pasta/arquivo. No Google Drive é opaco ("1a2B3c4D..."); no WebDAV
// e no Dropbox é o caminho ("/SwitchSaveSync/Zelda"). Cabe nos dois.
#define CLOUD_ID_MAX   512

// O que uma sessão carrega: no Drive é o access_token (o do Google passa de
// 1 KB), no WebDAV é o header Basic já montado.
#define CLOUD_AUTH_MAX 2048

typedef enum
{
    CLOUD_DRIVE = 0,   // Google Drive
    CLOUD_WEBDAV,      // NAS, Nextcloud, ownCloud, Synology, QNAP, Box
    CLOUD_COUNT,

    // Ainda não existem. Ficam listadas aqui, e não numa nota solta, porque
    // a pergunta "cadê o OneDrive?" vem do menu — e a resposta tem que estar
    // do lado do enum que o menu percorre.
    //
    // CLOUD_ONEDRIVE — dá: Microsoft Graph, com o mesmo device flow do
    //   Google. Precisa de um registro de app no portal da Azure (grátis).
    // CLOUD_DROPBOX  — dá: API /2/files, caminho em vez de id, mais simples
    //   que o Drive. Precisa de um app criado em dropbox.com/developers.
    // CLOUD_ICLOUD   — NÃO dá. Ver o comentário no topo do arquivo.
} CloudKind;

// --- qual nuvem está valendo -------------------------------------------
// Fica gravado em SYNC_APP_DIR/nuvem.cfg. Sem o arquivo, vale o Google Drive
// — que é o que sempre foi, então quem já usava não vê diferença.
CloudKind   cloud_current(void);
void        cloud_set_current(CloudKind kind);

const char *cloud_name(CloudKind kind);       // "Google Drive"
const char *cloud_key(CloudKind kind);        // "drive" (o que vai no arquivo)

// Já dá pra usar? (o Drive quer login feito; o WebDAV quer endereço e senha
// guardados). Quando é false, cloud_setup_hint diz o que falta, em português
// de tela.
bool        cloud_is_ready(CloudKind kind);
const char *cloud_setup_hint(CloudKind kind);

// Esquece o login/os dados de acesso dessa nuvem.
void        cloud_logout(CloudKind kind);

// --- operações, na nuvem que está valendo ------------------------------
//
// Todas recebem 'auth', que sai do cloud_begin. É o mesmo desenho que o
// drive.c já usava com o access_token: pega um no começo de cada trabalho e
// passa adiante, em vez de guardar estado global.

// Abre a sessão. No Drive troca o refresh_token por um access_token novo; no
// WebDAV monta o header Basic a partir do que está salvo. Devolve false
// quando não dá pra falar com a nuvem (sem login, sem rede, senha errada).
bool cloud_begin(char *auth, size_t authsz);

// A pasta-raiz do app, criada se não existir.
bool cloud_ensure_app_folder(const char *auth, char *id_out, size_t outsz);

// Subpasta dentro de outra. 'ensure' cria quando não acha; 'find' não cria.
//
// Quem LÊ da nuvem usa find. Isso não é preciosismo: o restore já usou ensure
// uma vez, e ensure devolveu uma pasta vazia recém-criada em vez de "não tem
// backup" — e o restore seguiu em frente e escreveu o nada por cima de um
// save bom.
bool cloud_ensure_subfolder(const char *auth, const char *parent_id,
                             const char *name, char *id_out, size_t outsz);
bool cloud_find_subfolder(const char *auth, const char *parent_id,
                           const char *name, char *id_out, size_t outsz);

// Um arquivo só, pra dentro/pra fora de uma pasta.
bool cloud_upload(const char *auth, const char *folder_id, const char *remote_name,
                   const char *local_path, const char *mime_type);
bool cloud_download(const char *auth, const char *folder_id, const char *remote_name,
                     const char *local_path);

// Tira um arquivo (não pasta) de dentro de uma pasta. Devolve true também
// quando não achou: o que se queria era "esse arquivo não está mais lá", e ele
// não está. Só dá false se a nuvem recusou de verdade.
//
// Existe pro teste de conexão limpar o arquivinho que ele mesmo sobe. Não é
// pra apagar backup — pra isso tem o prune_extras, que sabe o que é sobra.
bool cloud_delete_file(const char *auth, const char *folder_id, const char *remote_name);

// Uma pasta local inteira, recriando as subpastas do outro lado.
//
// upload_tree só ESCREVE: arquivo que sumiu do save continua na nuvem. Quem
// fecha o espelho é o prune_extras.
bool cloud_upload_tree(const char *auth, const char *folder_id, const char *local_dir);
bool cloud_download_tree(const char *auth, const char *folder_id, const char *local_dir);

// Os filhos diretos de uma pasta, um por chamada de cb. Quem usa é quem só
// quer contar ("quantos arquivos tem lá?") — as duas de cima já listam por
// dentro.
typedef void (*cloud_item_cb)(const char *id, const char *name, bool is_folder,
                               void *userdata);
bool cloud_list_children(const char *auth, const char *folder_id,
                          cloud_item_cb cb, void *userdata);

// Tira da nuvem o que não existe mais em local_dir (recursivo). Sem isso, um
// arquivo que o jogo apagou do save fica pra sempre lá e volta pro console no
// próximo restore.
//
// No Drive vai pra lixeira (30 dias de volta). No WebDAV depende do servidor:
// Nextcloud e Synology têm lixeira própria e pegam isso; um WebDAV pelado
// apaga na hora. Está avisado no webdav.c.
bool cloud_prune_extras(const char *auth, const char *folder_id, const char *local_dir);

// --- avisos pra UI ------------------------------------------------------
// Iguais aos do drive.h, e valem pra qualquer nuvem: as funções _tree são as
// demoradas, e sem isso a tela congela num passo só.
// action = "up" / "down" / "dir" / "del".
typedef void (*cloud_progress_cb)(const char *action, const char *name, bool ok);
void cloud_set_progress_cb(cloud_progress_cb cb);

// Perguntado antes de cada arquivo; true faz parar (a função devolve false,
// como qualquer outra falha). Parar no meio de um download é seguro: ele vai
// pro staging no cartão, e quem escreve no save é o syncjob, depois.
typedef bool (*cloud_abort_cb)(void);
void cloud_set_abort_cb(cloud_abort_cb cb);

#ifdef __cplusplus
}
#endif
