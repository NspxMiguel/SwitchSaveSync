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
// pasta e não vai existir. Quem lança é o overlay do Ultrahand, na mão. Se ele
// quebrar numa atualização de firmware, é só não lançar, e o console boota
// igual.

#include <switch.h>

#include <stdio.h>
#include <string.h>

#include "cloud.h"
#include "gfx.h"
#include "http.h"
#include "oauth.h"
#include "savemount.h"
#include "syncjob.h"
#include "lang.h"
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
#define MIN_PULL_HOLD_NS  5000000000ULL   // a tela fica de pe pelo menos 5 s
#define LANG_RELOAD_NS    2000000000ULL   // de quanto em quanto reler idioma.txt

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

    // Antes do primeiro TR: o idioma mora em idioma.txt, no cartão que acabou
    // de ser montado. Sem esta chamada o sysmodule escreve tudo em inglês
    // (é o padrão do lang.c) enquanto o app aparece em português — foi o que
    // aparecia no overlay.
    lang_load();

    syncstate_set_status(TR("Ligando: cartao montado", "Starting: SD card mounted"));

    // Relógio: o TLS precisa saber a data pra validar a validade do
    // certificado do Google. Sem isso todo handshake falha.
    rc = timeInitialize();
    if (R_SUCCEEDED(rc))
        __libnx_init_time();

    syncstate_set_status(TR("Ligando: pm", "Starting: pm"));
    pmdmntInitialize();
    pminfoInitialize();

    syncstate_set_status(TR("Ligando: vi/pl/hid", "Starting: vi/pl/hid"));

    // Pra tela de "Puxando save da nuvem": vi:m cria a camada por cima do
    // jogo, pl da a fonte do console, hid le o controle. Nenhum dos tres e
    // essencial — se falhar, o sync continua sem tela.
    viInitialize(ViServiceType_Manager);
    plInitialize(hosversionAtLeast(16, 0, 0) ? PlServiceType_User : PlServiceType_System);
    hidInitialize();

    syncstate_set_status(TR("Ligando: ns/account", "Starting: ns/account"));
    nsInitialize();   // nome do jogo via NACP
    // Administrator, e nao System.
    //
    // Os nomes da libnx enganam: AccountServiceType_System abre **acc:u1**, e
    // acc:u1 nao esta no service_access deste NPDM. A chamada falhava calada,
    // o servico nunca subia, e todo apelido de conta saia vazio — no Drive a
    // pasta do dono virava "conta-10012EE7DE4B4164" em vez de "Miguel".
    // Comparado lado a lado: o app, que abre acc:u0, resolvia o MESMO uid como
    // "Miguel" no mesmo cartao.
    //
    // AccountServiceType_Administrator abre acc:su, que e o que sysmodule usa
    // e o que este NPDM ja permitia desde sempre.
    accountInitialize(AccountServiceType_Administrator);

    syncstate_set_status(TR("Ligando: rede", "Starting: network"));

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

    // Y, e nao A: o menu do console esta vivo atras desta tela e recebe o mesmo
    // aperto que a gente (ver o comentario do nenhum_jogo_vivo). Um A aqui abre
    // o jogo em destaque no menu; o Y o menu ignora.
    if ((k & HidNpadButton_Y) && !done)
    {
        g_pull_sel = GFX_BTN_NEVER;
        // Cancela o download AGORA. E seguro parar no meio: o que baixou foi
        // pro staging no cartao, e o save do jogo so seria tocado no fim.
        g_pull_never = true;
        snprintf(g_pull_line, sizeof(g_pull_line), "Cancelando...");
    }
}

static u64 foreground_title_id(u64 *pid_out);

// Devolve true se NENHUM jogo está vivo agora.
//
// É a pergunta que separa "puxar da nuvem" de "corromper save". O download da
// varredura ociosa começa com o console parado no menu, mas ele demora — e
// nada impede que um jogo seja aberto no meio. A tela que a gente desenha por
// cima do menu não toma o controle: o A dela chega no menu de baixo e abre
// justamente o jogo em destaque, que é o mesmo que está sendo puxado.
static bool nenhum_jogo_vivo(void)
{
    u64 pid = 0;
    return foreground_title_id(&pid) == 0;
}

