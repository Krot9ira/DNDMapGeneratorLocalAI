#pragma once
// Deterministic map architect - C++ port of architect.py.
//
// The app describes a scene semantically (what kind of place, which areas, what
// terrain and props); every spatial decision is made here. Keeping geometry out
// of the language model is what makes generated layouts reliably sane.
#include "map_types.h"

#include <cmath>
#include <deque>
#include <map>
#include <random>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace dnd {

struct Rect {
    int x = 0, y = 0, w = 0, h = 0;
    int Area() const { return w * h; }
    std::pair<int, int> Center() const { return {x + w / 2, y + h / 2}; }
};

class Rng {
public:
    explicit Rng(uint32_t seed) : engine_(seed ? seed : 1u) {}
    int Int(int lo, int hi) {  // inclusive
        if (hi <= lo) return lo;
        return std::uniform_int_distribution<int>(lo, hi)(engine_);
    }
    float Float(float lo = 0.0f, float hi = 1.0f) {
        return std::uniform_real_distribution<float>(lo, hi)(engine_);
    }
    bool Chance(float p) { return Float() < p; }
    template <typename T> const T& Pick(const std::vector<T>& v) {
        return v[(size_t)Int(0, (int)v.size() - 1)];
    }
    template <typename T> void Shuffle(std::vector<T>& v) {
        std::shuffle(v.begin(), v.end(), engine_);
    }
private:
    std::mt19937 engine_;
};

namespace arch {

inline int Clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float Clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }

inline std::string Lower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

inline const std::vector<std::string>& LayoutNames() {
    static const std::vector<std::string> v = {"dungeon", "building", "cavern", "open",
                                               "forest", "swamp", "ruins", "street",
                                               "arena", "harbour", "custom"};
    return v;
}

// The grid is free-form; these are only convenient starting points.
constexpr int kMinCells = 10;
constexpr int kMaxCells = 150;

inline const std::vector<std::pair<std::string, std::pair<int, int>>>& SizePresets() {
    static const std::vector<std::pair<std::string, std::pair<int, int>>> v = {
        {"small", {17, 13}}, {"medium", {25, 19}}, {"large", {66, 50}},
        {"huge", {100, 75}}, {"giant", {150, 150}}};
    return v;
}

inline const std::set<std::string>& KnownProps() {
    static const std::set<std::string> s = {
        "torch", "brazier", "bookshelf", "barrel", "crate", "bed", "sarcophagus", "anvil",
        "throne", "statue", "banner", "weapon_rack", "cabinet", "bar", "forge", "console",
        "locker", "dumpster", "chest", "coffin", "shelf", "altar", "fountain", "well",
        "campfire", "table", "portal", "crystal", "obelisk", "cauldron", "hearth", "tree",
        "boulder", "mushroom", "stalagmite", "pillar", "bones", "skull", "skeleton", "lamp",
        "lantern", "sconce", "cart", "wagon", "vending", "net", "rope_coil", "bollard",
        "capstan", "mast", "gem", "shard", "bush", "shrub", "stump", "rock", "stone", "keg",
        "desk", "workbench", "chair", "stool", "tomb", "shrine", "arch", "gate", "flag",
        "sign", "bench", "column"};
    return s;
}

// Language models write "wooden_barrels" and "mooring_bollards"; without this
// every such prop degrades to a featureless blob.
// Object kinds somebody added to the wording file themselves. Set once at
// startup from the loaded phrasebook; empty until then, which is harmless.
inline std::set<std::string>& CustomProps() {
    static std::set<std::string> s;
    return s;
}

inline std::string NormalizeProp(const std::string& raw) {
    std::string name = Lower(raw);
    for (char& c : name) if (c == ' ' || c == '-') c = '_';
    if (name.empty()) return name;
    const auto& known = KnownProps();
    if (known.count(name)) return name;
    // A kind defined by hand is left exactly as written, or "raspberry_bush"
    // collapses into "bush" and loses the description written for it.
    if (CustomProps().count(name)) return name;
    if (name.size() > 1 && name.back() == 's' && known.count(name.substr(0, name.size() - 1)))
        return name.substr(0, name.size() - 1);
    // Longest known kind inside the phrase wins, so "stacked_crates" resolves
    // to "crate" rather than to a shorter accidental match.
    std::string best;
    for (const auto& k : known)
        if (name.find(k) != std::string::npos && k.size() > best.size()) best = k;
    return best.empty() ? name : best;
}

// Props that genuinely shape play get pinned; the rest is left to the renderer.
inline bool IsStructuralPropBuiltIn(const std::string& k);

inline bool IsStructuralProp(const std::string& k) {
    // Something defined by hand was defined deliberately, so it is pinned with
    // its own rectangle rather than left to the renderer's judgement.
    if (CustomProps().count(Lower(k))) return true;
    return IsStructuralPropBuiltIn(k);
}

inline bool IsStructuralPropBuiltIn(const std::string& k) {
    static const std::set<std::string> s = {
        "pillar", "column", "statue", "obelisk", "totem", "idol", "altar", "shrine",
        "sarcophagus", "coffin", "tomb", "table", "bed", "bunk", "throne", "anvil",
        "forge", "hearth", "well", "fountain", "pool", "tree", "boulder", "stalagmite",
        "crystal", "mast", "capstan", "portal", "gate", "arch", "bookshelf", "bar",
        "workbench", "cart", "wagon", "dumpster", "brazier", "campfire", "cauldron",
        "bench", "desk"};
    return s.count(k) > 0;
}

inline bool IsWallProp(const std::string& k) {
    static const std::set<std::string> s = {
        "torch", "brazier", "bookshelf", "barrel", "crate", "bed", "sarcophagus", "anvil",
        "throne", "statue", "banner", "weapon_rack", "cabinet", "bar", "forge", "console",
        "locker", "dumpster", "chest", "coffin", "shelf"};
    return s.count(k) > 0;
}

inline const std::vector<std::string>& DefaultFiller(const std::string& layout) {
    static const std::map<std::string, std::vector<std::string>> m = {
        {"dungeon", {"pillar", "torch", "crate", "barrel", "chest", "brazier", "rubble"}},
        {"building", {"table", "chair", "barrel", "crate", "bookshelf", "bed", "hearth"}},
        {"cavern", {"stalagmite", "boulder", "crystal", "mushroom", "bones"}},
        {"open", {"tree", "boulder", "campfire", "bush", "stump"}},
        {"street", {"crate", "barrel", "dumpster", "lamp", "cart"}},
        {"arena", {"pillar", "brazier", "statue", "bones", "weapon_rack"}},
        {"harbour", {"crate", "barrel", "rope_coil", "bollard", "cart", "net", "lamp"}},
        {"custom", {"pillar", "torch", "crate", "barrel"}},
    };
    auto it = m.find(layout);
    return it != m.end() ? it->second : m.at("dungeon");
}

inline float AmountScale(const std::string& raw) {
    std::string s = Lower(raw);
    if (s == "none") return 0.0f;
    if (s == "low" || s == "light") return 0.5f;
    if (s == "high" || s == "heavy") return 1.6f;
    return 1.0f;
}

// -- geometry helpers -------------------------------------------------

inline std::vector<Rect> BspSplit(Rect root, int count, Rng& rng, int minLeaf) {
    std::vector<Rect> leaves = {root};
    int guard = 0;
    while ((int)leaves.size() < count && guard++ < 200) {
        std::sort(leaves.begin(), leaves.end(),
                  [](const Rect& a, const Rect& b) { return a.Area() > b.Area(); });
        int idx = -1;
        for (int i = 0; i < (int)leaves.size(); ++i) {
            if (leaves[i].w >= minLeaf * 2 || leaves[i].h >= minLeaf * 2) { idx = i; break; }
        }
        if (idx < 0) break;
        Rect r = leaves[idx];
        leaves.erase(leaves.begin() + idx);
        bool horizontal = r.w >= r.h;
        if (horizontal && r.w < minLeaf * 2) horizontal = false;
        if (!horizontal && r.h < minLeaf * 2) horizontal = true;
        if (horizontal) {
            int cut = rng.Int(minLeaf, r.w - minLeaf);
            leaves.push_back({r.x, r.y, cut, r.h});
            leaves.push_back({r.x + cut, r.y, r.w - cut, r.h});
        } else {
            int cut = rng.Int(minLeaf, r.h - minLeaf);
            leaves.push_back({r.x, r.y, r.w, cut});
            leaves.push_back({r.x, r.y + cut, r.w, r.h - cut});
        }
    }
    return leaves;
}

inline std::set<std::pair<int, int>> LargestComponent(const TileGrid& g,
                                                      const std::set<Tile>& kinds) {
    std::set<std::pair<int, int>> seen, best;
    for (int y = 0; y < g.rows; ++y) {
        for (int x = 0; x < g.cols; ++x) {
            if (seen.count({x, y}) || !kinds.count(g.Get(x, y))) continue;
            std::set<std::pair<int, int>> comp;
            std::deque<std::pair<int, int>> q = {{x, y}};
            seen.insert({x, y});
            while (!q.empty()) {
                auto [cx, cy] = q.front();
                q.pop_front();
                comp.insert({cx, cy});
                const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
                for (int i = 0; i < 4; ++i) {
                    int nx = cx + dx[i], ny = cy + dy[i];
                    if (!g.Inside(nx, ny) || seen.count({nx, ny})) continue;
                    if (!kinds.count(g.Get(nx, ny))) continue;
                    seen.insert({nx, ny});
                    q.push_back({nx, ny});
                }
            }
            if (comp.size() > best.size()) best = std::move(comp);
        }
    }
    return best;
}

inline std::vector<std::pair<int, int>> CarveCorridor(TileGrid& g, std::pair<int, int> a,
                                                      std::pair<int, int> b, Rng& rng) {
    std::vector<std::pair<int, int>> path;
    std::vector<std::pair<std::pair<int, int>, std::pair<int, int>>> legs;
    if (rng.Chance(0.5f)) {
        legs = {{a, {b.first, a.second}}, {{b.first, a.second}, b}};
    } else {
        legs = {{a, {a.first, b.second}}, {{a.first, b.second}, b}};
    }
    for (auto& leg : legs) {
        int sx = leg.first.first, sy = leg.first.second;
        int ex = leg.second.first, ey = leg.second.second;
        int stepX = (ex == sx) ? 0 : (ex > sx ? 1 : -1);
        int stepY = (ey == sy) ? 0 : (ey > sy ? 1 : -1);
        int cx = sx, cy = sy, guard = 0;
        while (guard++ < 500) {
            if (g.Inside(cx, cy)) path.push_back({cx, cy});
            if (cx == ex && cy == ey) break;
            cx += stepX;
            cy += stepY;
        }
    }
    for (auto& p : path)
        if (g.Get(p.first, p.second) == Tile::Void) g.Set(p.first, p.second, Tile::Floor);
    return path;
}

