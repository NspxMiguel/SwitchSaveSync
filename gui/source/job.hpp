// job.hpp — trabalho pesado (rede, leitura/escrita de save) fora da thread
// da interface.
//
// Por que isso existe: as funções de ../core são bloqueantes — um upload
// de save pode levar dezenas de segundos. Se rodassem na thread da UI, a
// tela congelaria inteira (sem animação, sem hint, sem nada) e o console
// pareceria travado. Então elas rodam numa thread separada e conversam com
// a UI por aqui: a thread de trabalho só escreve status/linhas de log, e a
// thread da UI só lê, sempre com o mutex.
#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

struct JobLine
{
    std::string text;
    bool error;
};

// Uma saída possível pra um trabalho que parou porque só ele pode decidir.
// Hoje só o conflito de save usa: os dois lados andaram, e escolher sozinho
// apagaria progresso de um deles.
struct JobChoice
{
    std::string label;
    std::string description;
    std::function<void()> action; // roda na thread da UI, no clique
};

class Job
{
  public:
    // Devolve true se o trabalho terminou bem. Roda na thread de trabalho.
    using Work = std::function<bool(Job*)>;

    Job(const std::string& title, Work work);
    ~Job();

    void start();

    // ---- chamados de dentro da thread de trabalho ----
    void setStatus(const std::string& status);
    void log(const std::string& line, bool error = false);
    bool cancelRequested() const { return this->cancel.load(); }

    // ---- chamados da thread da UI ----
    const std::string& getTitle() const { return this->title; }
    bool isFinished() const { return this->finished.load(); }
    bool succeeded() const { return this->ok.load(); }
    void requestCancel() { this->cancel.store(true); }

    std::string getStatus();
    // Devolve as linhas novas desde a última chamada (e esvazia a fila).
    std::vector<JobLine> takeNewLines();

    // Login: a thread de trabalho deposita aqui a URL e o código que o
    // Google devolveu, e a thread da UI pega uma vez só pra montar o QR.
    void setDeviceLogin(const std::string& url, const std::string& code,
        const std::string& urlWithCode);
    // Devolve true (uma única vez) quando há um login novo pra desenhar.
    bool takeDeviceLogin(std::string& url, std::string& code, std::string& urlWithCode);

    // Conflito de save: a thread de trabalho deixa aqui as saídas possíveis e
    // a JobPage desenha uma linha clicável pra cada, na mesma tela. Antes a
    // mensagem mandava ele sair, achar o jogo na lista e apertar Y — três
    // passos pra uma decisão que já estava tomada na cabeça dele.
    void offerChoice(const std::string& label, const std::string& description,
        std::function<void()> action);
    std::vector<JobChoice> takeChoices();

    // A ponte pros callbacks em C do ../core (drive_set_progress_cb e os
    // callbacks do oauth), que são ponteiros de função sem userdata e por
    // isso precisam achar o job em andamento por uma global. Só existe um
    // job por vez — a UI não deixa abrir dois.
    static Job* current;

  private:
    std::string title;
    Work work;

    mutable std::mutex mutex;
    std::string status;
    std::vector<JobLine> pending;

    std::string deviceUrl, deviceCode, deviceUrlWithCode;
    bool devicePending = false;

    std::vector<JobChoice> choices;

    std::atomic<bool> finished { false };
    std::atomic<bool> ok { false };
    std::atomic<bool> cancel { false };

    std::thread thread;
};
