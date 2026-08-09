// SwitchSaveSync — versao com interface grafica.
//
// A logica (OAuth, Drive, montar save) e a mesma de ../core, compartilhada
// com o app de console em ../app. Aqui so tem UI: telas, listas e o que
// acontece quando aperta A.
//
// Tudo que demora roda num Job (thread separada) e aparece numa JobPage, pra
// tela nunca congelar. Ver job.hpp.

extern "C" {
#include "config.h"
#include "drive.h"
#include "http.h"
#include "oauth.h"
#include "savemount.h"
#include "syncjob.h"
#include "syncstate.h"
#include "titles.h"
}

#include <borealis.hpp>
#include <switch.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <sys/stat.h>

#include "job.hpp"
#include "job_page.hpp"

#define APP_DIR     "sdmc:/switch/SwitchSaveSync"
#define STAGING_DIR APP_DIR "/staging"
#define TEST_LOCAL  APP_DIR "/test_upload.txt"
#define TEST_DOWN   APP_DIR "/test_downloaded.txt"
#define TEST_REMOTE "switchsavesync_test.txt"
#define MAX_TITLES  64

static TitleEntry g_titles[MAX_TITLES];
static size_t g_titles_count = 0;
static u8 g_icon_buffer[0x20000];

static brls::TabFrame* g_root         = nullptr;
static brls::ListItem* g_account_item = nullptr;
static brls::ListItem* g_login_item   = nullptr;
static brls::ListItem* g_logout_item  = nullptr;

// Diagnóstico da inicialização de rede, mostrado na aba Conta.
static Result g_socket_rc  = 0;
static bool g_socket_lean  = false;
static bool g_applet_mode  = false;

// ============================ pontes com o ../core ============================
// O core e' C puro e os callbacks dele sao ponteiros de funcao sem userdata,
// entao acham o job em andamento pela global Job::current.

extern "C" void gui_device_cb(const char* url, const char* code, const char* urlWithCode)
{
    if (Job::current)
        Job::current->setDeviceLogin(url ? url : "", code ? code : "",
            urlWithCode ? urlWithCode : "");
}

extern "C" void gui_progress_cb(const char* action, const char* name, bool ok)
{
    Job* job = Job::current;
    if (!job)
        return;

    std::string verb = strcmp(action, "dir") == 0   ? "pasta"
        : strcmp(action, "up") == 0                 ? "enviado"
                                                    : "baixado";

    if (ok)
        job->log(verb + ": " + name);
    else
        job->log(std::string("falhou: ") + name, true);
}

extern "C" void gui_oauth_status(const char* msg)
{
    if (!Job::current)
        return;

    Job::current->setStatus(msg);

    // Tambem vai pro log: o status e' uma linha so, e a proxima mensagem
    // apaga a anterior. Quando o login falha, a explicacao ("status 401",
    // "invalid_client") chegava aqui e sumia antes de ele conseguir ler —
    // era o "Login nao concluido. e mais nada".
    Job::current->log(msg);
}

extern "C" bool gui_oauth_cancel(void)
{
    return Job::current && Job::current->cancelRequested();
}

// ============================ helpers ============================

static void updateAccountViews()
{
    bool logged = oauth_is_logged_in();

    if (g_account_item)
        g_account_item->setValue(logged ? "conectada" : "desconectada", !logged);

    // Com a conta conectada, "Entrar" não faz sentido nenhum na tela — ele
    // conversa privada removida do historico
    // conversa privada removida do historico
    // trocar de conta, mas dizendo o que realmente faz.
    if (g_login_item)
    {
        g_login_item->setLabel(logged ? "Trocar de conta" : "Entrar");
        g_login_item->setValue(logged ? "ja conectado" : "", logged);
    }

    if (g_logout_item)
        g_logout_item->setValue(logged ? "" : "nada pra sair", !logged);

    if (g_root)
        g_root->setSubtitle(logged ? "Google Drive: conta conectada"
                                   : "Google Drive: nenhuma conta conectada",
            "v" APP_VERSION_STRING);
}

static void openJob(Job* job, bool cancellable,
    std::function<void(bool)> onFinished = nullptr)
{
    JobPage* page = new JobPage(job, cancellable);
    if (onFinished)
        page->setOnFinished(onFinished);
    brls::Application::pushView(page);
}

