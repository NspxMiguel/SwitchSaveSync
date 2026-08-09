// SwitchSaveSync — versão com interface gráfica.
//
// A lógica (OAuth, Drive, montar save) é a mesma de ../core, compartilhada
// com o app de console em ../app. Aqui só tem UI: telas, listas e o que
// acontece quando aperta A.
//
// Tudo que demora roda num Job (thread separada) e aparece numa JobPage, pra
// tela nunca congelar. Ver job.hpp.
//
// Os textos saem em português ou inglês pelo TR() do core/lang.h.

extern "C" {
#include "config.h"
#include "drive.h"
#include "http.h"
#include "lang.h"
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
#include <vector>

#include "job.hpp"
#include "job_page.hpp"
#include "parental.hpp"

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
static brls::ListItem* g_pin_item     = nullptr;

// Caminho deste .nro, guardado do argv[0] — é o que envSetNextLoad precisa pra
// reabrir o app quando ele troca de idioma.
static std::string g_nro_path;

// Diagnóstico da inicialização de rede, mostrado na aba Conta.
static Result g_socket_rc = 0;
static bool g_socket_lean = false;
static bool g_applet_mode = false;

// ============================ pontes com o ../core ============================
// O core é C puro e os callbacks dele são ponteiros de função sem userdata,
// então acham o job em andamento pela global Job::current.

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

    std::string verb = strcmp(action, "dir") == 0 ? TR("pasta", "folder")
        : strcmp(action, "up") == 0               ? TR("enviado", "uploaded")
                                                  : TR("baixado", "downloaded");

    if (ok)
        job->log(verb + ": " + name);
    else
        job->log(std::string(TR("falhou: ", "failed: ")) + name, true);
}

extern "C" void gui_oauth_status(const char* msg)
{
    if (!Job::current)
        return;

    Job::current->setStatus(msg);

    // Também vai pro log: o status é uma linha só, e a próxima mensagem apaga
    // a anterior. Quando o login falha, a explicação ("status 401",
    // "invalid_client") chegava aqui e sumia antes de ele conseguir ler — era
    // o "Login nao concluido. e mais nada".
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
        g_account_item->setValue(logged ? TR("conectada", "connected")
                                        : TR("desconectada", "disconnected"),
            !logged);

    // Com a conta conectada, "Entrar" não faz sentido nenhum na tela — ele
    // conversa privada removida do historico
    // conversa privada removida do historico
    // trocar de conta, mas dizendo o que realmente faz.
    if (g_login_item)
    {
        g_login_item->setLabel(logged ? TR("Trocar de conta", "Switch account")
                                      : TR("Entrar", "Sign in"));
        g_login_item->setValue(logged ? TR("já conectado", "already signed in") : "", logged);
    }

    if (g_logout_item)
        g_logout_item->setValue(logged ? "" : TR("nada pra sair", "not signed in"), !logged);

    if (g_root)
        g_root->setSubtitle(logged ? TR("Google Drive: conta conectada",
                                        "Google Drive: account connected")
                                   : TR("Google Drive: nenhuma conta conectada",
                                        "Google Drive: no account connected"),
            "v" APP_VERSION_STRING);
}

// "De quem é este save?" — string vazia quando não há com o que confundir.
//
// Só aparece quando o jogo tem mais de um save neste console. É a mesma regra
// que decide o nome da pasta no Drive (save_folder_name, no syncjob.c): tem que
// bater, senão a tela diz uma coisa e a nuvem guarda outra.
static std::string saveOwnerLabel(const TitleEntry& title)
{
    if (!title.shared_game)
        return "";
    if (title.device_save)
        return TR("console (vale pra todas as contas)", "console (shared by all accounts)");
    if (title.account[0])
        return title.account;
    return "";
}

// O nome curto do dono, pra linha da tela de escolha. Ao contrário do de cima,
// este responde sempre — quem chama já sabe que há mais de um save.
static std::string saveOwnerName(const TitleEntry& title)
{
    if (title.device_save)
        return TR("Console", "Console");
    if (title.account[0])
        return title.account;
    return TR("Conta sem apelido", "Unnamed account");
}

static std::string saveOwnerDescription(const TitleEntry& title)
{
    if (title.device_save)
        return TR("Save do próprio console, sem dono. Vale pra todos os perfis — "
                  "é onde jogos como o Animal Crossing guardam a ilha.",
            "The console's own save, with no owner. Shared by every profile — "
            "it's where games like Animal Crossing keep the island.");
    return TR("Save desta conta do console.", "This console profile's save.");
}

// Jogo + dono, quando há dono pra mostrar. Usado no título das telas e dos
// trabalhos, pra ele nunca ficar em dúvida sobre qual save está mexendo.
static std::string titleWithOwner(const TitleEntry& title)
{
    std::string dono = saveOwnerLabel(title);
    return dono.empty() ? std::string(title.name)
                        : std::string(title.name) + " — " + dono;
}

