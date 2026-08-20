#pragma once
// Build an Ideogram 4 structured JSON caption from map geometry - C++ port of
// ideogram_prompt.py.
//
// Ideogram 4 is trained exclusively on structured JSON captions, and its
// bounding boxes are a trained spatial interface rather than decorative text.
// The architect already knows the exact rectangle of every room, water body,
// ship and prop, so the layout is handed over as coordinates instead of being
// squeezed through a ControlNet - which is what kept flattening the render.
//
//   bbox = [y1, x1, y2, x2], normalised 0-1000 on BOTH axes, top-left origin.
#include "map_types.h"
#include "map_architect.h"

#include <nlohmann/json.hpp>

#include <map>
#include <numeric>
#include <string>
#include <vector>

namespace dnd {

class IdeogramCaption {
public:
    static constexpr int kMaxElements = 60;
    // Walls are merged into the longest runs the grid allows, so this is generous.
    static constexpr int kMaxWallRuns = 24;
    // Split a rectangle into tiles when it covers a big share of the map. A
    // single box the size of half the frame is not a useful instruction: the
    // model satisfies it with one blob somewhere inside.
    static std::vector<Rect> TileRect(int x, int y, int w, int h, int cols, int rows) {
        if (cols <= 0 || rows <= 0 || w * h <= 0.22 * cols * rows)
            return {{x, y, w, h}};
        int nx = (w >= cols * 0.6) ? 3 : 2;
        int ny = (h >= rows * 0.6) ? 3 : 2;
        while (nx * ny > 6) (nx >= ny ? nx : ny) -= 1;
        std::vector<Rect> out;
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                int tx = x + w * i / nx, ty = y + h * j / ny;
                int tw = x + w * (i + 1) / nx - tx, th = y + h * (j + 1) / ny - ty;
                if (tw > 0 && th > 0) out.push_back({tx, ty, tw, th});
            }
        }
        if (out.empty()) out.push_back({x, y, w, h});
        return out;
    }

    // Repeated on everything whose position is load-bearing.
    static constexpr const char* WINDOW_TEXT =
        "a window set into the wall: a stone or timber frame holding small panes of glass, "
        "its sill and lintel clearly drawn, filling the whole opening in the wall and set "
        "flush into it, with solid wall continuing on both sides.";

    static constexpr const char* kExact =
        "It sits exactly inside this rectangle and nowhere else";
    static constexpr const char* SHIP_TEXT =
        "One large wooden sailing ship floating on the water, viewed from directly above so "
        "only its weather deck is visible: a pointed bow at the right, a raised quarterdeck at "
        "the left, a continuous timber bulwark rail running round the hull, weathered oak deck "
        "planking running fore and aft, a square cargo hatch amidships, the round base of a "
        "single mast with coiled rigging, and mooring ropes running to the dock. No sails and "
        "no lower decks.";
    static constexpr const char* DOOR_TEXT =
        "A heavy closed timber door with iron banding and a ring handle, set into the wall and "
        "completely filling the opening as a solid closed door leaf, not an empty gap.";

    static nlohmann::json Build(const MapData& map, const StyleDef* style,
                                const BaseStyle& base) {
        TileGrid g = arch::ZonesToGrid(map);
        const int cols = g.cols, rows = g.rows;

        nlohmann::json cap;
        int div = std::gcd(cols, rows);
        if (div <= 0) div = 1;
        cap["aspect_ratio"] = std::to_string(cols / div) + ":" + std::to_string(rows / div);

        std::string title = map.meta.title.empty() ? "battle map" : map.meta.title;
        std::string head = "A hand-painted top-down fantasy tabletop RPG battle map of " +
                           title + ", seen from directly overhead";
        std::string summary = Trim(map.meta.scene_summary);
        if (!summary.empty()) {
            while (!summary.empty() && summary.back() == '.') summary.pop_back();
            summary[0] = (char)tolower((unsigned char)summary[0]);
            head += ", " + summary;
        }
        // Ideogram takes no negative prompt, so the ban on text and creatures
        // has to be stated positively, inside the caption.
        // Counted openings. Without this the renderer decorates a long wall with
        // a row of invented archways, which changes how the map plays.
        int doorCells = 0, windowCells = 0;
        for (const Rect& r : MergeRuns(g, Tile::Door, cols, rows)) doorCells += r.w * r.h;
        for (const Rect& r : MergeRuns(g, Tile::Window, cols, rows)) windowCells += r.w * r.h;
        cap["high_level_description"] =
            CapWords(head, 44) + ". " + base.forbidden_suffix +
            ". Every wall in this map is solid from end to end. There are exactly " +
            std::to_string(doorCells) + " doors and " + std::to_string(windowCells) +
            " windows in the whole picture, each one listed below with its own rectangle, "
            "and no other door, doorway, archway, gate, gap or window exists anywhere in "
            "any wall.";

        nlohmann::json sd;
        sd["aesthetics"] = (style && !style->aesthetics.empty()) ? style->aesthetics
                                                                 : base.aesthetics;
        sd["lighting"] = (style && !style->lighting.empty()) ? style->lighting : base.lighting;
        sd["medium"] = base.medium;
        nlohmann::json palette = nlohmann::json::array();
        const auto& colours = (style && !style->hex_palette.empty()) ? style->hex_palette
                                                                     : base.default_palette;
        for (const auto& hex : colours) palette.push_back(hex);
        sd["color_palette"] = palette;
        cap["style_description"] = sd;

        // Elements are gathered by priority, because the cap must never drop
        // the things whose position actually matters.
        // Four tiers. `structure` is what the map is: walls, doors, windows, the
        // ship. It outranks rooms and scenery, because a battle map with the
        // wrong walls is the wrong map however well it is painted.
        nlohmann::json critical = nlohmann::json::array();
        // Walls come before the things set into them: the model reads the list
        // in order, and an opening only makes sense once its run exists.
        nlohmann::json walls = nlohmann::json::array();
        nlohmann::json structure = nlohmann::json::array();
        nlohmann::json normal = nlohmann::json::array();
        nlohmann::json filler = nlohmann::json::array();

        // 1. Hand-written annotations win: the user placed them deliberately.
        for (const auto& a : map.annotations) {
            if (a.label.empty()) continue;
            std::string text = a.label;
            if (!a.description.empty()) {
                std::string d = a.description;
                while (!d.empty() && d.back() == '.') d.pop_back();
                text += ". " + d;
            }
            text += ". " + ElaborationPhrase(a.elaboration) + ". " + kExact;
            critical.push_back({{"type", "obj"},
                                {"bbox", Bbox(a.x, a.y, a.w, a.h, cols, rows)},
                                {"desc", text}});
        }

        // 2. Effects sit on top of everything, so they are described as overlays
        //    and must never be mistaken for a change of ground material.
        for (const auto& eff : map.effects) {
            std::string text;
            if (!eff.label.empty()) {
                text = eff.label;
                if (!eff.description.empty()) {
                    std::string d = eff.description;
                    while (!d.empty() && d.back() == '.') d.pop_back();
                    text += ". " + d;
                }
                text += ". " + ElaborationPhrase(eff.elaboration);
            } else {
                text = EffectText(eff.kind);
                if (text.empty()) continue;
                text[0] = (char)toupper((unsigned char)text[0]);
            }
            std::string how = eff.intensity == "low" ? "faint and thin"
                            : (eff.intensity == "high" ? "thick and dominating the area"
                                                       : "clearly visible");
            std::string body = text + ". This is an atmospheric effect painted over the "
                               "scene, " + how + ", lying on top of the ground without "
                               "replacing it";
            // One huge box makes the model paint the effect in a single corner
            // and call it done. Tiling a large area forces real coverage,
            // because every tile has to be filled on its own.
            for (const Rect& t : TileRect(eff.x, eff.y, eff.w, eff.h, cols, rows)) {
                std::string spread =
                    (t.w == eff.w && t.h == eff.h)
                        ? ""
                        : ", and this patch of it is one part of a single continuous effect "
                          "that covers the whole marked region";
                critical.push_back({
                    {"type", "obj"},
                    {"bbox", Bbox(t.x, t.y, t.w, t.h, cols, rows)},
                    {"desc", body + spread + ". It fills this whole rectangle. " + kExact}});
            }
        }

        // 3. Structures.
        for (const auto& st : map.structures) {
            if (st.kind != "ship") continue;
            structure.push_back({
                {"type", "obj"},
                {"bbox", Bbox(st.x, st.y, st.w, st.h, cols, rows)},
                {"desc", SHIP_TEXT}});
        }

        // 4. Doors. Few in number and load-bearing for how the map plays, so each
        //    gets its own box - without this the renderer put openings where it
        //    liked, or left a plain gap where a door belongs.
        for (const Rect& r : MergeRuns(g, Tile::Door, cols, rows)) {
            structure.push_back({
                {"type", "obj"},
                {"bbox", Bbox(r.x, r.y, r.w, r.h, cols, rows)},
                {"desc", std::string(DOOR_TEXT) + " " + kExact}});
        }

        // 4b. Windows. Like doors: few, load-bearing, and invented anywhere the
        //     renderer likes unless it is told exactly where they belong.
        for (const Rect& r : MergeRuns(g, Tile::Window, cols, rows)) {
            structure.push_back({
                {"type", "obj"},
                {"bbox", Bbox(r.x, r.y, r.w, r.h, cols, rows)},
                {"desc", std::string(WINDOW_TEXT) + " " + kExact}});
        }

        // 5. Terrain bodies worth naming.
        // 4c. Walls. Until now the renderer was handed room rectangles and left
        //     to invent every wall line itself, which is exactly where the
        //     layout came apart. The architect knows each run, so each is given.
        {
            // Caves and woodland have no straight walls to speak of; calling
            // their rock a "straight unbroken run" would be a lie the renderer
            // would then try to obey.
            bool organic = map.meta.layout == "cavern" || map.meta.layout == "forest" ||
                           map.meta.layout == "swamp";
            int emitted = 0;
            for (const Rect& r : MergeRuns(g, Tile::Wall, cols, rows)) {
                if (emitted >= kMaxWallRuns) break;
                // Only genuinely elongated runs. The rest are stubs, or one lump
                // of an irregular mass, and read as noise either way.
                if (std::max(r.w, r.h) < 3 || r.w * r.h < 2) continue;
                if (organic) {
                    walls.push_back({
                        {"type", "obj"},
                        {"bbox", Bbox(r.x, r.y, r.w, r.h, cols, rows)},
                        {"desc", std::string("A mass of solid rough rock wall filling this "
                                 "whole rectangle solidly, its face irregular and broken but "
                                 "with no passage, gap or opening through it anywhere. ") +
                                 kExact}});
                    ++emitted;
                    continue;
                }
                std::string shape =
                    (r.w >= r.h)
                        ? "a horizontal wall running the full width of this rectangle from "
                          "its left edge to its right edge"
                        : "a vertical wall running the full height of this rectangle from "
                          "its top edge to its bottom edge";
                walls.push_back({
                    {"type", "obj"},
                    {"bbox", Bbox(r.x, r.y, r.w, r.h, cols, rows)},
                    {"desc", "A straight unbroken run of solid stone wall with visible "
                             "courses: " + shape + ", filling it completely and keeping the "
                             "same thickness along its entire length, with square ends and "
                             "no gap, opening or archway anywhere in it. " + kExact}});
                ++emitted;
            }
        }

        AddTerrain(normal, g, Tile::Water, "dark green water", cols, rows);
        AddTerrain(normal, g, Tile::Pit, "an open pit dropping into darkness", cols, rows);
        AddTerrain(normal, g, Tile::Vegetation, "dense undergrowth", cols, rows);

        // 6. Rooms.
        for (const auto& a : map.areas) {
            std::string label = Trim(a.label);
            std::string lower = arch::Lower(label);
            if (label.empty() || lower == "moored ship" || lower == "quay") continue;
            normal.push_back({
                {"type", "obj"},
                {"bbox", Bbox(a.x, a.y, a.w, a.h, cols, rows)},
                {"desc", "The " + label + ": the roofless interior of a room seen from "
                         "directly above, its floor and furniture fully visible and filling "
                         "this rectangle, with no roof, no ceiling and nothing overhanging "
                         "it"}});
        }

        // 7. Pinned props get a box; clutter is only described.
        std::map<std::string, int> loose;
        for (const auto& f : map.features) {
            if (!f.structural) {
                std::string pretty = f.kind;
                std::replace(pretty.begin(), pretty.end(), '_', ' ');
                loose[pretty]++;
                continue;
            }
            if (!f.label.empty()) {
                std::string text = f.label;
                if (!f.description.empty()) {
                    std::string d = f.description;
                    while (!d.empty() && d.back() == '.') d.pop_back();
                    text += ". " + d;
                }
                text += ". " + ElaborationPhrase(f.elaboration) + ". " + kExact;
                critical.push_back({{"type", "obj"},
                                    {"bbox", Bbox(f.x, f.y, 1, 1, cols, rows)},
                                    {"desc", text}});
                continue;
            }
            std::string phrase = PropPhrase(f.kind);
            if (phrase.empty()) continue;
            filler.push_back({{"type", "obj"},
                              {"bbox", Bbox(f.x, f.y, 1, 1, cols, rows)},
                              {"desc", phrase + ", seen from directly above"}});
        }

        nlohmann::json elements = nlohmann::json::array();
        for (const auto& e : critical) elements.push_back(e);
        for (const auto& e : walls) elements.push_back(e);
        for (const auto& e : structure) elements.push_back(e);
        for (const auto& e : normal) elements.push_back(e);
        for (const auto& e : filler) elements.push_back(e);
        while ((int)elements.size() > kMaxElements) elements.erase(elements.end() - 1);

        if (!loose.empty()) {
            std::vector<std::pair<std::string, int>> sorted(loose.begin(), loose.end());
            std::sort(sorted.begin(), sorted.end(),
                      [](const auto& a, const auto& b) { return a.second > b.second; });
            std::string listed;
            for (size_t i = 0; i < sorted.size() && i < 6; ++i) {
                if (i) listed += ", ";
                listed += std::to_string(sorted[i].second) + " " + sorted[i].first;
                if (sorted[i].second > 1 && sorted[i].first.back() != 's') listed += "s";
            }
            elements.push_back({
                {"type", "obj"},
                {"desc", "Scattered clutter across the walkable ground: " + listed +
                         ", arranged against walls and in corners, casting soft shadows"}});
        }

        std::string ground = (style && !style->ground.empty()) ? style->ground
                                                               : "worn stone paving";
        // The style's material description is the richest theme detail we have.
        if (style && !style->materials.empty()) {
            std::string mats = style->materials;
            while (!mats.empty() && mats.back() == '.') mats.pop_back();
            ground += ". " + mats;
        }
        std::string background = ground + " " + base.background_suffix;

        // The blank ring around the playable field. Content boxes are already
        // inset by it, but the model still has to be told the margin is meant
        // to stay empty, or it fills the space with invented scenery.
        int border = arch::BorderOf(map);
        if (border > 0 && cols > 0 && rows > 0) {
            int pctX = std::max(1, (int)std::lround(border * 100.0 / cols));
            int pctY = std::max(1, (int)std::lround(border * 100.0 / rows));
            background += ". " + base.border_note + ". The margin is " +
                          std::to_string(pctX) + " percent of the image width down each side "
                          "and " + std::to_string(pctY) +
                          " percent of its height along the top and bottom";
        }

        cap["compositional_deconstruction"] = {
            {"background", background},
            {"elements", elements}};
        return cap;
    }

    // Minified single line, exactly as the model expects.
    static std::string BuildJson(const MapData& map, const StyleDef* style,
                                 const BaseStyle& base) {
        return Build(map, style, base).dump(-1, ' ', false,
                                      nlohmann::json::error_handler_t::replace);
    }

