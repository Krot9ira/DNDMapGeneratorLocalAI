#pragma once
// map.json <-> MapData. The on-disk format is identical to the one the Python
// tools and AI agents use, so maps move freely between the app and the CLI.
#include "map_types.h"
#include "map_architect.h"

#include <nlohmann/json.hpp>

#include <filesystem>
#include <fstream>
#include <string>

namespace dnd {

class MapSerializer {
public:
    static nlohmann::json ToJson(const MapData& map) {
        nlohmann::json j;
        j["meta"] = {{"name", map.meta.name},
                     {"title", map.meta.title},
                     {"style", map.meta.style},
                     {"layout", map.meta.layout},
                     {"scene_summary", map.meta.scene_summary},
                     {"render_details", map.meta.render_details},
                     {"terrain_kind", map.meta.terrain_kind},
                     {"terrain_amount", map.meta.terrain_amount},
                     {"prop_density", map.meta.prop_density},
                     {"border", map.meta.border},
                     {"seed", map.meta.seed}};
        j["grid"] = {{"cols", map.grid.cols},
                     {"rows", map.grid.rows},
                     {"cell_px", map.grid.cell_px}};

        j["zones"] = nlohmann::json::array();
        for (const auto& z : map.zones)
            j["zones"].push_back({{"id", z.id}, {"kind", z.kind},
                                  {"x", z.x}, {"y", z.y}, {"w", z.w}, {"h", z.h}});

        j["features"] = nlohmann::json::array();
        for (const auto& f : map.features) {
            nlohmann::json fj = {{"kind", f.kind}, {"x", f.x}, {"y", f.y},
                                 {"structural", f.structural}};
            if (!f.label.empty()) fj["label"] = f.label;
            if (!f.description.empty()) fj["description"] = f.description;
            if (!f.label.empty() || !f.description.empty())
                fj["elaboration"] = ElaborationName(f.elaboration);
            j["features"].push_back(fj);
        }

        j["structures"] = nlohmann::json::array();
        for (const auto& st : map.structures)
            j["structures"].push_back({{"kind", st.kind}, {"x", st.x}, {"y", st.y},
                                       {"w", st.w}, {"h", st.h}, {"facing", st.facing}});

        j["annotations"] = nlohmann::json::array();
        for (const auto& a : map.annotations)
            j["annotations"].push_back({{"label", a.label}, {"description", a.description},
                                        {"elaboration", ElaborationName(a.elaboration)},
                                        {"x", a.x}, {"y", a.y}, {"w", a.w}, {"h", a.h}});

        j["effects"] = nlohmann::json::array();
        for (const auto& e : map.effects)
            j["effects"].push_back({{"kind", e.kind}, {"label", e.label},
                                    {"description", e.description},
                                    {"elaboration", ElaborationName(e.elaboration)},
                                    {"intensity", e.intensity},
                                    {"x", e.x}, {"y", e.y}, {"w", e.w}, {"h", e.h}});

        j["areas"] = nlohmann::json::array();
        for (const auto& a : map.areas)
            j["areas"].push_back({{"id", a.id}, {"label", a.label},
                                  {"x", a.x}, {"y", a.y}, {"w", a.w}, {"h", a.h}});

        // Labels exist for the human preview only and are never rendered into
        // the control image.
        j["labels"] = nlohmann::json::array();
        for (const auto& a : map.areas)
            if (!a.label.empty())
                j["labels"].push_back({{"text", a.label},
                                       {"x", a.x + a.w / 2}, {"y", a.y + a.h / 2},
                                       {"size", "md"}});
        return j;
    }

