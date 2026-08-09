// SwitchSaveSync — sysmodule de autosync.
//
// Sync nos dois sentidos, sozinho:
//   - jogo ABRE  -> congela o processo, puxa o save da nuvem, solta;
//   - jogo FECHA -> sobe o save pro Google Drive.
//
// Como ele enxerga os dois momentos: pmdmntGetApplicationProcessId devolve o
// pid da aplicação em primeiro plano (0 se não tem jogo), e pminfoGetProgramId
// traduz pid -> title id. Mesma técnica do sys-clk e do Fizeau, e a consulta é
// barata o bastante pra rodar a cada 100 ms.
//
// O que ele NÃO faz, de propósito: subir no boot. Não existe boot2.flag nessa
// conversa privada removida do historico
// overlay do Ultrahand, na mão. Se ele quebrar numa atualização de firmware,
// é só não lançar, e o console boota igual.

#include <switch.h>

#include <stdio.h>
#include <string.h>

#include "drive.h"
#include "gfx.h"
#include "http.h"
#include "oauth.h"
#include "savemount.h"
#include "syncjob.h"
#include "syncstate.h"
#include "titles.h"

// ---------------------------------------------------------------------------
// Boilerplate de sysmodule
// ---------------------------------------------------------------------------

// 8 MB. Parece muito pra um sysmodule (o sys-clk vive com 128 KB), mas aqui
// dentro roda curl + mbedtls: handshake TLS, buffers de socket e o corpo das
// respostas do Drive. É a diferença entre "sysmodule que lê um sensor" e
// "sysmodule que fala HTTPS com a internet".
#define INNER_HEAP_SIZE (8 * 1024 * 1024)

// 100 ms, e não 1 s: a janela entre "o jogo virou processo" e "o jogo abre o
// save" é curta. Quanto antes a gente congelar, mais seguro é escrever o save
// que veio da nuvem antes de qualquer leitura do jogo.
#define POLL_INTERVAL_NS  100000000ULL    // 100 ms
#define SETTLE_DELAY_NS   3000000000ULL   // 3 s antes de subir save de jogo que fechou
# conversa privada removida do historico

u32 __nx_applet_type = AppletType_None;
u32 __nx_fs_num_sessions = 1;

// A camada da tela de pull passa pelo nvdrv; 256 KB de transfer memory e o
// mesmo que o Tesla usa.
u32 __nx_nv_transfermem_size = 0x40000;
ViLayerFlags __nx_vi_stray_layer_flags = (ViLayerFlags)0;

// time:s em vez de time:u — time:u é pra aplicação, sysmodule não consegue abrir.
u32 __nx_time_service_type = TimeServiceType_System;

// A libnx exporta essa função (é ela que liga o relógio do sistema no time()
// da newlib, que o mbedtls usa pra checar validade de certificado), mas não
// declara em header nenhum. Sem ela, todo handshake TLS falha por "certificado
// fora da validade", porque o relógio do processo fica em 1970.
extern void __libnx_init_time(void);

void __libnx_initheap(void)
{
    static u8 inner_heap[INNER_HEAP_SIZE];
    extern void *fake_heap_start;
    extern void *fake_heap_end;

    fake_heap_start = inner_heap;
    fake_heap_end   = inner_heap + sizeof(inner_heap);
}

static bool g_socket_ok = false;

