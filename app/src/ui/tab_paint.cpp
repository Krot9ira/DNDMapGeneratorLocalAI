#include "tab_paint.h"
#include "ui_context.h"
#include "map_canvas.h"
#include "../services/render_service.h"
#include "../../include/ideogram_caption.h"
#include <nlohmann/json.hpp>
#include <fstream>
#include <algorithm>

namespace dnd {

static void CaptionField(const char* label, const nlohmann::ordered_json& obj, const char* key) {
    if (!obj.contains(key) || !obj[key].is_string()) return;
    ImGui::SeparatorText(label);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(obj[key].get<std::string>().c_str());
    ImGui::PopTextWrapPos();
}

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

static const nlohmann::ordered_json& ParsedOrEmpty(const std::string& text) {
    static std::string cached;
    static nlohmann::ordered_json parsed;
    if (text != cached) {
        cached = text;
        try {
            parsed = nlohmann::ordered_json::parse(text);
        } catch (const std::exception&) {
            parsed = nlohmann::ordered_json();
        }
    }
    return parsed;
}

void TabPaint::DrawCaptionPanel(AppState& app) {
    if (!ImGui::CollapsingHeader("Caption that will be sent", ImGuiTreeNodeFlags_DefaultOpen))
        return;

    app.SyncMapFromGrid();
    nlohmann::ordered_json auto_cap = IdeogramCaption::Build(
        app.map, app.styles.Find(app.map.meta.style), app.styles.base, app.styles.phrases);
    std::string autoText = auto_cap.dump(2);

    if (app.captionManual) {
        ImGui::TextColored(ImVec4(0.95f, 0.66f, 0.30f, 1.0f),
                           "Hand-written - the map no longer rewrites this");
    } else {
        ImGui::TextDisabled("Rebuilt from the plan every time you change it.");
    }

    if (app.captionManual) {
        if (ImGui::Button("Back to automatic")) {
            app.captionManual = false;
            app.captionText.clear();
        }
        ImGui::SetItemTooltip("Throw the hand-written version away and go back to the "
                              "caption built from the plan.");
        ImGui::SameLine();
        if (ImGui::Button("Reload from plan")) app.captionText = autoText;
        ImGui::SetItemTooltip("Replace what you typed with a fresh caption built from the "
                              "current plan.");
    } else {
        if (ImGui::Button("Edit by hand")) {
            app.captionManual = true;
            app.captionText = autoText;
        }
        ImGui::SetItemTooltip("Take the caption over and write it yourself. It is then sent "
                              "exactly as you leave it.");
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy")) {
        ImGui::SetClipboardText(app.captionManual ? app.captionText.c_str() : autoText.c_str());
    }
    ImGui::SetItemTooltip("Copy the whole caption to the clipboard.");

    float h = std::clamp(ImGui::GetContentRegionAvail().y - 190.0f, 200.0f, 900.0f);

    if (ImGui::BeginTabBar("##captabs")) {
        if (ImGui::BeginTabItem("Readable")) {
            const nlohmann::ordered_json& j = app.captionManual
                                     ? ParsedOrEmpty(app.captionText)
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
                            ImGui::TextColored(AccentGold(), "%2d.  %s", n, box.c_str());
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
            if (app.captionManual) {
                InputTextMultilineString("##capraw", &app.captionText, ImVec2(-1, h));
                bool valid = !ParsedOrEmpty(app.captionText).is_null();
                if (valid)
                    ImGui::TextColored(ImVec4(0.45f, 0.80f, 0.45f, 1.0f),
                                       "Valid JSON, %d characters.",
                                       (int)app.captionText.size());
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

void TabPaint::Draw(AppState& app, TextureLoader& texLoader) {
    ImGui::BeginChild("##renderleft", ImVec2(PanelWidth(0.34f, 460.0f, 900.0f), 0),
                      ImGuiChildFlags_Borders);

    ComfyConfig& c = app.config.comfy;
    ImGui::TextColored(AccentGold(), "Renderer: Ideogram 4");
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextDisabled("The layout is sent as bounding boxes inside a JSON caption, so the "
                        "plan is followed exactly and no blueprint image is needed.");
    ImGui::PopTextWrapPos();

    static const char* kPresetItems[] = {"Ultra - 64 steps", "Quality - 48 steps",
                                         "Default - 20 steps", "Turbo - 12 steps"};
    static const int kPresetSteps[] = {64, 48, 20, 12};
    static const char* kPresetIds[] = {"Ultra", "Quality", "Default", "Turbo"};
    int preset = c.preset == "Ultra"   ? 0
               : c.preset == "Quality" ? 1
               : c.preset == "Turbo"   ? 3
                                       : 2;
    if (ImGui::Combo("Quality", &preset, kPresetItems, 4)) {
        c.preset = kPresetIds[preset];
        c.steps = kPresetSteps[preset];
    }
    ImGui::SetItemTooltip("How many painting steps the renderer takes. Turbo is for trying "
                          "an idea out; Quality is the one to use. Ultra costs a third again "
                          "the time for a little more settling in the fine detail - worth it "
                          "on a map you are keeping, not on a draft.");
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
    bool busy = app.job.running.load();
    ImGui::BeginDisabled(busy || app.grid.cols <= 0);
    ImGui::PushStyleColor(ImGuiCol_Button, GoButton());
    if (ImGui::Button("RENDER THIS MAP", ImVec2(-1, 40)))
        RenderService::StartRenderCurrent(app);
    ImGui::PopStyleColor();
    ImGui::EndDisabled();
    if (busy && ImGui::Button("Cancel", ImVec2(-1, 24))) app.job.cancel = true;

    ImGui::Text("Status: %s", app.job.Status().c_str());

    ImGui::Separator();
    DrawCaptionPanel(app);

    DrawJobLog(app, 140);
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##renderright", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (texLoader.GetResultTexture()) {
        ImGui::Text("Result: %d x %d px", texLoader.GetResultWidth(), texLoader.GetResultHeight());
        ImGui::SameLine();
        if (ImGui::Button("Save a copy")) {
            std::string dir = OutputDir(app, app.map.meta.name);
            std::string path = dir + "/battlemap_copy.png";
            std::ofstream f(path, std::ios::binary);
            const auto& png = texLoader.GetResultPng();
            f.write((const char*)png.data(), (std::streamsize)png.size());
            app.job.Log("Copy saved to " + path);
        }
        ImVec2 avail = ImGui::GetContentRegionAvail();
        float aspect = texLoader.GetResultHeight() ? (float)texLoader.GetResultWidth() / (float)texLoader.GetResultHeight() : 1.0f;
        float w = std::min(avail.x, avail.y * aspect);
        ImGui::Image((ImTextureID)texLoader.GetResultTexture(), ImVec2(w, w / aspect));
    } else {
        ImGui::TextDisabled("No render yet.");
        ImGui::Separator();
        ImGui::TextColored(AccentGold(), "Plan that will be rendered");
        MapCanvas::Draw(app, false, ImVec2(0, 0));
    }
    ImGui::EndChild();
}

} // namespace dnd