// Walls are derived from the floor, never trusted from input: this is what
// guarantees every generated map is properly enclosed.
inline void DeriveWalls(TileGrid& g) {
    std::vector<std::pair<int, int>> add;
    for (int y = 0; y < g.rows; ++y) {
        for (int x = 0; x < g.cols; ++x) {
            if (g.Get(x, y) != Tile::Void) continue;
            bool touching = false;
            for (int dy = -1; dy <= 1 && !touching; ++dy) {
                for (int dx = -1; dx <= 1; ++dx) {
                    if (!dx && !dy) continue;
                    Tile t = g.Get(x + dx, y + dy);
                    if (t != Tile::Void && t != Tile::Wall) { touching = true; break; }
                }
            }
            if (touching) add.push_back({x, y});
        }
    }
    for (auto& p : add) g.Set(p.first, p.second, Tile::Wall);
}

inline void DissolveBorderCell(TileGrid& g, int x, int y) {
    if (g.Get(x, y) != Tile::Wall) return;
    const Tile prefer[4] = {Tile::Floor, Tile::Water, Tile::Vegetation, Tile::Rubble};
    const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
    for (Tile want : prefer) {
        for (int i = 0; i < 4; ++i) {
            if (g.Get(x + dx[i], y + dy[i]) == want) { g.Set(x, y, want); return; }
        }
    }
    g.Set(x, y, Tile::Floor);
}

// Outdoor scenes should read as a slice of a bigger world, not a walled room.
inline void OpenEdges(TileGrid& g) {
    for (int x = 0; x < g.cols; ++x) {
        DissolveBorderCell(g, x, 0);
        DissolveBorderCell(g, x, g.rows - 1);
    }
    for (int y = 0; y < g.rows; ++y) {
        DissolveBorderCell(g, 0, y);
        DissolveBorderCell(g, g.cols - 1, y);
    }
}

inline void PlaceDoors(TileGrid& g, const std::vector<std::pair<RoomSpec, Rect>>& rooms,
                       const std::vector<std::vector<std::pair<int, int>>>& paths) {
    auto inRoom = [&](int px, int py) {
        for (int i = 0; i < (int)rooms.size(); ++i) {
            const Rect& r = rooms[i].second;
            if (px >= r.x && px < r.x + r.w && py >= r.y && py < r.y + r.h) return i;
        }
        return -1;
    };
    std::set<std::pair<int, int>> doors;
    for (const auto& path : paths) {
        if (path.empty()) continue;
        int prev = inRoom(path[0].first, path[0].second);
        for (size_t i = 1; i < path.size(); ++i) {
            int cur = inRoom(path[i].first, path[i].second);
            if (cur != prev) {
                auto cell = (prev != -1) ? path[i] : path[i - 1];
                if (g.Get(cell.first, cell.second) == Tile::Floor && !doors.count(cell)) {
                    int walls = 0;
                    const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
                    for (int k = 0; k < 4; ++k)
                        if (g.Get(cell.first + dx[k], cell.second + dy[k]) == Tile::Wall) ++walls;
                    if (walls >= 2) doors.insert(cell);
                }
            }
            prev = cur;
        }
    }
    for (auto& d : doors) g.Set(d.first, d.second, Tile::Door);
}

// A door only makes sense inside a wall run. Generators and terrain can leave a
// door tile with open ground on both sides, where it reads as a plain gap - and
// then the caption promises a door the renderer has nowhere to put.
inline void FixDoors(TileGrid& g) {
    for (int y = 0; y < g.rows; ++y) {
        for (int x = 0; x < g.cols; ++x) {
            if (g.Get(x, y) != Tile::Door) continue;
            Tile left = g.Get(x - 1, y), right = g.Get(x + 1, y);
            Tile up = g.Get(x, y - 1), down = g.Get(x, y + 1);
            if (left == Tile::Wall && right == Tile::Wall) continue;
            if (up == Tile::Wall && down == Tile::Wall) continue;
            auto solid = [](Tile t) { return t == Tile::Void || t == Tile::Wall; };
            if (solid(left) && solid(right)) {
                g.Set(x - 1, y, Tile::Wall);
                g.Set(x + 1, y, Tile::Wall);
            } else if (solid(up) && solid(down)) {
                g.Set(x, y - 1, Tile::Wall);
                g.Set(x, y + 1, Tile::Wall);
            } else {
                g.Set(x, y, Tile::Floor);  // honest: an opening, not a door
            }
        }
    }
}

inline void Blob(TileGrid& g, float cx, float cy, float radius, Tile kind, Rng& rng,
                 const std::set<Tile>* onlyOn = nullptr, float squish = 1.0f) {
    int r = (int)radius + 2;
    for (int y = (int)cy - r; y <= (int)cy + r; ++y) {
        for (int x = (int)(cx - r * squish) - 1; x <= (int)(cx + r * squish) + 1; ++x) {
            if (!g.Inside(x, y)) continue;
            float dx = (x - cx) / std::max(0.01f, squish);
            float dy = (float)y - cy;
            if (std::sqrt(dx * dx + dy * dy) <= radius * rng.Float(0.8f, 1.1f)) {
                if (!onlyOn || onlyOn->count(g.Get(x, y))) g.Set(x, y, kind);
            }
        }
    }
}

using RoomList = std::vector<std::pair<RoomSpec, Rect>>;
using PathList = std::vector<std::vector<std::pair<int, int>>>;

inline PathList ConnectRooms(TileGrid& g, const RoomList& rooms, Rng& rng, bool loops) {
    PathList paths;
    if (rooms.size() < 2) return paths;
    std::vector<int> connected = {0}, remaining;
    for (int i = 1; i < (int)rooms.size(); ++i) remaining.push_back(i);
    while (!remaining.empty()) {
        int bestD = INT32_MAX, bestC = 0, bestR = 0, bestIdx = 0;
        for (int ci : connected) {
            for (int ri = 0; ri < (int)remaining.size(); ++ri) {
                auto a = rooms[ci].second.Center();
                auto b = rooms[remaining[ri]].second.Center();
                int d = std::abs(a.first - b.first) + std::abs(a.second - b.second);
                if (d < bestD) { bestD = d; bestC = ci; bestR = remaining[ri]; bestIdx = ri; }
            }
        }
        paths.push_back(CarveCorridor(g, rooms[bestC].second.Center(),
                                      rooms[bestR].second.Center(), rng));
        connected.push_back(bestR);
        remaining.erase(remaining.begin() + bestIdx);
    }
    if (loops && rooms.size() >= 4) {
        int extra = rng.Int(0, (int)rooms.size() / 3);
        for (int i = 0; i < extra; ++i) {
            int a = rng.Int(0, (int)rooms.size() - 1);
            int b = rng.Int(0, (int)rooms.size() - 1);
            if (a != b)
                paths.push_back(CarveCorridor(g, rooms[a].second.Center(),
                                              rooms[b].second.Center(), rng));
        }
    }
    return paths;
}

// -- layout generators ------------------------------------------------

inline void CarveShip(TileGrid& g, int hx, int hy, int w, int h) {
    float half = (h - 1) / 2.0f;
    int bow = Clampi((int)(w * 0.34f), 3, 10);
    int stern = Clampi((int)(w * 0.16f), 2, 5);
    std::vector<int> insets((size_t)w, 0);
    for (int i = 0; i < w; ++i) {
        float inset = 0.0f;
        if (i >= w - bow) {
            float t = Clampf((float)(i - (w - bow) + 1) / (float)bow, 0.0f, 1.0f);
            inset = half * (1.0f - std::sqrt(std::max(0.0f, 1.0f - t * t)));  // elliptical bow
        } else if (i < stern) {
            float t = (float)(stern - i) / (float)(stern + 1);
            inset = t * half * 0.45f;
        }
        insets[(size_t)i] = std::max(0, (int)std::lround(inset));
    }
    for (int i = 0; i < w; ++i) {
        int y0 = hy + insets[(size_t)i], y1 = hy + h - insets[(size_t)i];
        for (int y = y0; y < y1; ++y) g.Set(hx + i, y, Tile::Wall);
    }
    // Deck is planking, not stone: it must not read as more quay.
    for (int i = 1; i < w - 1; ++i) {
        int inset = insets[(size_t)i] + 1;
        for (int y = hy + inset; y < hy + h - inset; ++y) g.Set(hx + i, y, Tile::Bridge);
    }
}

inline RoomList GenDungeon(TileGrid& g, const DesignSpec& spec, Rng& rng, PathList& paths) {
    int n = Clampi((int)spec.rooms.size(), 2, 9);
    int minLeaf = Clampi(std::min(g.cols, g.rows) / 3, 6, 12);
    auto leaves = BspSplit({1, 1, g.cols - 2, g.rows - 2}, n, rng, minLeaf);
    std::sort(leaves.begin(), leaves.end(),
              [](const Rect& a, const Rect& b) { return a.Area() > b.Area(); });

    std::vector<int> order(spec.rooms.size());
    for (size_t i = 0; i < order.size(); ++i) order[i] = (int)i;
    std::sort(order.begin(), order.end(), [&](int a, int b) {
        auto weight = [&](char c) { return c == 'l' ? 3 : (c == 's' ? 1 : 2); };
        return weight(spec.rooms[(size_t)a].size) > weight(spec.rooms[(size_t)b].size);
    });

    RoomList rooms;
    size_t slots = std::min(leaves.size(), order.size());
    for (size_t s = 0; s < slots; ++s) {
        Rect L = leaves[s];
        int padX = (L.w > 9) ? rng.Int(1, 2) : 1;
        int padY = (L.h > 9) ? rng.Int(1, 2) : 1;
        Rect r{L.x + padX, L.y + padY, L.w - padX * 2, L.h - padY * 2};
        if (r.w < 3 || r.h < 3) r = {L.x + 1, L.y + 1, L.w - 2, L.h - 2};
        r.w = std::min(r.w, g.cols - 1 - r.x);
        r.h = std::min(r.h, g.rows - 1 - r.y);
        if (r.w < 3 || r.h < 3) continue;
        g.FillRect(r.x, r.y, r.w, r.h, Tile::Floor);
        rooms.push_back({spec.rooms[(size_t)order[s]], r});
    }
    paths = ConnectRooms(g, rooms, rng, true);
    return rooms;
}