// Toda operação de rede passa por aqui antes de tocar em socket. Sem isso, a
// libnx aborta o processo (o "crash ao clicar em Testar conexao").
static bool ensureNetwork(Job* job)
{
    if (http_is_ready())
        return true;

    job->setStatus("A rede nao subiu quando o app abriu.");
    job->log("Detalhes na aba Conta, embaixo de Diagnostico.", true);
    return false;
}

static bool ensureToken(Job* job, char* out, size_t outsz)
{
    if (!ensureNetwork(job))
        return false;

    if (!oauth_is_logged_in())
    {
        job->setStatus("Voce ainda nao conectou uma conta Google.");
        job->log("Va na aba Conta e escolha Entrar.", true);
        return false;
    }

    job->setStatus("Renovando o acesso ao Google...");
    if (!oauth_get_fresh_access_token(out, outsz))
    {
        job->setStatus("Nao consegui renovar o acesso.");
        job->log("Entre na conta de novo pela aba Conta.", true);
        return false;
    }

    return true;
}

// ============================ trabalhos ============================

static bool jobLogin(Job* job)
{
    if (!ensureNetwork(job))
        return false;

    job->setStatus("Pedindo um codigo ao Google...");

    if (!oauth_start_device_flow(gui_oauth_status, gui_oauth_cancel))
    {
        job->setStatus("Login nao concluido.");
        job->log("O motivo esta na linha acima. \"status 401\" ou "
                 "\"invalid_client\" = o CLIENT_ID/SECRET do core/config.h nao "
                 "confere com o que esta no Google Cloud.",
            true);
        job->log("Se o navegador falou em \"app nao verificado\", clique em "
                 "Avancado e depois em Acessar SwitchSaveSync.",
            true);
        return false;
    }

    job->setStatus("Conta conectada. O token fica salvo no cartao, nao precisa repetir.");
    return true;
}

static bool jobConnectionTest(Job* job)
{
    if (!ensureNetwork(job))
        return false;

    char token[512];
    if (!ensureToken(job, token, sizeof(token)))
        return false;

    job->setStatus("Procurando a pasta no Drive...");
    char folder[128];
    if (!drive_ensure_app_folder(token, folder, sizeof(folder)))
    {
        job->setStatus("Nao consegui criar/achar a pasta no Drive.");
        return false;
    }
    job->log(std::string("pasta pronta: ") + DRIVE_APP_FOLDER_NAME);

    mkdir(APP_DIR, 0777);
    FILE* f = fopen(TEST_LOCAL, "w");
    if (!f)
    {
        job->setStatus("Nao consegui escrever no cartao SD.");
        return false;
    }
    fprintf(f, "SwitchSaveSync - arquivo de teste.\n");
    fprintf(f, "Se voce esta lendo isso depois de um download, o ciclo "
               "upload->Drive->download funcionou.\n");
    fclose(f);

    job->setStatus("Enviando um arquivo de teste...");
    if (!drive_upload(token, folder, TEST_REMOTE, TEST_LOCAL, "text/plain"))
    {
        job->setStatus("O upload falhou.");
        return false;
    }
    job->log("upload OK");

    job->setStatus("Baixando o mesmo arquivo de volta...");
    if (!drive_download(token, folder, TEST_REMOTE, TEST_DOWN))
    {
        job->setStatus("O download falhou.");
        return false;
    }
    job->log("download OK");

    f = fopen(TEST_DOWN, "r");
    if (f)
    {
        char line[256];
        while (fgets(line, sizeof(line), f))
        {
            std::string s = line;
            while (!s.empty() && (s.back() == '\n' || s.back() == '\r'))
                s.pop_back();
            if (!s.empty())
                job->log("recebido do Drive: " + s);
        }
        fclose(f);
    }

    job->setStatus("Rede, TLS e Drive API funcionando.");
    return true;
}

// Daqui pra baixo, TUDO que encosta em save de jogo passa pelo core/syncjob.
// As regras que protegem save (montar read-only pra ler, limpar o staging
// antes de usar, commitar só no fim e só se deu certo) moram lá, numa cópia
// só. Antes existiam duas implementações — uma aqui e outra no core — e elas
// já tinham começado a divergir. O app agora cuida do token e da tela.
static void jobLogLine(const char* m)
{
    if (Job::current)
        Job::current->log(m);
}

