#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>
#include <algorithm>

namespace dnd {

// -- tile vocabulary --------------------------------------------------
enum class Tile : uint8_t {
    Void = 0, Floor, Wall, Door, Window, Water, Pit, Rubble, Vegetation, Stairs, Bridge, COUNT
};

inline const char* TileName(Tile t) {
    switch (t) {
    case Tile::Void: return "void";
    case Tile::Floor: return "floor";
    case Tile::Wall: return "wall";
    case Tile::Door: return "door";
    case Tile::Window: return "window";
    case Tile::Water: return "water";
    case Tile::Pit: return "pit";
    case Tile::Rubble: return "rubble";
    case Tile::Vegetation: return "vegetation";
    case Tile::Stairs: return "stairs";
    case Tile::Bridge: return "bridge";
    default: return "floor";
    }
}

inline Tile TileFromName(const std::string& raw) {
    std::string s;
    s.reserve(raw.size());
    for (char c : raw) s.push_back((char)tolower((unsigned char)c));
    if (s == "void") return Tile::Void;
    if (s == "wall") return Tile::Wall;
    if (s == "door") return Tile::Door;
    if (s == "window") return Tile::Window;
    if (s == "water") return Tile::Water;
    if (s == "pit") return Tile::Pit;
    if (s == "rubble") return Tile::Rubble;
    if (s == "vegetation") return Tile::Vegetation;
    if (s == "stairs") return Tile::Stairs;
    if (s == "bridge") return Tile::Bridge;
    return Tile::Floor;  // covers floor plus legacy aliases (room, corridor, ...)
}

inline bool IsWalkable(Tile t) {
    return t == Tile::Floor || t == Tile::Bridge || t == Tile::Stairs ||
           t == Tile::Door || t == Tile::Rubble || t == Tile::Vegetation;
}

// Painter order: later kinds overwrite earlier ones when rasterizing zones.
inline const std::vector<Tile>& PaintOrder() {
    static const std::vector<Tile> order = {
        Tile::Void, Tile::Floor, Tile::Bridge, Tile::Stairs, Tile::Water, Tile::Pit,
        Tile::Rubble, Tile::Vegetation, Tile::Wall, Tile::Door, Tile::Window};
    return order;
}

// -- map document -----------------------------------------------------
struct MetaConfig {
    std::string name = "battlemap";
    std::string title = "Battle Map";
    std::string style = "gothic_crypt";
    std::string layout = "dungeon";
    std::string scene_summary;
    std::string render_details;
    std::string lighting;          // the scene's own, overriding the style's
    // Kept so opening a plan restores the settings it was built with.
    std::string terrain_kind = "none";
    std::string terrain_amount = "medium";
    std::string prop_density = "high";
    // Width of the blank ring around the playable field, in cells. Stored so a
    // map that is reopened knows which squares belong to the user and which are
    // margin. Zero on older files, which is exactly right for them.
    int border = 0;
    int64_t seed = 0;
};

struct GridConfig {
    int cols = 25;
    int rows = 19;
    int cell_px = 32;
};

struct Zone {
    std::string id;
    std::string kind = "floor";
    int x = 0, y = 0, w = 1, h = 1;
};

// How much freedom the renderer gets with a custom prop's description.
enum class Elaboration : uint8_t { Exact = 0, Some = 1, Free = 2 };

inline const char* ElaborationName(Elaboration e) {
    switch (e) {
    case Elaboration::Exact: return "exact";
    case Elaboration::Free: return "free";
    default: return "some";
    }
}

inline Elaboration ElaborationFromName(const std::string& s) {
    if (s == "exact") return Elaboration::Exact;
    if (s == "free") return Elaboration::Free;
    return Elaboration::Some;
}

struct Feature {
    std::string kind = "pillar";
    int x = 0, y = 0;
    // True when the architect sprinkled this in to fill a floor rather than
    // anybody asking for it. Only filler is ever folded into a summary.
    bool filler = false;
    // Set only for hand-placed custom props: the renderer is told exactly what
    // this thing is, in the user's own words.
    std::string label;
    std::string description;
    Elaboration elaboration = Elaboration::Some;
    // Load-bearing props are pinned by the control image / caption; decorative
    // clutter is only described, which the renderer paints far better than a
    // forced blob at an exact cell.
    bool structural = true;
};

// An object too fine for the tile grid to express - a ship hull needs a bow,
// and six cells of height has no room for one. Drawn as a vector shape.
struct Structure {
    std::string kind = "ship";
    int x = 0, y = 0, w = 1, h = 1;
    std::string facing = "e";
};

// A hand-written note pinned to a rectangle. Outranks everything else in the
// caption: the user placed it deliberately, so it must land exactly there.
struct Annotation {
    std::string label;
    std::string description;
    Elaboration elaboration = Elaboration::Some;
    int x = 0, y = 0, w = 1, h = 1;
};

// An atmospheric overlay. Effects sit on a layer above everything else and
// never change what a square is made of or whether you can walk through it.
struct Effect {
    std::string kind = "fog";
    std::string label;          // set for custom effects
    std::string description;
    Elaboration elaboration = Elaboration::Some;
    std::string intensity = "medium";   // low | medium | high
    int x = 0, y = 0, w = 1, h = 1;
};

// A named part of the map. Each becomes an element in the caption with its own
// rectangle, so the name and the description are what the renderer is told
// about that room - not decoration for the editor.
struct Area {
    std::string id;
    std::string label;
    std::string description;
    int x = 0, y = 0, w = 1, h = 1;
};

struct MapData {
    MetaConfig meta;
    GridConfig grid;
    std::vector<Zone> zones;
    std::vector<Feature> features;
    std::vector<Area> areas;
    std::vector<Structure> structures;
    std::vector<Annotation> annotations;
    std::vector<Effect> effects;

