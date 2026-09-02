#pragma once

#include "../app_state.h"
#include "../../include/app_theme.h"
#include <imgui.h>
#include <string>
#include <vector>
#include <functional>

namespace dnd {

extern const char* kLayoutNames[];
extern const char* kTerrainNames[];
extern const char* kAmountNames[];
extern const char* kPaintTiles[];
extern const char* kTileHints[];

struct EffectInfo {
    const char* kind;
    const char* label;
    const char* hint;
    ImU32 tint;
};

extern const EffectInfo kEffects[];
extern const size_t kEffectsCount;

ImU32 EffectTint(const std::string& kind);
const char* EffectLabel(const std::string& kind);

struct PropInfo {
    const char* kind;
    const char* label;
    const char* hint;
};

extern const PropInfo kProps[];
extern const size_t kPropsCount;

const char* PropLabel(const std::string& kind);
ImU32 TileColor(Tile t);

float PanelWidth(float fraction, float minimum, float maximum);
void GridMetrics(float minCell, int& outPerRow, float& outCellW);
std::string FitText(const std::string& text, float maxWidth);

void DrawTileGlyph(ImDrawList* dl, ImVec2 c, float r, Tile t, ImU32 col);
void DrawPropGlyph(ImDrawList* dl, ImVec2 c, float r, const char* kind, ImU32 col);
void DrawEffectGlyph(ImDrawList* dl, ImVec2 c, float r, const char* kind, ImU32 col);
void DrawToolGlyph(ImDrawList* dl, ImVec2 c, float r, int tool, ImU32 col);

bool IconButton(const char* id, const char* label, ImVec2 size,
                const std::function<void(ImDrawList*, ImVec2, float, ImU32)>& glyph,
                ImU32 glyphCol);

bool InputTextString(const char* label, std::string* str, ImGuiInputTextFlags flags = 0);
bool InputTextMultilineString(const char* label, std::string* str, const ImVec2& size);
void HelpMarker(const char* text);
void DrawJobLog(AppState& app, float height);
ImU32 HexToCol(const std::string& hex, ImU32 fallback);
bool StyleBadge(const std::string& origin, const char** label, ImU32* fill, ImU32* text);
void DrawStylePicker(AppState& app);

void SetupFonts();
std::string OutputDir(const AppState& app, const std::string& name);
void PaintBleedMargin(std::vector<uint8_t>& png, const MapData& map);
void AttachStyle(AppState& app, DesignSpec& spec);
DesignSpec SpecFromUi(AppState& app);
uint32_t PickSeed(const AppState& app);

} // namespace dnd