static bool pull_abort_cb(void)
{
    // Aborta também quando um jogo aparece. Parar aqui é de graça: o que baixou
    // foi pro staging no cartão e o savedata só seria tocado no fim. O guarda de
    // escrita (syncjob_set_write_guard) fecha o resto da janela; este aborto é o
    // que evita continuar baixando o save de um jogo que já está sendo jogado.
    return g_pull_never || !nenhum_jogo_vivo();
}

// Conta quantos arquivos tem no backup desse jogo no Drive, pra barra ter uma
// porcentagem de verdade em vez de so girar. Custa duas chamadas de API; se
// falhar, a tela mostra "..." e segue.
// Chamado pelo cloud.c a cada arquivo.
static void progress_cb(const char *action, const char *name, bool ok)
{
    if (!ok)
        return;
    if (action[0] == 'd' && action[1] == 'i') // "dir"
        return;

    g_files_done++;
    syncstate_set_status(TR("Puxando save da nuvem... (%d arquivos)", "Downloading save from the cloud... (%d files)"), g_files_done);

    // Primeiro arquivo de verdade: agora sim vale acender a tela. Se o
    // compositor recusar a camada, o pull continua sem tela — melhor puxar
    // calado do que nao puxar.
    if (!g_screen)
        g_screen = gfx_init();

    snprintf(g_pull_line, sizeof(g_pull_line), "Baixando %s", name);
    pull_pump_input(false);
    pull_redraw(false);
}

// A última linha que o job escreveu, guardada pro resumo lá embaixo.
//
// Sem isto: com o refresh token do cartão revogado, o job escrevia "Sem token
// válido — precisa entrar na conta pelo app" no status, e meio segundo depois
// o resumo passava por cima com "cartao OK, nuvem FALHOU". Quem lê a tela não
// tem como saber que era só refazer o login — a palavra "FALHOU" é a mesma que
// aparece quando o Wi-Fi cai.
static char g_last_log[160];

static void log_line(const char *msg)
{
    syncstate_log("%s", msg);
    syncstate_set_status("%s", msg);
    snprintf(g_last_log, sizeof(g_last_log), "%s", msg);
}

