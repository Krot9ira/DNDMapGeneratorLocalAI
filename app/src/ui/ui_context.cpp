#include "ui_context.h"
#include "../../include/ollama_service.h"
#include <stb_image.h>
#include <stb_image_write.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

namespace dnd {

const char* kLayoutNames[] = {
    "(from style)", "dungeon", "building", "cavern",
    "open", "forest", "swamp", "ruins", "deck",
    "street", "district", "arena", "harbour"
};

const char* kTerrainNames[] = {"none", "water", "pit", "rubble", "vegetation"};
const char* kAmountNames[] = {"low", "medium", "high"};

const char* kPaintTiles[] = {
    "floor", "wall", "door", "window", "water", "pit",
    "rubble", "vegetation", "bridge", "stairs", "void"
};

const char* kTileHints[] = {
    "Walkable ground.",
    "Solid wall. Blocks movement and sight.",
    "Door. Only valid inside a wall - elsewhere it becomes a plain opening.",
    "Window. A glazed or shuttered opening in a wall. Lets light and sight "
    "through, but not a body.",
    "Water. Difficult or impassable, depending on your table.",
    "Pit or drop.",
    "Loose rubble. Difficult ground.",
    "Undergrowth. Light cover.",
    "Timber walkway or ship decking.",
    "Stairs.",
    "Eraser: clears back to empty space."
};

const EffectInfo kEffects[] = {
    {"fire", "Fire", "Leaping flames throwing light on the ground.", IM_COL32(255, 140, 40, 90)},
    {"embers", "Embers", "Glowing coals with sparks rising.", IM_COL32(255, 90, 30, 80)},
    {"smoke", "Smoke", "Thick grey smoke curling upward.", IM_COL32(120, 120, 125, 95)},
    {"fog", "Fog", "Low bank of pale drifting fog.", IM_COL32(200, 205, 215, 90)},
    {"mist", "Mist", "Thin silver mist clinging low.", IM_COL32(190, 210, 220, 70)},
    {"fireflies", "Fireflies", "Tiny warm points of light in the air.", IM_COL32(220, 255, 120, 70)},
    {"magic_glow", "Arcane", "Soft violet arcane glow.", IM_COL32(170, 110, 255, 85)},
    {"holy_light", "Holy light", "Shaft of pale golden light from above.", IM_COL32(255, 230, 150, 85)},
    {"poison_gas", "Poison", "Sickly yellow-green vapour lying low.", IM_COL32(150, 220, 90, 90)},
    {"blood", "Blood", "Dark red blood pooled and smeared.", IM_COL32(160, 30, 35, 95)},
    {"ice", "Ice", "Sheet of pale blue ice with frost.", IM_COL32(150, 210, 250, 85)},
    {"webs", "Webs", "Sheets of dusty grey spider web.", IM_COL32(225, 225, 230, 80)},
    {"sparks", "Sparks", "Bright white sparks arcing.", IM_COL32(255, 250, 200, 75)},
    {"ash", "Ash", "Grey ash settled in drifts.", IM_COL32(140, 138, 132, 85)},
    {"steam", "Steam", "White steam venting in soft billows.", IM_COL32(230, 235, 240, 80)},
    {"shadow", "Shadow", "Unnatural pool of deep shadow.", IM_COL32(20, 18, 30, 120)},
};
const size_t kEffectsCount = sizeof(kEffects) / sizeof(kEffects[0]);

ImU32 EffectTint(const std::string& kind) {
    for (size_t i = 0; i < kEffectsCount; ++i) {
        if (kind == kEffects[i].kind) return kEffects[i].tint;
    }
    return IM_COL32(200, 160, 255, 80);
}

const char* EffectLabel(const std::string& kind) {
    for (size_t i = 0; i < kEffectsCount; ++i) {
        if (kind == kEffects[i].kind) return kEffects[i].label;
    }
    return kind.c_str();
}

const PropInfo kProps[] = {
    {"barrel", "Barrel", "Wooden barrel with iron bands. Blocks movement."},
    {"crate", "Crate", "Stacked cargo crate. Blocks movement."},
    {"chest", "Chest", "Lidded storage chest. Loot container."},
    {"table", "Table", "Long timber table. Half cover."},
    {"chair", "Chair", "Single seat."},
    {"bench", "Bench", "Low timber bench."},
    {"bed", "Bed", "Straw mattress on a frame."},
    {"bookshelf", "Shelf", "Tall shelf packed with ledgers. Full cover."},
    {"bar", "Bar", "Polished bar counter."},
    {"desk", "Desk", "Writing desk covered in papers."},
    {"cabinet", "Cabinet", "Closed storage cabinet."},
    {"torch", "Torch", "Wall torch. Light source."},
    {"lamp", "Lamp", "Hanging lantern. Light source."},
    {"brazier", "Brazier", "Iron brazier of live coals. Light and fire."},
    {"campfire", "Campfire", "Ring of stones round a fire."},
    {"hearth", "Hearth", "Stone fireplace with burning logs."},
    {"forge", "Forge", "Stone forge glowing with coals."},
    {"cauldron", "Cauldron", "Iron cauldron on a tripod."},
    {"anvil", "Anvil", "Black iron anvil on a stump."},
    {"pillar", "Pillar", "Thick stone column. Blocks line of sight."},
    {"statue", "Statue", "Carved figure on a plinth. Full cover."},
    {"altar", "Altar", "Carved stone altar block."},
    {"sarcophagus", "Sarcophagus", "Heavy stone coffin with a chipped lid."},
    {"bones", "Bones", "Heap of old bones. Difficult ground."},
    {"throne", "Throne", "High-backed carved seat."},
    {"well", "Well", "Round stone well with a winch."},
    {"fountain", "Fountain", "Carved fountain basin."},
    {"portal", "Arch", "Standing stone archway."},
    {"tree", "Tree", "Broad canopy seen from above. Full cover."},
    {"bush", "Bush", "Low shrub. Light cover."},
    {"stump", "Stump", "Cut tree stump."},
    {"boulder", "Boulder", "Moss-covered rock. Blocks movement."},
    {"stalagmite", "Stalagmite", "Jagged rock spire."},
    {"crystal", "Crystal", "Cluster of glowing shards. Light source."},
    {"mushroom", "Mushroom", "Oversized cave fungus."},
    {"cart", "Cart", "Two-wheeled handcart."},
    {"wagon", "Wagon", "Four-wheeled timber wagon."},
    {"rope_coil", "Rope", "Coiled rope on the ground."},
    {"net", "Net", "Fishing net spread out."},
    {"bollard", "Bollard", "Iron mooring post."},
    {"capstan", "Capstan", "Timber capstan with bars."},
    {"mast", "Mast", "Base of a mast with rigging."},
    {"console", "Console", "Wall console with cracked screens."},
    {"locker", "Locker", "Steel storage locker."},
    {"dumpster", "Dumpster", "Dented steel dumpster."},
    {"weapon_rack", "Weapons", "Rack of spears and blades."},
    {"banner", "Banner", "Hanging cloth banner."},
};
const size_t kPropsCount = sizeof(kProps) / sizeof(kProps[0]);

const char* PropLabel(const std::string& kind) {
    for (size_t i = 0; i < kPropsCount; ++i) {
        if (kind == kProps[i].kind) return kProps[i].label;
    }
    return kind.c_str();
}

ImU32 TileColor(Tile t) {
    switch (t) {
    case Tile::Void: return IM_COL32(28, 30, 36, 255);
    case Tile::Floor: return IM_COL32(232, 226, 214, 255);
    case Tile::Wall: return IM_COL32(58, 62, 72, 255);
    case Tile::Door: return IM_COL32(196, 150, 74, 255);
    case Tile::Window: return IM_COL32(150, 196, 214, 255);
    case Tile::Water: return IM_COL32(86, 140, 178, 255);
    case Tile::Pit: return IM_COL32(34, 34, 40, 255);
    case Tile::Rubble: return IM_COL32(176, 166, 150, 255);
    case Tile::Vegetation: return IM_COL32(118, 156, 104, 255);
    case Tile::Stairs: return IM_COL32(198, 190, 176, 255);
    case Tile::Bridge: return IM_COL32(166, 130, 88, 255);
    default: return IM_COL32(120, 120, 120, 255);
    }
}

float PanelWidth(float fraction, float minimum, float maximum) {
    float avail = ImGui::GetContentRegionAvail().x;
    return std::clamp(avail * fraction, minimum, std::min(maximum, avail * 0.6f));
}

void GridMetrics(float minCell, int& outPerRow, float& outCellW) {
    float avail = ImGui::GetContentRegionAvail().x;
    outPerRow = std::max(1, (int)(avail / minCell));
    outCellW = avail / outPerRow;
}

std::string FitText(const std::string& text, float maxWidth) {
    if (ImGui::CalcTextSize(text.c_str()).x <= maxWidth) return text;
    std::string out = text;
    while (out.size() > 4 &&
           ImGui::CalcTextSize((out + "...").c_str()).x > maxWidth) {
        out.pop_back();
    }
    return out + "...";
}

void DrawTileGlyph(ImDrawList* dl, ImVec2 c, float r, Tile t, ImU32 col) {
    float w = std::max(1.5f, r * 0.18f);
    switch (t) {
    case Tile::Wall:
        for (int i = 0; i < 3; ++i) {
            float y = c.y - r * 0.6f + i * r * 0.6f;
            dl->AddLine(ImVec2(c.x - r, y), ImVec2(c.x + r, y), col, w);
        }
        dl->AddLine(ImVec2(c.x - r * 0.4f, c.y - r * 0.6f),
                    ImVec2(c.x - r * 0.4f, c.y), col, w);
        dl->AddLine(ImVec2(c.x + r * 0.4f, c.y), ImVec2(c.x + r * 0.4f, c.y + r * 0.6f), col, w);
        break;
    case Tile::Door:
        dl->AddRect(ImVec2(c.x - r * 0.55f, c.y - r * 0.8f),
                    ImVec2(c.x + r * 0.55f, c.y + r * 0.8f), col, 0, 0, w);
        dl->AddCircleFilled(ImVec2(c.x + r * 0.3f, c.y), r * 0.13f, col);
        break;
    case Tile::Window:
        dl->AddRect(ImVec2(c.x - r * 0.75f, c.y - r * 0.62f),
                    ImVec2(c.x + r * 0.75f, c.y + r * 0.62f), col, 0, 0, w);
        dl->AddLine(ImVec2(c.x, c.y - r * 0.62f), ImVec2(c.x, c.y + r * 0.62f), col, w);
        dl->AddLine(ImVec2(c.x - r * 0.75f, c.y), ImVec2(c.x + r * 0.75f, c.y), col, w);
        break;
    case Tile::Water:
        for (int i = 0; i < 2; ++i) {
            float y = c.y - r * 0.3f + i * r * 0.6f;
            for (int k = 0; k < 3; ++k) {
                float x0 = c.x - r + k * r * 0.7f;
                dl->AddBezierQuadratic(ImVec2(x0, y), ImVec2(x0 + r * 0.35f, y - r * 0.35f),
                                       ImVec2(x0 + r * 0.7f, y), col, w, 8);
            }
        }
        break;
    case Tile::Pit:
        dl->AddCircle(c, r * 0.8f, col, 16, w);
        for (int i = -1; i <= 1; ++i)
            dl->AddLine(ImVec2(c.x + i * r * 0.4f - r * 0.3f, c.y + r * 0.6f),
                        ImVec2(c.x + i * r * 0.4f + r * 0.3f, c.y - r * 0.6f), col, w * 0.8f);
        break;
    case Tile::Rubble:
        dl->AddCircleFilled(ImVec2(c.x - r * 0.45f, c.y + r * 0.2f), r * 0.2f, col);
        dl->AddCircleFilled(ImVec2(c.x + r * 0.1f, c.y - r * 0.35f), r * 0.16f, col);
        dl->AddCircleFilled(ImVec2(c.x + r * 0.5f, c.y + r * 0.35f), r * 0.22f, col);
        break;
    case Tile::Vegetation:
        for (int i = -1; i <= 1; ++i) {
            float x = c.x + i * r * 0.55f;
            dl->AddLine(ImVec2(x, c.y + r * 0.6f), ImVec2(x, c.y - r * 0.2f), col, w);
            dl->AddLine(ImVec2(x, c.y - r * 0.2f), ImVec2(x - r * 0.3f, c.y - r * 0.6f), col, w);
            dl->AddLine(ImVec2(x, c.y - r * 0.2f), ImVec2(x + r * 0.3f, c.y - r * 0.6f), col, w);
        }
        break;
    case Tile::Bridge:
        for (int i = 0; i < 4; ++i) {
            float y = c.y - r * 0.6f + i * r * 0.4f;
            dl->AddLine(ImVec2(c.x - r, y), ImVec2(c.x + r, y), col, w);
        }
        break;
    case Tile::Stairs:
        for (int i = 0; i < 4; ++i) {
            float y = c.y - r * 0.7f + i * r * 0.45f;
            float x = c.x - r + i * r * 0.4f;
            dl->AddLine(ImVec2(x, y), ImVec2(c.x + r, y), col, w);
        }
        break;
    case Tile::Void:
        dl->AddRect(ImVec2(c.x - r * 0.8f, c.y - r * 0.5f),
                    ImVec2(c.x + r * 0.8f, c.y + r * 0.5f), col, 0, 0, w);
        dl->AddLine(ImVec2(c.x - r * 0.8f, c.y + r * 0.5f),
                    ImVec2(c.x + r * 0.8f, c.y - r * 0.5f), col, w);
        break;
    default:
        dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), col, w);
        dl->AddLine(ImVec2(c.x - r * 0.35f, c.y - r * 0.7f),
                    ImVec2(c.x - r * 0.35f, c.y), col, w);
        dl->AddLine(ImVec2(c.x + r * 0.35f, c.y), ImVec2(c.x + r * 0.35f, c.y + r * 0.7f), col, w);
        break;
    }
}

