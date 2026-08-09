#include "job.hpp"

Job* Job::current = nullptr;

Job::Job(const std::string& title, Work work)
    : title(title)
    , work(work)
{
}

Job::~Job()
{
    // Nunca destruir um job com a thread rodando: ela ainda escreve nos
    // membros daqui. A UI só apaga o job depois de isFinished(), mas o
    // join() aqui é a garantia final.
    if (this->thread.joinable())
        this->thread.join();
}

void Job::start()
{
    Job::current = this;

    this->thread = std::thread([this]() {
        bool result = false;

        if (this->work)
            result = this->work(this);

        this->ok.store(result);
        this->finished.store(true);
    });
}

void Job::setStatus(const std::string& status)
{
    std::lock_guard<std::mutex> guard(this->mutex);
    this->status = status;
}

void Job::log(const std::string& line, bool error)
{
    std::lock_guard<std::mutex> guard(this->mutex);
    this->pending.push_back({ line, error });
}

std::string Job::getStatus()
{
    std::lock_guard<std::mutex> guard(this->mutex);
    return this->status;
}

void Job::setDeviceLogin(const std::string& url, const std::string& code,
    const std::string& urlWithCode)
{
    std::lock_guard<std::mutex> guard(this->mutex);
    this->deviceUrl         = url;
    this->deviceCode        = code;
    this->deviceUrlWithCode = urlWithCode;
    this->devicePending     = true;
}

bool Job::takeDeviceLogin(std::string& url, std::string& code, std::string& urlWithCode)
{
    std::lock_guard<std::mutex> guard(this->mutex);
    if (!this->devicePending)
        return false;

    url                 = this->deviceUrl;
    code                = this->deviceCode;
    urlWithCode         = this->deviceUrlWithCode;
    this->devicePending = false;
    return true;
}

void Job::offerChoice(const std::string& label, const std::string& description,
    std::function<void()> action)
{
    std::lock_guard<std::mutex> guard(this->mutex);
    this->choices.push_back({ label, description, action });
}

std::vector<JobChoice> Job::takeChoices()
{
    std::lock_guard<std::mutex> guard(this->mutex);
    std::vector<JobChoice> out;
    out.swap(this->choices);
    return out;
}

std::vector<JobLine> Job::takeNewLines()
{
    std::lock_guard<std::mutex> guard(this->mutex);
    std::vector<JobLine> out;
    out.swap(this->pending);
    return out;
}
