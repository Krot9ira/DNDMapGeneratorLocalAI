#include "map_canvas.h"
#include "ui_context.h"
#include <algorithm>
#include <cmath>
#include <vector>

namespace dnd {

void MapCanvas::Draw(AppState& app, bool interactive, ImVec2 size) {
    TileGrid& grid = app.grid;
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
        float old = app.zoom;
        app.zoom = std::clamp(app.zoom * (io.MouseWheel > 0 ? 1.12f : 1.0f / 1.12f),
                                0.15f, 6.0f);
        ImVec2 m(io.MousePos.x - origin.x - app.pan.x, io.MousePos.y - origin.y - app.pan.y);
        float k = app.zoom / old;
        app.pan.x -= m.x * (k - 1.0f);
        app.pan.y -= m.y * (k - 1.0f);
    }
    if (interactive && ImGui::IsItemActive() &&
        (ImGui::IsMouseDragging(ImGuiMouseButton_Middle) ||
         ImGui::IsMouseDragging(ImGuiMouseButton_Right))) {
        app.pan.x += io.MouseDelta.x;
        app.pan.y += io.MouseDelta.y;
    }

    float base = interactive ? 26.0f : 0.0f;
    if (!interactive) {
        base = std::min(size.x / grid.cols, size.y / grid.rows);
    }
    float cell = interactive ? base * app.zoom : base;
    ImVec2 off = interactive
                     ? ImVec2(origin.x + app.pan.x, origin.y + app.pan.y)
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

    if (app.showProps) {
        for (const auto& f : app.features) {
            ImVec2 c(off.x + (f.x + 0.5f) * cell, off.y + (f.y + 0.5f) * cell);
            float r = cell * 0.34f;
            dl->AddCircleFilled(c, r, IM_COL32(34, 34, 42, 210), 14);
            if (f.label.empty() && cell >= 10.0f) {
                DrawPropGlyph(dl, c, r * 0.92f, f.kind.c_str(), IM_COL32(244, 228, 180, 255));
            } else {
                dl->AddCircle(c, r * 0.75f, IM_COL32(240, 220, 160, 255), 14,
                              std::max(1.0f, cell * 0.05f));
                dl->AddCircleFilled(c, r * 0.28f, IM_COL32(240, 220, 160, 255), 10);
            }
        }
    }