void DrawPropGlyph(ImDrawList* dl, ImVec2 c, float r, const char* kind, ImU32 col) {
    std::string k = kind;
    float t = std::max(1.5f, r * 0.16f);
    auto ring = [&](float rad) { dl->AddCircle(c, rad, col, 16, t); };
    auto box = [&](float hw, float hh) {
        dl->AddRect(ImVec2(c.x - hw, c.y - hh), ImVec2(c.x + hw, c.y + hh), col, 0, 0, t);
    };
    if (k == "barrel" || k == "keg") { ring(r * 0.8f);
        dl->AddLine(ImVec2(c.x - r * 0.8f, c.y), ImVec2(c.x + r * 0.8f, c.y), col, t); }
    else if (k == "crate" || k == "chest" || k == "locker" || k == "cabinet" ||
             k == "dumpster") { box(r * 0.8f, r * 0.65f);
        dl->AddLine(ImVec2(c.x - r * 0.8f, c.y), ImVec2(c.x + r * 0.8f, c.y), col, t); }
    else if (k == "table" || k == "desk" || k == "bar" || k == "bench") box(r * 0.9f, r * 0.5f);
    else if (k == "chair") box(r * 0.4f, r * 0.4f);
    else if (k == "bed") { box(r * 0.55f, r * 0.9f);
        dl->AddLine(ImVec2(c.x - r * 0.55f, c.y - r * 0.4f),
                    ImVec2(c.x + r * 0.55f, c.y - r * 0.4f), col, t); }
    else if (k == "pillar" || k == "stalagmite" || k == "bollard" || k == "capstan" ||
             k == "mast") { ring(r * 0.8f); ring(r * 0.38f); }
    else if (k == "torch" || k == "lamp") { ring(r * 0.35f);
        dl->AddLine(ImVec2(c.x, c.y + r * 0.35f), ImVec2(c.x, c.y + r * 0.9f), col, t); }
    else if (k == "brazier" || k == "campfire" || k == "cauldron" || k == "hearth" ||
             k == "forge") { ring(r * 0.8f);
        for (int a = 0; a < 360; a += 60)
            dl->AddLine(c, ImVec2(c.x + r * 0.5f * cosf(a * 3.14159f / 180.0f),
                                  c.y + r * 0.5f * sinf(a * 3.14159f / 180.0f)), col, t); }
    else if (k == "well" || k == "fountain") { ring(r * 0.9f); ring(r * 0.45f); }
    else if (k == "bookshelf" || k == "weapon_rack") { box(r * 0.9f, r * 0.42f);
        for (int i = -1; i <= 1; ++i)
            dl->AddLine(ImVec2(c.x + i * r * 0.42f, c.y - r * 0.42f),
                        ImVec2(c.x + i * r * 0.42f, c.y + r * 0.42f), col, t * 0.7f); }
    else if (k == "statue" || k == "throne" || k == "portal")
        dl->AddTriangle(ImVec2(c.x, c.y - r * 0.85f), ImVec2(c.x + r * 0.7f, c.y + r * 0.6f),
                        ImVec2(c.x - r * 0.7f, c.y + r * 0.6f), col, t);
    else if (k == "crystal") {
        ImVec2 pts[4] = {ImVec2(c.x, c.y - r * 0.9f), ImVec2(c.x + r * 0.5f, c.y),
                         ImVec2(c.x, c.y + r * 0.9f), ImVec2(c.x - r * 0.5f, c.y)};
        dl->AddPolyline(pts, 4, col, ImDrawFlags_Closed, t);
    }
    else if (k == "altar" || k == "sarcophagus") { box(r * 0.85f, r * 0.55f);
        dl->AddLine(ImVec2(c.x, c.y - r * 0.4f), ImVec2(c.x, c.y + r * 0.4f), col, t); }
    else if (k == "tree" || k == "bush" || k == "mushroom") { ring(r * 0.9f); ring(r * 0.42f); }
    else if (k == "rope_coil" || k == "net") { ring(r * 0.85f); ring(r * 0.55f); ring(r * 0.25f); }
    else if (k == "cart" || k == "wagon" || k == "console") { box(r * 0.9f, r * 0.55f);
        dl->AddLine(ImVec2(c.x - r * 0.3f, c.y - r * 0.55f),
                    ImVec2(c.x - r * 0.3f, c.y + r * 0.55f), col, t * 0.7f); }
    else if (k == "bones") { ring(r * 0.35f);
        dl->AddLine(ImVec2(c.x - r * 0.8f, c.y + r * 0.5f),
                    ImVec2(c.x + r * 0.8f, c.y + r * 0.25f), col, t); }
    else { ring(r * 0.7f);
        dl->AddLine(ImVec2(c.x - r * 0.45f, c.y), ImVec2(c.x + r * 0.45f, c.y), col, t); }
}

