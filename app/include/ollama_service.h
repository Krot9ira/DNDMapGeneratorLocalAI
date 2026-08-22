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
        payload["think"] = false;  // thinking preambles leak into the JSON
        payload["options"] = {{"temperature", cfg.temperature}, {"num_predict", 1400}};

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
            result.error = "the model did not return usable JSON";
            return result;
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
                spec.rooms.push_back(r);
                if (spec.rooms.size() >= 9) break;
            }
        }
        bool outdoor = spec.layout == "harbour" || spec.layout == "open" || spec.layout == "street";
        spec.edge_walls = !outdoor;
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
