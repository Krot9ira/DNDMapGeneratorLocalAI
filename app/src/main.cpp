// D&D AI Battle Map Generator - desktop application.
//
// Stage 1 (brain)    scene description -> design spec -> deterministic geometry
// Stage 2 (renderer) blueprint -> ComfyUI -> finished top-down battle map
//
// The whole app is one ImGui window with tabs. Long operations run on a worker
// thread and hand their results back through Job; nothing but the main thread
// ever touches D3D, ImGui or the tile grid.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX  // windows.h min/max macros break std::min/std::max
#include <windows.h>
#include <commdlg.h>
#include <d3d11.h>

#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

#define STB_IMAGE_IMPLEMENTATION
#include <stb/stb_image.h>
#include <stb/stb_image_write.h>

#include <array>

#include "app_state.h"
#include "app_theme.h"
#include "../resources/resource.h"
#include "comfy_service.h"
#include "ideogram_caption.h"
#include "map_rasterizer.h"
#include "ollama_service.h"

#include <algorithm>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <functional>
#include <thread>
#include <vector>

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")

namespace fs = std::filesystem;
using namespace dnd;

// ---------------------------------------------------------------- D3D state
static ID3D11Device* g_device = nullptr;
static ID3D11DeviceContext* g_context = nullptr;
static IDXGISwapChain* g_swapChain = nullptr;
static ID3D11RenderTargetView* g_rtv = nullptr;
static UINT g_resizeW = 0, g_resizeH = 0;

static AppState g_app;
static ID3D11ShaderResourceView* g_resultTex = nullptr;
static int g_resultW = 0, g_resultH = 0;
static std::vector<uint8_t> g_resultPng;

// ---------------------------------------------------------------- helpers
static const char* kLayoutNames[] = {"(from style)", "dungeon", "building", "cavern",
                                     "open", "forest", "swamp", "ruins",
                                     "street", "arena", "harbour"};
static const char* kTerrainNames[] = {"none", "water", "pit", "rubble", "vegetation"};
static const char* kAmountNames[] = {"low", "medium", "high"};

static const char* kPaintTiles[] = {"floor", "wall", "door", "window", "water", "pit",
                                    "rubble", "vegetation", "bridge", "stairs", "void"};
static const char* kTileHints[] = {
    "Walkable ground.",
    "Solid wall. Blocks movement and sight.",
    "Door. Only valid inside a wall - elsewhere it becomes a plain opening.",
    "Window. A glazed or shuttered opening in a wall. Lets light and sight "
    "through, but not a body.",
    "Water. Difficult or impassable, depending on your table.",
    "Pit or drop.",
    "Loose rubble. Difficult ground.",
    "Undergrowth. Light cover.",
    "Timber walkway or ship decking.",
    "Stairs.",
    "Eraser: clears back to empty space."};
struct EffectInfo {
    const char* kind;
    const char* label;
    const char* hint;
    ImU32 tint;            // how it is washed over the canvas
};

// Atmospheric overlays. They never change the ground or block movement - they
// are light, smoke and weather painted on top of the finished map.
static const EffectInfo kEffects[] = {
    {"fire", "Fire", "Leaping flames throwing light on the ground.", IM_COL32(255, 140, 40, 90)},
    {"embers", "Embers", "Glowing coals with sparks rising.", IM_COL32(255, 90, 30, 80)},
    {"smoke", "Smoke", "Thick grey smoke curling upward.", IM_COL32(120, 120, 125, 95)},
    {"fog", "Fog", "Low bank of pale drifting fog.", IM_COL32(200, 205, 215, 90)},
    {"mist", "Mist", "Thin silver mist clinging low.", IM_COL32(190, 210, 220, 70)},
    {"fireflies", "Fireflies", "Tiny warm points of light in the air.",
     IM_COL32(220, 255, 120, 70)},
    {"magic_glow", "Arcane", "Soft violet arcane glow.", IM_COL32(170, 110, 255, 85)},
    {"holy_light", "Holy light", "Shaft of pale golden light from above.",
     IM_COL32(255, 230, 150, 85)},
    {"poison_gas", "Poison", "Sickly yellow-green vapour lying low.", IM_COL32(150, 220, 90, 90)},
    {"blood", "Blood", "Dark red blood pooled and smeared.", IM_COL32(160, 30, 35, 95)},
    {"ice", "Ice", "Sheet of pale blue ice with frost.", IM_COL32(150, 210, 250, 85)},
    {"webs", "Webs", "Sheets of dusty grey spider web.", IM_COL32(225, 225, 230, 80)},
    {"sparks", "Sparks", "Bright white sparks arcing.", IM_COL32(255, 250, 200, 75)},
    {"ash", "Ash", "Grey ash settled in drifts.", IM_COL32(140, 138, 132, 85)},
    {"steam", "Steam", "White steam venting in soft billows.", IM_COL32(230, 235, 240, 80)},
    {"shadow", "Shadow", "Unnatural pool of deep shadow.", IM_COL32(20, 18, 30, 120)},
};

static ImU32 EffectTint(const std::string& kind) {
    for (const auto& e : kEffects)
        if (kind == e.kind) return e.tint;
    return IM_COL32(200, 160, 255, 80);
}

struct PropInfo {
    const char* kind;
    const char* label;
    const char* hint;
};

// Every prop the renderer has a concrete description for, grouped so the picker
// reads like a catalogue rather than an alphabetical dump.
static const PropInfo kProps[] = {
    {"barrel", "Barrel", "Wooden barrel with iron bands. Blocks movement."},
    {"crate", "Crate", "Stacked cargo crate. Blocks movement."},
    {"chest", "Chest", "Lidded storage chest. Loot container."},
    {"table", "Table", "Long timber table. Half cover."},
    {"chair", "Chair", "Single seat."},
    {"bench", "Bench", "Low timber bench."},
    {"bed", "Bed", "Straw mattress on a frame."},
    {"bookshelf", "Shelf", "Tall shelf packed with ledgers. Full cover."},
    {"bar", "Bar", "Polished bar counter."},
    {"desk", "Desk", "Writing desk covered in papers."},
    {"cabinet", "Cabinet", "Closed storage cabinet."},
    {"torch", "Torch", "Wall torch. Light source."},
    {"lamp", "Lamp", "Hanging lantern. Light source."},
    {"brazier", "Brazier", "Iron brazier of live coals. Light and fire."},
    {"campfire", "Campfire", "Ring of stones round a fire."},
    {"hearth", "Hearth", "Stone fireplace with burning logs."},
    {"forge", "Forge", "Stone forge glowing with coals."},
    {"cauldron", "Cauldron", "Iron cauldron on a tripod."},
    {"anvil", "Anvil", "Black iron anvil on a stump."},
    {"pillar", "Pillar", "Thick stone column. Blocks line of sight."},
    {"statue", "Statue", "Carved figure on a plinth. Full cover."},
    {"altar", "Altar", "Carved stone altar block."},
    {"sarcophagus", "Sarcophagus", "Heavy stone coffin with a chipped lid."},
    {"bones", "Bones", "Heap of old bones. Difficult ground."},
    {"throne", "Throne", "High-backed carved seat."},
    {"well", "Well", "Round stone well with a winch."},
    {"fountain", "Fountain", "Carved fountain basin."},
    {"portal", "Arch", "Standing stone archway."},
    {"tree", "Tree", "Broad canopy seen from above. Full cover."},
    {"bush", "Bush", "Low shrub. Light cover."},
    {"stump", "Stump", "Cut tree stump."},
    {"boulder", "Boulder", "Moss-covered rock. Blocks movement."},
    {"stalagmite", "Stalagmite", "Jagged rock spire."},
    {"crystal", "Crystal", "Cluster of glowing shards. Light source."},
    {"mushroom", "Mushroom", "Oversized cave fungus."},
    {"cart", "Cart", "Two-wheeled handcart."},
    {"wagon", "Wagon", "Four-wheeled timber wagon."},
    {"rope_coil", "Rope", "Coiled rope on the ground."},
    {"net", "Net", "Fishing net spread out."},
    {"bollard", "Bollard", "Iron mooring post."},
    {"capstan", "Capstan", "Timber capstan with bars."},
    {"mast", "Mast", "Base of a mast with rigging."},
    {"console", "Console", "Wall console with cracked screens."},
    {"locker", "Locker", "Steel storage locker."},
    {"dumpster", "Dumpster", "Dented steel dumpster."},
    {"weapon_rack", "Weapons", "Rack of spears and blades."},
    {"banner", "Banner", "Hanging cloth banner."},
};

// Side panels take a share of the window rather than a fixed pixel width, so
// maximising the app widens the lists too instead of only the canvas.
static float PanelWidth(float fraction, float minimum, float maximum) {
    float avail = ImGui::GetContentRegionAvail().x;
    return std::clamp(avail * fraction, minimum, std::min(maximum, avail * 0.6f));
}

// Lay a grid out so the cards fill the row exactly, with no ragged gap.
static void GridMetrics(float minCell, int& outPerRow, float& outCellW) {
    float avail = ImGui::GetContentRegionAvail().x;
    outPerRow = std::max(1, (int)(avail / minCell));
    outCellW = avail / outPerRow;
}

// Trim a label to the width it actually has, rather than to a fixed count.
static std::string FitText(const std::string& text, float maxWidth) {
    if (ImGui::CalcTextSize(text.c_str()).x <= maxWidth) return text;
    std::string out = text;
    while (out.size() > 4 &&
           ImGui::CalcTextSize((out + "...").c_str()).x > maxWidth) {
        out.pop_back();
    }
    return out + "...";
}

static void DrawTileGlyph(ImDrawList* dl, ImVec2 c, float r, Tile t, ImU32 col);
static void DrawPropGlyph(ImDrawList* dl, ImVec2 c, float r, const char* kind, ImU32 col);
static void DrawEffectGlyph(ImDrawList* dl, ImVec2 c, float r, const char* kind, ImU32 col);

// What the catalogue calls a prop or an effect, for hover text.
static const char* PropLabel(const std::string& kind) {
    for (const auto& info : kProps)
        if (kind == info.kind) return info.label;
    return kind.c_str();
}

static const char* EffectLabel(const std::string& kind) {
    for (const auto& info : kEffects)
        if (kind == info.kind) return info.label;
    return kind.c_str();
}

static ImU32 TileColor(Tile t) {
    switch (t) {
    case Tile::Void: return IM_COL32(28, 30, 36, 255);
    case Tile::Floor: return IM_COL32(232, 226, 214, 255);
    case Tile::Wall: return IM_COL32(58, 62, 72, 255);
    case Tile::Door: return IM_COL32(196, 150, 74, 255);
    case Tile::Window: return IM_COL32(150, 196, 214, 255);
    case Tile::Water: return IM_COL32(86, 140, 178, 255);
    case Tile::Pit: return IM_COL32(34, 34, 40, 255);
    case Tile::Rubble: return IM_COL32(176, 166, 150, 255);
    case Tile::Vegetation: return IM_COL32(118, 156, 104, 255);
    case Tile::Stairs: return IM_COL32(198, 190, 176, 255);
    case Tile::Bridge: return IM_COL32(166, 130, 88, 255);
    default: return IM_COL32(120, 120, 120, 255);
    }
}

static ID3D11ShaderResourceView* CreateTextureRGBA(const uint8_t* pixels, int w, int h) {
    if (!pixels || w <= 0 || h <= 0 || !g_device) return nullptr;
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;

    D3D11_SUBRESOURCE_DATA init = {};
    init.pSysMem = pixels;
    init.SysMemPitch = w * 4;

    ID3D11Texture2D* tex = nullptr;
    if (FAILED(g_device->CreateTexture2D(&desc, &init, &tex)) || !tex) return nullptr;
    D3D11_SHADER_RESOURCE_VIEW_DESC srv = {};
    srv.Format = desc.Format;
    srv.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srv.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* view = nullptr;
    g_device->CreateShaderResourceView(tex, &srv, &view);
    tex->Release();
    return view;
}

static void SetResultImage(const std::vector<uint8_t>& png) {
    if (g_resultTex) { g_resultTex->Release(); g_resultTex = nullptr; }
    g_resultW = g_resultH = 0;
    if (png.empty()) return;
    int w = 0, h = 0, ch = 0;
    unsigned char* raw = stbi_load_from_memory(png.data(), (int)png.size(), &w, &h, &ch, 4);
    if (!raw) return;
    g_resultTex = CreateTextureRGBA(raw, w, h);
    g_resultW = w;
    g_resultH = h;
    stbi_image_free(raw);
    g_resultPng = png;
}

static std::string OutputDir(const std::string& name) {
    fs::path dir = fs::path(g_app.config.output_dir) / (name.empty() ? "map" : name);
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir.string();
}

static DesignSpec SpecFromUi() {
    DesignSpec spec;
    spec.title = "Battle Map";
    spec.name = OllamaService::SanitizeName(g_app.sceneText.substr(0, 40));
    spec.style = g_app.selectedStyle;
    spec.scene_summary = g_app.sceneText;
    spec.render_details = g_app.map.meta.render_details;
    spec.prop_density = g_app.propDensity;
    spec.terrain_kind = g_app.terrainKind;
    spec.terrain_amount = g_app.terrainAmount;

    const StyleDef* style = g_app.styles.Find(g_app.selectedStyle);
    spec.layout = g_app.layoutIndex == 0
                      ? (style ? style->default_layout : std::string("dungeon"))
                      : kLayoutNames[g_app.layoutIndex];
    if (style) spec.style_props = style->props;

    spec.cols = g_app.cols;
    spec.rows = g_app.rows;
    spec.border = std::clamp(g_app.config.border_cells, 0, 8);

    bool outdoor = spec.layout == "harbour" || spec.layout == "open" ||
                   spec.layout == "street" || spec.layout == "forest" ||
                   spec.layout == "swamp" || spec.layout == "ruins";
    spec.edge_walls = !outdoor;

    // Without a described scene the architect still needs areas to fill.
    spec.rooms = {{"area_1", "Main Area", 'l', "none", {}, false, 0, 0, 0, 0},
                  {"area_2", "Second Area", 'm', "none", {}, false, 0, 0, 0, 0},
                  {"area_3", "Third Area", 'm', "none", {}, false, 0, 0, 0, 0}};
    return spec;
}

static uint32_t PickSeed() {
    if (!g_app.randomSeed && g_app.seed > 0) return (uint32_t)g_app.seed;
    return (uint32_t)(GetTickCount64() & 0x7FFFFFFF);
}

// ---------------------------------------------------------------- jobs
// Every job follows the same contract: run on a worker thread, log progress,
// leave results in Job for the main thread to pick up.
static bool BeginJob(const char* what) {
    if (g_app.job.running.exchange(true)) return false;
    g_app.job.cancel = false;
    {
        std::lock_guard<std::mutex> lock(g_app.job.mtx);
        g_app.job.log.clear();
        g_app.job.finishedMessage.clear();
        g_app.job.failed = false;
        g_app.job.status = what;
    }
    return true;
}

static void FinishJob(bool ok, const std::string& message) {
    std::lock_guard<std::mutex> lock(g_app.job.mtx);
    g_app.job.failed = !ok;
    g_app.job.finishedMessage = message;
    g_app.job.status = ok ? "Done" : "Failed";
    g_app.job.running = false;
}

// Stage 2. Ideogram takes the layout as bounding boxes inside a JSON caption,
// so nothing is uploaded and no ControlNet is involved.
// Fill the outer band of a finished render flat, like the mount around a
// printed map. The colour is taken from what is already in the margin ring, so
// it never looks bolted on.
static void PaintBleedMargin(std::vector<uint8_t>& png, const MapData& map) {
    int border = arch::BorderOf(map);
    if (border <= 0 || map.grid.cols <= 0 || map.grid.rows <= 0 || png.empty()) return;

    int w = 0, h = 0, comp = 0;
    unsigned char* pixels = stbi_load_from_memory(png.data(), (int)png.size(), &w, &h, &comp, 3);
    if (!pixels || w <= 0 || h <= 0) {
        if (pixels) stbi_image_free(pixels);
        return;
    }

    int mx = std::max(1, (int)std::lround((double)w * border / map.grid.cols));
    int my = std::max(1, (int)std::lround((double)h * border / map.grid.rows));

    auto at = [&](int x, int y) { return pixels + ((size_t)y * w + x) * 3; };
    std::vector<std::array<int, 3>> ring;
    for (int x = 0; x < w; x += std::max(1, w / 120)) {
        unsigned char* a = at(x, std::min(my / 2, h - 1));
        unsigned char* b = at(x, std::max(0, h - 1 - my / 2));
        ring.push_back({a[0], a[1], a[2]});
        ring.push_back({b[0], b[1], b[2]});
    }
    for (int y = 0; y < h; y += std::max(1, h / 120)) {
        unsigned char* a = at(std::min(mx / 2, w - 1), y);
        unsigned char* b = at(std::max(0, w - 1 - mx / 2), y);
        ring.push_back({a[0], a[1], a[2]});
        ring.push_back({b[0], b[1], b[2]});
    }
    std::sort(ring.begin(), ring.end(), [](const auto& a, const auto& b) {
        return a[0] + a[1] + a[2] < b[0] + b[1] + b[2];
    });
    std::array<int, 3> fill = ring.empty() ? std::array<int, 3>{150, 145, 125}
                                           : ring[ring.size() / 2];

    auto band = [&](int x0, int y0, int x1, int y1) {
        for (int y = std::max(0, y0); y < std::min(h, y1); ++y) {
            for (int x = std::max(0, x0); x < std::min(w, x1); ++x) {
                unsigned char* p = at(x, y);
                p[0] = (unsigned char)fill[0];
                p[1] = (unsigned char)fill[1];
                p[2] = (unsigned char)fill[2];
            }
        }
    };
    band(0, 0, w, my);
    band(0, h - my, w, h);
    band(0, 0, mx, h);
    band(w - mx, 0, w, h);

    std::vector<uint8_t> out;
    stbi_write_png_to_func(
        [](void* ctx, void* data, int size) {
            auto* v = (std::vector<uint8_t>*)ctx;
            v->insert(v->end(), (uint8_t*)data, (uint8_t*)data + size);
        },
        &out, w, h, 3, pixels, w * 3);
    stbi_image_free(pixels);
    if (!out.empty()) png.swap(out);
}