void __appInit(void)
{
    Result rc;

    rc = smInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_SM));

    // hosversionSet: sem isso a libnx trata tudo como firmware 1.0.0 e várias
    // chamadas novas (as de save data, inclusive) falham sem explicação.
    rc = setsysInitialize();
    if (R_SUCCEEDED(rc))
    {
        SetSysFirmwareVersion fw;
        if (R_SUCCEEDED(setsysGetFirmwareVersion(&fw)))
            hosversionSet(MAKEHOSVERSION(fw.major, fw.minor, fw.micro));
        setsysExit();
    }

    rc = fsInitialize();
    if (R_FAILED(rc))
        diagAbortWithResult(MAKERESULT(Module_Libnx, LibnxError_InitFail_FS));
    fsdevMountSdmc();

    // A partir daqui já dá pra escrever no cartão, e a primeira coisa a fazer
    // é justamente isso: deixar marca. Se o processo morrer no meio da
    // inicialização (serviço faltando no NPDM, heap curta), o overlay só sabe
    // dizer "nunca rodou". Com um marcador por etapa, o arquivo de status vira
    // o ponto exato onde ele parou.
    syncstate_ensure_dirs();
    syncstate_set_status("Ligando: cartao montado");

    // Relógio: o TLS precisa saber a data pra validar a validade do
    // certificado do Google. Sem isso todo handshake falha.
    rc = timeInitialize();
    if (R_SUCCEEDED(rc))
        __libnx_init_time();

    syncstate_set_status("Ligando: pm");
    pmdmntInitialize();
    pminfoInitialize();

    syncstate_set_status("Ligando: vi/pl/hid");

    // Pra tela de "Puxando save da nuvem": vi:m cria a camada por cima do
    // jogo, pl da a fonte do console, hid le o controle. Nenhum dos tres e
    // essencial — se falhar, o sync continua sem tela.
    viInitialize(ViServiceType_Manager);
    plInitialize(hosversionAtLeast(16, 0, 0) ? PlServiceType_User : PlServiceType_System);
    hidInitialize();

    syncstate_set_status("Ligando: ns/account");
    nsInitialize();   // nome do jogo via NACP
    accountInitialize(AccountServiceType_System);

    syncstate_set_status("Ligando: rede");

    // Buffers enxutos: aqui não tem streaming de vídeo, são requisições
    // pequenas e um upload de poucos MB. Cada KB a mais sai da heap acima.
    // bsd:s (BsdServiceType_System) é o que sysmodule pode abrir — bsd:u é
    // pra aplicação, e pedir o errado dá erro na inicialização.
    SocketInitConfig cfg = *socketGetDefaultInitConfig();
    cfg.tcp_tx_buf_size     = 0x4000;
    cfg.tcp_rx_buf_size     = 0x8000;
    cfg.tcp_tx_buf_max_size = 0x20000;
    cfg.tcp_rx_buf_max_size = 0x20000;
    cfg.udp_tx_buf_size     = 0x800;
    cfg.udp_rx_buf_size     = 0x1000;
    cfg.sb_efficiency       = 1;
    cfg.num_bsd_sessions    = 2;
    cfg.bsd_service_type    = BsdServiceType_System;

    g_socket_ok = R_SUCCEEDED(socketInitialize(&cfg));
}

void __appExit(void)
{
    if (g_socket_ok)
        socketExit();
    accountExit();
    nsExit();
    hidExit();
    plExit();
    viExit();
    pminfoExit();
    pmdmntExit();
    timeExit();
    fsdevUnmountAll();
    fsExit();
    smExit();
}

// ---------------------------------------------------------------------------
// Autosync
// ---------------------------------------------------------------------------

static int  g_files_done  = 0;
static int  g_files_total = 0;   // 0 = nao deu pra contar
static char g_pull_game[0x201];
static char g_pull_line[128];
static int  g_pull_sel   = GFX_BTN_OK;
static bool g_pull_never = false; // apertou "Nao puxar save da nuvem nesse jogo"
static bool g_screen     = false; // a camada subiu?

static void pull_redraw(bool done)
{
    if (!g_screen)
        return;

    int pct = -1;
    if (done)
        pct = 100;
    else if (g_files_total > 0)
    {
        pct = (g_files_done * 100) / g_files_total;
        if (pct > 99)
            pct = 99; // 100% e so quando acabou de verdade
    }

    gfx_draw_pull(g_pull_game, pct, g_pull_line, g_pull_sel, done);
}