// O nome do jogo com o dono do save junto, quando o jogo tem mais de um.
//
// Sem isto o log ficava assim, com três segundos de diferença:
//
//   SUPER MARIO ODYSSEY: nunca subiu pra nuvem por aqui — nao puxei
//   Console parado: SUPER MARIO ODYSSEY — puxando save da nuvem
//
// Que lido de fora é uma contradição — e não era: são duas CONTAS, uma sem
// registro de sync e outra com. A varredura é por save, não por jogo, e o log
// escondia exatamente a informação que explicava as duas linhas.
static const char *rotulo(const TitleEntry *t)
{
    static char buf[0x201 + 0x28];

    if (!t->shared_game)
        snprintf(buf, sizeof(buf), "%s", t->name);
    else if (t->device_save)
        snprintf(buf, sizeof(buf), TR("%s (console)", "%s (console)"), t->name);
    else if (t->account[0])
        snprintf(buf, sizeof(buf), "%s (%s)", t->name, t->account);
    else
        snprintf(buf, sizeof(buf), "%s", t->name);

    return buf;
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
// Foi assim que save de jogo já foi parar em "dados corrompidos".
//
// Congelar não é exclusividade. A regra que ficou no lugar: nenhuma escrita em
// savedata de jogo com processo vivo — o download acontece com o console parado
// no menu (idle_pull_sweep), e o upload só depois do processo do jogo sumir.

static void backup_now(u64 application_id)
{
    // Zera antes de começar: motivo velho de um jogo anterior não pode ser
    // apresentado como explicação do backup de agora.
    g_last_log[0] = '\0';

    if (syncstate_is_excluded(application_id))
    {
        syncstate_log(TR("%016lX esta na lista de excluidos — nao subi", "%016lX is on the excluded list — did not upload"), application_id);
        syncstate_set_status(TR("Ultimo jogo esta excluido do sync", "Last game is excluded from syncing"));
        return;
    }

    // Dois destinos independentes: cartao e nuvem. Da pra ter os dois, um so,
    // ou nenhum (ai o autosync nao faz nada).
    bool want_local = syncstate_dest_local();
    bool want_cloud = syncstate_dest_cloud();

    if (!want_local && !want_cloud)
    {
        syncstate_set_status(TR("Nenhum destino ligado (cartao/nuvem)", "No destination turned on (SD card/cloud)"));
        return;
    }

    // TODAS as contas que têm save deste jogo, e não só a primeira.
    //
    // O sysmodule só sabe o application_id do jogo que fechou — nunca a conta.
    // Com duas pessoas jogando o mesmo jogo no mesmo console, pegar a primeira
    // da enumeração era pegar a ordem que o fsSaveDataInfoReader devolveu, que
    // não tem nada a ver com quem acabou de jogar: uma conta fechava o jogo, o
    // save de OUTRA conta (que não mudou) subia de novo, e a tela dizia "nuvem
    // OK". O progresso de quem tinha acabado de jogar não ia pra lugar nenhum.
    //
    // Subir todas custa pouco: quem não mudou tem a mesma impressão digital do
    // último upload e o backup sai barato. O que não dá é adivinhar.
    static TitleEntry saves[8]; // um console tem 8 perfis, no máximo (~4,7 KB: fora da pilha)
    size_t quantos = syncjob_find_all_titles(application_id, saves, 8);

    if (quantos == 0)
    {
        syncstate_log(TR("%016lX nao tem save data de usuario — nada a subir", "%016lX has no user save data — nothing to upload"), application_id);
        syncstate_set_status(TR("Ultimo jogo nao tem save pra subir", "Last game has no save to upload"));
        return;
    }

    TitleEntry title = saves[0]; // o nome do jogo é o mesmo em todas
    syncstate_log(TR("Jogo fechou: %s", "Game closed: %s"), title.name);

    if (quantos > 1)
        syncstate_log(TR("%s tem save de %d contas — vou subir todas", "%s has saves from %d accounts — uploading all of them"),
            title.name, (int)quantos);

    // Começam em true e caem no primeiro tropeço: com várias contas, "deu
    // certo" só vale se deu certo em TODAS. Só são lidos quando o destino
    // correspondente está ligado.
    bool local_ok = true, cloud_ok = true;
    bool tem_login = false;

    // Cartao primeiro: e' rapido, nao depende de rede nem de login, e serve
    // de rede de seguranca caso o upload falhe no meio.
    if (want_local)
        for (size_t i = 0; i < quantos; i++)
            if (!syncjob_backup_title_local(&saves[i], log_line))
            {
                local_ok = false;
                syncstate_log(TR("%s: backup no cartao falhou", "%s: backup to the SD card failed"), saves[i].name);
            }

    if (want_cloud)
    {
        // O resumo lá embaixo usa a ÚLTIMA linha de log como motivo da falha da
        // nuvem. A linha do CARTÃO, que acabou de passar por aqui dizendo
        // "guardado", não pode ocupar esse lugar: sem login nenhum, o overlay
        // mostrava "Cartao OK, nuvem: Backup de Zelda guardado em sdmc:/..." —
        // uma frase de sucesso no campo da nuvem, com zero bytes enviados.
        g_last_log[0] = '\0';

        tem_login = oauth_load_saved_login();

        if (!tem_login)
        {
            cloud_ok = false;
            log_line(TR("Sem conta Google salva — abra o app e faca login", "No Google account saved — open the app and sign in"));
            if (!want_local)
            {
                syncstate_set_status(TR("Sem conta Google — faca login no app", "No Google account — sign in from the app"));
                return;
            }
        }

        for (size_t i = 0; tem_login && i < quantos; i++)
        {
            bool nuvem_bate = false; // o prune limpou tudo? ver syncjob_backup_title_ex

            if (!syncjob_backup_title_ex(&saves[i], log_line, &nuvem_bate))
            {
                cloud_ok = false;
                continue;
            }

            // Marca o estado que acabou de subir. É contra isso que o pull
            // compara pra decidir se pode escrever por cima do save local.
            //
            // Só quando a nuvem ficou IGUAL ao save. Subiu mas sobrou coisa lá
            // (o prune falhou)? Então os dois lados não estão iguais, e marcar
            // faria a varredura ociosa achar a nuvem mais nova e ressuscitar,
            // dentro do savedata, o arquivo que o jogo tinha apagado — sozinha,
            // sem ninguém na frente do console pra desconfiar.
            u64 fp = 0;
            if (nuvem_bate && syncjob_fingerprint(&saves[i], &fp))
                syncjob_mark_synced(&saves[i], fp);
            else if (!nuvem_bate)
                syncstate_log(TR("%s: sobrou coisa na nuvem — nao marquei como sincronizado",
                                 "%s: leftovers in the cloud — did not mark as synced"),
                    saves[i].name);
        }
    }

    const char *ok   = TR("OK", "OK");
    const char *fail = TR("FALHOU", "FAILED");

    // Quando a nuvem falha, o motivo vale mais que a palavra "FALHOU" — e ele
    // acabou de passar pelo log_line. O nome do jogo sai fora nesse caso: ele
    // está no log, e a faixa do overlay é estreita demais pras duas coisas.
    if (want_cloud && !cloud_ok && g_last_log[0])
    {
        if (want_local)
            syncstate_set_status(TR("Cartao %s, nuvem: %s", "SD card %s, cloud: %s"),
                local_ok ? ok : fail, g_last_log);
        else
            syncstate_set_status(TR("Nuvem: %s", "Cloud: %s"), g_last_log);
    }
    // O resultado na FRENTE e o nome do jogo atrás.
    //
    // Era ao contrário, e o nome do jogo tem até 0x201 bytes (titles.h): um
    // título japonês come a faixa inteira do overlay e empurra o "OK/FALHOU"
    // pra fora da tela. Quem lê "Zelda…" sem o fim conclui que deu certo. O que
    // importa é o resultado; o nome é o detalhe, e é ele que pode faltar.
    else if (want_local && want_cloud)
        syncstate_set_status(TR("Cartao %s, nuvem %s — %s", "SD card %s, cloud %s — %s"),
            local_ok ? ok : fail, cloud_ok ? ok : fail, title.name);
    else if (want_local)
        syncstate_set_status(TR("Cartao %s — %s", "SD card %s — %s"),
            local_ok ? ok : fail, title.name);
    else
        syncstate_set_status(TR("Nuvem %s — %s", "Cloud %s — %s"),
            cloud_ok ? ok : fail, title.name);
}

static void pull_one_save_idle(const TitleEntry *entrada);

// Puxa o save da nuvem com o console PARADO — nenhum jogo rodando. Quem chama
// é a varredura ociosa; que não haja processo de jogo vivo é pré-condição dela.
//
// Só que "pré-condição" não basta e essa foi a lição: o download demora, e o
// jogo pode ser aberto no meio dele. Por isso, além da condição de entrada,
// existem duas travas que valem DURANTE: o pull_abort_cb (para de baixar assim
// que um jogo aparece) e o guarda de escrita registrado no main (recusa montar
// pra escrita se aparecer). A pré-condição diz quando começar; as travas dizem
// quando desistir.
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
        syncstate_set_status(TR("Sem conta Google — jogo abriu com save local", "No Google account — game opened with the local save"));
        return;
    }

    // Mesma história do backup: com duas contas jogando o mesmo jogo, o
    // application_id não diz de quem é o save. Puxa pra todas, uma de cada vez.
    //
    // static, e não na pilha: esta função é chamada de dentro da varredura, que
    // já carrega um TitleEntry[64] (38 KB) no mesmo caminho, e daqui ainda se
    // desce pro curl e pro mbedtls no meio de um handshake TLS. São 128 KB de
    // pilha no total (o main_thread_stack_size do NPDM) e 4,7 KB a mais aqui
    // sairiam justo do trecho mais fundo. Uma thread só chama isto.
    static TitleEntry saves[8];
    size_t quantos = syncjob_find_all_titles(application_id, saves, 8);
    if (quantos == 0)
        return; // jogo sem save data de usuário: não há o que puxar

    for (size_t i = 0; i < quantos; i++)
        pull_one_save_idle(&saves[i]);
}

