// SwitchSaveSync — overlay do Ultrahand.
//
// É a interface que roda POR CIMA do jogo, sem fechar nada. Serve pra:
//   - ver o que o autosync está fazendo;
//   - ligar/desligar o sysmodule (que não sobe no boot, de propósito —
// conversa privada removida do historico
//   - ligar/desligar o autosync sem matar o sysmodule;
//   - marcar jogo por jogo o que NÃO deve sincronizar. Essa é a lista do
// conversa privada removida do historico
// conversa privada removida do historico
//
// É libtesla puro. O Ultrahand carrega .ovl de /switch/.overlays, e overlay
// de libtesla funciona lá do mesmo jeito — não existe SDK "de Ultrahand"
// separado pra isso.
//
// O overlay NÃO fala com o Google. Ele roda com o jogo na tela, memória
// curta e o usuário esperando; abrir TLS aqui seria pedir travada. Tudo que
// é rede fica no sysmodule, e os dois conversam por arquivo no SD
// (core/syncstate.c explica o porquê).

#define TESLA_INIT_IMPL
#include <tesla.hpp>

extern "C" {
#include "syncstate.h"
#include "titles.h"
}

// Esse id NÃO pode cair na faixa de aplicação do Atmosphère (>= 0100000000010000).
// O primeiro id que usei, 0100000000535953, caía — e lá dentro
// /atmosphere/contents/<id>/ não significa "programa avulso no cartão", significa
// "mod LayeredFS do jogo instalado com esse id". Como jogo nenhum tem esse id, o
// Atmosphère não tinha o que montar e o pmshellLaunchProgram devolvia
// 0x0035F202 = fs::ResultNotMounted (2-6905). Foi por isso que o sysmodule nunca
// subiu uma única vez, nem gerou crash report (ele nunca chegou a executar).
// 00FF... é o mesmo estilo do sys-clk (00FF0000636C6BFF), que sobe nesse console.
#define SYSMODULE_TID 0x00FF0000535953FFULL
#define MAX_TITLES    64

// ---------------------------------------------------------------------------
// sysmodule: está rodando? liga/desliga
// ---------------------------------------------------------------------------

static bool sysmoduleRunning()
{
    u64 pid = 0;
    if (R_FAILED(pmdmntGetProcessId(&pid, SYSMODULE_TID)))
        return false;
    return pid != 0;
}

// Guarda o último erro de verdade (Result cru) pra tela poder dizer o que
// aconteceu em vez de só "falhou". Sem isso, "nada acontece" era exatamente
// o que ele via: o overlay tentava, falhava e não sobrava rastro nenhum.
static Result g_lastRc   = 0;
static bool   g_pmshellOk = false;

static bool sysmoduleStart()
{
    if (!g_pmshellOk)
    {
        g_lastRc = MAKERESULT(Module_Libnx, LibnxError_NotInitialized);
        return false;
    }

    NcmProgramLocation loc {
        .program_id = SYSMODULE_TID,
        .storageID  = static_cast<u8>(NcmStorageId_None),
    };

    u64 pid  = 0;
    g_lastRc = pmshellLaunchProgram(0, &loc, &pid);
    if (R_FAILED(g_lastRc))
        return false;

    // Lançar com sucesso não quer dizer continuar vivo: se o sysmodule aborta
    // na inicialização (serviço faltando no NPDM, heap, TLS), o pm devolve OK
    // e o processo morre logo em seguida. Espera meio segundo e confere de
    // novo — senão o botão fica "Ligado" mentindo.
    svcSleepThread(500000000ULL);

    u64 check = 0;
    Result rc = pmdmntGetProcessId(&check, SYSMODULE_TID);
    if (R_FAILED(rc) || check == 0)
    {
        g_lastRc = rc;
        return false;
    }

    return true;
}

static bool sysmoduleStop()
{
    g_lastRc = pmshellTerminateProgram(SYSMODULE_TID);
    return R_SUCCEEDED(g_lastRc);
}

