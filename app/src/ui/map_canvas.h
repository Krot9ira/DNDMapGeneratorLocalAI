#pragma once

#include "../app_state.h"
#include <imgui.h>

namespace dnd {

class MapCanvas {
public:
    static void Draw(AppState& app, bool interactive, ImVec2 size = ImVec2(0, 0));
};

} // namespace dnd
