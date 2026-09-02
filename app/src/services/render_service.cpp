#include "render_service.h"
#include "../ui/ui_context.h"
#include "../../include/comfy_service.h"
#include "../../include/ollama_service.h"
#include "../../include/ideogram_caption.h"
#include "../../include/map_rasterizer.h"
#include "../../include/map_serializer.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <thread>
#include <cmath>

namespace dnd {

Rebuild RenderService::s_pendingRebuild = Rebuild::None;

bool RenderService::RunRender(AppState& app, MapData map) {
    Job& job = app.job;
    ComfyConfig cfg = app.config.comfy;

    if (app.ollamaOk) {
        job.SetStatus("Freeing the planner model...");
        if (OllamaService::Unload(app.config.ollama.base_url, app.config.ollama.model))
            job.Log("Ollama released " + app.config.ollama.model + ".");
    }

    std::string version, error;
    job.SetStatus("Contacting ComfyUI...");
    if (!ComfyService::CheckConnection(cfg.base_url, version, error)) {
        job.Log("ComfyUI is not reachable at " + cfg.base_url + " (" + error + ")");
        job.Log("Start ComfyUI and press Generate again.");
        return false;
    }
    job.Log("Connected to " + version);

    std::string dir = OutputDir(app, map.meta.name);
    MapSerializer::SaveToFile(dir + "/map.json", map);
    MapRasterizer::ExportPng(MapRasterizer::RenderPreview(map), dir + "/preview.png");

    std::string caption;
    if (app.captionManual && !app.captionText.empty()) {
        caption = app.captionText;
        try {
            caption = nlohmann::json::parse(caption).dump(
                -1, ' ', false, nlohmann::json::error_handler_t::replace);
        } catch (const std::exception&) {
            job.Log("Hand-written caption is not valid JSON - sending it exactly as typed.");
        }
        job.Log("Using the hand-written caption.");
    } else {
        caption = IdeogramCaption::BuildJson(map, app.styles.Find(map.meta.style),
                                             app.styles.base, app.styles.phrases);
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

void RenderService::StartRenderCurrent(AppState& app) {
    if (!app.BeginJob("Rendering...")) return;
    app.SyncMapFromGrid();
    MapData map = app.map;
    std::thread([&app, map]() {
        bool ok = RunRender(app, map);
        app.FinishJob(ok, ok ? "Battle map ready." : "Render failed - see the log.");
    }).detach();
}

void RenderService::StartQuickBlueprint(AppState& app) {
    if (!app.BeginJob("Building blueprint...")) return;
    DesignSpec spec = SpecFromUi(app);
    AttachStyle(app, spec);
    std::vector<std::string> styleWarnings =
        IdeogramCaption::StyleWarnings(app.styles.Find(spec.style));
    Phrasebook phrasebook = app.styles.phrases;
    uint32_t seed = PickSeed(app);

    std::thread([&app, spec, seed, styleWarnings, phrasebook]() {
        Job& job = app.job;
        for (const auto& warn : styleWarnings) job.Log("[warn] " + warn);
        MapData map = arch::Build(spec, seed);
        for (const auto& warn : IdeogramCaption::MapWarnings(map, &phrasebook))
            job.Log("[warn] " + warn);
        job.Log("Built " + std::to_string(map.grid.cols) + "x" +
                std::to_string(map.grid.rows) + " " + spec.layout + " map, " +
                std::to_string(map.areas.size()) + " areas, " +
                std::to_string(map.features.size()) + " props");
        {
            std::lock_guard<std::mutex> lock(job.mtx);
            job.map = map;
            job.hasMap = true;
        }
        app.FinishJob(true, "Blueprint ready - open the Editor tab to adjust it.");
    }).detach();
}

void RenderService::StartPlanAndRender(AppState& app, bool alsoRender) {
    if (!app.BeginJob("Planning scene...")) return;
    OllamaConfig ocfg = app.config.ollama;
    std::string scene = app.sceneText;
    std::string styleId = app.selectedStyle;
    std::string size = "medium";
    {
        long long best = -1;
        for (const auto& pr : arch::SizePresets()) {
            long long d = std::abs((long long)pr.second.first - app.cols) +
                          std::abs((long long)pr.second.second - app.rows);
            if (best < 0 || d < best) { best = d; size = pr.first; }
        }
    }
    uint32_t seed = PickSeed(app);

    std::vector<std::string> ids;
    std::string catalogue;
    for (const auto& kv : app.styles.styles) {
        ids.push_back(kv.first);
        catalogue += "- " + kv.first + ": " + kv.second.name + " - " + kv.second.description + "\n";
    }
    std::map<std::string, std::vector<std::string>> styleProps;
    std::map<std::string, std::pair<std::string, std::string>> styleShape;
    std::map<std::string, std::vector<std::string>> styleWarn;
    std::map<std::string, std::string> styleLayout;
    for (const auto& kv : app.styles.styles) {
        styleProps[kv.first] = kv.second.props;
        styleShape[kv.first] = {kv.second.category, kv.second.enclosure};
        styleWarn[kv.first] = IdeogramCaption::StyleWarnings(&kv.second);
        styleLayout[kv.first] = kv.second.default_layout;
    }
    Phrasebook phrasebook = app.styles.phrases;
    std::string comfyUrl = app.config.comfy.base_url;
    bool comfyUp = app.comfyOk;

    std::thread([&app, ocfg, scene, styleId, size, ids, catalogue, styleProps,
                 styleShape, styleWarn, phrasebook, comfyUrl, comfyUp, seed, alsoRender]() {
        Job& job = app.job;
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
            app.FinishJob(false, "Planning failed - see the log.");
            return;
        }
        DesignSpec spec = plan.spec;
        if (spec.style.empty()) spec.style = styleId;
        auto propIt = styleProps.find(spec.style);
        if (propIt != styleProps.end()) spec.style_props = propIt->second;
        auto shapeIt = styleShape.find(spec.style);
        if (shapeIt != styleShape.end()) {
            spec.style_category = shapeIt->second.first;
            spec.style_enclosure = shapeIt->second.second;
        }
        spec.edge_walls = arch::EnclosureOf(spec.style_enclosure, spec.style_category,
                                            spec.layout, "") != "open";
        auto warnIt = styleWarn.find(spec.style);
        std::vector<std::string> planStyleWarnings =
            warnIt == styleWarn.end() ? std::vector<std::string>{} : warnIt->second;
        job.Log("Layout: " + spec.layout + " | areas: " + std::to_string(spec.rooms.size()));
        if (!spec.scene_summary.empty()) job.Log("Scene: " + spec.scene_summary);

        MapData map = arch::Build(spec, seed);
        job.Log("Blueprint: " + std::to_string(map.grid.cols) + "x" +
                std::to_string(map.grid.rows) + ", " + std::to_string(map.features.size()) +
                " props");
        for (const auto& warn : planStyleWarnings) job.Log("[warn] " + warn);
        for (const auto& warn : IdeogramCaption::MapWarnings(map, &phrasebook))
            job.Log("[warn] " + warn);
        {
            std::lock_guard<std::mutex> lock(job.mtx);
            job.map = map;
            job.hasMap = true;
        }
        if (!alsoRender) {
            app.FinishJob(true, "Blueprint ready.");
            return;
        }

        bool ok = RunRender(app, map);
        app.FinishJob(ok, ok ? "Battle map ready." : "Render failed - see the log.");
    }).detach();
}

bool RenderService::SaveCurrentPlan(AppState& app, std::string* outPath) {
    app.SyncMapFromGrid();
    std::string dir = OutputDir(app, app.map.meta.name);
    std::string path = dir + "/map.json";
    if (!MapSerializer::SaveToFile(path, app.map)) return false;
    MapRasterizer::ExportPng(MapRasterizer::RenderPreview(app.map), dir + "/preview.png");
    app.dirty = false;
    app.handEdited = false;
    app.currentFile = path;
    if (outPath) *outPath = path;
    return true;
}

void RenderService::RequestRebuild(AppState& app, Rebuild what) {
    if (app.handEdited) {
        s_pendingRebuild = what;
        return;
    }
    RunRebuild(app, what);
}

void RenderService::RunRebuild(AppState& app, Rebuild what) {
    switch (what) {
    case Rebuild::Blueprint:     StartQuickBlueprint(app); break;
    case Rebuild::Plan:          StartPlanAndRender(app, false); break;
    case Rebuild::PlanAndRender: StartPlanAndRender(app, true); break;
    default: break;
    }
}

void RenderService::DrawRebuildGuard(AppState& app) {
    if (s_pendingRebuild != Rebuild::None && !ImGui::IsPopupOpen("Replace your plan?"))
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
        if (SaveCurrentPlan(app, &path)) {
            app.job.Log("Saved " + path + " before rebuilding.");
            Rebuild what = s_pendingRebuild;
            s_pendingRebuild = Rebuild::None;
            ImGui::CloseCurrentPopup();
            RunRebuild(app, what);
        } else {
            app.job.Log("Could not save the plan - nothing was replaced.");
            s_pendingRebuild = Rebuild::None;
            ImGui::CloseCurrentPopup();
        }
    }
    ImGui::SetItemTooltip("Writes map.json and preview.png into the output folder, then "
                          "starts building the new plan.");

    if (ImGui::Button("Replace it without saving", ImVec2(-1, 28))) {
        Rebuild what = s_pendingRebuild;
        s_pendingRebuild = Rebuild::None;
        ImGui::CloseCurrentPopup();
        RunRebuild(app, what);
    }
    if (ImGui::Button("Keep what I have", ImVec2(-1, 28)) ||
        ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        s_pendingRebuild = Rebuild::None;
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

} // namespace dnd