inline RoomList GenBuilding(TileGrid& g, const DesignSpec& spec, Rng& rng, PathList& paths) {
    int n = Clampi((int)spec.rooms.size(), 2, 8);
    int minLeaf = Clampi(std::min(g.cols, g.rows) / 4, 5, 10);
    auto leaves = BspSplit({1, 1, g.cols - 2, g.rows - 2}, n, rng, minLeaf);
    RoomList rooms;
    for (size_t i = 0; i < leaves.size() && i < spec.rooms.size(); ++i) {
        Rect L = leaves[i];
        // Inset by one so the untouched cells between leaves become partitions.
        Rect r{L.x + 1, L.y + 1, L.w - 1, L.h - 1};
        if (r.w < 2 || r.h < 2) continue;
        g.FillRect(r.x, r.y, r.w, r.h, Tile::Floor);
        rooms.push_back({spec.rooms[i], r});
    }
    paths = ConnectRooms(g, rooms, rng, false);
    if (!rooms.empty()) {
        const Rect* entry = &rooms[0].second;
        for (auto& rp : rooms) if (rp.second.y < entry->y) entry = &rp.second;
        g.Set(entry->x + entry->w / 2, entry->y - 1, Tile::Door);
    }
    return rooms;
}

// Chambers joined by winding passages, then softened into rock. Pure cellular
// automata kept collapsing below the safety threshold and falling back to a
// single oval, so every cave came out as one featureless blob.
inline RoomList GenCavern(TileGrid& g, const DesignSpec& spec, Rng& rng, PathList&) {
    g.FillRect(0, 0, g.cols, g.rows, Tile::Void);
    float span = (float)std::min(g.cols, g.rows);

    int n = Clampi((int)spec.rooms.size(), 1, 6);
    std::vector<std::pair<int, int>> centres;
    RoomList rooms;
    for (int i = 0; i < n; ++i) {
        // Spread the chambers around the map instead of clustering them.
        float ang = (i / (float)n) * 6.2831853f + rng.Float(-0.4f, 0.4f);
        float spread = rng.Float(0.22f, 0.36f);
        float cx = g.cols / 2.0f + std::cos(ang) * g.cols * spread;
        float cy = g.rows / 2.0f + std::sin(ang) * g.rows * spread;
        char size = spec.rooms[(size_t)i].size ? spec.rooms[(size_t)i].size : 'm';
        float hint = size == 'l' ? 0.30f : (size == 's' ? 0.18f : 0.24f);
        float radius = std::clamp(span * hint * rng.Float(0.9f, 1.25f), 3.0f, span * 0.34f);
        cx = std::clamp(cx, radius + 2, g.cols - radius - 2);
        cy = std::clamp(cy, radius + 2, g.rows - radius - 2);
        Blob(g, cx, cy, radius, Tile::Floor, rng, nullptr, rng.Float(0.7f, 1.5f));
        centres.push_back({(int)cx, (int)cy});
        int r = (int)radius;
        rooms.push_back({spec.rooms[(size_t)i],
                         {Clampi((int)cx - r / 2, 0, g.cols - 2),
                          Clampi((int)cy - r / 2, 0, g.rows - 2),
                          std::max(3, r), std::max(3, r)}});
    }

    // Winding passages. A straight corridor would look quarried, so each leg is
    // carved and then blistered outwards.
    for (size_t i = 1; i < centres.size(); ++i) {
        auto path = CarveCorridor(g, centres[i - 1], centres[i], rng);
        for (size_t j = 0; j < path.size(); ++j) {
            Blob(g, (float)path[j].first, (float)path[j].second, rng.Float(1.4f, 2.0f),
                 Tile::Floor, rng);
            if (j % std::max<size_t>(2, path.size() / 5) == 0)
                Blob(g, (float)path[j].first, (float)path[j].second, rng.Float(2.2f, 3.6f),
                     Tile::Floor, rng);
        }
    }
    if (centres.size() > 2 && rng.Chance(0.7f))
        CarveCorridor(g, centres.back(), centres.front(), rng);

    // One round of smoothing to eat the corners off.
    {
        std::vector<Tile> snap = g.cells;
        for (int y = 1; y < g.rows - 1; ++y) {
            for (int x = 1; x < g.cols - 1; ++x) {
                int solid = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx) {
                        if (!dx && !dy) continue;
                        if (snap[(size_t)(y + dy) * g.cols + (x + dx)] == Tile::Void) ++solid;
                    }
                Tile here = snap[(size_t)y * g.cols + x];
                if (here == Tile::Floor && solid >= 6) g.Set(x, y, Tile::Void);
                else if (here == Tile::Void && solid <= 2) g.Set(x, y, Tile::Floor);
            }
        }
    }

    // A rim of rock, so the cave never runs off the edge of the map.
    for (int x = 0; x < g.cols; ++x)
        for (int y = 0; y < g.rows; ++y)
            if (x == 0 || y == 0 || x == g.cols - 1 || y == g.rows - 1)
                g.Set(x, y, Tile::Void);

    // A cave has to be worth walking into. If the chambers landed small or on
    // top of each other, grow them until the field is properly hollowed out.
    for (int grow = 0; grow < 4; ++grow) {
        int floorNow = 0;
        for (int y = 0; y < g.rows; ++y)
            for (int x = 0; x < g.cols; ++x)
                if (g.Get(x, y) != Tile::Void) ++floorNow;
        if (floorNow >= (int)(g.cols * g.rows * 0.30)) break;
        for (auto& c : centres)
            Blob(g, (float)c.first, (float)c.second, span * rng.Float(0.16f, 0.24f),
                 Tile::Floor, rng, nullptr, rng.Float(0.8f, 1.4f));
    }

    // Roughen the rock face. The passages are carved along axes, so without
    // this a cave ends up with long straight edges and looks quarried.
    {
        std::vector<std::pair<int, int>> edge;
        for (int y = 1; y < g.rows - 1; ++y) {
            for (int x = 1; x < g.cols - 1; ++x) {
                if (g.Get(x, y) != Tile::Void) continue;
                if (g.Get(x + 1, y) == Tile::Floor || g.Get(x - 1, y) == Tile::Floor ||
                    g.Get(x, y + 1) == Tile::Floor || g.Get(x, y - 1) == Tile::Floor)
                    edge.push_back({x, y});
            }
        }
        for (auto& c : edge)
            if (rng.Chance(0.32f)) g.Set(c.first, c.second, Tile::Floor);
        edge.clear();
        for (int y = 1; y < g.rows - 1; ++y) {
            for (int x = 1; x < g.cols - 1; ++x) {
                if (g.Get(x, y) != Tile::Void) continue;
                if (g.Get(x + 1, y) == Tile::Floor || g.Get(x - 1, y) == Tile::Floor ||
                    g.Get(x, y + 1) == Tile::Floor || g.Get(x, y - 1) == Tile::Floor)
                    edge.push_back({x, y});
            }
        }
        for (auto& c : edge)
            if (rng.Chance(0.18f)) g.Set(c.first, c.second, Tile::Floor);
    }

    auto keep = LargestComponent(g, {Tile::Floor});
    for (int y = 0; y < g.rows; ++y)
        for (int x = 0; x < g.cols; ++x)
            if (g.Get(x, y) == Tile::Floor && !keep.count({x, y})) g.Set(x, y, Tile::Void);

    std::set<Tile> onFloor{Tile::Floor};
    int heaps = Clampi(g.cols * g.rows / 130, 2, 16);
    for (int i = 0; i < heaps; ++i)
        Blob(g, rng.Float(0, (float)g.cols), rng.Float(0, (float)g.rows),
             rng.Float(1.0f, 2.4f), Tile::Rubble, rng, &onFloor);
    return rooms;
}

inline RoomList GenOpen(TileGrid& g, const DesignSpec& spec, Rng& rng, PathList&) {
    g.FillRect(0, 0, g.cols, g.rows, Tile::Floor);
    for (int x = 0; x < g.cols; ++x)
        for (int y = 0; y < g.rows; ++y) {
            int edge = std::min(std::min(x, y), std::min(g.cols - 1 - x, g.rows - 1 - y));
            if (edge == 0 || (edge == 1 && rng.Chance(0.55f))) g.Set(x, y, Tile::Vegetation);
        }
    RoomList rooms;
    int n = Clampi((int)spec.rooms.size(), 1, 6);
    int colsN = n > 2 ? 3 : n;
    int rowsN = std::max(1, (n + colsN - 1) / colsN);
    int cw = g.cols / colsN, ch = g.rows / rowsN;
    for (int i = 0; i < n; ++i) {
        int gx = i % colsN, gy = i / colsN;
        rooms.push_back({spec.rooms[(size_t)i],
                         Rect{gx * cw + cw / 4, gy * ch + ch / 4,
                              std::max(3, cw / 2), std::max(3, ch / 2)}});
    }
    if (g.cols > 10) {  // a worn track gives the renderer something to follow
        int py = g.rows / 2 + rng.Int(-1, 1);
        for (int x = 0; x < g.cols; ++x) {
            int wob = (int)std::lround(std::sin(x / 3.5f) * 1.5f);
            for (int dy = -1; dy <= 0; ++dy)
                g.Set(x, Clampi(py + wob + dy, 1, g.rows - 2), Tile::Rubble);
        }
    }
    return rooms;
}

