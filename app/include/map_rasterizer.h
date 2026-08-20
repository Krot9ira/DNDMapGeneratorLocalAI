#pragma once
// Human-facing preview renderer.
//
// This draws the colour-coded picture of a layout that a game master reads to
// check the plan. It is never sent to the image model - Ideogram receives the
// layout as bounding boxes inside its JSON caption, so no control image exists.
#include "map_types.h"

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace dnd {

struct ImageBuffer {
    int width = 0, height = 0;
    std::vector<uint8_t> pixels;  // RGBA

    void Allocate(int w, int h, uint8_t r = 0, uint8_t g = 0, uint8_t b = 0) {
        width = w;
        height = h;
        pixels.assign((size_t)w * h * 4, 255);
        for (int i = 0; i < w * h; ++i) {
            pixels[(size_t)i * 4 + 0] = r;
            pixels[(size_t)i * 4 + 1] = g;
            pixels[(size_t)i * 4 + 2] = b;
        }
    }

    bool Inside(int x, int y) const { return x >= 0 && y >= 0 && x < width && y < height; }

    void SetPixel(int x, int y, uint8_t r, uint8_t g, uint8_t b) {
        if (!Inside(x, y)) return;
        size_t i = ((size_t)y * width + x) * 4;
        pixels[i + 0] = r;
        pixels[i + 1] = g;
        pixels[i + 2] = b;
        pixels[i + 3] = 255;
    }

    void FillRect(int x0, int y0, int w, int h, uint8_t r, uint8_t g, uint8_t b) {
        for (int y = y0; y < y0 + h; ++y)
            for (int x = x0; x < x0 + w; ++x) SetPixel(x, y, r, g, b);
    }

    void DrawLine(float ax, float ay, float bx, float by, int thickness,
                  uint8_t r, uint8_t g, uint8_t b) {
        float dx = bx - ax, dy = by - ay;
        int steps = (int)std::max(std::fabs(dx), std::fabs(dy)) + 1;
        int half = std::max(0, thickness / 2);
        for (int i = 0; i <= steps; ++i) {
            float t = steps ? (float)i / steps : 0.0f;
            int px = (int)std::lround(ax + dx * t), py = (int)std::lround(ay + dy * t);
            for (int oy = -half; oy <= half; ++oy)
                for (int ox = -half; ox <= half; ++ox) SetPixel(px + ox, py + oy, r, g, b);
        }
    }

    void DrawCircle(float cx, float cy, float rad, int thickness,
                    uint8_t r, uint8_t g, uint8_t b) {
        int segments = 40;
        float px = cx + rad, py = cy;
        for (int i = 1; i <= segments; ++i) {
            float a = (float)(i * 2.0 * 3.14159265 / segments);
            float x = cx + rad * std::cos(a), y = cy + rad * std::sin(a);
            DrawLine(px, py, x, y, thickness, r, g, b);
            px = x;
            py = y;
        }
    }
};

class MapRasterizer {
public:
    // Colour-coded plan with prop markers. For humans only.
    static ImageBuffer RenderPreview(const MapData& map, int cellPx = 28);
    static bool ExportPng(const ImageBuffer& img, const std::string& filePath);
};

}  // namespace dnd