// Todos os saves de um jogo neste console: uma conta, várias contas, ou uma
// conta e o console. Relê a lista global em vez de guardar índices porque a
// lista é recarregada com o X, e índice velho apontaria pro jogo errado.
static std::vector<TitleEntry> savesOf(u64 application_id)
{
    std::vector<TitleEntry> out;
    for (size_t i = 0; i < g_titles_count; i++)
        if (g_titles[i].application_id == application_id)
            out.push_back(g_titles[i]);
    return out;
}

static void updatePinItem()
{
    if (!g_pin_item)
        return;

    // Só label e valor mudam: a ListItem da borealis não tem setter pra
    // descrição (ela só entra no construtor), então o texto de baixo é fixo e
    // quem conta o estado é o valor da direita.
    bool on = Parental::isSet();
    g_pin_item->setLabel(on ? TR("Trocar ou tirar a senha", "Change or remove the password")
                            : TR("Ligar a senha", "Turn the password on"));
    g_pin_item->setValue(on ? TR("ligada", "on") : TR("desligada", "off"), !on);
}

// Escolher senha nova. Pergunta duas vezes de propósito: senha digitada
// errada e confirmada errada tranca o app do dono, e destrancar exigiria
// achar o arquivo no cartão pelo PC. Barato prevenir, caro consertar.
static void askForNewPin()
{
    std::string primeira, segunda;

    if (!Parental::prompt(TR("Senha nova", "New password"),
            TR("Escolha de 4 a 8 números", "Pick 4 to 8 digits"), primeira))
        return; // cancelou

    if (!Parental::prompt(TR("Confirme a senha", "Confirm the password"),
            TR("Digite os mesmos números de novo", "Type the same digits again"), segunda))
        return;

    if (primeira != segunda)
    {
        brls::Application::notify(TR("As duas não bateram — a senha continua como estava",
            "They didn't match — the password is unchanged"));
        return;
    }

    if (!Parental::save(primeira))
    {
        brls::Application::notify(TR("Não consegui gravar no cartão — a senha NÃO foi ligada",
            "Couldn't write to the SD card — the password was NOT turned on"));
        return;
    }

    brls::Application::notify(TR("Senha ligada", "Password on"));
    updatePinItem();
}

static void onPinItemClicked()
{
    if (!Parental::isSet())
    {
        askForNewPin();
        return;
    }

    // Já tem senha: só passa por aqui quem souber a atual. Sem isso, a
    // criança que já está dentro do app desligaria a trava em dois cliques.
    std::string atual;
    if (!Parental::prompt(TR("Senha atual", "Current password"),
            TR("Confirme que é você", "Confirm it's you"), atual))
        return;

    if (!Parental::matches(atual))
    {
        brls::Application::notify(TR("Senha errada", "Wrong password"));
        return;
    }

    brls::Dialog* dialog = new brls::Dialog(TR("Senha atual conferida. O que você quer fazer?",
        "Password confirmed. What do you want to do?"));

    dialog->addButton(TR("Trocar a senha", "Change it"), [dialog](brls::View* view) {
        dialog->close([]() { askForNewPin(); });
    });
    dialog->addButton(TR("Tirar a senha", "Remove it"), [dialog](brls::View* view) {
        dialog->close([]() {
            Parental::clear();
            updatePinItem();
            brls::Application::notify(TR("Senha removida — o app abre direto agora",
                "Password removed — the app opens straight away now"));
        });
    });

    dialog->setCancelable(true);
    dialog->open();
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

    job->setStatus(TR("A rede não subiu quando o app abriu.",
        "The network didn't come up when the app started."));
    job->log(TR("Detalhes na aba Conta, embaixo de Diagnóstico.",
                "Details are in the Account tab, under Diagnostics."),
        true);
    return false;
}

static bool ensureToken(Job* job, char* out, size_t outsz)
{
    if (!ensureNetwork(job))
        return false;

    if (!oauth_is_logged_in())
    {
        job->setStatus(TR("Você ainda não conectou uma conta Google.",
            "You haven't connected a Google account yet."));
        job->log(TR("Vá na aba Conta e escolha Entrar.",
                    "Go to the Account tab and choose Sign in."),
            true);
        return false;
    }

    job->setStatus(TR("Renovando o acesso ao Google...", "Refreshing Google access..."));
    if (!oauth_get_fresh_access_token(out, outsz))
    {
        job->setStatus(TR("Não consegui renovar o acesso.", "Couldn't refresh the access token."));
        job->log(TR("Entre na conta de novo pela aba Conta.",
                    "Sign in again from the Account tab."),
            true);
        return false;
    }

    return true;
}

// ============================ trabalhos ============================

static bool jobLogin(Job* job)
{
    if (!ensureNetwork(job))
        return false;

    job->setStatus(TR("Pedindo um código ao Google...", "Asking Google for a code..."));

    if (!oauth_start_device_flow(gui_oauth_status, gui_oauth_cancel))
    {
        job->setStatus(TR("Login não concluído.", "Sign-in didn't complete."));
        job->log(TR("O motivo está na linha acima. \"status 401\" ou \"invalid_client\" = o "
                    "CLIENT_ID/SECRET do core/config.h não confere com o que está no "
                    "Google Cloud.",
                    "The reason is on the line above. \"status 401\" or \"invalid_client\" = "
                    "the CLIENT_ID/SECRET in core/config.h doesn't match what's in "
                    "Google Cloud."),
            true);
        job->log(TR("Se o navegador falou em \"app não verificado\", clique em Avançado e "
                    "depois em Acessar SwitchSaveSync.",
                    "If the browser said \"app isn't verified\", click Advanced and then "
                    "Go to SwitchSaveSync."),
            true);
        return false;
    }

    job->setStatus(TR("Conta conectada. O token fica salvo no cartão, não precisa repetir.",
        "Account connected. The token is saved on the SD card — no need to do this again."));
    return true;
}