private:
    static std::string EffectText(const std::string& kind) {
        static const std::map<std::string, std::string> m = {
            {"fire", "leaping orange flames with a bright hot core, throwing firelight across "
                     "the surrounding ground"},
            {"embers", "a bed of glowing orange embers pulsing with heat, drifting sparks "
                       "rising from it"},
            {"smoke", "thick grey smoke curling upward, the ground dimly visible through it"},
            {"fog", "a low bank of pale drifting fog, thinning at its edges, the ground still "
                    "readable beneath it"},
            {"mist", "thin silver mist clinging low to the ground"},
            {"fireflies", "a scatter of tiny warm yellow-green points of light hanging in the air"},
            {"magic_glow", "a soft violet arcane glow washing over the ground, brightest at "
                           "its centre"},
            {"holy_light", "a shaft of pale golden light falling from above onto the ground"},
            {"poison_gas", "a sickly yellow-green vapour lying heavy and low"},
            {"blood", "dark red blood pooled and smeared across the ground"},
            {"ice", "a sheet of pale blue ice with white frost feathering out from its edge"},
            {"webs", "sheets of dusty grey spider web strung across the space"},
            {"sparks", "bright white sparks arcing and scattering"},
            {"ash", "grey ash settled in drifts, more of it still falling"},
            {"steam", "white steam venting upward in soft billows"},
            {"shadow", "an unnatural pool of deep shadow that swallows the light"},
        };
        auto it = m.find(kind);
        return it != m.end() ? it->second : std::string();
    }

    static std::string ElaborationPhrase(Elaboration e) {
        switch (e) {
        case Elaboration::Exact:
            return "Render this exactly as described and add nothing to it";
        case Elaboration::Free:
            return "Take this as a starting point and elaborate it richly with fitting detail";
        default:
            return "Render this as described, filling in fitting detail";
        }
    }

    static std::string Trim(std::string s) {
        while (!s.empty() && isspace((unsigned char)s.front())) s.erase(s.begin());
        while (!s.empty() && isspace((unsigned char)s.back())) s.pop_back();
        return s;
    }

    static std::string CapWords(const std::string& text, size_t limit) {
        std::vector<std::string> words;
        std::string cur;
        for (char c : text) {
            if (c == ' ') { if (!cur.empty()) { words.push_back(cur); cur.clear(); } }
            else cur.push_back(c);
        }
        if (!cur.empty()) words.push_back(cur);
        if (words.size() <= limit) {
            std::string out = text;
            while (!out.empty() && (out.back() == ',' || out.back() == '.')) out.pop_back();
            return out;
        }
        std::string out;
        for (size_t i = 0; i < limit; ++i) out += (i ? " " : "") + words[i];
        while (!out.empty() && (out.back() == ',' || out.back() == '.')) out.pop_back();
        return out;
    }

    static nlohmann::json Bbox(int x, int y, int w, int h, int cols, int rows) {
        auto clamp = [](long v) { return (int)std::max(0L, std::min(1000L, v)); };
        return nlohmann::json::array({
            clamp(std::lround(y * 1000.0 / rows)),
            clamp(std::lround(x * 1000.0 / cols)),
            clamp(std::lround((y + h) * 1000.0 / rows)),
            clamp(std::lround((x + w) * 1000.0 / cols))});
    }

    // Greedy rectangles for one tile kind, largest first.
    static std::vector<Rect> MergeRuns(const TileGrid& g, Tile kind, int cols, int rows) {
        std::vector<char> claimed((size_t)cols * rows, 0);
        std::vector<Rect> rects;
        for (int y = 0; y < rows; ++y) {
            for (int x = 0; x < cols; ++x) {
                if (claimed[(size_t)y * cols + x] || g.Get(x, y) != kind) continue;
                int w = 0;
                while (x + w < cols && !claimed[(size_t)y * cols + x + w] &&
                       g.Get(x + w, y) == kind) ++w;
                int h = 1;
                while (y + h < rows) {
                    bool ok = true;
                    for (int i = 0; i < w && ok; ++i)
                        ok = !claimed[(size_t)(y + h) * cols + x + i] &&
                             g.Get(x + i, y + h) == kind;
                    if (!ok) break;
                    ++h;
                }
                for (int yy = y; yy < y + h; ++yy)
                    for (int xx = x; xx < x + w; ++xx) claimed[(size_t)yy * cols + xx] = 1;
                rects.push_back({x, y, w, h});
            }
        }
        std::sort(rects.begin(), rects.end(),
                  [](const Rect& a, const Rect& b) { return a.Area() > b.Area(); });
        return rects;
    }

    static void AddTerrain(nlohmann::json& elements, const TileGrid& g, Tile kind,
                           const std::string& phrase, int cols, int rows) {
        std::vector<Rect> rects = MergeRuns(g, kind, cols, rows);
        int minArea = std::max(4, (int)(cols * rows * 0.02));
        for (size_t i = 0; i < rects.size() && i < 2; ++i) {
            if (rects[i].Area() < minArea) break;
            elements.push_back({
                {"type", "obj"},
                {"bbox", Bbox(rects[i].x, rects[i].y, rects[i].w, rects[i].h, cols, rows)},
                {"desc", "A body of " + phrase + " filling this region, its edge meeting "
                         "the surrounding ground in a clean line. " + kExact}});
        }
    }

    static std::string PropPhrase(const std::string& kind) {
        static const std::map<std::string, std::string> m = {
            {"pillar", "a thick round stone pillar"},
            {"column", "a fluted stone column"},
            {"statue", "a weathered stone statue on a square plinth"},
            {"altar", "a carved stone altar block"},
            {"sarcophagus", "a heavy stone sarcophagus with a chipped lid"},
            {"coffin", "a plain timber coffin"},
            {"table", "a long timber table"},
            {"desk", "a writing desk covered in papers"},
            {"workbench", "a scarred wooden workbench"},
            {"bench", "a low timber bench"},
            {"bed", "a straw mattress on a timber frame"},
            {"throne", "a high-backed carved throne"},
            {"bookshelf", "a tall shelf packed with ledgers"},
            {"bar", "a polished timber bar counter"},
            {"anvil", "a black iron anvil on a stump"},
            {"forge", "a stone forge glowing with coals"},
            {"hearth", "a stone hearth with burning logs"},
            {"brazier", "an iron brazier holding live coals"},
            {"campfire", "a ring of stones around a burning campfire"},
            {"cauldron", "a black iron cauldron on a tripod"},
            {"well", "a round stone well with a timber winch"},
            {"fountain", "a carved stone fountain basin"},
            {"tree", "a broad tree seen from above, its canopy spreading wide"},
            {"boulder", "a moss-covered boulder"},
            {"stalagmite", "a jagged rock spire"},
            {"crystal", "a cluster of glowing crystal shards"},
            {"mast", "the base of a timber mast with coiled rigging"},
            {"capstan", "a timber capstan with protruding bars"},
            {"cart", "a two-wheeled timber handcart"},
            {"wagon", "a four-wheeled timber wagon"},
            {"dumpster", "a dented steel dumpster"},
            {"portal", "a standing stone archway"},
            {"gate", "a heavy iron-bound gate"},
            {"arch", "a carved stone arch"},
        };
        auto it = m.find(kind);
        return it != m.end() ? it->second : std::string();
    }
};

}  // namespace dnd
