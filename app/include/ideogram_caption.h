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
    static constexpr int kMaxElements = 24;      // a one-room scene
    static constexpr int kMaxElementsCeiling = 40;

    // A flat budget was measured on a map whose elements were mostly identical
    // wall runs: forty of those drown each other out. Now that every element
    // says something different, a six-room dungeon needs room for its six
    // rooms, their doors and their walls before anything is left over.
    static int ElementBudget(const MapData& map) {
        int rooms = 0;
        for (const Area& a : map.areas)
            if (!a.label.empty()) ++rooms;
        return std::max(kMaxElements, std::min(kMaxElementsCeiling, 16 + 3 * rooms));
    }
    // Walls are merged into the longest runs the grid allows, so this is generous.
    static constexpr int kMaxWallRuns = 8;

    // How many of one kind of *filler* object get their own rectangle before
    // the rest join the clutter sentence. Filler is what the architect
    // sprinkles in to fill a floor; props somebody asked for are never folded
    // away - a room with twelve chests in it is a plan, not noise.
    static constexpr int kMaxSameProp = 3;
    // 8-connected blobs of the given tile kinds, as bounding rectangles with
    // their cell counts, biggest first.
    struct Blob { Rect rect; int cells; std::vector<std::pair<int, int>> at; };
    static std::vector<Blob> Components(const TileGrid& g,
                                        const std::vector<Tile>& kinds) {
        auto wanted = [&](Tile t) {
            for (Tile k : kinds) if (k == t) return true;
            return false;
        };
        std::vector<char> seen((size_t)g.cols * g.rows, 0);
        std::vector<Blob> out;
        for (int sy = 0; sy < g.rows; ++sy) {
            for (int sx = 0; sx < g.cols; ++sx) {
                if (seen[(size_t)sy * g.cols + sx] || !wanted(g.Get(sx, sy))) continue;
                std::vector<std::pair<int, int>> stack{{sx, sy}};
                seen[(size_t)sy * g.cols + sx] = 1;
                int x0 = sx, x1 = sx, y0 = sy, y1 = sy, n = 0;
                std::vector<std::pair<int, int>> at;
                while (!stack.empty()) {
                    auto [x, y] = stack.back();
                    stack.pop_back();
                    ++n;
                    at.push_back({x, y});
                    x0 = std::min(x0, x); x1 = std::max(x1, x);
                    y0 = std::min(y0, y); y1 = std::max(y1, y);
                    for (int dy = -1; dy <= 1; ++dy) {
                        for (int dx = -1; dx <= 1; ++dx) {
                            int nx = x + dx, ny = y + dy;
                            if (nx < 0 || ny < 0 || nx >= g.cols || ny >= g.rows) continue;
                            if (seen[(size_t)ny * g.cols + nx] || !wanted(g.Get(nx, ny)))
                                continue;
                            seen[(size_t)ny * g.cols + nx] = 1;
                            stack.push_back({nx, ny});
                        }
                    }
                }
                out.push_back({{x0, y0, x1 - x0 + 1, y1 - y0 + 1}, n, std::move(at)});
            }
        }
        std::sort(out.begin(), out.end(), [](const Blob& a, const Blob& b) {
            return a.rect.w * a.rect.h > b.rect.w * b.rect.h;
        });
        return out;
    }

    // How much walkable ground a wall shuts in. A cliff running down one side
    // of a gorge is a wall component like any other, and it was being handed
    // over as "one single building standing alone inside this rectangle" -
    // which is how a river gorge came back as a walled compound seven renders
    // running. A wall is a building when you cannot get into what it surrounds
    // without going through it.
    static int EnclosesFloor(const TileGrid& g,
                             const std::vector<std::pair<int, int>>& wallCells) {
        std::set<std::pair<int, int>> wall(wallCells.begin(), wallCells.end());
        std::vector<char> seen((size_t)g.cols * g.rows, 0);
        std::vector<std::pair<int, int>> stack;
        auto seed = [&](int x, int y) {
            if (wall.count({x, y}) || seen[(size_t)y * g.cols + x]) return;
            seen[(size_t)y * g.cols + x] = 1;
            stack.push_back({x, y});
        };
        for (int x = 0; x < g.cols; ++x) { seed(x, 0); seed(x, g.rows - 1); }
        for (int y = 0; y < g.rows; ++y) { seed(0, y); seed(g.cols - 1, y); }
        while (!stack.empty()) {
            auto [x, y] = stack.back();
            stack.pop_back();
            const int d[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (auto& o : d) {
                int nx = x + o[0], ny = y + o[1];
                if (nx < 0 || ny < 0 || nx >= g.cols || ny >= g.rows) continue;
                if (seen[(size_t)ny * g.cols + nx] || wall.count({nx, ny})) continue;
                seen[(size_t)ny * g.cols + nx] = 1;
                stack.push_back({nx, ny});
            }
        }
        int shutIn = 0;
        for (int y = 0; y < g.rows; ++y)
            for (int x = 0; x < g.cols; ++x)
                if (!seen[(size_t)y * g.cols + x] && !wall.count({x, y}) &&
                    IsWalkable(g.Get(x, y))) ++shutIn;
        return shutIn;
    }

    // Pull the biggest solid rectangles out of a boolean mask. Greedy row
    // merging leaves long thin slivers, which is why most of the open ground
    // went undescribed; this finds real blocks.
    static std::vector<Rect> LargestRects(std::vector<char> mask, int cols, int rows,
                                          int count, int minArea) {
        std::vector<Rect> out;
        for (int pass = 0; pass < count; ++pass) {
            std::vector<int> heights((size_t)cols, 0);
            int bestArea = 0;
            Rect best{0, 0, 0, 0};
            for (int y = 0; y < rows; ++y) {
                for (int x = 0; x < cols; ++x)
                    heights[x] = mask[(size_t)y * cols + x] ? heights[x] + 1 : 0;
                std::vector<std::pair<int, int>> stack;   // start, height
                for (int x = 0; x <= cols; ++x) {
                    int h = (x < cols) ? heights[x] : 0;
                    int start = x;
                    while (!stack.empty() && stack.back().second >= h) {
                        auto [sx, sh] = stack.back();
                        stack.pop_back();
                        int area = sh * (x - sx);
                        if (area > bestArea) {
                            bestArea = area;
                            best = {sx, y - sh + 1, x - sx, sh};
                        }
                        start = sx;
                    }
                    stack.push_back({start, h});
                }
            }
            if (bestArea < minArea) break;
            out.push_back(best);
            for (int y = best.y; y < best.y + best.h; ++y)
                for (int x = best.x; x < best.x + best.w; ++x)
                    mask[(size_t)y * cols + x] = 0;
        }
        return out;
    }

    // Split a rectangle into tiles when it covers a big share of the map. A
    // single box the size of half the frame is not a useful instruction: the
    // model satisfies it with one blob somewhere inside.
    // Where a rectangle sits and how far it reaches, in words.
    static std::string SpreadOf(int x, int y, int w, int h, int cols, int rows) {
        bool wide = w >= 0.55 * cols;
        bool tall = h >= 0.55 * rows;
        double cx = (x + w / 2.0) / std::max(1, cols);
        double cy = (y + h / 2.0) / std::max(1, rows);
        std::string side = cx < 0.38 ? "west" : (cx > 0.62 ? "east" : "");
        std::string band = cy < 0.38 ? "north" : (cy > 0.62 ? "south" : "");
        if (tall && !wide)
            return side.empty() ? "down the middle from top to bottom" : "down the " + side + " side";
        if (wide && !tall)
            return band.empty() ? "across the middle from side to side" : "across the " + band + " end";
        if (wide && tall) return "over most of the map";
        std::string where = (!band.empty() && !side.empty()) ? band + "-" + side
                                                             : (band.empty() ? side : band);
        return where.empty() ? "in the middle" : "in the " + where;
    }

    // One sentence naming the biggest things and where they lie. The
    // description at the top of a caption is the most powerful text in it, and
    // until now it was whatever prose the scene came with - which for a gorge
    // said "a war camp with a fire in the middle", so a war camp with a fire in
    // the middle is what came back, four times over, however carefully every
    // rectangle below it was drawn. This puts the plan's own geography in that
    // same place, in the same voice, so the strongest text in the caption
    // agrees with the weakest.
    static std::string ShapeOfMap(const MapData& map, int cols, int rows) {
        struct Piece { int area; std::string label; Rect r; };
        std::vector<Piece> pieces;
        for (const Annotation& a : map.annotations)
            if (!Trim(a.label).empty())
                pieces.push_back({std::max(1, a.w) * std::max(1, a.h), Trim(a.label),
                                  {a.x, a.y, a.w, a.h}});
        for (const Area& a : map.areas)
            if (!Trim(a.label).empty())
                pieces.push_back({std::max(1, a.w) * std::max(1, a.h), Trim(a.label),
                                  {a.x, a.y, a.w, a.h}});
        std::stable_sort(pieces.begin(), pieces.end(),
                         [](const Piece& a, const Piece& b) { return a.area > b.area; });
        // Measured against the playable field, not the stored grid: every map
        // carries an empty bleed margin, and counting it made the one area that
        // fills the whole map look like two thirds of it.
        int b = arch::BorderOf(map);
        int field = std::max(1, (cols - 2 * b) * (rows - 2 * b));
        std::vector<std::string> said;
        std::set<std::string> seen;
        for (const Piece& piece : pieces) {
            if (said.size() >= 4 || piece.area < 0.02 * field) break;
            std::string low = arch::Lower(piece.label);
            if (piece.area > 0.7 * field || seen.count(low)) continue;
            std::string where = SpreadOf(piece.r.x, piece.r.y, piece.r.w, piece.r.h,
                                         cols, rows);
            // One place, one answer. Two landmarks both "across the north end"
            // is a sentence that has stopped saying anything about the shape of
            // the map.
            if (seen.count(where)) continue;
            seen.insert(low);
            seen.insert(where);
            said.push_back(LowerFirst(TheLabel(piece.label)) + " " + where);
        }
        if (said.size() < 2) return "";
        std::string out = "Laid out on the map, and in these places and no others: ";
        for (size_t i = 0; i < said.size(); ++i) {
            if (i) out += (i + 1 == said.size()) ? " and " : ", ";
            out += said[i];
        }
        return out + ".";
    }

    static std::vector<Rect> TileRect(int x, int y, int w, int h, int cols, int rows,
                                      int maxTiles = 6) {
        if (cols <= 0 || rows <= 0 || w * h <= 0.22 * cols * rows)
            return {{x, y, w, h}};
        int nx = (w >= cols * 0.6) ? 3 : 2;
        int ny = (h >= rows * 0.6) ? 3 : 2;
        while (nx * ny > maxTiles) (nx >= ny ? nx : ny) -= 1;
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
    // Seen from above, a door shows only the top face of its leaf. Asking for a
    // "door leaf with a ring handle" asks for a front view, which is what made
    // the intended doors come out tilted and arched.
    static constexpr const char* DOOR_TEXT =
        "A closed door filling this opening in the wall, seen from directly overhead so only "
        "the flat top face of the door leaf is visible: a plain rectangular timber panel with "
        "dark iron bands across it, lying flush inside the wall opening and squared up with "
        "the wall, exactly as wide as the wall is thick. It is drawn flat, straight on and "
        "level with everything around it, with no perspective, no tilt, no arch or vault "
        "above it, no door frame standing proud, no steps and no visible handle.";

    // Join a phrase to a following sentence with exactly one full stop, however
    // the phrase happens to end. The wording file is edited by hand, so half
    // its entries end with a stop and half do not.
    // Which wall of which building - so no two doors read the same. Six doors
    // described in identical words are six chances to read the map as a
    // repeating pattern, and the renderer obliges by mirroring it.
    static std::string WhichWall(const Rect& d, const std::vector<Rect>& buildings,
                                 const std::vector<std::string>& names, int cols, int rows) {
        const Rect* host = nullptr;
        std::string hostName;
        for (size_t i = 0; i < buildings.size(); ++i) {
            const Rect& b = buildings[i];
            if (d.x >= b.x - 1 && d.y >= b.y - 1 && d.x + d.w <= b.x + b.w + 1 &&
                d.y + d.h <= b.y + b.h + 1) {
                host = &b;
                if (i < names.size()) hostName = names[i];
                break;
            }
        }
        Rect r = host ? *host : Rect{0, 0, cols, rows};
        double dx = (d.x + d.w / 2.0 - (r.x + r.w / 2.0)) / std::max(1.0, r.w / 2.0);
        double dy = (d.y + d.h / 2.0 - (r.y + r.h / 2.0)) / std::max(1.0, r.h / 2.0);
        std::string side = std::abs(dx) > std::abs(dy) ? (dx > 0 ? "east" : "west")
                                                       : (dy > 0 ? "south" : "north");
        std::string where = "In the " + side + " wall";
        if (!hostName.empty() && hostName != "a building") return where + " of " + hostName;
        return where + (host ? " of this building" : " of the map");
    }

    // "The Crypt" already has its article; do not give it a second one.
    // A short phrase for where something stands, to tell two alike things apart.
    static std::string WhereOnMap(int x, int y, int cols, int rows) {
        double fx = (x + 0.5) / std::max(1, cols), fy = (y + 0.5) / std::max(1, rows);
        std::string band = fy < 0.34 ? "north" : (fy > 0.66 ? "south" : "");
        std::string side = fx < 0.34 ? "west" : (fx > 0.66 ? "east" : "");
        if (!band.empty() && !side.empty()) return "In the " + band + "-" + side + " of the map";
        if (!band.empty() || !side.empty()) return "In the " + band + side + " of the map";
        return "Near the middle of the map";
    }

    static std::string TheLabel(const std::string& label) {
        if (label.size() >= 4) {
            std::string head = label.substr(0, 4);
            for (char& c : head) c = (char)tolower((unsigned char)c);
            if (head == "the ") return label;
        }
        return "The " + label;
    }

    static std::string LowerFirst(std::string s) {
        if (!s.empty()) s[0] = (char)tolower((unsigned char)s[0]);
        return s;
    }

    static std::string UpperFirst(std::string s) {
        if (!s.empty()) s[0] = (char)toupper((unsigned char)s[0]);
        return s;
    }

    static std::string Sentence(std::string head, const std::string& tail) {
        while (!head.empty() && (head.back() == ' ' || head.back() == '.')) head.pop_back();
        if (head.empty()) return tail;
        return head + ". " + tail;
    }

    // Places where a style argues with the map it is painting, or asks for
    // something that can only be seen from the side. Mirrors
    // ideogram_prompt.style_warnings, so the app warns about a style somebody
    // writes here exactly as the tools do.
    static std::vector<std::string> StyleWarnings(const StyleDef* style) {
        std::vector<std::string> out;
        if (!style) return out;
        static const std::vector<std::string> sideOn = {
            "on the wall", "from the wall", "up the wall", "wall-mounted", "wall-hung",
            "mounted on", "hanging from", "hanging over", "hanging above", "hung on",
            "hung from", "hung above", "from the ceiling", "on the ceiling",
            "floor-to-ceiling", "vaulted", "stalactite", "chandelier", "rafter",
            "suspended", "its face", "their faces", "the face of", "facade", "frontage",
            "flank", "rising the whole", "rising on either", "rises above", "towering",
            "taller than a man", "taller than a person", "seen from the side", "in profile",
            "silhouette", "elevation", "to the roof", "to the ceiling", "floor to roof",
            "head height", "upper level", "upper storey", "tall window",
            "standing upright"};
        static const std::vector<std::string> placing = {
            "central ", "in the middle", "round the", "along the", "down the",
            "beyond the", "at the far", "on one side", "in the centre"};

        const std::pair<const char*, std::string> fields[] = {
            {"materials", style->materials}, {"ground", style->ground},
            {"lighting", style->lighting},   {"boundary", style->boundary},
            {"description", style->description}};

        std::string light = arch::Lower(style->lighting);
        for (const std::string& w : placing)
            if (light.find(w) != std::string::npos) {
                out.push_back("style '" + style->id + "' says '" + w + "' in its lighting. "
                              "Lighting says what the light is like, not where anything "
                              "stands: this one puts that thing on every map the style "
                              "touches, whatever the plan says.");
                break;
            }
        for (const std::string& w : sideOn) {
            bool said = false;
            for (const auto& f : fields) {
                if (arch::Lower(f.second).find(w) == std::string::npos) continue;
                out.push_back("style '" + style->id + "' says '" + w + "' in its " +
                              f.first + ". Nothing can be shown on a wall or overhead from "
                              "directly above, so the renderer draws the wall from the side "
                              "to fit it in and the map comes back tilted. Say where the "
                              "thing stands on the floor instead.");
                said = true;
                break;
            }
            if (said) break;
        }
        return out;
    }

    static nlohmann::json Build(const MapData& map, const StyleDef* style,
                                const BaseStyle& base, const Phrasebook& ph = {}) {
        // Every phrase below can be overridden in styles/_phrases.json; the
        // literals are the fallback for a missing file or a deleted key.
        auto say = [&ph](const char* section, const char* key, const char* fallback) {
            return ph.Get(section, key, fallback);
        };
        const std::string kExactS = say("phrasing", "exact", kExact);
        TileGrid g = arch::ZonesToGrid(map);
        const int cols = g.cols, rows = g.rows;

        // What closes this site in decides what its edge is called, whether the
        // map may be described as a building at all, and what a way in is.
        const std::string enclosure = arch::EnclosureOf(
            style ? style->enclosure : std::string(), style ? style->category : std::string(),
            map.meta.layout, style ? style->default_layout : std::string());
        std::string encWall = "solid stone wall with visible courses";
        std::string encFace = "plain masonry";
        std::string encBoundary = "a thick stone wall of plain masonry closing the site in "
                                  "on all four sides";
        if (enclosure == "rock") {
            encWall = "solid rough rock wall";
            encFace = "raw rock";
            encBoundary = "sheer natural rock closing the site in on all four sides, its "
                          "face broken and irregular, and nowhere a course of laid stone";
        } else if (enclosure == "timber") {
            encWall = "solid timber bulwark of close-fitted planking";
            encFace = "close-fitted planking";
            encBoundary = "a continuous timber bulwark of close-fitted planking running "
                          "right round the outside";
        } else if (enclosure == "open") {
            // Open air outside; what is built on it is still built. Only the
            // boundary is natural - a stone house on a city street does not
            // have walls of raw rock, which is what it was being told it had.
            encWall = "solid stone wall with visible courses";
            encFace = "plain masonry";
            encBoundary = "a continuous natural edge closing the site in on all four sides "
                          "- rising ground, rock, earth and dense growth, whatever the "
                          "surroundings are - every part of it natural and unbuilt, with no "
                          "course of laid stone, no brickwork and no doorway anywhere along "
                          "it";
        }
        encWall = ph.Get("enclosure", (enclosure + "_wall").c_str(), encWall.c_str());
        encFace = ph.Get("enclosure", (enclosure + "_face").c_str(), encFace.c_str());
        encBoundary = ph.Get("enclosure", (enclosure + "_boundary").c_str(),
                             encBoundary.c_str());
        if (style) {
            if (!Trim(style->wall).empty()) encWall = Trim(style->wall);
            if (!Trim(style->face).empty()) encFace = Trim(style->face);
            if (!Trim(style->boundary).empty()) encBoundary = Trim(style->boundary);
        }

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
        // A hall that fills the frame fights the renderer's prior that a big
        // roofless building has rooms in it. An element buried in the list is
        // too quiet to win that argument; it has to be said in the headline.
        std::string onlyRoom;
        {
            const Area* only = nullptr;
            int named = 0;
            for (const Area& a : map.areas)
                if (!a.label.empty()) { ++named; only = &a; }
            if (named == 1 && only &&
                (double)only->w * only->h / std::max(1, cols * rows) > 0.45) {
                onlyRoom = std::string(" The whole of this map is one single ") +
                       (enclosure == "masonry" ? "room" : "open space") + ", " +
                       LowerFirst(TheLabel(only->label)) +
                           ", and nothing else: " +
                           std::string(enclosure == "masonry"
                                           ? "one continuous floor from wall to wall with "
                                           : "one continuous open ground with ") +
                           "no interior wall, no partition, no corridor and no smaller room "
                           "anywhere inside it. Everything standing on it is furniture and "
                           "scenery, not architecture.";
            }
        }
        std::string shape = ShapeOfMap(map, cols, rows);
        if (!shape.empty()) shape = " " + shape;
        cap["high_level_description"] =
            CapWords(head, 44) + ". " + base.forbidden_suffix + "." + shape + onlyRoom +
            " Every wall in this picture is one continuous face of " + encFace + " from "
            "corner to corner, interrupted only by " + std::to_string(doorCells) +
            " doorways and " + std::to_string(windowCells) +
            " windows, each of which is listed below with its own rectangle. Everywhere "
            "else that face runs straight on.";
        // Nothing in the contract forbade perspective, so a scene that happened
        // to mention a ceiling came back drawn from the corner of the room.
        if (!base.viewpoint_note.empty())
            cap["high_level_description"] =
                cap["high_level_description"].get<std::string>() + " " +
                Trim(base.viewpoint_note) + ".";

        nlohmann::json sd;
        sd["aesthetics"] = (style && !style->aesthetics.empty()) ? style->aesthetics
                                                                 : base.aesthetics;
        // A scene that says how it is lit outranks the style's general idea of
        // how places like it are lit.
        sd["lighting"] = !Trim(map.meta.lighting).empty()
                             ? Trim(map.meta.lighting)
                             : ((style && !style->lighting.empty()) ? style->lighting
                                                                   : base.lighting);
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

        // The blank margin is asked for in the background text and painted onto
        // the finished image afterwards. A box saying "nothing is here" gives
        // the renderer nothing to place, so it never worked.
        const int border = arch::BorderOf(map);

        // 1. Hand-written annotations win: the user placed them deliberately.
        for (const auto& a : map.annotations) {
            if (a.label.empty()) continue;
            std::string text = a.label;
            if (!a.description.empty()) {
                std::string d = a.description;
                while (!d.empty() && d.back() == '.') d.pop_back();
                text += ". " + d;
            }
            text += ". " + ElaborationPhrase(a.elaboration) + ". " + kExactS;
            critical.push_back({{"type", "obj"},
                                {"bbox", Bbox(a.x, a.y, a.w, a.h, cols, rows)},
                                {"desc", text}});
        }

        // Effects covering the whole map; they go in the background.
        std::vector<std::string> atmosphere;
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
                text = say("effects", eff.kind.c_str(), EffectText(eff.kind).c_str());
                if (text.empty()) continue;
                text[0] = (char)toupper((unsigned char)text[0]);
            }
            std::string how = eff.intensity == "low" ? say("phrasing", "effect_low", "faint and thin")
                            : (eff.intensity == "high" ? say("phrasing", "effect_high", "thick and dominating the area")
                                       : say("phrasing", "effect_medium", "clearly visible"));
            std::string body = text + ". This is an atmospheric effect painted over the "
                               "scene, " + how + ", lying on top of the ground without "
                               "replacing it";
            // An effect lying over most of the map is atmosphere, not a
            // feature. Handed over as boxes it came back as six identical
            // elements, which is both a quarter of the budget and the
            // repeating pattern the rest of the caption spends its length
            // arguing against. It goes in the background instead, which is
            // where something covering everything belongs.
            if (eff.w * eff.h >= 0.25 * cols * rows) {
                atmosphere.push_back(text + ", " + how + ", lying over the whole map on top "
                                     "of everything else without replacing any of it");
                continue;
            }
            // One huge box makes the model paint the effect in a single corner
            // and call it done. Tiling a large area forces real coverage,
            // because every tile has to be filled on its own.
            for (const Rect& t : TileRect(eff.x, eff.y, eff.w, eff.h, cols, rows, 4)) {
                std::string spread =
                    (t.w == eff.w && t.h == eff.h)
                        ? ""
                        : ", and this patch of it is one part of a single continuous effect "
                          "that covers the whole marked region";
                critical.push_back({
                    {"type", "obj"},
                    {"bbox", Bbox(t.x, t.y, t.w, t.h, cols, rows)},
                    {"desc", body + spread + ". It fills this whole rectangle. " + kExactS}});
            }
        }

        // 3. Structures.
        for (const auto& st : map.structures) {
            if (st.kind != "ship") continue;
            structure.push_back({
                {"type", "obj"},
                {"bbox", Bbox(st.x, st.y, st.w, st.h, cols, rows)},
                {"desc", say("structure", "ship", SHIP_TEXT)}});
        }

        // 4b. Windows. Like doors: few, load-bearing, and invented anywhere the
        //     renderer likes unless it is told exactly where they belong.
        for (const Rect& r : MergeRuns(g, Tile::Window, cols, rows)) {
            structure.push_back({
                {"type", "obj"},
                {"bbox", Bbox(r.x, r.y, r.w, r.h, cols, rows)},
                {"desc", Sentence(say("structure", "window", WINDOW_TEXT), kExactS)}});
        }

        // 5. Terrain bodies worth naming.
        // 4b-2. Buildings as whole objects. A dozen wall runs that all read
        //       alike invite a symmetrical building of the renderer's own
        //       invention; three named footprints of different sizes do not.
        bool organic = enclosure == "rock" || map.meta.layout == "forest" ||
                       map.meta.layout == "swamp";
        if (organic && enclosure != "rock") {
            // Nothing in a cave or a wood is laid course by course.
            encWall = say("structure", "wall_organic", "solid rough rock wall");
            encFace = "raw rock";
        }
        std::vector<Rect> buildings;
        std::vector<std::string> buildingNames;
        // A building holding exactly one room is described once, at one
        // rectangle. Handed over as a shell and an interior inset inside it by
        // the thickness of the wall, it leaves a ring between the two that the
        // renderer fills with alcoves it invented - which is what lined the
        // walls of every single-room map with little booths.
        std::map<size_t, std::pair<std::string, std::string>> singleRoomShell;
        bool haveSiteEdge = false;
        bool siteSpokenFor = false;   // ...and whether the scene already described it
        Rect siteEdge{0, 0, 0, 0};
        {
            std::vector<Rect> hulls;
            for (const auto& st : map.structures)
                if (st.kind == "ship") hulls.push_back({st.x, st.y, st.w, st.h});
            std::vector<Blob> footprints = Components(g, {Tile::Wall, Tile::Door,
                                                          Tile::Window});
            // Put them in an order both ports agree on. Flood fill visits cells
            // in whatever order each language happens to, and two buildings
            // swapping places is two captions that are not the same caption.
            std::stable_sort(footprints.begin(), footprints.end(),
                             [](const Blob& a, const Blob& b) {
                                 if (a.cells != b.cells) return a.cells > b.cells;
                                 if (a.rect.y != b.rect.y) return a.rect.y < b.rect.y;
                                 return a.rect.x < b.rect.x;
                             });
            for (const Blob& b : footprints) {
                if (buildings.size() >= 6) break;
                if (b.cells < 6 || b.rect.w < 3 || b.rect.h < 3) continue;
                // A ship's hull is drawn out of wall tiles and has an element of
                // its own already.
                double cx = b.rect.x + b.rect.w / 2.0, cy = b.rect.y + b.rect.h / 2.0;
                bool isHull = false;
                for (const Rect& hr : hulls)
                    if (cx >= hr.x && cx <= hr.x + hr.w && cy >= hr.y && cy <= hr.y + hr.h)
                        isHull = true;
                if (isHull) continue;
                // A ring of wall round the whole map is the boundary of the
                // site. Anywhere but a building it is cliffs, treeline or
                // fence, and calling it "one single large building with its
                // roof removed" is how a gorge became a masonry hall with a
                // timber door in it. Measured against the playable field, since
                // every map carries an empty bleed margin around it.
                int fb = arch::BorderOf(map);
                int field = std::max(1, (cols - 2 * fb) * (rows - 2 * fb));
                bool fillsMap = b.rect.w * b.rect.h >= field * 0.75 ||
                                (b.rect.x <= fb + 1 && b.rect.y <= fb + 1 &&
                                 b.rect.x + b.rect.w >= cols - fb - 1 &&
                                 b.rect.y + b.rect.h >= rows - fb - 1);
                if (fillsMap && enclosure != "masonry") {
                    haveSiteEdge = true;
                    siteEdge = b.rect;
                    continue;
                }
                // Caves and woodland have no buildings in them; the loop runs
                // only to find the boundary of the site.
                if (organic) continue;
                // A wall is a building when you cannot get into what it
                // surrounds without going through it. A cliff down one side of
                // a gorge surrounds nothing.
                if (EnclosesFloor(g, b.at) < 6) continue;
                buildings.push_back(b.rect);
                double share = (double)b.rect.w * b.rect.h / std::max(1, cols * rows);
                std::string sizeWord = share > 0.12 ? "large"
                                     : (share < 0.05 ? "small" : "mid-sized");
                // A count attached to the wall it belongs to holds far better
                // than one stated once for the whole map.
                int doorsHere = 0;
                for (int yy = b.rect.y; yy < b.rect.y + b.rect.h; ++yy)
                    for (int xx = b.rect.x; xx < b.rect.x + b.rect.w; ++xx)
                        if (g.Get(xx, yy) == Tile::Door) ++doorsHere;
                // Stated as what the wall is, not as what it lacks: diffusion
                // text encoders handle negation badly.
                std::string doorNote =
                    doorsHere == 1
                        ? "One single plank-filled gap sits in its wall and the rest of that "
                          "wall is one continuous face of " + encFace + " running corner to "
                          "corner"
                  : doorsHere == 0
                        ? "All four of its walls are one continuous face of " + encFace +
                          " running corner to corner"
                        : std::to_string(doorsHere) +
                          " plank-filled gaps sit in its wall and the rest of that wall is "
                          "one continuous face of " + encFace + " running corner to corner";
                // Match a name to a footprint by where it sits, not by list
                // order - the areas include things that are not buildings, which
                // is how a warehouse came to be called "the Moored Ship".
                std::string label;
                std::string inside;
                std::vector<const Area*> held;
                for (const Area& ar : map.areas) {
                    if (ar.label.empty()) continue;
                    double ax = ar.x + ar.w / 2.0, ay = ar.y + ar.h / 2.0;
                    if (ax >= b.rect.x && ax <= b.rect.x + b.rect.w &&
                        ay >= b.rect.y && ay <= b.rect.y + b.rect.h)
                        held.push_back(&ar);
                }
                if (held.size() == 1) {
                    label = LowerFirst(TheLabel(held[0]->label));
                    // The room element is skipped when a building holds one, so
                    // without this its description would be thrown away.
                    inside = Trim(held[0]->description);
                } else if (held.size() > 1) {
                    // Naming it after one of its rooms was a lie that cost the
                    // other rooms their place in the caption.
                    std::string names;
                    for (size_t k = 0; k < held.size(); ++k) {
                        if (k) names += (k + 1 == held.size()) ? " and " : ", ";
                        names += held[k]->label;
                    }
                    inside = "Inside it, divided from each other by the interior walls "
                             "listed below, are exactly " + std::to_string(held.size()) +
                             " rooms and no others: " + names +
                             ". Each is described separately with its own rectangle";
                }
                if (label.empty() && held.empty()) {
                    // Nothing named it, so name it by where it stands. Five
                    // buildings all called "a building" read as one building
                    // drawn five times.
                    double fx = (b.rect.x + b.rect.w / 2.0) / std::max(1, cols);
                    double fy = (b.rect.y + b.rect.h / 2.0) / std::max(1, rows);
                    std::string band = fy < 0.38 ? "north" : (fy > 0.62 ? "south" : "");
                    std::string side = fx < 0.38 ? "west" : (fx > 0.62 ? "east" : "");
                    std::string where = band;
                    if (!band.empty() && !side.empty()) where += "-";
                    where += side;
                    label = where.empty() ? "the middle building"
                                          : "the " + where + " building";
                }
                buildingNames.push_back(label);
                // How thick the wall actually is, said as a share of the
                // picture. "Thick outer walls" is an invitation, and the
                // renderer accepted it: a one-square wall came back as a band a
                // sixth of the map wide, and a band that wide is a place, so it
                // got furnished.
                int depth = 0;
                for (int step = 0; step < b.rect.w; ++step) {
                    Tile t = g.Get(b.rect.x + step, b.rect.y + b.rect.h / 2);
                    if (t == Tile::Wall || t == Tile::Door || t == Tile::Window) ++depth;
                    else break;
                }
                int wallShare = std::max(2, (int)std::lround(100.0 * std::max(1, depth) /
                                                             std::max(b.rect.w, b.rect.h)));
                std::string shell = "One single " + sizeWord + " building" +
                                    (label.empty() ? "" : ", " + label) +
                                    ", standing alone inside this rectangle and nowhere "
                                    "else: one unbroken outer wall running right round the "
                                    "edges of the rectangle, drawn as a narrow line about " +
                                    std::to_string(wallShare) +
                                    " percent of the width of this rectangle and no wider - "
                                    "a line, not a band - with its roof removed so the "
                                    "furnished floor inside is fully visible from above.";
                std::string outsideNote =
                    doorNote + ". The inner face of those outer walls is flat and plain the "
                    "whole way round, and the floor runs right up to it everywhere. The "
                    "ground immediately outside the building on every side is open and free "
                    "of any wall.";
                if (held.size() == 1) {
                    singleRoomShell[buildings.size() - 1] = {shell, outsideNote};
                    continue;
                }
                walls.push_back({
                    {"type", "obj"},
                    {"bbox", Bbox(b.rect.x, b.rect.y, b.rect.w, b.rect.h, cols, rows)},
                    {"desc", shell + (inside.empty() ? std::string()
                                                     : " " + TrimStop(inside) + ".") +
                             " " + outsideNote + " " + kExactS}});
            }
        }

        if (haveSiteEdge) {
            // If the plan already says what the edge of this place is - cliffs
            // down both sides, a treeline along the top - then it has been
            // said, and saying it again in general terms gives the renderer two
            // answers to one question.
            std::set<std::pair<int, int>> band;
            for (int yy = siteEdge.y; yy < siteEdge.y + siteEdge.h; ++yy) {
                for (int xx = siteEdge.x; xx < siteEdge.x + siteEdge.w; ++xx) {
                    Tile k = g.Get(xx, yy);
                    if (xx == siteEdge.x || xx == siteEdge.x + siteEdge.w - 1 ||
                        yy == siteEdge.y || yy == siteEdge.y + siteEdge.h - 1 ||
                        k == Tile::Wall || k == Tile::Door || k == Tile::Window)
                        band.insert({xx, yy});
                }
            }
            // Widened a couple of squares inward: a cliff drawn along the west
            // side of a gorge stands next to the wall, not on it, and asking
            // for an exact overlap found nothing at all - so the style went on
            // describing a palisade round the whole map over the top of two
            // sheer rock faces.
            std::set<std::pair<int, int>> bandNear = band;
            for (const auto& c : band)
                for (int dx = -2; dx <= 2; ++dx)
                    for (int dy = -2; dy <= 2; ++dy)
                        bandNear.insert({c.first + dx, c.second + dy});
            std::set<std::pair<int, int>> spoken;
            for (const auto& note : map.annotations)
                for (int yy = note.y; yy < note.y + std::max(1, note.h); ++yy)
                    for (int xx = note.x; xx < note.x + std::max(1, note.w); ++xx)
                        if (bandNear.count({xx, yy})) spoken.insert({xx, yy});
            // Counted side by side as well as in total. A gorge says what its
            // west and east walls are and says nothing about its ends, which is
            // barely a third of the ring.
            int sidesSpoken = 0;
            const double halves[4][3] = {
                {(double)siteEdge.x, siteEdge.x + siteEdge.w / 2.0, 0},
                {siteEdge.x + siteEdge.w / 2.0, (double)(siteEdge.x + siteEdge.w), 0},
                {(double)siteEdge.y, siteEdge.y + siteEdge.h / 2.0, 1},
                {siteEdge.y + siteEdge.h / 2.0, (double)(siteEdge.y + siteEdge.h), 1}};
            for (const auto& hh : halves) {
                size_t total = 0, said = 0;
                for (const auto& c : bandNear) {
                    double v = hh[2] == 0 ? c.first : c.second;
                    if (v < hh[0] || v >= hh[1]) continue;
                    ++total;
                    if (spoken.count(c)) ++said;
                }
                if (total && said >= 0.35 * total) ++sidesSpoken;
            }
            // Only the sentence is dropped. The rectangle is still the edge
            // of the site, and the wall-run pass needs it to know that the four
            // runs along it are that edge - without it they came back as four
            // separate masonry walls, and a gorge with cliffs down both sides
            // was drawn as a walled compound.
            if (!bandNear.empty() &&
                (spoken.size() >= 0.4 * bandNear.size() || sidesSpoken >= 2))
                siteSpokenFor = true;
        }
        if (haveSiteEdge && !siteSpokenFor) {
            walls.push_back({
                {"type", "obj"},
                {"bbox", Bbox(siteEdge.x, siteEdge.y, siteEdge.w, siteEdge.h, cols, rows)},
                {"desc", "The outer edge of the site, drawn as a continuous band right round "
                         "the four sides of this rectangle and nowhere else: " + encBoundary +
                         ". It is seen from directly overhead, so what is drawn is the flat "
                         "top of it looking straight down and never its face; it is solid "
                         "all the way through, and its edge against the ground inside is one "
                         "clean continuous line the whole way round. It is the far limit of "
                         "the map, not a building: nothing stands on it and nothing is set "
                         "into it. Everything inside it is open ground. " + kExactS}});
        }

        // 4. Doors. Few in number and load-bearing for how the map plays, so each
        //    gets its own box - without this the renderer put openings where it
        //    liked, or left a plain gap where a door belongs.
        auto areaAt = [&map](int px, int py) -> std::string {
            for (const Area& a : map.areas) {
                if (Trim(a.label).empty()) continue;
                if (px >= a.x && px < a.x + a.w && py >= a.y && py < a.y + a.h)
                    return Trim(a.label);
            }
            return std::string();
        };
        // Every door used to be called "in the east wall of this building",
        // which for a door between two chambers is simply false - and the
        // renderer answered it by punching holes in the outer wall where none
        // belong. Look both ways along both axes: a one-cell doorway gives no
        // clue which wall it is in, and guessing from its shape put half of
        // them in the wrong wall.
        auto doorPlace = [&](const Rect& r) -> std::string {
            std::vector<std::string> found;
            const int dirs[4][2] = {{0, -1}, {0, 1}, {-1, 0}, {1, 0}};
            for (auto& d : dirs) {
                for (int step = 1; step <= 2; ++step) {
                    int px = r.x + (d[0] > 0 ? r.w - 1 : 0) + d[0] * step;
                    int py = r.y + (d[1] > 0 ? r.h - 1 : 0) + d[1] * step;
                    std::string got = areaAt(px, py);
                    if (!got.empty()) {
                        if (std::find(found.begin(), found.end(), got) == found.end())
                            found.push_back(got);
                        break;
                    }
                }
            }
            if (found.size() > 1 && found[0] != found[1])
                return "In the interior wall between " + LowerFirst(TheLabel(found[0])) +
                       " and " + LowerFirst(TheLabel(found[1])) + ", and in no other wall";
            if (found.size() == 1)
                return "In the outer wall of the building, the way in and out of " +
                       LowerFirst(TheLabel(found[0]));
            return WhichWall(r, buildings, buildingNames, cols, rows);
        };
        for (const Rect& r : MergeRuns(g, Tile::Door, cols, rows)) {
            structure.push_back({
                {"type", "obj"},
                {"bbox", Bbox(r.x, r.y, r.w, r.h, cols, rows)},
                {"desc", UpperFirst(doorPlace(r)) + ": " +
                         LowerFirst(say("structure", "door", DOOR_TEXT)) + ". " + kExactS}});
        }

        // 4c. Walls that stand on their own. Anything already inside a building
        //     is covered by that building's element; repeating it a dozen times
        //     is what made the renderer read the map as a repeating pattern.
        {
            // Merge each logical wall into one rectangle by treating its
            // openings as wall while the runs are found. The same wall handed
            // over as four separate boxes reads as a repeating pattern, and the
            // renderer answers a repeating pattern with a symmetrical building
            // it invented itself.
            TileGrid solid(cols, rows, Tile::Void);
            for (int yy = 0; yy < rows; ++yy) {
                for (int xx = 0; xx < cols; ++xx) {
                    Tile k = g.Get(xx, yy);
                    solid.Set(xx, yy, (k == Tile::Wall || k == Tile::Door ||
                                       k == Tile::Window) ? Tile::Wall : k);
                }
            }
            int considered = 0;
            for (const Rect& r : MergeRuns(solid, Tile::Wall, cols, rows)) {
                if (considered++ >= (organic ? 4 : kMaxWallRuns)) break;
                // Only genuinely elongated runs. The rest are stubs, or one lump
                // of an irregular mass, and read as noise either way.
                if (std::max(r.w, r.h) < 3 || r.w * r.h < 2) continue;
                // The building element describes its own outline, so an outer
                // wall repeated is noise. An interior partition is the
                // opposite: it is the only thing that says where one room ends
                // and the next begins, and dropping it left a keep as one empty
                // shell for the renderer to subdivide however it liked.
                bool onOutline = false;
                // The boundary of a site is often two cells thick, so a run
                // lying along it can sit two cells in from the corner of its
                // bounding box. Handed over as a wall of its own, it becomes a
                // second wall drawn just inside the first one.
                std::vector<std::pair<Rect, int>> outlines;
                if (haveSiteEdge) outlines.push_back({siteEdge, 2});
                for (const Rect& bb : buildings) outlines.push_back({bb, 1});
                for (const auto& ob : outlines) {
                    const Rect& b = ob.first;
                    const int slack = ob.second;
                    bool inside = r.x >= b.x - 1 && r.y >= b.y - 1 &&
                                  r.x + r.w <= b.x + b.w + 1 && r.y + r.h <= b.y + b.h + 1;
                    if (!inside) continue;
                    bool hugsSide = r.x <= b.x + slack || r.x + r.w >= b.x + b.w - slack;
                    bool hugsTop = r.y <= b.y + slack || r.y + r.h >= b.y + b.h - slack;
                    if ((r.w >= r.h && hugsTop) || (r.h > r.w && hugsSide)) onOutline = true;
                    break;
                }
                if (onOutline) continue;
                // If a region the scene described covers this run, it has
                // already been said what it is made of. A treeline laid down as
                // wall came back drawn as a course of dressed stone, because
                // the generic wall element sat on top of the annotation that
                // called it mossy trunks.
                int spokenRun = 0;
                for (const Annotation& n : map.annotations)
                    for (int yy = r.y; yy < r.y + r.h; ++yy)
                        for (int xx = r.x; xx < r.x + r.w; ++xx)
                            if (xx >= n.x && xx < n.x + std::max(1, n.w) &&
                                yy >= n.y && yy < n.y + std::max(1, n.h)) ++spokenRun;
                if (spokenRun >= 0.5 * std::max(1, r.w * r.h)) continue;
                if (organic) {
                    walls.push_back({
                        {"type", "obj"},
                        {"bbox", Bbox(r.x, r.y, r.w, r.h, cols, rows)},
                        {"desc", std::string("A mass of " + encWall + " filling this "
                                 "whole rectangle solidly, its face irregular and broken but "
                                 "with no passage, gap or opening through it anywhere. ") +
                                 kExactS}});
                    continue;
                }
                // Four wall runs described in identical words are four chances
                // to read the map as a repeating pattern. Where each one runs
                // is both a difference and something the renderer can use.
                double fx = (r.x + r.w / 2.0) / std::max(1, cols);
                double fy = (r.y + r.h / 2.0) / std::max(1, rows);
                std::string band = fy < 0.34 ? "north" : (fy > 0.66 ? "south" : "middle");
                std::string side = fx < 0.34 ? "west" : (fx > 0.66 ? "east" : "centre");
                std::string shape =
                    (r.w >= r.h)
                        ? "a horizontal wall running across the " + band +
                              " of the map, filling the full width of this rectangle from "
                              "its left edge to its right edge"
                        : "a vertical wall running down the " + side +
                              " of the map, filling the full height of this rectangle from "
                              "its top edge to its bottom edge";
                // On an open-air site a wall standing on its own is landscape,
                // not building: a treeline, a hedge, a cliff. Only what belongs
                // to a building is masonry, and a building's own outline never
                // gets here.
                bool inBuilding = false;
                for (const Rect& bb : buildings)
                    if (r.x >= bb.x - 1 && r.y >= bb.y - 1 &&
                        r.x + r.w <= bb.x + bb.w + 1 && r.y + r.h <= bb.y + bb.h + 1)
                        inBuilding = true;
                // ...and only where it runs along the edge of the site. A wall
                // standing in the middle of a ruined field is a piece of the
                // ruin, and calling it a bank of drifted sand is no better than
                // calling a treeline masonry.
                int fbEdge = arch::BorderOf(map);
                bool onFieldEdge = r.x <= fbEdge + 2 || r.y <= fbEdge + 2 ||
                                   r.x + r.w >= cols - fbEdge - 2 ||
                                   r.y + r.h >= rows - fbEdge - 2;
                if (enclosure == "open" && !inBuilding && onFieldEdge) {
                    std::string natural = encBoundary;
                    for (const char* art : {"a ", "an ", "the "}) {
                        std::string low = arch::Lower(natural);
                        if (low.rfind(art, 0) == 0) {
                            natural = natural.substr(std::strlen(art));
                            break;
                        }
                    }
                    walls.push_back({
                        {"type", "obj"},
                        {"bbox", Bbox(r.x, r.y, r.w, r.h, cols, rows)},
                        {"desc", "A band of " + natural + ", " +
                                 LowerFirst(WhereOnMap(r.x, r.y, cols, rows)) + ": " + shape +
                                 ", filling it completely and keeping the same thickness "
                                 "along its entire length, solid the whole way with no gap, "
                                 "no gate and no opening through it. It is seen from "
                                 "directly overhead, so what is drawn is the top of it "
                                 "looking straight down and never its side. " + kExactS}});
                    continue;
                }
                walls.push_back({
                    {"type", "obj"},
                    {"bbox", Bbox(r.x, r.y, r.w, r.h, cols, rows)},
                    {"desc", "A run of " + encWall + ", " +
                             LowerFirst(WhereOnMap(r.x, r.y, cols, rows)) + ": " + shape +
                             ", filling it completely and keeping the same thickness along "
                             "its entire length, with square ends and solid " + encFace +
                             " everywhere except at the doors listed separately below. " +
                             kExactS}});
            }
        }

        // 4d. The open ground. Without it the renderer treats every empty square
        //     as somewhere a building could go, and fills the map with rooms it
        //     invented.
        {
            std::string openWord = (style && !style->ground.empty()) ? style->ground
                                                                     : "open paved ground";
            std::vector<char> free_((size_t)cols * rows, 0);
            for (int y = 0; y < rows; ++y)
                for (int x = 0; x < cols; ++x)
                    free_[(size_t)y * cols + x] = (g.Get(x, y) == Tile::Floor) ? 1 : 0;
            std::vector<Rect> claimed = buildings;
            // Two elements over one rectangle spend the budget twice and read
            // as two different places stacked on top of each other.
            for (const Area& ar : map.areas)
                if (!Trim(ar.label).empty()) claimed.push_back({ar.x, ar.y, ar.w, ar.h});
            for (const Rect& b : claimed)
                for (int y = b.y; y < b.y + b.h; ++y)
                    for (int x = b.x; x < b.x + b.w; ++x)
                        if (x >= 0 && y >= 0 && x < cols && y < rows)
                            free_[(size_t)y * cols + x] = 0;
            int given = 0;
            for (const Rect& r : LargestRects(free_, cols, rows, 4,
                                              std::max(6, (int)(cols * rows * 0.012)))) {
                (void)given;
                // A one-square strip along the edge is not a piece of open
                // ground worth an element of its own; it is the sliver left
                // over between a room and the edge of the field, and four of
                // them ate a sixth of the budget.
                if (std::min(r.w, r.h) < 3) continue;
                walls.push_back({
                    {"type", "obj"},
                    {"bbox", Bbox(r.x, r.y, r.w, r.h, cols, rows)},
                    {"desc", "Open ground of " + openWord + " filling this whole rectangle, "
                             "unbroken from edge to edge: no building, no wall and no "
                             "partition stands anywhere inside it, only loose objects lying "
                             "on the ground. " + kExactS}});
                ++given;
            }
        }

        AddTerrain(normal, g, Tile::Water, say("terrain", "water", "dark green water"),
                   cols, rows, kExactS, map.annotations);
        AddTerrain(normal, g, Tile::Pit,
                   say("terrain", "pit", "an open pit dropping into darkness"), cols, rows,
                   kExactS, map.annotations);
        AddTerrain(normal, g, Tile::Rubble,
                   say("terrain", "rubble", "loose rubble and broken stone"), cols, rows,
                   kExactS, map.annotations);
        AddTerrain(normal, g, Tile::Vegetation,
                   say("terrain", "vegetation", "dense undergrowth"), cols, rows, kExactS,
                   map.annotations);

        // 6. Rooms.
        for (const auto& a : map.areas) {
            std::string label = Trim(a.label);
            std::string lower = arch::Lower(label);
            if (label.empty() || lower == "moored ship" || lower == "quay") continue;
            double rcx = a.x + a.w / 2.0, rcy = a.y + a.h / 2.0;
            const Rect* host = nullptr;
            size_t hostIndex = 0;
            for (size_t bi = 0; bi < buildings.size(); ++bi) {
                const Rect& b = buildings[bi];
                if (rcx >= b.x && rcx <= b.x + b.w && rcy >= b.y && rcy <= b.y + b.h) {
                    host = &b;
                    hostIndex = bi;
                }
            }
            bool single = false;
            if (host) {
                int others = 0;
                for (const Area& a2 : map.areas) {
                    if (&a2 == &a || a2.label.empty()) continue;
                    double ox = a2.x + a2.w / 2.0, oy = a2.y + a2.h / 2.0;
                    if (ox >= host->x && ox <= host->x + host->w &&
                        oy >= host->y && oy <= host->y + host->h) ++others;
                }
                // Only a building with exactly one room in it is fully
                // described by its own element. With two or more, every room
                // needs its own, or the renderer is told the outline and left
                // to invent the inside.
                single = (others == 0);
            }
            // The one thing the renderer gets wrong on every single-room map: it
            // lines the inside of the outer wall with little alcoves. Said as
            // what the wall is rather than as what it lacks, and said here,
            // against the room itself, not once at the top of the caption.
            const std::string undivided =
                "It is a single undivided space filling the whole of this rectangle from "
                "side to side: one unbroken floor, with the four walls around it and "
                "nothing else. The inner face of every one of those walls is flat and plain "
                "along its whole length, and the open floor runs right up to it on all four "
                "sides, so there is no interior wall, no partition, no screen, no alcove, no "
                "niche, no recess, no booth and no smaller room anywhere inside it";
            auto shellIt = host ? singleRoomShell.find(hostIndex) : singleRoomShell.end();
            if (host && shellIt != singleRoomShell.end()) {
                const Rect& b = buildings[hostIndex];
                normal.push_back({
                    {"type", "obj"},
                    {"bbox", Bbox(b.x, b.y, b.w, b.h, cols, rows)},
                    {"desc", shellIt->second.first + " Inside those walls is " +
                             LowerFirst(TheLabel(label)) + " and nothing else, seen from "
                             "directly above with its floor and furniture fully visible and "
                             "no roof, no ceiling and nothing overhanging it. " + undivided +
                             "." +
                             (a.description.empty()
                                  ? std::string()
                                  : " " + UpperFirst(TrimStop(Trim(a.description))) + ".") +
                             " " + shellIt->second.second + " " + kExactS}});
                continue;
            }
            std::string oneRoom = single ? ". " + undivided : "";
            // The site being open air says nothing about one area inside it: a
            // stone house on a burning street is a room even though the street
            // is not. The boundary of the site does not count as its wall, or
            // every cave would be a room - the architect rings the whole field
            // whatever is on it.
            auto onSiteEdge = [&](int px, int py) {
                if (!haveSiteEdge) return false;
                const Rect& e = siteEdge;
                if (px < e.x - 1 || px > e.x + e.w || py < e.y - 1 || py > e.y + e.h)
                    return false;
                return px <= e.x + 2 || px >= e.x + e.w - 3 ||
                       py <= e.y + 2 || py >= e.y + e.h - 3;
            };
            int ringCells = 0, ownWall = 0;
            auto countRing = [&](int px, int py) {
                ++ringCells;
                Tile t = g.Get(px, py);
                if ((t == Tile::Wall || t == Tile::Door || t == Tile::Window) &&
                    !onSiteEdge(px, py)) ++ownWall;
            };
            for (int xx = a.x - 1; xx <= a.x + a.w; ++xx) {
                countRing(xx, a.y - 1);
                countRing(xx, a.y + a.h);
            }
            for (int yy = a.y; yy < a.y + a.h; ++yy) {
                countRing(a.x - 1, yy);
                countRing(a.x + a.w, yy);
            }
            bool walledIn = ringCells > 0 && ownWall >= 0.25 * ringCells;
            std::string opening =
                (enclosure != "masonry" && !host && !walledIn)
                    ? "the open ground of this place seen from directly above, filling this "
                      "rectangle, with nothing above it and nothing overhanging it"
                    : "the roofless interior of a room seen from directly above, its floor "
                      "and furniture fully visible and filling this rectangle, with no roof, "
                      "no ceiling and nothing overhanging it";
            normal.push_back({
                {"type", "obj"},
                {"bbox", Bbox(a.x, a.y, a.w, a.h, cols, rows)},
                {"desc", TheLabel(label) + ": " + opening + oneRoom +
                         (a.description.empty() ? std::string()
                                                : ". " + TrimStop(Trim(a.description)))}});
        }

        // 7. Pinned props get a box; clutter is only described.
        std::map<std::string, int> loose;
        std::map<std::string, int> pinned;
        std::map<std::string, int> kindCounts;
        for (const Feature& f : map.features) ++kindCounts[f.kind];
        for (const auto& f : map.features) {
            // "Structural" says whether a kind of thing is load-bearing enough
            // to pin. It was also deciding whether something asked for by name
            // got a position at all, so a vault with twelve chests came out as
            // one sentence with no chest anywhere in particular.
            bool askedFor = !f.filler;
            if (!f.structural && !askedFor) {
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
                text += ". " + ElaborationPhrase(f.elaboration) + ". " + kExactS;
                critical.push_back({{"type", "obj"},
                                    {"bbox", Bbox(f.x, f.y, 1, 1, cols, rows)},
                                    {"desc", text}});
                continue;
            }
            if (f.filler) {
                // Filler standing against a wall does not get a rectangle of
                // its own. A dozen small boxes ringing the inside of a room
                // reads as a row of compartments, and the renderer duly built
                // one: three renders of a single tavern hall came back lined
                // with little timber bays, one for each crate and barrel
                // pinned to the wall. It is still drawn - it goes into the
                // clutter sentence, which says what there is without saying
                // where each piece stands.
                const int nd[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                bool hugsWall = false;
                for (auto& q : nd) {
                    Tile t = g.Get(f.x + q[0], f.y + q[1]);
                    if (t == Tile::Wall || t == Tile::Door || t == Tile::Window)
                        hugsWall = true;
                }
                // Nor does filler get a box of its own on top of something
                // the scene has already described. Five heaps of rubble pinned
                // inside the rectangle of "a round stone well head standing
                // alone" is two instructions for one square, and the renderer
                // has to pick.
                bool onNote = false;
                for (const Annotation& n : map.annotations)
                    if (f.x >= n.x && f.x < n.x + std::max(1, n.w) &&
                        f.y >= n.y && f.y < n.y + std::max(1, n.h)) onNote = true;
                if (hugsWall || onNote) {
                    std::string pretty = f.kind;
                    std::replace(pretty.begin(), pretty.end(), '_', ' ');
                    ++loose[pretty];
                    continue;
                }
                // Past a few, identical filler elements stop saying "there are
                // several of these" and start saying "this map is a repeating
                // pattern", which the renderer obliges by mirroring the layout.
                if (++pinned[f.kind] > kMaxSameProp) {
                    std::string pretty = f.kind;
                    std::replace(pretty.begin(), pretty.end(), '_', ' ');
                    ++loose[pretty];
                    continue;
                }
            }
            std::string phrase = say("props", f.kind.c_str(), PropPhrase(f.kind).c_str());
            if (phrase.empty()) {
                if (!askedFor) continue;
                // Asked for by name but nobody wrote a description for it. Its
                // own name is a poor description, and still better than silence.
                std::string pretty = f.kind;
                std::replace(pretty.begin(), pretty.end(), '_', ' ');
                phrase = "a " + pretty;
            }
            if (phrase.empty()) continue;
            // Several of one kind described in identical words read as one
            // thing repeated, which is the repetition that mirrors a layout.
            std::string body = phrase.find("from directly above") != std::string::npos
                                   ? phrase
                                   : phrase + ", seen from directly above";
            if (kindCounts[f.kind] > 1)
                body = WhereOnMap(f.x, f.y, cols, rows) + ", " + body;
            filler.push_back({{"type", "obj"},
                              {"bbox", Bbox(f.x, f.y, 1, 1, cols, rows)},
                              {"desc", body}});
        }

        nlohmann::json elements = nlohmann::json::array();
        for (const auto& e : critical) elements.push_back(e);
        for (const auto& e : walls) elements.push_back(e);
        for (const auto& e : structure) elements.push_back(e);
        for (const auto& e : normal) elements.push_back(e);
        for (const auto& e : filler) elements.push_back(e);
        int budget = ElementBudget(map);
        while ((int)elements.size() > budget) elements.erase(elements.end() - 1);

        if (!loose.empty()) {
            std::vector<std::pair<std::string, int>> sorted(loose.begin(), loose.end());
            std::sort(sorted.begin(), sorted.end(), [](const auto& a, const auto& b) {
                if (a.second != b.second) return a.second > b.second;
                return a.first < b.first;
            });
            std::string listed;
            for (size_t i = 0; i < sorted.size() && i < 6; ++i) {
                if (i) listed += ", ";
                listed += std::to_string(sorted[i].second) + " " +
                          Plural(sorted[i].first, sorted[i].second);
            }
            elements.push_back({
                {"type", "obj"},
                {"desc", "Scattered clutter across the walkable ground: " + listed +
                         ", standing about the floor, some of it against the walls and some "
                         "out in the open, casting soft shadows"}});
        }

        std::string ground = (style && !style->ground.empty()) ? style->ground
                                                               : "worn stone paving";
        // A style's materials text describes a whole imagined scene - tents in
        // a ring round a fire, stalls along a street - and it is the strongest
        // text in the caption. Reframing it as "texture only" was not enough: a
        // gorge with cliffs down both sides came back as a palisaded camp round
        // a central fire, which is what bandit_camp describes and nothing like
        // what the plan said. When the map has its own things in its own
        // places, only the first sentence is kept - the one that names what
        // kind of place this is - and the rest, which says where everything
        // stands, is dropped. A map with nothing of its own to say still gets
        // all of it, because then there is nothing for it to argue with.
        // The map's own list of materials. The planner asks every agent for one
        // - "a dense comma-separated list of concrete materials, surface
        // finishes" - and it was stored on the map and then never used
        // anywhere, so a scene that said exactly what it was made of was
        // painted out of a style's general idea of the genre instead. It goes
        // where the style's text used to, and it cannot argue with the plan,
        // because it came with it.
        {
            std::string details = Trim(map.meta.render_details);
            while (!details.empty() && details.back() == '.') details.pop_back();
            if (!details.empty()) ground += ". " + details;
        }
        size_t saidByMap = 0;
        for (const Area& a : map.areas) saidByMap += a.description.size();
        bool speaksForItself = !map.annotations.empty() || saidByMap > 200;
        if (style && !style->materials.empty()) {
            std::string mats = style->materials;
            if (speaksForItself) {
                size_t stop = mats.find(". ");
                if (stop != std::string::npos) mats = mats.substr(0, stop + 1);
            }
            while (!mats.empty() && mats.back() == '.') mats.pop_back();
            // A style describes a whole imagined scene - tents in a ring round
            // a fire, stalls along a street - and that description is the
            // strongest text in the caption, so it used to quietly overrule the
            // plan it was meant to be painting. It is kept for its materials
            // and colour and told, in as many words, that it does not decide
            // where anything goes.
            if (speaksForItself) {
                ground += ". " + mats;
            } else {
                // Used whole, it needs saying that it does not decide where
                // anything goes; trimmed to its first sentence, there is
                // nothing left in it that could.
                ground += ". The following names the kinds of thing this place is made of, "
                          "for texture, material and colour only; it does not say where "
                          "anything stands, and wherever it disagrees with the rectangles "
                          "listed below, the rectangles are right and this is ignored: " +
                          mats;
            }
        }
        std::string background = ground + " " + base.background_suffix;
        for (const std::string& note : atmosphere) background += ". " + note;

        // The blank ring around the playable field. Content boxes are already
        // inset by it, but the model still has to be told the margin is meant
        // to stay empty, or it fills the space with invented scenery.
        if (border > 0 && cols > 0 && rows > 0) {
            int pctX = std::max(1, (int)std::lround(border * 100.0 / cols));
            int pctY = std::max(1, (int)std::lround(border * 100.0 / rows));
            background += ". " + base.border_note + ". The margin is " +
                          std::to_string(pctX) + " percent of the image width down each side "
                          "and " + std::to_string(pctY) +
                          " percent of its height along the top and bottom";
        }

        // The ban opens the caption, but the caption is long and the last thing
        // read carries weight too. Two human figures once appeared in a map
        // whose opening sentence forbade them.
        background += ". " + base.forbidden_suffix;

        cap["compositional_deconstruction"] = {
            {"background", background},
            {"elements", elements}};
        // Said last, because until the elements are built nobody knows how many
        // buildings there are - and this sentence used to claim several of them
        // on a map with one room in it, which is an instruction, not a caveat.
        // It is how a single tavern hall came back ringed with timber bays.
        cap["high_level_description"] =
            cap["high_level_description"].get<std::string>() +
            (buildings.size() > 1
                 ? " The buildings are of different sizes and stand in an irregular "
                   "arrangement."
             : buildings.size() == 1
                 ? " There is exactly one building in this picture and no second building "
                   "anywhere."
                 : "");
        // Said as what the halves are rather than as what the picture is not:
        // the renderer answered "not mirrored" with a map whose left half was a
        // mirror of its right, because the word it acted on was "mirrored".
        cap["high_level_description"] =
            cap["high_level_description"].get<std::string>() +
            " The left half of this map and the right half are different from each other, "
            "and so are the top half and the bottom half: every part of the picture is its "
            "own shape, and each thing in it appears once, in one place, at its own angle."
            // Last, because last is where this caption is strongest, and
            // because a map drawn at a tilt cannot be played on: a wall drawn
            // as a face covers squares a figure has to stand on. Said again in
            // six words rather than trusting the long sentence higher up.
            " Every single thing in this picture is drawn as seen from straight above it: "
            "the top of the wall, the top of the tent, the top of the rock, the top of the "
            "table. No side of anything is visible anywhere in the picture.";
        return cap;
    }

    // Minified single line, exactly as the model expects.
    static std::string BuildJson(const MapData& map, const StyleDef* style,
                                 const BaseStyle& base, const Phrasebook& ph = {}) {
        return Build(map, style, base, ph).dump(-1, ' ', false,
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

    // The tools leave a room description without a closing stop; match that so
    // the two captions are byte for byte the same.
    // Enough English to keep "3 torchs" out of the caption.
    static std::string Plural(const std::string& word, int count) {
        if (count <= 1 || word.empty()) return word;
        // Things there is no plural of: you have three heaps of rubble, not
        // three rubbles, and a scatter of bones is already as plural as it gets.
        static const std::set<std::string> kUncounted = {
            "rubble", "bones", "straw", "hay", "sand", "gravel", "moss", "water",
            "scree", "debris", "wreckage", "grass", "ash", "mud", "ice", "snow",
            "foliage", "undergrowth", "vegetation", "webbing", "rigging"};
        std::string low = word;
        for (char& c : low) c = (char)tolower((unsigned char)c);
        if (kUncounted.count(low)) return word;
        auto endsWith = [&word](const char* suffix) {
            size_t n = std::strlen(suffix);
            return word.size() >= n && word.compare(word.size() - n, n, suffix) == 0;
        };
        if (endsWith("s")) return word;
        if (endsWith("x") || endsWith("z") || endsWith("ch") || endsWith("sh"))
            return word + "es";
        if (endsWith("y") && word.size() > 1 &&
            std::string("aeiou").find(word[word.size() - 2]) == std::string::npos)
            return word.substr(0, word.size() - 1) + "ies";
        return word + "s";
    }

    static std::string TrimStop(std::string s) {
        while (!s.empty() && (s.back() == '.' || s.back() == ' ')) s.pop_back();
        return s;
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
        std::sort(rects.begin(), rects.end(), [](const Rect& a, const Rect& b) {
            if (a.Area() != b.Area()) return a.Area() > b.Area();
            if (a.y != b.y) return a.y < b.y;
            return a.x < b.x;
        });
        return rects;
    }

    static void AddTerrain(nlohmann::json& elements, const TileGrid& g, Tile kind,
                           const std::string& phrase, int cols, int rows,
                           const std::string& exact,
                           const std::vector<Annotation>& notes) {
        std::vector<Rect> rects = MergeRuns(g, kind, cols, rows);
        int minArea = std::max(4, (int)(cols * rows * 0.02));
        for (size_t i = 0; i < rects.size() && i < 2; ++i) {
            if (rects[i].Area() < minArea) break;
            // Has the scene already said what this is, in its own words? A lake
            // the author called "absolutely still black water" does not want a
            // second element calling the same rectangle "dark green water".
            const Rect& rr = rects[i];
            int covered = 0;
            for (int yy = rr.y; yy < rr.y + rr.h; ++yy)
                for (int xx = rr.x; xx < rr.x + rr.w; ++xx)
                    for (const Annotation& n : notes)
                        if (xx >= n.x && xx < n.x + std::max(1, n.w) &&
                            yy >= n.y && yy < n.y + std::max(1, n.h)) {
                            ++covered;
                            break;
                        }
            if (covered >= 0.5 * std::max(1, rr.Area())) continue;
            elements.push_back({
                {"type", "obj"},
                {"bbox", Bbox(rects[i].x, rects[i].y, rects[i].w, rects[i].h, cols, rows)},
                {"desc", "A body of " + phrase + " filling this region, its edge meeting "
                         "the surrounding ground in a clean line. " + exact}});
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
