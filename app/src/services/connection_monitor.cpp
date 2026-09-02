#include "connection_monitor.h"
#include "../core/process_runner.h"
#include "../../include/ollama_service.h"
#include "../../include/comfy_service.h"
#include <thread>

namespace dnd {

void ConnectionMonitor::CheckServicesAsync(AppState& app) {
    if (!app.BeginJob("Checking services...")) return;
    OllamaConfig ocfg = app.config.ollama;
    ComfyConfig ccfg = app.config.comfy;

    std::thread([&app, ocfg, ccfg]() {
        std::vector<std::string> models;
        std::string err;
        if (OllamaService::CheckConnection(ocfg.base_url, models, err)) {
            app.ollamaOk = true;
            app.ollamaModels = models;
            std::string list;
            for (const auto& m : models) list += (list.empty() ? "" : ", ") + m;
            app.ollamaStatus = "ready (" + list + ")";
            app.job.Log("Ollama OK: " + list);
            bool found = false;
            for (const auto& m : models) if (m == ocfg.model) found = true;
            if (!found)
                app.job.Log("Warning: model '" + ocfg.model + "' is not in that list.");
        } else {
            app.ollamaOk = false;
            app.ollamaStatus = "unreachable (" + err + ")";
            app.job.Log("Ollama unreachable: " + err);
        }

        std::string version;
        if (ComfyService::CheckConnection(ccfg.base_url, version, err)) {
            app.comfyOk = true;
            app.comfyStatus = version;
            app.job.Log("ComfyUI OK: " + version);
        } else {
            app.comfyOk = false;
            app.comfyStatus = "unreachable (" + err + ")";
            app.job.Log("ComfyUI unreachable: " + err);
        }

        app.FinishJob(true, "Service check complete.");
    }).detach();
}

void ConnectionMonitor::PullOllamaModelAsync(AppState& app, const std::string& modelName) {
    if (modelName.empty()) return;
    std::string jobName = "Pulling model " + modelName + " into Ollama...";
    if (!app.BeginJob(jobName.c_str())) return;

    std::thread([&app, modelName]() {
        std::string cmd = "python -u tools/ollama_client.py pull \"" + modelName + "\"";
        app.job.Log("Pulling model from Ollama registry: " + cmd);
        bool ok = ProcessRunner::RunCapture(cmd, "", [&](const std::string& line) {
            app.job.Log(line);
        }, &app.job.cancel);
        CheckServicesAsync(app);
        app.FinishJob(ok, ok ? ("Successfully pulled " + modelName + "!") : ("Failed to pull " + modelName + " - see log."));
    }).detach();
}

} // namespace dnd
