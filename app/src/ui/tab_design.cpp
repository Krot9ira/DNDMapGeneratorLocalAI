#include "tab_design.h"
#include "ui_context.h"
#include "map_canvas.h"
#include <algorithm>

namespace dnd {

void TabDesign::DrawEffectPicker(AppState& app) {
    int perRow = 1;
    float cellW = 74.0f;
    GridMetrics(74.0f, perRow, cellW);
    const float cellH = 76.0f;
    float gridH = std::clamp(ImGui::GetContentRegionAvail().y - 120.0f, 180.0f, 900.0f);
    ImGui::BeginChild("##effectpicker", ImVec2(0, gridH), ImGuiChildFlags_Borders);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (size_t i = 0; i < kEffectsCount; ++i) {
        if (i % perRow != 0) ImGui::SameLine();
        const EffectInfo& info = kEffects[i];
        bool active = app.effectKind == info.kind;

        ImGui::PushID((int)(3000 + i));
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##e", ImVec2(cellW - 6.0f, cellH - 6.0f)))
            app.effectKind = info.kind;
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

void TabDesign::DrawPropPicker(AppState& app) {
    int perRow = 1;
    float cellW = 74.0f;
    GridMetrics(74.0f, perRow, cellW);
    const float cellH = 76.0f;
    float gridH = std::clamp(ImGui::GetContentRegionAvail().y - 90.0f, 200.0f, 900.0f);

    ImGui::BeginChild("##proppicker", ImVec2(0, gridH), ImGuiChildFlags_Borders);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    for (size_t i = 0; i < kPropsCount; ++i) {
        if (i % perRow != 0) ImGui::SameLine();
        const PropInfo& info = kProps[i];
        bool active = app.propKind == info.kind;

        ImGui::PushID((int)i);
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##p", ImVec2(cellW - 6.0f, cellH - 6.0f)))
            app.propKind = info.kind;
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

    int extra = (int)kPropsCount;
    auto propWords = app.styles.phrases.sections.find("props");
    if (propWords != app.styles.phrases.sections.end()) {
        for (const auto& [kind, phrase] : propWords->second) {
            bool builtIn = false;
            for (size_t i = 0; i < kPropsCount; ++i)
                if (kind == kProps[i].kind) builtIn = true;
            if (builtIn) continue;

            if (extra % perRow != 0) ImGui::SameLine();
            bool active = app.propKind == kind;
            ImGui::PushID(extra++);
            ImVec2 p0 = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##pc", ImVec2(cellW - 6.0f, cellH - 6.0f)))
                app.propKind = kind;
            bool hov = ImGui::IsItemHovered();
            if (hov)
                ImGui::SetTooltip("%s\nYour own object. The renderer is told:\n%s",
                                  kind.c_str(), phrase.c_str());
            ImVec2 p1(p0.x + cellW - 6.0f, p0.y + cellH - 6.0f);
            if (active || hov)
                dl->AddRectFilled(p0, p1, active ? IM_COL32(90, 70, 26, 255)
                                                 : IM_COL32(52, 56, 66, 255), 4.0f);
            if (active) dl->AddRect(p0, p1, IM_COL32(250, 200, 70, 255), 4.0f, 0, 2.0f);
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

void TabDesign::Draw(AppState& app) {
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

    for (int i = 0; i < 7; ++i) {
        bool active = (int)app.tool == i;
        if (active) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.65f, 0.20f, 1.0f));
        ImGui::PushID(i);
        ImU32 gcol = active ? IM_COL32(30, 26, 12, 255) : IM_COL32(226, 222, 210, 255);
        if (IconButton("##tool", toolNames[i], ImVec2(-1, 30),
                       [i](ImDrawList* dl, ImVec2 c, float r, ImU32 col) {
                           DrawToolGlyph(dl, c, r, i, col);
                       }, gcol))
            app.tool = (Tool)i;
        ImGui::SetItemTooltip("%s", toolHints[i]);
        ImGui::PopID();
        if (active) ImGui::PopStyleColor();
    }

    ImGui::Separator();
    if (app.tool == Tool::Paint || app.tool == Tool::RectFill) {
        ImGui::Text("Material");
        for (int i = 0; i < 11; ++i) {
            Tile t = TileFromName(kPaintTiles[i]);
            if (std::string(kPaintTiles[i]) == "void") t = Tile::Void;
            bool active = app.paintTile == t;
            ImVec4 col = ImGui::ColorConvertU32ToFloat4(TileColor(t));
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
                app.paintTile = t;
            ImGui::PopID();
            ImGui::PopStyleColor(4);
            ImGui::PopStyleVar();
            ImGui::SetItemTooltip("%s", kTileHints[i]);
        }
        if (app.tool == Tool::Paint) ImGui::SliderInt("Brush size", &app.brushSize, 1, 6);
    } else if (app.tool == Tool::PlaceProp) {
        ImGui::Checkbox("Custom object", &app.customProp);
        ImGui::SetItemTooltip("Place something the catalogue does not have: you name it and "
                              "describe it, and the renderer is told exactly that.");
        if (app.customProp) {
            ImGui::Text("Name");
            InputTextString("##clabel", &app.customLabel);
            ImGui::SetItemTooltip("Short name, e.g. 'blood-stained altar'.");
            ImGui::Text("Description");
            InputTextMultilineString("##cdesc", &app.customDesc, ImVec2(-1, 70));
            ImGui::SetItemTooltip("What it looks like from above: materials, colour, damage.");
            ImGui::Text("Embellishment level");
            ImGui::Combo("##celab", &app.customElaboration,
                         "No - exactly as written\0A little\0Freely\0");
            ImGui::SetItemTooltip("How much licence the renderer gets with your description.");
            if (app.customLabel.empty())
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.5f, 1.0f), "Give it a name first.");
        } else {
            ImGui::Text("Prop: %s", app.propKind.c_str());
            DrawPropPicker(app);
        }
    } else if (app.tool == Tool::Annotate) {
        ImGui::TextWrapped("Drag a rectangle on the map, then describe what belongs there "
                           "in your own words. Use it for custom walls, doors, gates "
                           "or anything the material list has no word for.");
        ImGui::Text("Name");
        InputTextString("##alabel", &app.annLabel);
        ImGui::SetItemTooltip("Short name, e.g. 'barred iron gate' or 'collapsed wall'.");
        ImGui::Text("Description");
        InputTextMultilineString("##adesc", &app.annDesc, ImVec2(-1, 70));
        ImGui::SetItemTooltip("What it looks like from above.");
        ImGui::Text("Embellishment level");
        ImGui::Combo("##aelab", &app.annElaboration,
                     "No - exactly as written\0A little\0Freely\0");
        ImGui::SetItemTooltip("How much licence the renderer gets with your words.");
        if (app.annLabel.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.5f, 1.0f), "Give it a name first.");

        ImGui::Separator();
        ImGui::Text("Custom areas on this map: %d", (int)app.annotations.size());
        for (int i = 0; i < (int)app.annotations.size(); ++i) {
            ImGui::PushID(1000 + i);
            if (ImGui::SmallButton("x")) {
                app.PushUndo();
                app.annotations.erase(app.annotations.begin() + i);
                ImGui::PopID();
                break;
            }
            ImGui::SameLine();
            ImGui::TextUnformatted(app.annotations[i].label.c_str());
            ImGui::PopID();
        }
    } else if (app.tool == Tool::Effects) {
        ImGui::TextWrapped("Drag a rectangle on the map to lay an effect over it.");
        ImGui::Combo("Strength", &app.effIntensity, "Faint\0Clear\0Heavy\0");
        ImGui::SetItemTooltip("How strongly the effect reads in the finished map.");
        ImGui::Checkbox("Custom effect", &app.customEffect);
        ImGui::SetItemTooltip("Describe an effect the catalogue does not have.");
        if (app.customEffect) {
            ImGui::Text("Name");
            InputTextString("##elabel", &app.effLabel);
            ImGui::Text("Description");
            InputTextMultilineString("##edesc", &app.effDesc, ImVec2(-1, 66));
            ImGui::Text("Embellishment level");
            ImGui::Combo("##eelab", &app.effElaboration,
                         "No - exactly as written\0A little\0Freely\0");
            if (app.effLabel.empty())
                ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.5f, 1.0f), "Give it a name first.");
        } else {
            DrawEffectPicker(app);
        }

        ImGui::Separator();
        ImGui::Text("Effects on this map: %d", (int)app.effects.size());
        for (int i = 0; i < (int)app.effects.size(); ++i) {
            ImGui::PushID(2000 + i);
            if (ImGui::SmallButton("x")) {
                app.PushUndo();
                app.effects.erase(app.effects.begin() + i);
                ImGui::PopID();
                break;
            }
            ImGui::SameLine();
            const Effect& e = app.effects[i];
            ImGui::TextUnformatted(e.label.empty() ? e.kind.c_str() : e.label.c_str());
            ImGui::PopID();
        }
    }
    ImGui::Separator();
    ImGui::BeginDisabled(app.undoStack.empty());
    if (ImGui::Button("Undo", ImVec2(-1, 26))) app.Undo();
    ImGui::EndDisabled();
    ImGui::BeginDisabled(app.redoStack.empty());
    if (ImGui::Button("Redo", ImVec2(-1, 26))) app.Redo();
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::Checkbox("Cell guides", &app.showCellGuides);
    ImGui::SetItemTooltip("Editor aid only. The grid is never drawn into the generated map - "
                          "your virtual tabletop draws its own.");
    ImGui::Checkbox("Show props", &app.showProps);
    if (ImGui::Button("Reset view", ImVec2(-1, 24))) {
        app.zoom = 1.0f;
        app.pan = ImVec2(40, 40);
    }