static bool jobConnectionTest(Job* job)
{
    if (!ensureNetwork(job))
        return false;

    char token[512];
    if (!ensureToken(job, token, sizeof(token)))
        return false;

    job->setStatus(TR("Procurando a pasta no Drive...", "Looking for the folder on Drive..."));
    char folder[128];
    if (!drive_ensure_app_folder(token, folder, sizeof(folder)))
    {
        job->setStatus(TR("Não consegui criar/achar a pasta no Drive.",
            "Couldn't create/find the folder on Drive."));
        return false;
    }
    job->log(std::string(TR("pasta pronta: ", "folder ready: ")) + DRIVE_APP_FOLDER_NAME);

    mkdir(APP_DIR, 0777);
    FILE* f = fopen(TEST_LOCAL, "w");
    if (!f)
    {
        job->setStatus(TR("Não consegui escrever no cartão SD.", "Couldn't write to the SD card."));
        return false;
    }
    fprintf(f, "SwitchSaveSync - arquivo de teste / test file.\n");
    fprintf(f, "Se voce esta lendo isso depois de um download, o ciclo "
               "upload->Drive->download funcionou.\n");
    fclose(f);

    job->setStatus(TR("Enviando um arquivo de teste...", "Uploading a test file..."));
    if (!drive_upload(token, folder, TEST_REMOTE, TEST_LOCAL, "text/plain"))
    {
        job->setStatus(TR("O upload falhou.", "The upload failed."));
        return false;
    }
    job->log(TR("upload OK", "upload OK"));

    job->setStatus(TR("Baixando o mesmo arquivo de volta...", "Downloading the same file back..."));
    if (!drive_download(token, folder, TEST_REMOTE, TEST_DOWN))
    {
        job->setStatus(TR("O download falhou.", "The download failed."));
        return false;
    }
    job->log(TR("download OK", "download OK"));

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
                job->log(std::string(TR("recebido do Drive: ", "got back from Drive: ")) + s);
        }
        fclose(f);
    }

    job->setStatus(TR("Rede, TLS e Drive API funcionando.", "Network, TLS and the Drive API work."));
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

    job->setStatus(TR("Comparando o save do console com o do Drive...",
        "Comparing the console's save with the one on Drive..."));

    switch (syncjob_sync_title(&title, jobLogLine))
    {
        case SYNCJOB_SYNC_UPLOADED:
            job->setStatus(TR("Pronto. O save daqui está guardado no Drive.",
                "Done. This console's save is stored on Drive."));
            return true;

        case SYNCJOB_SYNC_DOWNLOADED:
            job->setStatus(TR("Pronto. O save do Drive está no console.",
                "Done. The save from Drive is on the console."));
            return true;

        case SYNCJOB_SYNC_EQUAL:
            job->setStatus(TR("Já estava sincronizado. Não mexi em nada.",
                "Already in sync. Nothing was touched."));
            return true;

        case SYNCJOB_SYNC_NOTHING:
            job->setStatus(TR("Esse jogo ainda não tem save nenhum pra sincronizar.",
                "This game has no save yet, on either side."));
            return true;

        case SYNCJOB_SYNC_CONFLICT:
            job->setStatus(TR("Os dois lados têm save e são diferentes. Escolher sozinho "
                              "apagaria progresso de um deles — escolha você, abaixo.",
                "Both sides have a save and they differ. Choosing on my own would erase "
                "progress from one of them — you pick, below."));

            // A ordem importa: a primeira é a que recebe o foco, e o caso que
            // leva ao conflito quase sempre é jogo reinstalado com o save de
            // verdade no Drive. Nada é escrito até ele apertar A.
            job->offerChoice(TR("Trazer o save do Drive pro console",
                                   "Bring the save from Drive to the console"),
                TR("Apaga o save que está no console e põe o do Drive no lugar. É o que "
                   "você quer se reinstalou o jogo.",
                    "Erases the console's save and puts the one from Drive in its place. "
                    "This is what you want if you reinstalled the game."),
                [title] {
                    openJob(new Job(std::string(TR("Restaurar — ", "Restore — ")) + titleWithOwner(title),
                                [title](Job* j) { return jobRestore(j, title); }),
                        false);
                });

            job->offerChoice(TR("Mandar o save do console pro Drive",
                                   "Send the console's save to Drive"),
                TR("Substitui o que está no Drive pelo save deste console.",
                    "Replaces what's on Drive with this console's save."),
                [title] {
                    openJob(new Job(std::string(TR("Backup — ", "Backup — ")) + titleWithOwner(title),
                                [title](Job* j) { return jobBackup(j, title); }),
                        false);
                });

            return false;

        case SYNCJOB_SYNC_FAILED:
        default:
            job->setStatus(TR("Não consegui sincronizar. O motivo está nas linhas acima.",
                "Couldn't sync. The reason is in the lines above."));
            return false;
    }
}

