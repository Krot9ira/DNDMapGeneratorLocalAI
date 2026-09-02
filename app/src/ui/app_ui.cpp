#include "app_ui.h"
#include "ui_context.h"
#include "tab_prompt.h"
#include "tab_design.h"
#include "tab_paint.h"
#include "tab_dungeondraft.h"
#include "tab_gallery.h"
#include "tab_settings.h"
#include "tab_docs.h"
#include "../core/file_dialogs.h"
#include "../services/render_service.h"
#include "../services/connection_monitor.h"
#include "../../include/map_rasterizer.h"
#include "../../include/map_serializer.h"
#include <filesystem>
#include <algorithm>
#include <vector>

namespace fs = std::filesystem;

namespace dnd {

struct MapEntry {
    std::string path;
    std::string folder;
    std::string title;
    int cols = 0, rows = 0;
    ID3D11ShaderResourceView* thumb = nullptr;
    int thumbW = 0, thumbH = 0;
};

static std::vector<MapEntry> s_mapEntries;

static void ReleaseMapEntries() {
    for (auto& e : s_mapEntries) {
        if (e.thumb) e.thumb->Release();
    }
    s_mapEntries.clear();
}

static void ScanMaps(const AppState& app, TextureLoader& texLoader) {
    ReleaseMapEntries();
    std::error_code ec;
    if (!fs::exists(app.config.output_dir, ec)) return;
    for (const auto& dir : fs::directory_iterator(app.config.output_dir, ec)) {
        if (!dir.is_directory()) continue;
        fs::path candidate = dir.path() / "map.json";
        if (!fs::exists(candidate)) continue;

        MapEntry e;
        e.path = candidate.string();
        e.folder = dir.path().filename().string();
        MapData peek;
        if (!MapSerializer::LoadFromFile(e.path, peek)) continue;
        e.title = peek.meta.title;
        e.cols = peek.grid.cols;
        e.rows = peek.grid.rows;

        int cell = std::max(2, 132 / std::max(1, e.cols));
        ImageBuffer img = MapRasterizer::RenderPreview(peek, cell);
        e.thumb = texLoader.CreateTextureRGBA(img.pixels.data(), img.width, img.height);
        e.thumbW = img.width;
        e.thumbH = img.height;
        s_mapEntries.push_back(e);
    }
}

static std::string TidyMapPath(std::string p) {
    while (!p.empty() && (isspace((unsigned char)p.front()) || p.front() == '"'))
        p.erase(p.begin());
    while (!p.empty() && (isspace((unsigned char)p.back()) || p.back() == '"'))
        p.pop_back();
    if (p.empty()) return p;
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
        fs::path inside = fs::path(p) / "map.json";
        if (fs::exists(inside, ec)) return inside.string();
    }
    return p;
}

void AppUI::SaveCurrentMap(AppState& app) {
    app.SyncMapFromGrid();
    std::string dir = OutputDir(app, app.map.meta.name);
    if (MapSerializer::SaveToFile(dir + "/map.json", app.map)) {
        app.dirty = false;
        app.handEdited = false;
        app.job.Log("Saved " + dir + "/map.json");
    } else {
        app.job.Log("Could not save " + dir + "/map.json");
    }
}

