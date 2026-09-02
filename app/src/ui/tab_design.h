#pragma once

#include "../app_state.h"

namespace dnd {

class TabDesign {
public:
    static void Draw(AppState& app);
    static void DrawPropPicker(AppState& app);
    static void DrawEffectPicker(AppState& app);
};

} // namespace dnd
