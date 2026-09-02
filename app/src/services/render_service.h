#pragma once

#include "../app_state.h"
#include <string>

namespace dnd {

enum class Rebuild { None, Blueprint, Plan, PlanAndRender };

class RenderService {
public:
    static bool RunRender(AppState& app, MapData map);
    static void StartRenderCurrent(AppState& app);
    static void StartQuickBlueprint(AppState& app);
    static void StartPlanAndRender(AppState& app, bool alsoRender);
    static bool SaveCurrentPlan(AppState& app, std::string* outPath = nullptr);

    static void RequestRebuild(AppState& app, Rebuild what);
    static void RunRebuild(AppState& app, Rebuild what);
    static void DrawRebuildGuard(AppState& app);

    static Rebuild s_pendingRebuild;
};

} // namespace dnd