static bool jobBackup(Job* job, TitleEntry title)
{
    if (!ensureLogin(job))
        return false;

    job->setStatus(TR("Enviando o save pro Drive...", "Uploading the save to Drive..."));
    if (!syncjob_backup_title(&title, jobLogLine))
    {
        job->setStatus(TR("O backup não terminou. Nada foi alterado no console.",
            "The backup didn't finish. Nothing on the console was changed."));
        return false;
    }

    job->setStatus(std::string(TR("Backup concluído. Está em ", "Backup done. It's in "))
        + DRIVE_APP_FOLDER_NAME + "/" + title.name
        + TR("/ no seu Drive.", "/ on your Drive."));
    return true;
}

// conversa privada removida do historico
// conversa privada removida do historico
static bool jobBackupLocal(Job* job, TitleEntry title)
{
    job->setStatus(TR("Copiando o save pro cartão...", "Copying the save to the SD card..."));
    if (!syncjob_backup_title_local(&title, jobLogLine))
    {
        job->setStatus(TR("Não consegui copiar o save pro cartão.",
            "Couldn't copy the save to the SD card."));
        return false;
    }

    job->setStatus(TR("Save guardado no cartão, em switch/SwitchSaveSync/backups.",
        "Save stored on the SD card, in switch/SwitchSaveSync/backups."));
    return true;
}

static bool jobRestoreLocal(Job* job, TitleEntry title)
{
    job->setStatus(TR("Gravando o save do cartão no console...",
        "Writing the SD card's save onto the console..."));
    if (!syncjob_restore_title_local(&title, jobLogLine))
    {
        job->setStatus(TR("Não consegui restaurar do cartão.", "Couldn't restore from the SD card."));
        return false;
    }

    job->setStatus(TR("Save restaurado do cartão.", "Save restored from the SD card."));
    return true;
}

static bool jobRestore(Job* job, TitleEntry title)
{
    if (!ensureLogin(job))
        return false;

    job->setStatus(TR("Trazendo o save do Drive...", "Fetching the save from Drive..."));
    if (!syncjob_restore_title(&title, jobLogLine))
    {
        job->setStatus(TR("Não restaurei nada. Se falhou no meio da gravação, o save do "
                          "console continua como estava (nada é commitado sem dar certo).",
            "Nothing was restored. If it failed mid-write, the console's save is still as "
            "it was (nothing is committed unless it succeeds)."));
        return false;
    }

    job->setStatus(TR("Save restaurado do Drive.", "Save restored from Drive."));
    return true;
}

// ============================ telas ============================

