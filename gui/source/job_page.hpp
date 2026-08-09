// job_page.hpp — a tela que aparece enquanto um Job roda: status grande no
// topo e o log rolando embaixo (cada arquivo enviado/baixado vira linha).
#pragma once

#include <borealis.hpp>

#include <memory>

#include "job.hpp"

class JobPage : public brls::AppletFrame
{
  public:
    // cancellable=true deixa o B abortar (só faz sentido no login, onde
    // abortar não deixa nada pela metade). No backup/restore fica false: no
    // meio de uma gravação de save, sair na marra é como arrancar o cartão.
    JobPage(Job* job, bool cancellable);
    ~JobPage();

    void draw(NVGcontext* vg, int x, int y, unsigned width, unsigned height,
        brls::Style* style, brls::FrameContext* ctx) override;

    bool onCancel() override;

    // Chamado quando o job termina bem — usado pra atualizar a tela de
    // trás (ex: o item "Conta Google" depois do login).
    void setOnFinished(std::function<void(bool success)> cb) { this->onFinished = cb; }

  private:
    std::unique_ptr<Job> job;
    bool cancellable;
    bool finishedHandled = false;

    brls::List* list          = nullptr;
    brls::Label* statusLabel  = nullptr;
    brls::ListItem* backItem  = nullptr;
    std::string lastStatus;

    std::function<void(bool success)> onFinished;

    void pump();
    void onBackPressed();
};