void AppUI::DrawMainMenu(AppState& app, TextureLoader& texLoader) {
    (void)texLoader;
    if (!ImGui::BeginMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New empty map")) {
            app.PushUndo();
            const int b = arch::kBorderCells;
            int w = std::max(arch::kMinCells, app.cols);
            int h = std::max(arch::kMinCells, app.rows);
            app.grid = TileGrid(w + 2 * b, h + 2 * b, Tile::Void);
            app.grid.FillRect(b + 1, b + 1, w - 2, h - 2, Tile::Floor);
            arch::DeriveWalls(app.grid);
            app.features.clear();
            app.annotations.clear();
            app.effects.clear();
            app.map.areas.clear();
            app.map.meta.border = b;
            app.map.meta.name = "battlemap";
            app.MarkEdited();
        }
        ImGui::SetItemTooltip("A blank field at the size set on the Create tab, walled and ready to paint.");
        if (ImGui::MenuItem("Open map.json...", "Ctrl+O")) app.showOpenDialog = true;
        ImGui::SetItemTooltip("Load a plan built by the app or command line, and edit it here.");
        if (ImGui::MenuItem("Save map.json", "Ctrl+S")) SaveCurrentMap(app);
        if (ImGui::MenuItem("Export plan preview PNG")) {
            app.SyncMapFromGrid();
            std::string dir = OutputDir(app, app.map.meta.name);
            MapRasterizer::ExportPng(MapRasterizer::RenderPreview(app.map), dir + "/preview.png");
            app.job.Log("Preview written to " + dir + "/preview.png");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) PostQuitMessage(0);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Presets")) {
        if (app.styles.presets.empty()) ImGui::TextDisabled("none saved yet");
        for (const auto& kv : app.styles.presets) {
            if (ImGui::MenuItem(kv.first.c_str())) {
                app.map = kv.second;
                app.SyncGridFromMap();
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Save current as preset")) {
            app.SyncMapFromGrid();
            app.styles.SavePreset(app.map.meta.name, app.map);
            app.job.Log("Preset saved: " + app.map.meta.name);
        }
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        ImGui::TextDisabled("How this works");
        ImGui::Separator();
        ImGui::TextWrapped(
            "1. Create tab: describe the scene, pick a look, press MAKE MY BATTLE MAP.\n"
            "2. Editor tab: paint walls, water and props by hand if you want changes.\n"
            "3. Render tab: press RENDER THIS MAP to paint the plan again.\n\n"
            "The generated image never contains text, creatures or a grid - your virtual "
            "tabletop adds the grid, and you place creature tokens yourself.");
        ImGui::EndMenu();
    }
    ImGui::EndMenuBar();
}

