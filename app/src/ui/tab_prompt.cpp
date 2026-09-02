#include "tab_prompt.h"
#include "ui_context.h"
#include "map_canvas.h"
#include "../services/render_service.h"
#include <algorithm>

namespace dnd {

void TabPrompt::Draw(AppState& app, TextureLoader& texLoader) {
    ImGui::BeginChild("##createleft", ImVec2(PanelWidth(0.34f, 420.0f, 900.0f), 0),
                      ImGuiChildFlags_Borders);

    ImGui::TextColored(AccentGold(), "1. Describe the scene");
    ImGui::TextWrapped("Write what the place looks like from above. Plain language is fine.");
    InputTextMultilineString("##scene", &app.sceneText, ImVec2(-1, 110));

    ImGui::Spacing();
    ImGui::TextColored(AccentGold(), "2. Pick a look");
    const StyleDef* style = app.styles.Find(app.selectedStyle);
    DrawStylePicker(app);
    if (style) {
        ImGui::TextColored(AccentGold(), "%s", style->name.c_str());
        ImGui::PushTextWrapPos(0.0f);
        ImGui::TextDisabled("%s", style->description.c_str());
        ImGui::PopTextWrapPos();
    }

    ImGui::Spacing();
    ImGui::TextColored(AccentGold(), "3. Size and shape");
    ImGui::SliderInt("Width", &app.cols, arch::kMinCells, arch::kMaxCells, "%d cells");
    ImGui::SetItemTooltip("How many squares across. One square is 5 feet.");
    ImGui::SliderInt("Height", &app.rows, arch::kMinCells, arch::kMaxCells, "%d cells");
    ImGui::SetItemTooltip("How many squares down. One square is 5 feet.");
    ImGui::TextDisabled("%d x %d cells  (%d x %d ft)", app.cols, app.rows,
                        app.cols * 5, app.rows * 5);
    if (ImGui::SmallButton("Small")) { app.cols = 17; app.rows = 13; }
    ImGui::SameLine();
    if (ImGui::SmallButton("Medium")) { app.cols = 25; app.rows = 19; }
    ImGui::SameLine();
    if (ImGui::SmallButton("Large")) { app.cols = 66; app.rows = 50; }
    ImGui::SameLine();
    if (ImGui::SmallButton("Huge")) { app.cols = 100; app.rows = 75; }
    ImGui::SameLine();
    if (ImGui::SmallButton("Giant")) { app.cols = 150; app.rows = 150; }
    ImGui::Combo("Layout", &app.layoutIndex, kLayoutNames, 13);
    HelpMarker("Leave on (from style) unless you want to force a particular shape, e.g. a "
               "harbour with a moored ship or an open forest with no walls.");

    int terrainIdx = 0, amountIdx = 1;
    for (int i = 0; i < 5; ++i)
        if (app.terrainKind == kTerrainNames[i]) terrainIdx = i;
    for (int i = 0; i < 3; ++i)
        if (app.terrainAmount == kAmountNames[i]) amountIdx = i;
    if (ImGui::Combo("Ground hazard", &terrainIdx, kTerrainNames, 5))
        app.terrainKind = kTerrainNames[terrainIdx];
    if (terrainIdx != 0) {
        if (ImGui::Combo("How much", &amountIdx, kAmountNames, 3))
            app.terrainAmount = kAmountNames[amountIdx];
    }

    ImGui::Checkbox("Random seed", &app.randomSeed);
    if (!app.randomSeed) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        ImGui::InputInt("##seed", &app.seed);
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    bool busy = app.job.running.load();
    ImGui::BeginDisabled(busy);
    ImGui::PushStyleColor(ImGuiCol_Button, GoButton());
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, GoButtonHovered());
    if (ImGui::Button("MAKE MY BATTLE MAP", ImVec2(-1, 46)))
        RenderService::RequestRebuild(app, Rebuild::PlanAndRender);
    ImGui::PopStyleColor(2);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("Plans the scene with the local AI, then paints it in ComfyUI. "
                        "Takes a few minutes.");
    ImGui::PopTextWrapPos();

    ImGui::Spacing();
    if (ImGui::Button("Blueprint only (instant, no AI)", ImVec2(-1, 30)))
        RenderService::RequestRebuild(app, Rebuild::Blueprint);
    ImGui::SetItemTooltip("Builds the floor plan straight away without the language model. "
                          "Good for iterating on a layout before spending time on a render.");
    if (ImGui::Button("Plan with AI, do not render yet", ImVec2(-1, 30)))
        RenderService::RequestRebuild(app, Rebuild::Plan);
    ImGui::SetItemTooltip("Runs only Stage 1, so you can review and edit the plan first.");
    ImGui::EndDisabled();

    if (busy) {
        ImGui::Spacing();
        if (ImGui::Button("Cancel", ImVec2(-1, 26))) app.job.cancel = true;
    }

    ImGui::Spacing();
    ImGui::Text("Status: %s", app.job.Status().c_str());
    DrawJobLog(app, 160);

    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("##createright", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::TextColored(AccentGold(), "Blueprint preview");
    ImGui::TextDisabled("This is the plan, not the finished map. Edit it on the Editor tab.");
    MapCanvas::Draw(app, false, ImVec2(0, ImGui::GetContentRegionAvail().y * 0.55f));
    ImGui::Separator();
    if (texLoader.GetResultTexture()) {
        ImGui::TextColored(AccentGold(), "Finished battle map");
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float aspect = texLoader.GetResultHeight() ? (float)texLoader.GetResultWidth() / (float)texLoader.GetResultHeight() : 1.0f;
        float w = std::min(avail.x, avail.y * aspect);
        ImGui::Image((ImTextureID)texLoader.GetResultTexture(), ImVec2(w, w / aspect));
    } else {
        ImGui::TextDisabled("The finished map will appear here.");
    }
    ImGui::EndChild();
}

} // namespace dnd
