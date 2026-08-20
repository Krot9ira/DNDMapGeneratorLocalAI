#include "map_rasterizer.h"
#include "map_architect.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb/stb_image_write.h>

namespace dnd {

struct Rgb { uint8_t r, g, b; };

static Rgb PreviewColor(Tile t) {
    switch (t) {
    case Tile::Void: return {28, 30, 36};
    case Tile::Floor: return {232, 226, 214};
    case Tile::Wall: return {58, 62, 72};
    case Tile::Door: return {196, 150, 74};
    case Tile::Window: return {150, 196, 214};
    case Tile::Water: return {86, 140, 178};
    case Tile::Pit: return {34, 34, 40};
    case Tile::Rubble: return {176, 166, 150};
    case Tile::Vegetation: return {118, 156, 104};
    case Tile::Stairs: return {198, 190, 176};
    case Tile::Bridge: return {166, 130, 88};
    default: return {120, 120, 120};
    }
}

// Half-width of the hull along its length (0 stern, 1 bow). The ship also exists
// as tiles for walkability, but a six-cell-tall blob does not read as a vessel,
// so the preview traces the real shape.
static float HullProfile(float t) {
    if (t < 0.14f) return 0.70f + 0.30f * (t / 0.14f);
    if (t < 0.58f) return 1.0f;
    float u = (t - 0.58f) / 0.42f;
    return std::max(0.0f, std::sqrt(std::max(0.0f, 1.0f - u * u)));
}

static void DrawShip(ImageBuffer& img, const Structure& st, int cell) {
    const float half = st.h / 2.0f;
    const int steps = 48;
    float prevTopX = 0, prevTopY = 0, prevBotX = 0, prevBotY = 0;
    for (int i = 0; i <= steps; ++i) {
        float t = (float)i / steps;
        float hw = HullProfile(t) * half;
        float px = (st.x + t * st.w) * cell;
        float topY = (st.y + half - hw) * cell;
        float botY = (st.y + half + hw) * cell;
        if (i > 0) {
            img.DrawLine(prevTopX, prevTopY, px, topY, std::max(2, cell / 12), 52, 40, 26);
            img.DrawLine(prevBotX, prevBotY, px, botY, std::max(2, cell / 12), 52, 40, 26);
        }
        prevTopX = px; prevTopY = topY;
        prevBotX = px; prevBotY = botY;
    }
    // Deck planking, running fore and aft.
    int planks = std::max(3, st.h * 2);
    for (int i = 1; i < planks; ++i) {
        float py = (st.y + (float)st.h * i / planks) * cell;
        img.DrawLine((st.x + st.w * 0.12f) * cell, py, (st.x + st.w * 0.88f) * cell, py,
                     std::max(1, cell / 16), 122, 92, 58);
    }
    float mid = (st.y + half) * cell;
    img.DrawCircle((st.x + st.w * 0.46f) * cell, mid,
                   std::min(st.w, st.h) * cell * 0.10f, 2, 40, 32, 20);
}

ImageBuffer MapRasterizer::RenderPreview(const MapData& map, int cellPx) {
    TileGrid g = arch::ZonesToGrid(map);
    const int cell = std::max(6, cellPx);
    ImageBuffer img;
    Rgb bg = PreviewColor(Tile::Void);
    img.Allocate(g.cols * cell, g.rows * cell, bg.r, bg.g, bg.b);

    for (int y = 0; y < g.rows; ++y) {
        for (int x = 0; x < g.cols; ++x) {
            Rgb c = PreviewColor(g.Get(x, y));
            img.FillRect(x * cell, y * cell, cell, cell, c.r, c.g, c.b);
        }
    }

    for (const auto& st : map.structures)
        if (st.kind == "ship") DrawShip(img, st, cell);

    // Faint cell guides so a human can count squares.
    for (int x = 0; x <= g.cols; ++x)
        img.DrawLine((float)(x * cell), 0.0f, (float)(x * cell), (float)(g.rows * cell),
                     1, 0, 0, 0);
    for (int y = 0; y <= g.rows; ++y)
        img.DrawLine(0.0f, (float)(y * cell), (float)(g.cols * cell), (float)(y * cell),
                     1, 0, 0, 0);

    for (const auto& f : map.features) {
        float cx = (f.x + 0.5f) * cell, cy = (f.y + 0.5f) * cell;
        // Pinned props read as a solid ring; loose clutter as a light dot.
        img.DrawCircle(cx, cy, cell * (f.structural ? 0.30f : 0.18f),
                       f.structural ? std::max(2, cell / 12) : 1, 40, 40, 48);
    }
    return img;
}

bool MapRasterizer::ExportPng(const ImageBuffer& img, const std::string& filePath) {
    if (img.width <= 0 || img.height <= 0) return false;
    return stbi_write_png(filePath.c_str(), img.width, img.height, 4, img.pixels.data(),
                          img.width * 4) != 0;
}

}  // namespace dnd