// Le o controle no meio do download. So o botao de cancelar responde aqui — o
// OK so acende quando termina.
static void pull_pump_input(bool done)
{
    if (!g_screen)
        return;

    u64 k = gfx_keys_down();

    if (k & (HidNpadButton_Left | HidNpadButton_StickLLeft | HidNpadButton_AnyLeft))
        g_pull_sel = GFX_BTN_NEVER;
    if (k & (HidNpadButton_Right | HidNpadButton_StickLRight | HidNpadButton_AnyRight))
        g_pull_sel = GFX_BTN_OK;

    if ((k & HidNpadButton_A) && g_pull_sel == GFX_BTN_NEVER && !done)
    {
        // Cancela o download AGORA. E seguro parar no meio: o que baixou foi
        // pro staging no cartao, e o save do jogo so seria tocado no fim.
        g_pull_never = true;
        snprintf(g_pull_line, sizeof(g_pull_line), "Cancelando...");
    }
}

static bool pull_abort_cb(void)
{
    return g_pull_never;
}

// Conta quantos arquivos tem no backup desse jogo no Drive, pra barra ter uma
// porcentagem de verdade em vez de so girar. Custa duas chamadas de API; se
// falhar, a tela mostra "..." e segue.
typedef struct
{
    const char *token;
    int n;
} CountCtx;

static void count_cb(const char *id, const char *name, bool is_folder, void *ud)
{
    (void)name;
    CountCtx *c = (CountCtx *)ud;
    if (is_folder)
        drive_list_children(c->token, id, count_cb, c);
    else
        c->n++;
}

static int count_remote_files(const char *safe_name)
{
    char token[2048];
    if (!oauth_get_fresh_access_token(token, sizeof(token)))
        return 0;

    char root_id[128];
    if (!drive_ensure_app_folder(token, root_id, sizeof(root_id)))
        return 0;

    char game_id[128];
    if (!drive_ensure_subfolder(token, root_id, safe_name, game_id, sizeof(game_id)))
        return 0;

    CountCtx ctx = { token, 0 };
    drive_list_children(token, game_id, count_cb, &ctx);
    return ctx.n;
}

// Chamado pelo drive.c a cada arquivo.
static void progress_cb(const char *action, const char *name, bool ok)
{
    if (!ok)
        return;
    if (action[0] == 'd' && action[1] == 'i') // "dir"
        return;

    g_files_done++;
    syncstate_set_status("Puxando save da nuvem... (%d arquivos)", g_files_done);

    snprintf(g_pull_line, sizeof(g_pull_line), "Baixando %s", name);
    pull_pump_input(false);
    pull_redraw(false);
}

static void log_line(const char *msg)
{
    syncstate_log("%s", msg);
    syncstate_set_status("%s", msg);
}

// Devolve o title id do jogo em primeiro plano, ou 0 se não tem nenhum.
// Escreve o pid em *pid_out (precisa dele pra congelar o processo).
static u64 foreground_title_id(u64 *pid_out)
{
    u64 pid = 0;
    if (R_FAILED(pmdmntGetApplicationProcessId(&pid)) || pid == 0)
    {
        *pid_out = 0;
        return 0;
    }

    u64 tid = 0;
    if (R_FAILED(pminfoGetProgramId(&tid, pid)))
    {
        *pid_out = 0;
        return 0;
    }

    *pid_out = pid;
    return tid;
}

// ---------------------------------------------------------------------------
// Congelar o jogo: REMOVIDO, e não vai voltar
// ---------------------------------------------------------------------------
//
// O plano original era congelar o jogo com svcDebugActiveProcess (anexar como
// depurador suspende o processo, fechar o handle o solta) e escrever o save da
// nuvem nessa janela, ~100 ms depois do jogo virar processo.
//
// Isso está errado, e o erro é conceitual: suspender o processo **não fecha a
// sessão de filesystem que ele já abriu no savedata**. O jogo para de executar,
// mas continua sendo dono daquele save. Montar o mesmo savedata aqui em modo
// escrita e chamar fsdevCommitDevice finaliza um estado que o dono não conhece.
// Foi assim que dois jogos do Miguel foram parar em "dados corrompidos" em
//.
//
// Congelar não é exclusividade. A regra que ficou no lugar: nenhuma escrita em
// savedata de jogo com processo vivo — o download acontece com o console parado
// no menu (idle_pull_sweep), e o upload só depois do processo do jogo sumir.

