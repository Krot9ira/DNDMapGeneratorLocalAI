#pragma once
// Stage 1 brain: ask the local model for a *design spec*, never for coordinates.
//
// The model describes the place (which areas, what terrain, what props); the
// architect turns that into geometry. Asking an LLM for grid rectangles is what
// used to produce overlapping rooms and doors in the middle of the floor.
#include "map_types.h"
#include "winhttp_client.h"

#include <nlohmann/json.hpp>

#include <regex>
#include <string>
#include <vector>

namespace dnd {

struct PlanResult {
    bool ok = false;
    DesignSpec spec;
    std::string error;
    std::string raw;
};

class OllamaService {
public:
    static bool CheckConnection(const std::string& baseUrl, std::vector<std::string>& outModels,
                                std::string& outError) {
        outModels.clear();
        HttpResponse resp = WinHttpClient::Get(baseUrl + "/api/tags", 5);
        if (!resp.success) {
            outError = resp.errorMessage.empty()
                           ? ("HTTP " + std::to_string(resp.statusCode))
                           : resp.errorMessage;
            return false;
        }
        try {
            auto j = nlohmann::json::parse(resp.body);
            if (j.contains("models") && j["models"].is_array())
                for (const auto& m : j["models"])
                    if (m.contains("name")) outModels.push_back(m["name"].get<std::string>());
            return true;
        } catch (const std::exception& e) {
            outError = e.what();
            return false;
        }
    }

    // Ollama keeps a model resident for minutes after a request. A zero
    // keep_alive drops it now, which is the difference between ComfyUI having
    // the card to itself and both of them thrashing.
    static bool Unload(const std::string& baseUrl, const std::string& model) {
        if (model.empty()) return false;
        nlohmann::json payload = {{"model", model}, {"keep_alive", 0}, {"prompt", ""}};
        HttpResponse resp =
            WinHttpClient::PostJson(baseUrl + "/api/generate", payload.dump(), 30);
        return resp.success && resp.statusCode >= 200 && resp.statusCode < 300;
    }