// Dense woodland: the default state is thicket, and clearings are carved out of
// it. The opposite of `open`, where the default is bare ground.
inline RoomList GenForest(TileGrid& g, const DesignSpec& spec, Rng& rng, PathList&) {
    g.FillRect(0, 0, g.cols, g.rows, Tile::Vegetation);

    RoomList rooms;
    int n = Clampi((int)spec.rooms.size(), 1, 6);
    float span = (float)std::min(g.cols, g.rows);
    for (int i = 0; i < n; ++i) {
        const RoomSpec& rs = spec.rooms[i];
        char size = rs.size ? rs.size : 'm';
        float hint = size == 'l' ? 0.20f : (size == 's' ? 0.09f : 0.14f);
        float radius = std::clamp(span * hint, 2.0f, 14.0f);
        float cx = rng.Float(radius + 1, g.cols - radius - 1);
        float cy = rng.Float(radius + 1, g.rows - radius - 1);
        Blob(g, cx, cy, radius, Tile::Floor, rng, nullptr, rng.Float(0.8f, 1.4f));
        int r = (int)radius;
        rooms.push_back({rs, {Clampi((int)cx - r / 2, 0, g.cols - 2),
                              Clampi((int)cy - r / 2, 0, g.rows - 2),
                              std::max(3, r), std::max(3, r)}});
    }

    // Trodden paths joining the clearings, so the scene is actually crossable.
    for (size_t i = 1; i < rooms.size(); ++i) {
        for (auto cell : CarveCorridor(g, rooms[i - 1].second.Center(),
                                       rooms[i].second.Center(), rng)) {
            g.Set(cell.first, cell.second, Tile::Rubble);
            if (rng.Chance(0.5f))
                g.Set(cell.first, cell.second + (rng.Chance(0.5f) ? -1 : 1), Tile::Rubble);
        }
    }

    // A scatter of thicker undergrowth so the canopy is not uniform.
    std::set<Tile> onFloor{Tile::Floor};
    int clumps = Clampi(g.cols * g.rows / 120, 2, 20);
    for (int i = 0; i < clumps; ++i)
        Blob(g, rng.Float(0, (float)g.cols), rng.Float(0, (float)g.rows),
             rng.Float(1.5f, 4.0f), Tile::Vegetation, rng, &onFloor);
    return rooms;
}

// Standing water broken by reed beds and tussocks of solid ground.
inline RoomList GenSwamp(TileGrid& g, const DesignSpec& spec, Rng& rng, PathList&) {
    g.FillRect(0, 0, g.cols, g.rows, Tile::Water);
    float span = (float)std::min(g.cols, g.rows);

    RoomList rooms;
    int n = Clampi((int)spec.rooms.size(), 1, 6);
    for (int i = 0; i < n; ++i) {
        float radius = std::clamp(span * rng.Float(0.10f, 0.18f), 2.0f, 12.0f);
        float cx = rng.Float(radius + 1, g.cols - radius - 1);
        float cy = rng.Float(radius + 1, g.rows - radius - 1);
        Blob(g, cx, cy, radius, Tile::Floor, rng, nullptr, rng.Float(0.7f, 1.5f));
        int r = (int)radius;
        rooms.push_back({spec.rooms[i], {Clampi((int)cx - r / 2, 0, g.cols - 2),
                                         Clampi((int)cy - r / 2, 0, g.rows - 2),
                                         std::max(3, r), std::max(3, r)}});
    }

    // Reed beds along the waterline.
    std::set<Tile> onWater{Tile::Water};
    int beds = Clampi(g.cols * g.rows / 90, 3, 26);
    for (int i = 0; i < beds; ++i)
        Blob(g, rng.Float(0, (float)g.cols), rng.Float(0, (float)g.rows),
             rng.Float(1.5f, 4.0f), Tile::Vegetation, rng, &onWater);

    // Plank walkways between the islands - a swamp you cannot cross is useless.
    for (size_t i = 1; i < rooms.size(); ++i) {
        for (auto cell : CarveCorridor(g, rooms[i - 1].second.Center(),
                                       rooms[i].second.Center(), rng)) {
            Tile t = g.Get(cell.first, cell.second);
            if (t == Tile::Water || t == Tile::Vegetation)
                g.Set(cell.first, cell.second, Tile::Bridge);
        }
    }
    return rooms;
}

// An open site strewn with fragments of collapsed building.
inline RoomList GenRuins(TileGrid& g, const DesignSpec& spec, Rng& rng, PathList&) {
    g.FillRect(0, 0, g.cols, g.rows, Tile::Floor);
    for (int x = 0; x < g.cols; ++x) {
        for (int y = 0; y < g.rows; ++y) {
            int edge = std::min(std::min(x, y), std::min(g.cols - 1 - x, g.rows - 1 - y));
            if (edge == 0 || (edge == 1 && rng.Chance(0.4f))) g.Set(x, y, Tile::Vegetation);
        }
    }

    RoomList rooms;
    int n = Clampi((int)spec.rooms.size(), 2, 7);
    int minLeaf = Clampi(std::min(g.cols, g.rows) / 5, 4, 12);
    for (const Rect& leaf : BspSplit({2, 2, g.cols - 4, g.rows - 4}, n, rng, minLeaf)) {
        size_t i = rooms.size();
        if (i >= spec.rooms.size() || leaf.w < 4 || leaf.h < 4) continue;
        int rx = leaf.x + 1, ry = leaf.y + 1, rw = leaf.w - 2, rh = leaf.h - 2;
        // Broken outline: each wall run survives only in pieces.
        for (int x = rx; x < rx + rw; ++x) {
            if (rng.Chance(0.65f)) g.Set(x, ry, Tile::Wall);
            if (rng.Chance(0.65f)) g.Set(x, ry + rh - 1, Tile::Wall);
        }
        for (int y = ry; y < ry + rh; ++y) {
            if (rng.Chance(0.65f)) g.Set(rx, y, Tile::Wall);
            if (rng.Chance(0.65f)) g.Set(rx + rw - 1, y, Tile::Wall);
        }
        rooms.push_back({spec.rooms[i], {rx + 1, ry + 1, std::max(2, rw - 2),
                                         std::max(2, rh - 2)}});
    }

    std::set<Tile> onFloor{Tile::Floor};
    int heaps = Clampi(g.cols * g.rows / 100, 3, 24);
    for (int i = 0; i < heaps; ++i)
        Blob(g, rng.Float(0, (float)g.cols), rng.Float(0, (float)g.rows),
             rng.Float(1.0f, 3.0f), Tile::Rubble, rng, &onFloor);
    return rooms;
}

// One ship under way, open water on every side. `harbour` builds a quay and
// moors a vessel against it; a fight on the deck of a ship at sea is a
// different map, where the deck is the whole playing field.
inline RoomList GenDeck(TileGrid& g, const DesignSpec& spec, Rng&, PathList&,
                        std::vector<Structure>& structures) {
    g.FillRect(0, 0, g.cols, g.rows, Tile::Water);

    int hullW = Clampi((int)(g.cols * 0.82f), 8, g.cols - 2);
    int hullH = Clampi((int)(g.rows * 0.62f), 5, g.rows - 4);
    int hx = (g.cols - hullW) / 2, hy = (g.rows - hullH) / 2;
    CarveShip(g, hx, hy, hullW, hullH);

    int midY = hy + hullH / 2;
    for (int dx = -1; dx <= 1; ++dx) g.Set(hx + hullW / 2 + dx, midY, Tile::Bridge);

    const char* fallback[3] = {"Forecastle", "Main Deck", "Quarterdeck"};
    const float at[3] = {0.70f, 0.38f, 0.10f};
    RoomList rooms;
    for (int i = 0; i < 3; ++i) {
        RoomSpec rs = (i < (int)spec.rooms.size()) ? spec.rooms[i] : RoomSpec{};
        if (rs.label.empty()) rs.label = fallback[i];
        if (rs.id.empty()) rs.id = rs.label;
        int w = std::max(3, hullW / 5), h = std::max(3, hullH / 2);
        int cx = hx + (int)(hullW * at[i]);
        rooms.push_back({rs, {Clampi(cx - w / 2, hx + 1, g.cols - w - 1),
                              Clampi(midY - h / 2, hy + 1, g.rows - h - 1), w, h}});
    }
    structures.push_back({"ship", hx, hy, hullW, hullH});
    return rooms;
}

inline RoomList GenStreet(TileGrid& g, const DesignSpec& spec, Rng& rng, PathList&) {
    g.FillRect(0, 0, g.cols, g.rows, Tile::Floor);
    int roadH = Clampi(g.rows / 3, 4, 8);
    int roadY = (g.rows - roadH) / 2;
    RoomList rooms;
    int n = Clampi((int)spec.rooms.size(), 2, 8);
    int perSide = std::max(2, (n + 1) / 2);
    int available = g.cols - 2;
    int bw = Clampi((available + 1) / perSide - 1, 5, 13);
    for (int side = 0; side < 2; ++side) {
        int top = side == 0 ? 1 : roadY + roadH + 1;
        int h = side == 0 ? (roadY - 1) : (g.rows - top - 1);
        if (h < 4) continue;
        for (int i = 0; i < perSide; ++i) {
            int idx = side * perSide + i;
            if (idx >= n || idx >= (int)spec.rooms.size()) break;
            int bx = 1 + i * (bw + 1);
            if (bx + bw > g.cols - 1) break;
            g.FillRect(bx, top, bw, h, Tile::Void);
            g.FillRect(bx + 1, top + 1, bw - 2, h - 2, Tile::Floor);
            rooms.push_back({spec.rooms[(size_t)idx],
                             Rect{bx + 1, top + 1, std::max(2, bw - 2), std::max(2, h - 2)}});
            g.Set(bx + bw / 2, side == 0 ? top + h - 1 : top, Tile::Door);
            // Some cottages open onto the side path instead of, or as well as,
            // the lane. A row of identical front doors reads as a barracks.
            if (h >= 6 && rng.Chance(0.55f)) {
                int sx = rng.Chance(0.5f) ? bx : bx + bw - 1;
                int sy = top + 2 + rng.Int(0, std::max(0, h - 5));
                g.Set(sx, sy, Tile::Door);
            }
        }
    }
    // Too small for buildings either side: the road is the whole map, and one
    // named stretch of it is better than nothing to hang props on.
    if (rooms.empty()) {
        RoomSpec rs = spec.rooms.empty()
                          ? RoomSpec{"street", "Street", "", 'm', "none", {}, false, 0, 0, 0, 0}
                          : spec.rooms[0];
        rooms.push_back({rs, {1, 1, std::max(2, g.cols - 2), std::max(2, g.rows - 2)}});
    }
    return rooms;
}