// Só confere que dá pra falar com o Google (e dispara o aviso certo quando
// não dá). Quem pega o token de verdade é o syncjob, na hora que precisa.
static bool ensureLogin(Job* job)
{
    char token[512];
    return ensureToken(job, token, sizeof(token));
}

// Declarados aqui porque o conflito do jobSync oferece os dois como saída, e
// eles só são definidos mais abaixo.
static bool jobBackup(Job* job, TitleEntry title);
static bool jobRestore(Job* job, TitleEntry title);

// O clique principal: "clica pra synca o save com a nuvem e PRONTO".
// Quem decide pra que lado vai é o syncjob_sync_title.
static bool jobSync(Job* job, TitleEntry title)
{
    if (!ensureLogin(job))
        return false;

    job->setStatus("Comparando o save do console com o do Drive...");

    switch (syncjob_sync_title(&title, jobLogLine))
    {
        case SYNCJOB_SYNC_UPLOADED:
            job->setStatus("Pronto. O save daqui esta guardado no Drive.");
            return true;

        case SYNCJOB_SYNC_DOWNLOADED:
            job->setStatus("Pronto. O save do Drive esta no console.");
            return true;

        case SYNCJOB_SYNC_EQUAL:
            job->setStatus("Ja estava sincronizado. Nao mexi em nada.");
            return true;

        case SYNCJOB_SYNC_NOTHING:
            job->setStatus("Esse jogo ainda nao tem save nenhum pra sincronizar.");
            return true;

        case SYNCJOB_SYNC_CONFLICT:
            job->setStatus("Os dois lados tem save e sao diferentes. Escolher sozinho "
                           "apagaria progresso de um deles — escolha voce, abaixo.");

            // A ordem importa: a primeira é a que recebe o foco, e o caso que
            // leva ao conflito quase sempre é jogo reinstalado com o save de
            // verdade no Drive. Nada é escrito até ele apertar A.
            job->offerChoice("Trazer o save do Drive pro console",
                "Apaga o save que esta no console e poe o do Drive no lugar. "
                "E o que voce quer se reinstalou o jogo.",
                [title] {
                    openJob(new Job(std::string("Restaurar — ") + title.name,
                                [title](Job* j) { return jobRestore(j, title); }),
                        false);
                });

            job->offerChoice("Mandar o save do console pro Drive",
                "Substitui o que esta no Drive pelo save deste console.",
                [title] {
                    openJob(new Job(std::string("Backup — ") + title.name,
                                [title](Job* j) { return jobBackup(j, title); }),
                        false);
                });

            return false;

        case SYNCJOB_SYNC_FAILED:
        default:
            job->setStatus("Nao consegui sincronizar. O motivo esta nas linhas acima.");
            return false;
    }
}

static bool jobBackup(Job* job, TitleEntry title)
{
    if (!ensureLogin(job))
        return false;

    job->setStatus("Enviando o save pro Drive...");
    if (!syncjob_backup_title(&title, jobLogLine))
    {
        job->setStatus("O backup nao terminou. Nada foi alterado no console.");
        return false;
    }

    job->setStatus(std::string("Backup concluido. Esta em ") + DRIVE_APP_FOLDER_NAME
        + "/" + title.name + "/ no seu Drive.");
    return true;
}

// conversa privada removida do historico
// conversa privada removida do historico
static bool jobBackupLocal(Job* job, TitleEntry title)
{
    job->setStatus("Copiando o save pro cartao...");
    if (!syncjob_backup_title_local(&title, jobLogLine))
    {
        job->setStatus("Nao consegui copiar o save pro cartao.");
        return false;
    }

    job->setStatus("Save guardado no cartao, em switch/SwitchSaveSync/backups.");
    return true;
}

static bool jobRestoreLocal(Job* job, TitleEntry title)
{
    job->setStatus("Gravando o save do cartao no console...");
    if (!syncjob_restore_title_local(&title, jobLogLine))
    {
        job->setStatus("Nao consegui restaurar do cartao.");
        return false;
    }

    job->setStatus("Save restaurado do cartao.");
    return true;
}

static bool jobRestore(Job* job, TitleEntry title)
{
    if (!ensureLogin(job))
        return false;

    job->setStatus("Trazendo o save do Drive...");
    if (!syncjob_restore_title(&title, jobLogLine))
    {
        job->setStatus("Nao restaurei nada. Se falhou no meio da gravacao, o save "
                       "do console continua como estava (nada e commitado sem dar certo).");
        return false;
    }

    job->setStatus("Save restaurado do Drive.");
    return true;
}