    ImGui::Separator();
    ImGui::Text("Map size");
    int cols = app.grid.cols, rows = app.grid.rows;
    ImGui::SetNextItemWidth(80);
    ImGui::InputInt("cols", &cols, 0);
    ImGui::SetNextItemWidth(80);
    ImGui::InputInt("rows", &rows, 0);
    if (ImGui::Button("Resize canvas", ImVec2(-1, 24))) {
        cols = std::clamp(cols, 11, 60);
        rows = std::clamp(rows, 9, 45);
        if (cols != app.grid.cols || rows != app.grid.rows) {
            app.PushUndo();
            TileGrid ng(cols, rows, Tile::Void);
            for (int y = 0; y < std::min(rows, app.grid.rows); ++y)
                for (int x = 0; x < std::min(cols, app.grid.cols); ++x)
                    ng.Set(x, y, app.grid.Get(x, y));
            app.grid = ng;
            app.features.erase(std::remove_if(app.features.begin(), app.features.end(),
                                                [&](const Feature& f) {
                                                    return f.x >= cols || f.y >= rows;
                                                }),
                                 app.features.end());
        }
    }

    ImGui::Separator();
    if (ImGui::Button("Rebuild walls", ImVec2(-1, 26))) {
        app.PushUndo();
        arch::DeriveWalls(app.grid);
    }
    ImGui::SetItemTooltip("Turns every empty cell touching open ground into a wall, so a "
                          "hand-drawn room ends up properly enclosed.");

    ImGui::EndChild();
    ImGui::SameLine();

    ImGui::BeginChild("##edcanvas", ImVec2(0, 0), ImGuiChildFlags_Borders);
    ImGui::Text("Left: use tool  |  Right-click: name and options  |  "
                "Right or middle drag: pan  |  Wheel: zoom");
    ImGui::SameLine();
    int hb = app.map.meta.border;
    if (hb > 0)
        ImGui::TextDisabled("(%d x %d field  +%d bleed)", app.grid.cols - 2 * hb,
                            app.grid.rows - 2 * hb, hb);
    else
        ImGui::TextDisabled("(%d x %d)", app.grid.cols, app.grid.rows);
    MapCanvas::Draw(app, true, ImVec2(0, 0));
    ImGui::EndChild();
}

} // namespace dnd
