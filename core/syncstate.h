// syncstate.h — o "estado compartilhado" do projeto, em arquivo no SD.
//
// Três programas diferentes precisam concordar sobre as mesmas coisas: o app
// gráfico (.nro), o sysmodule de autosync e o overlay do Ultrahand. A forma
// óbvia de ligar os três seria um serviço IPC próprio no sysmodule (é o que o
// sys-clk e o Fizeau fazem). Aqui é arquivo no SD de propósito:
//
//   - o overlay do Ultrahand roda com o jogo na tela e memória curta; ler duas
//     linhas de texto é bem mais barato que abrir sessão de IPC;
//   - se o sysmodule não estiver rodando, o app e o overlay continuam
//     funcionando sozinhos — com IPC, cada um teria que tratar "serviço não
//     existe" na mão;
//   - dá pra editar tudo com o SD no PC quando algo der errado.
//
// O custo é não ter notificação: quem quer saber de mudança tem que reler. Como
// nada aqui muda mais de uma vez por segundo, tudo bem.
#pragma once
#include <switch.h>
#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SYNC_APP_DIR      "sdmc:/switch/SwitchSaveSync"
#define SYNC_STAGING_DIR  SYNC_APP_DIR "/staging"
#define SYNC_CONFIG_PATH  SYNC_APP_DIR "/autosync.cfg"
#define SYNC_EXCLUDE_PATH SYNC_APP_DIR "/excluidos.txt"
#define SYNC_STATUS_PATH  SYNC_APP_DIR "/status.txt"
#define SYNC_LOG_PATH     SYNC_APP_DIR "/autosync.log"
#define SYNC_LOG_TEMP_PATH SYNC_APP_DIR "/autosync.log.novo"
#define SYNC_REQUEST_PATH SYNC_APP_DIR "/pedido.txt"
#define SYNC_DEST_PATH    SYNC_APP_DIR "/destino.cfg"

// Backup local, sem nuvem nenhuma: uma copia do save numa pasta do proprio
// cartao. E o destino que funciona sem rede e sem conta Google. Um jogo = uma
// pasta aqui dentro, espelhada (o backup novo substitui o antigo).
#define SYNC_LOCAL_DIR    SYNC_APP_DIR "/backups"

// Cria SYNC_APP_DIR se não existir. Idempotente.
void syncstate_ensure_dirs(void);

// --- liga/desliga o autosync (o overlay escreve, o sysmodule lê) ---------
// Default quando o arquivo não existe: DESLIGADO. Este comentário já disse o
// contrário do que o código faz, e é o tipo de mentira que custa caro: quem
// lê aqui e confia decide o comportamento errado lá na frente.
bool syncstate_autosync_enabled(void);
void syncstate_set_autosync_enabled(bool enabled);

// --- lista de exclusão --------------------------------------------------
// É a mesma lista do botão "Não puxar save da nuvem nesse jogo": um
// application_id em hex por linha. Vale pros dois sentidos — jogo excluído
// não sobe no autosync nem vai puxar save quando o fs.mitm entrar (v2).
// Pra onde vai o backup. Os dois podem estar ligados ao mesmo tempo, e os
// dois podem estar desligados (ai o autosync nao faz nada). Sem o arquivo,
// vale o padrao: cartao E nuvem.
bool syncstate_dest_local(void);   // cartao SD
bool syncstate_dest_cloud(void);   // Google Drive
void syncstate_set_dest(bool local, bool cloud);

bool syncstate_is_excluded(u64 application_id);
// Devolve false quando a lista nao ficou como pedido (cartao cheio, por
// exemplo). Quem chama tem que dizer isso na tela: esta lista e o "nao mexa
// nesses jogos", e falhar calado devolve o jogo pro autosync sem ninguem ver.
bool syncstate_set_excluded(u64 application_id, bool excluded);
// Lê a lista inteira pra out[]; devolve quantos leu.
size_t syncstate_list_excluded(u64 *out, size_t max);

// --- pedido manual (overlay -> sysmodule) -------------------------------
// O overlay roda com o jogo na tela e nao tem como falar HTTPS ali; entao
// "fazer backup agora" vira um arquivinho que o sysmodule le no proximo
// segundo. syncstate_take_request consome o pedido (apaga o arquivo).
void syncstate_request_backup(u64 application_id);
bool syncstate_take_request(u64 *application_id_out);

// --- status, pro overlay mostrar ----------------------------------------
// Sobrescreve status.txt com uma linha só. O overlay lê e desenha.
void syncstate_set_status(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
bool syncstate_get_status(char *out, size_t outsz);

// --- log ----------------------------------------------------------------
// Anexa uma linha em autosync.log. Trunca o arquivo sozinho quando passa de
// ~64 KB, senão um sysmodule que fica meses ligado enche o cartão.
void syncstate_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

// Troca por '_' tudo que não pode virar nome de pasta (o nome do jogo vira
// caminho no SD e no Drive). Escreve em out, sempre null-terminated.
void syncstate_sanitize_name(const char *in, char *out, size_t outsz);

// --- em que pasta da nuvem esse save mora --------------------------------
//
// Existe por causa de um problema real: renomear a conta e o backup sumir.
//
// Quando um jogo tem save de mais de uma conta, o nome da pasta na nuvem leva
// o apelido junto — "Jogo (apelido da conta)". O apelido é o nome de exibição
// da conta, e o console deixa trocar quando quiser. Trocou, o app passa a
// procurar uma pasta que não existe, faz uma nova, e o backup antigo fica
// órfão: não some, mas some da vista, e o app trata como primeira sync.
//
// O conserto é não depender do nome pra achar de novo. A conta tem um uid, que
// NÃO muda quando o apelido muda — então o que identifica de verdade é
// (application_id + uid). Aqui fica o de-para: esse par aponta pro nome de
// pasta que foi realmente usado na nuvem.
//
// O nome continua sendo o bonito, com o apelido dentro, porque quem abre o
// Drive no navegador tem que entender o que está vendo. O que muda é que o
// nome virou etiqueta, e não mais a identidade.
#define SYNC_FOLDERS_PATH SYNC_APP_DIR "/pastas.txt"

// Guarda "o save desse jogo, dessa conta, mora nesta pasta". Chamar depois de
// um backup que deu certo. Regravar com nome diferente substitui a linha.
// Em device save, passe um uid zerado — é como o resto do código já trata.
void syncstate_remember_folder(u64 application_id, AccountUid uid, const char *folder);

// O contrário: qual pasta foi usada da última vez. false se nunca gravou —
// e aí quem chamou usa o nome calculado, que é o que sempre fez.
bool syncstate_recall_folder(u64 application_id, AccountUid uid, char *out, size_t outsz);

// Esquece o registro desse save (o backup dele saiu da nuvem, por exemplo).
void syncstate_forget_folder(u64 application_id, AccountUid uid);

#ifdef __cplusplus
}
#endif
