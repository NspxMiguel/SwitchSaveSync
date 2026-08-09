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

// Troca os caracteres que nao podem virar nome de pasta no cartao. O nome de
// verdade (com ":" e tudo) continua sendo o que vai pro Drive — isso aqui e'
// so pro staging local.
static std::string sanitizeForPath(const std::string& in)
{
    std::string out;
    for (char c : in)
    {
        bool bad = c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|';
        out += bad ? '_' : c;
    }
    return out;
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

static bool jobBackup(Job* job, TitleEntry title)
{
    char token[512];
    if (!ensureToken(job, token, sizeof(token)))
        return false;

    std::string staging = std::string(STAGING_DIR) + "/" + sanitizeForPath(title.name);
    mkdir(APP_DIR, 0777);
    mkdir(STAGING_DIR, 0777);
    mkdir(staging.c_str(), 0777);

    // read_only=true: um backup nao consegue estragar o save nem por bug.
    job->setStatus("Abrindo o save do jogo (somente leitura)...");
    if (!savemount_mount(title.application_id, title.uid, true))
    {
        job->setStatus("Nao consegui abrir o save desse jogo.");
        return false;
    }

    job->setStatus("Copiando o save pro cartao...");
    bool copied = savemount_copy_tree("save:/", staging.c_str());
    savemount_unmount(false); // so lemos, nao ha o que commitar

    if (!copied)
    {
        job->setStatus("Falha ao copiar o save pra area de trabalho.");
        return false;
    }

    job->setStatus("Preparando as pastas no Drive...");
    char appFolder[128], gameFolder[128];
    if (!drive_ensure_app_folder(token, appFolder, sizeof(appFolder)) || !drive_ensure_subfolder(token, appFolder, title.name, gameFolder, sizeof(gameFolder)))
    {
        job->setStatus("Nao consegui criar as pastas no Drive.");
        return false;
    }

    job->setStatus("Enviando pro Drive...");
    if (!drive_upload_tree(token, gameFolder, staging.c_str()))
    {
        job->setStatus("O backup terminou com erro em pelo menos um arquivo.");
        return false;
    }

    job->setStatus(std::string("Backup concluido. Esta em ") + DRIVE_APP_FOLDER_NAME + "/" + title.name + "/ no seu Drive.");
    return true;
}

// conversa privada removida do historico
// conversa privada removida do historico
static bool jobBackupLocal(Job* job, TitleEntry title)
{
    job->setStatus("Copiando o save pro cartao...");
    if (!syncjob_backup_title_local(&title, [](const char* m) {
            if (Job::current)
                Job::current->log(m);
        }))
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
    if (!syncjob_restore_title_local(&title, [](const char* m) {
            if (Job::current)
                Job::current->log(m);
        }))
    {
        job->setStatus("Nao consegui restaurar do cartao.");
        return false;
    }

    job->setStatus("Save restaurado do cartao.");
    return true;
}

static bool jobRestore(Job* job, TitleEntry title)
{
    char token[512];
    if (!ensureToken(job, token, sizeof(token)))
        return false;

    std::string staging = std::string(STAGING_DIR) + "/" + sanitizeForPath(title.name);
    mkdir(APP_DIR, 0777);
    mkdir(STAGING_DIR, 0777);

    job->setStatus("Procurando o save desse jogo no Drive...");
    char appFolder[128], gameFolder[128];
    if (!drive_ensure_app_folder(token, appFolder, sizeof(appFolder)) || !drive_ensure_subfolder(token, appFolder, title.name, gameFolder, sizeof(gameFolder)))
    {
        job->setStatus("Nao achei a pasta desse jogo no Drive.");
        job->log("Ja fez backup dele alguma vez?", true);
        return false;
    }

    job->setStatus("Baixando do Drive...");
    if (!drive_download_tree(token, gameFolder, staging.c_str()))
    {
        job->setStatus("Falha ao baixar. Nada foi escrito no console.");
        return false;
    }

    job->setStatus("Abrindo o save do jogo pra escrita...");
    if (!savemount_mount(title.application_id, title.uid, false))
    {
        job->setStatus("Nao consegui abrir o save. Nada foi escrito.");
        return false;
    }

    job->setStatus("Gravando no save do console...");
    bool copied = savemount_copy_tree(staging.c_str(), "save:/");
    savemount_unmount(copied); // so commita se a copia deu certo

    if (!copied)
    {
        job->setStatus("Falha ao gravar — o save NAO foi commitado, "
                       "continua como estava antes.");
        return false;
    }

    job->setStatus("Save restaurado.");
    return true;
}

// Apaga o save do console. Existe pro teste do ciclo completo: apaga aqui,
// entra no jogo, e o save tem que voltar da nuvem. So roda depois de
// confirmar que o backup do Drive existe — nunca apaga sem rede de seguranca.
static bool jobWipe(Job* job, TitleEntry title)
{
    char token[512];
    if (!ensureToken(job, token, sizeof(token)))
        return false;

    job->setStatus("Conferindo se o backup existe no Drive...");
    char appFolder[128], gameFolder[128];
    if (!drive_ensure_app_folder(token, appFolder, sizeof(appFolder)) || !drive_ensure_subfolder(token, appFolder, title.name, gameFolder, sizeof(gameFolder)))
    {
        job->setStatus("Nao achei backup desse jogo no Drive. Nao apaguei nada.");
        job->log("Faz o backup primeiro — sem ele nao da pra voltar atras.", true);
        return false;
    }

    job->setStatus("Abrindo o save do jogo pra escrita...");
    if (!savemount_mount(title.application_id, title.uid, false))
    {
        job->setStatus("Nao consegui abrir o save. Nada foi apagado.");
        return false;
    }

    job->setStatus("Apagando o save do console...");
    bool wiped = savemount_wipe_contents();
    savemount_unmount(wiped); // so commita se apagou tudo mesmo

    if (!wiped)
    {
        job->setStatus("Nao consegui apagar tudo — nada foi commitado, "
                       "o save continua como estava.");
        return false;
    }

    job->setStatus("Save apagado do console. O do Drive continua la.");
    return true;
}

// ============================ telas ============================

// Tela de um jogo: icone grande no cabecalho e as duas acoes.
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

    brls::ListItem* wipeItem = new brls::ListItem("Apagar o save do console (teste)",
        "Apaga o save daqui pra testar se ele volta da nuvem. So funciona se "
        "ja existir backup no Drive.");
    wipeItem->getClickEvent()->subscribe([title](brls::View* view) {
        brls::Dialog* dialog = new brls::Dialog(
            std::string("Apagar o save de \"") + title.name + "\" DO CONSOLE.\n\n"
            "Depois disso o jogo abre como se nunca tivesse sido jogado, ate "
            "voce restaurar do Drive. Confiro o backup antes de apagar.\n\n"
            "Continuar?");

        dialog->addButton("Cancelar", [dialog](brls::View* view) { dialog->close(); });
        dialog->addButton("Apagar", [dialog, title](brls::View* view) {
            dialog->close([title]() {
                openJob(new Job(std::string("Apagar save — ") + title.name,
                            [title](Job* job) { return jobWipe(job, title); }),
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
    list->addView(wipeItem);

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

        item->getClickEvent()->subscribe([title](brls::View* view) {
            openGamePage(title);
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

    // Precisa ter pelo menos UM item focavel: uma aba só de texto é um beco
    // sem saída pra navegação — ao ir pra direita não há onde pousar o foco, e
    // a tela parece travada.
    brls::ListItem* versionItem = new brls::ListItem("Versao", APP_VERSION_STRING);
    versionItem->setValue(APP_VERSION_STRING);
    list->addView(versionItem);

    list->addView(new brls::Header("SwitchSaveSync " APP_VERSION_STRING, false));
    list->addView(new brls::Label(brls::LabelStyle::DESCRIPTION,
        "Sincroniza os saves dos seus jogos com o Google Drive, na pasta \"" DRIVE_APP_FOLDER_NAME "\".\n\n"
        "Cada jogo vira uma subpasta com o nome dele, e os arquivos ficam "
        "soltos la dentro — da pra baixar pelo site do Drive normalmente.\n\n"
        "Esta versao e o app avulso, onde o backup e o restore sao na mao. O "
        "proximo passo do projeto e o sysmodule, que faz isso sozinho ao "
        "entrar e sair do jogo.",
        true));

    list->addView(new brls::Header("Privacidade", false));
    list->addView(new brls::Label(brls::LabelStyle::DESCRIPTION,
        "O app fala direto com o Google, sem servidor no meio. O acesso "
        "pedido e' o \"drive.file\": o app so enxerga os arquivos que ele "
        "mesmo criou, nao o resto do seu Drive.",
        true));

    return list;
}

// ============================ main ============================

// Roda em modo aplicação (title takeover)? Se sim, o app se recusa a abrir.
//
// Por quê, com nome e sobrenome: pra ter memória de aplicação, o
// Sphaira "sequestra" o id de um jogo instalado e roda o homebrew no lugar
// dele. Quem faz isso não ganha só a RAM do jogo — ganha o savedata do jogo,
// montado pelo próprio loader como se fosse o save do processo. Aí, se o
// processo morre no meio (e o nosso morria: os crash reports saíram como
// "hbloader" com o Program ID do jogo), o save fica com escrita pela metade e
// o console marca "dados corrompidos". Foi exatamente isso que aconteceu com
// o Mario 3D World (010028600EBDA000) e o Animal Crossing (01006F8002326000):
// os dois títulos que o Sphaira sequestrou.
//
// A resposta certa não é "arrumar o crash e tentar de novo". É não rodar aí.
// Um app que mexe em save nunca deve rodar em cima do id de outro jogo, nem
// que rode perfeitamente — porque o dia em que ele travar, o estrago cai no
// save alheio. Em modo applet o processo é o álbum, e o save que a gente monta
// é sempre por id explícito.
static bool running_as_hijacked_title()
{
    AppletType t = appletGetAppletType();
    return t == AppletType_Application || t == AppletType_SystemApplication;
}

static void refuse_and_explain()
{
    consoleInit(NULL);

    padConfigureInput(1, HidNpadStyleSet_NpadStandard);
    PadState pad;
    padInitializeDefault(&pad);

    printf("\n  SwitchSaveSync\n");
    printf("  --------------------------------------------------\n\n");
    printf("  Este app NAO abre em modo aplicacao.\n\n");
    printf("  Modo aplicacao = o Sphaira pegou o id de um jogo\n");
    printf("  instalado emprestado pra dar mais memoria. Junto\n");
    printf("  vem o save DAQUELE jogo, montado pra escrita.\n");
    printf("  Se o homebrew fecha mal ali dentro, quem fica com\n");
    printf("  o save pela metade e o jogo.\n\n");
    printf("  Foi assim que o Mario 3D World e o Animal Crossing\n");
    printf("  apareceram como dados corrompidos.\n\n");
    printf("  Abra pelo album (modo applet). Funciona igual.\n\n");
    printf("  --------------------------------------------------\n");
    printf("  + para sair\n");

    while (appletMainLoop())
    {
        padUpdate(&pad);
        if (padGetButtonsDown(&pad) & HidNpadButton_Plus)
            break;
        consoleUpdate(NULL);
    }

    consoleExit(NULL);
}

int main(int argc, char* argv[])
{
    // Primeira coisa do processo, antes de romfs, rede, borealis e qualquer
    // fs: se estamos rodando em cima do id de um jogo, saímos agora. Nada
    // aqui pra baixo chega a executar.
    if (running_as_hijacked_title())
    {
        refuse_and_explain();
        return EXIT_SUCCESS;
    }

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
