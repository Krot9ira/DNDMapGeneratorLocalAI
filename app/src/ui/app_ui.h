#pragma once

#include "../app_state.h"
#include "texture_loader.h"

namespace dnd {

class AppUI {
public:
    static void Init(AppState& app, TextureLoader& texLoader);
    static void Render(AppState& app, TextureLoader& texLoader);
    static void DrainJobResults(AppState& app, TextureLoader& texLoader);

private:
    static void DrawMainMenu(AppState& app, TextureLoader& texLoader);
    static void DrawOpenDialog(AppState& app, TextureLoader& texLoader);
    static void DrawStatusBar(AppState& app);
    static void SaveCurrentMap(AppState& app);
};

} // namespace dnd