void DrawEffectGlyph(ImDrawList* dl, ImVec2 c, float r, const char* kind, ImU32 col) {
    std::string k = kind;
    float t = std::max(1.5f, r * 0.16f);
    if (k == "fire" || k == "embers") {
        dl->AddTriangle(ImVec2(c.x, c.y - r), ImVec2(c.x + r * 0.7f, c.y + r * 0.7f),
                        ImVec2(c.x - r * 0.7f, c.y + r * 0.7f), col, t);
        dl->AddTriangleFilled(ImVec2(c.x, c.y - r * 0.3f), ImVec2(c.x + r * 0.3f, c.y + r * 0.6f),
                              ImVec2(c.x - r * 0.3f, c.y + r * 0.6f), col);
    } else if (k == "fog" || k == "mist" || k == "smoke" || k == "steam" || k == "ash") {
        for (int i = 0; i < 3; ++i) {
            float y = c.y - r * 0.5f + i * r * 0.5f;
            dl->AddBezierQuadratic(ImVec2(c.x - r, y), ImVec2(c.x, y - r * 0.5f),
                                   ImVec2(c.x + r, y), col, t, 10);
        }
    } else if (k == "fireflies" || k == "sparks") {
        const float px[5] = {-0.6f, 0.1f, 0.7f, -0.2f, 0.5f};
        const float py[5] = {-0.5f, -0.7f, -0.1f, 0.4f, 0.7f};
        for (int i = 0; i < 5; ++i)
            dl->AddCircleFilled(ImVec2(c.x + px[i] * r, c.y + py[i] * r), r * 0.15f, col);
    } else if (k == "magic_glow" || k == "holy_light") {
        dl->AddCircle(c, r * 0.4f, col, 14, t);
        for (int a = 0; a < 360; a += 45)
            dl->AddLine(ImVec2(c.x + r * 0.6f * cosf(a * 3.14159f / 180.0f),
                               c.y + r * 0.6f * sinf(a * 3.14159f / 180.0f)),
                        ImVec2(c.x + r * 0.95f * cosf(a * 3.14159f / 180.0f),
                               c.y + r * 0.95f * sinf(a * 3.14159f / 180.0f)), col, t);
    } else if (k == "poison_gas" || k == "shadow") {
        dl->AddCircle(c, r * 0.85f, col, 18, t);
        dl->AddCircle(ImVec2(c.x - r * 0.4f, c.y + r * 0.3f), r * 0.4f, col, 12, t);
    } else if (k == "ice") {
        for (int a = 0; a < 180; a += 60) {
            float rad = a * 3.14159f / 180.0f;
            dl->AddLine(ImVec2(c.x - r * cosf(rad), c.y - r * sinf(rad)),
                        ImVec2(c.x + r * cosf(rad), c.y + r * sinf(rad)), col, t);
        }
    } else if (k == "webs") {
        for (int a = 0; a < 360; a += 60)
            dl->AddLine(c, ImVec2(c.x + r * cosf(a * 3.14159f / 180.0f),
                                  c.y + r * sinf(a * 3.14159f / 180.0f)), col, t * 0.8f);
        dl->AddCircle(c, r * 0.5f, col, 12, t * 0.8f);
    } else if (k == "blood") {
        dl->AddCircleFilled(ImVec2(c.x - r * 0.2f, c.y + r * 0.2f), r * 0.5f, col);
        dl->AddCircleFilled(ImVec2(c.x + r * 0.45f, c.y - r * 0.35f), r * 0.22f, col);
    } else {
        dl->AddCircle(c, r * 0.7f, col, 16, t);
    }
}