static bool RunRender(MapData map) {
    Job& job = g_app.job;
    ComfyConfig cfg = g_app.config.comfy;

    // The planner is the other heavy tenant of the graphics card. Ask it to let
    // go before a render, or the two of them fight over the same memory.
    if (g_app.ollamaOk) {
        job.SetStatus("Freeing the planner model...");
        if (OllamaService::Unload(g_app.config.ollama.base_url, g_app.config.ollama.model))
            job.Log("Ollama released " + g_app.config.ollama.model + ".");
    }

    std::string version, error;
    job.SetStatus("Contacting ComfyUI...");
    if (!ComfyService::CheckConnection(cfg.base_url, version, error)) {
        job.Log("ComfyUI is not reachable at " + cfg.base_url + " (" + error + ")");
        job.Log("Start ComfyUI and press Generate again.");
        return false;
    }
    job.Log("Connected to " + version);

    std::string dir = OutputDir(map.meta.name);
    MapSerializer::SaveToFile(dir + "/map.json", map);
    MapRasterizer::ExportPng(MapRasterizer::RenderPreview(map), dir + "/preview.png");

    std::string caption;
    if (g_app.captionManual && !g_app.captionText.empty()) {
        // A hand-written caption is sent exactly as typed. Minified first if it
        // parses, so ComfyUI gets the shape the builder would have sent.
        caption = g_app.captionText;
        try {
            caption = nlohmann::json::parse(caption).dump(
                -1, ' ', false, nlohmann::json::error_handler_t::replace);
        } catch (const std::exception&) {
            job.Log("Hand-written caption is not valid JSON - sending it exactly as typed.");
        }
        job.Log("Using the hand-written caption.");
    } else {
        caption = IdeogramCaption::BuildJson(map, g_app.styles.Find(map.meta.style),
                                             g_app.styles.base, g_app.styles.phrases);
    }
    {
        std::ofstream f(dir + "/caption.json");
        if (f.is_open()) f << caption;
    }

    int width = 0, height = 0;
    ComfyService::TargetSize(map.grid, cfg.megapixels, width, height);
    int64_t seed = ComfyService::ResolveSeed(cfg.seed);
    job.Log("Caption " + std::to_string(caption.size()) + " chars, " +
            std::to_string(width) + "x" + std::to_string(height) + " px, no control image");

    nlohmann::json graph = ComfyService::BuildGraph(cfg, caption, width, height, seed);
    job.SetStatus("Queuing render...");
    std::string promptId = ComfyService::QueuePrompt(cfg.base_url, graph, error);
    if (promptId.empty()) {
        job.Log("ComfyUI refused the job: " + error);
        return false;
    }
    job.Log("Queued " + promptId + " (seed " + std::to_string(seed) + ")");

    ComfyResult result = ComfyService::PollAndDownload(
        cfg.base_url, promptId, 2400,
        [&job](const std::string& note) { job.SetStatus(note); },
        [&job]() { return job.cancel.load(); });

    if (!result.ok) {
        job.Log("Render failed: " + result.error);
        return false;
    }

    // The renderer is told the content is inset and usually obliges, but under
    // a large effect it paints out to the frame edge. The margin is mechanical,
    // so it is guaranteed here rather than negotiated.
    PaintBleedMargin(result.images[0], map);

    std::string savedPath = dir + "/battlemap.png";
    {
        std::ofstream f(savedPath, std::ios::binary);
        f.write((const char*)result.images[0].data(), (std::streamsize)result.images[0].size());
    }
    job.Log("Saved " + savedPath);
    {
        std::lock_guard<std::mutex> lock(job.mtx);
        job.imagePng = result.images[0];
        job.hasImage = true;
    }
    return true;
}

// Generating a plan throws the current one away. If the current one is the
// user's own work, that has to be their decision, not a side effect of pressing
// a green button.
enum class Rebuild { None, Blueprint, Plan, PlanAndRender };
static Rebuild g_pendingRebuild = Rebuild::None;

static void RunRebuild(Rebuild what);

static void RequestRebuild(Rebuild what) {
    if (g_app.handEdited) {
        g_pendingRebuild = what;
        return;                 // the modal below takes it from here
    }
    RunRebuild(what);
}

static void StartQuickBlueprint() {
    if (!BeginJob("Building blueprint...")) return;
    DesignSpec spec = SpecFromUi();
    uint32_t seed = PickSeed();
    std::thread([spec, seed]() {
        Job& job = g_app.job;
        MapData map = arch::Build(spec, seed);
        job.Log("Built " + std::to_string(map.grid.cols) + "x" +
                std::to_string(map.grid.rows) + " " + spec.layout + " map, " +
                std::to_string(map.areas.size()) + " areas, " +
                std::to_string(map.features.size()) + " props");
        {
            std::lock_guard<std::mutex> lock(job.mtx);
            job.map = map;
            job.hasMap = true;
        }
        FinishJob(true, "Blueprint ready - open the Editor tab to adjust it.");
    }).detach();
}

static void StartPlanAndRender(bool alsoRender) {
    if (!BeginJob(alsoRender ? "Planning scene..." : "Planning scene...")) return;
    OllamaConfig ocfg = g_app.config.ollama;
    std::string scene = g_app.sceneText;
    std::string styleId = g_app.selectedStyle;
    // The planner still takes a size word; pick the nearest preset to the sliders.
    std::string size = "medium";
    {
        long long best = -1;
        for (const auto& pr : arch::SizePresets()) {
            long long d = std::abs((long long)pr.second.first - g_app.cols) +
                          std::abs((long long)pr.second.second - g_app.rows);
            if (best < 0 || d < best) { best = d; size = pr.first; }
        }
    }
    uint32_t seed = PickSeed();

    std::vector<std::string> ids;
    std::string catalogue;
    for (const auto& kv : g_app.styles.styles) {
        ids.push_back(kv.first);
        catalogue += "- " + kv.first + ": " + kv.second.name + " - " + kv.second.description + "\n";
    }
    std::map<std::string, std::vector<std::string>> styleProps;
    std::map<std::string, std::string> styleLayout;
    for (const auto& kv : g_app.styles.styles) {
        styleProps[kv.first] = kv.second.props;
        styleLayout[kv.first] = kv.second.default_layout;
    }

    std::string comfyUrl = g_app.config.comfy.base_url;
    bool comfyUp = g_app.comfyOk;

    std::thread([=]() {
        Job& job = g_app.job;
        // ComfyUI holds ~27 GB of weights once it has rendered. Ask it to let go
        // before the planner needs the card, or the planner crawls.
        if (comfyUp) {
            job.SetStatus("Freeing the renderer models...");
            if (ComfyService::FreeMemory(comfyUrl))
                job.Log("ComfyUI released its models.");
        }
        job.Log("Asking " + ocfg.model + " to design the scene...");
        PlanResult plan = OllamaService::PlanScene(ocfg, scene, styleId, size, ids, catalogue);
        if (!plan.ok) {
            job.Log("Planning failed: " + plan.error);
            job.Log("Tip: the Quick blueprint button works without the language model.");
            FinishJob(false, "Planning failed - see the log.");
            return;
        }
        DesignSpec spec = plan.spec;
        if (spec.style.empty()) spec.style = styleId;
        auto propIt = styleProps.find(spec.style);
        if (propIt != styleProps.end()) spec.style_props = propIt->second;
        job.Log("Layout: " + spec.layout + " | areas: " + std::to_string(spec.rooms.size()));
        if (!spec.scene_summary.empty()) job.Log("Scene: " + spec.scene_summary);

        MapData map = arch::Build(spec, seed);
        job.Log("Blueprint: " + std::to_string(map.grid.cols) + "x" +
                std::to_string(map.grid.rows) + ", " + std::to_string(map.features.size()) +
                " props");
        {
            std::lock_guard<std::mutex> lock(job.mtx);
            job.map = map;
            job.hasMap = true;
        }
        if (!alsoRender) {
            FinishJob(true, "Blueprint ready.");
            return;
        }

        bool ok = RunRender(map);
        FinishJob(ok, ok ? "Battle map ready." : "Render failed - see the log.");
    }).detach();
}

static void StartRenderCurrent() {
    if (!BeginJob("Rendering...")) return;
    g_app.SyncMapFromGrid();
    MapData map = g_app.map;
    std::thread([map]() {
        bool ok = RunRender(map);
        FinishJob(ok, ok ? "Battle map ready." : "Render failed - see the log.");
    }).detach();
}

static void RunRebuild(Rebuild what) {
    switch (what) {
    case Rebuild::Blueprint:     StartQuickBlueprint(); break;
    case Rebuild::Plan:          StartPlanAndRender(false); break;
    case Rebuild::PlanAndRender: StartPlanAndRender(true); break;
    default: break;
    }
}

// Saves the current plan where the rest of the app expects to find it.
static bool SaveCurrentPlan(std::string* outPath) {
    g_app.SyncMapFromGrid();
    std::string dir = OutputDir(g_app.map.meta.name);
    std::string path = dir + "/map.json";
    if (!MapSerializer::SaveToFile(path, g_app.map)) return false;
    MapRasterizer::ExportPng(MapRasterizer::RenderPreview(g_app.map), dir + "/preview.png");
    g_app.dirty = false;
    g_app.handEdited = false;
    g_app.currentFile = path;
    if (outPath) *outPath = path;
    return true;
}

