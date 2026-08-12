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
#include "cloud.h"
#include "webdav.h"
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
static brls::ListItem* g_pin_item     = nullptr;
static brls::ListItem* g_webdav_item  = nullptr;

// A lista da aba "Tudo de uma vez". Guardada porque os textos dela dizem o
// nome da nuvem, e trocar de nuvem tem que repintar ela na hora — senão fica
// oferecendo "subir pro Google Drive" com o WebDAV escolhido.
static brls::List* g_bulk_list = nullptr;

// Caminho deste .nro, guardado do argv[0] — é o que envSetNextLoad precisa pra
// reabrir o app quando ele troca de idioma.
static std::string g_nro_path;

// Diagnóstico da inicialização de rede, mostrado na aba Ajustes.
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

// O "cancelar" chegando na nuvem.
//
// Sem isto, apertar B só era visto ENTRE um jogo e o outro: o download ou o
// upload do jogo atual ia até o fim, e com save grande isso é dezenas de
// conversa privada removida do historico
// conversa privada removida do historico
//
// Parar no meio é seguro, e o motivo está no cloud.h: o download vai pro
// staging no cartão, e quem escreve no save é o syncjob DEPOIS, só se o
// download inteiro tiver dado certo. Abortado, o cloud_download_tree devolve
// false e ninguém encosta no save. No upload, o pior caso é uma pasta pela
// metade na nuvem — que não é marcada como sincronizada, então a próxima sync
// refaz. É o mesmo que acontece quando a internet cai no meio.
extern "C" bool gui_abort_cb(void)
{
    Job* job = Job::current;
    return job && job->cancelRequested();
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

    // O rodapé diz a nuvem que está valendo, não "Google Drive" na mão: desde
    // que dá pra escolher WebDAV, dizer Drive aqui seria mentira metade das
    // vezes.
    if (g_root)
    {
        CloudKind nuvem = cloud_current();
        std::string sub = std::string(cloud_name(nuvem)) + ": "
            + (cloud_is_ready(nuvem) ? TR("pronto pra sincronizar", "ready to sync")
                                     : TR("falta configurar", "still to set up"));
        g_root->setSubtitle(sub, "v" APP_VERSION_STRING);
    }
}

// O que aparece na linha do servidor WebDAV, na aba Ajustes.
static void updateWebdavItem()
{
    if (!g_webdav_item)
        return;

    WebdavConfig cfg;
    if (webdav_get_config(&cfg))
        g_webdav_item->setValue(cfg.url);
    else
        g_webdav_item->setValue(TR("não configurado", "not set up"), true);
}

// O nome da nuvem que está valendo: "Google Drive", "Servidor WebDAV".
//
// Existe porque as telas e os logs diziam "Drive" escrito na mão, e desde que
// dá pra escolher onde salvar isso vira mentira metade das vezes. Mesmo
// motivo do nuvem() lá no core/syncjob.c.
static const char* nuvem()
{
    return cloud_name(cloud_current());
}

// Teclado de texto do console.
//
// Não uso o brls::Swkbd::openForText porque ele não tem como esconder o que
// está sendo digitado, e um dos campos daqui é senha. Chamo a libnx direto,
// do mesmo jeito que o parental.cpp faz com o teclado numérico.
//
// Devolve false quando ele cancela. Texto vazio conta como "deixa como está" —
// quem chama decide o que fazer com isso.
static bool pedirTexto(const std::string& header, const std::string& sub,
    const std::string& inicial, bool senha, size_t maxLen, std::string& out)
{
    out.clear();

    SwkbdConfig cfg;
    if (R_FAILED(swkbdCreate(&cfg, 0)))
        return false;

    swkbdConfigMakePresetDefault(&cfg);
    swkbdConfigSetType(&cfg, SwkbdType_Normal);
    swkbdConfigSetHeaderText(&cfg, header.c_str());
    swkbdConfigSetSubText(&cfg, sub.c_str());
    swkbdConfigSetStringLenMax(&cfg, (u32)maxLen);
    swkbdConfigSetBlurBackground(&cfg, true);

    if (senha)
        swkbdConfigSetPasswordFlag(&cfg, 1); // bolinha em vez de letra
    else
        swkbdConfigSetInitialText(&cfg, inicial.c_str());

    std::vector<char> buffer(maxLen + 1, '\0');
    Result rc = swkbdShow(&cfg, buffer.data(), buffer.size());
    swkbdClose(&cfg);

    if (R_FAILED(rc))
        return false;

    out = buffer.data();
    return true;
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

    // Todo job repinta a conta ao terminar, deu certo ou não.
    //
    // O motivo: "conectada" nesta tela sempre quis dizer só "existe token.txt
    // com alguma coisa dentro" — ninguém perguntava ao Google se ele ainda
    // aceita. Então um refresh token revogado deixava o app dizendo
    // "conectada" enquanto o overlay dizia "sem token válido", e os dois
    // conversa privada removida do historico
    // e não tinha como decifrar.
    //
    // Agora um refresh recusado apaga o token (ver oauth_refresh_access_token),
    // e é aqui que a tela toma conhecimento — senão a linha continuaria
    // mentindo até alguém entrar na tela da conta por outro motivo.
    page->setOnFinished([onFinished](bool success) {
        updateAccountViews();
        if (onFinished)
            onFinished(success);
    });

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
    job->log(TR("Detalhes na aba Ajustes, embaixo de Diagnóstico.",
                "Details are in the Settings tab, under Diagnostics."),
        true);
    return false;
}

static bool ensureToken(Job* job, char* out, size_t outsz)
{
    if (!ensureNetwork(job))
        return false;

    // Vale pra qualquer nuvem: no Drive isto renova o token do OAuth, no
    // WebDAV monta o cabeçalho com usuário e senha. Quem sabe a diferença é o
    // core/cloud.c.
    CloudKind nuvem   = cloud_current();
    const char* falta = cloud_setup_hint(nuvem);

    if (!cloud_is_ready(nuvem))
    {
        job->setStatus(std::string(TR("Falta configurar ", "Still to set up "))
            + cloud_name(nuvem) + ".");
        if (falta && falta[0])
            job->log(falta, true);
        job->log(TR("A aba Ajustes tem tudo isso, em Onde salvar.",
                    "The Settings tab has all of it, under Where to save."),
            true);
        return false;
    }

    job->setStatus(std::string(TR("Abrindo o acesso a ", "Opening access to ")) + cloud_name(nuvem)
        + "...");
    if (!cloud_begin(out, outsz))
    {
        job->setStatus(std::string(TR("Não consegui abrir o acesso a ",
                           "Couldn't open access to "))
            + cloud_name(nuvem) + ".");
        job->log(TR("Confira os dados de acesso na aba Ajustes, em Onde salvar.",
                    "Check the credentials in the Settings tab, under Where to save."),
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

    char token[CLOUD_AUTH_MAX];
    if (!ensureToken(job, token, sizeof(token)))
        return false;

    const char* nuvem = cloud_name(cloud_current());

    job->setStatus(std::string(TR("Procurando a pasta em ", "Looking for the folder on ")) + nuvem
        + "...");
    char folder[CLOUD_ID_MAX];
    if (!cloud_ensure_app_folder(token, folder, sizeof(folder)))
    {
        job->setStatus(std::string(TR("Não consegui criar/achar a pasta em ",
                           "Couldn't create/find the folder on "))
            + nuvem + ".");
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
               "upload->nuvem->download funcionou.\n");
    fclose(f);

    job->setStatus(TR("Enviando um arquivo de teste...", "Uploading a test file..."));
    if (!cloud_upload(token, folder, TEST_REMOTE, TEST_LOCAL, "text/plain"))
    {
        job->setStatus(TR("O upload falhou.", "The upload failed."));
        return false;
    }
    job->log(TR("upload OK", "upload OK"));

    job->setStatus(TR("Baixando o mesmo arquivo de volta...", "Downloading the same file back..."));
    bool baixou = cloud_download(token, folder, TEST_REMOTE, TEST_DOWN);

    // O arquivo de teste não tem por que continuar morando na nuvem dele. Apaga
    // aqui, tendo dado certo ou não: se o download falhou, o upload de cima
    // funcionou do mesmo jeito e a sobra ficaria lá pra sempre. Cada "testar
    // conexão" deixava uma.
    cloud_delete_file(token, folder, TEST_REMOTE);

    if (!baixou)
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
                job->log(std::string(TR("recebido de volta: ", "got back: ")) + s);
        }
        fclose(f);
    }

    // E os dois do cartão também. Mesma ideia da limpeza lá em cima.
    ::remove(TEST_LOCAL);
    ::remove(TEST_DOWN);

    job->setStatus(std::string(TR("Rede, TLS e ", "Network, TLS and ")) + nuvem
        + TR(" funcionando.", " all work."));
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
    char token[CLOUD_AUTH_MAX];
    return ensureToken(job, token, sizeof(token));
}

// Declarados aqui porque o conflito do jobSync oferece os dois como saída, e
// eles só são definidos mais abaixo.
static bool jobBackup(Job* job, TitleEntry title);
static bool jobRestore(Job* job, TitleEntry title);

// Declarada aqui porque a tela de um jogo oferece "ver o que tem dentro do
// arquivo deste jogo", e ela vem antes no arquivo.
static void openArchiveContents(std::string caminho, std::string titulo);

static bool arquivoExiste(const char* caminho)
{
    struct stat st;
    return stat(caminho, &st) == 0;
}

