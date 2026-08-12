// SwitchSaveSync — overlay do Ultrahand.
//
// É a interface que roda POR CIMA do jogo, sem fechar nada. Serve pra:
//   - ver o que o autosync está fazendo;
//   - ligar/desligar o sysmodule (que não sobe no boot, de propósito);
//   - ligar/desligar o autosync sem matar o sysmodule;
//   - marcar jogo por jogo o que NÃO deve sincronizar. Essa é a lista do
//     botão "Não puxar save da nuvem nesse jogo", e é aqui que ela se
//     desmarca.
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
#include "lang.h"
}

// Sem acento de propósito, dos dois lados. A fonte que a libtesla carrega é a
// do sistema, mas o overlay desenha numa faixa estreita e caractere composto
// já apareceu cortado aqui — não vale o risco por uma cedilha.

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
// o que se via: o overlay tentava, falhava e não sobrava rastro nenhum.
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
        auto* frame = new tsl::elm::OverlayFrame("SwitchSaveSync", TR("jogos", "games"));
        auto* list  = new tsl::elm::List();

        list->addItem(new tsl::elm::CategoryHeader(TR("Off = nao sobe save nem puxa da nuvem", "Off = no upload, no download"), true));

        // Estático: a lista de TitleEntry é grande demais pra pilha do overlay.
        static TitleEntry entries[MAX_TITLES];
        size_t count = titles_list_with_savedata(entries, MAX_TITLES);

        if (count == 0)
        {
            list->addItem(new tsl::elm::ListItem(TR("Nenhum jogo com save encontrado", "No games with save data found")));
        }

        for (size_t i = 0; i < count; i++)
        {
            u64 id = entries[i].application_id;

            auto* item = new tsl::elm::ToggleListItem(
                entries[i].name, !syncstate_is_excluded(id), "Sync", "Off");

            // Se a gravação falhar, o botão volta pro que ele era: o desenho na
            // tela é a única pista de que ficou gravado, e deixar "Off" aceso
            // com a lista intacta no cartão é dizer que o jogo está protegido
            // quando ele não está.
            item->setStateChangedListener([id, item](bool on) {
                if (!syncstate_set_excluded(id, !on))
                    item->setState(!on);
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

        // O status NÃO é um ListItem.
        //
        // Era, e no console aparecia assim: "ame closed, preparing backup...".
        // O ListItem rola o texto que não cabe, e a linha de status quase
        // sempre não cabe — leva o nome do jogo junto. O que se via era o meio
        // de uma frase, cortado nas duas pontas, mudando de posição sozinho.
        //
        // Aqui a frase é quebrada em linhas e mostrada inteira. Três linhas de
        // altura, que é o que cabe sem empurrar os botões pra fora da tela.
        list->addItem(new tsl::elm::CustomDrawer(
            [this](tsl::gfx::Renderer* r, s32 x, s32 y, s32 w, s32 h) {
                this->rewrap(r, w - 40);
                r->drawString(this->wrapped, false, x + 20, y + 26, 20,
                              a(tsl::style::color::ColorText));
            }), 96); // a altura vai no addItem, nao no CustomDrawer

        list->addItem(new tsl::elm::CategoryHeader(TR("Controle", "Controls"), true));

        // O sysmodule não sobe no boot: quem liga é este botão.
        this->moduleItem = new tsl::elm::ToggleListItem(
            "Sysmodule", sysmoduleRunning(), TR("Ligado", "On"), TR("Desligado", "Off"));
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
                    TR("%s falhou: %08X (mod %d, desc %d)",
                       "%s failed: %08X (mod %d, desc %d)"),
                    on ? TR("Ligar", "Turning on") : TR("Desligar", "Turning off"),
                    (unsigned)g_lastRc,
                    R_MODULE(g_lastRc), R_DESCRIPTION(g_lastRc));
            }
            else
            {
                this->failMsg[0] = '\0';
            }
        });
        list->addItem(this->moduleItem);

        auto* autosyncItem = new tsl::elm::ToggleListItem(
            TR("Backup ao fechar", "Back up on close"), syncstate_autosync_enabled(), TR("Ligado", "On"), TR("Desligado", "Off"));
        autosyncItem->setStateChangedListener([](bool on) {
            syncstate_set_autosync_enabled(on);
        });
        list->addItem(autosyncItem);

        list->addItem(new tsl::elm::CategoryHeader(TR("Onde salvar", "Where to save"), true));

        // Salvar direto no cartao do console, e nao so na nuvem: os dois
        // destinos sao independentes e valem um sem o outro.
        auto* localItem = new tsl::elm::ToggleListItem(
            TR("Cartao SD", "SD card"), syncstate_dest_local(), TR("Ligado", "On"), TR("Desligado", "Off"));
        localItem->setStateChangedListener([](bool on) {
            syncstate_set_dest(on, syncstate_dest_cloud());
        });
        list->addItem(localItem);

        auto* cloudItem = new tsl::elm::ToggleListItem(
            "Google Drive", syncstate_dest_cloud(), TR("Ligado", "On"), TR("Desligado", "Off"));
        cloudItem->setStateChangedListener([](bool on) {
            syncstate_set_dest(syncstate_dest_local(), on);
        });
        list->addItem(cloudItem);

        auto* gamesItem = new tsl::elm::ListItem(TR("Jogos", "Games"), "▶");
        gamesItem->setClickListener([](u64 keys) {
            if (keys & HidNpadButton_A)
            {
                tsl::changeTo<GuiGames>();
                return true;
            }
            return false;
        });
        list->addItem(gamesItem);

        list->addItem(new tsl::elm::CategoryHeader(TR("Agora", "Now"), true));

        // Backup sob demanda: o overlay só deixa o pedido escrito, quem faz é
        // o sysmodule — e só com o jogo fechado, senão o save no cartão pode
        // estar pela metade.
        this->backupItem = new tsl::elm::ListItem(TR("Fazer backup do ultimo jogado", "Back up the last game played"));
        this->backupItem->setClickListener([this](u64 keys) {
            if (!(keys & HidNpadButton_A))
                return false;

            // A lista INTEIRA, e nao entries[0] de uma listagem de um.
            //
            // Pedir 1 nao devolve "o mais recente": devolve o primeiro que o
            // fsSaveDataInfoReader cuspiu, que e ordem de criacao do save. A
            // ordenacao por ultimo jogado acontece depois, sobre a lista pronta,
            // e com um elemento so ela nem roda (titles.c: `if (count < 2)
            // return;`). Ou seja, "fazer backup do ultimo jogado" mandava subir
            // o save de um jogo qualquer — o mais VELHO, normalmente — e escrevia
            // "pedido enviado" do mesmo jeito.
            static TitleEntry entries[MAX_TITLES];
            size_t count = titles_list_with_savedata(entries, MAX_TITLES); // [0] = mais recente

            if (count == 0)
                this->backupItem->setValue(TR("sem jogos", "no games"), true);
            else if (!sysmoduleRunning())
                this->backupItem->setValue(TR("ligue o sysmodule", "turn the sysmodule on"), true);
            else
            {
                syncstate_request_backup(entries[0].application_id);
                this->backupItem->setValue(TR("pedido enviado", "request sent"));
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
            snprintf(this->statusText, sizeof(this->statusText), "%s", this->failMsg);
            return;
        }

        // 640, e nao 128: a linha de status pode levar o nome do jogo junto, e
        // nome de jogo tem ate 0x201 bytes (titles.h). Com 128 o fgets cortava a
        // linha e jogava o resto fora sem avisar nada — na faixa da tela sobrava
        // um pedaco do nome, e o resultado sumia. O sysmodule agora poe o
        // resultado na frente, mas o buffer tem que caber a linha inteira do
        // mesmo jeito: o resto dela e o que se le rolando o item.
        char status[640];
        if (syncstate_get_status(status, sizeof(status)))
            snprintf(this->statusText, sizeof(this->statusText), "%s", status);
        else if (sysmoduleRunning())
            snprintf(this->statusText, sizeof(this->statusText), "%s",
                TR("Rodando, mas sem status ainda", "Running, no status yet"));
        else
            snprintf(this->statusText, sizeof(this->statusText), "%s",
                TR("Desligado (nunca escreveu status)", "Off (never wrote a status)"));
    }

private:
    // Quebra a linha de status no espaco disponivel.
    //
    // O maxWidth do drawString da libtesla NAO quebra linha: ele para de
    // desenhar no meio da frase. Era isso que aparecia no console —
    // "Nao consegui puxar — save local", sem o "mantido". Quem quebra tem que
    // ser quem escreve, e a unica coisa que a libtesla entende e o '\n'.
    //
    // Medir e o proprio drawString com cor transparente: e o jeito que a
    // libtesla usa internamente pra saber o tamanho de um texto.
    void rewrap(tsl::gfx::Renderer* r, s32 maxW)
    {
        if (strcmp(this->statusText, this->wrappedFrom) == 0)
            return; // mesma frase de antes: nada a refazer neste quadro
        snprintf(this->wrappedFrom, sizeof(this->wrappedFrom), "%s", this->statusText);

        char linha[sizeof(this->statusText)] = {0};
        size_t linhas = 1;
        this->wrapped[0] = '\0';

        const char* p = this->statusText;
        while (*p)
        {
            // Uma palavra, com o espaco que vier depois dela.
            char palavra[128];
            size_t n = 0;
            while (*p && *p != ' ' && n < sizeof(palavra) - 2) palavra[n++] = *p++;
            while (*p == ' ' && n < sizeof(palavra) - 1) { palavra[n++] = *p++; }
            palavra[n] = '\0';
            if (n == 0) break;

            char tentativa[sizeof(linha)];
            snprintf(tentativa, sizeof(tentativa), "%s%s", linha, palavra);

            auto [larg, alt] = r->drawString(tentativa, false, 0, 0, 20,
                                             tsl::style::color::ColorTransparent);
            if ((s32)larg > maxW && linha[0] != '\0')
            {
                if (linhas >= 3)
                    break; // mais que isso empurraria os botoes pra fora da tela
                strncat(this->wrapped, linha, sizeof(this->wrapped) - strlen(this->wrapped) - 2);
                strncat(this->wrapped, "\n", sizeof(this->wrapped) - strlen(this->wrapped) - 1);
                linhas++;
                snprintf(linha, sizeof(linha), "%s", palavra);
            }
            else
            {
                snprintf(linha, sizeof(linha), "%s", tentativa);
            }
        }

        strncat(this->wrapped, linha, sizeof(this->wrapped) - strlen(this->wrapped) - 1);
    }

    // Lido pelo CustomDrawer do status a cada quadro. char[], e nao
    // std::string, porque quem escreve aqui e o update() e quem le e o
    // desenho — realocar no meio do quadro nao vale a pena por 640 bytes.
    char statusText[640] = {0};
    char wrapped[768]    = {0};
    char wrappedFrom[640] = {0};
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
        // A tesla NÃO monta o sdmc, por mais que pareça. Ela tem um helper
        // (doWithSDCardHandle) que monta e desmonta dentro de um escopo, e esse
        // helper não é chamado em lugar nenhum do tesla.hpp. Ou seja: sem esta
        // linha, `sdmc:` não existe pro overlay e TODO fopen daqui falha
        // calado — era por isso que aparecia "Rodando, mas sem status ainda"
        // com o sysmodule vivo e o status.txt escrito no cartão. Mesma coisa
        // valia pra lista de jogos excluídos, que era marcada e não gravava.
        // (fsInitialize já foi feito pelo __appInit da tesla.)
        fsdevMountSdmc();

        // Depois do fsdevMountSdmc, nunca antes: o idioma mora num arquivo no
        // cartão (idioma.txt), então chamar isto acima da montagem lê nada e
        // cai calado no inglês. É o mesmo idioma que o app usa — quem troca lá
        // troca aqui, sem tela de configuração própria.
        lang_load();

        // pm:shell é o que lança o sysmodule. Se o loader do overlay não tiver
        // esse acesso, tudo falha aqui e não adianta culpar o sysmodule — por
        // isso o resultado é guardado em vez de jogado fora.
        g_pmshellOk = R_SUCCEEDED(pmshellInitialize());
        nsInitialize();
        accountInitialize(AccountServiceType_System);

        // pdm:qry TEM que ser aberto aqui, e nao la dentro do titles.c.
        //
        // O initServices roda dentro do doWithSmSession da tesla; a tela e os
        // cliques rodam DEPOIS, com a sessao do sm ja fechada. Quem tentar abrir
        // um servico novo la na frente cai num smGetService sem sessao e falha
        // sempre. Era o caso do pdm: o titles.c chamava pdmqryInitialize no meio
        // da listagem, tomava erro, desistia em silencio — e a lista NUNCA saia
        // ordenada por ultimo jogado no overlay. Abrindo aqui, o Initialize de
        // la vira um refcount++ e devolve 0 sem falar com o sm, que e como o ns
        // e o account ja funcionavam.
        pdmqryInitialize();
    }

    virtual void exitServices() override
    {
        pdmqryExit();
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
