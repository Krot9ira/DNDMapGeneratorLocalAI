#include "tab_gallery.h"
#include "ui_context.h"
#include <algorithm>

namespace dnd {

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

void TabGallery::DrawPhraseEditor(AppState& app) {
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
        if (app.styles.SavePhrases()) {
            app.job.Log("Saved styles/_phrases.json");
            dirty = false;
        } else {
            app.job.Log("Could not save the wording: " + app.styles.lastError);
        }
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    if (ImGui::Button("Reload from file")) {
        app.styles.LoadPhrases();
        dirty = false;
    }
    ImGui::SetItemTooltip("Throw away unsaved changes and read the file again.");
    if (dirty) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.66f, 0.30f, 1.0f), "unsaved");
    }

    std::string needle = filter;
    for (char& c : needle) c = (char)tolower((unsigned char)c);

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
        app.styles.phrases.sections["props"][key] = newPhrase;
        if (app.styles.SavePhrases()) {
            app.job.Log("Added object '" + key + "' to styles/_phrases.json");
            newKey.clear();
            newPhrase.clear();
        } else {
            app.job.Log("Could not save: " + app.styles.lastError);
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
        auto it = app.styles.phrases.sections.find(sec.key);
        if (it == app.styles.phrases.sections.end() || it->second.empty()) continue;

        int shown = 0;
        for (const auto& kv : it->second) {
            if (needle.empty()) { ++shown; continue; }
            std::string hay = kv.first + " " + kv.second;
            for (char& c : hay) c = (char)tolower((unsigned char)c);
            if (hay.find(needle) != std::string::npos) ++shown;
        }
        if (!shown) continue;

        if (!ImGui::CollapsingHeader(sec.title, needle.empty() && std::string(sec.key) == "structure"
                                                    ? ImGuiTreeNodeFlags_DefaultOpen
                                                    : (needle.empty() ? 0 : ImGuiTreeNodeFlags_DefaultOpen)))
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
    if (app.styles.phrases.sections.empty()) {
        ImGui::TextDisabled("styles/_phrases.json is missing, so the built-in wording is in "
                            "use. Reinstall the styles folder to edit it here.");
    }
    ImGui::EndChild();
}

void TabGallery::Draw(AppState& app) {
    static bool editingPhrases = false;
    static std::string editingId;
    static StyleDef editing;
    static bool editingBase = false;

    ImGui::BeginChild("##stylelist", ImVec2(PanelWidth(0.22f, 250.0f, 560.0f), 0),
                      ImGuiChildFlags_Borders);
    ImGui::TextColored(AccentGold(), "Styles");
    ImGui::TextDisabled("One JSON file each, in styles/");

    if (ImGui::Button("New style", ImVec2(-1, 26))) {
        editing = StyleDef{};
        editing.id = "custom_" + std::to_string(app.styles.styles.size() + 1);
        editing.name = "New style";
        editing.category = "Custom";
        editing.materials = "Describe the materials, surfaces and colours of this place.";
        editing.ground = "worn stone paving";
        editing.default_layout = "building";
        editingId = editing.id;
        editingBase = false;
        editingPhrases = false;
        app.job.Log("New style started. Give it a name and an id, then Save style.");
    }
    ImGui::SetItemTooltip("Starts a blank style. Nothing is written until you press "
                          "Save style, and the file is named after its id.");
    if (ImGui::Button("Reload from disk", ImVec2(-1, 26))) app.styles.LoadAll();
    ImGui::SetItemTooltip("Throw away unsaved edits and read the styles folder again.");

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

    ImGui::BeginChild("##stylenames", ImVec2(0, 0));
    for (const auto& kv : app.styles.styles) {
        if (ImGui::Selectable(kv.second.name.c_str(), editingId == kv.first && !editingBase)) {
            editingId = kv.first;
            editing = kv.second;
            editingBase = false;
            editingPhrases = false;
        }
    }
    ImGui::EndChild();
    ImGui::EndChild();

    ImGui::SameLine();
    ImGui::BeginChild("##styleedit", ImVec2(0, 0), ImGuiChildFlags_Borders);
    if (editingPhrases) {
        DrawPhraseEditor(app);
    } else if (editingBase) {
        ImGui::TextColored(AccentGold(), "Shared caption contract");
        ImGui::Text("Never rendered (this is what bans text and creatures)");
        InputTextMultilineString("##bf", &app.styles.base.forbidden_suffix, ImVec2(-1, 90));
        ImGui::Text("Default aesthetics");
        InputTextMultilineString("##ba", &app.styles.base.aesthetics, ImVec2(-1, 110));
        ImGui::Text("Medium");
        InputTextMultilineString("##bm", &app.styles.base.medium, ImVec2(-1, 60));
        ImGui::Text("Default lighting");
        InputTextMultilineString("##bl", &app.styles.base.lighting, ImVec2(-1, 60));
        ImGui::Text("Background suffix");
        InputTextMultilineString("##bg", &app.styles.base.background_suffix, ImVec2(-1, 80));
        if (ImGui::Button("Save contract", ImVec2(200, 30))) app.styles.SaveBase();
    } else if (!editingId.empty()) {
        bool isNew = app.styles.styles.find(editing.id) == app.styles.styles.end();
        ImGui::TextColored(AccentGold(), "Editing: %s", editing.id.c_str());
        if (isNew) {
            InputTextString("Style id", &editing.id);
            ImGui::SetItemTooltip("Short, lowercase, no spaces - it names the file in "
                                  "styles/ and is how the tools refer to this style.");
        } else {
            ImGui::BeginDisabled();
            std::string frozen = editing.id;
            InputTextString("Style id", &frozen);
            ImGui::EndDisabled();
            ImGui::SetItemTooltip("The id names the file on disk, so it cannot be changed "
                                  "here. Copy the file if you want it under another name.");
        }
        InputTextString("Display name", &editing.name);
        InputTextString("Category", &editing.category);
        InputTextString("Description", &editing.description);
        InputTextString("Palette", &editing.palette);
        InputTextString("Lighting", &editing.lighting);
        int layoutPick = 0;
        for (int i = 1; i < 13; ++i)
            if (editing.default_layout == kLayoutNames[i]) layoutPick = i - 1;
        if (ImGui::Combo("Default layout", &layoutPick, kLayoutNames + 1, 12))
            editing.default_layout = kLayoutNames[layoutPick + 1];
        ImGui::SetItemTooltip("The shape this style builds when the Create tab is left on "
                              "'(from style)'. A typed name that matches nothing would "
                              "silently fall back to dungeon, so it is a list.");
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

        ImGui::BeginDisabled(editing.id.empty() || editing.name.empty());
        if (ImGui::Button("Save style", ImVec2(160, 30))) {
            std::string id;
            for (char c : editing.id)
                id += (c == ' ' ? '_' : (char)tolower((unsigned char)c));
            editing.id = id;
            if (app.styles.SaveStyle(editing)) {
                editingId = editing.id;
                app.job.Log("Saved styles/" + editing.id + ".json");
            } else {
                app.job.Log("Could not save the style: " + app.styles.lastError);
            }
        }
        ImGui::EndDisabled();
        if (editing.id.empty() || editing.name.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("needs an id and a name");
        }
        ImGui::SameLine();
        if (ImGui::Button("Delete style", ImVec2(160, 30))) {
            app.styles.DeleteStyle(editing.id);
            editingId.clear();
        }
    } else {
        ImGui::TextDisabled("Pick a style on the left.");
    }
    ImGui::EndChild();
}

} // namespace dnd