void DrawToolGlyph(ImDrawList* dl, ImVec2 c, float r, int tool, ImU32 col) {
    float t = std::max(1.5f, r * 0.20f);
    switch (tool) {
    case 0:
        dl->AddLine(ImVec2(c.x - r, c.y), ImVec2(c.x + r, c.y), col, t);
        dl->AddLine(ImVec2(c.x, c.y - r), ImVec2(c.x, c.y + r), col, t);
        dl->AddTriangleFilled(ImVec2(c.x + r, c.y), ImVec2(c.x + r * 0.4f, c.y - r * 0.4f),
                              ImVec2(c.x + r * 0.4f, c.y + r * 0.4f), col);
        dl->AddTriangleFilled(ImVec2(c.x - r, c.y), ImVec2(c.x - r * 0.4f, c.y - r * 0.4f),
                              ImVec2(c.x - r * 0.4f, c.y + r * 0.4f), col);
        break;
    case 1:
        dl->AddLine(ImVec2(c.x - r * 0.7f, c.y + r * 0.7f), ImVec2(c.x + r * 0.3f, c.y - r * 0.3f),
                    col, t);
        dl->AddTriangleFilled(ImVec2(c.x + r * 0.15f, c.y - r * 0.45f),
                              ImVec2(c.x + r, c.y - r * 0.9f),
                              ImVec2(c.x + r * 0.6f, c.y + r * 0.1f), col);
        break;
    case 2:
        dl->AddRect(ImVec2(c.x - r * 0.85f, c.y - r * 0.6f),
                    ImVec2(c.x + r * 0.85f, c.y + r * 0.6f), col, 0, 0, t);
        break;
    case 3:
        dl->AddCircle(ImVec2(c.x - r * 0.2f, c.y), r * 0.6f, col, 14, t);
        dl->AddLine(ImVec2(c.x + r * 0.55f, c.y - r * 0.5f),
                    ImVec2(c.x + r * 0.55f, c.y + r * 0.1f), col, t);
        dl->AddLine(ImVec2(c.x + r * 0.25f, c.y - r * 0.2f),
                    ImVec2(c.x + r * 0.85f, c.y - r * 0.2f), col, t);
        break;
    case 4:
        dl->AddCircle(c, r * 0.7f, col, 14, t);
        dl->AddLine(ImVec2(c.x - r * 0.5f, c.y + r * 0.5f),
                    ImVec2(c.x + r * 0.5f, c.y - r * 0.5f), col, t);
        break;
    default: {
        dl->AddRect(ImVec2(c.x - r * 0.85f, c.y - r * 0.7f),
                    ImVec2(c.x + r * 0.5f, c.y + r * 0.7f), col, 0, 0, t);
        dl->AddLine(ImVec2(c.x - r * 0.5f, c.y - r * 0.25f),
                    ImVec2(c.x + r * 0.15f, c.y - r * 0.25f), col, t * 0.8f);
        dl->AddLine(ImVec2(c.x - r * 0.5f, c.y + r * 0.15f),
                    ImVec2(c.x + r * 0.15f, c.y + r * 0.15f), col, t * 0.8f);
        break;
    }
    }
}

