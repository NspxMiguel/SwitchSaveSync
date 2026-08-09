#include "job_page.hpp"

#include "qr_view.hpp"

JobPage::JobPage(Job* job, bool cancellable)
    : brls::AppletFrame(true, true)
    , job(job)
    , cancellable(cancellable)
{
    this->setTitle(job->getTitle());
    this->setFooterText("Trabalhando...");

    this->list = new brls::List();

    this->statusLabel = new brls::Label(brls::LabelStyle::DIALOG, "Comecando...", true);
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
    this->backItem = new brls::ListItem(cancellable ? "Cancelar" : "Aguarde...");
    this->backItem->getClickEvent()->subscribe([this](brls::View* view) {
        this->onBackPressed();
    });
    this->list->addView(this->backItem);

    this->setContentView(this->list);

    this->job->start();
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
        this->backItem->setLabel("Cancelando...");
        brls::Application::notify("Cancelando...");
    }
    else
    {
        brls::Application::notify("Nao da pra sair no meio dessa operacao");
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
        // 260 px, e não 340: a tela útil entre o cabeçalho e o rodapé é de
        // ~560 px, e o QR dividia esse espaço com o status, o botão, o código
        // e a explicação — sobrava QR cortado pela metade. Nesse tamanho a
        // câmera de celular ainda lê de longe, na TV.
        // O link e o código entram ANTES do QR de propósito. Na ordem antiga
        // eles vinham depois, e como a lista rola, o que cabia na tela era só
        // conversa privada removida do historico
        // câmera à mão aponta pro QR, e quem não tem lê e digita, sem rolar.
        brls::Label* linkLabel = new brls::Label(brls::LabelStyle::DIALOG, url, true);
        linkLabel->setHorizontalAlign(NVG_ALIGN_CENTER);
        this->list->addView(linkLabel);

        brls::Label* codeLabel = new brls::Label(brls::LabelStyle::DIALOG,
            "Codigo: " + code, true);
        codeLabel->setHorizontalAlign(NVG_ALIGN_CENTER);
        this->list->addView(codeLabel);

        this->loginViews.push_back(linkLabel);
        this->loginViews.push_back(codeLabel);

        // 260 px, e não 340: a tela útil entre o cabeçalho e o rodapé é de
        // ~560 px, e o QR dividia esse espaço com o status, o botão, o código
        // e a explicação — sobrava QR cortado pela metade. Nesse tamanho a
        // câmera de celular ainda lê de longe, na TV.
        QrView* qr = new QrView(urlWithCode, 260);
        brls::View* qrView = qr;
        if (!qr->isValid())
        {
            delete qr;
            qrView = new brls::Label(brls::LabelStyle::SMALL,
                "(nao consegui gerar o QR — use o endereco e o codigo acima)", true);
        }
        this->list->addView(qrView);
        this->loginViews.push_back(qrView);

        brls::View* helpLabel = new brls::Label(brls::LabelStyle::DESCRIPTION,
            "Abra o endereco acima e digite o codigo, ou aponte a camera pro QR "
            "(ja vai com o codigo preenchido). O Switch nao pede senha.", true);
        this->list->addView(helpLabel);
        this->loginViews.push_back(helpLabel);
    }

    for (const JobLine& line : this->job->takeNewLines())
    {
        brls::Label* label = new brls::Label(brls::LabelStyle::SMALL, line.text, true);
        if (line.error)
            label->setColor(nvgRGB(255, 92, 92));
        this->list->addView(label);
    }

    if (this->job->isFinished() && !this->finishedHandled)
    {
        this->finishedHandled = true;

        bool success = this->job->succeeded();
        this->setFooterText(success ? "Concluido" : "Falhou");

        // Some com o link, o código e o QR. Sem isso, acabar o login não mudava
        // nada de visível — o QR continuava ocupando a tela inteira, e ele leu
        // isso como login quebrado. Escondo em vez de remover porque remover
        // view na borealis vem com aviso de corrupção de memória no header
        // dela; e o layout já pula filho escondido, então some do mesmo jeito.
        for (brls::View* v : this->loginViews)
            v->hide([] {}, false);
        this->loginViews.clear();
        this->invalidate();

        if (!success)
            this->statusLabel->setColor(nvgRGB(255, 92, 92));

        this->backItem->setLabel("Voltar");
        brls::Application::giveFocus(this->backItem);

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