// O clique principal: "clica pra synca o save com a nuvem e PRONTO".
// Quem decide pra que lado vai é o syncjob_sync_title.
static bool jobSync(Job* job, TitleEntry title)
{
    if (!ensureLogin(job))
        return false;

    job->setStatus(std::string(TR("Comparando o save do console com o de ",
                       "Comparing the console's save with the one on "))
        + nuvem() + "...");

    switch (syncjob_sync_title(&title, jobLogLine))
    {
        case SYNCJOB_SYNC_UPLOADED:
            job->setStatus(std::string(TR("Pronto. O save daqui está guardado em ",
                               "Done. This console's save is stored on "))
                + nuvem() + ".");
            return true;

        case SYNCJOB_SYNC_DOWNLOADED:
            job->setStatus(std::string(TR("Pronto. O save de ", "Done. The save from "))
                + nuvem() + TR(" está no console.", " is on the console."));
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
            // verdade na nuvem. Nada é escrito até ele apertar A.
            job->offerChoice(std::string(TR("Trazer o save de ", "Bring the save from "))
                    + nuvem() + TR(" pro console", " to the console"),
                std::string(TR("Apaga o save que está no console e põe o de ",
                    "Erases the console's save and puts the one from "))
                    + nuvem()
                    + TR(" no lugar. É o que você quer se reinstalou o jogo.",
                        " in its place. This is what you want if you reinstalled the game."),
                [title] {
                    openJob(new Job(std::string(TR("Restaurar — ", "Restore — ")) + titleWithOwner(title),
                                [title](Job* j) { return jobRestore(j, title); }),
                        false);
                });

            job->offerChoice(std::string(TR("Mandar o save do console pro ",
                                 "Send the console's save to "))
                    + nuvem(),
                std::string(TR("Substitui o que está em ", "Replaces what's on "))
                    + nuvem()
                    + TR(" pelo save deste console.", " with this console's save."),
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

    job->setStatus(std::string(TR("Enviando o save pro ", "Uploading the save to ")) + nuvem()
        + "...");
    if (!syncjob_backup_title(&title, jobLogLine))
    {
        job->setStatus(TR("O backup não terminou. Nada foi alterado no console.",
            "The backup didn't finish. Nothing on the console was changed."));
        return false;
    }

    job->setStatus(std::string(TR("Backup concluído. Está em ", "Backup done. It's in "))
        + DRIVE_APP_FOLDER_NAME + "/" + title.name + TR("/ em ", "/ on ") + nuvem() + ".");
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

    job->setStatus(std::string(TR("Trazendo o save de ", "Fetching the save from ")) + nuvem()
        + "...");
    if (!syncjob_restore_title(&title, jobLogLine))
    {
        job->setStatus(TR("Não restaurei nada. Se falhou no meio da gravação, o save do "
                          "console continua como estava (nada é commitado sem dar certo).",
            "Nothing was restored. If it failed mid-write, the console's save is still as "
            "it was (nothing is committed unless it succeeds)."));
        return false;
    }

    job->setStatus(std::string(TR("Save restaurado de ", "Save restored from ")) + nuvem() + ".");
    return true;
}

// ============================ tudo de uma vez ============================

// "faz salvar todos os saves".
//
// A lista chega COPIADA de propósito. O X da tela dos jogos refaz o g_titles na
// thread da interface, e isto aqui roda na thread de trabalho — ler o array
// enquanto o outro lado o reescreve é como se sincroniza o save errado.
static bool jobSyncAll(Job* job, std::vector<TitleEntry> titles)
{
    if (!ensureLogin(job))
        return false;

    size_t subiram = 0, desceram = 0, iguais = 0, vazios = 0;
    std::vector<std::string> conflitos, falharam;
    size_t feitos = 0;

    for (size_t i = 0; i < titles.size(); i++)
    {
        // Só entre um jogo e o outro. Parar no meio de um restore deixaria um
        // save pela metade — o botão diz "cancelar", não "abortar".
        if (job->cancelRequested())
        {
            job->log(TR("Parado a pedido. Os jogos que faltavam ficaram como estavam.",
                "Stopped on request. The remaining games were left as they were."));
            break;
        }

        const TitleEntry& t = titles[i];
        std::string quantos = "(" + std::to_string(i + 1) + "/" + std::to_string(titles.size()) + ") ";
        std::string nome    = titleWithOwner(t);

        job->setStatus(quantos + TR("Sincronizando ", "Syncing ") + nome + "...");
        feitos++;

        switch (syncjob_sync_title(&t, jobLogLine))
        {
            case SYNCJOB_SYNC_UPLOADED:
                subiram++;
                job->log(quantos + nome + TR(" — subiu pro ", " — uploaded to ") + nuvem());
                break;

            case SYNCJOB_SYNC_DOWNLOADED:
                desceram++;
                job->log(quantos + nome + TR(" — veio de ", " — pulled from ") + nuvem());
                break;

            case SYNCJOB_SYNC_EQUAL:
                iguais++;
                job->log(quantos + nome + TR(" — já estava igual", " — already in sync"));
                break;

            case SYNCJOB_SYNC_NOTHING:
                vazios++;
                job->log(quantos + nome + TR(" — sem save nenhum dos dois lados",
                    " — no save on either side"));
                break;

            case SYNCJOB_SYNC_CONFLICT:
                // Não pergunta agora. Perguntar no meio de trinta jogos vira
                // trinta perguntas, e "sincronizar tudo" deixa de ser um clique
                // só. Ficam listados no fim, pra ele resolver um a um no Y.
                conflitos.push_back(nome);
                job->log(quantos + nome + TR(" — os dois lados mudaram, não mexi",
                              " — both sides changed, left alone"),
                    true);
                break;

            case SYNCJOB_SYNC_FAILED:
            default:
                falharam.push_back(nome);
                job->log(quantos + nome + TR(" — falhou", " — failed"), true);
                break;
        }
    }

    std::string resumo = std::to_string(feitos) + TR(" jogos: ", " games: ")
        + std::to_string(subiram) + TR(" subiram, ", " uploaded, ")
        + std::to_string(desceram) + TR(" desceram, ", " downloaded, ")
        + std::to_string(iguais) + TR(" já estavam iguais, ", " already in sync, ")
        + std::to_string(vazios) + TR(" sem save.", " with no save.");

    if (!conflitos.empty())
    {
        resumo += TR("\n\nEstes têm save dos dois lados, diferentes, e eu não escolhi por "
                     "você — abra cada um e aperte Y:\n",
            "\n\nThese have different saves on both sides and I didn't choose for you — "
            "open each one and press Y:\n");
        for (const std::string& nome : conflitos)
            resumo += "  • " + nome + "\n";
    }

    if (!falharam.empty())
    {
        resumo += TR("\n\nNão deu certo em:\n", "\n\nDidn't work for:\n");
        for (const std::string& nome : falharam)
            resumo += "  • " + nome + "\n";
    }

    job->setStatus(resumo);

    // Conflito não é erro: é o app fazendo o certo e devolvendo a escolha pra
    // ele. Só falha de verdade pinta a tela de vermelho.
    return falharam.empty();
}

// ---------------------------------------------------------------------------
// Tudo num arquivo só
// ---------------------------------------------------------------------------

// O syncjob pergunta por um ponteiro de função sem userdata, então o jeito de
// chegar no job em andamento é a global — igual ao jobLogLine.
static bool jobStopAsked()
{
    return Job::current && Job::current->cancelRequested();
}

static bool jobArchiveAll(Job* job, std::vector<TitleEntry> titles, bool subir)
{
    // O login primeiro: juntar 8 GB de save pra depois descobrir que não tem
    // conta conectada é desperdício de tempo dele.
    if (subir && !ensureLogin(job))
        return false;

    job->setStatus(TR("Juntando os saves num arquivo só...",
        "Packing the saves into a single file..."));

    size_t quantos = syncjob_archive_titles(titles.data(), titles.size(), jobLogLine, jobStopAsked);
    if (quantos == 0)
    {
        job->setStatus(TR("Nada foi guardado. Se já existia um arquivo, ele continua "
                          "inteiro do jeito que estava.",
            "Nothing was stored. If a file already existed, it's still intact as it was."));
        return false;
    }

    if (!subir)
    {
        job->setStatus(std::to_string(quantos)
            + TR(" saves num arquivo só, em switch/SwitchSaveSync/" SYNC_ARCHIVE_NAME ".",
                " saves in a single file, at switch/SwitchSaveSync/" SYNC_ARCHIVE_NAME "."));
        return true;
    }

    job->setStatus(std::string(TR("Subindo o arquivo pro ", "Uploading the file to ")) + nuvem()
        + "...");
    if (!syncjob_archive_upload(jobLogLine))
    {
        job->setStatus(std::string(TR("O arquivo ficou pronto no cartão, mas não subiu pro ",
                           "The file is ready on the SD card, but it didn't upload to "))
            + nuvem() + ".");
        return false;
    }

    job->setStatus(std::to_string(quantos)
        + TR(" saves num arquivo só, no cartão e em ",
            " saves in a single file, on the SD card and on ")
        + nuvem() + ".");
    return true;
}

static bool jobArchiveUpload(Job* job)
{
    if (!ensureLogin(job))
        return false;

    job->setStatus(std::string(TR("Subindo o arquivo pro ", "Uploading the file to ")) + nuvem()
        + "...");
    if (!syncjob_archive_upload(jobLogLine))
    {
        job->setStatus(TR("Não consegui subir o arquivo.", "Couldn't upload the file."));
        return false;
    }

    job->setStatus(std::string(TR("O arquivo está em ", "The file is on ")) + nuvem() + ".");
    return true;
}

static bool jobArchiveDownload(Job* job)
{
    if (!ensureLogin(job))
        return false;

    job->setStatus(std::string(TR("Baixando o arquivo de ", "Downloading the file from ")) + nuvem()
        + "...");
    if (!syncjob_archive_download(jobLogLine))
    {
        job->setStatus(TR("Não consegui baixar o arquivo. Nada no console foi alterado.",
            "Couldn't download the file. Nothing on the console was changed."));
        return false;
    }

    job->setStatus(TR("Arquivo no cartão. Ele ainda NÃO foi escrito em save nenhum — pra "
                      "isso, abra o jogo na lista e escolha \"Do arquivo com tudo\".",
        "File is on the SD card. It has NOT been written to any save yet — for that, open "
        "the game in the list and pick \"From the file with everything\"."));
    return true;
}

static bool jobArchiveRestore(Job* job, TitleEntry title)
{
    job->setStatus(TR("Tirando o save de dentro do arquivo...",
        "Pulling the save out of the file..."));

    if (!syncjob_archive_restore_title(&title, jobLogLine))
    {
        job->setStatus(TR("Não restaurei nada. Se falhou no meio da gravação, o save do "
                          "console continua como estava.",
            "Nothing was restored. If it failed mid-write, the console's save is still as "
            "it was."));
        return false;
    }

    job->setStatus(TR("Save restaurado de dentro do arquivo com tudo.",
        "Save restored from inside the file with everything."));
    return true;
}

// conversa privada removida do historico
//. Um arquivo do JOGO, separado do arquivo com tudo — juntar as
// contas de um jogo não pode passar por cima do backup geral.
static bool jobGameArchive(Job* job, std::vector<TitleEntry> saves, bool subir)
{
    if (saves.empty())
        return false;

    if (subir && !ensureLogin(job))
        return false;

    char caminho[0x300];
    syncjob_game_archive_path(&saves[0], caminho, sizeof(caminho));

    job->setStatus(TR("Juntando as contas num arquivo só...",
        "Packing the accounts into a single file..."));

    size_t quantos = syncjob_archive_titles_to(caminho, saves.data(), saves.size(),
        jobLogLine, jobStopAsked);
    if (quantos == 0)
    {
        job->setStatus(TR("Nada foi guardado. Se já existia um arquivo deste jogo, ele "
                          "continua inteiro do jeito que estava.",
            "Nothing was stored. If a file for this game already existed, it's still "
            "intact as it was."));
        return false;
    }

    const char* soONome = strrchr(caminho, '/');
    std::string nome    = soONome ? soONome + 1 : caminho;

    if (!subir)
    {
        job->setStatus(std::to_string(quantos)
            + TR(" contas em switch/SwitchSaveSync/", " accounts in switch/SwitchSaveSync/") + nome);
        return true;
    }

    job->setStatus(std::string(TR("Subindo o arquivo pro ", "Uploading the file to ")) + nuvem()
        + "...");
    if (!syncjob_game_archive_upload(&saves[0], caminho, jobLogLine))
    {
        job->setStatus(std::string(TR("O arquivo ficou pronto no cartão, mas não subiu pro ",
                           "The file is ready on the SD card, but it didn't upload to "))
            + nuvem() + ".");
        return false;
    }

    job->setStatus(std::to_string(quantos)
        + TR(" contas em ", " accounts in ") + nome
        + TR(", no cartão e em ", ", on the SD card and on ") + nuvem() + ".");
    return true;
}

// Restaura uma pasta do arquivo por cima do save de `dest` — que pode ser de
// outra conta que não a dona original.
static bool jobArchiveRestoreFolder(Job* job, std::string caminho, std::string folder,
    TitleEntry dest)
{
    job->setStatus(TR("Tirando o save de dentro do arquivo...",
        "Pulling the save out of the file..."));

    if (!syncjob_archive_restore_folder(caminho.c_str(), folder.c_str(), &dest, jobLogLine))
    {
        job->setStatus(TR("Não restaurei nada. Se falhou no meio da gravação, o save do "
                          "console continua como estava.",
            "Nothing was restored. If it failed mid-write, the console's save is still as "
            "it was."));
        return false;
    }

    job->setStatus(TR("Save restaurado de dentro do arquivo.",
        "Save restored from inside the file."));
    return true;
}

// Conta recém-criada não tem save de jogo nenhum, então antes de restaurar é
// preciso criar a save data — vazia, do tamanho que o próprio jogo pede.
static bool jobRestoreIntoNewAccount(Job* job, std::string caminho, std::string folder,
    TitleEntry molde, AccountUid conta, std::string apelido)
{
    TitleEntry dest = molde;
    dest.uid        = conta;
    dest.device_save = false;
    snprintf(dest.account, sizeof(dest.account), "%s", apelido.c_str());

    if (!savemount_save_exists(dest.application_id, conta))
    {
        job->setStatus(TR("Criando o save deste jogo na conta nova...",
            "Creating this game's save on the new account..."));

        u64 tamanho = 0, diario = 0;
        if (!titles_save_sizes(dest.application_id, false, &tamanho, &diario))
        {
            job->setStatus(TR("Não consegui descobrir de que tamanho é o save deste jogo. "
                              "Abra o jogo uma vez com a conta nova — isso cria o save — e "
                              "volte aqui.",
                "Couldn't work out how big this game's save should be. Open the game once "
                "with the new account — that creates the save — and come back."));
            return false;
        }

        job->log(TR("Tamanho pedido pelo jogo: ", "Size the game asks for: ")
            + std::to_string(tamanho / 1024) + " KB");

        if (!savemount_create_account_save(dest.application_id, conta, tamanho, diario))
        {
            job->setStatus(TR("Não consegui criar o save na conta nova. Abra o jogo uma vez "
                              "com ela e volte aqui.",
                "Couldn't create the save on the new account. Open the game once with it "
                "and come back."));
            return false;
        }

        job->log(TR("Save criado, vazio.", "Save created, empty."));
    }

    return jobArchiveRestoreFolder(job, caminho, folder, dest);
}

// ============================ telas ============================

// Tela de um jogo: o que fazer quando o clique simples não serve.
//
// Ela NÃO é mais o caminho normal — o caminho normal é apertar A na lista e o
// app resolver sozinho ("clica pra synca o save com a nuvem e PRONTO"). Isso
// aqui é o Y: mandar pra que lado quando os dois lados mudaram, e as cópias no
// próprio cartão, que não dependem de internet nem de conta.
// "Todas as contas num arquivo só" — a mesma ação em dois lugares: na tela de
// escolher conta (onde ele vê que o jogo tem mais de uma) e na tela do jogo.
static void perguntarArquivoDoJogo(std::vector<TitleEntry> saves)
{
    brls::Dialog* dialog = new brls::Dialog(
        TR(std::string("Vou juntar os ") + std::to_string(saves.size())
                + " saves deste jogo num arquivo só.\n\nGuardar onde?",
            std::string("I'll pack this game's ") + std::to_string(saves.size())
                + " saves into a single file.\n\nStore it where?"));

    dialog->addButton(TR("Cancelar", "Cancel"), [dialog](brls::View* view) { dialog->close(); });
    dialog->addButton(TR("Só no cartão", "SD card only"), [dialog, saves](brls::View* view) {
        dialog->close([saves]() {
            openJob(new Job(std::string(saves[0].name) + TR(" — todas as contas", " — every account"),
                        [saves](Job* job) { return jobGameArchive(job, saves, false); }),
                true);
        });
    });
    dialog->addButton(std::string(TR("Cartão e ", "SD card and ")) + nuvem(),
        [dialog, saves](brls::View* view) {
        dialog->close([saves]() {
            openJob(new Job(std::string(saves[0].name) + TR(" — todas as contas", " — every account"),
                        [saves](Job* job) { return jobGameArchive(job, saves, true); }),
                true);
        });
    });

    dialog->setCancelable(true);
    dialog->open();
}

static brls::ListItem* itemTodasAsContas(const std::vector<TitleEntry>& saves)
{
    brls::ListItem* item = new brls::ListItem(
        TR("Todas as contas num arquivo só", "Every account in one file"),
        TR("Junta o save de todas as contas deste jogo num arquivo com o nome do jogo, "
           "separado do arquivo que tem tudo.",
            "Packs every account's save for this game into a file named after the game, "
            "separate from the file that holds everything."));
    item->setValue(std::to_string(saves.size()) + TR(" saves", " saves"));
    item->getClickEvent()->subscribe(
        [saves](brls::View* view) { perguntarArquivoDoJogo(saves); });
    return item;
}

static void openGamePage(const TitleEntry& title)
{
    brls::AppletFrame* frame = new brls::AppletFrame(true, true);
    frame->setTitle(titleWithOwner(title));

    size_t iconLen = 0;
    if (titles_get_icon(title.application_id, g_icon_buffer, sizeof(g_icon_buffer), &iconLen))
        frame->setIcon(g_icon_buffer, iconLen);

    brls::List* list = new brls::List();

    std::vector<TitleEntry> saves = savesOf(title.application_id);

    brls::ListItem* backupItem = new brls::ListItem(
        std::string(TR("Em ", "On ")) + nuvem(),
        std::string(TR("Lê o save que está no console e sobe pro ",
            "Reads the console's save and uploads it to "))
            + nuvem() + TR(". Não altera nada no console.", ". Nothing on the console is changed."));
    backupItem->getClickEvent()->subscribe([title](brls::View* view) {
        openJob(new Job(std::string(TR("Backup — ", "Backup — ")) + titleWithOwner(title),
                    [title](Job* job) { return jobBackup(job, title); }),
            false);
    });

    brls::ListItem* restoreItem = new brls::ListItem(
        std::string(TR("De ", "From ")) + nuvem(),
        std::string(TR("Substitui o save que está no console pelo que está em ",
            "Replaces the console's save with the one on "))
            + nuvem() + ".");
    restoreItem->getClickEvent()->subscribe([title](brls::View* view) {
        brls::Dialog* dialog = new brls::Dialog(
            TR(std::string("Isso apaga o save de \"") + titleWithOwner(title)
                    + "\" que está no console e põe no lugar o que está em " + nuvem()
                    + ". O save atual se perde.\n\nContinuar?",
                std::string("This erases the \"") + titleWithOwner(title)
                    + "\" save on the console and puts the one from " + nuvem()
                    + " in its place. The current save is lost.\n\nContinue?"));

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
        TR("No cartão SD", "On the SD card"),
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
        TR("Do cartão SD", "From the SD card"),
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

    // Só aparece quando existe arquivo com tudo no cartão. Oferecer "restaurar de
    // um arquivo que não existe" é oferecer um erro.
    brls::ListItem* restoreArchiveItem = nullptr;
    if (syncjob_has_archive())
    {
        restoreArchiveItem = new brls::ListItem(
            TR("Do arquivo com tudo", "From the file with everything"),
            TR("Tira o save deste jogo de dentro do " SYNC_ARCHIVE_NAME " e escreve por "
               "cima do save do console.",
                "Pulls this game's save out of " SYNC_ARCHIVE_NAME " and writes it over "
                "the console's save."));
        restoreArchiveItem->getClickEvent()->subscribe([title](brls::View* view) {
            brls::Dialog* dialog = new brls::Dialog(
                TR(std::string("Isso apaga o save de \"") + titleWithOwner(title)
                        + "\" que está no console e põe no lugar o que está dentro do "
                          "arquivo com tudo. O save atual se perde.\n\nContinuar?",
                    std::string("This erases the \"") + titleWithOwner(title)
                        + "\" save on the console and puts what's inside the single file "
                          "in its place. The current save is lost.\n\nContinue?"));

            dialog->addButton(TR("Cancelar", "Cancel"), [dialog](brls::View* view) { dialog->close(); });
            dialog->addButton(TR("Restaurar", "Restore"), [dialog, title](brls::View* view) {
                dialog->close([title]() {
                    openJob(new Job(std::string(TR("Restaurar do arquivo — ", "Restore from file — "))
                                + titleWithOwner(title),
                                [title](Job* job) { return jobArchiveRestore(job, title); }),
                        false);
                });
            });

            dialog->setCancelable(true);
            dialog->open();
        });
    }

    // O arquivo com TODAS as contas deste jogo, quando ele existe. É o par do
    // "salvar tudo num arquivo só" da tela de escolher conta: sem isto, o
    // arquivo era feito e não tinha caminho de volta.
    brls::ListItem* restoreGameArchiveItem = nullptr;
    char arquivoDoJogo[0x300];
    syncjob_game_archive_path(&title, arquivoDoJogo, sizeof(arquivoDoJogo));
    if (arquivoExiste(arquivoDoJogo))
    {
        std::string caminho = arquivoDoJogo;

        restoreGameArchiveItem = new brls::ListItem(
            TR("Do arquivo deste jogo", "From this game's file"),
            TR("Abre o arquivo com todas as contas deste jogo e deixa você escolher qual "
               "save volta — inclusive pra uma conta diferente da original.",
                "Opens the file holding every account of this game and lets you pick which "
                "save comes back — including into a different account than the original."));
        restoreGameArchiveItem->getClickEvent()->subscribe([caminho, title](brls::View* view) {
            openArchiveContents(caminho, std::string(title.name));
        });
    }

    // A separação é por SENTIDO, não por lugar: tudo que só lê em cima, tudo que
    // escreve por cima do save embaixo do aviso.
    //
    // E os dois lados na MESMA ordem — Drive, cartão, arquivo. Estavam trocados
    // (guardar começava no Drive, trazer de volta começava no cartão), o que
    // obriga a ler linha por linha em vez de bater o olho.
    list->addView(new brls::Header(TR("Guardar uma cópia", "Store a copy")));
    list->addView(backupItem);
    list->addView(backupLocalItem);
    if (saves.size() > 1)
        list->addView(itemTodasAsContas(saves));

    list->addView(new brls::Header(TR("Trazer de volta", "Bring it back")));
    list->addView(new brls::Label(brls::LabelStyle::DESCRIPTION,
        TR("Daqui pra baixo, tudo ESCREVE por cima do save que está no console — o que "
           "está lá agora se perde. Cada um ainda pergunta antes.",
            "From here down, everything WRITES over the save on the console — what's there "
            "now is lost. Each one still asks first."),
        true));
    list->addView(restoreItem);
    list->addView(restoreLocalItem);
    if (restoreGameArchiveItem)
        list->addView(restoreGameArchiveItem);
    if (restoreArchiveItem)
        list->addView(restoreArchiveItem);

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

    // conversa privada removida do historico
    // conversa privada removida do historico
    // de 4 contas já nasce fora do campo de visão.
    list->addView(new brls::Header(TR("Todas as contas de uma vez", "Every account at once")));
    list->addView(itemTodasAsContas(saves));

    list->addView(new brls::Header(TR("Ou escolha uma conta", "Or pick one account")));

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
        TR("Cada save destes é separado: tem a sua pasta na nuvem e no cartão, e "
           "sincronizar um não encosta no outro.",
            "Each of these saves is separate: it has its own folder in the cloud and on the "
            "SD card, and syncing one doesn't touch the other."),
        true));

    frame->setContentView(list);
    brls::Application::pushView(frame);
}

static std::string humanSize(u64 bytes)
{
    char texto[32];
    if (bytes >= 1024ull * 1024 * 1024)
        snprintf(texto, sizeof(texto), "%.1f GB", (double)bytes / (1024.0 * 1024 * 1024));
    else if (bytes >= 1024 * 1024)
        snprintf(texto, sizeof(texto), "%.1f MB", (double)bytes / (1024.0 * 1024));
    else if (bytes >= 1024)
        snprintf(texto, sizeof(texto), "%.0f KB", (double)bytes / 1024.0);
    else
        snprintf(texto, sizeof(texto), "%llu B", (unsigned long long)bytes);
    return texto;
}

// O "o que tem dentro" do arquivo único: uma linha por jogo guardado. Roda na
// thread da interface porque só lê o índice, que é pequeno e está no fim do
// arquivo — não desempacota nada.
struct ArchiveListing
{
    std::vector<std::pair<std::string, u64>>* linhas;
    size_t jogos;
    u64 bytes;
};

static void archiveListingRow(const char* pasta, u64 bytes, void* userdata)
{
    ArchiveListing* ctx = (ArchiveListing*)userdata;
    ctx->jogos++;
    ctx->bytes += bytes;
    ctx->linhas->push_back({ pasta, bytes });
}

// O caminho contrário do save_folder_name: dada uma pasta que veio de dentro
// do arquivo, de qual save DESTE console ela é?
static bool saveDaPasta(const std::string& pasta, TitleEntry* out)
{
    for (size_t i = 0; i < g_titles_count; i++)
    {
        char nome[0x201];
        syncjob_save_folder_name(&g_titles[i], nome, sizeof(nome));
        if (pasta == nome)
        {
            *out = g_titles[i];
            return true;
        }
    }
    return false;
}

// E quando a conta não bate: pelo menos o JOGO é deste console?
//
// A pasta é o nome do jogo, com " (conta)" no fim quando o jogo tem mais de um
// save. Então o jogo é o que sobra tirando o sufixo — por isso a comparação é
// por começo, e exigindo espaço logo depois: sem isso "Mario" casaria com
// "Mario Kart".
static bool jogoDaPasta(const std::string& pasta, u64* out_app)
{
    for (size_t i = 0; i < g_titles_count; i++)
    {
        char limpo[0x201];
        syncstate_sanitize_name(g_titles[i].name, limpo, sizeof(limpo));
        std::string base = limpo;

        if (pasta == base
            || (pasta.size() > base.size() && pasta.compare(0, base.size(), base) == 0
                && pasta[base.size()] == ' '))
        {
            *out_app = g_titles[i].application_id;
            return true;
        }
    }
    return false;
}

// As contas do console, na ordem em que o sistema devolve.
static std::vector<AccountUid> contasDoConsole()
{
    std::vector<AccountUid> out;

    bool nosso = R_SUCCEEDED(accountInitialize(AccountServiceType_Application));

    AccountUid uids[ACC_USER_LIST_SIZE];
    s32 quantas = 0;
    if (R_SUCCEEDED(accountListAllUsers(uids, ACC_USER_LIST_SIZE, &quantas)))
        for (s32 i = 0; i < quantas; i++)
            out.push_back(uids[i]);

    if (nosso)
        accountExit();

    return out;
}

static std::string apelidoDaConta(AccountUid uid)
{
    std::string apelido;

    bool nosso = R_SUCCEEDED(accountInitialize(AccountServiceType_Application));

    AccountProfile perfil;
    if (R_SUCCEEDED(accountGetProfile(&perfil, uid)))
    {
        AccountProfileBase base;
        AccountUserData dados;
        if (R_SUCCEEDED(accountProfileGet(&perfil, &dados, &base)))
        {
            char nome[sizeof(base.nickname) + 1] = { 0 };
            memcpy(nome, base.nickname, sizeof(base.nickname));
            apelido = nome;
        }
        accountProfileClose(&perfil);
    }

    if (nosso)
        accountExit();

    return apelido;
}

static bool mesmaConta(const AccountUid& a, const AccountUid& b)
{
    return a.uid[0] == b.uid[0] && a.uid[1] == b.uid[1];
}

// O nome da conta que estava escrito na pasta: "Jogo (Player 1)" -> "Miguel".
//
// Só existe quando o jogo tinha mais de um save na hora em que o arquivo foi
// feito — é o mesmo sufixo que o save_folder_name põe. Serve pra dizer a ele
// QUAL conta ele está procurando, na hora de criar uma.
static std::string contaDaPasta(const std::string& pasta)
{
    if (pasta.empty() || pasta.back() != ')')
        return "";

    size_t abre = pasta.rfind(" (");
    if (abre == std::string::npos)
        return "";

    return pasta.substr(abre + 2, pasta.size() - abre - 3);
}

// Abre a tela de criar usuário do próprio console e, na volta, põe o save na
// conta que nasceu.
//
// A libnx não tem função de criar usuário — o que ela tem é pselShowUserCreator,
// que abre a MESMA tela de Configurações > Usuários. É o caminho certo de
// qualquer jeito: a conta nasce de verdade, com foto e nome, em vez de a gente
// escrever no banco de perfis do sistema na mão.
static void criarContaERestaurar(std::string caminho, std::string pasta, TitleEntry molde)
{
    // Applet mode não abre tela de sistema. Se tentar, a chamada falha e o app
    // fica esperando uma resposta que não vem.
    if (g_applet_mode)
    {
        brls::Dialog* aviso = new brls::Dialog(
            TR("Pra criar conta, o app precisa estar aberto como aplicação — segurando R num "
               "jogo instalado, e não pelo Álbum.\n\n"
               "Ou crie a conta pelo menu do console (Configurações > Usuários > Adicionar "
               "usuário) e volte aqui.",
                "To create an account the app has to be running as an application — hold R on "
                "an installed game instead of opening it from the Album.\n\n"
                "Or create the account from the console menu (System Settings > Users > Add "
                "user) and come back."));
        aviso->addButton(TR("Entendi", "Got it"), [aviso](brls::View* view) { aviso->close(); });
        aviso->setCancelable(true);
        aviso->open();
        return;
    }

    // Quem é a conta nova? A que não estava aqui antes. O pselShowUserCreator
    // não devolve o uid do que foi criado, então a resposta é a diferença entre
    // as duas listas.
    std::vector<AccountUid> antes = contasDoConsole();

    if (R_FAILED(pselShowUserCreator()))
    {
        brls::Application::notify(
            TR("Não consegui abrir a tela do console", "Couldn't open the console's screen"));
        return;
    }

    AccountUid nova = { { 0, 0 } };
    for (const AccountUid& uid : contasDoConsole())
    {
        bool jaExistia = false;
        for (const AccountUid& velha : antes)
            if (mesmaConta(uid, velha))
            {
                jaExistia = true;
                break;
            }

        if (!jaExistia)
        {
            nova = uid;
            break;
        }
    }

    if (!accountUidIsValid(&nova))
    {
        brls::Application::notify(
            TR("Nenhuma conta nova foi criada", "No new account was created"));
        return;
    }

    std::string apelido = apelidoDaConta(nova);

    openJob(new Job(std::string(TR("Restaurar — ", "Restore — ")) + molde.name,
                [caminho, pasta, molde, nova, apelido](Job* job) {
                    return jobRestoreIntoNewAccount(job, caminho, pasta, molde, nova, apelido);
                }),
        false);
}

// "colocar em outra conta": a terceira opção da pergunta.
//
// Lista TODAS as contas do console, não só as que já têm save deste jogo. A
// conta que nunca abriu o jogo é justamente um destino bom — e criar o save que
// falta é a mesma coisa que o trabalho já faz pra conta recém-criada.
static void openOtherAccountPage(std::string caminho, std::string pasta, TitleEntry molde)
{
    brls::AppletFrame* frame = new brls::AppletFrame(true, true);
    frame->setTitle(TR("Colocar em outra conta", "Put it on another account"));

    brls::List* list = new brls::List();

    std::vector<AccountUid> contas = contasDoConsole();

    if (contas.empty())
    {
        list->addView(new brls::Label(brls::LabelStyle::DESCRIPTION,
            TR("Este console não tem nenhuma conta de usuário.",
                "This console has no user accounts."),
            true));
    }
    else
    {
        list->addView(new brls::Label(brls::LabelStyle::DESCRIPTION,
            TR(std::string("O save guardado vai virar save da conta que você escolher, em \"")
                    + molde.name + "\". Pergunto de novo antes.",
                std::string("The stored save becomes the chosen account's save, in \"")
                    + molde.name + "\". I'll ask again first."),
            true));

        for (const AccountUid& uid : contas)
        {
            std::string apelido = apelidoDaConta(uid);
            if (apelido.empty())
                apelido = TR("conta sem nome", "unnamed account");

            // Conta que já tem save deste jogo é conta que vai PERDER o save
            // dela. Isso tem que estar na linha, não só no aviso de cima.
            bool temSave = savemount_save_exists(molde.application_id, uid);

            brls::ListItem* item = new brls::ListItem(apelido,
                temSave ? TR("Já tem save deste jogo — ele será apagado e substituído.",
                              "Already has a save for this game — it'll be erased and replaced.")
                        : TR("Ainda não tem save deste jogo. Eu crio o save e ponho o guardado "
                             "dentro.",
                              "Doesn't have a save for this game yet. I'll create it and put "
                              "the stored one inside."));

            item->getClickEvent()->subscribe(
                [caminho, pasta, molde, uid, apelido, temSave](brls::View* view) {
                    brls::Dialog* dialog = new brls::Dialog(
                        temSave
                            ? TR(std::string("Isso apaga o save de \"") + molde.name
                                        + "\" da conta \"" + apelido
                                        + "\" e põe no lugar o que está guardado no arquivo.\n\n"
                                          "O jogo não pode estar aberto. Continuar?",
                                  std::string("This erases the \"") + molde.name
                                        + "\" save of account \"" + apelido
                                        + "\" and puts what's stored in the file in its place."
                                          "\n\nThe game must not be running. Continue?")
                            : TR(std::string("Vou criar o save de \"") + molde.name
                                        + "\" na conta \"" + apelido
                                        + "\" e pôr dentro o que está guardado no arquivo.\n\n"
                                          "O jogo não pode estar aberto. Continuar?",
                                  std::string("I'll create the \"") + molde.name
                                        + "\" save on account \"" + apelido
                                        + "\" and put what's stored in the file inside.\n\n"
                                          "The game must not be running. Continue?"));

                    dialog->addButton(TR("Cancelar", "Cancel"),
                        [dialog](brls::View* view) { dialog->close(); });
                    dialog->addButton(temSave ? TR("Restaurar", "Restore") : TR("Criar", "Create"),
                        [dialog, caminho, pasta, molde, uid, apelido](brls::View* view) {
                            dialog->close([caminho, pasta, molde, uid, apelido]() {
                                openJob(new Job(std::string(TR("Restaurar — ", "Restore — "))
                                            + molde.name,
                                            [caminho, pasta, molde, uid, apelido](Job* job) {
                                                return jobRestoreIntoNewAccount(
                                                    job, caminho, pasta, molde, uid, apelido);
                                            }),
                                    false);
                            });
                        });

                    dialog->setCancelable(true);
                    dialog->open();
                });

            list->addView(item);
        }
    }

    frame->setContentView(list);
    brls::Application::pushView(frame);
}

// conversa privada removida do historico
// conversa privada removida do historico
//
// Era uma tela separada, com o mesmo conteúdo. Virou pergunta na hora porque é
// nessa hora que ele quer decidir — e as três respostas cabem em três botões.
static void perguntarContaQueFalta(std::string caminho, std::string pasta, u64 appId)
{
    std::vector<TitleEntry> locais = savesOf(appId);

    // Molde do jogo: serve só pro nome e pro application_id. A conta e o tipo de
    // save quem preenche é o trabalho.
    TitleEntry molde = locais[0];
    for (const TitleEntry& t : locais)
        if (!t.device_save)
        {
            molde = t;
            break;
        }

    std::string queria = contaDaPasta(pasta);
    std::string aConta = queria.empty() ? TR("dona deste save", "that owns this save")
                                        : std::string("\"") + queria + "\"";

    brls::Dialog* dialog = new brls::Dialog(
        TR(std::string("O jogo este console conhece: \"") + molde.name
                + "\". Quem não existe aqui é a conta " + aConta
                + ", e save do Switch é preso a uma conta.\n\nDeseja criar essa conta?"
                + (queria.empty()
                          ? std::string("")
                          : std::string(" Na tela do console, ponha o nome \"") + queria
                                + "\" — é o nome que estava no save."),
            std::string("This console knows the game: \"") + molde.name
                + "\". What it doesn't have is the account " + aConta
                + ", and a Switch save is tied to an account.\n\nDo you want to create that "
                  "account?"
                + (queria.empty() ? std::string("")
                                  : std::string(" On the console's screen, use the name \"")
                                        + queria + "\" — that's the name the save had.")));

    dialog->addButton(TR("Sim", "Yes"), [dialog, caminho, pasta, molde](brls::View* view) {
        dialog->close([caminho, pasta, molde]() { criarContaERestaurar(caminho, pasta, molde); });
    });
    dialog->addButton(TR("Não", "No"), [dialog](brls::View* view) { dialog->close(); });
    dialog->addButton(TR("Colocar em outra conta", "Put it on another account"),
        [dialog, caminho, pasta, molde](brls::View* view) {
            dialog->close([caminho, pasta, molde]() {
                openOtherAccountPage(caminho, pasta, molde);
            });
        });

    dialog->setCancelable(true);
    dialog->open();
}

// Clicou num save de dentro do arquivo. De quem ele é neste console?
static void openArchiveEntry(std::string caminho, std::string pasta)
{
    // Caso normal: a conta que gravou é uma conta daqui. Só confirmar.
    TitleEntry dest;
    if (saveDaPasta(pasta, &dest))
    {
        brls::Dialog* dialog = new brls::Dialog(
            TR(std::string("Isso apaga o save de \"") + titleWithOwner(dest)
                    + "\" que está no console e põe no lugar o que está guardado no arquivo.\n\n"
                      "O jogo não pode estar aberto. Continuar?",
                std::string("This erases the \"") + titleWithOwner(dest)
                    + "\" save on the console and puts what's stored in the file in its "
                      "place.\n\nThe game must not be running. Continue?"));

        dialog->addButton(TR("Cancelar", "Cancel"), [dialog](brls::View* view) { dialog->close(); });
        dialog->addButton(TR("Restaurar", "Restore"), [dialog, caminho, pasta, dest](brls::View* view) {
            dialog->close([caminho, pasta, dest]() {
                openJob(new Job(std::string(TR("Restaurar — ", "Restore — ")) + titleWithOwner(dest),
                            [caminho, pasta, dest](Job* job) {
                                return jobArchiveRestoreFolder(job, caminho, pasta, dest);
                            }),
                    false);
            });
        });

        dialog->setCancelable(true);
        dialog->open();
        return;
    }

    // O jogo está aqui, a conta não. Tem conversa: dá pra pôr noutra conta ou
    // criar a que falta.
    u64 appId = 0;
    if (jogoDaPasta(pasta, &appId))
    {
        // Menos um caso: save do CONSOLE, não de conta. Não tem conta pra
        // escolher nem pra criar — o que falta é o save do console existir, e
        // quem cria ele é o jogo abrindo uma vez.
        if (contaDaPasta(pasta) == "console")
        {
            brls::Dialog* aviso = new brls::Dialog(
                TR("Este save é do console inteiro, não de uma conta — é o caso do Animal "
                   "Crossing, por exemplo.\n\nO console ainda não tem esse save. Abra o jogo "
                   "uma vez (pode fechar em seguida) pra ele criar o save, e volte aqui.",
                    "This save belongs to the whole console, not to an account — Animal "
                    "Crossing is one of these.\n\nThis console doesn't have that save yet. "
                    "Open the game once (you can close it right after) so it creates the "
                    "save, then come back."));

            aviso->addButton(TR("Entendi", "Got it"), [aviso](brls::View* view) { aviso->close(); });
            aviso->setCancelable(true);
            aviso->open();
            return;
        }

        perguntarContaQueFalta(caminho, pasta, appId);
        return;
    }

    brls::Dialog* aviso = new brls::Dialog(
        TR(std::string("\"") + pasta
                + "\" não bate com nada deste console.\n\n"
                  "Ou o jogo não está instalado aqui, ou ele nunca foi aberto — save só "
                  "existe depois que o jogo roda uma vez. Instale/abra o jogo e volte: aí "
                  "esta linha passa a ter pra onde ir.",
            std::string("\"") + pasta
                + "\" doesn't match anything on this console.\n\n"
                  "Either the game isn't installed here, or it has never been opened — a save "
                  "only exists after the game runs once. Install/open the game and come back: "
                  "then this row will have somewhere to go."));

    aviso->addButton(TR("Entendi", "Got it"), [aviso](brls::View* view) { aviso->close(); });
    aviso->setCancelable(true);
    aviso->open();
}

// O "o que tem dentro" de um arquivo: uma linha por save guardado, e cada linha
// restaura. Roda na thread da interface porque só lê o índice, que é pequeno e
// está no fim do arquivo — não desempacota nada.
static void openArchiveContents(std::string caminho, std::string titulo)
{
    brls::AppletFrame* frame = new brls::AppletFrame(true, true);
    frame->setTitle(titulo);

    brls::List* list = new brls::List();

    std::vector<std::pair<std::string, u64>> linhas;
    ArchiveListing ctx = { &linhas, 0, 0 };

    if (!syncjob_archive_list_path(caminho.c_str(), archiveListingRow, &ctx) || ctx.jogos == 0)
    {
        list->addView(new brls::Label(brls::LabelStyle::DESCRIPTION,
            TR("Não consegui ler o arquivo. Ou ele não existe, ou está corrompido — em "
               "qualquer um dos casos, dá pra fazer outro.",
                "Couldn't read the file. Either it doesn't exist or it's corrupted — "
                "either way, you can make another one."),
            true));
    }
    else
    {
        frame->setFooterText(std::to_string(ctx.jogos) + TR(" saves, ", " saves, ")
            + humanSize(ctx.bytes) + TR(" sem compressão", " uncompressed"));

        list->addView(new brls::Label(brls::LabelStyle::DESCRIPTION,
            TR("Aperte A num save pra pôr ele de volta no console. Isso ESCREVE por cima do "
               "save que está lá — pergunto antes, em cada um.\n\n"
               "Save de conta que não existe mais aqui também tem saída: eu ofereço outra "
               "conta, ou criar a conta que falta.",
                "Press A on a save to put it back on the console. That OVERWRITES the save "
                "that's there — I ask first, every time.\n\n"
                "A save from an account that no longer exists here has a way out too: I'll "
                "offer another account, or creating the missing one."),
            true));

        for (const std::pair<std::string, u64>& linha : linhas)
        {
            const std::string& pasta = linha.first;

            brls::ListItem* item = new brls::ListItem(pasta);
            item->setValue(humanSize(linha.second));
            item->getClickEvent()->subscribe(
                [caminho, pasta](brls::View* view) { openArchiveEntry(caminho, pasta); });

            list->addView(item);
        }
    }

    frame->setContentView(list);
    brls::Application::pushView(frame);
}

// Refaz uma lista sem perder o foco.
//
// Refazer com o foco DENTRO dela mata o foco do app inteiro: o clear(true)
// apaga a linha em foco e o destrutor da View zera o Application::currentFocus
// (view.cpp:618) — ninguém devolve depois. E foco nulo trava tudo: o navigate
// desiste na primeira linha (application.cpp:491, "if (!currentFocus...) return")
// e o próprio X não volta, porque a ação está registrada NA LISTA e a busca por
// ação começa no que está em foco (application.cpp:563). Resultado: apertar X
// uma vez deixava a aba de pedra — nem D-pad, nem A, nem outro X — até sair da
// aba e voltar.
static void refillKeepingFocus(brls::List* list, const std::function<void()>& refill)
{
    // Onde o foco estava, em índice de filho da lista. -1 = o foco não estava
    // aqui dentro, e aí não há o que devolver.
    int indice = -1;
    for (brls::View* v = brls::Application::getCurrentFocus(); v; v = v->getParent())
    {
        if (v->getParent() != list)
            continue;

        for (size_t i = 0; i < list->getViewsCount(); i++)
            if (list->getChild(i) == v)
            {
                indice = (int)i;
                break;
            }
        break;
    }

    refill();

    if (indice < 0)
        return;

    // O mesmo lugar de antes, ou o primeiro dali pra baixo que aceite foco: a
    // lista mudou porque foi atualizada, e a linha de antes pode não existir
    // mais. getDefaultFocus() devolve nulo em Label, que é o que separa linha
    // clicável de texto solto.
    for (size_t i = (size_t)indice; i < list->getViewsCount(); i++)
        if (brls::View* alvo = list->getChild(i)->getDefaultFocus())
        {
            brls::Application::giveFocus(alvo);
            return;
        }

    brls::Application::giveFocus(list); // pega o primeiro que aceitar

    // Lista sem nenhuma linha focável (ex: "nenhum jogo encontrado"): sobe até
    // achar quem aceite — a barra lateral das abas aceita. Sem isto o app fica
    // sem foco nenhum, que é exatamente o travamento que este helper existe
    // pra evitar.
    for (brls::View* v = list->getParent(); v && !brls::Application::getCurrentFocus();
         v             = v->getParent())
        brls::Application::giveFocus(v);
}

// A tela do arquivo com tudo. Todo o assunto ".nxsaves" mora aqui dentro.
//
// Estava tudo solto na aba "Tudo de uma vez", que ficou com quatro cabeçalhos,
// sete linhas e um textão numa lista só — "organiza melhor, muito bagunçado.
// por exemplo o tudo de uma vez, tem umas 30 sessoes totalmente confusas"
//. Seis daquelas linhas eram sobre este arquivo, que é assunto de
// quem já decidiu usar arquivo; quem não decidiu não precisa ler nada disso pra
// achar o "sincronizar tudo".
static void fillArchivePage(brls::List* list, std::vector<TitleEntry> todos)
{
    list->clear(true);

    // Metade das linhas desta tela só faz sentido com o arquivo já no cartão.
    bool tem = syncjob_has_archive();

    list->addView(new brls::Header(TR("Fazer o arquivo", "Make the file")));

    brls::ListItem* fazerESubir = new brls::ListItem(
        std::string(TR("Juntar tudo e subir pro ", "Pack everything and upload to ")) + nuvem(),
        std::string(TR("Junta o save de todos os jogos num arquivo só e manda pro ",
            "Packs every game's save into a single file and sends it to "))
            + nuvem() + ".");
    fazerESubir->getClickEvent()->subscribe([todos](brls::View* view) {
        openJob(new Job(TR("Tudo num arquivo só", "Everything in one file"),
                    [todos](Job* job) { return jobArchiveAll(job, todos, true); }),
            true);
    });
    list->addView(fazerESubir);

    brls::ListItem* soFazer = new brls::ListItem(
        TR("Só juntar, sem subir", "Just pack it, don't upload"),
        TR("Deixa o arquivo em switch/SwitchSaveSync/ e não encosta na internet.",
            "Leaves the file in switch/SwitchSaveSync/ and doesn't touch the internet."));
    soFazer->getClickEvent()->subscribe([todos](brls::View* view) {
        openJob(new Job(TR("Tudo num arquivo só", "Everything in one file"),
                    [todos](Job* job) { return jobArchiveAll(job, todos, false); }),
            true);
    });
    list->addView(soFazer);

    if (tem)
    {
        brls::ListItem* subir = new brls::ListItem(
            TR("Subir o que já está no cartão", "Upload what's already on the SD card"),
            std::string(TR("Manda pro ", "Sends the current file to ")) + nuvem()
                + TR(" o arquivo de agora, sem refazer.", " without rebuilding it."));
        subir->getClickEvent()->subscribe([](brls::View* view) {
            openJob(new Job(TR("Subir o arquivo", "Upload the file"), jobArchiveUpload), false);
        });
        list->addView(subir);
    }

    list->addView(new brls::Header(TR("Trazer de volta", "Bring it back")));

    brls::ListItem* baixar = new brls::ListItem(
        std::string(TR("Baixar de ", "Download from ")) + nuvem(),
        TR("Traz o arquivo pro cartão. Não escreve em save nenhum — isso é escolha sua, "
           "save por save.",
            "Brings the file to the SD card. Doesn't write to any save — that's your call, "
            "save by save."));
    baixar->getClickEvent()->subscribe([](brls::View* view) {
        openJob(new Job(TR("Baixar o arquivo", "Download the file"), jobArchiveDownload), false,
            [](bool success) {
                if (success)
                    brls::Application::notify(TR("Aperte X pra atualizar esta tela",
                        "Press X to refresh this screen"));
            });
    });
    list->addView(baixar);

    if (tem)
    {
        char caminho[0x300];
        syncjob_archive_path(caminho, sizeof(caminho));
        std::string arquivo = caminho;

        brls::ListItem* dentro = new brls::ListItem(
            TR("Ver e restaurar o que tem dentro", "See and restore what's inside"),
            TR("Lista os saves guardados no arquivo do cartão. Cada um dá pra pôr de volta.",
                "Lists the saves stored in the file on the SD card. Each one can be put back."));
        dentro->getClickEvent()->subscribe([arquivo](brls::View* view) {
            openArchiveContents(arquivo, TR("Dentro do arquivo", "Inside the file"));
        });
        list->addView(dentro);
    }

    // O textão de antes virou três linhas. O que precisava sobreviver era só
    // isto: o formato é nosso, então o arquivo depende deste app existir — a
    // diferença entre uma escolha e uma armadilha.
    //
    // E o aviso das linhas que ainda não estão na tela vem virado do avesso:
    // "quando tiver, aparece mais coisa aqui", e não "você não tem". Dito como
    // falta, ele lia como recusa logo na primeira vez.
    list->addView(new brls::Label(brls::LabelStyle::DESCRIPTION,
        std::string(tem ? "" :
            TR("Assim que existir um arquivo no cartão, aparecem aqui mais duas linhas: "
               "subir o que já está pronto e ver o que tem dentro. Aperte X pra atualizar.\n\n",
                "As soon as a file exists on the SD card, two more lines show up here: "
                "upload what's already made, and see what's inside. Press X to refresh.\n\n"))
        + TR(".nxsaves é um formato nosso: não é zip e só este app lê. O modo normal — uma "
             "pasta por jogo na nuvem — continua sendo o recomendado, e os dois convivem. "
             "O arquivo é prático pra levar tudo de uma vez; a pasta por jogo é o que "
             "continua servindo se um dia este app sumir.",
              ".nxsaves is a format of ours: it isn't a zip, and only this app reads it. The "
              "normal mode — one folder per game in the cloud — is still the recommended one, "
              "and the two coexist. The file is handy for carrying everything at once; the "
              "folder per game is what still works if this app ever disappears."),
        true));
}

static void openArchivePage(std::vector<TitleEntry> todos)
{
    brls::AppletFrame* frame = new brls::AppletFrame(true, true);
    frame->setTitle(TR("Arquivo com tudo", "The file with everything"));
    frame->setFooterText(TR("Um pacote só, com o save de todos os jogos",
        "A single package, with every game's save"));

    brls::List* list = new brls::List();
    fillArchivePage(list, todos);

    // O X refaz a lista porque duas linhas dependem do arquivo existir: sem
    // isto, depois de juntar ou de baixar ele teria que sair e voltar pra elas
    // aparecerem.
    list->registerAction(TR("Atualizar", "Refresh"), brls::Key::X, [list, todos] {
        refillKeepingFocus(list, [list, todos] { fillArchivePage(list, todos); });
        return true;
    });

    frame->setContentView(list);
    brls::Application::pushView(frame);
}

// A aba "Tudo de uma vez": o que age em TODOS os saves de uma vez só.
//
// Estava tudo espremido no topo da lista de jogos, empurrando os jogos pra
// baixo e misturando "sincroniza esse aí" com "sincroniza tudo". São coisas
// diferentes e agora moram em lugares diferentes.
static void fillBulkList(brls::List* list)
{
    list->clear(true);

    // A cópia que os trabalhos em lote levam. Feita AQUI, na thread da
    // interface: assim o que a tela diz e o que o trabalho faz são a mesma
    // coisa. Vai todo mundo, não a lista de jogos: o mesmo jogo com dois saves
    // aparece uma linha só lá, mas são dois saves pra sincronizar.
    std::vector<TitleEntry> todos(g_titles, g_titles + g_titles_count);

    if (todos.empty())
    {
        list->addView(new brls::Label(brls::LabelStyle::DESCRIPTION,
            TR("Nenhum save encontrado neste console. Abra a aba \"Meus jogos\" e aperte X "
               "pra procurar de novo.",
                "No saves found on this console. Open the \"My games\" tab and press X to "
                "look again."),
            true));
        return;
    }

    list->addView(new brls::Header(TR("Todos os saves de uma vez", "Every save at once")));

    brls::ListItem* syncAllItem = new brls::ListItem(
        TR("Sincronizar todos os saves", "Sync every save"),
        TR("Passa por cada save decidindo sozinho pra que lado vai. Quando os dois lados "
           "mudaram, deixa quieto e avisa no fim.",
            "Goes through every save deciding which way each one goes. When both sides have "
            "changed, it leaves it alone and tells you at the end."));
    syncAllItem->setValue(std::to_string(todos.size()) + TR(" saves", " saves"));
    syncAllItem->getClickEvent()->subscribe([todos](brls::View* view) {
        brls::Dialog* dialog = new brls::Dialog(
            TR(std::string("Vou passar pelos ") + std::to_string(todos.size())
                    + " saves, um por um, e decidir sozinho pra que lado cada um vai.\n\n"
                      "Quando os dois lados tiverem mudado, eu NÃO escolho: aquele jogo "
                      "fica de fora e aparece na lista do fim, pra você resolver.\n\n"
                      "Começar?",
                std::string("I'll go through all ") + std::to_string(todos.size())
                    + " saves, one by one, deciding which way each one goes.\n\n"
                      "When both sides have changed I will NOT choose: that game is left "
                      "alone and shows up in the list at the end for you to sort out.\n\n"
                      "Start?"));

        dialog->addButton(TR("Cancelar", "Cancel"), [dialog](brls::View* view) { dialog->close(); });
        dialog->addButton(TR("Sincronizar", "Sync"), [dialog, todos](brls::View* view) {
            dialog->close([todos]() {
                openJob(new Job(TR("Sincronizando todos os saves", "Syncing every save"),
                            [todos](Job* job) { return jobSyncAll(job, todos); }),
                    true);
            });
        });

        dialog->setCancelable(true);
        dialog->open();
    });
    list->addView(syncAllItem);

    // Uma linha só pro arquivo, e o assunto inteiro dele numa tela à parte.
    // Esta aba tem duas coisas pra fazer, não sete.
    brls::ListItem* arquivoItem = new brls::ListItem(
        TR("Guardar tudo num arquivo só", "Store everything in one file"),
        TR("Junta o save de todos os jogos num pacote só, pra levar tudo de uma vez. Abre a "
           "tela de juntar, subir, baixar e restaurar.",
            "Packs every game's save into a single package, to carry everything at once. "
            "Opens the pack, upload, download and restore screen."));
    // Só diz alguma coisa quando TEM. O "ainda não tem" que ficava aqui era o
    // primeiro de dois avisos de falta que ele levava antes de fazer nada:
    // conversa privada removida do historico
    // conversa privada removida do historico
    // faltando — e porta não precisa avisar que a sala está vazia.
    if (syncjob_has_archive())
        arquivoItem->setValue(TR("tem um no cartão", "one is on the SD card"));
    arquivoItem->getClickEvent()->subscribe(
        [todos](brls::View* view) { openArchivePage(todos); });
    list->addView(arquivoItem);

    list->addView(new brls::Label(brls::LabelStyle::DESCRIPTION,
        TR("Os dois fazem coisas diferentes. Sincronizar mantém uma pasta por jogo na nuvem, "
           "que é o modo normal e o recomendado. O arquivo é um pacote só, prático pra levar "
           "tudo de uma vez — e dá pra usar os dois.",
            "These two do different things. Syncing keeps one folder per game in the cloud, "
            "which is the normal, recommended mode. The file is a single package, handy for "
            "carrying everything at once — and you can use both."),
        true));
}

// A lista de jogos, e só ela. O que agia em tudo de uma vez saiu daqui pra aba
// "Tudo de uma vez": estava empurrando os jogos pra baixo da tela.
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
        refillKeepingFocus(list, [list] { fillGamesList(list); });
        brls::Application::notify(TR("Lista atualizada", "List refreshed"));
        return true;
    });

    return list;
}