// One dramatic chamber: a sand floor ringed by a barrier with four gates. It
// used to be a plain rectangle with two rows of pillars, which a tile histogram
// could not tell apart from `building`.
// Buildings edge to edge, alleys between them, city to every margin. `street`
// is one road with open ground beyond it; a city fight usually is not that.
inline RoomList GenDistrict(TileGrid& g, const DesignSpec& spec, Rng& rng, PathList&) {
    g.FillRect(0, 0, g.cols, g.rows, Tile::Floor);

    RoomList rooms;
    Rect inner{1, 1, g.cols - 2, g.rows - 2};
    if (inner.w < 6 || inner.h < 6) {
        RoomSpec rs = spec.rooms.empty()
                          ? RoomSpec{"street", "Street", "", 'm', "none", {}, false, 0, 0, 0, 0}
                          : spec.rooms[0];
        rooms.push_back({rs, {1, 1, std::max(2, g.cols - 2), std::max(2, g.rows - 2)}});
        return rooms;
    }

    int wanted = Clampi(spec.rooms.empty() ? 3 : (int)spec.rooms.size(), 2, 9);
    int minLeaf = Clampi(std::min(g.cols, g.rows) / 4, 6, 14);
    for (const Rect& leaf : BspSplit(inner, wanted, rng, minLeaf)) {
        // The alley is the gap left between one block and the next.
        int bx = leaf.x + 1, by = leaf.y + 1, bw = leaf.w - 2, bh = leaf.h - 2;
        if (bw < 4 || bh < 4) continue;   // too thin for a building; leave it as street

        g.FillRect(bx, by, bw, bh, Tile::Wall);
        g.FillRect(bx + 1, by + 1, bw - 2, bh - 2, Tile::Floor);

        // Bigger houses get a partition, so an interior is not one bare box.
        if (bw >= 9 && bh >= 6 && rng.Chance(0.7f)) {
            int px = bx + 3 + rng.Int(0, std::max(0, bw - 7));
            for (int y = by + 1; y < by + bh - 1; ++y) g.Set(px, y, Tile::Wall);
            g.Set(px, by + 1 + rng.Int(0, std::max(0, bh - 3)), Tile::Door);
        } else if (bh >= 9 && bw >= 6 && rng.Chance(0.7f)) {
            int py = by + 3 + rng.Int(0, std::max(0, bh - 7));
            for (int x = bx + 1; x < bx + bw - 1; ++x) g.Set(x, py, Tile::Wall);
            g.Set(bx + 1 + rng.Int(0, std::max(0, bw - 3)), py, Tile::Door);
        }

        // A street door, on a side that actually faces an alley.
        struct Side { int x, y, ox, oy; };
        std::vector<Side> sides;
        if (by > 1) sides.push_back({bx + bw / 2, by, bx + bw / 2, by - 1});
        if (by + bh < g.rows - 1)
            sides.push_back({bx + bw / 2, by + bh - 1, bx + bw / 2, by + bh});
        if (bx > 1) sides.push_back({bx, by + bh / 2, bx - 1, by + bh / 2});
        if (bx + bw < g.cols - 1)
            sides.push_back({bx + bw - 1, by + bh / 2, bx + bw, by + bh / 2});
        for (size_t i = sides.size(); i > 1; --i)
            std::swap(sides[i - 1], sides[(size_t)rng.Int(0, (int)i - 1)]);
        for (const Side& sd : sides) {
            if (g.Get(sd.ox, sd.oy) == Tile::Floor) {
                g.Set(sd.x, sd.y, Tile::Door);
                break;
            }
        }

        size_t idx = rooms.size();
        RoomSpec rs = idx < spec.rooms.size() ? spec.rooms[idx] : RoomSpec{};
        if (rs.label.empty()) rs.label = "Building " + std::to_string(idx + 1);
        if (rs.id.empty()) rs.id = rs.label;
        rooms.push_back({rs, {bx + 1, by + 1, std::max(2, bw - 2), std::max(2, bh - 2)}});
    }

    if (rooms.empty()) {
        RoomSpec rs = spec.rooms.empty()
                          ? RoomSpec{"street", "Street", "", 'm', "none", {}, false, 0, 0, 0, 0}
                          : spec.rooms[0];
        rooms.push_back({rs, {1, 1, std::max(2, g.cols - 2), std::max(2, g.rows - 2)}});
    }
    return rooms;
}