    static const char* SystemPrompt() {
        return
            "You are a professional tabletop RPG cartographer designing a top-down D&D battle map.\n"
            "\n"
            "You describe scenes; you never compute coordinates. A separate deterministic engine\n"
            "builds the actual geometry from your description, so do NOT output x/y/width/height.\n"
            "\n"
            "The game master's description is often a single short line. Enriching it is your job:\n"
            "invent plausible, specific, setting-consistent detail so the finished map feels lived in\n"
            "rather than empty. Never answer with a bare restatement of the request.\n"
            "\n"
            "Rules:\n"
            "- Pick the `layout` that matches the scene:\n"
            "  dungeon  = separate rooms linked by corridors (crypts, tombs, keeps, temples)\n"
            "  building = the inside of one structure (tavern, house, shop, ship interior, station)\n"
            "  cavern   = natural irregular cave\n"
            "  open     = outdoor scene with no walls (forest, field, crossroads, camp)\n"
            "  street   = city block with buildings along a road\n"
            "  arena    = one single dramatic chamber (boss fight, throne room)\n"
            "  harbour  = waterfront with a quay, a gangway and a moored ship\n"
            "- `rooms` are the distinct areas of the scene, 3 to 6 of them. Give each a short human\n"
            "  label, a size (s/m/l) and 5 to 9 props that belong in it. Vary the props between areas\n"
            "  so each one has its own purpose and character.\n"
            "- Props are physical objects only: furniture, containers, scenery, tools, light sources.\n"
            "  NEVER list people, creatures, animals or monsters - the game master places those as\n"
            "  tokens afterwards.\n"
            "- Every building is drawn with its roof removed so the interior is visible from above.\n"
            "  Never describe roofs, rooftops or tiles.\n"
            "- `terrain` describes ground hazards covering the map.\n"
            "- `scene_summary` is two or three vivid sentences describing what the place physically\n"
            "  looks like from directly above: the surfaces underfoot, the state of the walls, what has\n"
            "  happened here and what has been left behind.\n"
            "- `render_details` is a dense comma-separated list of concrete materials, surface finishes,\n"
            "  colours, wear and damage, stains, and the quality of the light. Be specific: name the kind\n"
            "  of stone, the kind of timber, what is chipped, damp, scorched or overgrown.\n"
            "- Work it out properly before you answer. Take the description apart: what is underfoot,\n"
            "  what is built, what is growing, what is broken, what is burning, what blocks a line of\n"
            "  sight, where a person comes in and where they can go. Every one of those is something\n"
            "  you can say below, and anything you leave unsaid is invented by the artist instead.\n"
            "- `annotations` are the strongest tool you have: a named region described in your own\n"
            "  words, drawn exactly where you put it. Use one for every specific thing the description\n"
            "  names - a fountain, a collapsed span, a stand of trees, a stair, an altar, a barricade.\n"
            "  Give each a `label` of two or three words and a `description` of one or two sentences\n"
            "  saying what it looks like from directly above. Place it with `where` and size it with\n"
            "  `size`. Six to twelve of them is a rich map; none is a bare one.\n"
            "- `terrain_zones` put the ground itself somewhere: water, pit, rubble, vegetation, bridge,\n"
            "  stairs, wall or plain floor, placed with the same `where` and `size`. Use them whenever\n"
            "  the description says a river runs somewhere, a cliff closes a side, a boardwalk crosses\n"
            "  a bog or a floor has fallen in. Anything you call a cliff, a wall or a barricade in an\n"
            "  annotation needs a `wall` zone under it, or the map will have open floor where you said\n"
            "  there was rock.\n"
            "- `effects` lay fire, smoke, fog, mist, embers, magic glow, sparks, ash, steam or shadow\n"
            "  over a region. They sit on top of everything and change nothing underneath.\n"
            "- `props` may be anything you can name, not only the catalogue: \"bush of raspberries\",\n"
            "  \"toppled milk churn\", \"cracked scrying bowl\". An object nobody has a word for is still\n"
            "  drawn, so name what the scene actually needs rather than the nearest stock item.\n"
            "- `lighting` is optional and says how this place is lit and nothing else - the colour and\n"
            "  quality of the light and where it falls off. Do not use it to say where anything stands:\n"
            "  a lighting line that mentions a central fire puts one on the map whatever the plan says.\n"
            "- `enclosed` on a room says whether it has walls round it. On an outdoor map a street, a\n"
            "  square or a yard has none; a house standing on that street does.\n"
            "Answer with JSON only.\n";
    }

