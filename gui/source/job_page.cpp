#include "job_page.hpp"

#include "qr_view.hpp"

extern "C" {
#include "lang.h"
}

namespace
{

// Uma Label que aceita foco.
//
// A borealis só rola a lista até o item FOCADO — é o ScrollView reagindo ao
// onChildFocusGained, e não existe jeito público de rolar sem foco: o scrollY
// dele é privado e não tem setter. E Label não recebe foco: o
// View::getDefaultFocus() devolve nullptr por padrão, e a Label não
// sobrescreve.
//
// Como toda linha de log é uma Label, o único item focável desta tela era o
// botão — que fica em cima do log. Resultado: a lista nunca descia, e com
// conversa privada removida do historico
// conversa privada removida do historico
// conversa privada removida do historico
//
// É o mesmo bug que já tinha comido a aba Sobre, e lá a saída foi um item
// focável por seção. Aqui não dava pra usar ListItem por linha: ele tem altura
// fixa e separador, e cem deles viram uma parede — justamente numa tela que ele
// já achou bagunçada. Uma Label que aceita foco deixa o texto igualzinho e faz
// o D-pad andar por ele, com o realce mostrando onde você está.
class LogLabel : public brls::Label
{
  public:
    LogLabel(brls::LabelStyle style, std::string text, bool multiline)
        : brls::Label(style, text, multiline)
    {
    }

    brls::View* getDefaultFocus() override { return this; }
};

} // namespace

JobPage::JobPage(Job* job, bool cancellable)
    : brls::AppletFrame(true, true)
    , job(job)
    , cancellable(cancellable)
{
    this->setTitle(job->getTitle());
    this->setFooterText(TR("Trabalhando...", "Working..."));

    this->list = new brls::List();

    // O espaçamento padrão da List é 61 px ENTRE CADA FILHO. Na tela de login
    // isso era o bug inteiro: a área útil é 475 px (720 de tela − 88 do
    // cabeçalho − 73 do rodapé − 42+42 de margem da lista), e cinco vãos de 61
    // comiam 305 px — mais espaço em branco do que o QR ocupa. O QR começava no
    // conversa privada removida do historico
    //). Com 10 px de vão o login inteiro fecha em 429 px e sobra
    // folga. De quebra, na sincronização cabe muito mais linha de log por tela.
    this->list->setSpacing(10);

    this->statusLabel = new brls::Label(brls::LabelStyle::DIALOG, TR("Começando...", "Starting..."), true);
    this->statusLabel->setHorizontalAlign(NVG_ALIGN_CENTER);
    this->list->addView(this->statusLabel);

    // Esse botão nasce junto com a tela, e não no fim do trabalho, por um
    // motivo que não é de estética: a borealis NÃO aguenta tela sem nada
    // focável. Label não recebe foco, então uma página só de texto deixa
    // Application::currentFocus nulo, e daí pra frente qualquer coisa que
    // mexa em foco (rolagem, navegação, dica de rodapé) trabalha em cima de
    // ponteiro nulo. Foi assim que o "Testar conexao" morreu,
    // e a tela de login é exatamente a mesma armadilha. Com um item focável
    // desde o primeiro quadro o problema deixa de existir por construção,
    // em vez de depender de eu tapar cada buraco um por um.
    this->backItem = new brls::ListItem(cancellable ? TR("Cancelar", "Cancel") : TR("Aguarde...", "Please wait..."));
    this->backItem->getClickEvent()->subscribe([this](brls::View* view) {
        this->onBackPressed();
    });
    this->list->addView(this->backItem);

    this->setContentView(this->list);

    this->job->start();
}

// O + mata o app na hora, e aqui isso é uma bomba.
//
// A borealis registra "+ = Application::quit()" em TODA view que entra na
// pilha (application.cpp, dentro do pushView). Numa tela de trabalho isso é
// derrubar o processo com a thread de sync no meio do caminho — e ela pode
// estar com um savedata MONTADO pra escrita. É o mesmo caminho que já corrompeu
// dois jogos de quem usa. "cliquei o botao + e crashou o switch"
//.
//
// Dá pra tomar a tecla de volta porque registerAction SUBSTITUI a ação da mesma
// tecla (view.cpp:359) e o willAppear roda DEPOIS do registro da borealis, no
// fim do pushView. Terminado o trabalho, o + volta a sair normalmente.
void JobPage::willAppear(bool resetState)
{
    brls::AppletFrame::willAppear(resetState);

    this->registerAction(TR("Sair", "Exit"), brls::Key::PLUS, [this] {
        if (this->job->isFinished())
        {
            brls::Application::quit();
            return true;
        }

        brls::Application::notify(TR("Espere terminar — sair agora pode corromper o save",
            "Wait for it to finish — leaving now can corrupt the save"));
        return true;
    });
}

// Um caminho só pro B e pro clique no botão — assim os dois nunca discordam.
void JobPage::onBackPressed()
{
    if (this->job->isFinished())
    {
        brls::Application::popView();
        return;
    }

    if (this->cancellable)
    {
        this->job->requestCancel();
        this->backItem->setLabel(TR("Cancelando...", "Cancelling..."));

        // conversa privada removida do historico
        // olhando pra tela grande, onde continuava escrito "Sincronizando
        // fulano..." — do lado de fora, cancelar não tinha acontecido. O rodapé
        // é o único texto que nada mais sobrescreve enquanto o trabalho roda.
        this->setFooterText(TR("Cancelando — parando assim que der",
            "Cancelling — stopping as soon as it can"));
        brls::Application::notify(TR("Cancelando...", "Cancelling..."));
    }
    else
    {
        brls::Application::notify(TR("Não dá pra sair no meio dessa operação",
            "You can't leave in the middle of this"));
    }
}