static void backup_now(u64 application_id)
{
    if (syncstate_is_excluded(application_id))
    {
        syncstate_log("%016lX esta na lista de excluidos — nao subi", application_id);
        syncstate_set_status("Ultimo jogo esta excluido do sync");
        return;
    }

    // Dois destinos independentes: cartao e nuvem. Da pra ter os dois, um so,
    // ou nenhum (ai o autosync nao faz nada).
    bool want_local = syncstate_dest_local();
    bool want_cloud = syncstate_dest_cloud();

    if (!want_local && !want_cloud)
    {
        syncstate_set_status("Nenhum destino ligado (cartao/nuvem)");
        return;
    }

    TitleEntry title;
    if (!syncjob_find_title(application_id, &title))
    {
        syncstate_log("%016lX nao tem save data de usuario — nada a subir", application_id);
        syncstate_set_status("Ultimo jogo nao tem save pra subir");
        return;
    }

    syncstate_log("Jogo fechou: %s", title.name);

    bool local_ok = false, cloud_ok = false;

    // Cartao primeiro: e' rapido, nao depende de rede nem de login, e serve
    // de rede de seguranca caso o upload falhe no meio.
    if (want_local)
    {
        local_ok = syncjob_backup_title_local(&title, log_line);
        if (!local_ok)
            syncstate_log("%s: backup no cartao falhou", title.name);
    }

    if (want_cloud)
    {
        if (!oauth_load_saved_login())
        {
            syncstate_log("Sem conta Google salva — abra o app e faca login");
            if (!want_local)
            {
                syncstate_set_status("Sem conta Google — faca login no app");
                return;
            }
        }
        else if (syncjob_backup_title(&title, log_line))
        {
            cloud_ok = true;

            // Marca o estado que acabou de subir. É contra isso que o pull
            // compara pra decidir se pode escrever por cima do save local.
            u64 fp = 0;
            if (syncjob_fingerprint(&title, &fp))
                syncjob_mark_synced(&title, fp);
        }
    }

    if (want_local && want_cloud)
        syncstate_set_status("%s: cartao %s, nuvem %s", title.name,
            local_ok ? "OK" : "FALHOU", cloud_ok ? "OK" : "FALHOU");
    else if (want_local)
        syncstate_set_status("%s: cartao %s", title.name, local_ok ? "OK" : "FALHOU");
    else
        syncstate_set_status("%s: nuvem %s", title.name, cloud_ok ? "OK" : "FALHOU");
}

