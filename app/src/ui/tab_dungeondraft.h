#pragma once

#include "../app_state.h"
#include <string>

namespace dnd {

class TabDungeondraft {
public:
    static void Draw(AppState& app);
    static void RefreshDungeondraftStatsAsync(AppState& app);
    static void StartDungeondraftExport(AppState& app);
    static void StartDungeondraftScan(AppState& app);
    static void StartDungeondraftEnrich(AppState& app, const std::string& scope = "all");
    static void StartDungeondraftQualityCheck(AppState& app);
    static void StartDungeondraftValidate(AppState& app);
    static void StartDungeondraftFoundrySatisfy(AppState& app, const std::string& exportPath);
};

} // namespace dnd