static void DrawRebuildGuard() {
    if (g_pendingRebuild != Rebuild::None && !ImGui::IsPopupOpen("Replace your plan?"))
        ImGui::OpenPopup("Replace your plan?");

    ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(520, 0), ImGuiCond_Appearing);
    if (!ImGui::BeginPopupModal("Replace your plan?", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize))
        return;

    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(
        "You have changed this plan by hand. Building a new one replaces all of it - "
        "the walls you painted, the props you placed, your custom areas and effects.");
    ImGui::PopTextWrapPos();
    ImGui::Spacing();

    if (ImGui::Button("Save it first, then build", ImVec2(-1, 32))) {
        std::string path;
        if (SaveCurrentPlan(&path)) {
            g_app.job.Log("Saved " + path + " before rebuilding.");
            Rebuild what = g_pendingRebuild;
            g_pendingRebuild = Rebuild::None;
            ImGui::CloseCurrentPopup();
            RunRebuild(what);
        } else {
            g_app.job.Log("Could not save the plan - nothing was replaced.");
            g_pendingRebuild = Rebuild::None;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SetItemTooltip("Writes map.json and preview.png into the output folder, then "
                          "starts building the new plan.");

    if (ImGui::Button("Replace it without saving", ImVec2(-1, 28))) {
        Rebuild what = g_pendingRebuild;
        g_pendingRebuild = Rebuild::None;
        ImGui::CloseCurrentPopup();
        RunRebuild(what);
    }
    if (ImGui::Button("Keep what I have", ImVec2(-1, 28)) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        g_pendingRebuild = Rebuild::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

static void StartConnectionCheck() {
    if (!BeginJob("Checking services...")) return;
    OllamaConfig ocfg = g_app.config.ollama;
    ComfyConfig ccfg = g_app.config.comfy;
    std::thread([ocfg, ccfg]() {
        Job& job = g_app.job;
        std::vector<std::string> models;
        std::string err;
        if (OllamaService::CheckConnection(ocfg.base_url, models, err)) {
            g_app.ollamaOk = true;
            g_app.ollamaModels = models;
            std::string list;
            for (const auto& m : models) list += (list.empty() ? "" : ", ") + m;
            g_app.ollamaStatus = "ready (" + list + ")";
            job.Log("Ollama OK: " + list);
            bool found = false;
            for (const auto& m : models) if (m == ocfg.model) found = true;
            if (!found)
                job.Log("Warning: model '" + ocfg.model + "' is not in that list.");
        } else {
            g_app.ollamaOk = false;
            g_app.ollamaStatus = "unreachable (" + err + ")";
            job.Log("Ollama unreachable: " + err);
        }
        std::string version;
        if (ComfyService::CheckConnection(ccfg.base_url, version, err)) {
            g_app.comfyOk = true;
            g_app.comfyStatus = version;
            job.Log("ComfyUI OK: " + version);
        } else {
            g_app.comfyOk = false;
            g_app.comfyStatus = "unreachable (" + err + ")";
            job.Log("ComfyUI unreachable: " + err);
        }
        FinishJob(true, "Service check complete.");
    }).detach();
}

// Applied on the main thread only.
static void DrainJobResults() {
    Job& job = g_app.job;
    std::lock_guard<std::mutex> lock(job.mtx);
    if (job.hasMap) {
        g_app.map = job.map;
        g_app.SyncGridFromMap();
        g_app.SyncMapFromGrid();
        // A generated map is not the user's handiwork - it is ours.
        g_app.dirty = true;
        g_app.handEdited = false;
        job.hasMap = false;
    }
    if (job.hasImage) {
        SetResultImage(job.imagePng);
        job.hasImage = false;
    }
}

// ---------------------------------------------------------------- UI pieces
static bool InputTextString(const char* label, std::string* str, ImGuiInputTextFlags flags = 0) {
    char buf[1024];
    strncpy_s(buf, sizeof(buf), str->c_str(), _TRUNCATE);
    if (ImGui::InputText(label, buf, sizeof(buf), flags)) {
        *str = buf;
        return true;
    }
    return false;
}

static bool InputTextMultilineString(const char* label, std::string* str, const ImVec2& size) {
    static std::vector<char> buf;
    buf.assign(str->begin(), str->end());
    buf.resize(std::max<size_t>(buf.size() + 1, 8192), '\0');
    if (ImGui::InputTextMultiline(label, buf.data(), buf.size(), size)) {
        *str = buf.data();
        return true;
    }
    return false;
}

static void HelpMarker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

static void DrawJobLog(float height) {
    ImGui::BeginChild("##joblog", ImVec2(0, height), ImGuiChildFlags_Borders);
    std::lock_guard<std::mutex> lock(g_app.job.mtx);
    for (const auto& line : g_app.job.log) ImGui::TextWrapped("%s", line.c_str());
    if (!g_app.job.finishedMessage.empty()) {
        ImGui::Separator();
        ImVec4 col = g_app.job.failed ? ImVec4(1.0f, 0.45f, 0.40f, 1.0f)
                                      : ImVec4(0.45f, 0.90f, 0.55f, 1.0f);
        ImGui::TextColored(col, "%s", g_app.job.finishedMessage.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40.0f) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

// Blueprint canvas. Shared by the Create preview (read-only) and the Editor.
static void DrawMapCanvas(bool interactive, ImVec2 size) {
    TileGrid& grid = g_app.grid;
    if (grid.cols <= 0 || grid.rows <= 0) {
        ImGui::TextDisabled("No map yet. Build one on the Create tab.");
        return;
    }

    ImVec2 origin = ImGui::GetCursorScreenPos();
    if (size.x <= 0) size.x = ImGui::GetContentRegionAvail().x;
    if (size.y <= 0) size.y = ImGui::GetContentRegionAvail().y;
    size.x = std::max(size.x, 60.0f);
    size.y = std::max(size.y, 60.0f);

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(origin, ImVec2(origin.x + size.x, origin.y + size.y),
                      IM_COL32(23, 19, 15, 255));

    ImGui::InvisibleButton("##canvas", size,
                           ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
                               ImGuiButtonFlags_MouseButtonMiddle);
    bool hovered = ImGui::IsItemHovered();
    ImGuiIO& io = ImGui::GetIO();

    if (interactive && hovered && io.MouseWheel != 0.0f) {
        float old = g_app.zoom;
        g_app.zoom = std::clamp(g_app.zoom * (io.MouseWheel > 0 ? 1.12f : 1.0f / 1.12f),
                                0.15f, 6.0f);
        // Keep the cell under the cursor anchored while zooming.
        ImVec2 m(io.MousePos.x - origin.x - g_app.pan.x, io.MousePos.y - origin.y - g_app.pan.y);
        float k = g_app.zoom / old;
        g_app.pan.x -= m.x * (k - 1.0f);
        g_app.pan.y -= m.y * (k - 1.0f);
    }
    if (interactive && ImGui::IsItemActive() &&
        (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
         ImGui::IsMouseDragging(ImGuiMouseButton_Right))) {
        g_app.pan.x += io.MouseDelta.x;
        g_app.pan.y += io.MouseDelta.y;
    }

    float base = interactive ? 26.0f : 0.0f;
    if (!interactive) {
        // Fit the whole map into the preview box.
        base = std::min(size.x / grid.cols, size.y / grid.rows);
    }
    float cell = interactive ? base * g_app.zoom : base;
    ImVec2 off = interactive
                     ? ImVec2(origin.x + g_app.pan.x, origin.y + g_app.pan.y)
                     : ImVec2(origin.x + (size.x - cell * grid.cols) * 0.5f,
                              origin.y + (size.y - cell * grid.rows) * 0.5f);

    dl->PushClipRect(origin, ImVec2(origin.x + size.x, origin.y + size.y), true);

    for (int y = 0; y < grid.rows; ++y) {
        for (int x = 0; x < grid.cols; ++x) {
            ImVec2 a(off.x + x * cell, off.y + y * cell);
            ImVec2 b(a.x + cell + 0.6f, a.y + cell + 0.6f);
            if (b.x < origin.x || a.x > origin.x + size.x) continue;
            if (b.y < origin.y || a.y > origin.y + size.y) continue;
            dl->AddRectFilled(a, b, TileColor(grid.Get(x, y)));
        }
    }

    if (g_app.showProps) {
        for (const auto& f : g_app.features) {
            ImVec2 c(off.x + (f.x + 0.5f) * cell, off.y + (f.y + 0.5f) * cell);
            float r = cell * 0.34f;
            dl->AddCircleFilled(c, r, IM_COL32(34, 34, 42, 210), 14);
            if (f.label.empty() && cell >= 10.0f) {
                DrawPropGlyph(dl, c, r * 0.92f, f.kind.c_str(), IM_COL32(244, 228, 180, 255));
            } else {
                // Custom props have no drawn form - their look lives in the text.
                dl->AddCircle(c, r * 0.75f, IM_COL32(240, 220, 160, 255), 14,
                              std::max(1.0f, cell * 0.05f));
                dl->AddCircleFilled(c, r * 0.28f, IM_COL32(240, 220, 160, 255), 10);
            }
        }
    }

    // The blank margin. It is not the user's field, so it is drawn as what it
    // is: dead space outside the map, hatched and dimmed.
    const int border = g_app.map.meta.border;
    if (border > 0) {
        ImVec2 in0(off.x + border * cell, off.y + border * cell);
        ImVec2 in1(off.x + (grid.cols - border) * cell, off.y + (grid.rows - border) * cell);
        ImVec2 out0(off.x, off.y), out1(off.x + grid.cols * cell, off.y + grid.rows * cell);
        ImU32 shade = IM_COL32(12, 13, 17, 165);
        dl->AddRectFilled(out0, ImVec2(out1.x, in0.y), shade);
        dl->AddRectFilled(ImVec2(out0.x, in1.y), out1, shade);
        dl->AddRectFilled(ImVec2(out0.x, in0.y), ImVec2(in0.x, in1.y), shade);
        dl->AddRectFilled(ImVec2(in1.x, in0.y), ImVec2(out1.x, in1.y), shade);
        // Diagonal hatching, so it reads as "not part of the map" at a glance.
        float step = std::max(9.0f, cell * 0.7f);
        ImU32 hatch = IM_COL32(150, 140, 115, 34);
        float span = (out1.x - out0.x) + (out1.y - out0.y);
        dl->PushClipRect(out0, out1, true);
        for (float d = 0.0f; d < span; d += step)
            dl->AddLine(ImVec2(out0.x + d, out0.y),
                        ImVec2(out0.x + d - (out1.y - out0.y), out1.y), hatch, 1.0f);
        dl->PopClipRect();
        dl->AddRect(in0, in1, IM_COL32(214, 186, 122, 190), 0, 0,
                    std::max(1.5f, cell * 0.07f));
    }

    if (g_app.showCellGuides && cell >= 6.0f) {
        ImU32 guide = IM_COL32(0, 0, 0, 46);
        for (int x = 0; x <= grid.cols; ++x)
            dl->AddLine(ImVec2(off.x + x * cell, off.y),
                        ImVec2(off.x + x * cell, off.y + grid.rows * cell), guide);
        for (int y = 0; y <= grid.rows; ++y)
            dl->AddLine(ImVec2(off.x, off.y + y * cell),
                        ImVec2(off.x + grid.cols * cell, off.y + y * cell), guide);
    }

    // Effects are the top layer, so they wash over everything below them.
    for (const auto& e : g_app.effects) {
        ImVec2 p0(off.x + e.x * cell, off.y + e.y * cell);
        ImVec2 p1(off.x + (e.x + e.w) * cell, off.y + (e.y + e.h) * cell);
        ImU32 tint = e.label.empty() ? EffectTint(e.kind) : IM_COL32(200, 160, 255, 90);
        dl->AddRectFilled(p0, p1, tint, 3.0f);
        // The effect's own icon, so the layer reads without hunting for labels.
        float gr = std::min(std::min(p1.x - p0.x, p1.y - p0.y) * 0.30f, 26.0f);
        if (gr > 5.0f)
            DrawEffectGlyph(dl, ImVec2((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f), gr,
                            e.label.empty() ? e.kind.c_str() : "custom",
                            IM_COL32(255, 255, 255, 225));
        if (cell >= 9.0f) {
            const char* name = e.label.empty() ? e.kind.c_str() : e.label.c_str();
            dl->AddText(ImVec2(p0.x + 5, p1.y - 18), IM_COL32(255, 255, 255, 215), name);
        }
    }

    // Hand-written notes, drawn so you can see what you pinned where.
    for (const auto& a : g_app.annotations) {
        ImVec2 p0(off.x + a.x * cell, off.y + a.y * cell);
        ImVec2 p1(off.x + (a.x + a.w) * cell, off.y + (a.y + a.h) * cell);
        dl->AddRectFilled(p0, p1, IM_COL32(90, 170, 220, 40));
        dl->AddRect(p0, p1, IM_COL32(120, 220, 255, 220), 0, 0, 2.0f);
        // A written-page mark, so a described area is distinguishable at a glance.
        float gr = std::min(std::min(p1.x - p0.x, p1.y - p0.y) * 0.28f, 22.0f);
        if (gr > 5.0f) {
            ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
            ImU32 col = IM_COL32(170, 235, 255, 220);
            dl->AddRect(ImVec2(c.x - gr * 0.7f, c.y - gr * 0.85f),
                        ImVec2(c.x + gr * 0.7f, c.y + gr * 0.85f), col, 0, 0,
                        std::max(1.5f, gr * 0.14f));
            for (int k = -1; k <= 1; ++k)
                dl->AddLine(ImVec2(c.x - gr * 0.42f, c.y + k * gr * 0.34f),
                            ImVec2(c.x + gr * 0.42f, c.y + k * gr * 0.34f), col,
                            std::max(1.0f, gr * 0.11f));
        }
        if (cell >= 9.0f && !a.label.empty()) {
            ImVec2 ts = ImGui::CalcTextSize(a.label.c_str());
            dl->AddRectFilled(ImVec2(p0.x + 2, p0.y + 2),
                              ImVec2(p0.x + ts.x + 8, p0.y + ts.y + 6),
                              IM_COL32(18, 34, 46, 220), 3.0f);
            dl->AddText(ImVec2(p0.x + 5, p0.y + 3), IM_COL32(160, 230, 255, 255),
                        a.label.c_str());
        }
    }

    // Area labels are UI only - they never reach the rendered image.
    // Area name chips. They are draggable handles, not decoration, so their
    // screen rectangles are kept for the hit-testing below.
    static std::vector<ImVec4> areaChips;
    areaChips.assign(g_app.map.areas.size(), ImVec4(0, 0, 0, 0));
    for (size_t i = 0; i < g_app.map.areas.size(); ++i) {
        const Area& a = g_app.map.areas[i];
        if (a.label.empty() || cell < 9.0f) continue;
        ImVec2 p(off.x + (a.x + a.w * 0.5f) * cell, off.y + (a.y + a.h * 0.5f) * cell);
        ImVec2 ts = ImGui::CalcTextSize(a.label.c_str());
        ImVec2 c0(p.x - ts.x * 0.5f - 5, p.y - ts.y * 0.5f - 3);
        ImVec2 c1(p.x + ts.x * 0.5f + 5, p.y + ts.y * 0.5f + 3);
        areaChips[i] = ImVec4(c0.x, c0.y, c1.x, c1.y);
        bool over = interactive && io.MousePos.x >= c0.x && io.MousePos.x <= c1.x &&
                    io.MousePos.y >= c0.y && io.MousePos.y <= c1.y;
        dl->AddRectFilled(c0, c1, over ? IM_COL32(58, 48, 30, 235)
                                       : IM_COL32(20, 22, 28, 200), 3.0f);
        if (over) dl->AddRect(c0, c1, IM_COL32(240, 220, 160, 220), 3.0f, 0, 1.5f);
        dl->AddText(ImVec2(p.x - ts.x * 0.5f, p.y - ts.y * 0.5f),
                    IM_COL32(240, 220, 160, 255), a.label.c_str());
    }

    // -- editing ------------------------------------------------------
    if (interactive && cell > 0.0f) {
        int cx = (int)std::floor((io.MousePos.x - off.x) / cell);
        int cy = (int)std::floor((io.MousePos.y - off.y) / cell);
        // Editing stops at the margin: it belongs to the renderer, not the map.
        bool inGrid = grid.Inside(cx, cy) && cx >= border && cy >= border &&
                      cx < grid.cols - border && cy < grid.rows - border;
        bool inMargin = grid.Inside(cx, cy) && !inGrid;

        if (hovered && inGrid) {
            ImVec2 a(off.x + cx * cell, off.y + cy * cell);
            dl->AddRect(a, ImVec2(a.x + cell, a.y + cell), IM_COL32(255, 214, 120, 220), 0, 0,
                        std::max(1.5f, cell * 0.06f));
        }

        // Whatever sits under the cursor. Used both for the hover name and for
        // the right-click menu, so the two can never disagree.
        int hitFeature = -1, hitEffect = -1, hitArea = -1;
        if (inGrid) {
            for (int i = 0; i < (int)g_app.features.size(); ++i)
                if (g_app.features[i].x == cx && g_app.features[i].y == cy) hitFeature = i;
            for (int i = 0; i < (int)g_app.effects.size(); ++i) {
                const Effect& e = g_app.effects[i];
                if (cx >= e.x && cx < e.x + e.w && cy >= e.y && cy < e.y + e.h) hitEffect = i;
            }
            for (int i = 0; i < (int)g_app.annotations.size(); ++i) {
                const Annotation& a = g_app.annotations[i];
                if (cx >= a.x && cx < a.x + a.w && cy >= a.y && cy < a.y + a.h) hitArea = i;
            }
        }

        // Which name chip is under the cursor.
        int hitChip = -1;
        for (int i = 0; i < (int)areaChips.size(); ++i) {
            const ImVec4& r = areaChips[i];
            if (r.z <= r.x) continue;
            if (io.MousePos.x >= r.x && io.MousePos.x <= r.z &&
                io.MousePos.y >= r.y && io.MousePos.y <= r.w) hitChip = i;
        }
        // Dragging a chip moves its area. It starts only on the chip, so it
        // never fights the painting tools.
        static int dragArea = -1;
        static int dragDX = 0, dragDY = 0;
        if (hovered && hitChip >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            g_app.PushUndo();
            dragArea = hitChip;
            dragDX = cx - g_app.map.areas[hitChip].x;
            dragDY = cy - g_app.map.areas[hitChip].y;
        }
        if (dragArea >= 0) {
            if (dragArea < (int)g_app.map.areas.size() &&
                ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                Area& a = g_app.map.areas[dragArea];
                a.x = std::clamp(cx - dragDX, 0, std::max(0, grid.cols - a.w));
                a.y = std::clamp(cy - dragDY, 0, std::max(0, grid.rows - a.h));
                g_app.MarkEdited();
                ImVec2 p0(off.x + a.x * cell, off.y + a.y * cell);
                ImVec2 p1(off.x + (a.x + a.w) * cell, off.y + (a.y + a.h) * cell);
                dl->AddRect(p0, p1, IM_COL32(240, 220, 160, 230), 0, 0,
                            std::max(2.0f, cell * 0.06f));
            }
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) dragArea = -1;
        }

        static int menuFeature = -1, menuEffect = -1, menuArea = -1;
        static int menuNamed = -1;
        static int menuX = 0, menuY = 0;
        static ImVec2 rightPress(0, 0);

        // Right-drag still pans, so only a right click that stayed put opens
        // the menu.
        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) rightPress = io.MousePos;
        if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
            float dx = io.MousePos.x - rightPress.x, dy = io.MousePos.y - rightPress.y;
            if (dx * dx + dy * dy < 25.0f) {
                menuFeature = hitFeature;
                menuEffect = hitEffect;
                menuArea = hitArea;
                menuNamed = hitChip;
                if (menuNamed < 0) {
                    // Right-clicking anywhere inside an area works too.
                    for (int i = 0; i < (int)g_app.map.areas.size(); ++i) {
                        const Area& a = g_app.map.areas[i];
                        if (cx >= a.x && cx < a.x + a.w && cy >= a.y && cy < a.y + a.h)
                            menuNamed = i;
                    }
                }
                menuX = cx;
                menuY = cy;
                ImGui::OpenPopup("##mapctx");
            }
        }

        // Hovering alone names what is there.
        if (hovered && inGrid && !ImGui::IsPopupOpen("##mapctx")) {
            std::string text;
            if (hitFeature >= 0) {
                const Feature& f = g_app.features[hitFeature];
                text = f.label.empty() ? PropLabel(f.kind) : f.label;
                if (!f.description.empty()) text += "\n" + f.description;
            }
            if (hitEffect >= 0) {
                const Effect& e = g_app.effects[hitEffect];
                if (!text.empty()) text += "\n";
                text += std::string("Effect: ") +
                        (e.label.empty() ? EffectLabel(e.kind) : e.label);
            }
            if (hitArea >= 0) {
                const Annotation& a = g_app.annotations[hitArea];
                if (!text.empty()) text += "\n";
                text += "Custom area: " + a.label;
            }
            if (text.empty()) text = TileName(grid.Get(cx, cy));
            ImGui::SetTooltip("%s\n[%d, %d] - right-click for options", text.c_str(), cx, cy);
        }

        if (hovered && hitChip >= 0 && hitChip < (int)g_app.map.areas.size() &&
            !ImGui::IsPopupOpen("##mapctx")) {
            const Area& a = g_app.map.areas[hitChip];
            ImGui::SetTooltip(
                "Area: %s%s%s\n"
                "This names a part of the map for the renderer. It is sent with this\n"
                "rectangle, so the painter knows which room is which - the words\n"
                "themselves are never drawn into the picture.\n"
                "Drag to move it. Right-click to rename, describe or delete it.",
                a.label.c_str(), a.description.empty() ? "" : " - ",
                a.description.c_str());
        }
        if (hovered && inMargin) {
            ImGui::SetTooltip(
                "Bleed margin - not part of your map\n"
                "Empty ground added outside your %d x %d field, never taken out of it.\n"
                "Image models are least reliable at the very edge of a picture, so\n"
                "whatever goes wrong there happens here instead of in one of your rooms.",
                grid.cols - 2 * border, grid.rows - 2 * border);
        }

        if (ImGui::BeginPopup("##mapctx")) {
            bool anything = false;
            if (menuFeature >= 0 && menuFeature < (int)g_app.features.size()) {
                anything = true;
                Feature& f = g_app.features[menuFeature];
                ImGui::SeparatorText(f.label.empty() ? PropLabel(f.kind) : f.label.c_str());
                if (f.label.empty()) {
                    ImGui::TextDisabled("From the catalogue.");
                } else {
                    // A custom object: everything about it is editable here.
                    ImGui::SetNextItemWidth(240.0f);
                    if (InputTextString("Name##ctxp", &f.label)) g_app.MarkEdited();
                    if (InputTextMultilineString("##ctxpd", &f.description, ImVec2(300, 58)))
                        g_app.MarkEdited();
                    int el = (int)f.elaboration;
                    ImGui::SetNextItemWidth(240.0f);
                    if (ImGui::Combo("Embellish##ctxp", &el,
                                     "No - exactly as written\0A little\0Freely\0")) {
                        f.elaboration = (Elaboration)el;
                        g_app.MarkEdited();
                    }
                }
                if (ImGui::MenuItem("Delete this object")) {
                    g_app.PushUndo();
                    g_app.features.erase(g_app.features.begin() + menuFeature);
                    menuFeature = -1;
                    ImGui::CloseCurrentPopup();
                }
            }
            if (menuEffect >= 0 && menuEffect < (int)g_app.effects.size()) {
                anything = true;
                Effect& e = g_app.effects[menuEffect];
                ImGui::SeparatorText(e.label.empty() ? EffectLabel(e.kind) : e.label.c_str());
                if (!e.label.empty()) {
                    ImGui::SetNextItemWidth(240.0f);
                    if (InputTextString("Name##ctxe", &e.label)) g_app.MarkEdited();
                    if (InputTextMultilineString("##ctxed", &e.description, ImVec2(300, 58)))
                        g_app.MarkEdited();
                    int el = (int)e.elaboration;
                    ImGui::SetNextItemWidth(240.0f);
                    if (ImGui::Combo("Embellish##ctxe", &el,
                                     "No - exactly as written\0A little\0Freely\0")) {
                        e.elaboration = (Elaboration)el;
                        g_app.MarkEdited();
                    }
                }
                int strength = e.intensity == "low" ? 0 : (e.intensity == "high" ? 2 : 1);
                ImGui::SetNextItemWidth(240.0f);
                if (ImGui::Combo("Strength##ctxe", &strength, "Faint\0Clear\0Heavy\0")) {
                    e.intensity = strength == 0 ? "low" : (strength == 2 ? "high" : "medium");
                    g_app.MarkEdited();
                }
                if (ImGui::MenuItem("Delete this effect")) {
                    g_app.PushUndo();
                    g_app.effects.erase(g_app.effects.begin() + menuEffect);
                    menuEffect = -1;
                    ImGui::CloseCurrentPopup();
                }
            }
            if (menuArea >= 0 && menuArea < (int)g_app.annotations.size()) {
                anything = true;
                Annotation& a = g_app.annotations[menuArea];
                ImGui::SeparatorText(a.label.empty() ? "Custom area" : a.label.c_str());
                ImGui::SetNextItemWidth(240.0f);
                if (InputTextString("Name##ctxa", &a.label)) g_app.MarkEdited();
                if (InputTextMultilineString("##ctxad", &a.description, ImVec2(300, 58)))
                    g_app.MarkEdited();
                int el = (int)a.elaboration;
                ImGui::SetNextItemWidth(240.0f);
                if (ImGui::Combo("Embellish##ctxa", &el,
                                 "No - exactly as written\0A little\0Freely\0")) {
                    a.elaboration = (Elaboration)el;
                    g_app.MarkEdited();
                }
                if (ImGui::MenuItem("Delete this area")) {
                    g_app.PushUndo();
                    g_app.annotations.erase(g_app.annotations.begin() + menuArea);
                    menuArea = -1;
                    ImGui::CloseCurrentPopup();
                }
            }
            if (menuNamed >= 0 && menuNamed < (int)g_app.map.areas.size()) {
                anything = true;
                Area& a = g_app.map.areas[menuNamed];
                ImGui::SeparatorText(a.label.empty() ? "Area" : a.label.c_str());
                ImGui::TextDisabled("Names this part of the map for the renderer.");
                ImGui::SetNextItemWidth(240.0f);
                if (InputTextString("Name##ctxar", &a.label)) g_app.MarkEdited();
                ImGui::SetItemTooltip("What this room is called. Sent to the renderer with "
                                      "the room's rectangle; never drawn in the picture.");
                if (InputTextMultilineString("##ctxard", &a.description, ImVec2(300, 58)))
                    g_app.MarkEdited();
                ImGui::SetItemTooltip("Anything else the painter should know about this "
                                      "room. Optional.");
                if (ImGui::MenuItem("Delete this area")) {
                    g_app.PushUndo();
                    g_app.map.areas.erase(g_app.map.areas.begin() + menuNamed);
                    menuNamed = -1;
                    ImGui::CloseCurrentPopup();
                }
            }
            if (!anything) {
                ImGui::SeparatorText(TileName(grid.Get(menuX, menuY)));
                ImGui::TextDisabled("Nothing placed on this square.");
                if (ImGui::MenuItem("Paint with this material")) {
                    g_app.paintTile = grid.Get(menuX, menuY);
                    g_app.tool = Tool::Paint;
                }
            }
            ImGui::EndPopup();
        }

        static bool strokeActive = false;
        static int rectStartX = 0, rectStartY = 0;
        bool leftDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        bool leftClicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        bool leftReleased = ImGui::IsMouseReleased(ImGuiMouseButton_Left);

        if (g_app.tool == Tool::Paint && inGrid) {
            if (leftClicked) { g_app.PushUndo(); strokeActive = true; }
            if (strokeActive && leftDown) {
                int r = g_app.brushSize - 1;
                for (int dy = -r; dy <= r; ++dy)
                    for (int dx = -r; dx <= r; ++dx) grid.Set(cx + dx, cy + dy, g_app.paintTile);
                g_app.MarkEdited();
            }
            if (leftReleased) strokeActive = false;
        } else if (g_app.tool == Tool::RectFill && inGrid) {
            if (leftClicked) { rectStartX = cx; rectStartY = cy; strokeActive = true; }
            if (strokeActive) {
                int x0 = std::min(rectStartX, cx), x1 = std::max(rectStartX, cx);
                int y0 = std::min(rectStartY, cy), y1 = std::max(rectStartY, cy);
                ImVec2 a(off.x + x0 * cell, off.y + y0 * cell);
                ImVec2 b(off.x + (x1 + 1) * cell, off.y + (y1 + 1) * cell);
                dl->AddRectFilled(a, b, TileColor(g_app.paintTile) & 0x60FFFFFF);
                dl->AddRect(a, b, IM_COL32(255, 214, 120, 255), 0, 0, 2.0f);
                // Stamp the material's own icon in the middle so it is obvious
                // what is about to be painted.
                float gr = std::min(std::min(b.x - a.x, b.y - a.y) * 0.30f, 26.0f);
                if (gr > 5.0f)
                    DrawTileGlyph(dl, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), gr,
                                  g_app.paintTile, IM_COL32(255, 236, 190, 235));
                if (leftReleased) {
                    g_app.PushUndo();
                    grid.FillRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1, g_app.paintTile);
                    strokeActive = false;
                    g_app.MarkEdited();
                }
            }
        } else if (g_app.tool == Tool::PlaceProp && inGrid) {
            if (leftClicked) {
                g_app.PushUndo();
                g_app.features.erase(
                    std::remove_if(g_app.features.begin(), g_app.features.end(),
                                   [&](const Feature& f) { return f.x == cx && f.y == cy; }),
                    g_app.features.end());
                Feature nf;
                if (g_app.customProp && !g_app.customLabel.empty()) {
                    nf.kind = "custom";
                    nf.label = g_app.customLabel;
                    nf.description = g_app.customDesc;
                    nf.elaboration = (Elaboration)g_app.customElaboration;
                    nf.structural = true;   // hand-placed, so it is pinned
                } else {
                    nf.kind = g_app.propKind;
                    nf.structural = arch::IsStructuralProp(nf.kind);
                }
                nf.x = cx;
                nf.y = cy;
                g_app.features.push_back(nf);
                g_app.MarkEdited();
            }
        } else if (g_app.tool == Tool::Annotate && inGrid) {
            if (leftClicked) { rectStartX = cx; rectStartY = cy; strokeActive = true; }
            if (strokeActive) {
                int x0 = std::min(rectStartX, cx), x1 = std::max(rectStartX, cx);
                int y0 = std::min(rectStartY, cy), y1 = std::max(rectStartY, cy);
                dl->AddRect(ImVec2(off.x + x0 * cell, off.y + y0 * cell),
                            ImVec2(off.x + (x1 + 1) * cell, off.y + (y1 + 1) * cell),
                            IM_COL32(120, 220, 255, 255), 0, 0, 2.5f);
                if (leftReleased) {
                    strokeActive = false;
                    if (!g_app.annLabel.empty()) {
                        g_app.PushUndo();
                        Annotation a;
                        a.label = g_app.annLabel;
                        a.description = g_app.annDesc;
                        a.elaboration = (Elaboration)g_app.annElaboration;
                        a.x = x0;
                        a.y = y0;
                        a.w = x1 - x0 + 1;
                        a.h = y1 - y0 + 1;
                        g_app.annotations.push_back(a);
                        g_app.MarkEdited();
                    }
                }
            }
        } else if (g_app.tool == Tool::Effects && inGrid) {
            if (leftClicked) { rectStartX = cx; rectStartY = cy; strokeActive = true; }
            if (strokeActive) {
                int x0 = std::min(rectStartX, cx), x1 = std::max(rectStartX, cx);
                int y0 = std::min(rectStartY, cy), y1 = std::max(rectStartY, cy);
                ImU32 tint = g_app.customEffect ? IM_COL32(200, 160, 255, 90)
                                                : EffectTint(g_app.effectKind);
                ImVec2 a(off.x + x0 * cell, off.y + y0 * cell);
                ImVec2 b(off.x + (x1 + 1) * cell, off.y + (y1 + 1) * cell);
                dl->AddRectFilled(a, b, tint);
                float gr = std::min(std::min(b.x - a.x, b.y - a.y) * 0.30f, 26.0f);
                if (gr > 5.0f && !g_app.customEffect)
                    DrawEffectGlyph(dl, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), gr,
                                    g_app.effectKind.c_str(), IM_COL32(255, 255, 255, 230));
                if (leftReleased) {
                    strokeActive = false;
                    bool custom = g_app.customEffect && !g_app.effLabel.empty();
                    if (custom || !g_app.customEffect) {
                        g_app.PushUndo();
                        Effect e;
                        e.kind = custom ? "custom" : g_app.effectKind;
                        if (custom) {
                            e.label = g_app.effLabel;
                            e.description = g_app.effDesc;
                            e.elaboration = (Elaboration)g_app.effElaboration;
                        }
                        e.intensity = g_app.effIntensity == 0 ? "low"
                                    : (g_app.effIntensity == 2 ? "high" : "medium");
                        e.x = x0;
                        e.y = y0;
                        e.w = x1 - x0 + 1;
                        e.h = y1 - y0 + 1;
                        g_app.effects.push_back(e);
                        g_app.MarkEdited();
                    }
                }
            }
        } else if (g_app.tool == Tool::EraseProp && inGrid) {
            if (leftDown && hovered) {
                size_t before = g_app.features.size();
                g_app.features.erase(
                    std::remove_if(g_app.features.begin(), g_app.features.end(),
                                   [&](const Feature& f) { return f.x == cx && f.y == cy; }),
                    g_app.features.end());
                if (g_app.features.size() != before) g_app.MarkEdited();
            }
        }
    }

    dl->PopClipRect();
}

