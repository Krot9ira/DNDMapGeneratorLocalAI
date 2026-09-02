#include "tab_settings.h"
#include "ui_context.h"
#include "../core/file_dialogs.h"
#include "../services/connection_monitor.h"

namespace dnd {

void TabSettings::Draw(AppState& app) {
    ImGui::BeginChild("##settings", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::TextColored(AccentGold(), "Local services");

    InputTextString("Ollama address", &app.config.ollama.base_url);
    if (!app.ollamaModels.empty()) {
        if (ImGui::BeginCombo("Planner Model (LLM)", app.config.ollama.model.c_str())) {
            for (const auto& m : app.ollamaModels)
                if (ImGui::Selectable(m.c_str(), m == app.config.ollama.model))
                    app.config.ollama.model = m;
            ImGui::EndCombo();
        }
    } else {
        InputTextString("Planner Model (LLM)", &app.config.ollama.model);
    }

    ImGui::Indent(12.0f);
    ImGui::TextDisabled("Recommended Planner: qwen3.8:27b (best floorplan reasoning, ~18GB)");
    ImGui::SameLine();
    bool hasPlannerRec = false;
    for (const auto& m : app.ollamaModels) {
        if (m.find("qwen3.8") != std::string::npos || m.find("qwen2.5:27b") != std::string::npos) {
            hasPlannerRec = true; break;
        }
    }
    if (!hasPlannerRec) {
        ImGui::BeginDisabled(app.job.running.load());
        if (ImGui::SmallButton("Download qwen3.8:27b")) {
            ConnectionMonitor::StartOllamaPull(app, "qwen3.8:27b");
        }
        ImGui::EndDisabled();
    } else {
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1.0f), "[Installed]");
    }
    ImGui::Unindent(12.0f);

    ImGui::SliderFloat("Planner creativity", &app.config.ollama.temperature, 0.0f, 1.2f, "%.2f");
    ImGui::TextColored(app.ollamaOk ? ImVec4(0.45f, 0.9f, 0.55f, 1) : ImVec4(1, 0.5f, 0.45f, 1),
                       "Ollama: %s", app.ollamaStatus.c_str());

    ImGui::Spacing();
    InputTextString("ComfyUI address", &app.config.comfy.base_url);
    ImGui::TextColored(app.comfyOk ? ImVec4(0.45f, 0.9f, 0.55f, 1) : ImVec4(1, 0.5f, 0.45f, 1),
                       "ComfyUI: %s", app.comfyStatus.c_str());

    ImGui::Spacing();
    ImGui::BeginDisabled(app.job.running.load());
    if (ImGui::Button("Test both connections", ImVec2(240, 32)))
        ConnectionMonitor::StartConnectionCheck(app);
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::TextColored(AccentGold(), "Maps");
    ImGui::SliderInt("Bleed margin", &app.config.border_cells, 0, 6, "%d cells");
    ImGui::SetItemTooltip(
        "An empty ring added around every new map. It is added outside the size you pick, "
        "never taken out of it.\n"
        "Image models are least reliable at the very edge of a picture, so the margin is "
        "where their mistakes go instead of into one of your rooms.\n"
        "Set it to 0 if you want the map bled right to the edge.");

    ImGui::Separator();
    ImGui::TextColored(AccentGold(), "Ideogram 4 models in ComfyUI");
    InputTextString("Diffusion model", &app.config.comfy.unet);
    InputTextString("Unconditional model", &app.config.comfy.unet_uncond);
    ImGui::SetItemTooltip("Required. It supplies the negative half of classifier-free "
                          "guidance; without it renders come out washed out.");
    InputTextString("Text encoder", &app.config.comfy.clip);
    InputTextString("VAE", &app.config.comfy.vae);

    ImGui::Separator();
    InputTextString("Output folder", &app.config.output_dir);