    const int border = app.map.meta.border;
    if (border > 0) {
        ImVec2 in0(off.x + border * cell, off.y + border * cell);
        ImVec2 in1(off.x + (grid.cols - border) * cell, off.y + (grid.rows - border) * cell);
        ImVec2 out0(off.x, off.y), out1(off.x + grid.cols * cell, off.y + grid.rows * cell);
        ImU32 shade = IM_COL32(12, 13, 17, 165);
        dl->AddRectFilled(out0, ImVec2(out1.x, in0.y), shade);
        dl->AddRectFilled(ImVec2(out0.x, in1.y), out1, shade);
        dl->AddRectFilled(ImVec2(out0.x, in0.y), ImVec2(in0.x, in1.y), shade);
        dl->AddRectFilled(ImVec2(in1.x, in0.y), ImVec2(out1.x, in1.y), shade);

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

    if (app.showCellGuides && cell >= 6.0f) {
        ImU32 guide = IM_COL32(0, 0, 0, 46);
        for (int x = 0; x <= grid.cols; ++x)
            dl->AddLine(ImVec2(off.x + x * cell, off.y),
                        ImVec2(off.x + x * cell, off.y + grid.rows * cell), guide);
        for (int y = 0; y <= grid.rows; ++y)
            dl->AddLine(ImVec2(off.x, off.y + y * cell),
                        ImVec2(off.x + grid.cols * cell, off.y + y * cell), guide);
    }

    for (const auto& e : app.effects) {
        ImVec2 p0(off.x + e.x * cell, off.y + e.y * cell);
        ImVec2 p1(off.x + (e.x + e.w) * cell, off.y + (e.y + e.h) * cell);
        ImU32 tint = e.label.empty() ? EffectTint(e.kind) : IM_COL32(200, 160, 255, 90);
        dl->AddRectFilled(p0, p1, tint, 3.0f);
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

    for (const auto& a : app.annotations) {
        ImVec2 p0(off.x + a.x * cell, off.y + a.y * cell);
        ImVec2 p1(off.x + (a.x + a.w) * cell, off.y + (a.y + a.h) * cell);
        dl->AddRectFilled(p0, p1, IM_COL32(90, 170, 220, 40));
        dl->AddRect(p0, p1, IM_COL32(120, 220, 255, 220), 0, 0, 2.0f);
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

    static std::vector<ImVec4> areaChips;
    areaChips.assign(app.map.areas.size(), ImVec4(0, 0, 0, 0));
    for (size_t i = 0; i < app.map.areas.size(); ++i) {
        const Area& a = app.map.areas[i];
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
        bool inGrid = grid.Inside(cx, cy) && cx >= border && cy >= border &&
                      cx < grid.cols - border && cy < grid.rows - border;
        bool inMargin = grid.Inside(cx, cy) && !inGrid;

        if (hovered && inGrid) {
            ImVec2 a(off.x + cx * cell, off.y + cy * cell);
            dl->AddRect(a, ImVec2(a.x + cell, a.y + cell), IM_COL32(255, 214, 120, 220), 0, 0,
                        std::max(1.5f, cell * 0.06f));
        }

        int hitFeature = -1, hitEffect = -1, hitArea = -1;
        if (inGrid) {
            for (int i = 0; i < (int)app.features.size(); ++i)
                if (app.features[i].x == cx && app.features[i].y == cy) hitFeature = i;
            for (int i = 0; i < (int)app.effects.size(); ++i) {
                const Effect& e = app.effects[i];
                if (cx >= e.x && cx < e.x + e.w && cy >= e.y && cy < e.y + e.h) hitEffect = i;
            }
            for (int i = 0; i < (int)app.annotations.size(); ++i) {
                const Annotation& a = app.annotations[i];
                if (cx >= a.x && cx < a.x + a.w && cy >= a.y && cy < a.y + a.h) hitArea = i;
            }
        }

        int hitChip = -1;
        for (int i = 0; i < (int)areaChips.size(); ++i) {
            const ImVec4& r = areaChips[i];
            if (r.z <= r.x) continue;
            if (io.MousePos.x >= r.x && io.MousePos.x <= r.z &&
                io.MousePos.y >= r.y && io.MousePos.y <= r.w) hitChip = i;
        }

        static int dragArea = -1;
        static int dragDX = 0, dragDY = 0;
        if (hovered && hitChip >= 0 && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            app.PushUndo();
            dragArea = hitChip;
            dragDX = cx - app.map.areas[hitChip].x;
            dragDY = cy - app.map.areas[hitChip].y;
        }
        if (dragArea >= 0) {
            if (dragArea < (int)app.map.areas.size() &&
                ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                Area& a = app.map.areas[dragArea];
                a.x = std::clamp(cx - dragDX, 0, std::max(0, grid.cols - a.w));
                a.y = std::clamp(cy - dragDY, 0, std::max(0, grid.rows - a.h));
                app.MarkEdited();
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

        if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) rightPress = io.MousePos;
        if (hovered && ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
            float dx = io.MousePos.x - rightPress.x, dy = io.MousePos.y - rightPress.y;
            if (dx * dx + dy * dy < 25.0f) {
                menuFeature = hitFeature;
                menuEffect = hitEffect;
                menuArea = hitArea;
                menuNamed = hitChip;
                if (menuNamed < 0) {
                    for (int i = 0; i < (int)app.map.areas.size(); ++i) {
                        const Area& a = app.map.areas[i];
                        if (cx >= a.x && cx < a.x + a.w && cy >= a.y && cy < a.y + a.h)
                            menuNamed = i;
                    }
                }
                menuX = cx;
                menuY = cy;
                ImGui::OpenPopup("##mapctx");
            }
        }

        if (hovered && inGrid && !ImGui::IsPopupOpen("##mapctx")) {
            std::string text;
            if (hitFeature >= 0) {
                const Feature& f = app.features[hitFeature];
                text = f.label.empty() ? PropLabel(f.kind) : f.label;
                if (!f.description.empty()) text += "\n" + f.description;
            }
            if (hitEffect >= 0) {
                const Effect& e = app.effects[hitEffect];
                if (!text.empty()) text += "\n";
                text += std::string("Effect: ") +
                        (e.label.empty() ? EffectLabel(e.kind) : e.label);
            }
            if (hitArea >= 0) {
                const Annotation& a = app.annotations[hitArea];
                if (!text.empty()) text += "\n";
                text += "Custom area: " + a.label;
            }
            if (text.empty()) text = TileName(grid.Get(cx, cy));
            ImGui::SetTooltip("%s\n[%d, %d] - right-click for options", text.c_str(), cx, cy);
        }

        if (hovered && hitChip >= 0 && hitChip < (int)app.map.areas.size() &&
            !ImGui::IsPopupOpen("##mapctx")) {
            const Area& a = app.map.areas[hitChip];
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
            if (menuFeature >= 0 && menuFeature < (int)app.features.size()) {
                anything = true;
                Feature& f = app.features[menuFeature];
                ImGui::SeparatorText(f.label.empty() ? PropLabel(f.kind) : f.label.c_str());
                if (f.label.empty()) {
                    ImGui::TextDisabled("From the catalogue.");
                } else {
                    ImGui::SetNextItemWidth(240.0f);
                    if (InputTextString("Name##ctxp", &f.label)) app.MarkEdited();
                    if (InputTextMultilineString("##ctxpd", &f.description, ImVec2(300, 58)))
                        app.MarkEdited();
                    int el = (int)f.elaboration;
                    ImGui::SetNextItemWidth(240.0f);
                    if (ImGui::Combo("Embellish##ctxp", &el,
                                     "No - exactly as written\0A little\0Freely\0")) {
                        f.elaboration = (Elaboration)el;
                        app.MarkEdited();
                    }
                }
                if (ImGui::MenuItem("Delete this object")) {
                    app.PushUndo();
                    app.features.erase(app.features.begin() + menuFeature);
                    menuFeature = -1;
                    ImGui::CloseCurrentPopup();
                }
            }
            if (menuEffect >= 0 && menuEffect < (int)app.effects.size()) {
                anything = true;
                Effect& e = app.effects[menuEffect];
                ImGui::SeparatorText(e.label.empty() ? EffectLabel(e.kind) : e.label.c_str());
                if (!e.label.empty()) {
                    ImGui::SetNextItemWidth(240.0f);
                    if (InputTextString("Name##ctxe", &e.label)) app.MarkEdited();
                    if (InputTextMultilineString("##ctxed", &e.description, ImVec2(300, 58)))
                        app.MarkEdited();
                    int el = (int)e.elaboration;
                    ImGui::SetNextItemWidth(240.0f);
                    if (ImGui::Combo("Embellish##ctxe", &el,
                                     "No - exactly as written\0A little\0Freely\0")) {
                        e.elaboration = (Elaboration)el;
                        app.MarkEdited();
                    }
                }
                int strength = e.intensity == "low" ? 0 : (e.intensity == "high" ? 2 : 1);
                ImGui::SetNextItemWidth(240.0f);
                if (ImGui::Combo("Strength##ctxe", &strength, "Faint\0Clear\0Heavy\0")) {
                    e.intensity = strength == 0 ? "low" : (strength == 2 ? "high" : "medium");
                    app.MarkEdited();
                }
                if (ImGui::MenuItem("Delete this effect")) {
                    app.PushUndo();
                    app.effects.erase(app.effects.begin() + menuEffect);
                    menuEffect = -1;
                    ImGui::CloseCurrentPopup();
                }
            }
            if (menuArea >= 0 && menuArea < (int)app.annotations.size()) {
                anything = true;
                Annotation& a = app.annotations[menuArea];
                ImGui::SeparatorText(a.label.empty() ? "Custom area" : a.label.c_str());
                ImGui::SetNextItemWidth(240.0f);
                if (InputTextString("Name##ctxa", &a.label)) app.MarkEdited();
                if (InputTextMultilineString("##ctxad", &a.description, ImVec2(300, 58)))
                    app.MarkEdited();
                int el = (int)a.elaboration;
                ImGui::SetNextItemWidth(240.0f);
                if (ImGui::Combo("Embellish##ctxa", &el,
                                 "No - exactly as written\0A little\0Freely\0")) {
                    a.elaboration = (Elaboration)el;
                    app.MarkEdited();
                }
                if (ImGui::MenuItem("Delete this area")) {
                    app.PushUndo();
                    app.annotations.erase(app.annotations.begin() + menuArea);
                    menuArea = -1;
                    ImGui::CloseCurrentPopup();
                }
            }
            if (menuNamed >= 0 && menuNamed < (int)app.map.areas.size()) {
                anything = true;
                Area& a = app.map.areas[menuNamed];
                ImGui::SeparatorText(a.label.empty() ? "Area" : a.label.c_str());
                ImGui::TextDisabled("Names this part of the map for the renderer.");
                ImGui::SetNextItemWidth(240.0f);
                if (InputTextString("Name##ctxar", &a.label)) app.MarkEdited();
                ImGui::SetItemTooltip("What this room is called. Sent to the renderer with "
                                      "the room's rectangle; never drawn in the picture.");
                if (InputTextMultilineString("##ctxard", &a.description, ImVec2(300, 58)))
                    app.MarkEdited();
                ImGui::SetItemTooltip("Anything else the painter should know about this "
                                      "room. Optional.");
                if (ImGui::MenuItem("Delete this area")) {
                    app.PushUndo();
                    app.map.areas.erase(app.map.areas.begin() + menuNamed);
                    menuNamed = -1;
                    ImGui::CloseCurrentPopup();
                }
            }
            if (!anything) {
                ImGui::SeparatorText(TileName(grid.Get(menuX, menuY)));
                ImGui::TextDisabled("Nothing placed on this square.");
                if (ImGui::MenuItem("Paint with this material")) {
                    app.paintTile = grid.Get(menuX, menuY);
                    app.tool = Tool::Paint;
                }
            }
            ImGui::EndPopup();
        }

        static bool strokeActive = false;
        static int rectStartX = 0, rectStartY = 0;
        bool leftDown = ImGui::IsMouseDown(ImGuiMouseButton_Left);
        bool leftClicked = hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left);
        bool leftReleased = ImGui::IsMouseReleased(ImGuiMouseButton_Left);

        if (app.tool == Tool::Paint && inGrid) {
            if (leftClicked) { app.PushUndo(); strokeActive = true; }
            if (strokeActive && leftDown) {
                int r = app.brushSize - 1;
                for (int dy = -r; dy <= r; ++dy)
                    for (int dx = -r; dx <= r; ++dx) grid.Set(cx + dx, cy + dy, app.paintTile);
                app.MarkEdited();
            }
            if (leftReleased) strokeActive = false;
        } else if (app.tool == Tool::RectFill && inGrid) {
            if (leftClicked) { rectStartX = cx; rectStartY = cy; strokeActive = true; }
            if (strokeActive) {
                int x0 = std::min(rectStartX, cx), x1 = std::max(rectStartX, cx);
                int y0 = std::min(rectStartY, cy), y1 = std::max(rectStartY, cy);
                ImVec2 a(off.x + x0 * cell, off.y + y0 * cell);
                ImVec2 b(off.x + (x1 + 1) * cell, off.y + (y1 + 1) * cell);
                dl->AddRectFilled(a, b, TileColor(app.paintTile) & 0x60FFFFFF);
                dl->AddRect(a, b, IM_COL32(255, 214, 120, 255), 0, 0, 2.0f);
                float gr = std::min(std::min(b.x - a.x, b.y - a.y) * 0.30f, 26.0f);
                if (gr > 5.0f)
                    DrawTileGlyph(dl, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), gr,
                                  app.paintTile, IM_COL32(255, 236, 190, 235));
                if (leftReleased) {
                    app.PushUndo();
                    grid.FillRect(x0, y0, x1 - x0 + 1, y1 - y0 + 1, app.paintTile);
                    strokeActive = false;
                    app.MarkEdited();
                }
            }
        } else if (app.tool == Tool::PlaceProp && inGrid) {
            if (leftClicked) {
                app.PushUndo();
                app.features.erase(
                    std::remove_if(app.features.begin(), app.features.end(),
                                   [&](const Feature& f) { return f.x == cx && f.y == cy; }),
                    app.features.end());
                Feature nf;
                if (app.customProp && !app.customLabel.empty()) {
                    nf.kind = "custom";
                    nf.label = app.customLabel;
                    nf.description = app.customDesc;
                    nf.elaboration = (Elaboration)app.customElaboration;
                    nf.structural = true;
                } else {
                    nf.kind = app.propKind;
                    nf.structural = arch::IsStructuralProp(nf.kind);
                }
                nf.x = cx;
                nf.y = cy;
                app.features.push_back(nf);
                app.MarkEdited();
            }
        } else if (app.tool == Tool::Annotate && inGrid) {
            if (leftClicked) { rectStartX = cx; rectStartY = cy; strokeActive = true; }
            if (strokeActive) {
                int x0 = std::min(rectStartX, cx), x1 = std::max(rectStartX, cx);
                int y0 = std::min(rectStartY, cy), y1 = std::max(rectStartY, cy);
                dl->AddRect(ImVec2(off.x + x0 * cell, off.y + y0 * cell),
                            ImVec2(off.x + (x1 + 1) * cell, off.y + (y1 + 1) * cell),
                            IM_COL32(120, 220, 255, 255), 0, 0, 2.5f);
                if (leftReleased) {
                    strokeActive = false;
                    if (!app.annLabel.empty()) {
                        app.PushUndo();
                        Annotation a;
                        a.label = app.annLabel;
                        a.description = app.annDesc;
                        a.elaboration = (Elaboration)app.annElaboration;
                        a.x = x0;
                        a.y = y0;
                        a.w = x1 - x0 + 1;
                        a.h = y1 - y0 + 1;
                        app.annotations.push_back(a);
                        app.MarkEdited();
                    }
                }
            }
        } else if (app.tool == Tool::Effects && inGrid) {
            if (leftClicked) { rectStartX = cx; rectStartY = cy; strokeActive = true; }
            if (strokeActive) {
                int x0 = std::min(rectStartX, cx), x1 = std::max(rectStartX, cx);
                int y0 = std::min(rectStartY, cy), y1 = std::max(rectStartY, cy);
                ImU32 tint = app.customEffect ? IM_COL32(200, 160, 255, 90)
                                              : EffectTint(app.effectKind);
                ImVec2 a(off.x + x0 * cell, off.y + y0 * cell);
                ImVec2 b(off.x + (x1 + 1) * cell, off.y + (y1 + 1) * cell);
                dl->AddRectFilled(a, b, tint);
                float gr = std::min(std::min(b.x - a.x, b.y - a.y) * 0.30f, 26.0f);
                if (gr > 5.0f && !app.customEffect)
                    DrawEffectGlyph(dl, ImVec2((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f), gr,
                                    app.effectKind.c_str(), IM_COL32(255, 255, 255, 230));
                if (leftReleased) {
                    strokeActive = false;
                    bool custom = app.customEffect && !app.effLabel.empty();
                    if (custom || !app.customEffect) {
                        app.PushUndo();
                        Effect e;
                        e.kind = custom ? "custom" : app.effectKind;
                        if (custom) {
                            e.label = app.effLabel;
                            e.description = app.effDesc;
                            e.elaboration = (Elaboration)app.effElaboration;
                        }
                        e.intensity = app.effIntensity == 0 ? "low"
                                    : (app.effIntensity == 2 ? "high" : "medium");
                        e.x = x0;
                        e.y = y0;
                        e.w = x1 - x0 + 1;
                        e.h = y1 - y0 + 1;
                        app.effects.push_back(e);
                        app.MarkEdited();
                    }
                }
            }
        } else if (app.tool == Tool::EraseProp && inGrid) {
            if (leftDown && hovered) {
                size_t before = app.features.size();
                app.features.erase(
                    std::remove_if(app.features.begin(), app.features.end(),
                                   [&](const Feature& f) { return f.x == cx && f.y == cy; }),
                    app.features.end());
                if (app.features.size() != before) app.MarkEdited();
            }
        }
    }

    dl->PopClipRect();
}

} // namespace dnd