    static bool FromJson(const nlohmann::json& j, MapData& out) {
        try {
            out.Clear();
            if (j.contains("meta") && j["meta"].is_object()) {
                const auto& m = j["meta"];
                out.meta.name = m.value("name", "battlemap");
                out.meta.title = m.value("title", "Battle Map");
                out.meta.style = m.value("style", "");
                out.meta.layout = m.value("layout", "dungeon");
                out.meta.scene_summary = m.value("scene_summary", "");
                out.meta.render_details = m.value("render_details", "");
                out.meta.terrain_kind = m.value("terrain_kind", std::string("none"));
                out.meta.terrain_amount = m.value("terrain_amount", std::string("medium"));
                out.meta.prop_density = m.value("prop_density", std::string("high"));
                out.meta.border = m.value("border", 0);
                if (m.contains("seed") && m["seed"].is_number())
                    out.meta.seed = m["seed"].get<int64_t>();
            }
            if (j.contains("grid") && j["grid"].is_object()) {
                const auto& g = j["grid"];
                out.grid.cols = g.value("cols", 25);
                out.grid.rows = g.value("rows", 19);
                out.grid.cell_px = g.value("cell_px", 32);
            }
            if (j.contains("zones") && j["zones"].is_array()) {
                for (const auto& zj : j["zones"]) {
                    Zone z;
                    z.id = zj.value("id", std::string("zone"));
                    z.kind = zj.value("kind", std::string("floor"));
                    z.x = zj.value("x", 0);
                    z.y = zj.value("y", 0);
                    z.w = zj.value("w", 1);
                    z.h = zj.value("h", 1);
                    out.zones.push_back(z);
                }
            }
            if (j.contains("features") && j["features"].is_array()) {
                for (const auto& fj : j["features"]) {
                    Feature f;
                    f.kind = fj.value("kind", std::string("pillar"));
                    f.x = fj.value("x", 0);
                    f.y = fj.value("y", 0);
                    f.label = fj.value("label", std::string(""));
                    f.description = fj.value("description", std::string(""));
                    f.elaboration = ElaborationFromName(
                        fj.value("elaboration", std::string("some")));
                    f.structural = fj.value("structural",
                                            arch::IsStructuralProp(f.kind) || !f.label.empty());
                    out.features.push_back(f);
                }
            }
            if (j.contains("structures") && j["structures"].is_array()) {
                for (const auto& sj : j["structures"]) {
                    Structure st;
                    st.kind = sj.value("kind", std::string("ship"));
                    st.x = sj.value("x", 0);
                    st.y = sj.value("y", 0);
                    st.w = sj.value("w", 1);
                    st.h = sj.value("h", 1);
                    st.facing = sj.value("facing", std::string("e"));
                    out.structures.push_back(st);
                }
            }
            if (j.contains("annotations") && j["annotations"].is_array()) {
                for (const auto& aj : j["annotations"]) {
                    Annotation a;
                    a.label = aj.value("label", std::string(""));
                    if (a.label.empty()) continue;
                    a.description = aj.value("description", std::string(""));
                    a.elaboration = ElaborationFromName(
                        aj.value("elaboration", std::string("some")));
                    a.x = aj.value("x", 0);
                    a.y = aj.value("y", 0);
                    a.w = aj.value("w", 1);
                    a.h = aj.value("h", 1);
                    out.annotations.push_back(a);
                }
            }
            if (j.contains("effects") && j["effects"].is_array()) {
                for (const auto& ej : j["effects"]) {
                    Effect e;
                    e.kind = ej.value("kind", std::string("fog"));
                    e.label = ej.value("label", std::string(""));
                    e.description = ej.value("description", std::string(""));
                    e.elaboration = ElaborationFromName(
                        ej.value("elaboration", std::string("some")));
                    e.intensity = ej.value("intensity", std::string("medium"));
                    e.x = ej.value("x", 0);
                    e.y = ej.value("y", 0);
                    e.w = ej.value("w", 1);
                    e.h = ej.value("h", 1);
                    out.effects.push_back(e);
                }
            }
            if (j.contains("areas") && j["areas"].is_array()) {
                for (const auto& aj : j["areas"]) {
                    Area a;
                    a.id = aj.value("id", std::string(""));
                    a.label = aj.value("label", std::string(""));
                    a.x = aj.value("x", 0);
                    a.y = aj.value("y", 0);
                    a.w = aj.value("w", 1);
                    a.h = aj.value("h", 1);
                    out.areas.push_back(a);
                }
            }
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }

    static bool SaveToFile(const std::string& path, const MapData& map) {
        try {
            auto parent = std::filesystem::path(path).parent_path();
            if (!parent.empty()) std::filesystem::create_directories(parent);
            std::ofstream f(path);
            if (!f.is_open()) return false;
            f << ToJson(map).dump(2);
            return true;
        } catch (const std::exception&) {
            return false;
        }
    }

    static bool LoadFromFile(const std::string& path, MapData& out) {
        try {
            std::ifstream f(path);
            if (!f.is_open()) return false;
            nlohmann::json j;
            f >> j;
            return FromJson(j, out);
        } catch (const std::exception&) {
            return false;
        }
    }
};

}  // namespace dnd