    static nlohmann::json SpecSchema(const std::vector<std::string>& styleIds) {
        nlohmann::json schema;
        schema["type"] = "object";
        auto& props = schema["properties"];
        props["name"] = {{"type", "string"}};
        props["title"] = {{"type", "string"}};
        props["layout"] = {{"type", "string"}, {"enum", arch::LayoutNames()}};
        props["size"] = {{"type", "string"},
                         {"enum", std::vector<std::string>{"small", "medium", "large", "huge"}}};
        props["scene_summary"] = {{"type", "string"}};
        props["render_details"] = {{"type", "string"}};
        props["lighting"] = {{"type", "string"}};
        props["prop_density"] = {{"type", "string"},
                                 {"enum", std::vector<std::string>{"low", "medium", "high"}}};
        if (!styleIds.empty()) props["style"] = {{"type", "string"}, {"enum", styleIds}};

        // Where something goes, without asking for coordinates.
        const std::vector<std::string> places = {
            "across the middle", "along the east edge", "along the north edge",
            "along the south edge", "along the west edge", "centre", "down the middle",
            "east", "north", "north-east", "north-west", "over the whole map", "south",
            "south-east", "south-west", "west"};
        const std::vector<std::string> sizes = {"s", "m", "l"};

        nlohmann::json note;
        note["type"] = "object";
        note["properties"]["label"] = {{"type", "string"}};
        note["properties"]["description"] = {{"type", "string"}};
        note["properties"]["where"] = {{"type", "string"}, {"enum", places}};
        note["properties"]["size"] = {{"type", "string"}, {"enum", sizes}};
        note["required"] = std::vector<std::string>{"label", "description", "where"};
        props["annotations"] = {{"type", "array"}, {"items", note}};

        nlohmann::json fx;
        fx["type"] = "object";
        fx["properties"]["kind"] = {{"type", "string"},
                                    {"enum", std::vector<std::string>{
                                        "fire", "embers", "smoke", "fog", "mist",
                                        "fireflies", "magic_glow", "holy_light",
                                        "poison_gas", "blood", "ice", "webs", "sparks",
                                        "ash", "steam", "shadow"}}};
        fx["properties"]["where"] = {{"type", "string"}, {"enum", places}};
        fx["properties"]["size"] = {{"type", "string"}, {"enum", sizes}};
        fx["properties"]["strength"] = {{"type", "string"},
                                        {"enum", std::vector<std::string>{
                                            "low", "medium", "high"}}};
        fx["required"] = std::vector<std::string>{"kind", "where"};
        props["effects"] = {{"type", "array"}, {"items", fx}};

        nlohmann::json zone;
        zone["type"] = "object";
        zone["properties"]["kind"] = {{"type", "string"},
                                      {"enum", std::vector<std::string>{
                                          "water", "pit", "rubble", "vegetation", "bridge",
                                          "stairs", "wall", "floor"}}};
        zone["properties"]["where"] = {{"type", "string"}, {"enum", places}};
        zone["properties"]["size"] = {{"type", "string"}, {"enum", sizes}};
        zone["required"] = std::vector<std::string>{"kind", "where"};
        props["terrain_zones"] = {{"type", "array"}, {"items", zone}};

        nlohmann::json terrain;
        terrain["type"] = "object";
        terrain["properties"]["kind"] = {
            {"type", "string"},
            {"enum", std::vector<std::string>{"none", "water", "pit", "rubble", "vegetation"}}};
        terrain["properties"]["amount"] = {
            {"type", "string"}, {"enum", std::vector<std::string>{"low", "medium", "high"}}};
        terrain["properties"]["shape"] = {
            {"type", "string"}, {"enum", std::vector<std::string>{"pools", "river"}}};
        terrain["required"] = std::vector<std::string>{"kind"};
        props["terrain"] = terrain;

        nlohmann::json room;
        room["type"] = "object";
        room["properties"]["label"] = {{"type", "string"}};
        room["properties"]["size"] = {{"type", "string"},
                                      {"enum", std::vector<std::string>{"s", "m", "l"}}};
        room["properties"]["terrain"] = {
            {"type", "string"},
            {"enum", std::vector<std::string>{"none", "water", "pit", "rubble", "vegetation"}}};
        room["properties"]["props"] = {{"type", "array"},
                                       {"items", nlohmann::json{{"type", "string"}}}};
        room["required"] = std::vector<std::string>{"label", "size", "props"};
        props["rooms"] = {{"type", "array"}, {"items", room}};

        schema["required"] = std::vector<std::string>{"title", "layout", "size", "scene_summary",
                                                      "render_details", "rooms"};
        return schema;
    }