// ============================ telas ============================

// Tela de um jogo: o que fazer quando o clique simples nao serve.
//
// Ela NAO e mais o caminho normal — o caminho normal e apertar A na lista e o
// app resolver sozinho ("clica pra synca o save com a nuvem e PRONTO"). Isso
// aqui e o Y: mandar pra que lado quando os dois lados mudaram, e as copias no
// proprio cartao, que nao dependem de internet nem de conta.
static void openGamePage(const TitleEntry& title)
{
    brls::AppletFrame* frame = new brls::AppletFrame(true, true);
    frame->setTitle(title.name);

    size_t iconLen = 0;
    if (titles_get_icon(title.application_id, g_icon_buffer, sizeof(g_icon_buffer), &iconLen))
        frame->setIcon(g_icon_buffer, iconLen);

    brls::List* list = new brls::List();

    brls::ListItem* backupItem = new brls::ListItem("Enviar o save pro Drive",
        "Le o save que esta no console e sobe pro Google Drive. Nao altera "
        "nada no console.");
    backupItem->getClickEvent()->subscribe([title](brls::View* view) {
        openJob(new Job(std::string("Backup — ") + title.name,
                    [title](Job* job) { return jobBackup(job, title); }),
            false);
    });

    brls::ListItem* restoreItem = new brls::ListItem("Baixar o save do Drive",
        "Substitui o save que esta no console pelo que esta no Drive.");
    restoreItem->getClickEvent()->subscribe([title](brls::View* view) {
        brls::Dialog* dialog = new brls::Dialog(
            std::string("Isso apaga o save de \"") + title.name + "\" que esta no console e poe no lugar o que esta no Drive. O save atual se perde.\n\nContinuar?");

        dialog->addButton("Cancelar", [dialog](brls::View* view) { dialog->close(); });
        dialog->addButton("Restaurar", [dialog, title](brls::View* view) {
            dialog->close([title]() {
                openJob(new Job(std::string("Restaurar — ") + title.name,
                            [title](Job* job) { return jobRestore(job, title); }),
                    false);
            });
        });

        dialog->setCancelable(true);
        dialog->open();
    });

    brls::ListItem* backupLocalItem = new brls::ListItem("Salvar o save no cartao SD",
        "Guarda uma copia em switch/SwitchSaveSync/backups, no proprio "
        "console. Nao usa internet nem conta Google.");
    backupLocalItem->getClickEvent()->subscribe([title](brls::View* view) {
        openJob(new Job(std::string("Backup no cartao — ") + title.name,
                    [title](Job* job) { return jobBackupLocal(job, title); }),
            false);
    });

    brls::ListItem* restoreLocalItem = new brls::ListItem("Restaurar do cartao SD",
        "Substitui o save que esta no console pela copia guardada no cartao.");
    restoreLocalItem->getClickEvent()->subscribe([title](brls::View* view) {
        brls::Dialog* dialog = new brls::Dialog(
            std::string("Isso apaga o save de \"") + title.name + "\" que esta no console e poe no lugar a copia do cartao. O save atual se perde.\n\nContinuar?");

        dialog->addButton("Cancelar", [dialog](brls::View* view) { dialog->close(); });
        dialog->addButton("Restaurar", [dialog, title](brls::View* view) {
            dialog->close([title]() {
                openJob(new Job(std::string("Restaurar do cartao — ") + title.name,
                            [title](Job* job) { return jobRestoreLocal(job, title); }),
                    false);
            });
        });

        dialog->setCancelable(true);
        dialog->open();
    });

    list->addView(backupItem);
    list->addView(backupLocalItem);
    list->addView(restoreLocalItem);
    list->addView(restoreItem);

    char idText[64];
    snprintf(idText, sizeof(idText), "Title ID: %016lX", title.application_id);
    list->addView(new brls::Label(brls::LabelStyle::DESCRIPTION, idText, true));

    frame->setContentView(list);
    brls::Application::pushView(frame);
}