// Tela de um jogo: o que fazer quando o clique simples não serve.
//
// Ela NÃO é mais o caminho normal — o caminho normal é apertar A na lista e o
// app resolver sozinho ("clica pra synca o save com a nuvem e PRONTO"). Isso
// aqui é o Y: mandar pra que lado quando os dois lados mudaram, e as cópias no
// próprio cartão, que não dependem de internet nem de conta.
static void openGamePage(const TitleEntry& title)
{
    brls::AppletFrame* frame = new brls::AppletFrame(true, true);
    frame->setTitle(titleWithOwner(title));

    size_t iconLen = 0;
    if (titles_get_icon(title.application_id, g_icon_buffer, sizeof(g_icon_buffer), &iconLen))
        frame->setIcon(g_icon_buffer, iconLen);

    brls::List* list = new brls::List();

    brls::ListItem* backupItem = new brls::ListItem(
        TR("Enviar o save pro Drive", "Upload the save to Drive"),
        TR("Lê o save que está no console e sobe pro Google Drive. Não altera nada no "
           "console.",
            "Reads the console's save and uploads it to Google Drive. Nothing on the "
            "console is changed."));
    backupItem->getClickEvent()->subscribe([title](brls::View* view) {
        openJob(new Job(std::string(TR("Backup — ", "Backup — ")) + titleWithOwner(title),
                    [title](Job* job) { return jobBackup(job, title); }),
            false);
    });

    brls::ListItem* restoreItem = new brls::ListItem(
        TR("Baixar o save do Drive", "Download the save from Drive"),
        TR("Substitui o save que está no console pelo que está no Drive.",
            "Replaces the console's save with the one on Drive."));
    restoreItem->getClickEvent()->subscribe([title](brls::View* view) {
        brls::Dialog* dialog = new brls::Dialog(
            TR(std::string("Isso apaga o save de \"") + titleWithOwner(title)
                    + "\" que está no console e põe no lugar o que está no Drive. O save "
                      "atual se perde.\n\nContinuar?",
                std::string("This erases the \"") + titleWithOwner(title)
                    + "\" save on the console and puts the one from Drive in its place. "
                      "The current save is lost.\n\nContinue?"));

        dialog->addButton(TR("Cancelar", "Cancel"), [dialog](brls::View* view) { dialog->close(); });
        dialog->addButton(TR("Restaurar", "Restore"), [dialog, title](brls::View* view) {
            dialog->close([title]() {
                openJob(new Job(std::string(TR("Restaurar — ", "Restore — ")) + titleWithOwner(title),
                            [title](Job* job) { return jobRestore(job, title); }),
                    false);
            });
        });

        dialog->setCancelable(true);
        dialog->open();
    });

    brls::ListItem* backupLocalItem = new brls::ListItem(
        TR("Salvar o save no cartão SD", "Save to the SD card"),
        TR("Guarda uma cópia em switch/SwitchSaveSync/backups, no próprio console. Não "
           "usa internet nem conta Google.",
            "Keeps a copy in switch/SwitchSaveSync/backups, on the console itself. Uses "
            "no internet and no Google account."));
    backupLocalItem->getClickEvent()->subscribe([title](brls::View* view) {
        openJob(new Job(std::string(TR("Backup no cartão — ", "SD card backup — ")) + titleWithOwner(title),
                    [title](Job* job) { return jobBackupLocal(job, title); }),
            false);
    });

    brls::ListItem* restoreLocalItem = new brls::ListItem(
        TR("Restaurar do cartão SD", "Restore from the SD card"),
        TR("Substitui o save que está no console pela cópia guardada no cartão.",
            "Replaces the console's save with the copy kept on the SD card."));
    restoreLocalItem->getClickEvent()->subscribe([title](brls::View* view) {
        brls::Dialog* dialog = new brls::Dialog(
            TR(std::string("Isso apaga o save de \"") + titleWithOwner(title)
                    + "\" que está no console e põe no lugar a cópia do cartão. O save "
                      "atual se perde.\n\nContinuar?",
                std::string("This erases the \"") + titleWithOwner(title)
                    + "\" save on the console and puts the SD card copy in its place. The "
                      "current save is lost.\n\nContinue?"));

        dialog->addButton(TR("Cancelar", "Cancel"), [dialog](brls::View* view) { dialog->close(); });
        dialog->addButton(TR("Restaurar", "Restore"), [dialog, title](brls::View* view) {
            dialog->close([title]() {
                openJob(new Job(std::string(TR("Restaurar do cartão — ", "SD card restore — "))
                            + titleWithOwner(title),
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

// conversa privada removida do historico
// conversa privada removida do historico
//
// Antes o mesmo jogo aparecia duas vezes na lista, uma por conta, e as duas
// linhas tinham o mesmo ícone e o mesmo nome — dava pra sincronizar a conta
// errada sem perceber. Agora o jogo aparece uma vez só e a escolha acontece
// aqui, com nome de gente na tela.
//
// Com um save só isso nem aparece: chama `then` na hora, e o clique continua
// sendo um clique só.
static void pickSave(u64 application_id, std::function<void(TitleEntry)> then)
{
    std::vector<TitleEntry> saves = savesOf(application_id);

    if (saves.empty())
        return;

    if (saves.size() == 1)
    {
        then(saves[0]);
        return;
    }

    brls::AppletFrame* frame = new brls::AppletFrame(true, true);
    frame->setTitle(saves[0].name);
    frame->setFooterText(TR("Escolha de quem é o save", "Pick whose save"));

    size_t iconLen = 0;
    if (titles_get_icon(application_id, g_icon_buffer, sizeof(g_icon_buffer), &iconLen))
        frame->setIcon(g_icon_buffer, iconLen);

    brls::List* list = new brls::List();

    for (const TitleEntry& save : saves)
    {
        brls::ListItem* item = new brls::ListItem(saveOwnerName(save), saveOwnerDescription(save));
        item->getClickEvent()->subscribe([save, then](brls::View* view) {
            // Sai da tela de escolha ANTES de abrir o trabalho, senão ela
            // ficaria empilhada atrás e o B do fim voltaria pra uma pergunta
            // que ele já respondeu.
            brls::Application::popView(brls::ViewAnimation::FADE, [save, then]() { then(save); });
        });
        list->addView(item);
    }

    list->addView(new brls::Label(brls::LabelStyle::DESCRIPTION,
        TR("Cada save destes é separado: tem a sua pasta no Drive e no cartão, e "
           "sincronizar um não encosta no outro.",
            "Each of these saves is separate: it has its own folder on Drive and on the "
            "SD card, and syncing one doesn't touch the other."),
        true));

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
            TR("Nenhum jogo com save de usuário foi encontrado neste console.",
                "No game with save data was found on this console."),
            true));
        return;
    }

    for (size_t i = 0; i < g_titles_count; i++)
    {
        TitleEntry title = g_titles[i];

        // Um jogo, uma linha — mesmo com dois saves. A segunda entrada do
        // mesmo application_id é pulada aqui; quem separa as contas agora é a
        // tela do pickSave, no clique.
        bool jaListado = false;
        for (size_t j = 0; j < i; j++)
            if (g_titles[j].application_id == title.application_id)
            {
                jaListado = true;
                break;
            }
        if (jaListado)
            continue;

        brls::ListItem* item = new brls::ListItem(title.name);

        // Quantos saves esse jogo tem aqui. Vale a pena dizer na lista: é o
        // aviso de que apertar A vai perguntar algo antes de fazer.
        size_t quantos = savesOf(title.application_id).size();
        if (quantos > 1)
        {
            char valor[32];
            snprintf(valor, sizeof(valor), TR("%zu saves", "%zu saves"), quantos);
            item->setValue(valor);
        }

        size_t iconLen = 0;
        if (titles_get_icon(title.application_id, g_icon_buffer, sizeof(g_icon_buffer), &iconLen))
            item->setThumbnail(g_icon_buffer, iconLen); // a Image copia o buffer

        // A sincroniza direto. O app decide sozinho pra que lado vai; só para
        // pra perguntar quando há mais de um save, ou quando os dois lados
        // mudaram.
        u64 appId = title.application_id;
        item->getClickEvent()->subscribe([appId](brls::View* view) {
            pickSave(appId, [](TitleEntry escolhido) {
                openJob(new Job(std::string(TR("Sincronizando — ", "Syncing — "))
                            + titleWithOwner(escolhido),
                            [escolhido](Job* job) { return jobSync(job, escolhido); }),
                    false);
            });
        });

        // Y abre a tela com as ações separadas. Fica registrado no item, e não
        // na lista, porque é assim que a ação sabe de QUAL jogo ela é: a
        // borealis procura a ação no item que está em foco.
        item->registerAction(TR("Mais opções", "More options"), brls::Key::Y, [appId] {
            pickSave(appId, [](TitleEntry escolhido) { openGamePage(escolhido); });
            return true;
        });

        list->addView(item);
    }
}

static brls::List* createGamesTab()
{
    brls::List* list = new brls::List();
    fillGamesList(list);

    // Recarregar a lista sem sair do app (útil depois de instalar um jogo novo
    // ou criar um save).
    list->registerAction(TR("Atualizar lista", "Refresh list"), brls::Key::X, [list] {
        fillGamesList(list);
        brls::Application::notify(TR("Lista atualizada", "List refreshed"));
        return true;
    });

    return list;
}

// Reabrir o app depois de trocar de idioma.
//
// Não é preguiça: os textos das abas, da barra lateral e de cada linha são
// montados uma vez, na abertura, e a borealis não tem como refazer a barra
// lateral depois de pronta. Trocar o idioma sem reabrir deixaria metade da tela
// numa língua e metade na outra. envSetNextLoad pede pro hbloader abrir este
// mesmo .nro assim que este processo sair, então a volta é imediata.
static void restartApp()
{
    if (!envHasNextLoad() || g_nro_path.empty()
        || R_FAILED(envSetNextLoad(g_nro_path.c_str(), g_nro_path.c_str())))
    {
        brls::Application::notify(TR("Abra o app de novo pra trocar o idioma",
            "Open the app again to switch the language"));
        return;
    }

    brls::Application::quit();
}

static brls::List* createAccountTab()
{
    brls::List* list = new brls::List();

    list->addView(new brls::Header(TR("Conta do Google Drive", "Google Drive account"), false));

    g_account_item = new brls::ListItem(TR("Conta Google", "Google account"));
    list->addView(g_account_item);

    brls::ListItem* loginItem = new brls::ListItem(TR("Entrar", "Sign in"),
        TR("Abre um código pra você digitar no celular ou no PC. O Switch não pede senha "
           "em momento nenhum.",
            "Shows a code for you to type on your phone or PC. The Switch never asks for "
            "a password."));
    loginItem->getClickEvent()->subscribe([](brls::View* view) {
        openJob(new Job(TR("Entrar na conta Google", "Sign in to Google"), jobLogin), true,
            [](bool success) { updateAccountViews(); });
    });
    list->addView(loginItem);
    g_login_item = loginItem;

    brls::ListItem* logoutItem = new brls::ListItem(TR("Sair da conta", "Sign out"),
        TR("Apaga o token guardado no cartão. Não mexe em nada no Drive.",
            "Deletes the token kept on the SD card. Nothing on Drive is touched."));
    logoutItem->getClickEvent()->subscribe([](brls::View* view) {
        oauth_logout();
        updateAccountViews();
        brls::Application::notify(TR("Conta desconectada", "Account disconnected"));
    });
    list->addView(logoutItem);
    g_logout_item = logoutItem;

    // ---- idioma ----
    list->addView(new brls::Header(TR("Idioma", "Language"), false));

    // A ordem aqui é a mesma do enum LangChoice (auto, pt, en), então o índice
    // que a borealis devolve já é o valor — sem tabela de conversão pra
    // dessincronizar depois.
    brls::SelectListItem* langItem = new brls::SelectListItem(
        TR("Idioma do app", "App language"),
        { TR("Automático (segue o console)", "Automatic (follow the console)"),
            "Português", "English" },
        (unsigned)lang_choice(),
        TR("No automático, o app fala a língua do console. O app reabre sozinho pra "
           "trocar tudo de uma vez.",
            "On automatic, the app follows the console's language. The app reopens itself "
            "to switch everything at once."));

    langItem->getValueSelectedEvent()->subscribe([](int selected) {
        if (selected < 0)
            return; // cancelou o dropdown

        LangChoice escolha = (LangChoice)selected;
        if (escolha == lang_choice())
            return;

        if (!lang_set(escolha))
        {
            brls::Application::notify(TR("Não consegui gravar a escolha no cartão",
                "Couldn't save the choice to the SD card"));
            return;
        }

        restartApp();
    });
    list->addView(langItem);

    // ---- controle parental ----
    list->addView(new brls::Header(TR("Controle parental", "Parental control"), false));

    g_pin_item = new brls::ListItem(TR("Ligar a senha", "Turn the password on"),
        TR("Uma senha de 4 a 8 números, pedida toda vez que o app abre. Pra trocar ou "
           "tirar depois, o app pede a senha atual antes.",
            "A 4 to 8 digit password, asked every time the app opens. To change or remove "
            "it later, the app asks for the current one first."));
    g_pin_item->getClickEvent()->subscribe([](brls::View* view) { onPinItemClicked(); });
    list->addView(g_pin_item);
    updatePinItem();

    list->addView(new brls::Label(brls::LabelStyle::DESCRIPTION,
        TR("Com a senha ligada, o app pergunta os números toda vez que abre — sem "
           "acertar, ninguém chega nos botões que mexem em save.\n\n"
           "Não é cofre: quem pegar o cartão SD no PC apaga o arquivo da senha e entra. "
           "Serve pra criança não apagar progresso sem querer, e é só pra isso que dá "
           "pra contar com ela.",
            "With the password on, the app asks for the digits every time it opens — "
            "without them, nobody reaches the buttons that touch saves.\n\n"
            "It is not a vault: anyone who puts the SD card in a PC can delete the "
            "password file and get in. It's there so a kid doesn't wipe progress by "
            "accident, and that's all it can be counted on for."),
        true));

    // ---- diagnóstico ----
    list->addView(new brls::Header(TR("Diagnóstico", "Diagnostics"), false));

    brls::ListItem* testItem = new brls::ListItem(TR("Testar conexão", "Test the connection"),
        TR("Sobe e baixa um arquivinho de teste. Não encosta em save nenhum.",
            "Uploads and downloads a tiny test file. No save is touched."));
    testItem->getClickEvent()->subscribe([](brls::View* view) {
        openJob(new Job(TR("Teste de conexão", "Connection test"), jobConnectionTest), true);
    });
    list->addView(testItem);

    // Estado da rede em texto, pra parar de sair só "falha ao iniciar a rede"
    // sem dizer o que falhou.
    char net[640];
    if (R_SUCCEEDED(g_socket_rc))
    {
        snprintf(net, sizeof(net), TR("Rede iniciada%s.\nModo: %s.", "Network up%s.\nMode: %s."),
            g_socket_lean ? TR(" (config enxuta, o padrão não coube na memória)",
                              " (lean config, the default didn't fit in memory)")
                          : "",
            g_applet_mode ? TR("APPLET", "APPLET") : TR("aplicação", "application"));
    }
    else
    {
        snprintf(net, sizeof(net),
            TR("A rede NÃO iniciou. Erro %08X (módulo %d, descrição %d).\nModo: %s.%s",
                "The network did NOT start. Error %08X (module %d, description %d).\n"
                "Mode: %s.%s"),
            (unsigned)g_socket_rc, R_MODULE(g_socket_rc), R_DESCRIPTION(g_socket_rc),
            g_applet_mode ? TR("APPLET", "APPLET") : TR("aplicação", "application"),
            g_applet_mode
                ? TR("\n\nQuase certo que é isso: em modo applet o app fica com ~448 MB no "
                     "total, e a interface gráfica + a rede não cabem. Feche o hbmenu, "
                     "segure R e abra um JOGO — o hbmenu abre no lugar dele em modo "
                     "aplicação, com ~3 GB.",
                      "\n\nAlmost certainly this: in applet mode the app gets ~448 MB "
                      "total, and the GUI plus the network don't fit. Close the hbmenu, "
                      "hold R and open a GAME — the hbmenu opens in its place in "
                      "application mode, with ~3 GB.")
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
    brls::ListItem* versionItem = new brls::ListItem(TR("Versão", "Version"));
    versionItem->setValue(APP_VERSION_STRING);
    list->addView(versionItem);

    list->addView(new brls::ListItem(TR("O que este app faz", "What this app does"),
        TR("Sincroniza os saves dos seus jogos com o Google Drive, na pasta \"" DRIVE_APP_FOLDER_NAME
           "\". Cada jogo vira uma subpasta com o nome dele, e os arquivos ficam soltos lá "
           "dentro — dá pra baixar pelo site do Drive normalmente.",
            "Syncs your games' saves with Google Drive, in the \"" DRIVE_APP_FOLDER_NAME
            "\" folder. Each game becomes a subfolder named after it, with the files "
            "loose inside — you can download them from the Drive website normally.")));

    list->addView(new brls::ListItem(TR("Como sincronizar", "How to sync"),
        TR("Na lista de jogos, A sincroniza sozinho: ele compara o save do console com o "
           "da nuvem e decide o lado. Se o jogo tiver mais de um save (duas contas, ou "
           "uma conta e o console), ele pergunta antes de qual você quer. Se os dois "
           "lados mudaram desde a última vez, ele não escolhe por você — para e deixa a "
           "escolha na tela. Só aparecem jogos instalados no console.",
            "In the game list, A syncs on its own: it compares the console's save with "
            "the cloud one and picks the direction. If the game has more than one save "
            "(two accounts, or one account and the console), it asks which one first. If "
            "both sides changed since last time, it won't choose for you — it stops and "
            "leaves the choice on screen. Only games installed on the console show up.")));

    list->addView(new brls::ListItem(TR("Privacidade", "Privacy"),
        TR("O app fala direto com o Google, sem servidor no meio. O acesso pedido é o "
           "\"drive.file\": o app só enxerga os arquivos que ele mesmo criou, não o resto "
           "do seu Drive. O login fica só no cartão, em "
           "/switch/SwitchSaveSync/token.txt, e sai de vez com o Sair da conta.",
            "The app talks straight to Google, with no server in between. The scope it "
            "asks for is \"drive.file\": it only sees the files it created itself, not "
            "the rest of your Drive. The login stays on the SD card only, in "
            "/switch/SwitchSaveSync/token.txt, and Sign out removes it for good.")));

    list->addView(new brls::ListItem(TR("Em que pé está o projeto", "Where the project stands"),
        TR("Esta é a versão avulsa, em que você abre o app e sincroniza na mão. O próximo "
           "passo é o sysmodule, que faz isso sozinho ao entrar e sair do jogo — fora do "
           "escopo por enquanto.",
            "This is the standalone version, where you open the app and sync by hand. The "
            "next step is the sysmodule, doing it by itself when a game starts and stops "
            "— out of scope for now.")));

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
    // Guardado antes de qualquer coisa: é o caminho que o restartApp() usa pra
    // reabrir o app quando ele troca de idioma.
    if (argc > 0 && argv[0])
        g_nro_path = argv[0];

    // O idioma antes de tudo, senão a primeira tela do app (a da senha) sairia
    // sempre em português.
    lang_load();

    // A senha, logo em seguida.
    //
    // Aqui em cima de propósito: a borealis ainda não subiu (nada de vídeo,
    // nada de rede nossa), então o teclado do console aparece sozinho na tela
    // e sair é só dar return — não tem nada montado pra desmontar. Colocar a
    // pergunta depois da interface significaria a criança ver a lista de jogos
    // por trás do teclado, que é justamente o que a trava existe pra evitar.
    if (!Parental::unlockAtStartup())
        return EXIT_SUCCESS;

    // romfs antes da borealis: ela carrega as fontes de romfs:/ na init.
    // O cacert.pem que o http.c usa também mora lá.
    // Mesma história da rede: a borealis já chama romfsInit() no userAppInit().
    // Uma segunda montagem responde "já está montado", que é sucesso disfarçado
    // de erro. Só desmontamos no fim se fomos nós que montamos.
    Result rc        = romfsInit();
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
    g_socket_rc        = socketInitializeDefault();

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
            g_socket_lean = true;
            we_own_socket = true;
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

    g_root->addTab(TR("Meus jogos", "My games"), createGamesTab());
    g_root->addTab(TR("Conta", "Account"), createAccountTab());
    g_root->addSeparator();
    g_root->addTab(TR("Sobre", "About"), createAboutTab());

    updateAccountViews();

    brls::Application::pushView(g_root);

    // Primeira vez no app (nenhuma conta salva): já oferece o login em vez de
    // deixar ele descobrir sozinho que precisa ir na aba Conta.
    if (!oauth_is_logged_in() && http_ok)
    {
        brls::Dialog* welcome = new brls::Dialog(
            TR("Pra sincronizar os saves, o app precisa de uma conta do Google Drive.\n\n"
               "Você conecta pelo celular ou pelo PC: aparece um QR code aqui, você aponta "
               "a câmera, e pronto. O Switch não pede senha em momento nenhum.",
                "To sync saves, the app needs a Google Drive account.\n\nYou connect from "
                "your phone or PC: a QR code shows up here, you point the camera at it, "
                "and that's it. The Switch never asks for a password."));

        welcome->addButton(TR("Agora não", "Not now"),
            [welcome](brls::View* view) { welcome->close(); });
        welcome->addButton(TR("Conectar conta", "Connect account"), [welcome](brls::View* view) {
            welcome->close([]() {
                openJob(new Job(TR("Entrar na conta Google", "Sign in to Google"), jobLogin), true,
                    [](bool success) { updateAccountViews(); });
            });
        });

        welcome->setCancelable(true);
        welcome->open();
    }

    // Se a rede ou o romfs não subiram, o app abre mesmo assim — mas avisa, em
    // vez de só falhar depois em toda operação sem explicar por quê.
    if (!romfs_ok)
        brls::Application::notify(TR("Falha ao montar o romfs: os certificados não carregaram",
            "Failed to mount romfs: the certificates didn't load"));
    else if (!http_ok && g_applet_mode)
        brls::Application::notify(TR("Rede falhou (modo applet) — ver aba Conta",
            "Network failed (applet mode) — see the Account tab"));
    else if (!http_ok)
        brls::Application::notify(TR("Rede falhou — ver detalhes na aba Conta",
            "Network failed — details in the Account tab"));

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