// Puxa o save da nuvem com o console PARADO — nenhum jogo rodando. Quem chama
// é a varredura ociosa, e a garantia de que não há processo do jogo vivo é
// pré-condição dela, não desta função.
static void pull_title_idle(u64 application_id)
{
    if (!syncstate_autosync_enabled() || syncstate_is_excluded(application_id))
        return;

    // Puxar da nuvem so faz sentido se a nuvem for um destino. Backup so no
    // cartao = nada pra puxar quando o jogo abre.
    if (!syncstate_dest_cloud())
        return;

    if (!oauth_load_saved_login())
    {
        syncstate_set_status("Sem conta Google — jogo abriu com save local");
        return;
    }

    TitleEntry title;
    if (!syncjob_find_title(application_id, &title))
        return; // jogo sem save data de usuário: não há o que puxar

    // A trava que evita atropelar progresso: se o save local mudou desde o
    // último upload, quem está atrasado é a NUVEM, não o console. Nesse caso
    // não baixa nada — o local fica como está e o autosync sobe ele quando o
    // jogo fechar. É o "conflito" do roadmap, resolvido do jeito conservador:
    // na dúvida, o local ganha.
    //
    // Mudou de "só protege quem já sincronizou" pra "protege sempre": antes a
    // condição exigia have_synced, então na PRIMEIRA vez de cada jogo a trava
    // simplesmente não existia — justo quando o risco é maior. Agora, sem
    // registro de sync anterior, não se puxa nada.
    u64 now_fp = 0, synced_fp = 0;
    bool have_now    = syncjob_fingerprint(&title, &now_fp);
    bool have_synced = syncjob_last_synced(&title, &synced_fp);

    if (!have_synced)
    {
        syncstate_log("%s: nunca subiu pra nuvem por aqui — nao puxei", title.name);
        return;
    }

    if (have_now && now_fp != synced_fp)
    {
        syncstate_log("%s: save local mudou desde o ultimo upload — nao puxei",
            title.name);
        syncstate_set_status("Save local mais novo que a nuvem — mantive o local");
        return;
    }

    // Rede de segurança que faltava: cópia do save local no cartão ANTES de
    // qualquer escrita. Se o que vier da nuvem estiver errado, dá pra voltar
    // pelo app. Sem isso, "restaurar" é uma operação sem volta.
    if (!syncjob_backup_title_local(&title, NULL))
    {
        syncstate_log("%s: nao consegui salvar copia local antes de puxar — abortei",
            title.name);
        syncstate_set_status("Sem copia de seguranca — nao puxei %s", title.name);
        return;
    }

    u64 started = armGetSystemTick();
    g_files_done  = 0;
    g_files_total = 0;
    g_pull_never  = false;
    g_pull_sel    = GFX_BTN_OK;
    snprintf(g_pull_game, sizeof(g_pull_game), "%s", title.name);
    snprintf(g_pull_line, sizeof(g_pull_line), "Conectando...");

    syncstate_log("Console parado: %s — puxando save da nuvem", title.name);
    syncstate_set_status("Puxando save da nuvem...");

    // A tela por cima do menu. Se o compositor recusar a camada, o pull
    // continua sem tela — melhor puxar calado do que nao puxar.
    g_screen = gfx_init();
    pull_redraw(false);

    char safe[0x201];
    syncstate_sanitize_name(title.name, safe, sizeof(safe));
    g_files_total = count_remote_files(safe);
    pull_redraw(false);

    drive_set_progress_cb(progress_cb);
    drive_set_abort_cb(pull_abort_cb);
    bool ok = syncjob_restore_title(&title, NULL);
    drive_set_progress_cb(NULL);
    drive_set_abort_cb(NULL);

    if (g_pull_never)
    {
        // "Nao puxar save da nuvem nesse jogo" — vale pra sempre, e da pra
        // conversa privada removida do historico
        syncstate_set_excluded(application_id, true);
        syncstate_log("%s: mandaram nao puxar — jogo marcado como excluido", title.name);
        syncstate_set_status("Nao vou mais puxar save de %s", title.name);
        snprintf(g_pull_line, sizeof(g_pull_line), "Cancelado. Esse jogo nao puxa mais.");
        ok = false;
    }
    else if (ok)
    {
        // Acabou de escrever o que a nuvem tem: essa passa a ser a referência
        // de "não mudou desde o sync".
        u64 fp = 0;
        if (syncjob_fingerprint(&title, &fp))
            syncjob_mark_synced(&title, fp);

        syncstate_set_status("Save da nuvem carregado: %s", title.name);
        snprintf(g_pull_line, sizeof(g_pull_line), "Save carregado. Bom jogo.");
    }
    else
    {
        // Falhou: fica o save local, do jeito que estava. Nada foi commitado.
        syncstate_set_status("Nao consegui puxar — save local mantido");
        snprintf(g_pull_line, sizeof(g_pull_line), "Nao consegui puxar — o save local ficou como estava");
    }

    // "esperar pelo menos 5 segundos" e "ai tem um botao ... e um OK": a tela
    // fica de pe com o OK aceso ate ele apertar. Os 5 s valem tambem como
    // tempo minimo antes de o OK responder, pra ninguem apertar sem ler.
    // Aqui ninguém está preso esperando: o console está no menu.
    if (g_screen)
    {
        g_pull_sel = GFX_BTN_OK;
        u64 shown = armGetSystemTick();

        for (;;)
        {
            u64 waited = armTicksToNs(armGetSystemTick() - shown);
            bool can_ok = waited >= MIN_PULL_HOLD_NS;

            pull_redraw(true);

            u64 k = gfx_keys_down();
            if (can_ok && (k & (HidNpadButton_A | HidNpadButton_B | HidNpadButton_Plus)))
                break;

            // Rede caiu, ele largou o console na mesa, sei la: em 60 s a tela
            // sai sozinha. O jogo nunca fica preso aqui.
            if (waited >= 60000000000ULL)
                break;

            svcSleepThread(16000000ULL); // ~60 quadros por segundo
        }

        gfx_exit();
        g_screen = false;
    }
    else if (ok)
    {
        u64 elapsed = armTicksToNs(armGetSystemTick() - started);
        if (elapsed < MIN_PULL_HOLD_NS)
            svcSleepThread(MIN_PULL_HOLD_NS - elapsed);
    }
}