// ---------------------------------------------------------------- tabs
// Parses "#RRGGBB" from a style's palette so each swatch actually looks like
// the style it stands for.
static ImU32 HexToCol(const std::string& hex, ImU32 fallback) {
    if (hex.size() < 7 || hex[0] != '#') return fallback;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int v[6];
    for (int i = 0; i < 6; ++i) {
        v[i] = nib(hex[(size_t)i + 1]);
        if (v[i] < 0) return fallback;
    }
    return IM_COL32(v[0] * 16 + v[1], v[2] * 16 + v[3], v[4] * 16 + v[5], 255);
}

// A scrolling catalogue of styles, each shown as its own palette. With eleven
// styles a dropdown made you read every name to find the one you wanted.
static void DrawStylePicker() {
    int perRow = 1;
    float cellW = 152.0f;
    GridMetrics(152.0f, perRow, cellW);
    const float cellH = 64.0f;
    // Grow with the window, but never eat the whole panel.
    float gridH = std::clamp(ImGui::GetContentRegionAvail().y * 0.40f, 150.0f, 460.0f);

    ImGui::BeginChild("##stylepicker", ImVec2(0, gridH), ImGuiChildFlags_Borders);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    int i = 0;
    for (const auto& kv : g_app.styles.styles) {
        if (i % perRow != 0) ImGui::SameLine();
        const StyleDef& st = kv.second;
        bool active = g_app.selectedStyle == kv.first;

        ImGui::PushID(i++);
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##s", ImVec2(cellW - 8.0f, cellH - 8.0f))) {
            g_app.selectedStyle = kv.first;
            g_app.map.meta.style = kv.first;
        }
        bool hovered = ImGui::IsItemHovered();
        if (hovered && !st.description.empty())
            ImGui::SetTooltip("%s\n%s", st.name.c_str(), st.description.c_str());

        ImVec2 p1(p0.x + cellW - 8.0f, p0.y + cellH - 8.0f);
        dl->AddRectFilled(p0, p1, hovered ? IM_COL32(48, 52, 62, 255)
                                          : IM_COL32(32, 35, 42, 255), 4.0f);

        // Palette strip: the style's own colours, so it reads at a glance.
        int swatches = (int)st.hex_palette.size();
        if (swatches > 0) {
            float w = (cellW - 16.0f) / swatches;
            for (int c = 0; c < swatches; ++c) {
                ImVec2 a(p0.x + 4.0f + c * w, p0.y + 4.0f);
                ImVec2 b(a.x + w - 1.0f, p0.y + 22.0f);
                dl->AddRectFilled(a, b, HexToCol(st.hex_palette[(size_t)c],
                                                 IM_COL32(120, 120, 120, 255)));
            }
        }
        if (active) dl->AddRect(p0, p1, IM_COL32(250, 200, 70, 255), 4.0f, 0, 2.5f);

        // Trim to the card's real width, so a wider window shows the full name.
        std::string name = FitText(st.name, cellW - 20.0f);
        dl->AddText(ImVec2(p0.x + 6.0f, p0.y + 28.0f),
                    active ? IM_COL32(250, 214, 120, 255) : IM_COL32(214, 212, 206, 255),
                    name.c_str());
        ImGui::PopID();
    }
    ImGui::EndChild();
}

static void TabCreate() {
    ImGui::BeginChild("##createleft", ImVec2(PanelWidth(0.34f, 420.0f, 900.0f), 0),
                      ImGuiChildFlags_Borders);

    ImGui::TextColored(AccentGold(), "1. Describe the scene");
    ImGui::TextWrapped("Write what the place looks like from above. Plain language is fine.");
    InputTextMultilineString("##scene", &g_app.sceneText, ImVec2(-1, 110));

    ImGui::Spacing();
    ImGui::TextColored(AccentGold(), "2. Pick a look");
    const StyleDef* style = g_app.styles.Find(g_app.selectedStyle);
    DrawStylePicker();
    if (style) {
        ImGui::TextColored(AccentGold(), "%s", style->name.c_str());
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", style->description.c_str());
        ImGui::PopTextWrapPos();
    }

    ImGui::Spacing();
    ImGui::TextColored(AccentGold(), "3. Size and shape");
    ImGui::SliderInt("Width", &g_app.cols, arch::kMinCells, arch::kMaxCells, "%d cells");
    ImGui::SetItemTooltip("How many squares across. One square is 5 feet.");
    ImGui::SliderInt("Height", &g_app.rows, arch::kMinCells, arch::kMaxCells, "%d cells");
    ImGui::SetItemTooltip("How many squares down. One square is 5 feet.");
    ImGui::TextDisabled("%d x %d cells  (%d x %d ft)", g_app.cols, g_app.rows,
                        g_app.cols * 5, g_app.rows * 5);
    if (ImGui::SmallButton("Small")) { g_app.cols = 17; g_app.rows = 13; }
    ImGui::SameLine();
    if (ImGui::SmallButton("Medium")) { g_app.cols = 25; g_app.rows = 19; }
    ImGui::SameLine();
    if (ImGui::SmallButton("Large")) { g_app.cols = 66; g_app.rows = 50; }
    ImGui::SameLine();
    if (ImGui::SmallButton("Huge")) { g_app.cols = 100; g_app.rows = 75; }
    ImGui::SameLine();
    if (ImGui::SmallButton("Giant")) { g_app.cols = 150; g_app.rows = 150; }
    ImGui::Combo("Layout", &g_app.layoutIndex, kLayoutNames, IM_ARRAYSIZE(kLayoutNames));
    HelpMarker("Leave on (from style) unless you want to force a particular shape, e.g. a "
               "harbour with a moored ship or an open forest with no walls.");

    int terrainIdx = 0, amountIdx = 1;
    for (int i = 0; i < IM_ARRAYSIZE(kTerrainNames); ++i)
        if (g_app.terrainKind == kTerrainNames[i]) terrainIdx = i;
    for (int i = 0; i < IM_ARRAYSIZE(kAmountNames); ++i)
        if (g_app.terrainAmount == kAmountNames[i]) amountIdx = i;
    if (ImGui::Combo("Ground hazard", &terrainIdx, kTerrainNames, IM_ARRAYSIZE(kTerrainNames)))
        g_app.terrainKind = kTerrainNames[terrainIdx];
    if (terrainIdx != 0) {
        if (ImGui::Combo("How much", &amountIdx, kAmountNames, IM_ARRAYSIZE(kAmountNames)))
            g_app.terrainAmount = kAmountNames[amountIdx];
    }

    ImGui::Checkbox("Random seed", &g_app.randomSeed);
    if (!g_app.randomSeed) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt("##seed", &g_app.seed);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    bool busy = g_app.job.running.load();
    ImGui::BeginDisabled(busy);
    ImGui::PushStyleColor(ImGuiCol_Button, GoButton());
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, GoButtonHovered());
    if (ImGui::Button("MAKE MY BATTLE MAP", ImVec2(-1, 46)))
        RequestRebuild(Rebuild::PlanAndRender);
    ImGui::PopStyleColor(2);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Plans the scene with the local AI, then paints it in ComfyUI. "
                        "Takes a few minutes.");
    ImGui::PopTextWrapPos();

    ImGui::Spacing();
    if (ImGui::Button("Blueprint only (instant, no AI)", ImVec2(-1, 30)))
        RequestRebuild(Rebuild::Blueprint);
    ImGui::SetItemTooltip("Builds the floor plan straight away without the language model. "
                          "Good for iterating on a layout before spending time on a render.");
    if (ImGui::Button("Plan with AI, do not render yet", ImVec2(-1, 30)))
        RequestRebuild(Rebuild::Plan);
    ImGui::SetItemTooltip("Runs only Stage 1, so you can review and edit the plan first.");
    ImGui::EndDisabled();

    if (busy) {
        ImGui::Spacing();
        if (ImGui::Button("Cancel", ImVec2(-1, 26))) g_app.job.cancel = true;
    }

    ImGui::Spacing();
    ImGui::Text("Status: %s", g_app.job.Status().c_str());
    DrawJobLog(160);

    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("##createright", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::TextColored(AccentGold(), "Blueprint preview");
    ImGui::TextDisabled("This is the plan, not the finished map. Edit it on the Editor tab.");
    DrawMapCanvas(false, ImVec2(0, ImGui::GetContentRegionAvail().y * 0.55f));
    ImGui::Separator();
    if (g_resultTex) {
        ImGui::TextColored(AccentGold(), "Finished battle map");
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float aspect = g_resultH ? (float)g_resultW / (float)g_resultH : 1.0f;
        float w = std::min(avail.x, avail.y * aspect);
        ImGui::Image((ImTextureID)g_resultTex, ImVec2(w, w / aspect));
    } else {
        ImGui::TextDisabled("The finished map will appear here.");
    }
    ImGui::EndChild();
}

// Tool pictograms. Drawn beside the name, never instead of it - the icon
// speeds up recognition, the word removes any doubt.
static void DrawToolGlyph(ImDrawList* dl, ImVec2 c, float r, int tool, ImU32 col) {
    float t = std::max(1.5f, r * 0.20f);
    switch (tool) {
    case 0:  // look around: four-way arrows
        dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), col, t);
        dl->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), col, t);
        dl->AddTriangleFilled(ImVec2(c.x + r, c.y), ImVec2(c.x + r * 0.4f, c.y - r * 0.4f),
                              ImVec2(c.x + r * 0.4f, c.y + r * 0.4f), col);
        dl->AddTriangleFilled(ImVec2(c.x - r, c.y), ImVec2(c.x - r * 0.4f, c.y - r * 0.4f),
                              ImVec2(c.x - r * 0.4f, c.y + r * 0.4f), col);
        break;
    case 1:  // brush: a bristled head on a handle
        dl->AddLine(ImVec2(c.x - r * 0.7f, c.y + r * 0.7f), ImVec2(c.x + r * 0.3f, c.y - r * 0.3f),
                    col, t);
        dl->AddTriangleFilled(ImVec2(c.x + r * 0.15f, c.y - r * 0.45f),
                              ImVec2(c.x + r, c.y - r * 0.9f),
                              ImVec2(c.x + r * 0.6f, c.y + r * 0.1f), col);
        break;
    case 2:  // rectangle
        dl->AddRect(ImVec2(c.x - r * 0.85f, c.y - r * 0.6f),
                    ImVec2(c.x + r * 0.85f, c.y + r * 0.6f), col, 0, 0, t);
        break;
    case 3:  // place prop: a barrel with a plus
        dl->AddCircle(ImVec2(c.x - r * 0.2f, c.y), r * 0.6f, col, 14, t);
        dl->AddLine(ImVec2(c.x + r * 0.55f, c.y - r * 0.5f),
                    ImVec2(c.x + r * 0.55f, c.y + r * 0.1f), col, t);
        dl->AddLine(ImVec2(c.x + r * 0.25f, c.y - r * 0.2f),
                    ImVec2(c.x + r * 0.85f, c.y - r * 0.2f), col, t);
        break;
    case 4:  // erase prop: a circle struck through
        dl->AddCircle(c, r * 0.7f, col, 14, t);
        dl->AddLine(ImVec2(c.x - r * 0.5f, c.y + r * 0.5f),
                    ImVec2(c.x + r * 0.5f, c.y - r * 0.5f), col, t);
        break;
    default: {  // note: a tagged rectangle
        dl->AddRect(ImVec2(c.x - r * 0.85f, c.y - r * 0.7f),
                    ImVec2(c.x + r * 0.5f, c.y + r * 0.7f), col, 0, 0, t);
        dl->AddLine(ImVec2(c.x - r * 0.5f, c.y - r * 0.25f),
                    ImVec2(c.x + r * 0.15f, c.y - r * 0.25f), col, t * 0.8f);
        dl->AddLine(ImVec2(c.x - r * 0.5f, c.y + r * 0.15f),
                    ImVec2(c.x + r * 0.15f, c.y + r * 0.15f), col, t * 0.8f);
        break;
    }
    }
}

