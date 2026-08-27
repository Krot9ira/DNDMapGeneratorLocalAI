#pragma once
// Style library and prompt assembly.
//
// Styles live as individual JSON files in styles/ so they can be edited, added
// and removed without touching the app. styles/_base.json holds the fragments
// shared by every style - most importantly the negative list that keeps text,
// creatures and grid overlays out of every render.
#include "map_architect.h"
#include "map_types.h"
#include "map_serializer.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace dnd {

class StyleManager {
public:
    std::string stylesDir = "styles";
    std::string presetsDir = "presets";
    std::map<std::string, StyleDef> styles;
    std::map<std::string, MapData> presets;
    BaseStyle base;
    Phrasebook phrases;
    std::string lastError;

    void LoadAll() {
        styles.clear();
        presets.clear();
        LoadBase();
        LoadPhrases();

        if (fs::exists(stylesDir)) {
            for (const auto& entry : fs::directory_iterator(stylesDir)) {
                if (entry.path().extension() != ".json") continue;
                std::string stem = entry.path().stem().string();
                if (!stem.empty() && stem[0] == '_') continue;  // _base and friends
                StyleDef s;
                if (LoadStyleFile(entry.path().string(), s)) styles[s.id] = s;
            }
        }
        if (fs::exists(presetsDir)) {
            for (const auto& entry : fs::directory_iterator(presetsDir)) {
                if (entry.path().extension() != ".json") continue;
                MapData m;
                if (MapSerializer::LoadFromFile(entry.path().string(), m))
                    presets[entry.path().stem().string()] = m;
            }
        }
    }

    void LoadBase() {
        base.aesthetics =
            "Hand-painted fantasy cartography, rough painterly brushwork with visible strokes, "
            "strong dark ink outlines around every object and wall, restrained desaturated "
            "colour, inked line art over watercolour and gouache";
        base.medium = "Inked line art combined with watercolour and gouache painting over "
                      "digital texture";
        base.lighting = "Soft warm ambient light from directly above with gentle directional "
                        "shadows beneath walls and furniture";
        base.forbidden_suffix =
            "The scene is completely empty of people, creatures and animals, and carries no "
            "text, letters, numbers, labels or grid lines anywhere";
        base.border_note =
            "A plain flat unpainted margin runs right around the outside of the image on all "
            "four sides, empty of scenery, buildings, water and props, exactly like the blank "
            "paper border of a printed battle map sheet; the map itself sits wholly inside "
            "that margin and its outer edge is a clean straight line";
        base.background_suffix =
            "painted with visible brushwork, grime in the joints and damp patches, every part "
            "of the ground fully painted with no blank patches inside the map area";
        base.default_palette = {"#C8B99A", "#8A7B63", "#4A4038", "#2E2A26", "#6E7A6B"};

        fs::path p = fs::path(stylesDir) / "_base.json";
        if (!fs::exists(p)) return;
        try {
            std::ifstream f(p);
            nlohmann::json j;
            f >> j;
            base.description = j.value("description", base.description);
            base.aesthetics = j.value("aesthetics", base.aesthetics);
            base.medium = j.value("medium", base.medium);
            base.lighting = j.value("lighting", base.lighting);
            base.forbidden_suffix = j.value("forbidden_suffix", base.forbidden_suffix);
            base.background_suffix = j.value("background_suffix", base.background_suffix);
            base.border_note = j.value("border_note", base.border_note);
            base.viewpoint_note = j.value("viewpoint_note", base.viewpoint_note);
            base.viewpoint_note_open = j.value("viewpoint_note_open", base.viewpoint_note_open);
            if (j.contains("default_palette") && j["default_palette"].is_array()) {
                base.default_palette.clear();
                for (const auto& c : j["default_palette"])
                    base.default_palette.push_back(c.get<std::string>());
            }
        } catch (const std::exception& e) {
            lastError = std::string("_base.json: ") + e.what();
        }
    }

    // The wording file is optional: anything missing falls back to the text
    // compiled into the caption builder.
    // The architect needs to know which kinds were defined by hand, so it can
    // leave their names alone and pin them with their own rectangles.
    void PublishCustomProps() {
        arch::CustomProps().clear();
        auto it = phrases.sections.find("props");
        if (it == phrases.sections.end()) return;
        for (const auto& [kind, phrase] : it->second)
            if (!phrase.empty() && !arch::KnownProps().count(kind))
                arch::CustomProps().insert(kind);
    }

    void LoadPhrases() {
        phrases.sections.clear();
        fs::path p = fs::path(stylesDir) / "_phrases.json";
        if (!fs::exists(p)) return;
        try {
            std::ifstream f(p);
            nlohmann::json j;
            f >> j;
            for (auto& [section, values] : j.items()) {
                if (!values.is_object()) continue;
                for (auto& [key, value] : values.items())
                    if (value.is_string())
                        phrases.sections[section][key] = value.get<std::string>();
            }
        } catch (const std::exception& e) {
            lastError = std::string("_phrases.json: ") + e.what();
        }
        PublishCustomProps();
    }