bool IconButton(const char* id, const char* label, ImVec2 size,
                const std::function<void(ImDrawList*, ImVec2, float, ImU32)>& glyph,
                ImU32 glyphCol) {
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    bool pressed = ImGui::Button(id, size);
    ImVec2 actual = ImGui::GetItemRectSize();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float r = std::min(actual.y, 26.0f) * 0.32f;
    glyph(dl, ImVec2(p0.x + 8.0f + r, p0.y + actual.y * 0.5f), r, glyphCol);
    ImVec2 ts = ImGui::CalcTextSize(label);
    dl->AddText(ImVec2(p0.x + 16.0f + r * 2.0f, p0.y + (actual.y - ts.y) * 0.5f),
                ImGui::GetColorU32(ImGuiCol_Text), label);
    return pressed;
}

bool InputTextString(const char* label, std::string* str, ImGuiInputTextFlags flags) {
    char buf[1024];
    strncpy_s(buf, sizeof(buf), str->c_str(), _TRUNCATE);
    if (ImGui::InputText(label, buf, sizeof(buf), flags)) {
        *str = buf;
        return true;
    }
    return false;
}

bool InputTextMultilineString(const char* label, std::string* str, const ImVec2& size) {
    static std::vector<char> buf;
    buf.assign(str->begin(), str->end());
    buf.resize(std::max<size_t>(buf.size() + 1, 8192), '\0');
    if (ImGui::InputTextMultiline(label, buf.data(), buf.size(), size)) {
        *str = buf.data();
        return true;
    }
    return false;
}

