#include "tab_docs.h"
#include "ui_context.h"

namespace dnd {

void TabDocs::Draw(AppState& app) {
    (void)app;
    ImGui::BeginChild("##docs_child", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::TextColored(AccentGold(), "D&D Battle Map Generator - User Guide");
    ImGui::Separator();

    if (ImGui::CollapsingHeader("Workflow Overview", ImGuiTreeNodeFlags_DefaultOpen)) {
        ImGui::BulletText("1. Create Tab: Describe the desired battle map scene in plain language, pick a style and dimensions, and generate an initial blueprint or finished map.");
        ImGui::BulletText("2. Editor Tab: Fine-tune the 2D layout using intuitive painting tools, wall derivation, prop placement, annotations, and environmental effect zones.");
        ImGui::BulletText("3. Render Tab: Inspect and edit the auto-generated JSON prompt structure, configure quality presets, and render photorealistic battlemaps via ComfyUI (Ideogram 4).");
        ImGui::BulletText("4. Dungeondraft Tab: Export blueprint layouts directly into native .dungeondraft_map format, scan custom asset packs, and run vision model tagging.");
        ImGui::BulletText("5. Styles Tab: Customize visual styles, shared negative constraints, and prop catalog descriptors.");
    }

    if (ImGui::CollapsingHeader("Tips for Best Results")) {
        ImGui::BulletText("Top-Down Perspective: Always describe scenes from an overhead perspective. Avoid describing vertical walls or ceilings as elevations.");
        ImGui::BulletText("Bleed Margin: The generator adds an empty border ring around the map to prevent edge distortion artifacts from diffusion models.");
        ImGui::BulletText("Dungeondraft Asset Enrichment: Run vision tagging on your asset packs to allow semantic matching of props instead of relying strictly on filenames.");
    }

    ImGui::EndChild();
}

} // namespace dnd