static void fillGamesList(brls::List* list)
{
    list->clear(true);

    g_titles_count = titles_list_with_savedata(g_titles, MAX_TITLES);

    if (g_titles_count == 0)
    {
        list->addView(new brls::Label(brls::LabelStyle::DESCRIPTION,
            "Nenhum jogo com save de usuario foi encontrado neste console.", true));
        return;
    }

    for (size_t i = 0; i < g_titles_count; i++)
    {
        TitleEntry title = g_titles[i];

        brls::ListItem* item = new brls::ListItem(title.name);

        size_t iconLen = 0;
        if (titles_get_icon(title.application_id, g_icon_buffer, sizeof(g_icon_buffer), &iconLen))
            item->setThumbnail(g_icon_buffer, iconLen); // a Image copia o buffer

        // A sincroniza direto. O app decide sozinho pra que lado vai; so
        // para pra perguntar quando os dois lados mudaram.
        item->getClickEvent()->subscribe([title](brls::View* view) {
            openJob(new Job(std::string("Sincronizando — ") + title.name,
                        [title](Job* job) { return jobSync(job, title); }),
                false);
        });

        // Y abre a tela com as acoes separadas. Fica registrado no item, e nao
        // na lista, porque e assim que a acao sabe de QUAL jogo ela e: a
        // borealis procura a acao no item que esta em foco.
        item->registerAction("Mais opcoes", brls::Key::Y, [title] {
            openGamePage(title);
            return true;
        });

        list->addView(item);
    }
}

static brls::List* createGamesTab()
{
    brls::List* list = new brls::List();
    fillGamesList(list);

    // Recarregar a lista sem sair do app (util depois de instalar um jogo
    // novo ou criar um save).
    list->registerAction("Atualizar lista", brls::Key::X, [list] {
        fillGamesList(list);
        brls::Application::notify("Lista atualizada");
        return true;
    });

    return list;
}

static brls::List* createAccountTab()
{
    brls::List* list = new brls::List();

    list->addView(new brls::Header("Conta do Google Drive", false));

    g_account_item = new brls::ListItem("Conta Google");
    list->addView(g_account_item);

    brls::ListItem* loginItem = new brls::ListItem("Entrar",
        "Abre um codigo pra voce digitar no celular ou no PC. O Switch nao "
        "pede senha em momento nenhum.");
    loginItem->getClickEvent()->subscribe([](brls::View* view) {
        openJob(new Job("Entrar na conta Google", jobLogin), true,
            [](bool success) { updateAccountViews(); });
    });
    list->addView(loginItem);
    g_login_item = loginItem;

    brls::ListItem* logoutItem = new brls::ListItem("Sair da conta",
        "Apaga o token guardado no cartao. Nao mexe em nada no Drive.");
    logoutItem->getClickEvent()->subscribe([](brls::View* view) {
        oauth_logout();
        updateAccountViews();
        brls::Application::notify("Conta desconectada");
    });
    list->addView(logoutItem);
    g_logout_item = logoutItem;

    list->addView(new brls::Header("Diagnostico", false));

    brls::ListItem* testItem = new brls::ListItem("Testar conexao",
        "Sobe e baixa um arquivinho de teste. Nao encosta em save nenhum.");
    testItem->getClickEvent()->subscribe([](brls::View* view) {
        openJob(new Job("Teste de conexao", jobConnectionTest), true);
    });
    list->addView(testItem);

    // Estado da rede em texto, pra parar de sair só "falha ao iniciar a rede"
    // sem dizer o que falhou.
    char net[512];
    if (R_SUCCEEDED(g_socket_rc))
    {
        snprintf(net, sizeof(net), "Rede iniciada%s.\nModo: %s.",
            g_socket_lean ? " (config enxuta, o padrao nao coube na memoria)" : "",
            g_applet_mode ? "APPLET" : "aplicacao");
    }
    else
    {
        snprintf(net, sizeof(net),
            "Rede NAO iniciou. Erro %08X (modulo %d, descricao %d).\n"
            "Modo: %s.%s",
            (unsigned)g_socket_rc, R_MODULE(g_socket_rc), R_DESCRIPTION(g_socket_rc),
            g_applet_mode ? "APPLET" : "aplicacao",
            g_applet_mode
                ? "\n\nQuase certo que e' isso: em modo applet o app fica com "
                  "~448 MB no total, e a interface grafica + a rede nao cabem. "
                  "Fecha o hbmenu, segura R e abre um JOGO — o hbmenu abre no "
                  "lugar dele em modo aplicacao, com ~3 GB."
                : "");
    }
    list->addView(new brls::Label(brls::LabelStyle::DESCRIPTION, net, true));

    return list;
}