void AppUI::DrawOpenDialog(AppState& app, TextureLoader& texLoader) {
    static bool wasOpen = false;
    static std::string chosen;
    static std::string manualPath;
    static std::string openError;

    if (app.showOpenDialog && !wasOpen) {
        ImGui::OpenPopup("Open a map");
        chosen.clear();
        ScanMaps(app, texLoader);
    }
    wasOpen = app.showOpenDialog;
    if (!app.showOpenDialog) return;

    ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImVec2 vp = ImGui::GetMainViewport()->WorkSize;
    ImGui::SetNextWindowSize(ImVec2(std::clamp(vp.x * 0.62f, 660.0f, 1400.0f),
                                    std::clamp(vp.y * 0.68f, 520.0f, 1000.0f)),
                             ImGuiCond_Appearing);
    bool stayOpen = true;
    if (!ImGui::BeginPopupModal("Open a map", &stayOpen, ImGuiWindowFlags_NoSavedSettings)) {
        app.showOpenDialog = false;
        ReleaseMapEntries();
        return;
    }

    auto loadPath = [&](const std::string& raw) {
        std::string path = TidyMapPath(raw);
        MapData loaded;
        if (path.empty()) return false;
        std::error_code ec;
        if (!fs::exists(path, ec)) {
            openError = "There is no file at that path:\n" + path;
            return false;
        }
        if (fs::is_directory(path, ec)) {
            openError = "That folder has no map.json in it:\n" + path;
            return false;
        }
        if (!MapSerializer::LoadFromFile(path, loaded)) {
            openError = "That file is not a map plan this app can read:\n" + path +
                        "\n\nA plan is the map.json written beside preview.png. "
                        "caption.json and spec.json are not plans.";
            return false;
        }
        if (loaded.grid.cols <= 0 || loaded.grid.rows <= 0) {
            openError = "That plan has no grid in it, so there is nothing to open:\n" + path;
            return false;
        }
        openError.clear();
        std::vector<std::string> problems;
        arch::ValidateMap(loaded, &problems);
        app.map = loaded;
        app.SyncGridFromMap();
        app.currentFile = path;
        app.dirty = false;
        app.handEdited = false;

        if (!loaded.meta.style.empty()) app.selectedStyle = loaded.meta.style;
        if (!loaded.meta.scene_summary.empty()) app.sceneText = loaded.meta.scene_summary;
        int lb = arch::BorderOf(loaded);
        app.cols = std::max(arch::kMinCells, loaded.grid.cols - 2 * lb);
        app.rows = std::max(arch::kMinCells, loaded.grid.rows - 2 * lb);
        app.layoutIndex = 0;
        for (int i = 1; i < 13; ++i)
            if (loaded.meta.layout == kLayoutNames[i]) app.layoutIndex = i;
        if (!loaded.meta.terrain_kind.empty()) app.terrainKind = loaded.meta.terrain_kind;
        if (!loaded.meta.terrain_amount.empty()) app.terrainAmount = loaded.meta.terrain_amount;
        if (!loaded.meta.prop_density.empty()) app.propDensity = loaded.meta.prop_density;
        if (loaded.meta.seed > 0) {
            app.seed = (int)loaded.meta.seed;
            app.randomSeed = false;
        }
        app.job.Log("Opened " + path);
        for (const auto& pr : problems) app.job.Log("repaired: " + pr);
        return true;
    };

    std::string target = !manualPath.empty() ? manualPath : chosen;
    ImGui::BeginDisabled(target.empty());
    if (ImGui::Button("Open", ImVec2(150, 34))) {
        if (loadPath(target)) {
            app.showOpenDialog = false;
            manualPath.clear();
            ReleaseMapEntries();
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(150, 34)) || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        app.showOpenDialog = false;
        manualPath.clear();
        ReleaseMapEntries();
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("double-click a row to open it");

    ImGui::Separator();
    ImGui::TextWrapped("Plans found in '%s'.", app.config.output_dir.c_str());

    ImGui::BeginChild("##maplist", ImVec2(0, -74), ImGuiChildFlags_Borders);
    int perRow = 1;
    float cardW = 160.0f;
    GridMetrics(160.0f, perRow, cardW);
    const float cardH = 150.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (size_t i = 0; i < s_mapEntries.size(); ++i) {
        if (i % perRow != 0) ImGui::SameLine();
        MapEntry& e = s_mapEntries[i];
        bool active = chosen == e.path;

        ImGui::PushID((int)i);
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##m", ImVec2(cardW - 8.0f, cardH - 8.0f))) chosen = e.path;
        bool hovered = ImGui::IsItemHovered();
        if (hovered)
            ImGui::SetTooltip("%s\n%s\n%d x %d cells", e.folder.c_str(), e.title.c_str(),
                              e.cols, e.rows);
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            if (loadPath(e.path)) {
                app.showOpenDialog = false;
                manualPath.clear();
                ImGui::CloseCurrentPopup();
                ImGui::PopID();
                break;
            }
        }

        ImVec2 p1(p0.x + cardW - 8.0f, p0.y + cardH - 8.0f);
        dl->AddRectFilled(p0, p1, hovered ? IM_COL32(48, 52, 62, 255) : IM_COL32(30, 33, 40, 255), 4.0f);
        if (e.thumb && e.thumbH > 0) {
            float boxW = cardW - 20.0f, boxH = 96.0f;
            float aspect = (float)e.thumbW / (float)e.thumbH;
            float w = std::min(boxW, boxH * aspect);
            float h = w / aspect;
            ImVec2 t0(p0.x + (cardW - 8.0f - w) * 0.5f, p0.y + 6.0f + (boxH - h) * 0.5f);
            dl->AddImage((ImTextureID)e.thumb, t0, ImVec2(t0.x + w, t0.y + h));
        }
        if (active) dl->AddRect(p0, p1, IM_COL32(250, 200, 70, 255), 4.0f, 0, 2.5f);

        std::string name = FitText(e.folder, cardW - 20.0f);
        dl->AddText(ImVec2(p0.x + 8.0f, p1.y - 40.0f),
                    active ? IM_COL32(250, 214, 120, 255) : IM_COL32(220, 218, 212, 255),
                    name.c_str());
        std::string dims = std::to_string(e.cols) + " x " + std::to_string(e.rows) + " cells";
        dl->AddText(ImVec2(p0.x + 8.0f, p1.y - 22.0f), IM_COL32(150, 152, 158, 255), dims.c_str());
        ImGui::PopID();
    }
    if (s_mapEntries.empty())
        ImGui::TextDisabled("No plans in this folder yet. Build one on the Create tab, or use Browse to open a plan from anywhere.");
    ImGui::EndChild();

    ImGui::Text("Somewhere else? Point at any map.json:");
    ImGui::SetNextItemWidth(-140.0f);
    if (InputTextString("##manualpath", &manualPath, ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (loadPath(manualPath)) {
            manualPath.clear();
            app.showOpenDialog = false;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SetItemTooltip("Paste a path and press Enter.");
    ImGui::SameLine();
    if (ImGui::Button("Browse...", ImVec2(-1, 0))) {
        std::string picked = FileDialogs::PickMapFile();
        if (!picked.empty() && loadPath(picked)) {
            manualPath.clear();
            app.showOpenDialog = false;
            ImGui::CloseCurrentPopup();
        }
    }
    if (!openError.empty()) {
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextColored(ImVec4(0.93f, 0.45f, 0.35f, 1.0f), "%s", openError.c_str());
        ImGui::PopTextWrapPos();
    }
    ImGui::EndPopup();
}

void AppUI::DrawStatusBar(AppState& app) {
    ImGui::Separator();
    ImGui::Text("Ollama: %s", app.ollamaOk ? "ready" : "offline");
    ImGui::SameLine();
    ImGui::Text("| ComfyUI: %s", app.comfyOk ? "ready" : "offline");
    ImGui::SameLine();
    int sb = app.map.meta.border;
    ImGui::Text("| Map: %d x %d", app.grid.cols - 2 * sb, app.grid.rows - 2 * sb);
    if (sb > 0)
        ImGui::SetItemTooltip("Your field is %d x %d cells. A %d-cell bleed margin is added around it for the renderer, so the image is %d x %d.",
                              app.grid.cols - 2 * sb, app.grid.rows - 2 * sb, sb,
                              app.grid.cols, app.grid.rows);
    ImGui::SameLine();
    ImGui::Text("| %s", app.job.Status().c_str());
}

void AppUI::DrainJobResults(AppState& app, TextureLoader& texLoader) {
    std::lock_guard<std::mutex> lock(app.job.mtx);
    if (app.job.hasMap) {
        app.map = app.job.map;
        app.SyncGridFromMap();
        app.job.hasMap = false;
    }
    if (app.job.hasImage) {
        texLoader.SetResultImage(app.job.imagePng);
        app.job.hasImage = false;
    }
}

void AppUI::Init(AppState& app, TextureLoader& texLoader) {
    (void)texLoader;
    {
        std::string err;
        ConfigStore::Load(app.configPath, app.config, err);
        if (!err.empty()) app.job.Log(err);
    }
    app.styles.LoadAll();
    if (!app.styles.lastError.empty()) app.job.Log(app.styles.lastError);
    if (app.styles.Find(app.config.default_style))
        app.selectedStyle = app.config.default_style;
    else if (!app.styles.styles.empty())
        app.selectedStyle = app.styles.styles.begin()->first;
    for (const auto& pr : arch::SizePresets())
        if (app.config.default_size == pr.first) {
            app.cols = pr.second.first;
            app.rows = pr.second.second;
        }

    DesignSpec spec = SpecFromUi(app);
    AttachStyle(app, spec);
    app.map = arch::Build(spec, 7u);
    app.map.meta.style = app.selectedStyle;
    app.SyncGridFromMap();
    app.SyncMapFromGrid();

    ConnectionMonitor::StartConnectionCheck(app);
}

void AppUI::Render(AppState& app, TextureLoader& texLoader) {
    DrainJobResults(app, texLoader);

    const ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("##main", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove |
                     ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoNavFocus |
                     ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar(2);

    DrawMainMenu(app, texLoader);

    if (ImGui::GetIO().KeyCtrl && !ImGui::GetIO().WantTextInput) {
        if (ImGui::IsKeyPressed(ImGuiKey_O, false)) app.showOpenDialog = true;
        if (ImGui::IsKeyPressed(ImGuiKey_S, false)) SaveCurrentMap(app);
    }

    float statusH = ImGui::GetFrameHeight() + 8.0f;
    ImGui::BeginChild("##tabhost", ImVec2(0, -statusH));
    if (ImGui::BeginTabBar("##tabs")) {
        if (ImGui::BeginTabItem("Create")) { TabPrompt::Draw(app, texLoader); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Editor")) { TabDesign::Draw(app); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Render")) { TabPaint::Draw(app, texLoader); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Dungeondraft")) { TabDungeondraft::Draw(app); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Styles")) { TabGallery::Draw(app); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Settings")) { TabSettings::Draw(app); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Docs")) { TabDocs::Draw(app); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    DrawOpenDialog(app, texLoader);
    RenderService::DrawRebuildGuard(app);
    DrawStatusBar(app);

    ImGui::End();
}

} // namespace dnd