// Material pictograms: bricks for wall, waves for water, and so on.
static void DrawTileGlyph(ImDrawList* dl, ImVec2 c, float r, Tile t, ImU32 col) {
    float w = std::max(1.5f, r * 0.18f);
    switch (t) {
    case Tile::Wall:  // running bond
        for (int i = 0; i < 3; ++i) {
            float y = c.y - r * 0.6f + i * r * 0.6f;
            dl->AddLine(ImVec2(c.x - r, y), ImVec2(c.x + r, y), col, w);
        }
        dl->AddLine(ImVec2(c.x - r * 0.4f, c.y - r * 0.6f),
                    ImVec2(c.x - r * 0.4f, c.y), col, w);
        dl->AddLine(ImVec2(c.x + r * 0.4f, c.y), ImVec2(c.x + r * 0.4f, c.y + r * 0.6f), col, w);
        break;
    case Tile::Door:  // leaf with a handle
        dl->AddRect(ImVec2(c.x - r * 0.55f, c.y - r * 0.8f),
                    ImVec2(c.x + r * 0.55f, c.y + r * 0.8f), col, 0, 0, w);
        dl->AddCircleFilled(ImVec2(c.x + r * 0.3f, c.y), r * 0.13f, col);
        break;
    case Tile::Window:  // a four-pane casement
        dl->AddRect(ImVec2(c.x - r * 0.75f, c.y - r * 0.62f),
                    ImVec2(c.x + r * 0.75f, c.y + r * 0.62f), col, 0, 0, w);
        dl->AddLine(ImVec2(c.x, c.y - r * 0.62f), ImVec2(c.x, c.y + r * 0.62f), col, w);
        dl->AddLine(ImVec2(c.x - r * 0.75f, c.y), ImVec2(c.x + r * 0.75f, c.y), col, w);
        break;
    case Tile::Water:  // two wave crests
        for (int i = 0; i < 2; ++i) {
            float y = c.y - r * 0.3f + i * r * 0.6f;
            for (int k = 0; k < 3; ++k) {
                float x0 = c.x - r + k * r * 0.7f;
                dl->AddBezierQuadratic(ImVec2(x0, y), ImVec2(x0 + r * 0.35f, y - r * 0.35f),
                                       ImVec2(x0 + r * 0.7f, y), col, w, 8);
            }
        }
        break;
    case Tile::Pit:  // hatched hole
        dl->AddCircle(c, r * 0.8f, col, 16, w);
        for (int i = -1; i <= 1; ++i)
            dl->AddLine(ImVec2(c.x + i * r * 0.4f - r * 0.3f, c.y + r * 0.6f),
                        ImVec2(c.x + i * r * 0.4f + r * 0.3f, c.y - r * 0.6f), col, w * 0.8f);
        break;
    case Tile::Rubble:  // scattered chips
        dl->AddCircleFilled(ImVec2(c.x - r * 0.45f, c.y + r * 0.2f), r * 0.2f, col);
        dl->AddCircleFilled(ImVec2(c.x + r * 0.1f, c.y - r * 0.35f), r * 0.16f, col);
        dl->AddCircleFilled(ImVec2(c.x + r * 0.5f, c.y + r * 0.35f), r * 0.22f, col);
        break;
    case Tile::Vegetation:  // three tufts
        for (int i = -1; i <= 1; ++i) {
            float x = c.x + i * r * 0.55f;
            dl->AddLine(ImVec2(x, c.y + r * 0.6f), ImVec2(x, c.y - r * 0.2f), col, w);
            dl->AddLine(ImVec2(x, c.y - r * 0.2f), ImVec2(x - r * 0.3f, c.y - r * 0.6f), col, w);
            dl->AddLine(ImVec2(x, c.y - r * 0.2f), ImVec2(x + r * 0.3f, c.y - r * 0.6f), col, w);
        }
        break;
    case Tile::Bridge:  // planks
        for (int i = 0; i < 4; ++i) {
            float y = c.y - r * 0.6f + i * r * 0.4f;
            dl->AddLine(ImVec2(c.x - r, y), ImVec2(c.x + r, y), col, w);
        }
        break;
    case Tile::Stairs:  // steps
        for (int i = 0; i < 4; ++i) {
            float y = c.y - r * 0.7f + i * r * 0.45f;
            float x = c.x - r + i * r * 0.4f;
            dl->AddLine(ImVec2(x, y), ImVec2(c.x + r, y), col, w);
        }
        break;
    case Tile::Void:  // eraser
        dl->AddRect(ImVec2(c.x - r * 0.8f, c.y - r * 0.5f),
                    ImVec2(c.x + r * 0.8f, c.y + r * 0.5f), col, 0, 0, w);
        dl->AddLine(ImVec2(c.x - r * 0.8f, c.y + r * 0.5f),
                    ImVec2(c.x + r * 0.8f, c.y - r * 0.5f), col, w);
        break;
    default:  // floor: flagstone joints
        dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), col, w);
        dl->AddLine(ImVec2(c.x - r * 0.35f, c.y - r * 0.7f),
                    ImVec2(c.x - r * 0.35f, c.y), col, w);
        dl->AddLine(ImVec2(c.x + r * 0.35f, c.y), ImVec2(c.x + r * 0.35f, c.y + r * 0.7f), col, w);
        break;
    }
}

// A button with a pictogram to the left of its label.
static bool IconButton(const char* id, const char* label, ImVec2 size,
                       const std::function<void(ImDrawList*, ImVec2, float, ImU32)>& glyph,
                       ImU32 glyphCol) {
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::Button(id, size);
    ImVec2 actual = ImGui::GetItemRectSize();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float r = std::min(actual.y, 26.0f) * 0.32f;
    glyph(dl, ImVec2(p0.x + 8.0f + r, p0.y + actual.y * 0.5f), r, glyphCol);
    ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p0.x + 16.0f + r * 2.0f, p0.y + (actual.y - ts.y) * 0.5f),
                ImGui::GetColorU32(ImGuiCol_Text), label);
    return pressed;
}

// A small pictogram per prop so the picker reads at a glance. The shapes match
// the ones drawn on the canvas and in the exported preview.
static void DrawPropGlyph(ImDrawList* dl, ImVec2 c, float r, const char* kind, ImU32 col) {
    std::string k = kind;
    float t = std::max(1.5f, r * 0.16f);
    auto ring = [&](float rad) { dl->AddCircle(c, rad, col, 16, t); };
    auto box = [&](float hw, float hh) {
        dl->AddRect(ImVec2(c.x - hw, c.y - hh), ImVec2(c.x + hw, c.y + hh), col, 0, 0, t);
    };
    if (k == "barrel" || k == "keg") { ring(r * 0.8f);
        dl->AddLine(ImVec2(c.x - r * 0.8f, c.y), ImVec2(c.x + r * 0.8f, c.y), col, t); }
    else if (k == "crate" || k == "chest" || k == "locker" || k == "cabinet" ||
             k == "dumpster") { box(r * 0.8f, r * 0.65f);
        dl->AddLine(ImVec2(c.x - r * 0.8f, c.y), ImVec2(c.x + r * 0.8f, c.y), col, t); }
    else if (k == "table" || k == "desk" || k == "bar" || k == "bench") box(r * 0.9f, r * 0.5f);
    else if (k == "chair") box(r * 0.4f, r * 0.4f);
    else if (k == "bed") { box(r * 0.55f, r * 0.9f);
        dl->AddLine(ImVec2(c.x - r * 0.55f, c.y - r * 0.4f),
                    ImVec2(c.x + r * 0.55f, c.y - r * 0.4f), col, t); }
    else if (k == "pillar" || k == "stalagmite" || k == "bollard" || k == "capstan" ||
             k == "mast") { ring(r * 0.8f); ring(r * 0.38f); }
    else if (k == "torch" || k == "lamp") { ring(r * 0.35f);
        dl->AddLine(ImVec2(c.x, c.y + r * 0.35f), ImVec2(c.x, c.y + r * 0.9f), col, t); }
    else if (k == "brazier" || k == "campfire" || k == "cauldron" || k == "hearth" ||
             k == "forge") { ring(r * 0.8f);
        for (int a = 0; a < 360; a += 60)
            dl->AddLine(c, ImVec2(c.x + r * 0.5f * cosf(a * 3.14159f / 180.0f),
                                  c.y + r * 0.5f * sinf(a * 3.14159f / 180.0f)), col, t); }
    else if (k == "well" || k == "fountain") { ring(r * 0.9f); ring(r * 0.45f); }
    else if (k == "bookshelf" || k == "weapon_rack") { box(r * 0.9f, r * 0.42f);
        for (int i = -1; i <= 1; ++i)
            dl->AddLine(ImVec2(c.x + i * r * 0.42f, c.y - r * 0.42f),
                        ImVec2(c.x + i * r * 0.42f, c.y + r * 0.42f), col, t * 0.7f); }
    else if (k == "statue" || k == "throne" || k == "portal")
        dl->AddTriangle(ImVec2(c.x, c.y - r * 0.85f), ImVec2(c.x + r * 0.7f, c.y + r * 0.6f),
                        ImVec2(c.x - r * 0.7f, c.y + r * 0.6f), col, t);
    else if (k == "crystal") {
        ImVec2 pts[4] = {ImVec2(c.x, c.y - r * 0.9f), ImVec2(c.x + r * 0.5f, c.y),
                         ImVec2(c.x, c.y + r * 0.9f), ImVec2(c.x - r * 0.5f, c.y)};
        dl->AddPolyline(pts, 4, col, ImDrawFlags_Closed, t);
    }
    else if (k == "altar" || k == "sarcophagus") { box(r * 0.85f, r * 0.55f);
        dl->AddLine(ImVec2(c.x, c.y - r * 0.4f), ImVec2(c.x, c.y + r * 0.4f), col, t); }
    else if (k == "tree" || k == "bush" || k == "mushroom") { ring(r * 0.9f); ring(r * 0.42f); }
    else if (k == "rope_coil" || k == "net") { ring(r * 0.85f); ring(r * 0.55f); ring(r * 0.25f); }
    else if (k == "cart" || k == "wagon" || k == "console") { box(r * 0.9f, r * 0.55f);
        dl->AddLine(ImVec2(c.x - r * 0.3f, c.y - r * 0.55f),
                    ImVec2(c.x - r * 0.3f, c.y + r * 0.55f), col, t * 0.7f); }
    else if (k == "bones") { ring(r * 0.35f);
        dl->AddLine(ImVec2(c.x - r * 0.8f, c.y + r * 0.5f),
                    ImVec2(c.x + r * 0.8f, c.y + r * 0.25f), col, t); }
    else { ring(r * 0.7f);
        dl->AddLine(ImVec2(c.x - r * 0.45f, c.y), ImVec2(c.x + r * 0.45f, c.y), col, t); }
}

// Effect pictograms, so the layer reads at a glance.
static void DrawEffectGlyph(ImDrawList* dl, ImVec2 c, float r, const char* kind, ImU32 col) {
    std::string k = kind;
    float t = std::max(1.5f, r * 0.16f);
    if (k == "fire" || k == "embers") {
        dl->AddTriangle(ImVec2(c.x, c.y - r), ImVec2(c.x + r * 0.7f, c.y + r * 0.7f),
                        ImVec2(c.x - r * 0.7f, c.y + r * 0.7f), col, t);
        dl->AddTriangleFilled(ImVec2(c.x, c.y - r * 0.3f), ImVec2(c.x + r * 0.3f, c.y + r * 0.6f),
                              ImVec2(c.x - r * 0.3f, c.y + r * 0.6f), col);
    } else if (k == "fog" || k == "mist" || k == "smoke" || k == "steam" || k == "ash") {
        for (int i = 0; i < 3; ++i) {
            float y = c.y - r * 0.5f + i * r * 0.5f;
            dl->AddBezierQuadratic(ImVec2(c.x - r, y), ImVec2(c.x, y - r * 0.5f),
                                   ImVec2(c.x + r, y), col, t, 10);
        }
    } else if (k == "fireflies" || k == "sparks") {
        const float px[5] = {-0.6f, 0.1f, 0.7f, -0.2f, 0.5f};
        const float py[5] = {-0.5f, -0.7f, -0.1f, 0.4f, 0.7f};
        for (int i = 0; i < 5; ++i)
            dl->AddCircleFilled(ImVec2(c.x + px[i] * r, c.y + py[i] * r), r * 0.15f, col);
    } else if (k == "magic_glow" || k == "holy_light") {
        dl->AddCircle(c, r * 0.4f, col, 14, t);
        for (int a = 0; a < 360; a += 45)
            dl->AddLine(ImVec2(c.x + r * 0.6f * cosf(a * 3.14159f / 180.0f),
                               c.y + r * 0.6f * sinf(a * 3.14159f / 180.0f)),
                        ImVec2(c.x + r * 0.95f * cosf(a * 3.14159f / 180.0f),
                               c.y + r * 0.95f * sinf(a * 3.14159f / 180.0f)), col, t);
    } else if (k == "poison_gas" || k == "shadow") {
        dl->AddCircle(c, r * 0.85f, col, 18, t);
        dl->AddCircle(ImVec2(c.x - r * 0.4f, c.y + r * 0.3f), r * 0.4f, col, 12, t);
    } else if (k == "ice") {
        for (int a = 0; a < 180; a += 60) {
            float rad = a * 3.14159f / 180.0f;
            dl->AddLine(ImVec2(c.x - r * cosf(rad), c.y - r * sinf(rad)),
                        ImVec2(c.x + r * cosf(rad), c.y + r * sinf(rad)), col, t);
        }
    } else if (k == "webs") {
        for (int a = 0; a < 360; a += 60)
            dl->AddLine(c, ImVec2(c.x + r * cosf(a * 3.14159f / 180.0f),
                                  c.y + r * sinf(a * 3.14159f / 180.0f)), col, t * 0.8f);
        dl->AddCircle(c, r * 0.5f, col, 12, t * 0.8f);
    } else if (k == "blood") {
        dl->AddCircleFilled(ImVec2(c.x - r * 0.2f, c.y + r * 0.2f), r * 0.5f, col);
        dl->AddCircleFilled(ImVec2(c.x + r * 0.45f, c.y - r * 0.35f), r * 0.22f, col);
    } else {
        dl->AddCircle(c, r * 0.7f, col, 16, t);
    }
}

static void DrawEffectPicker() {
    int perRow = 1;
    float cellW = 74.0f;
    GridMetrics(74.0f, perRow, cellW);
    const float cellH = 76.0f;
    float gridH = std::clamp(ImGui::GetContentRegionAvail().y - 120.0f, 180.0f, 900.0f);
    ImGui::BeginChild("##effectpicker", ImVec2(0, gridH), ImGuiChildFlags_Borders);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < IM_ARRAYSIZE(kEffects); ++i) {
        if (i % perRow != 0) ImGui::SameLine();
        const EffectInfo& info = kEffects[i];
        bool active = g_app.effectKind == info.kind;

        ImGui::PushID(3000 + i);
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##e", ImVec2(cellW - 6.0f, cellH - 6.0f)))
            g_app.effectKind = info.kind;
        bool hovered = ImGui::IsItemHovered();
        if (hovered) ImGui::SetTooltip("%s\n%s", info.label, info.hint);

        ImVec2 p1(p0.x + cellW - 6.0f, p0.y + cellH - 6.0f);
        if (active || hovered)
            dl->AddRectFilled(p0, p1, active ? IM_COL32(90, 70, 26, 255)
                                             : IM_COL32(52, 56, 66, 255), 4.0f);
        if (active) dl->AddRect(p0, p1, IM_COL32(250, 200, 70, 255), 4.0f, 0, 2.0f);
        DrawEffectGlyph(dl, ImVec2((p0.x + p1.x) * 0.5f, p0.y + 26.0f), 17.0f, info.kind,
                        IM_COL32(236, 226, 200, 255));
        std::string label = FitText(info.label, cellW - 10.0f);
        ImVec2 ts = ImGui::CalcTextSize(label.c_str());
        dl->AddText(ImVec2((p0.x + p1.x) * 0.5f - ts.x * 0.5f, p1.y - 18.0f),
                    IM_COL32(210, 208, 200, 255), label.c_str());
        ImGui::PopID();
    }
    ImGui::EndChild();
}

// Scrolling catalogue: as many icons per row as fit, each with its name and a
// one-line hint on hover.
static void DrawPropPicker() {
    int perRow = 1;
    float cellW = 74.0f;
    GridMetrics(74.0f, perRow, cellW);
    const float cellH = 76.0f;
    float gridH = std::clamp(ImGui::GetContentRegionAvail().y - 90.0f, 200.0f, 900.0f);

    ImGui::BeginChild("##proppicker", ImVec2(0, gridH), ImGuiChildFlags_Borders);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < IM_ARRAYSIZE(kProps); ++i) {
        if (i % perRow != 0) ImGui::SameLine();
        const PropInfo& info = kProps[i];
        bool active = g_app.propKind == info.kind;

        ImGui::PushID(i);
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##p", ImVec2(cellW - 6.0f, cellH - 6.0f)))
            g_app.propKind = info.kind;
        bool hovered = ImGui::IsItemHovered();
        if (hovered) ImGui::SetTooltip("%s\n%s", info.label, info.hint);

        ImVec2 p1(p0.x + cellW - 6.0f, p0.y + cellH - 6.0f);
        if (active || hovered)
            dl->AddRectFilled(p0, p1, active ? IM_COL32(90, 70, 26, 255)
                                             : IM_COL32(52, 56, 66, 255), 4.0f);
        if (active) dl->AddRect(p0, p1, IM_COL32(250, 200, 70, 255), 4.0f, 0, 2.0f);
        DrawPropGlyph(dl, ImVec2((p0.x + p1.x) * 0.5f, p0.y + 26.0f), 18.0f, info.kind,
                      IM_COL32(236, 226, 200, 255));
        std::string label = FitText(info.label, cellW - 10.0f);
        ImVec2 ts = ImGui::CalcTextSize(label.c_str());
        dl->AddText(ImVec2((p0.x + p1.x) * 0.5f - ts.x * 0.5f, p1.y - 18.0f),
                    IM_COL32(210, 208, 200, 255), label.c_str());
        ImGui::PopID();
    }

    // Anything in the Objects section of the wording file that the built-in
    // catalogue has never heard of. This is how somebody's own raspberry bush
    // becomes a thing you can place.
    int extra = IM_ARRAYSIZE(kProps);
    auto propWords = g_app.styles.phrases.sections.find("props");
    if (propWords != g_app.styles.phrases.sections.end()) {
        for (const auto& [kind, phrase] : propWords->second) {
            bool builtIn = false;
            for (const auto& info : kProps)
                if (kind == info.kind) builtIn = true;
            if (builtIn) continue;

            if (extra % perRow != 0) ImGui::SameLine();
            bool active = g_app.propKind == kind;
            ImGui::PushID(extra++);
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##pc", ImVec2(cellW - 6.0f, cellH - 6.0f)))
                g_app.propKind = kind;
            bool hov = ImGui::IsItemHovered();
            if (hov)
                ImGui::SetTooltip("%s\nYour own object. The renderer is told:\n%s",
                                  kind.c_str(), phrase.c_str());
            ImVec2 p1(p0.x + cellW - 6.0f, p0.y + cellH - 6.0f);
            if (active || hov)
                dl->AddRectFilled(p0, p1, active ? IM_COL32(90, 70, 26, 255)
                                                 : IM_COL32(52, 56, 66, 255), 4.0f);
            if (active) dl->AddRect(p0, p1, IM_COL32(250, 200, 70, 255), 4.0f, 0, 2.0f);
            // No drawn form for something we know nothing about - a marked
            // point, the same as a custom object placed by hand.
            ImVec2 c((p0.x + p1.x) * 0.5f, p0.y + 26.0f);
            dl->AddCircle(c, 13.0f, IM_COL32(236, 226, 200, 255), 16, 2.0f);
            dl->AddCircleFilled(c, 4.5f, IM_COL32(236, 226, 200, 255), 12);
            std::string label = FitText(kind, cellW - 10.0f);
            ImVec2 ts = ImGui::CalcTextSize(label.c_str());
            dl->AddText(ImVec2((p0.x + p1.x) * 0.5f - ts.x * 0.5f, p1.y - 18.0f),
                        IM_COL32(210, 208, 200, 255), label.c_str());
            ImGui::PopID();
        }
    }
    ImGui::EndChild();
}