JobPage::~JobPage()
{
    // O destrutor do Job dá join na thread, então nunca fica thread solta
    // escrevendo em memória já liberada.
    Job::current = nullptr;
}

// Roda na thread da UI, uma vez por frame. Só lê o que a thread de trabalho
// deixou pronto — nada de rede aqui.
void JobPage::pump()
{
    std::string status = this->job->getStatus();
    if (!status.empty() && status != this->lastStatus)
    {
        this->lastStatus = status;
        this->statusLabel->setText(status);
        this->invalidate();
    }

    // Login: quando o Google devolve o código, troca o texto corrido por um
    // QR grande + o código em letra de cartaz. Só acontece uma vez.
    std::string url, code, urlWithCode;
    if (this->job->takeDeviceLogin(url, code, urlWithCode))
    {
        // Endereço e código numa linha só, ACIMA do QR. Em duas linhas
        // separadas eram 40 px de texto + 10 de vão a mais empurrando o QR pra
        // baixo, e o QR é justamente quem não tem onde encolher. Acima porque a
        // conversa privada removida do historico
        // conversa privada removida do historico
        brls::Label* infoLabel = new brls::Label(brls::LabelStyle::DIALOG,
            url + TR("   —   código: ", "   —   code: ") + code, true);
        infoLabel->setHorizontalAlign(NVG_ALIGN_CENTER);
        this->list->addView(infoLabel);
        this->loginViews.push_back(infoLabel);

        // 250 px é o teto real, não uma escolha de gosto: sobram 475 px de
        // altura útil, e status (40) + botão (69) + esta linha (40) + três vãos
        // de 10 já levam 179. Não custa nada em nitidez — o módulo do QR é
        // arredondado pra pixel inteiro, e tanto 250 quanto os 260 de antes
        // dão módulo de 6 px, ou seja, o MESMO desenho de 246 px. O que
        // mudava não era o tamanho do QR, era ele caber ou não.
        QrView* qr = new QrView(urlWithCode, 250);
        brls::View* qrView = qr;
        if (!qr->isValid())
        {
            delete qr;
            qrView = new brls::Label(brls::LabelStyle::SMALL,
                TR("(não consegui gerar o QR — use o endereço e o código acima)",
                    "(couldn't draw the QR — use the address and code above)"),
                true);
        }
        this->list->addView(qrView);
        this->loginViews.push_back(qrView);
    }

    for (const JobLine& line : this->job->takeNewLines())
    {
        LogLabel* label = new LogLabel(brls::LabelStyle::SMALL, line.text, true);
        if (line.error)
            label->setColor(nvgRGB(255, 92, 92));
        this->list->addView(label);
        this->lastLogLine = label;
    }

    if (this->job->isFinished() && !this->finishedHandled)
    {
        this->finishedHandled = true;

        bool success = this->job->succeeded();
        this->setFooterText(success ? TR("Concluído", "Done") : TR("Falhou", "Failed"));

        // Some com o endereço, o código e o QR. Sem isso, acabar o login não
        // mudava nada de visível — o QR continuava ocupando a tela inteira, e
        // ele leu isso como login quebrado.
        //
        // collapse(), e não hide(): as duas somem da tela, mas o BoxLayout só
        // pula filho ESCONDIDO na conta da altura total — ele continua somando
        // a altura dele no avanço vertical (box_layout.cpp, `yAdvance +=`).
        // Ou seja, hide() deixaria um buraco invisível de 250 px empurrando
        // todo o log pra baixo, dentro de um container que já não conta esse
        // espaço. collapse() zera a altura de verdade, e o vão junto.
        // Remover era a terceira opção e está fora: o header da borealis avisa
        // que removeView provavelmente corrompe memória.
        for (brls::View* v : this->loginViews)
            v->collapse(false);
        this->loginViews.clear();

        if (!success)
            this->statusLabel->setColor(nvgRGB(255, 92, 92));

        this->backItem->setLabel(TR("Voltar", "Back"));

        // Conflito de save: o trabalho parou porque só ele pode decidir. As
        // saídas viram linha clicável aqui mesmo, logo abaixo dos números dos
        // dois lados. Antes a mensagem mandava ele sair da tela, achar o jogo
        // e apertar Y — três passos depois de uma decisão que ele já tinha
        // tomado antes de ler a frase até o fim.
        // Sem escolha pra fazer, o foco vai pro fim do log em vez do botão: é
        // onde está o resumo, e daí se lê pra cima. O B continua saindo da tela
        // de qualquer lugar, então nada fica preso.
        brls::View* focusOn = this->lastLogLine ? this->lastLogLine : (brls::View*)this->backItem;
        bool temEscolha = false;

        for (const JobChoice& choice : this->job->takeChoices())
        {
            brls::ListItem* item = new brls::ListItem(choice.label, choice.description);
            std::function<void()> action = choice.action;
            item->getClickEvent()->subscribe([action](brls::View* view) { action(); });
            this->list->addView(item);

            if (!temEscolha)
            {
                temEscolha = true;
                focusOn    = item; // tendo escolha, é ela que recebe o foco
            }
        }

        this->invalidate();
        brls::Application::giveFocus(focusOn);

        if (this->onFinished)
            this->onFinished(success);
    }
}

void JobPage::draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height,
    brls::Style* style, brls::FrameContext* ctx)
{
    // Importante: mexer na lista ANTES de deixar o AppletFrame desenhar os
    // filhos. Adicionar view no meio da travessia invalidaria o iterador.
    this->pump();

    brls::AppletFrame::draw(vg, x, y, width, height, style, ctx);
}

bool JobPage::onCancel()
{
    this->onBackPressed();
    return true; // consome o B: quem tira a tela é o onBackPressed
}