// ---------------------------------------------------------------------------
// Tela dos jogos: liga/desliga o sync de cada um
// ---------------------------------------------------------------------------

class GuiGames : public tsl::Gui
{
public:
    virtual tsl::elm::Element* createUI() override
    {
        auto* frame = new tsl::elm::OverlayFrame("SwitchSaveSync", "jogos");
        auto* list  = new tsl::elm::List();

        list->addItem(new tsl::elm::CategoryHeader("Off = nao sobe save nem puxa da nuvem", true));

        // Estático: a lista de TitleEntry é grande demais pra pilha do overlay.
        static TitleEntry entries[MAX_TITLES];
        size_t count = titles_list_with_savedata(entries, MAX_TITLES);

        if (count == 0)
        {
            list->addItem(new tsl::elm::ListItem("Nenhum jogo com save encontrado"));
        }

        for (size_t i = 0; i < count; i++)
        {
            u64 id = entries[i].application_id;

            auto* item = new tsl::elm::ToggleListItem(
                entries[i].name, !syncstate_is_excluded(id), "Sync", "Off");

            item->setStateChangedListener([id](bool on) {
                syncstate_set_excluded(id, !on);
            });

            list->addItem(item);
        }

        frame->setContent(list);
        return frame;
    }
};

// ---------------------------------------------------------------------------
// Tela principal
// ---------------------------------------------------------------------------

class GuiMain : public tsl::Gui
{
public:
    virtual tsl::elm::Element* createUI() override
    {
        auto* frame = new tsl::elm::OverlayFrame("SwitchSaveSync", "autosync");
        auto* list  = new tsl::elm::List();

        list->addItem(new tsl::elm::CategoryHeader("Status", true));

        this->statusItem = new tsl::elm::ListItem("");
        this->statusItem->setValue("");
        list->addItem(this->statusItem);

        list->addItem(new tsl::elm::CategoryHeader("Controle", true));

        // O sysmodule não sobe no boot: quem liga é este botão.
        this->moduleItem = new tsl::elm::ToggleListItem(
            "Sysmodule", sysmoduleRunning(), "Ligado", "Desligado");
        this->moduleItem->setStateChangedListener([this](bool on) {
            bool ok = on ? sysmoduleStart() : sysmoduleStop();
            if (!ok)
            {
                // Volta o botão pro estado real em vez de mentir na tela.
                this->moduleItem->setState(!on);

                // Erro FICA na tela. Antes isso era um bool que o update()
                // do quadro seguinte já apagava, escrevendo "Sem status" por
                // cima — o motivo aparecia por 1/60 de segundo e sumia.
                snprintf(this->failMsg, sizeof(this->failMsg),
                    "%s falhou: %08X (mod %d, desc %d)",
                    on ? "Ligar" : "Desligar", (unsigned)g_lastRc,
                    R_MODULE(g_lastRc), R_DESCRIPTION(g_lastRc));
            }
            else
            {
                this->failMsg[0] = '\0';
            }
        });
        list->addItem(this->moduleItem);

        auto* autosyncItem = new tsl::elm::ToggleListItem(
            "Backup ao fechar jogo", syncstate_autosync_enabled(), "Ligado", "Desligado");
        autosyncItem->setStateChangedListener([](bool on) {
            syncstate_set_autosync_enabled(on);
        });
        list->addItem(autosyncItem);

        list->addItem(new tsl::elm::CategoryHeader("Onde salvar", true));

        // conversa privada removida do historico
        // conversa privada removida do historico
        auto* localItem = new tsl::elm::ToggleListItem(
            "Cartao SD", syncstate_dest_local(), "Ligado", "Desligado");
        localItem->setStateChangedListener([](bool on) {
            syncstate_set_dest(on, syncstate_dest_cloud());
        });
        list->addItem(localItem);

        auto* cloudItem = new tsl::elm::ToggleListItem(
            "Google Drive", syncstate_dest_cloud(), "Ligado", "Desligado");
        cloudItem->setStateChangedListener([](bool on) {
            syncstate_set_dest(syncstate_dest_local(), on);
        });
        list->addItem(cloudItem);

        auto* gamesItem = new tsl::elm::ListItem("Jogos", "▶");
        gamesItem->setClickListener([](u64 keys) {
            if (keys & HidNpadButton_A)
            {
                tsl::changeTo<GuiGames>();
                return true;
            }
            return false;
        });
        list->addItem(gamesItem);

        list->addItem(new tsl::elm::CategoryHeader("Agora", true));

        // Backup sob demanda: o overlay só deixa o pedido escrito, quem faz é
        // o sysmodule — e só com o jogo fechado, senão o save no cartão pode
        // estar pela metade.
        this->backupItem = new tsl::elm::ListItem("Fazer backup do ultimo jogado");
        this->backupItem->setClickListener([this](u64 keys) {
            if (!(keys & HidNpadButton_A))
                return false;

            static TitleEntry entries[MAX_TITLES];
            size_t count = titles_list_with_savedata(entries, 1); // [0] = mais recente

            if (count == 0)
                this->backupItem->setValue("sem jogos", true);
            else if (!sysmoduleRunning())
                this->backupItem->setValue("ligue o sysmodule", true);
            else
            {
                syncstate_request_backup(entries[0].application_id);
                this->backupItem->setValue("pedido enviado");
            }
            return true;
        });
        list->addItem(this->backupItem);

        frame->setContent(list);
        return frame;
    }