static void TabEditor() {
    ImGui::BeginChild("##edtools", ImVec2(PanelWidth(0.20f, 240.0f, 620.0f), 0),
                      ImGuiChildFlags_Borders);
    ImGui::TextColored(AccentGold(), "Tools");

    const char* toolNames[] = {"Look around", "Brush", "Rectangle", "Place prop",
                               "Erase prop", "Custom area", "Effect"};
    const char* toolHints[] = {
        "Move around without changing anything.",
        "Paint the chosen material cell by cell.",
        "Drag out a rectangle and fill it with the chosen material.",
        "Click to drop a prop, or your own custom object.",
        "Click to remove a prop.",
        "Custom rectangle: drag one out and describe what belongs there in your own "
        "words - a barred gate, a collapsed wall, anything. The renderer is told exactly "
        "that, at exactly that spot.",
        "Drag a rectangle to lay fire, fog, fireflies and the like over the map. "
        "Effects are a top layer: they never change the ground or block movement."};
    for (int i = 0; i < IM_ARRAYSIZE(toolNames); ++i) {
        bool active = (int)g_app.tool == i;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.65f, 0.20f, 1.0f));
        ImGui::PushID(i);
        ImU32 gcol = active ? IM_COL32(30, 26, 12, 255) : IM_COL32(226, 222, 210, 255);
        if (IconButton("##tool", toolNames[i], ImVec2(-1, 30),
                       [i](ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
                           DrawToolGlyph(dl, c, r, i, col);
                       }, gcol))
            g_app.tool = (Tool)i;
        ImGui::SetItemTooltip("%s", toolHints[i]);
        ImGui::PopID();
        if (active) ImGui::PopStyleColor();
    }

    ImGui::Separator();
    if (g_app.tool == Tool::Paint || g_app.tool == Tool::RectFill) {
        ImGui::Text("Material");
        for (int i = 0; i < IM_ARRAYSIZE(kPaintTiles); ++i) {
            Tile t = TileFromName(kPaintTiles[i]);
            if (std::string(kPaintTiles[i]) == "void") t = Tile::Void;
            bool active = g_app.paintTile == t;
            ImVec4 col = ImGui::ColorConvertU32ToFloat4(TileColor(t));
            // Pick the label colour from the swatch's luminance so every entry
            // stays readable, including the near-black pit and void swatches.
            float lum = 0.299f * col.x + 0.587f * col.y + 0.114f * col.z;
            ImGui::PushStyleColor(ImGuiCol_Button, col);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                                  ImVec4(col.x * 1.25f + 0.06f, col.y * 1.25f + 0.06f,
                                         col.z * 1.25f + 0.06f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_Text, lum > 0.55f ? ImVec4(0.08f, 0.08f, 0.10f, 1.0f)
                                                             : ImVec4(0.96f, 0.96f, 0.98f, 1.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, active ? 2.5f : 0.0f);
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.98f, 0.78f, 0.28f, 1.0f));
            ImGui::PushID(i);
            ImU32 gcol = lum > 0.55f ? IM_COL32(20, 20, 24, 255) : IM_COL32(240, 240, 244, 255);
            if (IconButton("##mat", kPaintTiles[i], ImVec2(-1, 28),
                           [t](ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
                               DrawTileGlyph(dl, c, r, t, col);
                           }, gcol))
                g_app.paintTile = t;
            ImGui::PopID();
            ImGui::PopStyleColor(4);
            ImGui::PopStyleVar();
            ImGui::SetItemTooltip("%s", kTileHints[i]);
        }
        if (g_app.tool == Tool::Paint) ImGui::SliderInt("Brush size", &g_app.brushSize, 1, 6);
    } else if (g_app.tool == Tool::PlaceProp) {
        ImGui::Checkbox("Custom object", &g_app.customProp);
        ImGui::SetItemTooltip("Place something the catalogue does not have: you name it and "
                              "describe it, and the renderer is told exactly that.");
        if (g_app.customProp) {
            ImGui::Text("Name");
            InputTextString("##clabel", &g_app.customLabel);
            ImGui::SetItemTooltip("Short name, e.g. 'blood-stained altar'.");
            ImGui::Text("Description");
            InputTextMultilineString("##cdesc", &g_app.customDesc, ImVec2(-1, 70));
            ImGui::SetItemTooltip("What it looks like from above: materials, colour, damage.");
            ImGui::Text("Let the AI embellish");
            ImGui::Combo("##celab", &g_app.customElaboration,
                         "No - exactly as written\0A little\0Freely\0");
            ImGui::SetItemTooltip("How much licence the renderer gets with your description.");
            if (g_app.customLabel.empty())
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.5f, 1.0f), "Give it a name first.");
        } else {
            ImGui::Text("Prop: %s", g_app.propKind.c_str());
            DrawPropPicker();
        }
    } else if (g_app.tool == Tool::Annotate) {
        ImGui::TextWrapped("Drag a rectangle on the map, then describe what belongs there "
                           "in your own words. Use it for custom walls, doors, gates "
                           "or anything the material list has no word for.");
        ImGui::Text("Name");
        InputTextString("##alabel", &g_app.annLabel);
        ImGui::SetItemTooltip("Short name, e.g. 'barred iron gate' or 'collapsed wall'.");
        ImGui::Text("Description");
        InputTextMultilineString("##adesc", &g_app.annDesc, ImVec2(-1, 70));
        ImGui::SetItemTooltip("What it looks like from above.");
        ImGui::Text("Let the AI embellish");
        ImGui::Combo("##aelab", &g_app.annElaboration,
                     "No - exactly as written\0A little\0Freely\0");
        ImGui::SetItemTooltip("How much licence the renderer gets with your words.");
        if (g_app.annLabel.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.5f, 1.0f), "Give it a name first.");

        ImGui::Separator();
        ImGui::Text("Custom areas on this map: %d", (int)g_app.annotations.size());
        for (int i = 0; i < (int)g_app.annotations.size(); ++i) {
            ImGui::PushID(1000 + i);
            if (ImGui::SmallButton("x")) {
                g_app.PushUndo();
                g_app.annotations.erase(g_app.annotations.begin() + i);
                ImGui::PopID();
                break;
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(g_app.annotations[i].label.c_str());
            ImGui::PopID();
        }
    } else if (g_app.tool == Tool::Effects) {
        ImGui::TextWrapped("Drag a rectangle on the map to lay an effect over it.");
        ImGui::Combo("Strength", &g_app.effIntensity, "Faint\0Clear\0Heavy\0");
        ImGui::SetItemTooltip("How strongly the effect reads in the finished map.");
        ImGui::Checkbox("Custom effect", &g_app.customEffect);
        ImGui::SetItemTooltip("Describe an effect the catalogue does not have.");
        if (g_app.customEffect) {
            ImGui::Text("Name");
            InputTextString("##elabel", &g_app.effLabel);
            ImGui::Text("Description");
            InputTextMultilineString("##edesc", &g_app.effDesc, ImVec2(-1, 66));
            ImGui::Text("Let the AI embellish");
            ImGui::Combo("##eelab", &g_app.effElaboration,
                         "No - exactly as written\0A little\0Freely\0");
            if (g_app.effLabel.empty())
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.5f, 1.0f), "Give it a name first.");
        } else {
            DrawEffectPicker();
        }

        ImGui::Separator();
        ImGui::Text("Effects on this map: %d", (int)g_app.effects.size());
        for (int i = 0; i < (int)g_app.effects.size(); ++i) {
            ImGui::PushID(2000 + i);
            if (ImGui::SmallButton("x")) {
                g_app.PushUndo();
                g_app.effects.erase(g_app.effects.begin() + i);
                ImGui::PopID();
                break;
            }
            ImGui::SameLine();
            const Effect& e = g_app.effects[i];
            ImGui::TextUnformatted(e.label.empty() ? e.kind.c_str() : e.label.c_str());
            ImGui::PopID();
        }
    }
    ImGui::Separator();
    ImGui::BeginDisabled(g_app.undoStack.empty());
    if (ImGui::Button("Undo", ImVec2(-1, 26))) g_app.Undo();
    ImGui::EndDisabled();
    ImGui::BeginDisabled(g_app.redoStack.empty());
    if (ImGui::Button("Redo", ImVec2(-1, 26))) g_app.Redo();
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::Checkbox("Cell guides", &g_app.showCellGuides);
    ImGui::SetItemTooltip("Editor aid only. The grid is never drawn into the generated map - "
                          "your virtual tabletop draws its own.");
    ImGui::Checkbox("Show props", &g_app.showProps);
    if (ImGui::Button("Reset view", ImVec2(-1, 24))) {
        g_app.zoom = 1.0f;
        g_app.pan = ImVec2(40, 40);
    }

    ImGui::Separator();
    ImGui::Text("Map size");
    int cols = g_app.grid.cols, rows = g_app.grid.rows;
    ImGui::SetNextItemWidth(80);
    ImGui::InputInt("cols", &cols, 0);
    ImGui::SetNextItemWidth(80);
    ImGui::InputInt("rows", &rows, 0);
    if (ImGui::Button("Resize canvas", ImVec2(-1, 24))) {
        cols = std::clamp(cols, 11, 60);
        rows = std::clamp(rows, 9, 45);
        if (cols != g_app.grid.cols || rows != g_app.grid.rows) {
            g_app.PushUndo();
            TileGrid ng(cols, rows, Tile::Void);
            for (int y = 0; y < std::min(rows, g_app.grid.rows); ++y)
                for (int x = 0; x < std::min(cols, g_app.grid.cols); ++x)
                    ng.Set(x, y, g_app.grid.Get(x, y));
            g_app.grid = ng;
            g_app.features.erase(std::remove_if(g_app.features.begin(), g_app.features.end(),
                                                [&](const Feature& f) {
                                                    return f.x >= cols || f.y >= rows;
                                                }),
                                 g_app.features.end());
        }
    }

    ImGui::Separator();
    if (ImGui::Button("Rebuild walls", ImVec2(-1, 26))) {
        g_app.PushUndo();
        arch::DeriveWalls(g_app.grid);
    }
    ImGui::SetItemTooltip("Turns every empty cell touching open ground into a wall, so a "
                          "hand-drawn room ends up properly enclosed.");

    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("##edcanvas", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::Text("Left: use tool  |  Right-click: name and options  |  "
                "Right or middle drag: pan  |  Wheel: zoom");
    ImGui::SameLine();
    int hb = g_app.map.meta.border;
    if (hb > 0)
        ImGui::TextDisabled("(%d x %d field  +%d bleed)", g_app.grid.cols - 2 * hb,
                            g_app.grid.rows - 2 * hb, hb);
    else
        ImGui::TextDisabled("(%d x %d)", g_app.grid.cols, g_app.grid.rows);
    DrawMapCanvas(true, ImVec2(0, 0));
    ImGui::EndChild();
}

// One labelled paragraph of the caption.
static void CaptionField(const char* label, const nlohmann::json& obj, const char* key) {
    if (!obj.contains(key) || !obj[key].is_string()) return;
    ImGui::SeparatorText(label);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(obj[key].get<std::string>().c_str());
    ImGui::PopTextWrapPos();
}

// The palette as swatches - hex codes tell nobody anything.
static void DrawPaletteRow(const nlohmann::json& colours) {
    ImGui::SeparatorText("Palette");
    for (const auto& c : colours) {
        if (!c.is_string()) continue;
        unsigned int v = 0;
        std::string hex = c.get<std::string>();
        if (!hex.empty() && hex[0] == '#') hex.erase(0, 1);
        if (hex.size() != 6) continue;
        v = (unsigned int)strtoul(hex.c_str(), nullptr, 16);
        ImGui::ColorButton(("##sw" + hex).c_str(),
                           ImVec4(((v >> 16) & 0xFF) / 255.0f, ((v >> 8) & 0xFF) / 255.0f,
                                  (v & 0xFF) / 255.0f, 1.0f),
                           ImGuiColorEditFlags_NoTooltip | ImGuiColorEditFlags_NoDragDrop,
                           ImVec2(30, 22));
        ImGui::SameLine();
        ImGui::TextDisabled("%s", c.get<std::string>().c_str());
        ImGui::SameLine();
    }
    ImGui::NewLine();
}

// Parse once for the readable view; null means "not JSON yet".
static const nlohmann::json& ParsedOrEmpty(const std::string& text) {
    static std::string cached;
    static nlohmann::json parsed;
    if (text != cached) {
        cached = text;
        try {
            parsed = nlohmann::json::parse(text);
        } catch (const std::exception&) {
            parsed = nlohmann::json();
        }
    }
    return parsed;
}