static brls::List* createAboutTab()
{
    brls::List* list = new brls::List();

    // Cada seção é um ListItem, e não Header+Label, por causa de navegação: a
    // borealis só rola a lista até o item FOCADO, e Label não recebe foco. Com
    // um único item focável no topo, o D-pad não tinha pra onde descer, a lista
    // nunca rolava, e tudo que passasse da primeira tela virava texto
    // inalcançável — foi assim que a privacidade, que é a última seção, ficou
    // conversa privada removida do historico
    // Um item focável por seção resolve isso por construção, em vez de depender
    // do texto caber por sorte.
    brls::ListItem* versionItem = new brls::ListItem("Versao");
    versionItem->setValue(APP_VERSION_STRING);
    list->addView(versionItem);

    list->addView(new brls::ListItem("O que este app faz",
        "Sincroniza os saves dos seus jogos com o Google Drive, na pasta \""
        DRIVE_APP_FOLDER_NAME "\". Cada jogo vira uma subpasta com o nome dele, "
        "e os arquivos ficam soltos la dentro — da pra baixar pelo site do "
        "Drive normalmente."));

    list->addView(new brls::ListItem("Como sincronizar",
        "Na lista de jogos, A sincroniza sozinho: ele compara o save do console "
        "com o da nuvem e decide o lado. Se os dois mudaram desde a ultima vez, "
        "ele nao escolhe por voce — para e deixa a escolha no botao Y. So "
        "aparecem jogos instalados no console."));

    list->addView(new brls::ListItem("Privacidade",
        "O app fala direto com o Google, sem servidor no meio. O acesso pedido "
        "e' o \"drive.file\": o app so enxerga os arquivos que ele mesmo criou, "
        "nao o resto do seu Drive. O login fica so no cartao, em "
        "/switch/SwitchSaveSync/token.txt, e sai de vez com o Sair da conta."));

    list->addView(new brls::ListItem("Em que pe esta o projeto",
        "Esta e a versao avulsa, em que voce abre o app e sincroniza na mao. O "
        "proximo passo e o sysmodule, que faz isso sozinho ao entrar e sair do "
        "jogo — fora do escopo por enquanto."));

    return list;
}

// ============================ main ============================

// A recusa de rodar em modo aplicação saiu daqui.
//
// Ela existia porque eu tinha concluído que o "dados corrompidos" do Mario 3D
// World vinha do title takeover: o loader monta o savedata do jogo sequestrado,
// conversa privada removida do historico
// conversa privada removida do historico
// sustenta: quem monta o save do jogo sequestrado é o hbloader, no instante do
// takeover, independente do que o nosso app faça depois. Recusar não protegia
// save nenhum; só impedia o app de abrir do jeito que ele quer usar.
//
// O que de fato protege continua valendo e não mudou: todo save que a gente
// monta é por application_id explícito (savemount_mount), nunca "o save do
// processo atual". Rodar em applet ou em aplicação não muda uma linha disso.