void HelpMarker(const char* text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::BeginItemTooltip()) {
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 28.0f);
        ImGui::TextUnformatted(text);
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

void DrawJobLog(AppState& app, float height) {
    ImGui::BeginChild("##joblog", ImVec2(0, height), ImGuiChildFlags_Borders);
    std::lock_guard<std::mutex> lock(app.job.mtx);
    for (const auto& line : app.job.log) ImGui::TextWrapped("%s", line.c_str());
    if (!app.job.finishedMessage.empty()) {
        ImGui::Separator();
        ImVec4 col = app.job.failed ? ImVec4(1.0f, 0.45f, 0.40f, 1.0f)
                                    : ImVec4(0.45f, 0.90f, 0.55f, 1.0f);
        ImGui::TextColored(col, "%s", app.job.finishedMessage.c_str());
    }
    if (ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40.0f) ImGui::SetScrollHereY(1.0f);
    ImGui::EndChild();
}

ImU32 HexToCol(const std::string& hex, ImU32 fallback) {
    if (hex.size() < 7 || hex[0] != '#') return fallback;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    int v[6];
    for (int i = 0; i < 6; ++i) {
        v[i] = nib(hex[(size_t)i + 1]);
        if (v[i] < 0) return fallback;
    }
    return IM_COL32(v[0] * 16 + v[1], v[2] * 16 + v[3], v[4] * 16 + v[5], 255);
}