inline RoomList GenArena(TileGrid& g, const DesignSpec& spec, Rng&, PathList&) {
    g.FillRect(0, 0, g.cols, g.rows, Tile::Void);
    const int m = 1;
    g.FillRect(m, m, g.cols - m * 2, g.rows - m * 2, Tile::Floor);

    float cx = g.cols / 2.0f, cy = g.rows / 2.0f;
    float radius = std::min(g.cols, g.rows) / 2.0f - 3.0f;

    if (radius >= 3.0f) {
        for (int y = 0; y < g.rows; ++y) {
            for (int x = 0; x < g.cols; ++x) {
                float dx = (x - cx) / std::max(1.0f, radius);
                float dy = (y - cy) / std::max(1.0f, radius * 0.82f);
                float d = std::sqrt(dx * dx + dy * dy);
                if (d >= 0.97f && d <= 1.06f) g.Set(x, y, Tile::Wall);
            }
        }
        // Cut the gates by walking outward from the centre: trigonometry kept
        // missing the ring and sealing the gallery off.
        const int dirs[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
        for (auto& d : dirs) {
            for (int lane = -1; lane <= 1; ++lane) {
                int x = (int)cx + (d[1] ? lane : 0);
                int y = (int)cy + (d[0] ? lane : 0);
                for (int step = 0; step < std::max(g.cols, g.rows); ++step) {
                    x += d[0];
                    y += d[1];
                    if (!g.Inside(x, y)) break;
                    if (g.Get(x, y) == Tile::Wall) g.Set(x, y, Tile::Floor);
                }
            }
        }
        for (int y = 0; y < g.rows; ++y) {
            for (int x = 0; x < g.cols; ++x) {
                float dx = (x - cx) / std::max(1.0f, radius);
                float dy = (y - cy) / std::max(1.0f, radius * 0.82f);
                if (std::sqrt(dx * dx + dy * dy) < 0.95f && g.Get(x, y) == Tile::Floor)
                    g.Set(x, y, Tile::Rubble);
            }
        }
    }

    RoomSpec first = spec.rooms.empty()
                         ? RoomSpec{"arena", "Arena", "", 'l', "none", {}, false, 0, 0, 0, 0}
                         : spec.rooms[0];
    int r = std::max(2, (int)(radius * 0.7f));
    RoomList rooms;
    rooms.push_back({first, {Clampi((int)cx - r, 1, g.cols - 3),
                             Clampi((int)cy - r / 2, 1, g.rows - 3),
                             std::max(3, r * 2), std::max(3, r)}});
    for (size_t i = 1; i < spec.rooms.size() && i < 3; ++i) {
        int side = (i % 2 == 1) ? -1 : 1;
        int w = std::max(3, g.cols / 7), h = std::max(3, g.rows / 6);
        int rx = Clampi((int)(cx + side * (g.cols / 2.0f - w / 2.0f - 1)) - w / 2, 1,
                        g.cols - w - 1);
        int ry = Clampi((int)cy - h / 2, 1, g.rows - h - 1);
        rooms.push_back({spec.rooms[i], {rx, ry, w, h}});
    }
    return rooms;
}

inline RoomList GenHarbour(TileGrid& g, DesignSpec& spec, Rng& rng, PathList&,
                          std::vector<Structure>& outStructures) {
    float frac = Clampf(spec.water_fraction, 0.2f, 0.6f);
    int split = Clampi((int)std::lround(g.rows * (1.0f - frac)), 5, g.rows - 5);
    g.FillRect(0, 0, g.cols, g.rows, Tile::Floor);
    g.FillRect(0, split, g.cols, g.rows - split, Tile::Water);
    int quayY = split - 1;

    RoomList rooms;
    int waterRows = g.rows - split;
    int hullH = Clampi(waterRows - 2, 4, 9);
    int hullW = Clampi((int)(g.cols * 0.55f), 8, g.cols - 4);
    int hx = (g.cols - hullW) / 2, hy = split + 1;
    if (hy + hullH > g.rows) hullH = g.rows - hy;
    CarveShip(g, hx, hy, hullW, hullH);
    outStructures.push_back({"ship", hx, hy, hullW, hullH, "e"});
    rooms.push_back({RoomSpec{"ship", "Moored Ship", "", 'l', "none",
                              {"mast", "capstan", "crate", "barrel", "rope_coil"},
                              false, 0, 0, 0, 0},
                     Rect{hx + 2, hy + 1, std::max(2, hullW - 4), std::max(2, hullH - 2)}});

    int gangX = hx + hullW / 2;  // gangway from the deck onto the quay
    for (int y = quayY + 1; y <= hy; ++y) {
        g.Set(gangX, y, Tile::Bridge);
        g.Set(gangX + 1, y, Tile::Bridge);
    }

    // Drop any area the planner named after the vessel - this generator already
    // built the ship, and a warehouse labelled "Moored Ship" reads as a bug.
    std::vector<RoomSpec> landSpecs;
    for (const auto& r : spec.rooms) {
        std::string hay = Lower(r.label + "_" + r.id);
        bool isShip = false;
        for (const char* w : {"ship", "vessel", "boat", "galleon", "hull", "deck"})
            if (hay.find(w) != std::string::npos) isShip = true;
        if (!isShip) landSpecs.push_back(r);
        if (landSpecs.size() >= 4) break;
    }

    int buildH = quayY - 2;
    if (buildH >= 5 && !landSpecs.empty()) {
        size_t count = landSpecs.size();
        auto leaves = BspSplit({1, 1, g.cols - 2, buildH}, (int)count, rng,
                               Clampi(g.cols / 5, 5, 10));
        for (size_t i = 0; i < leaves.size() && i < count; ++i) {
            Rect L = leaves[i];
            int bw = L.w - 2, bh = L.h - 2;
            if (L.w < 5 || L.h < 5) continue;
            g.FillRect(L.x + 1, L.y + 1, bw, bh, Tile::Void);
            g.FillRect(L.x + 2, L.y + 2, bw - 2, bh - 2, Tile::Floor);
            rooms.push_back({landSpecs[i], Rect{L.x + 2, L.y + 2,
                                                std::max(2, bw - 2), std::max(2, bh - 2)}});
            g.Set(L.x + 1 + bw / 2, L.y + bh, Tile::Door);
        }
    }
    // The dock itself is an area, otherwise it renders as blank background.
    int dockTop = std::max(1, quayY - 3);
    rooms.push_back({RoomSpec{"quay", "Quay", "", 'l', "none",
                              {"crate", "barrel", "cart", "rope_coil", "net", "crate", "barrel"},
                              false, 0, 0, 0, 0},
                     Rect{1, dockTop, g.cols - 2, quayY - dockTop}});
    return rooms;
}

inline RoomList GenCustom(TileGrid& g, const DesignSpec& spec, Rng& rng, PathList& paths) {
    RoomList rooms;
    for (const auto& r : spec.rooms) {
        if (!r.hasRect) continue;
        int x = Clampi(r.x, 0, g.cols - 2), y = Clampi(r.y, 0, g.rows - 2);
        int w = Clampi(r.w, 2, g.cols - x), h = Clampi(r.h, 2, g.rows - y);
        g.FillRect(x, y, w, h, Tile::Floor);
        rooms.push_back({r, Rect{x, y, w, h}});
    }
    if (rooms.empty()) return GenDungeon(g, spec, rng, paths);
    paths = ConnectRooms(g, rooms, rng, false);
    return rooms;
}

// -- terrain, connectivity, props -------------------------------------

inline void ApplyTerrain(TileGrid& g, const RoomList& rooms, const DesignSpec& spec, Rng& rng) {
    auto normalise = [](std::string k) -> Tile {
        k = Lower(k);
        if (k == "lava" || k == "magma" || k == "acid" || k == "blood" || k == "sea" ||
            k == "river" || k == "pool")
            return Tile::Water;   // liquids all render alike; the style colours them
        if (k == "water") return Tile::Water;
        if (k == "pit") return Tile::Pit;
        if (k == "rubble") return Tile::Rubble;
        if (k == "vegetation") return Tile::Vegetation;
        return Tile::Void;        // "none"
    };

    Tile kind = normalise(spec.terrain_kind);
    // The harbour lays down its own ocean; a second global water terrain on top
    // carves a river straight through the quay and the warehouses.
    if (spec.layout == "harbour" && kind == Tile::Water) kind = Tile::Void;
    float amount = AmountScale(spec.terrain_amount);
    std::string shape = Lower(spec.terrain_shape);
    std::set<Tile> onFloor = {Tile::Floor};

    if (kind != Tile::Void && amount > 0.0f) {
        int span = std::min(g.cols, g.rows);
        if (shape == "river" || shape == "stream" || shape == "channel") {
            int py = g.rows / 2 + rng.Int(-2, 2);
            int width = Clampi((int)std::lround(span * 0.045f * amount), 0, 3);
            float phase = rng.Float(0.0f, 6.28f);
            int prev = INT32_MIN;
            for (int x = 0; x < g.cols; ++x) {
                int cy = py + (int)std::lround(std::sin(x / 4.5f + phase) * 1.5f);
                int lo = (prev == INT32_MIN) ? cy : std::min(prev, cy);
                int hi = (prev == INT32_MIN) ? cy : std::max(prev, cy);
                for (int yy = lo - width; yy <= hi + width; ++yy)
                    if (g.Get(x, yy) == Tile::Floor) g.Set(x, yy, kind);
                prev = cy;
            }
        } else {
            int count = Clampi((int)std::lround(2 * amount) + (int)rooms.size() / 3, 1, 6);
            for (int i = 0; i < count; ++i) {
                int cx, cy;
                if (!rooms.empty() && rng.Chance(0.75f)) {
                    const Rect& r = rooms[(size_t)rng.Int(0, (int)rooms.size() - 1)].second;
                    cx = r.x + rng.Int(0, std::max(0, r.w - 1));
                    cy = r.y + rng.Int(0, std::max(0, r.h - 1));
                } else {
                    cx = rng.Int(2, std::max(2, g.cols - 3));
                    cy = rng.Int(2, std::max(2, g.rows - 3));
                }
                float radius = Clampf(span * 0.09f * amount * rng.Float(0.7f, 1.4f), 1.2f, 6.0f);
                Blob(g, (float)cx, (float)cy, radius, kind, rng, &onFloor, rng.Float(0.8f, 1.6f));
            }
        }
    }
    for (const auto& rp : rooms) {
        Tile rk = normalise(rp.first.terrain);
        if (rk == Tile::Void) continue;
        const Rect& r = rp.second;
        Blob(g, r.x + r.w / 2.0f, r.y + r.h / 2.0f, std::min(r.w, r.h) * 0.42f, rk, rng,
             &onFloor, (float)r.w / std::max(1.0f, (float)r.h));
    }
}

// Words that name a built interior. On an open-air site everything else is
// outdoors, and outdoors does not have walls round it. Mirrors architect.py.
inline bool RoomIsBuilt(const RoomSpec& room) {
    static const std::vector<std::string> built = {
        "house", "hut", "cottage", "cabin", "barn", "shed", "stable", "granary",
        "mill", "forge", "smithy", "workshop", "warehouse", "store", "storeroom",
        "shop", "inn", "tavern", "hall", "keep", "tower", "turret", "gatehouse",
        "chapel", "church", "temple", "shrine", "crypt", "vault", "cellar",
        "room", "chamber", "kitchen", "bedroom", "study", "library", "office",
        "guardhouse", "barracks", "lodge", "manor", "villa", "interior"};
    static const std::vector<std::string> open = {
        "street", "square", "yard", "courtyard", "market", "plaza", "green",
        "field", "meadow", "clearing", "glade", "shore", "bank", "beach", "path",
        "road", "track", "lane", "terrace", "garden", "grove", "camp", "dock",
        "quay", "pier", "bridge", "ford", "crossing", "floor", "ground", "mire",
        "bog", "marsh", "gorge", "pass", "ravine", "plain", "slope", "rise"};
    auto has = [](const std::string& hay, const std::vector<std::string>& needles) {
        for (const std::string& n : needles)
            if (hay.find(n) != std::string::npos) return true;
        return false;
    };
    if (room.enclosed >= 0) return room.enclosed != 0;
    std::string label = Lower(room.label);
    std::string all = label + " " + Lower(room.description);
    if (has(label, open)) return false;
    if (has(label, built)) return true;
    if (has(all, open)) return false;
    return has(all, built);
}

// Join the outdoor rooms of an open-air site into one continuous surface.
// Walls are derived from where floor meets nothing, so two rooms with a gap
// between them get a wall each and a door between them. Outdoors there is no
// gap: the square runs into the street.
inline void OpenUpOutdoorRooms(TileGrid& g,
                               const std::vector<std::pair<RoomSpec, Rect>>& rooms,
                               const std::string& enclosure) {
    if (enclosure != "open") return;
    std::vector<Rect> outdoor, built;
    for (const auto& rp : rooms)
        (RoomIsBuilt(rp.first) ? built : outdoor).push_back(rp.second);
    if (outdoor.size() < 2) return;
    int x0 = g.cols, y0 = g.rows, x1 = 0, y1 = 0;
    for (const Rect& r : outdoor) {
        x0 = std::min(x0, r.x);        y0 = std::min(y0, r.y);
        x1 = std::max(x1, r.x + r.w);  y1 = std::max(y1, r.y + r.h);
    }
    std::set<std::pair<int, int>> keep;
    for (const Rect& r : built)
        for (int yy = r.y - 1; yy <= r.y + r.h; ++yy)
            for (int xx = r.x - 1; xx <= r.x + r.w; ++xx) keep.insert({xx, yy});
    for (int yy = y0; yy < y1; ++yy)
        for (int xx = x0; xx < x1; ++xx)
            if (!keep.count({xx, yy}) && g.Get(xx, yy) == Tile::Void)
                g.Set(xx, yy, Tile::Floor);
}

// --- What closes a site in -------------------------------------------------
// A gorge is not a building and a clearing has no masonry, but until this
// existed every map was described to the renderer as though it were a walled
// house: cliffs became courses of dressed stone and the way in became a timber
// door with iron bands. Mirrors architect.enclosure_of exactly.
inline std::string EnclosureOf(const std::string& declared, const std::string& category,
                               const std::string& layout, const std::string& defaultLayout) {
    auto low = [](std::string v) {
        for (auto& c : v) c = (char)std::tolower((unsigned char)c);
        while (!v.empty() && (v.front() == ' ')) v.erase(v.begin());
        while (!v.empty() && (v.back() == ' ')) v.pop_back();
        return v;
    };
    std::string d = low(declared);
    if (d == "masonry" || d == "rock" || d == "timber" || d == "open") return d;
    std::string L = low(layout);
    if (L.empty()) L = low(defaultLayout);
    if (L == "building" || L == "dungeon" || L == "arena") return "masonry";
    if (L == "cavern") return "rock";
    if (L == "deck") return "timber";
    if (L == "open" || L == "forest" || L == "swamp" || L == "ruins" ||
        L == "street" || L == "district" || L == "harbour") return "open";
    std::string C = low(category);
    if (C == "cavern" || C == "underground") return "rock";
    if (C == "nautical") return "timber";
    if (C == "wilderness" || C == "settlement" || C == "urban") return "open";
    return "masonry";
}

// A battle map you cannot walk across is a bug, not a feature.
// Walkable regions with no door and no way off the edge of the map. A cave
// that runs off the frame needs nothing; a walled room does.
inline std::vector<std::vector<std::pair<int, int>>> SealedRegions(const TileGrid& g,
                                                                  int margin = 0) {
    std::vector<char> seen((size_t)g.cols * g.rows, 0);
    std::vector<std::vector<std::pair<int, int>>> sealed;
    for (int sy = 0; sy < g.rows; ++sy) {
        for (int sx = 0; sx < g.cols; ++sx) {
            if (seen[(size_t)sy * g.cols + sx] || !IsWalkable(g.Get(sx, sy))) continue;
            std::vector<std::pair<int, int>> stack{{sx, sy}}, cells;
            seen[(size_t)sy * g.cols + sx] = 1;
            bool touchesEdge = false, hasOpening = false;
            while (!stack.empty()) {
                auto [x, y] = stack.back();
                stack.pop_back();
                cells.push_back({x, y});
                // The playable field, not the stored grid: every map carries an
                // empty bleed margin, so ground running off the field is ground
                // running off the map.
                if (x <= margin || y <= margin || x >= g.cols - 1 - margin ||
                    y >= g.rows - 1 - margin) touchesEdge = true;
                Tile here = g.Get(x, y);
                // A door only counts as a way in if it leads out of this
                // region. Every door in a fortress is a door, but if all of
                // them are between one chamber and the next then the fortress
                // has no entrance, and nothing used to notice.
                if (here == Tile::Door || here == Tile::Window) {
                    const int od[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                    for (auto& q : od) {
                        int ax = x + q[0], ay = y + q[1];
                        if (!g.Inside(ax, ay) || g.Get(ax, ay) == Tile::Void) {
                            hasOpening = true;
                            break;
                        }
                    }
                }
                const int d[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (auto& o : d) {
                    int nx = x + o[0], ny = y + o[1];
                    if (nx < 0 || ny < 0 || nx >= g.cols || ny >= g.rows) continue;
                    if (seen[(size_t)ny * g.cols + nx] || !IsWalkable(g.Get(nx, ny))) continue;
                    seen[(size_t)ny * g.cols + nx] = 1;
                    stack.push_back({nx, ny});
                }
            }
            if (!touchesEdge && !hasOpening && cells.size() >= 12) sealed.push_back(cells);
        }
    }
    return sealed;
}

inline void EnsureAWayIn(TileGrid& g, const std::string& enclosure) {
    for (int attempt = 0; attempt < 12; ++attempt) {
        auto sealed = SealedRegions(g);
        if (sealed.empty()) return;
        const auto& cells = sealed.front();
        std::set<std::pair<int, int>> inside(cells.begin(), cells.end());
        int bestScore = 99, bwx = -1, bwy = -1, box = -1, boy = -1, bgx = 0, bgy = 0;
        for (auto& c : cells) {
            const int d[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (auto& o : d) {
                int wx = c.first + o[0], wy = c.second + o[1];
                if (g.Get(wx, wy) != Tile::Wall) continue;
                int ox = wx + o[0], oy = wy + o[1];
                if (inside.count({ox, oy})) continue;
                int score;
                if (!g.Inside(ox, oy)) score = 2;            // opens off the map edge
                else if (IsWalkable(g.Get(ox, oy))) score = 0;
                else if (g.Get(ox, oy) == Tile::Void) score = 1;
                else continue;
                if (score < bestScore) {
                    bestScore = score;
                    bwx = wx; bwy = wy; box = ox; boy = oy;
                    bgx = o[0]; bgy = o[1];
                }
            }
        }
        if (bwx < 0) return;
        // Does this wall face the outside of the map, or another part of it?
        // A hut standing in a clearing is built and gets a door; the cliff
        // round the clearing is not and gets a gap. Walking outward from the
        // wall answers it: if nothing walkable lies that way, it is the
        // boundary.
        bool facesOutside = true;
        for (int cx = bwx, cy = bwy; g.Inside(cx, cy); cx += bgx, cy += bgy) {
            if (IsWalkable(g.Get(cx, cy)) && !inside.count({cx, cy})) {
                facesOutside = false;
                break;
            }
        }
        bool cutOpen = enclosure == "rock" || (enclosure == "open" && facesOutside);
        if (!cutOpen) {
            // Rock does not have doors in it; a built wall does.
            g.Set(bwx, bwy, Tile::Door);
            if (g.Inside(box, boy) && g.Get(box, boy) == Tile::Void) g.Set(box, boy, Tile::Floor);
            continue;
        }
        // A gap that stops inside the cliff is a gap to nowhere: outdoor maps
        // are entered from off the page, so the passage runs right off the edge.
        for (int cx = bwx, cy = bwy; g.Inside(cx, cy); cx += bgx, cy += bgy) {
            if (!IsWalkable(g.Get(cx, cy))) g.Set(cx, cy, Tile::Floor);
        }
    }
}

inline void EnsureConnected(TileGrid& g, Rng& rng) {
    std::set<Tile> walk;
    for (int i = 0; i < (int)Tile::COUNT; ++i)
        if (IsWalkable((Tile)i)) walk.insert((Tile)i);

    // Six passes, each joining a single cell, left a swamp with two dozen reed
    // beds mostly unreachable. Run until the map is whole.
    for (int attempt = 0; attempt < 60; ++attempt) {
        auto main = LargestComponent(g, walk);
        if (main.empty()) return;
        std::vector<std::pair<int, int>> stranded;
        for (int y = 0; y < g.rows; ++y)
            for (int x = 0; x < g.cols; ++x)
                if (walk.count(g.Get(x, y)) && !main.count({x, y})) stranded.push_back({x, y});
        if (stranded.empty()) return;

        // One stranded cell against the main region, not every pair: the
        // all-pairs search was the reason the pass budget had to be tiny.
        std::pair<int, int> a = stranded.front(), b{0, 0};
        int bestD = INT32_MAX;
        for (auto& mcell : main) {
            int d = std::abs(a.first - mcell.first) + std::abs(a.second - mcell.second);
            if (d < bestD) { bestD = d; b = mcell; }
        }
        for (auto& cell : CarveCorridor(g, a, b, rng)) {
            Tile t = g.Get(cell.first, cell.second);
            if (t == Tile::Water || t == Tile::Pit || t == Tile::Void)
                g.Set(cell.first, cell.second, Tile::Bridge);
            else if (t == Tile::Wall)
                // FixDoors runs afterwards and demotes this to a plain opening
                // if it turns out not to be seated in a wall run.
                g.Set(cell.first, cell.second, Tile::Door);
        }
    }
}

inline std::vector<std::pair<int, int>> PropSlots(const TileGrid& g, const Rect& r, bool wantWall) {
    std::vector<std::pair<int, int>> slots;
    for (int y = r.y - 1; y <= r.y + r.h; ++y) {
        for (int x = r.x - 1; x <= r.x + r.w; ++x) {
            Tile t = g.Get(x, y);
            // Outdoor layouts are mostly undergrowth, so restricting placement
            // to bare floor left a forest with three props on it.
            if (t != Tile::Floor && t != Tile::Bridge && t != Tile::Vegetation &&
                t != Tile::Rubble) continue;
            bool nearWall = false, nearDoor = false;
            const int dx[4] = {1, -1, 0, 0}, dy[4] = {0, 0, 1, -1};
            for (int i = 0; i < 4; ++i)
                if (g.Get(x + dx[i], y + dy[i]) == Tile::Wall) nearWall = true;
            for (int ddy = -1; ddy <= 1; ++ddy)
                for (int ddx = -1; ddx <= 1; ++ddx)
                    if (g.Get(x + ddx, y + ddy) == Tile::Door) nearDoor = true;
            if (nearDoor) continue;
            if (nearWall == wantWall) slots.push_back({x, y});
        }
    }
    return slots;
}

inline std::vector<Feature> PlaceProps(const TileGrid& g, const RoomList& rooms,
                                       const DesignSpec& spec, Rng& rng) {
    float density = AmountScale(spec.prop_density);
    const std::vector<std::string>& pool =
        spec.style_props.empty() ? DefaultFiller(spec.layout) : spec.style_props;

    std::set<std::pair<int, int>> used;
    std::vector<Feature> features;
    auto commit = [&](const std::string& kind, std::pair<int, int> cell,
                      bool filler = false) {
        if (used.count(cell)) return false;
        for (int dy = -1; dy <= 1; ++dy)      // keep props readable at table scale
            for (int dx = -1; dx <= 1; ++dx)
                if (used.count({cell.first + dx, cell.second + dy})) return false;
        used.insert(cell);
        Feature f;
        f.kind = kind;
        f.x = cell.first;
        f.y = cell.second;
        f.structural = IsStructuralProp(kind);
        f.filler = filler;
        features.push_back(f);
        return true;
    };

    for (const auto& rp : rooms) {
        const Rect& r = rp.second;
        std::vector<std::string> wanted;
        for (const auto& p : rp.first.props) {
            std::string k = NormalizeProp(p);
            if (!k.empty()) wanted.push_back(k);
        }
        int budget = (int)std::lround(std::max(1, r.w * r.h) / 22.0f * density);
        size_t askedFor = wanted.size();
        // A room that named its own props has said what belongs in it. The
        // style's list says what belongs in this kind of place in general, and
        // using it here is how an abandoned tavern common room came back with a
        // bed and three bookshelves in it - each of which the renderer then
        // built a little side room around.
        std::vector<std::string> roomPool;
        for (const std::string& k : wanted)
            if (std::find(roomPool.begin(), roomPool.end(), k) == roomPool.end())
                roomPool.push_back(k);
        if (roomPool.empty()) roomPool = pool;
        for (int i = 0; i < budget && !roomPool.empty(); ++i)
            wanted.push_back(rng.Pick(roomPool));

        auto wallSlots = PropSlots(g, r, true);
        auto openSlots = PropSlots(g, r, false);
        rng.Shuffle(wallSlots);
        rng.Shuffle(openSlots);
        auto centre = r.Center();
        // Toward the middle, but not in a queue: sorted strictly by distance
        // the props came out as a plus sign of five identical heaps around the
        // exact centre of the room, which is the repeating pattern the caption
        // spends its length warning against. Keys are drawn up front, because
        // a comparator that answers differently each time it is asked is not a
        // comparator.
        const float spread = std::max(6.0f, (r.w + r.h) * 0.35f);
        std::vector<std::pair<float, std::pair<int, int>>> ranked;
        ranked.reserve(openSlots.size());
        for (const auto& c : openSlots)
            ranked.push_back({(float)(std::abs(c.first - centre.first) +
                                      std::abs(c.second - centre.second)) +
                                  rng.Float(0.0f, spread), c});
        std::stable_sort(ranked.begin(), ranked.end(),
                         [](const auto& a, const auto& b) { return a.first < b.first; });
        for (size_t i = 0; i < ranked.size(); ++i) openSlots[i] = ranked[i].second;

        int placed = 0;
        size_t slot = 0;
        for (const auto& kind : wanted) {
            bool isFiller = slot++ >= askedFor;
            if (++placed > 14) break;
            bool prefersWall = IsWallProp(kind);
            std::vector<std::pair<int, int>>* pools[2] = {
                prefersWall ? &wallSlots : &openSlots, prefersWall ? &openSlots : &wallSlots};
            for (auto* p : pools) {
                bool done = false;
                for (size_t i = 0; i < p->size(); ++i) {
                    if (commit(kind, (*p)[i], isFiller)) {
                        p->erase(p->begin() + i);
                        done = true;
                        break;
                    }
                }
                if (done) break;
            }
        }
    }
    // Per-room budgets alone leave a big map almost bare - a 62x51 cavern came
    // out with three props - because "rooms" on organic layouts are only small
    // sample points. Top up against the actual walkable area.
    std::vector<std::pair<int, int>> walkable;
    for (int y = 0; y < g.rows; ++y)
        for (int x = 0; x < g.cols; ++x) {
            Tile t = g.Get(x, y);
            if (t == Tile::Floor || t == Tile::Bridge || t == Tile::Vegetation ||
                t == Tile::Rubble) walkable.push_back({x, y});
        }
    int target = Clampi((int)(walkable.size() / 26.0 * density), 0, 260);
    std::vector<std::string> askedKinds;
    for (const auto& rp : rooms)
        for (const auto& p : rp.first.props) {
            std::string k = NormalizeProp(p);
            if (!k.empty() &&
                std::find(askedKinds.begin(), askedKinds.end(), k) == askedKinds.end())
                askedKinds.push_back(k);
        }
    const std::vector<std::string>& topPool = askedKinds.empty() ? pool : askedKinds;
    if (!topPool.empty() && (int)features.size() < target) {
        rng.Shuffle(walkable);
        for (const auto& cell : walkable) {
            if ((int)features.size() >= target) break;
            commit(rng.Pick(topPool), cell, /*filler=*/true);
        }
    }
    return features;
}

// -- tiles <-> editable rectangles ------------------------------------

inline std::vector<Zone> ExtractZones(const TileGrid& g) {
    std::vector<Zone> zones;
    zones.push_back({"base", "void", 0, 0, g.cols, g.rows});
    std::vector<char> claimed((size_t)g.cols * g.rows, 0);
    std::map<std::string, int> counters;
    const auto& order = PaintOrder();
    for (size_t oi = 1; oi < order.size(); ++oi) {
        Tile kind = order[oi];
        for (int y = 0; y < g.rows; ++y) {
            for (int x = 0; x < g.cols; ++x) {
                if (claimed[(size_t)y * g.cols + x] || g.Get(x, y) != kind) continue;
                int w = 0;
                while (x + w < g.cols && !claimed[(size_t)y * g.cols + x + w] &&
                       g.Get(x + w, y) == kind)
                    ++w;
                int h = 1;
                while (y + h < g.rows) {
                    bool ok = true;
                    for (int i = 0; i < w && ok; ++i)
                        ok = !claimed[(size_t)(y + h) * g.cols + x + i] &&
                             g.Get(x + i, y + h) == kind;
                    if (!ok) break;
                    ++h;
                }
                for (int yy = y; yy < y + h; ++yy)
                    for (int xx = x; xx < x + w; ++xx) claimed[(size_t)yy * g.cols + xx] = 1;
                std::string name = TileName(kind);
                zones.push_back({name + "_" + std::to_string(++counters[name]), name, x, y, w, h});
            }
        }
    }
    return zones;
}

inline TileGrid ZonesToGrid(const MapData& map) {
    int cols = Clampi(map.grid.cols, 3, 200);
    int rows = Clampi(map.grid.rows, 3, 200);
    TileGrid g(cols, rows, Tile::Void);
    for (const auto& z : map.zones)
        g.FillRect(z.x, z.y, std::max(1, z.w), std::max(1, z.h), TileFromName(z.kind));
    return g;
}

// -- public entry point -----------------------------------------------

// An empty ring added outside the playable field, in cells. It is not part of
// the size the user picked - it is added on top of it. Image models are least
// reliable at the very edge of a frame, so a margin means the mess happens in
// blank space instead of eating the corner of a room; and a printed battle map
// looks like this anyway, its content inset from the paper edge.
constexpr int kBorderCells = 2;

// How wide this map's blank ring is.
inline int BorderOf(const MapData& map) { return std::max(0, map.meta.border); }

// Grow a finished map by an empty ring on every side. Calling it twice does
// nothing, so a map that already carries a border passes through unharmed.
inline void AddBorder(MapData& map, int cells) {
    int b = std::max(0, cells);
    if (b == 0 || map.meta.border > 0) return;
    map.grid.cols += 2 * b;
    map.grid.rows += 2 * b;
    for (auto& z : map.zones)       { z.x += b; z.y += b; }
    for (auto& f : map.features)    { f.x += b; f.y += b; }
    for (auto& a : map.areas)       { a.x += b; a.y += b; }
    for (auto& s : map.structures)  { s.x += b; s.y += b; }
    for (auto& a : map.annotations) { a.x += b; a.y += b; }
    for (auto& e : map.effects)     { e.x += b; e.y += b; }
    map.meta.border = b;
}

inline MapData Build(DesignSpec spec, uint32_t seed) {
    Rng rng(seed);
    if (spec.rooms.empty()) {
        spec.rooms = {
            {"main_hall", "Main Hall",
             "The largest space, worn smooth down the middle where people walk.", 'l',
             "none", {}, false, 0, 0, 0, 0},
            {"side_room", "Side Chamber",
             "A smaller room off the main one, its floor less worn.", 'm',
             "none", {}, false, 0, 0, 0, 0},
            {"back_room", "Back Chamber",
             "The room furthest from the entrance, dusty and little used.", 'm',
             "none", {}, false, 0, 0, 0, 0}};
    }
    spec.cols = Clampi(spec.cols, kMinCells, kMaxCells);
    spec.rows = Clampi(spec.rows, kMinCells, kMaxCells);

    TileGrid g(spec.cols, spec.rows, Tile::Void);
    PathList paths;
    RoomList rooms;
    std::vector<Structure> structures;
    const std::string& L = spec.layout;
    if (L == "building") rooms = GenBuilding(g, spec, rng, paths);
    else if (L == "cavern") rooms = GenCavern(g, spec, rng, paths);
    else if (L == "open") rooms = GenOpen(g, spec, rng, paths);
    else if (L == "forest") rooms = GenForest(g, spec, rng, paths);
    else if (L == "swamp") rooms = GenSwamp(g, spec, rng, paths);
    else if (L == "ruins") rooms = GenRuins(g, spec, rng, paths);
    else if (L == "deck") rooms = GenDeck(g, spec, rng, paths, structures);
    else if (L == "street") rooms = GenStreet(g, spec, rng, paths);
    else if (L == "district") rooms = GenDistrict(g, spec, rng, paths);
    else if (L == "arena") rooms = GenArena(g, spec, rng, paths);
    else if (L == "harbour") rooms = GenHarbour(g, spec, rng, paths, structures);
    else if (L == "custom") rooms = GenCustom(g, spec, rng, paths);
    else rooms = GenDungeon(g, spec, rng, paths);

    OpenUpOutdoorRooms(g, rooms, EnclosureOf(spec.style_enclosure, spec.style_category,
                                             L, ""));

    ApplyTerrain(g, rooms, spec, rng);
    DeriveWalls(g);
    if (!paths.empty()) PlaceDoors(g, rooms, paths);
    EnsureConnected(g, rng);
    DeriveWalls(g);
    if (!spec.edge_walls) OpenEdges(g);
    EnsureAWayIn(g, EnclosureOf(spec.style_enclosure, spec.style_category, L, ""));
    FixDoors(g);

    MapData map;
    map.meta.name = spec.name;
    map.meta.title = spec.title;
    map.meta.style = spec.style;
    map.meta.layout = spec.layout;
    map.meta.scene_summary = spec.scene_summary;
    map.meta.render_details = spec.render_details;
    // The scene's own lighting. Carried this far and then dropped, so a
    // description that said "lit only by the fire" was painted in whatever the
    // style felt like.
    map.meta.lighting = spec.lighting;
    map.meta.terrain_kind = spec.terrain_kind;
    map.meta.terrain_amount = spec.terrain_amount;
    map.meta.prop_density = spec.prop_density;
    map.meta.seed = (int64_t)seed;
    map.grid.cols = g.cols;
    map.grid.rows = g.rows;
    map.grid.cell_px = 32;
    map.zones = ExtractZones(g);
    map.structures = structures;
    map.features = PlaceProps(g, rooms, spec, rng);
    for (const auto& rp : rooms)
        map.areas.push_back({rp.first.id, rp.first.label, rp.first.description,
                             rp.second.x, rp.second.y, rp.second.w, rp.second.h});
    // The blank ring goes on last, so every generator above works in plain
    // 0..cols coordinates and knows nothing about it.
    AddBorder(map, Clampi(spec.border, 0, 8));
    return map;
}

inline void ValidateMap(MapData& map, std::vector<std::string>* problems = nullptr) {
    auto note = [&](const std::string& m) { if (problems) problems->push_back(m); };
    map.grid.cols = Clampi(map.grid.cols, kMinCells, kMaxCells + 2 * 8);
    map.grid.rows = Clampi(map.grid.rows, kMinCells, kMaxCells + 2 * 8);
    map.meta.border = Clampi(map.meta.border, 0, 8);
    map.grid.cell_px = Clampi(map.grid.cell_px, 8, 128);

    std::vector<Zone> clean;
    for (auto z : map.zones) {
        z.x = Clampi(z.x, 0, map.grid.cols - 1);
        z.y = Clampi(z.y, 0, map.grid.rows - 1);
        z.w = Clampi(z.w, 1, map.grid.cols - z.x);
        z.h = Clampi(z.h, 1, map.grid.rows - z.y);
        z.kind = TileName(TileFromName(z.kind));
        clean.push_back(z);
    }
    if (clean.empty()) {
        note("no zones - inserting an empty room");
        clean.push_back({"base", "void", 0, 0, map.grid.cols, map.grid.rows});
        clean.push_back({"floor_1", "floor", 1, 1, map.grid.cols - 2, map.grid.rows - 2});
    } else if (clean[0].kind != "void" || clean[0].w < map.grid.cols ||
               clean[0].h < map.grid.rows) {
        clean.insert(clean.begin(), {"base", "void", 0, 0, map.grid.cols, map.grid.rows});
    }
    map.zones = clean;

    for (auto& f : map.features) {
        f.x = Clampi(f.x, 0, map.grid.cols - 1);
        f.y = Clampi(f.y, 0, map.grid.rows - 1);
    }
    for (auto& st : map.structures) {
        st.x = Clampi(st.x, 0, map.grid.cols - 1);
        st.y = Clampi(st.y, 0, map.grid.rows - 1);
        st.w = Clampi(st.w, 1, map.grid.cols - st.x);
        st.h = Clampi(st.h, 1, map.grid.rows - st.y);
    }
    TileGrid g = ZonesToGrid(map);
    int walkable = 0;
    for (Tile t : g.cells) if (IsWalkable(t)) ++walkable;
    if (walkable < 12) note("map has almost no walkable space");
}

}  // namespace arch
}  // namespace dnd

