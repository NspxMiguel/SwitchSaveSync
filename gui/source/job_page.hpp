// job_page.hpp — a tela que aparece enquanto um Job roda: status grande no
// topo e o log rolando embaixo (cada arquivo enviado/baixado vira linha).
#pragma once

#include <borealis.hpp>

#include <memory>
#include <vector>

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

    // Só existe pra tomar o + de volta da borealis. Ver o corpo dela.
    void willAppear(bool resetState = false) override;

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

    // QR, código e link. Ficam guardados porque precisam SUMIR quando o login
    // termina: enquanto eles continuavam na tela, acabar o login não mudava
    // nada visível — a tela ficava idêntica, e o login parecia quebrado.
    std::vector<brls::View*> loginViews;

    // A última linha de log escrita. Guardada pra receber o foco quando o
    // trabalho acaba: numa sincronização de trinta jogos, o que interessa está
    // no FIM (o resumo do que ficou de fora), e começar o foco no topo obrigaria
    // a segurar o D-pad por trinta linhas pra chegar lá.
    brls::View* lastLogLine = nullptr;

    std::function<void(bool success)> onFinished;

    void pump();
    void onBackPressed();
};