    static PlanResult PlanScene(const OllamaConfig& cfg, const std::string& sceneDescription,
                                const std::string& styleId, const std::string& size,
                                const std::vector<std::string>& styleIds,
                                const std::string& styleCatalogue) {
        PlanResult result;
        std::string prompt = "Scene requested by the game master:\n\"" + sceneDescription + "\"\n\n";
        if (!styleId.empty())
            prompt += "Use the visual style `" + styleId + "`.\n";
        else
            prompt += "Choose the most fitting `style` from this library:\n" + styleCatalogue + "\n";
        prompt += "Use size `" + size + "` unless the scene clearly needs another.\n";
        prompt += "Return the design spec as JSON now.";

        nlohmann::json payload;
        payload["model"] = cfg.model;
        payload["prompt"] = prompt;
        payload["system"] = SystemPrompt();
        payload["stream"] = false;
        payload["format"] = SpecSchema(styleIds);
        // Time spent here is nothing beside the render that follows, so the
        // model is given room to think and room to answer: a plan that got
        // truncated is a plan somebody has to do again.
        payload["think"] = true;
        payload["options"] = {{"temperature", cfg.temperature},
                              {"num_predict", 4096},
                              {"num_ctx", 16384}};

        HttpResponse resp = WinHttpClient::PostJson(cfg.base_url + "/api/generate",
                                                    payload.dump(), cfg.timeout_seconds);
        if (!resp.success && resp.statusCode == 400) {
            // Not every model accepts `think`; retry once without it.
            payload.erase("think");
            resp = WinHttpClient::PostJson(cfg.base_url + "/api/generate", payload.dump(),
                                           cfg.timeout_seconds);
        }
        if (!resp.success) {
            result.error = resp.errorMessage.empty()
                               ? ("Ollama returned HTTP " + std::to_string(resp.statusCode) +
                                  ": " + resp.body)
                               : resp.errorMessage;
            return result;
        }

        std::string text;
        try {
            auto j = nlohmann::json::parse(resp.body);
            text = j.value("response", std::string(""));
            if (text.empty()) text = j.value("thinking", std::string(""));
        } catch (const std::exception& e) {
            result.error = std::string("could not parse Ollama reply: ") + e.what();
            return result;
        }
        result.raw = text;

        nlohmann::json specJson;
        if (!ExtractJson(text, specJson)) {
            // Some models put their working in the answer. Ask again plainly
            // rather than giving up on the plan.
            payload["think"] = false;
            HttpResponse retry = WinHttpClient::PostJson(cfg.base_url + "/api/generate",
                                                         payload.dump(),
                                                         cfg.timeout_seconds);
            if (retry.success) {
                try {
                    nlohmann::json j = nlohmann::json::parse(retry.body);
                    text = j.value("response", std::string(""));
                    result.raw = text;
                } catch (const std::exception&) {
                }
            }
            if (!ExtractJson(text, specJson)) {
                result.error = "the model did not return usable JSON";
                return result;
            }
        }
        result.spec = SpecFromJson(specJson, styleId, size);
        result.ok = true;
        return result;
    }

    static bool ExtractJson(const std::string& raw, nlohmann::json& out) {
        std::string cleaned = std::regex_replace(raw, std::regex("<think>[\\s\\S]*?</think>"), "");
        std::smatch m;
        if (std::regex_search(cleaned, m, std::regex("```(?:json)?\\s*([\\s\\S]*?)\\s*```")))
            cleaned = m[1].str();
        size_t first = cleaned.find('{'), last = cleaned.rfind('}');
        if (first == std::string::npos || last == std::string::npos || last <= first) return false;
        std::string snippet = cleaned.substr(first, last - first + 1);
        snippet = std::regex_replace(snippet, std::regex(",\\s*([}\\]])"), "$1");
        try {
            out = nlohmann::json::parse(snippet);
            return out.is_object();
        } catch (const std::exception&) {
            return false;
        }
    }