// Varredura ociosa: com o console parado no menu, deixa os saves locais em dia
// com a nuvem, pra quando ele abrir o jogo já estar certo. É aqui que mora o
// "auto-download" do projeto — só que num momento em que não existe processo de
// jogo pra atropelar.
//
// Um título por passada e no máximo uma passada por título por sessão: a lista
// é lida do console, cada pull fala com o Drive, e não faz sentido varrer a
// biblioteca inteira em looping.
#define IDLE_QUIET_NS   30000000000ULL  // 30 s sem jogo antes de começar
#define IDLE_GAP_NS     10000000000ULL  // respiro entre um título e o próximo

static u64  g_idle_done[64];
static int  g_idle_done_n = 0;

static bool idle_already_did(u64 tid)
{
    for (int i = 0; i < g_idle_done_n; i++)
        if (g_idle_done[i] == tid)
            return true;
    return false;
}

static void idle_mark_done(u64 tid)
{
    if (g_idle_done_n < (int)(sizeof(g_idle_done) / sizeof(g_idle_done[0])))
        g_idle_done[g_idle_done_n++] = tid;
}

// Devolve true se fez alguma coisa (pra quem chama espaçar a próxima).
static bool idle_pull_sweep(void)
{
    if (!syncstate_autosync_enabled() || !syncstate_dest_cloud())
        return false;
    if (!oauth_load_saved_login())
        return false;

    TitleEntry list[64];
    int n = (int)titles_list_with_savedata(list, sizeof(list) / sizeof(list[0]));

    for (int i = 0; i < n; i++)
    {
        u64 tid = list[i].application_id;
        if (idle_already_did(tid) || syncstate_is_excluded(tid))
            continue;

        idle_mark_done(tid);
        pull_title_idle(tid);
        return true;
    }

    return false;
}

