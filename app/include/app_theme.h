#pragma once
#include <imgui.h>

namespace dnd {

inline void SetupDarkFantasyTheme() {
    ImGuiStyle& style = ImGui::GetStyle();
    ImVec4* colors = style.Colors;

    // Base geometry & polish
    style.WindowRounding = 8.0f;
    style.ChildRounding = 6.0f;
    style.FrameRounding = 5.0f;
    style.PopupRounding = 6.0f;
    style.ScrollbarRounding = 6.0f;
    style.GrabRounding = 4.0f;
    style.TabRounding = 6.0f;

    style.WindowPadding = ImVec2(12.0f, 12.0f);
    style.FramePadding = ImVec2(8.0f, 5.0f);
    style.ItemSpacing = ImVec2(8.0f, 8.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.IndentSpacing = 20.0f;
    style.ScrollbarSize = 14.0f;

    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;

    // Dark Fantasy Palette
    // Backgrounds
    colors[ImGuiCol_WindowBg]             = ImVec4(0.09f, 0.10f, 0.12f, 1.00f); // #171a1f
    colors[ImGuiCol_ChildBg]              = ImVec4(0.12f, 0.13f, 0.16f, 1.00f); // #1f2129
    colors[ImGuiCol_PopupBg]              = ImVec4(0.11f, 0.12f, 0.15f, 0.98f);
    colors[ImGuiCol_Border]               = ImVec4(0.24f, 0.27f, 0.32f, 0.60f);
    colors[ImGuiCol_BorderShadow]         = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);

    // Headers & Text
    colors[ImGuiCol_Text]                 = ImVec4(0.92f, 0.93f, 0.95f, 1.00f);
    colors[ImGuiCol_TextDisabled]         = ImVec4(0.50f, 0.53f, 0.58f, 1.00f);

    // Frames & Inputs
    colors[ImGuiCol_FrameBg]              = ImVec4(0.16f, 0.18f, 0.22f, 1.00f);
    colors[ImGuiCol_FrameBgHovered]       = ImVec4(0.22f, 0.25f, 0.30f, 1.00f);
    colors[ImGuiCol_FrameBgActive]        = ImVec4(0.26f, 0.29f, 0.36f, 1.00f);

    // Title & Menus
    colors[ImGuiCol_TitleBg]              = ImVec4(0.08f, 0.09f, 0.11f, 1.00f);
    colors[ImGuiCol_TitleBgActive]        = ImVec4(0.14f, 0.16f, 0.20f, 1.00f);
    colors[ImGuiCol_TitleBgCollapsed]     = ImVec4(0.08f, 0.09f, 0.11f, 0.75f);
    colors[ImGuiCol_MenuBarBg]            = ImVec4(0.11f, 0.12f, 0.15f, 1.00f);

    // Scrollbar
    colors[ImGuiCol_ScrollbarBg]          = ImVec4(0.09f, 0.10f, 0.12f, 0.60f);
    colors[ImGuiCol_ScrollbarGrab]        = ImVec4(0.25f, 0.28f, 0.34f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.35f, 0.39f, 0.46f, 1.00f);
    colors[ImGuiCol_ScrollbarGrabActive]  = ImVec4(0.85f, 0.65f, 0.20f, 1.00f); // Gold

    // Sliders & Checkmarks
    colors[ImGuiCol_CheckMark]            = ImVec4(0.92f, 0.72f, 0.25f, 1.00f); // Gold
    colors[ImGuiCol_SliderGrab]           = ImVec4(0.80f, 0.60f, 0.20f, 1.00f);
    colors[ImGuiCol_SliderGrabActive]     = ImVec4(0.95f, 0.78f, 0.30f, 1.00f);

    // Buttons (Rich amber/gold or deep slate)
    colors[ImGuiCol_Button]               = ImVec4(0.20f, 0.23f, 0.29f, 1.00f);
    colors[ImGuiCol_ButtonHovered]        = ImVec4(0.70f, 0.52f, 0.18f, 0.85f);
    colors[ImGuiCol_ButtonActive]         = ImVec4(0.85f, 0.65f, 0.22f, 1.00f);

    // Headers & TreeNodes
    colors[ImGuiCol_Header]               = ImVec4(0.20f, 0.24f, 0.30f, 0.80f);
    colors[ImGuiCol_HeaderHovered]        = ImVec4(0.30f, 0.35f, 0.44f, 0.80f);
    colors[ImGuiCol_HeaderActive]         = ImVec4(0.75f, 0.55f, 0.20f, 0.90f);

    // Tabs
    colors[ImGuiCol_Tab]                  = ImVec4(0.14f, 0.16f, 0.20f, 1.00f);
    colors[ImGuiCol_TabHovered]           = ImVec4(0.75f, 0.55f, 0.18f, 0.85f);
    colors[ImGuiCol_TabSelected]            = ImVec4(0.25f, 0.28f, 0.35f, 1.00f);
    colors[ImGuiCol_TabDimmed]         = ImVec4(0.10f, 0.11f, 0.14f, 1.00f);
    colors[ImGuiCol_TabDimmedSelected]   = ImVec4(0.16f, 0.18f, 0.22f, 1.00f);

    // Separators
    colors[ImGuiCol_Separator]            = ImVec4(0.24f, 0.27f, 0.34f, 0.60f);
    colors[ImGuiCol_SeparatorHovered]     = ImVec4(0.85f, 0.65f, 0.20f, 0.78f);
    colors[ImGuiCol_SeparatorActive]      = ImVec4(0.85f, 0.65f, 0.20f, 1.00f);

    // Resize grip
    colors[ImGuiCol_ResizeGrip]           = ImVec4(0.85f, 0.65f, 0.20f, 0.20f);
    colors[ImGuiCol_ResizeGripHovered]    = ImVec4(0.85f, 0.65f, 0.20f, 0.67f);
    colors[ImGuiCol_ResizeGripActive]     = ImVec4(0.85f, 0.65f, 0.20f, 0.95f);
}

} // namespace dnd