    void Clear() {
        zones.clear();
        features.clear();
        areas.clear();
        structures.clear();
        annotations.clear();
        effects.clear();
    }
};

// -- tile grid --------------------------------------------------------
struct TileGrid {
    int cols = 0, rows = 0;
    std::vector<Tile> cells;

    TileGrid() = default;
    TileGrid(int c, int r, Tile fill = Tile::Void)
        : cols(c), rows(r), cells((size_t)std::max(0, c * r), fill) {}

    bool Inside(int x, int y) const { return x >= 0 && y >= 0 && x < cols && y < rows; }

    Tile Get(int x, int y, Tile fallback = Tile::Void) const {
        if (!Inside(x, y)) return fallback;
        return cells[(size_t)y * cols + x];
    }

    void Set(int x, int y, Tile t) {
        if (Inside(x, y)) cells[(size_t)y * cols + x] = t;
    }

    void FillRect(int x, int y, int w, int h, Tile t) {
        for (int yy = y; yy < y + h; ++yy)
            for (int xx = x; xx < x + w; ++xx) Set(xx, yy, t);
    }

    int Count(Tile t) const {
        int n = 0;
        for (Tile c : cells) if (c == t) ++n;
        return n;
    }
};

// -- design spec (what the LLM or the user describes) -----------------
struct RoomSpec {
    std::string id;
    std::string label;
    // What this room is, in the planner's own words. It travels to the area and
    // from there into the caption: the second half of naming a room.
    std::string description;
    char size = 'm';                 // s / m / l
    std::string terrain = "none";
    std::vector<std::string> props;
    bool hasRect = false;
    int x = 0, y = 0, w = 0, h = 0;  // only for the "custom" layout
    // -1 unset, 0 outdoors, 1 walled. Unset means the name decides: see
    // RoomIsBuilt. Only matters on an open-air site, where a street is not a
    // room and should not be walled like one. Last, so the positional
    // initialisers the generators use are unaffected.
    int enclosed = -1;
};

// Ground the caller placed itself. Scattering is fine for atmosphere, but a
// description that says "a river runs down the middle" is giving a rectangle
// and should be able to say so.
struct TerrainZone {
    std::string kind = "rubble";
    int x = 0, y = 0, w = 1, h = 1;
};

struct DesignSpec {
    std::string name = "battlemap";
    std::string title = "Battle Map";
    std::string style;
    std::string layout = "dungeon";
    std::string scene_summary;
    std::string render_details;
    std::string lighting;
    int cols = 25, rows = 19;
    std::string terrain_kind = "none";
    std::string terrain_amount = "medium";
    std::string terrain_shape = "pools";
    std::string prop_density = "medium";
    std::vector<RoomSpec> rooms;
    std::vector<std::string> style_props;
    // Filled in from the chosen style so the architect can tell a walled house
    // from a gorge: see EnclosureOf.
    std::string style_category;
    std::string style_enclosure;
    std::vector<TerrainZone> terrain_zones;
    std::vector<Annotation> annotations;
    std::vector<Effect> effects;
    bool edge_walls = true;
    float water_fraction = 0.32f;
    // Width of the blank ring added around the finished map, in cells.
    int border = 2;
};

// -- styles -----------------------------------------------------------
struct StyleDef {
    std::string id;
    std::string name;
    std::string category = "General";
    std::string description;
    std::string materials;                  // what this place is made of
    std::string palette;
    std::string lighting;
    std::string aesthetics;                 // Ideogram caption: style_description
    std::string ground;                     // Ideogram caption: background surface
    std::vector<std::string> hex_palette;   // Ideogram caption: committed colours
    std::string default_layout = "dungeon";
    std::string enclosure;                  // masonry | rock | timber | open
    std::string wall;                       // overrides for what the edge is
    std::string face;
    std::string boundary;
    std::vector<std::string> props;
    std::vector<std::string> tags;
};

// Fragments merged into every caption. `forbidden_suffix` is the only thing
// keeping text and creatures out: Ideogram takes no negative prompt.
struct BaseStyle {
    std::string description;
    std::string aesthetics;
    std::string medium;
    std::string lighting;
    std::string forbidden_suffix;
    std::string background_suffix;
    std::string border_note;
    std::string viewpoint_note;   // orthographic, roof off, no perspective
    std::vector<std::string> default_palette;
};

// What the renderer is told about each kind of thing. Lives in
// styles/_phrases.json so it can be edited without touching code; the caption
// builder carries the same text only as a fallback.
struct Phrasebook {
    // section -> key -> phrase, kept generic so the editor can walk it.
    std::map<std::string, std::map<std::string, std::string>> sections;