// Uma conta, um save. Quem separa as contas é o pull_title_idle acima.
static void pull_one_save_idle(const TitleEntry *entrada)
{
    TitleEntry title = *entrada;
    u64 application_id = title.application_id;

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
        syncstate_log(TR("%s: nunca subiu pra nuvem por aqui — nao puxei", "%s: never uploaded from here — did not download"), rotulo(&title));
        return;
    }

    if (have_now && now_fp != synced_fp)
    {
        syncstate_log(TR("%s: save local mudou desde o ultimo upload — nao puxei", "%s: local save changed since the last upload — did not download"),
            rotulo(&title));
        syncstate_set_status(TR("Save local mais novo que a nuvem — mantive o local", "Local save is newer than the cloud — kept the local one"));
        return;
    }

    // Rede de segurança que faltava: cópia do save local no cartão ANTES de
    // qualquer escrita. Se o que vier da nuvem estiver errado, dá pra voltar
    // pelo app. Sem isso, "restaurar" é uma operação sem volta.
    if (!syncjob_backup_title_local(&title, NULL))
    {
        syncstate_log(TR("%s: nao consegui salvar copia local antes de puxar — abortei", "%s: could not save a local copy before downloading — aborted"),
            rotulo(&title));
        syncstate_set_status(TR("Sem copia de seguranca — nao puxei %s", "No safety copy — did not download %s"), title.name);
        return;
    }

    u64 started = armGetSystemTick();
    g_files_done  = 0;
    g_files_total = 0;
    g_pull_never  = false;
    g_pull_sel    = GFX_BTN_OK;
    snprintf(g_pull_game, sizeof(g_pull_game), "%s", title.name);
    snprintf(g_pull_line, sizeof(g_pull_line), "Conectando...");

    syncstate_log(TR("Console parado: %s — puxando save da nuvem", "Console idle: %s — downloading save from the cloud"), rotulo(&title));
    syncstate_set_status(TR("Puxando save da nuvem...", "Downloading save from the cloud..."));

    // A camada NAO nasce aqui.
    //
    // Nascia, e o resultado no console era uma caixa de lixo grafico por cima
    // do menu: a camada fica visivel no instante em que e criada, e o que ela
    // mostra ate o primeiro desenho e memoria de video de outra pessoa. Como o
    // download morria antes de comecar (era o CA bundle que faltava no
    // sysmodule), a caixa aparecia, ficava parada e sumia — sem nunca ter tido
    // nada pra dizer.
    //
    // Agora quem acende a tela e o progress_cb, no primeiro arquivo que chega
    // de verdade. Falhou antes disso, ninguem ve tela nenhuma: so o log e o
    // status, que e onde essa informacao serve pra alguma coisa.
    g_screen = false;

    g_files_total = syncjob_cloud_file_count(&title);

    cloud_set_progress_cb(progress_cb);
    cloud_set_abort_cb(pull_abort_cb);
    // log_line, e nao NULL: era NULL, e por isso o autosync.log terminava em
    // "puxando save da nuvem" e nunca dizia o que aconteceu depois. O motivo da
    // falha existia lá dentro e era jogado fora na saída da função.
    bool ok = syncjob_restore_title(&title, log_line);
    cloud_set_progress_cb(NULL);
    cloud_set_abort_cb(NULL);

    if (g_pull_never)
    {
        // "Nao puxar save da nuvem nesse jogo" — vale pra sempre, e da pra
        // desmarcar depois no overlay do Ultrahand.
        //
        // Se a lista nao foi gravada, dizer "marcado" e mentira que custa caro:
        // na proxima varredura este jogo e puxado de novo, exatamente o que o
        // usuario acabou de mandar nao fazer.
        if (syncstate_set_excluded(application_id, true))
        {
            syncstate_log(TR("%s: pedido pra nao puxar — jogo marcado como excluido", "%s: told not to download — game is marked excluded"), title.name);
            syncstate_set_status(TR("Nao vou mais puxar save de %s", "Will not download saves for %s anymore"), title.name);
            snprintf(g_pull_line, sizeof(g_pull_line), "Cancelado. Esse jogo nao puxa mais.");
        }
        else
        {
            syncstate_log(TR("%s: NAO consegui gravar a lista de excluidos", "%s: could NOT write the excluded list"), title.name);
            syncstate_set_status(TR("Parei o download, mas nao consegui marcar %s — cartao cheio?", "Stopped the download, but couldn't mark %s — SD card full?"), title.name);
            snprintf(g_pull_line, sizeof(g_pull_line), "Parei agora, mas nao consegui marcar o jogo.");
        }
        ok = false;
    }
    else if (ok)
    {
        // Acabou de escrever o que a nuvem tem: essa passa a ser a referência
        // de "não mudou desde o sync".
        u64 fp = 0;
        if (syncjob_fingerprint(&title, &fp))
            syncjob_mark_synced(&title, fp);

        syncstate_log(TR("%s: save da nuvem carregado", "%s: cloud save loaded"), rotulo(&title));
        syncstate_set_status(TR("Save da nuvem carregado: %s", "Cloud save loaded: %s"), rotulo(&title));
        snprintf(g_pull_line, sizeof(g_pull_line), "Save carregado. Bom jogo.");
    }
    else
    {
        // Falhou: fica o save local, do jeito que estava. Nada foi commitado.
        //
        // A linha de log tem que existir mesmo com o motivo já registrado pelo
        // log_line acima: sem ela, quem lê o arquivo depois não sabe se o
        // download terminou, foi abortado no meio, ou se o sysmodule morreu.
        syncstate_log(TR("%s: NAO consegui puxar — save local mantido", "%s: could NOT download — kept the local save"), rotulo(&title));
        syncstate_set_status(TR("Nao consegui puxar — save local mantido", "Could not download — kept the local save"));
        snprintf(g_pull_line, sizeof(g_pull_line), "Nao consegui puxar — o save local ficou como estava");
    }

    // A tela fica de pe com o OK aceso ate alguem apertar. Os 5 s valem tambem
    // como tempo minimo antes de o OK responder, pra ninguem apertar sem ler.
    // Aqui ninguém está preso esperando: o console está no menu.
    if (g_screen)
    {
        g_pull_sel = GFX_BTN_OK;
        u64 shown = armGetSystemTick();

        // Desenha UMA vez e depois só espera.
        //
        // Daqui pra frente o quadro não muda mais: o OK já está aceso, e este
        // laço não mexe na seleção. Redesenhar 60 vezes por segundo custaria
        // rasterizar todo o texto de novo (o stb_truetype aloca e libera um
        // bitmap por letra) dentro de um processo de sistema, e prenderia o
        // laço no framebufferBegin — que espera o compositor devolver buffer e
        // não tem prazo. Com um quadro só, a espera abaixo é relógio e botão,
        // e a saída por tempo sempre acontece. A camada continua mostrando o
        // último quadro entregue.
        pull_redraw(true);

        for (;;)
        {
            u64 waited = armTicksToNs(armGetSystemTick() - shown);
            bool can_ok = waited >= MIN_PULL_HOLD_NS;

            u64 k = gfx_keys_down();
            // Sem o A, de novo: fechar esta tela nao pode abrir jogo nenhum.
            if (can_ok && (k & (HidNpadButton_B | HidNpadButton_Plus)))
                break;

            // Rede caiu, o console ficou largado na mesa, sei la: em 60 s a
            // tela sai sozinha. O jogo nunca fica preso aqui.
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
// com a nuvem, pra quando o jogo abrir já estar certo. É aqui que mora o
// "auto-download" do projeto — só que num momento em que não existe processo de
// jogo pra atropelar.
//
// Um título por passada, e cada título no máximo uma vez a cada meia hora: a
// lista é lida do console, cada pull fala com o Drive, e não faz sentido varrer
// a biblioteca inteira em looping.
#define IDLE_QUIET_NS   30000000000ULL  // 30 s sem jogo antes de começar
#define IDLE_GAP_NS     10000000000ULL  // respiro entre um título e o próximo

// Respiro depois de uma passada que não teve o que fazer. Ver o main.
#define IDLE_QUIET_GAP_NS (5ULL * 60ULL * 1000000000ULL)

// Quanto tempo uma varredura vale antes de o título poder ser olhado de novo.
//
// Isto era uma lista sem prazo, e o "no máximo uma passada por título por
// sessão" acabava significando "por execução do sysmodule": jogo puxado uma vez
// nunca mais era olhado até o console reiniciar. Quem joga no PC ou num segundo
// Switch e volta pra este não recebia o save novo — o console tinha decidido,
// horas antes, que aquele título já estava resolvido.
//
// Meia hora é longa o bastante pra não ficar batendo no Drive à toa e curta o
// bastante pra que uma pausa pro almoço já traga o save de fora.
#define IDLE_RECHECK_NS (30ULL * 60ULL * 1000000000ULL)

typedef struct {
    u64 tid;
    u64 quando; // tick da última varredura deste título
} IdleMark;

static IdleMark g_idle_done[64];
static int      g_idle_done_n = 0;

static bool idle_already_did(u64 tid)
{
    u64 agora = armGetSystemTick();

    for (int i = 0; i < g_idle_done_n; i++)
        if (g_idle_done[i].tid == tid)
            return armTicksToNs(agora - g_idle_done[i].quando) < IDLE_RECHECK_NS;

    return false;
}

static void idle_mark_done(u64 tid)
{
    // Atualiza a entrada que já existe em vez de acrescentar outra. Sem isso a
    // lista enchia e, cheia, parava de anotar — e título que não é anotado é
    // varrido em TODA passada, que é o oposto do que esta lista existe pra
    // fazer.
    for (int i = 0; i < g_idle_done_n; i++)
    {
        if (g_idle_done[i].tid == tid)
        {
            g_idle_done[i].quando = armGetSystemTick();
            return;
        }
    }

    if (g_idle_done_n < (int)(sizeof(g_idle_done) / sizeof(g_idle_done[0])))
    {
        g_idle_done[g_idle_done_n].tid    = tid;
        g_idle_done[g_idle_done_n].quando = armGetSystemTick();
        g_idle_done_n++;
    }
}

// Devolve true se fez alguma coisa (pra quem chama espaçar a próxima).
static bool idle_pull_sweep(void)
{
    if (!syncstate_autosync_enabled() || !syncstate_dest_cloud())
        return false;
    if (!oauth_load_saved_login())
        return false;

    // static, e nao no stack: cada TitleEntry tem 584 bytes (o name[0x201] pesa
    // sozinho), entao list[64] sao ~37 KB de uma pilha de 128 KB — e daqui pra
    // baixo a chamada ainda desce no curl e no mbedTLS, que tambem querem a
    // deles. O sysmodule tem uma thread so, entao static aqui e seguro; o
    // saves[8] do backup ja e static pelo mesmo motivo.
    static TitleEntry list[64];
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

    // A última trava antes de qualquer escrita em savedata, registrada UMA vez
    // e válida pro processo inteiro. O core pergunta isso dentro do
    // write_over_save, imediatamente antes de montar pra escrita — inclusive
    // depois de um download demorado, que é justamente quando a resposta pode
    // ter mudado desde que a varredura decidiu que o console estava parado.
    syncjob_set_write_guard(nenhum_jogo_vivo);

    if (!g_socket_ok)
    {
        // Sem rede não dá pra fazer nada, mas morrer aqui só deixaria o
        // usuário sem pista nenhuma. Fica vivo escrevendo o motivo.
        syncstate_set_status(TR("Rede nao subiu — sysmodule sem funcao", "Network did not come up — sysmodule has nothing to do"));
        syncstate_log(TR("socketInitialize falhou; nada de upload nessa sessao", "socketInitialize failed; no uploads this session"));
    }
    else if (!http_init())
    {
        g_socket_ok = false;
        syncstate_set_status(TR("curl nao iniciou — sysmodule sem funcao", "curl did not start — sysmodule has nothing to do"));
        syncstate_log(TR("http_init falhou", "http_init failed"));
    }
    else if (http_ca_bundle()[0] == '\0')
    {
        // A rede subiu, o curl subiu — e mesmo assim nenhuma conexão HTTPS vai
        // fechar, porque não existe CA bundle pra este processo. Dizer isso
        // aqui, na hora, é a diferença entre um problema de dois minutos e o
        // que aconteceu de verdade: toda sincronização falhando com "sem token
        // válido, entre na conta pelo app", com a conta certa.
        syncstate_set_status(TR("Falta o cacert.pem no cartao — abra o app uma vez pra ele copiar",
                                "cacert.pem is missing from the SD card — open the app once so it copies it"));
        syncstate_log(TR("Sem CA bundle: nenhuma conexao HTTPS vai funcionar. O app grava o arquivo em sdmc:/switch/SwitchSaveSync/cacert.pem ao abrir.",
                         "No CA bundle: no HTTPS connection will work. The app writes sdmc:/switch/SwitchSaveSync/cacert.pem when it opens."));
    }
    else
    {
        syncstate_set_status(TR("Ativo", "Active"));
    }

    u64 last_tid    = 0;   // jogo que estava aberto na última volta
    u64 pending_tid = 0;   // jogo que fechou e ainda não subiu
    u64 pending_at  = 0;   // tick em que ele fechou
    u64 idle_since  = armGetSystemTick(); // desde quando não tem jogo aberto
    u64 last_idle_work = 0;               // última varredura ociosa
    u64 last_lang      = 0;               // última releitura do idioma
    u64 idle_gap       = IDLE_GAP_NS;     // vira 5 min quando não tem o que fazer

    while (true)
    {
        // Relê o idioma de dois em dois segundos.
        //
        // Reler evita a armadilha de trocar o idioma no app e o sysmodule
        // continuar no antigo até alguém desligar e religar ele. Mas reler a
        // CADA volta (100 ms) não é o "fopen de três bytes" que parecia: no
        // modo automático, o lang_load vai no setInitialize/setGetSystemLanguage
        // /setExit, ou seja, abre e fecha uma sessão do serviço `set` dez vezes
        // por segundo, pra sempre — mais o fopen no cartão, que disputa o SD
        // com o jogo que está rodando. Dois segundos é rápido pra quem acabou
        // de trocar o idioma e é 20x menos tráfego.
        u64 now_lang = armGetSystemTick();
        if (last_lang == 0 || armTicksToNs(now_lang - last_lang) >= LANG_RELOAD_NS)
        {
            lang_load();
            last_lang = now_lang;
        }

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
                syncstate_set_status(TR("Jogo fechou, preparando backup...", "Game closed, preparing backup..."));
            }
            else if (tid != 0)
            {
                // Jogo abriu.
                //
                // Aqui ANTES o processo era congelado e o save da nuvem escrito
                // por cima. Isso está proibido, e o motivo custou caro:
                // svcDebugActiveProcess para a CPU do jogo, mas NÃO fecha a
                // sessão de filesystem que ele já tem aberta no savedata.
                // Montar esse mesmo savedata em outro processo e dar
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
                    syncstate_log(TR("Backup de %016lX ficou pendente", "Backup of %016lX left pending"), pending_tid);
                }

                syncstate_set_status(TR("Jogando — sync so quando fechar", "Playing — sync only once it closes"));

                svcSleepThread(POLL_INTERVAL_NS);
                continue;
            }
            last_tid   = tid;
            idle_since = armGetSystemTick(); // acabou de voltar pro menu
            idle_gap   = IDLE_GAP_NS;        // jogou: tem coisa nova pra olhar
        }

        // "Fazer backup agora", pedido pelo overlay. Só atende com nenhum jogo
        // aberto — com jogo rodando o save no cartão pode estar pela metade.
        u64 asked = 0;
        if (g_socket_ok && syncstate_take_request(&asked))
        {
            if (tid != 0)
            {
                syncstate_set_status(TR("Feche o jogo primeiro — pedido ignorado", "Close the game first — request ignored"));
                syncstate_log(TR("Pedido manual com jogo aberto: ignorado", "Manual request with a game open: ignored"));
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
                syncstate_set_status(TR("Autosync desligado no overlay", "Autosync turned off in the overlay"));
            pending_tid = 0;
        }

        // Download da nuvem: só com o console parado no menu, sem upload
        // pendente na fila e depois de um tempo quieto. É o único momento em
        // que dá pra escrever num savedata sem disputar com o dono dele.
        if (tid == 0 && pending_tid == 0 && g_socket_ok
            && armTicksToNs(armGetSystemTick() - idle_since) >= IDLE_QUIET_NS
            && armTicksToNs(armGetSystemTick() - last_idle_work) >= idle_gap)
        {
            // A passada que não fez nada custa a enumeração inteira dos saves:
            // por jogo, uma consulta de instalação, o perfil da conta e um
            // NsApplicationControlData de 384 KB, mais o pdm. Numa biblioteca de
            // 30 jogos isso é ~11 MB de IPC — e estava acontecendo a cada 10 s,
            // pra sempre, num console parado no menu que já não tinha nada a
            // fazer. Fez alguma coisa: volta em 10 s. Não fez: volta em 5 min.
            idle_gap       = idle_pull_sweep() ? IDLE_GAP_NS : IDLE_QUIET_GAP_NS;
            last_idle_work = armGetSystemTick();
        }

        svcSleepThread(POLL_INTERVAL_NS);
    }

    http_shutdown();
    return 0;
}
