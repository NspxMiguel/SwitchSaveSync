// qr_view.hpp — desenha um QR code direto com o nanovg, sem gerar imagem.
//
// Cada módulo do QR vira um retângulo. É bobo e é rápido: um QR de versão
// pequena tem ~25x25 módulos, ou seja algumas centenas de retângulos por
// frame, nada perto de pesar.
#pragma once

#include <borealis.hpp>

#include <string>
#include <vector>

class QrView : public brls::View
{
  public:
    // text é o que vai dentro do QR (aqui, a URL de login com o código já
    // embutido). Se não couber num QR, isValid() vira false e a view só
    // desenha um aviso — nunca deixa de desenhar alguma coisa.
    QrView(const std::string& text, unsigned sizePx);

    void draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height,
        brls::Style* style, brls::FrameContext* ctx) override;

    void layout(NVGcontext* vg, brls::Style* style, brls::FontStash* stash) override;

    bool isValid() const { return this->valid; }

  private:
    std::vector<uint8_t> qrcode;
    int moduleCount = 0;
    unsigned sizePx = 0;
    bool valid      = false;
};