    bool SavePhrases() {
        try {
            fs::create_directories(stylesDir);
            fs::path p = fs::path(stylesDir) / "_phrases.json";
            nlohmann::json j;
            // Keep the descriptive header the file ships with.
            if (fs::exists(p)) {
                std::ifstream in(p);
                in >> j;
            }
            j["id"] = "_phrases";
            j["name"] = "Object wording";
            for (const auto& [section, values] : phrases.sections) {
                nlohmann::json out = nlohmann::json::object();
                for (const auto& [key, value] : values) out[key] = value;
                j[section] = out;
            }
            std::ofstream f(p);
            if (!f.is_open()) return false;
            f << j.dump(2);
            return true;
        } catch (const std::exception& e) {
            lastError = e.what();
            return false;
        }
    }

    bool LoadStyleFile(const std::string& path, StyleDef& out) {
        try {
            std::ifstream f(path);
            nlohmann::json j;
            f >> j;
            out.id = j.value("id", fs::path(path).stem().string());
            out.name = j.value("name", out.id);
            out.category = j.value("category", std::string("General"));
            out.description = j.value("description", std::string(""));
            out.materials = j.value("materials", std::string(""));
            out.palette = j.value("palette", std::string(""));
            out.lighting = j.value("lighting", std::string(""));
            out.aesthetics = j.value("aesthetics", std::string(""));
            out.ground = j.value("ground", std::string(""));
            out.hex_palette.clear();
            if (j.contains("hex_palette") && j["hex_palette"].is_array())
                for (const auto& c : j["hex_palette"])
                    out.hex_palette.push_back(c.get<std::string>());
            out.default_layout = j.value("default_layout", std::string("dungeon"));
            out.enclosure = j.value("enclosure", std::string());
            out.wall = j.value("wall", std::string());
            out.face = j.value("face", std::string());
            out.boundary = j.value("boundary", std::string());
            out.props.clear();
            if (j.contains("props") && j["props"].is_array())
                for (const auto& p : j["props"]) out.props.push_back(p.get<std::string>());
            out.tags.clear();
            if (j.contains("tags") && j["tags"].is_array())
                for (const auto& t : j["tags"]) out.tags.push_back(t.get<std::string>());
            return true;
        } catch (const std::exception& e) {
            lastError = fs::path(path).filename().string() + ": " + e.what();
            return false;
        }
    }

    const StyleDef* Find(const std::string& id) const {
        auto it = styles.find(id);
        return it != styles.end() ? &it->second : nullptr;
    }

    bool SaveStyle(const StyleDef& s) {
        try {
            fs::create_directories(stylesDir);
            nlohmann::json j;
            j["id"] = s.id;
            j["name"] = s.name;
            j["category"] = s.category;
            j["description"] = s.description;
            j["materials"] = s.materials;
            j["palette"] = s.palette;
            j["lighting"] = s.lighting;
            j["aesthetics"] = s.aesthetics;
            j["ground"] = s.ground;
            j["hex_palette"] = s.hex_palette;
            j["default_layout"] = s.default_layout;
            j["props"] = s.props;
            j["tags"] = s.tags;
            std::ofstream f(fs::path(stylesDir) / (s.id + ".json"));
            if (!f.is_open()) return false;
            f << j.dump(2);
            styles[s.id] = s;
            return true;
        } catch (const std::exception& e) {
            lastError = e.what();
            return false;
        }
    }

    bool SaveBase() {
        try {
            fs::create_directories(stylesDir);
            nlohmann::json j;
            j["id"] = "_base";
            j["name"] = "Shared caption contract";
            j["description"] = base.description;
            j["aesthetics"] = base.aesthetics;
            j["medium"] = base.medium;
            j["lighting"] = base.lighting;
            j["forbidden_suffix"] = base.forbidden_suffix;
            j["background_suffix"] = base.background_suffix;
            j["border_note"] = base.border_note;
            j["viewpoint_note"] = base.viewpoint_note;
            j["viewpoint_note_open"] = base.viewpoint_note_open;
            j["default_palette"] = base.default_palette;
            std::ofstream f(fs::path(stylesDir) / "_base.json");
            if (!f.is_open()) return false;
            f << j.dump(2);
            return true;
        } catch (const std::exception& e) {
            lastError = e.what();
            return false;
        }
    }

    bool DeleteStyle(const std::string& id) {
        std::error_code ec;
        fs::remove(fs::path(stylesDir) / (id + ".json"), ec);
        styles.erase(id);
        return !ec;
    }

    bool SavePreset(const std::string& name, const MapData& map) {
        try {
            fs::create_directories(presetsDir);
            if (!MapSerializer::SaveToFile((fs::path(presetsDir) / (name + ".json")).string(), map))
                return false;
            presets[name] = map;
            return true;
        } catch (const std::exception& e) {
            lastError = e.what();
            return false;
        }
    }

    bool DeletePreset(const std::string& name) {
        std::error_code ec;
        fs::remove(fs::path(presetsDir) / (name + ".json"), ec);
        presets.erase(name);
        return !ec;
    }

};

}  // namespace dnd