    const std::string& Get(const std::string& section, const std::string& key,
                           const std::string& fallback) const {
        auto s = sections.find(section);
        if (s != sections.end()) {
            auto k = s->second.find(key);
            if (k != s->second.end() && !k->second.empty()) return k->second;
        }
        static std::string tmp;
        tmp = fallback;
        return tmp;
    }
};

// -- service configuration -------------------------------------------
struct OllamaConfig {
    std::string base_url = "http://127.0.0.1:11434";
    std::string model = "qwen3.8:27b";
    float temperature = 0.6f;
    int timeout_seconds = 600;
};

struct ComfyConfig {
    std::string base_url = "http://127.0.0.1:8188";

    // Ideogram 4. The layout travels as bounding boxes inside the JSON caption,
    // so there is no control image and no ControlNet anywhere in the pipeline.
    std::string unet = "ideogram4_fp8_scaled.safetensors";
    std::string unet_uncond = "ideogram4_unconditional_fp8_scaled.safetensors";
    std::string clip = "qwen3vl_8b_fp8_scaled.safetensors";
    std::string vae = "flux2-vae.safetensors";
    std::string preset = "Quality";      // kept for older config files
    int steps = 48;                      // 4..64; the presets are three points on it
    float cfg = 7.0f;
    float cfg_late = 3.0f;               // Ideogram drops CFG for the last stretch
    float cfg_late_start = 0.7f;
    float megapixels = 1.8f;
    int64_t seed = -1;                   // -1 = random each run
};

struct AppConfig {
    OllamaConfig ollama;
    ComfyConfig comfy;
    std::string default_style = "city_harbour";
    std::string default_size = "medium";
    std::string output_dir = "output";
    // Width of the blank bleed margin added around every new map, in cells.
    int border_cells = 2;
};

}  // namespace dnd
