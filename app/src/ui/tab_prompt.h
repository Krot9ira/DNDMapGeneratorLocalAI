#pragma once

#include "../app_state.h"
#include "texture_loader.h"

namespace dnd {

class TabPrompt {
public:
    static void Draw(AppState& app, TextureLoader& texLoader);
};

} // namespace dnd