    // Roda a cada frame do overlay: relê o status.txt que o sysmodule escreve.
    virtual void update() override
    {
        // O erro tem prioridade sobre o status: se a última coisa que ele fez
        // foi tentar ligar e não deu, é isso que ele precisa ler.
        if (this->failMsg[0])
        {
            this->statusItem->setText(this->failMsg);
            return;
        }

        char status[128];
        if (syncstate_get_status(status, sizeof(status)))
            this->statusItem->setText(status);
        else if (sysmoduleRunning())
            this->statusItem->setText("Rodando, mas sem status ainda");
        else
            this->statusItem->setText("Desligado (nunca escreveu status)");
    }

private:
    tsl::elm::ListItem* statusItem = nullptr;
    tsl::elm::ListItem* backupItem = nullptr;
    tsl::elm::ToggleListItem* moduleItem = nullptr;
    char failMsg[128] = {0};
};

// ---------------------------------------------------------------------------

class SwitchSaveSyncOverlay : public tsl::Overlay
{
public:
    // A tesla sobe sm, fs, hid, pl, pmdmnt, hidsys e setsys — e SÓ isso.
    virtual void initServices() override
    {
        // Eu escrevi "a tesla já monta o sdmc" e não era verdade. Ela tem um
        // helper (doWithSDCardHandle) que monta e desmonta dentro de um escopo,
        // e esse helper não é chamado em lugar nenhum do tesla.hpp. Ou seja: sem
        // esta linha, `sdmc:` não existe pro overlay e TODO fopen daqui falha
        // conversa privada removida do historico
        // o sysmodule vivo e o status.txt escrito no cartão. Mesma coisa valia
        // pra lista de jogos excluídos, que ele marcava e não gravava.
        // ( — fsInitialize já foi feito pelo __appInit da tesla.)
        fsdevMountSdmc();

        // pm:shell é o que lança o sysmodule. Se o loader do overlay não tiver
        // esse acesso, tudo falha aqui e não adianta culpar o sysmodule — por
        // isso o resultado é guardado em vez de jogado fora.
        g_pmshellOk = R_SUCCEEDED(pmshellInitialize());
        nsInitialize();
        accountInitialize(AccountServiceType_System);
    }

    virtual void exitServices() override
    {
        accountExit();
        nsExit();
        pmshellExit();
        fsdevUnmountDevice("sdmc");
    }

    virtual std::unique_ptr<tsl::Gui> loadInitialGui() override
    {
        return initially<GuiMain>();
    }
};

int main(int argc, char** argv)
{
    return tsl::loop<SwitchSaveSyncOverlay>(argc, argv);
}