bool StyleBadge(const std::string& origin, const char** label, ImU32* fill, ImU32* text) {
    if (origin == "custom" || origin == "user") {
        *label = "Custom";
        *fill = IM_COL32(52, 98, 74, 255);
        *text = IM_COL32(230, 245, 235, 255);
        return true;
    }
    return false;
}

void DrawStylePicker(AppState& app) {
    int perRow = 1;
    float cellW = 152.0f;
    GridMetrics(152.0f, perRow, cellW);
    const float cellH = 64.0f;
    float gridH = std::clamp(ImGui::GetContentRegionAvail().y * 0.40f, 150.0f, 460.0f);

    ImGui::BeginChild("##stylepicker", ImVec2(0, gridH), ImGuiChildFlags_Borders);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    int i = 0;
    for (const auto& kv : app.styles.styles) {
        if (i % perRow != 0) ImGui::SameLine();
        const StyleDef& st = kv.second;
        bool active = app.selectedStyle == kv.first;

        ImGui::PushID(i++);
        ImVec2 p0 = ImGui::GetCursorScreenPos();
        if (ImGui::InvisibleButton("##s", ImVec2(cellW - 8.0f, cellH - 8.0f))) {
            app.selectedStyle = kv.first;
            app.map.meta.style = kv.first;
        }
        bool hovered = ImGui::IsItemHovered();
        const char* where = st.origin == "user"  ? "Your own style"
                                                 : "Shipped with the program";
        if (hovered && !st.description.empty())
            ImGui::SetTooltip("%s\n%s\n\n%s - styles/%s.json", st.name.c_str(),
                              st.description.c_str(), where, st.id.c_str());

        ImVec2 p1(p0.x + cellW - 8.0f, p0.y + cellH - 8.0f);
        dl->AddRectFilled(p0, p1, hovered ? IM_COL32(48, 52, 62, 255)
                                          : IM_COL32(32, 35, 42, 255), 4.0f);

        int swatches = (int)st.hex_palette.size();
        if (swatches > 0) {
            float w = (cellW - 16.0f) / swatches;
            for (int c = 0; c < swatches; ++c) {
                ImVec2 a(p0.x + 4.0f + c * w, p0.y + 4.0f);
                ImVec2 b(a.x + w - 1.0f, p0.y + 22.0f);
                dl->AddRectFilled(a, b, HexToCol(st.hex_palette[(size_t)c],
                                                 IM_COL32(120, 120, 120, 255)));
            }
        }
        if (active) dl->AddRect(p0, p1, IM_COL32(250, 200, 70, 255), 4.0f, 0, 2.5f);

        const char* badge = nullptr;
        ImU32 badgeFill = 0, badgeText = 0;
        float nameRoom = cellW - 20.0f;
        if (StyleBadge(st.origin, &badge, &badgeFill, &badgeText)) {
            ImVec2 size = ImGui::CalcTextSize(badge);
            ImVec2 b1(p1.x - 5.0f, p1.y - 5.0f);
            ImVec2 b0(b1.x - size.x - 8.0f, b1.y - size.y - 2.0f);
            dl->AddRectFilled(b0, b1, badgeFill, 3.0f);
            dl->AddText(ImVec2(b0.x + 4.0f, b0.y + 1.0f), badgeText, badge);
            nameRoom -= size.x + 12.0f;
        }

        std::string name = FitText(st.name, nameRoom);
        dl->AddText(ImVec2(p0.x + 6.0f, p0.y + 28.0f),
                    active ? IM_COL32(250, 214, 120, 255) : IM_COL32(214, 212, 206, 255),
                    name.c_str());
        ImGui::PopID();
    }
    ImGui::EndChild();
}

void SetupFonts() {
    ImGuiIO& io = ImGui::GetIO();
    const char* candidates[] = {
        "C:/Windows/Fonts/segoeui.ttf",
        "C:/Windows/Fonts/tahoma.ttf",
        "C:/Windows/Fonts/arial.ttf"
    };
    for (const char* path : candidates) {
        if (!fs::exists(path)) continue;
        ImFontConfig cfg;
        cfg.OversampleH = 2;
        cfg.OversampleV = 1;
        if (io.Fonts->AddFontFromFileTTF(path, 18.0f, &cfg, io.Fonts->GetGlyphRangesCyrillic()))
            return;
    }
    io.Fonts->AddFontDefault();
}

std::string OutputDir(const AppState& app, const std::string& name) {
    fs::path dir = fs::path(app.config.output_dir) / (name.empty() ? "map" : name);
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir.string();
}