static brls::List* createBulkTab()
{
    brls::List* list = new brls::List();
    g_bulk_list      = list;
    fillBulkList(list);

    // Mesmo X da aba dos jogos. Aqui ele importa por outro motivo: depois de
    // baixar o arquivo do Drive, é o que faz o "ver o que tem dentro" aparecer.
    list->registerAction(TR("Atualizar", "Refresh"), brls::Key::X, [list] {
        g_titles_count = titles_list_with_savedata(g_titles, MAX_TITLES);
        refillKeepingFocus(list, [list] { fillBulkList(list); });
        brls::Application::notify(TR("Atualizado", "Refreshed"));
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

// Confere se o servidor WebDAV responde com o que está gravado. Roda num Job
// porque bate na rede, e rede no Switch demora o quanto quiser.
static bool jobWebdavTest(Job* job)
{
    if (!ensureNetwork(job))
        return false;

    WebdavConfig cfg;
    if (!webdav_get_config(&cfg))
    {
        job->setStatus(TR("Nenhum servidor gravado ainda.", "No server saved yet."));
        return false;
    }

    job->setStatus(std::string(TR("Falando com ", "Talking to ")) + cfg.url + "...");

    char erro[256] = { 0 };
    if (!webdav_test_connection(erro, sizeof(erro)))
    {
        job->setStatus(TR("O servidor não aceitou.", "The server didn't accept it."));
        job->log(erro, true);
        return false;
    }

    job->setStatus(TR("Servidor respondeu. Endereço, usuário e senha estão certos.",
        "The server answered. Address, username and password are right."));
    return true;
}

// A tela da conta do Google Drive.
//
// Estava aberta na aba Ajustes: três linhas (conta, entrar, sair) mais um
// textão sobre OneDrive/Dropbox/iCloud, tudo empilhado antes do idioma. Virou
// uma linha só que abre isto aqui, igual à do WebDAV — as duas nuvens agora se
// configuram do mesmo jeito, em vez de uma ser um botão e a outra ser meia
// tela ("organiza melhor, muito bagunçado",).
//
// Nada daqui vai pra variável global de propósito: esta tela morre quando ele
// aperta B, e ponteiro guardado pra ela viraria ponteiro solto.
static void openGoogleSetup()
{
    brls::AppletFrame* frame = new brls::AppletFrame(true, true);
    frame->setTitle(TR("Conta do Google Drive", "Google Drive account"));

    brls::List* list = new brls::List();

    list->addView(new brls::Label(brls::LabelStyle::DESCRIPTION,
        TR("Pra entrar, o app mostra um código pra você digitar no celular ou no PC. O "
           "Switch não pede senha em momento nenhum, e não passa senha nenhuma pra frente.",
            "To sign in, the app shows a code for you to type on your phone or PC. The "
            "Switch never asks for a password, and never passes one along."),
        true));

    list->addView(new brls::Header(TR("Conta", "Account"), false));

    brls::ListItem* estado = new brls::ListItem(TR("Situação", "Status"));
    brls::ListItem* entrar = new brls::ListItem(TR("Entrar", "Sign in"));
    brls::ListItem* sair   = new brls::ListItem(TR("Sair da conta", "Sign out"),
        TR("Apaga o token guardado no cartão. Não mexe em nada no Drive.",
            "Deletes the token kept on the SD card. Nothing on Drive is touched."));

    // Um lambda só pra repintar as três linhas e a aba de trás. Com a conta
    // conversa privada removida do historico
    // conversa privada removida do historico
    // conversa privada removida do historico
    // dizendo o que realmente faz.
    auto repinta = [estado, entrar, sair]() {
        bool logged = oauth_is_logged_in();

        estado->setValue(logged ? TR("conectada", "connected")
                                : TR("desconectada", "disconnected"),
            !logged);

        entrar->setLabel(logged ? TR("Trocar de conta", "Switch account")
                                : TR("Entrar", "Sign in"));
        entrar->setValue(logged ? TR("já conectado", "already signed in") : "", logged);

        sair->setValue(logged ? "" : TR("nada pra sair", "not signed in"), !logged);

        updateAccountViews();
    };

    entrar->getClickEvent()->subscribe([repinta](brls::View* view) {
        openJob(new Job(TR("Entrar na conta Google", "Sign in to Google"), jobLogin), true,
            [repinta](bool success) { repinta(); });
    });

    sair->getClickEvent()->subscribe([repinta](brls::View* view) {
        oauth_logout();
        repinta();
        brls::Application::notify(TR("Conta desconectada", "Account disconnected"));
    });

    list->addView(estado);
    list->addView(entrar);
    list->addView(sair);
    repinta();

    list->addView(new brls::Label(brls::LabelStyle::DESCRIPTION,
        TR("O acesso pedido é o \"drive.file\": o app enxerga só os arquivos que ele mesmo "
           "criou, não o resto do seu Drive. O login fica no cartão, em "
           "/switch/SwitchSaveSync/token.txt, e sai de vez no Sair da conta.",
            "The scope it asks for is \"drive.file\": the app only sees the files it created "
            "itself, not the rest of your Drive. The login stays on the SD card, in "
            "/switch/SwitchSaveSync/token.txt, and Sign out removes it for good."),
        true));

    frame->setContentView(list);
    brls::Application::pushView(frame);
}

// conversa privada removida do historico
//
// O texto é o mesmo de antes, só que fora do caminho: estava aberto na aba
// Ajustes, entre o seletor de nuvem e a conta do Google, e é uma resposta que
// se lê uma vez na vida.
static void openOutrasNuvens()
{
    brls::AppletFrame* frame = new brls::AppletFrame(true, true);
    frame->setTitle(TR("E as outras nuvens?", "What about the other clouds?"));

    brls::List* list = new brls::List();

    // Antes esta linha só dizia "dá pra fazer, mas não está feito", que é um
    // conversa privada removida do historico
    // conversa privada removida do historico
    // conversa privada removida do historico
    list->addView(new brls::ListItem("OneDrive, Dropbox",
        TR("Dão pra fazer, e não estão feitos. Os dois exigem registrar um aplicativo no "
           "portal deles pra sair um código de acesso — de graça, só chato.\n\n"
           "Se você quiser fazer, tem um guia passo a passo no NUVENS.md do repositório "
           "(CLOUDS.md em inglês): "
           "são doze funções num arquivo novo e três linhas de registro, e os endereços do "
           "OneDrive já estão mastigados lá. O resto do app não muda.",
            "Both are doable, and neither is done. Each requires registering an app on "
            "their portal to get an access key — free, just tedious.\n\n"
            "If you want to do it, there's a step-by-step guide in the repository's "
            "CLOUDS.md: twelve functions in one new file and three lines to register them, "
            "with the OneDrive endpoints already chewed through. Nothing else in the app "
            "changes.")));

    list->addView(new brls::ListItem("iCloud Drive",
        TR("Não dá. A Apple não tem API pública pra outro programa mexer no iCloud Drive "
           "de fora, e o caminho que existe (CloudKit Web Services) cobra a assinatura "
           "anual de desenvolvedor. Fica de fora, e é definitivo.",
            "Can't be done. Apple has no public API for another program to touch iCloud "
            "Drive from outside, and the one path that exists (CloudKit Web Services) "
            "requires the paid yearly developer membership. It's out, and that's final.")));

    list->addView(new brls::ListItem(TR("NAS, Nextcloud, Synology, QNAP, Box",
                                        "NAS, Nextcloud, Synology, QNAP, Box"),
        TR("Esses já dão, hoje: todos falam WebDAV, que é a segunda opção do seletor de "
           "nuvem. É endereço, usuário e senha, e pronto.",
            "These already work today: they all speak WebDAV, which is the second option "
            "in the cloud selector. Address, username and password, and that's it.")));

    frame->setContentView(list);
    brls::Application::pushView(frame);
}

// A tela do servidor WebDAV: endereço, usuário, senha e um botão pra conferir.
//
// Em página separada de propósito. São quatro campos que só interessam a quem
// escolheu WebDAV, e enfiar tudo isso no meio dos Ajustes empurrava idioma e
// senha pra fora da tela.
static void openWebdavSetup()
{
    brls::AppletFrame* frame = new brls::AppletFrame(true, true);
    frame->setTitle(TR("Servidor WebDAV", "WebDAV server"));

    brls::List* list = new brls::List();

    list->addView(new brls::Label(brls::LabelStyle::DESCRIPTION,
        TR("Serve pra qualquer coisa que fale WebDAV: Nextcloud, ownCloud, NAS da "
           "Synology ou da QNAP, Box, e servidor que você mesmo suba. É o jeito de "
           "guardar save sem depender de conta de empresa nenhuma.\n\n"
           "O endereço é o que o seu servidor chama de \"WebDAV\" nas configurações — "
           "no Nextcloud é algo como https://sua-nuvem/remote.php/dav/files/seu-usuario.",
            "Works with anything that speaks WebDAV: Nextcloud, ownCloud, a Synology or "
            "QNAP NAS, Box, and servers you host yourself. It's the way to keep saves "
            "without depending on any company account.\n\n"
            "The address is whatever your server calls \"WebDAV\" in its settings — on "
            "Nextcloud it looks like https://your-cloud/remote.php/dav/files/your-user."),
        true));

    list->addView(new brls::Header(TR("Dados de acesso", "Credentials"), false));

    // O get devolve false quando ainda não tem endereço, mas preenche o struct
    // de qualquer jeito — então cada linha olha pro SEU campo. Sem isso, quem
    // digitasse o usuário antes do endereço via "vazio" no que acabou de
    // escrever.
    WebdavConfig atual;
    webdav_get_config(&atual);

    brls::ListItem* urlItem = new brls::ListItem(TR("Endereço", "Address"));
    urlItem->setValue(atual.url[0] ? atual.url : TR("vazio", "empty"), !atual.url[0]);

    brls::ListItem* userItem = new brls::ListItem(TR("Usuário", "Username"));
    userItem->setValue(atual.user[0] ? atual.user : TR("vazio", "empty"), !atual.user[0]);

    brls::ListItem* passItem = new brls::ListItem(TR("Senha", "Password"));
    passItem->setValue(atual.pass[0] ? "••••••••" : TR("vazia", "empty"), !atual.pass[0]);

    // Um lambda só pra reler do cartão e repintar as três linhas: qualquer
    // campo que muda mexe no mesmo arquivo, e assim nenhuma linha fica
    // mostrando o valor velho.
    auto repinta = [urlItem, userItem, passItem]() {
        WebdavConfig c;
        webdav_get_config(&c);

        urlItem->setValue(c.url[0] ? c.url : TR("vazio", "empty"), !c.url[0]);
        userItem->setValue(c.user[0] ? c.user : TR("vazio", "empty"), !c.user[0]);
        passItem->setValue(c.pass[0] ? "••••••••" : TR("vazia", "empty"), !c.pass[0]);

        updateWebdavItem();
        updateAccountViews();
    };

    urlItem->getClickEvent()->subscribe([repinta](brls::View* view) {
        WebdavConfig c;
        webdav_get_config(&c); // zera o struct quando não tem nada gravado

        std::string texto;
        if (!pedirTexto(TR("Endereço do servidor", "Server address"),
                TR("Cole o endereço WebDAV inteiro, com https:// na frente",
                    "Paste the whole WebDAV address, starting with https://"),
                c.url, false, WEBDAV_URL_MAX - 1, texto))
            return;

        if (texto.empty())
            return;

        snprintf(c.url, sizeof(c.url), "%s", texto.c_str());
        webdav_set_config(&c);
        repinta();
    });

    userItem->getClickEvent()->subscribe([repinta](brls::View* view) {
        WebdavConfig c;
        webdav_get_config(&c);

        std::string texto;
        if (!pedirTexto(TR("Usuário", "Username"),
                TR("O mesmo com que você entra no servidor",
                    "The same one you sign in to the server with"),
                c.user, false, WEBDAV_USER_MAX - 1, texto))
            return;

        if (texto.empty())
            return;

        snprintf(c.user, sizeof(c.user), "%s", texto.c_str());
        webdav_set_config(&c);
        repinta();
    });

    passItem->getClickEvent()->subscribe([repinta](brls::View* view) {
        WebdavConfig c;
        webdav_get_config(&c);

        std::string texto;
        if (!pedirTexto(TR("Senha", "Password"),
                TR("De preferência uma senha de aplicativo, não a sua senha principal",
                    "Preferably an app password, not your main one"),
                "", true, WEBDAV_PASS_MAX - 1, texto))
            return;

        if (texto.empty())
            return;

        snprintf(c.pass, sizeof(c.pass), "%s", texto.c_str());
        webdav_set_config(&c);
        repinta();
    });

    list->addView(urlItem);
    list->addView(userItem);
    list->addView(passItem);

    list->addView(new brls::Header(TR("Conferir", "Check"), false));

    brls::ListItem* testar = new brls::ListItem(TR("Testar conexão", "Test the connection"),
        TR("Pergunta ao servidor se ele está lá e se a senha serve. Não escreve nada.",
            "Asks the server whether it's there and whether the password works. Writes "
            "nothing."));
    testar->getClickEvent()->subscribe([](brls::View* view) {
        openJob(new Job(TR("Testar o servidor WebDAV", "Test the WebDAV server"), jobWebdavTest),
            true);
    });
    list->addView(testar);

    brls::ListItem* apagar = new brls::ListItem(TR("Apagar os dados de acesso",
        "Delete the credentials"));
    apagar->getClickEvent()->subscribe([repinta](brls::View* view) {
        brls::Dialog* d = new brls::Dialog(TR("Apagar endereço, usuário e senha do cartão?\n\n"
                                              "Não mexe em nada que já esteja no servidor.",
            "Delete the address, username and password from the SD card?\n\n"
            "Nothing already on the server is touched."));

        d->addButton(TR("Apagar", "Delete"), [d, repinta](brls::View* v) {
            d->close([repinta]() {
                webdav_clear_config();
                repinta();
                brls::Application::notify(TR("Dados apagados", "Credentials deleted"));
            });
        });
        d->addButton(TR("Deixa quieto", "Leave it"), [d](brls::View* v) { d->close(); });

        d->setCancelable(true);
        d->open();
    });
    list->addView(apagar);

    list->addView(new brls::Label(brls::LabelStyle::DESCRIPTION,
        TR("Duas coisas ditas na cara, porque as duas mordem:\n\n"
           "A senha fica escrita em texto puro no cartão SD, igual ao token do Google. "
           "Quem pegar o cartão lê. Por isso: crie uma senha de aplicativo no servidor e "
           "use ela aqui — assim, se vazar, você revoga só ela.\n\n"
           "Quando eu apago algo do servidor pra deixar igual ao console, mando um "
           "DELETE. Nextcloud, ownCloud e Synology têm lixeira e seguram isso por uns "
           "dias. Servidor WebDAV pelado apaga na hora, sem volta.",
            "Two things said plainly, because both bite:\n\n"
            "The password is stored in plain text on the SD card, same as the Google "
            "token. Whoever gets the card can read it. So: create an app password on the "
            "server and use that here — if it leaks, you revoke only that one.\n\n"
            "When I delete something from the server to match the console, I send a "
            "DELETE. Nextcloud, ownCloud and Synology have a trash bin and hold it for a "
            "few days. A bare WebDAV server deletes on the spot, with no way back."),
        true));

    frame->setContentView(list);
    brls::Application::pushView(frame);
}

static brls::List* createSettingsTab()
{
    brls::List* list = new brls::List();

    // ---- onde salvar ----
    // Primeira coisa da aba porque é a que manda em todas as outras: sem
    // escolher a nuvem, nada mais aqui tem efeito.
    list->addView(new brls::Header(TR("Onde salvar", "Where to save"), false));

    // A ordem da lista é a do enum CloudKind, então o índice que a borealis
    // devolve já é o valor — mesmo arranjo do seletor de idioma, e pelo mesmo
    // motivo: sem tabela de conversão no meio pra dessincronizar.
    std::vector<std::string> nuvens;
    for (int k = 0; k < CLOUD_COUNT; k++)
        nuvens.push_back(cloud_name((CloudKind)k));

    brls::SelectListItem* cloudItem = new brls::SelectListItem(TR("Nuvem", "Cloud"), nuvens,
        (unsigned)cloud_current(),
        TR("Onde os saves ficam guardados. Trocar aqui não move nada: cada nuvem fica "
           "com a cópia que já tem, e a próxima sincronização usa a que estiver "
           "escolhida.",
            "Where the saves are kept. Switching here moves nothing: each cloud keeps "
            "whatever copy it already has, and the next sync uses whichever one is "
            "selected."));

    cloudItem->getValueSelectedEvent()->subscribe([](int selected) {
        if (selected < 0)
            return; // fechou o dropdown sem escolher

        CloudKind escolha = (CloudKind)selected;
        cloud_set_current(escolha);
        updateAccountViews();

        // A aba "Tudo de uma vez" tem o nome da nuvem escrito nos botões, e
        // ela só se refaz no X. Sem isto ficaria dizendo o nome da nuvem
        // antiga até ele atualizar na mão.
        if (g_bulk_list)
            refillKeepingFocus(g_bulk_list, [] { fillBulkList(g_bulk_list); });

        const char* falta = cloud_setup_hint(escolha);
        if (falta && falta[0])
            brls::Application::notify(std::string(TR("Falta: ", "Missing: ")) + falta);
        else
            brls::Application::notify(std::string(cloud_name(escolha)) + " — "
                + TR("pronto", "ready"));
    });
    list->addView(cloudItem);

    // As duas nuvens configuram do mesmo jeito: uma linha cada, que abre a
    // tela dela. Antes o WebDAV era uma linha e o Google era meia aba.
    g_account_item = new brls::ListItem(TR("Conta do Google Drive", "Google Drive account"),
        TR("Entrar, trocar de conta ou sair. O Switch não pede senha em momento nenhum.",
            "Sign in, switch account or sign out. The Switch never asks for a password."));
    g_account_item->getClickEvent()->subscribe([](brls::View* view) { openGoogleSetup(); });
    list->addView(g_account_item);

    g_webdav_item = new brls::ListItem(TR("Servidor WebDAV", "WebDAV server"),
        TR("Endereço, usuário e senha do seu servidor. Vale pra Nextcloud, ownCloud, NAS "
           "da Synology ou da QNAP, Box e qualquer servidor que fale WebDAV.",
            "Address, username and password of your server. Works with Nextcloud, "
            "ownCloud, a Synology or QNAP NAS, Box and any server that speaks WebDAV."));
    g_webdav_item->getClickEvent()->subscribe([](brls::View* view) { openWebdavSetup(); });
    list->addView(g_webdav_item);

    brls::ListItem* outrasItem = new brls::ListItem(
        TR("E as outras nuvens?", "What about the other clouds?"),
        TR("A resposta sobre OneDrive, Dropbox e iCloud Drive — o que dá, o que não dá e "
           "por quê.",
            "The answer about OneDrive, Dropbox and iCloud Drive — what's possible, what "
            "isn't, and why."));
    outrasItem->getClickEvent()->subscribe([](brls::View* view) { openOutrasNuvens(); });
    list->addView(outrasItem);

    updateWebdavItem();

    // ---- idioma ----
    // Antes eram dois cabeçalhos, "Idioma" e "Controle parental", com UMA linha
    // debaixo de cada — mais título que conteúdo. Viraram um grupo só: as duas
    // são ajuste do app em si, e não de pra onde o save vai.
    list->addView(new brls::Header(TR("O app", "The app"), false));

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

    g_pin_item = new brls::ListItem(TR("Ligar a senha", "Turn the password on"),
        TR("Uma senha de 4 a 8 números, pedida toda vez que o app abre. Pra trocar ou "
           "tirar depois, o app pede a senha atual antes.",
            "A 4 to 8 digit password, asked every time the app opens. To change or remove "
            "it later, the app asks for the current one first."));
    g_pin_item->getClickEvent()->subscribe([](brls::View* view) { onPinItemClicked(); });
    list->addView(g_pin_item);
    updatePinItem();

    // Seis linhas viraram uma. O que não podia sumir é o "não é cofre": vender
    // a senha como proteção de verdade seria mentira, e mentira sobre save é
    // caro.
    list->addView(new brls::Label(brls::LabelStyle::DESCRIPTION,
        TR("A senha não é cofre: quem levar o cartão pro PC apaga o arquivo dela e entra. "
           "Serve pra criança não apagar progresso sem querer, e é só pra isso que dá pra "
           "contar com ela.",
            "The password is not a vault: anyone who takes the card to a PC can delete its "
            "file and get in. It's there so a kid doesn't wipe progress by accident, and "
            "that's all it can be counted on for."),
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

    // Vem logo depois da versão, e não no fim, porque é o tipo de aviso que só
    // serve se for lido antes — não depois que o save já deu problema.
    list->addView(new brls::ListItem(TR("Antes de mais nada", "Before anything else"),
        TR("Olha... isso aqui foi feito por uma pessoa só. Uma. Sem equipe de "
           "testes, sem QA, sem ninguém pra olhar por cima do meu ombro e dizer "
           "\"tem certeza disso?\". Foi testado no meu console, com os meus "
           "jogos, do meu jeito.\n\n"
           "Então sim, pode ter bug. Provavelmente tem, e eu ainda não faço "
           "ideia de qual — só vou descobrir do pior jeito possível, que é "
           "alguém me contando que quebrou.\n\n"
           "Se você achar algum, me conta? Por favor mesmo. Não precisa ser "
           "bonito nem técnico: um \"travou quando eu fiz tal coisa\" já me "
           "ajuda mais do que você imagina. É em "
           "github.com/NspxMiguel/SwitchSaveSync, na aba Issues.\n\n"
           "Eu conserto. Choro um pouquinho antes, mas conserto.\n\n"
           "E antes que dê ruim: guarda uma cópia dos saves que você não pode "
           "perder. Não porque eu ache que vai dar errado — mas porque save é "
           "save, e eu ia dormir bem melhor sabendo que você tem uma.",

            "Look... this was made by one person. One. No test team, no QA, "
            "nobody looking over my shoulder going \"you sure about that?\". It "
            "was tested on my console, with my games, my way.\n\n"
            "So yes, it can have bugs. It probably does, and I have no idea "
            "which ones yet — I'll only find out the worst way possible, which "
            "is someone telling me it broke.\n\n"
            "If you find one, will you tell me? Please. It doesn't have to be "
            "pretty or technical: a \"it froze when I did this\" helps more "
            "than you'd think. It's at github.com/NspxMiguel/SwitchSaveSync, "
            "under Issues.\n\n"
            "I'll fix it. I'll cry a little first, but I'll fix it.\n\n"
            "And before anything goes wrong: keep a copy of the saves you can't "
            "afford to lose. Not because I think it'll break — but a save is a "
            "save, and I'd sleep a lot better knowing you have one.")));

    list->addView(new brls::ListItem(TR("O que este app faz", "What this app does"),
        TR("Sincroniza os saves dos seus jogos com a nuvem que você escolher, na pasta \""
               DRIVE_APP_FOLDER_NAME "\". Cada jogo vira uma subpasta com o nome dele, e os "
           "arquivos ficam soltos lá dentro — dá pra baixar pelo site da nuvem normalmente.\n\n"
           "Dá pra escolher entre a conta do Google Drive e um servidor seu que fale WebDAV "
           "(Nextcloud, NAS da Synology ou da QNAP, e afins). Em Ajustes, Onde salvar.",
            "Syncs your games' saves with whichever cloud you pick, in the \""
                DRIVE_APP_FOLDER_NAME "\" folder. Each game becomes a subfolder named after "
            "it, with the files loose inside — you can download them from the cloud's "
            "website normally.\n\n"
            "You can pick between a Google Drive account and your own server that speaks "
            "WebDAV (Nextcloud, a Synology or QNAP NAS, and the like). In Settings, under "
            "Where to save.")));

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

    // O resto do "como instalar" mora no README, porque quem está lendo esta
    // tela já instalou. Este passo é a exceção: é o único que se faz DEPOIS,
    // com o app na mão, e é justamente o que ninguém descobre sozinho.
    list->addView(new brls::ListItem(TR("Deixar com cara de jogo, na tela inicial",
                                        "Putting it on the home screen, like a game"),
        TR("Dá pra ter um ícone deste app na tela inicial do console, do lado "
           "dos jogos, e abrir por ali. Quem faz isso é o Sphaira, no próprio "
           "console — não precisa de PC, nem de hacbrewpack, nem da sua "
           "prod.keys.\n\n"
           "1. Abra o Sphaira e ache o SwitchSaveSync na lista de homebrew.\n"
           "2. Abra as opções e escolha \"Install Forwarder\".\n"
           "3. Instalar vem desligado de fábrica no Sphaira; ele pergunta se "
           "pode ligar — responda que sim.\n\n"
           "Vale a pena por um motivo além da beleza: o atalho é instalado "
           "como APLICAÇÃO. Aberto pelo Álbum, o homebrew fica com uns 448 MB "
           "e a rede às vezes nem sobe; aberto pelo atalho, ele tem a memória "
           "e a rede inteiras. É o mesmo que o truque de segurar R num jogo, "
           "só que sem truque nenhum.\n\n"
           "Depois disso, não mude o .nro de lugar. O atalho guarda o caminho "
           "do arquivo, e o Sphaira tira o ID do título de um hash desse "
           "caminho — movido o arquivo, o atalho aponta pro vazio, e refazer a "
           "partir do caminho novo cria um segundo ícone em vez de consertar o "
           "primeiro. Deixe em /switch/SwitchSaveSync.nro e pronto.",

            "You can have an icon for this app on the console's home screen, "
            "next to your games, and open it from there. Sphaira does it, on "
            "the console itself — no PC, no hacbrewpack, no prod.keys of "
            "yours.\n\n"
            "1. Open Sphaira and find SwitchSaveSync in the homebrew list.\n"
            "2. Open the options and pick \"Install Forwarder\".\n"
            "3. Installing ships disabled in Sphaira; it asks whether it may "
            "turn it on — say yes.\n\n"
            "It's worth it for more than the looks: the shortcut is installed "
            "as an APPLICATION. Launched from the Album, homebrew gets around "
            "448 MB and the network sometimes fails to come up at all; "
            "launched from the shortcut, it has full memory and networking. "
            "Same as the hold-R-on-a-game trick, minus the trick.\n\n"
            "After that, don't move the .nro. The shortcut stores the file's "
            "path, and Sphaira derives the title ID from a hash of that path — "
            "move the file and the shortcut points at nothing, and rebuilding "
            "it from the new path creates a second icon instead of fixing the "
            "first. Leave it at /switch/SwitchSaveSync.nro and be done.")));

    list->addView(new brls::ListItem(TR("Privacidade", "Privacy"),
        TR("O app fala direto com a nuvem, sem servidor meu no meio.\n\n"
           "A credencial do Google que vem embutida é minha — é o que te poupa de "
           "criar um projeto no Google Cloud só pra usar um homebrew de save. Mas a "
           "conta é sua, o Drive é seu e os arquivos são seus: eu não tenho acesso a "
           "nada disso. Quem quiser usar uma credencial própria é só compilar; está "
           "explicado no README.\n\n"
           "No Google Drive, o acesso pedido é o \"drive.file\": o app só enxerga os "
           "arquivos que ele mesmo criou, não o resto do seu Drive. O login fica só no "
           "cartão, em /switch/SwitchSaveSync/token.txt, e sai de vez com o Sair da conta.\n\n"
           "No WebDAV, o endereço, o usuário e a senha ficam em "
           "/switch/SwitchSaveSync/webdav.cfg, em texto puro — por isso a tela pede uma "
           "senha de aplicativo, não a sua principal.",
            "The app talks straight to the cloud, with no server of mine in between.\n\n"
            "The Google credentials it ships with are mine — that's what saves you "
            "from creating a Google Cloud project just to use a save-sync homebrew. "
            "But the account is yours, the Drive is yours and the files are yours: I "
            "have no access to any of it. If you'd rather use your own credentials, "
            "just build it yourself; the README explains how.\n\n"
            "On Google Drive, the scope it asks for is \"drive.file\": it only sees the "
            "files it created itself, not the rest of your Drive. The login stays on the SD "
            "card only, in /switch/SwitchSaveSync/token.txt, and Sign out removes it for "
            "good.\n\n"
            "On WebDAV, the address, username and password live in "
            "/switch/SwitchSaveSync/webdav.cfg, in plain text — which is why the screen "
            "asks for an app password, not your main one.")));

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

    cloud_set_progress_cb(gui_progress_cb);
    cloud_set_abort_cb(gui_abort_cb);
    oauth_set_device_cb(gui_device_cb);
    oauth_load_saved_login();

    g_root = new brls::TabFrame();
    g_root->setTitle("SwitchSaveSync");
    g_root->setIcon(BOREALIS_ASSET("icon/app.jpg")); // o mesmo icone do .nro

    g_root->addTab(TR("Meus jogos", "My games"), createGamesTab());
    g_root->addTab(TR("Tudo de uma vez", "All at once"), createBulkTab());
    g_root->addTab(TR("Ajustes", "Settings"), createSettingsTab());
    g_root->addSeparator();
    g_root->addTab(TR("Sobre", "About"), createAboutTab());

    updateAccountViews();

    brls::Application::pushView(g_root);

    // Primeira vez no app (nenhuma nuvem configurada): já oferece os dois
    // caminhos em vez de deixar ele descobrir sozinho que precisa ir na aba
    // Ajustes. A pergunta olha pra nuvem escolhida, não só pro Google — quem
    // já pôs o servidor WebDAV não é perguntado de novo.
    if (!cloud_is_ready(cloud_current()) && http_ok)
    {
        brls::Dialog* welcome = new brls::Dialog(
            // Doze linhas viraram quatro. O texto antigo empurrava os três
            // botões pra fora da tela — mandaram a foto, com o
            // diálogo inteiro de texto e nenhum botão à vista, só o B pra sair.
            // A causa de raiz está consertada no dialog.cpp da borealis, mas
            // três parágrafos numa tela de primeira vez estavam errados de
            // qualquer jeito: os próprios botões já dizem quais são as opções.
            TR("Pra sincronizar, o app precisa de um lugar pra guardar os saves.\n\n"
               "Google Drive — você entra pelo celular, com um QR code. O Switch "
               "nunca pede senha.\n\n"
               "Servidor seu (WebDAV) — Nextcloud, NAS da Synology ou da QNAP.",

                "To sync, the app needs somewhere to keep your saves.\n\n"
                "Google Drive — you sign in from your phone, with a QR code. The "
                "Switch never asks for a password.\n\n"
                "Your own server (WebDAV) — Nextcloud, a Synology or QNAP NAS."));

        welcome->addButton(TR("Agora não", "Not now"),
            [welcome](brls::View* view) { welcome->close(); });
        welcome->addButton(TR("Conta Google", "Google account"), [welcome](brls::View* view) {
            welcome->close([]() {
                cloud_set_current(CLOUD_DRIVE);
                openJob(new Job(TR("Entrar na conta Google", "Sign in to Google"), jobLogin), true,
                    [](bool success) { updateAccountViews(); });
            });
        });
        welcome->addButton(TR("Servidor meu", "My own server"), [welcome](brls::View* view) {
            welcome->close([]() {
                cloud_set_current(CLOUD_WEBDAV);
                updateAccountViews();
                openWebdavSetup();
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
        brls::Application::notify(TR("Rede falhou (modo applet) — ver aba Ajustes",
            "Network failed (applet mode) — see the Settings tab"));
    else if (!http_ok)
        brls::Application::notify(TR("Rede falhou — ver detalhes na aba Ajustes",
            "Network failed — details in the Settings tab"));

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
