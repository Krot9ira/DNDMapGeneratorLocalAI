#pragma once

#include "../app_state.h"
#include "texture_loader.h"

namespace dnd {

class TabPaint {
public:
    static void Draw(AppState& app, TextureLoader& texLoader);
    static void DrawCaptionPanel(AppState& app);
};

} // namespace dnd