int main(int argc, char *argv[])
{
    syncstate_ensure_dirs();
    syncstate_log("--- sysmodule iniciado ---");

    if (!g_socket_ok)
    {
        // Sem rede não dá pra fazer nada, mas morrer aqui só deixaria o
        // usuário sem pista nenhuma. Fica vivo escrevendo o motivo.
        syncstate_set_status("Rede nao subiu — sysmodule sem funcao");
        syncstate_log("socketInitialize falhou; nada de upload nessa sessao");
    }
    else if (!http_init())
    {
        g_socket_ok = false;
        syncstate_set_status("curl nao iniciou — sysmodule sem funcao");
        syncstate_log("http_init falhou");
    }
    else
    {
        syncstate_set_status("Ativo");
    }

    u64 last_tid    = 0;   // jogo que estava aberto na última volta
    u64 pending_tid = 0;   // jogo que fechou e ainda não subiu
    u64 pending_at  = 0;   // tick em que ele fechou
    u64 idle_since  = armGetSystemTick(); // desde quando não tem jogo aberto
    u64 last_idle_work = 0;               // última varredura ociosa

    while (true)
    {
        u64 pid = 0;
        u64 tid = foreground_title_id(&pid);

        if (tid != last_tid)
        {
            if (tid == 0 && last_tid != 0)
            {
                // fechou: agenda o upload em vez de sair correndo. O sistema
                // ainda está finalizando o processo do jogo, e o save só é
                // gravado de verdade no commit que acontece nesse fim.
                pending_tid = last_tid;
                pending_at  = armGetSystemTick();
                syncstate_set_status("Jogo fechou, preparando backup...");
            }
            else if (tid != 0)
            {
                // Jogo abriu.
                //
                // Aqui ANTES eu congelava o processo e escrevia o save da nuvem
                // por cima. Isso está proibido, e o motivo custou caro em
                //: svcDebugActiveProcess para a CPU do jogo, mas NÃO
                // fecha a sessão de filesystem que ele já tem aberta no
                // savedata. Montar esse mesmo savedata em outro processo e dar
                // fsdevCommitDevice finaliza um estado que o jogo não conhece —
                // é assim que save corrompe. Congelar não é exclusividade.
                //
                // A regra agora é simples e não tem exceção: enquanto existir
                // processo do jogo, ninguém encosta no save dele. O download da
                // nuvem passou a acontecer com o console parado no menu (ver
                // idle_pull_sweep), que é onde o Steam também sincroniza — o
                // save já está certo quando o jogo abre.
                last_tid = tid;

                if (pending_tid != 0 && pending_tid != tid)
                {
                    // Abriu outro jogo antes de eu subir o anterior. O upload
                    // fica pendente; save com jogo aberto sai pela metade.
                    syncstate_log("Backup de %016lX ficou pendente", pending_tid);
                }

                syncstate_set_status("Jogando — sync so quando fechar");

                svcSleepThread(POLL_INTERVAL_NS);
                continue;
            }
            last_tid   = tid;
            idle_since = armGetSystemTick(); // acabou de voltar pro menu
        }

        // "Fazer backup agora", pedido pelo overlay. Só atende com nenhum jogo
        // aberto — com jogo rodando o save no cartão pode estar pela metade.
        u64 asked = 0;
        if (g_socket_ok && syncstate_take_request(&asked))
        {
            if (tid != 0)
            {
                syncstate_set_status("Feche o jogo primeiro — pedido ignorado");
                syncstate_log("Pedido manual com jogo aberto: ignorado");
            }
            else
            {
                backup_now(asked);
                pending_tid = 0;
            }
        }

        if (pending_tid != 0 && tid == 0 && g_socket_ok
            && armTicksToNs(armGetSystemTick() - pending_at) >= SETTLE_DELAY_NS)
        {
            if (syncstate_autosync_enabled())
                backup_now(pending_tid);
            else
                syncstate_set_status("Autosync desligado no overlay");
            pending_tid = 0;
        }

        // Download da nuvem: só com o console parado no menu, sem upload
        // pendente na fila e depois de um tempo quieto. É o único momento em
        // que dá pra escrever num savedata sem disputar com o dono dele.
        if (tid == 0 && pending_tid == 0 && g_socket_ok
            && armTicksToNs(armGetSystemTick() - idle_since) >= IDLE_QUIET_NS
            && armTicksToNs(armGetSystemTick() - last_idle_work) >= IDLE_GAP_NS)
        {
            idle_pull_sweep();
            last_idle_work = armGetSystemTick();
        }

        svcSleepThread(POLL_INTERVAL_NS);
    }

    http_shutdown();
    return 0;
}