    ImGui::Separator();
    ImGui::TextColored(AccentGold(), "Dungeondraft & Vision Tagging");
    if (!app.ollamaModels.empty()) {
        if (ImGui::BeginCombo("Vision Model (Tagging)", app.config.dungeondraft.vision_model.c_str())) {
            for (const auto& m : app.ollamaModels)
                if (ImGui::Selectable(m.c_str(), m == app.config.dungeondraft.vision_model))
                    app.config.dungeondraft.vision_model = m;
            ImGui::EndCombo();
        }
    } else {
        InputTextString("Vision Model (Tagging)", &app.config.dungeondraft.vision_model);
    }
    ImGui::SetItemTooltip("Ollama vision model (e.g. gemma4:12b) used to catalogue and enrich Dungeondraft asset thumbnails.");

    ImGui::Indent(12.0f);
    bool hasGemma4 = false;
    for (const auto& m : app.ollamaModels) {
        if (m.find("gemma4") != std::string::npos) { hasGemma4 = true; break; }
    }
    ImGui::TextDisabled("Recommended: gemma4:12b (7.6GB - highest quality object descriptions)");
    ImGui::SameLine();
    if (!hasGemma4) {
        ImGui::BeginDisabled(app.job.running.load());
        if (ImGui::SmallButton("Download gemma4:12b")) {
            ConnectionMonitor::StartOllamaPull(app, "gemma4:12b");
        }
        ImGui::EndDisabled();
    } else {
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1.0f), "[Installed]");
    }

    bool hasOrnith = false;
    for (const auto& m : app.ollamaModels) {
        if (m.find("ornith") != std::string::npos) { hasOrnith = true; break; }
    }
    ImGui::TextDisabled("Alternative: ornith-1.5:9b (6.6GB - fast cataloguer)");
    ImGui::SameLine();
    if (!hasOrnith) {
        ImGui::BeginDisabled(app.job.running.load());
        if (ImGui::SmallButton("Download ornith-1.5:9b")) {
            ConnectionMonitor::StartOllamaPull(app, "ornith-1.5:9b");
        }
        ImGui::EndDisabled();
    } else {
        ImGui::TextColored(ImVec4(0.45f, 0.85f, 0.55f, 1.0f), "[Installed]");
    }
    ImGui::Unindent(12.0f);

    InputTextString("Custom Assets Folder", &app.config.dungeondraft.custom_assets_dir);
    ImGui::SameLine();
    if (ImGui::Button("Browse...##ddassets")) {
        std::string p = FileDialogs::PickFolderDialog("Select Dungeondraft Custom Assets Folder");
        if (!p.empty()) app.config.dungeondraft.custom_assets_dir = p;
    }
    ImGui::SetItemTooltip("Folder containing your custom .dungeondraft_pack files (e.g. D:\\programs\\dungeondraft\\assets)");

    InputTextString("Dungeondraft Executable", &app.config.dungeondraft.app_path);
    ImGui::SameLine();
    if (ImGui::Button("Browse...##ddexe")) {
        std::string p = FileDialogs::PickExecutableFile();
        if (!p.empty()) app.config.dungeondraft.app_path = p;
    }
    ImGui::SetItemTooltip("Path to Dungeondraft.exe (optional, allows launching maps directly from the app)");

    ImGui::Spacing();
    if (ImGui::Button("Save settings", ImVec2(200, 32))) {
        std::string err;
        if (ConfigStore::Save(app.configPath, app.config, err))
            app.job.Log("Settings saved to " + app.configPath);
        else
            app.job.Log("Could not save settings: " + err);
    }
    ImGui::SameLine();
    if (ImGui::Button("Reload settings", ImVec2(200, 32))) {
        std::string err;
        ConfigStore::Load(app.configPath, app.config, err);
    }

    ImGui::Separator();
    ImGui::TextDisabled(
        "These settings live in config.json next to the executable and are shared with the "
        "Python command line tools.");
    DrawJobLog(app, 140);
    ImGui::EndChild();
}

} // namespace dnd
