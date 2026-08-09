#include "qr_view.hpp"

extern "C" {
#include "qrcodegen.h"
}

// Versão 10 dá folga de sobra pra uma URL do Google com o código junto
// (~60 caracteres), e mantém o QR grosso o suficiente pra câmera de celular
// pegar de longe na TV.
#define QR_MAX_VERSION 10

QrView::QrView(const std::string& text, unsigned sizePx)
    : sizePx(sizePx)
{
    this->qrcode.resize(qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION));
    std::vector<uint8_t> temp(qrcodegen_BUFFER_LEN_FOR_VERSION(QR_MAX_VERSION));

    this->valid = qrcodegen_encodeText(text.c_str(), temp.data(), this->qrcode.data(),
        qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN, QR_MAX_VERSION,
        qrcodegen_Mask_AUTO, true);

    if (this->valid)
        this->moduleCount = qrcodegen_getSize(this->qrcode.data());

    this->setWidth(sizePx);
    this->setHeight(sizePx);
}

void QrView::layout(NVGcontext* vg, brls::Style* style, brls::FontStash* stash)
{
    this->setWidth(this->sizePx);
    this->setHeight(this->sizePx);
}

void QrView::draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height,
    brls::Style* style, brls::FrameContext* ctx)
{
    if (!this->valid || this->moduleCount <= 0)
        return;

    // Margem branca (quiet zone) obrigatória: sem ela muito leitor não
    // reconhece o código. O padrão pede 4 módulos de cada lado.
    const int quiet = 4;
    const int total = this->moduleCount + quiet * 2;

    // Tamanho do módulo em pixels inteiros — se deixar fracionário, as
    // bordas ficam borradas e a leitura piora.
    int module = (int)(this->sizePx / total);
    if (module < 1)
        module = 1;

    int drawn  = module * total;
    int startX = x + ((int)width - drawn) / 2;
    int startY = y + ((int)height - drawn) / 2;

    // Fundo branco cobrindo inclusive a quiet zone. O QR tem que ser
    // escuro-sobre-claro mesmo no tema escuro do console: invertido, boa
    // parte dos leitores não pega.
    nvgFillColor(vg, nvgRGB(255, 255, 255));
    nvgBeginPath(vg);
    nvgRect(vg, startX, startY, drawn, drawn);
    nvgFill(vg);

    nvgFillColor(vg, nvgRGB(0, 0, 0));
    for (int row = 0; row < this->moduleCount; row++)
    {
        for (int col = 0; col < this->moduleCount; col++)
        {
            if (!qrcodegen_getModule(this->qrcode.data(), col, row))
                continue;

            nvgBeginPath(vg);
            nvgRect(vg,
                startX + (col + quiet) * module,
                startY + (row + quiet) * module,
                module, module);
            nvgFill(vg);
        }
    }
}