void PaintBleedMargin(std::vector<uint8_t>& png, const MapData& map) {
    int border = arch::BorderOf(map);
    if (border <= 0 || map.grid.cols <= 0 || map.grid.rows <= 0 || png.empty()) return;

    int w = 0, h = 0, comp = 0;
    unsigned char* pixels = stbi_load_from_memory(png.data(), (int)png.size(), &w, &h, &comp, 3);
    if (!pixels || w <= 0 || h <= 0) {
        if (pixels) stbi_image_free(pixels);
        return;
    }

    int mx = std::max(1, (int)std::lround((double)w * border / map.grid.cols));
    int my = std::max(1, (int)std::lround((double)h * border / map.grid.rows));

    auto at = [&](int x, int y) { return pixels + ((size_t)y * w + x) * 3; };
    std::vector<std::array<int, 3>> ring;
    for (int x = 0; x < w; x += std::max(1, w / 120)) {
        unsigned char* a = at(x, std::min(my / 2, h - 1));
        unsigned char* b = at(x, std::max(0, h - 1 - my / 2));
        ring.push_back({a[0], a[1], a[2]});
        ring.push_back({b[0], b[1], b[2]});
    }
    for (int y = 0; y < h; y += std::max(1, h / 120)) {
        unsigned char* a = at(std::min(mx / 2, w - 1), y);
        unsigned char* b = at(std::max(0, w - 1 - mx / 2), y);
        ring.push_back({a[0], a[1], a[2]});
        ring.push_back({b[0], b[1], b[2]});
    }
    std::sort(ring.begin(), ring.end(), [](const auto& a, const auto& b) {
        return a[0] + a[1] + a[2] < b[0] + b[1] + b[2];
    });
    std::array<int, 3> fill = ring.empty() ? std::array<int, 3>{150, 145, 125}
                                           : ring[ring.size() / 2];

    auto band = [&](int x0, int y0, int x1, int y1) {
        for (int y = std::max(0, y0); y < std::min(h, y1); ++y) {
            for (int x = std::max(0, x0); x < std::min(w, x1); ++x) {
                unsigned char* p = at(x, y);
                p[0] = (unsigned char)fill[0];
                p[1] = (unsigned char)fill[1];
                p[2] = (unsigned char)fill[2];
            }
        }
    };
    band(0, 0, w, my);
    band(0, h - my, w, h);
    band(0, 0, mx, h);
    band(w - mx, 0, w, h);

    std::vector<uint8_t> out;
    stbi_write_png_to_func(
        [](void* ctx, void* data, int size) {
            auto* v = (std::vector<uint8_t>*)ctx;
            v->insert(v->end(), (uint8_t*)data, (uint8_t*)data + size);
        },
        &out, w, h, 3, pixels, w * 3);
    stbi_image_free(pixels);
    if (!out.empty()) png.swap(out);
}

void AttachStyle(AppState& app, DesignSpec& spec) {
    const StyleDef* s = app.styles.Find(spec.style);
    if (s) {
        spec.style_category = s->category;
        spec.style_enclosure = s->enclosure;
    }
    spec.edge_walls = arch::EnclosureOf(spec.style_enclosure, spec.style_category,
                                        spec.layout, "") != "open";
}

DesignSpec SpecFromUi(AppState& app) {
    DesignSpec spec;
    spec.title = "Battle Map";
    spec.name = OllamaService::SanitizeName(app.sceneText.substr(0, 40));
    spec.style = app.selectedStyle;
    spec.scene_summary = app.sceneText;
    spec.render_details = app.map.meta.render_details;
    spec.prop_density = app.propDensity;
    spec.terrain_kind = app.terrainKind;
    spec.terrain_amount = app.terrainAmount;

    const StyleDef* style = app.styles.Find(app.selectedStyle);
    spec.layout = app.layoutIndex == 0
                      ? (style ? style->default_layout : std::string("dungeon"))
                      : kLayoutNames[app.layoutIndex];
    if (style) spec.style_props = style->props;

    spec.cols = app.cols;
    spec.rows = app.rows;
    spec.border = std::clamp(app.config.border_cells, 0, 8);

    spec.rooms = {
        {"main_hall", "Main Hall",
         "The largest space, worn smooth down the middle where people walk.", 'l',
         "none", {}, false, 0, 0, 0, 0},
        {"side_room", "Side Chamber",
         "A smaller room off the main one, its floor less worn.", 'm',
         "none", {}, false, 0, 0, 0, 0},
        {"back_room", "Back Chamber",
         "The room furthest from the entrance, dusty and little used.", 'm',
         "none", {}, false, 0, 0, 0, 0}
    };
    return spec;
}

uint32_t PickSeed(const AppState& app) {
    if (!app.randomSeed && app.seed > 0) return (uint32_t)app.seed;
    return (uint32_t)(GetTickCount64() & 0x7FFFFFFF);
}

} // namespace dnd
