#pragma once

#include "../app_state.h"
#include <string>

namespace dnd {

class ConnectionMonitor {
public:
    static void CheckServicesAsync(AppState& app);
    static void StartConnectionCheck(AppState& app) { CheckServicesAsync(app); }

    static void PullOllamaModelAsync(AppState& app, const std::string& modelName);
    static void StartOllamaPull(AppState& app, const std::string& modelName) { PullOllamaModelAsync(app, modelName); }
};

} // namespace dnd