    static DesignSpec SpecFromJson(const nlohmann::json& j, const std::string& styleOverride,
                                   const std::string& sizeFallback) {
        DesignSpec spec;
        spec.title = j.value("title", std::string("Battle Map"));
        spec.name = SanitizeName(j.value("name", spec.title));
        spec.style = styleOverride.empty() ? j.value("style", std::string("")) : styleOverride;
        spec.layout = j.value("layout", std::string("dungeon"));
        spec.scene_summary = j.value("scene_summary", std::string(""));
        spec.render_details = j.value("render_details", std::string(""));
        spec.lighting = j.value("lighting", std::string(""));
        spec.prop_density = j.value("prop_density", std::string("medium"));

        std::string size = j.value("size", sizeFallback);
        for (const auto& p : arch::SizePresets())
            if (p.first == size) { spec.cols = p.second.first; spec.rows = p.second.second; }

        if (j.contains("terrain") && j["terrain"].is_object()) {
            spec.terrain_kind = j["terrain"].value("kind", std::string("none"));
            spec.terrain_amount = j["terrain"].value("amount", std::string("medium"));
            spec.terrain_shape = j["terrain"].value("shape", std::string("pools"));
        }
        if (j.contains("rooms") && j["rooms"].is_array()) {
            for (const auto& rj : j["rooms"]) {
                if (!rj.is_object()) continue;
                RoomSpec r;
                r.label = rj.value("label", std::string("Area"));
                r.id = SanitizeName(r.label);
                std::string s = rj.value("size", std::string("m"));
                r.size = s.empty() ? 'm' : (char)tolower((unsigned char)s[0]);
                r.terrain = rj.value("terrain", std::string("none"));
                if (rj.contains("props") && rj["props"].is_array())
                    for (const auto& p : rj["props"]) {
                        if (!p.is_string()) continue;
                        std::string k = arch::NormalizeProp(p.get<std::string>());
                        if (!k.empty()) r.props.push_back(k);
                    }
                if (rj.contains("enclosed") && rj["enclosed"].is_boolean())
                    r.enclosed = rj["enclosed"].get<bool>() ? 1 : 0;
                spec.rooms.push_back(r);
                if (spec.rooms.size() >= 9) break;
            }
        }
        // The playable field, so a direction can become a rectangle. The bleed
        // margin is added later and everything shifts with it.
        auto preset = arch::SizePresetFor(size);
        int fieldCols = preset.first, fieldRows = preset.second;
        if (j.contains("annotations") && j["annotations"].is_array()) {
            for (const auto& aj : j["annotations"]) {
                if (!aj.is_object()) continue;
                Annotation a;
                a.label = aj.value("label", std::string(""));
                if (a.label.empty()) continue;
                a.description = aj.value("description", std::string(""));
                a.elaboration = Elaboration::Exact;
                Rect r = arch::PlaceInField(aj.value("where", std::string("centre")),
                                            aj.value("size", std::string("m")),
                                            fieldCols, fieldRows);
                a.x = r.x; a.y = r.y; a.w = r.w; a.h = r.h;
                spec.annotations.push_back(a);
                if (spec.annotations.size() >= 14) break;
            }
        }
        if (j.contains("effects") && j["effects"].is_array()) {
            for (const auto& ej : j["effects"]) {
                if (!ej.is_object()) continue;
                Effect e;
                e.kind = arch::Lower(ej.value("kind", std::string("")));
                if (e.kind.empty()) continue;
                e.intensity = ej.value("strength", std::string("medium"));
                Rect r = arch::PlaceInField(ej.value("where", std::string("centre")),
                                            ej.value("size", std::string("m")),
                                            fieldCols, fieldRows);
                e.x = r.x; e.y = r.y; e.w = r.w; e.h = r.h;
                spec.effects.push_back(e);
                if (spec.effects.size() >= 6) break;
            }
        }
        if (j.contains("terrain_zones") && j["terrain_zones"].is_array()) {
            for (const auto& zj : j["terrain_zones"]) {
                if (!zj.is_object()) continue;
                TerrainZone z;
                z.kind = arch::Lower(zj.value("kind", std::string("")));
                if (z.kind.empty()) continue;
                Rect r = arch::PlaceInField(zj.value("where", std::string("centre")),
                                            zj.value("size", std::string("m")),
                                            fieldCols, fieldRows);
                z.x = r.x; z.y = r.y; z.w = r.w; z.h = r.h;
                spec.terrain_zones.push_back(z);
                if (spec.terrain_zones.size() >= 8) break;
            }
        }

        // Filled in later, once the style is attached: what closes the site in
        // decides it, and "custom" - which is what a planner almost always
        // answers with - tells you nothing on its own.
        spec.edge_walls = true;
        return spec;
    }

    static std::string SanitizeName(const std::string& raw) {
        std::string out;
        for (char c : arch::Lower(raw)) {
            if (isalnum((unsigned char)c)) out.push_back(c);
            else if (!out.empty() && out.back() != '_') out.push_back('_');
            if (out.size() >= 40) break;
        }
        while (!out.empty() && out.back() == '_') out.pop_back();
        return out.empty() ? "battlemap" : out;
    }
};

}  // namespace dnd