int main(int argc, char* argv[])
{
    // romfs antes da borealis: ela carrega as fontes de romfs:/ na init.
    // O cacert.pem que o http.c usa tambem mora la.
    // Mesma história da rede: a borealis já chama romfsInit() no userAppInit().
    // Uma segunda montagem responde "já está montado", que é sucesso disfarçado
    // de erro. Só desmontamos no fim se fomos nós que montamos.
    Result rc = romfsInit();
    bool we_own_romfs = R_SUCCEEDED(rc);
    bool romfs_ok     = we_own_romfs
        || R_VALUE(rc) == MAKERESULT(Module_Libnx, LibnxError_AlreadyMapped)
        || R_VALUE(rc) == MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized);

    // A rede.
    //
    // **A borealis já sobe a rede antes do main.** O switch_wrapper.c dela
    // define userAppInit(), que a libnx chama na inicialização do processo, e
    // lá dentro tem romfsInit() + socketInitializeDefault() + nxlinkStdio().
    // Então quando este main chamava socketInitializeDefault() de novo, a
    // resposta era 0xF59 (Module_Libnx / LibnxError_AlreadyInitialized) — que
    // quer dizer "já está pronto", e não "falhou". Eu tratava como falha e o
    // app inteiro ficava sem Drive com a rede funcionando do lado.
    // Era esse o "falha ao iniciar rede" que aparecia mesmo em modo aplicação.
    //
    // Falha de verdade que ainda pode acontecer é de memória (modo applet, com
    // ~448 MB, onde borealis + curl + buffers de socket não cabem); pra essa
    // ainda tentamos de novo com buffers enxutos antes de desistir.
    bool we_own_socket = false;
    g_socket_rc = socketInitializeDefault();

    if (R_VALUE(g_socket_rc) == MAKERESULT(Module_Libnx, LibnxError_AlreadyInitialized))
    {
        g_socket_rc = 0; // a borealis já cuidou disso — quem desliga é ela
    }
    else if (R_SUCCEEDED(g_socket_rc))
    {
        we_own_socket = true;
    }
    else
    {
        SocketInitConfig lean = *socketGetDefaultInitConfig();
        lean.tcp_tx_buf_size     = 0x2000;
        lean.tcp_rx_buf_size     = 0x4000;
        lean.tcp_tx_buf_max_size = 0x10000;
        lean.tcp_rx_buf_max_size = 0x10000;
        lean.udp_tx_buf_size     = 0x800;
        lean.udp_rx_buf_size     = 0x1000;
        lean.sb_efficiency       = 1;
        g_socket_rc              = socketInitialize(&lean);
        if (R_SUCCEEDED(g_socket_rc))
        {
            g_socket_lean  = true;
            we_own_socket  = true;
        }
    }

    bool socket_ok = R_SUCCEEDED(g_socket_rc);
    bool http_ok   = socket_ok && http_init();
    g_applet_mode  = (appletGetAppletType() != AppletType_Application
                      && appletGetAppletType() != AppletType_SystemApplication);

    brls::Logger::setLogLevel(brls::LogLevel::INFO);
    brls::i18n::loadTranslations();

    if (!brls::Application::init("SwitchSaveSync"))
    {
        brls::Logger::error("Unable to init Borealis application");
        return EXIT_FAILURE;
    }

    drive_set_progress_cb(gui_progress_cb);
    oauth_set_device_cb(gui_device_cb);
    oauth_load_saved_login();

    g_root = new brls::TabFrame();
    g_root->setTitle("SwitchSaveSync");
    g_root->setIcon(BOREALIS_ASSET("icon/borealis.jpg"));

    g_root->addTab("Meus jogos", createGamesTab());
    g_root->addTab("Conta", createAccountTab());
    g_root->addSeparator();
    g_root->addTab("Sobre", createAboutTab());

    updateAccountViews();

    brls::Application::pushView(g_root);

    // Primeira vez no app (nenhuma conta salva): já oferece o login em vez de
    // deixar ele descobrir sozinho que precisa ir na aba Conta.
    if (!oauth_is_logged_in() && http_ok)
    {
        brls::Dialog* welcome = new brls::Dialog(
            "Pra sincronizar os saves, o app precisa de uma conta do Google "
            "Drive.\n\nVoce conecta pelo celular ou pelo PC: aparece um QR "
            "code aqui, voce aponta a camera, e pronto. O Switch nao pede "
            "senha em momento nenhum.");

        welcome->addButton("Agora nao", [welcome](brls::View* view) { welcome->close(); });
        welcome->addButton("Conectar conta", [welcome](brls::View* view) {
            welcome->close([]() {
                openJob(new Job("Entrar na conta Google", jobLogin), true,
                    [](bool success) { updateAccountViews(); });
            });
        });

        welcome->setCancelable(true);
        welcome->open();
    }

    // Se a rede ou o romfs nao subiram, o app abre mesmo assim — mas avisa,
    // em vez de so falhar depois em toda operacao sem explicar por que.
    if (!romfs_ok)
        brls::Application::notify("Falha ao montar o romfs: os certificados nao carregaram");
    else if (!http_ok && g_applet_mode)
        brls::Application::notify("Rede falhou (modo applet) — ver aba Conta");
    else if (!http_ok)
        brls::Application::notify("Rede falhou — ver detalhes na aba Conta");

    while (brls::Application::mainLoop())
        ;

    if (http_ok)
        http_shutdown();
    // Só desliga a rede se fomos nós que ligamos. socketExit() não é contado
    // por referência: chamar aqui e de novo no userAppExit() da borealis
    // derrubaria o socket duas vezes.
    if (we_own_socket)
        socketExit();
    if (we_own_romfs)
        romfsExit();

    return EXIT_SUCCESS;
}