// The caption is the whole instruction the painter receives, so it is worth
// showing properly - and worth letting somebody rewrite by hand when they know
// exactly what they want.
static void DrawCaptionPanel() {
    if (!ImGui::CollapsingHeader("Caption that will be sent",
                                 ImGuiTreeNodeFlags_DefaultOpen))
        return;

    g_app.SyncMapFromGrid();
    nlohmann::json auto_cap = IdeogramCaption::Build(
        g_app.map, g_app.styles.Find(g_app.map.meta.style), g_app.styles.base,
        g_app.styles.phrases);
    std::string autoText = auto_cap.dump(2);

    if (g_app.captionManual) {
        ImGui::TextColored(ImVec4(0.95f, 0.66f, 0.30f, 1.0f),
                           "Hand-written - the map no longer rewrites this");
    } else {
        ImGui::TextDisabled("Rebuilt from the plan every time you change it.");
    }

    if (g_app.captionManual) {
        if (ImGui::Button("Back to automatic")) {
            g_app.captionManual = false;
            g_app.captionText.clear();
        }
        ImGui::SetItemTooltip("Throw the hand-written version away and go back to the "
                              "caption built from the plan.");
        ImGui::SameLine();
        if (ImGui::Button("Reload from plan")) g_app.captionText = autoText;
        ImGui::SetItemTooltip("Replace what you typed with a fresh caption built from the "
                              "current plan.");
    } else {
        if (ImGui::Button("Edit by hand")) {
            g_app.captionManual = true;
            g_app.captionText = autoText;
        }
        ImGui::SetItemTooltip("Take the caption over and write it yourself. It is then sent "
                              "exactly as you leave it.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy")) {
        ImGui::SetClipboardText(g_app.captionManual ? g_app.captionText.c_str()
                                                     : autoText.c_str());
    }
    ImGui::SetItemTooltip("Copy the whole caption to the clipboard.");

    // Height follows the window, because reading JSON through a letterbox is
    // what made this panel useless before.
    float h = std::clamp(ImGui::GetContentRegionAvail().y - 190.0f, 200.0f, 900.0f);

    if (ImGui::BeginTabBar("##captabs")) {
        if (ImGui::BeginTabItem("Readable")) {
            const nlohmann::json& j = g_app.captionManual ? ParsedOrEmpty(g_app.captionText)
                                                          : auto_cap;
            ImGui::BeginChild("##capread", ImVec2(0, h), ImGuiChildFlags_Borders);
            if (j.is_null()) {
                ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.40f, 1.0f),
                                   "Not valid JSON yet - see the Raw tab.");
            } else {
                CaptionField("Scene", j, "high_level_description");
                if (j.contains("style_description") && j["style_description"].is_object()) {
                    const auto& sd = j["style_description"];
                    CaptionField("Look", sd, "aesthetics");
                    CaptionField("Light", sd, "lighting");
                    CaptionField("Medium", sd, "medium");
                    if (sd.contains("color_palette") && sd["color_palette"].is_array())
                        DrawPaletteRow(sd["color_palette"]);
                }
                if (j.contains("compositional_deconstruction")) {
                    const auto& cd = j["compositional_deconstruction"];
                    CaptionField("Ground", cd, "background");
                    if (cd.contains("elements") && cd["elements"].is_array()) {
                        ImGui::SeparatorText("Placed objects");
                        int n = 0;
                        for (const auto& e : cd["elements"]) {
                            ++n;
                            std::string box = "no box";
                            if (e.contains("bbox") && e["bbox"].is_array() &&
                                e["bbox"].size() == 4) {
                                box = "y " + std::to_string((int)e["bbox"][0]) + "-" +
                                      std::to_string((int)e["bbox"][2]) + "  x " +
                                      std::to_string((int)e["bbox"][1]) + "-" +
                                      std::to_string((int)e["bbox"][3]);
                            }
                            ImGui::TextColored(AccentGold(), "%2d.  %s",
                                               n, box.c_str());
                            ImGui::SameLine();
                            ImGui::PushTextWrapPos(0.0f);
                            ImGui::TextUnformatted(e.value("desc", std::string()).c_str());
                            ImGui::PopTextWrapPos();
                            ImGui::Spacing();
                        }
                        ImGui::TextDisabled("%d objects, %d of them positioned.", n,
                                            (int)std::count_if(
                                                cd["elements"].begin(), cd["elements"].end(),
                                                [](const nlohmann::json& e) {
                                                    return e.contains("bbox");
                                                }));
                    }
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Raw JSON")) {
            if (g_app.captionManual) {
                InputTextMultilineString("##capraw", &g_app.captionText, ImVec2(-1, h));
                bool valid = !ParsedOrEmpty(g_app.captionText).is_null();
                if (valid)
                    ImGui::TextColored(ImVec4(0.45f, 0.80f, 0.45f, 1.0f),
                                       "Valid JSON, %d characters.",
                                       (int)g_app.captionText.size());
                else
                    ImGui::TextColored(ImVec4(0.95f, 0.45f, 0.40f, 1.0f),
                                       "Not valid JSON. It will still be sent as typed, but "
                                       "the model expects a JSON object.");
            } else {
                ImGui::BeginChild("##capraw2", ImVec2(0, h), ImGuiChildFlags_Borders);
                ImGui::TextUnformatted(autoText.c_str());
                ImGui::EndChild();
                ImGui::TextDisabled("Press \"Edit by hand\" above to change this directly.");
            }
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

static void TabRender() {
    ImGui::BeginChild("##renderleft", ImVec2(PanelWidth(0.34f, 460.0f, 900.0f), 0),
                      ImGuiChildFlags_Borders);

    ComfyConfig& c = g_app.config.comfy;
    ImGui::TextColored(AccentGold(), "Renderer: Ideogram 4");
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("The layout is sent as bounding boxes inside a JSON caption, so the "
                        "plan is followed exactly and no blueprint image is needed.");
    ImGui::PopTextWrapPos();

    static const char* kPresetItems[] = {"Quality - 48 steps", "Default - 20 steps",
                                         "Turbo - 12 steps"};
    int preset = c.preset == "Quality" ? 0 : (c.preset == "Turbo" ? 2 : 1);
    if (ImGui::Combo("Quality", &preset, kPresetItems, IM_ARRAYSIZE(kPresetItems)))
        c.preset = preset == 0 ? "Quality" : (preset == 2 ? "Turbo" : "Default");
    ImGui::SetItemTooltip("How many painting steps. Quality is slow and best; Turbo is for "
                          "trying an idea out.");
    ImGui::SliderFloat("Guidance", &c.cfg, 1.0f, 12.0f, "%.1f");
    ImGui::SetItemTooltip("How literally the caption is followed. Higher sticks to the "
                          "description, lower lets the model improvise.");
    ImGui::SliderFloat("Megapixels", &c.megapixels, 0.6f, 3.0f, "%.1f");

    bool fixedSeed = c.seed >= 0;
    if (ImGui::Checkbox("Fixed seed", &fixedSeed)) c.seed = fixedSeed ? 12345 : -1;
    if (fixedSeed) {
        ImGui::SameLine();
        int sd = (int)c.seed;
        ImGui::SetNextItemWidth(140);
        if (ImGui::InputInt("##seed", &sd)) c.seed = std::max(0, sd);
    }

    ImGui::Spacing();
    bool busy = g_app.job.running.load();
    ImGui::BeginDisabled(busy || g_app.grid.cols <= 0);
    ImGui::PushStyleColor(ImGuiCol_Button, GoButton());
    if (ImGui::Button("RENDER THIS MAP", ImVec2(-1, 40))) StartRenderCurrent();
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    if (busy && ImGui::Button("Cancel", ImVec2(-1, 24))) g_app.job.cancel = true;

    ImGui::Text("Status: %s", g_app.job.Status().c_str());

    ImGui::Separator();
    DrawCaptionPanel();

    DrawJobLog(140);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##renderright", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (g_resultTex) {
        ImGui::Text("Result: %d x %d px", g_resultW, g_resultH);
        ImGui::SameLine();
        if (ImGui::Button("Save a copy")) {
            std::string dir = OutputDir(g_app.map.meta.name);
            std::string path = dir + "/battlemap_copy.png";
            std::ofstream f(path, std::ios::binary);
            f.write((const char*)g_resultPng.data(), (std::streamsize)g_resultPng.size());
            g_app.job.Log("Copy saved to " + path);
        }
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float aspect = g_resultH ? (float)g_resultW / (float)g_resultH : 1.0f;
        float w = std::min(avail.x, avail.y * aspect);
        ImGui::Image((ImTextureID)g_resultTex, ImVec2(w, w / aspect));
    } else {
        ImGui::TextDisabled("No render yet.");
        ImGui::Separator();
        ImGui::TextColored(AccentGold(), "Plan that will be rendered");
        DrawMapCanvas(false, ImVec2(0, 0));
    }
    ImGui::EndChild();
}

// What each section of the wording file is for, in the user's words.
struct PhraseSection {
    const char* key;
    const char* title;
    const char* hint;
};

static const PhraseSection kPhraseSections[] = {
    {"structure", "Structure",
     "Doors, windows, walls, open ground and the ship. These are the load-bearing "
     "descriptions: each is sent with the exact rectangle it applies to."},
    {"phrasing", "Wording",
     "The sentences bolted onto other descriptions - how strictly a rectangle is meant, "
     "how freely the AI may embellish something, how strong an effect is."},
    {"terrain", "Terrain",
     "Water, pits, rubble and undergrowth."},
    {"effects", "Effects",
     "The atmospheric layer: fire, fog, fireflies and the rest."},
    {"props", "Objects",
     "Every object the renderer is given a concrete description for. Anything not listed "
     "here is still named, but left to the painter's judgement."},
};

// The wording every caption is built from. It lives in styles/_phrases.json, and
// this is where it is edited - the phrases are the first thing anybody will want
// to change and the last thing they should have to edit a file for.
static void DrawPhraseEditor() {
    static std::string filter;
    static bool dirty = false;

    ImGui::TextColored(AccentGold(), "What the renderer is told about each kind of thing");
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled(
        "Every phrase below goes into the caption as a description of one object, joined to "
        "the rectangle it sits in. Write them as descriptions, not as commands. A style file "
        "can override door, window, wall and ground for itself.");
    ImGui::PopTextWrapPos();

    ImGui::SetNextItemWidth(320.0f);
    InputTextString("Find", &filter);
    ImGui::SameLine();
    ImGui::BeginDisabled(!dirty);
    if (ImGui::Button("Save wording")) {
        if (g_app.styles.SavePhrases()) {
            g_app.job.Log("Saved styles/_phrases.json");
            dirty = false;
        } else {
            g_app.job.Log("Could not save the wording: " + g_app.styles.lastError);
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Reload from file")) {
        g_app.styles.LoadPhrases();
        dirty = false;
    }
    ImGui::SetItemTooltip("Throw away unsaved changes and read the file again.");
    if (dirty) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.66f, 0.30f, 1.0f), "unsaved");
    }

    std::string needle = filter;
    for (char& c : needle) c = (char)tolower((unsigned char)c);

    // Adding your own object, which is the whole point of this being a file.
    ImGui::Separator();
    static std::string newKey, newPhrase;
    ImGui::TextColored(AccentGold(), "Add your own object");
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled(
        "Give it a short name with no spaces - raspberry_bush - and describe what the "
        "renderer should draw. It then appears in the prop catalogue on the Editor tab like "
        "any built-in object, and is drawn from your description every time.");
    ImGui::PopTextWrapPos();
    ImGui::SetNextItemWidth(220.0f);
    InputTextString("Name##newprop", &newKey);
    ImGui::SetNextItemWidth(-1.0f);
    InputTextMultilineString("##newpropdesc", &newPhrase, ImVec2(-1, 46));
    bool named = !newKey.empty() && !newPhrase.empty();
    ImGui::BeginDisabled(!named);
    if (ImGui::Button("Add to the catalogue")) {
        std::string key;
        for (char c : newKey) key += (c == ' ' ? '_' : (char)tolower((unsigned char)c));
        g_app.styles.phrases.sections["props"][key] = newPhrase;
        if (g_app.styles.SavePhrases()) {
            g_app.job.Log("Added object '" + key + "' to styles/_phrases.json");
            newKey.clear();
            newPhrase.clear();
        } else {
            g_app.job.Log("Could not save: " + g_app.styles.lastError);
        }
    }
    ImGui::EndDisabled();
    if (!named) {
        ImGui::SameLine();
        ImGui::TextDisabled("give it a name and a description first");
    }
    ImGui::Separator();

    ImGui::BeginChild("##phrases", ImVec2(0, 0), ImGuiChildFlags_Borders);
    for (const PhraseSection& sec : kPhraseSections) {
        auto it = g_app.styles.phrases.sections.find(sec.key);
        if (it == g_app.styles.phrases.sections.end() || it->second.empty()) continue;

        // Count what survives the filter before drawing a header for nothing.
        int shown = 0;
        for (const auto& kv : it->second) {
            if (needle.empty()) { ++shown; continue; }
            std::string hay = kv.first + " " + kv.second;
            for (char& c : hay) c = (char)tolower((unsigned char)c);
            if (hay.find(needle) != std::string::npos) ++shown;
        }
        if (!shown) continue;

        if (!ImGui::CollapsingHeader(sec.title, needle.empty() && std::string(sec.key) ==
                                                        "structure"
                                                    ? ImGuiTreeNodeFlags_DefaultOpen
                                                    : (needle.empty() ? 0
                                                                      : ImGuiTreeNodeFlags_DefaultOpen)))
            continue;
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", sec.hint);
        ImGui::PopTextWrapPos();

        for (auto& kv : it->second) {
            if (!needle.empty()) {
                std::string hay = kv.first + " " + kv.second;
                for (char& c : hay) c = (char)tolower((unsigned char)c);
                if (hay.find(needle) == std::string::npos) continue;
            }
            ImGui::PushID(kv.first.c_str());
            float h = kv.second.size() > 160 ? 76.0f : (kv.second.size() > 70 ? 54.0f : 30.0f);
            ImGui::TextColored(AccentGold(), "%s", kv.first.c_str());
            if (InputTextMultilineString("##v", &kv.second, ImVec2(-1, h))) dirty = true;
            ImGui::PopID();
        }
        ImGui::Spacing();
    }
    if (g_app.styles.phrases.sections.empty()) {
        ImGui::TextDisabled("styles/_phrases.json is missing, so the built-in wording is in "
                            "use. Reinstall the styles folder to edit it here.");
    }
    ImGui::EndChild();
}

static void TabStyles() {
    static bool editingPhrases = false;
    static std::string editingId;
    static StyleDef editing;
    static bool editingBase = false;

    ImGui::BeginChild("##stylelist", ImVec2(PanelWidth(0.22f, 250.0f, 560.0f), 0),
                      ImGuiChildFlags_Borders);
    ImGui::TextColored(AccentGold(), "Styles");
    ImGui::TextDisabled("One JSON file each, in styles/");
    for (const auto& kv : g_app.styles.styles) {
        if (ImGui::Selectable(kv.second.name.c_str(), editingId == kv.first && !editingBase)) {
            editingId = kv.first;
            editing = kv.second;
            editingBase = false;
            editingPhrases = false;
        }
    }
    ImGui::Separator();
    if (ImGui::Selectable("Shared caption contract", editingBase && !editingPhrases)) {
        editingBase = true;
        editingPhrases = false;
    }
    ImGui::SetItemTooltip("Merged into every caption. The 'forbidden' line is the only thing "
                          "keeping text and creatures out - Ideogram takes no negative prompt.");
    if (ImGui::Selectable("Object wording", editingPhrases)) {
        editingBase = true;
        editingPhrases = true;
    }
    ImGui::SetItemTooltip("What the renderer is told about a door, a wall, a barrel, a fire - "
                          "every kind of thing the caption can name.");
    ImGui::Separator();
    if (ImGui::Button("New style", ImVec2(-1, 26))) {
        editing = StyleDef{};
        editing.id = "custom_" + std::to_string(g_app.styles.styles.size() + 1);
        editing.name = "New style";
        editing.category = "Custom";
        editing.materials = "Describe the materials, surfaces and colours of this place.";
        editing.ground = "worn stone paving";
        editingId = editing.id;
        editingBase = false;
    }
    if (ImGui::Button("Reload from disk", ImVec2(-1, 26))) g_app.styles.LoadAll();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##styleedit", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (editingPhrases) {
        DrawPhraseEditor();
    } else if (editingBase) {
        ImGui::TextColored(AccentGold(), "Shared caption contract");
        ImGui::Text("Never rendered (this is what bans text and creatures)");
        InputTextMultilineString("##bf", &g_app.styles.base.forbidden_suffix, ImVec2(-1, 90));
        ImGui::Text("Default aesthetics");
        InputTextMultilineString("##ba", &g_app.styles.base.aesthetics, ImVec2(-1, 110));
        ImGui::Text("Medium");
        InputTextMultilineString("##bm", &g_app.styles.base.medium, ImVec2(-1, 60));
        ImGui::Text("Default lighting");
        InputTextMultilineString("##bl", &g_app.styles.base.lighting, ImVec2(-1, 60));
        ImGui::Text("Background suffix");
        InputTextMultilineString("##bg", &g_app.styles.base.background_suffix, ImVec2(-1, 80));
        if (ImGui::Button("Save contract", ImVec2(200, 30))) g_app.styles.SaveBase();
    } else if (!editingId.empty()) {
        ImGui::TextColored(AccentGold(), "Editing: %s", editing.id.c_str());
        InputTextString("Display name", &editing.name);
        InputTextString("Category", &editing.category);
        InputTextString("Description", &editing.description);
        InputTextString("Palette", &editing.palette);
        InputTextString("Lighting", &editing.lighting);
        InputTextString("Default layout", &editing.default_layout);
        ImGui::Text("What this place is made of");
        InputTextMultilineString("##sp", &editing.materials, ImVec2(-1, 130));
        InputTextString("Ground surface", &editing.ground);
        ImGui::SetItemTooltip("Describes the floor under everything - it becomes the caption "
                              "background.");

        ImGui::Text("Props the architect may scatter");
        std::string joined;
        for (const auto& p : editing.props) joined += (joined.empty() ? "" : ", ") + p;
        if (InputTextString("##props", &joined)) {
            editing.props.clear();
            std::string cur;
            for (char ch : joined + ",") {
                if (ch == ',') {
                    while (!cur.empty() && cur.front() == ' ') cur.erase(cur.begin());
                    while (!cur.empty() && cur.back() == ' ') cur.pop_back();
                    if (!cur.empty()) editing.props.push_back(cur);
                    cur.clear();
                } else cur.push_back(ch);
            }
        }

        if (ImGui::Button("Save style", ImVec2(160, 30))) g_app.styles.SaveStyle(editing);
        ImGui::SameLine();
        if (ImGui::Button("Delete style", ImVec2(160, 30))) {
            g_app.styles.DeleteStyle(editing.id);
            editingId.clear();
        }
    } else {
        ImGui::TextDisabled("Pick a style on the left.");
    }
    ImGui::EndChild();
}

static void TabSettings() {
    ImGui::BeginChild("##settings", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::TextColored(AccentGold(), "Local services");

    InputTextString("Ollama address", &g_app.config.ollama.base_url);
    if (!g_app.ollamaModels.empty()) {
        if (ImGui::BeginCombo("Model", g_app.config.ollama.model.c_str())) {
            for (const auto& m : g_app.ollamaModels)
                if (ImGui::Selectable(m.c_str(), m == g_app.config.ollama.model))
                    g_app.config.ollama.model = m;
            ImGui::EndCombo();
        }
    } else {
        InputTextString("Model", &g_app.config.ollama.model);
    }
    ImGui::SliderFloat("Planner creativity", &g_app.config.ollama.temperature, 0.0f, 1.2f, "%.2f");
    ImGui::TextColored(g_app.ollamaOk ? ImVec4(0.45f, 0.9f, 0.55f, 1) : ImVec4(1, 0.5f, 0.45f, 1),
                       "Ollama: %s", g_app.ollamaStatus.c_str());

    ImGui::Spacing();
    InputTextString("ComfyUI address", &g_app.config.comfy.base_url);
    ImGui::TextColored(g_app.comfyOk ? ImVec4(0.45f, 0.9f, 0.55f, 1) : ImVec4(1, 0.5f, 0.45f, 1),
                       "ComfyUI: %s", g_app.comfyStatus.c_str());

    ImGui::Spacing();
    ImGui::BeginDisabled(g_app.job.running.load());
    if (ImGui::Button("Test both connections", ImVec2(240, 32))) StartConnectionCheck();
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::TextColored(AccentGold(), "Maps");
    ImGui::SliderInt("Bleed margin", &g_app.config.border_cells, 0, 6, "%d cells");
    ImGui::SetItemTooltip(
        "An empty ring added around every new map. It is added outside the size you pick, "
        "never taken out of it.\n"
        "Image models are least reliable at the very edge of a picture, so the margin is "
        "where their mistakes go instead of into one of your rooms.\n"
        "Set it to 0 if you want the map bled right to the edge.");

    ImGui::Separator();
    ImGui::TextColored(AccentGold(), "Ideogram 4 models in ComfyUI");
    InputTextString("Diffusion model", &g_app.config.comfy.unet);
    InputTextString("Unconditional model", &g_app.config.comfy.unet_uncond);
    ImGui::SetItemTooltip("Required. It supplies the negative half of classifier-free "
                          "guidance; without it renders come out washed out.");
    InputTextString("Text encoder", &g_app.config.comfy.clip);
    InputTextString("VAE", &g_app.config.comfy.vae);

    ImGui::Separator();
    InputTextString("Output folder", &g_app.config.output_dir);

    ImGui::Spacing();
    if (ImGui::Button("Save settings", ImVec2(200, 32))) {
        std::string err;
        if (ConfigStore::Save(g_app.configPath, g_app.config, err))
            g_app.job.Log("Settings saved to " + g_app.configPath);
        else
            g_app.job.Log("Could not save settings: " + err);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload settings", ImVec2(200, 32))) {
        std::string err;
        ConfigStore::Load(g_app.configPath, g_app.config, err);
    }

    ImGui::Separator();
    ImGui::TextDisabled(
        "These settings live in config.json next to the executable and are shared with the "
        "Python command line tools.");
    DrawJobLog(140);
    ImGui::EndChild();
}

// One row of the open dialog: a real thumbnail of the plan, so a map is picked
// by looking at it rather than by reading a folder name.
struct MapEntry {
    std::string path;
    std::string folder;
    std::string title;
    int cols = 0, rows = 0;
    ID3D11ShaderResourceView* thumb = nullptr;
    int thumbW = 0, thumbH = 0;
};

static std::vector<MapEntry> g_mapEntries;

static void ReleaseMapEntries() {
    for (auto& e : g_mapEntries)
        if (e.thumb) e.thumb->Release();
    g_mapEntries.clear();
}

// Built once when the dialog opens: scanning and rasterizing every plan on each
// frame would be far too slow.
// The native open dialog. Worth the Win32 for this one job: an agent writes a
// plan wherever it likes, and asking somebody to type that path is not a
// serious answer.
static HWND g_hwnd = nullptr;

static std::string PickMapFile() {
    wchar_t buf[MAX_PATH] = L"";
    OPENFILENAMEW ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = g_hwnd;
    ofn.lpstrFilter = L"Map plans (map.json)\0*.json\0All files\0*.*\0";
    ofn.lpstrFile = buf;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = L"Open a map.json";
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_NOCHANGEDIR;
    if (!GetOpenFileNameW(&ofn)) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, buf, -1, nullptr, 0, nullptr, nullptr);
    std::string out((size_t)std::max(0, n - 1), 0);
    WideCharToMultiByte(CP_UTF8, 0, buf, -1, out.data(), n, nullptr, nullptr);
    return out;
}

