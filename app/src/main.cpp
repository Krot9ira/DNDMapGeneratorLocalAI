#include "app_state.h"
#include "core/window.h"
#include "core/dx11_renderer.h"
#include "ui/ui_context.h"
#include "ui/texture_loader.h"
#include "ui/app_ui.h"
#include "../include/ideogram_caption.h"
#include "../include/map_serializer.h"
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <vector>
#include <string>

namespace fs = std::filesystem;

static void ResolveProjectDirectory() {
    wchar_t exePath[MAX_PATH];
    if (GetModuleFileNameW(nullptr, exePath, MAX_PATH)) {
        fs::path dir = fs::path(exePath).parent_path();
        fs::path root = dir;
        for (int up = 0; up < 5; ++up) {
            if (fs::exists(dir / "styles" / "_base.json")) { root = dir; break; }
            if (!dir.has_parent_path() || dir.parent_path() == dir) break;
            dir = dir.parent_path();
        }
        SetCurrentDirectoryW(root.c_str());
    }
}

static int RunCaptionDump(const std::vector<std::string>& args) {
    if (args.size() < 2) return 2;
    dnd::MapData map;
    if (!dnd::MapSerializer::LoadFromFile(args[1], map)) return 1;
    dnd::StyleManager styles;
    styles.stylesDir = "styles";
    styles.LoadAll();
    std::string caption = dnd::IdeogramCaption::BuildJson(
        map, styles.Find(map.meta.style), styles.base, styles.phrases);
    std::string out = args.size() >= 3 ? args[2] : (args[1] + ".caption.json");
    std::ofstream f(out, std::ios::binary);
    if (!f.is_open()) return 1;
    f << caption;
    return 0;
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    ResolveProjectDirectory();

    if (__argc >= 3 && std::string(__argv[1]) == "--caption") {
        std::vector<std::string> args;
        for (int i = 1; i < __argc; ++i) args.push_back(__argv[i]);
        return RunCaptionDump(args);
    }

    dnd::Window window(hInstance, L"D&D Battle Map Generator", 1560, 960);
    if (!window.GetHwnd()) return 1;

    dnd::DX11Renderer renderer;
    if (!renderer.Init(window.GetHwnd())) return 1;

    window.SetResizeCallback([&](UINT w, UINT h) {
        renderer.Resize(w, h);
    });

    window.Show(nCmdShow);

    dnd::SetupFonts();
    dnd::SetupDarkFantasyTheme();

    dnd::AppState app;
    dnd::TextureLoader texLoader(renderer.GetDevice(), renderer.GetContext());
    dnd::AppUI::Init(app, texLoader);

    while (window.ProcessMessages()) {
        renderer.BeginFrame();
        dnd::AppUI::Render(app, texLoader);
        renderer.EndFrame();
    }

    app.job.cancel = true;
    for (int i = 0; i < 40 && app.job.running.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

    renderer.Shutdown();
    return 0;
}
