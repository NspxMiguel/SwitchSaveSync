#include "ota_update.hpp"
#include "job_page.hpp"
extern "C"
{
#include "http.h"
#include "lang.h"
#include "ota.h"
}
#include <atomic>
#include <cstdio>
#include <ctime>
#include <memory>
#include <switch.h>
#include <sys/stat.h>

namespace
{
constexpr const char* checkPath = "sdmc:/switch/SwitchSaveSync/update-check.txt";
OtaFile pendingFiles[OTA_ASSET_COUNT];
std::atomic<bool> pending { false };
Job* activeJob = nullptr;
constexpr const char* errorPath = "sdmc:/switch/SwitchSaveSync/update-error.txt";

bool moduleStopped()
{
    if (R_FAILED(pmdmntInitialize()))
        return false;
    u64 pid = 0;
    Result result = pmdmntGetProcessId(&pid, OTA_MODULE_ID);
    pmdmntExit();
    // Atmosphere pm::ResultProcessNotFound (module 15, description 1).
    return result == MAKERESULT(15, 1);
}
bool downloadProgress(unsigned asset, uint64_t received, uint64_t total, void* user)
{
    Job* job = static_cast<Job*>(user);
    job->setStatus(std::string(TR("Baixando ", "Downloading ")) + ota_asset_names[asset]);
    if (total)
        job->setProgress(static_cast<int>(received * 100 / total));
    else
        job->setProgress(-1);
    return !job->cancelRequested();
}
void startUpdate(OtaRelease release, std::string path)
{
    if (pending || !moduleStopped())
    {
        brls::Application::notify(TR("Pare o SwitchSaveSync no Ultrahand antes de atualizar. Se já "
                                     "parou, reinicie o console.",
            "Stop SwitchSaveSync in Ultrahand before updating. If already stopped, reboot the "
            "console."));
        return;
    }
    JobPage* page = new JobPage(
        new Job(TR("Atualizando", "Updating"),
            [release, path](Job* job)
            {
                if (!ota_plan(pendingFiles, path.c_str()))
                {
                    job->setStatus(TR("Abra o app pelo arquivo .nro no cartão para atualizar.",
                        "Launch the .nro from the SD card to update."));
                    return false;
                }
                if (!ota_stage(&release, pendingFiles, downloadProgress, job))
                {
                    job->setStatus(TR("Download ou SHA-256 falhou. Tente novamente; os arquivos "
                                      "instalados foram preservados.",
                        "Download or SHA-256 failed. Try again; installed files were preserved."));
                    return false;
                }
                pending = true;
                job->setProgress(100);
                job->setStatus(TR("Verificado. A atualização será instalada ao sair do app.",
                    "Verified. The update will be installed when you exit the app."));
                return true;
            }),
        true);
    activeJob = Job::current;
    page->setOnFinished(
        [path](bool success)
        {
            activeJob = nullptr;
            if (!success)
                return;
            auto* dialog
                = new brls::Dialog(TR("Os quatro arquivos foram verificados. Reiniciar agora para "
                                      "instalar? Mantenha o sysmodule parado até terminar.",
                    "All four files were verified. Restart now to install? Keep the sysmodule "
                    "stopped until installation finishes."));
            dialog->addButton(TR("Depois", "Later"), [dialog](brls::View*) { dialog->close(); });
            dialog->addButton(TR("Reiniciar agora", "Restart now"),
                [dialog, path](brls::View*)
                {
                    dialog->close(
                        [path]
                        {
                            if (envHasNextLoad())
                                envSetNextLoad(path.c_str(), path.c_str());
                            brls::Application::quit();
                        });
                });
            dialog->open();
        });
    brls::Application::pushView(page);
}
}

void checkForUpdates(const std::string& path, bool automatic)
{
    if (automatic)
    {
        FILE* error = fopen(errorPath, "r");
        if (error)
        {
            fclose(error);
            remove(errorPath);
            brls::Application::notify(TR("A instalação da atualização falhou. Confira os arquivos "
                                         ".bak/.ota no cartão antes de tentar novamente.",
                "Update installation failed. Inspect .bak/.ota files on the SD card before trying "
                "again."));
        }
    }
    if (pending)
    {
        if (!automatic)
            brls::Application::notify(TR("Saia do app para instalar a atualização já baixada.",
                "Exit the app to install the downloaded update."));
        return;
    }
    if (!http_is_ready())
    {
        if (!automatic)
            brls::Application::notify(
                TR("Confira a conexão e tente novamente.", "Check your connection and try again."));
        return;
    }
    int64_t now = static_cast<int64_t>(time(nullptr));
    mkdir("sdmc:/switch/SwitchSaveSync", 0777);
    if (!ota_check_due(checkPath, now) || !ota_record_check(checkPath, now))
    {
        if (!automatic)
            brls::Application::notify(
                TR("Consulta limitada a uma vez por dia. Confira a data do console e o cartão.",
                    "Checks are limited to once a day. Check the console date and SD card."));
        return;
    }
    auto release = std::make_shared<OtaRelease>();
    auto available = std::make_shared<bool>(false);
    auto* page = new JobPage(
        new Job(TR("Verificar atualizações", "Check for updates"),
            [release, available](Job* job)
            {
                job->setStatus(
                    TR("Consultando a versão mais recente...", "Checking the latest release..."));
                int order = 0;
                if (!ota_fetch_release(release.get())
                    || !ota_version_compare(release->version, APP_VERSION_STRING, &order))
                {
                    job->setStatus(TR("Não foi possível consultar uma release completa com "
                                      "SHA256SUMS. Tente amanhã.",
                        "Could not check a complete release with SHA256SUMS. Try tomorrow."));
                    return false;
                }
                *available = order > 0;
                job->setStatus(*available
                        ? TR("Atualização disponível.", "Update available.")
                        : TR("Você está na versão mais recente.", "You're up to date."));
                return true;
            }),
        false);
    activeJob = Job::current;
    page->setOnFinished(
        [release, available, path](bool success)
        {
            activeJob = nullptr;
            if (!success || !*available)
                return;
            std::string message = std::string(TR("Nova versão: ", "New version: "))
                + release->version + "\n\n" + release->notes;
            auto* dialog = new brls::Dialog(message);
            dialog->addButton(TR("Depois", "Later"), [dialog](brls::View*) { dialog->close(); });
            dialog->addButton(TR("Atualizar agora", "Update now"),
                [dialog, release, path](brls::View*)
                { dialog->close([release, path] { startUpdate(*release, path); }); });
            dialog->setCancelable(true);
            dialog->open();
        });
    brls::Application::pushView(page);
}

bool finishOtaUpdate()
{
    // A system-requested exit can bypass the JobPage buttons. Join before HTTP shutdown.
    if (activeJob)
    {
        activeJob->requestCancel();
        activeJob->waitForCompletion();
        activeJob = nullptr;
    }
    if (!pending)
        return true;
    bool ok = moduleStopped() && ota_apply(pendingFiles, OTA_ASSET_COUNT);
    if (!ok)
    {
        // Cancel an already queued restart if installation failed.
        if (envHasNextLoad())
            envSetNextLoad("", "");
        FILE* error = fopen(errorPath, "w");
        if (error)
        {
            fputs("OTA replacement failed\n", error);
            fclose(error);
        }
        brls::Logger::error("OTA replacement failed; inspect .bak and .ota files on SD card");
    }
    return ok;
}