// What somebody actually pastes: a quoted path from Explorer, a folder rather
// than the file inside it, a trailing newline. Accept all of it.
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

static void ScanMaps() {
    ReleaseMapEntries();
    std::error_code ec;
    if (!fs::exists(g_app.config.output_dir, ec)) return;
    for (const auto& dir : fs::directory_iterator(g_app.config.output_dir, ec)) {
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

        // A small plan render is cheap and always matches the file.
        int cell = std::max(2, 132 / std::max(1, e.cols));
        ImageBuffer img = MapRasterizer::RenderPreview(peek, cell);
        e.thumb = CreateTextureRGBA(img.pixels.data(), img.width, img.height);
        e.thumbW = img.width;
        e.thumbH = img.height;
        g_mapEntries.push_back(e);
    }
}

// Lists every plan on disk so a map built by an agent or the CLI can be opened
// and edited here - which is the whole point of having both paths.
static void DrawOpenDialog() {
    static bool wasOpen = false;
    static std::string chosen;
    static std::string manualPath;
    static std::string openError;

    if (g_app.showOpenDialog && !wasOpen) {
        // Only open it on the transition. Calling OpenPopup every frame kept
        // re-opening the popup, so Cancel and Open could never take effect.
        ImGui::OpenPopup("Open a map");
        chosen.clear();
        ScanMaps();
    }
    wasOpen = g_app.showOpenDialog;
    if (!g_app.showOpenDialog) return;

    ImVec2 centre = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(centre, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    // Scale with the window so a maximised app shows more thumbnails at once.
    ImVec2 vp = ImGui::GetMainViewport()->WorkSize;
    ImGui::SetNextWindowSize(ImVec2(std::clamp(vp.x * 0.62f, 660.0f, 1400.0f),
                                    std::clamp(vp.y * 0.68f, 520.0f, 1000.0f)),
                             ImGuiCond_Appearing);
    bool stayOpen = true;
    if (!ImGui::BeginPopupModal("Open a map", &stayOpen, ImGuiWindowFlags_NoSavedSettings)) {
        g_app.showOpenDialog = false;
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
        g_app.map = loaded;
        g_app.SyncGridFromMap();
        g_app.currentFile = path;
        g_app.dirty = false;
        g_app.handEdited = false;

        // Restore everything the plan was made with, so the Create tab shows the
        // settings behind the map you just opened rather than stale ones.
        if (!loaded.meta.style.empty()) g_app.selectedStyle = loaded.meta.style;
        if (!loaded.meta.scene_summary.empty()) g_app.sceneText = loaded.meta.scene_summary;
        // The stored grid includes the bleed margin; the sliders show the field.
        int lb = arch::BorderOf(loaded);
        g_app.cols = std::max(arch::kMinCells, loaded.grid.cols - 2 * lb);
        g_app.rows = std::max(arch::kMinCells, loaded.grid.rows - 2 * lb);
        g_app.layoutIndex = 0;
        for (int i = 1; i < IM_ARRAYSIZE(kLayoutNames); ++i)
            if (loaded.meta.layout == kLayoutNames[i]) g_app.layoutIndex = i;
        if (!loaded.meta.terrain_kind.empty()) g_app.terrainKind = loaded.meta.terrain_kind;
        if (!loaded.meta.terrain_amount.empty()) g_app.terrainAmount = loaded.meta.terrain_amount;
        if (!loaded.meta.prop_density.empty()) g_app.propDensity = loaded.meta.prop_density;
        if (loaded.meta.seed > 0) {
            g_app.seed = (int)loaded.meta.seed;
            g_app.randomSeed = false;
        }
        g_app.job.Log("Opened " + path);
        for (const auto& pr : problems) g_app.job.Log("repaired: " + pr);
        return true;
    };

    // Buttons first: they must never end up below the fold, which is exactly
    // what made this dialog impossible to close.
    std::string target = !manualPath.empty() ? manualPath : chosen;
    ImGui::BeginDisabled(target.empty());
    if (ImGui::Button("Open", ImVec2(150, 34))) {
        if (loadPath(target)) {
            g_app.showOpenDialog = false;
            manualPath.clear();
            ReleaseMapEntries();
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Cancel", ImVec2(150, 34)) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        g_app.showOpenDialog = false;
        manualPath.clear();
        ReleaseMapEntries();
        ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
        return;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("double-click a row to open it");

    ImGui::Separator();
    ImGui::TextWrapped("Plans found in '%s'. These can come from this app, the command line, "
                       "or an AI agent - the file format is the same.",
                       g_app.config.output_dir.c_str());

    ImGui::BeginChild("##maplist", ImVec2(0, -74), ImGuiChildFlags_Borders);
    int perRow = 1;
    float cardW = 160.0f;
    GridMetrics(160.0f, perRow, cardW);
    const float cardH = 150.0f;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (int i = 0; i < (int)g_mapEntries.size(); ++i) {
        if (i % perRow != 0) ImGui::SameLine();
        MapEntry& e = g_mapEntries[i];
        bool active = chosen == e.path;

        ImGui::PushID(i);
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##m", ImVec2(cardW - 8.0f, cardH - 8.0f))) chosen = e.path;
        bool hovered = ImGui::IsItemHovered();
        if (hovered)
            ImGui::SetTooltip("%s\n%s\n%d x %d cells", e.folder.c_str(), e.title.c_str(),
                              e.cols, e.rows);
        if (hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
            if (loadPath(e.path)) {
                g_app.showOpenDialog = false;
                manualPath.clear();
                ImGui::CloseCurrentPopup();
                ImGui::PopID();
                break;
            }
        }

        ImVec2 p1(p0.x + cardW - 8.0f, p0.y + cardH - 8.0f);
        dl->AddRectFilled(p0, p1, hovered ? IM_COL32(48, 52, 62, 255)
                                          : IM_COL32(30, 33, 40, 255), 4.0f);
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
        dl->AddText(ImVec2(p0.x + 8.0f, p1.y - 22.0f), IM_COL32(150, 152, 158, 255),
                    dims.c_str());
        ImGui::PopID();
    }
    if (g_mapEntries.empty())
        ImGui::TextDisabled("No plans in this folder yet. Build one on the Create tab, or use "
                            "Browse to open a plan from anywhere.");
    ImGui::EndChild();

    ImGui::Text("Somewhere else? Point at any map.json:");
    ImGui::SetNextItemWidth(-140.0f);
    if (InputTextString("##manualpath", &manualPath,
                        ImGuiInputTextFlags_EnterReturnsTrue)) {
        if (loadPath(manualPath)) {
            manualPath.clear();
            g_app.showOpenDialog = false;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SetItemTooltip("Paste a path and press Enter. A folder works too, and so do the "
                          "quotes Explorer's \"Copy as path\" puts round it.");
    ImGui::SameLine();
    if (ImGui::Button("Browse...", ImVec2(-1, 0))) {
        std::string picked = PickMapFile();
        if (!picked.empty() && loadPath(picked)) {
            manualPath.clear();
            g_app.showOpenDialog = false;
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

static void SaveCurrentMap() {
    g_app.SyncMapFromGrid();
    std::string dir = OutputDir(g_app.map.meta.name);
    if (MapSerializer::SaveToFile(dir + "/map.json", g_app.map)) {
        g_app.dirty = false;
        g_app.handEdited = false;
        g_app.job.Log("Saved " + dir + "/map.json");
    } else {
        g_app.job.Log("Could not save " + dir + "/map.json");
    }
}

static void MainMenu() {
    if (!ImGui::BeginMenuBar()) return;
    if (ImGui::BeginMenu("File")) {
        if (ImGui::MenuItem("New empty map")) {
            g_app.PushUndo();
            const int b = arch::kBorderCells;
            int w = std::max(arch::kMinCells, g_app.cols);
            int h = std::max(arch::kMinCells, g_app.rows);
            g_app.grid = TileGrid(w + 2 * b, h + 2 * b, Tile::Void);
            g_app.grid.FillRect(b + 1, b + 1, w - 2, h - 2, Tile::Floor);
            arch::DeriveWalls(g_app.grid);
            g_app.features.clear();
            g_app.annotations.clear();
            g_app.effects.clear();
            g_app.map.areas.clear();
            g_app.map.meta.border = b;
            g_app.map.meta.name = "battlemap";
            g_app.MarkEdited();
        }
        ImGui::SetItemTooltip("A blank field at the size set on the Create tab, walled and "
                              "ready to paint.");
        if (ImGui::MenuItem("Open map.json...", "Ctrl+O")) g_app.showOpenDialog = true;
        ImGui::SetItemTooltip("Load a plan built by an agent, the command line, or an earlier "
                              "session, and edit it here.");
        if (ImGui::MenuItem("Save map.json", "Ctrl+S")) SaveCurrentMap();
        if (ImGui::MenuItem("Export plan preview PNG")) {
            g_app.SyncMapFromGrid();
            std::string dir = OutputDir(g_app.map.meta.name);
            MapRasterizer::ExportPng(MapRasterizer::RenderPreview(g_app.map),
                                     dir + "/preview.png");
            g_app.job.Log("Preview written to " + dir + "/preview.png");
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Exit")) PostQuitMessage(0);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Presets")) {
        if (g_app.styles.presets.empty()) ImGui::TextDisabled("none saved yet");
        for (const auto& kv : g_app.styles.presets) {
            if (ImGui::MenuItem(kv.first.c_str())) {
                g_app.map = kv.second;
                g_app.SyncGridFromMap();
            }
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Save current as preset")) {
            g_app.SyncMapFromGrid();
            g_app.styles.SavePreset(g_app.map.meta.name, g_app.map);
            g_app.job.Log("Preset saved: " + g_app.map.meta.name);
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

// ---------------------------------------------------------------- platform
extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

static LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg) {
    case WM_SIZE:
        if (wParam != SIZE_MINIMIZED) {
            g_resizeW = (UINT)LOWORD(lParam);
            g_resizeH = (UINT)HIWORD(lParam);
        }
        return 0;
    case WM_SYSCOMMAND:
        if ((wParam & 0xfff0) == SC_KEYMENU) return 0;
        break;
    case WM_DESTROY:
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}

static void CreateRenderTarget() {
    ID3D11Texture2D* back = nullptr;
    g_swapChain->GetBuffer(0, IID_PPV_ARGS(&back));
    if (back) {
        g_device->CreateRenderTargetView(back, nullptr, &g_rtv);
        back->Release();
    }
}

static void CleanupRenderTarget() {
    if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; }
}

static bool CreateDeviceD3D(HWND hWnd) {
    DXGI_SWAP_CHAIN_DESC sd = {};
    sd.BufferCount = 2;
    sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 60;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hWnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL level;
    const D3D_FEATURE_LEVEL levels[2] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
                                               levels, 2, D3D11_SDK_VERSION, &sd, &g_swapChain,
                                               &g_device, &level, &g_context);
    if (hr == DXGI_ERROR_UNSUPPORTED)
        hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, levels, 2,
                                           D3D11_SDK_VERSION, &sd, &g_swapChain, &g_device,
                                           &level, &g_context);
    if (FAILED(hr)) return false;
    CreateRenderTarget();
    return true;
}

static void CleanupDeviceD3D() {
    CleanupRenderTarget();
    if (g_swapChain) { g_swapChain->Release(); g_swapChain = nullptr; }
    if (g_context) { g_context->Release(); g_context = nullptr; }
    if (g_device) { g_device->Release(); g_device = nullptr; }
}

static void SetupFonts() {
    ImGuiIO& io = ImGui::GetIO();
    // Default ImGui font has no Cyrillic and no emoji. Scene descriptions get
    // typed in whatever language the user speaks, so load a real UI font with
    // Cyrillic coverage and keep the interface text plain ASCII.
    const char* candidates[] = {"C:/Windows/Fonts/segoeui.ttf", "C:/Windows/Fonts/tahoma.ttf",
                                "C:/Windows/Fonts/arial.ttf"};
    for (const char* path : candidates) {
        if (!fs::exists(path)) continue;
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        cfg.OversampleV = 1;
        if (io.Fonts->AddFontFromFileTTF(path, 18.0f, &cfg, io.Fonts->GetGlyphRangesCyrillic()))
            return;
    }
    io.Fonts->AddFontDefault();
}

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE, LPSTR, int nCmdShow) {
    // Find the project data (styles/, presets/, config.json) by walking up from
    // the executable. One shared copy means editing a style in the app also
    // changes it for the Python command line tools - no divergent duplicates.
    {
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

    // The same icon in three places: title bar, alt-tab, and the executable
    // itself (that last one comes from the resource file, not from here).
    HICON appIcon = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APPICON), IMAGE_ICON,
                                      0, 0, LR_DEFAULTSIZE | LR_SHARED);
    HICON appIconSmall = (HICON)LoadImageW(hInstance, MAKEINTRESOURCEW(IDI_APPICON),
                                           IMAGE_ICON, GetSystemMetrics(SM_CXSMICON),
                                           GetSystemMetrics(SM_CYSMICON), LR_SHARED);
    WNDCLASSEXW wc = {sizeof(wc), CS_CLASSDC, WndProc, 0L, 0L, hInstance, appIcon, nullptr,
                      nullptr, nullptr, L"DndBattleMapGenClass", appIconSmall};
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"D&D AI Battle Map Generator",
                              WS_OVERLAPPEDWINDOW, 80, 60, 1560, 960, nullptr, nullptr,
                              hInstance, nullptr);
    if (!CreateDeviceD3D(hwnd)) {
        CleanupDeviceD3D();
        UnregisterClassW(wc.lpszClassName, hInstance);
        MessageBoxW(nullptr, L"Could not initialise DirectX 11.", L"Startup failed", MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, nCmdShow);
    UpdateWindow(hwnd);

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.IniFilename = "imgui.ini";
    SetupFonts();
    SetupDarkFantasyTheme();
    g_hwnd = hwnd;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(g_device, g_context);

    // -- initial state ------------------------------------------------
    {
        std::string err;
        ConfigStore::Load(g_app.configPath, g_app.config, err);
        if (!err.empty()) g_app.job.Log(err);
    }
    g_app.styles.LoadAll();
    if (!g_app.styles.lastError.empty()) g_app.job.Log(g_app.styles.lastError);
    if (g_app.styles.Find(g_app.config.default_style))
        g_app.selectedStyle = g_app.config.default_style;
    else if (!g_app.styles.styles.empty())
        g_app.selectedStyle = g_app.styles.styles.begin()->first;
    for (const auto& pr : arch::SizePresets())
        if (g_app.config.default_size == pr.first) {
            g_app.cols = pr.second.first;
            g_app.rows = pr.second.second;
        }

    // Start with a real map so the app is never a blank window.
    {
        DesignSpec spec = SpecFromUi();
        g_app.map = arch::Build(spec, 7u);
        g_app.map.meta.style = g_app.selectedStyle;
        g_app.SyncGridFromMap();
        g_app.SyncMapFromGrid();
    }
    StartConnectionCheck();

    bool done = false;
    while (!done) {
        MSG msg;
        while (PeekMessage(&msg, nullptr, 0U, 0U, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;

        if (g_resizeW && g_resizeH) {
            CleanupRenderTarget();
            g_swapChain->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0);
            g_resizeW = g_resizeH = 0;
            CreateRenderTarget();
        }

        DrainJobResults();

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

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

        MainMenu();

        // The two shortcuts everybody tries first.
        if (ImGui::GetIO().KeyCtrl && !ImGui::GetIO().WantTextInput) {
            if (ImGui::IsKeyPressed(ImGuiKey_O, false)) g_app.showOpenDialog = true;
            if (ImGui::IsKeyPressed(ImGuiKey_S, false)) SaveCurrentMap();
        }

        // Tabs host their content in child regions - never in nested windows,
        // which is what made the old build render panels on top of each other.
        float statusH = ImGui::GetFrameHeight() + 8.0f;
        ImGui::BeginChild("##tabhost", ImVec2(0, -statusH));
        if (ImGui::BeginTabBar("##tabs")) {
            if (ImGui::BeginTabItem("Create")) { TabCreate(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Editor")) { TabEditor(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Render")) { TabRender(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Styles")) { TabStyles(); ImGui::EndTabItem(); }
            if (ImGui::BeginTabItem("Settings")) { TabSettings(); ImGui::EndTabItem(); }
            ImGui::EndTabBar();
        }
        ImGui::EndChild();
        DrawOpenDialog();
        DrawRebuildGuard();

        ImGui::Separator();
        ImGui::Text("Ollama: %s", g_app.ollamaOk ? "ready" : "offline");
        ImGui::SameLine();
        ImGui::Text("| ComfyUI: %s", g_app.comfyOk ? "ready" : "offline");
        ImGui::SameLine();
        int sb = g_app.map.meta.border;
        ImGui::Text("| Map: %d x %d", g_app.grid.cols - 2 * sb, g_app.grid.rows - 2 * sb);
        if (sb > 0)
            ImGui::SetItemTooltip("Your field is %d x %d cells. A %d-cell bleed margin is "
                                  "added around it for the renderer, so the image is "
                                  "%d x %d.",
                                  g_app.grid.cols - 2 * sb, g_app.grid.rows - 2 * sb, sb,
                                  g_app.grid.cols, g_app.grid.rows);
        ImGui::SameLine();
        ImGui::Text("| %s", g_app.job.Status().c_str());

        ImGui::End();

        ImGui::Render();
        const float clear[4] = {0.07f, 0.08f, 0.10f, 1.0f};
        g_context->OMSetRenderTargets(1, &g_rtv, nullptr);
        g_context->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swapChain->Present(1, 0);
    }

    // Let any in-flight worker notice the shutdown before we tear down.
    g_app.job.cancel = true;
    for (int i = 0; i < 40 && g_app.job.running.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

    ReleaseMapEntries();
    if (g_resultTex) g_resultTex->Release();
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    CleanupDeviceD3D();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, hInstance);
    return 0;
}
